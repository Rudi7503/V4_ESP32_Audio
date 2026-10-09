/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#include "esp_check.h"
#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_gmf_task.h"
#include "esp_heap_caps.h"
#include "esp_heap_caps_init.h"   /* heap_caps_add_region/remove_region */
#include "esp_log.h"
#include "esp_timer.h"
#include "i2s_input.h"
#include "bt_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_bt_audio_defs.h"
#include "esp_bt_audio_media.h"
#include "esp_bt_audio_stream.h"
#include "esp_gmf_audio_dec.h"
#include "esp_gmf_audio_enc.h"
#include "esp_sbc_enc.h"
#include "linear_resample.h"
#include "esp_gmf_audio_helper.h"
#include "esp_gmf_rate_cvt.h"
#include "esp_gmf_bit_cvt.h"
#include "esp_gmf_ch_cvt.h"
#include "esp_gmf_mixer.h"
#include "esp_gmf_eq.h"
#include "esp_gmf_new_databus.h"
#include "esp_gmf_oal_mem.h"
#include "esp_gmf_port.h"
#include "esp_gmf_element.h"
#include "esp_gmf_err.h"
#include "esp_gmf_pipeline.h"
#include "esp_gmf_pool.h"
#include "esp_gmf_io_bt.h"
#include "esp_gmf_asrc.h"
#if CONFIG_BT_NIMBLE_ENABLED && CONFIG_BT_AUDIO && CONFIG_BT_ISO && CONFIG_SOC_MODEM_SUPPORT_ETM
#include "esp_board_manager.h"
#include "esp_board_manager_defs.h"
#include "esp_board_periph.h"
#include "esp_codec_dev.h"
#include "dev_audio_codec.h"
#include "esp_bt_audio_le_playback_sync.h"
#endif  /* CONFIG_BT_NIMBLE_ENABLED && CONFIG_BT_AUDIO && CONFIG_BT_ISO && CONFIG_SOC_MODEM_SUPPORT_ETM */

#include "stream_proc.h"
#include "codec_defs.h"

#define STREAM_PROC_ASRC_MAX_CH          4
#define STREAM_PROC_ASRC_MAX_WEIGHT_LEN  (STREAM_PROC_ASRC_MAX_CH * STREAM_PROC_ASRC_MAX_CH)
#define STREAM_PROC_CMD_QUEUE_SIZE       12
#define STREAM_PROC_TASK_STACK_SIZE      4096
#define STREAM_PROC_TASK_PRIO            20

typedef enum {
    STREAM_PROC_PIPELINE_PREPARE,
    STREAM_PROC_PIPELINE_RUN,
    STREAM_PROC_PIPELINE_STOP_RESET,
} stream_proc_pipeline_action_t;

typedef struct {
    stream_proc_pipeline_action_t  action;
    esp_gmf_pipeline_handle_t      pipe;
    const char                    *uri;
    bool                           report_codec_input_info;
} stream_proc_cmd_t;

static const char *TAG = "STREAM_PROC";

/* Playlist configuration.
 *
 * These are the files that are actually on our test card. The "file://" form is
 * what esp_gmf_io_file wants as the pipeline URI; it strips the scheme and
 * opens "/sdcard/<name>".
 *
 * Only MP3 entries here on purpose: the pipeline is configured with an MP3
 * decoder (see the esp_gmf_audio_dec_reconfig call in stream_proc_prepare), so
 * the WAV files on the card would only produce decoder errors. */
static const char *playlist[] = {
    "file://sdcard/test2.mp3",
    "file://sdcard/test.mp3",
};
static const size_t playlist_len = sizeof(playlist) / sizeof(playlist[0]);
static size_t playlist_cur_index = 0;

/*
 * Zielformat am Mischereingang und damit auch am Bluetooth-Ausgang.
 *
 * 48000 Hz, NICHT 44100 (Befund 02.10.): der SBC-Encoder resampelt nicht, er
 * bekommt die Rate nur gesagt. Die Soundbar handelt 48000 Hz aus, der
 * Kopfhoerer 44100 Hz. Mit einer festen 44100er-Kette zog die Soundbar
 * 7,5 SBC-Rahmen je 20 ms, waehrend wir nur ~6,9 lieferten - der Puffer der
 * Senke lief leer, hoerbar als Ruckler. Deshalb passt die Kette jetzt zur
 * Soundbar.
 *
 * Dauerhafte Loesung: diese Rate beim Stream-Start aus der Aushandlung
 * uebernehmen (esp_bt_audio_stream_get_codec_info in i2s2bt_set_stream) -
 * dann stimmt sie fuer jede Senke.
 */
#define I2S2BT_RATE_HZ      48000
#define I2S2BT_BITS         16
#define I2S2BT_CHANNELS     2

/*
 * Wartezeiten des Mischers - Startwerte.
 *
 * prefill_ms  Wartezeit zwischen Start der Zubringer und Start des Mischers.
 *             Sie bestimmt den Vorlauf im Ringpuffer. 400 ms genuegen, damit
 *             der I2S-Eingang und ein eventuell laufender Datei-Zweig den
 *             Puffer gefuellt haben.
 * transit_ms  transit_time je Mischer-Quelle (esp_ae_mixer_info_t).
 *
 * Beide sind zur LAUFZEIT einstellbar (CLI-Kommando "mixer"), damit sich die
 * Frage "kann der Puffer unterlaufen?" ohne neues Flashen beantworten laesst:
 * Wert verkuerzen, start_media, und im Log Minimum/leer der Pufferzeilen
 * ansehen. Die Startwerte sind bewusst die zuletzt auf Hardware bewaehrten.
 */
#define MIXER_PREFILL_MS_DEFAULT    400
#define MIXER_TRANSIT_MS_DEFAULT    100

static int s_mixer_prefill_ms = MIXER_PREFILL_MS_DEFAULT;
static int s_mixer_transit_ms = MIXER_TRANSIT_MS_DEFAULT;

/* Pipeline handles */
static esp_gmf_task_handle_t bt2codec_task = NULL;
static esp_gmf_pipeline_handle_t bt2codec_pipe = NULL;

static esp_gmf_task_handle_t codec2bt_task = NULL;
static esp_gmf_pipeline_handle_t codec2bt_pipe = NULL;

static esp_gmf_task_handle_t local2bt_task = NULL;
static esp_gmf_pipeline_handle_t local2bt_pipe = NULL;
static esp_bt_audio_stream_handle_t local2bt_stream = NULL;

/* I2S-Eingang der Vampire -> Mischer */
static esp_gmf_task_handle_t i2s2bt_task = NULL;
static esp_gmf_pipeline_handle_t i2s2bt_pipe = NULL;
static esp_bt_audio_stream_handle_t i2s2bt_stream = NULL;


/* Die beiden Ringpuffer zwischen Zubringer und Mischer (siehe
 * connect_branch_to_mixer). */
static esp_gmf_db_handle_t i2s_branch_db = NULL;
static esp_gmf_db_handle_t file_branch_db = NULL;

/* Mischer samt Encoder und Bluetooth-Ausgang */
static esp_gmf_task_handle_t mixer_task = NULL;
static esp_gmf_pipeline_handle_t mixer_pipe = NULL;

static QueueHandle_t stream_proc_cmd_queue = NULL;

/* Vom Event-Callback gesetzt, vom stream_proc_task abgearbeitet. */
static volatile bool local2bt_stop_requested = false;

/*
 * Wunsch, den I2S-Eingang der Vampire nach Bluetooth zu schicken. Wird ueber
 * das CLI-Kommando gesetzt und beim naechsten A2DP-Stream ausgewertet.
 */
static volatile bool i2s2bt_requested = false;

/* Autostart der Uebertragung beim Verbinden (0.9.68), Vorgabe an. */
static volatile bool s_media_autostart = true;

/*
 * Obergrenze fuer den SBC-Bitpool (0.9.71), 0 = Vorgabe der Komponente.
 *
 * esp_bt_audio waehlt fuer Stereo 53 (A2DP_SRC_BITPOOL_STEREO_DEFAULT) und
 * deckelt nur auf das Maximum der Senke - in unserem Fall ebenfalls 53. Das
 * sind rund 327 kbit/s: viel Rechenzeit im Encoder (gemessen 31 % der
 * Gesamt-CPU allein fuer den Mischer-Task ohne jede Quelle, /tmp/eq_test.log)
 * und viel Luft auf der 2,4-GHz-Strecke. Der Bitpool darf laut A2DP frei
 * innerhalb [min,max] der Senke gewaehlt werden - hier lässt sich das messen:
 *
 *   sbc bitpool 35   -> naechster Streamstart uebernimmt den Wert
 */
static volatile int s_sbc_bitpool_cap;

/* Vorwaertsdeklaration: der Stream-Callback steht weiter oben in der Datei. */
void i2s2bt_set_stream(esp_bt_audio_stream_handle_t stream);

/* Diagnose-Helfer (Definition weiter unten, benutzt schon in den Aufbauten). */
static const char *gmf_state_to_str(int state);
static int port_count(esp_gmf_port_handle_t head);
static void dump_pipeline(const char *what, esp_gmf_pipeline_handle_t pipe);
static void dump_pipeline_state(const char *what, esp_gmf_pipeline_handle_t pipe);

/*
 * Dekoder-Arena als statischer Bereich (Weg 2, 0.9.81).
 *
 * Der MP3-Dekoder braucht rund 32 KB am Stueck; der Aufbau des Datei-Zweigs
 * verbraucht davon noch ~9 KB (gemessen 0.9.80: 36864 -> 27648 Byte). Deshalb
 * ein Bereich in .bss: der ist per Definition zusammenhaengend und kann gar
 * nicht zersplittern.
 *
 * Er wird NICHT dauerhaft in den Heap gehaengt, sondern nur waehrend der
 * Wiedergabe angemeldet (heap_caps_add_region) und danach wieder abgemeldet
 * (heap_caps_remove_region). Damit ist er im Ruhezustand aus dem Heap heraus
 * (die uebrigen Verbraucher haben die vollen ~40 KB wie bisher) und waehrend
 * der Wiedergabe steht er dem Dekoder als frischer, grosser Block zur
 * Verfuegung - vorher kann niemand hineinallokiert haben.
 *
 * ACHTUNG: Abmelden nur, wenn der Datei-Zweig wirklich geschlossen ist -
 * heap_caps_remove_region darf keine lebenden Allokationen mehr enthalten.
 */
/*
 * 42 -> 36 KB (0.9.82). Der Dekoder bekam mit 42 KB durchgehend 38,9-40,9 KB
 * (Bedarf ~32 KB), also sind 6 KB verschenkt - und die fehlten dem BT-Stack:
 * mit der 42-KB-Arena lief der Heap im laengeren Betrieb auf free=64 Byte und
 * der Chip panickte (StoreProhibited). Mit 36 KB bleiben dem Dekoder ~33 KB,
 * dem System ~6 KB mehr.
 */
/*
 * 36 -> 38 KB (0.9.83). Messung 0.9.82: bei 36 KB blieben dem Dekoder stabil
 * 34816 Byte (~32 KB Bedarf, also nur ~2,8 KB Reserve) und von zehn
 * Durchlaeufen scheiterte einer. Bei 42 KB kippte dagegen der Heap (Panic).
 * 38 KB ist die Mitte; die ACL-Reduktion auf 1 Verbindung hat +0,7 KB gebracht.
 */
#define LOCAL2BT_ARENA_BYTES  (38u * 1024u)

static uint8_t s_local2bt_arena[LOCAL2BT_ARENA_BYTES] __attribute__((aligned(16)));
static int     s_local2bt_arena_offen;

static size_t local2bt_groesster(void)
{
    return heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
}

/*
 * Bereich EINMALIG anmelden und angemeldet lassen (0.9.81).
 *
 * heap_caps_remove_region gibt es in dieser IDF-Version nicht. Das ist auch
 * nicht noetig: multi_heap vergibt nach Best-Fit, kleine Anforderungen (BT,
 * Bruecke) greifen den grossen Block also nicht an. Der Aufbau des
 * Datei-Zweigs nimmt 9 KB davon, dem Dekoder bleiben 33 KB von 42 KB - genug
 * fuer seine rund 32 KB, und der Bereich kann nicht weiter zersplittern.
 */
static void local2bt_arena_reserve(const char *wer)
{
    esp_err_t e;

    if (s_local2bt_arena_offen != 0) {
        return;
    }
    e = heap_caps_add_region((intptr_t)&s_local2bt_arena[0],
                             (intptr_t)&s_local2bt_arena[LOCAL2BT_ARENA_BYTES]);
    if (e == ESP_OK) {
        s_local2bt_arena_offen = 1;
        ESP_LOGI(TAG, "Dekoder-Arena %s: %u Byte angemeldet, groesster Block %u Byte",
                 wer, (unsigned)LOCAL2BT_ARENA_BYTES, (unsigned)local2bt_groesster());
    } else {
        ESP_LOGE(TAG, "Dekoder-Arena nicht anmeldbar: %d", (int)e);
    }
}

/* Messpunkt vor dem Dekoderstart - der Bereich bleibt angemeldet. */
static void local2bt_arena_release(void)
{
    ESP_LOGI(TAG, "Heap vor dem Dekoderstart: frei %u, groesster Block %u Byte",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
             (unsigned)local2bt_groesster());
}

static void local2bt_request_stop(void)
{
    local2bt_stop_requested = true;
}

/*
 * A2DP-Uebertragung anhalten - NUR vormerken.
 *
 * esp_bt_audio_media_stop() darf NICHT aus dem GMF-Event-Callback gerufen
 * werden: der Callback laeuft im GMF-Task, und media_stop wartet auf den
 * Bluetooth-/Stream-Ablauf, der seinerseits auf den GMF-Task wartet. Das ist
 * ein klassischer Ringschluss - am 03.10. endete er im Task-Watchdog:
 *
 *   I STREAM_PROC: [a2dp source pipeline] state => ERROR(7)
 *   E STREAM_PROC: A2DP Source error
 *   I BT_AUD_A2D_SRC: A2DP source media stop
 *   rst:0x8 (TG1WDT_SYS_RESET)
 *
 * Deshalb wird hier nur ein Wunsch gesetzt; ausgefuehrt wird er in
 * local2bt_process_stop_request(), also im stream_proc_task.
 */
/*
 * Sperre fuer den Umbau des Datei-Zweigs (0.9.38).
 *
 * Zwei Aufgaben greifen auf dieselbe Pipeline zu:
 *   - die Konsolen-Aufgabe bei 'playfile'          (local2bt_play)
 *   - die stream_proc-Aufgabe bei Dateiende/Fehler (local2bt_process_stop_request)
 *
 * Am 03.10. liefen beide gleichzeitig, als ein Titel genau beim Eintreffen des
 * naechsten 'playfile' endete:
 *
 *   ... label:aud_lin_resample_file_close     (alter Titel fertig)
 *   Datei -> Mischer: test_tone_440.wav       (neuer Auftrag)
 *   W Element[aud_dec-...] not ready to register job, ret:0xffffdff8
 *   state => ERROR(7)   ... und danach Watchdog-Neustart
 *
 * Diese Sperre serialisiert beide Wege.
 */
static SemaphoreHandle_t s_local2bt_lock = NULL;

/*
 * Merker: laeuft der Datei-Zweig? (0.9.39)
 *
 * Auf esp_gmf_pipeline_t::state ist KEIN Verlass. Am 03.10. stand das Feld auf
 * OPENING, waehrend ein 5-Minuten-MP3 lief - die Zustandspruefung hielt den
 * Zweig deshalb fuer "nicht laufend", stoppte ihn nicht und baute den Decoder
 * mitten im Titel um:
 *
 *   Datei-Zweig vor dem Umbau: OPENING
 *   Decoder auf Dateityp 0x20564157 eingestellt (test_tone_440.wav)
 *   E WAV_Parser: Not a WAV file          <- es kamen noch die alten Daten
 *   state => ERROR(7)  ... Watchdog-Neustart
 *
 * Deshalb fuehren wir selbst Buch: gesetzt nach pipeline_run(), geloescht nach
 * einem erfolgreichen Stop+Reset.
 */
static volatile bool s_local2bt_laeuft = false;

/*
 * Zuletzt angespielter URI des Datei-Zweigs (0.9.57).
 *
 * Wird zusammen mit s_local2bt_laeuft gesetzt und geloescht, damit das
 * I2C-Protokoll der Vampire den laufenden Titel melden kann. Nur der
 * Datei-Zweig schreibt hier - eine Kopie ohne Sperre genuegt deshalb.
 */
static char s_local2bt_uri[192];

static bool local2bt_lock_take(const char *wer)
{
    if (s_local2bt_lock == NULL) {
        s_local2bt_lock = xSemaphoreCreateMutex();
        if (s_local2bt_lock == NULL) {
            return true;        /* ohne Sperre weiterarbeiten statt blockieren */
        }
    }
    if (xSemaphoreTake(s_local2bt_lock, pdMS_TO_TICKS(4000)) != pdTRUE) {
        ESP_LOGW(TAG, "%s: Datei-Zweig ist besetzt - Umbau uebersprungen", wer);
        return false;
    }
    return true;
}

static void local2bt_lock_give(void)
{
    if (s_local2bt_lock != NULL) {
        xSemaphoreGive(s_local2bt_lock);
    }
}

/*
 * Hier stand bis 0.9.61 local2bt_request_media_stop(): es merkte vor, die
 * gesamte A2DP-Uebertragung anzuhalten, und wurde ausschliesslich aus der
 * Fehlerbehandlung des Datei-Zweigs gerufen. Das war falsch - der Mischer
 * traegt weiter den I2S-Ton der Vampire, ein gescheitertes playfile darf den
 * nicht mitreissen (docs/MP3_STARTFEHLER.md). Geblieben ist der Weg ueber
 * local2bt_request_stop(), der nur den Datei-Zweig stoppt und zuruecksetzt.
 * Die Uebertragung selbst beendet weiterhin das Konsolenkommando 'stop_media'.
 */

