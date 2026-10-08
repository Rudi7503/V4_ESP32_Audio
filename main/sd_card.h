/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

/**
 * @brief  Mount the microSD card at /sdcard
 *
 * Try 1: 1-bit SDMMC on the fixed ESP32 slot-1 pins (CLK=GPIO14, CMD=GPIO15,
 * D0=GPIO2), card detect on GPIO34 (low = card inserted).
 *
 * Try 2: if SDMMC fails, the same three wires are used as SPI (CLK, MOSI=CMD,
 * MISO=D0) with the card-detect line doubling as chip select - no rewiring.
 * Reason: the replacement module's SDMMC block times out on its very first
 * clock-update command (0x107), before any data transfer, while holder, card
 * and wiring are unchanged and worked with the previous module.
 *
 * @return
 *       - ESP_OK     Card mounted (either transport)
 *       - ESP_FAIL   No card, or the filesystem could not be mounted
 */
esp_err_t sd_card_mount(void);

/**
 * @brief  Which transport the mounted card uses ("SDMMC 1 Bit" or "SPI")
 */
const char *sd_card_transport(void);

/**
 * @brief  Sector size of the mounted card in bytes (0 when nothing is mounted)
 *
 * Das I2C-Protokoll der Vampire meldet die Sektorgroesse in SD_INFO.
 */
uint16_t sd_card_sector_size(void);

/**
 * @brief  Unmount the card if it is mounted
 *
 * @return
 *       - ESP_OK     Unmounted (or nothing to do)
 */
esp_err_t sd_card_unmount(void);

/**
 * @brief  Whether a card is currently mounted
 */
bool sd_card_is_mounted(void);

/**
 * @brief  Raw state of the card-detect switch (true = card inserted)
 *
 * Note: GPIO34 has no internal pull-up, so without the external 10k resistor
 * only the "present" answer can be trusted.
 */
bool sd_card_is_present(void);

#ifdef __cplusplus
}
#endif  /* __cplusplus */
