/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * linear_resample.h - billiger 60 kHz/32 Bit -> 44,1 kHz/16 Bit Umsetzer.
 *
 * Ersetzt im I2S-Zweig die Kette aud_rate_cvt_i2s + aud_bit_cvt_i2s. Die
 * Rechnung stammt aus dem Vorgaengerprojekt
 * (ESP32-I2S-to-BT/main/My_Audio_converter_60to44_1khz.c) und ist dort auf
 * demselben Chip gelaufen.
 */

#pragma once

/*
 * Groesse, die der Ausgangspuffer des Wandlers IMMER bekommt (0.9.75).
 *
 * Der Wert stand bisher nur in linear_resample.c; stream_proc.c braucht ihn
 * aber, um den Puffer beim Pipelineaufbau vorab zu reservieren (solange der
 * DRAM zusammenhaengend ist, siehe reserve_output_payload).
 *
 * Herleitung: 1152 Frames x 48000/44100 x 2 Kanaele x 2 Byte = 5016, auf die
 * naechsten 1024 aufgerundet = 5120. Immer dieselbe Groesse, damit der
 * GMF-Port nicht bei jedem Block umbaut.
 */
#define LIN_RESAMPLE_OUT_PAYLOAD_MAX 5120

#include "esp_gmf_element.h"
#include "esp_gmf_err.h"

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

/* Vorgabe der Ausgangsrate. Sie wird beim Stream-Start durch die
 * A2DP-Aushandlung ersetzt (aud_lin_resample_set_out_rate). */
#define AUD_LIN_RESAMPLE_OUT_RATE_DEFAULT   48000

/**
 * @brief  Create the linear 60 kHz -> 44.1 kHz resampler element
 *
 * @param[out]  handle  Receives the element handle (tag: aud_lin_resample)
 *
 * @return
 *       - ESP_GMF_ERR_OK           On success
 *       - ESP_GMF_ERR_MEMORY_LACK  Not enough memory
 */
esp_gmf_err_t aud_lin_resample_init(esp_gmf_element_handle_t *handle);

/**
 * @brief  Konfiguration fuer eine Instanz des linearen Umsetzers
 *
 * Es gibt zwei Einsatzorte mit unterschiedlichen Eingangsformaten:
 *
 *   I2S-Zweig:  32 Bit, obere 16 Bit gueltig (>>16), Rate fest 60000 Hz
 *   Datei-Zweig: 16 Bit direkt (Decoder-Ausgabe), Rate kommt aus der Datei
 *
 * Der Datei-Zweig braucht ihn, weil GMFs aud_rate_cvt fuer die Wandlung
 * zwischen den Raten-Familien (44100 <-> 48000) eine Koeffizienten-Matrix von
 * 15360 Byte anfordert. Im No-PSRAM-Build scheitert das an der Fragmentierung
 * (gemessen: 82008 Byte frei, 15360 gebraucht, trotzdem kein zusammenhaengender
 * Block). Dieser Umsetzer braucht ein paar Dutzend Byte.
 */
typedef struct {
    const char *tag;        /* Elementname in der Pipeline (NULL = "aud_lin_resample") */
    uint8_t     in_bits;    /* 32 = obere 16 Bit nehmen, 16 = direkt uebernehmen */
    uint32_t    in_rate;    /* 0 = aus der Toninformation der Quelle uebernehmen */
    uint32_t    out_rate;   /* Ausgangsrate (wird beim Stream-Start nachgezogen) */
} aud_lin_resample_cfg_t;

/**
 * @brief  Instanz mit eigener Konfiguration anlegen (z. B. fuer den Datei-Zweig)
 */
esp_gmf_err_t aud_lin_resample_init_cfg(const aud_lin_resample_cfg_t *cfg,
                                        esp_gmf_element_handle_t *handle);

/**
 * @brief  Ausgangsrate zur Laufzeit setzen (aus der A2DP-Aushandlung)
 *
 * Der A2DP-Sender handelt die Abtastrate mit der Senke aus (44100 oder 48000);
 * der SBC-Encoder resampelt nicht, er bekommt sie nur gesagt. Diese Funktion
 * setzt das Umsetzverhaeltnis passend, damit die gelieferte Rate zur
 * ausgehandelten passt. Muss VOR dem Start der Pipeline gerufen werden.
 *
 * @param[in]  handle    Element der Pipeline (aus esp_gmf_pipeline_get_el_by_name)
 * @param[in]  out_rate  Ausgangsrate in Hz (z. B. 44100 oder 48000)
 */
esp_gmf_err_t aud_lin_resample_set_out_rate(esp_gmf_element_handle_t handle, uint32_t out_rate);

#ifdef __cplusplus
}
#endif  /* __cplusplus */
