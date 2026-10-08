# V4_ESP32 - Audio-Bruecke ESP32 <-> Vampire V4

Der ESP32 ist **A2DP-Quelle** (Bluetooth-Sender) und schickt zwei Tonquellen
gemischt an einen Lautsprecher oder Kopfhoerer:

1. **I2S-Eingang von der Vampire V4** (Sklave, 60000 Hz, 32 Bit, stereo) -
   dieser Ton muss **immer** zu hoeren sein.
2. **Dateien von der SD-Karte** (MP3, WAV), die dazugemischt werden
   (`playfile <name>`), wahlweise auch allein.

Dazu kommt die **Bedienung durch die Vampire ueber I2C** (Adresse 0x50):
Bluetooth-Geraete auflisten/verbinden/trennen, SD-Karte durchsuchen, Datei
abspielen, Status abfragen. Dieselben Dinge gehen weiterhin ueber die serielle
Konsole.

## Stand

**0.9.61** - Zwei Korrekturen am Datei-Zweig, beide aus der Analyse in
[`docs/MP3_STARTFEHLER.md`](docs/MP3_STARTFEHLER.md):
(1) Ein Fehler im Datei-Zweig stoppt nicht mehr die ganze A2DP-Uebertragung -
bisher riss ein gescheitertes `playfile` den I2S-Ton der Vampire mit und
loeste zusaetzlich einen Task-Watchdog-Neustart aus. (2) Der Wandler fordert
seinen Ausgangspuffer jetzt mit **einer festen Groesse** an (6144 Byte) statt
je Block mit einer anderen; die Reallokation, die dabei mit NULL fehlschlug,
kann damit nur noch einmal auftreten. Dazu laeuft vorerst eine Diagnosezeile
im `open` des Wandlers (Ausrichtungen, Puffergroesse, freie Bytes je
Heap-Bereich), die nach der Klaerung wieder entfernt wird.
Noch **nicht** auf Hardware geprueft - dafuer muss geflasht werden.

**0.9.60 auf Hardware verifiziert (08.10.2026)** - Per `usbipd` von WSL aus
geflasht (ESP32-D0WD-V3, MAC `ec:c9:ff:fd:60:c0`) und am Board geprueft:
SD-Karte mountet im **ersten** Versuch (`mounted at /sdcard (SDMMC 1 Bit)`,
1067 ms - der entfernte SPI-Rueckfall fehlt nicht), der I2C-Slave startet
(`slave 0x50 on SDA=18 SCL=23`), `v4_bus` meldet beide Leitungen im Leerlauf
high (`SDA=1 SCL=1`, noch kein Verkehr), Bluetooth-Ereignisse kommen in
`bt_manager.c` an (Verbindung mit `66:FE:5A:E3:41:DF`), Heap 123 KB frei.
`v4_selftest` lief **komplett durch, ohne BAD_CRC** - und hat dabei erstmals auf
Hardware den **BULK-Weg** gefahren (`FILE_READ`: 128 gueltige Byte im 140-Byte-
Rahmen, CRC ok, `END` hinterm Dateiende, `chunk=256` bestaetigt). Offen bleibt
nur die echte I2C-Leitung zur Vampire - Details in
[`docs/I2C_BRUECKE.md`](docs/I2C_BRUECKE.md), Abschnitt 6.

**0.9.60** - SPI-Rueckfall beim SD-Mount entfernt. `sd_card_mount()` versucht nur
noch 1-Bit-SDMMC (bis zu 3 Versuche, 4 MHz); der zweite Weg ueber SPI auf
denselben Leitungen ist weg, ebenso `SD_SPI_MAX_FREQ_KHZ`, die Zustandsmerker
`s_spi_bus`/`s_over_spi` und die beiden Includes `driver/sdspi_host.h` und
`driver/spi_common.h`. Aus der Komponentenliste in `main/CMakeLists.txt` faellt
`esp_driver_sdspi`. Er war reine Absicherung und wurde nie gebraucht: die
Ursachen des SD-Ausfalls waren der Flash-Takt (80 statt 40 MHz) und der fehlende
DAT0-Pull-up, beide behoben. Ein zweiter Mountweg bedeutet nur einen zweiten
Fehlerpfad, den niemand wartet - und er kostete **32 848 Byte** im Image, weil
der SPI-Master-Treiber mitging (0x20c980 -> 0x204930, jetzt 33 % frei).
GPIO13 heisst jetzt `SD_PIN_DAT3` statt `SD_PIN_CS`: es ist DAT3, das beim
SDMMC-Init HIGH sein muss, kein Chip Select.