static void local2bt_process_stop_request(void)
{
    if (!local2bt_stop_requested) {
        return;
    }
    local2bt_stop_requested = false;

    if (local2bt_pipe == NULL) {
        return;
    }
    if (!local2bt_lock_take("Dateiende")) {
        return;
    }
    /*
     * Nur stoppen, wenn wirklich etwas laeuft.
     *
     * esp_gmf_task_stop() wartet nach einem Timeout UNENDLICH auf das
     * STOP-Bit (esp_gmf_task.c:724, "wait more until task full quit"). Ist die
     * Aufgabe schon angehalten und wartet auf den naechsten Lauf, kommt dieses
     * Bit nie - der Aufrufer haengt fuer immer. Genau so ist es passiert:
     *
     *   W ESP_GMF_TASK: Stop timeout for [local2bt_task,0x3ffd084c], retrying...
     *
     * ... und danach war die Konsole tot (kein 'version' mehr). Der Zustand
     * steht in esp_gmf_pipeline_t::state (oeffentlich, esp_gmf_pipeline.h:46).
     */
    esp_gmf_pipeline_t *p = (esp_gmf_pipeline_t *)local2bt_pipe;
    /*
     * ERROR gehoert mit dazu (seit 0.9.30).
     *
     * Ohne ERROR blieb der Datei-Zweig nach einem fehlgeschlagenen Job im
     * Zustand ERROR stehen - angehalten, zurueckgesetzt und aufgeraeumt wurde
     * nichts. Das naechste 'playfile' traf dann auf eine Pipeline, die noch
     * mitten in ihrer Fehlerbehandlung stand; im Log vom 03.10. endete das in
     * einem Interrupt-Watchdog-Neustart:
     *
     *   E WAV_Parser: Not a WAV file
     *   E ESP_GMF_TASK: Job failed [... aud_dec_proc], ret:-1
     *   I STREAM_PROC: [a2dp source pipeline] state => ERROR(7)
     *   Guru Meditation Error: Core 1 panic'ed (Interrupt wdt timeout on CPU1)
     */
    if (p->state == ESP_GMF_EVENT_STATE_RUNNING || p->state == ESP_GMF_EVENT_STATE_PAUSED ||
        p->state == ESP_GMF_EVENT_STATE_FINISHED || p->state == ESP_GMF_EVENT_STATE_ERROR) {
        ESP_LOGI(TAG, "Wiedergabe beendet - stoppe den Datei-Zweig (%s)", gmf_state_to_str(p->state));
        esp_gmf_pipeline_stop(local2bt_pipe);
        esp_gmf_pipeline_reset(local2bt_pipe);
        s_local2bt_laeuft = false;
        s_local2bt_uri[0] = '\0';
    } else {
        ESP_LOGI(TAG, "Wiedergabe beendet - Datei-Zweig war schon angehalten (%s)", gmf_state_to_str(p->state));
    }
    local2bt_lock_give();
    local2bt_arena_reserve("nach dem Stopp");
}

static float bt2codec_asrc_weight[STREAM_PROC_ASRC_MAX_WEIGHT_LEN];
static float codec2bt_input_asrc_weight[STREAM_PROC_ASRC_MAX_WEIGHT_LEN];
static float codec2bt_output_asrc_weight[STREAM_PROC_ASRC_MAX_WEIGHT_LEN];
static float local2bt_asrc_weight[STREAM_PROC_ASRC_MAX_WEIGHT_LEN];

static bool stream_proc_post_cmd(const stream_proc_cmd_t *cmd)
{
    if (stream_proc_cmd_queue == NULL) {
        ESP_LOGE(TAG, "Stream processor task is not initialized");
        return false;
    }
    if (xQueueSend(stream_proc_cmd_queue, cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Stream processor command queue full, drop action %d", cmd->action);
        return false;
    }
    return true;
}

static void stream_proc_post_pipeline_action(esp_gmf_pipeline_handle_t pipe, stream_proc_pipeline_action_t action)
{
    stream_proc_cmd_t cmd = {
        .action = action,
        .pipe = pipe,
    };
    stream_proc_post_cmd(&cmd);
}

static esp_gmf_element_handle_t stream_proc_get_asrc(esp_gmf_pipeline_handle_t pipe, uint8_t index)
{
    const void *iterator = NULL;
    esp_gmf_element_handle_t cur_el = NULL;
    uint8_t asrc_count = 0;
    while (esp_gmf_pipeline_iterate_element(pipe, &iterator, &cur_el) == ESP_GMF_ERR_OK) {
        char *el_tag = NULL;
        esp_gmf_obj_get_tag((esp_gmf_obj_handle_t)cur_el, &el_tag);
        if (el_tag && strcasecmp(el_tag, "aud_asrc") == 0) {
            if (asrc_count == index) {
                return cur_el;
            }
            asrc_count++;
        }
    }
    ESP_LOGE(TAG, "No aud_asrc[%d] found in pipeline %p", index, pipe);
    return NULL;
}

static void stream_proc_fill_asrc_weight(float *weight, uint32_t weight_cap, uint8_t src_ch, uint8_t dest_ch)
{
    uint32_t weight_len = src_ch * dest_ch;
    if (weight == NULL || weight_len > weight_cap) {
        return;
    }
    memset(weight, 0, weight_len * sizeof(weight[0]));
    for (uint8_t dest = 0; dest < dest_ch; dest++) {
        if (src_ch == dest_ch) {
            weight[dest * src_ch + dest] = 1.0f;
        } else if (src_ch == 1) {
            weight[dest] = 1.0f;
        } else if (dest_ch == 1) {
            weight[dest * src_ch] = 1.0f / src_ch;
            for (uint8_t src = 1; src < src_ch; src++) {
                weight[dest * src_ch + src] = 1.0f / src_ch;
            }
        } else {
            weight[dest * src_ch + (dest < src_ch ? dest : src_ch - 1)] = 1.0f;
        }
    }
}

static void stream_proc_set_asrc_dest(esp_gmf_pipeline_handle_t pipe, uint8_t index, uint32_t sample_rate,
                                      uint8_t src_ch, uint8_t dest_ch, float *weight, uint32_t weight_cap)
{
    esp_gmf_element_handle_t asrc = stream_proc_get_asrc(pipe, index);
    if (asrc == NULL) {
        return;
    }
    if (src_ch == 0 || dest_ch == 0 || src_ch > STREAM_PROC_ASRC_MAX_CH || dest_ch > STREAM_PROC_ASRC_MAX_CH) {
        ESP_LOGE(TAG, "Invalid ASRC channel config, src: %d, dest: %d", src_ch, dest_ch);
        return;
    }
    esp_gmf_asrc_set_dest_rate(asrc, sample_rate);
    esp_gmf_asrc_set_dest_ch(asrc, dest_ch);
    stream_proc_fill_asrc_weight(weight, weight_cap, src_ch, dest_ch);
    esp_asrc_cfg_t *cfg = (esp_asrc_cfg_t *)OBJ_GET_CFG(asrc);
    if (cfg) {
        cfg->weight = weight;
        cfg->weight_len = src_ch * dest_ch;
    }
}

#if CONFIG_BT_NIMBLE_ENABLED && CONFIG_BT_AUDIO && CONFIG_BT_ISO && CONFIG_SOC_MODEM_SUPPORT_ETM
#define STREAM_PROC_CLK_SYNC_DIFF_THRESHOLD  2
#define STREAM_PROC_CLK_SYNC_MON_QUEUE_SIZE  10
#define STREAM_PROC_CLK_SYNC_MON_TASK_STACK  4096
#define STREAM_PROC_CLK_SYNC_MON_TASK_PRIO   5

static esp_bt_audio_le_playback_sync_handle_t playback_sync = NULL;
static esp_bt_audio_le_clk_sync_handle_t clk_sync = NULL;
static QueueHandle_t clk_sync_monitor_queue = NULL;
static TaskHandle_t clk_sync_monitor_task = NULL;
static volatile bool clk_sync_monitor_task_running = false;

static esp_err_t stream_proc_open_dac(dev_audio_codec_handles_t *dac_handle)
{
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = CODEC_DAC_SAMPLE_RATE,
        .bits_per_sample = CODEC_DAC_BITS_PER_SAMPLE,
        .channel = CODEC_DAC_CHANNELS,
    };
    return esp_codec_dev_open(dac_handle->codec_dev, &fs);
}

static i2s_chan_handle_t get_i2s_chan_handle(const char *name)
{
    i2s_chan_handle_t ch = NULL;
    dev_audio_codec_config_t *codec_config = NULL;
    esp_err_t ret = esp_board_manager_get_device_config(name, (void **)&codec_config);
    ESP_RETURN_ON_FALSE(ret == ESP_OK, NULL, TAG, "get device config failed");
    ret = esp_board_periph_get_handle(codec_config->i2s_cfg.name, (void **)&ch);
    ESP_RETURN_ON_FALSE(ret == ESP_OK, NULL, TAG, "get i2s chan handle failed");
    ESP_LOGI(TAG, "get i2s[%s:%s] handle %p", name, codec_config->i2s_cfg.name, ch);
    return ch;
}

static void stream_proc_clk_sync_monitor_task(void *arg)
{
    (void)arg;
    esp_bt_audio_le_clk_sync_msg_t msg = {0};

    while (clk_sync_monitor_task_running) {
        if (xQueueReceive(clk_sync_monitor_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!clk_sync_monitor_task_running) {
            break;
        }
        ESP_LOGW(TAG, "Clock sync monitor: diff=%ld, fifo=%" PRIu32 ", bck=%" PRIu32,
                 (long)msg.diff, msg.fifo_cnt, msg.bck_cnt);
    }
    clk_sync_monitor_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t stream_proc_ensure_clk_sync_monitor(void)
{
    if (!clk_sync_monitor_queue) {
        clk_sync_monitor_queue = xQueueCreate(STREAM_PROC_CLK_SYNC_MON_QUEUE_SIZE,
                                              sizeof(esp_bt_audio_le_clk_sync_msg_t));
        ESP_RETURN_ON_FALSE(clk_sync_monitor_queue, ESP_ERR_NO_MEM, TAG, "Create clock sync monitor queue failed");
    }

    if (!clk_sync_monitor_task) {
        clk_sync_monitor_task_running = true;
        BaseType_t ret = xTaskCreate(stream_proc_clk_sync_monitor_task, "clk_sync_mon",
                                     STREAM_PROC_CLK_SYNC_MON_TASK_STACK, NULL,
                                     STREAM_PROC_CLK_SYNC_MON_TASK_PRIO, &clk_sync_monitor_task);
        if (ret != pdPASS) {
            clk_sync_monitor_task_running = false;
            ESP_LOGE(TAG, "Create clock sync monitor task failed");
            return ESP_ERR_NO_MEM;
        }
    }

    xQueueReset(clk_sync_monitor_queue);
    return ESP_OK;
}

static void stream_proc_deinit_clk_sync_monitor(void)
{
    if (clk_sync_monitor_task) {
        esp_bt_audio_le_clk_sync_msg_t msg = {0};
        clk_sync_monitor_task_running = false;
        if (clk_sync_monitor_queue) {
            xQueueSend(clk_sync_monitor_queue, &msg, 0);
        }
        for (uint8_t i = 0; i < 10 && clk_sync_monitor_task; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (clk_sync_monitor_task) {
            vTaskDelete(clk_sync_monitor_task);
            clk_sync_monitor_task = NULL;
        }
    }

    if (clk_sync_monitor_queue) {
        vQueueDelete(clk_sync_monitor_queue);
        clk_sync_monitor_queue = NULL;
    }
    clk_sync_monitor_task_running = false;
}

static void stream_proc_deinit_playback_sync(void)
{
    if (!playback_sync) {
        return;
    }

    esp_err_t ret = esp_bt_audio_le_playback_sync_disable(playback_sync);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Playback sync disable failed: %s", esp_err_to_name(ret));
    }
    ret = esp_bt_audio_le_playback_sync_deinit(playback_sync);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Playback sync deinit failed: %s", esp_err_to_name(ret));
    }
    playback_sync = NULL;
}

static void stream_proc_deinit_clk_sync(void)
{
    if (!clk_sync) {
        return;
    }

    esp_err_t ret = esp_bt_audio_le_clk_sync_disable(clk_sync);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Clock sync disable failed: %s", esp_err_to_name(ret));
    }
    ret = esp_bt_audio_le_clk_sync_deinit(clk_sync);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Clock sync deinit failed: %s", esp_err_to_name(ret));
    }
    clk_sync = NULL;
    stream_proc_deinit_clk_sync_monitor();
}

static void stream_proc_prepare_clk_sync(esp_bt_audio_stream_handle_t stream)
{
    if (clk_sync) {
        return;
    }

    uint16_t iso_interval = 0;
    esp_err_t ret = esp_bt_audio_stream_get_iso_interval(stream, &iso_interval);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Get ISO interval failed: %s", esp_err_to_name(ret));
        return;
    }

    uint64_t ideal_count = ((uint64_t)CODEC_DAC_SAMPLE_RATE * CODEC_DAC_CHANNELS * iso_interval + 500000U) / 1000000U;
    if (ideal_count == 0 || ideal_count > UINT32_MAX) {
        ESP_LOGW(TAG, "Invalid clock sync ideal count: %" PRIu64, ideal_count);
        return;
    }

    i2s_chan_handle_t tx_handle = get_i2s_chan_handle(ESP_BOARD_DEVICE_NAME_AUDIO_DAC);
    if (!tx_handle) {
        ESP_LOGE(TAG, "Get I2S TX handle failed");
        return;
    }

    ret = stream_proc_ensure_clk_sync_monitor();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Prepare clock sync monitor failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bt_audio_le_clk_sync_init(tx_handle, (uint32_t)ideal_count, STREAM_PROC_CLK_SYNC_DIFF_THRESHOLD,
                                        true, clk_sync_monitor_queue, &clk_sync);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Clock sync init failed: %s", esp_err_to_name(ret));
        return;
    }
    ret = esp_bt_audio_le_clk_sync_enable(clk_sync);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Clock sync enable failed: %s", esp_err_to_name(ret));
        esp_bt_audio_le_clk_sync_deinit(clk_sync);
        clk_sync = NULL;
        return;
    }

    ESP_LOGI(TAG, "Clock sync enabled, iso_interval=%u(us), ideal_count=%lu",
             iso_interval, (uint32_t)ideal_count);
}

static void stream_proc_prepare_playback_sync(void)
{
    if (playback_sync) {
        return;
    }

    dev_audio_codec_handles_t *dac_handle = NULL;
    esp_err_t ret = esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_AUDIO_DAC, (void **)&dac_handle);
    if (ret != ESP_OK || !dac_handle) {
        ESP_LOGE(TAG, "get audio dac handle failed");
        return;
    }

    ret = esp_codec_dev_close(dac_handle->codec_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "close audio dac failed: %s", esp_err_to_name(ret));
        return;
    }

    i2s_chan_handle_t tx_handle = get_i2s_chan_handle(ESP_BOARD_DEVICE_NAME_AUDIO_DAC);
    if (!tx_handle) {
        ESP_LOGE(TAG, "get i2s tx handle failed");
        stream_proc_open_dac(dac_handle);
        return;
    }
    ret = esp_bt_audio_le_playback_sync_init(tx_handle, &playback_sync);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Playback sync init failed: %s", esp_err_to_name(ret));
        stream_proc_open_dac(dac_handle);
        return;
    }

    ret = esp_bt_audio_le_playback_sync_enable(playback_sync);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Playback sync enable failed: %s", esp_err_to_name(ret));
        esp_bt_audio_le_playback_sync_deinit(playback_sync);
        playback_sync = NULL;
        stream_proc_open_dac(dac_handle);
        return;
    }

    ret = stream_proc_open_dac(dac_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "open audio dac failed: %s", esp_err_to_name(ret));
        stream_proc_deinit_playback_sync();
    }
}
#else
static void stream_proc_prepare_playback_sync(void)
{
}

static void stream_proc_prepare_clk_sync(esp_bt_audio_stream_handle_t stream)
{
    (void)stream;
}

static void stream_proc_deinit_playback_sync(void)
{
}

static void stream_proc_deinit_clk_sync(void)
{
}

static void stream_proc_deinit_clk_sync_monitor(void)
{
}
#endif  /* CONFIG_BT_NIMBLE_ENABLED && CONFIG_BT_AUDIO && CONFIG_BT_ISO && CONFIG_SOC_MODEM_SUPPORT_ETM */

