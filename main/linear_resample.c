/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * linear_resample.c - billiger 60 kHz -> 44,1 kHz Umsetzer mit 32->16 Bit.
 *
 * WARUM DIESES ELEMENT:
 * Der GMF-eigene aud_rate_cvt rechnet mit einem Polyphasen-Filter und war im
 * Task i2s2bt_task mit 27 % der groesste Einzelposten im ganzen System (gemessen
 * ueber die FreeRTOS-Laufzeitstatistik). Zusammen mit dem Mischer (25 %) und dem
 * Bluetooth-Stack (15 %) war Kern 0 am Anschlag (98 %, Interrupt-Watchdog).
 *
 * Der Vorgaenger dieses Projekts hat dieselbe Umsetzung auf demselben Chip mit
 * einer linearen Interpolation in Festkomma geschafft - siehe
 * ESP32-I2S-to-BT/main/My_Audio_converter_60to44_1khz.c. Genau diese Rechnung
 * ist hier uebernommen, nur als GMF-Element statt als ESP-ADF-Element.
 *
 * Die Konstanten sind unveraendert aus My_Audio_converter_60to44_1khz.h:
 *   Q_SHIFT          16          (Q16.16 Festkomma)
 *   Q_ONE            65536
 *   RATE_RATIO_FIXED 89088       (= 60000/44100 * 65536)
 *
 * Eingang:  Vorzeichenbehaftete 32-Bit-Samples, stereo. Die oberen 16 Bit
 *           werden uebernommen (>> 16) - das entspricht
 *           My_Audio_converter_32to16bit.c:60.
 * Ausgang: 16-Bit-Samples, stereo, 44,1 kHz.
 *
 * Hinweis zur Anbindung: gmf_audio_common.h ist PRIVAT (private_include), der
 * Helfer gmf_audio_update_snd_info ist von aussen also nicht erreichbar. Er ist
 * hier mit den oeffentlichen Funktionen nachgebaut (siehe unten).
 */

#include <string.h>
#include <stdlib.h>

#include "esp_gmf_audio_element.h"
#include "esp_gmf_element.h"
#include "esp_gmf_err.h"
#include "esp_gmf_info.h"
#include "esp_gmf_oal_mem.h"
#include "esp_gmf_port.h"
#include "esp_log.h"

/* Eigener Header: liefert aud_lin_resample_cfg_t und die oeffentlichen
 * Funktionen. Er MUSS eingebunden sein, sonst kennt die Datei ihren eigenen
 * Konfigurationstyp nicht (der Fehler beim Bau von 0.9.25). */
#include "linear_resample.h"

static const char *TAG = "LIN_RESAMPLE";

/* --- Festkomma wie im Vorgaengerprojekt ----------------------------------- */
#define Q_SHIFT             16
#define Q_ONE               (1 << Q_SHIFT)

/*
 * 60000 / 48000 in Q16.16 = 1,25 * 65536 = 81920.
 *
 * WARUM 48 kHz UND NICHT MEHR 44,1 kHz (Befund 02.10.): die Ausgangsrate muss
 * die Rate sein, die der A2DP-Sender mit der Senke AUSGEHANDELT hat. Der
 * SBC-Encoder resampelt naemlich nicht - er bekommt die Rate nur gesagt.
 *
 *   Kopfhoerer: handelte 44100 Hz aus  -> Kette lief mit 44100, Ton sauber
 *   Soundbar:   handelte 48000 Hz aus  -> Kette lief weiter mit 44100
 *
 * Folge bei der Soundbar: sie zieht 7,5 SBC-Rahmen je 20 ms, wir lieferten
 * aber nur ~6,9 - der Puffer der Senke lief leer, hoerbar als Ruckler.
 *
 * Dauerhafte Loesung waere, die Rate beim Stream-Start aus der Aushandlung zu
 * uebernehmen; das ist der naechste Schritt. Bis dahin fest 48 kHz.
 */
#define RATE_RATIO_FIXED    81920       /* 60000 / 48000 in Q16.16 */

#define LIN_RESAMPLE_TAG_DEFAULT "aud_lin_resample"
#define LIN_RESAMPLE_IN_RATE    60000
#define LIN_RESAMPLE_OUT_RATE   48000
#define LIN_RESAMPLE_CHANNELS   2
#define LIN_RESAMPLE_IN_BITS    32
#define LIN_RESAMPLE_OUT_BITS   16

