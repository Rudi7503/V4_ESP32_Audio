/*
 * bt_manager.c - Bluetooth-Zustand und Geraeteliste fuer das V4-Protokoll.
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Der Vertrag in bt_manager.h stammt aus dem Vorgaengerprojekt
 * (ESP32-I2S-to-BT). Dort war bt_manager.c eine eigene Schicht auf Bluedroid
 * GAP, weil das Projekt die Inquiry selbst gefahren hat. In diesem Projekt
 * liegen Bluetooth und A2DP in der Komponente esp_bt_audio, und main.c ist die
 * einzige Ereignissenke - es gibt keine zweite GAP-Callback-Registrierung, die
 * Bluedroid ablehnen wuerde.
 *
 * Deshalb ist diese Datei NEU geschrieben: sie haelt nur noch den Zustand und
 * die Geraetetabelle, die das I2C-Protokoll der Vampire abfragen kann. Alles,
 * was die Komponente selbst kann (Scannen, Verbinden, Trennen), wird an
 * esp_bt_audio_classic_* durchgereicht.
 *
 * Was gegenuber dem Vorgaengerprojekt NICHT mehr geht, steht ausdruecklich im
 * Code, damit niemand eine Faehigkeit annimmt, die es nicht gibt:
 *   - Inquiry-Laenge und Dauerbetrieb: esp_bt_audio_classic_discovery_start()
 *     hat keine Parameter. Die Laenge bestimmt die Komponente; der Wunsch des
 *     Masters wird nur mitgefuehrt und im Log gemeldet. Den Dauerbetrieb bilden
 *     wir selbst nach (Neustart, wenn die Komponente das Ende meldet).
 *   - Auto-Scan nach dem Verbinden ist hier standardmaessig AUS. Im
 *     Vorgaengerprojekt lief er an; das kostet aber CPU und kann den Ton
 *     stocken lassen, und der Master kann ihn per SCAN_START jederzeit
 *     anfordern.
 */

#include <string.h>

#include "esp_log.h"
#include "esp_err.h"

#include "esp_bt_defs.h"
#include "esp_gap_bt_api.h"        /* esp_bt_gap_remove_bond_device() - FORGET */

#include "esp_bt_audio_classic.h"
#include "esp_bt_audio_defs.h"
#include "esp_timer.h"
#include "nvs.h"

#include "v4_proto.h"
#include "bt_manager.h"

static const char *TAG = "bt_mgr";

/* ------------------------------------------------------------------ */
/* Zustand                                                            */
/* ------------------------------------------------------------------ */

/*
 * Die Geraetetabelle ist bewusst NICHT an den Verbindungszustand gekoppelt und
 * wird nie automatisch geleert - sonst waere die Liste waehrend einer laufenden
 * Verbindung leer (das war im Urprojekt der Fehler, den bt_manager behoben hat).
 */
static bt_mgr_dev_t  s_devs[BT_MGR_MAX_DEVICES];
static int           s_dev_count;
static uint16_t      s_scan_gen;

static volatile bool s_discovering;
static volatile bool s_connected;
static volatile bool s_connecting;
static volatile bool s_suspended;
static volatile bool s_streaming;

static uint8_t       s_conn_bda[ESP_BD_ADDR_LEN];

/* --- Autoverbindung (0.9.66) ---------------------------------------------- */
#define BT_MGR_NVS_NAMESPACE  "v4bt"
#define BT_MGR_NVS_KEY_PEER   "peer"

static uint8_t        s_saved_bda[ESP_BD_ADDR_LEN];
static bool           s_saved_valid;
static volatile bool  s_peer_dirty;          /* neu verbunden, noch nicht im NVS */
static volatile bool  s_want_autoconnect = true;  /* false nach DISCONNECT/FORGET */
static int64_t        s_last_try_us;
static int64_t        s_connect_since_us;   /* Beginn des laufenden Versuchs */
#define BT_MGR_CONNECT_TIMEOUT_MS  20000u   /* Notausstieg, siehe tick() */
static bool          s_auto_scan;
static bool          s_initialized;

/* Wunsch des Masters, siehe Kopfkommentar */
static uint8_t       s_scan_units = BT_MGR_SCAN_DEF_UNITS;
static bool          s_scan_continuous;

