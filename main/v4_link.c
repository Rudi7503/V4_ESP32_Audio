/*
 * v4_link.c - I2C slave side of the V4 protocol.
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/i2c_slave.h"

#include "v4_proto.h"
#include "v4_link.h"
#include "bt_manager.h"
#include "sd_fs.h"
#include "audio_source.h"
#include "stream_proc.h"
#include "esp_bt_audio_media.h"
#include "esp_bt_audio_defs.h"
#include "esp_timer.h"
#include "driver/gpio.h"

static const char *TAG = "v4_link";

#define V4_WRITE_TIMEOUT_MS     100

/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  kind;                              /* RX_KIND_* */
    uint8_t  data[V4P_WRITE_FRAME_LEN];
    uint8_t  len;
    uint32_t t_rx_us;                           /* esp_timer at rx-done (ISR) */
} rx_msg_t;

#define RX_KIND_FRAME       0
#define RX_KIND_SELFTEST    1

typedef enum {
    DEFER_NONE = 0,
    DEFER_SD_MOUNT,
    DEFER_BULK_LOAD,
    DEFER_SD_PLAY,
    DEFER_SD_STOP,
    DEFER_DIR_LOAD,      /* read one directory entry into s_dir_cache      */
    DEFER_DIR_OPEN,      /* open a directory (I/O, so not in the hot path) */
    DEFER_FILE_OPEN,     /* open a file      (I/O, so not in the hot path) */
    DEFER_MEDIA_START,   /* start the A2DP stream (0.9.65, needs waiting)  */
} defer_op_t;

/*
 * Cached DIR_NEXT result, one slot per open directory handle.
 *
 * `valid` means "a result for `index` is cached" - including a *failed* one.
 * That distinction matters: if only successful entries were cached, a read that
 * failed after being deferred would answer BUSY again on every repeat and the
 * master would loop forever. `status` therefore carries OK, END or the error.
 */
typedef struct {
    bool     valid;               /* result for `index` sits in the cache     */
    bool     pending;             /* a deferred load is queued                */
    uint16_t pending_index;       /* ... for this index                       */
    uint16_t index;
    uint8_t  status;              /* v4p_status_t of the cached result */
    uint8_t  attr;
    uint32_t size;
    bool     is_dir;
    bool     truncated;
    char     name[V4P_NAME_MAX + 1];
} dir_entry_cache_t;

/*
 * Parameters for one deferred operation. dispatch() fills the handoff
 * variables below; the caller copies them into a work_msg_t and hands that to
 * whichever task performs the work. Copying rather than sharing is what makes
 * it safe to run the work in a *different* task from the one answering the
 * master: the handoff variables may be overwritten by the next command while
 * the worker is still busy.
 */
typedef struct {
    defer_op_t op;
    uint8_t    seq;
    uint8_t    handle;
    uint16_t   index;                 /* DIR_NEXT index / FILE_READ block   */
    /* PLAY_FILE needs BOTH forms of the path: sd_fs_file_open() wants it
     * relative to the mount ("test2.mp3"), audio_source_play_sd() wants the
     * full VFS path ("/sdcard/test2.mp3"). Handing the wrong one to
     * sd_fs_file_open() is a silent NOT_FOUND. DIR_OPEN/FILE_OPEN use `path`,
     * which is the relative form. */
    char       path[SD_FS_PATH_BUF];
    char       raw[V4P_PATH_MAX + 1];
} work_msg_t;

/*
 * Result of a deferred open. DIR_OPEN and FILE_OPEN cannot answer inside the
 * hot path - a FAT lookup is I/O and takes longer than the master's t_wait - so
 * the first call answers BUSY and the master's repeat (same cmd, same SEQ)
 * collects the stored result.
 *
 * That also makes the open idempotent. The retry no longer executes a second
 * open, which is what used to consume a second handle on every unsafe retry and
 * eventually turned every open into NO_HANDLE.
 */
typedef struct {
    bool     valid;               /* result ready to be collected             */
    bool     pending;             /* queued, the worker has not finished yet  */
    uint8_t  cmd;
    uint8_t  seq;
    uint8_t  status;
    uint8_t  handle;
    uint32_t size;
} open_result_t;

static i2c_slave_dev_handle_t s_slave;
static QueueHandle_t          s_rx_queue;

/* assembled path for PATH_CLEAR / PATH_APPEND */
static char     s_path[V4P_PATH_MAX + 1];
static uint16_t s_path_len;

/* negotiable FILE_READ chunk size */
static uint16_t s_chunk = V4P_CHUNK_DEFAULT;

/* single slot block cache: BUSY first, data on the re-sent command */
static uint8_t  s_bulk_cache[V4P_BULK_PAYLOAD_MAX];
static bool     s_bulk_valid;
static uint8_t  s_bulk_handle;
static uint16_t s_bulk_block;
static uint16_t s_bulk_len;
static uint8_t  s_bulk_status;

/* Response scratch buffer. File scope on purpose: the bulk frame is about 1 KB
 * and would otherwise eat a quarter of the protocol task's stack. Only the
 * protocol task touches it, so this is safe. */
static uint8_t  s_resp[V4P_BULK_FRAME_MAX];

/*
 * Aufgaben-Handles nur fuer die Stack-Auslastung im 'v4_bus'-Bericht (0.9.62).
 *
 * Die Kommandowege der Bruecke nesten tief (dispatch -> sd_fs -> VFS -> FatFs),
 * deshalb wird V4_TASK_STACK bei jeder Gelegenheit geprueft statt geschaetzt:
 * steht in v4_bus "rest" nahe 0, ist der Stack zu knapp.
 */
static TaskHandle_t s_link_task_h;
static TaskHandle_t s_work_task_h;

/* deferred (after-reply) work */
static uint8_t  s_defer_handle;
static uint16_t s_defer_block;

/* PLAY_FILE bookkeeping: the pipeline switch is slow, so the command answers
 * BUSY first and the master repeats it. If the switch then fails, remember the
 * reason so the repeat reports it instead of looping on BUSY forever. */
static char         s_defer_path[SD_FS_PATH_BUF];
static char         s_defer_raw[V4P_PATH_MAX + 1];   /* relative form, see work_msg_t */
static v4p_status_t s_last_play_err = V4P_ST_OK;
static char         s_last_play_path[SD_FS_PATH_BUF];

/* SD mount bookkeeping so a missing card does not spin */
static bool     s_sd_attempted;
static esp_err_t s_sd_last_err = ESP_OK;

static dir_entry_cache_t s_dir_cache[SD_FS_MAX_DIR_HANDLES];

/* Command-to-answer timing. PROTOCOL_V4_SYNC.md §6 records that a 2000 us
 * t_wait was not enough on real hardware: roughly every fourth read carried a
 * bad magic while the bus itself reported OK. The cause is structural - the
 * answer is assembled in the protocol task, not in the receive callback, so a
 * task that is scheduled late leaves the master reading stale bytes instead of
 * a BUSY frame. These counters measure how long the round really takes.
 *
 * They only fill while a real master is on the bus; the self test is synthetic
 * and excluded (s_rx_stamp_us stays 0). */
typedef struct {
    uint32_t frames;
    uint32_t max_us;
    uint32_t over_2000;
    uint32_t over_5000;
    uint32_t over_10000;
} lat_stats_t;

static lat_stats_t s_lat;
static uint32_t    s_rx_stamp_us;

/* Bus activity counters. A rejected frame is logged as well, but the counters
 * make "nothing ever arrived" distinguishable from "arrived but rejected"
 * without hunting for a log line: the first is a wiring/addressing fault, the
 * second a timing or frame-format fault. Those need opposite fixes. */
static uint32_t    s_rx_count;
static uint32_t    s_rx_bad;

/* Deferred-work plumbing. dispatch() fills the s_defer_* handoff variables and
 * the caller snapshots them into a work_msg_t (see there for why the copy
 * matters). s_work_queue feeds a separate task, so a slow SD access can no
 * longer delay the answer to the next command - that delay was the whole
 * "falsche Magic" problem. */
static uint16_t      s_defer_index;      /* DIR_NEXT index handoff         */
static open_result_t s_open_res;         /* deferred DIR_OPEN / FILE_OPEN  */
static bool          s_play_pending;     /* a PLAY_FILE rebuild is queued  */
static char          s_play_pending_path[SD_FS_PATH_BUF];
/* Ergebnis der letzten MEDIA_START-Anforderung; BUSY heisst "noch keins". */
static v4p_status_t  s_media_err = V4P_ST_BUSY;
static bool          s_media_pending;   /* Start laeuft im Arbeitstask */
static QueueHandle_t s_work_queue;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static v4p_status_t map_sd_err(sd_fs_err_t e)
{
    switch (e) {
    case SD_FS_OK:              return V4P_ST_OK;
    case SD_FS_ERR_NOT_MOUNTED: return V4P_ST_NO_SD;
    case SD_FS_ERR_NOT_FOUND:   return V4P_ST_NOT_FOUND;
    case SD_FS_ERR_NO_HANDLE:   return V4P_ST_NO_HANDLE;
    case SD_FS_ERR_END:         return V4P_ST_END;
    case SD_FS_ERR_ARG:         return V4P_ST_BAD_ARG;
    case SD_FS_ERR_IO:
    default:                    return V4P_ST_IO_ERR;
    }
}

