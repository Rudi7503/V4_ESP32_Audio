/*
 * audio_source.h - which signal feeds the A2DP source.
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * The ESP32 has two possible audio sources for the Bluetooth sink:
 *
 *   AUDIO_SOURCE_I2S - the Vampire V4 clocks audio into the ESP32 as an I2S
 *                      slave, through the two converter elements. This is the
 *                      normal mode and the default at boot.
 *
 *   AUDIO_SOURCE_SD  - a file on the SD card is decoded and streamed instead.
 *                      Selected with the V4 protocol's PLAY_FILE command, so the
 *                      master can let the ESP32 play something on its own.
 *
 * Switching means rebuilding the pipeline's element chain. The implementation
 * lives in play_bt_source_example.c because that file owns the pipeline; it is
 * declared here so v4_link.c can drive it.
 */

#ifndef AUDIO_SOURCE_H
#define AUDIO_SOURCE_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_SOURCE_I2S = 0,
    AUDIO_SOURCE_SD  = 1,
} audio_source_t;

/**
 * @brief Make the SD card the audio source and start playing a file.
 *
 * The ADF auto decoder picks the format from the stream itself, so MP3, WAV,
 * FLAC, OGG, OPUS, AAC/M4A and raw PCM all work without the master having to
 * say which one it is.
 *
 * Blocks while the pipeline is rebuilt (tens of milliseconds), so the protocol
 * layer answers BUSY first and runs this afterwards.
 *
 * @param vfs_path absolute path as produced by sd_fs_resolve_path(), e.g.
 *                 "/sdcard/MUSIC/track.mp3"
 */
esp_err_t audio_source_play_sd(const char *vfs_path);

/** @brief Go back to the I2S slave input. Safe to call when already on I2S. */
esp_err_t audio_source_use_i2s(void);

/** @brief Which source is currently feeding the A2DP writer. */
audio_source_t audio_source_get(void);

/** @brief Path of the file being played, or NULL/empty when on I2S. */
const char *audio_source_current_path(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_SOURCE_H */
