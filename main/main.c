/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * main.c - Start und Bluetooth-Ereignisse.
 *
 * Das Projekt ist eine reine A2DP-QUELLE ueber Classic Bluetooth: der ESP32
 * sendet, eine Soundbar oder ein Kopfhoerer empfaempfaengt. Es gibt deshalb
 * keinen Codec und keinen Board-Manager - die Tonquellen sind der I2S-Eingang
 * der Vampire und Dateien von der SD-Karte, beide laufen ueber den Mischer in
 * stream_proc.c.
 */

#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_bt.h"
#include "esp_bt_audio.h"
#include "esp_bt_audio_classic.h"
#include "esp_bt_audio_media.h"
#include "esp_bt_audio_playback.h"
#include "esp_gmf_pool.h"

#include "cmd_reg.h"
#include "pool_reg.h"
#include "sd_card.h"
#include "sd_fs.h"
#include "i2s_input.h"
#include "stream_proc.h"
#include "codec_defs.h"
#include "version.h"
#include "bt_manager.h"
#include "v4_link.h"

/* Sendevorgang des A2DP-Senders: eigener Task auf Kern 1. */
#define A2DP_SRC_SEND_TASK_CORE_ID     1
#define A2DP_SRC_SEND_TASK_PRIO        10
#define A2DP_SRC_SEND_TASK_STACK_SIZE  4096

#define VOLUME_CTRL_QUEUE_SIZE        8
#define VOLUME_CTRL_TASK_STACK_SIZE   3072
#define VOLUME_CTRL_TASK_PRIO         5

static const char *TAG = "BT_AUD_EXAMPLE";

static const char *media_ctrl_cmd_str[] = {
    "UNKNOWN",
    "PLAY",
    "PAUSE",
    "STOP",
    "NEXT",
    "PREV",
};

static const char *playback_metadata_type_str[] = {
    "TITLE",
    "ARTIST",
    "ALBUM",
    "TRACK_NUM",
    "NUM_TRACKS",
    "GENRE",
    "PLAYING_TIME",
    "COVER_ART",
};

static esp_gmf_pool_handle_t pool = NULL;
static QueueHandle_t volume_ctrl_queue = NULL;

typedef enum {
    VOLUME_CTRL_CMD_ABSOLUTE,
    VOLUME_CTRL_CMD_RELATIVE,
} volume_ctrl_cmd_type_t;

typedef struct {
    volume_ctrl_cmd_type_t         type;
    uint8_t                        vol;
    bool                           mute;
    bool                           up_down;
    esp_bt_audio_stream_context_t  context;
} volume_ctrl_cmd_t;

static inline const char *media_ctrl_cmd_to_str(esp_bt_audio_media_ctrl_cmd_t cmd)
{
    return media_ctrl_cmd_str[cmd];
}

static inline const char *playback_metadata_type_to_str(uint32_t type)
{
    return playback_metadata_type_str[__builtin_ctz(type)];
}

/*
 * Lautstaerke aus AVRCP.
 *
 * Es gibt hier keinen lokalen Codec, dessen Lautstaerke wir setzen koennten -
 * die Werte kommen von der Gegenseite (AVRCP) und werden nur mitgefuehrt und
 * geloggt. Das genuegt, um beim Aufbau der Kette zu sehen, was ankommt.
 */
static int volume_ctrl_current = 50;