**0.9.59** - Die Platinen-Diagnosekommandos entfernt: `sdreg` (189 Zeilen),
`sdpins` (132), `sd_mount_spi` (85) und `scanpins` (35), zusammen mit ihren
Helfern (`sdreg_fsm`, `sdreg_cmd8_test`, `sdtrace_task`, `sdreg_lage`,
`sdreg_wait_reset`) und den Includes, die nur sie brauchten. Sie haben ihren
Zweck erfuellt: mit `sdpins` und `sdreg` wurde der fehlende DAT0-Pull-up
gefunden, `scanpins` hat die RS232-Leitungen zugeordnet. Kein Konsolenbefehl
kostet im Betrieb Laufzeit, aber 441 Zeilen in `cmd_reg.c` waren zu pflegen, wenn
sich an den Treibern etwas aendert. `cmd_version` zeigt den Flash-Takt weiterhin
(dafuer bleibt `flash_clock_mhz`).

**0.9.58** - Diagnosecode entfernt, der im Betrieb mitlief. Der Ton war bewiesen,
die Messhilfen kosteten nur noch: ein Task schrieb alle 5 s die CPU-Last beider
Kerne ins Log (auf Kern 1, dem Audio-Kern) und rechnete dafuer ueber alle
Tasklisten - dieselben Zahlen liefert das Kommando `tasks` auf Abruf. Ausserdem
entfernt: der tote Durchsatz-Helfer `i2s2bt_log_io_speed` (ohne Aufrufer seit
0.9.41), der leere Stub `i2s_input_start_monitor()`, der Byte-Zaehler im
I2S-Eingang (`enable_speed_monitor`, ohne Auswertung) und zwei Kommentare, die
entfernte Tasks beschrieben. Die Ketten-Diagnose (`dump_pipeline`,
`dump_pipeline_state`) bleibt, schreibt aber auf **Debug-Stufe** - mit
`log_level STREAM_PROC debug` wieder sichtbar.

**0.9.57** - I2C-Bruecke zur Vampire V4 eingebaut (Protokoll v3, Slave 0x50 auf
SDA=GPIO18/SCL=GPIO23). Der Stand **uebersetzt fehlerfrei mit ESP-IDF v6.1**
(Zielvariante ohne PSRAM: 31 % der App-Partition frei, 66 KB DRAM frei, keine
Warnung in den neuen Dateien) - aber er ist **noch nie geflasht und nie auf
Hardware gelaufen**. Siehe [`docs/I2C_BRUECKE.md`](docs/I2C_BRUECKE.md) fuer den
genauen Pruefstand, die Unterschiede zum Vorgaenger und die Inbetriebnahme.

**0.9.39 bis 0.9.56** - MP3 und WAV klingen sauber, auch im schnellen Wechsel
(acht Titel im 6-Sekunden-Takt), die Vampire ist durchgehend zu hoeren.
Im Referenzlauf: 0 Resets, 0 Job-Fehler. Dazu SD-Ausfall behoben (fehlender
Pull-up auf DAT0) und der Equalizer hinter dem Mischer.

Die vollstaendige Messreihe mit allen Fehlern, Ursachen und Belegen steht in
[`docs/MESSREIHE.md`](docs/MESSREIHE.md) - inklusive der Messwerte, die den
Weg gewiesen haben (Durchsatz, Ringpuffer im 2-ms-Raster, Selbsttest 1:1).

## Aufbau

```
I2S (Vampire) --> io_i2s --> aud_lin_resample -----------\
                                                          >-- aud_mixer --> aud_eq --> aud_enc_mix --> io_bt --> A2DP
SD-Karte --> io_file --> aud_dec --> aud_lin_resample_file /

I2C (Vampire, Master) --> v4_link (Slave 0x50) --> bt_manager / sd_fs / audio_source
```

* **`aud_lin_resample`** ist ein eigenes GMF-Element
  ([`main/linear_resample.c`](main/linear_resample.c)). Es wandelt Rate und
  Bittiefe und gibt immer stereo aus (Mono wird auf beide Kanaele kopiert).
  Es ersetzt GMFs `aud_rate_cvt`, der fuer die Wandlung zwischen den
  Raten-Familien (44100 <-> 48000) 15360 Byte am Stueck braucht und auf dem
  Modul ohne PSRAM daran scheitert.

* Beide Zubringer schreiben ueber je einen Ringpuffer in den `aud_mixer`.
  Der Mischer holt je Aufruf 1024 Byte und fuellt fehlende Bytes mit Nullen -
  ein Zubringer, der weniger liefert, erzeugt deshalb hoerbare Aussetzer.

* Die A2DP-Abtastrate wird mit der Senke ausgehandelt (44100 oder 48000) und
  beim Stream-Start in die Kette uebernommen.

* **Der I2C-Weg ist Zusatz.** Faellt er aus, laeuft die Tonbruecke weiter:
  `v4_link_init()` wird in `app_main` bewusst ohne `ESP_ERROR_CHECK` aufgerufen.

## Bauen, Flashen, Testen

Die Skripte in [`tools/`](tools/) sind die im Projekt benutzten Arbeitsmittel
(sie erwarten die Verzeichnisse `D:\Coding\ESP-IDF\V4_ESP32` und
`D:\Coding\ESP-IDF\.tmp\logs`):