static const char *gmf_state_to_str(int state)
{
    switch (state) {
        case ESP_GMF_EVENT_STATE_NONE:
            return "NONE";
        case ESP_GMF_EVENT_STATE_INITIALIZED:
            return "INITIALIZED";
        case ESP_GMF_EVENT_STATE_OPENING:
            return "OPENING";
        case ESP_GMF_EVENT_STATE_RUNNING:
            return "RUNNING";
        case ESP_GMF_EVENT_STATE_PAUSED:
            return "PAUSED";
        case ESP_GMF_EVENT_STATE_STOPPED:
            return "STOPPED";
        case ESP_GMF_EVENT_STATE_FINISHED:
            return "FINISHED";
        case ESP_GMF_EVENT_STATE_ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

/*
 * Datei aus der Wiedergabeliste abspielen - als ZUBRINGER in den Mischer.
 *
 * Wichtig: diese Pipeline hat KEINEN eigenen Bluetooth-Ausgang mehr. Sie
 * dekodiert und wandelt nur um (auf 44,1 kHz, 16 Bit, stereo) und schiebt das
 * Ergebnis ueber den Ringpuffer in den aud_mixer. Den Ausgang nach Bluetooth
 * besitzt mixer_pipe - den Stream setzt i2s2bt_set_stream() beim A2DP-Start.
 *
 * Deshalb wird hier auch kein io_bt-Stream mehr gesetzt: der Ausgang dieser
 * Pipeline existiert nicht (out_name war NULL beim Anlegen).
 *
 * Damit der Zweig auch anlaeuft, muss danach 'start_media' kommen - das
 * startet den Mischer, und der zieht beide Zubringer.
 */
static void local2bt_play(const char *uri)
{
    /*
     * 0.9.80: Die Freigabe steht NICHT mehr hier, sondern nach dem Aufbau des
     * Datei-Zweigs (kurz vor der Messzeile). Messung aus 0.9.79:
     *   freigegeben        -> groesster Block 36 864
     *   nach dem Aufbau    -> 32 768  (der Aufbau nimmt ~4 KB aus der Luecke)
     *   Dekoder braucht ~32 KB am Stueck
     * Mal reichte es (Wiedergabe lief), mal nicht - ein Wettlauf. Wird erst
     * nach dem Aufbau freigegeben, hat der Dekoder die vollen 36 864 Byte.
     */
    if (local2bt_pipe == NULL) {
        ESP_LOGE(TAG, "Datei-Pipeline ist nicht angelegt");
        return;
    }
    if (mixer_pipe == NULL) {
        ESP_LOGE(TAG, "Mischer-Pipeline ist nicht angelegt");
        return;
    }
    /*
     * Stop nur, wenn der Zweig wirklich laeuft - sonst haengt der Aufruf
     * (Begruendung und Messbeleg in local2bt_process_stop_request()). Der
     * Aufruf hier kommt aus der Konsolen-Aufgabe; ein Haenger legt damit die
     * ganze Bedienung lahm.
     */
    esp_gmf_pipeline_t *p = (esp_gmf_pipeline_t *)local2bt_pipe;


    /* Mit der stream_proc-Aufgabe absprechen: sie stoppt den Zweig bei
     * Dateiende, hier wird er neu aufgebaut. Gleichzeitig geht nicht. */
    if (!local2bt_lock_take("playfile")) {
        return;
    }

    /*
     * Laufenden Zweig anhalten.
     *
     * NUR bei RUNNING und PAUSED: in diesen Zustaenden laeuft die Aufgabe und
     * verarbeitet die STOP-Anforderung. Bei FINISHED/NONE/ERROR wurde bereits
     * ueber local2bt_process_stop_request() angehalten - ein zweiter Stop wartet
     * dort unendlich auf das STOP-Bit (esp_gmf_task.c:724).
     */
    ESP_LOGI(TAG, "Datei-Zweig vor dem Umbau: %s, laeuft=%d",
             gmf_state_to_str(p->state), (int)s_local2bt_laeuft);

    /*
     * Den Ringpuffer zum Mischer LEEREN.
     *
     * Sonst schleppt der neue Titel die Reste des alten mit - und, schlimmer,
     * einen eventuell halb geschriebenen Frame. Beides verschiebt den
     * Datenstrom gegen die Frame-Grenze und klingt verzerrt.
     * esp_gmf_db_reset() setzt Zeiger, Fuellstand und Abbruchmerker zurueck
     * (esp_gmf_ringbuffer.c:95).
     */
    if (file_branch_db != NULL) {
        uint32_t rest = 0;
        esp_gmf_db_get_filled_size(file_branch_db, &rest);
        esp_gmf_db_reset(file_branch_db);
        esp_gmf_db_clear_abort(file_branch_db);
        if (rest > 0) {
            ESP_LOGW(TAG, "Datei-Ringpuffer geleert (%u Byte Reste des vorigen Titels)", (unsigned)rest);
        }
    }
    /*
     * Anhalten nach UNSEREM Merker, nicht nach esp_gmf_pipeline_t::state.
     *
     * Das Zustandsfeld ist unzuverlaessig: am 03.10. stand es auf OPENING,
     * waehrend ein 5-Minuten-MP3 lief (test.mp3 hat 4630501 Byte). Die Pruefung
     * "nur bei RUNNING/PAUSED stoppen" griff deshalb nicht, der Decoder wurde
     * mitten im laufenden Titel umkonfiguriert und bekam weiter die alten
     * Daten:
     *
     *   Datei-Zweig vor dem Umbau: OPENING
     *   Decoder auf Dateityp 0x20564157 eingestellt (test_tone_440.wav)
     *   E WAV_Parser: Not a WAV file
     *   state => ERROR(7) ... Watchdog-Neustart
     *
     * Ist der Zweig schon angehalten, darf NICHT gestoppt werden: ein zweiter
     * Stop wartet in esp_gmf_task.c:724 unendlich auf das STOP-Bit.
     */
    if (s_local2bt_laeuft) {
        esp_gmf_pipeline_stop(local2bt_pipe);
        esp_gmf_pipeline_reset(local2bt_pipe);
        s_local2bt_laeuft = false;
        s_local2bt_uri[0] = '\0';
        ESP_LOGI(TAG, "Datei-Zweig angehalten und zurueckgesetzt");
    }

    /*
     * Dem Decoder sagen, WAS fuer eine Datei kommt.
     *
     * Das ist Pflicht und wurde beim Umbau auf den Mischer uebersehen: der
     * Dekodierer ist generisch und oeffnet erst, wenn er den Typ kennt.
     * Vorher erledigte das stream_proc_prepare() auf dem Umweg ueber den
     * A2DP-Datei-Stream - den umgeht der direkte Aufruf hier.
     *
     * Ohne diese Zeile:
     *
     *   E Job failed[tsk:local2bt_task: ... aud_dec_open], ret:-1
     *
     * Genau so macht es das offizielle Beispiel: pipeline_howl.c:197-198
     * ruft nach set_in_uri() erst reconfig_decoder_by_uri(), und diese
     * Funktion liest den Typ ueber esp_gmf_audio_helper_get_audio_type_by_uri
     * aus der Dateiendung.
     */
    uint32_t format_id = 0;
    esp_gmf_err_t dec_ret = esp_gmf_audio_helper_get_audio_type_by_uri(uri, &format_id);
    if (dec_ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Dateityp nicht erkannt aus '%s': %d", uri, dec_ret);
        local2bt_lock_give();
        return;
    }
    esp_gmf_element_handle_t dec = NULL;
    if (esp_gmf_pipeline_get_el_by_name(local2bt_pipe, "aud_dec", &dec) == ESP_GMF_ERR_OK && dec != NULL) {
        esp_gmf_info_sound_t dec_info = {
            .format_id = format_id,
        };
        dec_ret = esp_gmf_audio_dec_reconfig_by_sound_info(dec, &dec_info);
        if (dec_ret != ESP_GMF_ERR_OK) {
            ESP_LOGE(TAG, "Decoder auf Typ 0x%x nicht einstellbar: %d", (unsigned)format_id, dec_ret);
            local2bt_lock_give();
            return;
        }
        ESP_LOGI(TAG, "Decoder auf Dateityp 0x%x eingestellt (%s)", (unsigned)format_id, uri);
    } else {
        ESP_LOGE(TAG, "aud_dec im Datei-Zweig nicht gefunden");
        local2bt_lock_give();
        return;
    }

    esp_gmf_pipeline_set_in_uri(local2bt_pipe, uri);

    /*
     * Der Datei-Zweig meldet dem Mischer sein Format - genau wie der I2S-Zweig.
     * Ohne diese Meldung bleiben die informationsabhaengigen Elemente
     * (aud_rate_cvt_file ab ESP_GMF_INFO_SOUND, esp_gmf_rate_cvt.c:186) im
     * Zustand STATE_NONE und das Registrieren der Jobs scheitert mit
     * ESP_GMF_ERR_NOT_READY (0xffffdff8).
     */
    esp_gmf_info_sound_t file_info = {
        .sample_rates = I2S2BT_RATE_HZ,
        .channels = I2S2BT_CHANNELS,
        .bits = I2S2BT_BITS,
    };
    esp_gmf_err_t ret = esp_gmf_pipeline_report_info(local2bt_pipe, ESP_GMF_INFO_SOUND, &file_info, sizeof(file_info));
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGW(TAG, "Datei-Format konnte nicht gemeldet werden: %d", ret);
    }

    ESP_LOGI(TAG, "Datei -> Mischer: %s (Eintrag %d)", uri, playlist_cur_index);
    esp_gmf_pipeline_loading_jobs(local2bt_pipe);
    esp_gmf_pipeline_run(local2bt_pipe);
    s_local2bt_laeuft = true;
    snprintf(s_local2bt_uri, sizeof(s_local2bt_uri), "%s", uri);
    local2bt_lock_give();
    local2bt_arena_release();   /* 0.9.80: erst jetzt - siehe oben */
    ESP_LOGI(TAG, "Nach dem Start des Datei-Zweigs: groesster Block %u Byte",
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}

/*
 * 'playfile <pfad>' - eine Datei direkt abspielen, ohne Wiedergabeliste.
 *
 * Pfad entweder als GMF-URI ("file://sdcard/test.mp3") oder als normaler Pfad
 * ("/sdcard/test.mp3"). Der Datei-Zweig laeuft in den Mischer, der Ton der
 * Vampire bleibt dabei hoerbar - das ist der Sinn des Mischers.
 */
void local2bt_play_file(const char *uri)
{
    if (uri == NULL || uri[0] == '\0') {
        ESP_LOGE(TAG, "Kein Dateiname angegeben");
        return;
    }

    char buf[192];
    if (strncmp(uri, "file://", 7) == 0) {
        snprintf(buf, sizeof(buf), "%s", uri);
    } else if (uri[0] == '/') {
        /* /sdcard/test.mp3 -> file://sdcard/test.mp3 */
        snprintf(buf, sizeof(buf), "file://%s", uri + 1);
    } else {
        snprintf(buf, sizeof(buf), "file://sdcard/%s", uri);
    }

    local2bt_play(buf);
}

void local2bt_play_next(void)
{
    if (local2bt_stream == NULL) {
        ESP_LOGE(TAG, "Local to BT stream is not initialized");
        return;
    }
    playlist_cur_index = (playlist_cur_index + 1) % playlist_len;
    local2bt_play(playlist[playlist_cur_index]);
}

void local2bt_play_prev(void)
{
    if (local2bt_stream == NULL) {
        ESP_LOGE(TAG, "Local to BT stream is not initialized");
        return;
    }
    playlist_cur_index = (playlist_cur_index + playlist_len - 1) % playlist_len;
    local2bt_play(playlist[playlist_cur_index]);
}

/* ------------------------------------------------------------------ */
/* Zugriffe fuer das I2C-Protokoll der Vampire (0.9.57)                */
/* ------------------------------------------------------------------ */

/*
 * Datei-Zweig anhalten - nur vormerken.
 *
 * Ausfuehren darf das NUR die stream_proc-Aufgabe: esp_gmf_pipeline_stop()
 * wartet auf den GMF-Task, und der I2C-Task darf dabei nicht blockieren (die
 * Antwort muss innerhalb der festen Wartezeit des Masters stehen). Deshalb
 * setzt dieser Aufruf nur local2bt_stop_requested; local2bt_process_stop_request()
 * raeumt auf. Der I2S-Zweig der Vampire laeuft dabei unberuehrt weiter - der
 * Mischer mischt beide Quellen, ein Stop betrifft nur die Datei.
 */
void local2bt_stop(void)
{
    local2bt_request_stop();
}

/** @brief Laeuft gerade eine Datei aus dem Datei-Zweig? */
bool local2bt_is_playing(void)
{
    return s_local2bt_laeuft;
}

/** @brief URI der laufenden Datei, oder "" wenn keine laeuft. */
const char *local2bt_current_uri(void)
{
    return s_local2bt_uri;
}

static void stream_proc_destroy(stream_user_data_t *user_d)
{
    ESP_LOGI(TAG, "stream_user_data_destroy %p", user_d);
    if (user_d) {
        free(user_d);
    }
}

/*
 * Speicher fuer die Stream-Daten - mit Rueckfall auf internes RAM.
 *
 * Vorher stand hier fest MALLOC_CAP_SPIRAM. Auf einem Modul OHNE PSRAM gibt es
 * diese Speicherart nicht, das calloc liefert NULL, und der I2S-Zweig startet
 * gar nicht:
 *
 *   I STREAM_PROC: I2S-Zweig ausgewaehlt (Datei-Zweig bleibt aus)
 *   E STREAM_PROC: calloc user data failed
 *   I STREAM_PROC: Stream state changed: ... state STARTED
 *   E STREAM_PROC: Stream user data not prepared for stream ...
 *
 * Reihenfolge: erst PSRAM versuchen (dort stoert es nicht), dann normales
 * calloc. Auf einem Modul MIT PSRAM kommt der erste Versuch zum Zug, ohne
 * PSRAM der zweite. MALLOC_CAP_DEFAULT waere die Kurzform, aber der
 * ausdrueckliche Rueckfall ist klarer und funktioniert in beiden Builds.
 */
static void *stream_user_data_calloc(void)
{
    void *p = heap_caps_calloc(1, sizeof(stream_user_data_t), MALLOC_CAP_SPIRAM);
    if (p == NULL) {
        p = calloc(1, sizeof(stream_user_data_t));
    }
    return p;
}

static void stream_proc_prepare(esp_bt_audio_stream_handle_t stream, stream_user_data_t **out)
{
    stream_user_data_t *user_d = stream_user_data_calloc();
    if (user_d == NULL) {
        ESP_LOGE(TAG, "calloc user data failed");
        *out = NULL;
        return;
    }
    esp_bt_audio_stream_codec_info_t codec_info = {0};
    esp_bt_audio_stream_get_codec_info(stream, &codec_info);
    ESP_LOGI(TAG, "Codec Info: type=%d, bits=%d, channels=%d, sample_rate=%d, cfg_size=%d, codec_cfg=%p",
             codec_info.codec_type,
             codec_info.bits,
             codec_info.channels,
             codec_info.sample_rate,
             codec_info.cfg_size,
             codec_info.codec_cfg);

    esp_bt_audio_stream_dir_t dir = ESP_BT_AUDIO_STREAM_DIR_UNKNOWN;
    if (esp_bt_audio_stream_get_dir(stream, &dir) != ESP_OK) {
        ESP_LOGE(TAG, "Get stream dir failed, stream=%p", stream);
        free(user_d);
        *out = NULL;
        return;
    }

    if (dir == ESP_BT_AUDIO_STREAM_DIR_SINK) {
        ESP_LOGI(TAG, "Prepare bt to codec pipeline");
        user_d->pipe = bt2codec_pipe;
        esp_gmf_io_bt_set_stream(ESP_GMF_PIPELINE_GET_IN_INSTANCE(user_d->pipe), stream);
        esp_audio_simple_dec_cfg_t simple_dec_cfg = {
            .dec_type = codec_info.codec_type == ESP_BT_AUDIO_STREAM_CODEC_SBC ? ESP_AUDIO_TYPE_SBC : ESP_AUDIO_TYPE_LC3,
            .dec_cfg = codec_info.codec_cfg,
            .cfg_size = codec_info.cfg_size,
        };
        esp_gmf_audio_dec_reconfig(user_d->pipe->head_el, &simple_dec_cfg);

        stream_proc_set_asrc_dest(user_d->pipe, 0, CODEC_DAC_SAMPLE_RATE, __builtin_popcount(codec_info.channels),
                                  CODEC_DAC_CHANNELS, bt2codec_asrc_weight, STREAM_PROC_ASRC_MAX_WEIGHT_LEN);

        stream_proc_prepare_playback_sync();
        stream_proc_post_pipeline_action(user_d->pipe, STREAM_PROC_PIPELINE_PREPARE);
    } else {
        uint8_t output_asrc_index = 0;
        uint32_t context = 0;
        bool report_codec_input_info = false;
        const char *uri = NULL;
        if (esp_bt_audio_stream_get_context(stream, &context) != ESP_OK) {
            ESP_LOGE(TAG, "Get stream context failed, stream=%p", stream);
        }
        if (context == ESP_BT_AUDIO_STREAM_CONTEXT_MEDIA) {
            ESP_LOGI(TAG, "Prepare local to bt pipeline");
            user_d->pipe = local2bt_pipe;
            local2bt_stream = stream;

            esp_audio_simple_dec_cfg_t simple_dec_cfg = {
                .dec_type = ESP_AUDIO_TYPE_MP3,
                .dec_cfg = NULL,
                .cfg_size = 0,
            };
            esp_gmf_audio_dec_reconfig(user_d->pipe->head_el, &simple_dec_cfg);
            uri = playlist[playlist_cur_index];
            ESP_LOGI(TAG, "Set media file: %s (index %d)", playlist[playlist_cur_index], playlist_cur_index);
        } else {
            ESP_LOGI(TAG, "Prepare codec to bt pipeline");
            user_d->pipe = codec2bt_pipe;
            output_asrc_index = 1;
            report_codec_input_info = true;
        }
        /*
         * Den Bluetooth-Ausgang nur setzen, wenn diese Pipeline ueberhaupt
         * einen hat.
         *
         * Der Datei-Zweig ist seit dem Umbau auf den Mischer reiner ZUBRINGER:
         * beim Anlegen wurde out_name = NULL uebergeben, es gibt also keinen
         * Ausgang. ESP_GMF_PIPELINE_GET_OUT_INSTANCE() liefert dann NULL, und
         * esp_gmf_io_bt_set_stream() bricht ab ("Got NULL Pointer") - schlimmer
         * noch, die Fehlerbehandlung riss danach den Heap mit:
         *
         *   assert failed: heap_caps_free heap_caps_base.c:80
         *   (heap != NULL && "free() target pointer is outside heap areas")
         *
         * Den Ausgang nach Bluetooth besitzt mixer_pipe; gesetzt wird er in
         * i2s2bt_set_stream().
         */
        if (ESP_GMF_PIPELINE_GET_OUT_INSTANCE(user_d->pipe) != NULL) {
            esp_gmf_io_bt_set_stream(ESP_GMF_PIPELINE_GET_OUT_INSTANCE(user_d->pipe), stream);
        }

        if (context == ESP_BT_AUDIO_STREAM_CONTEXT_MEDIA) {
            /*
             * Der Datei-Zweig braucht keinen eigenen Encoder und keinen ASRC
             * mehr - der Mischer erledigt Ausgang und Format. Was er braucht,
             * ist der Stream AM MISCHER, damit A2DP ueberhaupt sendet.
             */
            ESP_LOGI(TAG, "Datei-Zweig: Stream wird am Mischer gesetzt");
            i2s2bt_set_stream(stream);
            local2bt_play(uri);
            *out = user_d;
            return;
        }

        uint8_t output_src_ch = 1;
        float *asrc_weight = codec2bt_output_asrc_weight;
        stream_proc_set_asrc_dest(user_d->pipe, output_asrc_index, codec_info.sample_rate, output_src_ch,
                                  __builtin_popcount(codec_info.channels), asrc_weight,
                                  STREAM_PROC_ASRC_MAX_WEIGHT_LEN);

        esp_audio_enc_config_t enc_cfg = {
            .type = codec_info.codec_type == ESP_BT_AUDIO_STREAM_CODEC_SBC ? ESP_AUDIO_TYPE_SBC : ESP_AUDIO_TYPE_LC3,
            .cfg = codec_info.codec_cfg,
            .cfg_sz = codec_info.cfg_size,
        };
        esp_gmf_audio_enc_reconfig(user_d->pipe->last_el, &enc_cfg);
        stream_proc_cmd_t cmd = {
            .action = STREAM_PROC_PIPELINE_PREPARE,
            .pipe = user_d->pipe,
            .uri = uri,
            .report_codec_input_info = report_codec_input_info,
        };
        stream_proc_post_cmd(&cmd);
    }
    *out = user_d;
}

void stream_proc_state_chg(esp_bt_audio_stream_handle_t stream, esp_bt_audio_stream_state_t state)
{
    const char *state_str[] = {"ALLOCATED", "STARTED", "STOPPED", "RELEASED"};
    esp_bt_audio_stream_dir_t dir = ESP_BT_AUDIO_STREAM_DIR_UNKNOWN;
    esp_bt_audio_stream_get_dir(stream, &dir);
    ESP_LOGI(TAG, "Stream state changed: stream %p, dir %d, state %s", stream, dir, state_str[state]);
    switch (state) {
        case ESP_BT_AUDIO_STREAM_STATE_ALLOCATED: {
            stream_user_data_t *user_dat = NULL;
            esp_bt_audio_stream_get_local_data(stream, (void **)&user_dat);
            if (user_dat) {
                stream_proc_destroy(user_dat);
                esp_bt_audio_stream_set_local_data(stream, NULL);
                user_dat = NULL;
            }

            /*
             * Ein Element kann nur an EINEN Task gebunden sein
             * (esp_gmf_pipeline.c:389 "not ready to register job"). Die
             * I2S-Pipeline und die Datei-Pipeline benutzen aber beide aud_asrc
             * und aud_enc - sie koennen deshalb nicht gleichzeitig laufen.
             *
             * Solange nicht gemischt wird (Schritt 2c), laeuft wahlweise der
             * eine oder der andere Zweig. Das Mischen loest das spaeter so wie
             * pipeline_howl: eigene Elemente pro Zweig, die erst im aud_mixer
             * zusammenkommen.
             */
            if (i2s2bt_requested && i2s2bt_pipe != NULL) {
                ESP_LOGI(TAG, "I2S-Zweig ausgewaehlt (Datei-Zweig bleibt aus)");
                user_dat = stream_user_data_calloc();
                if (user_dat == NULL) {
                    ESP_LOGE(TAG, "calloc user data failed");
                    break;
                }
                i2s2bt_set_stream(stream);
            } else {
                stream_proc_prepare(stream, &user_dat);
            }
            esp_bt_audio_stream_set_local_data(stream, user_dat);
            break;
        }
        case ESP_BT_AUDIO_STREAM_STATE_STARTED: {
            stream_user_data_t *user_d = NULL;
            esp_bt_audio_stream_get_local_data(stream, (void **)&user_d);
            if (user_d == NULL) {
                ESP_LOGE(TAG, "Stream user data not prepared for stream %p", stream);
                break;
            }
            if (i2s2bt_requested) {
                /* Der I2S-Zweig wurde bereits in i2s2bt_set_stream() gestartet. */
                break;
            }
            if (user_d->pipe) {
                if (dir == ESP_BT_AUDIO_STREAM_DIR_SINK) {
                    stream_proc_prepare_clk_sync(stream);
                }
                stream_proc_post_pipeline_action(user_d->pipe, STREAM_PROC_PIPELINE_RUN);
            } else {
                ESP_LOGE(TAG, "No pipeline for stream %p", stream);
            }
            break;
        }
        case ESP_BT_AUDIO_STREAM_STATE_STOPPED: {
            stream_user_data_t *user_d = NULL;
            esp_bt_audio_stream_get_local_data(stream, (void **)&user_d);
            if (user_d && user_d->pipe) {
                ESP_LOGI(TAG, "Schedule reset pipeline %p", user_d->pipe);
                stream_proc_post_pipeline_action(user_d->pipe, STREAM_PROC_PIPELINE_STOP_RESET);
            } else if (i2s2bt_requested) {
                /* I2S-Zweig: der wird hier direkt angehalten. */
                i2s2bt_stop();
            }
            if (dir == ESP_BT_AUDIO_STREAM_DIR_SINK) {
                stream_proc_deinit_clk_sync();
                stream_proc_deinit_playback_sync();
            }
            break;
        }
        case ESP_BT_AUDIO_STREAM_STATE_RELEASED: {
            stream_user_data_t *user_d = NULL;
            esp_bt_audio_stream_get_local_data(stream, (void **)&user_d);
            if (dir == ESP_BT_AUDIO_STREAM_DIR_SINK) {
                stream_proc_deinit_clk_sync();
                stream_proc_deinit_playback_sync();
            }
            if (user_d) {
                stream_proc_destroy(user_d);
                esp_bt_audio_stream_set_local_data(stream, NULL);
            }
            if (local2bt_stream == stream) {
                local2bt_stream = NULL;
            }
            if (i2s2bt_stream == stream) {
                i2s2bt_stream = NULL;
            }
            break;
        }
        default:
            break;
    }
}

static esp_gmf_err_t bt2codec_pipe_event_cb(esp_gmf_event_pkt_t *pkt, void *ctx)
{
    if (pkt == NULL) {
        return ESP_GMF_ERR_OK;
    }
    if (pkt->type == ESP_GMF_EVT_TYPE_CHANGE_STATE) {
        ESP_LOGI(TAG, "[bt2codec pipeline] state => %s(%d)", gmf_state_to_str(pkt->sub), pkt->sub);
    }
    return ESP_GMF_ERR_OK;
}

static esp_gmf_err_t codec2bt_pipe_event_cb(esp_gmf_event_pkt_t *pkt, void *ctx)
{
    if (pkt == NULL) {
        return ESP_GMF_ERR_OK;
    }
    if (pkt->type == ESP_GMF_EVT_TYPE_CHANGE_STATE) {
        ESP_LOGI(TAG, "[codec2bt pipeline] state => %s(%d)", gmf_state_to_str(pkt->sub), pkt->sub);
    }
    return ESP_GMF_ERR_OK;
}

/*
 * Ende der Wiedergabe sauber beenden.
 *
 * Problem auf Hardware: nach dem letzten MP3-Byte meldet die Datei-IO
 * "No more data" und setzt is_done. Decoder- und ASRC-Job werden daraufhin
 * fertig, die Pipeline bleibt aber im Zustand RUNNING und der Encoder sendet
 * weiter, was er noch vom Eingangsport bekommt. Ueber A2DP hoert man das als
 * leises Brummen, das nicht mehr aufhoert, weil niemand die Uebertragung
 * beendet.
 *
 * Ein eigener Task-Strategie-Weg (esp_gmf_task_set_strategy_func mit
 * GMF_TASK_STRATEGY_ACTION_STOP) wurde versucht und hat NICHT gegriffen:
 * esp_gmf_task.c fragt die Strategie erst, wenn ALLE Jobs fertig sind
 * (Zeile 347) - im Log fehlte die Zeile "Finish, strategy action:".
 *
 * Der Weg, den die offiziellen GMF-Beispiele gehen (und der hier uebernommen
 * ist): das Pipeline-Event merken und die Pipeline ausserhalb des GMF-Tasks
 * stoppen. Siehe local2bt_request_stop() oben.
 */

static esp_gmf_err_t local2bt_pipe_event_cb(esp_gmf_event_pkt_t *pkt, void *ctx){
    if (pkt == NULL) {
        return ESP_GMF_ERR_OK;
    }
    if (pkt->type == ESP_GMF_EVT_TYPE_CHANGE_STATE) {
        ESP_LOGI(TAG, "[a2dp source pipeline] state => %s(%d)", gmf_state_to_str(pkt->sub), pkt->sub);
        /*
         * Muster wie in den GMF-Beispielen (pipeline_play_sdcard_music,
         * pipeline_loop_play_no_gap): hier nur MERKEN, gestoppt wird im
         * stream_proc_task. Dieser Callback laeuft im Kontext des GMF-Tasks,
         * ein Stop von hier aus wuerde auf sich selbst warten.
         */
        if (pkt->sub == ESP_GMF_EVENT_STATE_FINISHED) {
            ESP_LOGI(TAG, "A2DP Source finished - Wiedergabe wird beendet");
            local2bt_request_stop();
        } else if (pkt->sub == ESP_GMF_EVENT_STATE_ERROR) {
            /*
             * Fehler im DATEI-Zweig (0.9.61).
             *
             * Hier wurde bisher zusaetzlich local2bt_request_media_stop()
             * gerufen - das stoppte die ganze A2DP-Uebertragung, obwohl nur der
             * Datei-Zubringer gescheitert war. Der Mischer traegt aber weiter
             * den I2S-Ton der Vampire: ein fehlgeschlagenes playfile darf den
             * nicht mitreissen. Am 08.10. war genau das zu sehen
             * (docs/MP3_STARTFEHLER.md):
             *
             *   E ESP_GMF_PORT: ... reallocate payload buffer failed, el:aud_lin_resample_file
             *   E STREAM_PROC: A2DP Source error
             *   W STREAM_PROC: A2DP-Uebertragung wird angehalten   <- hier
             *
             * Der Datei-Zweig wird weiterhin gestoppt und zurueckgesetzt
             * (local2bt_process_stop_request raeumt auch den ERROR-Zustand auf),
             * der Stream laeuft weiter.
             */
            ESP_LOGE(TAG, "Fehler im Datei-Zweig - Wiedergabe wird beendet, Stream laeuft weiter");
            local2bt_request_stop();
        }
    }
    return ESP_GMF_ERR_OK;
}

/*
 * Die beiden Kodec-Pipelines (Bluetooth-Empfang -> Codec und Codec ->
 * Bluetooth-Sender) gehoeren zum Board-Manager-Zweig: sie brauchen io_codec_dev
 * und einen angeschlossenen Audio-Codec. Beides gibt es in diesem Projekt nicht
 * mehr - die Anwendung ist reine A2DP-QUELLE, der Ausgang geht ueber den
 * Mischer nach Bluetooth.
 *
 * Beide Pipelines wurden trotzdem angelegt und belegten Speicher; ausserdem
 * meldeten sie beim Start Fehler, weil io_codec_dev nicht im Pool liegt:
 *
 *   E ESP_GMF_POOL: Not found WRITER port, name:io_codec_dev
 *   E ESP_GMF_PIPELINE: esp_gmf_pipeline.c:363 (esp_gmf_pipeline_bind_task): Got NULL Pointer
 *
 * Mit POOL_SMALL (Module ohne PSRAM) entfallen sie ganz.
 */
#if !POOL_SMALL
static void setup_pipeline_bt2codec(esp_gmf_pool_handle_t pool)
{
    const char *name[] = {"aud_dec", "aud_asrc"};
    esp_gmf_pool_new_pipeline(pool, "io_bt", name, sizeof(name) / sizeof(char *), "io_codec_dev", &bt2codec_pipe);
    esp_gmf_pipeline_set_event(bt2codec_pipe, bt2codec_pipe_event_cb, NULL);

    esp_gmf_task_cfg_t cfg = DEFAULT_ESP_GMF_TASK_CONFIG();
    cfg.thread.core = 0;
    cfg.thread.stack = 5120;
    cfg.thread.prio = 15;
    cfg.thread.stack_in_ext = true;
    cfg.name = "bt2codec_task";
    esp_gmf_task_init(&cfg, &bt2codec_task);

    esp_gmf_pipeline_bind_task(bt2codec_pipe, bt2codec_task);
}

static void setup_pipeline_codec2bt(esp_gmf_pool_handle_t pool)
{
    const char *name[] = {"aud_asrc", "ai_aec", "aud_asrc", "aud_enc"};
    esp_gmf_pool_new_pipeline(pool, "io_codec_dev", name, sizeof(name) / sizeof(char *), "io_bt", &codec2bt_pipe);
    esp_gmf_pipeline_set_event(codec2bt_pipe, codec2bt_pipe_event_cb, NULL);

    esp_gmf_task_cfg_t cfg = DEFAULT_ESP_GMF_TASK_CONFIG();
    cfg.thread.core = 1;
    cfg.thread.stack = 5120;
    cfg.thread.prio = 15;
    cfg.thread.stack_in_ext = true;
    cfg.name = "codec2bt_task";
    esp_gmf_task_init(&cfg, &codec2bt_task);

    stream_proc_set_asrc_dest(codec2bt_pipe, 0, 8000, CODEC_ADC_CHANNELS, CODEC_ADC_CHANNELS,
                              codec2bt_input_asrc_weight, STREAM_PROC_ASRC_MAX_WEIGHT_LEN);

    esp_gmf_pipeline_bind_task(codec2bt_pipe, codec2bt_task);
}
#endif  /* !POOL_SMALL */

/*
 * Gemeinsames Format, auf das beide Zubringer wandeln und das der Mischer
 * ausgibt. Es ist die A2DP-Rate des SBC-Encoders. Die #define stehen weiter
 * oben bei der Wiedergabeliste, weil auch local2bt_play() sie braucht.
 */
/*
 * Puffer fuer den Datei-Zweig VORAB anlegen (0.9.75).
 *
 * Warum: mit eingeschalteten BT-Profilen (die den Ton glatt halten, siehe
 * README 0.9.74) ist der DRAM nach dem BT-Init und dem Streamstart so
 * zersplittert, dass der groesste zusammenhaengende Block nur noch 272 Byte
 * gross ist (/tmp/final974.log). Eine 4608-Byte-Anforderung des MP3-Dekoders
 * kann dann nie mehr klappen, und der BT-Stack scheitert gleich mit
 * ("calloc failed", "Failed to send frame batch: ESP_ERR_NO_MEM").
 *
 * Deshalb werden die zwei grossen Puffer des Datei-Zweigs JETZT angefordert -
 * beim Pipelineaufbau, wo der DRAM noch zusammenhaengend ist. Sie bleiben am
 * Port haengen (der Port gibt seinen self_payload erst beim Zerstoeren der
 * Pipeline frei, nicht bei Stop/Reset) und werden fuer jeden Titel
 * wiederverwendet.
 */
static void reserve_output_payload(esp_gmf_pipeline_handle_t pipe,
                                   const char *tag, int bytes, uint8_t align)
{
    esp_gmf_element_handle_t el = NULL;
    esp_gmf_port_handle_t    out;
    esp_gmf_payload_t       *load = NULL;

    if (pipe == NULL) {
        return;
    }
    if (esp_gmf_pipeline_get_el_by_name(pipe, tag, &el) != ESP_GMF_ERR_OK || el == NULL) {
        ESP_LOGW(TAG, "Vorab-Puffer: Element '%s' nicht gefunden", tag);
        return;
    }
    out = ESP_GMF_ELEMENT_GET(el)->out;
    if (out == NULL) {
        ESP_LOGW(TAG, "Vorab-Puffer: '%s' hat keinen Ausgang", tag);
        return;
    }
    if (esp_gmf_payload_new(&load) != ESP_GMF_ERR_OK || load == NULL) {
        ESP_LOGW(TAG, "Vorab-Puffer: Payload fuer '%s' nicht angelegt", tag);
        return;
    }
    load->buf = (uint8_t *)heap_caps_aligned_alloc(align, (size_t)bytes, MALLOC_CAP_DEFAULT);
    if (load->buf == NULL) {
        ESP_LOGW(TAG, "Vorab-Puffer: %d Byte fuer '%s' nicht verfuegbar", bytes, tag);
        esp_gmf_payload_delete(load);
        return;
    }
    load->buf_length = (size_t)bytes;
    load->needs_free  = 1;
    if (esp_gmf_port_set_payload(out, load) != ESP_GMF_ERR_OK) {
        ESP_LOGW(TAG, "Vorab-Puffer: '%s' nahm den Puffer nicht an", tag);
        esp_gmf_payload_delete(load);
        return;
    }
    ESP_LOGI(TAG, "Vorab-Puffer fuer '%s': %d Byte (Ausrichtung %u) - bleibt reserviert",
             tag, bytes, (unsigned)align);
}


static void setup_pipeline_local2bt(esp_gmf_pool_handle_t pool)
{
    /*
     * Datei-Zubringer fuer den Mischer: dekodieren und auf das Mischer-Format
     * bringen (44,1 kHz, 16 Bit, stereo), dann ueber einen Ringpuffer in den
     * aud_mixer. Encoder und Bluetooth-Ausgang liegen in mixer_pipe - ein
     * Element kann nur an EINEN Task gebunden werden.
     *
     * Der Raten- und der Bit-Wandler sind hier durch den eigenen linearen
     * Umsetzer ersetzt (aud_lin_resample_file, siehe pool_reg.c): GMFs
     * aud_rate_cvt fordert fuer 44100 <-> 48000 eine Koeffizienten-Matrix von
     * 15360 Byte und scheitert damit auf dem Modul ohne PSRAM an der
     * Fragmentierung - MP3 liess sich deshalb gar nicht abspielen.
     */
    const char *name[] = {"aud_dec", "aud_lin_resample_file"};
    esp_gmf_pool_new_pipeline(pool, "io_file", name, sizeof(name) / sizeof(char *), NULL, &local2bt_pipe);
    esp_gmf_pipeline_set_event(local2bt_pipe, local2bt_pipe_event_cb, NULL);

    /*
     * Rate und Bittiefe macht der lineare Umsetzer selbst: er uebernimmt die
     * Rate der Datei aus der Toninformation (er hat dependency = true und
     * bekommt sie ueber den Event-Kanal) und gibt 16 Bit aus. Die Zielrate
     * zieht i2s2bt_set_stream() aus der A2DP-Aushandlung nach.
     */
    /* Letztes Element: Kanalwandler auf Stereo (Bypass, wenn schon stereo). */
    /*
     * Kanalwandler entfaellt (0.9.33): aud_lin_resample_file gibt immer stereo
     * aus und kopiert Mono selbst auf beide Kanaele. Damit ist ein eigenes
     * ch_cvt-Element im Datei-Zweig nicht mehr noetig - ein Element weniger,
     * das Zustand halten kann.
     */

    esp_gmf_task_cfg_t cfg = DEFAULT_ESP_GMF_TASK_CONFIG();
    /*
     * Datei-Zweig auf KERN 0 - geaendert am 03.10.
     *
     * Fruer lag er auf Kern 1, weil damals der Mischer auf Kern 0 sass und
     * dieser mit 98 % in den Interrupt-Watchdog lief. Diese Verteilung ist
     * inzwischen ueberholt: der Mischer wurde auf Kern 1 verschoben (er wurde
     * auf Kern 0 vom Bluetooth-Stack verdraengt, siehe mixer_task), und damit
     * lagen auf Kern 1 ploetzlich ALLE vier Audio-Tasks:
     *
     *   Kern 0: nur Bluetooth                      -> 24 % (fast leer)
     *   Kern 1: mixer 18% + i2s2bt + io_i2s + Datei -> 76 % mit WAV
     *
     * Der Datei-Zweig ist der gierigste der drei Zubringer, weil er NICHT in
     * Echtzeit arbeitet: er dekodiert so schnell er kann und fuellt seinen
     * Ringpuffer sofort. Auf demselben Kern verdraengt er damit den
     * Echtzeit-Zweig der Vampire - genau das war schon einmal zu sehen
     * ("Puffer I2S-Zweig: 4888/20480 -> 0/20480, leer 1 mal").
     *
     * Jetzt: I2S-Zweig und Mischer auf Kern 1 (47 % Reserve), Bluetooth und
     * Datei-Zweig auf Kern 0 (76 % Reserve). Beide Zubringer tauschen nur ueber
     * den Ringpuffer mit dem Mischer, die Kernzuordnung ist deshalb unkritisch.
     */
    cfg.thread.core = 0;
    cfg.thread.stack = 5120;
    cfg.thread.prio = 15;
    cfg.thread.stack_in_ext = true;
    cfg.name = "local2bt_task";
    esp_gmf_task_init(&cfg, &local2bt_task);

    /* Keine eigene Task-Strategie: das Dateiende wird ueber das Pipeline-Event
     * behandelt (siehe local2bt_request_stop). Eine Strategie waere hier auch
     * wirkungslos, denn esp_gmf_task.c fragt sie erst, wenn ALLE Jobs fertig
     * sind - das war auf Hardware nicht der Fall. */

    esp_gmf_pipeline_bind_task(local2bt_pipe, local2bt_task);
    /*
     * 0.9.75: die zwei grossen Puffer des Datei-Zweigs sofort reservieren -
     * Dekoderausgang (ein MP3-Frame = 4608) und der Ausgang unseres Wandlers
     * (1152 Frames x 48000/44100 x 2 Kanaele x 2 Byte = 5016, aufgerundet).
     */
    local2bt_arena_reserve("beim Start");
    reserve_output_payload(local2bt_pipe, "aud_dec", 4608, 16u);
    /*
     * Der Wandler bekommt seinen Ausgang erst beim Verbinden mit dem Mischer
     * (connect_branch_to_mixer); dort wird sein Puffer reserviert - gemessen am
     * 08.10.: "Vorab-Puffer: 'aud_lin_resample_file' hat keinen Ausgang", und
     * genau dieser Puffer fehlte dann beim Abspielen.
     */
}

/*
 * Format der QUELLE, also was die Vampire sendet. Diese Werte werden als
 * Toninformation an die Pipeline gemeldet - ohne sie bleiben die
 * informationsabhaengigen Elemente (aud_rate_cvt, aud_bit_cvt) im Zustand
 * STATE_NONE und die Jobs lassen sich nicht registrieren.
 */
#define I2S2BT_SRC_RATE_HZ  60000
/* 32 Bit: der I2S-Empfaenger liest die vollen 32-Bit-Frames (i2s_input.c). */
#define I2S2BT_SRC_BITS     32
#define I2S2BT_SRC_CHANNELS 2

/*
 * Nur noch die Zubringer fuer den Mischer.
 *
 * Der I2S-Zweig wandelt das Vampire-Format (60 kHz, 32 Bit) auf das
 * Mischer-Format um: 44,1 kHz, 16 Bit, stereo. Der Mischer selbst, die
 * Ratenwandlung auf die A2DP-Rate und der Encoder liegen in mixer_pipe - so
 * wie in pipeline_howl, wo pipe_music und pipe_mic jeweils eigene Elemente
 * haben und nur der aud_mixer in einer dritten Pipeline steht.
 *
 * Ohne Encoder hier: ein Element kann nur an EINEN Task gebunden werden, und
 * das ist jetzt mixer_task.
 */
static void setup_pipeline_i2s2bt(esp_gmf_pool_handle_t pool)
{
    /*
     * Der I2S-Zweig besteht aus EINEM Element: aud_lin_resample
     * (linear_resample.c) - 60000 Hz/32 Bit -> A2DP-Rate/16 Bit, stereo.
     *
     * Es ist Kopf UND Ende zugleich: Kopf, weil die Toninformation beim ersten
     * dependency-Element stehen bleibt (eigener event_receiver noetig); Ende,
     * weil sein Ausgang fuer den Ringpuffer zum Mischer frei sein muss.
     */    {
        const char *name[1] = {"aud_lin_resample"};
        size_t name_num = 1;

        esp_gmf_err_t ret = esp_gmf_pool_new_pipeline(pool, "io_i2s", name, name_num,
                                                      NULL, &i2s2bt_pipe);
        if (ret != ESP_GMF_ERR_OK || i2s2bt_pipe == NULL) {
            ESP_LOGE(TAG, "I2S-Pipeline konnte nicht erstellt werden: %d (io_i2s vorhanden?)", ret);
            i2s2bt_pipe = NULL;
            return;
        }
        esp_gmf_pipeline_set_event(i2s2bt_pipe, local2bt_pipe_event_cb, NULL);
        ESP_LOGI(TAG, "I2S-Zweig: 60000 Hz/32 Bit -> %d Hz/%d Bit", I2S2BT_RATE_HZ, I2S2BT_BITS);


        esp_gmf_task_cfg_t cfg = DEFAULT_ESP_GMF_TASK_CONFIG();
        cfg.thread.core = 1;
        cfg.thread.stack = 5120;
        /*
         * Prioritaet 16: HOEHER als der Datei-Zweig (local2bt_task, 15).
         *
         * Der I2S-Zweig ist die Echtzeit-Quelle (Vampire). Im Mischbetrieb
         * laufen io_i2s, i2s2bt_task und local2bt_task alle auf Kern 1 - bei
         * gleicher Prioritaet verdraengt der gierige Datei-Decoder den
         * Echtzeit-Zweig, und der I2S-Puffer laeuft leer (gemessen:
         * "Puffer I2S-Zweig: 0/20480 (0%), leer 1 mal" waehrend der WAV lief).
         * Echtzeit schlaegt Puffer.
         */
        cfg.thread.prio = 16;
        cfg.thread.stack_in_ext = true;
        cfg.name = "i2s2bt_task";
        esp_gmf_task_init(&cfg, &i2s2bt_task);

        esp_gmf_pipeline_bind_task(i2s2bt_pipe, i2s2bt_task);
        dump_pipeline("I2S-Zweig", i2s2bt_pipe);
    }
}

/*
 * Der Mischer samt Ausgang - das Herz des Ganzen.
 *
 * Aufbau (Vorbild pipeline_howl.c:239-286):
 *   mixer_pipe = aud_mixer -> aud_rate_cvt -> aud_bit_cvt -> aud_enc -> io_bt
 *   io_bt wird zur Laufzeit mit dem A2DP-Stream verkabelt.
 *
 * Die beiden Zubringer kommen ueber Ringpuffer an den aud_mixer. Der Mischer
 * hat einen MULTI-Eingangsport (ESP_GMF_EL_PORT_CAP_MULTI, esp_gmf_mixer.c:453),
 * jeder Zubringer haengt seinen eigenen Port ein.
 *
 * Wichtig fuer unser Ziel "Vampire hoert immer zu": der Mischer blockiert
 * NICHT, wenn ein Eingang nichts liefert - bei Timeout fuellt er den fehlenden
 * Teil mit Nullen auf (esp_gmf_mixer.c:211-218). Er gibt erst dann Stille aus,
 * wenn ALLE Eingaenge beendet sind. Deshalb kann der Datei-Zweig pausieren,
 * waehrend der I2S-Zweig weiterlaeuft.
 */
#define MIXER_SRC_NUM 2
#define MIXER_SRC_I2S 0
#define MIXER_SRC_FILE 1

/*
 * Gewicht 1.0 = voller Pegel; beim Dateizweig etwas darunter, damit die
 * Vampire im Vordergrund bleibt.
 *
 * Die Wartezeiten (prefill/transit) stehen weiter oben bei s_mixer_prefill_ms -
 * sie sind zur Laufzeit einstellbar.
 */
#define MIXER_WEIGHT_I2S   1.0f
#define MIXER_WEIGHT_FILE  0.7f

/*
 * Groesse der Ringpuffer zwischen einem Zubringer und dem Mischer.
 *
 * 20 x 1024 Byte = 20 KB je Zweig (zwei Zweige = 40 KB). Zum Vergleich: das
 * offizielle Beispiel (pipeline_howl.c:246) nimmt 10 x 1024 = 10 KB und setzt
 * damit voraus, dass die Quellen immer rechtzeitig liefern.
 *
 * Bezug zur Datenrate: der I2S-Zweig liefert nach der Wandlung 44100 Hz *
 * 2 Kanaele * 2 Byte = 176400 Byte/s, ein 20-KB-Puffer deckt also ~116 ms ab.
 *
 * WARUM NICHT GROESSER: auf einem Modul OHNE PSRAM stehen nur ~234 KiB
 * interner RAM zur Verfuegung. Mit 40 KB je Zweig (80 KB zusammen) ging dem
 * Mischer der Speicher aus:
 *
 *   E ESP_GMF_BLOCK: esp_gmf_block.c:117 (esp_gmf_block_create): Memory exhausted
 *
 * Der Mischer startet dann gar nicht (Stream sofort STOPPED, Puffer bleiben
 * leer). 20 KB je Zweig ist der Kompromiss: genug Reserve gegen kurze Stockungen,
 * aber es bleibt Speicher fuer den Mischer.
 *
 */
/*
 * WARUM 12 UND NICHT MEHR 20 (geaendert 03.10.):
 *
 * Der Datei-Zweig scheiterte im No-PSRAM-Build an genau dieser Zeile:
 *
 *   E ESP_AE_RATE_CVT: Failed to allocate memory for
 *                      'coefficients matrix'(15360) on matrix_init
 *
 * 15360 Byte fehlten. GMF braucht kein PSRAM (siehe README_RATE_CVT.md,
 * "Heap Memory(Byte)") - es fehlte schlicht interner RAM.
 *
 * Diese beiden Ringpuffer sind der groesste Posten, den wir selbst bestimmen:
 * je Zweig MIXER_DB_ITEMS * MIXER_DB_ITEM_SIZE. Bei 20 KB je Zweig (40 KB
 * zusammen) fehlten 15 KB; mit 12 KB je Zweig (24 KB zusammen) werden 16 KB
 * frei - genug fuer die Matrix, mit etwas Luft.
 *
 * Vertretbar, weil der I2S-Zweig-Puffer durchgehend bei 100 % stand (Minimum
 * 18560 von 20480) - er hatte also Reserve. Die frueheren Einbrueche auf 22 %
 * kamen vom Mischer auf dem falschen Kern (siehe mixer_task), nicht von der
 * Puffergroesse.
 *
 * 12 KB decken bei 176400 Byte/s rund 70 ms ab - fuer die Uebergabe zwischen
 * zwei Tasks auf demselben Kern reichlich.
 */
/*
 * Blockgroesse des Mischers in Byte (siehe setup_pipeline_mixer).
 * 1024 Byte = 256 Stereoframes bei 16 Bit = 5,3 ms bei 48 kHz.
 */
#define MIXER_PROC_BYTES    1024
/*
 * I2S-Zweig: 12 KB (0.9.69 wieder 12).
 *
 * In 0.9.63 wurden hier 2 KB fuer den MP3-Dekoder abgezweigt (10 statt 12 KB).
 * Feldmeldung vom 08.10.2026: "sound ist kratzig" - genau die Aussetzer, vor
 * denen der Kommentar damals gewarnt hat ("erste Stellschraube"). Der Puffer
 * stand bei 20 KB durchgehend auf 100 % (Minimum 18560 von 20480), 12 KB decken
 * bei 176400 Byte/s rund 70 ms ab. Die 2 KB kommen jetzt aus den Aufgaben-Stacks
 * der I2C-Bruecke (V4_TASK_STACK), die bei echtem V4-Verkehr nur 2352 Byte
 * belegen (Mitschnitt /tmp/v4_traffic.log: 3792 von 6144 frei).
 */
#define MIXER_DB_ITEMS      12
#define MIXER_DB_ITEM_SIZE  1024
/*
 * Der Datei-Zweig bekommt nur 4 KB (0.9.31).
 *
 * Er ist NICHT echtzeitkritisch: der Decoder laeuft so schnell er kann und
 * wird vom Ringpuffer gebremst.
 *
 * WARUM 8 KB UND NICHT WENIGER (Lehre aus 0.9.31):
 * Eine MP3-Ausgabe ist 1152 Frames je Kanal = 4608 Byte; nach der
 * Ratenwandlung 44100 -> 48000 sind es 5016 Byte (im Log:
 * "Block 2: in_frames=1152 ... out_samples=2508"). Mit nur 4 KB passte ein
 * Block NICHT in den Ring. Der Ring schreibt dann in Teilen
 * (esp_gmf_ringbuffer.c:242-278) und wartet zwischendurch auf Platz. Wird der
 * Zweig am Dateiende abgebrochen, waehrend so ein Teil-Schreibvorgang laeuft,
 * bleibt ein HALBER Frame liegen (esp_gmf_ringbuffer.c:259 "abort_write") -
 * und der Datenstrom ist danach um 2 Byte verschoben. Hoerbar als Verzerrung
 * ab dem zweiten Titel (die erste Datei lief, weil der Ring da leer war).
 */
/*
 * 0.9.78: 8 -> 6. Der Ring muss EINEN Dekoderblock aufnehmen: nach der
 * Ratenwandlung 44100 -> 48000 sind das 5016 Byte, 6 x 1024 = 6144 passt also
 * weiterhin. Die 2 KB fehlen dem MP3-Dekoder, der beim Oeffnen rund 32 KB am
 * Stueck braucht (siehe Dekoder-Arena).
 */
#define FILE_DB_ITEMS       6

static esp_gmf_err_t setup_pipeline_mixer(esp_gmf_pool_handle_t pool)
{
    /*
     * Ohne aud_rate_cvt und aud_bit_cvt (0.9.31).
     *
     * Beide waren aus dem offiziellen Beispiel uebernommen, sind hier aber
     * reine Durchlaeufer: der Mischer gibt bereits GENAU das ausgehandelte
     * Format aus (48000 bzw. 44100 Hz, 16 Bit, stereo), und
     * esp_gmf_rate_cvt.c:61 setzt in dem Fall "bypass = src_rate == dest_rate".
     * Sie belegten also nur Port-Puffer. Der Encoder bekommt seine Parameter
     * ohnehin in i2s2bt_set_stream() gesetzt.
     */
    /*
     * Seit 0.9.56 haengt der Equalizer (aud_eq) zwischen Mischer und Encoder -
     * eine Instanz formt beide Quellen (Vampire und Datei), siehe pool_reg.c.
     */
    const char *name[] = {"aud_mixer", "aud_eq", "aud_enc_mix"};
    esp_gmf_err_t ret = esp_gmf_pool_new_pipeline(pool, NULL, name, sizeof(name) / sizeof(char *),
                                                  "io_bt", &mixer_pipe);
    if (ret != ESP_GMF_ERR_OK || mixer_pipe == NULL) {
        ESP_LOGE(TAG, "Mischer-Pipeline konnte nicht erstellt werden: %d", ret);
        mixer_pipe = NULL;
        return ESP_GMF_ERR_FAIL;
    }

    /*
     * BLOCKGROESSE DES MISCHERS AUF 1024 BYTE (0.9.35).
     *
     * Der Mischer holt sich je Aufruf und Eingang
     *   MIXER_GET_FRAME_BYTE_SIZE = 10 ms * rate * kanaele * bits / 8000
     * also bei 48 kHz/stereo/16 Bit 1920 Byte (esp_gmf_mixer.c:24-25). Liefert
     * ein Zubringer weniger, fuellt der Mischer den Rest mit NULLEN
     * (esp_gmf_mixer.c:216-218):
     *
     *   read_len = in_load[i]->valid_size;
     *   if (read_len < process_num) memset(in_arr[i] + read_len, 0, ...);
     *
     * Unser Datei-Zweig liefert 1024 Byte je Block (der WAV-Decoder gibt 512
     * Byte aus, das ergibt 256 Stereoframes = 1024 Byte). Damit waren in jedem
     * 10-ms-Block 896 Byte Stille - hoerbar als stotternder Sinus. Die Vampire
     * war sauber, weil ihr Ringpuffer voll ist und der Mischer dort immer die
     * vollen 1920 Byte bekommt.
     *
     * process_num kommt aus in_attr.data_size, wenn es nicht 0 ist
     * (esp_gmf_mixer.c:119) - deshalb wird es hier einmal fest gesetzt.
     */
    esp_gmf_element_handle_t mixer_el = NULL;
    if (esp_gmf_pipeline_get_el_by_name(mixer_pipe, "aud_mixer", &mixer_el) == ESP_GMF_ERR_OK && mixer_el != NULL) {
        ESP_GMF_ELEMENT_GET(mixer_el)->in_attr.data_size = MIXER_PROC_BYTES;
        ESP_LOGI(TAG, "Mischer-Blockgroesse auf %d Byte gesetzt (statt 1920 bei 48 kHz)", MIXER_PROC_BYTES);
    }
    esp_gmf_pipeline_set_event(mixer_pipe, local2bt_pipe_event_cb, NULL);

    /* Mischer auf unser Format einstellen und die zwei Zubringer benennen. */
    esp_gmf_element_handle_t mixer = NULL;
    if (esp_gmf_pipeline_get_el_by_name(mixer_pipe, "aud_mixer", &mixer) == ESP_GMF_ERR_OK && mixer != NULL) {
        esp_ae_mixer_cfg_t *cfg = (esp_ae_mixer_cfg_t *)OBJ_GET_CFG(mixer);
        if (cfg != NULL) {
            cfg->sample_rate = I2S2BT_RATE_HZ;
            cfg->channel = I2S2BT_CHANNELS;
            cfg->bits_per_sample = I2S2BT_BITS;
            cfg->src_num = MIXER_SRC_NUM;
            cfg->src_info = esp_gmf_oal_calloc(MIXER_SRC_NUM, sizeof(esp_ae_mixer_info_t));
            if (cfg->src_info == NULL) {
                ESP_LOGE(TAG, "Kein Speicher fuer die Mischer-Quellen");
                return ESP_GMF_ERR_MEMORY_LACK;
            }
            cfg->src_info[MIXER_SRC_I2S].weight1 = MIXER_WEIGHT_I2S;
            cfg->src_info[MIXER_SRC_I2S].weight2 = MIXER_WEIGHT_I2S;
            cfg->src_info[MIXER_SRC_I2S].transit_time = s_mixer_transit_ms;
            cfg->src_info[MIXER_SRC_FILE].weight1 = MIXER_WEIGHT_FILE;
            cfg->src_info[MIXER_SRC_FILE].weight2 = MIXER_WEIGHT_FILE;
            cfg->src_info[MIXER_SRC_FILE].transit_time = s_mixer_transit_ms;
            ESP_LOGI(TAG, "Mischer: %d Hz, %d Bit, %d ch, %d Quellen (I2S %.1f, Datei %.1f)",
                     I2S2BT_RATE_HZ, I2S2BT_BITS, I2S2BT_CHANNELS, MIXER_SRC_NUM,
                     MIXER_WEIGHT_I2S, MIXER_WEIGHT_FILE);
        }
    } else {
        ESP_LOGE(TAG, "aud_mixer nicht gefunden");
    }

    /* Ratenwandlung auf die A2DP-Rate (die Datei kommt z. B. mit 48 kHz an). */
    esp_gmf_element_handle_t rate = NULL;
    if (esp_gmf_pipeline_get_el_by_name(mixer_pipe, "aud_rate_cvt", &rate) == ESP_GMF_ERR_OK && rate != NULL) {
        esp_gmf_rate_cvt_set_dest_rate(rate, I2S2BT_RATE_HZ);
    }
    esp_gmf_element_handle_t bit = NULL;
    if (esp_gmf_pipeline_get_el_by_name(mixer_pipe, "aud_bit_cvt", &bit) == ESP_GMF_ERR_OK && bit != NULL) {
        esp_gmf_bit_cvt_set_dest_bits(bit, I2S2BT_BITS);
    }

    esp_gmf_task_cfg_t cfg = DEFAULT_ESP_GMF_TASK_CONFIG();
    /*
     * Der Mischer laeuft auf Kern 0, nicht auf Kern 1.
     *
     * Gemessen mit der eingebauten Lastanzeige: mit dem Mischer auf Kern 1
     * lagen dort 79 % an (I2S-Ratenwandlung 60->44,1 kHz UND SBC-Encoder),
     * auf Kern 0 nur 48 %. Folge war der Timer-Watchdog:
     *
     *   CPU-Last: Kern0 48%, Kern1 79%  (22 Tasks)
     *   rst:0x8 (TG1WDT_SYS_RESET)
     *
     * und hoerbar ein abgehackter Ton. Der Datei-Zweig (local2bt_task) liegt
     * bereits auf Kern 0; beide tauschen nur ueber den Ringpuffer aus, die
     * Kernzuordnung ist deshalb unkritisch.
     */
    /*
     * KERN 1, nicht Kern 0 - geaendert am 03.10. mit Begruendung.
     *
     * Vorher lag der Mischer auf Kern 0, weil dort frueher der Datei-Zweig lief.
     * Inzwischen liegen auf Kern 0 NUR noch Bluetooth (BTU_TASK 20,
     * btController 23) - und die druecken den Mischer (15) weg. Gemessen im
     * Mischbetrieb:
     *
     *   Kern 0: mixer_task 20% | BTU_TASK 5,8% | btController ~5% | IDLE0 nur 15%
     *   Kern 1: IDLE1 44% | i2s2bt_task 3% | io_i2s 1,2% | local2bt_task 0-20%
     *
     * Kern 0 ist mit 85 % praktisch voll, Kern 1 zur Haelfte leer. Wird der
     * Mischer vom BT-Stack verdraengt, liefert der Encoder zu spaet, der
     * A2DP-Sender (a2dp_src_send, Prio 10) bekommt nichts und die Senke laeuft
     * leer - hoerbar als Ruckler. Der Mischer gehoert auf den Kern mit Reserve.
     *
     * Alle vier Audio-Tasks liegen damit auf Kern 1 und tauschen nur ueber
     * Ringpuffer aus; die Kernzuordnung zwischen ihnen ist unkritisch.
     */
    cfg.thread.core = 1;
    cfg.thread.stack = 5120;
    cfg.thread.prio = 15;
    cfg.thread.stack_in_ext = true;
    cfg.name = "mixer_task";
    esp_gmf_task_init(&cfg, &mixer_task);

    esp_gmf_pipeline_bind_task(mixer_pipe, mixer_task);
    dump_pipeline("Mischer", mixer_pipe);
    return ESP_GMF_ERR_OK;
}

/* Zaehlt die Ports einer Port-Liste (Muster aus esp_gmf_element.c:38). */
static int port_count(esp_gmf_port_handle_t head)
{
    int k = 0;
    while (head != NULL) {
        k++;
        head = head->next;
    }
    return k;
}

/*
 * Diagnose: zeigt die Elemente einer Pipeline mit ihren Port-Zahlen.
 *
 * Wird gebraucht, weil ein connect_pipe mit
 *   "Can't register more out ports for an element that is only support single port, cnt:1"
 * scheitert, obwohl das letzte Element der Kette beim Aufbau nur seinen
 * Eingangsport bekommen haben darf. Ohne diese Ausgabe ist nicht zu sehen,
 * welches Element wirklich wie viele Ports hat.
 */
static void dump_pipeline(const char *what, esp_gmf_pipeline_handle_t pipe)
{
    if (pipe == NULL) {
        ESP_LOGE(TAG, "%s: Pipeline ist NULL", what);
        return;
    }
    const void *it = NULL;
    esp_gmf_element_handle_t el = NULL;
    int idx = 0;
    while (esp_gmf_pipeline_iterate_element(pipe, &it, &el) == ESP_GMF_ERR_OK && el != NULL) {
        ESP_LOGD(TAG, "%s[%d] %s: in=%d, out=%d", what, idx, OBJ_GET_TAG(el),
                 port_count(ESP_GMF_ELEMENT_GET(el)->in), port_count(ESP_GMF_ELEMENT_GET(el)->out));
        idx++;
    }
}

/*
 * Einen Zubringer-Zweig ueber einen Ringpuffer an den Mischer haengen.
 *
 * Muster aus pipeline_howl.c:242-259: Ausgangsport am letzten Element des
 * Zubringers, Eingangsport am aud_mixer, dazwischen ein Ringpuffer.
 */
static esp_gmf_err_t connect_branch_to_mixer(esp_gmf_pipeline_handle_t branch, const char *last_el_tag,
                                             esp_gmf_db_handle_t *db_out, int db_items)
{
    esp_gmf_element_handle_t last_el = NULL;
    esp_gmf_err_t ret = esp_gmf_pipeline_get_el_by_name(branch, last_el_tag, &last_el);
    if (ret != ESP_GMF_ERR_OK || last_el == NULL) {
        ESP_LOGE(TAG, "Element %s nicht gefunden: %d", last_el_tag, ret);
        return ret;
    }

    /*
     * Der Ausgangsport des Zubringers darf noch nicht belegt sein. Ist er es
     * doch, hilft kein zweiter Versuch weiter - dann muss der Aufbau selbst
     * geklaert werden. Deshalb hier abbrechen und den Ist-Zustand melden.
     */
    int used = port_count(ESP_GMF_ELEMENT_GET(last_el)->out);
    if (used != 0) {
        ESP_LOGE(TAG, "%s hat bereits %d Ausgangsport(s) - kein zweiter moeglich", last_el_tag, used);
        dump_pipeline("I2S-Zweig", i2s2bt_pipe);
        dump_pipeline("Datei-Zweig", local2bt_pipe);
        dump_pipeline("Mischer", mixer_pipe);
        return ESP_GMF_ERR_NOT_SUPPORT;
    }

    /*
     * Ringpuffer zwischen Zubringer und Mischer.
     *
     * Groesser als im Beispiel (pipeline_howl.c:246 nutzt 10 x 1024 = 10 KB):
     * die Beispiele setzen das Vorlaufen der Quellen voraus. Bei uns liefert der
     * I2S-Eingang in Echtzeit und der Datei-Zweig laeuft erst an, wenn der
     * Mischer schon zieht. Ein leerer Eingang liefert dem Mischer
     * ESP_GMF_IO_FAIL (nicht TIMEOUT), und darauf bricht er seinen Job ab
     * (esp_gmf_mixer.c:206-210). Mehr Puffer = mehr Vorlauf, den der Mischer
     * ueberbruecken kann.
     */
    esp_gmf_db_handle_t db = NULL;
    ret = esp_gmf_db_new_ringbuf(db_items, MIXER_DB_ITEM_SIZE, &db);
    if (ret != ESP_GMF_ERR_OK || db == NULL) {
        ESP_LOGE(TAG, "Ringpuffer liess sich nicht anlegen: %d", ret);
        return ESP_GMF_ERR_FAIL;
    }

    /*
     * Wartezeit des EINGANGS: 0, also nicht blockieren.
     *
     * Der Mischer holt jeden Eingang mit
     * esp_gmf_port_acquire_in(in_port, ..., max(in_port->wait_ticks, frame_time/index)).
     * Der Wert geht durch bis zum Ringpuffer (esp_gmf_port.c:245 ->
     * esp_gmf_io.c:495 -> esp_gmf_ringbuffer.c:169).
     *
     * Hier standen nacheinander 1 und 100:
     *   1   -> ein leerer Puffer lieferte ESP_GMF_IO_FAIL, und FAIL bricht den
     *          Mischer-Job ab (esp_gmf_mixer.c:206-210): die Datei endete nach
     *          0,2 s. In der Praxis trat das auf, weil der Puffer schneller
     *          leer war, als 1 ms erlaubt.
     *   100 -> kein Abbruch mehr, aber der Mischer wartete pro Frame bis zu
     *          100 ms je Eingang. Bei zwei Eingaengen sind das bis zu 200 ms je
     *          Frame statt der noetigen ~11 ms - die Datei lief deshalb rund
     *          15x zu langsam (8,7 s Musik waren nach 35 s noch nicht fertig).
     *          Das war das "Abhacken".
     *
     * 0 ist der Wert aus dem offiziellen Beispiel (pipeline_howl.c:252/258
     * uebergibt 0 bzw. 1): dann wird nicht gewartet, der Puffer wird sofort
     * geprueft. Ist er leer, kommt ESP_GMF_IO_TIMEOUT - und TIMEOUT ist fuer
     * den Mischer der Normalfall "Eingang liefert gerade nichts": er zaehlt
     * status_end hoch und fuellt mit Nullen auf (esp_gmf_mixer.c:211-218).
     * Damit bleibt die Vampire hoerbar und der Datei-Zweig laeuft im Takt.
     */
    esp_gmf_port_handle_t out_port = NEW_ESP_GMF_PORT_OUT_BYTE(esp_gmf_db_acquire_write, esp_gmf_db_release_write,
                                                              esp_gmf_db_deinit, db, 4096, ESP_GMF_MAX_DELAY);
    esp_gmf_port_handle_t in_port = NEW_ESP_GMF_PORT_IN_BYTE(esp_gmf_db_acquire_read, esp_gmf_db_release_read,
                                                             esp_gmf_db_deinit, db, 4096, 0);
    if (out_port == NULL || in_port == NULL) {
        ESP_LOGE(TAG, "Ports liessen sich nicht anlegen");
        return ESP_GMF_ERR_MEMORY_LACK;
    }

    esp_gmf_element_handle_t mixer_el = NULL;
    int mixer_in = -1;
    if (esp_gmf_pipeline_get_el_by_name(mixer_pipe, "aud_mixer", &mixer_el) == ESP_GMF_ERR_OK && mixer_el != NULL) {
        mixer_in = port_count(ESP_GMF_ELEMENT_GET(mixer_el)->in);
    }
    /*
     * Der Mischer hat genau MIXER_SRC_NUM Eingaenge. Ist er voll, hilft kein
     * Versuch mehr - der dritte Zubringer wuerde nur den Speicher des
     * Ringpuffers verlieren.
     */
    if (mixer_in >= MIXER_SRC_NUM) {
        ESP_LOGE(TAG, "Mischer ist voll (%d von %d Eingaengen belegt) - %s passt nicht mehr",
                 mixer_in, MIXER_SRC_NUM, last_el_tag);
        esp_gmf_db_deinit(db);
        return ESP_GMF_ERR_OUT_OF_RANGE;
    }
    ESP_LOGI(TAG, "Verbinde %s (out=%d) mit aud_mixer (in=%d von %d)", last_el_tag, used, mixer_in, MIXER_SRC_NUM);
    ret = esp_gmf_pipeline_connect_pipe(branch, last_el_tag, out_port, mixer_pipe, "aud_mixer", in_port);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Zubringer liess sich nicht mit dem Mischer verbinden: %d", ret);
        dump_pipeline("I2S-Zweig", i2s2bt_pipe);
        dump_pipeline("Datei-Zweig", local2bt_pipe);
        dump_pipeline("Mischer", mixer_pipe);
        return ret;
    }
    if (db_out != NULL) {
        *db_out = db;
    }
    return ESP_GMF_ERR_OK;
}

/*
 * Wird aus dem A2DP-Stream-Callback aufgerufen, sobald ein Stream in
 * Senderichtung bereitsteht - genau wie bei local2bt.
 *
 * Wichtig: hier wird nur der Stream gesetzt und der Port verkabelt. Gestartet
 * wird ueber den stream_proc_task, weil der Callback im Kontext des
 * Bluetooth-/GMF-Ablaufs laeuft und ein direktes run() dort stoeren kann. Bei
 * local2bt laeuft die Pipeline ebenfalls erst im Zustand STARTED.
 */
void i2s2bt_set_stream(esp_bt_audio_stream_handle_t stream)
{
    if (mixer_pipe == NULL) {
        ESP_LOGE(TAG, "Mischer-Pipeline nicht vorhanden");
        return;
    }
    i2s2bt_stream = stream;
    esp_gmf_io_bt_set_stream(ESP_GMF_PIPELINE_GET_OUT_INSTANCE(mixer_pipe), stream);


    esp_gmf_err_t ret = ESP_GMF_ERR_OK;

    if (i2s2bt_pipe == NULL) {
        ESP_LOGE(TAG, "I2S-Pipeline nicht vorhanden");
        return;
    }

    {
        /* Erst verkabeln, dann das Format melden: die Meldung laeuft ueber den
         * Event-Weiterleiter, der beim Verbinden entsteht; der aud_mixer kommt
         * nur so nach INITIALIZED. */
        static bool branches_connected = false;
        if (!branches_connected) {
            /*
             * Das letzte Element des I2S-Zweigs haengt am Messfall:
             *   Modus 2 -> aud_lin_resample (einziges Element)
             *   Modus 1/3/4 -> aud_ch_cvt_i2s
             * Siehe setup_pipeline_i2s2bt().
             */
            ret = connect_branch_to_mixer(i2s2bt_pipe, "aud_lin_resample", &i2s_branch_db, MIXER_DB_ITEMS);
            if (ret != ESP_GMF_ERR_OK) {
                ESP_LOGE(TAG, "I2S-Zweig liess sich nicht an den Mischer haengen: %d", ret);
                return;
            }
            ret = connect_branch_to_mixer(local2bt_pipe, "aud_lin_resample_file", &file_branch_db, FILE_DB_ITEMS);
            if (ret == ESP_GMF_ERR_OK) {
                /* 0.9.76: jetzt existiert der Ausgang - Puffer sofort reservieren. */
                reserve_output_payload(local2bt_pipe, "aud_lin_resample_file",
                                       LIN_RESAMPLE_OUT_PAYLOAD_MAX, 16u);
            }
            if (ret != ESP_GMF_ERR_OK) {
                ESP_LOGE(TAG, "Datei-Zweig liess sich nicht an den Mischer haengen: %d", ret);
                return;
            }
            branches_connected = true;
            ESP_LOGI(TAG, "Beide Zubringer haengen am Mischer");
        }

        /*
         * Toninformationen fuer die informationsabhaengigen Elemente melden - das
         * ist PFLICHT, nicht Kosmetik.
         *
         * Elemente wie aud_rate_cvt werden mit dependency = true angelegt
         * (esp_gmf_rate_cvt.c:297) und starten deshalb im Zustand STATE_NONE. Sie
         * kommen erst in INITIALIZED, wenn sie diese Meldung erhalten
         * (esp_gmf_rate_cvt.c:186). Ohne sie scheitert das Registrieren der Jobs:
         *
         *   Element[aud_rate_cvt_i2s-...] not ready to register job, ret:0xffffdff8
         *   Run timeout, [tsk:i2s2bt_task]
         *
         * Gemeldet wird das Format der QUELLE des I2S-Zweigs (die Vampire).
         * Die Datei meldet ihr Format selbst in local2bt_play().
         */
        esp_gmf_info_sound_t i2s_info = {
            .sample_rates = I2S2BT_SRC_RATE_HZ,
            .channels = I2S2BT_SRC_CHANNELS,
            .bits = I2S2BT_SRC_BITS,
        };
        dump_pipeline_state("vor-report", i2s2bt_pipe);
        ret = esp_gmf_pipeline_report_info(i2s2bt_pipe, ESP_GMF_INFO_SOUND, &i2s_info, sizeof(i2s_info));
        dump_pipeline_state("nach-report", i2s2bt_pipe);
        if (ret == ESP_GMF_ERR_OK) {
            ESP_LOGI(TAG, "I2S-Quellformat gemeldet: %d Hz, %d Bit, %d ch",
                     I2S2BT_SRC_RATE_HZ, I2S2BT_SRC_BITS, I2S2BT_SRC_CHANNELS);
        } else {
            ESP_LOGE(TAG, "I2S-Toninformationen konnten nicht gemeldet werden: %d", ret);
        }
    }

    /*
     * Und das Mischer-FORMAT direkt an die Mischer-Pipeline melden.
     *
     * Warum zusaetzlich: die Meldung an den I2S-Zweig laeuft nur die EIGENE
     * Kette entlang und bleibt am ersten dependency-Element haengen
     * (esp_gmf_pipeline.c:210-216) - das ist hier aud_ch_cvt_i2s. Der Mischer
     * sitzt in einer ANDEREN Pipeline und bekaeme sie deshalb nie. Er ist
     * selbst ein dependency-Element (esp_gmf_mixer.c:457) und kommt nur ueber
     * eine ESP_GMF_INFO_SOUND-Meldung nach INITIALIZED
     * (esp_gmf_mixer.c:299). Ohne sie:
     *
     *   Element[aud_mixer-...] not ready to register job, ret:0xffffdff8
     *   Run timeout,[mixer_task,...]
     *
     * Genauso macht es das offizielle Beispiel: pipeline_howl.c:302-303 meldet
     * das Format an pipe_mic UND an pipe_mix.
     */
    esp_gmf_info_sound_t mix_info = {
        .sample_rates = I2S2BT_RATE_HZ,
        .channels = I2S2BT_CHANNELS,
        .bits = I2S2BT_BITS,
    };
    ret = esp_gmf_pipeline_report_info(mixer_pipe, ESP_GMF_INFO_SOUND, &mix_info, sizeof(mix_info));
    if (ret == ESP_GMF_ERR_OK) {
        ESP_LOGI(TAG, "Mischer-Format gemeldet: %d Hz, %d Bit, %d ch",
                 I2S2BT_RATE_HZ, I2S2BT_BITS, I2S2BT_CHANNELS);
    } else {
        ESP_LOGE(TAG, "Mischer-Format konnte nicht gemeldet werden: %d", ret);
    }

    /*
     * Die Wartezeiten noch einmal nachziehen.
     *
     * setup_pipeline_mixer() traegt sie beim Aufbau ein - also VOR jeder
     * CLI-Eingabe. Wer mit "mixer 0 0" kuerzere Zeiten setzt, muss sie hier
     * wirksam bekommen, sonst bliebe die Aenderung bis zum Neustart liegen.
     * Die Pipeline laeuft zu diesem Zeitpunkt noch nicht (sie wird erst unten
     * gestartet), das Setzen ist deshalb unkritisch.
     */
    esp_gmf_element_handle_t mix_el = NULL;
    if (esp_gmf_pipeline_get_el_by_name(mixer_pipe, "aud_mixer", &mix_el) == ESP_GMF_ERR_OK && mix_el != NULL) {
        esp_ae_mixer_cfg_t *mix_cfg = (esp_ae_mixer_cfg_t *)OBJ_GET_CFG(mix_el);
        if (mix_cfg != NULL && mix_cfg->src_info != NULL) {
            mix_cfg->src_info[MIXER_SRC_I2S].transit_time = s_mixer_transit_ms;
            mix_cfg->src_info[MIXER_SRC_FILE].transit_time = s_mixer_transit_ms;
            ESP_LOGI(TAG, "Mischer-Wartezeiten: prefill %d ms, transit %d ms je Quelle",
                     s_mixer_prefill_ms, s_mixer_transit_ms);
        }
    }

    /*
     * Encoder mit den AUSGEHANDELTEN SBC-Parametern einstellen - nicht mit
     * Standardwerten.
     *
     * Der Weg ueber reconfig_by_sound_info() setzt bei stereo
     * ESP_SBC_CH_MODE_DUAL und laesst bitpool auf dem Standardwert
     * (esp_gmf_audio_enc.c:448 ff.). Damit wurden die SBC-Rahmen 268 Byte gross,
     * und der A2DP-Sender verwarf jeden Block:
     *
     *   W BT_AUD_A2D_SRC: Drop oversized frame batch: 1340 bytes exceeds MTU 666
     *
     * 5 Rahmen * 268 Byte = 1340 Byte bei MTU 666 - hoerbar als abgehackter Ton.
     * Beim Datei-Zweig sind die Rahmen 118 Byte, weil dort die ausgehandelte
     * Konfiguration uebergeben wird (siehe stream_proc_prepare).
     *
     * Die Parameter liefert der Stream selbst. Reihenfolge: erst der Typ, dann
     * die Parameter - reconfig() prueft, ob der Typ bereits passt.
     */
    esp_bt_audio_stream_codec_info_t codec_info = {0};
    if (esp_bt_audio_stream_get_codec_info(stream, &codec_info) == ESP_OK &&
        codec_info.codec_cfg != NULL && codec_info.cfg_size > 0) {
        /*
         * ZUERST die ausgehandelte Abtastrate auf die ganze Kette anwenden.
         *
         * Der SBC-Encoder resampelt nicht - er bekommt die Rate nur gesagt.
         * Die Senke handelt sie aus, und DIESELBE Soundbar lieferte dabei am
         * 02.10. einmal 48000 Hz und einmal 44100 Hz. Liefert die Kette eine
         * andere Rate als ausgehandelt, zieht die Senke je 20 ms mehr Rahmen,
         * als nachkommen, und ihr Puffer laeuft leer: hoerbar als Ruckler.
         *
         * Deshalb wird hier alles umgestellt, BEVOR die Pipeline laeuft:
         * Mischer, Ratenwandler des Mischers, Ratenwandler des Datei-Zweigs
         * und der lineare Umsetzer im I2S-Zweig.
         */
        uint32_t nego_rate = 0;
        if (codec_info.codec_cfg != NULL && codec_info.cfg_size >= sizeof(esp_sbc_enc_config_t)) {
            const esp_sbc_enc_config_t *sbc = (const esp_sbc_enc_config_t *)codec_info.codec_cfg;
            nego_rate = sbc->sample_rate;
        }
        if (nego_rate >= 8000 && nego_rate <= 48000) {
            esp_gmf_element_handle_t el = NULL;
            if (esp_gmf_pipeline_get_el_by_name(mixer_pipe, "aud_mixer", &el) == ESP_GMF_ERR_OK && el != NULL) {
                esp_ae_mixer_cfg_t *mcfg = (esp_ae_mixer_cfg_t *)OBJ_GET_CFG(el);
                if (mcfg != NULL) {
                    mcfg->sample_rate = nego_rate;
                }
            }
            if (esp_gmf_pipeline_get_el_by_name(mixer_pipe, "aud_rate_cvt", &el) == ESP_GMF_ERR_OK && el != NULL) {
                esp_gmf_rate_cvt_set_dest_rate(el, nego_rate);
            }
            if (esp_gmf_pipeline_get_el_by_name(local2bt_pipe, "aud_lin_resample_file", &el) == ESP_GMF_ERR_OK && el != NULL) {
                aud_lin_resample_set_out_rate(el, nego_rate);
            }
            if (esp_gmf_pipeline_get_el_by_name(i2s2bt_pipe, "aud_lin_resample", &el) == ESP_GMF_ERR_OK && el != NULL) {
                aud_lin_resample_set_out_rate(el, nego_rate);
            }
            ESP_LOGI(TAG, "Kette auf die ausgehandelte Rate umgestellt: %u Hz",
                     (unsigned)nego_rate);
        } else {
            ESP_LOGW(TAG, "Keine brauchbare Abtastrate ausgehandelt (%u Hz) - es bleibt bei %d Hz",
                     (unsigned)nego_rate, I2S2BT_RATE_HZ);
        }

        esp_gmf_element_handle_t enc = NULL;
        if (esp_gmf_pipeline_get_el_by_name(mixer_pipe, "aud_enc_mix", &enc) == ESP_GMF_ERR_OK && enc != NULL) {
            /* 1. Typ setzen, damit der Encoder-Unterbau passt. */
            esp_gmf_info_sound_t enc_info = {
                .format_id = ESP_AUDIO_TYPE_SBC,
                .sample_rates = (nego_rate >= 8000 && nego_rate <= 48000) ? nego_rate : I2S2BT_RATE_HZ,
                .channels = I2S2BT_CHANNELS,
                .bits = I2S2BT_BITS,
            };
            ret = esp_gmf_audio_enc_reconfig_by_sound_info(enc, &enc_info);
            if (ret != ESP_GMF_ERR_OK) {
                ESP_LOGE(TAG, "Encoder-Typ konnte nicht gesetzt werden: %d", ret);
            }
            /* 2. Ausgehandelte Parameter anwenden: bitpool, block_length,
             *    sub_bands, ch_mode (JOINT_STEREO statt DUAL). */
            esp_audio_enc_config_t enc_cfg = {
                .type   = ESP_AUDIO_TYPE_SBC,
                .cfg    = codec_info.codec_cfg,
                .cfg_sz = codec_info.cfg_size,
            };
            /*
             * 0.9.71: Bitpool deckeln, falls gewuenscht. Der ausgehandelte
             * Block ist ein esp_sbc_enc_config_t (die Komponente druckt genau
             * dessen Felder); gearbeitet wird auf einer lokalen Kopie, damit
             * die Aushandlung selbst unangetastet bleibt.
             */
            esp_sbc_enc_config_t sbc_local;
            if (s_sbc_bitpool_cap > 0 && enc_cfg.cfg_sz >= sizeof(sbc_local)) {
                /* ERST kopieren, dann pruefen - sbc_local ist vorher uninitialisiert
                 * (dieser Fehler steckte im ersten Wurf von 0.9.71). */
                memcpy(&sbc_local, enc_cfg.cfg, sizeof(sbc_local));
                if ((int)sbc_local.bitpool > s_sbc_bitpool_cap) {
                    ESP_LOGW(TAG, "SBC-Bitpool %u -> %d gedeckelt (CPU/Strecke)",
                             (unsigned)sbc_local.bitpool, s_sbc_bitpool_cap);
                    sbc_local.bitpool = (uint16_t)s_sbc_bitpool_cap;
                    enc_cfg.cfg = &sbc_local;
                    enc_cfg.cfg_sz = (uint32_t)sizeof(sbc_local);
                }
            }
            ret = esp_gmf_audio_enc_reconfig(enc, &enc_cfg);
            if (ret != ESP_GMF_ERR_OK) {
                ESP_LOGE(TAG, "Ausgehandelte SBC-Parameter nicht uebernommen: %d", ret);
            } else {
                ESP_LOGI(TAG, "SBC-Encoder mit ausgehandelten Parametern gesetzt (%d Byte cfg)",
                         codec_info.cfg_size);
            }
        } else {
            ESP_LOGE(TAG, "aud_enc im mixer_pipe nicht gefunden");
        }
    } else {
        ESP_LOGW(TAG, "Keine Codec-Information vom Stream - der Encoder bleibt auf "
                      "Standardwerten und die Rahmen werden zu gross fuer die MTU");
    }

    /*
     * Startreihenfolge mit Kopfstart.
     *
     * Erst die ZUBRINGER starten, dann kurz warten, dann den Mischer. Grund:
     * der Mischer bricht seinen Job ab, wenn ein Eingang beim ersten Zugriff
     * noch leer ist (esp_gmf_mixer.c:206-210 liefert dann ESP_GMF_IO_FAIL,
     * nicht TIMEOUT). Genau das liess den Datei-Zweig nach 0,4 s enden:
     *
     *   Datei -> Mischer: file://sdcard/test2.mp3
     *   Wiedergabe beendet            (statt der vollen 8,7 s Spieldauer)
     *
     * Mit dem Kopfstart hat jeder Zubringer gefuellt, bevor der Mischer zieht.
     * Fehlt ein Zweig ganz (z.B. noch keine Datei gewaehlt), fuellt der Mischer
     * ihn mit Nullen auf - die Vampire bleibt also zu hoeren.
     */
    ESP_LOGI(TAG, "Starte I2S-Zubringer, dann nach %d ms den Mischer", s_mixer_prefill_ms);
    stream_proc_post_pipeline_action(i2s2bt_pipe, STREAM_PROC_PIPELINE_PREPARE);
    stream_proc_post_pipeline_action(i2s2bt_pipe, STREAM_PROC_PIPELINE_RUN);
    vTaskDelay(pdMS_TO_TICKS(s_mixer_prefill_ms));

    ESP_LOGI(TAG, "Starte Mischer");
    stream_proc_post_pipeline_action(mixer_pipe, STREAM_PROC_PIPELINE_PREPARE);
    stream_proc_post_pipeline_action(mixer_pipe, STREAM_PROC_PIPELINE_RUN);
}

void i2s2bt_request(void)
{
    if (i2s2bt_pipe == NULL) {
        ESP_LOGE(TAG, "I2S-Pipeline nicht vorhanden (io_i2s fehlt)");
        return;
    }
    i2s2bt_requested = true;
    ESP_LOGI(TAG, "I2S-Eingang angefordert - startet mit dem naechsten A2DP-Stream");
}

void i2s2bt_stop(void)
{
    if (i2s2bt_pipe == NULL) {
        return;
    }
    ESP_LOGI(TAG, "I2S-Pipeline wird gestoppt");
    esp_gmf_pipeline_stop(i2s2bt_pipe);
    esp_gmf_pipeline_reset(i2s2bt_pipe);
}

bool i2s2bt_is_ready(void)
{
    return (i2s2bt_pipe != NULL);
}

/*
 * Hier stand bis 0.9.58 i2s2bt_log_io_speed(): es las auf Anfrage die
 * Durchsatz-Statistik des I2S-Eingangs. Die Funktion hatte keinen Aufrufer mehr
 * (das zugehoerige Konsolenkommando war schon in 0.9.40/0.9.41 entfernt worden),
 * war also toter Code. Der Ton selbst ist der Messwert.
 */

void i2s2bt_set_mixer_wait(int prefill_ms, int transit_ms)
{
    if (prefill_ms >= 0) {
        s_mixer_prefill_ms = prefill_ms;
    }
    if (transit_ms >= 0) {
        s_mixer_transit_ms = transit_ms;
    }
}

void i2s2bt_get_mixer_wait(int *prefill_ms, int *transit_ms)
{
    if (prefill_ms != NULL) {
        *prefill_ms = s_mixer_prefill_ms;
    }
    if (transit_ms != NULL) {
        *transit_ms = s_mixer_transit_ms;
    }
}

/*--------------------------------------------------------------------
 * Equalizer (0.9.56)
 *
 * Der EQ haengt in der Mischer-Pipeline zwischen aud_mixer und aud_enc_mix,
 * formt also beide Quellen mit einer Instanz. Die Bandzahl ist fest
 * (MIXER_EQ_BANDS), aktiviert werden die Baender 0..N-1 - so laesst sich der
 * Einfluss einzelner Baender auf die CPU-Last messen, ohne die Pipeline neu
 * aufzubauen.
 *------------------------------------------------------------------*/

/** EQ-Element aus der Mischer-Pipeline holen (NULL, wenn nicht vorhanden) */
static esp_gmf_element_handle_t eq_element(void)
{
    if (mixer_pipe == NULL) {
        ESP_LOGW(TAG, "EQ: Mischer-Pipeline gibt es noch nicht");
        return NULL;
    }
    esp_gmf_element_handle_t eq = NULL;
    if (esp_gmf_pipeline_get_el_by_name(mixer_pipe, "aud_eq", &eq) != ESP_GMF_ERR_OK || eq == NULL) {
        ESP_LOGW(TAG, "EQ: Element aud_eq nicht in der Pipeline");
        return NULL;
    }
    return eq;
}

/*
 * Wie viele Baender gerade filtern. Wird hier mitgefuehrt, weil die
 * esp_ae_eq-Schnittstelle zwar setzen, aber den Schaltzustand nicht abfragen
 * laesst; Vorgabe ist "alle Baender an" (so legt pool_reg.c sie an).
 */
static int s_eq_active = MIXER_EQ_BANDS;

int stream_proc_eq_info(int *bands, int *active)
{
    if (eq_element() == NULL) {
        return -1;
    }
    if (bands != NULL) {
        *bands = MIXER_EQ_BANDS;
    }
    if (active != NULL) {
        *active = s_eq_active;
    }
    return 0;
}

int stream_proc_eq_get(int idx, int *typ, unsigned *fc, float *q, float *gain)
{
    esp_gmf_element_handle_t eq = eq_element();
    esp_ae_eq_filter_para_t  para = {0};

    if (eq == NULL || idx < 0 || idx >= MIXER_EQ_BANDS) {
        return -1;
    }
    if (esp_gmf_eq_get_para(eq, (uint8_t)idx, &para) != ESP_GMF_ERR_OK) {
        return -1;
    }
    if (typ != NULL) {
        *typ = (int)para.filter_type;
    }
    if (fc != NULL) {
        *fc = (unsigned)para.fc;
    }
    if (q != NULL) {
        *q = para.q;
    }
    if (gain != NULL) {
        *gain = para.gain;
    }
    return 0;
}

int stream_proc_eq_set_bands(int n)
{
    if (n < 0 || n > MIXER_EQ_BANDS) {
        ESP_LOGW(TAG, "EQ: Bandzahl %d ungueltig (0..%d)", n, MIXER_EQ_BANDS);
        return -1;
    }
    esp_gmf_element_handle_t eq = eq_element();
    if (eq == NULL) {
        return -1;
    }
    for (int i = 0; i < MIXER_EQ_BANDS; i++) {
        esp_gmf_err_t ret = esp_gmf_eq_enable_filter(eq, (uint8_t)i, i < n);
        if (ret != ESP_GMF_ERR_OK) {
            ESP_LOGW(TAG, "EQ: Band %d konnte nicht geschaltet werden: %d", i, ret);
            return -1;
        }
    }
    s_eq_active = n;
    ESP_LOGI(TAG, "EQ: %d von %d Baendern aktiv", n, MIXER_EQ_BANDS);
    return n;
}

int stream_proc_eq_set(int idx, int typ, unsigned fc, float q, float gain)
{
    if (idx < 0 || idx >= MIXER_EQ_BANDS) {
        ESP_LOGW(TAG, "EQ: Bandindex %d ungueltig (0..%d)", idx, MIXER_EQ_BANDS - 1);
        return -1;
    }
    if (typ < ESP_AE_EQ_FILTER_HIGH_PASS || typ >= ESP_AE_EQ_FILTER_MAX) {
        ESP_LOGW(TAG, "EQ: Filtertyp %d ungueltig (1..5)", typ);
        return -1;
    }
    esp_gmf_element_handle_t eq = eq_element();
    if (eq == NULL) {
        return -1;
    }
    esp_ae_eq_filter_para_t para = {
        .filter_type = (esp_ae_eq_filter_type_t)typ,
        .fc          = fc,
        .q           = q,
        .gain        = gain,
    };
    esp_gmf_err_t ret = esp_gmf_eq_set_para(eq, (uint8_t)idx, &para);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGW(TAG, "EQ: Band %d nicht einstellbar: %d", idx, ret);
        return -1;
    }
    ESP_LOGI(TAG, "EQ: Band %d = Typ %d, fc %u Hz, Q %.2f, Gain %.1f dB", idx, typ, fc, q, gain);
    return 0;
}

