/*
 * sd_fs.c - SD-Zugriff fuer das V4-I2C-Protokoll.
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Uebernommen aus dem Vorgaengerprojekt (ESP32-I2S-to-BT, dort ebenfalls
 * sd_fs.c). Geaendert ist nur der hardwarenahe Teil: Karte, Takt, Kartenkontakt
 * und Mountpunkt gehoeren jetzt sd_card.c. Damit gibt es weiterhin genau EINEN
 * Mountpfad im Projekt - den, den auch der Boot und die lokale Wiedergabe
 * benutzen.
 *
 * Hier bleibt der dateiorientierte Teil (Verzeichnisliste, Datei lesen), weil
 * das I2C-Protokoll der Vampire genau das braucht und sd_card.c das nicht
 * anbietet. Der Zugriff laeuft ueber den POSIX-VFS auf FatFs - dieselbe Quelle
 * wie der Konsolenbefehl "sd_ls" in cmd_reg.c.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_check.h"

#include "esp_vfs_fat.h"

#include "v4_proto.h"
#include "sd_card.h"
#include "sd_fs.h"

static const char *TAG = "sd_fs";

/* mount point plus a path plus margin */
#define SD_FULL_PATH_MAX  (sizeof(SD_FS_MOUNT_POINT) + V4P_PATH_MAX + 2)

typedef struct {
    bool     used;
    DIR     *dir;
    char     path[SD_FULL_PATH_MAX];
} sd_dir_slot_t;

typedef struct {
    bool     used;
    int      fd;
    uint32_t size;
    char     path[SD_FULL_PATH_MAX];
} sd_file_slot_t;

/*
 * KEIN eigenes "gemountet"-Flag: die Karte gehoert sd_card.c, und nur dort wird
 * sie eingebunden - auch beim Start. Ein zweites Flag hier waere nach dem
 * Boot-Mount falsch gewesen (und genau dieser Fehler steckte im ersten Entwurf).
 */
static sd_dir_slot_t    s_dirs[SD_FS_MAX_DIR_HANDLES];
static sd_file_slot_t   s_files[SD_FS_MAX_FILE_HANDLES];

/* Capacity snapshot. Refreshed only in the mount path - see sd_fs_info(). */
static uint32_t         s_total_kb;
static uint32_t         s_free_kb;
static uint16_t         s_sector_size;

/* ------------------------------------------------------------------ */
/* Card detect                                                        */
/* ------------------------------------------------------------------ */

/*
 * Der Kartenkontakt gehoert sd_card.c - dort steht auch die Begruendung, warum
 * ohne externen Pull-up nur die Antwort "Karte steckt" belastbar ist.
 */
bool sd_fs_card_present(void)
{
    return sd_card_is_present();
}

/* ------------------------------------------------------------------ */
/* Path handling                                                      */
/* ------------------------------------------------------------------ */

/**
 * @brief Turn a master supplied path into an absolute VFS path.
 *
 * "" and "/" both map to the mount point. Leading slashes are normalised and any
 * ".." component is rejected so the master cannot climb out of the mount.
 *
 * @return SD_FS_OK or SD_FS_ERR_ARG.
 */
static sd_fs_err_t build_path(const char *in, char *out, size_t out_len)
{
    if (in == NULL || out == NULL) {
        return SD_FS_ERR_ARG;
    }

    /* reject parent traversal */
    if (strstr(in, "..") != NULL) {
        ESP_LOGW(TAG, "rejected path with '..': %s", in);
        return SD_FS_ERR_ARG;
    }

    while (*in == '/') {
        in++;
    }

    int n;
    if (*in == '\0') {
        n = snprintf(out, out_len, "%s", SD_FS_MOUNT_POINT);
    } else {
        n = snprintf(out, out_len, "%s/%s", SD_FS_MOUNT_POINT, in);
    }
    if (n < 0 || (size_t)n >= out_len) {
        return SD_FS_ERR_ARG;
    }
    return SD_FS_OK;
}