/*
 * Ein Sample lesen - je nach Eingangsformat.
 *   32 Bit: die oberen 16 Bit sind gueltig (I2S der Vampire)
 *   16 Bit: direkt uebernehmen (Decoder-Ausgabe im Datei-Zweig)
 */
static inline int16_t lin_read_sample(const void *buf, uint8_t in_bits, int index)
{
    if (in_bits == 32) {
        return (int16_t)(((const int32_t *)buf)[index] >> 16);
    }
    return ((const int16_t *)buf)[index];
}

typedef struct {
    esp_gmf_audio_element_t parent;     /* muss erstes Feld sein */
    int32_t input_pos_fixed;            /* Position im Eingang, Q16.16 */
    int16_t last_sample_cache[2];       /* letztes Frame des vorigen Blocks */
    /*
     * Ausgangsrate und Verhaeltnis zur LAUFZEIT.
     *
     * Sie stehen nicht mehr als Konstante im Code, weil der A2DP-Sender die
     * Rate mit der Senke aushandelt und dabei zwischen 44100 und 48000
     * wechselt (am 02.10. bei derselben Soundbar beides gesehen). Der Encoder
     * resampelt nicht, er bekommt die Rate nur gesagt - passt sie nicht zum
     * gelieferten Ton, laeuft der Puffer der Senke leer (Ruckler).
     *
     * Eingestellt wird sie beim Stream-Start aus der Aushandlung
     * (aud_lin_resample_set_out_rate, gerufen aus i2s2bt_set_stream).
     */
    uint32_t out_rate;
    int32_t ratio_fixed;                /* in_rate / out_rate in Q16.16 */
    uint8_t in_bits;                    /* 32 = obere 16 Bit, 16 = direkt */
    /*
     * Kanalzahl der QUELLE. Sie wird aus der Toninformation uebernommen und
     * auch so wieder ausgegeben - der Kanalwandler dahinter (aud_ch_cvt_file)
     * macht aus Mono dann Stereo. Fest 2 waere falsch: eine Mono-WAV wurde
     * damit als Stereoframe gelesen (zwei Monosamples = ein Frame) und lief
     * doppelt so schnell.
     */
    uint8_t in_channels;
    uint32_t in_rate;                   /* Eingangsrate der Quelle */
    bool in_rate_from_source;           /* true = aus der Toninformation */

} lin_resample_t;

/*
 * Ersatz fuer GMF_AUDIO_UPDATE_SND_INFO (privat): meldet das neue Tonformat
 * und gibt es an die Kette weiter. Reihenfolge wie im Original - erst setzen,
 * dann melden.
 */
static void lin_resample_set_snd_info(esp_gmf_element_handle_t self, uint32_t rate, uint8_t bits, uint8_t ch)
{
    esp_gmf_info_sound_t info = {0};
    esp_gmf_audio_el_get_snd_info(self, &info);
    info.sample_rates = rate;
    info.channels = ch;
    info.bits = bits;
    esp_gmf_audio_el_set_snd_info(self, &info);
    esp_gmf_element_notify_snd_info(self, &info);
}

/* --- Element-Lebenszyklus ------------------------------------------------- */

/* Vorwaertsdeklarationen: werden schon in lin_resample_new() eingetragen. */
static esp_gmf_err_t lin_resample_destroy(esp_gmf_obj_handle_t handle);
static esp_gmf_err_t lin_resample_received_event(esp_gmf_event_pkt_t *evt, void *ctx);
static esp_gmf_job_err_t lin_resample_open(esp_gmf_element_handle_t self, void *para);
static esp_gmf_job_err_t lin_resample_process(esp_gmf_element_handle_t self, void *para);
static esp_gmf_job_err_t lin_resample_close(esp_gmf_element_handle_t self, void *para);

