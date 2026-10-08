/*
 * bt_manager.h - Bluetooth Classic discovery / connection state machine.
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Replaces the ad-hoc logic of the old firmware where discovery was never
 * restarted once a device had been selected and where every disconnect ended in
 * esp_restart(). Here the device table is persistent and independent of the
 * connection state, so the master can always ask for the list, also while a
 * stream is running.
 */

#ifndef BT_MANAGER_H
#define BT_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_bt_defs.h"

#include "v4_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Same value as ESP_BT_GAP_MAX_BDNAME_LEN, repeated here so that this header
 * does not have to pull in esp_gap_bt_api.h for every includer. bt_manager.c
 * asserts at compile time that the two still match. */
#define BT_MGR_NAME_LEN     248

/*
 * Groesse der Geraetetabelle (0.9.62).
 *
 * Der Vertrag (v4_proto.h, V4P_MAX_DEVICES) laesst 16 Eintraege zu je 256 Byte
 * zu; die Tabelle liegt aber im knappen internen RAM des Moduls ohne PSRAM und
 * kostet damit 4 KB. Genau diese 4 KB fehlten dem MP3-Dekoder, wenn der
 * Datei-Zweig startete (docs/MP3_STARTFEHLER.md).
 *
 * Acht gemerkte Geraete reichen fuer den Anwendungsfall. Der Vertrag bleibt
 * unangetastet: GET_INFO.max_devices meldet die tatsaechlich unterstuetzte
 * Zahl, und die Vampire liest diesen Wert (sie prueft nicht auf 16).
 */
/*
 * 8 -> 4 (0.9.74): Die BT-Profile HFP/GOEPCS/Cover-Art kosten rund 12 KB
 * internen Speicher (gemessen: 87232 Byte frei ohne sie, 74808 mit ihnen).
 * Sie muessen aber AN bleiben - ohne sie verdoppelt sich die CPU-Last des
 * Mischer-Tasks (17 % -> 33 %, Mitschnitte /tmp/eq_test.log gegen
 * /tmp/audio_973.log). Die 4 KB fuer den MP3-Start kommen deshalb hier und bei
 * zwei weiteren Puffern her. Vier gemerkte Geraete reichen; die Bindung selbst
 * liegt weiterhin in Bluedroids NVS.
 */
#define BT_MGR_MAX_DEVICES  2      /* 0.9.78: 4 -> 2, je Eintrag ~256 Byte */

typedef struct {
    esp_bd_addr_t bda;
    char          name[BT_MGR_NAME_LEN + 1];
    uint8_t       name_len;
} bt_mgr_dev_t;

/* Scan modes for bt_mgr_scan_start() */
#define BT_MGR_SCAN_ONCE        0x00    /* stop when the inquiry window ends   */
#define BT_MGR_SCAN_CONTINUOUS  0x01    /* restart until bt_mgr_scan_stop()    */

/** Maximum inquiry window, in units of 1.28 s (Bluetooth spec limit 0x30). */
#define BT_MGR_SCAN_MAX_UNITS   0x30
#define BT_MGR_SCAN_DEF_UNITS   0x08    /* ~10 s */

/**
 * @brief Initialise NVS, load a previously stored peer address and register
 *        the GAP callback. Requires the BT controller and Bluedroid to be
 *        enabled already (done by app_main).
 */
esp_err_t bt_mgr_init(void);

/**
 * @brief Start a Classic BT inquiry. Devices accumulate in the table; existing
 *        entries are never dropped, so the list survives across scans.
 *
 * @param inq_units  inquiry length in 1.28 s units, clamped to
 *                   [1, BT_MGR_SCAN_MAX_UNITS]
 * @param mode       BT_MGR_SCAN_ONCE or BT_MGR_SCAN_CONTINUOUS
 */
esp_err_t bt_mgr_scan_start(uint8_t inq_units, uint8_t mode);

/** @brief Stop an ongoing inquiry. Safe to call when idle. */
esp_err_t bt_mgr_scan_stop(void);

/** @brief Number of devices currently in the table. */
int bt_mgr_dev_count(void);

/**
 * @brief Copy one table entry out.
 *
 * @return true if idx was in range.
 */
bool bt_mgr_dev_copy(int idx, bt_mgr_dev_t *out);

/**
 * @brief Generation counter of the device table. The master polls this cheaply
 *        and only re-reads the list when it changed.
 */
