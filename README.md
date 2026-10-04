# V4_ESP32 - Audio-Bruecke ESP32 <-> Vampire V4

Der ESP32 ist **A2DP-Quelle** (Bluetooth-Sender) und schickt zwei Tonquellen
gemischt an einen Lautsprecher oder Kopfhoerer:

1. **I2S-Eingang von der Vampire V4** (Sklave, 60000 Hz, 32 Bit, stereo) -
   dieser Ton muss **immer** zu hoeren sein.
2. **Dateien von der SD-Karte** (MP3, WAV), die dazugemischt werden
   (`playfile <name>`), wahlweise auch allein.

Dazu kommen die Bedienbefehle ueber die serielle Konsole (Bluetooth-Geraete
auflisten/verbinden/trennen, SD-Karte durchsuchen, Wiedergabe steuern,
Messwerte abfragen).

## Stand

**0.9.39** - MP3 und WAV klingen sauber, auch im schnellen Wechsel
(acht Titel im 6-Sekunden-Takt), die Vampire ist durchgehend zu hoeren.
Im Referenzlauf: 0 Resets, 0 Job-Fehler.

Die vollstaendige Messreihe mit allen Fehlern, Ursachen und Belegen steht in
[`docs/MESSREIHE.md`](docs/MESSREIHE.md) - inklusive der Messwerte, die den
Weg gewiesen haben (Durchsatz, Ringpuffer im 2-ms-Raster, Selbsttest 1:1).

## Aufbau

```
I2S (Vampire) --> io_i2s --> aud_lin_resample -----------\
                                                          >-- aud_mixer --> aud_enc_mix --> io_bt --> A2DP
SD-Karte --> io_file --> aud_dec --> aud_lin_resample_file /
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

## Konsolenbefehle (Auszug)

```
version            Version und Takt
free               Speicher: frei, Minimum, groesster Block, je Heap-Bereich
tasks              Laufzeitstatistik der Aufgaben
playfile <datei>   Datei von der SD-Karte in den Mischer spielen
start_media        A2DP-Stream starten
connect <MAC>      Senke verbinden        disconnect   trennen
sd_ls              SD-Karte auflisten     sd_mount     einbinden
bufstat [reset]    Ringpuffer-Statistik   i2sstat      I2S-Eingang
i2smode [0..4]     Messfall (im NVS)      restart      Neustart
log_level <tag> <stufe>
```

## Offene Punkte

* Diagnosezeilen (`Block n: in_frames=...`, Durchsatz, 2-ms-Raster) sind noch
  aktiv; sie kosten UART-Zeit im Audio-Task und sollten fuer den Dauerbetrieb
  abschaltbar sein.
* I2C-Protokoll zur Vampire V4 (Bedienung/Status) ist noch nicht umgesetzt.
* Die Messfaelle 1 und 3 (GMF-Wandler) laufen, sind aber nicht mehr der
  bevorzugte Weg.