static esp_gmf_err_t lin_resample_new(void *cfg, esp_gmf_obj_handle_t *handle)
{
    ESP_GMF_NULL_CHECK(TAG, handle, { return ESP_GMF_ERR_INVALID_ARG; });

    /*
     * Konfiguration uebernehmen. Wichtig: esp_gmf_obj_dupl() ruft new_obj mit
     * GENAU DIESER cfg erneut auf - deshalb wird sie unten per
     * esp_gmf_obj_set_config() am Objekt hinterlegt. Sonst haette der Klon in
     * der Pipeline wieder die Vorgabewerte (dieselbe Falle wie bei den ops).
     */
    aud_lin_resample_cfg_t c = {0};
    if (cfg != NULL) {
        c = *(aud_lin_resample_cfg_t *)cfg;
    }
    if (c.tag == NULL)      { c.tag = LIN_RESAMPLE_TAG_DEFAULT; }
    if (c.in_bits == 0)     { c.in_bits = LIN_RESAMPLE_IN_BITS; }
    if (c.out_rate == 0)    { c.out_rate = LIN_RESAMPLE_OUT_RATE; }
    bool rate_from_source = (c.in_rate == 0);
    /*
     * Vorgabe bis die Quelle ihre Rate meldet: 1:1, also keine
     * Tonhoehenaenderung. c bleibt dabei auf in_rate = 0 stehen - denn
     * esp_gmf_obj_dupl() ruft new_obj() mit genau dieser cfg erneut auf, und
     * nur so erkennt auch der Klon den Quellen-Modus.
     */
    uint32_t rate_effektiv = rate_from_source ? c.out_rate : c.in_rate;

    lin_resample_t *self = esp_gmf_oal_calloc(1, sizeof(lin_resample_t));
    ESP_GMF_MEM_VERIFY(TAG, self, { return ESP_GMF_ERR_MEMORY_LACK; }, "linear resample", sizeof(lin_resample_t));
    self->last_sample_cache[0] = 0;
    self->last_sample_cache[1] = 0;
    self->out_rate = c.out_rate;
    self->in_bits = c.in_bits;
    self->in_channels = LIN_RESAMPLE_CHANNELS; /* Vorgabe stereo, s. received_event */
    self->in_rate = c.in_rate;                 /* 0 = noch unbekannt */
    self->in_rate_from_source = rate_from_source;
    self->ratio_fixed = (int32_t)(((int64_t)rate_effektiv << Q_SHIFT) / (int64_t)c.out_rate);

    esp_gmf_element_cfg_t el_cfg = {0};
    ESP_GMF_ELEMENT_IN_PORT_ATTR_SET(el_cfg.in_attr, ESP_GMF_EL_PORT_CAP_SINGLE, 0, 0,
        ESP_GMF_PORT_TYPE_BLOCK | ESP_GMF_PORT_TYPE_BYTE, ESP_GMF_ELEMENT_PORT_DATA_SIZE_DEFAULT);
    ESP_GMF_ELEMENT_IN_PORT_ATTR_SET(el_cfg.out_attr, ESP_GMF_EL_PORT_CAP_SINGLE, 0, 0,
        ESP_GMF_PORT_TYPE_BLOCK | ESP_GMF_PORT_TYPE_BYTE, ESP_GMF_ELEMENT_PORT_DATA_SIZE_DEFAULT);
    el_cfg.dependency = true;   /* braucht die Toninformation, siehe Event-Handler */

    esp_gmf_err_t ret = esp_gmf_audio_el_init(self, &el_cfg);
    ESP_GMF_RET_ON_ERROR(TAG, ret, { esp_gmf_oal_free(self); return ret; }, "init linear resample");

    esp_gmf_obj_t *obj = (esp_gmf_obj_t *)self;
    obj->new_obj = lin_resample_new;
    obj->del_obj = lin_resample_destroy;

    /*
     * ALLE ops hier setzen, nicht in aud_lin_resample_init().
     *
     * Grund (auf Hardware nachgemessen): die Pipeline arbeitet mit einer KOPIE
     * des Elements. esp_gmf_obj_dupl() ruft dafuer obj->new_obj - also genau
     * diese Funktion. Alles, was erst nach dem Pool-Aufbau am Element gesetzt
     * wird, fehlt der Kopie. Die Diagnose zeigte das eindeutig:
     *
     *   vor-report[1] aud_lin_resample: dependency=1, event_receiver=NEIN, state=0
     *
     * Ohne Empfaenger kommt das dependency-Element nie nach INITIALIZED, wird
     * nie geoeffnet, und der Zubringer liefert nichts.
     */
    esp_gmf_element_handle_t el = (esp_gmf_element_handle_t)self;
    ESP_GMF_ELEMENT_GET(el)->ops.open = lin_resample_open;
    ESP_GMF_ELEMENT_GET(el)->ops.process = lin_resample_process;
    ESP_GMF_ELEMENT_GET(el)->ops.close = lin_resample_close;
    ESP_GMF_ELEMENT_GET(el)->ops.event_receiver = lin_resample_received_event;

    /*
     * Die Konfiguration muss den Aufruf UEBERLEBEN.
     *
     * esp_gmf_obj_set_config() merkt sich nur den ZEIGER (esp_gmf_obj.c:71).
     * Eine Stack-Kopie ist nach dem Return ungueltig - genau daran ist 0.9.25
     * abgestuerzt: esp_gmf_obj_dupl() (esp_gmf_obj.c:24) reicht obj->cfg an
     * new_obj() weiter, dort wurde der Zeiger auf die tote Stack-Variable
     * gelesen und strlen() darauf gerufen:
     *
     *   Guru Meditation Error: Core 0 panic'ed (LoadProhibited). EXCVADDR: 0x80092968
     *   Backtrace: esp_gmf_obj_set_tag <- lin_resample_new <- esp_gmf_obj_dupl
     *              <- esp_gmf_pool_new_element <- esp_gmf_pool_new_pipeline
     *
     * Deshalb hier eine eigene Kopie auf dem Heap. Sie gehoert dem Objekt und
     * wird in lin_resample_destroy() wieder freigegeben.
     */
    aud_lin_resample_cfg_t *cfg_keep = esp_gmf_oal_calloc(1, sizeof(aud_lin_resample_cfg_t));
    ESP_GMF_MEM_VERIFY(TAG, cfg_keep, { esp_gmf_oal_free(self); return ESP_GMF_ERR_MEMORY_LACK; },
                       "linear resample cfg", sizeof(aud_lin_resample_cfg_t));
    *cfg_keep = c;      /* c.tag zeigt auf ein Literal bzw. bleibt gueltig */
    esp_gmf_obj_set_config(obj, cfg_keep, sizeof(aud_lin_resample_cfg_t));
    ret = esp_gmf_obj_set_tag(obj, c.tag);
    ESP_GMF_RET_ON_ERROR(TAG, ret, { esp_gmf_oal_free(self); return ret; }, "set tag");

    *handle = obj;
    ESP_LOGI(TAG, "Element angelegt: %s (%u Hz %u Bit -> %u Hz %u Bit, linear Q16.16%s)",
             c.tag, (unsigned)rate_effektiv, c.in_bits, (unsigned)c.out_rate, LIN_RESAMPLE_OUT_BITS,
             rate_from_source ? ", Eingangsrate aus der Quelle" : "");
    return ESP_GMF_ERR_OK;
}

