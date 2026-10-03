/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_log.h"
#include "esp_gmf_io_i2s_pdm.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "i2s_input.h"

static const char *TAG = "I2S_INPUT";

/*
 * Wiring, taken from the schematic notes:
 *
 *     ESP32 GPIO25 -> Vampire P20.6   WSEL / word select
 *     ESP32 GPIO5  -> Vampire P20.8   BCLK
 *     ESP32 GPIO35 <- Vampire P20.10  DIN
 *
 * The Vampire is the master and supplies BCLK and WS, so the ESP32 runs as
 * I2S_ROLE_SLAVE. A slave does not need MCLK, and the Vampire does not provide
 * one either, hence MCLK is left unused.
 *
 * GPIO35 is input-only on the ESP32 (SOC_GPIO_VALID_OUTPUT_GPIO_MASK excludes
 * bits 34-39) - exactly right for a data input.
 */
#define I2S_INPUT_BCLK_IO   GPIO_NUM_5
#define I2S_INPUT_WS_IO     GPIO_NUM_25
#define I2S_INPUT_DIN_IO    GPIO_NUM_35

/*
 * Format: unchanged from the old project that ran with this hardware.
 *
 * 60000 Hz is unusual, but that is what the Vampire sends and what the old
 * project read successfully. 32 bit per slot and stereo likewise.
 *
 * Note: SOC_I2S_MAX_DATA_WIDTH is 24 on the ESP32, yet the old project used
 * I2S_DATA_BIT_WIDTH_32BIT with a 32 bit slot and converted down in software.
 * The I2S driver accepts it, so we keep the same setting - the conversion to
 * 16 bit happens in GMF (aud_asrc / aud_bit_cvt).
 *
 * mclk_multiple 1152 as in the old project; it only affects MCLK, which is
 * unused here, but keeping it means the clock configuration matches a setup
 * that is known to work.
 */
#define I2S_INPUT_RATE_HZ       60000
/*
 * 32 Bit einlesen - NICHT auf 16 Bit verkuerzen.
 *
 * Der Versuch, nur die oberen 16 Bit zu lesen (um Datenmenge und
 * Ratenwandlung zu entlasten), ist fehlgeschlagen: der Mischer rechnet fest
 * in 32 Bit (esp_gmf_mixer.c, MISMATCH_CHANNELS_BITS), und die erzwungene
 * 16-Bit-Eingabe brachte die Formate durcheinander. Folge:
 *
 *   E Job failed[...aud_ch_cvt_file_proc], ret:-1
 *
 * und die A2DP-Verbindung kam gar nicht mehr zustande. Der I2S-Empfaenger
 * liest deshalb wieder die vollen 32 Bit; die Umwandlung auf 16 Bit macht der
 * aud_bit_cvt im I2S-Zweig, genau wie vorher.
 */
#define I2S_INPUT_BITS          I2S_DATA_BIT_WIDTH_32BIT
#define I2S_INPUT_SLOT_BITS     I2S_SLOT_BIT_WIDTH_32BIT
#define I2S_INPUT_MCLK_MULT     I2S_MCLK_MULTIPLE_1152
#define I2S_INPUT_PORT          I2S_NUM_0

