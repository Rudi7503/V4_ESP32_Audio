# I2C-Bruecke Vampire V4 ⇄ ESP32 (ab 0.9.57)

Diese Datei beschreibt den I2C-Weg, ueber den die Vampire V4 den ESP32
**bedient** - im Unterschied zum I2S-Weg, ueber den sie ihm **Ton liefert**.

```
        Vampire V4 (m68k, AmigaOS)                 ESP32
   +--------------------------------+      +--------------------------------+
   |  i2c.library  -> I2C-MASTER    | I2C  |  I2C-SLAVE 0x50  -> v4_link.c  |
   |  v4_master/ (I2C-test-Projekt) |----->|  Befehl -> bt_manager / sd_fs  |
   |                                |      |         -> audio_source        |
   +--------------------------------+      +--------------------------------+
   |  Audio-Ausgang  -> I2S-SLAVE   | I2S  |  I2S-MASTER -> Mischer -> A2DP |
   +--------------------------------+----->+--------------------------------+
```

Beide Wege existieren unabhaengig voneinander: faellt I2C aus, laeuft die
Tonbruecke weiter (und umgekehrt).

---

## 1. Verdrahtung

Aus `PIN verbindungen.txt` des Vorgaengerprojekts - sie gilt unveraendert:

| ESP32 | Vampire | Funktion |
|---|---|---|
| GND | P20 Pin 2 | GND |
| GPIO35 | P20 Pin 6 | Din (I2S-Daten V4 → ESP32) |
| GPIO5 | P20 Pin 8 | Bclk |
| GPIO25 | P20 Pin 10 | Wsel (Word Select) |
| GPIO18 | I2C Pin 2 | SDA |
| GPIO23 | I2C Pin 3 | SCL |
| GND | I2C Pin 4 | GND |

I2C-Adresse des ESP32: **0x50** (7 Bit) - auf der Leitung `0xA0` schreiben,
`0xA1` lesen. Der ESP32 kann als Slave **nicht** clock-stretchen; daraus folgt
das gesamte Umlaufmuster (Befehl schreiben → warten → lesen, siehe §3).

GPIO18/GPIO23 sind im Projekt sonst nicht belegt (SD: 14/15/2/34/13,
I2S-Eingang: 5/25/35). Geprueft mit einer Aufstellung aller `GPIO_NUM_*` im
Quellbaum.

---

## 2. Was wo liegt

| Datei | Herkunft | Aenderung |
|---|---|---|
| `main/v4_proto.h`, `main/v4_proto.c` | Vorgaengerprojekt `ESP32-I2S-to-BT/main/` | **unveraendert** uebernommen (byte-identisch) |
| `main/v4_link.h`, `main/v4_link.c` | dito | uebernommen; nur ein veralteter Kommentar zum Slave-Treiber richtiggestellt |
| `main/sd_fs.h`, `main/sd_fs.c` | dito | **angepasst**: Mounten/Kartenkontakt/Sektorgroesse kommen jetzt aus `sd_card.c`, der dateiorientierte Teil (Verzeichnis, Datei lesen) blieb |
| `main/bt_manager.h` | dito | Vertrag uebernommen; der ADF-Haken-Block wurde durch den Ereignis-Eingang dieses Projekts ersetzt |
| `main/bt_manager.c` | **neu geschrieben** | im Vorgaengerprojekt lag dort eine eigene Bluedroid-GAP-Schicht; hier haelt die Datei nur Zustand und Geraeteliste und reicht Scannen/Verbinden an `esp_bt_audio_classic_*` durch |
| `main/audio_source.h` | dito | Vertrag unveraendert |
| `main/audio_source.c` | **neu geschrieben** | liegt jetzt auf `local2bt_play_file()` / `local2bt_stop()` / `local2bt_is_playing()` |
| `main/main.c` | dieses Projekt | `bt_mgr_init()` + `v4_link_init()` in `app_main`, Ereignisse aus `bt_audio_event_cb` werden an `bt_manager` weitergereicht |
| `main/cmd_reg.c` | dieses Projekt | zwei Konsolenbefehle: `v4_selftest`, `v4_bus` |
| `main/sd_card.c`, `main/sd_card.h` | dieses Projekt | `sd_card_sector_size()` ergaenzt (fuer `SD_INFO`) |
| `main/stream_proc.c`, `main/stream_proc.h` | dieses Projekt | `local2bt_stop()`, `local2bt_is_playing()`, `local2bt_current_uri()` ergaenzt |

