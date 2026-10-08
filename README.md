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

## Flashen (Kurzfassung)

**Jeder Flash** laeuft ueber `tools/flash_linux.sh [Buildverzeichnis]` — auch ein
fremdes oder altes Image zum Vergleich. Das Skript entschaerft CRLF in
`flash_args`, prueft den Erfolg am Log und gibt danach **immer** den Hinweis
"FLASH FERTIG - JETZT UMSTECKEN" aus: erst Jumper/Modul zurueckbauen und neu
starten, dann ist die SD-Karte wieder da. Details in
[`tools/README.md`](tools/README.md).

**Die Vampire laeuft nur mit gestecktem ESP32-Modul.** Waehrend des Umsteckens
ist sie stromlos - V4-Uploads (`make upload`), `make log` und Messungen gegen die
V4 gehen erst **nach** dem Umstecken und dem Stromzyklus.

## Stand

**0.9.76** - **Reservierung an die richtige Stelle.** 0.9.75 legte den Puffer des
Dekoders schon beim Pipelineaufbau an (das klappte), der Puffer des linearen
Wandlers scheiterte dort aber: `Vorab-Puffer: 'aud_lin_resample_file' hat keinen
Ausgang` - der Ausgang entsteht erst beim Verbinden mit dem Mischer
(`connect_branch_to_mixer`). Genau dieser Puffer fehlte dann beim Abspielen
("groesster Block 608 Byte"). Jetzt wird er direkt nach dem Verbinden
reserviert.

