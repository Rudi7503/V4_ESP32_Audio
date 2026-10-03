/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_gmf_pool.h"
#include "esp_gmf_err.h"
#include "esp_log.h"

// GMF Audio Elements
#include "esp_audio_enc_default.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_gmf_audio_dec.h"
#include "esp_gmf_audio_enc.h"
#include "esp_gmf_rate_cvt.h"
#include "esp_gmf_ch_cvt.h"
#include "esp_gmf_asrc.h"
#include "esp_gmf_bit_cvt.h"
#include "esp_gmf_aec.h"
#include "esp_gmf_copier.h"
#include "esp_gmf_mixer.h"

// GMF IO Types
#include "esp_gmf_io_file.h"
#include "esp_gmf_io_bt.h"

#include "i2s_input.h"
#include "linear_resample.h"

static const char *TAG = "POOL_INIT";

static float asrc_stereo_weight[] = {
    1.0f, 0.0f,
    0.0f, 1.0f,
};

/**
 * @brief  Register GMF pool with required elements and IO types
 *
 * @param[in]  pool  GMF pool handle to initialize
 *
 * @return
 *       - ESP_GMF_ERR_OK           Success
 *       - ESP_GMF_ERR_INVALID_ARG  Invalid pool handle
 *       - ESP_GMF_ERR_MEMORY_LACK  Memory allocation failed
 */