sd_fs_err_t sd_fs_resolve_path(const char *path, char *out, size_t out_len)
{
    return build_path(path, out, out_len);
}

/* ------------------------------------------------------------------ */
/* Mount / unmount                                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief Refresh the cached capacity snapshot.
 *
 * Slow by nature: esp_vfs_fat_info() uses f_getfree(), which is O(FAT size).
 * Only ever call this from the mount path, never from a command handler.
 */
static void sd_info_cache_update(void)
{
    uint64_t total = 0, free_b = 0;

    s_total_kb = 0;
    s_free_kb = 0;
    s_sector_size = sd_card_sector_size();

    if (esp_vfs_fat_info(SD_FS_MOUNT_POINT, &total, &free_b) != ESP_OK) {
        ESP_LOGW(TAG, "esp_vfs_fat_info failed; reporting 0 capacity");
        return;
    }
    s_total_kb = (uint32_t)(total / 1024);
    s_free_kb = (uint32_t)(free_b / 1024);
}

/*
 * Mounten macht sd_card.c (erst SDMMC 1 Bit, dann SPI auf denselben Leitungen).
 * Hier wird nur die Kapazitaet gemerkt.
 *
 * Aufgerufen wird das an ZWEI Stellen: einmal beim Start in app_main (damit
 * SD_INFO und GET_STATUS von Anfang an Werte haben) und danach aus dem
 * Arbeitstask, wenn der Master SD_MOUNT(force=1) schickt.
 *
 * LAUFZEIT: sd_card_mount() versucht es bis zu 3 + 3 mal mit je 200 ms Pause.
 * Das ist viel zu lang fuer den heissen Pfad des I2C-Protokolls, in dem die
 * Antwort innerhalb der festen Wartezeit des Masters stehen muss. Der Aufruf
 * aus dem Protokoll laeuft deshalb ausschliesslich im Arbeitstask, nachdem BUSY
 * geantwortet wurde (v4_link.c, DEFER_SD_MOUNT).
 */
esp_err_t sd_fs_mount(void)
{
    esp_err_t err = sd_card_mount();
    if (err != ESP_OK) {
        return err;
    }

    /* Kapazitaet NUR hier auffrischen: sd_fs_info() wird von GET_STATUS gerufen
     * und muss ohne Dateisystemzugriff antworten koennen. esp_vfs_fat_info()
     * geht ueber f_getfree() und ist damit O(FAT-Groesse). */
    sd_info_cache_update();

    ESP_LOGI(TAG, "gemountet: %s (%u KB gesamt, %u KB frei)",
             SD_FS_MOUNT_POINT, (unsigned)s_total_kb, (unsigned)s_free_kb);
    return ESP_OK;
}

esp_err_t sd_fs_unmount(void)
{
    /* Close anything the master left open. */
    for (int i = 0; i < SD_FS_MAX_DIR_HANDLES; i++) {
        sd_fs_dir_close(i);
    }
    for (int i = 0; i < SD_FS_MAX_FILE_HANDLES; i++) {
        sd_fs_file_close(i);
    }

    esp_err_t err = sd_card_unmount();
    s_total_kb = 0;
    s_free_kb = 0;
    s_sector_size = 0;
    ESP_LOGI(TAG, "unmount (%s)", esp_err_to_name(err));
    return err;
}

bool sd_fs_is_mounted(void)
{
    /* Immer die Wahrheit aus sd_card.c - hier wird kein zweiter Zustand gefuehrt. */
    return sd_card_is_mounted();
}