void stream_proc_eq_list(void)
{
    esp_gmf_element_handle_t eq = eq_element();
    if (eq == NULL) {
        return;
    }
    static const char *typen[] = { "ungueltig", "HighPass", "LowPass", "Peak", "HighShelf", "LowShelf" };
    for (int i = 0; i < MIXER_EQ_BANDS; i++) {
        esp_ae_eq_filter_para_t para = {0};
        if (esp_gmf_eq_get_para(eq, (uint8_t)i, &para) != ESP_GMF_ERR_OK) {
            printf("  Band %2d: nicht lesbar\n", i);
            continue;
        }
        const char *tn = (para.filter_type > 0 && para.filter_type < ESP_AE_EQ_FILTER_MAX)
                         ? typen[para.filter_type] : "?";
        printf("  Band %2d: %-9s fc %5u Hz  Q %4.2f  Gain %+5.1f dB\n",
               i, tn, (unsigned)para.fc, para.q, para.gain);
    }
}

/*
 * Autostart: Uebertragung starten, sobald eine Gegenstelle verbunden ist
 * (0.9.68).
 *
 * Feldmeldung vom 08.10.2026: "habe v4 am laufen, esp32 resetet, hoere aber
 * keinen sound der v4. nach abspielen des mp3 laeuft der v4 sound." Genau das
 * war die Luecke: der ESP32 verband sich mit der Senke, startete die
 * Uebertragung aber nicht. Angestossen wurde sie erst durch MEDIA_START,
 * PLAY_FILE oder 'start_media' - bis dahin war die Vampire stumm.
 *
 * Laeuft im Takt von stream_proc_task (kein eigener Task, kein Stack) und
 * wiederholt den Versuch alle 5 s, bis die Uebertragung steht. 'stop_media'
 * schaltet den Autostart ab, eine neue Verbindung schaltet ihn wieder ein.
 */