esp_gmf_err_t pool_reg(esp_gmf_pool_handle_t pool)
{
    ESP_GMF_NULL_CHECK(TAG, pool, return ESP_GMF_ERR_INVALID_ARG);

    ESP_LOGI(TAG, "Registering GMF pool");

    esp_gmf_err_t ret = ESP_GMF_ERR_OK;
    esp_gmf_element_handle_t element = NULL;
    esp_gmf_io_handle_t io = NULL;

    // ========== Register GMF Audio Elements ==========
    ret = esp_audio_dec_register_default();
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register audio decoders");
    ret = esp_audio_simple_dec_register_default();
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register audio simple decoders");
    ret = esp_audio_enc_register_default();
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register audio encoders");

    // 1. Audio Decoder (aud_dec)
    esp_audio_simple_dec_cfg_t dec_cfg = DEFAULT_ESP_GMF_AUDIO_DEC_CONFIG();
    ret = esp_gmf_audio_dec_init(&dec_cfg, &element);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to init audio decoder");
    ret = esp_gmf_pool_register_element(pool, element, NULL);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register audio decoder");
    ESP_LOGD(TAG, "Registered: aud_dec");

    // 2. Audio Encoder (aud_enc)
    esp_audio_enc_config_t enc_cfg = DEFAULT_ESP_GMF_AUDIO_ENC_CONFIG();
    ret = esp_gmf_audio_enc_init(&enc_cfg, &element);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to init audio encoder");
    ret = esp_gmf_pool_register_element(pool, element, NULL);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register audio encoder");
    ESP_LOGD(TAG, "Registered: aud_enc");

    // 3. Audio Rate Converter (aud_rate_cvt)
    esp_ae_rate_cvt_cfg_t rate_cfg = DEFAULT_ESP_GMF_RATE_CVT_CONFIG();
    ret = esp_gmf_rate_cvt_init(&rate_cfg, &element);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to init rate converter");
    ret = esp_gmf_pool_register_element(pool, element, NULL);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register rate converter");
    ESP_LOGD(TAG, "Registered: aud_rate_cvt");

    // 4. Audio Channel Converter (aud_ch_cvt)
    esp_ae_ch_cvt_cfg_t ch_cfg = DEFAULT_ESP_GMF_CH_CVT_CONFIG();
    ret = esp_gmf_ch_cvt_init(&ch_cfg, &element);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to init channel converter");
    ret = esp_gmf_pool_register_element(pool, element, NULL);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register channel converter");
    ESP_LOGD(TAG, "Registered: aud_ch_cvt");

    // 5. Audio Bit Converter (aud_bit_cvt)
    esp_ae_bit_cvt_cfg_t bit_cfg = DEFAULT_ESP_GMF_BIT_CVT_CONFIG();
    ret = esp_gmf_bit_cvt_init(&bit_cfg, &element);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to init bit converter");
    ret = esp_gmf_pool_register_element(pool, element, NULL);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register bit converter");
    ESP_LOGD(TAG, "Registered: aud_bit_cvt");

    /*
     * KEIN aud_aec und KEIN aud_asrc mehr.
     *
     * Beide waren aus dem Beispiel uebernommen, werden aber von diesem Projekt
     * nie benutzt:
     *  - aud_aec (Echo-Unterdrueckung) gehoert zu einem Mikrofonpfad. Es gibt
     *    hier keinen: die Anwendung ist reine A2DP-QUELLE.
     *  - aud_asrc wurde schon frueher durch die Kette
     *    aud_rate_cvt -> aud_bit_cvt ersetzt, weil es beim Registrieren der Jobs
     *    ESP_GMF_ERR_NOT_READY (0xffffdff8) meldete.
     *
     * Beide belegen beim Aufbau Speicher und Arbeitsspeicher fuer Puffer. Auf
     * dem Ersatzmodul OHNE PSRAM stehen nur ~234 KiB interner RAM zur Verfuegung;
     * mit ihnen scheiterte der Pool-Aufbau:
     *
     *   E ESP_GMF_POOL: esp_gmf_pool.c:71 (esp_gmf_pool_init): Memory exhausted
     *   ESP_ERROR_CHECK failed: esp_err_t 0xffffdffe
     *   expression: esp_gmf_pool_init(&pool)
     *
     * Der Code bleibt in einem #if stehen, damit die Messreihe beide Varianten
     * bauen kann (siehe main/CMakeLists.txt, POOL_SMALL).
     */
#if !POOL_SMALL
    esp_gmf_aec_cfg_t aec_cfg = {
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
        .filter_len = 4,
#else
        .filter_len = 2,
#endif  /* CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4 */
        .type = AFE_TYPE_VC_8K,
        .mode = AFE_MODE_HIGH_PERF,
#if CONFIG_IDF_TARGET_ESP32S31
        .input_format = "RMNN",
#else
        .input_format = "RM",
#endif  /* CONFIG_IDF_TARGET_ESP32S31 */
    };
    ret = esp_gmf_aec_init(&aec_cfg, &element);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to init AEC");
    ret = esp_gmf_pool_register_element(pool, element, NULL);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register AEC");
    ESP_LOGI(TAG, "Registered: aud_aec");

    /* 6. Audio ASRC (aud_asrc) */
    esp_asrc_cfg_t asrc_cfg = DEFAULT_ESP_GMF_ASRC_CONFIG();
    asrc_cfg.weight = asrc_stereo_weight;
    asrc_cfg.weight_len = sizeof(asrc_stereo_weight) / sizeof(asrc_stereo_weight[0]);
    ret = esp_gmf_asrc_init(&asrc_cfg, &element);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to init ASRC");
    ret = esp_gmf_pool_register_element(pool, element, NULL);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register ASRC");
    ESP_LOGD(TAG, "Registered: aud_asrc");
#else
    ESP_LOGI(TAG, "POOL_SMALL: aud_aec und aud_asrc werden nicht registriert");
#endif  /* !POOL_SMALL */

    /*
     * Eigene Instanzen fuer den I2S-Zweig.
     *
     * Warum eigene Instanzen: ein Element kann nur an EINEN Task gebunden
     * werden (esp_gmf_pipeline.c:389 "not ready to register job"). Die
     * Beispiele machen es genauso - in pipeline_howl hat pipe_music eigene
     * Konverter, pipe_mic ein eigenes aud_howl, nur der aud_mixer liegt in
     * einer dritten Pipeline.
     *
     * Raten- und Bit-Konverter statt aud_asrc: der ASRC meldet beim
     * Job-Registrieren ESP_GMF_ERR_NOT_READY (0xffffdff8) und laesst die
     * Pipeline in den Run-Timeout laufen - auch dann, wenn nur die I2S-Pipeline
     * existiert. Die Beispiele setzen fuer solche Umsetzungen auf die Kette
     * rate_cvt -> bit_cvt -> ch_cvt (pipeline_howl: aud_dec, aud_rate_cvt,
     * aud_bit_cvt, aud_ch_cvt).
     */
    /*
     * NUR registrieren, was der gebaute Messfall auch benutzt.
     *
     * Jede Registrierung ist ein Element-Objekt samt Puffern - auf dem Modul
     * ohne PSRAM ist der interne Speicher der knappste Rohstoff. Messfall 2
     * (Vorgabe) besteht ausschliesslich aus aud_lin_resample, die Messfaelle 1
     * und 3 ausschliesslich aus der GMF-Kette. Vorher lagen immer beide im
     * Pool. Gemessen (0.9.28, I2S+MP3): der Heap fiel bis auf 25 528 Byte, und
     * der Bluetooth-Stack scheiterte an einer 622-Byte-Anforderung.
     */
#if I2S_MODE == 2 || I2S_MODE == 0
    ESP_LOGI(TAG, "Messfall %d: die GMF-Wandler fuer den I2S-Zweig entfallen", I2S_MODE);
#else
    esp_ae_rate_cvt_cfg_t rate_cfg_i2s = DEFAULT_ESP_GMF_RATE_CVT_CONFIG();
    ret = esp_gmf_rate_cvt_init(&rate_cfg_i2s, &element);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to init rate converter for I2S");
    ret = esp_gmf_pool_register_element(pool, element, "aud_rate_cvt_i2s");
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register rate converter for I2S");
    ESP_LOGD(TAG, "Registered: aud_rate_cvt_i2s");

    esp_ae_bit_cvt_cfg_t bit_cfg_i2s = DEFAULT_ESP_GMF_BIT_CVT_CONFIG();
    ret = esp_gmf_bit_cvt_init(&bit_cfg_i2s, &element);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to init bit converter for I2S");
    ret = esp_gmf_pool_register_element(pool, element, "aud_bit_cvt_i2s");
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register bit converter for I2S");
    ESP_LOGD(TAG, "Registered: aud_bit_cvt_i2s");
#endif  /* I2S_MODE == 2 || I2S_MODE == 0 */

    /*
     * Kanalwandler als LETZTES Element des I2S-Zweigs.
     *
     * Das ist kein Zierrat, sondern noetig: esp_gmf_pool_new_pipeline haengt
     * beim Aufbau der Kette einen Ausgangsport an das vorletzte Element
     * (esp_gmf_pool.c:249) und einen Eingangsport an das letzte. Das LETZTE
     * Element muss danach noch einen freien Ausgang haben, sonst kann
     * connect_pipe den Zubringer nicht an den Mischer haengen:
     *
     *   aud_bit_cvt_i2s hat bereits 1 Ausgangsport(s) - kein zweiter moeglich
     *
     * In pipeline_howl endet die Quellkette genau deshalb mit "aud_ch_cvt"
     * (pipeline_howl.c:182). Stereo bleibt Stereo, der Wandler laeuft dann im
     * Bypass (esp_gmf_ch_cvt.c:91).
     */
#if I2S_MODE == 1 || I2S_MODE == 3 || I2S_MODE == 4
    esp_ae_ch_cvt_cfg_t ch_cfg_i2s = DEFAULT_ESP_GMF_CH_CVT_CONFIG();
    ret = esp_gmf_ch_cvt_init(&ch_cfg_i2s, &element);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to init channel converter for I2S");
    ret = esp_gmf_pool_register_element(pool, element, "aud_ch_cvt_i2s");
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register channel converter for I2S");
    ESP_LOGD(TAG, "Registered: aud_ch_cvt_i2s");
#endif  /* I2S_MODE == 1 || 3 || 4 */

    /*
     * Eigener, billiger Umsetzer fuer den I2S-Zweig: 60 kHz/32 Bit -> 44,1 kHz/16 Bit.
     *
     * Ersetzt fuer den I2S-Zweig aud_rate_cvt_i2s + aud_bit_cvt_i2s. Grund:
     * gemessen ueber die FreeRTOS-Laufzeitstatistik war i2s2bt_task mit 27 %
     * der groesste Einzelposten im System, und Kern 0 lief deshalb mit 98 % in
     * den Interrupt-Watchdog. Der GMF-eigene aud_rate_cvt rechnet polyphasig;
     * das Vorgaengerprojekt hat dieselbe Umsetzung mit linearer Interpolation
     * auf diesem Chip geschafft (My_Audio_converter_60to44_1khz.c).
     */
#if I2S_MODE == 2
    ret = aud_lin_resample_init(&element);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to init linear resampler for I2S");
    ret = esp_gmf_pool_register_element(pool, element, "aud_lin_resample");
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register linear resampler for I2S");
    ESP_LOGI(TAG, "Registered: aud_lin_resample (linear 60k/32 -> 44.1k/16)");
#else
    ESP_LOGI(TAG, "Messfall %d: aud_lin_resample fuer den I2S-Zweig entfaellt", I2S_MODE);
#endif  /* I2S_MODE == 2 */

    /*
     * Zweite Instanz desselben Umsetzers fuer den DATEI-Zweig (16 Bit, Rate aus
     * der Datei).
     *
     * Warum ueberhaupt: GMFs aud_rate_cvt fordert fuer die Wandlung zwischen
     * den Raten-Familien (44100 <-> 48000) eine Koeffizienten-Matrix von
     * 15360 Byte an. Auf dem Modul ohne PSRAM scheitert genau das an der
     * Fragmentierung - gemessen: 82008 Byte frei, 15360 gebraucht, trotzdem
     * kein zusammenhaengender Block:
     *
     *   E ESP_AE_RATE_CVT: Failed to allocate memory for 'coefficients matrix'(15360)
     *   E STREAM_PROC: aud_rate_cvt_file konnte nicht geoeffnet werden
     *
     * Der lineare Umsetzer braucht dafuer ein paar Dutzend Byte. Die Raten
     * INNERHALB einer Familie (z. B. 48000 -> 48000) koennte GMF auch, nur
     * deckt das die Dateien nicht ab.
     */
    const aud_lin_resample_cfg_t lin_file_cfg = {
        .tag      = "aud_lin_resample_file",
        .in_bits  = 16,     /* Decoder-Ausgabe ist 16 Bit */
        .in_rate  = 0,      /* 0 = Rate aus der Toninformation der Datei */
        .out_rate = AUD_LIN_RESAMPLE_OUT_RATE_DEFAULT,
    };
    ret = aud_lin_resample_init_cfg(&lin_file_cfg, &element);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to init linear resampler for file");
    ret = esp_gmf_pool_register_element(pool, element, lin_file_cfg.tag);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register linear resampler for file");
    ESP_LOGI(TAG, "Registered: %s (linear 16 Bit, Rate aus der Datei -> 48k)", lin_file_cfg.tag);

    /*
     * AUFGERAEUMT (0.9.14): aud_shift16 und aud_copier_i2s sind entfallen.
     *
     * aud_shift16 (eigener 32->16-Bit-Shifter) war der Messfall 4. Er ist auf
     * Hardware abgestuerzt - der nachfolgende GMF-Ratenwandler bekommt von ihm
     * keinen Port-Payload und prueft das nicht (esp_gmf_rate_cvt.c:104 greift
     * ohne NULL-Test zu). Der Messfall ist gesperrt, das Element damit tot.
     * Der belastbare Weg ist Messfall 3: GMFs aud_bit_cvt_i2s vor der
     * Ratenwandlung.
     *
     * aud_copier_i2s war der Versuch, die I2S-Kette mit einem Copy-Element
     * ANZUFUEHREN, damit die Toninformation weiterlaeuft. Seit die Kette in
     * Messfall 2 nur noch aus aud_lin_resample besteht (Kopf und Ende zugleich)
     * bzw. in Messfall 1/3 mit einem GMF-Wandler beginnt, wird der Copier in
     * keiner Kette mehr benutzt.
     *
     * Beide belegten Pool-Speicher, ohne je zu laufen - auf einem Modul ohne
     * PSRAM ist das der knappste Rohstoff.
     */

    /*
     * Und eigene fuer den Datei-Zubringer. Beide Zubringer muessen dem Mischer
     * dasselbe Format liefern (44,1 kHz, 16 Bit, stereo), also wandelt jeder
     * fuer sich - mit eigenen Elementinstanzen.
     */
    /*
     * ENTFALLEN (0.9.29): aud_rate_cvt_file und aud_bit_cvt_file.
     *
     * Der Datei-Zweig macht Rate und Bittiefe seit 0.9.25 mit dem eigenen
     * linearen Umsetzer (aud_lin_resample_file). Die beiden GMF-Wandler wurden
     * trotzdem mitregistriert und belegten Speicher, ohne je zu laufen.
     */

    /*
     * ENTFALLEN (0.9.33): aud_ch_cvt_file.
     *
     * aud_lin_resample_file gibt seit 0.9.33 IMMER stereo aus und kopiert eine
     * Mono-Quelle selbst auf beide Kanaele. Der Kanalwandler waere damit nur
     * noch ein Durchlaeufer - und jedes Element im Zweig ist eine Stelle, die
     * Zustand halten und den Ton verschieben kann (genau das war am 03.10. der
     * Fall: eine Mono-WAV klang verzerrt).
     */

    /*
     * Ein Encoder fuer den Mischer-Ausgang. Er bekommt in i2s2bt_set_stream()
     * die ausgehandelten A2DP-Parameter.
     */
    esp_audio_enc_config_t enc_cfg_mix = DEFAULT_ESP_GMF_AUDIO_ENC_CONFIG();
    ret = esp_gmf_audio_enc_init(&enc_cfg_mix, &element);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to init mixer encoder");
    ret = esp_gmf_pool_register_element(pool, element, "aud_enc_mix");
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register mixer encoder");
    ESP_LOGD(TAG, "Registered: aud_enc_mix");

    /*
     * Mischer fuer die Zusammenfuehrung von I2S- und Datei-Zweig.
     *
     * Der Pool registriert ihn nicht von selbst - die Beispiele holen ihn ueber
     * gmf_loader_setup_audio_effects_default(), das wir als Abhaengigkeit nicht
     * haben. Ohne diese Registrierung scheitert der Pipeline-Aufbau mit
     * ESP_GMF_ERR_NOT_FOUND (-8202):
     *
     *   E STREAM_PROC: Mischer-Pipeline konnte nicht erstellt werden: -8202
     */
    esp_ae_mixer_cfg_t mixer_cfg = DEFAULT_ESP_GMF_MIXER_CONFIG();
    ret = esp_gmf_mixer_init(&mixer_cfg, &element);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to init mixer");
    ret = esp_gmf_pool_register_element(pool, element, NULL);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register mixer");
    ESP_LOGI(TAG, "Registered: aud_mixer");

    // ========== Register GMF IO Types ==========
    // Note: there is deliberately no io_codec_dev endpoint here. It would
    // require a configured esp_codec_dev device, and the board manager that
    // provided one is gone from this project. Our endpoints are the SD card
    // (io_file) and the Bluetooth stream (io_bt).

    // 1. File IO - Reader
    /*
     * Datei-Cache (0.9.37).
     *
     * FILE_IO_CFG_DEFAULT() setzt cache_size = 0 - dann geht JEDER Lesevorgang
     * direkt auf die SD-Karte. Der Decoder liest in 512-Byte-Schritten; bei
     * einer WAV sind das 187 Lesevorgaenge je Sekunde, und der Datei-Zweig
     * schaffte damit nur 163766 Byte/s statt der noetigen 192000 (gemessen
     * 0.9.36). Folge: der Ringpuffer lief leer, der Mischer fuellte die
     * fehlenden Bytes mit Nullen - hoerbar als stotternder Sinus.
     *
     * Laut Header gilt: "Larger cache size will improve read and write
     * performance but consume more memory".
     */
    file_io_cfg_t file_rx_cfg = FILE_IO_CFG_DEFAULT();
    file_rx_cfg.cache_size = 4096;
    file_rx_cfg.dir = ESP_GMF_IO_DIR_READER;
    ret = esp_gmf_io_file_init(&file_rx_cfg, &io);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to init file reader");
    ret = esp_gmf_pool_register_io(pool, io, NULL);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register file reader");
    ESP_LOGD(TAG, "Registered: io_file (reader)");

    // 2. File IO - Writer
    file_io_cfg_t file_tx_cfg = FILE_IO_CFG_DEFAULT();
    file_tx_cfg.cache_size = 4096;
    file_tx_cfg.dir = ESP_GMF_IO_DIR_WRITER;
    ret = esp_gmf_io_file_init(&file_tx_cfg, &io);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to init file writer");
    ret = esp_gmf_pool_register_io(pool, io, NULL);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register file writer");
    ESP_LOGD(TAG, "Registered: io_file (writer)");

    // 3. Bluetooth IO - Reader and Writer
    bt_io_cfg_t bt_rx_cfg = ESP_GMF_BT_IO_CFG_DEFAULT();
    bt_rx_cfg.dir = ESP_GMF_IO_DIR_READER;
    bt_rx_cfg.stream = NULL;
    ret = esp_gmf_io_bt_init(&bt_rx_cfg, &io);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to init BT reader");
    ret = esp_gmf_pool_register_io(pool, io, NULL);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register BT reader");
    ESP_LOGD(TAG, "Registered: io_bt (reader)");

    bt_io_cfg_t bt_tx_cfg = ESP_GMF_BT_IO_CFG_DEFAULT();
    bt_tx_cfg.dir = ESP_GMF_IO_DIR_WRITER;
    bt_tx_cfg.stream = NULL;
    ret = esp_gmf_io_bt_init(&bt_tx_cfg, &io);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to init BT writer");
    ret = esp_gmf_pool_register_io(pool, io, NULL);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register BT writer");
    ESP_LOGD(TAG, "Registered: io_bt (writer)");

    /*
     * 4. I2S input from the Vampire (reader).
     *
     * Failure here is not fatal: the SD and Bluetooth paths keep working without
     * the I2S source, and a missing I2S input should not stop the board from
     * booting.
     */
    ret = i2s_input_create(&io);
    if (ret == ESP_OK) {
        ret = esp_gmf_pool_register_io(pool, io, NULL);
        ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register I2S input");
        ESP_LOGD(TAG, "Registered: io_i2s (reader)");
    } else {
        ESP_LOGW(TAG, "I2S input not available: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "GMF pool initialization completed successfully");

    return ESP_GMF_ERR_OK;
}
