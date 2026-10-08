/*
 * sd_fs.h - SD card access for the V4 protocol.
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Hardware: microSD socket wired for 1-bit SDMMC (the ESP32 slot-1 IOMUX pins),
 * taken from the ESP32-LyraT V4.3 / WROVER-KIT layout:
 *
 *     CLK  = GPIO14
 *     CMD  = GPIO15
 *     D0   = GPIO2
 *
 * with 10k pull-ups on CMD/D0 (required during the SD init phase) and a pull-up
 * on CLK which is harmless because the host drives CLK push-pull. There is no
 * card-detect line in use, so mounting is on demand via sd_fs_mount().
 *
 * Paths passed in are relative to the mount point: "" or "/" is the root,
 * "/MUSIC" or "MUSIC" a subdirectory. ".." components are rejected.
 *
 * NOTE on attributes: the listing is produced through the POSIX VFS, which does
 * not expose raw FAT attribute bytes. The reported attribute byte is therefore
 * best effort: V4P_ATTR_DIR for directories, otherwise V4P_ATTR_ARCHIVE, plus
 * V4P_ATTR_RDONLY when the entry is not writable.
 */

#ifndef SD_FS_H
#define SD_FS_H

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#include "v4_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SD_FS_MOUNT_POINT       "/sdcard"
#define SD_FS_MAX_DIR_HANDLES   2
#define SD_FS_MAX_FILE_HANDLES  4

/** Buffer size needed for a resolved absolute VFS path. */
#define SD_FS_PATH_BUF          (sizeof(SD_FS_MOUNT_POINT) + V4P_PATH_MAX + 2)

/*
 * Conservative bus clock. The card is a 1-bit SDMMC link with 10k pull-ups;
 * 20 MHz is what the LyraT board uses, drop to 10000 if you ever see CRC errors
 * during long directory walks.
 */
#define SD_FS_MAX_FREQ_KHZ      20000

typedef enum {
    SD_FS_OK = 0,
    SD_FS_ERR_NOT_MOUNTED,
    SD_FS_ERR_NOT_FOUND,
    SD_FS_ERR_NO_HANDLE,
    SD_FS_ERR_IO,
    SD_FS_ERR_END,
    SD_FS_ERR_ARG,
} sd_fs_err_t;

/** @brief Mount the card if it is not mounted yet. Idempotent. */
esp_err_t sd_fs_mount(void);

/** @brief Unmount and forget the card. */
esp_err_t sd_fs_unmount(void);

bool sd_fs_is_mounted(void);

/**
 * @brief Resolve a master supplied path into the absolute VFS path.
 *
 * Same normalisation as the file/directory commands: "" and "/" mean the mount
 * point, leading slashes are collapsed, and any ".." is rejected.
 *
 * @param out  buffer of at least SD_FS_PATH_BUF bytes
 */
sd_fs_err_t sd_fs_resolve_path(const char *path, char *out, size_t out_len);

/**
 * @brief Read the socket's card-detect switch (GPIO34, low = card inserted).
 *
 * NOTE: GPIO34 is input-only and has no internal pull-up, and the driver does
 * not enable one. With a card inserted the switch gives a hard low, so a true
 * return is reliable. Without a card the pin floats unless an external 10k
 * pull-up to 3.3 V is fitted, so a false return may just mean "floating".
 */
bool sd_fs_card_present(void);

/**
 * @brief Capacity information.
 *
 * @param total_kb    [out] may be NULL
 * @param free_kb     [out] may be NULL
 * @param sector_size [out] may be NULL
 * @param fat_type    [out] may be NULL, currently always 0 (not exposed by the
 *                    IDF VFS layer)
 */
sd_fs_err_t sd_fs_info(uint32_t *total_kb, uint32_t *free_kb,
                       uint16_t *sector_size, uint8_t *fat_type);

/* ------------------------------------------------------------------ */
/* Directory iteration                                                */
/* ------------------------------------------------------------------ */

/**
 * @brief Open a directory for iteration.
 *
 * @return handle >= 0, or a negative sd_fs_err_t value.
 */
int sd_fs_dir_open(const char *path);

/**
 * @brief Read the next entry.
 *
 * @param handle    handle from sd_fs_dir_open()
 * @param name_out  buffer for the name
 * @param name_max  size of name_out, names are truncated to name_max - 1
 * @param is_dir    [out] true for directories
 * @param size      [out] file size in bytes, 0 for directories
 * @param attr      [out] best-effort attribute byte, see the note above
 * @param truncated [out] true if the name did not fit
 *
 * @return SD_FS_OK, SD_FS_ERR_END when the listing is finished, or an error.
 */
sd_fs_err_t sd_fs_dir_next(int handle, char *name_out, size_t name_max,
                           bool *is_dir, uint32_t *size, uint8_t *attr,
                           bool *truncated);

void sd_fs_dir_close(int handle);

/* ------------------------------------------------------------------ */
/* File access                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief Open a file for reading.
 *
 * @return handle >= 0, or a negative sd_fs_err_t value.
 */
int sd_fs_file_open(const char *path, uint32_t *size_out);

/**
 * @brief Random access read. The file is seeked, so the caller only supplies the
 *        absolute offset - there is no hidden cursor to keep in sync.
 */
sd_fs_err_t sd_fs_file_read(int handle, uint32_t offset,
                            uint8_t *buf, uint16_t len, uint16_t *read_out);

void sd_fs_file_close(int handle);

/** @brief Size of an open file, or 0. */
uint32_t sd_fs_file_size(int handle);

#ifdef __cplusplus
}
#endif

#endif /* SD_FS_H */
