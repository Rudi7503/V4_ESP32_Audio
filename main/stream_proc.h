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

/* ------------------------------------------------------------------ */
/* Zugriffe fuer das I2C-Protokoll der Vampire (0.9.57)                */
/* ------------------------------------------------------------------ */

/**
 * @brief  Datei-Zweig anhalten
 *
 * Setzt nur den Wunsch; ausgefuehrt wird er in der stream_proc-Aufgabe. Der
 * I2S-Eingang der Vampire laeuft weiter - der Mischer traegt beide Quellen,
 * ein Stop betrifft nur die Datei.
 */
void local2bt_stop(void);

/**
 * @brief  Laeuft der Datei-Zweig gerade?
 *
 * Liest den eigenen Merker des Projekts, NICHT esp_gmf_pipeline_t::state -
 * dieses Feld hat sich als unzuverlaessig erwiesen (siehe local2bt_play in
 * stream_proc.c).
 */
bool local2bt_is_playing(void);

/** @brief URI der laufenden Datei, oder "" wenn keine laeuft. */
const char *local2bt_current_uri(void);

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
 * @brief Autostart der Uebertragung ein-/ausschalten (0.9.68).
 *
 * Vorgabe an. Ist er an, sendet der ESP32 den I2S-Eingang der Vampire von
 * selbst, sobald eine Gegenstelle verbunden ist - ohne ihn bleibt die Vampire
 * stumm, bis der erste Titel laeuft. 'stop_media' auf der Konsole schaltet ihn
 * ab, 'start_media' wieder ein; eine neue Verbindung schaltet ihn ebenfalls
 * wieder ein.
 */
void stream_proc_set_media_autostart(bool on);

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
 * Bandzahl des Equalizers hinter dem Mischer (0.9.56).
 *
 * Fest 10 - die Baender werden zur Laufzeit einzeln zu- und abgeschaltet
 * ("eq bands N"), damit die CPU-Kosten je Band messbar sind. Der EQ passt sich
 * der Abtastrate des Streams selbst an (esp_gmf_eq.c: eq_received_event_handler
 * setzt need_reopen und oeffnet den EQ mit der neuen Rate neu).
 */
#define MIXER_EQ_BANDS   10

/**
 * @brief  Erste N Baender des Equalizers aktivieren, den Rest abschalten
 *
 * @param[in]  n  0 = alles aus (Durchlauf), 1..MIXER_EQ_BANDS
 *
 * @return   die gesetzte Bandzahl oder -1 (EQ nicht gefunden / nicht bereit)
 */
int stream_proc_eq_set_bands(int n);

/**
 * @brief  Ein Band des Equalizers einstellen
 *
 * @param[in]  idx   Bandindex 0..MIXER_EQ_BANDS-1
 * @param[in]  typ   1 = High-Pass, 2 = Low-Pass, 3 = Peak, 4 = High-Shelf, 5 = Low-Shelf
 * @param[in]  fc    Mitten-/Grenzfrequenz in Hz
 * @param[in]  q     Guete (0,1 .. 20)
 * @param[in]  gain  Verstaerkung in dB (-15 .. 15, nur Shelf/Peak)
 *
 * @return   0 bei Erfolg, -1 sonst
 */
int stream_proc_eq_set(int idx, int typ, unsigned fc, float q, float gain);

/**
 * @brief  Alle Baender des Equalizers ausgeben (Typ, fc, Q, Gain, aktiv)
 */
void stream_proc_eq_list(void);