#define MEDIA_AUTOSTART_RETRY_MS  5000

static void stream_proc_autostart_tick(void)
{
    static bool    was_connected;
    static int64_t last_try_us;
    int64_t        now       = esp_timer_get_time();
    bool           connected = bt_mgr_is_connected();

    if (connected && !was_connected) {
        s_media_autostart = true;          /* neue Sitzung, neuer Versuch */
        last_try_us       = 0;
    }
    was_connected = connected;

    if (!connected || !s_media_autostart || bt_mgr_audio_streaming()) {
        last_try_us = 0;
        return;
    }
    if (last_try_us != 0
        && (now - last_try_us) < (int64_t)MEDIA_AUTOSTART_RETRY_MS * 1000) {
        return;
    }
    last_try_us = now;

    /* Die Vampire ist der Zweck der Bruecke: ihren I2S-Eingang mitschicken. */
    i2s2bt_request();

    esp_err_t err = esp_bt_audio_media_start(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, NULL);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Autostart: Uebertragung angefordert (I2S-Eingang der Vampire)");
    } else {
        ESP_LOGW(TAG, "Autostart: Uebertragung abgelehnt: %s", esp_err_to_name(err));
    }
}

/*
 * Zustandsbericht fuer die Fehlersuche am Ton (0.9.70).
 *
 * NUR auf Abruf, nie periodisch: die alte Puffer-Diagnose lief im Betrieb mit
 * und verursachte selbst ein "leises Knacken im Sekundentakt" (Commit 9408af7) -
 * ein Ringpegel-Lesen stoert den Tonpfad. Ein einzelner Aufruf von Hand ist
 * unkritisch und sagt im Moment des Stotterns mehr als jede Statistik.
 */
