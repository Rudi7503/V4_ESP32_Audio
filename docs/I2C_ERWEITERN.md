# Wie eine Konsolen-Funktion ueber I2C zur Vampire kommt

Kurz: **es braucht immer beide Seiten.** Ein neuer Befehl existiert erst, wenn
der ESP32 ihn versteht *und* der V4-Master ihn spricht. Die vorhandenen 25
Befehle (PING … RESET) sind dagegen schon auf beiden Seiten fertig - die brauchen
keine Aenderung, nur den laufenden Port.

## 1. Was die V4 heute schon kann

Der Vertrag (`v4_proto.h`, beide Fassungen) kennt diese Befehle; der V4-Master
hat fuer jeden eine Funktion (`v4_<name>()` in `v4_master.h`) und das
Konsolenprogramm eine Bedienung:

| Gruppe | Befehle |
|---|---|
| Kennenlernen | `PING` 0x01, `GET_STATUS` 0x02, `GET_INFO` 0x03 |
| Bluetooth | `SCAN_START` 0x10, `SCAN_STOP` 0x11, `DEV_COUNT` 0x12, `DEV_GET` 0x13, `CONNECT` 0x20, `CONNECT_BDA` 0x21, `DISCONNECT` 0x22, `FORGET` 0x23 |
| Karte | `SD_MOUNT` 0x30, `SD_INFO` 0x31, `SET_CHUNK` 0x32 |
| Pfad/Dateien | `PATH_CLEAR` 0x38, `PATH_APPEND` 0x39, `DIR_OPEN` 0x40, `DIR_NEXT` 0x41, `DIR_CLOSE` 0x42, `FILE_OPEN` 0x50, `FILE_READ` 0x51, `FILE_CLOSE` 0x52 |
| Wiedergabe | `PLAY_FILE` 0x60, `STOP_PLAY` 0x61, `RESET` 0x7E |

## 2. Die Luecken - Konsolenbefehl gegen Protokoll

Die Konsole des ESP32 kann mehr als das Protokoll. „Aufwand ESP32" heisst: der
Aufruf, den es schon gibt, wird nur im Dispatch von `v4_link.c` eingetragen.

| Konsolenbefehl | Protokoll | Aufwand ESP32 | Aufwand V4 |
|---|---|---|---|
| `connect <mac>` | `CONNECT`/`CONNECT_BDA` | - | - |
| `disconnect` | `DISCONNECT` | - | - |
| `start_discovery` / `stop_discovery` | `SCAN_START` / `SCAN_STOP` | - | - |
| `sd_mount` / `sd_ls` | `SD_MOUNT` / `DIR_*` | - | - |
| `playfile <datei>` | `PLAY_FILE` | - | - |
| **`start_media` / `stop_media`** | **fehlt** | klein: `esp_bt_audio_media_start/stop()` | neu: Funktion + Menue + Tests |
| **`eq list/bands/set`** | **fehlt** | klein + **Lese-Rueckweg fehlt** (`stream_proc_eq_list` druckt nur) | neu, mit Nutzlast-Layout |
| `i2s_media [off]` | fehlt | klein: `i2s2bt_request()` / `i2s2bt_stop()` | neu |
| `mixer` (lesen/setzen) | fehlt | klein: `i2s2bt_set/get_mixer_wait()` | neu |
| `vol_set` / `vol_up` / `vol_down` | fehlt | klein: `esp_bt_audio_vol_set_*()` | neu |
| `play` / `pause` / `next` / `prev` | fehlt | klein: `esp_bt_audio_playback_*()` | neu |
| `metadata [maske]` | fehlt | klein: `esp_bt_audio_playback_request_metadata()` | neu |
| `sd_unmount` | fehlt | klein: `sd_fs_unmount()` | neu |
| `free` / `tasks` | fehlt | klein: `esp_get_free_heap_size()` usw. | neu (besser **ein** `GET_DIAG`) |
| `log_level`, `restart` | fehlt | klein: `esp_log_level_set()`, `esp_restart()` | neu |
| `sdreg`, `sdpins`, `scanpins`, `sd_mount_spi` | - | **entfernt (0.9.58/0.9.59)** | - |
| `v4_selftest`, `v4_bus` | - | bleibt: Bring-up-Hilfen fuer **diese** Bruecke, nur ueber die Konsole | - |
| `hf_*`, `call_*`, `pb_fetch`, `le_*` | - | **nicht vorsehen** | - |

Die letzte Zeile ist Absicht: HFP/Telefonbuch/LE Audio gehoeren nicht zu einem
A2DP-Sender. Sie ueber I2C erreichbar zu machen, vergroessert nur die
Angriffsflaeche. Die Platinendiagnose in der Zeile darueber ist inzwischen
komplett aus dem Projekt entfernt (sie hat den fehlenden DAT0-Pull-up gefunden
und wurde danach nicht mehr gebraucht) - sie war nie fuer den Bus gedacht.

## 3. Fuer **einen** neuen Befehl ist das zu tun

### ESP32-Seite (`V4_ESP32_Audio`)

1. `main/v4_proto.h`: Befehlscode aus einem freien Bereich eintragen
   (frei sind 0x04-0x0F, 0x14-0x1F, 0x24-0x2F, 0x33-0x37, 0x3A-0x3F, 0x43-0x4F,
   0x53-0x5F, 0x62-0x7D, 0x7F). Bei Antworten mit Nutzlast: Offsets definieren.
2. `main/v4_link.c`: `case V4P_CMD_<NEU>:` im Dispatch. Der Aufruf ist meist
   schon da und nicht `static` (siehe Tabelle) - also nur Argumente pruefen,
   Antwort fuellen.