esp_err_t i2s_input_create(esp_gmf_io_handle_t *io)
{
    if (io == NULL) {
        return ESP_FAIL;
    }
    *io = NULL;

    ESP_LOGI(TAG, "I2S RX, SLAVE (Vampire is master): %d Hz, %d bit, stereo",
             I2S_INPUT_RATE_HZ, (int)I2S_INPUT_BITS);
    ESP_LOGI(TAG, "  BCLK=GPIO%d  WS=GPIO%d  DIN=GPIO%d",
             I2S_INPUT_BCLK_IO, I2S_INPUT_WS_IO, I2S_INPUT_DIN_IO);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_INPUT_PORT, I2S_ROLE_SLAVE);
    i2s_chan_handle_t rx_chan = NULL;
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_INPUT_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_INPUT_BITS, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_INPUT_BCLK_IO,
            .ws   = I2S_INPUT_WS_IO,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_INPUT_DIN_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_INPUT_MCLK_MULT;
    std_cfg.slot_cfg.slot_bit_width = I2S_INPUT_SLOT_BITS;

    err = i2s_channel_init_std_mode(rx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        i2s_del_channel(rx_chan);
        return err;
    }

    /*
     * The GMF I2S IO is named "..._pdm", but the read path is driver-agnostic:
     * _i2s_pdm_acquire_read() only calls i2s_channel_read() on the handle we
     * pass in. Only the write path is PDM-specific, so this is the right piece
     * for a standard I2S input too.
     */
    i2s_pdm_io_cfg_t io_cfg = ESP_GMF_IO_I2S_PDM_CFG_DEFAULT();
    io_cfg.pdm_chan = rx_chan;
    io_cfg.dir = ESP_GMF_IO_DIR_READER;
    io_cfg.name = "io_i2s";
    /*
     * Task UND Puffer muessen zusammen gesetzt werden.
     *
     * esp_gmf_io.c:327-350: ist thread.stack > 0, laeuft die IO asynchron mit
     * eigenem Task und braucht einen Datenbus; ist dann buffer_size == 0,
     * bricht esp_gmf_io_open() mit "Failed to create data bus" ab. Genau das
     * ist passiert, als hier nur der Stack gesetzt war.
     *
     *   buffer_size = Groesse des Datenbusses (esp_gmf_io.c:329)
     *   io_size     = Groesse eines Lesevorgangs daraus (esp_gmf_io.c:114)
     *
     * 16 KB Puffer und 2 KB pro Lesevorgang: bei 60 kHz/32 Bit/stereo sind das
     * 480000 Byte/s, ein Datenbus deckt damit rund 34 ms ab.
     *
     * Vorher standen hier 32 KB (~68 ms). Das ist auf einem Modul OHNE PSRAM zu
     * viel: der Datenbus wird mit esp_gmf_db_new_block(1, buffer_size)
     * angelegt (esp_gmf_io.c:329), also als EIN zusammenhaengender Block im
     * internen RAM. Zusammen mit den beiden 20-KB-Ringpuffern ging dem Mischer
     * danach der Speicher aus ("esp_gmf_block_create: Memory exhausted") und er
     * startete gar nicht. 16 KB ist der Kompromiss.
     */
    io_cfg.io_cfg.thread.stack = 3072;
    /*
     * Prioritaet 16: HOEHER als der Datei-Zweig (local2bt_task, 15).
     *
     * WARUM (gemessen im Mischbetrieb): der I2S-Eingang ist eine
     * ECHTZEIT-Quelle mit DMA-Deadline. Der Datei-Zweig dagegen dekodiert so
     * schnell er kann und fuellt seinen Ringpuffer sofort auf 100 % - er ist
     * eine Puffer-Quelle. Bei gleicher Prioritaet verdraengt der gierige
     * Datei-Zweig den Echtzeit-Zweig, und der I2S-Puffer laeuft leer:
     *
     *   Puffer I2S-Zweig: 4888/20480 (23%) -> 0/20480 (0%), leer 1 mal
     *   Puffer Datei-Zweig: 20480/20480 (100%)
     *
     * Die Vampire verschwindet dann. Mit hoeherer Prioritaet holt sich der
     * Echtzeit-Zweig seine CPU zurueck; der Datei-Zweig bekommt den Rest
     * (er braucht nur ~176 kB/s, also weit weniger als eine Kernhaelfte).
     */
    io_cfg.io_cfg.thread.prio = 16;
    /*
     * Kern 1, NICHT Kern 0.
     *
     * WARUM (am 02.10. gemessen, Mischbetrieb Vampire + WAV): mit io_i2s auf
     * Kern 0 lief der I2S-Zweig-Puffer leer, sobald eine Datei dazukam:
     *
     *   Puffer I2S-Zweig: 0/20480 Byte (0%), Minimum 0, leer 1 mal
     *   CPU-Last: Kern0 81% -> 93%
     *
     * und die Task-Statistik zeigte, warum:
     *
     *   Kern 0: mixer_task 24,3% | BTU_TASK 6,8% | btController 6,7% |
     *           io_i2s 3,1% | IDLE0 nur 5,4%   -> Kern 0 ist voll
     *   Kern 1: IDLE1 44,6% | i2s2bt_task 4,0% | a2dp_src_send 1,3%  -> Platz
     *
     * io_i2s ist der ERZEUGER des Vampire-Tons. Bekommt er keine CPU, liefert
     * er nicht, der Ringpuffer laeuft leer und die Vampire verschwindet - genau
     * das, was nicht passieren darf. Der Erzeuger gehoert auf den Kern mit
     * Reserve.
     */
    io_cfg.io_cfg.thread.core = 1;
    io_cfg.io_cfg.thread.stack_in_ext = true;
    io_cfg.io_cfg.buffer_cfg.io_size = 2048;
    /*
     * 12 KB statt 16 KB (geaendert 03.10.).
     *
     * Der Datei-Zweig scheiterte an 15360 Byte, die der GMF-Ratenwandler fuer
     * seine Koeffizienten-Matrix braucht (40 kHz-Familie <-> 11025er-Familie).
     * Zusammen mit den kleineren Ringpuffern (MIXER_DB_ITEMS 20 -> 12) werden
     * hier weitere 4 KB frei.
     *
     * 12 KB decken bei 480000 Byte/s (60 kHz, 32 Bit, stereo) rund 25 ms ab;
     * gelesen wird in 2048-Byte-Schritten. Der Task laeuft auf Kern 1 und ist
     * der einzige Echtzeit-Erzeuger dort - 25 ms Reserve sind ausreichend.
     */
        /*
     * 6 KB statt 12 KB (0.9.31).
     *
     * Gemessen mit 'free' (0.9.30): waehrend des Streams waren nur 27 KB frei,
     * und der groesste ZUSAMMENHAENGENDE Block war 1,4-2,4 KB. Der MP3-Decoder
     * braucht 28 KB am Stueck und scheiterte deshalb beim zweiten Dateistart:
     *
     *   E ESP_MP3_DEC: Fail to init MP3 decoder ret 10
     *
     * Der Datenbus ist mit 2048 Byte Lesevorgang auch mit 6 KB noch
     * dreifach ueberdimensioniert.
     */
    io_cfg.io_cfg.buffer_cfg.buffer_size = 6 * 1024;
    /*
     * Eingebauter Durchsatz-Monitor: er zaehlt die gelesenen Bytes. Damit sehen
     * wir ohne Zusatzcode, ob ueberhaupt Daten ankommen - und ob die Menge zur
     * erwarteten Datenrate passt.
     *
     * Erwartung bei 60 kHz, 32 Bit, stereo:
     *     60000 * 4 Byte * 2 Kanaele = 480000 Byte/s = 3840 kbit/s
     */
    io_cfg.io_cfg.enable_speed_monitor = true;

    /*
     * Kein Durchsatz-Monitor mehr.
     *
     * Der eingebaute Geschwindigkeitszaehler wird im Datenbus-Pfad des IOs
     * aktualisiert, und die Pipeline arbeitet mit einer KLON des Pool-IO. Ein
     * Monitor, der am Pool-Exemplar liest, sieht deshalb immer 0 - und hat mit
     * seinem Sekundentakt samt Warnzeile nur UART-Zeit und CPU gefressen, was
     * sich als abgehackter Ton bemerkbar macht. Der Ton selbst ist der beste
     * Messwert.
     */

    esp_gmf_err_t gmf_err = esp_gmf_io_i2s_pdm_init(&io_cfg, io);
    if (gmf_err != ESP_GMF_ERR_OK || *io == NULL) {
        ESP_LOGE(TAG, "esp_gmf_io_i2s_pdm_init failed: %d", gmf_err);
        i2s_del_channel(rx_chan);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "I2S input ready");
    return ESP_OK;
}

void i2s_input_start_monitor(void)
{
    /* Bewusst leer - siehe die Erklaerung in i2s_input_create(). */
}