void stream_proc_buffer_report(void)
{
    uint32_t filled = 0, total = 0;

    if (i2s_branch_db != NULL && esp_gmf_db_get_total_size(i2s_branch_db, &total) == ESP_GMF_ERR_OK) {
        (void)esp_gmf_db_get_filled_size(i2s_branch_db, &filled);
        printf("I2S-Ring : %5u von %5u Byte (%u %%)\n",
               (unsigned)filled, (unsigned)total,
               (unsigned)(total ? (filled * 100u) / total : 0u));
    } else {
        printf("I2S-Ring : nicht vorhanden\n");
    }
    filled = 0; total = 0;
    if (file_branch_db != NULL && esp_gmf_db_get_total_size(file_branch_db, &total) == ESP_GMF_ERR_OK) {
        (void)esp_gmf_db_get_filled_size(file_branch_db, &filled);
        printf("Datei-Ring: %5u von %5u Byte (%u %%)\n",
               (unsigned)filled, (unsigned)total,
               (unsigned)(total ? (filled * 100u) / total : 0u));
    } else {
        printf("Datei-Ring: nicht vorhanden\n");
    }
    printf("Mischer  : prefill %d ms, transit %d ms\n", s_mixer_prefill_ms, s_mixer_transit_ms);
    printf("Zweige   : I2S %s, Datei %s, Uebertragung %s, Autostart %s\n",
           i2s2bt_requested ? "angefordert" : "aus",
           local2bt_is_playing() ? "spielt" : "aus",
           bt_mgr_audio_streaming() ? "laeuft" : "haelt",
           s_media_autostart ? "an" : "aus");
}