static esp_gmf_err_t lin_resample_destroy(esp_gmf_obj_handle_t handle)
{
    if (handle) {
        /* Die Konfigurationskopie aus lin_resample_new() gehoert dem Objekt. */
        void *cfg = OBJ_GET_CFG(handle);
        esp_gmf_audio_el_deinit((esp_gmf_element_handle_t)handle);
        esp_gmf_oal_free(cfg);
        esp_gmf_oal_free(handle);
    }
    return ESP_GMF_ERR_OK;
}

static esp_gmf_job_err_t lin_resample_open(esp_gmf_element_handle_t self, void *para)
{
    (void)para;
    lin_resample_t *res = (lin_resample_t *)self;
    res->input_pos_fixed = 0;
    res->last_sample_cache[0] = 0;
    res->last_sample_cache[1] = 0;

    if (res->ratio_fixed <= 0) {
        /* Kann bei vernuenftigen Raten nicht passieren - aber eine Division
         * durch 0 in process() waere ein Absturz. */
        ESP_LOGW(TAG, "Verhaeltnis unbrauchbar (%d) - es wird 1:1 gerechnet", (int)res->ratio_fixed);
        res->ratio_fixed = Q_ONE;
    }
    /* Ausgangsformat festschreiben, damit die Folgenglieder es kennen. */
    lin_resample_set_snd_info(self, res->out_rate, LIN_RESAMPLE_OUT_BITS, LIN_RESAMPLE_CHANNELS);
    ESP_LOGI(TAG, "Open: %u Hz, %d ch -> %u Hz, %d ch (Verhaeltnis Q16.16: %d)",
             (unsigned)(res->in_rate ? res->in_rate : res->out_rate), res->in_channels,
             (unsigned)res->out_rate, res->in_channels, (int)res->ratio_fixed);
    return ESP_GMF_JOB_ERR_OK;
}

static esp_gmf_job_err_t lin_resample_close(esp_gmf_element_handle_t self, void *para)
{
    (void)self;
    (void)para;
    return ESP_GMF_JOB_ERR_OK;
}

/*
 * Der Kern: aus 32-Bit-Stereo-Frames werden 16-Bit-Frames mit 44,1 kHz.
 *
 * Die Rechnung ist Zeile fuer Zeile die aus
 * My_Audio_converter_60to44_1khz.c:63-131, nur dass hier zusaetzlich die
 * 32-Bit-Samples auf die oberen 16 Bit gekuerzt werden (>> 16).
 */
