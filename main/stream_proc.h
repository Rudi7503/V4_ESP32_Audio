/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include "esp_gmf_pool.h"
#include "esp_bt_audio_stream.h"

/**
 * @brief  Structure for stream user data
 */
typedef struct {
    esp_gmf_pipeline_handle_t  pipe;  /*!< Pipeline handle */
} stream_user_data_t;

/**
 * @brief  Initialize the stream processor
 *
 * @param[in]  pool  Pool handle
 *
 * @return
 *       - void
 */
void stream_proc_init(esp_gmf_pool_handle_t pool);

/**
 * @brief  State change callback for the stream processor
 *
 * @param[in]  stream  Stream handle
 * @param[in]  state   Stream state
 *
 * @return
 *       - void
 */
void stream_proc_state_chg(esp_bt_audio_stream_handle_t stream, esp_bt_audio_stream_state_t state);

/**
 * @brief  Play the next track
 *
 * @return
 *       - void
 */
void local2bt_play_next(void);

/**
 * @brief  Play the previous track
 *
 * @return
 *       - void
 */
void local2bt_play_prev(void);

/**
 * @brief  Play one file as a feeder branch into the mixer
 *
 * @param[in]  uri  Path or GMF uri ("/sdcard/test.mp3" or "file://sdcard/test.mp3")
 *
 * @return
 *       - void
 */
void local2bt_play_file(const char *uri);

/**
 * @brief  Ask for the I2S input (Vampire) to be sent to Bluetooth
 *
 * Sets the request flag; it is picked up when the next A2DP source stream is
 * allocated. Call after connect, then start_media.
 *
 * @return
 *       - void
 */
void i2s2bt_request(void);

/**
 * @brief  Stop the I2S pipeline if it is running
 */
void i2s2bt_stop(void);

/**
 * @brief  Whether the I2S pipeline could be created (io_i2s present)
 */
bool i2s2bt_is_ready(void);


/**
 * @brief  Wartezeiten des Mischers setzen (Laufzeit, kein Neustart noetig)
 *
 * prefill_ms: Wartezeit zwischen Start der Zubringer und Start des Mischers.
 *             Bestimmt, wie viel Vorlauf im Ringpuffer liegt, bevor der
 *             Mischer zieht.
 * transit_ms: transit_time je Mischer-Quelle (esp_ae_mixer_info_t).
 *
 * Beide wirken beim NAECHSTEN Stream-Aufbau (start_media). Ein negativer Wert
 * laesst den jeweiligen Wert unveraendert.
 */
void i2s2bt_set_mixer_wait(int prefill_ms, int transit_ms);

/**
 * @brief  Aktuelle Wartezeiten lesen (NULL = nicht abfragen)
 */
void i2s2bt_get_mixer_wait(int *prefill_ms, int *transit_ms);

/**
 * @brief  Pufferstatistik der Zubringer-Ringpuffer lesen oder neu starten
 *
 * "Minimum" und "leer N mal" laufen sonst seit dem Einschalten weiter und waeren
 * ueber mehrere Messfaelle hinweg nicht vergleichbar. Vor jedem Fall einmal
 * zuruecksetzen.
 *
 * Der Unterlauf-Fall ist: min_* == 0 und empty_* > 0.
 *
 * @param[in]   reset       true = Zaehler neu starten (die uebrigen Parameter
 *                          werden dann nicht beschrieben)
 * @param[out]  min_i2s     kleinster Fuellstand des I2S-Zweigs (-1 = noch nichts gemessen)
 * @param[out]  empty_i2s   Anzahl Sekunden mit leerem I2S-Puffer
 * @param[out]  min_file    kleinster Fuellstand des Datei-Zweigs (-1 = nichts gemessen)
 * @param[out]  empty_file  Anzahl Sekunden mit leerem Datei-Puffer
 */
void stream_proc_buffer_stats(bool reset, int *min_i2s, int *empty_i2s,
                              int *min_file, int *empty_file);