void stream_proc_set_sbc_bitpool_cap(int bitpool)
{
    s_sbc_bitpool_cap = (bitpool > 0 && bitpool <= 250) ? bitpool : 0;
    ESP_LOGI(TAG, "SBC-Bitpool-Obergrenze: %s",
             s_sbc_bitpool_cap > 0 ? "gesetzt (naechster Stream)" : "aus (Komponenten-Vorgabe)");
}

void stream_proc_set_media_autostart(bool on)
{
    if (s_media_autostart != on) {
        ESP_LOGI(TAG, "Autostart der Uebertragung %s", on ? "an" : "aus");
    }
    s_media_autostart = on;
}

static void stream_proc_task(void *arg)
{
    (void)arg;
    stream_proc_cmd_t cmd = {0};

    while (true) {
        if (xQueueReceive(stream_proc_cmd_queue, &cmd, pdMS_TO_TICKS(200)) != pdTRUE) {
            /* Kein Kommando - Gelegenheit, eine anstehende Stopp-Anforderung
             * aus dem Pipeline-Event abzuarbeiten. */
            local2bt_process_stop_request();
            /*
             * 0.9.66: Takt fuer die Autoverbindung. Sie laeuft hier und nicht in
             * der I2C-Bruecke - deren Verkehr ist davon unabhaengig, und dieser
             * Aufruf kehrt sofort zurueck (der Verbindungsaufbau selbst laeuft
             * asynchron im BT-Stack).
             */
            bt_mgr_autoconnect_tick();
            /* 0.9.68: und die Uebertragung starten, sobald eine Senke steht. */
            stream_proc_autostart_tick();
            continue;
        }

        switch (cmd.action) {
            case STREAM_PROC_PIPELINE_PREPARE: {
                if (cmd.report_codec_input_info) {
                    esp_gmf_info_sound_t info = {
                        .sample_rates = CODEC_ADC_SAMPLE_RATE,
                        .channels = CODEC_ADC_CHANNELS,
                        .bits = CODEC_ADC_BITS_PER_SAMPLE,
                    };
                    esp_gmf_pipeline_report_info(cmd.pipe, ESP_GMF_INFO_SOUND, &info, sizeof(info));
                }
                if (cmd.uri) {
                    esp_gmf_pipeline_set_in_uri(cmd.pipe, cmd.uri);
                    /*
                     * Die Datei-IO fuehrt zwar eine Position (esp_gmf_io.c zaehlt
                     * valid_size auf attr.pos), setzt aber nie die Gesamtgroesse -
                     * attr.size bleibt 0. Fuer die Erkennung des Dateiendes
                     * brauchen wir sie nicht, fuer ein lesbares Log schon.
                     * Deshalb hier aus der Datei selbst holen.
                     *
                     * Nur fuer Datei-URIs: andere Quellen (io_bt, http) haben
                     * keinen Pfad, und eine Warnung waere dort nur Rauschen.
                     */
                    esp_gmf_io_handle_t in = NULL;
                    if (strncmp(cmd.uri, "file://", 7) == 0 &&
                        esp_gmf_pipeline_get_in(cmd.pipe, &in) == ESP_GMF_ERR_OK && in != NULL) {
                        const char *rel = cmd.uri + 7;
                        char full[272];
                        int n = snprintf(full, sizeof(full), "/%s", rel);
                        struct stat st;
                        if (n > 0 && n < (int)sizeof(full) && stat(full, &st) == 0) {
                            esp_gmf_io_set_size(in, (uint64_t)st.st_size);
                            ESP_LOGI(TAG, "Datei %s, %ld Byte", full, (long)st.st_size);
                        } else {
                            ESP_LOGW(TAG, "Groesse von %s nicht lesbar: %s", full, strerror(errno));
                        }
                    }
                }
                esp_gmf_pipeline_loading_jobs(cmd.pipe);
                break;
            }
            case STREAM_PROC_PIPELINE_RUN:
                esp_gmf_pipeline_run(cmd.pipe);
                break;
            case STREAM_PROC_PIPELINE_STOP_RESET:
                ESP_LOGI(TAG, "Reset pipeline %p", cmd.pipe);
                esp_gmf_pipeline_stop(cmd.pipe);
                esp_gmf_pipeline_reset(cmd.pipe);
                break;
            default:
                ESP_LOGW(TAG, "Unknown stream processor action %d", cmd.action);
                break;
        }

        /* Nach jedem Kommando ebenfalls nachsehen: das FINISHED-Event kann
         * waehrend der Abarbeitung eingetroffen sein. */
        local2bt_process_stop_request();
    }
}