3. **Langsames hinter BUSY legen.** Alles, was einen Pipeline-Umbau oder eine
   Dateisystem-Operation ausloest, darf nicht vor der Antwort laufen. Das Muster
   steht schon im Code: Antwort mit `V4P_ST_BUSY`, Arbeit in den Arbeitstask
   (`defer_op_t`, `work_msg_t`), Master wiederholt den **kompletten** Befehl.
   Betrifft mindestens `start_media`, EQ-Umbau, `sd_unmount`.
4. `main/cmd_reg.c`: nur wenn es noch keinen nicht-statischen Aufruf gibt. Sonst
   nichts - die Konsole bleibt unveraendert.
5. `main/CMakeLists.txt`: nichts (die Datei ist schon drin).
6. `v4_selftest` um den neuen Befehl erweitern, damit er ohne Vampire pruefbar
   ist.

### V4-Seite (`ApolloCrossDev/Projects/I2C-test/v4_master`)

1. `v4_proto.h`: denselben Befehlscode und dasselbe Nutzlast-Layout eintragen.
   **Achtung: das ist eine zweite Datei, kein gemeinsamer Header** (siehe
   `docs/I2C_BRUECKE.md` §4) - beide Fassungen muessen gleich bleiben.
2. `v4_proto.c`: Codec-Funktion fuer die Nutzlast (`v4p_enc_*` / `v4p_dec_*`),
   ausschliesslich byteweise ueber `v4p_put_*_le` / `v4p_get_*_le`.
3. `v4_master.c/.h`: `uint8_t v4_<name>(v4_master_t *m, ...)` nach dem Muster der
   vorhandenen Funktionen (`v4_transact` + Statuspruefung + BUSY-Wiederholung).
4. `v4_mock.c`: **den Befehl im Emulator ergaenzen** - sonst kann keine der
   Wirtsproben ihn abdecken.
5. `v4_console.c`: Bedienung im Menue.
6. Tests: `test_proto.c` (Golden Frame + Zahlencode-Pinning),
   `test_master.c` (Verhalten gegen den Mock), bei BUSY: `test_session.c`.
7. `make verify` muss gruen bleiben: `lint`, `test`, `test-san`, `smoke`, `asm`
   (nur `move.b` auf Puffer), `amiga` (`-Wall -Wextra -Werror`),
   `check-no-moviw`, `probe`. Diese Gates sind der Grund, warum die V4-Seite
   mehr Arbeit ist als die ESP32-Seite.
8. `PROTOCOL_V4_SYNC.md` fortschreiben - das ist die verbindliche Fassung, nicht
   `PROTOCOL_V4.md`.

## 4. Wovon der Master erfaehrt, was der Slave kann

Heute verraten `PING`/`GET_INFO` nur Rahmenlaengen, `chunk`, `proto_ver` und
`fw_ver`. Fuer neue Befehle ist das zu wenig: eine V4 mit neuem Programm an einem
ESP32 mit alter Firmware muss das erkennen koennen.

**Die zwei reservierten Bytes sind dafuer schon da.** Die GET_INFO-Nutzlast ist
12 Byte; die Bytes +10/+11 sind auf beiden Seiten ausdruecklich reserviert
(`v4p_info_payload_t` bzw. `v4p_info_t.reserved[2]`). Dort gehoert eine
**Faehigkeitsliste als u16 LE** hin, z. B.:

```
Bit 0  start_media/stop_media      Bit 4  vol_*
Bit 1  eq (list/bands/set)         Bit 5  playback play/pause/next/prev
Bit 2  i2s_media                   Bit 6  get_diag (free/tasks)
Bit 3  mixer                       Bit 7  sd_unmount / log_level / restart
```

Der alte Slave schickt dort 0 (alles unbekannt), der alte Master liest sie nicht.
Damit laesst sich jede Erweiterung einzeln ausrollen, ohne die Gegenstelle
mitzuziehen. Dazu `V4P_FW_VERSION` erhoehen (steht heute auf 0x02), weil sich die
Bedeutung der reservierten Bytes aendert.

## 5. Reihenfolge

1. **Zuerst den Port auf Hardware verifizieren** - flashen, `v4_bus`,
   `v4_selftest`, dann `v4_probe` von der V4. Ohne diese Basis ist jede
   Erweiterung auf Sand gebaut. (Der Stand uebersetzt, ist aber nie gelaufen.)
2. **Stufe 1 - Betrieb:** `start_media` / `stop_media`. Ohne die kann die V4
   verbinden und eine Datei waehlen, aber nichts hoeren.
3. **Stufe 2 - Klang:** `eq` samt Lese-Rueckweg (dafuer fehlt auf ESP32-Seite
   eine Funktion, die den Zustand *zurueckgibt* statt ihn zu drucken), danach
   `mixer` und `i2s_media`.
4. **Stufe 3 - Komfort:** `vol_*`, `play`/`pause`/`next`/`prev`, `metadata`.
5. **Stufe 4 - Diagnose:** ein einziger `GET_DIAG` (Heap, Taskliste) statt
   mehrerer Befehle; `sd_unmount`, `log_level`, `restart`.
6. **Offen und riskant, unabhaengig davon:** `FILE_READ` (BULK) ist auf **keiner**
   Seite je auf Hardware gelaufen (siehe `docs/I2C_BRUECKE.md` §6). Wenn die V4
   Dateien ueber I2C lesen soll, ist das der unerprobte Weg - erst danach lohnt
   sich ein „Datei auf die V4 holen" als Feature.

Jede Stufe einzeln: eine Unbekannte pro Aenderung (`docs/ARBEITSWEISE.md`,
Regel 2), also erst auf Hardware pruefen, dann die naechste.