static int find_bda(const uint8_t *bda)
{
    for (int i = 0; i < s_dev_count; i++) {
        if (memcmp(s_devs[i].bda, bda, ESP_BD_ADDR_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

static bool bda_is_zero(const uint8_t *bda)
{
    for (int i = 0; i < ESP_BD_ADDR_LEN; i++) {
        if (bda[i] != 0) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Start                                                              */
/* ------------------------------------------------------------------ */

/*
 * Gegenstelle im eigenen NVS ablegen bzw. lesen (0.9.66).
 *
 * Der Schreibvorgang laeuft NICHT im BT-Ereigniskontext: dort stehen nur
 * CONFIG_BT_BTC_TASK_STACK_SIZE (3072) Byte zur Verfuegung, und NVS-Schreiben
 * braucht eigenen Platz. bt_mgr_evt_connection() setzt deshalb nur s_peer_dirty,
 * geschrieben wird in bt_mgr_autoconnect_tick() (stream_proc_task, 4096 Byte).
 */
static void nvs_save_peer(const uint8_t *bda)
{
    nvs_handle_t h;

    if (nvs_open(BT_MGR_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS nicht verfuegbar - Gegenstelle nicht gemerkt");
        return;
    }
    if (nvs_set_blob(h, BT_MGR_NVS_KEY_PEER, bda, ESP_BD_ADDR_LEN) == ESP_OK) {
        (void)nvs_commit(h);
    } else {
        ESP_LOGW(TAG, "Gegenstelle konnte nicht geschrieben werden");
    }
    nvs_close(h);
}

static bool nvs_load_peer(uint8_t *bda)
{
    nvs_handle_t h;
    size_t       len = ESP_BD_ADDR_LEN;
    bool         ok  = false;

    if (nvs_open(BT_MGR_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    if (nvs_get_blob(h, BT_MGR_NVS_KEY_PEER, bda, &len) == ESP_OK
        && len == ESP_BD_ADDR_LEN && !bda_is_zero(bda)) {
        ok = true;
    }
    nvs_close(h);
    return ok;
}

static void nvs_clear_peer(void)
{
    nvs_handle_t h;

    if (nvs_open(BT_MGR_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    (void)nvs_erase_key(h, BT_MGR_NVS_KEY_PEER);
    (void)nvs_commit(h);
    nvs_close(h);
}

esp_err_t bt_mgr_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    s_initialized = true;
    s_dev_count = 0;
    s_scan_gen = 0;
    memset(s_conn_bda, 0, sizeof(s_conn_bda));

    s_saved_valid = nvs_load_peer(s_saved_bda);
    if (s_saved_valid) {
        ESP_LOGI(TAG, "gemerkte Gegenstelle %02X:%02X:%02X:%02X:%02X:%02X - "
                      "Autoverbindung alle %u s",
                 s_saved_bda[0], s_saved_bda[1], s_saved_bda[2],
                 s_saved_bda[3], s_saved_bda[4], s_saved_bda[5],
                 (unsigned)(BT_MGR_RETRY_MS / 1000u));
    }

    ESP_LOGI(TAG, "Geraeteliste bereit (max %d Eintraege), Auto-Scan %s",
             BT_MGR_MAX_DEVICES, s_auto_scan ? "an" : "aus");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Ereignisse aus main.c                                              */
/* ------------------------------------------------------------------ */

void bt_mgr_evt_discovered(const char *name, const uint8_t *bda)
{
    if (bda == NULL) {
        return;
    }
    const char *n = (name != NULL) ? name : "";
    size_t len = strlen(n);
    if (len > BT_MGR_NAME_LEN) {
        len = BT_MGR_NAME_LEN;
    }

    int idx = find_bda(bda);
    if (idx < 0) {
        if (s_dev_count >= BT_MGR_MAX_DEVICES) {
            /* Tabelle voll: lieber einen Eintrag verlieren als die Liste
             * umwerfen - die Indizes, die der Master schon gelesen hat, bleiben
             * damit gueltig. */
            ESP_LOGD(TAG, "Geraetetabelle voll (%d), neuer Fund verworfen", BT_MGR_MAX_DEVICES);
            return;
        }
        idx = s_dev_count++;
        memcpy(s_devs[idx].bda, bda, ESP_BD_ADDR_LEN);
        s_devs[idx].name[0] = '\0';
        s_devs[idx].name_len = 0;
    }

    /* Namen ergaenzen oder aendern. Nur eine echte Aenderung zaehlt als neue
     * Generation - sonst holt der Master die Liste bei jedem Fund erneut. */
    if (len != s_devs[idx].name_len || memcmp(s_devs[idx].name, n, len) != 0) {
        memcpy(s_devs[idx].name, n, len);
        s_devs[idx].name[len] = '\0';
        s_devs[idx].name_len = (uint8_t)len;
        s_scan_gen++;
    }
}

void bt_mgr_evt_discovery(bool discovering)
{
    s_discovering = discovering;

    /*
     * Dauerbetrieb selbst nachbilden: esp_bt_audio_classic_discovery_start()
     * hat keine Parameter, also gibt es in der Komponente keinen "bis auf
     * weiteres"-Modus. Der Master wuenscht ihn ueber SCAN_START(mode Bit0=1);
     * hier wird nach dem Ende der Inquiry neu gestartet.
     */
    if (!discovering && s_scan_continuous) {
        esp_err_t err = esp_bt_audio_classic_discovery_start();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Dauer-Scan liess sich nicht neu starten: %s - Scan beendet",
                     esp_err_to_name(err));
            s_scan_continuous = false;
        }
    }
}

void bt_mgr_evt_connection(bool connected, const uint8_t *bda)
{
    if (connected) {
        if (bda != NULL) {
            memcpy(s_conn_bda, bda, ESP_BD_ADDR_LEN);
        }
        s_connected = true;
        s_connecting = false;
        s_connect_since_us = 0;
        s_suspended = false;

        /*
         * Verbinden bricht einen laufenden Scan ab: die Inquiry stoert den
         * Verbindungsaufbau (so steht es in PROTOCOL_V4.md, Abschnitt 12).
         */
        if (s_discovering) {
            s_scan_continuous = false;
            esp_bt_audio_classic_discovery_stop();
            s_discovering = false;
        }

        /*
         * Auto-Scan (standardmaessig AUS, siehe Kopfkommentar): nach dem
         * Verbindungsaufbau im Dauerbetrieb weiterscannen, damit die
         * Geraeteliste auch waehrend des Streamens aktuell bleibt. Das kann den
         * Ton kurz stocken lassen - deshalb nicht voreingestellt.
         */
        if (s_auto_scan) {
            s_scan_continuous = true;
            if (esp_bt_audio_classic_discovery_start() == ESP_OK) {
                s_discovering = true;
            } else {
                s_scan_continuous = false;
            }
        }
        /*
         * Erfolgreiche Verbindung merken (0.9.66). Nur den Auftrag setzen - das
         * NVS-Schreiben erledigt bt_mgr_autoconnect_tick(), weil dieser Kontext
         * (BT-Ereignis) zu wenig Stack hat.
         */
        if (!s_saved_valid || memcmp(s_saved_bda, s_conn_bda, ESP_BD_ADDR_LEN) != 0) {
            memcpy(s_saved_bda, s_conn_bda, ESP_BD_ADDR_LEN);
            s_saved_valid = true;
            s_peer_dirty  = true;
        }
        s_want_autoconnect = true;

        ESP_LOGI(TAG, "verbunden mit %02X:%02X:%02X:%02X:%02X:%02X",
                 s_conn_bda[0], s_conn_bda[1], s_conn_bda[2],
                 s_conn_bda[3], s_conn_bda[4], s_conn_bda[5]);
    } else {
        if (s_connected) {
            ESP_LOGI(TAG, "Verbindung getrennt");
        }
        s_connected = false;
        s_connecting = false;
        s_connect_since_us = 0;
        s_suspended = false;
        s_streaming = false;
        memset(s_conn_bda, 0, sizeof(s_conn_bda));
    }
}

void bt_mgr_evt_stream(bool streaming)
{
    if (s_streaming != streaming) {
        ESP_LOGI(TAG, "A2DP-Stream %s", streaming ? "laeuft" : "haelt an");
    }
    s_streaming = streaming;
}

/* ------------------------------------------------------------------ */
/* Scan                                                               */
/* ------------------------------------------------------------------ */

esp_err_t bt_mgr_scan_start(uint8_t inq_units, uint8_t mode)
{
    s_want_autoconnect = false;   /* 0.9.87: der Scan darf nicht gegen die Autoverbindung kaempfen */
    if (inq_units == 0) {
        inq_units = BT_MGR_SCAN_DEF_UNITS;
    }
    if (inq_units > BT_MGR_SCAN_MAX_UNITS) {
        inq_units = BT_MGR_SCAN_MAX_UNITS;
    }
    s_scan_units = inq_units;
    s_scan_continuous = (mode & BT_MGR_SCAN_CONTINUOUS) != 0;

    esp_err_t err = esp_bt_audio_classic_discovery_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "discovery_start: %s", esp_err_to_name(err));
        s_scan_continuous = false;
        return err;
    }
    s_discovering = true;

    /* Ehrlich melden, was der Wunsch ist und was die Komponente daraus macht. */
    ESP_LOGI(TAG, "Scan gestartet (Wunsch: %u Einheiten, %s; die Inquiry-Laenge "
                  "bestimmt esp_bt_audio selbst)",
             (unsigned)inq_units, s_scan_continuous ? "Dauerbetrieb" : "einmalig");
    return ESP_OK;
}

esp_err_t bt_mgr_scan_stop(void)
{
    s_scan_continuous = false;

    esp_err_t err = esp_bt_audio_classic_discovery_stop();
    if (err == ESP_ERR_INVALID_STATE) {
        /* Lief gerade nichts - kein Fehler fuer den Master. */
        s_discovering = false;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "discovery_stop: %s", esp_err_to_name(err));
        return err;
    }
    s_discovering = false;
    ESP_LOGI(TAG, "Scan gestoppt");
    return ESP_OK;
}

bool bt_mgr_scan_active(void)
{
    return s_discovering;
}

uint16_t bt_mgr_scan_generation(void)
{
    return s_scan_gen;
}

void bt_mgr_set_auto_scan(bool on)
{
    s_auto_scan = on;
    ESP_LOGI(TAG, "Auto-Scan nach dem Verbinden: %s", on ? "an" : "aus");
}

/* ------------------------------------------------------------------ */
/* Geraeteliste                                                       */
/* ------------------------------------------------------------------ */

int bt_mgr_dev_count(void)
{
    return s_dev_count;
}

bool bt_mgr_dev_copy(int idx, bt_mgr_dev_t *out)
{
    if (out == NULL || idx < 0 || idx >= s_dev_count) {
        return false;
    }
    *out = s_devs[idx];
    return true;
}

/* ------------------------------------------------------------------ */
/* Verbindung                                                         */
/* ------------------------------------------------------------------ */

esp_err_t bt_mgr_connect_bda(const uint8_t *bda)
{
    if (bda == NULL || bda_is_zero(bda)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Ein laufender Scan stoert den Verbindungsaufbau - erst beenden. */
    if (s_discovering) {
        s_scan_continuous = false;
        esp_bt_audio_classic_discovery_stop();
        s_discovering = false;
    }

    /* Die Komponente will uint8_t*, nicht const uint8_t*. */
    uint8_t addr[ESP_BD_ADDR_LEN];
    memcpy(addr, bda, sizeof(addr));

    s_connecting = true;
    s_connect_since_us = esp_timer_get_time();
    s_suspended = false;
    s_want_autoconnect = true;   /* ein Wunsch schaltet die Autoverbindung wieder ein */
    memcpy(s_conn_bda, addr, sizeof(addr));

    esp_err_t err = esp_bt_audio_classic_connect(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, addr);
    if (err != ESP_OK) {
        s_connecting = false;
        ESP_LOGW(TAG, "connect %02X:%02X:%02X:%02X:%02X:%02X: %s",
                 addr[0], addr[1], addr[2], addr[3], addr[4], addr[5],
                 esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Verbinde mit %02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    return ESP_OK;
}

esp_err_t bt_mgr_connect_index(int idx)
{
    if (idx < 0 || idx >= s_dev_count) {
        return ESP_ERR_INVALID_ARG;
    }
    return bt_mgr_connect_bda(s_devs[idx].bda);
}

esp_err_t bt_mgr_disconnect(void)
{
    /*
     * Ausdrueckliches Trennen schaltet die Autoverbindung ab (0.9.66) - sonst
     * waere die Gegenstelle 15 s spaeter wieder da, was niemand erwartet. Die
     * gemerkte Adresse bleibt: beim naechsten Start wird wieder verbunden.
     */
    s_want_autoconnect = false;

    if (!s_connected && !s_connecting) {
        return ESP_OK;
    }
    uint8_t addr[ESP_BD_ADDR_LEN];
    memcpy(addr, s_conn_bda, sizeof(addr));

    esp_err_t err = esp_bt_audio_classic_disconnect(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, addr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "disconnect: %s", esp_err_to_name(err));
        return err;
    }
    /* Der Zustandswechsel kommt als Ereignis nach - hier nichts vorwegnehmen. */
    ESP_LOGI(TAG, "Trennen angefordert");
    return ESP_OK;
}

esp_err_t bt_mgr_forget(void)
{
    /*
     * Erst trennen, dann die Bindung loeschen. esp_bt_gap_remove_bond_device()
     * ist die Bluedroid-Schnittstelle dafuer; esp_bt_audio bietet dafuer nichts
     * an. Sie ist erlaubt, weil beide denselben Host benutzen.
     */
    if (s_connected || s_connecting) {
        bt_mgr_disconnect();
    }
    if (bda_is_zero(s_conn_bda)) {
        ESP_LOGW(TAG, "forget: keine Gegenstelle bekannt");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_bt_gap_remove_bond_device(s_conn_bda);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Bond loeschen fehlgeschlagen: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Bindung geloescht: %02X:%02X:%02X:%02X:%02X:%02X",
             s_conn_bda[0], s_conn_bda[1], s_conn_bda[2],
             s_conn_bda[3], s_conn_bda[4], s_conn_bda[5]);
    /* Auch unsere eigene Ablage leeren (0.9.66) - sonst wuerde der naechste
     * Start die geloeschte Gegenstelle wieder ansprechen. */
    nvs_clear_peer();
    s_saved_valid = false;
    memset(s_saved_bda, 0, sizeof(s_saved_bda));
    s_want_autoconnect = false;

    memset(s_conn_bda, 0, sizeof(s_conn_bda));
    return ESP_OK;
}

bool bt_mgr_is_connected(void)
{
    return s_connected;
}

int bt_mgr_connected_index(void)
{
    if (!s_connected || bda_is_zero(s_conn_bda)) {
        return V4P_BT_NO_INDEX;
    }
    int idx = find_bda(s_conn_bda);
    return (idx >= 0) ? idx : (int)V4P_BT_NO_INDEX;
}

const uint8_t *bt_mgr_remote_bda(void)
{
    return s_conn_bda;
}

v4p_bt_state_t bt_mgr_state(void)
{
    if (s_connected) {
        return s_suspended ? V4P_BT_SUSPENDED : V4P_BT_CONNECTED;
    }
    if (s_connecting) {
        return V4P_BT_CONNECTING;
    }
    if (s_discovering) {
        return V4P_BT_SCANNING;
    }
    return V4P_BT_IDLE;
}

bool bt_mgr_audio_streaming(void)
{
    return s_streaming;
}

bool bt_mgr_autoconnect_saved(uint8_t *bda_out)
{
    if (!s_saved_valid) {
        return false;
    }
    if (bda_out != NULL) {
        memcpy(bda_out, s_saved_bda, ESP_BD_ADDR_LEN);
    }
    return true;
}

void bt_mgr_autoconnect_tick(void)
{
    int64_t now = esp_timer_get_time();

    /* Neue Gegenstelle nachtragen; das NVS-Schreiben gehoert hierher und nicht
     * in den BT-Ereigniskontext (siehe nvs_save_peer). */
    if (s_peer_dirty) {
        s_peer_dirty = false;
        nvs_save_peer(s_saved_bda);
    }

    /*
     * Notausstieg: liefert ein Verbindungsversuch gar kein Ereignis (Gegenstelle
     * aus, kein Page-Timeout-Ereignis), bliebe s_connecting stehen und die
     * Autoverbindung waere blockiert. Nach BT_MGR_CONNECT_TIMEOUT_MS gilt der
     * Versuch als gescheitert.
     */
    if (s_connecting) {
        if (s_connect_since_us != 0
            && (now - s_connect_since_us) > (int64_t)BT_MGR_CONNECT_TIMEOUT_MS * 1000) {
            ESP_LOGW(TAG, "Verbindungsversuch ohne Ereignis nach %u s - neuer Versuch",
                     (unsigned)(BT_MGR_CONNECT_TIMEOUT_MS / 1000u));
            s_connecting = false;
            s_connect_since_us = 0;
        } else {
            return;
        }
    }

    if (!s_want_autoconnect || !s_saved_valid || s_connected
        || s_discovering) {
        s_last_try_us = now;
        return;
    }
    if (s_last_try_us != 0 && (now - s_last_try_us) < (int64_t)BT_MGR_RETRY_MS * 1000) {
        return;
    }
    s_last_try_us = now;

    ESP_LOGI(TAG, "Autoverbindung: Versuch %02X:%02X:%02X:%02X:%02X:%02X (alle %u s)",
             s_saved_bda[0], s_saved_bda[1], s_saved_bda[2],
             s_saved_bda[3], s_saved_bda[4], s_saved_bda[5],
             (unsigned)(BT_MGR_RETRY_MS / 1000u));
    (void)bt_mgr_connect_bda(s_saved_bda);
}
