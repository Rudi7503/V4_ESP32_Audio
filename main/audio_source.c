/*
 * audio_source.c - welche Quelle den A2DP-Sender speist.
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Uebernommen aus dem Vorgaengerprojekt (ESP32-I2S-to-BT). Die Schnittstelle in
 * audio_source.h ist unveraendert geblieben, die Umsetzung liegt jetzt aber auf
 * den Mitteln dieses Projekts:
 *
 *   ALT (ADF-Kette)              NEU (GMF, dieses Projekt)
 *   audio_source_play_sd()  ->   local2bt_play_file()
 *   audio_source_use_i2s()  ->   local2bt_stop()
 *   audio_source_get()      ->   local2bt_is_playing()
 *   current_path()          ->   local2bt_current_uri()
 *
 * WICHTIGER UNTERSCHIED zum Vorgaengerprojekt: dort war die Quelle ENTWEDER der
 * I2S-Eingang ODER die SD-Datei - die Kette wurde dafuer umgebaut. Hier laufen
 * beide ueber den Mischer, der Ton der Vampire ist also immer dabei.
 * AUDIO_SOURCE_SD heisst deshalb nur "zusaetzlich laeuft gerade eine Datei" und
 * nicht "der I2S-Eingang ist abgeschaltet". Fuer das Protokoll ist das die
 * richtige Aussage: STOP_PLAY haelt die Datei an, und der Vampire-Ton bleibt
 * hoerbar - genau das verspricht PROTOCOL_V4.md bei PLAY_FILE/STOP_PLAY.
 */

#include <string.h>

#include "esp_log.h"
#include "esp_err.h"

#include "audio_source.h"
#include "stream_proc.h"

static const char *TAG = "audio_source";

esp_err_t audio_source_play_sd(const char *vfs_path)
{
    if (vfs_path == NULL || vfs_path[0] == '\0') {
        ESP_LOGW(TAG, "play_sd ohne Pfad");
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * local2bt_play_file() nimmt "/sdcard/MUSIC/track.mp3" genauso wie
     * "file://sdcard/MUSIC/track.mp3" und baut daraus den GMF-URI. Der Aufruf
     * blockiert kurz (Pipeline-Umbau, Zehner Millisekunden), deshalb antwortet
     * das I2C-Protokoll vorher BUSY und wiederholt den Befehl
     * (v4_link.c, DEFER_SD_PLAY).
     */
    ESP_LOGI(TAG, "SD-Datei nach Bluetooth: %s", vfs_path);
    local2bt_play_file(vfs_path);
    return ESP_OK;
}

esp_err_t audio_source_use_i2s(void)
{
    /*
     * Nur vormerken: ausgefuehrt wird der Stop in der stream_proc-Aufgabe. Der
     * Aufruf kommt aus dem Arbeitstask des I2C-Protokolls und darf dort nicht
     * auf den GMF-Task warten (siehe v4_link.c, Abschnitt zum BUSY-Muster).
     */
    local2bt_stop();
    return ESP_OK;
}

audio_source_t audio_source_get(void)
{
    return local2bt_is_playing() ? AUDIO_SOURCE_SD : AUDIO_SOURCE_I2S;
}

const char *audio_source_current_path(void)
{
    return local2bt_current_uri();
}