static esp_gmf_job_err_t lin_resample_process(esp_gmf_element_handle_t self, void *para)
{
    (void)para;
    lin_resample_t *res = (lin_resample_t *)self;
    esp_gmf_port_handle_t in_port = ESP_GMF_ELEMENT_GET(self)->in;
    esp_gmf_port_handle_t out_port = ESP_GMF_ELEMENT_GET(self)->out;

    esp_gmf_payload_t *in_load = NULL;
    esp_gmf_payload_t *out_load = NULL;
    esp_gmf_job_err_t out_len = ESP_GMF_JOB_ERR_OK;

    esp_gmf_err_io_t load_ret = esp_gmf_port_acquire_in(in_port, &in_load, ESP_GMF_ELEMENT_GET(self)->in_attr.data_size,
                                                        ESP_GMF_MAX_DELAY);
    if (load_ret != ESP_GMF_IO_OK || in_load == NULL) {
        if (load_ret == ESP_GMF_IO_ABORT) {
            return ESP_GMF_JOB_ERR_ABORT;
        }
        if (load_ret == ESP_GMF_IO_TIMEOUT) {
            return ESP_GMF_JOB_ERR_OK;
        }
        ESP_LOGE(TAG, "Eingang nicht lesbar: %d", load_ret);
        return ESP_GMF_JOB_ERR_FAIL;
    }

    /*
     * Anzahl der Eingangsframes im Block.
     *
     * Kanalzahl und Bittiefe kommen aus der Toninformation der Quelle: die
     * Vampire liefert 2 Kanaele mit 32 Bit, eine Mono-WAV im Datei-Zweig nur
     * einen Kanal mit 16 Bit.
     */
    int ch = res->in_channels;
    if (ch != 1 && ch != 2) {
        ch = LIN_RESAMPLE_CHANNELS;
    }
    /*
     * Ausgang ist IMMER stereo (LIN_RESAMPLE_CHANNELS), auch bei Mono-Quelle:
     * ein Monosample wird auf beide Kanaele kopiert. Damit braucht der
     * Datei-Zweig keinen eigenen Kanalwandler mehr (0.9.33) - und wir koennen
     * das Ergebnis hier selbst pruefen.
     */
    const int out_ch = LIN_RESAMPLE_CHANNELS;
    int in_frames = in_load->valid_size / (ch * (res->in_bits / 8));
    const void *in_buf = in_load->buf;
    int16_t *out_samples = NULL;
    int out_index = 0;

    /*
     * Angeforderte Ausgangsgroesse: ein Ausgangssample je
     * ratio_fixed/65536 Eingangsframes. Bei einer ABwaertswandlung
     * (60000 -> 48000) sind das MEHR Ausgangsframes als Eingangsframes,
     * deshalb wird hier GETRUNT, nicht multipliziert.
     *
     * Fassung 0.9.26 hatte "in_frames * ratio" stehen - damit war der Ausgang
     * regelmaessig zu klein, die Schleife brach jeden Block zu frueh ab und die
     * Restposition lief immer weiter ins Minus. Sobald sie unter -1 Frame lag,
     * lieferte das Element gar nichts mehr:
     *
     *   LIN_RESAMPLE: Block 2: in_frames=1152 ... out_samples=0, uebrig=-1158 (Ausgang leer!)
     *
     * Das war die Verzerrung der MP3 (Bloccke fielen aus).
     */
    int64_t out_frames_max = (((int64_t)in_frames << Q_SHIFT) / res->ratio_fixed) + 2;
    int max_out_bytes = (int)out_frames_max * out_ch * (LIN_RESAMPLE_OUT_BITS / 8);
    /*
     * Auf 1024 Byte aufrunden.
     *
     * Der Port vergroessert seinen Puffer auf Anforderung, und zwar mit
     * freiem+belegtem Speicher bei JEDER anderen Groesse
     * (esp_gmf_payload_realloc_buffer_with_separate_alignment). Die Bloecke des
     * Decoders sind aber unterschiedlich gross (gemessen: 47, 128 und 1152
     * Frames) - ohne Aufrunden ergibt das staendig andere Anforderungen und
     * damit einen zersplitterten Heap. Genau daran ist der Bluetooth-Stack
     * gescheitert:
     *
     *   E BT_OSI: calloc failed (caller=0x401077df size=622)
     *   W BT_AUD_A2D_SRC: Failed to send frame batch: ESP_ERR_NO_MEM
     *
     * Mit 1024er-Schritten gibt es nur noch wenige Groessen, und weil der Port
     * einen einmal grossen Puffer nicht mehr verkleinert, ist es nach dem
     * ersten grossen Block nur noch EINE Groesse.
     */
    max_out_bytes = ((max_out_bytes + 1023) / 1024) * 1024;
    load_ret = esp_gmf_port_acquire_out(out_port, &out_load, max_out_bytes, ESP_GMF_MAX_DELAY);
    ESP_GMF_PORT_ACQUIRE_OUT_CHECK(TAG, load_ret, out_len, { goto __release; });
    out_samples = (int16_t *)out_load->buf;

    /*
     * Sicherheitsnetz: sollte der Port trotz Anforderung weniger Platz
     * geliefert haben, wird nur so viel geschrieben, wie hineinpasst. Das darf
     * nicht passieren (der Port vergroessert seinen Puffer auf Anforderung) -
     * deshalb eine ERROR-Zeile, damit es im Log auffaellt.
     */
    int out_cap_frames = out_load->buf_length / (out_ch * (LIN_RESAMPLE_OUT_BITS / 8));
    if (out_cap_frames <= 0) {
        ESP_LOGE(TAG, "Ausgangspuffer zu klein: %d Byte fuer %d Frames", (int)out_load->buf_length, in_frames);
        out_len = ESP_GMF_JOB_ERR_FAIL;
        goto __release;
    }
    bool abgeschnitten = (out_cap_frames < out_frames_max);
    if (abgeschnitten) {
        ESP_LOGE(TAG, "Ausgangspuffer reicht nicht: %d Frames Platz, %d gebraucht - es wird abgeschnitten",
                 out_cap_frames, (int)out_frames_max);
    }

    int32_t pos = res->input_pos_fixed;

    while (((pos >> Q_SHIFT) < in_frames) && ((out_index / out_ch) < out_cap_frames)) {
        int idx_a = pos >> Q_SHIFT;
        int idx_b = idx_a + 1;
        int32_t frac = pos & (Q_ONE - 1);

        if (idx_b >= in_frames) {
            break;      /* das Folgefame liefert der naechste Block */
        }
        if (idx_a < -1 || idx_a >= in_frames) {
            /* Darf nicht vorkommen: die Restposition liegt immer in [-1, 0). */
            ESP_LOGW(TAG, "Position ausserhalb des Blocks (%d) - Block wird verworfen", idx_a);
            break;
        }

        for (int c = 0; c < out_ch; c++) {
            /* Mono-Quelle: beide Ausgangskanaele lesen denselben Quellkanal. */
            int sc = (ch == 1) ? 0 : c;
            int16_t a = (idx_a == -1) ? res->last_sample_cache[sc]
                                      : lin_read_sample(in_buf, res->in_bits, idx_a * ch + sc);
            int16_t b = lin_read_sample(in_buf, res->in_bits, idx_b * ch + sc);
            int64_t interp = (int64_t)a * (Q_ONE - frac) + (int64_t)b * frac;
            out_samples[out_index++] = (int16_t)(interp >> Q_SHIFT);
        }

        pos += res->ratio_fixed;
    }

    /*
     * Wurde abgeschnitten, zeigt pos hinter das Blockende. Die nicht mehr
     * passenden Eingangsframes sind dann verloren; die Position wird auf das
     * Blockende gezogen, damit der naechste Block wieder bei 0 anfaengt (sonst
     * bliebe pos dauerhaft negativ und die Schleife liefe nie wieder an).
     */
    if (abgeschnitten) {
        pos = (int32_t)((int64_t)in_frames << Q_SHIFT);
    }

    /* Restposition in den naechsten Block uebernehmen */
    int32_t block_len_fixed = (int32_t)in_frames << Q_SHIFT;
    res->input_pos_fixed = pos - block_len_fixed;

    /*
     * Uebergangswert fuer den naechsten Block: IMMER das LETZTE Sample dieses
     * Blocks (Index in_frames-1).
     *
     * Hier stand bis 0.9.33 "last_idx = (pos - ratio_fixed) >> Q_SHIFT" - also
     * das zuletzt AUSGEGEBENE Sample. Das ist falsch: die Restposition zeigt
     * nach dem Abbruch auf Index -1 des naechsten Blocks, und das ist genau das
     * letzte Sample DIESES Blocks. Mit dem alten Wert wurde pro Block das letzte
     * Sample verworfen und das vorletzte wiederholt.
     *
     * Nachgewiesen am 03.10. mit test_tone_48k.wav (440-Hz-Sinus, 48 kHz, mono)
     * gegen die Originaldatei:
     *
     *   Datei:  s[254]=14438  s[255]=13969  s[256]=13453
     *   Log:    erstes Ausgangssample von Block 2 = 14438  (richtig: 13969)
     *           in[0] von Block 2 = 13453                  (stimmt)
     *
     * Der Fehler sass also an JEDER Blockgrenze (alle 256 Samples = 5,3 ms) und
     * war beim 1:1-Durchlauf einer Mono-WAV am deutlichsten hoerbar.
     */
    if (in_frames > 0) {
        for (int c = 0; c < ch; c++) {
            res->last_sample_cache[c] = lin_read_sample(in_buf, res->in_bits, (in_frames - 1) * ch + c);
        }
    }

    out_load->valid_size = out_index * sizeof(int16_t);
    out_load->pts = in_load->pts;
    out_load->is_done = in_load->is_done;
    if (in_load->is_done) {
        out_len = ESP_GMF_JOB_ERR_DONE;
    }

__release:
    if (out_load != NULL) {
        if (esp_gmf_port_release_out(out_port, out_load, ESP_GMF_MAX_DELAY) < ESP_GMF_IO_OK) {
            out_len = ESP_GMF_JOB_ERR_FAIL;
        }
    }
    if (in_load != NULL) {
        if (esp_gmf_port_release_in(in_port, in_load, ESP_GMF_MAX_DELAY) < ESP_GMF_IO_OK) {
            out_len = ESP_GMF_JOB_ERR_FAIL;
        }
    }
    return out_len;
}