static void write_response(const uint8_t *frame, size_t len)
{
    uint32_t written = 0;
    esp_err_t err = i2c_slave_write(s_slave, frame, (uint32_t)len, &written, V4_WRITE_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2c_slave_write(%u) failed: %s", (unsigned)len, esp_err_to_name(err));
    } else if (written != len) {
        ESP_LOGW(TAG, "only %u of %u bytes buffered", (unsigned)written, (unsigned)len);
    }

    /* How long did the master have to wait for this answer, measured from the
     * moment the write completed on the bus? Unsigned arithmetic handles the
     * 32-bit wrap of esp_timer_get_time() correctly. */
    if (s_rx_stamp_us != 0u) {
        uint32_t dt = (uint32_t)esp_timer_get_time() - s_rx_stamp_us;
        s_lat.frames++;
        if (dt > s_lat.max_us) {
            s_lat.max_us = dt;
        }
        if (dt > 2000u)  { s_lat.over_2000++;  }
        if (dt > 5000u)  { s_lat.over_5000++;  }
        if (dt > 10000u) { s_lat.over_10000++; }
        if (s_lat.frames <= 3u || (s_lat.frames & 0x3Fu) == 0u) {
            ESP_LOGI(TAG, "LATENZ n=%u max=%u us  ueber2ms=%u ueber5ms=%u ueber10ms=%u",
                     (unsigned)s_lat.frames, (unsigned)s_lat.max_us,
                     (unsigned)s_lat.over_2000, (unsigned)s_lat.over_5000,
                     (unsigned)s_lat.over_10000);
        }
    }
}

/** @brief Resolve the path argument: payload wins, otherwise the assembled path. */
static const char *resolve_path(const uint8_t *p, uint8_t plen, char *tmp, size_t tmp_len)
{
    if (plen > 0) {
        if ((size_t)plen >= tmp_len) {
            return NULL;
        }
        memcpy(tmp, p, plen);
        tmp[plen] = '\0';
        return tmp;
    }
    s_path[s_path_len] = '\0';
    return s_path;   /* empty string means "root" for sd_fs */
}

static void fill_info_payload(uint8_t *out)
{
    out[0] = V4P_PROTO_VERSION;
    out[1] = V4P_FW_VERSION;
    out[2] = V4P_WRITE_FRAME_LEN;
    out[3] = V4P_READ_FRAME_LEN;
    v4p_put_u16le(&out[4], V4P_BULK_PAYLOAD_MAX);
    v4p_put_u16le(&out[6], s_chunk);
    out[8] = BT_MGR_MAX_DEVICES;
    out[9] = V4P_PATH_MAX;
    out[10] = 0;
    out[11] = 0;
}

/*
 * Pfadvergleich fuer "spielt schon derselbe Titel?" (0.9.65).
 *
 * audio_source_current_path() liefert den GMF-URI ("file://sdcard/test2.mp3"),
 * die Protokollpfade sind VFS-Pfade ("/sdcard/test2.mp3"). Der frueher direkte
 * strcmp() konnte deshalb NIE gleich sein: die Idempotenzpruefung in PLAY_FILE
 * war toter Code, und jede Wiederholung baute den Zweig neu auf. Im V4-Test am
 * 08.10. waren das 225 PLAY_FILE in vier Minuten - der Ton riss dabei ab,
 * obwohl der Master nur den BUSY-Zyklus bestaetigt hat.
 */
static bool same_media_path(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    if (strncmp(a, "file://", 7) == 0) {
        a += 7;
    }
    if (strncmp(b, "file://", 7) == 0) {
        b += 7;
    }
    while (*a == '/') {
        a++;
    }
    while (*b == '/') {
        b++;
    }
    return strcmp(a, b) == 0;
}

/*
 * A2DP-Uebertragung starten und warten, bis der Mischer laeuft (0.9.65).
 *
 * Laeuft NUR im Arbeitstask (verzoegert), nie im Antworte-Pfad - das Warten
 * sprengte dort den t_wait des Masters. Nach dem Start braucht die Kette rund
 * 400-500 ms ("Starte I2S-Zubringer, dann nach 400 ms den Mischer"), und erst
 * danach hat der Datei-Zweig einen Abnehmer. Ohne dieses Warten endete
 * PLAY_FILE im Fehler (ERROR statt FINISHED, Mitschnitt /tmp/v4_traffic.log).
 */
#define MEDIA_START_WAIT_MS     2000
#define MEDIA_START_POLL_MS       50

static v4p_status_t media_start_wait(void)
{
    if (bt_mgr_audio_streaming()) {
        return V4P_ST_OK;
    }
    if (!bt_mgr_is_connected()) {
        /* Ohne verbundenes Geraet registriert der BT-Stack die Uebertragung
         * nicht ("a2d_media_start is not registered"). */
        return V4P_ST_BAD_STATE;
    }

    /*
     * ZUERST den I2S-Eingang der Vampire anfordern (0.9.67).
     *
     * Ohne diesen Wunsch bindet der Stream die Datei-Pipeline (local2bt_pipe)
     * an Bluetooth - und die hat seit dem Umbau auf den Mischer GAR KEINEN
     * Ausgang mehr (stream_proc.c: "Der Datei-Zweig ist ... reiner ZUBRINGER").
     * Folge: es ist nichts hoerbar, und der Datei-Zweig endet mit ERROR
     * (Mitschnitt /tmp/v4_traffic.log). Mit dem Wunsch verdrahtet
     * i2s2bt_set_stream() den Mischer - samt BT-Ausgang - und startet den
     * I2S-Zweig; Dateien kommen ueber den Mischer dazu. Der Wunsch wird beim
     * Start des Streams ausgewertet, muss also davor stehen.
     */
    i2s2bt_request();

    esp_err_t err = esp_bt_audio_media_start(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Uebertragung abgelehnt: %s", esp_err_to_name(err));
        return V4P_ST_BT_ERR;
    }

    for (int waited = 0; waited < MEDIA_START_WAIT_MS; waited += MEDIA_START_POLL_MS) {
        if (bt_mgr_audio_streaming()) {
            ESP_LOGI(TAG, "A2DP-Uebertragung laeuft (nach %d ms)", waited);
            return V4P_ST_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(MEDIA_START_POLL_MS));
    }
    ESP_LOGW(TAG, "A2DP-Uebertragung nach %d ms nicht gestartet", MEDIA_START_WAIT_MS);
    return V4P_ST_BT_ERR;
}

/* ------------------------------------------------------------------ */
/* Deferred work                                                      */
/* ------------------------------------------------------------------ */

static void run_deferred(const work_msg_t *w)
{
    switch (w->op) {
    case DEFER_SD_MOUNT: {
        s_sd_last_err = sd_fs_mount();
        s_sd_attempted = true;
        if (s_sd_last_err != ESP_OK) {
            ESP_LOGW(TAG, "SD mount attempt failed: %s", esp_err_to_name(s_sd_last_err));
        }
        break;
    }
    case DEFER_BULK_LOAD: {
        uint32_t offset = (uint32_t)w->index * s_chunk;
        uint16_t got = 0;
        sd_fs_err_t e = sd_fs_file_read(w->handle, offset, s_bulk_cache, s_chunk, &got);

        s_bulk_status = map_sd_err(e);
        s_bulk_len = (e == SD_FS_OK) ? got : 0;
        s_bulk_handle = w->handle;
        s_bulk_block = w->index;
        s_bulk_valid = true;
        break;
    }
    case DEFER_DIR_LOAD: {
        if (w->handle >= SD_FS_MAX_DIR_HANDLES) {
            break;
        }
        dir_entry_cache_t *c = &s_dir_cache[w->handle];
        char        name[V4P_NAME_MAX + 1];
        bool        is_dir = false, trunc = false;
        uint32_t    size = 0;
        uint8_t     attr = 0;

        sd_fs_err_t e = sd_fs_dir_next(w->handle, name, sizeof(name),
                                       &is_dir, &size, &attr, &trunc);

        c->pending = false;
        c->index = w->index;
        c->valid = true;
        if (e == SD_FS_OK) {
            c->status = V4P_ST_OK;
            c->attr = attr;
            c->size = size;
            c->is_dir = is_dir;
            c->truncated = trunc;
            strncpy(c->name, name, sizeof(c->name) - 1);
            c->name[sizeof(c->name) - 1] = '\0';
        } else {
            /* Cache the failure too, otherwise the repeat answers BUSY again
             * and the master loops forever. */
            c->status = (e == SD_FS_ERR_END) ? V4P_ST_END : map_sd_err(e);
            c->truncated = false;
            c->name[0] = '\0';
        }
        break;
    }
    case DEFER_DIR_OPEN:
    case DEFER_FILE_OPEN: {
        const bool     is_dir = (w->op == DEFER_DIR_OPEN);
        const uint8_t  cmd = is_dir ? V4P_CMD_DIR_OPEN : V4P_CMD_FILE_OPEN;

        s_open_res.valid = false;
        s_open_res.cmd = cmd;
        s_open_res.seq = w->seq;
        s_open_res.handle = 0;
        s_open_res.size = 0;

        if (is_dir) {
            int h = sd_fs_dir_open(w->path);
            if (h < 0) {
                s_open_res.status = map_sd_err((sd_fs_err_t)(-h));
            } else {
                if (h < SD_FS_MAX_DIR_HANDLES) {
                    memset(&s_dir_cache[h], 0, sizeof(s_dir_cache[h]));
                }
                s_open_res.status = V4P_ST_OK;
                s_open_res.handle = (uint8_t)h;
            }
        } else {
            uint32_t size = 0;
            int h = sd_fs_file_open(w->path, &size);
            if (h < 0) {
                s_open_res.status = map_sd_err((sd_fs_err_t)(-h));
            } else {
                s_bulk_valid = false;   /* handles get reused, drop the cache */
                s_open_res.status = V4P_ST_OK;
                s_open_res.handle = (uint8_t)h;
                s_open_res.size = size;
            }
        }
        s_open_res.pending = false;
        s_open_res.valid = true;
        break;
    }
    case DEFER_SD_PLAY: {
        /* The existence check lives here now, not in the answering path. It is
         * SD I/O, and doing it while answering both blew the t_wait budget and
         * occupied a file handle on *every* unsafe retry - which is exactly how
         * the handle table got exhausted and every later open answered
         * NO_HANDLE (observed: handles 0..3 all pointing at the same file). */
        v4p_status_t st = V4P_ST_OK;
        uint32_t     size = 0;
        int h = sd_fs_file_open(w->raw, &size);   /* relative form! */
        if (h < 0) {
            sd_fs_err_t e = (sd_fs_err_t)(-h);
            st = (e == SD_FS_ERR_NOT_FOUND) ? V4P_ST_NOT_FOUND : map_sd_err(e);
        } else {
            sd_fs_file_close(h);
            /*
             * 0.9.65: Die Uebertragung muss laufen, sonst hat der Datei-Zweig
             * keinen Abnehmer (Mitschnitt /tmp/v4_traffic.log: "Wiedergabe
             * beendet - stoppe den Datei-Zweig (ERROR)"). Master, die
             * MEDIA_START nicht kennen, funktionieren damit unveraendert.
             */
            v4p_status_t mst = media_start_wait();
            if (mst != V4P_ST_OK) {
                st = mst;
            } else if (audio_source_play_sd(w->path) != ESP_OK) {
                st = V4P_ST_IO_ERR;
            }
        }

        strncpy(s_last_play_path, w->path, sizeof(s_last_play_path) - 1);
        s_last_play_path[sizeof(s_last_play_path) - 1] = '\0';
        s_last_play_err = st;
        s_play_pending = false;

        if (st == V4P_ST_OK) {
            ESP_LOGI(TAG, "SD playback started: %s (%u Byte)", w->path, (unsigned)size);
        } else {
            ESP_LOGW(TAG, "SD playback of %s failed (status 0x%02X)",
                     w->path, (unsigned)st);
        }
        break;
    }
    case DEFER_MEDIA_START: {
        s_media_err = media_start_wait();
        s_media_pending = false;
        break;
    }
    case DEFER_SD_STOP:
        audio_source_use_i2s();
        s_last_play_err = V4P_ST_OK;
        s_last_play_path[0] = '\0';
        break;
    case DEFER_NONE:
    default:
        break;
    }
}

