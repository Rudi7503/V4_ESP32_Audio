/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>

#include "esp_err.h"
#include "esp_chip_info.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_clk_tree.h"
#include "esp_private/esp_clk.h"
#include "soc/clk_tree_defs.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "soc/soc.h"
#include "soc/dport_access.h"
#include "soc/sdmmc_pins.h"
#include "soc/gpio_reg.h"
#include "soc/spi_struct.h"
#include "soc/sdmmc_struct.h"
#include "soc/dport_reg.h"

#include "version.h"

#include "esp_bt_audio_defs.h"
#include "esp_bt_audio_classic.h"
#include "esp_bt_audio_le.h"
#include "esp_bt_audio_vol.h"
#include "esp_bt_audio_media.h"
#include "esp_bt_audio_playback.h"
#include "esp_gmf_oal_sys.h"
#include "esp_bt_audio_tel.h"
#include "esp_bt_audio_pb.h"

#include "cmd_reg.h"
#include "sd_card.h"
#include "stream_proc.h"

static uint8_t target_device_bda[6] = {0};
#if CONFIG_BT_CLASSIC_ENABLED && defined(CONFIG_GMF_EXAMPLE_A2DP_SOURCE)
static char target_device_name[32] = {0};
static const char *TAG = "CMD_REG";
#endif  /* CONFIG_BT_CLASSIC_ENABLED && defined(CONFIG_GMF_EXAMPLE_A2DP_SOURCE) */

static int cmd_playback_play(int argc, char **argv)
{
    esp_err_t ret = esp_bt_audio_playback_play();
    if (ret == ESP_OK) {
        printf("Media play command sent\n");
    } else {
        printf("Failed to send media play command: %s\n", esp_err_to_name(ret));
    }
    return 0;
}

static int cmd_playback_pause(int argc, char **argv)
{
    esp_err_t ret = esp_bt_audio_playback_pause();
    if (ret == ESP_OK) {
        printf("Media pause command sent\n");
    } else {
        printf("Failed to send media pause command: %s\n", esp_err_to_name(ret));
    }
    return 0;
}

static int cmd_playback_stop(int argc, char **argv)
{
    esp_err_t ret = esp_bt_audio_playback_stop();
    if (ret == ESP_OK) {
        printf("Media stop command sent\n");
    } else {
        printf("Failed to send media stop command: %s\n", esp_err_to_name(ret));
    }
    return 0;
}

static int cmd_playback_next(int argc, char **argv)
{
    esp_err_t ret = esp_bt_audio_playback_next();
    if (ret == ESP_OK) {
        printf("Media next command sent\n");
    } else {
        printf("Failed to send media next command: %s\n", esp_err_to_name(ret));
    }
    return 0;
}

static int cmd_playback_prev(int argc, char **argv)
{
    esp_err_t ret = esp_bt_audio_playback_prev();
    if (ret == ESP_OK) {
        printf("Media previous command sent\n");
    } else {
        printf("Failed to send media previous command: %s\n", esp_err_to_name(ret));
    }
    return 0;
}

/*
 * 'playfile <datei>' - eine Datei von der SD-Karte in den Mischer spielen.
 *
 * Der Ton der Vampire (I2S) laeuft dabei weiter und wird dazugemischt - das
 * ist der eigentliche Zweck des Mischers. Ohne Argument wird der erste Eintrag
 * der eingebauten Wiedergabeliste genommen.
 */
static int cmd_playfile(int argc, char **argv)
{
    const char *uri = (argc > 1) ? argv[1] : "test2.mp3";
    printf("Datei -> Mischer: %s\n", uri);
    printf("  (Vampire-Ton laeuft weiter und wird dazugemischt)\n");
    local2bt_play_file(uri);
    printf("  Zum Hoeren jetzt 'start_media' senden, falls noch nicht gestartet.\n");
    return 0;
}

static int cmd_playback_metadata(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage: metadata <mask>\n");
        printf("  mask bits: 0x1 title, 0x2 artist, 0x4 album, 0x8 track_num,\n");
        printf("             0x10 num_tracks, 0x20 genre, 0x40 time, 0x80 cover\n");
        return 1;
    }

    char *endptr = NULL;
    uint32_t mask = (uint32_t)strtoul(argv[1], &endptr, 0);
    if (argv[1][0] == '\0' || (endptr && *endptr != '\0')) {
        printf("Invalid mask: %s\n", argv[1]);
        return 1;
    }

    esp_err_t ret = esp_bt_audio_playback_request_metadata(mask);
    if (ret == ESP_OK) {
        printf("Media metadata request sent\n");
    } else {
        printf("Failed to send media metadata request: %s\n", esp_err_to_name(ret));
    }
    return 0;
}

static int cmd_volume_set(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage: vol_set <volume>\n");
        printf("  volume: 0-100\n");
        return 1;
    }

    int volume = atoi(argv[1]);
    if (volume < 0 || volume > 100) {
        printf("Volume must be between 0 and 100\n");
        return 1;
    }
#if CONFIG_GMF_EXAMPLE_A2DP_SOURCE
    esp_err_t ret = esp_bt_audio_vol_set_absolute((uint32_t)volume);
    if (ret == ESP_OK) {
        printf("Volume set to %d\n", volume);
    } else {
        printf("Failed to set volume: %s\n", esp_err_to_name(ret));
    }
#endif  /* CONFIG_GMF_EXAMPLE_A2DP_SOURCE */
    return 0;
}

static int cmd_volume_up(int argc, char **argv)
{
#if CONFIG_GMF_EXAMPLE_A2DP_SOURCE
    esp_err_t ret = esp_bt_audio_vol_set_relative(true);
    if (ret == ESP_OK) {
        printf("Volume up\n");
    } else {
        printf("Failed to increase volume: %s\n", esp_err_to_name(ret));
    }
#endif  /* CONFIG_GMF_EXAMPLE_A2DP_SOURCE */
    return 0;
}

static int cmd_volume_down(int argc, char **argv)
{
#if CONFIG_GMF_EXAMPLE_A2DP_SOURCE
    esp_err_t ret = esp_bt_audio_vol_set_relative(false);
    if (ret == ESP_OK) {
        printf("Volume down\n");
    } else {
        printf("Failed to decrease volume: %s\n", esp_err_to_name(ret));
    }
#endif  /* CONFIG_GMF_EXAMPLE_A2DP_SOURCE */
    return 0;
}