/*
 * Auf die Toninformation reagieren. Das Element hat dependency = true und kommt
 * deshalb nur ueber diese Meldung von STATE_NONE nach INITIALIZED - sonst
 * scheitert das Registrieren der Jobs mit ESP_GMF_ERR_NOT_READY (0xffffdff8).
 */
static esp_gmf_err_t lin_resample_received_event(esp_gmf_event_pkt_t *evt, void *ctx)
{
    /*
     * Der Aufruf kam beim Umbau zunaechst gar nicht an - deshalb stand hier ein
     * printf ohne Log-Level-Filter. Jetzt genuegt eine Zeile pro Meldung: die
     * Toninformation kommt selten (einmal je Pipeline-Start), nicht je Block.
     */
    if (evt == NULL || ctx == NULL) {
        return ESP_GMF_ERR_INVALID_ARG;
    }
    if (evt->type != ESP_GMF_EVT_TYPE_REPORT_INFO || evt->sub != ESP_GMF_INFO_SOUND || evt->payload == NULL) {
        return ESP_GMF_ERR_OK;
    }
    esp_gmf_element_handle_t self = (esp_gmf_element_handle_t)ctx;
    esp_gmf_info_sound_t *info = (esp_gmf_info_sound_t *)evt->payload;
    lin_resample_t *res = (lin_resample_t *)self;

    /*
     * Im Datei-Zweig ist die Eingangsrate die Rate DER DATEI (44100, 48000, ...)
     * und nicht die feste I2S-Rate. Sie steht in der Toninformation, die der
     * Decoder meldet - hier wird sie uebernommen und das Verhaeltnis neu
     * gerechnet.
     */
    if (res->in_rate_from_source && info->sample_rates > 0 && info->sample_rates != res->in_rate) {
        res->in_rate = info->sample_rates;
        res->ratio_fixed = (int32_t)(((int64_t)res->in_rate << Q_SHIFT) / (int64_t)res->out_rate);
        ESP_LOGI(TAG, "Eingangsrate aus der Quelle: %u Hz -> %u Hz (Verhaeltnis Q16.16: %d)",
                 (unsigned)res->in_rate, (unsigned)res->out_rate, (int)res->ratio_fixed);
    }
    /* Kanalzahl der Quelle uebernehmen (1 = mono, 2 = stereo). */
    if ((info->channels == 1 || info->channels == 2) && info->channels != res->in_channels) {
        ESP_LOGI(TAG, "Kanalzahl aus der Quelle: %d (vorher %d)", info->channels, res->in_channels);
        res->in_channels = info->channels;
    }
    ESP_LOGI(TAG, "Toninformation: %d Hz, %d Bit, %d ch -> Ausgang %d Hz, %d Bit, %d ch",
             (int)info->sample_rates, info->bits, info->channels,
             (unsigned)((lin_resample_t *)self)->out_rate, LIN_RESAMPLE_OUT_BITS, LIN_RESAMPLE_CHANNELS);
    lin_resample_set_snd_info(self, ((lin_resample_t *)self)->out_rate, LIN_RESAMPLE_OUT_BITS, LIN_RESAMPLE_CHANNELS);
    esp_gmf_event_state_t st = ESP_GMF_EVENT_STATE_NONE;
    esp_gmf_element_get_state(self, &st);
    if (st == ESP_GMF_EVENT_STATE_NONE) {
        esp_gmf_element_set_state(self, ESP_GMF_EVENT_STATE_INITIALIZED);
    }
    return ESP_GMF_ERR_OK;
}

