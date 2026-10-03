/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include "esp_gmf_io.h"

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

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

/**
 * @brief  Start a task that logs the I2S throughput once per second
 *
 * Expected value at 60 kHz / 32 bit / stereo is 480000 bytes per second. A
 * reading of zero means the Vampire is not sending (or the wiring/pins are
 * wrong), which is the first thing to check.
 */
void i2s_input_start_monitor(void);

#ifdef __cplusplus
}
#endif  /* __cplusplus */