#if CONFIG_BT_CLASSIC_ENABLED && CONFIG_GMF_EXAMPLE_AUDIO_TECH_CLASSIC
static int cmd_connect_device(int argc, char **argv)
{
#if CONFIG_BT_CLASSIC_ENABLED && CONFIG_GMF_EXAMPLE_AUDIO_TECH_CLASSIC
    if (argc != 2) {
        printf("Usage: connect <mac_address>\n");
        printf("Example: connect 01:02:03:04:05:06\n");
        return 1;
    }

    uint8_t bda[6] = {0};
    if (sscanf(argv[1], "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
               &bda[0], &bda[1], &bda[2], &bda[3], &bda[4], &bda[5]) != 6) {
        printf("Invalid MAC address format. Use: XX:XX:XX:XX:XX:XX\n");
        return 1;
    }
#if CONFIG_GMF_EXAMPLE_A2DP_SOURCE
    esp_err_t ret = esp_bt_audio_classic_connect(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, bda);
#else   /* CONFIG_GMF_EXAMPLE_A2DP_SOURCE */
    esp_err_t ret = esp_bt_audio_classic_connect(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SNK, bda);
#endif  /* CONFIG_GMF_EXAMPLE_A2DP_SOURCE */
    if (ret == ESP_OK) {
        printf("Connecting to device %s...\n", argv[1]);
        memcpy(target_device_bda, bda, sizeof(target_device_bda));
    } else {
        printf("Failed to connect: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
#else
    (void)argc;
    (void)argv;
    printf("Classic connect is disabled in LE example mode\n");
    return 1;
#endif  /* CONFIG_BT_CLASSIC_ENABLED && CONFIG_GMF_EXAMPLE_AUDIO_TECH_CLASSIC */
}

static int cmd_disconnect_device(int argc, char **argv)
{
#if CONFIG_BT_CLASSIC_ENABLED && CONFIG_GMF_EXAMPLE_AUDIO_TECH_CLASSIC
    bool is_bda_empty = true;
    for (int i = 0; i < sizeof(target_device_bda); ++i) {
        if (target_device_bda[i] != 0) {
            is_bda_empty = false;
            break;
        }
    }
    if (is_bda_empty) {
        printf("No device is currently connected.\n");
        return 1;
    }
#if CONFIG_GMF_EXAMPLE_A2DP_SOURCE
    esp_err_t ret = esp_bt_audio_classic_disconnect(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, target_device_bda);
#else   /* CONFIG_GMF_EXAMPLE_A2DP_SOURCE */
    esp_err_t ret = esp_bt_audio_classic_disconnect(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SNK, target_device_bda);
#endif  /* CONFIG_GMF_EXAMPLE_A2DP_SOURCE */
    if (ret == ESP_OK) {
        printf("Disconnecting device...\n");
        memset(target_device_bda, 0, sizeof(target_device_bda));
    } else {
        printf("Failed to disconnect: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
#else
    (void)argc;
    (void)argv;
    printf("Classic disconnect is disabled in LE example mode\n");
    return 1;
#endif  /* CONFIG_BT_CLASSIC_ENABLED && CONFIG_GMF_EXAMPLE_AUDIO_TECH_CLASSIC */
}

static int cmd_hf_connect(int argc, char **argv)
{
#if CONFIG_BT_CLASSIC_ENABLED && CONFIG_GMF_EXAMPLE_AUDIO_TECH_CLASSIC
    if (argc != 2) {
        printf("Usage: hf_connect <mac_address>\n");
        printf("Example: hf_connect 01:02:03:04:05:06\n");
        return 1;
    }

    uint8_t bda[6];
    if (sscanf(argv[1], "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
               &bda[0], &bda[1], &bda[2], &bda[3], &bda[4], &bda[5]) != 6) {
        printf("Invalid MAC address format. Use: XX:XX:XX:XX:XX:XX\n");
        return 1;
    }

    esp_err_t ret = esp_bt_audio_classic_connect(ESP_BT_AUDIO_CLASSIC_ROLE_HFP_HF, bda);
    if (ret == ESP_OK) {
        printf("Connecting HFP HF to device %s...\n", argv[1]);
        memcpy(target_device_bda, bda, sizeof(target_device_bda));
    } else {
        printf("Failed to connect HFP HF: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
#else
    (void)argc;
    (void)argv;
    printf("HFP is disabled in LE example mode\n");
    return 1;
#endif  /* CONFIG_BT_CLASSIC_ENABLED && CONFIG_GMF_EXAMPLE_AUDIO_TECH_CLASSIC */
}

static int cmd_hf_disconnect(int argc, char **argv)
{
#if CONFIG_BT_CLASSIC_ENABLED && CONFIG_GMF_EXAMPLE_AUDIO_TECH_CLASSIC
    bool is_bda_empty = true;
    for (int i = 0; i < sizeof(target_device_bda); ++i) {
        if (target_device_bda[i] != 0) {
            is_bda_empty = false;
            break;
        }
    }
    if (is_bda_empty) {
        printf("No device is currently connected.\n");
        return 1;
    }

    esp_err_t ret = esp_bt_audio_classic_disconnect(ESP_BT_AUDIO_CLASSIC_ROLE_HFP_HF, target_device_bda);
    if (ret == ESP_OK) {
        printf("Disconnecting HFP HF device...\n");
        memset(target_device_bda, 0, sizeof(target_device_bda));
    } else {
        printf("Failed to disconnect HFP HF: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
#else
    (void)argc;
    (void)argv;
    printf("HFP is disabled in LE example mode\n");
    return 1;
#endif  /* CONFIG_BT_CLASSIC_ENABLED && CONFIG_GMF_EXAMPLE_AUDIO_TECH_CLASSIC */
}
#endif  /* CONFIG_BT_CLASSIC_ENABLED && CONFIG_GMF_EXAMPLE_AUDIO_TECH_CLASSIC */

static int cmd_call_answer(int argc, char **argv)
{
    esp_err_t ret = esp_bt_audio_call_answer(0);
    if (ret == ESP_OK) {
        printf("Call answer command sent\n");
    } else {
        printf("Failed to send call answer command: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}

static int cmd_call_reject(int argc, char **argv)
{
    esp_err_t ret = esp_bt_audio_call_reject(0);
    if (ret == ESP_OK) {
        printf("Call reject/terminate command sent\n");
    } else {
        printf("Failed to send call reject/terminate command: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}

static int cmd_call_dial(int argc, char **argv)
{
    if (argc > 2) {
        printf("Usage: call_dial [number]\n");
        printf("  number: Optional. If omitted, redial last number\n");
        return 1;
    }
    const char *number = (argc == 2) ? argv[1] : NULL;
    esp_err_t ret = esp_bt_audio_call_dial(number);
    if (ret == ESP_OK) {
        printf("Call dial command sent\n");
    } else {
        printf("Failed to send call dial command: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}

static int cmd_pb_fetch(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: pb_fetch <target> [start_idx] [count]\n");
        printf("  target (1-5 only):\n");
        printf("    1  Main phonebook\n");
        printf("    2  Incoming call history\n");
        printf("    3  Outgoing call history\n");
        printf("    4  Missed call history\n");
        printf("    5  Combined call history\n");
        printf("  start_idx: Start index (default 0)\n");
        printf("  count:     Number of entries, 0 = fetch all (default 0)\n");
        return 1;
    }

    char *endptr = NULL;
    unsigned long tv = strtoul(argv[1], &endptr, 0);
    if (argv[1][0] == '\0' || (endptr && *endptr != '\0') || tv < 1 || tv > 5) {
        printf("Invalid target: %s (use 1-5, see pb_fetch with no args)\n", argv[1]);
        return 1;
    }
    uint8_t target = (uint8_t)tv;

    uint16_t start_idx = 0;
    uint16_t count = 0;
    if (argc >= 3) {
        char *endptr = NULL;
        unsigned long v = strtoul(argv[2], &endptr, 0);
        if (argv[2][0] == '\0' || (endptr && *endptr != '\0') || v > 0xFFFF) {
            printf("Invalid start_idx: %s\n", argv[2]);
            return 1;
        }
        start_idx = (uint16_t)v;
    }
    if (argc >= 4) {
        char *endptr = NULL;
        unsigned long v = strtoul(argv[3], &endptr, 0);
        if (argv[3][0] == '\0' || (endptr && *endptr != '\0') || v > 0xFFFF) {
            printf("Invalid count: %s\n", argv[3]);
            return 1;
        }
        count = (uint16_t)v;
    }

    esp_err_t ret = esp_bt_audio_pb_fetch(target, start_idx, count);
    if (ret == ESP_OK) {
        printf("Phonebook/call history fetch request sent (target=%u, start=%u, count=%u)\n",
               (unsigned)target, (unsigned)start_idx, (unsigned)count);
    } else {
        printf("Failed to send pb_fetch: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}

#if CONFIG_GMF_EXAMPLE_AUDIO_TECH_LE
static int cmd_le_scan_start(int argc, char **argv)
{
    uint32_t timeout_ms = CONFIG_GMF_EXAMPLE_LE_SCAN_TIMEOUT_MS;

    if (argc >= 2) {
        timeout_ms = (uint32_t)strtoul(argv[1], NULL, 0);
    }
    esp_err_t ret = esp_bt_audio_le_scan_start(timeout_ms);
    if (ret == ESP_OK) {
        printf("LE scan started (%u ms)\n", (unsigned)timeout_ms);
    } else {
        printf("Failed to start LE scan: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}

static int cmd_le_scan_stop(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t ret = esp_bt_audio_le_scan_stop();
    if (ret == ESP_OK) {
        printf("LE scan stopped\n");
    } else {
        printf("Failed to stop LE scan: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}

static int cmd_le_connect(int argc, char **argv)
{
    uint8_t bda[6] = {0};
    uint8_t addr_type = 0;
    uint32_t timeout_ms = CONFIG_GMF_EXAMPLE_LE_SCAN_TIMEOUT_MS;

    if (argc < 3) {
        printf("Usage: le_connect <addr_type> <mac_address> [timeout_ms]\n");
        printf("Example: le_connect 0 01:02:03:04:05:06 10000\n");
        return 1;
    }
    addr_type = (uint8_t)strtoul(argv[1], NULL, 0);
    if (sscanf(argv[2], "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
               &bda[0], &bda[1], &bda[2], &bda[3], &bda[4], &bda[5]) != 6) {
        printf("Invalid MAC address format. Use: XX:XX:XX:XX:XX:XX\n");
        return 1;
    }
    if (argc >= 4) {
        timeout_ms = (uint32_t)strtoul(argv[3], NULL, 0);
    }
    esp_err_t ret = esp_bt_audio_le_connect(addr_type, bda, timeout_ms);
    if (ret == ESP_OK) {
        printf("LE connect started (%s)\n", argv[2]);
    } else {
        printf("Failed to start LE connect: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}

static int cmd_le_disconnect(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t ret = esp_bt_audio_le_disconnect();
    if (ret == ESP_OK) {
        printf("LE disconnect requested\n");
    } else {
        printf("Failed to disconnect LE link: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}
#endif  /* CONFIG_GMF_EXAMPLE_AUDIO_TECH_LE */

#if CONFIG_BT_CLASSIC_ENABLED && defined(CONFIG_GMF_EXAMPLE_A2DP_SOURCE)
static int cmd_start_discovery(int argc, char **argv)
{
    memset(target_device_name, 0, sizeof(target_device_name));

    if (argc == 2) {
        strncpy(target_device_name, argv[1], sizeof(target_device_name) - 1);
        target_device_name[sizeof(target_device_name) - 1] = '\0';
        printf("Discovery started - looking for device: %s\n", target_device_name);
    } else if (argc == 1) {
        printf("Discovery started - scanning for all devices\n");
    } else {
        printf("Usage: start_discovery [device_name]\n");
        printf("  device_name: Optional. If provided, will auto-connect to this device\n");
        return 1;
    }

    esp_err_t ret = esp_bt_audio_classic_discovery_start();
    if (ret == ESP_OK) {
        if (strlen(target_device_name) > 0) {
            printf("Will auto-connect to '%s' when found\n", target_device_name);
        }
    } else {
        printf("Failed to start discovery: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}

static int cmd_stop_discovery(int argc, char **argv)
{
    esp_err_t ret = esp_bt_audio_classic_discovery_stop();
    if (ret == ESP_OK) {
        printf("Discovery stopped\n");
    } else {
        printf("Failed to stop discovery: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}

static int cmd_start_media(int argc, char **argv)
{
    esp_err_t ret = esp_bt_audio_media_start(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, NULL);
    if (ret == ESP_OK) {
        printf("Media started\n");
    } else {
        printf("Failed to start media: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}

static int cmd_stop_media(int argc, char **argv)
{
    esp_err_t ret = esp_bt_audio_media_stop(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC);
    if (ret == ESP_OK) {
        printf("Media stopped\n");
    } else {
        printf("Failed to stop media: %s\n", esp_err_to_name(ret));
    }
    return ret == ESP_OK ? 0 : 1;
}

/*
 * I2S-Eingang der Vampire nach Bluetooth schicken.
 *
 * Reihenfolge: connect -> start_media -> i2s_media. Der Wunsch wird gemerkt und
 * beim naechsten A2DP-Stream ausgewertet, weil ohne Stream kein Encoder-Ziel
 * existiert. Mit "i2s_media off" laesst sich der Zweig wieder anhalten.
 */
static int cmd_i2s_media(int argc, char **argv)
{
    if (!i2s2bt_is_ready()) {
        printf("I2S input is not available (io_i2s was not created)\n");
        return 1;
    }
    if (argc > 1 && (strcmp(argv[1], "off") == 0 || strcmp(argv[1], "stop") == 0)) {
        i2s2bt_stop();
        printf("I2S pipeline stopped\n");
        return 0;
    }
    i2s2bt_request();
    printf("I2S input requested - it starts with the next A2DP stream.\n");
    printf("Use 'connect <mac>' and 'start_media' first, or 'i2s_media off' to stop.\n");
    return 0;
}
static int cmd_mixer(int argc, char **argv)
{
    if (argc >= 2) {
        int prefill = atoi(argv[1]);
        int transit = (argc >= 3) ? atoi(argv[2]) : -1;
        if (prefill < 0) {
            printf("prefill_ms darf nicht negativ sein (ist: %s)\n", argv[1]);
            return 1;
        }
        i2s2bt_set_mixer_wait(prefill, transit);
    }

    int prefill = 0;
    int transit = 0;
    i2s2bt_get_mixer_wait(&prefill, &transit);
    printf("Mischer-Wartezeiten: prefill %d ms, transit %d ms\n", prefill, transit);
    printf("Aendern: 'mixer <prefill_ms> [transit_ms]' - wirkt beim naechsten 'start_media'.\n");
    return 0;
}

#endif  /* CONFIG_BT_CLASSIC_ENABLED && defined(CONFIG_GMF_EXAMPLE_A2DP_SOURCE) */
void cli_register_bt(void){
    const esp_console_cmd_t play_cmd = {
        .command = "play",
        .help = "Send media play command",
        .hint = NULL,
        .func = &cmd_playback_play,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&play_cmd));

    const esp_console_cmd_t pause_cmd = {
        .command = "pause",
        .help = "Send media pause command",
        .hint = NULL,
        .func = &cmd_playback_pause,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&pause_cmd));

    const esp_console_cmd_t stop_cmd = {
        .command = "stop",
        .help = "Send media stop command",
        .hint = NULL,
        .func = &cmd_playback_stop,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&stop_cmd));

    const esp_console_cmd_t next_cmd = {
        .command = "next",
        .help = "Send media next track command",
        .hint = NULL,
        .func = &cmd_playback_next,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&next_cmd));

    const esp_console_cmd_t prev_cmd = {
        .command = "prev",
        .help = "Send media previous track command",
        .hint = NULL,
        .func = &cmd_playback_prev,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&prev_cmd));

    const esp_console_cmd_t playfile_cmd = {
        .command = "playfile",
        .help = "Play a file from the SD card into the mixer (Vampire stays audible)",
        .hint = "[datei, Standard test2.mp3]",
        .func = &cmd_playfile,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&playfile_cmd));

    const esp_console_cmd_t metadata_cmd = {
        .command = "metadata",
        .help = "Request media metadata with mask",
        .hint = "<mask>",
        .func = &cmd_playback_metadata,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&metadata_cmd));

    const esp_console_cmd_t vol_set_cmd = {
        .command = "vol_set",
        .help = "Set volume level (0-100)",
        .hint = "<volume>",
        .func = &cmd_volume_set,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&vol_set_cmd));

    const esp_console_cmd_t vol_up_cmd = {
        .command = "vol_up",
        .help = "Increase volume by 10",
        .hint = NULL,
        .func = &cmd_volume_up,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&vol_up_cmd));

    const esp_console_cmd_t vol_down_cmd = {
        .command = "vol_down",
        .help = "Decrease volume by 10",
        .hint = NULL,
        .func = &cmd_volume_down,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&vol_down_cmd));

#if CONFIG_BT_CLASSIC_ENABLED && CONFIG_GMF_EXAMPLE_AUDIO_TECH_CLASSIC
    {
        const esp_console_cmd_t connect_cmd = {
            .command = "connect",
            .help = "Connect to a Bluetooth device",
            .hint = "<mac_address>",
            .func = &cmd_connect_device,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&connect_cmd));
    }

    {
        const esp_console_cmd_t disconnect_cmd = {
            .command = "disconnect",
            .help = "Disconnect from current Bluetooth device",
            .hint = NULL,
            .func = &cmd_disconnect_device,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&disconnect_cmd));
    }

    {
        const esp_console_cmd_t hf_connect_cmd = {
            .command = "hf_connect",
            .help = "Connect HFP HF to a Bluetooth device",
            .hint = "<mac_address>",
            .func = &cmd_hf_connect,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&hf_connect_cmd));
    }

    {
        const esp_console_cmd_t hf_disconnect_cmd = {
            .command = "hf_disconnect",
            .help = "Disconnect HFP HF from current Bluetooth device",
            .hint = NULL,
            .func = &cmd_hf_disconnect,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&hf_disconnect_cmd));
    }
#endif  /* CONFIG_BT_CLASSIC_ENABLED && CONFIG_GMF_EXAMPLE_AUDIO_TECH_CLASSIC */

    const esp_console_cmd_t call_answer_cmd = {
        .command = "call_answer",
        .help = "Answer incoming call",
        .hint = NULL,
        .func = &cmd_call_answer,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&call_answer_cmd));

    const esp_console_cmd_t call_reject_cmd = {
        .command = "call_reject",
        .help = "Reject / terminate call",
        .hint = NULL,
        .func = &cmd_call_reject,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&call_reject_cmd));

    const esp_console_cmd_t call_dial_cmd = {
        .command = "call_dial",
        .help = "Dial number; omit for redial",
        .hint = "[number]",
        .func = &cmd_call_dial,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&call_dial_cmd));

    const esp_console_cmd_t pb_fetch_cmd = {
        .command = "pb_fetch",
        .help = "Fetch phonebook or call history, target 1-5 = main_pb/incoming/outgoing/missed/combined",
        .hint = "<target> [start_idx] [count]",
        .func = &cmd_pb_fetch,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&pb_fetch_cmd));

#if CONFIG_BT_CLASSIC_ENABLED && defined(CONFIG_GMF_EXAMPLE_A2DP_SOURCE)
    const esp_console_cmd_t start_discovery_cmd = {
        .command = "start_discovery",
        .help = "Start Bluetooth device discovery with optional auto-connect",
        .hint = "[device_name]",
        .func = &cmd_start_discovery,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&start_discovery_cmd));

    const esp_console_cmd_t stop_discovery_cmd = {
        .command = "stop_discovery",
        .help = "Stop Bluetooth device discovery",
        .hint = NULL,
        .func = &cmd_stop_discovery,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&stop_discovery_cmd));

    const esp_console_cmd_t start_media_cmd = {
        .command = "start_media",
        .help = "Start media playback",
        .hint = NULL,
        .func = &cmd_start_media,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&start_media_cmd));

    const esp_console_cmd_t stop_media_cmd = {
        .command = "stop_media",
        .help = "Stop media playback",
        .hint = NULL,
        .func = &cmd_stop_media,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&stop_media_cmd));

    const esp_console_cmd_t i2s_media_cmd = {
        .command = "i2s_media",
        .help = "Send the I2S input (Vampire) to Bluetooth; 'i2s_media off' stops it",
        .hint = "[off]",
        .func = &cmd_i2s_media,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&i2s_media_cmd));

    {
        }

    {
        const esp_console_cmd_t mixer_cmd = {
            .command = "mixer",
            .help = "Mixer wait times: 'mixer <prefill_ms> <transit_ms>' (0 is allowed)",
            .hint = "[prefill_ms] [transit_ms]",
            .func = &cmd_mixer,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&mixer_cmd));
    }

    {
        }

    {
        }
#endif  /* CONFIG_BT_CLASSIC_ENABLED && defined(CONFIG_GMF_EXAMPLE_A2DP_SOURCE) */

#if CONFIG_GMF_EXAMPLE_AUDIO_TECH_LE
    {
        const esp_console_cmd_t le_scan_start_cmd = {
            .command = "le_scan_start",
            .help = "Start LE scan",
            .hint = "[timeout_ms]",
            .func = &cmd_le_scan_start,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&le_scan_start_cmd));
    }

    {
        const esp_console_cmd_t le_scan_stop_cmd = {
            .command = "le_scan_stop",
            .help = "Stop LE scan",
            .hint = NULL,
            .func = &cmd_le_scan_stop,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&le_scan_stop_cmd));
    }

    {
        const esp_console_cmd_t le_connect_cmd = {
            .command = "le_connect",
            .help = "Connect LE peer",
            .hint = "<addr_type> <mac_address> [timeout_ms]",
            .func = &cmd_le_connect,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&le_connect_cmd));
    }

    {
        const esp_console_cmd_t le_disconnect_cmd = {
            .command = "le_disconnect",
            .help = "Disconnect LE ACL link",
            .hint = NULL,
            .func = &cmd_le_disconnect,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&le_disconnect_cmd));
    }
#endif  /* CONFIG_GMF_EXAMPLE_AUDIO_TECH_LE */
}

static int restart(int argc, char **argv)
{
    printf("Restarting\n");
    esp_restart();
}

static int free_mem(int argc, char **argv)
{
    printf("\nFree heap size: internal %u, psram %u\nmin  heap size: internal %u, psram %u\n",
           heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    /*
     * Groesster ZUSAMMENHAENGENDER Block.
     *
     * Das ist der Wert, auf den es ankommt: der Bluetooth-Stack fordert beim
     * Senden rund 620 Byte am Stueck an. Am 03.10. war der freie Heap 25 528
     * Byte gross und genau so eine Anforderung scheiterte:
     *
     *   E BT_OSI: calloc failed (caller=0x401077df size=622)
     *   W BT_AUD_A2D_SRC: Failed to send frame batch: ESP_ERR_NO_MEM
     *
     * Ohne diesen Wert sieht man nur "es ist noch Speicher frei" - und sucht an
     * der falschen Stelle.
     */
    printf("groesster Block:        internal %u, psram %u\n",
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    /*
     * Zusaetzlich die Bereiche EINZELN (0.9.31).
     *
     * Der interne Heap besteht aus mehreren getrennten Bereichen (im Boot-Log
     * sichtbar: 5, 16, 88, 14, 111 und 24 KB). Diese Tabelle zeigt je Bereich,
     * wie viel frei ist und wie gross der groesste Block darin ist - damit ist
     * zu sehen, WELCHER Bereich zersplittert ist, statt nur "zu wenig".
     */
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);

    return 0;
}

/** 'tasks' command prints the list of tasks and related information */
static int tasks_info(int argc, char **argv)
{
    return esp_gmf_oal_sys_get_real_time_stats(1000, false);
}

static bool parse_log_level(const char *level_str, esp_log_level_t *level)
{
    if (strcmp(level_str, "none") == 0 || strcmp(level_str, "0") == 0) {
        *level = ESP_LOG_NONE;
    } else if (strcmp(level_str, "error") == 0 || strcmp(level_str, "err") == 0 || strcmp(level_str, "1") == 0) {
        *level = ESP_LOG_ERROR;
    } else if (strcmp(level_str, "warn") == 0 || strcmp(level_str, "warning") == 0 || strcmp(level_str, "2") == 0) {
        *level = ESP_LOG_WARN;
    } else if (strcmp(level_str, "info") == 0 || strcmp(level_str, "3") == 0) {
        *level = ESP_LOG_INFO;
    } else if (strcmp(level_str, "debug") == 0 || strcmp(level_str, "4") == 0) {
        *level = ESP_LOG_DEBUG;
    } else if (strcmp(level_str, "verbose") == 0 || strcmp(level_str, "5") == 0) {
        *level = ESP_LOG_VERBOSE;
    } else {
        return false;
    }
    return true;
}

static int log_level(int argc, char **argv)
{
    if (argc != 3) {
        printf("Usage: log_level <tag|*> <none|error|warn|info|debug|verbose|0-5>\n");
        printf("Example: log_level CLK_SYNC_EL debug\n");
        printf("Example: log_level * warn\n");
        return 1;
    }

    esp_log_level_t level = ESP_LOG_NONE;
    if (!parse_log_level(argv[2], &level)) {
        printf("Invalid log level: %s\n", argv[2]);
        return 1;
    }

    esp_log_level_set(argv[1], level);
    printf("Set log level: tag=%s level=%s\n", argv[1], argv[2]);
    return 0;
}

/*
 * microSD commands.
 *
 * They exist because the module has to be taken out of the board to be flashed,
 * which also disconnects the card: mounting at boot therefore fails by design
 * while the module is on the bench. With these commands the card can be brought
 * in later ("sd_mount") without flashing again.
 */
static int cmd_sd_mount(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (sd_card_is_mounted()) {
        printf("SD card is already mounted at /sdcard (via %s)\n", sd_card_transport());
        return 0;
    }
    printf("Card detect (GPIO34, low = inserted): %s\n", sd_card_is_present() ? "card inserted" : "no card / pin floating");
    esp_err_t ret = sd_card_mount();
    if (ret == ESP_OK) {
        printf("SD card mounted at /sdcard via %s\n", sd_card_transport());
        return 0;
    }
    printf("Mount failed: %s\n", esp_err_to_name(ret));
    return 1;
}

static int cmd_sd_unmount(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!sd_card_is_mounted()) {
        printf("No SD card mounted\n");
        return 0;
    }
    esp_err_t ret = sd_card_unmount();
    if (ret == ESP_OK) {
        printf("SD card unmounted\n");
        return 0;
    }
    printf("Unmount failed: %s\n", esp_err_to_name(ret));
    return 1;
}

static int cmd_sd_ls(int argc, char **argv)
{
    if (!sd_card_is_mounted()) {
        printf("No SD card mounted - run 'sd_mount' first\n");
        return 1;
    }
    const char *path = (argc > 1) ? argv[1] : "/sdcard";
    DIR *dir = opendir(path);
    if (dir == NULL) {
        printf("Cannot open %s: %s\n", path, strerror(errno));
        return 1;
    }
    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        printf("  %s%s\n", entry->d_name, (entry->d_type == DT_DIR) ? "/" : "");
        count++;
    }
    closedir(dir);
    printf("%d entries in %s\n", count, path);
    return 0;
}

/*
 * sd_mount_spi [clk mosi miso cs] - Karte ueber SPI auf BELIEBIGEN Pins.
 *
 * WOZU: Der SDMMC-Host des ESP32 hat keine GPIO-Matrix
 * (SDMMC_LL_SLOT_SUPPORT_GPIO_MATRIX = 0, esp_hal_sd/esp32/include/hal/sdmmc_ll.h:86),
 * seine Pins sind bei Slot 1 fest 14/15/2 (D1..D3 = 4/12/13) und Slot 0 sind
 * die Flash-Pins. Fuer SDMMC gibt es also keine Alternative.
 *
 * Der SPI-Weg laeuft dagegen ueber die GPIO-Matrix und darf jede Leitung
 * benutzen. Auf dem WROOM faellt er mit "send_if_cond (1) returned 0x108" aus
 * (CMD8 ohne Antwort), obwohl er den SDMMC-Block ueberhaupt nicht benutzt.
 * Mit diesem Kommando laesst sich die Karte an ANDEREN Pins anschliessen -
 * damit ist trennbar:
 *   - antwortet sie dort  -> die Pins 14/15/2/13 dieses Moduls sind die Ursache
 *   - antwortet sie auch dort nicht -> Karteninterface/Halter/Karte
 *
 * Aufruf ohne Argumente benutzt die Standardbelegung (14/15/2/13).
 */
static int cmd_sd_mount_spi(int argc, char **argv)
{
    int clk = 14, mosi = 15, miso = 2, cs = 13;

    if (argc == 5) {
        clk  = atoi(argv[1]);
        mosi = atoi(argv[2]);
        miso = atoi(argv[3]);
        cs   = atoi(argv[4]);
    } else if (argc != 1) {
        printf("Aufruf: sd_mount_spi [clk mosi miso cs]  (GPIO-Nummern)\n");
        return 1;
    }

    if (sd_card_is_mounted()) {
        printf("Es ist schon eine Karte gemountet - erst sd_unmount.\n");
        return 1;
    }

    printf("SPI-Mount: CLK=GPIO%d MOSI=GPIO%d MISO=GPIO%d CS=GPIO%d\n", clk, mosi, miso, cs);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 1000;
    host.unaligned_multi_block_rw_max_chunk_size = 8;

    spi_bus_config_t bus = {
        .mosi_io_num = mosi,
        .miso_io_num = miso,
        .sclk_io_num = clk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        printf("spi_bus_initialize: %s\n", esp_err_to_name(err));
        return 1;
    }

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SPI2_HOST;
    slot.gpio_cs = cs;
    slot.gpio_cd = SDSPI_SLOT_NO_CD;    /* GPIO34 ohne externen Pull-up ist wertlos */
    slot.gpio_wp = SDSPI_SLOT_NO_WP;

    esp_vfs_fat_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_card_t *card = NULL;
    err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mcfg, &card);
    if (err != ESP_OK) {
        printf("Mount ueber SPI fehlgeschlagen: %s\n", esp_err_to_name(err));
        spi_bus_free(SPI2_HOST);
        return 1;
    }

    printf("Ueber SPI gemountet auf /sdcard\n");
    sdmmc_card_print_info(stdout, card);
    return 0;
}

/*
 * sdpins - die drei SDMMC-Leitungen als normale GPIOs treiben und zuruecklesen.
 *
 * WOZU: Der SDMMC-Host des ESP32 hat keine GPIO-Matrix, seine Pins sind im
 * 1-Bit-Modus fest 14/15/2 (esp_hal_sd/esp32/include/soc/sdmmc_pins.h). Ein
 * Kurzschluss oder eine starke Last auf einer dieser Leitungen ist mit dem
 * Treiber nicht zu sehen - als GPIO sofort: der Ruecklesewert muss dem
 * getriebenen Pegel folgen.
 *
 * Erwartung bei intakter Leitung (die Eingaenge der Karte sind hochohmig, ihre
 * Pull-ups sind schwach):
 *     low  -> liest 0
 *     high -> liest 1
 * Folgt der Wert nicht, haelt etwas die Leitung fest oder sie ist gegen GND
 * bzw. 3,3 V kurzgeschlossen.
 *
 * Hintergrund: Im Registerverlauf (Abschnitt 12k/12m der Messreihe) geht die
 * Karte auf busy (DAT0 = LOW) und der Host sendet deshalb kein Kommando mehr -
 * genau das Bild einer festgehaltenen DAT0-Leitung.
 */
static int cmd_sdpins(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    static const int pins[3] = { SDMMC_SLOT1_IOMUX_PIN_NUM_CLK,   /* 14 */
                                 SDMMC_SLOT1_IOMUX_PIN_NUM_CMD,   /* 15 */
                                 SDMMC_SLOT1_IOMUX_PIN_NUM_D0 };  /*  2 */
    static const char *namen[3] = { "CLK", "CMD", "D0 " };

    printf("SDMMC-Leitungen als GPIO (14=CLK, 15=CMD, 2=D0):\n");

    /* Erst als Eingang lesen - zeigt, was die Leitung gerade macht. */
    for (int i = 0; i < 3; i++) {
        gpio_config_t in = {
            .pin_bit_mask = 1ULL << pins[i],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&in);
        printf("  GPIO%-2d %s als Eingang: %d\n", pins[i], namen[i], gpio_get_level(pins[i]));
    }

    /* Dann treiben und zuruecklesen. */
    int fehler = 0;
    for (int i = 0; i < 3; i++) {
        gpio_config_t out = {
            .pin_bit_mask = 1ULL << pins[i],
            .mode = GPIO_MODE_INPUT_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&out);

        gpio_set_level(pins[i], 0);
        esp_rom_delay_us(10);
        int r_low = gpio_get_level(pins[i]);
        gpio_set_level(pins[i], 1);
        esp_rom_delay_us(10);
        int r_high = gpio_get_level(pins[i]);

        bool ok = (r_low == 0) && (r_high == 1);
        if (!ok) {
            fehler++;
        }
        printf("  GPIO%-2d %s: low -> liest %d, high -> liest %d   %s\n",
               pins[i], namen[i], r_low, r_high,
               ok ? "folgt (Leitung in Ordnung)"
                  : "FOLGT NICHT - Leitung festgehalten oder kurzgeschlossen");
    }

    /* Pins wieder freigeben (der SDMMC-Treiber richtet sie selbst ein). */
    for (int i = 0; i < 3; i++) {
        gpio_config_t frei = {
            .pin_bit_mask = 1ULL << pins[i],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&frei);
    }

    printf("Ergebnis: %d von 3 Leitungen in Ordnung\n", 3 - fehler);

    /*
     * DAT1/DAT2/DAT3 mit und ohne internen Pull-up lesen.
     *
     * WOZU: DAT2 ist beim ESP32 gleichzeitig MTDI und legt beim Reset die
     * Flash-Spannung fest (ESP-IDF "SD Pullup Requirements": WROOM-32 = 3,3 V
     * Flash -> GPIO12 LOW, WROVER = 1,8 V Flash -> GPIO12 HIGH; "incompatible
     * with SD card operation"). Wohin die PLATINE diese Leitung zieht, sieht
     * man hier: bleibt der Pegel trotz Pull-up (oder Pull-down) gleich, zieht
     * ein externer Widerstaerker staerker als die internen ca. 45k.
     */
    printf("Alle SD-Leitungen: frei / mit internem Pull-up / mit internem Pull-down\n");
    printf("  (bleibt der Pegel trotz Pull-up auf 0, zieht die Platine oder das Modul");
    printf(" staerker als die internen ca. 45k)\n");
    static const int dat[6] = { 2, 14, 15, 4, 12, 13 };
    static const char *datname[6] = { "D0 ", "CLK", "CMD", "DAT1", "DAT2", "DAT3" };
    for (int i = 0; i < 6; i++) {
        gpio_config_t in = {
            .pin_bit_mask = 1ULL << dat[i],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&in);
        gpio_set_pull_mode(dat[i], GPIO_FLOATING);
        esp_rom_delay_us(50);
        int frei = gpio_get_level(dat[i]);

        gpio_set_pull_mode(dat[i], GPIO_PULLUP_ONLY);
        esp_rom_delay_us(50);
        int mit_pu = gpio_get_level(dat[i]);

        gpio_set_pull_mode(dat[i], GPIO_PULLDOWN_ONLY);
        esp_rom_delay_us(50);
        int mit_pd = gpio_get_level(dat[i]);

        printf("  GPIO%-2d %s: frei=%d, mit Pull-up=%d, mit Pull-down=%d  -> %s\n",
               dat[i], datname[i], frei, mit_pu, mit_pd,
               (frei == 1 && mit_pd == 1) ? "extern auf HIGH gezogen (stark)"
               : (frei == 0 && mit_pu == 0) ? "extern auf LOW gezogen (staerker als 45k)"
               : (mit_pu == 1 && mit_pd == 0) ? "folgt den internen Pulls (extern nichts)"
               : "undefiniert/hochohmig");
        gpio_set_pull_mode(dat[i], GPIO_FLOATING);
    }
    return fehler ? 1 : 0;
}

/*
 * scanpins - Pegel ALLER GPIOs direkt aus dem Eingangsregister lesen.
 *
 * WOZU: Damit laesst sich feststellen, welche Leitung der USB-Seriell-
 * Schnittstelle (DTR, RTS, TXD) auf welchen ESP32-Pin geht. Der Host setzt
 * dazu eine Leitung um, ruft scanpins auf und vergleicht: ein GPIO, dessen
 * Pegel mitgeht, ist mit dieser Leitung verbunden.
 *
 * WICHTIG - warum ueber die Register und nicht ueber gpio_config():
 * `gpio_config()` verweigert Pins, die ein Peripherietreiber beansprucht hat
 * (ESP_ERR_INVALID_STATE). Im ersten Versuch fehlten dadurch ausgerechnet
 * GPIO0, GPIO2, GPIO4 und GPIO5 - also genau die Pins, die DTR (GPIO0) und die
 * SD/I2S-Leitungen betreffen. Das direkte Lesen von GPIO_IN_REG/GPIO_IN1_REG
 * aendert dagegen nichts an der Konfiguration und liefert alle 40 Pins.
 *
 * Ausgabe: eine Zeile mit Hexmasken und eine Liste der acht wichtigsten Pins.
 */
static int cmd_scanpins(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    uint32_t in0 = REG_READ(GPIO_IN_REG);       /* GPIO0..31  */
    uint32_t in1 = REG_READ(GPIO_IN1_REG);      /* GPIO32..39 */

    printf("GPIO_IN=0x%08" PRIx32 " GPIO_IN1=0x%08" PRIx32 "\n", in0, in1);

    printf("Wichtige Pins:");
    static const int wichtig[] = { 0, 2, 4, 5, 12, 13, 14, 15 };
    for (unsigned i = 0; i < sizeof(wichtig) / sizeof(wichtig[0]); i++) {
        int g = wichtig[i];
        int pegel = (g < 32) ? (int)((in0 >> g) & 1) : (int)((in1 >> (g - 32)) & 1);
        printf(" %d=%d", g, pegel);
    }
    printf("\n");
    return 0;
}

/*
 * eq - Equalizer hinter dem Mischer bedienen (0.9.56).
 *
 * Aufrufe:
 *   eq                      -> Baender auflisten
 *   eq bands <0..10>        -> erste N Baender aktiv, Rest aus (0 = Durchlauf)
 *   eq set <idx> <typ> <fc> <q> <gain>
 *                           typ: 1=HighPass 2=LowPass 3=Peak 4=HighShelf 5=LowShelf
 *
 * Wozu die Bandzahl schaltbar ist: die CPU-Last haengt direkt an der Zahl der
 * Baender (Doku-Formel: (rate/8000) * kanaele * baender * base_load). Damit
 * laesst sich der echte Preis je Band auf diesem Chip messen, statt ihn aus
 * den ESP32-S3-Werten hochzurechnen.
 */
static int cmd_eq(int argc, char **argv)
{
    if (argc == 1) {
        printf("Equalizer hinter dem Mischer (max %d Baender):\n", MIXER_EQ_BANDS);
        stream_proc_eq_list();
        return 0;
    }

    if (strcmp(argv[1], "bands") == 0) {
        if (argc != 3) {
            printf("Aufruf: eq bands <0..%d>\n", MIXER_EQ_BANDS);
            return 1;
        }
        int n = atoi(argv[2]);
        if (stream_proc_eq_set_bands(n) < 0) {
            printf("Bandzahl konnte nicht gesetzt werden\n");
            return 1;
        }
        return 0;
    }

    if (strcmp(argv[1], "set") == 0) {
        if (argc != 7) {
            printf("Aufruf: eq set <idx> <typ> <fc> <q> <gain>\n");
            printf("        typ: 1=HighPass 2=LowPass 3=Peak 4=HighShelf 5=LowShelf\n");
            return 1;
        }
        int idx = atoi(argv[2]);
        int typ = atoi(argv[3]);
        unsigned fc = (unsigned)strtoul(argv[4], NULL, 10);
        float q = strtof(argv[5], NULL);
        float gain = strtof(argv[6], NULL);
        if (stream_proc_eq_set(idx, typ, fc, q, gain) < 0) {
            printf("Band konnte nicht gesetzt werden\n");
            return 1;
        }
        return 0;
    }

    printf("Unbekannt. Aufrufe: eq | eq bands <n> | eq set <idx> <typ> <fc> <q> <gain>\n");
    return 1;
}

/*
 * 'version' - welcher Stand laeuft gerade?
 *
 * Die Nummer steht in version.txt und wird bei jeder Aenderung hochgezaehlt.
 * Der Zeitstempel ist der Uebersetzungszeitpunkt dieser Datei: aendert er sich
 * nicht, wurde auch nichts neu gebaut - genau das war schon einmal die Ursache
 * fuer stundenlange Fehlersuche.
 *
 * Seit 0.9.16 steht hier auch der FLASH-TAKT - als Sollwert aus der
 * Konfiguration UND als Istwert aus den SPI-Registern.
 *
 * WARUM DER ISTWERT: der Flash-Takt hat die SD-Karte ausfallen lassen (0x107 im
 * SDMMC-Clock-Update, siehe sd_card.c). Auf einen Konfigurationsschalter allein
 * ist dabei kein Verlass - in diesem Projekt waren PSRAM- und sdkconfig-Schalter
 * nachweislich wirkungslos, weil die sdkconfig Vorrang vor den Defaults hatte.
 * Deshalb wird hier das Register gelesen, das der Treiber wirklich programmiert
 * hat.
 *
 * Herleitung (aus components/esp_hal_mspi/esp32/include/hal/spi_flash_ll.h):
 *   Quelltakt = SPI_FLASH_LL_CLOCK_FREQUENCY_MHZ = 80 MHz
 *   clkdiv == 1 -> Registerbit 31 gesetzt (kein Teiler)  -> 80 MHz
 *   sonst       -> clkdiv = (val & 0x3F) + 1             -> 80 / clkdiv
 *   clkdiv == 2 -> val = 0x1001                          -> 40 MHz
 */
static int flash_clock_mhz(uint32_t clock_val)
{
    uint32_t clkdiv = (clock_val & (1u << 31)) ? 1u : ((clock_val & 0x3Fu) + 1u);
    if (clkdiv == 0) {
        return 0;
    }
    return (int)(80u / clkdiv);
}

/*
 * sdreg - Registerdiagnose des SDMMC-Hosts.
 *
 * WOZU: Auf dem WROOM faellt der SDMMC-Weg schon im Clock-Update der CIU in
 * den Timeout - mit Karte, ohne Karte und mit abgeklemmtem SD-Modul identisch
 * (docs/MESSREIHE.md, Abschnitt 6):
 *
 *   E (2015) SD_HOST: sd_host_slot_clock_update_command(993): sd_host_start_command returned 0x107
 *
 * Der Treiber wartet an dieser Stelle darauf, dass die HARDWARE das Bit
 * SDMMC.cmd.start_command selbst wieder loescht
 * (esp_hal_sd/esp32/include/hal/sdmmc_ll.h:584-587, is_command_taken).
 * Bleibt es stehen, ist die CIU nie zum Zug gekommen - noch bevor ein Bit zur
 * Karte geht. Das ist keine Karten-, Halter- oder Verdrahtungsfrage.
 *
 * Dieses Kommando zeigt ohne Treiber, was der Block wirklich tut:
 *
 *   1. Ist das Bus-Takt-Gate des SDIO-Hosts offen? Auf dem ESP32 haengt der
 *      SDMMC-Host am WiFi/BT-Taktgate DPORT_WIFI_CLK_EN Bit 13
 *      (esp_hal_sd/esp32/include/hal/sdmmc_ll.h:122-130). Ist es zu, ist der
 *      Block tot und jede Registerabfrage laeuft in den Timeout.
 *   2. Kommt der Block aus dem Reset? (DPORT_CORE_RST_EN Bit 6 und der
 *      Reset-Handshake in SDMMC.ctrl, den sd_host_reset abfragt.)
 *   3. Nimmt die CIU ein Update-Clock-Kommando an? Das wird hier genauso
 *      aufgebaut wie in sd_host_slot_clock_update_command
 *      (sd_host_sdmmc.c:985-989): card_num, update_clk_reg, wait_complete.
 *
 * Zweckmaessig NACH einem fehlgeschlagenen "sd_mount" aufrufen, dann ist der
 * Zustand genau der des Fehlerfalls. Achtung: der Reset-Test stoert eine
 * laufende Karte - nur zur Diagnose benutzen.
 */
static const char *sdreg_fsm(uint32_t cmd_fsm_state)
{
    /* Synopsys CIU-Kommando-FSM (SDMMC.status.cmd_fsm_state). */
    switch (cmd_fsm_state) {
    case 0:  return "idle";
    case 1:  return "send_init";
    case 2:  return "send_cmd";
    case 4:  return "recv_resp";
    case 5:  return "wait_ccrc";
    case 6:  return "wait_ccrc2";
    case 7:  return "recv_resp_end";
    case 8:  return "wait_rcrc";
    case 9:  return "wait_rcrc2";
    case 10: return "wait_rcrc3";
    case 11: return "wait_ncrc";
    case 12: return "wait_ncrc2";
    case 13: return "wait_ncrc3";
    default: return "?";
    }
}

/*
 * Ist das Bus-Taktgate des SDIO-Hosts offen? Auf dem ESP32 ist das
 * DPORT_WIFI_CLK_EN Bit 13 (DPORT_WIFI_CLK_SDIO_HOST_EN), siehe
 * esp_hal_sd/esp32/include/hal/sdmmc_ll.h:122-130.
 */
static bool sdreg_gate_on(void)
{
    return (DPORT_REG_READ(DPORT_WIFI_CLK_EN_REG) & DPORT_WIFI_CLK_SDIO_HOST_EN) != 0;
}

/*
 * Ein CIU-Update-Clock-Kommando absetzen und warten, bis die Hardware
 * start_command selbst loescht - genau das, was sd_host_slot_start_command()
 * tut und was im Fehlerfall in den Timeout laeuft.
 *
 * Rueckgabe: true = Kommando angenommen, false = start_command bleibt stehen.
 */
/*
 * CMD8 absetzen und dabei die Kommando-FSM mitlesen.
 *
 * WOZU: Ein Update-Clock-Kommando laesst sich nicht von einem verlorenen
 * Schreibzugriff unterscheiden - beide lassen `start_command = 0` lesen (das
 * hat den ersten Testaufbau in die Irre gefuehrt). Ein normales Kommando mit
 * Antwortpflicht bewegt dagegen die Kommando-FSM des CIU
 * (SDMMC.status.cmd_fsm_state): steht sie nach dem Schreiben nicht mehr auf
 * "idle", hat die CIU das Kommando angenommen und arbeitet.
 */
static void sdreg_cmd8_test(const char *was)
{
    SDMMC.cmdarg = 0x1AA;               /* CMD8-Argument (VHS + Checkmuster) */

    SDMMC.cmd.cmd_index = 8;
    SDMMC.cmd.response_expect = 1;
    SDMMC.cmd.check_response_crc = 1;
    SDMMC.cmd.card_num = 1;
    SDMMC.cmd.use_hold_reg = 1;
    SDMMC.cmd.start_command = 1;

    int64_t t0 = esp_timer_get_time();
    int64_t t_clear = -1;
    uint32_t verlauf[8] = {0};
    int n_verlauf = 0;
    uint32_t letzte = 0xFF;

    while ((esp_timer_get_time() - t0) < 5000) {
        uint32_t fsm = SDMMC.status.cmd_fsm_state;
        if (fsm != letzte) {
            if (n_verlauf < 8) {
                verlauf[n_verlauf++] = fsm;
            }
            letzte = fsm;
        }
        if (SDMMC.cmd.start_command == 0) {
            t_clear = esp_timer_get_time() - t0;
            break;
        }
        esp_rom_delay_us(2);
    }

    printf("  CMD8 %s: start_command %s", was,
           SDMMC.cmd.start_command == 0 ? "geloescht" : "steht noch");
    if (t_clear >= 0) {
        printf(" nach %d us", (int)t_clear);
    } else {
        printf(" (5 ms)");
    }
    printf(", FSM-Verlauf:");
    for (int i = 0; i < n_verlauf; i++) {
        printf(" %s", sdreg_fsm(verlauf[i]));
    }
    printf("  RINTSTS=%08" PRIx32 "\n", SDMMC.rintsts.val);
}

static bool sdreg_ciu_test(int slot, int *dt_us)
{    SDMMC.clkena.val &= ~(uint32_t)BIT(slot);

    SDMMC.cmdarg = 0;
    SDMMC.cmd.card_num = slot;
    SDMMC.cmd.update_clk_reg = 1;
    SDMMC.cmd.wait_complete = 1;
    SDMMC.cmd.use_hold_reg = 1;
    SDMMC.cmd.start_command = 1;

    int64_t t0 = esp_timer_get_time();
    while (SDMMC.cmd.start_command != 0 && (esp_timer_get_time() - t0) < 200000) {
        esp_rom_delay_us(20);
    }
    *dt_us = (int)(esp_timer_get_time() - t0);
    return SDMMC.cmd.start_command == 0;
}

/*
 * Taktgate-Sampler. Laeuft auf dem zweiten Kern, waehrend der Konsolen-Task in
 * sdmmc_card_init() blockiert.
 *
 * Er protokolliert ALLE Aenderungen der fuer den Fehler wichtigen Register:
 *   start_command  - wird von der Hardware geloescht, wenn die CIU das
 *                    Kommando annimmt (sdmmc_struct.h:70)
 *   clkena         - Kartentakt (Bit n) und Low-Power (Bit 16+n)
 *   clkdiv/clksrc  - Kartenteiler
 *   clock (0x800)  - Hostteiler (div_factor_*)
 *   status         - u. a. cmd_fsm_state und data_busy
 * Damit ist sichtbar, welches der drei Update-Kommandos in
 * sd_host_slot_set_card_clk() (Zeilen 567, 590, 600) haengen bleibt und wann
 * welcher Registerwert geschrieben wurde.
 */
static volatile bool s_sdtrace_laeuft;

static void sdtrace_task(void *arg)
{
    int laufzeit_ms = (int)(intptr_t)arg;
    uint32_t l_start = 9, l_clkena = 9, l_clkdiv = 9, l_clock = 9, l_status = 9;
    int64_t t0 = esp_timer_get_time();

    printf("[sdtrace] Start: Gate=%s\n", sdreg_gate_on() ? "AN" : "AUS");
    while (s_sdtrace_laeuft && (esp_timer_get_time() - t0) < (int64_t)laufzeit_ms * 1000) {
        uint32_t start = SDMMC.cmd.start_command;
        uint32_t clkena = SDMMC.clkena.val;
        uint32_t clkdiv = SDMMC.clkdiv.val;
        uint32_t clock = SDMMC.clock.val;
        uint32_t status = SDMMC.status.val;

        if (start != l_start || clkena != l_clkena || clkdiv != l_clkdiv ||
            clock != l_clock || status != l_status) {
            printf("[sdtrace] %6d us: start=%u clkena=%08" PRIx32 " clkdiv=%08" PRIx32
                   " clock=%08" PRIx32 " status=%08" PRIx32 " (FSM=%s)\n",
                   (int)(esp_timer_get_time() - t0), (unsigned)start, clkena, clkdiv, clock,
                   status, sdreg_fsm((status >> 4) & 0xF));
            l_start = start;
            l_clkena = clkena;
            l_clkdiv = clkdiv;
            l_clock = clock;
            l_status = status;
        }
        esp_rom_delay_us(50);
    }
    printf("[sdtrace] Ende bei %d us\n", (int)(esp_timer_get_time() - t0));
    vTaskDelete(NULL);
}

/*
 * VERID des geclockten Blocks. Nur dieser Wert beweist, dass das
 * Registerinterface antwortet: bei geschlossenem Taktgate liefern ALLE
 * Register denselben Wert (0x0000b7cf gemessen), und ein "start_command = 0"
 * ist dann kein Erfolg, sondern ein verlorener Schreibzugriff.
 */
#define SDREG_VERID_LEBT  0x5342270aUL

static void sdreg_lage(const char *was)
{
    uint32_t verid = SDMMC.verid;
    printf("  %-30s VERID=%08" PRIx32 " %s  CLOCK=%08" PRIx32 " CLKDIV=%08" PRIx32
           " CLKENA=%08" PRIx32 " start=%u STATUS=%08" PRIx32 "\n",
           was, verid, (verid == SDREG_VERID_LEBT) ? "lebt" : "TOT ",
           SDMMC.clock.val, SDMMC.clkdiv.val, SDMMC.clkena.val,
           (unsigned)SDMMC.cmd.start_command, SDMMC.status.val);
}

static void sdreg_wait_reset(void)
{
    int64_t t0 = esp_timer_get_time();
    while ((SDMMC.ctrl.val & 0x7) != 0 && (esp_timer_get_time() - t0) < 100000) {
        esp_rom_delay_us(20);
    }
    printf("  Reset-Handshake: CTRL=%08" PRIx32 " nach %d us\n",
           SDMMC.ctrl.val, (int)(esp_timer_get_time() - t0));
}

static int cmd_sdreg(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /*
     * DPORT-Register duerfen NICHT mit REG_READ gelesen werden - IDF besteht
     * auf DPORT_REG_READ (soc/esp32/include/soc/soc.h:33-58, IS_DPORT_REG).
     */
    uint32_t clk_en = DPORT_REG_READ(DPORT_WIFI_CLK_EN_REG);
    printf("DPORT_WIFI_CLK_EN = 0x%08" PRIx32 "  SDIO-Host-Takt (Bit13): %s\n",
           clk_en, (clk_en & DPORT_WIFI_CLK_SDIO_HOST_EN) ? "AN" : "AUS");

    uint32_t core_rst = DPORT_REG_READ(DPORT_CORE_RST_EN_REG);
    printf("DPORT_CORE_RST_EN = 0x%08" PRIx32 "  SDIO-Host-Reset (Bit6): %s\n",
           core_rst, (core_rst & DPORT_SDIO_HOST_RST) ? "AKTIV" : "aus");

    printf("SDMMC VERID=0x%08" PRIx32 " HCON=0x%08" PRIx32
           " CTRL=0x%08" PRIx32 " CLKDIV=0x%08" PRIx32
           " CLKSRC=0x%08" PRIx32 " CLKENA=0x%08" PRIx32 " CLOCK=0x%08" PRIx32 "\n",
           SDMMC.verid, SDMMC.hcon.val, SDMMC.ctrl.val, SDMMC.clkdiv.val,
           SDMMC.clksrc.val, SDMMC.clkena.val, SDMMC.clock.val);
    printf("SDMMC STATUS=0x%08" PRIx32 " (FSM=%s, Daten-FSM %s) RINTSTS=0x%08" PRIx32
           " CDETECT=0x%08" PRIx32 " RST_N(cards)=0x%x\n",
           SDMMC.status.val, sdreg_fsm(SDMMC.status.cmd_fsm_state),
           SDMMC.status.data_fsm_busy ? "busy" : "frei",
           SDMMC.rintsts.val, SDMMC.cdetect.val, (unsigned)SDMMC.rst_n.cards);
    printf("SDMMC CMD.start_command=%u update_clk_reg=%u wait_complete=%u card_num=%u\n",
           (unsigned)SDMMC.cmd.start_command, (unsigned)SDMMC.cmd.update_clk_reg,
           (unsigned)SDMMC.cmd.wait_complete, (unsigned)SDMMC.cmd.card_num);
    printf("  Karte laut CDETECT: Karte0 %s, Karte1 %s\n",
           (SDMMC.cdetect.cards & BIT(0)) ? "fehlt" : "gesteckt",
           (SDMMC.cdetect.cards & BIT(1)) ? "fehlt" : "gesteckt");

    /*
     * Taktlage. Der SDMMC-Host haengt auf dem ESP32 FEST an PLL_F160M
     * (PLL/3): sdmmc_ll_select_clk_source() ist ein No-Op
     * (esp_hal_sd/esp32/include/hal/sdmmc_ll.h:218-221) und
     * esp_clk_tree_enable_src() fuehrt fuer PLL_F160M nur einen Zaehler, ohne
     * ein Register anzufassen (esp_hw_support/port/esp32/esp_clk_tree.c:114-153).
     * Und esp_clk_tree_src_get_freq_hz(PLL_F160M) liefert nur die KONSTANTE
     * CLK_LL_PLL_160M_FREQ_MHZ (ebd.:44-46) - also den Wert, der im Treiberlog
     * als "src_freq_hz: 160000000" erscheint. Das ist eine Annahme, kein
     * Messwert. Deshalb hier die tatsaechliche Lage: CPU, APB, XTAL und
     * PLL_D2 (PLL_D2 = PLL/2, daraus folgt die PLL selbst).
     */
    uint32_t f_xtal = 0, f_plld2 = 0, f_f160 = 0;
    esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_XTAL, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &f_xtal);
    esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_PLL_D2, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &f_plld2);
    esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_PLL_F160M, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &f_f160);
    printf("Takt: CPU=%d Hz, APB=%d Hz, XTAL=%u Hz, PLL_D2=%u Hz -> PLL=%u Hz, PLL_F160M=%u Hz\n",
           esp_clk_cpu_freq(), esp_clk_apb_freq(), (unsigned)f_xtal,
           (unsigned)f_plld2, (unsigned)(f_plld2 * 2), (unsigned)f_f160);
    printf("Flash-Register: SPI0.clock=0x%08" PRIx32 " SPI1.clock=0x%08" PRIx32 "\n",
           SPI0.clock.val, SPI1.clock.val);

    /*
     * ERSTE FRAGE: oeffnet der Treiber das Taktgate ueberhaupt, wenn es zu ist?
     * Bisher ist nur bewiesen, dass es AN *bleibt*, wenn ich es vorher selbst
     * gesetzt habe. Also: Gate erzwingen zu, dann sdmmc_host_init() und nachsehen.
     */
    DPORT_REG_CLR_BIT(DPORT_WIFI_CLK_EN_REG, DPORT_WIFI_CLK_SDIO_HOST_EN);
    printf("Gate zwangsweise AUS -> jetzt %s\n", sdreg_gate_on() ? "AN (leider)" : "AUS");
    esp_err_t rc_t = sdmmc_host_init();
    printf("sdmmc_host_init() aus AUS: 0x%x -> Gate jetzt %s  %s\n", rc_t,
           sdreg_gate_on() ? "AN" : "AUS",
           sdreg_gate_on() ? "(Treiber oeffnet das Gate)"
                           : "(TREIBER OEFFNET DAS GATE NICHT - das ist die Ursache)");
    sdreg_lage("nach sdmmc_host_init aus AUS");
    DPORT_REG_SET_BIT(DPORT_WIFI_CLK_EN_REG, DPORT_WIFI_CLK_SDIO_HOST_EN);

    /*
     * Der Block wird von Hand in den arbeitsfaehigen Zustand gebracht:
     * Taktgate auf (DPORT_WIFI_CLK_EN Bit 13) und Modul-Reset.
     */
    printf("Taktgate gesetzt -> %s\n", sdreg_gate_on() ? "AN" : "AUS (Schreiben wirkungslos!)");

    SDMMC.ctrl.controller_reset = 1;
    SDMMC.ctrl.fifo_reset = 1;
    SDMMC.ctrl.dma_reset = 1;
    sdreg_wait_reset();
    sdreg_lage("nach Reset");

    int dt = 0;
    bool ok = sdreg_ciu_test(1, &dt);
    printf("CIU mit Init-Teilern: %d us, start_command=%u -> %s\n", dt,
           (unsigned)SDMMC.cmd.start_command, ok ? "angenommen" : "haengt");
    sdreg_lage("nach CIU (Init-Teiler)");

    /*
     * Jetzt die Schritte aus sd_host_slot_set_card_clk() (sd_host_sdmmc.c:565-595)
     * EINZELN nachfahren. Verdacht: das Neuprogrammieren von SDMMC.clock (0x800)
     * stoppt den Kern des Blocks, danach nimmt die CIU nichts mehr an - genau das
     * Bild aus dem Fehlerlog. Fuer 400 kHz (Probing, erster Zugriff im
     * Karten-Init) rechnet der Treiber host_div=10, card_div=20
     * (sd_host_sdmmc.c:1125-1127); sdmmc_ll_set_clock_div(10) schreibt
     * div_factor_h = 9, div_factor_l = 4, div_factor_n = 9
     * (esp_hal_sd/esp32/include/hal/sdmmc_ll.h:229-247).
     */
    SDMMC.clkena.val &= ~(uint32_t)BIT(1);
    sdreg_lage("Kartentakt aus");

    SDMMC.clksrc.card1 = 1;
    SDMMC.clkdiv.div1 = 20;
    sdreg_lage("Kartenteiler 20");

    SDMMC.clock.div_factor_h = 9;
    SDMMC.clock.div_factor_l = 4;
    SDMMC.clock.div_factor_n = 9;
    SDMMC.clock.phase_dout = 4;
    SDMMC.clock.phase_din = 4;
    SDMMC.clock.phase_core = 0;
    sdreg_lage("Hostteiler 10 (clock 0x800)");

    ok = sdreg_ciu_test(1, &dt);
    printf("CIU mit 400-kHz-Teilern: %d us, start_command=%u -> %s\n", dt,
           (unsigned)SDMMC.cmd.start_command, ok ? "angenommen" : "haengt");
    sdreg_lage("nach CIU (400-kHz-Teiler)");

    /* Gegenprobe: zurueck auf die Teiler aus dem Treiber-Init (div = 2). */
    SDMMC.clock.div_factor_h = 1;
    SDMMC.clock.div_factor_l = 0;
    SDMMC.clock.div_factor_n = 1;
    sdreg_lage("Hostteiler 2 (clock 0x800)");
    ok = sdreg_ciu_test(1, &dt);
    printf("CIU mit Init-Teiler 2: %d us, start_command=%u -> %s\n", dt,
           (unsigned)SDMMC.cmd.start_command, ok ? "angenommen" : "haengt");

    /*
     * Und jetzt der echte Treiberweg. Nach jedem Schritt steht die Lebendigkeit
     * des Blocks dabei - damit ist zu sehen, ob der Treiber ihn umbringt.
     */
    printf("Treiberweg:\n");
    esp_err_t rc = sdmmc_host_init();
    printf("  sdmmc_host_init():       0x%x (Gate %s)\n", rc, sdreg_gate_on() ? "AN" : "AUS");
    sdreg_lage("nach sdmmc_host_init");

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk   = GPIO_NUM_14;
    slot.cmd   = GPIO_NUM_15;
    slot.d0    = GPIO_NUM_2;
    slot.width = 1;
    slot.cd    = GPIO_NUM_34;
    slot.wp    = SDMMC_SLOT_NO_WP;
    slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    rc = sdmmc_host_init_slot(1, &slot);
    printf("  sdmmc_host_init_slot(1): 0x%x\n", rc);
    sdreg_lage("nach sdmmc_host_init_slot");

    ok = sdreg_ciu_test(1, &dt);
    printf("  CIU nach Treiber-Init: %d us, start_command=%u -> %s\n", dt,
           (unsigned)SDMMC.cmd.start_command, ok ? "angenommen" : "haengt");

    /*
     * Der eigentliche Karten-Init - der Aufruf, der im Feld 0x107 liefert.
     * Ein zweiter Task tastet dabei das Taktgate ab.
     */
    s_sdtrace_laeuft = true;
    xTaskCreatePinnedToCore(sdtrace_task, "sdtrace", 3072, (void *)(intptr_t)1500, 10, NULL, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = 4000;   /* wie SD_MAX_FREQ_KHZ in sd_card.c */
    sdmmc_card_t card = {0};
    esp_err_t rc5 = sdmmc_card_init(&host, &card);
    printf("  sdmmc_card_init():       0x%x\n", rc5);
    sdreg_lage("nach sdmmc_card_init");
    vTaskDelay(pdMS_TO_TICKS(1700));    /* Sampler auslaufen lassen */
    s_sdtrace_laeuft = false;

    /*
     * Test 7: verarbeitet die CIU ueberhaupt Kommandos - und braucht sie dafuer
     * den Kartentakt? Beides einmal mit abgeschaltetem und einmal mit
     * eingeschaltetem cclk, jeweils aus einem frisch resetteten Block.
     */
    printf("Test 7 CIU-Kommandoverarbeitung (CMD8):\n");
    SDMMC.ctrl.controller_reset = 1;
    sdreg_wait_reset();
    SDMMC.clkena.val &= ~(uint32_t)BIT(1);          /* wie sd_host_slot_set_card_clk */
    sdreg_cmd8_test("bei Kartentakt AUS");

    SDMMC.ctrl.controller_reset = 1;
    sdreg_wait_reset();
    SDMMC.clkena.val |= (uint32_t)BIT(1);
    sdreg_cmd8_test("bei Kartentakt AN ");
    SDMMC.clkena.val &= ~(uint32_t)BIT(1);
    return 0;
}

static int cmd_version(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("V4_ESP32 Version %s\n", APP_VERSION_STRING);
    printf("  uebersetzt am %s um %s\n", __DATE__, __TIME__);
    printf("  ESP-IDF %s\n", esp_get_idf_version());

    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);
    printf("  Chip: %s, %d Kern(e), Revision %d\n", CONFIG_IDF_TARGET, chip.cores, chip.revision);

    /*
     * Flash-Takt: Soll (Konfiguration) und Ist (Register).
     * SPI0 ist laut soc/spi_struct.h "for internal use" und wird vom
     * Cache-/Flash-Controller benutzt; SPI1 ist der Host, den der
     * Flash-Treiber in IDF anspricht. Beide werden gezeigt, damit kein Zweifel
     * bleibt, welcher Wert wohin gehoert.
     */
    printf("  Flash: Soll %s aus der Konfiguration\n", CONFIG_ESPTOOLPY_FLASHFREQ);
    uint32_t c0 = SPI0.clock.val;
    uint32_t c1 = SPI1.clock.val;
    printf("  Flash: Ist  SPI0.clock=0x%08x -> %d MHz\n", (unsigned)c0, flash_clock_mhz(c0));
    printf("  Flash: Ist  SPI1.clock=0x%08x -> %d MHz\n", (unsigned)c1, flash_clock_mhz(c1));
    return 0;
}

void cli_register_sys()
{
    static const esp_console_cmd_t cmds[] = {
        {
            .command = "version",
            .help = "Show firmware version and build time",
            .hint = NULL,
            .func = &cmd_version,
        },
        {
            .command = "restart",
            .help = "Software reset of the chip",
            .hint = NULL,
            .func = &restart,
        },
        {
            .command = "free",
            .help = "Get the current size of free heap memory",
            .hint = NULL,
            .func = &free_mem,
        },
        {
            .command = "tasks",
            .help = "Get information about running tasks",
            .hint = NULL,
            .func = &tasks_info,
        },
        {
            .command = "log_level",
            .help = "Set runtime log level for a tag or *",
            .hint = "<tag|*> <level>",
            .func = &log_level,
        },
        {
            .command = "sd_mount",
            .help = "Mount the microSD card at /sdcard",
            .hint = NULL,
            .func = &cmd_sd_mount,
        },
        {
            .command = "sdreg",
            .help = "Dump SDMMC host registers and test the CIU clock update",
            .hint = NULL,
            .func = &cmd_sdreg,
        },
        {
            .command = "sd_mount_spi",
            .help = "Mount the SD card over SPI on any pins",
            .hint = "[clk mosi miso cs]",
            .func = &cmd_sd_mount_spi,
        },
        {
            .command = "sdpins",
            .help = "Drive the SDMMC lines as GPIOs and read them back",
            .hint = NULL,
            .func = &cmd_sdpins,
        },
        {
            .command = "eq",
            .help = "Equalizer behind the mixer: list, bands <n>, set <idx> <typ> <fc> <q> <gain>",
            .hint = "[bands <n> | set <idx> <typ> <fc> <q> <gain>]",
            .func = &cmd_eq,
        },
        {
            .command = "scanpins",
            .help = "Read all usable GPIOs as inputs (find wired serial lines)",
            .hint = NULL,
            .func = &cmd_scanpins,
        },
        {
            .command = "sd_unmount",
            .help = "Unmount the microSD card",
            .hint = NULL,
            .func = &cmd_sd_unmount,
        },
        {
            .command = "sd_ls",
            .help = "List a directory on the microSD card",
            .hint = "[path]",
            .func = &cmd_sd_ls,
        }};

    for (int i = 0; i < sizeof(cmds) / sizeof(esp_console_cmd_t); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
}

void cli_init()
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "BTAudio >";
#if CONFIG_ESP_CONSOLE_UART
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t cdc_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&cdc_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t usbjtag_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usbjtag_config, &repl_config, &repl));
#endif  /* CONFIG_ESP_CONSOLE_UART */

    cli_register_sys();
    cli_register_bt();

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

void cli_bt_device_found(const char *name, const uint8_t *bda)
{
#if CONFIG_BT_CLASSIC_ENABLED && CONFIG_GMF_EXAMPLE_A2DP_SOURCE
    if (strlen(target_device_name) > 0) {
        if (strstr(name, target_device_name) != NULL) {
            ESP_LOGI(TAG, "Found target device '%s', initiating auto-connect...", target_device_name);
            esp_bt_audio_classic_discovery_stop();
            esp_err_t ret = esp_bt_audio_classic_connect(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, (uint8_t *)bda);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "Connecting to %s...\n", name);
                memset(target_device_name, 0, sizeof(target_device_name));
            } else {
                ESP_LOGE(TAG, "Failed to connect to %s: %s\n", name, esp_err_to_name(ret));
            }
        }
    }
#endif  /* CONFIG_BT_CLASSIC_ENABLED && CONFIG_GMF_EXAMPLE_A2DP_SOURCE */
}

void cli_bt_device_conn_st_chg(const uint8_t *bda, bool connected)
{
    if (connected) {
        memcpy(target_device_bda, bda, sizeof(target_device_bda));
    } else {
        memset(target_device_bda, 0, sizeof(target_device_bda));
    }
}