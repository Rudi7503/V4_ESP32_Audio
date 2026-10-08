/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include "esp_gmf_io.h"

/*
 * Groessen des I2S-Eingangsdatenbusses (0.9.63).
 *
 * Bis 0.9.62 stand in stream_proc.c/log_buffer_sizes() fest "12 * 1024" - die
 * Senkung auf 6 KB aus 0.9.31 war dort nie nachgezogen und hat die Fehlersuche
 * um den MP3-Startfehler in die Irre gefuehrt. Jetzt gibt es die Werte einmal,
 * und der Bericht liest sie hier.
 *
 * ACHTUNG: ausserhalb des extern-"C"-Blocks, sonst sieht der C-Compiler sie
 * nicht (dieser Fehler ist beim Bau von 0.9.63 passiert).
 */
#define I2S_INPUT_DB_BYTES      6144    /* Datenbus (esp_gmf_io buffer_size) */
#define I2S_INPUT_READ_BYTES    2048    /* ein Lesevorgang daraus (io_size)  */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Create the I2S input that receives the Vampire's audio
 *
 * The Vampire is the I2S master and drives BCLK and WS; the ESP32 is the slave.
 * Wiring (from the schematic notes):
 *
 *     ESP32 GPIO25 -> Vampire P20.6   WSEL / word select
 *     ESP32 GPIO5  -> Vampire P20.8   BCLK
 *     ESP32 GPIO35 <- Vampire P20.10  DIN
 *
 * GPIO35 is input-only, which is what a data input needs.
 *
 * Format: taken over unchanged from the old project that worked with this
 * hardware - 60 kHz, 32 bit per slot, stereo. The conversion to the 44.1 kHz /
 * 16 bit that the SBC encoder needs happens in GMF (see the asrc element).
 *
 * @param[out]  io  Receives the GMF IO handle, to be registered with
 *                  esp_gmf_pool_register_io()
 *
 * @return
 *       - ESP_OK    IO created
 *       - ESP_FAIL  I2S channel or IO could not be created
 */
esp_err_t i2s_input_create(esp_gmf_io_handle_t *io);

#ifdef __cplusplus
}
#endif  /* __cplusplus */