Der Grund fuer die Aufteilung: `v4_link.c` ist die **erprobte** Fassung
(Feldprotokolle in `I2C-test/v4_master/tests/field-log-v4-*.txt`). Sie blieb
deshalb so weit wie moeglich unangetastet; angepasst wurde nur die Umgebung, auf
die sie aufsetzt.

---

## 3. Das Protokoll in vier Zeilen

Verbindliche Fassung: `PROTOCOL_V4_SYNC.md` (Fassung 3.0, im I2C-test-Projekt und
im Vorgaengerprojekt identisch). Kurzfassung:

* **R1** Ein Command-Write ergibt genau einen Response-Read. Der ESP32 sendet
  nie unaufgefordert.
* **R2** Jede Antwort hat eine vorab bekannte, feste Laenge: 128 Byte fuer
  Steuerbefehle, `12 + chunk` fuer `FILE_READ` (`chunk` Vorgabe 128).
* **R3** Der Master liest erst, wenn die Antwort bereitliegt. Langsame Arbeit
  (SD mounten, SD-Block lesen, Datei anspielen) passiert **nach** der Antwort;
  der Befehl meldet vorher `STATUS=BUSY`, und der Master sendet ihn komplett
  erneut.

Frames: WRITE 32 Byte (Magic `0xA5`), READ 128 Byte (Magic `0x5A`), BULK
`12 + chunk` (Magic `0x5B`); CRC-8 mit Polynom `0x07` ueber alles ausser dem
CRC-Feld. Mehrbyte-Felder sind **immer** Little Endian, weil die Vampire m68k
(Big Endian) ist.

Damit die Antwort innerhalb der festen Wartezeit des Masters steht, laeuft
`v4_link.c` mit zwei Aufgaben: der Protokoll-Task antwortet nur (RAM-Kopie),
der Arbeitstask erledigt danach das Langsame. Ist die Arbeitsliste voll, werden
die "vorgemerkt"-Merker wieder geloescht, damit der Master nicht dauerhaft
`BUSY` sieht.

---

## 4. Der Vertrag ist das Drahtformat - nicht die C-Datei

**Wichtig, weil es schon zu Verwechslungen gefuehrt hat:** die Dateien
`v4_proto.h`/`v4_proto.c` sind auf beiden Seiten **nicht dieselben Quellen**.
Sie beschreiben dasselbe Drahtformat, haben aber verschiedene C-Helfer:

| | ESP32 (Slave) | V4 (Master) |
|---|---|---|
| Rahmen bauen/pruefen | `v4p_wframe_build/check`, `v4p_rframe_begin/finish/check`, `v4p_bframe_build/check` | `v4p_build_write/read/bulk`, `v4p_check_read/bulk` |
| Nutzlast | `v4p_status_payload_t`, `v4p_info_payload_t` | `v4p_dec_*` / `v4p_enc_*` (`v4p_status_t`, `v4p_info_t`, ...) |

Abgeglichen und **gleich** sind die formatbestimmenden Werte: Magic `0xA5`/
`0x5A`/`0x5B`, `WRITE_FRAME_LEN=32`, `READ_FRAME_LEN=128`,
`WRITE_PAYLOAD_MAX=27`, `READ_PAYLOAD_MAX=121`, `CHUNK_DEFAULT=128`, CRC-8
Polynom `0x07`, BULK-Overhead 12 Byte, `proto_ver = 3`.

Zwei veraltete Kommentare in den Quellen (nicht geaendert, nur notiert):
`PROTOCOL_V4.md` nennt `main/v4_proto.h` eine "Vertragsdatei fuer beide Seiten",
und in der V4-Fassung steht bei `V4P_MAGIC_READ` noch "fest 64 Byte", obwohl
`V4P_READ_FRAME_LEN` direkt darunter 128 ist (proto_ver 3). Das Drahtformat ist
in beiden Faellen 128 Byte - die Kommentare sind der Rest von Version 2.

---

## 5. Bewusste Unterschiede zum Vorgaengerprojekt

1. **Inquiry-Laenge ist nicht einstellbar.** `esp_bt_audio_classic_discovery_start()`
   hat keine Parameter. `SCAN_START(units, mode)` nimmt den Wunsch des Masters
   entgegen, meldet ihn im Log, kann ihn aber nicht durchsetzen - die Laenge
   bestimmt die Komponente. Den **Dauerbetrieb** (mode Bit0) bildet
   `bt_manager.c` selbst nach: kommt das Ende der Inquiry als Ereignis, wird neu
   gestartet.
2. **Auto-Scan nach dem Verbinden ist AUS.** Im Vorgaengerprojekt lief er an
   (`bt_mgr_set_auto_scan(true)`). Er kostet CPU und kann den Ton stocken lassen;
   der Master kann ihn jederzeit per `SCAN_START` anfordern.