sd_fs_err_t sd_fs_info(uint32_t *total_kb, uint32_t *free_kb,
                       uint16_t *sector_size, uint8_t *fat_type)
{
    if (!sd_card_is_mounted()) {
        return SD_FS_ERR_NOT_MOUNTED;
    }

    /* Served from the snapshot taken in sd_fs_mount(): this must return instantly
     * because GET_STATUS calls it, and the master waits only a few milliseconds.
     * The values therefore describe the state at mount time; re-mount (SD_MOUNT)
     * to refresh them after writing to the card from elsewhere. */
    if (total_kb != NULL) {
        *total_kb = s_total_kb;
    }
    if (free_kb != NULL) {
        *free_kb = s_free_kb;
    }
    if (sector_size != NULL) {
        *sector_size = s_sector_size;
    }
    if (fat_type != NULL) {
        /* The IDF VFS layer does not expose the FATFS handle, so the FAT type is
         * not reported. 0 means "unknown". */
        *fat_type = 0;
    }
    return SD_FS_OK;
}

/* ------------------------------------------------------------------ */
/* Directory iteration                                                */
/* ------------------------------------------------------------------ */

int sd_fs_dir_open(const char *path)
{
    if (!sd_card_is_mounted()) {
        return -SD_FS_ERR_NOT_MOUNTED;
    }

    int slot = -1;
    for (int i = 0; i < SD_FS_MAX_DIR_HANDLES; i++) {
        if (!s_dirs[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return -SD_FS_ERR_NO_HANDLE;
    }

    char full[SD_FULL_PATH_MAX];
    sd_fs_err_t perr = build_path(path, full, sizeof(full));
    if (perr != SD_FS_OK) {
        return -perr;
    }

    DIR *d = opendir(full);
    if (d == NULL) {
        ESP_LOGW(TAG, "opendir(%s) failed: errno %d", full, errno);
        return -(errno == ENOENT ? SD_FS_ERR_NOT_FOUND : SD_FS_ERR_IO);
    }

    s_dirs[slot].used = true;
    s_dirs[slot].dir = d;
    strncpy(s_dirs[slot].path, full, sizeof(s_dirs[slot].path) - 1);
    s_dirs[slot].path[sizeof(s_dirs[slot].path) - 1] = '\0';
    ESP_LOGI(TAG, "dir handle %d -> %s", slot, full);
    return slot;
}

sd_fs_err_t sd_fs_dir_next(int handle, char *name_out, size_t name_max,
                           bool *is_dir, uint32_t *size, uint8_t *attr,
                           bool *truncated)
{
    if (!sd_card_is_mounted()) {
        return SD_FS_ERR_NOT_MOUNTED;
    }
    if (handle < 0 || handle >= SD_FS_MAX_DIR_HANDLES || !s_dirs[handle].used) {
        return SD_FS_ERR_NO_HANDLE;
    }
    if (name_out == NULL || name_max == 0) {
        return SD_FS_ERR_ARG;
    }

    DIR *d = s_dirs[handle].dir;
    for (;;) {
        errno = 0;
        struct dirent *e = readdir(d);
        if (e == NULL) {
            return SD_FS_ERR_END;
        }
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }

        char full[SD_FULL_PATH_MAX];
        int n = snprintf(full, sizeof(full), "%s/%s", s_dirs[handle].path, e->d_name);
        if (n < 0 || (size_t)n >= sizeof(full)) {
            continue;
        }

        struct stat st;
        bool have_stat = (stat(full, &st) == 0);
        bool dir = have_stat ? S_ISDIR(st.st_mode) : false;

        size_t nlen = strlen(e->d_name);
        bool trunc = (nlen >= name_max);
        if (trunc) {
            nlen = name_max - 1;
        }
        memcpy(name_out, e->d_name, nlen);
        name_out[nlen] = '\0';

        if (is_dir != NULL) {
            *is_dir = dir;
        }
        if (size != NULL) {
            *size = (have_stat && !dir) ? (uint32_t)st.st_size : 0;
        }
        if (attr != NULL) {
            uint8_t a = dir ? V4P_ATTR_DIR : V4P_ATTR_ARCHIVE;
            if (have_stat && !(st.st_mode & S_IWUSR)) {
                a |= V4P_ATTR_RDONLY;
            }
            *attr = a;
        }
        if (truncated != NULL) {
            *truncated = trunc;
        }
        return SD_FS_OK;
    }
}

void sd_fs_dir_close(int handle)
{
    if (handle < 0 || handle >= SD_FS_MAX_DIR_HANDLES || !s_dirs[handle].used) {
        return;
    }
    closedir(s_dirs[handle].dir);
    s_dirs[handle].dir = NULL;
    s_dirs[handle].used = false;
    s_dirs[handle].path[0] = '\0';
}

/* ------------------------------------------------------------------ */
/* File access                                                        */
/* ------------------------------------------------------------------ */

int sd_fs_file_open(const char *path, uint32_t *size_out)
{
    if (!sd_card_is_mounted()) {
        return -SD_FS_ERR_NOT_MOUNTED;
    }

    int slot = -1;
    for (int i = 0; i < SD_FS_MAX_FILE_HANDLES; i++) {
        if (!s_files[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return -SD_FS_ERR_NO_HANDLE;
    }

    char full[SD_FULL_PATH_MAX];
    sd_fs_err_t perr = build_path(path, full, sizeof(full));
    if (perr != SD_FS_OK) {
        return -perr;
    }

    struct stat st;
    if (stat(full, &st) != 0) {
        return -(errno == ENOENT ? SD_FS_ERR_NOT_FOUND : SD_FS_ERR_IO);
    }
    if (S_ISDIR(st.st_mode)) {
        return -SD_FS_ERR_ARG;
    }

    int fd = open(full, O_RDONLY);
    if (fd < 0) {
        ESP_LOGW(TAG, "open(%s) failed: errno %d", full, errno);
        return -(errno == ENOENT ? SD_FS_ERR_NOT_FOUND : SD_FS_ERR_IO);
    }

    s_files[slot].used = true;
    s_files[slot].fd = fd;
    s_files[slot].size = (uint32_t)st.st_size;
    strncpy(s_files[slot].path, full, sizeof(s_files[slot].path) - 1);
    s_files[slot].path[sizeof(s_files[slot].path) - 1] = '\0';

    if (size_out != NULL) {
        *size_out = s_files[slot].size;
    }
    ESP_LOGI(TAG, "file handle %d -> %s (%u bytes)", slot, full, (unsigned)s_files[slot].size);
    return slot;
}

sd_fs_err_t sd_fs_file_read(int handle, uint32_t offset,
                            uint8_t *buf, uint16_t len, uint16_t *read_out)
{
    if (!sd_card_is_mounted()) {
        return SD_FS_ERR_NOT_MOUNTED;
    }
    if (handle < 0 || handle >= SD_FS_MAX_FILE_HANDLES || !s_files[handle].used) {
        return SD_FS_ERR_NO_HANDLE;
    }
    if (buf == NULL || read_out == NULL) {
        return SD_FS_ERR_ARG;
    }

    *read_out = 0;

    if (offset >= s_files[handle].size) {
        return SD_FS_ERR_END;
    }

    off_t pos = lseek(s_files[handle].fd, (off_t)offset, SEEK_SET);
    if (pos < 0) {
        ESP_LOGW(TAG, "lseek failed: errno %d", errno);
        return SD_FS_ERR_IO;
    }

    ssize_t n = read(s_files[handle].fd, buf, len);
    if (n < 0) {
        ESP_LOGW(TAG, "read failed: errno %d", errno);
        return SD_FS_ERR_IO;
    }
    if (n == 0) {
        return SD_FS_ERR_END;
    }

    *read_out = (uint16_t)n;
    return SD_FS_OK;
}

void sd_fs_file_close(int handle)
{
    if (handle < 0 || handle >= SD_FS_MAX_FILE_HANDLES || !s_files[handle].used) {
        return;
    }
    close(s_files[handle].fd);
    s_files[handle].fd = -1;
    s_files[handle].used = false;
    s_files[handle].size = 0;
    s_files[handle].path[0] = '\0';
}

uint32_t sd_fs_file_size(int handle)
{
    if (handle < 0 || handle >= SD_FS_MAX_FILE_HANDLES || !s_files[handle].used) {
        return 0;
    }
    return s_files[handle].size;
}