| Skript | Zweck |
|---|---|
| `tools/mess_bauen.ps1 -Mode <0..4> [-NoPsram] [-Small]` | baut einen Messfall (loescht den CMake-Cache, sichert `sdkconfig`) |
| `tools/flash_only.ps1 -Port COMx` | flasht, wartet auf den Neustart von Hand |
| `tools/monitor_fenster.ps1 -Port COMx [-Log ...]` | serieller Monitor mit Auto-Reconnect und Befehlskanal |
| `tools/send_cmd.ps1` | schickt einen Befehl an den laufenden Monitor |
| `tools/idf61_env.ps1` | setzt die ESP-IDF-v6.1-Umgebung |

**Wichtig:** Nach jedem Flashen bleibt der Chip im Download-Bootloader stehen -
er laeuft erst nach einem Stromzyklus bzw. EN-Tastendruck.

Messfaelle (`-Mode`): 0 = nur Datei-Zweig, 1 = I2S mit GMF-Wandlern,
2 = I2S mit eigenem linearem Wandler (**Vorgabe**), 3 = GMF-Bitwandler vor der
GMF-Ratenwandlung, 4 = gesperrt (Absturz in `aud_rate_cvt_i2s`).

**Zweiter Weg: Uebersetzen unter Linux (nur Bauen, nicht Flashen).** Seit 0.9.57
gibt es daneben eine Linux-Umgebung, in der der ganze Stand ohne Windows
uebersetzt werden kann - damit sind Uebersetzungsfehler pruefbar, ohne das Board
anzufassen:

```bash
. ~/esp-idf/export.sh                 # ESP-IDF v6.1 + Werkzeuge in ~/.espressif
idf.py -DIDF_TARGET=esp32 build
```

Der `build/`-Ordner und die Komponenten liegen beide im Projekt und sind
gitignoriert. Geflasht wird weiter unter Windows (`tools/flash_only.ps1`) - in
WSL gibt es keinen seriellen Port.

## Konsolenbefehle

Die Liste ist aus der Registrierung in [`main/cmd_reg.c`](main/cmd_reg.c)
uebernommen (die frueheren Befehle `bufstat`, `i2sstat` und `i2smode` gibt es
seit dem Aufraeumen in 0.9.40/0.9.41 nicht mehr):

```
Ton und Bluetooth
  playfile <datei>   Datei von der SD-Karte in den Mischer spielen
  play|pause|stop|next|prev    AVRCP-Kommandos an die Gegenseite
  start_media        A2DP-Stream starten   stop_media   anhalten
  i2s_media [off]    I2S-Zweig (Vampire) ein-/ausschalten
  mixer              Wartezeiten des Mischers lesen/setzen
  connect <MAC>      Senke verbinden       disconnect   trennen
  start_discovery [name]   Geraetesuche    stop_discovery
  metadata [maske]   Titelinfos anfordern
  vol_set <0..100> | vol_up | vol_down

SD-Karte und System
  sd_mount           SD-Karte einbinden    sd_unmount   auswerfen
  sd_ls [pfad]       Verzeichnis auflisten
  eq ...             Equalizer hinter dem Mischer
  version            Version, Takt, Flash-Takt Soll/Ist
  free | tasks | log_level <tag|*> <stufe> | restart

I2C-Bruecke zur Vampire (neu in 0.9.57)
  v4_bus             Buszustand: Leitungen und Zaehler (Verdrahtung vs. Rahmen)
  v4_selftest        Befehlskette einmal ohne I2C-Master durchfahren
```

## Offene Punkte

* **Die I2C-Bruecke ist noch nie auf Hardware gelaufen**: sie uebersetzt
  warnungsfrei, aber nie geflasht und nie mit einer Vampire gesprochen. Erster
  Schritt nach dem Flashen sind `v4_bus` und `v4_selftest` - siehe
  [`docs/I2C_BRUECKE.md`](docs/I2C_BRUECKE.md), Abschnitt 7.
* Diagnoseausgaben der Kette (`dump_pipeline`, `dump_pipeline_state`) stehen auf
  Debug-Stufe und sind im Normalbetrieb still. Wer sie sehen will:
  `log_level STREAM_PROC debug`. Die frueheren periodischen Zeilen (CPU-Last,
  Durchsatz, Puffer im Sekundentakt) sind in 0.9.41 und 0.9.58 entfernt worden.
* Nach `start_media` kann das erste `playfile` einmal mit "Got NULL Pointer" im
  Resampler-Ausgangsport scheitern; der Datei-Zweig sollte erst starten, wenn
  die Mischer-Pipeline `RUNNING` meldet.
* Die Messfaelle 1 und 3 (GMF-Wandler) laufen, sind aber nicht mehr der
  bevorzugte Weg.