**0.9.75** - **Puffer frueh reservieren.** Die Messung nach 0.9.74 zeigt: die
*Menge* stimmt jetzt (79 628 Byte frei vor dem Start), aber der DRAM ist
**zersplittert** - der groesste zusammenhaengende Block ist nur noch **272 Byte**
gross, und selbst der BT-Stack scheitert ("calloc failed", "Failed to send frame
batch: ESP_ERR_NO_MEM"). In 0.9.56 waren es 95 KB frei mit **69 KB** groesstem
Block.

Deshalb fordert der Datei-Zweig seine zwei grossen Puffer jetzt **beim
Pipelineaufbau** an (`reserve_output_payload()` in `stream_proc.c`), solange der
DRAM noch zusammenhaengt: Dekoderausgang 4608 Byte und der Ausgang des linearen
Wandlers 5120 Byte, beide mit der vom Port verlangten 16-Byte-Ausrichtung. Sie
bleiben am Port haengen (der Port gibt seinen `self_payload` erst beim
Zerstoeren der Pipeline frei, nicht bei Stop/Reset) und werden fuer jeden Titel
wiederverwendet. `LIN_RESAMPLE_OUT_PAYLOAD_MAX` steht dafuer jetzt im Header -
es ist ein Vertragswert, den auch `stream_proc.c` braucht.

**0.9.74** - **Ton sauber UND MP3 moeglich.** Zwei Messergebnisse aus derselben
Sitzung:
1. Die in 0.9.63 abgeschalteten BT-Profile (HFP, GOEPCS, AVRCP-Cover-Art) sind
   die Ursache der kratzigen Wiedergabe: mit ihnen **an** liegt der Mischer-Task
   bei 16,3 % und Kern 1 hat 22,9 % Leerlauf, mit ihnen **aus** bei 33,0 % und
   1,3 % (Vergleich 0.9.56: 17,5 % / 23,0 %). Sie sind deshalb wieder an.
2. Sie kosten aber rund **12,4 KB** internen Speicher (95 232 Byte frei in
   0.9.56 ohne Bruecke, 87 232 mit Bruecke und ohne Profile, 74 808 mit beidem) -
   und damit fehlte der MP3-Start wieder. Die 8 KB kommen jetzt aus der Bruecke
   selbst: ihre beiden Aufgaben brauchten gemessen nur 2352 bzw. 780 Byte,
   hatten aber je 6144 (zusammen 12 KB). Jetzt 4096 + 2560, Gerätetabelle 4
   statt 8 Eintraege, Haupttask-Stack 3072 statt 3584. Der Datei-Ring bleibt bei
   den dokumentierten 8 KB. `v4_bus` zeigt weiterhin beide Stack-Reserven und
   `bufs` Ringpegel, Mischerzustand und CPU-Last.

**0.9.73** - **Equalizer ueber I2C.** Neu im Vertrag: `EQ_INFO` (0x70),
`EQ_BANDS` (0x71), `EQ_GET` (0x72) und `EQ_SET` (0x73). Damit schliesst sich die
Luecke aus [`docs/I2C_ERWEITERN.md`](docs/I2C_ERWEITERN.md) - bisher gab es den
Equalizer nur als Konsolenbefehl, und die Einstellung liess sich nicht
zuruecklesen: `stream_proc_eq_info()` und `stream_proc_eq_get()` liefern sie
jetzt als Daten. Q wird als Guete x 100, Gain als dB x 10 uebertragen, damit
Big-Endian-V4 und Little-Endian-ESP32 ohne Fliesskomma auskommen
(`V4P_EQ_OFF_*`). Die V4-Seite hat die Master-API, den Mock, den Testfall §15
und im Konsolenmenue den Punkt **(e)qualizer** (Baender anzeigen und stellen).
**Noch nicht auf Hardware geprueft.**

**0.9.71** - **Bitpool-Messschalter.** Die CPU-Messung mit `bufs` zeigt: der
Mischer-Task braucht **31 % der Gesamt-CPU, auch ohne jede Quelle** (also
≈62 % von Kern 1) - das ist der **SBC-Encoder**; der Equalizer kostet dagegen
nichts (32,4 % -> 32,9 % mit 0 Baendern). Die Komponente waehlt fuer Stereo
selbst den Maximalwert `A2DP_SRC_BITPOOL_STEREO_DEFAULT = 53` (~327 kbit/s,
gedeckelt auf das Maximum der Senke). Der Bitpool darf laut A2DP frei in
[min,max] gewaehlt werden, deshalb jetzt messbar:
`sbc bitpool <1..250|0>` deckelt ihn fuer den naechsten Streamstart (der
ausgehandelte Block bleibt unangetastet, gearbeitet wird auf einer Kopie).
Damit laesst sich CPU-Last gegen Tonqualitaet abwaegen, ohne zu raten.

**0.9.69** - **I2S-Ringpuffer wieder 12 KB.** In 0.9.63 wurden hier 2 KB fuer den
MP3-Dekoder abgezweigt (10 statt 12 KB); Feldmeldung vom 08.10.2026: *"sound ist
kratzig"* - genau die Aussetzer, vor denen der Kommentar damals gewarnt hat. Die
2 KB kommen jetzt aus den Aufgaben-Stacks der I2C-Bruecke (`V4_TASK_STACK` 6144 ->
5120), die bei echtem V4-Verkehr nur 2352 Byte belegen (Mitschnitt
`/tmp/v4_traffic.log`: 3792 von 6144 frei) - mit 5120 bleiben rund 2,7 KB
Reserve, und `v4_bus` zeigt sie weiterhin an. Die DRAM-Bilanz bleibt damit
unveraendert, der MP3-Startfehler also weiter behoben.

**0.9.68** - **Die Vampire ist sofort hoerbar.** Bisher startete die
Uebertragung erst durch einen Anstoss (`MEDIA_START`, `PLAY_FILE`,
`start_media`); nach einem Reset schwieg die V4, bis der erste Titel lief
(Feldmeldung vom 08.10.2026: *"habe v4 am laufen, esp32 resetet, hoere aber
keinen sound der v4. nach abspielen des mp3 laeuft der v4 sound."*). Jetzt
startet `stream_proc_autostart_tick()` die Uebertragung von selbst, sobald eine
Gegenstelle verbunden ist - samt `i2s2bt_request()`, damit der I2S-Eingang der
Vampire ueber den Mischer zum Bluetooth-Geraet geht. Der Takt kommt wieder aus
der 200-ms-Schleife von `stream_proc_task` (kein eigener Task, kein Stack);
solange keine Uebertragung steht, wird alle 5 s erneut versucht. `stop_media`
auf der Konsole schaltet den Autostart ab, `start_media` und eine neue
Verbindung schalten ihn wieder ein. **Noch nicht auf Hardware geprueft.**

**0.9.67** - `MEDIA_START`/`PLAY_FILE` fordern den **I2S-Zweig der Vampire** mit
an (`i2s2bt_request()` vor dem Streamstart). Ohne das bindet der Stream die
Datei-Pipeline `local2bt_pipe` an Bluetooth - und die hat seit dem Umbau auf den
Mischer **keinen Ausgang** mehr: es waere nichts hoerbar gewesen, und der
Datei-Zweig endete mit ERROR (Mitschnitt `/tmp/v4_traffic.log`). Mit dem Wunsch
verdrahtet `i2s2bt_set_stream()` den Mischer samt BT-Ausgang und startet den
I2S-Zweig; Dateien kommen ueber den Mischer dazu. Damit ist der V4-Ton **und**
die Datei ueber I2C angesteuert auf dem Bluetooth-Geraet. **Noch nicht auf
Hardware geprueft.**

**0.9.66** - Autoverbindung mit eigener Ablage. Die zuletzt erfolgreich
verbundene Gegenseite liegt jetzt im eigenen NVS (Namespace `v4bt`, Schluessel
`peer`; Bluedroid haelt die Bindung zusaetzlich). Beim Start verbindet Bluedroid
sie von sich aus (beobachtet rund 5 s nach dem Boot); kommt keine Verbindung
zustande, versucht `bt_mgr_autoconnect_tick()` es **alle 15 s** erneut, bis eine
steht. Der Takt kommt aus der 200-ms-Schleife von `stream_proc_task` - **keine
neue Aufgabe, kein zusaetzlicher Stack** (der DRAM ist knapp), und das
NVS-Schreiben liegt bewusst nicht im BT-Ereigniskontext (dort nur 3072 Byte
Stack), sondern im Takt. Ein Verbindungsversuch ohne Ereignis gilt nach 20 s als
gescheitert, sonst bliebe die Wiederholung stehen. `DISCONNECT` schaltet die
Autoverbindung ab (die Adresse bleibt fuer den naechsten Start), `FORGET` loescht
auch unsere Ablage. **Die I2C-Bruecke ist davon unabhaengig** - sie wartet nie
auf den Verbindungszustand, und der Verbindungsaufbau selbst laeuft asynchron im
BT-Stack. **Noch nicht auf Hardware geprueft.**

**0.9.65** - Die Vampire kann die A2DP-Uebertragung jetzt selbst starten.
Der Feldtest am 08.10. (Mitschnitt `/tmp/v4_traffic.log`, 1024 Rahmen, 225
`PLAY_FILE`) zeigte die Luecke: die Konsole der V4 spielte Dateien ab, aber es
kam kein Ton, weil der Datei-Zweig ohne laufende Uebertragung keinen Abnehmer
hat (`Wiedergabe beendet - stoppe den Datei-Zweig (ERROR)`). Neu:
`MEDIA_START` (0x62), und `PLAY_FILE` startet die Uebertragung ebenfalls, wenn
sie noch nicht laeuft - Master ohne dieses Kommando funktionieren damit
unveraendert. Der Slave wartet beim Start selbst, bis der Mischer laeuft (bis
2 s), und antwortet so lange BUSY. Ausserdem repariert: die Idempotenzpruefung
in `PLAY_FILE` verglich den GMF-URI mit dem VFS-Pfad und griff deshalb nie -
jede Wiederholung baute den Zweig neu auf. Die V4-Seite (`my_crossdev`,
Projekt `I2C-test/v4_master`) hat `v4_media_start()`, startet sie in `do_play()`
vor `PLAY_FILE` und kennt den Vertrag §11a (158 Tests, 0 Fehler).
**Noch nicht auf Hardware geprueft.**

**0.9.63 auf Hardware verifiziert (08.10.2026)** - **Die MP3 spielt wieder**,
zusammen mit dem durchgehenden I2S-Ton der Vampire. Ursache und Beleg stehen in
[`docs/MP3_STARTFEHLER.md`](docs/MP3_STARTFEHLER.md): dem Datei-Zweig fehlten im
DRAM-Bereich (`MALLOC_CAP_DEFAULT`) rund 1,5 KB fuer den 4608-Byte-Ausgangspuffer
des MP3-Dekoders. Freigemacht wurden zusammen ~19 KB bei laufendem Stream
(68 848 -> 87 232 Byte frei): FatFs mit einem gemeinsamen Sektor-Cache statt
512 Byte je Datei, Bluetooth-Geraetetabelle 8 statt 16 Eintraege, Sendering der
I2C-Bruecke 1152 statt 2048 Byte, ungenutzte BT-Profile (HFP, GOEPCS,
AVRCP-Cover-Art) aus, I2S-Ringpuffer 10 statt 12 KB und der eigene
Ausgangspuffer 5120 statt 6144 Byte. Ein Fehler im Datei-Zweig stoppt ausserdem
die A2DP-Uebertragung nicht mehr - der Vampire-Ton bleibt hoerbar.
Befehlsfolge der Messung: `i2s_media`, `connect <bda>`, `start_media`, `free`,
`playfile test2.mp3`, `free`, `playfile test.mp3`, `v4_bus`.

**0.9.64** - nur die temporaere Diagnosezeile im Wandler wieder entfernt
(Heap-/Ausrichtungsbericht, der den MP3-Startfehler geklaert hat). Sonst
unveraendert zu 0.9.63.

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