3. **`FORGET`** laeuft ueber `esp_bt_gap_remove_bond_device()` (Bluedroid), weil
   `esp_bt_audio` dafuer nichts anbietet. Beide benutzen denselben Host.
4. **Keine eigene NVS-Ablage der Gegenstelle**, also kein Auto-Verbinden beim
   Start: `bt_mgr_autoconnect_saved()` meldet immer `false`. Die Bindung
   verwalten Bluedroid/`esp_bt_audio`.
5. **Die Tonquelle ist nicht mehr exklusiv.** Im Vorgaengerprojekt war die
   Quelle *entweder* I2S *oder* SD-Datei. Hier laufen beide ueber den Mischer:
   `AUDIO_SOURCE_SD` heisst "zusaetzlich laeuft eine Datei", der Vampire-Ton ist
   immer dabei. `STOP_PLAY` haelt nur die Datei an - das ist genau die Zusage aus
   `PROTOCOL_V4.md`.
6. **Nur Classic-Geraete** landen in der Geraeteliste; die V4 steuert eine
   A2DP-Senke an und kann LE-Geraete damit nicht verbinden.
7. **Geraeteliste: 16 Eintraege** (`V4P_MAX_DEVICES`). Ist sie voll, wird ein
   neuer Fund verworfen statt die Liste umzuwerfen - die Indizes, die der Master
   schon gelesen hat, bleiben gueltig.

---

## 6. Status: was geprueft ist - und was nicht

**Geprueft (in dieser Umgebung moeglich):**

* `v4_proto.c` uebersetzt mit dem Host-Compiler fehlerfrei
  (`gcc -std=c11 -Wall -Wextra -Wpedantic`).
* Alle von `v4_link.c` benutzten Makros (`V4P_*`, `SD_FS_*`, `BT_MGR_*`,
  `AUDIO_SOURCE_*`) und alle 35 Aufrufe in die drei Schichten sind deklariert
  bzw. definiert - per Skript gegen die Header geprueft.
* Die Vertragswerte gegen die V4-Seite abgeglichen (§4).
* Die I2C-Slave-API in ESP-IDF 6.1 passt zum Code: `i2c_slave_config_t`-Felder
  und die Signatur von `i2c_slave_received_callback_t` stimmen Feld fuer Feld
  bzw. Zeichen fuer Zeichen. Der Slave-Treiber v2 ist in 6.1 der **einzige**;
  `CONFIG_I2C_ENABLE_SLAVE_DRIVER_VERSION_2` gibt es dort nicht mehr und musste
  deshalb **nicht** in `sdkconfig.defaults*` aufgenommen werden.
* GPIO18/GPIO23 sind sonst nirgends im Projekt belegt.

**NICHT geprueft:**

* **Nie mit ESP-IDF uebersetzt** - in dieser Umgebung gibt es keine
  ESP-IDF-Werkzeugkette und kein angeschlossenes Board (siehe auch
  `docs/ARBEITSWEISE.md`: gebaut wird unter Windows mit `tools/mess_bauen.ps1`).
  Der erste Build passiert auf der Windows-Seite.
* **Nie geflasht, nie auf Hardware gelaufen.** Kein I2C-Verkehr mit einer echten
  Vampire, kein A2DP-Test, kein SD-Test.
* `v4_selftest` ist noch nie ausgefuehrt worden.

---

## 7. Inbetriebnahme am Board

1. Bauen und flashen wie gewohnt (`tools/mess_bauen.ps1`, `tools/flash_only.ps1`,
   danach Stromzyklus - der Chip bleibt sonst im Download-Bootloader).
2. Im Log muss stehen:
   `I (xxxx) v4_link: slave 0x50 on SDA=18 SCL=23, frames 32/128, chunk 128`.
   Fehlt die Zeile, ist `v4_link_init()` gescheitert - die Ursache steht direkt
   darueber.
3. `v4_bus` senden. Erwartung **ohne** angeschlossene Vampire:
   `BUS SDA=1 SCL=1 ... rx_gesamt=0 verworfen=0`.
   * `SDA=0` oder `SCL=0` im Leerlauf: etwas zieht die Leitung herunter - falscher
     Kopfstift, Kurzschluss, haengendes Geraet, oder die Vampire haengt dran.
   * `rx_gesamt=0`, obwohl die Vampire sendet: Verdrahtung oder Adresse
     ("nichts kommt an").
   * `rx_gesamt>0`, aber `verworfen>0`: es kommt etwas an, wird aber als Rahmen
     abgelehnt - Takt oder Rahmenformat ("kommt an, passt nicht"). Beides
     braucht entgegengesetzte Massnahmen, deshalb wird getrennt gezaehlt.