esp_gmf_err_t aud_lin_resample_init_cfg(const aud_lin_resample_cfg_t *cfg, esp_gmf_element_handle_t *handle)
{
    ESP_GMF_NULL_CHECK(TAG, handle, { return ESP_GMF_ERR_INVALID_ARG; });
    *handle = NULL;
    esp_gmf_obj_handle_t obj = NULL;
    esp_gmf_err_t ret = lin_resample_new((void *)cfg, &obj);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "create linear resample");
    *handle = (esp_gmf_element_handle_t)obj;
    return ESP_GMF_ERR_OK;
}

esp_gmf_err_t aud_lin_resample_init(esp_gmf_element_handle_t *handle)
{
    ESP_GMF_NULL_CHECK(TAG, handle, { return ESP_GMF_ERR_INVALID_ARG; });
    *handle = NULL;

    /*
     * Die ops (open/process/close/event_receiver) werden NICHT hier gesetzt,
     * sondern in lin_resample_new(). Grund: die Pipeline arbeitet mit einer
     * Kopie, die ueber obj->new_obj erzeugt wird (esp_gmf_obj_dupl,
     * esp_gmf_obj.c:24). Alles, was nur am Prototyp dieses Aufrufs haengt,
     * fehlt der Kopie.
     *
     * Genau das war der Fehler, der die Kette lahmlegte - die Diagnose zeigte:
     *
     *   vor-report[1] aud_lin_resample: dependency=1, event_receiver=NEIN, state=0
     */
    esp_gmf_obj_handle_t obj = NULL;
    esp_gmf_err_t ret = lin_resample_new(NULL, &obj);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "create linear resample");

    *handle = (esp_gmf_element_handle_t)obj;
    return ESP_GMF_ERR_OK;
}