uint16_t bt_mgr_scan_generation(void);

bool bt_mgr_scan_active(void);

/** @brief Connect to the entry at idx. Sets the state to V4P_BT_CONNECTING. */
esp_err_t bt_mgr_connect_index(int idx);

/** @brief Connect to an explicit address (6 bytes, ESP_BD_ADDR_LEN). */
esp_err_t bt_mgr_connect_bda(const uint8_t *bda);

/** @brief Tear down the current A2DP link. Does NOT reboot the ESP32. */
esp_err_t bt_mgr_disconnect(void);

/** @brief Disconnect (if needed) and erase the stored peer address from NVS. */
esp_err_t bt_mgr_forget(void);

/**
 * @brief Enable auto-scan: whenever an A2DP link comes up, a continuous inquiry
 *        is started so the master always sees a current device list, even while
 *        streaming. The master can still stop it with SCAN_STOP.
 */
void bt_mgr_set_auto_scan(bool on);

/** @brief True while an A2DP link is established. */
bool bt_mgr_is_connected(void);

/** @brief Index of the connected device in the table, or V4P_BT_NO_INDEX. */
int bt_mgr_connected_index(void);

/** @brief Address of the current peer, valid when bt_mgr_is_connected(). */
const uint8_t *bt_mgr_remote_bda(void);

/** @brief Current state as reported to the master. */
v4p_bt_state_t bt_mgr_state(void);

/** @brief True while audio is actually streaming (A2DP started). */
bool bt_mgr_audio_streaming(void);

/*
 * Autoverbindung (0.9.66)
 *
 * Die zuletzt erfolgreich verbundene Gegenstelle liegt in unserem eigenen NVS
 * (Bluedroid haelt die Bindung ohnehin dort, aber ohne eigene Ablage kann die
 * Firmware nicht selbst entscheiden, wen sie nach einem Neustart anspricht und
 * ob sie es erneut versuchen soll).
 *
 * Ablauf: Beim Start verbindet Bluedroid die gebundene Gegenstelle von sich aus
 * (beobachtet: rund 5 s nach dem Boot). Klappt das nicht - die Senke ist z.B.
 * noch aus -, versucht bt_mgr_autoconnect_tick() es alle BT_MGR_RETRY_MS erneut,
 * solange bis eine Verbindung steht. Der Takt kommt aus stream_proc_task; die
 * I2C-Bruecke ist davon unabhaengig und wartet nie darauf.
 *
 * Nach einem ausdruecklichen DISCONNECT oder FORGET wird nicht automatisch
 * weiterverbunden (s_want_autoconnect).
 */
#define BT_MGR_RETRY_MS     15000u

/**
 * @brief Gemerkte Gegenstelle kopieren.
 *
 * @param[out] bda_out 6 Byte Puffer, darf NULL sein (dann nur die Abfrage)
 * @return true, wenn eine Gegenstelle gemerkt ist
 */
bool bt_mgr_autoconnect_saved(uint8_t *bda_out);

/**
 * @brief Autoverbindung antreiben (aus einer regulaessigen Schleife rufen).
 *
 * Schreibt eine neu verbundene Gegenstelle ins NVS und startet nach
 * BT_MGR_RETRY_MS einen neuen Verbindungsversuch, wenn keine steht. Kehrt
 * sofort zurueck; blockiert nie (der Verbindungsaufbau laeuft asynchron).
 */
void bt_mgr_autoconnect_tick(void);

/*
 * Ereignis-Eingang (0.9.57).
 *
 * Im Vorgaengerprojekt rief der ADF-Ereignisverteiler die Haken
 * bt_mgr_on_bt_connected/disconnected/suspended auf. In diesem Projekt gibt es
 * nur EINE Ereignissenke (bt_audio_event_cb in main.c); sie reicht die
 * Bluetooth-Ereignisse hierher weiter. Die Funktionen laufen damit im
 * Bluetooth-Ereignis-Task und halten sich kurz (keine blockierenden Aufrufe,
 * keine Ausgabe im Normalfall).
 */
void bt_mgr_evt_discovered(const char *name, const uint8_t *bda);
void bt_mgr_evt_discovery(bool discovering);
void bt_mgr_evt_connection(bool connected, const uint8_t *bda);
void bt_mgr_evt_stream(bool streaming);

#ifdef __cplusplus
}
#endif

#endif /* BT_MANAGER_H */
