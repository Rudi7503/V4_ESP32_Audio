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
#include "soc/spi_struct.h"     /* SPI0/SPI1.clock.val - Flash-Takt in cmd_version */

#include "version.h"

#include "esp_bt_audio_defs.h"
#include "esp_bt_audio_classic.h"
#include "v4_link.h"
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
    stream_proc_set_media_autostart(true);   /* 0.9.68: gewollt, also wieder erlauben */
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
    stream_proc_set_media_autostart(false);  /* 0.9.68: ausdruecklich aus */
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
/** 'bufs' - Zustand der Ringpuffer und der Zweige, dann die CPU-Last. */
static int bufs_info(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    stream_proc_buffer_report();
    printf("--- CPU-Last ueber 1000 ms ---\n");
    return esp_gmf_oal_sys_get_real_time_stats(1000, false);
}

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

/*
 * Bring-up-Hilfen fuer die I2C-Bruecke zur Vampire V4 (0.9.57).
 *
 * v4_selftest faehrt die echte Befehlskette einmal ohne I2C-Master durch:
 * Rahmen bauen -> pruefen -> verteilen -> Antwortrahmen bauen -> pruefen,
 * einschliesslich der BUSY-Runde und des BULK-Rahmens. Nur die I2C-Leitung
 * selbst bleibt ungeprueft. Damit laesst sich die Strecke VOR dem Anschluss der
 * Vampire pruefen.
 *
 * v4_bus zeigt, ob ueberhaupt etwas auf dem Bus ankommt. "nichts angekommen"
 * (Verdrahtung, Adresse) und "angekommen, aber verworfen" (Takt, Rahmen) sind
 * zwei verschiedene Fehler mit entgegengesetzter Ursache - deshalb wird beides
 * getrennt gezaehlt.
 */
static int cmd_v4_selftest(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    v4_link_selftest();
    return 0;
}

static int cmd_v4_bus(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    v4_link_bus_report();
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
            .command = "bufs",
            .help = "Ringpuffer, Mischer, Zweige und CPU-Last",
            .func = &bufs_info,
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
            .command = "eq",
            .help = "Equalizer behind the mixer: list, bands <n>, set <idx> <typ> <fc> <q> <gain>",
            .hint = "[bands <n> | set <idx> <typ> <fc> <q> <gain>]",
            .func = &cmd_eq,
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
        },
        {
            .command = "v4_selftest",
            .help = "Drive the V4 I2C command path once without a master",
            .hint = NULL,
            .func = &cmd_v4_selftest,
        },
        {
            .command = "v4_bus",
            .help = "Report the I2C bus state to the Vampire (wiring vs. framing)",
            .hint = NULL,
            .func = &cmd_v4_bus,
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