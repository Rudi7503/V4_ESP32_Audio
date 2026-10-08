/*
 * v4_link.h - I2C slave side of the V4 protocol.
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Uses the I2C slave driver v2 - in ESP-IDF 6.1 that is the ONLY slave driver
 * (the old v1 API and the switch CONFIG_I2C_ENABLE_SLAVE_DRIVER_VERSION_2 are
 * gone; the line still standing in the predecessor project's sdkconfig is a
 * leftover and harmless). The v2 API is what makes a sane protocol possible:
 *
 *  - the receive callback reports the actual received length, so a truncated
 *    master write is detectable instead of being padded silently;
 *  - i2c_slave_write() resets the TX hardware FIFO on ESP32 when it holds dirty
 *    data (see i2c_slave_v2.c, the !SOC_I2C_SLAVE_CAN_GET_STRETCH_CAUSE branch),
 *    so leftover bytes from a short master read cannot desync the next frame.
 *
 * The task keeps the invariant "one command write -> exactly one response read".
 * Slow work (SD mounts, SD block reads) is never done before replying: the
 * command returns BUSY, the work runs afterwards, and the master re-sends the
 * command. That keeps every response time bounded to a memory copy, which is
 * what makes a fixed master-side wait safe on a slave without clock stretching.
 */

#ifndef V4_LINK_H
#define V4_LINK_H

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "driver/gpio.h"

#include "v4_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* I2C wiring to the Vampire V4, matching PIN verbindungen.txt */
#define V4_I2C_PORT             I2C_NUM_0
#define V4_I2C_SCL_IO           GPIO_NUM_23
#define V4_I2C_SDA_IO           GPIO_NUM_18
#define V4_I2C_ADDR             0x50

/* Must be >= V4P_BULK_FRAME_MAX so a full chunk fits into the TX ring buffer. */
#define V4_TX_BUF_DEPTH         2048
/* The master writes 32 byte commands; a little headroom for the queues. */
#define V4_RX_BUF_DEPTH         64
#define V4_RX_QUEUE_LEN         4
#define V4_WORK_QUEUE_LEN       4
#define V4_TASK_STACK           6144
#define V4_TASK_PRIO            6

/**
 * @brief Create the I2C slave device and start the protocol task.
 *
 * Requires bt_mgr_init() and the ADF bluetooth service to be initialised first,
 * because the command handlers reach into both.
 */
esp_err_t v4_link_init(void);

/** @brief Current FILE_READ chunk size in bytes. */
uint16_t v4_link_chunk_size(void);

/**
 * @brief Drive the command dispatch with synthetic frames - no I2C master needed.
 *
 * Exercises the real SD card and Bluetooth state through exactly the code path
 * the V4 will use (frame build -> validation -> dispatch -> response frame ->
 * validation, including the BUSY round trip) and logs the outcome. Intended as
 * a hardware bring-up aid before the master software exists.
 */
void v4_link_selftest(void);

/**
 * @brief Log the I2C bus state and how many frames have been received.
 *
 * Separates "the master never addressed us" (wiring, address, no master) from
 * "frames arrive but are rejected" (timing, frame format). Those two need
 * opposite fixes, and the log is otherwise silent for both.
 */
void v4_link_bus_report(void);

#ifdef __cplusplus
}
#endif

#endif /* V4_LINK_H */