static bool volume_ctrl_post_cmd(const volume_ctrl_cmd_t *cmd)
{
    if (volume_ctrl_queue == NULL) {
        ESP_LOGE(TAG, "Volume control task is not initialized");
        return false;
    }
    if (xQueueSend(volume_ctrl_queue, cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Volume control command queue full, drop action %d", cmd->type);
        return false;
    }
    return true;
}

static void volume_ctrl_task(void *arg)
{
    (void)arg;
    volume_ctrl_cmd_t cmd = {0};

    while (true) {
        if (xQueueReceive(volume_ctrl_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (cmd.type) {
            case VOLUME_CTRL_CMD_ABSOLUTE:
                volume_ctrl_current = cmd.vol;
                ESP_LOGI(TAG, "Set absolute volume: vol %d, mute %d, context %d",
                         cmd.vol, cmd.mute, cmd.context);
                break;
            case VOLUME_CTRL_CMD_RELATIVE:
                volume_ctrl_current = cmd.up_down ?
                                      ((volume_ctrl_current >= 90) ? 100 : volume_ctrl_current + 10) :
                                      ((volume_ctrl_current <= 10) ? 0 : volume_ctrl_current - 10);
                ESP_LOGI(TAG, "Set relative volume: up_down %d, context %d, volume %d",
                         cmd.up_down, cmd.context, volume_ctrl_current);
                break;
            default:
                ESP_LOGW(TAG, "Unknown volume control action %d", cmd.type);
                break;
        }
    }
}

static void setup_volume_ctrl_task(void)
{
    if (volume_ctrl_queue) {
        return;
    }
    volume_ctrl_queue = xQueueCreate(VOLUME_CTRL_QUEUE_SIZE, sizeof(volume_ctrl_cmd_t));
    if (volume_ctrl_queue == NULL) {
        ESP_LOGE(TAG, "Create volume control command queue failed");
        return;
    }
    BaseType_t ret = xTaskCreate(volume_ctrl_task, "volume_ctrl_task", VOLUME_CTRL_TASK_STACK_SIZE, NULL,
                                 VOLUME_CTRL_TASK_PRIO, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Create volume control task failed");
        vQueueDelete(volume_ctrl_queue);
        volume_ctrl_queue = NULL;
    }
}

/* Der Geraetename bekommt die letzten beiden MAC-Bytes, damit sich mehrere
 * Module im Bluetooth-Menue unterscheiden lassen. */
static esp_err_t append_mac_suffix_to_device_name(char *device_name, size_t device_name_size)
{
    uint8_t mac[6] = {0};
    char default_name[ESP_BT_AUDIO_HOST_MAX_DEV_NAME_LEN] = {0};

    int written = snprintf(default_name, sizeof(default_name), "%s", device_name);
    if (written < 0 || (size_t)written >= sizeof(default_name)) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_BT);
    if (ret != ESP_OK) {
        return ret;
    }
    written = snprintf(device_name, device_name_size, "%s_%02X%02X", default_name, mac[4], mac[5]);
    if (written < 0 || (size_t)written >= device_name_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static uint32_t get_classic_roles(void)
{
    uint32_t roles = ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC;
#ifdef CONFIG_GMF_EXAMPLE_AVRC_CT
    roles |= ESP_BT_AUDIO_CLASSIC_ROLE_AVRC_CT;
#endif
#ifdef CONFIG_GMF_EXAMPLE_AVRC_TG
    roles |= ESP_BT_AUDIO_CLASSIC_ROLE_AVRC_TG;
#endif
    return roles;
}

/* Steuerbefehle der Gegenseite (AVRCP). NEXT/PREV schalten den Datei-Zweig
 * weiter, PLAY/PAUSE/STOP starten und stoppen die A2DP-Uebertragung. */
static void media_ctrl_cmd_proc(esp_bt_audio_media_ctrl_cmd_t cmd)
{
    ESP_LOGI(TAG, "Media control command: %s", media_ctrl_cmd_to_str(cmd));
    switch (cmd) {
        case ESP_BT_AUDIO_MEDIA_CTRL_CMD_PLAY:
            esp_bt_audio_media_start(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, NULL);
            break;
        case ESP_BT_AUDIO_MEDIA_CTRL_CMD_PAUSE:
        case ESP_BT_AUDIO_MEDIA_CTRL_CMD_STOP:
            esp_bt_audio_media_stop(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC);
            break;
        case ESP_BT_AUDIO_MEDIA_CTRL_CMD_NEXT:
            local2bt_play_next();
            break;
        case ESP_BT_AUDIO_MEDIA_CTRL_CMD_PREV:
            local2bt_play_prev();
            break;
        default:
            ESP_LOGW(TAG, "Media control command %d not supported", cmd);
            break;
    }
}

static void playback_status_chg_proc(esp_bt_audio_event_playback_st_t *event_data)
{
    switch (event_data->event) {
        case ESP_BT_AUDIO_PLAYBACK_EVENT_PLAY_STATUS_CHANGE:
            ESP_LOGI(TAG, "Playback status changed: %d", event_data->evt_param.play_status);
            break;
        case ESP_BT_AUDIO_PLAYBACK_EVENT_TRACK_CHANGE:
            ESP_LOGI(TAG, "Track changed, requesting metadata");
            esp_bt_audio_playback_request_metadata(ESP_BT_AUDIO_PLAYBACK_METADATA_TITLE |
                                                   ESP_BT_AUDIO_PLAYBACK_METADATA_ARTIST |
                                                   ESP_BT_AUDIO_PLAYBACK_METADATA_ALBUM |
                                                   ESP_BT_AUDIO_PLAYBACK_METADATA_GENRE |
                                                   ESP_BT_AUDIO_PLAYBACK_METADATA_COVER_ART);
            break;
        case ESP_BT_AUDIO_PLAYBACK_EVENT_PLAY_POS_CHANGED:
            ESP_LOGI(TAG, "Playback position changed: %d", event_data->evt_param.position);
            break;
        default:
            ESP_LOGW(TAG, "Playback event %02X", event_data->event);
            break;
    }
}

static void playback_metadata_proc(esp_bt_audio_event_playback_metadata_t *event_data)
{
    if (event_data->type == ESP_BT_AUDIO_PLAYBACK_METADATA_COVER_ART) {
        esp_bt_audio_playback_cover_art_t *cover_art = (esp_bt_audio_playback_cover_art_t *)event_data->value;
        if (cover_art != NULL && cover_art->data != NULL && cover_art->size > 0) {
            ESP_LOGI(TAG, "Cover art: size %d, format 0x%04X", cover_art->size, cover_art->format_fourcc);
        }
    } else {
        ESP_LOGI(TAG, "Metadata: %s:\t%s",
                 playback_metadata_type_to_str(event_data->type),
                 event_data->length > 0 ? (const char *)event_data->value : "");
    }
}

static void bt_audio_event_cb(esp_bt_audio_event_t event, void *event_data, void *user_data)
{
    switch (event) {
        case ESP_BT_AUDIO_EVENT_DISCOVERY_STATE_CHG: {
            esp_bt_audio_event_discovery_st_t *discovery_state = (esp_bt_audio_event_discovery_st_t *)event_data;
            ESP_LOGI(TAG, "Device Discovery State Changed: %s",
                     discovery_state->discovering ? "Discovering" : "Not discovering");
            /* Der I2C-Master fragt "scan_active" ab und will wissen, wann die
             * Liste fertig ist - deshalb hierher weiterreichen. */
            bt_mgr_evt_discovery(discovery_state->discovering);
            break;
        }
        case ESP_BT_AUDIO_EVENT_DEVICE_DISCOVERED: {
            esp_bt_audio_event_device_discovered_t *device_discovered = (esp_bt_audio_event_device_discovered_t *)event_data;
            ESP_LOGI(TAG, "Device discovered: %s  %02x:%02x:%02x:%02x:%02x:%02x  RSSI %d dBm",
                     device_discovered->name,
                     device_discovered->addr[0], device_discovered->addr[1], device_discovered->addr[2],
                     device_discovered->addr[3], device_discovered->addr[4], device_discovered->addr[5],
                     device_discovered->rssi);
            if (device_discovered->tech == ESP_BT_AUDIO_TECH_CLASSIC) {
                ESP_LOGI(TAG, "  CoD: 0x%06x", device_discovered->disc_data.classic.cod);
                /* Nur Classic-Geraete in die Tabelle des I2C-Protokolls: die
                 * V4 steuert eine A2DP-Senke an, LE-Geraete kann sie damit
                 * nicht verbinden. */
                bt_mgr_evt_discovered(device_discovered->name, device_discovered->addr);
            }
            cli_bt_device_found(device_discovered->name, device_discovered->addr);
            break;
        }
        case ESP_BT_AUDIO_EVENT_CONNECTION_STATE_CHG: {
            esp_bt_audio_event_connection_st_t *conn_st = (esp_bt_audio_event_connection_st_t *)event_data;
            ESP_LOGI(TAG, "Connection state changed: %s (%02x:%02x:%02x:%02x:%02x:%02x)",
                     conn_st->connected ? "Connected" : "Disconnected",
                     conn_st->addr[0], conn_st->addr[1], conn_st->addr[2],
                     conn_st->addr[3], conn_st->addr[4], conn_st->addr[5]);
            /*
             * Verbunden: nicht mehr sichtbar/koppelbar. Getrennt: wieder
             * sichtbar, damit sich ein anderes Geraet verbinden kann.
             */
            esp_bt_audio_classic_set_scan_mode(!conn_st->connected, false);
            bt_mgr_evt_connection(conn_st->connected, conn_st->addr);
            cli_bt_device_conn_st_chg(conn_st->addr, conn_st->connected);
            break;
        }
        case ESP_BT_AUDIO_EVENT_STREAM_STATE_CHG: {
            esp_bt_audio_event_stream_st_t *stream_state = (esp_bt_audio_event_stream_st_t *)event_data;
            bt_mgr_evt_stream(stream_state->state == ESP_BT_AUDIO_STREAM_STATE_STARTED);
            stream_proc_state_chg(stream_state->stream_handle, stream_state->state);
            break;
        }
        case ESP_BT_AUDIO_EVENT_MEDIA_CTRL_CMD: {
            esp_bt_audio_event_media_ctrl_t *media_ctrl_cmd = (esp_bt_audio_event_media_ctrl_t *)event_data;
            media_ctrl_cmd_proc(media_ctrl_cmd->cmd);
            break;
        }
        case ESP_BT_AUDIO_EVENT_PLAYBACK_STATUS_CHG: {
            playback_status_chg_proc((esp_bt_audio_event_playback_st_t *)event_data);
            break;
        }
        case ESP_BT_AUDIO_EVENT_PLAYBACK_METADATA: {
            playback_metadata_proc((esp_bt_audio_event_playback_metadata_t *)event_data);
            break;
        }
        case ESP_BT_AUDIO_EVENT_VOL_ABSOLUTE: {
            esp_bt_audio_event_vol_absolute_t *vol_absolute = (esp_bt_audio_event_vol_absolute_t *)event_data;
            volume_ctrl_cmd_t cmd = {
                .type = VOLUME_CTRL_CMD_ABSOLUTE,
                .vol = vol_absolute->vol,
                .mute = vol_absolute->mute,
                .context = vol_absolute->context,
            };
            volume_ctrl_post_cmd(&cmd);
            break;
        }
        case ESP_BT_AUDIO_EVENT_VOL_RELATIVE: {
            esp_bt_audio_event_vol_relative_t *vol_relative = (esp_bt_audio_event_vol_relative_t *)event_data;
            volume_ctrl_cmd_t cmd = {
                .type = VOLUME_CTRL_CMD_RELATIVE,
                .up_down = vol_relative->up_down,
                .context = vol_relative->context,
            };
            volume_ctrl_post_cmd(&cmd);
            break;
        }
        default:
            ESP_LOGD(TAG, "bt audio event %d", event);
            break;
    }
}

void app_main(void)
{
    /* Versionsstempel als erste Ausgabe: damit ist am Log sofort zu sehen,
     * welcher Stand auf dem Chip laeuft (die Nummer kommt aus version.txt). */
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, " V4_ESP32  Version %s", APP_VERSION_STRING);
    ESP_LOGI(TAG, " uebersetzt am %s um %s", __DATE__, __TIME__);
    ESP_LOGI(TAG, " ESP-IDF %s", esp_get_idf_version());
    ESP_LOGI(TAG, "==================================================");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    setup_volume_ctrl_task();

    /* Die SD-Karte muss vor dem ersten Medienstart bereitstehen. Fehlt sie,
     * laeuft Bluetooth trotzdem - nur die lokale Wiedergabe fehlt.
     *
     * Gemountet wird ueber sd_fs_mount() (das seinerseits sd_card_mount()
     * aufruft): nur so wird die Kapazitaet fuer SD_INFO/GET_STATUS des
     * I2C-Protokolls gleich beim Start einmal gemerkt. Der Mount selbst bleibt
     * damit an genau einer Stelle - in sd_card.c. */
    if (sd_fs_mount() != ESP_OK) {
        ESP_LOGW(TAG, "SD card not mounted - local playback will fail");
        ESP_LOGW(TAG, "  Falls das Modul ausserhalb der Platine sitzt: nach dem Einbau 'sd_mount' senden.");
    }

    ESP_ERROR_CHECK(esp_gmf_pool_init(&pool));
    ESP_ERROR_CHECK(pool_reg(pool));        /* Elemente und IO-Typen anmelden */
    stream_proc_init(pool);                 /* Pipelines aufbauen */

    /* Classic Bluetooth, nur BR/EDR (kein BLE). */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_audio_host_bluedroid_cfg_t host_cfg = ESP_BT_AUDIO_HOST_BLUEDROID_CFG_DEFAULT();
    ESP_ERROR_CHECK(append_mac_suffix_to_device_name(host_cfg.dev_name, sizeof(host_cfg.dev_name)));

    esp_bt_audio_config_t bt_config = {
        .host_config = &host_cfg,
        .event_cb = bt_audio_event_cb,
        .event_user_ctx = NULL,
        .classic.roles = get_classic_roles(),
        .classic.a2dp_src_send_task_core_id = A2DP_SRC_SEND_TASK_CORE_ID,
        .classic.a2dp_src_send_task_prio = A2DP_SRC_SEND_TASK_PRIO,
        .classic.a2dp_src_send_task_stack_size = A2DP_SRC_SEND_TASK_STACK_SIZE,
    };
    ESP_ERROR_CHECK(esp_bt_audio_init(&bt_config));

    /* Sichtbar und koppelbar, damit sich die Senke verbinden kann. */
    ESP_ERROR_CHECK(esp_bt_audio_classic_set_scan_mode(true, false));

    /*
     * Bruecke zur Vampire V4: erst die Zustands-/Geraeteliste fuer Bluetooth,
     * dann der I2C-Slave (Adresse 0x50, SDA=GPIO18, SCL=GPIO23 - siehe
     * PIN verbindungen.txt). Beides ist Zusatz zur Tonbruecke: faellt der
     * I2C-Weg aus, soll der A2DP-Sender trotzdem laufen, deshalb hier kein
     * ESP_ERROR_CHECK.
     */
    ESP_ERROR_CHECK(bt_mgr_init());
    esp_err_t v4_err = v4_link_init();
    if (v4_err != ESP_OK) {
        ESP_LOGE(TAG, "I2C-Bruecke zur Vampire V4 nicht gestartet: %s", esp_err_to_name(v4_err));
    }

    /* Wiedergabe-Statusmeldungen sind Sache einer A2DP-SENKE (sie melden, was
     * die Gegenstelle tut). Als Quelle registrieren wir sie nicht - der Aufruf
     * wuerde mit ESP_ERR_INVALID_STATE fehlschlagen und einen Neustart ausloesen. */

    cli_init();
}
