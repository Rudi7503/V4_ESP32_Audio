# Messreihe I2S-Zweig - Ablauf mit 0.9.9

Ab 0.9.9 steckt die Umschaltung **im Image**. Es muss also nur EINMAL geflasht
werden, danach werden alle Faelle per Konsole durchgeschaltet.

## Vorbereitung

1. Monitor laeuft (Fenster von `.tmp\monitor_starten.bat` oder `flash_und_monitor.bat`).
2. Modul neu starten (EN tippen oder ab-/anstecken).
3. Im Boot-Log pruefen, dass **0.9.9** laeuft:
   ```
   I (xxx) main:  V4_ESP32  Version 0.9.9
   ```
   Steht dort eine andere Nummer, laeuft ein altes Image - dann nicht messen,
   sondern erst flashen.

## Gleicher Ablauf fuer jeden Fall

Immer erst den Fall waehlen, dann neu starten, dann messen:

```
i2smode              # zeigt den aktuellen Fall
i2smode 2            # Fall setzen
restart              # wirkt erst jetzt (der Zweig wird beim Start gebaut)
```

Nach dem Neustart verbinden und starten:

```
connect <MAC des Kopfhoerers>
start_media
i2s_media
```

Dann **30-60 s laufen lassen** und aus dem Log notieren:

- die Zeile `I2S-Zweig (...)` - sie nennt den aktiven Fall
- `CPU-Last: Kern0 x%, Kern1 y%`
- `Puffer I2S-Zweig  : ... (z%), Minimum m, leer e mal`
- `Puffer Datei-Zweig: ... (z%), Minimum m, leer e mal`

## Die fuenf Faelle

| `i2smode` | Kette im I2S-Zweig | Frage, die er beantwortet |
|---|---|---|
| 0 | (kein I2S-Zweig) | Referenz: nur Datei-Zweig |
| 1 | `aud_rate_cvt_i2s -> aud_bit_cvt_i2s -> aud_ch_cvt_i2s` | was kosten die GMF-Wandler zusammen |
| 2 | `aud_lin_resample` | eigener linearer Q16.16-Umsetzer (bisher bester) |
| 3 | `aud_bit_cvt_i2s -> aud_rate_cvt_i2s -> aud_ch_cvt_i2s` | GMF-Shift vor der Ratenwandlung |
| 4 | `aud_shift16 -> aud_rate_cvt_i2s -> aud_ch_cvt_i2s` | eigener Shift (`>>16`) vor der Ratenwandlung |

Erwartung: die Ratenwandlung ist der teure Teil. In Fall 3 und 4 rechnet sie auf
der halben Datenmenge, in Fall 1 auf der vollen. Fall 4 ist die Absicherung
gegen Fall 3: sollte der GMF-Bitwandler teurer sein als ein Shift, sieht man es
dort - unser `aud_shift16` ist vier Zeilen lang.

## Puffer auf Unterlauf pruefen

Die Frage "kann der Puffer unterlaufen?" wird nicht geraten, sondern gemessen.
Die Wartezeiten sind zur Laufzeit einstellbar:

```
mixer                # zeigt prefill/transit
mixer 0 0            # gar kein Vorlauf
start_media          # wirkt beim naechsten Stream-Aufbau
```

Dann die Pufferzeilen ansehen:

- **`Minimum 0`** und **`leer N mal` mit N > 0** heisst: der Puffer ist
  tatsaechlich leer gelaufen - das ist der gesuchte Unterlauf.
- Bleibt `Minimum` deutlich ueber 0 und `leer 0 mal`, traegt der Puffer.

Stufen zum Testen: `mixer 400 100` (Startwert) -> `mixer 100 50` ->
`mixer 0 0`. Jeweils neu `start_media`.

Bezug zur Datenrate: der I2S-Zweig liefert nach der Wandlung
44100 Hz x 2 Kanaele x 2 Byte = **176400 Byte/s**. Die Ringpuffer sind
20 x 1024 Byte = **20480 Byte** je Zweig, das deckt also **~116 ms** ab.

## Wichtig: was NOCH nicht messbar ist

Der **Mischbetrieb** (Vampire + MP3 gleichzeitig) braucht den Datei-Zweig, und
der braucht die SD-Karte. Die SD-Karte initialisiert auf diesem Modul nicht
(`sdmmc_card_init failed (0x107)`, Timeout vor jeder Datenuebertragung). Siehe
Abschnitt "Blocker: die SD-Karte initialisiert nicht" in STAND_MESSREIHE.md -
Code, Pins und Kartenerkennung sind nachgeprueft, es deutet auf Karte/Halter
bzw. das Modul.

Bis dahin laesst sich immerhin die **Rechenlast** aller Faelle vergleichen
(CPU-Last), auch ohne Datei-Zweig. Der Mischer fuellt den fehlenden Eingang mit
Nullen auf, die Vampire bleibt also hoerbar.