/*
 * Der Task arbeitet die Kommandos ab und ist zugleich der Ort, an dem eine
 * anstehende Stopp-Anforderung aus dem Pipeline-Event ausgefuehrt wird -
 * siehe local2bt_request_stop().
 */
static void setup_stream_proc_task(void)
{
    if (stream_proc_cmd_queue) {
        return;
    }

    stream_proc_cmd_queue = xQueueCreate(STREAM_PROC_CMD_QUEUE_SIZE, sizeof(stream_proc_cmd_t));
    if (stream_proc_cmd_queue == NULL) {
        ESP_LOGE(TAG, "Create stream processor command queue failed");
        return;
    }

    BaseType_t ret = xTaskCreate(stream_proc_task, "stream_proc_task", STREAM_PROC_TASK_STACK_SIZE, NULL,
                                 STREAM_PROC_TASK_PRIO, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Create stream processor task failed");
        vQueueDelete(stream_proc_cmd_queue);
        stream_proc_cmd_queue = NULL;
    }
}

/*
 * Wiedergabe am Dateiende beenden.
 *
 * Muster aus den offiziellen GMF-Beispielen (esp-gmf-v1.0, gmf_examples):
 * pipeline_play_sdcard_music und pipeline_loop_play_no_gap warten beide auf das
 * Pipeline-Event und rufen DANACH esp_gmf_pipeline_stop() aus ihrem eigenen
 * Ablauf auf - nicht aus dem Event-Callback heraus:
 *
 *     if ((event->sub == ESP_GMF_EVENT_STATE_STOPPED) ||
 *         (event->sub == ESP_GMF_EVENT_STATE_FINISHED) ||
 *         (event->sub == ESP_GMF_EVENT_STATE_ERROR)) {
 *         xEventGroupSetBits(ctx, PIPELINE_BLOCK_BIT);
 *     }
 *     ...
 *     xEventGroupWaitBits(...);
 *     esp_gmf_pipeline_stop(pipe);
 *
 * Genau diesen Weg gehen wir hier auch. Der Callback laeuft im Kontext des
 * GMF-Tasks; ein Stop von dort heraus wuerde auf sich selbst warten.
 *
 * Warum das noetig ist: der Encoder beendet seinen Job am Dateiende korrekt
 * (esp_gmf_audio_enc.c:572 und :598 liefern bei in_load->is_done
 * ESP_GMF_JOB_ERR_DONE), die Pipeline geht also in FINISHED ueber. Ohne Stop
 * bleibt danach aber die A2DP-Uebertragung offen, und man hoert ueber
 * Bluetooth ein leises Brummen, das nicht mehr aufhoert.
 *
 * Der Datei-fortschritt dient nur noch als Rueckfall: falls FINISHED
 * ausbleibt, wird nach einer Sekunde ohne Positionsaenderung ebenfalls
 * gestoppt.
 */
#define LOCAL2BT_IDLE_POLL_MS   200
#define LOCAL2BT_IDLE_TICKS     5      /* 5 * 200 ms = 1 s ohne Fortschritt */

static void local2bt_eof_task(void *arg)
{
    (void)arg;
    uint64_t last_pos = 0;
    int idle = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(LOCAL2BT_IDLE_POLL_MS));

        if (local2bt_pipe == NULL || local2bt_stream == NULL) {
            idle = 0;
            continue;
        }

        esp_gmf_io_handle_t in = NULL;
        if (esp_gmf_pipeline_get_in(local2bt_pipe, &in) != ESP_GMF_ERR_OK || in == NULL) {
            idle = 0;
            continue;
        }

        uint64_t pos = 0, size = 0;
        if (esp_gmf_io_get_pos(in, &pos) != ESP_GMF_ERR_OK) {
            idle = 0;
            continue;
        }
        (void)esp_gmf_io_get_size(in, &size);

        if (pos == 0) {
            /* Noch nichts gelesen (frische Datei) - nicht als Stillstand zaehlen. */
            idle = 0;
            last_pos = 0;
            continue;
        }

        if (pos == last_pos) {
            idle++;
            /*
             * Nur eingreifen, wenn die Datei wirklich zu Ende gelesen ist
             * (Position == Groesse). Ohne diese Bedingung wuerde ein kurzer
             * Stillstand mitten in der Datei - etwa weil die A2DP-Uebertragung
             * stockt - die Wiedergabe faelschlich abbrechen.
             */
            if (idle >= LOCAL2BT_IDLE_TICKS && size > 0 && pos >= size) {
                ESP_LOGW(TAG, "Datei zu Ende gelesen (%llu von %llu Byte), aber kein FINISHED - "
                              "stoppe die Wiedergabe",
                         (unsigned long long)pos, (unsigned long long)size);
                idle = 0;
                local2bt_request_stop();
            }
        } else {
            idle = 0;
            last_pos = pos;
        }
    }
}

/*
 * Hier stand bis 0.9.58 ein Task, der alle 5 s die CPU-Last beider Kerne ins Log
 * schrieb - gerechnet aus der FreeRTOS-Laufzeitstatistik (IDLE-Zeit je Kern) und
 * uxTaskGetSystemState().
 *
 * Entfernt, weil er im Betrieb nichts beitraegt:
 *   - dieselben Zahlen liefert das Konsolenkommando "tasks" auf Abruf
 *     (esp_gmf_oal_sys_get_real_time_stats()),
 *   - der Task lief auf Kern 1, also auf demselben Kern wie der Mischer, und
 *     weckte dort alle 5 s die Auswertung ueber ALLE Tasklisten.
 * Die beiden FreeRTOS-Optionen in sdkconfig.defaults bleiben: das
 * "tasks"-Kommando braucht sie.
 */

/*
 * Hier stand bis 0.9.41 die "Puffer- und Leerlauf-Diagnose": ein Task mit
 * Prioritaet 2, der im Sekundentakt den Fuelstand der Ringpuffer zwischen
 * Zubringer und Mischer abtastete und Leerlaeufe zaehlte. Sie hat ihren Zweck
 * erfuellt (Befund: der Puffer lief leer, wenn der Datei-Zweig dazukam) und ist
 * zusammen mit der Puffer-Diagnose in 0.9.41 entfernt worden. Was bleibt, ist
 * die einmalige Ausgabe der festen Puffergroessen beim Start.
 */


/*
 * Diagnose: zeigt je Element der Kette, ob es ein dependency-Element ist, ob es
 * einen Ereignis-Empfaenger hat und in welchem Zustand es steht.
 *
 * Hintergrund: die Toninformation laeuft die Kette entlang und bleibt beim
 * ERSTEN dependency-Element stehen (esp_gmf_pipeline.c:210-216). Kommt sie dort
 * nicht an oder hat das Element keinen Empfaenger, wird es nie geoeffnet - die
 * Kette bleibt in OPENING stehen und der Zubringer liefert nichts. Genau das
 * war zu sehen; diese Ausgabe zeigt, woran es liegt.
 */
static void dump_pipeline_state(const char *what, esp_gmf_pipeline_handle_t pipe)
{
    if (pipe == NULL) {
        ESP_LOGE(TAG, "%s: Pipeline ist NULL", what);
        return;
    }
    const void *it = NULL;
    esp_gmf_element_handle_t el = NULL;
    int idx = 0;
    while (esp_gmf_pipeline_iterate_element(pipe, &it, &el) == ESP_GMF_ERR_OK && el != NULL) {
        esp_gmf_event_state_t st = ESP_GMF_EVENT_STATE_NONE;
        esp_gmf_element_get_state(el, &st);
        esp_gmf_element_t *raw = ESP_GMF_ELEMENT_GET(el);
        ESP_LOGD(TAG, "  %s[%d] %s: dependency=%d, event_receiver=%s, state=%d",
                 what, idx, OBJ_GET_TAG(el), raw->dependency,
                 raw->ops.event_receiver ? "ja" : "NEIN", (int)st);
        idx++;
    }
}

static void setup_local2bt_eof_task(void)
{
    BaseType_t ret = xTaskCreate(local2bt_eof_task, "local2bt_eof", 3072, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Create local2bt eof task failed");
    }
}

/*
 * Die festen Puffergroessen einmal ins Log, damit die Zahlen nachvollziehbar
 * sind. Die Ringpuffer zwischen Zubringer und Mischer entstehen in
 * connect_branch_to_mixer().
 */
static void log_buffer_sizes(void)
{
    ESP_LOGI(TAG, "Puffergroessen:");
    ESP_LOGI(TAG, "  I2S-Eingang (io_i2s): Datenbus %d Byte, Lesevorgang %d Byte",
             I2S_INPUT_DB_BYTES, I2S_INPUT_READ_BYTES);
    ESP_LOGI(TAG, "  Ringpuffer Zubringer->Mischer: I2S %d x %d = %d Byte, Datei %d x %d = %d Byte (zusammen %d)",
             MIXER_DB_ITEMS, MIXER_DB_ITEM_SIZE, MIXER_DB_ITEMS * MIXER_DB_ITEM_SIZE,
             FILE_DB_ITEMS, MIXER_DB_ITEM_SIZE, FILE_DB_ITEMS * MIXER_DB_ITEM_SIZE,
             (MIXER_DB_ITEMS + FILE_DB_ITEMS) * MIXER_DB_ITEM_SIZE);
    ESP_LOGI(TAG, "  Mischer-Innenpuffer (process_num): siehe esp_gmf_mixer.c");
    ESP_LOGI(TAG, "  BT-Sendepuffer: vom A2DP-Sender verwaltet (MTU 666 Byte)");
}

void stream_proc_init(esp_gmf_pool_handle_t pool)
{
    /* Datei-Zubringer, I2S-Zweig und Mischer aufbauen; der Datei-Zubringer
     * kommt beim Abspielen an den Mischer, der I2S-Zubringer laeuft immer. */
    setup_pipeline_local2bt(pool);
    setup_pipeline_i2s2bt(pool);
    setup_pipeline_mixer(pool);
    setup_stream_proc_task();
    setup_local2bt_eof_task();
    log_buffer_sizes();

}