/*
 * Snapshot the handoff variables dispatch() just filled into a work message.
 * Every parameter the worker needs travels by value, so the next command can
 * overwrite the handoff state without disturbing work that is still queued.
 */
static work_msg_t work_from_handoff(defer_op_t op, uint8_t seq)
{
    work_msg_t w;

    memset(&w, 0, sizeof(w));
    w.op = op;
    w.seq = seq;
    w.handle = s_defer_handle;
    w.index = (op == DEFER_DIR_LOAD) ? s_defer_index : s_defer_block;
    strncpy(w.path, s_defer_path, sizeof(w.path) - 1);
    w.path[sizeof(w.path) - 1] = '\0';
    strncpy(w.raw, s_defer_raw, sizeof(w.raw) - 1);
    w.raw[sizeof(w.raw) - 1] = '\0';
    return w;
}

/*
 * Runs whatever the protocol task handed over. Keeping this in its own task is
 * the point: while an SD mount or a pipeline rebuild is running here, the
 * protocol task stays free to answer the master inside t_wait.
 */
static void v4_work_task(void *arg)
{
    work_msg_t w;

    while (1) {
        if (xQueueReceive(s_work_queue, &w, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        run_deferred(&w);
    }
}

/* ------------------------------------------------------------------ */
/* Command dispatch                                                   */
/* ------------------------------------------------------------------ */

static void dispatch(uint8_t cmd, uint8_t seq, const uint8_t *p, uint8_t plen,
                     uint8_t *out, size_t *out_len, defer_op_t *defer)
{
    uint8_t     pbuf[V4P_READ_PAYLOAD_MAX];
    uint8_t     plen_out = 0;
    uint8_t     flags = V4P_FL_NONE;
    v4p_status_t status = V4P_ST_OK;

    memset(pbuf, 0, sizeof(pbuf));

    switch (cmd) {

    /* ---------------- identification ---------------- */
    case V4P_CMD_PING:
    case V4P_CMD_GET_INFO:
        fill_info_payload(pbuf);
        plen_out = 12;
        break;

    case V4P_CMD_GET_STATUS: {
        uint32_t free_kb = 0;
        if (sd_fs_is_mounted()) {
            sd_fs_info(NULL, &free_kb, NULL, NULL);
        }
        pbuf[V4P_ST_OFF_STATE]      = (uint8_t)bt_mgr_state();
        pbuf[V4P_ST_OFF_CONN_IDX]   = (uint8_t)bt_mgr_connected_index();
        pbuf[V4P_ST_OFF_DEV_COUNT]  = (uint8_t)bt_mgr_dev_count();
        pbuf[V4P_ST_OFF_SCAN_ACTIVE] = bt_mgr_scan_active() ? 1 : 0;
        pbuf[V4P_ST_OFF_SD_MOUNTED] = sd_fs_is_mounted() ? 1 : 0;
        pbuf[V4P_ST_OFF_AUDIO]      = (bt_mgr_audio_streaming() ? V4P_AUDIO_A2DP_STREAMING : 0)
                                    | ((audio_source_get() == AUDIO_SOURCE_SD)
                                       ? V4P_AUDIO_SD_PLAYBACK : 0);
        v4p_put_u16le(&pbuf[V4P_ST_OFF_SCAN_GEN], bt_mgr_scan_generation());
        v4p_put_u32le(&pbuf[V4P_ST_OFF_SD_FREE_KB], free_kb);
        v4p_put_u16le(&pbuf[V4P_ST_OFF_CHUNK], s_chunk);
        pbuf[V4P_ST_OFF_PROTO_VER]  = V4P_PROTO_VERSION;
        pbuf[V4P_ST_OFF_FW_VER]     = V4P_FW_VERSION;
        pbuf[V4P_ST_OFF_SD_CARD]    = sd_fs_card_present() ? 1 : 0;
        plen_out = V4P_ST_PAYLOAD_LEN;
        break;
    }

    /* ---------------- bluetooth discovery ---------------- */
    case V4P_CMD_SCAN_START: {
        uint8_t units = (plen >= 1) ? p[0] : 0;
        uint8_t mode = (plen >= 2) ? p[1] : BT_MGR_SCAN_ONCE;
        esp_err_t err = bt_mgr_scan_start(units, mode);
        if (err == ESP_ERR_INVALID_STATE) {
            status = V4P_ST_BAD_STATE;
        } else if (err != ESP_OK) {
            status = V4P_ST_BT_ERR;
        }
        break;
    }

    case V4P_CMD_SCAN_STOP:
        bt_mgr_scan_stop();
        break;

    case V4P_CMD_DEV_COUNT:
        pbuf[0] = (uint8_t)bt_mgr_dev_count();
        plen_out = 1;
        break;

    case V4P_CMD_DEV_GET: {
        if (plen < 1) {
            status = V4P_ST_BAD_ARG;
            break;
        }
        bt_mgr_dev_t dev;
        if (!bt_mgr_dev_copy(p[0], &dev)) {
            status = V4P_ST_NOT_FOUND;
            break;
        }
        uint8_t name_len = dev.name_len;
        if (name_len > (V4P_READ_PAYLOAD_MAX - V4P_DEV_OFF_NAME)) {
            name_len = V4P_READ_PAYLOAD_MAX - V4P_DEV_OFF_NAME;
            flags |= V4P_FL_NAME_TRUNCATED;
        }
        pbuf[V4P_DEV_OFF_INDEX] = p[0];
        memcpy(&pbuf[V4P_DEV_OFF_BDA], dev.bda, ESP_BD_ADDR_LEN);
        pbuf[V4P_DEV_OFF_NAME_LEN] = name_len;
        memcpy(&pbuf[V4P_DEV_OFF_NAME], dev.name, name_len);
        plen_out = V4P_DEV_OFF_NAME + name_len;
        break;
    }

    /* ---------------- bluetooth connection ---------------- */
    case V4P_CMD_CONNECT: {
        if (plen < 1) {
            status = V4P_ST_BAD_ARG;
            break;
        }
        esp_err_t err = bt_mgr_connect_index(p[0]);
        if (err == ESP_ERR_INVALID_ARG) {
            status = V4P_ST_NOT_FOUND;
        } else if (err != ESP_OK) {
            status = V4P_ST_BT_ERR;
        }
        break;
    }

    case V4P_CMD_CONNECT_BDA: {
        if (plen < ESP_BD_ADDR_LEN) {
            status = V4P_ST_BAD_ARG;
            break;
        }
        esp_err_t err = bt_mgr_connect_bda(p);
        if (err != ESP_OK) {
            status = V4P_ST_BT_ERR;
        }
        break;
    }

    case V4P_CMD_DISCONNECT: {
        esp_err_t err = bt_mgr_disconnect();
        if (err == ESP_ERR_INVALID_STATE) {
            status = V4P_ST_BAD_STATE;
        } else if (err != ESP_OK) {
            status = V4P_ST_BT_ERR;
        }
        break;
    }

    case V4P_CMD_FORGET:
        bt_mgr_forget();
        break;

    /* ---------------- sd card ---------------- */
    case V4P_CMD_SD_MOUNT: {
        bool force = (plen >= 1 && p[0] != 0);
        if (sd_fs_is_mounted()) {
            status = V4P_ST_OK;
        } else if (s_sd_attempted && !force && s_sd_last_err != ESP_OK) {
            /* Report the previous failure without touching the card again. The
             * master sets the force flag to retry after inserting a card. */
            status = V4P_ST_NO_SD;
        } else {
            status = V4P_ST_BUSY;
            *defer = DEFER_SD_MOUNT;
        }
        break;
    }

    case V4P_CMD_SD_INFO: {
        if (!sd_fs_is_mounted()) {
            status = V4P_ST_NO_SD;
            break;
        }
        uint32_t total_kb = 0, free_kb = 0;
        uint16_t sector = 0;
        uint8_t  fat = 0;
        sd_fs_err_t e = sd_fs_info(&total_kb, &free_kb, &sector, &fat);
        if (e != SD_FS_OK) {
            status = map_sd_err(e);
            break;
        }
        v4p_put_u32le(&pbuf[V4P_SD_OFF_TOTAL_KB], total_kb);
        v4p_put_u32le(&pbuf[V4P_SD_OFF_FREE_KB], free_kb);
        v4p_put_u16le(&pbuf[V4P_SD_OFF_SECTOR], sector);
        pbuf[V4P_SD_OFF_FAT_TYPE] = fat;
        plen_out = 11;
        break;
    }

    case V4P_CMD_SET_CHUNK: {
        if (plen < 2) {
            status = V4P_ST_BAD_ARG;
            break;
        }
        uint16_t want = v4p_get_u16le(p);
        if (want < V4P_CHUNK_MIN || want > V4P_CHUNK_MAX) {
            status = V4P_ST_BAD_ARG;
            break;
        }
        s_chunk = want;
        s_bulk_valid = false;
        ESP_LOGI(TAG, "chunk size set to %u", (unsigned)s_chunk);
        v4p_put_u16le(pbuf, s_chunk);
        plen_out = 2;
        break;
    }

    /* ---------------- path assembly ---------------- */
    case V4P_CMD_PATH_CLEAR:
        s_path_len = 0;
        break;

    case V4P_CMD_PATH_APPEND:
        if (plen == 0) {
            break;
        }
        if ((uint32_t)s_path_len + plen > V4P_PATH_MAX) {
            status = V4P_ST_TOO_LONG;
            break;
        }
        memcpy(&s_path[s_path_len], p, plen);
        s_path_len += plen;
        s_path[s_path_len] = '\0';
        break;

    /* ---------------- directory listing ---------------- */
    case V4P_CMD_DIR_OPEN: {
        char tmp[V4P_PATH_MAX + 1];
        const char *path = resolve_path(p, plen, tmp, sizeof(tmp));
        if (path == NULL) {
            status = V4P_ST_TOO_LONG;
            break;
        }
        /* Opening a directory is SD I/O, so it cannot happen here. First call:
         * BUSY and queue. Repeat (same command): collect the stored result, or
         * BUSY again while the worker is still running. */
        if (s_open_res.cmd == cmd && (s_open_res.valid || s_open_res.pending)) {
            if (s_open_res.valid) {
                status = s_open_res.status;
                s_open_res.valid = false;
                if (status == V4P_ST_OK) {
                    pbuf[0] = s_open_res.handle;
                    plen_out = 1;
                }
            } else {
                status = V4P_ST_BUSY;   /* do not open a second handle */
            }
            break;
        }

        strncpy(s_defer_path, path, sizeof(s_defer_path) - 1);
        s_defer_path[sizeof(s_defer_path) - 1] = '\0';
        s_open_res.cmd = cmd;
        s_open_res.seq = seq;
        s_open_res.valid = false;
        s_open_res.pending = true;
        *defer = DEFER_DIR_OPEN;
        status = V4P_ST_BUSY;
        break;
    }

    case V4P_CMD_DIR_NEXT: {
        if (plen < 3) {
            status = V4P_ST_BAD_ARG;
            break;
        }
        uint8_t  h = p[0];
        uint16_t index = v4p_get_u16le(&p[1]);
        if (h >= SD_FS_MAX_DIR_HANDLES) {
            status = V4P_ST_NO_HANDLE;
            break;
        }
        dir_entry_cache_t *c = &s_dir_cache[h];

        /* Idempotent: a repeat of an index we already hold returns the cached
         * result instead of advancing the directory cursor. The cached result
         * may be an error too - answering BUSY again for a read that failed
         * would make the master loop forever. */
        if (c->valid && c->index == index) {
            status = c->status;
            if (status == V4P_ST_OK) {
                v4p_put_u16le(&pbuf[V4P_DE_OFF_INDEX], c->index);
                pbuf[V4P_DE_OFF_ATTR] = c->attr;
                v4p_put_u32le(&pbuf[V4P_DE_OFF_SIZE], c->size);
                uint8_t nl = (uint8_t)strlen(c->name);
                pbuf[V4P_DE_OFF_NAME_LEN] = nl;
                memcpy(&pbuf[V4P_DE_OFF_NAME], c->name, nl);
                plen_out = V4P_DE_OFF_NAME + nl;
                if (c->truncated) {
                    flags |= V4P_FL_NAME_TRUNCATED;
                }
            }
            break;
        }
        if ((!c->valid && index != 0) || (c->valid && index != (uint16_t)(c->index + 1))) {
            status = V4P_ST_BAD_ARG;   /* master out of sync, re-open the dir */
            break;
        }

        /* Reading a directory entry is SD I/O and takes longer than the
         * master's t_wait, so it does NOT happen here. Answer BUSY and let the
         * worker fill the cache; the repeat above then serves it from RAM. */
        if (c->pending && c->pending_index == index) {
            status = V4P_ST_BUSY;      /* already queued, do not advance twice */
            break;
        }
        c->pending = true;
        c->pending_index = index;
        s_defer_handle = h;
        s_defer_index = index;
        *defer = DEFER_DIR_LOAD;
        status = V4P_ST_BUSY;
        break;
    }

    case V4P_CMD_DIR_CLOSE: {
        if (plen < 1) {
            status = V4P_ST_BAD_ARG;
            break;
        }
        uint8_t h = p[0];
        if (h < SD_FS_MAX_DIR_HANDLES) {
            memset(&s_dir_cache[h], 0, sizeof(s_dir_cache[h]));
        }
        /* A collected-but-uncollected open result must not survive the close,
         * or a later DIR_OPEN would hand out a handle that is already gone. */
        s_open_res.valid = false;
        sd_fs_dir_close(h);
        break;
    }

    /* ---------------- file access ---------------- */
    case V4P_CMD_FILE_OPEN: {
        char tmp[V4P_PATH_MAX + 1];
        const char *path = resolve_path(p, plen, tmp, sizeof(tmp));
        if (path == NULL) {
            status = V4P_ST_TOO_LONG;
            break;
        }
        /* See DIR_OPEN: the FAT lookup is I/O and moves to the worker. The
         * stored-result pattern also makes the open idempotent, so an unsafe
         * retry can no longer occupy a second handle. */
        if (s_open_res.cmd == cmd && (s_open_res.valid || s_open_res.pending)) {
            if (s_open_res.valid) {
                status = s_open_res.status;
                s_open_res.valid = false;
                if (status == V4P_ST_OK) {
                    pbuf[V4P_FO_OFF_HANDLE] = s_open_res.handle;
                    v4p_put_u32le(&pbuf[V4P_FO_OFF_SIZE], s_open_res.size);
                    pbuf[V4P_FO_OFF_ATTR] = V4P_ATTR_ARCHIVE;
                    plen_out = 6;
                }
            } else {
                status = V4P_ST_BUSY;   /* do not open a second handle */
            }
            break;
        }

        strncpy(s_defer_path, path, sizeof(s_defer_path) - 1);
        s_defer_path[sizeof(s_defer_path) - 1] = '\0';
        s_open_res.cmd = cmd;
        s_open_res.seq = seq;
        s_open_res.valid = false;
        s_open_res.pending = true;
        *defer = DEFER_FILE_OPEN;
        status = V4P_ST_BUSY;
        break;
    }

    case V4P_CMD_FILE_READ: {
        if (plen < 3) {
            status = V4P_ST_BAD_ARG;
            break;
        }
        uint8_t  h = p[0];
        uint16_t block = v4p_get_u16le(&p[1]);

        if (!sd_fs_is_mounted()) {
            *out_len = v4p_bframe_build(out, cmd, V4P_ST_NO_SD, h, block, NULL, 0, s_chunk);
            return;
        }

        if (s_bulk_valid && s_bulk_handle == h && s_bulk_block == block) {
            *out_len = v4p_bframe_build(out, cmd, s_bulk_status, h, block,
                                        s_bulk_cache, s_bulk_len, s_chunk);
            return;
        }

        /* Not cached: answer BUSY now, load the block after replying. */
        s_defer_handle = h;
        s_defer_block = block;
        *defer = DEFER_BULK_LOAD;
        *out_len = v4p_bframe_build(out, cmd, V4P_ST_BUSY, h, block, NULL, 0, s_chunk);
        return;
    }

    case V4P_CMD_FILE_CLOSE: {
        if (plen < 1) {
            status = V4P_ST_BAD_ARG;
            break;
        }
        sd_fs_file_close(p[0]);
        s_bulk_valid = false;
        s_open_res.valid = false;   /* see DIR_CLOSE */
        break;
    }

    /* ---------------- SD -> Bluetooth playback ---------------- */
    case V4P_CMD_PLAY_FILE: {
        char full[SD_FS_PATH_BUF];
        char tmp[V4P_PATH_MAX + 1];

        const char *path = resolve_path(p, plen, tmp, sizeof(tmp));
        if (path == NULL) {
            status = V4P_ST_TOO_LONG;
            break;
        }
        if (sd_fs_resolve_path(path, full, sizeof(full)) != SD_FS_OK) {
            status = V4P_ST_BAD_ARG;      /* ".." or path too long */
            break;
        }
        if (!sd_fs_is_mounted()) {
            status = V4P_ST_NO_SD;
            break;
        }

        /* Already playing exactly this file? Then the earlier BUSY already took
         * effect and this repeat is the confirmation the master is waiting for. */
        if (audio_source_get() == AUDIO_SOURCE_SD) {
            const char *cur = audio_source_current_path();
            if (same_media_path(cur, full)) {   /* URI gegen VFS-Pfad, siehe oben */
                status = V4P_ST_OK;
                break;
            }
        }

        /* A failed switch is reported on the repeat rather than looping BUSY. */
        if (s_last_play_err != V4P_ST_OK && strcmp(s_last_play_path, full) == 0) {
            status = s_last_play_err;
            break;
        }

        /* A repeat while the worker is still rebuilding: BUSY, but do not queue
         * a second rebuild. */
        if (s_play_pending && strcmp(s_play_pending_path, full) == 0) {
            status = V4P_ST_BUSY;
            break;
        }

        /* No existence check here any more: that was SD I/O inside the answering
         * path, and it took a file handle on *every* attempt. It now runs in
         * DEFER_SD_PLAY, where a retry cannot consume another handle. */
        strncpy(s_defer_path, full, sizeof(s_defer_path) - 1);
        s_defer_path[sizeof(s_defer_path) - 1] = '\0';
        /* Keep the relative form as well: the existence check in DEFER_SD_PLAY
         * hands it to sd_fs_file_open(), which resolves relative to the mount. */
        strncpy(s_defer_raw, path, sizeof(s_defer_raw) - 1);
        s_defer_raw[sizeof(s_defer_raw) - 1] = '\0';
        strncpy(s_play_pending_path, full, sizeof(s_play_pending_path) - 1);
        s_play_pending_path[sizeof(s_play_pending_path) - 1] = '\0';
        s_play_pending = true;
        s_last_play_err = V4P_ST_OK;

        ESP_LOGI(TAG, "PLAY_FILE request: %s", full);
        status = V4P_ST_BUSY;
        *defer = DEFER_SD_PLAY;
        break;
    }

    case V4P_CMD_STOP_PLAY:
        if (audio_source_get() != AUDIO_SOURCE_SD) {
            status = V4P_ST_OK;           /* nothing is playing */
            break;
        }
        status = V4P_ST_BUSY;
        *defer = DEFER_SD_STOP;
        break;

    case V4P_CMD_EQ_INFO: {
        int bands = 0, active = 0;
        if (stream_proc_eq_info(&bands, &active) != 0) {
            status = V4P_ST_BAD_STATE;      /* kein Equalizer im Pool */
            break;
        }
        pbuf[0] = (uint8_t)bands;
        pbuf[1] = (uint8_t)active;
        pbuf[2] = V4P_EQ_BAND_LEN;          /* Selbstauskunft des Bandformats */
        pbuf[3] = 0;
        plen_out = 4;
        break;
    }

    case V4P_CMD_EQ_BANDS: {
        if (plen < 1) {
            status = V4P_ST_BAD_ARG;
            break;
        }
        int n = stream_proc_eq_set_bands((int)p[0]);
        if (n < 0) {
            status = V4P_ST_BAD_ARG;
            break;
        }
        pbuf[0] = (uint8_t)n;
        plen_out = 1;
        break;
    }

    case V4P_CMD_EQ_GET: {
        int      typ = 0;
        unsigned fc  = 0;
        float    q = 0.0f, gain = 0.0f;
        int      bands = 0, active = 0;

        if (plen < 1) {
            status = V4P_ST_BAD_ARG;
            break;
        }
        if (stream_proc_eq_get((int)p[0], &typ, &fc, &q, &gain) != 0) {
            status = V4P_ST_BAD_ARG;
            break;
        }
        (void)stream_proc_eq_info(&bands, &active);

        /*
         * Q und Gain als Ganzzahlen (Q x 100, dB x 10): die V4 ist Big Endian,
         * der ESP32 Little Endian - so gibt es keinen Fliesskomma-Austausch.
         */
        memset(pbuf, 0, V4P_EQ_BAND_LEN);
        pbuf[V4P_EQ_OFF_IDX]     = p[0];
        pbuf[V4P_EQ_OFF_TYP]     = (uint8_t)typ;
        pbuf[V4P_EQ_OFF_ENABLED] = ((int)p[0] < active) ? 1 : 0;
        v4p_put_u32le(&pbuf[V4P_EQ_OFF_FC], fc);
        v4p_put_u16le(&pbuf[V4P_EQ_OFF_Q], (uint16_t)(int16_t)(q * 100.0f + (q >= 0 ? 0.5f : -0.5f)));
        v4p_put_u16le(&pbuf[V4P_EQ_OFF_GAIN],
                      (uint16_t)(int16_t)(gain * 10.0f + (gain >= 0 ? 0.5f : -0.5f)));
        plen_out = V4P_EQ_BAND_LEN;
        break;
    }

    case V4P_CMD_EQ_SET: {
        if (plen < V4P_EQ_BAND_LEN - 2) {   /* idx,typ,fc,q,gain = 10 Byte */
            status = V4P_ST_BAD_ARG;
            break;
        }
        uint8_t  idx  = p[0];
        uint8_t  typ  = p[1];
        uint32_t fc   = v4p_get_u32le(&p[2]);
        int16_t  q100 = (int16_t)v4p_get_u16le(&p[6]);
        int16_t  g10  = (int16_t)v4p_get_u16le(&p[8]);

        if (stream_proc_eq_set((int)idx, (int)typ, (unsigned)fc,
                               (float)q100 / 100.0f, (float)g10 / 10.0f) < 0) {
            status = V4P_ST_BAD_ARG;
            break;
        }
        break;                              /* OK, kein Nutzdatenteil */
    }

    case V4P_CMD_MEDIA_START: {
        if (bt_mgr_audio_streaming()) {
            s_media_err = V4P_ST_OK;
            status = V4P_ST_OK;               /* laeuft bereits */
            break;
        }
        if (s_media_pending) {
            status = V4P_ST_BUSY;             /* der erste Versuch laeuft noch */
            break;
        }
        if (s_media_err != V4P_ST_BUSY) {
            /* Ergebnis der letzten Anforderung einmal melden, danach wieder
             * einen frischen Versuch zulassen. */
            status = s_media_err;
            s_media_err = V4P_ST_BUSY;
            break;
        }
        s_media_pending = true;
        *defer = DEFER_MEDIA_START;
        status = V4P_ST_BUSY;
        break;
    }

    case V4P_CMD_RESET:
        s_path_len = 0;
        s_bulk_valid = false;
        s_open_res.valid = false;
        s_open_res.pending = false;
        s_play_pending = false;
        ESP_LOGI(TAG, "protocol reset requested");
        break;

    default:
        ESP_LOGW(TAG, "unknown command 0x%02x", (unsigned)cmd);
        status = V4P_ST_BAD_CMD;
        break;
    }

    uint8_t *pl = v4p_rframe_begin(out, cmd, seq, (uint8_t)status, flags);
    if (plen_out > 0 && pl != NULL) {
        memcpy(pl, pbuf, plen_out);
    }
    v4p_rframe_finish(out, plen_out);
    *out_len = V4P_READ_FRAME_LEN;
}

/* ------------------------------------------------------------------ */
/* Frame handling                                                     */
/* ------------------------------------------------------------------ */

static void process_frame(const uint8_t *data, size_t got)
{
    uint8_t *out = s_resp;
    size_t  out_len = 0;
    uint8_t cmd = 0, seq = 0, plen = 0;
    defer_op_t defer = DEFER_NONE;

    if (!v4p_wframe_check(data, got, &cmd, &seq, &plen)) {
        uint8_t *pl = v4p_rframe_begin(out, 0, 0, V4P_ST_BAD_CRC, V4P_FL_NONE);
        (void)pl;
        v4p_rframe_finish(out, 0);
        write_response(out, V4P_READ_FRAME_LEN);
        s_rx_bad++;
        /* The first bytes say what actually arrived: all-zero means the master
         * read or wrote an empty buffer, a plausible magic in the wrong place
         * means a shifted stream. */
        ESP_LOGW(TAG, "rejected request frame (%u bytes): %02X %02X %02X %02X ...",
                 (unsigned)got,
                 (got > 0) ? data[0] : 0, (got > 1) ? data[1] : 0,
                 (got > 2) ? data[2] : 0, (got > 3) ? data[3] : 0);
        return;
    }

    dispatch(cmd, seq, &data[V4P_W_DATA], plen, out, &out_len, &defer);

    if (out_len > 0) {
        write_response(out, out_len);
    }

    /* Slow work runs after the response is queued AND in a different task.
     * Running it here kept the protocol task from answering the next command,
     * which is how answers ended up arriving after the master had already
     * given up and read stale bytes. */
    if (defer != DEFER_NONE) {
        work_msg_t w = work_from_handoff(defer, seq);
        if (xQueueSend(s_work_queue, &w, 0) != pdTRUE) {
            /* Clear the "queued" markers as well, or the command would answer
             * BUSY forever because nothing will ever complete the work. The
             * master simply repeats and queues it again. */
            ESP_LOGW(TAG, "work queue full, deferred op %d dropped", (int)defer);
            s_play_pending = false;
            s_open_res.pending = false;
            if (s_defer_handle < SD_FS_MAX_DIR_HANDLES) {
                s_dir_cache[s_defer_handle].pending = false;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Built-in self test                                                 */
/* ------------------------------------------------------------------ */

/*
 * Bring-up aid: drive the real command dispatch with synthetic frames so the
 * whole protocol path can be verified on hardware BEFORE the V4 master software
 * exists. Covered: WRITE frame build -> frame validation -> dispatch ->
 * response frame build -> response validation, including the BUSY/deferred
 * round trip and the bulk frame.
 *
 * Only the I2C transport itself is not exercised.
 */

typedef struct {
    uint8_t        status;
    uint8_t        flags;
    uint16_t       len;         /* payload bytes, or valid data bytes for bulk  */
    bool           is_bulk;
    size_t         frame_len;
    const uint8_t *payload;     /* points into s_resp, valid until next st_run  */
} st_result_t;

static st_result_t st_run(uint8_t cmd, const uint8_t *args, uint8_t arg_len, uint8_t seq)
{
    st_result_t res;
    uint8_t wf[V4P_WRITE_FRAME_LEN];
    uint8_t rcmd = 0, rseq = 0, rlen = 0;

    memset(&res, 0, sizeof(res));
    /* Invalid sentinel: a zeroed struct would look like V4P_ST_OK with length 0,
     * so every failure path below returns this instead. */
    res.status = 0xFF;

    v4p_wframe_build(wf, cmd, seq, args, arg_len);
    if (!v4p_wframe_check(wf, sizeof(wf), &rcmd, &rseq, &rlen)) {
        ESP_LOGE(TAG, "SELFTEST: own write frame for cmd 0x%02x fails validation", (unsigned)cmd);
        return res;
    }

    /* Up to 4 rounds, mirroring what the real master does: on BUSY the whole
     * command is sent again with the same sequence number. */
    for (int round = 0; round < 4; round++) {
        size_t     out_len = 0;
        defer_op_t defer = DEFER_NONE;

        dispatch(rcmd, rseq, &wf[V4P_W_DATA], rlen, s_resp, &out_len, &defer);
        if (defer != DEFER_NONE) {
            /* The self test runs on the protocol task and needs the result
             * before its next round, so it performs the work inline instead of
             * handing it to the worker. */
            work_msg_t w = work_from_handoff(defer, rseq);
            run_deferred(&w);
        }

        if (out_len == V4P_READ_FRAME_LEN) {
            uint8_t status = 0, len = 0, flags = 0;
            if (!v4p_rframe_check(s_resp, V4P_READ_FRAME_LEN, cmd, seq, &status, &len, &flags)) {
                ESP_LOGE(TAG, "SELFTEST: response to cmd 0x%02x fails validation", (unsigned)cmd);
                return res;
            }
            if (status == V4P_ST_BUSY) {
                continue;
            }
            res.status = status;
            res.len = len;
            res.flags = flags;
            res.frame_len = out_len;
            res.is_bulk = false;
            res.payload = &s_resp[V4P_R_DATA];
            return res;
        }

        if (v4p_bframe_check(s_resp, out_len, s_chunk) == 0) {
            ESP_LOGE(TAG, "SELFTEST: bulk response to cmd 0x%02x fails validation", (unsigned)cmd);
            return res;
        }
        uint8_t status = s_resp[V4P_B_STATUS];
        if (status == V4P_ST_BUSY) {
            continue;
        }
        res.status = status;
        res.len = v4p_get_u16le(&s_resp[V4P_B_LEN]);
        res.frame_len = out_len;
        res.is_bulk = true;
        res.payload = &s_resp[V4P_B_DATA];
        return res;
    }

    ESP_LOGW(TAG, "SELFTEST: cmd 0x%02x never settled (still BUSY)", (unsigned)cmd);
    return res;
}

/** @brief Short status name for the self-test log (no dependency on the master lib). */
static const char *st_status_name(uint8_t status)
{
    switch (status) {
    case V4P_ST_OK:        return "OK";
    case V4P_ST_BUSY:      return "BUSY";
    case V4P_ST_BAD_CRC:   return "BAD_CRC";
    case V4P_ST_BAD_CMD:   return "BAD_CMD";
    case V4P_ST_BAD_ARG:   return "BAD_ARG";
    case V4P_ST_BAD_STATE: return "BAD_STATE";
    case V4P_ST_NO_SD:     return "NO_SD";
    case V4P_ST_NOT_FOUND: return "NOT_FOUND";
    case V4P_ST_IO_ERR:    return "IO_ERR";
    case V4P_ST_END:       return "END";
    case V4P_ST_TOO_LONG:  return "TOO_LONG";
    case V4P_ST_NO_HANDLE: return "NO_HANDLE";
    case V4P_ST_BT_ERR:    return "BT_ERR";
    default:               return "?";
    }
}

/** @brief Does the name look like something the auto decoder can play? */
static bool st_is_audio_name(const char *name)
{
    static const char *ext[] = {
        ".mp3", ".wav", ".flac", ".ogg", ".opus", ".aac", ".m4a", ".pcm"
    };
    size_t n = strlen(name);

    for (unsigned i = 0; i < sizeof(ext) / sizeof(ext[0]); i++) {
        size_t e = strlen(ext[i]);
        if (n <= e) {
            continue;
        }
        size_t k = 0;
        while (k < e && tolower((unsigned char)name[n - e + k]) == ext[i][k]) {
            k++;
        }
        if (k == e) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Open a path the way the master MUST for long names.
 *
 * A command payload holds only V4P_WRITE_PAYLOAD_MAX (27) bytes, so any longer
 * path has to go through PATH_CLEAR + PATH_APPEND and the command is then sent
 * with an empty payload. Music file names exceed 27 bytes easily, so this is the
 * normal case, not an edge case - and it exercises the path assembly code.
 */
static bool st_open_path(uint8_t cmd, const char *path, uint8_t seq, st_result_t *out)
{
    st_result_t r = st_run(V4P_CMD_PATH_CLEAR, NULL, 0, seq);
    if (r.status != V4P_ST_OK) {
        ESP_LOGW(TAG, "SELFTEST PATH_CLEAR status=%s", st_status_name(r.status));
        return false;
    }

    size_t      left = strlen(path);
    const char *p = path;
    uint8_t     s = (uint8_t)(seq + 1);

    while (left > 0) {
        uint8_t frag = (left > (size_t)V4P_WRITE_PAYLOAD_MAX)
                       ? (uint8_t)V4P_WRITE_PAYLOAD_MAX : (uint8_t)left;
        r = st_run(V4P_CMD_PATH_APPEND, (const uint8_t *)p, frag, s++);
        if (r.status != V4P_ST_OK) {
            ESP_LOGW(TAG, "SELFTEST PATH_APPEND status=%s", st_status_name(r.status));
            return false;
        }
        p += frag;
        left -= frag;
    }

    *out = st_run(cmd, NULL, 0, s);
    return true;
}

static volatile bool s_selftest_done;
static void st_run_all(void);

/**
 * @brief Request the built-in self test and wait until it has finished.
 *
 * The test runs ON THE PROTOCOL TASK, not on the caller. The command handlers
 * nest deeply (dispatch -> sd_fs -> VFS -> FatFs), and this task was sized for
 * exactly that workload, while app_main only has CONFIG_ESP_MAIN_TASK_STACK_SIZE
 * (3584 bytes here). Running it here also proves the stack is sufficient for the
 * real SD command path.
 */
void v4_link_selftest(void)
{
    rx_msg_t msg;

    if (s_rx_queue == NULL) {
        ESP_LOGW(TAG, "self test skipped: protocol task not started yet");
        return;
    }

    memset(&msg, 0, sizeof(msg));
    msg.kind = RX_KIND_SELFTEST;
    s_selftest_done = false;

    if (xQueueSend(s_rx_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "self test request could not be queued");
        return;
    }

    for (int i = 0; i < 300 && !s_selftest_done; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!s_selftest_done) {
        ESP_LOGW(TAG, "self test did not finish within 6 s");
    }
}

static void st_run_all(void)
{
    ESP_LOGI(TAG, "=== SELFTEST BEGINN (ohne I2C, ueber den echten Dispatch) ===");

    /* --- 1. Identifikation --- */
    st_result_t r = st_run(V4P_CMD_GET_INFO, NULL, 0, 0x01);
    if (r.status == V4P_ST_OK && r.len >= 12) {
        ESP_LOGI(TAG, "SELFTEST GET_INFO: proto=%u fw=%u frames=%u/%u chunk=%u dev_max=%u",
                 r.payload[0], r.payload[1], r.payload[2], r.payload[3],
                 (unsigned)v4p_get_u16le(&r.payload[6]), r.payload[8]);
    } else {
        ESP_LOGE(TAG, "SELFTEST GET_INFO fehlgeschlagen (status=%s)", st_status_name(r.status));
    }

    /* --- 2. Status inkl. Card Detect --- */
    r = st_run(V4P_CMD_GET_STATUS, NULL, 0, 0x02);
    if (r.status == V4P_ST_OK && r.len >= V4P_ST_PAYLOAD_LEN) {
        ESP_LOGI(TAG, "SELFTEST GET_STATUS: bt_state=%u devices=%u scan=%u sd_mounted=%u sd_card=%u free_kb=%u",
                 r.payload[V4P_ST_OFF_STATE], r.payload[V4P_ST_OFF_DEV_COUNT],
                 r.payload[V4P_ST_OFF_SCAN_ACTIVE], r.payload[V4P_ST_OFF_SD_MOUNTED],
                 r.payload[V4P_ST_OFF_SD_CARD],
                 (unsigned)v4p_get_u32le(&r.payload[V4P_ST_OFF_SD_FREE_KB]));
    } else {
        ESP_LOGE(TAG, "SELFTEST GET_STATUS fehlgeschlagen (status=%s)", st_status_name(r.status));
    }

    /* --- 3. Wurzelverzeichnis --- */
    /* Big enough for "dir/name": a path can be longer than one command payload,
     * which is exactly why PATH_CLEAR/PATH_APPEND exist. */
    char file_name[2 * V4P_NAME_MAX + 4];
    char dir_name[V4P_NAME_MAX + 1];
    char audio_name[2 * V4P_NAME_MAX + 4];
    file_name[0] = '\0';
    dir_name[0] = '\0';
    audio_name[0] = '\0';

    r = st_run(V4P_CMD_DIR_OPEN, NULL, 0, 0x10);   /* leere Nutzlast = "/" */
    if (r.status != V4P_ST_OK || r.len < 1) {
        ESP_LOGE(TAG, "SELFTEST DIR_OPEN(\"/\") fehlgeschlagen (status=%s)", st_status_name(r.status));
        ESP_LOGI(TAG, "=== SELFTEST ENDE (SD nicht nutzbar) ===");
        return;
    }
    int root_handle = r.payload[0];
    ESP_LOGI(TAG, "SELFTEST DIR_OPEN(\"/\") -> handle %d", root_handle);

    /* Zweiter Durchlauf sammelt auch den ersten Verzeichnisnamen ein. */
    for (int i = 0; i < 64; i++) {
        uint8_t args[3];
        args[0] = (uint8_t)root_handle;
        v4p_put_u16le(&args[1], (uint16_t)i);
        st_result_t e = st_run(V4P_CMD_DIR_NEXT, args, 3, (uint8_t)(0x20 + i));
        if (e.status == V4P_ST_END) {
            ESP_LOGI(TAG, "SELFTEST   (%d Eintraege in \"/\")", i);
            break;
        }
        if (e.status != V4P_ST_OK || e.len < V4P_DE_OFF_NAME) {
            ESP_LOGW(TAG, "SELFTEST   DIR_NEXT(%d) status=%s", i, st_status_name(e.status));
            break;
        }
        uint8_t nl = e.payload[V4P_DE_OFF_NAME_LEN];
        if (nl > V4P_NAME_MAX || e.len < (uint8_t)(V4P_DE_OFF_NAME + nl)) {
            ESP_LOGW(TAG, "SELFTEST   DIR_NEXT(%d) malformed", i);
            break;
        }
        char name[V4P_NAME_MAX + 1];
        memcpy(name, &e.payload[V4P_DE_OFF_NAME], nl);
        name[nl] = '\0';
        uint8_t  attr = e.payload[V4P_DE_OFF_ATTR];
        uint32_t size = v4p_get_u32le(&e.payload[V4P_DE_OFF_SIZE]);

        ESP_LOGI(TAG, "SELFTEST   [%d] %-32s %s %u", i, name,
                 (attr & V4P_ATTR_DIR) ? "<DIR>" : "     ", (unsigned)size);

        if ((attr & V4P_ATTR_DIR) && dir_name[0] == '\0') {
            strncpy(dir_name, name, sizeof(dir_name) - 1);
        }
        if (!(attr & V4P_ATTR_DIR) && file_name[0] == '\0') {
            strncpy(file_name, name, sizeof(file_name) - 1);
        }
        if (!(attr & V4P_ATTR_DIR) && audio_name[0] == '\0' && st_is_audio_name(name)) {
            strncpy(audio_name, name, sizeof(audio_name) - 1);
        }

        if (i == 1) {
            st_result_t again = st_run(V4P_CMD_DIR_NEXT, args, 3, (uint8_t)(0x20 + i));
            if (again.status == V4P_ST_OK && again.len >= V4P_DE_OFF_NAME) {
                uint8_t nl2 = again.payload[V4P_DE_OFF_NAME_LEN];
                if (nl2 == nl && memcmp(&again.payload[V4P_DE_OFF_NAME],
                                        &e.payload[V4P_DE_OFF_NAME], nl) == 0) {
                    ESP_LOGI(TAG, "SELFTEST   Retry auf Index 1 -> derselbe Eintrag: OK");
                } else {
                    ESP_LOGE(TAG, "SELFTEST   Retry auf Index 1 -> ANDERER Eintrag!");
                }
            }
        }
    }

    /* --- 4. Falls in der Wurzel keine Datei liegt: erstes Unterverzeichnis --- */
    if (file_name[0] == '\0' && dir_name[0] != '\0') {
        ESP_LOGI(TAG, "SELFTEST keine Datei in \"/\", gehe in \"%s\"", dir_name);

        r = st_run(V4P_CMD_DIR_CLOSE, (const uint8_t[]){ (uint8_t)root_handle }, 1, 0x30);
        (void)r;

        if (!st_open_path(V4P_CMD_DIR_OPEN, dir_name, 0x31, &r)) {
            r.status = 0xFF;
        }
        if (r.status == V4P_ST_OK && r.len >= 1) {
            int sub = r.payload[0];
            ESP_LOGI(TAG, "SELFTEST DIR_OPEN(\"%s\") -> handle %d", dir_name, sub);

            for (int i = 0; i < 64; i++) {
                uint8_t args[3];
                args[0] = (uint8_t)sub;
                v4p_put_u16le(&args[1], (uint16_t)i);
                st_result_t e = st_run(V4P_CMD_DIR_NEXT, args, 3, (uint8_t)(0x50 + i));
                if (e.status == V4P_ST_END) {
                    ESP_LOGI(TAG, "SELFTEST   (%d Eintraege)", i);
                    break;
                }
                if (e.status != V4P_ST_OK || e.len < V4P_DE_OFF_NAME) {
                    break;
                }
                uint8_t nl = e.payload[V4P_DE_OFF_NAME_LEN];
                if (nl > V4P_NAME_MAX) {
                    break;
                }
                char name[V4P_NAME_MAX + 1];
                memcpy(name, &e.payload[V4P_DE_OFF_NAME], nl);
                name[nl] = '\0';
                uint8_t attr = e.payload[V4P_DE_OFF_ATTR];
                ESP_LOGI(TAG, "SELFTEST   [%d] %-32s %s %u", i, name,
                         (attr & V4P_ATTR_DIR) ? "<DIR>" : "     ",
                         (unsigned)v4p_get_u32le(&e.payload[V4P_DE_OFF_SIZE]));
                if (!(attr & V4P_ATTR_DIR) && file_name[0] == '\0') {
                    snprintf(file_name, sizeof(file_name), "%s/%s", dir_name, name);
                }
                if (!(attr & V4P_ATTR_DIR) && audio_name[0] == '\0' && st_is_audio_name(name)) {
                    snprintf(audio_name, sizeof(audio_name), "%s/%s", dir_name, name);
                }
            }
            st_run(V4P_CMD_DIR_CLOSE, (const uint8_t[]){ (uint8_t)sub }, 1, 0x60);
        } else {
            ESP_LOGW(TAG, "SELFTEST DIR_OPEN(\"%s\") status=%s", dir_name, st_status_name(r.status));
        }
    } else {
        st_run(V4P_CMD_DIR_CLOSE, (const uint8_t[]){ (uint8_t)root_handle }, 1, 0x30);
    }

    /* --- 5. Datei oeffnen und lesen --- */
    if (file_name[0] == '\0') {
        ESP_LOGW(TAG, "SELFTEST keine Datei gefunden - Lesetest uebersprungen");
    } else {
        if (!st_open_path(V4P_CMD_FILE_OPEN, file_name, 0x70, &r)) {
            r.status = 0xFF;
        }
        if (r.status != V4P_ST_OK || r.len < 6) {
            ESP_LOGE(TAG, "SELFTEST FILE_OPEN(\"%s\") status=%s", file_name,
                     st_status_name(r.status));
        } else {
            uint8_t  fh = r.payload[V4P_FO_OFF_HANDLE];
            uint32_t fsize = v4p_get_u32le(&r.payload[V4P_FO_OFF_SIZE]);
            ESP_LOGI(TAG, "SELFTEST FILE_OPEN(\"%s\") -> handle %u, %u Byte", file_name,
                     (unsigned)fh, (unsigned)fsize);

            /* Block 0 mit der aktuellen Chunk-Groesse */
            uint8_t args[3];
            args[0] = fh;
            v4p_put_u16le(&args[1], 0);
            r = st_run(V4P_CMD_FILE_READ, args, 3, 0x71);
            if (r.status == V4P_ST_OK && r.is_bulk) {
                ESP_LOGI(TAG, "SELFTEST FILE_READ(block 0): %u gueltige Byte, Frame %u Byte, CRC ok",
                         (unsigned)r.len, (unsigned)r.frame_len);
            } else {
                ESP_LOGE(TAG, "SELFTEST FILE_READ(block 0) status=%s bulk=%d",
                         st_status_name(r.status), (int)r.is_bulk);
            }

            /* Block hinter dem Dateiende muss END liefern */
            uint16_t last_block = (uint16_t)(fsize / s_chunk + 1);
            args[0] = fh;
            v4p_put_u16le(&args[1], last_block);
            r = st_run(V4P_CMD_FILE_READ, args, 3, 0x72);
            ESP_LOGI(TAG, "SELFTEST FILE_READ(block %u, hinterm Ende): status=%s (erwartet END)",
                     (unsigned)last_block, st_status_name(r.status));

            /* Chunk-Groesse aendern und pruefen, dass die Framelaenge mitgeht */
            uint8_t chunk_args[2];
            v4p_put_u16le(chunk_args, 256);
            r = st_run(V4P_CMD_SET_CHUNK, chunk_args, 2, 0x73);
            if (r.status == V4P_ST_OK && r.len >= 2) {
                ESP_LOGI(TAG, "SELFTEST SET_CHUNK(256) -> bestaetigt %u", (unsigned)v4p_get_u16le(r.payload));

                args[0] = fh;
                v4p_put_u16le(&args[1], 0);
                r = st_run(V4P_CMD_FILE_READ, args, 3, 0x74);
                if (r.is_bulk && r.frame_len == (size_t)(V4P_BULK_HDR_LEN + 256 + V4P_BULK_CRC_LEN)) {
                    ESP_LOGI(TAG, "SELFTEST FILE_READ mit chunk=256: Frame %u Byte (%u gueltig) - OK",
                             (unsigned)r.frame_len, (unsigned)r.len);
                } else {
                    ESP_LOGE(TAG, "SELFTEST FILE_READ mit chunk=256: Frame %u Byte, status=%s",
                             (unsigned)r.frame_len, st_status_name(r.status));
                }

                v4p_put_u16le(chunk_args, V4P_CHUNK_DEFAULT);
                st_run(V4P_CMD_SET_CHUNK, chunk_args, 2, 0x75);
            } else {
                ESP_LOGE(TAG, "SELFTEST SET_CHUNK(256) status=%s", st_status_name(r.status));
            }

            st_run(V4P_CMD_FILE_CLOSE, (const uint8_t[]){ fh }, 1, 0x76);
        }
    }

    /* --- 6. PLAY_FILE / STOP_PLAY, sofern eine Audiodatei auf der Karte liegt --- */
    if (audio_name[0] == '\0' && sd_fs_is_mounted()) {
        /* The scans above are limited, and a freshly copied file sorts to the end
         * of a directory, so on a card with many entries it can fall outside the
         * limit. Look for the test files we hand out directly as well.
         *
         * The 48 kHz tone is tried first on purpose: Bluedroid's A2DP source is
         * hard-wired to 44100 Hz (btc_a2dp_source.c), so only a file with a
         * different rate can expose the mismatch. The 44.1 kHz tone always
         * matches and therefore proves nothing about it. */
        static const char *probes[] = { "test_tone_48k.wav", "test_tone_440.wav" };
        for (unsigned pi = 0; pi < sizeof(probes) / sizeof(probes[0]); pi++) {
            uint32_t probe_size = 0;
            int ph = sd_fs_file_open(probes[pi], &probe_size);
            if (ph >= 0) {
                sd_fs_file_close(ph);
                strncpy(audio_name, probes[pi], sizeof(audio_name) - 1);
                audio_name[sizeof(audio_name) - 1] = '\0';
                break;
            }
        }
    }

    if (audio_name[0] == '\0') {
        ESP_LOGI(TAG, "SELFTEST keine Audiodatei auf der Karte - PLAY_FILE-Test uebersprungen");
    } else {
        ESP_LOGI(TAG, "SELFTEST PLAY_FILE(\"%s\")", audio_name);
        if (!st_open_path(V4P_CMD_PLAY_FILE, audio_name, 0x90, &r)) {
            r.status = 0xFF;
        }
        ESP_LOGI(TAG, "SELFTEST PLAY_FILE -> status=%s, Quelle=%s",
                 st_status_name(r.status),
                 (audio_source_get() == AUDIO_SOURCE_SD) ? "SD" : "I2S");

        if (audio_source_get() == AUDIO_SOURCE_SD) {
            /* Kurz laufen lassen, damit die Kette wirklich Daten zieht, dann
             * zurueck auf I2S - sonst waere der Audio-Pfad nach dem Selbsttest
             * nicht mehr der urspruengliche. */
            vTaskDelay(pdMS_TO_TICKS(500));
            r = st_run(V4P_CMD_STOP_PLAY, NULL, 0, 0x93);
            ESP_LOGI(TAG, "SELFTEST STOP_PLAY -> status=%s, Quelle=%s",
                     st_status_name(r.status),
                     (audio_source_get() == AUDIO_SOURCE_SD) ? "SD" : "I2S");
        }
    }

    /* --- 7. Fehlerfaelle, die BAD_ARG liefern muessen --- */    r = st_run(V4P_CMD_FILE_OPEN, (const uint8_t *)"..", 2, 0x80);
    ESP_LOGI(TAG, "SELFTEST Pfad \"..\" -> status=%s (erwartet BAD_ARG)",
             st_status_name(r.status));

    r = st_run(V4P_CMD_DIR_NEXT, (const uint8_t[]){ 9, 0, 0 }, 3, 0x81);
    ESP_LOGI(TAG, "SELFTEST DIR_NEXT mit ungueltigem Handle -> status=%s (erwartet NO_HANDLE)",
             st_status_name(r.status));

    ESP_LOGI(TAG, "=== SELFTEST ENDE ===");
}

/* ------------------------------------------------------------------ */
/* I2C plumbing                                                       */
/* ------------------------------------------------------------------ */

static bool v4_on_receive(i2c_slave_dev_handle_t dev,
                          const i2c_slave_rx_done_event_data_t *evt,
                          void *arg)
{
    BaseType_t hp = pdFALSE;
    rx_msg_t msg;

    if (evt == NULL || evt->buffer == NULL) {
        return false;
    }

    size_t len = evt->length;
    if (len > V4P_WRITE_FRAME_LEN) {
        len = V4P_WRITE_FRAME_LEN;
    }
    memset(msg.data, 0, sizeof(msg.data));
    memcpy(msg.data, evt->buffer, len);
    msg.kind = RX_KIND_FRAME;
    msg.len = (uint8_t)len;
    /* ISR context: esp_timer_get_time() is the one esp_timer call allowed here
     * (esp_timer.h: "only esp_timer_get_time() function can be used"). */
    msg.t_rx_us = (uint32_t)esp_timer_get_time();
    s_rx_count++;

    xQueueSendFromISR(s_rx_queue, &msg, &hp);
    return (hp == pdTRUE);
}

/* Called from the metrics tick. Without it the log is equally silent when the
 * wiring is dead and when the master is simply not talking - and those need
 * opposite fixes. The internal pull-ups are enabled, so an idle line is HIGH:
 * a 0 here means something external is holding it down (wrong header pin,
 * short, or a stuck device). */
void v4_link_bus_report(void)
{
    ESP_LOGI(TAG, "BUS SDA=%d SCL=%d (Leerlauf muss 1/1 sein)  rx_gesamt=%u  verworfen=%u",
             gpio_get_level(V4_I2C_SDA_IO), gpio_get_level(V4_I2C_SCL_IO),
             (unsigned)s_rx_count, (unsigned)s_rx_bad);
    /* Kleinster Rest seit dem Start, in Byte. Nahe 0 heisst: Stack zu knapp. */
    if (s_link_task_h != NULL) {
        ESP_LOGI(TAG, "Stack v4_link: %u Byte von %u frei (Minimum seit Start)",
                 (unsigned)(uxTaskGetStackHighWaterMark(s_link_task_h) * sizeof(StackType_t)),
                 (unsigned)V4_LINK_TASK_STACK);
    }
    if (s_work_task_h != NULL) {
        ESP_LOGI(TAG, "Stack v4_work: %u Byte von %u frei (Minimum seit Start)",
                 (unsigned)(uxTaskGetStackHighWaterMark(s_work_task_h) * sizeof(StackType_t)),
                 (unsigned)V4_WORK_TASK_STACK);
    }
}

static void v4_link_task(void *arg)
{
    rx_msg_t msg;

    while (1) {
        if (xQueueReceive(s_rx_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (msg.kind == RX_KIND_SELFTEST) {
            s_rx_stamp_us = 0;      /* synthetic frames carry no bus timing */
            st_run_all();
            s_selftest_done = true;
            continue;
        }
        s_rx_stamp_us = msg.t_rx_us;
        process_frame(msg.data, msg.len);
    }
}

esp_err_t v4_link_init(void)
{
    s_path_len = 0;
    s_path[0] = '\0';

    s_rx_queue = xQueueCreate(V4_RX_QUEUE_LEN, sizeof(rx_msg_t));
    ESP_RETURN_ON_FALSE(s_rx_queue != NULL, ESP_ERR_NO_MEM, TAG, "no mem for rx queue");

    s_work_queue = xQueueCreate(V4_WORK_QUEUE_LEN, sizeof(work_msg_t));
    ESP_RETURN_ON_FALSE(s_work_queue != NULL, ESP_ERR_NO_MEM, TAG, "no mem for work queue");

    i2c_slave_config_t cfg = {
        .i2c_port = V4_I2C_PORT,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .scl_io_num = V4_I2C_SCL_IO,
        .sda_io_num = V4_I2C_SDA_IO,
        .slave_addr = V4_I2C_ADDR,
        .send_buf_depth = V4_TX_BUF_DEPTH,
        .receive_buf_depth = V4_RX_BUF_DEPTH,
        .addr_bit_len = I2C_ADDR_BIT_LEN_7,
        .intr_priority = 0,
    };
    cfg.flags.enable_internal_pullup = 1;

    esp_err_t err = i2c_new_slave_device(&cfg, &s_slave);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_slave_device failed: %s", esp_err_to_name(err));
        return err;
    }

    i2c_slave_event_callbacks_t cbs = {
        .on_receive = v4_on_receive,
        .on_request = NULL,   /* we always push before the master reads */
    };
    err = i2c_slave_register_event_callbacks(s_slave, &cbs, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register callbacks failed: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t ok = xTaskCreate(v4_link_task, "v4_link", V4_LINK_TASK_STACK, NULL,
                                V4_TASK_PRIO, &s_link_task_h);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "no mem for task");

    /* Same priority as the protocol task: neither may starve the other, and the
     * self test waits on this one from the protocol task. */
    BaseType_t wok = xTaskCreate(v4_work_task, "v4_work", V4_WORK_TASK_STACK, NULL,
                                 V4_TASK_PRIO, &s_work_task_h);
    ESP_RETURN_ON_FALSE(wok == pdPASS, ESP_ERR_NO_MEM, TAG, "no mem for work task");

    ESP_LOGI(TAG, "slave 0x%02x on SDA=%d SCL=%d, frames %d/%d, chunk %u",
             (unsigned)V4_I2C_ADDR, (int)V4_I2C_SDA_IO, (int)V4_I2C_SCL_IO,
             V4P_WRITE_FRAME_LEN, V4P_READ_FRAME_LEN, (unsigned)s_chunk);
    return ESP_OK;
}

uint16_t v4_link_chunk_size(void)
{
    return s_chunk;
}
