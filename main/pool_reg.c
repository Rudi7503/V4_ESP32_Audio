/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * pool_reg.c - GMF-Pool aufbauen.
 *
 * Der Pool enthaelt nur, was die beiden Ketten wirklich benutzen:
 *
 *   Datei-Zweig : io_file -> aud_dec -> aud_lin_resample_file
 *   I2S-Zweig   : io_i2s  -> aud_lin_resample
 *   Mischer     : aud_mixer -> aud_enc_mix -> io_bt
 *
 * Alles andere (aud_aec, aud_asrc, die GMF-Raten-/Bit-/Kanalwandler, die
 * Codec-Pipelines des Beispiels) ist entfallen. Auf einem Modul ohne PSRAM ist
 * interner Speicher der knappste Rohstoff - jedes nicht registrierte Element
 * spart Objekt und Puffer.
 */

#include "esp_gmf_pool.h"
#include "esp_gmf_err.h"
#include "esp_log.h"

// GMF-Audioelemente
#include "esp_audio_enc_default.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_gmf_audio_dec.h"
#include "esp_gmf_audio_enc.h"
#include "esp_gmf_mixer.h"

// GMF-IO-Typen
#include "esp_gmf_io_file.h"
#include "esp_gmf_io_bt.h"

#include "i2s_input.h"
#include "linear_resample.h"

static const char *TAG = "POOL_INIT";

/* Ausgangsrate des Mischers; wird beim Stream-Start auf die A2DP-Rate gesetzt. */
#define MIXER_RATE_HZ   48000

esp_gmf_err_t pool_reg(esp_gmf_pool_handle_t pool)
{
    ESP_GMF_NULL_CHECK(TAG, pool, return ESP_GMF_ERR_INVALID_ARG);
    ESP_LOGI(TAG, "Registering GMF pool");

    esp_gmf_err_t ret = ESP_GMF_ERR_OK;
    esp_gmf_element_handle_t element = NULL;
    esp_gmf_io_handle_t io = NULL;

    /* Decoder/Encoder beim Codec-Modul anmelden (MP3, WAV, SBC). */
    ret = esp_audio_dec_register_default();
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register audio decoders");
    ret = esp_audio_simple_dec_register_default();
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register audio simple decoders");
    ret = esp_audio_enc_register_default();
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register audio encoders");

    /* Datei-Zweig: Decoder (Kopf) */
    esp_audio_simple_dec_cfg_t dec_cfg = DEFAULT_ESP_GMF_AUDIO_DEC_CONFIG();
    ret = esp_gmf_audio_dec_init(&dec_cfg, &element);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to init audio decoder");
    ret = esp_gmf_pool_register_element(pool, element, NULL);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register audio decoder");
    ESP_LOGD(TAG, "Registered: aud_dec");

    /*
     * Eigener linearer Umsetzer - zweimal: einmal fuer den I2S-Eingang
     * (32 Bit, feste 60000 Hz) und einmal fuer den Datei-Zweig (16 Bit, Rate
     * aus der Toninformation der Datei).
     *
     * Warum nicht GMFs aud_rate_cvt: der fordert fuer die Wandlung zwischen den
     * Raten-Familien (44100 <-> 48000) eine Koeffizienten-Matrix von 15360 Byte
     * am Stueck an. Ohne PSRAM scheitert das an der Zersplitterung.
     *
     * Ein Element kann nur an EINEN Task gebunden werden, deshalb zwei
     * Instanzen.
     */
    const aud_lin_resample_cfg_t lin_i2s_cfg = {
        .tag      = "aud_lin_resample",
        .in_bits  = 32,     /* obere 16 Bit der I2S-Woerter */
        .in_rate  = 60000,  /* fest, die Vampire ist Taktgeber */
        .out_rate = MIXER_RATE_HZ,
    };
    ret = aud_lin_resample_init_cfg(&lin_i2s_cfg, &element);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to init linear resampler for I2S");
    ret = esp_gmf_pool_register_element(pool, element, lin_i2s_cfg.tag);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register linear resampler for I2S");
    ESP_LOGI(TAG, "Registered: %s (32 Bit, 60000 Hz -> %d Hz)", lin_i2s_cfg.tag, MIXER_RATE_HZ);

    const aud_lin_resample_cfg_t lin_file_cfg = {
        .tag      = "aud_lin_resample_file",
        .in_bits  = 16,     /* Decoder-Ausgabe */
        .in_rate  = 0,      /* 0 = Rate aus der Toninformation der Quelle */
        .out_rate = MIXER_RATE_HZ,
    };
    ret = aud_lin_resample_init_cfg(&lin_file_cfg, &element);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to init linear resampler for file");
    ret = esp_gmf_pool_register_element(pool, element, lin_file_cfg.tag);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register linear resampler for file");
    ESP_LOGI(TAG, "Registered: %s (16 Bit, Rate aus der Datei)", lin_file_cfg.tag);

    /* Mischer fuer die Zusammenfuehrung beider Zubringer. */
    esp_ae_mixer_cfg_t mixer_cfg = DEFAULT_ESP_GMF_MIXER_CONFIG();
    ret = esp_gmf_mixer_init(&mixer_cfg, &element);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to init mixer");
    ret = esp_gmf_pool_register_element(pool, element, NULL);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register mixer");
    ESP_LOGI(TAG, "Registered: aud_mixer");

    /* Encoder fuer den Mischerausgang - Parameter kommen in i2s2bt_set_stream(). */
    esp_audio_enc_config_t enc_cfg = DEFAULT_ESP_GMF_AUDIO_ENC_CONFIG();
    ret = esp_gmf_audio_enc_init(&enc_cfg, &element);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to init mixer encoder");
    ret = esp_gmf_pool_register_element(pool, element, "aud_enc_mix");
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register mixer encoder");
    ESP_LOGD(TAG, "Registered: aud_enc_mix");

    /* Datei-IO als Leser (SD-Karte). 4 KB Cache: ohne Cache geht jeder
     * 512-Byte-Lesevorgang direkt auf die Karte und der Datei-Zweig schafft die
     * Echtzeit nicht (siehe docs/MESSREIHE.md). */
    file_io_cfg_t file_cfg = FILE_IO_CFG_DEFAULT();
    file_cfg.cache_size = 4096;
    file_cfg.dir = ESP_GMF_IO_DIR_READER;
    ret = esp_gmf_io_file_init(&file_cfg, &io);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to init file reader");
    ret = esp_gmf_pool_register_io(pool, io, NULL);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register file reader");
    ESP_LOGD(TAG, "Registered: io_file (reader)");

    /* Bluetooth-IO als Schreiber (A2DP-Ausgang). */
    bt_io_cfg_t bt_cfg = ESP_GMF_BT_IO_CFG_DEFAULT();
    bt_cfg.dir = ESP_GMF_IO_DIR_WRITER;
    bt_cfg.stream = NULL;
    ret = esp_gmf_io_bt_init(&bt_cfg, &io);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to init BT writer");
    ret = esp_gmf_pool_register_io(pool, io, NULL);
    ESP_GMF_RET_ON_ERROR(TAG, ret, return ret, "Failed to register BT writer");
    ESP_LOGD(TAG, "Registered: io_bt (writer)");

    /* I2S-Eingang der Vampire. Fehlt er, laeuft der Rest trotzdem. */
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
