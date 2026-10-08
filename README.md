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

**0.9.57** - I2C-Bruecke zur Vampire V4 eingebaut (Protokoll v3, Slave 0x50 auf
SDA=GPIO18/SCL=GPIO23). Der Code ist aus dem erprobten Vorgaengerprojekt
uebernommen und statisch geprueft, aber **noch nicht mit ESP-IDF uebersetzt und
noch nie auf Hardware gelaufen** - siehe
[`docs/I2C_BRUECKE.md`](docs/I2C_BRUECKE.md) fuer den genauen Pruefstand, die
Unterschiede zum Vorgaenger und die Inbetriebnahme.

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
  sdreg | sdpins | scanpins | sd_mount_spi    Diagnose (SD, Leitungen)
  eq ...             Equalizer hinter dem Mischer
  version            Version und Takt
  free | tasks | log_level <tag|*> <stufe> | restart

I2C-Bruecke zur Vampire (neu in 0.9.57)
  v4_bus             Buszustand: Leitungen und Zaehler (Verdrahtung vs. Rahmen)
  v4_selftest        Befehlskette einmal ohne I2C-Master durchfahren
```

## Offene Punkte

* **Die I2C-Bruecke ist ungeprueft**: nie mit ESP-IDF uebersetzt, nie geflasht,
  nie mit einer Vampire gesprochen. Erster Schritt nach dem Flashen sind
  `v4_bus` und `v4_selftest` - siehe
  [`docs/I2C_BRUECKE.md`](docs/I2C_BRUECKE.md), Abschnitt 7.
* Diagnosezeilen (`Block n: in_frames=...`, Durchsatz, 2-ms-Raster) sind noch
  aktiv; sie kosten UART-Zeit im Audio-Task und sollten fuer den Dauerbetrieb
  abschaltbar sein.
* Nach `start_media` kann das erste `playfile` einmal mit "Got NULL Pointer" im
  Resampler-Ausgangsport scheitern; der Datei-Zweig sollte erst starten, wenn
  die Mischer-Pipeline `RUNNING` meldet.
* Die Messfaelle 1 und 3 (GMF-Wandler) laufen, sind aber nicht mehr der
  bevorzugte Weg.