/*
 * Ausgangsrate zur LAUFZEIT setzen.
 *
 * WARUM DAS NOETIG IST (Befund 02.10.): der A2DP-Sender handelt die Abtastrate
 * mit der Senke aus, und dieselbe Soundbar lieferte dabei einmal 48000 Hz und
 * einmal 44100 Hz. Der SBC-Encoder resampelt nicht - er bekommt die Rate nur
 * gesagt. Liefert die Kette eine andere Rate als ausgehandelt, zieht die Senke
 * mehr Rahmen, als nachkommen, ihr Puffer laeuft leer: hoerbar als Ruckler.
 *
 * Gerufen aus i2s2bt_set_stream(), sobald die Aushandlung feststeht und BEVOR
 * die Pipeline laeuft. Das Verhaeltnis wird aus der Quellrate (60000 Hz, fest
 * durch die Vampire) neu berechnet:
 *
 *     ratio_fixed = 60000 / out_rate * 65536     (Q16.16)
 *
 * 48000 Hz -> 81920       44100 Hz -> 89088   (Wert des Vorgaengerprojekts)
 */
esp_gmf_err_t aud_lin_resample_set_out_rate(esp_gmf_element_handle_t handle, uint32_t out_rate)
{
    if (handle == NULL || out_rate == 0) {
        return ESP_GMF_ERR_INVALID_ARG;
    }
    lin_resample_t *res = (lin_resample_t *)handle;
    res->out_rate = out_rate;
    int64_t rate_in = res->in_rate ? res->in_rate : (int64_t)out_rate;   /* 0 = unbekannt -> 1:1 */
    res->ratio_fixed = (int32_t)((rate_in << Q_SHIFT) / (int64_t)out_rate);
    ESP_LOGI(TAG, "Ausgangsrate auf %u Hz gesetzt (Verhaeltnis Q16.16: %d)",
             (unsigned)out_rate, (int)res->ratio_fixed);
    return ESP_GMF_ERR_OK;
}