4. `v4_selftest` senden (noch ohne Vampire). Er faehrt die echte Befehlskette mit
   synthetischen Rahmen durch: `GET_INFO`, `GET_STATUS`, `DIR_OPEN`/`NEXT`/`CLOSE`,
   `FILE_OPEN`/`READ`/`CLOSE`, `SET_CHUNK`, `PATH_*`, `PLAY_FILE`/`STOP_PLAY` -
   einschliesslich der BUSY-Runde und des BULK-Rahmens. Nur die I2C-Leitung
   selbst bleibt ungeprueft; sie ist der einzige Teil, fuer den es die Vampire
   braucht. Der Befehl blockiert die Konsole bis zu 6 Sekunden.
   Erwartung: eine Zeile `SELFTEST ...` je Befehl. **Nicht** alles muss `OK`
   sein:
   * `NOT_FOUND` bei `FILE_OPEN`/`PLAY_FILE`, wenn keine der Testdateien
     (`test_tone_48k.wav`, `test_tone_440.wav`) oder gar keine Audiodatei auf der
     Karte liegt - der Selbsttest nimmt sonst die erste gefundene Audiodatei.
   * `BAD_STATE`/Fehler bei `PLAY_FILE`, solange kein A2DP-Stream laeuft
     (`start_media` fehlt).
   * `BAD_STATE`/`DEV_COUNT = 0` bei den Bluetooth-Befehlen, solange kein Scan
     gelaufen ist.
   Worauf es ankommt: die Rahmenpruefung (CRC, Magic, SEQ) und die
   BUSY-Runde muessen sauber durchlaufen - also **keine** Zeile mit
   `BAD_CRC`.
5. Erst danach die Vampire anschliessen und auf der V4-Seite
   (`ApolloCrossDev/Projects/I2C-test/v4_master/`) mit `v4_probe` anfangen,
   dann `v4_console`.

---

## 8. Offene Punkte

* **Ungeprueft** (siehe §6) - das ist der wichtigste Punkt: der Stand ist ein
  sauber geschriebener, statisch gepruefter Port, kein auf Hardware bewiesener.
* Nach dem ersten Build ist mit Uebersetzungsfehlern zu rechnen; die
  Wahrscheinlichkeit ist durch die Pruefungen oben verkleinert, nicht
  ausgeschlossen.
* **Speicher:** die Bruecke kostet rund 20 KB - zwei Aufgaben mit je 6144 Byte
  Stack, ein 2 KB grosser I2C-Senderringpuffer (`V4_TX_BUF_DEPTH`), je 1 KB
  Rahmenpuffer und Blockcache, dazu die Geraetetabelle (16 × ~255 Byte). Auf dem
  Modul ohne PSRAM (WROOM) ist das spuerbar; falls `free` nach dem Start einen
  kleinen groessten Block zeigt, sind `V4_TX_BUF_DEPTH` und `V4_TASK_STACK` die
  ersten Schrauben. Die Puffer haengen an `V4P_BULK_PAYLOAD_MAX` (1024) bzw.
  `SET_CHUNK` - ein kleineres `chunk` spart direkt Speicher.
* `GET_STATUS.sd_free_kb` und `SD_INFO` liefern den Stand vom **Mount**; die
  Kapazitaet wird nur dort aufgefrischt, weil `esp_vfs_fat_info()` ueber
  `f_getfree()` laeuft und im Antwortpfad zu lange brauchte. Nach Aenderungen
  von aussen also `SD_MOUNT(force=1)` senden.
* `PLAY_FILE` baut den Datei-Zweig um und braucht einen laufenden A2DP-Stream
  (`start_media`). Ohne Stream meldet der Datei-Zweig einen Fehler.
* Zwei Fassungen von `v4_proto.*`, die auseinanderlaufen koennen (§4). Ein
  gemeinsamer Satz Header fuer beide Seiten waere besser, scheitert aber daran,
  dass die V4-Seite m68k-Code (GCC 6.5) ist und der Slave andere Helfer braucht.
* Ein Testfall im Vorgaengerprojekt war offen und bleibt es: direkt nach
  `start_media` kann das erste `playfile` einmal mit "Got NULL Pointer" im
  Resampler-Ausgangsport scheitern. Der Datei-Zweig sollte erst starten, wenn die
  Mischer-Pipeline `RUNNING` meldet.
