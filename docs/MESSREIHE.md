# Stand der Messreihe V4_ESP32 (I2S + MP3 + A2DP-Mischer)

Letzte Aktualisierung: 02.10.2026

## DURCHBRUCH (Version 0.9.8, auf Hardware bestaetigt)

**Die Vampire ist ueber Bluetooth zu hoeren - fluessig, ohne Stocken.**
Damit laeuft der komplette Weg: I2S (Vampire, 60 kHz/32 Bit) -> linearer
Umsetzer -> Mischer -> SBC-Encoder -> A2DP.

Messwerte aus dem Lauf:

```
vor-report[0] aud_lin_resample: dependency=1, event_receiver=ja, state=0
LIN_RESAMPLE: receive_event aufgerufen
[a2dp source pipeline] state => RUNNING(3)
LIN_RESAMPLE: Block 9800: in_frames=96 peak_in=1827, out_samples=142
LIN_RESAMPLE:   raus[0]=196 raus[1]=572
Puffer I2S-Zweig: 20480/20480 Byte (100%), Minimum 19016, leer 0 mal
CPU-Last: Kern0 57%, Kern1 8%   (21 Tasks)
```

Plausibilitaet der Rechnung: 96 Eingangs-Frames bei 60 kHz, Ausgang 44,1 kHz
-> 96 * 44,1/60 = 70,6 Frames = 141 Samples; gemessen 142 Samples (Rundung).
Die Interpolation arbeitet (in[0]=175 -> raus[0]=196).

**Zwei Fehler waren die Ursache - beide in meinem eigenen Code:**

1. **`ops` wurden am Klon nicht gesetzt.** Die Pipeline arbeitet mit einer KOPIE
   des Elements, erzeugt ueber `obj->new_obj` (`esp_gmf_obj_dupl`,
   `esp_gmf_obj.c:24`). Ich hatte open/process/close/event_receiver erst in
   `aud_lin_resample_init()` gesetzt - also NACH dem Aufbau. Die Kopie hatte
   damit keinen Empfaenger:
   `vor-report[1] aud_lin_resample: dependency=1, event_receiver=NEIN, state=0`
   -> Fix: alle `ops` in `lin_resample_new()` setzen.

2. **Der Copier am Kopf verschluckt die Toninformation.** Er hat von Haus aus
   keinen `ops.event_receiver` (`esp_gmf_copier_init` setzt nur `event_func`),
   und die Meldung geht an das KOPF-Element. Damit erreichte sie den Umsetzer
   nie. -> Fix: I2S-Kette auf EIN Element verkuerzt
   (`{"aud_lin_resample"}`), der Umsetzer ist Kopf und Ende zugleich.

**Wichtige Regel daraus:** In einer GMF-Kette muss das KOPF-Element die
Toninformation SELBST annehmen koennen (eigener `ops.event_receiver`). Die
GMF-Audioelemente haben ihn; der Copier nicht.

**Noch offen:** Der Ringpuffer des I2S-Zweigs steht bei 100 %. Der Umsetzer
liefert also schneller, als der Mischer zieht. Hoerbar stoert es nicht (die
Wiedergabe ist fluessig), aber es ist kein Gleichgewicht: der Umsetzer laeuft
dauerhaft in den vollen Puffer. Als Reserve ist das gut (kein Unterlauf
moeglich), der Mischer gibt den Takt vor.

## 0. Messfall 3 (`I2S_MODE=3`): erst 32->16 Bit per Shift, DANN GMF-Ratenwandlung

Der Bitshift ist praktisch gratis (`My_Audio_converter_32to16bit.c:60` macht
genau das: `sample >> 16`), und die GMF-Ratenwandlung rechnet danach nur noch
auf der **halben** Datenmenge - sie war mit 27 % der groesste Einzelposten.
Kette: `aud_bit_cvt_i2s -> aud_rate_cvt_i2s -> aud_ch_cvt_i2s`
(der Bit-Wandler hat einen eigenen Empfaenger und darf deshalb am Kopf stehen).
(Vergleich: Modus 1 = `rate -> bit`, Modus 2 = eigener linearer Umsetzer.)

**Puffer-Diagnose.** `buffer_mon_task()` in stream_proc.c schreibt im
Sekundentakt den Fuellstand beider Zubringer-Ringpuffer:

```
Puffer I2S-Zweig  : 35120/40960 Byte (85%), Minimum 12048, leer 0 mal
Puffer Datei-Zweig:  8192/40960 Byte (20%), Minimum 0, leer 3 mal
```

Damit ist die Frage "koennen die Puffer leerlaufen?" direkt beantwortet statt
geraten. Die Werte kommen aus dem Datenbus selbst
(`esp_gmf_db_get_filled_size` / `_get_total_size`, esp_gmf_data_bus.h:291/303).
Zusaetzlich werden die festen Puffergroessen beim Start einmal geloggt
(`log_buffer_sizes()`).

Einordnung: ein Puffer, der **regelmaessig leer** ist, erzeugt Knackser, auch
wenn die CPU Reserve hat. Deshalb ist das die richtige Messung - nicht die
CPU-Last.

**Ringpuffergroesse** jetzt als Konstante dokumentiert: `MIXER_DB_ITEMS 40` x
`MIXER_DB_ITEM_SIZE 1024` = 40 KB je Zweig (das offizielle Beispiel nutzt 10 KB).
Bei 176400 Byte/s Ausgangsdatenrate deckt das ~232 ms ab.

## 1. Was funktioniert (auf Hardware nachgewiesen)

| Punkt | Stand |
|---|---|
| Mischer-Verkabelung (I2S-Zweig + Datei-Zweig -> `aud_mixer`) | **läuft** |
| A2DP-Verbindung, SBC-Encoder mit ausgehandelten Parametern (118-Byte-Rahmen) | **läuft** |
| MP3 von SD über den Mischer nach Bluetooth | **läuft**, in Echtzeit (8,7 s Datei = 8,7 s Wiedergabe) |
| Vampire-Ton (I2S 60 kHz/32 Bit) im Mischer | **läuft** (mit 32-Bit-I2S Lesen) |
| CPU-Last-Anzeige beider Kerne alle 5 s | **läuft** |
| Versionsnummer beim Start + Konsolenbefehl `version` | **läuft** |

## 2. Die vier gefundenen Ursachen (alle im Code behoben)

1. **`-8197`/`-8201` beim Verkabeln**: `esp_gmf_pool_new_pipeline` belegt Ports selbst.
   Ein Element am Kettenende muss einen freien Ausgang haben, der Mischer einen
   freien Eingang. Lösung: letztes Element = Kanalwandler bzw. linearer Umsetzer.

2. **`Element[aud_mixer] not ready to register job`**: Der Mischer ist ein
   dependency-Element und kommt nur über `ESP_GMF_INFO_SOUND` nach INITIALIZED.
   Lösung: Format **zusätzlich direkt an `mixer_pipe` melden** (wie
   `pipeline_howl.c:302-303`), und zwar **nach** dem Verkabeln (der
   Event-Weiterleiter entsteht erst dabei).

3. **Absturz `heap_caps_free ... outside heap areas`**: `stream_proc_prepare`
   setzte den BT-Stream auf den Datei-Zweig, der seit dem Umbau keinen Ausgang
   mehr hat (NULL-Zeiger). Lösung: nur setzen, wenn ein Ausgang existiert.

4. **Abgehackter Ton / 15x zu langsame Wiedergabe**: Der Mischer wartete je
   Eingang bis zu 100 ms (`esp_gmf_mixer.c:199`). Lösung: Wartezeit 0
   (nicht blockierend) wie im offiziellen Beispiel.

## 3. Messwerte (FreeRTOS-Laufzeitstatistik, 240 MHz)

### 3a. Vergleich der I2S-Zweige (0.9.13, auf Hardware gemessen)

Alle Faelle mit derselben Last (Vampire am I2S-Eingang, A2DP verbunden, SBC 118
Byte), je ~45 s, Pufferzaehler vorher genullt. Der I2S-Zweig laeuft in
`i2s2bt_task` auf **Kern 1**, Mischer + SBC-Encoder in `mixer_task` auf Kern 0.

| `i2smode` | Kette im I2S-Zweig | Kern 0 | Kern 1 | Puffer-Minimum | Leerlaeufe |
|---|---|---|---|---|---|
| 2 (Vorgabe) | `aud_lin_resample` | 60 % | **7 %** | 18 928 B | 0 |
| 3 | `aud_bit_cvt_i2s -> aud_rate_cvt_i2s -> aud_ch_cvt_i2s` | 62 % | **17 %** | 19 656 B | 0 |
| 1 | `aud_rate_cvt_i2s -> aud_bit_cvt_i2s -> aud_ch_cvt_i2s` | 61 % | **19 %** | 18 576 B | 0 |
| 0 | kein I2S-Zweig | 1 % | 1 % | — | — |

**Lesart.** Auf Kern 0 liegen alle Faelle bei ~60 % - dort arbeiten Mischer und
SBC-Encoder, nicht der I2S-Zweig; die Unterschiede von 1-2 Punkten sind
Lauf-zu-Lauf-Schwankung (in einer frueheren Messung lag Fall 2 bei 54 %). Der
**aussagekraeftige Wert ist Kern 1**, wo der I2S-Zweig rechnet:

- **Der eigene lineare Umsetzer ist mit 7 % klar der billigste Weg** - rund
  ein Drittel dessen, was die GMF-Ketten brauchen (17-19 %).
- Der Shift **vor** der Ratenwandlung (Fall 3, 17 %) ist gegenueber Rate **vor**
  Bit (Fall 1, 19 %) nur ~2 Punkte guenstiger. Die Hoffnung, durch die halbe
  Datenmenge viel zu sparen, hat sich **nicht** bestaetigt: die GMF-Ratenwandlung
  wird nicht von der Sample-Breite bestimmt, sondern vom Filter selbst.
- Fall 0 (1 %) ist **kein** Kostenvergleich fuer den Datei-Zweig, sondern zeigt
  nur: ohne I2S-Zweig und ohne Datei gibt es keine Datenquelle, also rechnet
  nichts.

**Folge fuer die Vorgabe:** Fall 2 bleibt der Standard. Der Shift-Weg
funktioniert und klingt sauber, ist aber teurer als unsere eigene Rechnung.

### 3b. Fruehere Messung (FreeRTOS-Laufzeitstatistik, 240 MHz)

| Größe | Wert |
|---|---|
| `i2s2bt_task` (60k/32 -> 44,1k/16, GMF-Wandler) | **27,0 %** |
| `mixer_task` (Mischen + SBC-Encoder) | **25,5 %** |
| `io_i2s` | 7,2 % |
| BT-Stack zusammen (BTU_TASK, btController, hciT) | 15,3 % |
| `a2dp_src_send` | 2,5 % |
| MP3-Decoder (`local2bt_task`) | ~10–15 % |
| Kern 0 gesamt beim Mischen (mit GMF-Wandler) | **98 %** -> Interrupt-Watchdog |
| Kern 0 / Kern 1 mit linearem Umsetzer (0.8.1) | **26 % / 19 %** |

**Wichtigste Erkenntnis:** Die CPU war NICHT die Ursache des Abhackens. Der
lineare Umsetzer senkte die Last von 159 % auf 45 %, abgehackt hat es trotzdem —
die Ursache war die blockierende Wartezeit des Mischers (Punkt 4).

## 4. Neu gebaut, aber NICHT bewertet: linearer Q15-Umsetzer

`main/linear_resample.c` — lineare Interpolation in Q16.16, Konstanten aus
`ESP32-I2S-to-BT/main/My_Audio_converter_60to44_1khz.c`
(`Q_SHIFT 16`, `Q_ONE 65536`, `RATE_RATIO_FIXED 89088`), inklusive 32->16 Bit
per Bitshift. Ersetzt im I2S-Zweig `aud_rate_cvt` + `aud_bit_cvt` in einem
Durchgang. Enthält eine Diagnose, die Eingangs-/Ausgangspegel pro Block loggt -
damit lässt sich klären, ob die Vampire wirklich Daten liefert.

**Eine Falle ist dokumentiert:** Die Toninformation geht an das KOPF-Element und
läuft erst ab dem nächsten weiter, sie bleibt beim ersten dependency-Element
stehen (`esp_gmf_pipeline.c:202,210-216`). Ein Kanalwandler am Kopf fängt sie
deshalb ab (`esp_gmf_ch_cvt.c:297` setzt `dependency = true`). Lösung:
`aud_copier_i2s` als Kopf (`esp_gmf_copier.c:138` setzt `dependency = false`).

## 5. Messschalter und Umschaltung zur LAUFZEIT (ab 0.9.9)

Ab 0.9.9 gibt es **ein Image fuer alle Faelle**. Der Zweig wird zwar in
`stream_proc_init()` gebaut, aber der Modus ist zur Laufzeit waehlbar
(`s_i2s_mode`, Startwert = `I2S_MODE` aus dem Build):

```
i2smode          zeigt den aktuellen Fall
i2smode <0..4>   setzt ihn - wirkt nach 'restart'
mixer            zeigt prefill/transit in ms
mixer 150 0      setzt sie - wirkt beim naechsten 'start_media'
```

Das spart die Handarbeit beim Flashen (BOOT halten, EN tippen, ab- und wieder
anstecken) fuer jeden einzelnen Fall: **einmal flashen, fuenf Faelle messen.**

```
I2S_MODE = 0   nur Datei-Zweig (MP3 allein)
I2S_MODE = 1   I2S mit GMF-Wandlern (rate -> bit)
I2S_MODE = 2   I2S mit eigenem linearen Umsetzer (Vorgabe)
I2S_MODE = 3   I2S: erst 32->16 Bit mit GMF-Bitwandler, DANN GMF-Ratenwandlung
I2S_MODE = 4   I2S: erst 32->16 Bit mit EIGENEM Shift (>>16), DANN GMF-Ratenwandlung
```

**Unterschied 3 gegen 4 - Fall 4 ist GESPERRT (Absturz).**

Der GMF-Bitwandler steckt in einer vorkompilierten Bibliothek
(`managed_components/espressif__esp_audio_effects/lib`) - was er rechnet, ist von
aussen nicht nachlesbar. Deshalb gab es Fall 4 mit einem eigenen Element
`main/shift16.c`, das woertlich `out[i] = (int16_t)(in[i] >> 16)` rechnet
(`My_Audio_converter_32to16bit.c:60`).

**Dieser Weg ist auf Hardware abgestuerzt** - und der Grund ist lehrreich:

```
SHIFT16: Toninformation: 60000 Hz, 32 Bit, 2 ch -> 60000 Hz, 16 Bit, 2 ch   (korrekt)
E ESP_GMF_PORT: ACQ IN, there is no payload, el:...-aud_rate_cvt_i2s
Guru Meditation Error: Core 1 panic'ed (LoadProhibited)
  esp_gmf_rate_cvt_process  (esp_gmf_rate_cvt.c:104)
  esp_gmf_element_process_running (esp_gmf_element.c:300)
```

Zwei Stellen kommen zusammen:

1. Der Eingangsport von `aud_rate_cvt_i2s` hat keinen Payload. Bei "nicht erstes
   Element" braucht `esp_gmf_port_acquire_in` `port->payload`; fehlt er, bleibt
   `*load` NULL (`esp_gmf_port.c:201-215`). Unser `aud_shift16` reicht den
   Payload nicht so weiter, wie GMF das erwartet - der GMF-Bitwandler tut das.
2. GMF prueft das Ergebnis nicht: `esp_gmf_rate_cvt.c:103` holt den Eingang,
   Zeile 104 greift sofort auf `in_load->valid_size` zu -> NULL-Dereferenz.

Seit 0.9.13 baut `i2smode 4` deshalb bewusst **dieselbe Kette wie Modus 3** und
schreibt eine Warnung ins Log. Der belastbare Weg fuer "32->16 Bit per Shift,
danach GMF-Ratenanpassung" ist **Modus 3**: GMFs Bitwandler kuerzt die 32 Bit
ebenfalls durch Verschiebung, reicht die Ports aber korrekt weiter.

**Regel daraus:** Ein eigenes Element zwischen zwei GMF-Elementen muss den
Payload exakt so weiterreichen wie ein GMF-Element - sonst stuerzt der
nachfolgende GMF-Wandler ab, weil er den Fehler nicht prueft.

Weitere Schalter:

```
POOL_SMALL=1   kleinerer Pool: aud_aec, aud_asrc und die toten
               Pipelines bt2codec/codec2bt entfallen (fuer Module ohne PSRAM)
```

Bauen mit `.tmp\mess_bauen.ps1 -Mode <0|1|2|3|4> [-NoPsram] [-Small]`.
Das Skript loescht den CMake-Cache (sonst greift der Schalter nicht) und
schaltet PSRAM in `sdkconfig.defaults.esp32` um (Original wird als
`*.psram` gesichert, die Varianten werden daraus erzeugt).

## 6. SD-Karte: Ursache OFFEN - Flash-Takt ist NICHT die Loesung

**Verifiziert (0.9.16): der Flash laeuft wirklich mit 40 MHz.** Zwei unabhaengige
Nachweise:

```
Nachweis 1 - der Bootloader selbst (er konfiguriert den Flash):
  I (31) boot.esp32: SPI Speed      : 40MHz
  I (35) boot.esp32: SPI Mode       : DIO
  I (39) boot.esp32: SPI Flash Size : 4MB

Nachweis 2 - die App liest die SPI-Register aus (CLI "version"):
  Flash: Soll 40m aus der Konfiguration
  Flash: Ist  SPI0.clock=0x00001001 -> 40 MHz
  Flash: Ist  SPI1.clock=0x00001001 -> 40 MHz
```

`0x1001`: clkdiv = (val & 0x3F) + 1 = 2, Quelltakt 80 MHz / 2 = 40 MHz
(Herleitung aus `esp_hal_mspi/esp32/include/hal/spi_flash_ll.h:456-466`).

**Warum das vorher unsichtbar war:** unser Bootloader-Log stand auf WARN. Die
Zeile `boot: SPI Speed` wird auf INFO gedruckt und war beim Uebersetzen
**komplett entfernt** - im Bootloader-Binary kamen die Strings "SPI Speed",
"SPI Mode" und "boot:" nicht vor. Mit
`CONFIG_BOOTLOADER_LOG_LEVEL_INFO=y` sind sie drin (Bootloader 24 048 -> 26 176
Byte) und jeder Start nennt den Takt.

**Trotzdem faellt die SD auf dem WROOM weiter aus** - mit verifizierten 40 MHz:

```
send_if_cond (1) returned 0x108   (SPI-Weg, CMD8 ohne Antwort)
mount failed: ESP_ERR_INVALID_RESPONSE
```

**Damit ist der Flash-Takt als Ursache widerlegt.**

### 6-I. Der WROVER-Test ist konfundiert

Der Test "WROVER mit 80 MHz faellt aus, mit 40 MHz laeuft" hat **zwei** Dinge
gleichzeitig geaendert:

1. den Flash-Takt (80 -> 40 MHz), und
2. zum ersten Mal das Loeschen der `sdkconfig` (der Build-Skript-Fehler war
   gerade behoben worden) - die Defaults wurden also erstmals wirklich
   angewandt, was auch andere Einstellungen beeinflusst haben kann.

Der Test beweist daher **nicht**, dass der Flash-Takt die Ursache war. Sauber
waere: WROVER mit 80 MHz und bereits regenerierter sdkconfig erneut messen.

### 6-II. Naechste konkrete Hardware-Pruefpunkte

Aus einem Espressif-Maintainer-Kommentar zu genau unserem Fehlerbild
([arduino-esp32 Issue #8992](https://github.com/espressif/arduino-esp32/issues/8992),
dort war die Ursache am Ende ein vertauschtes MISO/MOSI):

> "Also you should **not** have pull-up on the CLK line. Just on CMD and D0."

1. **Pull-up auf CLK pruefen.** Ein Pull-up auf der Taktleitung stoert genau den
   Clock-Updater, der bei uns in den Timeout laeuft. CLK muss frei sein, CMD und
   D0 duerfen/sollen Pull-ups haben.
2. **Sockelkontakte der Pins 13, 23, 24** (`IO14`, `IO15`, `IO2`) - die beiden
   WROOM-Module koennten schlicht dieselbe Kontaktschwaeche haben.
3. **Oszilloskop:** toggelt CLK (GPIO14) ueberhaupt, wenn der Treiber den Takt
   setzt? Das trennt "Peripherie defekt" von "Leitung gestoert".

### 6-III. Meine falschen Diagnosen (vollstaendig)

1. "Der SDMMC-Block des Ersatzmoduls ist defekt." -> dasselbe Modul mountete mit
   einem PSRAM-Build.
2. "Der Flash-Takt ist DIE Ursache." -> auf dem WROOM mit verifizierten 40 MHz
   widerlegt.
3. "Der Modultausch hat es verursacht." -> die Fehler gab es schon am 27.09.
4. "`0x108` heisst, die Karte antwortet falsch." -> `send_if_cond` (CMD8) bekommt
   gar keine Antwort.
5. "`IGNORE_NOTFOUND` rettet die Anwendung." -> danach bricht
   `_esp_error_check_failed` ab (`addr2line` auf 0x40097e6f).

**Lehre:** Erst messen, dann behaupten. Jede dieser Aussagen war aus den Daten
"plausibel" und war falsch. Die Pruefung des Image-Headers und der Register hat
zwei Build-Skript-Fehler aufgedeckt, die sonst weiter falsche Messwerte
geliefert haetten.


**Zwei Faktoren, beide belegt:**

| Modul | Flash-Takt | PSRAM | SD |
|---|---|---|---|
| WROVER (alt) | 80 MHz | aus | faellt aus (`0x107`) |
| WROVER (alt) | **40 MHz** | aus | **laeuft** (nach 100 ms gemountet) |
| WROOM (Modul 2) | **40 MHz** | aus | **faellt aus** (`0x107`) |
| WROOM (Modul 3) | 80 MHz | aus | faellt aus |
| WROVER (alt) | 40 MHz | **an** | **laeuft** (die 0.8.3-Konfiguration) |

**Faktor 1 - Flash-Takt.** Am selben WROVER-Modul wurde nur diese eine Zeile
geaendert und die SD ging von "faellt aus" auf "mountet in 100 ms":

```
CONFIG_ESPTOOLPY_FLASHFREQ_40M=y
CONFIG_ESPTOOLPY_FLASHFREQ="40m"
```

Der SDMMC-Takt kommt aus derselben PLL wie der Flash-Takt. Mit PSRAM laeuft der
Flash ohnehin mit 40 MHz (Flash und PSRAM teilen sich den SPI-Takt), ohne PSRAM
zieht ESP-IDF 80 MHz. **Das erklaert, warum es wie ein Modulproblem aussah:**
WROVER-Module fahren PSRAM-Builds (40 MHz) und funktionierten, WROOM-Module
koennen nur No-PSRAM-Builds fahren (80 MHz) und fielen aus.

**Faktor 2 - das Modul selbst.** Der Flash-Takt allein reicht nicht: mit
derselben Firmware (0.9.15, Flash 40 MHz im Image-Header verifiziert) mountet
der WROVER und der WROOM faellt weiter aus. **Hier ist noch etwas offen.**

Weitere Unterschiede, die beim Vergleich der Boot-Logs auffielen:

- WROVER: 8 MB Flash -> `W spi_flash: Detected size(8192k) larger than the size
  in the binary image header(4096k)`. WROOM: 4 MB Flash, keine Warnung.
- Beide Chips: Revision **v3.1**, identisch.

**Konsequenz:** Der WROVER ist die Arbeitsgrundlage - er hat SD **und** PSRAM
(voller GMF-Pool, 40-KB-Puffer). Damit werden Mischbetrieb, `playfile` und der
SD-Zugriff von der V4 testbar. Die WROOM-Module bleiben fuer SD unbrauchbar,
solange Faktor 2 nicht geklaert ist.

**Ausgabe mit dem Fix (WROVER):**

```
I (970) SD_CARD: SDMMC-Versuch 1/3
I (1070) SD_CARD: mounted at /sdcard (SDMMC 1 Bit)
Name: USDU1   Type: SDHC   Speed: 20.00 MHz   SSR: bus_width=1
sd_ls -> GamesWinterEdition_v1.0.lha, WormsDirectorsCut_v1.3_AGA_12MB_0605.lha, ...
```

### 6a. Meine falschen Diagnosen - und woran sie gescheitert sind

1. **"Der SDMMC-Block des Ersatzmoduls ist defekt."** Zu frueh: dasselbe Modul
   mountete mit einem PSRAM-Build und fiel mit einem No-PSRAM-Build aus. Zwei
   Module mit demselben Fehler waren kein Zufall, sondern dieselbe Ursache
   (beide ohne PSRAM = 80 MHz Flash).
2. **"Der Flash-Takt ist DIE Ursache."** Auch zu frueh: er behebt den WROVER,
   nicht den WROOM. Es bleibt ein modulabhaengiger Rest.
3. **"Der Modultausch hat es verursacht."** Die SD-Fehler gab es schon am 27.09.
   (0.6.3/0.7.0/0.7.1, einmal in 0.7.2) mit dem alten Modul.
4. **"`0x108` heisst, die Karte antwortet falsch."** Falsch: die Zeile darueber
   nennt `send_if_cond` (CMD8), es kommt **gar keine** Antwort.

**Lehre:** Bei "geht nur auf manchen Modulen" zuerst pruefen, welche
BUILD-Konfiguration auf welchem Modul laeuft. Hier waren Modultyp und
PSRAM-Konfiguration immer gekoppelt (WROVER kann PSRAM-Builds, WROOM nicht) -
die Korrelation sah nach Hardware aus und war zu einem guten Teil Software.

### 6b. Zwei echte Fehler im Build-Skript, die das verdeckt haben

1. `sdkconfig.defaults.esp32.psram` enthielt ein `# CONFIG_SPIRAM is not set`
   mitten in den PSRAM-Optionen. Der Hauptschalter war damit aus, obwohl alle
   Unteroptionen gesetzt waren - PSRAM war nie aktivierbar.
2. `mess_bauen.ps1` loeschte nur den CMake-Cache, **nicht die `sdkconfig`**. Eine
   vorhandene `sdkconfig` hat Vorrang vor den Defaults, also blieb jede
   Konfigurationsumschaltung wirkungslos: "-NoPsram" und der PSRAM-Build ergaben
   Bit fuer Bit dasselbe Image (beide 2 206 240 Byte).

Beides ist behoben; `mess_bauen.ps1` loescht jetzt die `sdkconfig` (mit Sicherung
als `sdkconfig.vorheriger_lauf`). **Jeder** Build wird gegen
`build/config/sdkconfig.h` **und** den Image-Header geprueft statt gegen die
Imagegroesse:

```powershell
# spi_speed im Image-Header: 0=40MHz, 1=26MHz, 2=20MHz, 0xf=80MHz
$b=[System.IO.File]::ReadAllBytes('build\bt_audio.bin'); $b[3] -band 0x0F
```

Nebenbefund: `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` gibt es in IDF v6.1
nicht mehr; richtig ist `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM`. IDF liefert
fuer den alten Namen einen Alias, die GMF-Warnung ist also harmlos, sobald die
neue Option gesetzt ist.

### 6c. Auch korrigiert: IGNORE_NOTFOUND rettet die Anwendung nicht

`CONFIG_SPIRAM_IGNORE_NOTFOUND=y` laesst den Start durchlaufen, wenn kein PSRAM
antwortet:

```
E esp_psram: PSRAM enabled but initialization failed. Bailing out.
E cpu_start: Failed to init external RAM; continuing without it.
```

Die Anwendung bricht danach aber trotzdem ab, weil die GMF-Speicheranforderungen
auf PSRAM zielen:

```
abort() was called at PC 0x40097e6f   -> _esp_error_check_failed (esp_err.c:38)
```

Ein PSRAM-Build laeuft also **nicht** auf einem Modul ohne PSRAM. Fuer den
A/B-Test heisst das: die PSRAM-Frage laesst sich nur auf einem Modul MIT PSRAM
klaeren (so geschehen).

---

Die folgenden Abschnitte sind die **fruehere Analyse**, die zu diesem Ergebnis
gefuehrt hat - sie bleiben als Nachweis stehen, was alles ausgeschlossen wurde.

### 6-I. Fruehere Eingrenzung (Blocker-Beschreibung)

Der Fehler ist inzwischen **eingekreist, und er liegt nicht im Code**:

```
E SD_TRANS: sd_host_slot_sdmmc_do_transaction(500): failed to set clk
E sdmmc_req: sdmmc_host_do_transaction(24): failed to do SD transaction
E vfs_fat_sdmmc: sdmmc_card_init failed (0x107)
W SD_CARD: attempt 1 failed: ESP_ERR_TIMEOUT
```

Was geprueft wurde:

- `sd_card.c` ist in der Sache **identisch** mit dem alten, funktionierenden
  Projekt (`ESP32-I2S-to-BT/main/sd_fs.c`): gleiche Pins, gleiche Reihenfolge,
  gleiches `slot.width = 1`, gleiche Flags.
- `slot.clk/cmd/d0` sind auf dem ESP32 **wirkungslos** - Slot 1 hat feste
  IOMUX-Pins (`CLK=14, CMD=15, D0=2`, `soc/esp32/include/soc/sdmmc_pins.h`).
  Daran kann es also nicht liegen, die Pins stimmen.
- `slot.cd` ist kein Tippfehler: `sdmmc_host.h:38-41` fuehrt `gpio_cd` und `cd`
  als Union-Alias. `slot.cd = GPIO_NUM_34` ist korrekt.
- `SDMMC_SLOT_CONFIG_DEFAULT()` setzt fuer ESP32 6/11/7 (Slot 0); wir setzen
  die Pins danach explizit auf 14/15/2.
- Der **Card-Detect liest "card inserted"** (GPIO34 auf LOW), die Karte wird
  also erkannt.
- Das erste Kommando nach dem Enable geht durch; erst das **zweite** laeuft in
  den Timeout. Der Ablauf ist immer `enable` (ok) -> `disable` (ok) ->
  `enable` (ok) -> **Kommando ohne Antwort**.
- Der Fehler tritt **vor jeder Datenuebertragung** auf. Praeziser als frueher
  bekannt: `sd_host_slot_set_card_clk` scheitert daran, dass das
  CIU-Clock-Update-Kommando (`update_clk_reg`, `sd_host_sdmmc.c:984-993`) in den
  5-s-Timeout laeuft.
- Die Taktteiler sind **korrekt** gegen die 160-MHz-Quelle
  (`sd_host_sdmmc.c:1120-1127`): Probing 160 MHz / 10 / (2*20) = 400 kHz,
  Default 160 MHz / 8 = 20 MHz. Es ist also keine falsche Frequenz.

Zusammengefasst: Software, Pins, Kartenerkennung und Taktteiler sind
nachgeprueft. Der Ablauf bricht dort ab, wo die Karte selbst antworten muesste.
**Die Verdrahtung ist unveraendert und mit dem alten Modul gelaufen; getauscht
wurde nur das ESP32-Modul.** Damit bleiben Karte/Halter oder das Modul.

### 6a. SPI-Versuch auf denselben Leitungen (0.9.12) - und was er NICHT zeigt

Seit 0.9.12 versucht `sd_card_mount()` nach drei SDMMC-Fehlversuchen
automatisch **SPI** auf denselben drei Leitungen (CLK=14, MOSI=CMD=15,
MISO=D0=2). Ergebnis:

```
SDMMC-Versuch 1..3: ESP_ERR_TIMEOUT         (0x107, Clock kommt nicht hoch)
SPI-Versuch 1..3:   ESP_ERR_INVALID_RESPONSE (0x108)
   sdmmc_init_sd_if_cond: send_if_cond (1) returned 0x108     <- CMD8, keine Antwort
```

**Wichtig - eine Fehldeutung, die ich korrigiert habe:** `0x108`
(`ESP_ERR_INVALID_RESPONSE`) heisst hier **nicht** "die Karte antwortet, aber
falsch". Die Zeile darueber nennt die Stelle: `send_if_cond` (CMD8) bekommt
**gar keine** Antwort. Ueber SPI ist der Fehlercode nur ein anderer als ueber
SDMMC; beide bedeuten "keine Antwort". Der SPI-Versuch stuetzt damit **nicht**
die Hoffnung, dass der SDMMC-Block in Ordnung ist.

Zwei weitere Befunde aus diesem Versuch:

- **CS = Card Detect (GPIO34) ist nicht moeglich.** `esp_driver_sdspi` steuert CS
  per `gpio_set_level` und legt den Pin als **Ausgang** an
  (`sdspi_host.c:377-396`). GPIO34 ist input-only - das kann nicht gehen. Der
  SPI-Weg braucht einen echten freien Ausgang (im Code jetzt `SD_PIN_CS =
  GPIO13`) und damit eine zusaetzliche Leitung zum Kartenkontakt D3/CD.
- `gpio_pullup_en: input-only pad has no internal PU` und
  `gpio: conflict found for GPIO[13]` erscheinen im Log: der Treiber will auf
  dem CD-Pin einen internen Pull-up setzen (GPIO34 hat keinen), und GPIO13 war
  nicht verdrahtet.

**Offen / naechster sinnvoller Schritt:** siehe 6b - die SD-Fehler gab es
namlich schon VOR dem Modultausch.

### 6b. Der SD-Fehler ist NICHT neu - Zeitstrahl aus dem Log

Das Log widerlegt die naheliegende Annahme "das neue Modul ist schuld":

| Version | Datum | Modul | SD-Ergebnis |
|---|---|---|---|
| 0.6.3 | 27.09. | alt (COM3) | Fehler |
| 0.7.0 | 27.09. | alt | Fehler |
| 0.7.1 | 27.09. | alt | Fehler |
| **0.7.2** | 27.09. | alt | **Erfolg (Z6987, Z7377) UND Fehler (Z7560)** |
| 0.7.3 | 27.09. | alt | Erfolg |
| 0.8.0 | 27.09. | alt | Erfolg |
| ab 0.9.x | 02.10. | **neu (COM7)** | Fehler, reproduzierbar |

Zwei Dinge stehen damit fest:

1. **Erfolg und Fehler bei GLEICHER Firmware (0.7.2) am selben Tag und Modul.**
   Das ist die Signatur eines **Kontaktproblems** (Karte im Halter, Modul im
   Sockel), nicht eines Code- oder Konfigurationsfehlers.
2. **Der SD-Pfad ist mit identischem Code nachweislich gelaufen** - erfolgreich
   gemountet als `Type: SDHC, 60290MB, Speed: 20.00 MHz, SSR: bus_width=1`.

Der COM-Port wechselte am 01.10. von COM3 auf COM7; das ist der Modultausch
(die letzte erfolgreiche Sitzung war am 27.09. um 21:49 auf COM3).

### 6c. ESP32-WROOM vs WROVER-E - der Unterschied erklaert den SD-Fehler NICHT

Datenblatt ESP32-WROVER-E v2.4 (Module Overview, Pin Definitions):

| | ESP32-WROVER-E | ESP32-WROOM-32E |
|---|---|---|
| Chip | ESP32-D0WD-V3 bzw. D0WDR2-V3 | ESP32-D0WD-V3 |
| PSRAM | 8 MB (in-package) | keine |
| **GPIO16 / GPIO17** | **nicht herausgefuehrt** - im Modul als PSRAM_CS/PSRAM_CLK verbraucht | frei |
| VDD_SDIO | versorgt Flash **und** PSRAM | versorgt Flash |
| IO14 / IO15 / IO2 | vorhanden (SD_CLK / SD_CMD / SD_DATA0) | vorhanden |

Unsere Messung: altes Modul meldet `quad_psram: This chip is ESP32-D0WD`
(WROVER-Klasse mit 8 MB PSRAM), das neue `ESP32-D0WD-V3` rev v3.1 ohne PSRAM
(WROOM-Klasse).

**Fazit:** Die SD-Pins 14/15/2 sind auf beiden Modulen vorhanden und der Chip
ist dieselbe Baureihe. Der WROOM/WROVER-Unterschied erklaert den SDMMC-Fehler
also **nicht**. Was die fehlende PSRAM erklaert, ist unsere Speichernot
(`POOL_SMALL`, Puffer 40 -> 20 KB).

Nebenbefund aus dem Datenblatt, der zur Doppelbelegung passt: **IO12 ist
MTDI = SD_DATA2**, **IO15 ist MTDO = SD_CMD**, **IO5 ist GPIO5 = I2S-BCLK** -
alle drei sind Strapping-Pins (Tabelle 4: GPIO0=1, GPIO2=0, MTDI=0, MTDO=1,
GPIO5=1). Das gilt aber auf beiden Modulen gleich.

**Empfehlung:** Das Board ist fuer ein WROVER-Modul ausgelegt (die alte Platine
lief damit, inklusive PSRAM). Ein **ESP32-WROVER-E** wuerde beides beheben: die
SD-Frage klaeren **und** den vollen GMF-Pool samt 40-KB-Puffern zurueckbringen.
Der Fehler `0x107` beim allerersten CIU-Clock-Update ist ein reiner
Peripherie-interner Vorgang (ohne Karte, ohne Datenverkehr) - dafuer gibt es in
der Firmware keinen Hebel mehr.

## 6b. Mischbetrieb: GELOEST (0.9.21) - Mischer auf Kern 1

**Auf Hardware bestaetigt: "Sound ist jetzt super" (03.10.).**

Symptom vorher: Ruckler **und** Verzerrung vor allem in den tiefen Frequenzen -
an der Soundbar **und** am Headset, also nicht geraeteabhaengig.

Kette der Ursachen:

1. Der Mischer (`mixer_task`, Prio 15) lag auf **Kern 0** - zusammen mit dem
   Bluetooth-Stack (BTU_TASK 20, btController 23). Kern 0 war zu **85 % voll**
   (IDLE0 nur 15 %), Kern 1 zur Haelfte leer.
2. Wird der Mischer vom BT-Stack verdraengt, liefert der SBC-Encoder zu spaet,
   der A2DP-Sender (`a2dp_src_send`, Prio 10) bekommt nichts, und die Senke
   laeuft leer.
3. Zusaetzlich fuellt der Mischer bei Timeout mit Nullen auf
   (`esp_gmf_mixer.c:211-218`). Eine Folge solcher Null-Luecken ist kein
   Knackser, sondern **tiefe Verzerrung und Brummen** - genau das gehoerte Bild.

Messung davor: `Puffer I2S-Zweig: 90 % -> 50 % -> 22 % -> 90 %`,
`Minimum 4708 Byte` - eine Luecke von ~70 ms im Zubringer.

**Korrektur in 0.9.21:** `cfg.thread.core = 1` fuer `mixer_task`. Danach:

| Task | Kern | CPU | Prio |
|---|---|---|---|
| mixer_task | **1** | 18,5 % | 15 |
| i2s2bt_task | 1 | 3,2 % | 16 |
| io_i2s | 1 | 2,3 % | 16 |
| a2dp_src_send | 1 | 2,1 % | 10 |
| **IDLE0** | 0 | **38 %** (vorher 15 %) | - |
| IDLE1 | 1 | 24 % | - |

`CPU-Last: Kern0 23 %, Kern1 52 %`, Puffer I2S durchgehend 100 %,
`leer 0 mal`.

**Nicht die Ursache war** die Prioritaet der Audio-Tasks: `io_i2s` und
`i2s2bt_task` stehen mit 16 bereits ueber dem Datei-Zweig (15) und dem Mischer
(15), und die BT-Tasks (20/23) laufen auf dem anderen Kern. Die Reihenfolge
BT > I2S > Datei war korrekt - falsch war der KERN.

**Nebenwerkzeug `i2sstat` (0.9.21):** der I2S-Eingang fuehrt eine
Durchsatz-Statistik (`enable_speed_monitor`), die GMF nicht selbst loggt; das
Kommando liest sie aus. Der Wert ist mit Vorsicht zu lesen: gemessen 4190 statt
3840 kbit/s (+9 %), obwohl die Tonhoehe laut Hoerprobe stimmt - die Zeitbasis
der GMF-Statistik zaehlt offenbar nicht die reine Lesedauer. Als Nachweis
"es kommen Daten an" brauchbar, als Ratenmessung nicht.

## 6c. Rate folgt der Aushandlung (0.9.20)

Der SBC-Encoder resampelt nicht - er bekommt die Rate nur gesagt. Die Senke
handelt sie aus, und zwar **nicht stabil**: dieselbe Soundbar lieferte am
03.10. einmal 48000 Hz und nach erneutem Verbinden 44100 Hz; das Headset G435
44100 Hz. Mit einer festen 44100er-Kette zog die Senke 7,5 SBC-Rahmen je 20 ms,
waehrend wir nur ~6,9 lieferten - ihr Puffer lief leer.

`i2s2bt_set_stream` liest die Rate jetzt aus `codec_info` (SBC-Cfg) und stellt
die ganze Kette um: Mischer, Ratenwandler des Mischers, Ratenwandler des
Datei-Zweigs und `aud_lin_resample` (neu:
`aud_lin_resample_set_out_rate`, Verhaeltnis wird aus 60000/out_rate neu
gerechnet: 48000 -> 81920, 44100 -> 89088).

## 7. Datei-Zweig: offen - Speicher fuer die Ratenwandlung

Im No-PSRAM-Build bricht der Datei-Zweig ab:

```
E ESP_AE_RATE_CVT: Failed to allocate memory for 'coefficients matrix'(15360)
```

**GMF braucht dafuer KEIN PSRAM** - der Bedarf haengt nur von der
Ratenkombination ab (`espressif__esp_audio_effects/docs/README_RATE_CVT.md`,
Abschnitt "Heap Memory(Byte)"):

| Quelle -> Ziel | complexity 1 |
|---|---|
| 48000 -> Vielfaches von 4000 | **<2 kB** |
| **44100 -> 48000** | **<42 kB** |
| 44100 -> Vielfaches von 11025 | <2 kB |

Zwei Raten-Familien (Vielfache von 4000 und von 11025): **innerhalb** billig,
**zwischen** ihnen teuer. Belegt durch den Gegenversuch: mit
`test_tone_48k.wav` (48k -> 48k) laeuft `aud_rate_cvt_file_open` fehlerfrei
durch.

**Loesung (entschieden):** unseren eigenen linearen Umsetzer auch im
Datei-Zweig verwenden - ein paar Dutzend Byte statt 42 kB. Dafuer muss er die
Eingangsrate aus der Datei uebernehmen (Gegenstueck zu
`aud_lin_resample_set_out_rate`).

## 7. Dateien

| Datei | Zweck |
|---|---|
| `V4_ESP32/main/stream_proc.c` | Mischer, Verkabelung, Startfolge, CPU-Anzeige, Laufzeit-Umschaltung |
| `V4_ESP32/main/linear_resample.c/.h` | linearer Q16.16-Umsetzer (60k/32 -> 44,1k/16) - **bester Fall** |
| `V4_ESP32/main/sd_card.c/.h` | SDMMC mit SPI-Fallback auf denselben Leitungen |
| `V4_ESP32/main/pool_reg.c` | Elementregistrierung (Umsetzer, Wandler, Mixer ...) |
| `V4_ESP32/version.txt` | Version (wird beim Start ausgegeben) |
| `.tmp/mess_bauen.ps1` | Bauen der Messfälle `-Mode 0/1/2/3/4 [-NoPsram] [-Small]` |
| `.tmp/MESSREIHE_ABLAUF.md` | Ablauf der Messreihe mit den Konsolenbefehlen |
| `.tmp/flash_und_monitor.bat` | Flashen + Anzeige in einem Fenster |
| `.tmp/port.txt` | COM-Anschluss (wechselt nach Neustart) |

### 7a. Aufgeraeumt in 0.9.14

Entfernt (jeweils toter Code, der Pool-Speicher belegte ohne je zu laufen):

- `main/shift16.c/.h` samt Registrierung `aud_shift16` - der eigene
  32->16-Bit-Shifter aus Messfall 4, gesperrt wegen des GMF-Absturzes.
- `main/copier_forward.c/.h` samt Registrierung `aud_copier_i2s` - der Copier
  fuehrte die I2S-Kette an, seit diese in Messfall 2 nur noch aus
  `aud_lin_resample` besteht aber in keiner Kette mehr benutzt.

Im Image verifiziert: `aud_shift16`, `SHIFT16`, `aud_copier_i2s` und
`copier_forward` kommen nicht mehr vor, alle benoetigten Elemente
(`aud_lin_resample`, `aud_bit_cvt_i2s`, `aud_rate_cvt_i2s`, `aud_mixer`,
`aud_enc_mix`, `aud_ch_cvt_file`) sowie die Kommandos `i2smode`/`bufstat` sind
weiterhin enthalten. Image: 2 132 960 Byte (vorher 2 137 072).

## 8. Bedienung der Messreihe (Kurzfassung)

Der Messfall liegt im **NVS** und uebersteht damit einen Neustart (ohne das war
die Umschaltung wirkungslos: `i2smode 4` -> `restart` -> es lief wieder 2).

```
i2smode            zeigt den Fall
i2smode <0..4>     setzt ihn (wird gespeichert)
restart            baut den Zweig neu
connect <MAC>      Kopfhoerer verbinden (verbindet sich oft selbst)
i2s_media          I2S-Eingang anfordern
start_media        Stream starten
bufstat [reset]    Pufferstatistik zeigen / nullen
mixer [p] [t]      Wartezeiten (prefill/transit) setzen
```

**Wichtig bei Automatisierung:** Befehle erst senden, wenn der Chip fertig
gebootet hat. Ein `i2s_media` waehrend des Bootens geht verloren (auf Hardware
passiert: Befehl bei Uptime 8,2 s, danach lief der Stream ohne I2S-Zweig).

---

## 9. Stand 03.10.2026 abends (0.9.25 bis 0.9.31)

### 9.1 Der Datei-Zweig hat einen eigenen Umsetzer (ab 0.9.25)

GMFs `aud_rate_cvt` fordert fuer die Wandlung zwischen den Raten-Familien
(44100 <-> 48000) eine Koeffizienten-Matrix von **15360 Byte am Stueck** an.
Auf dem Modul ohne PSRAM scheitert das an der Zersplitterung (gemessen:
82008 Byte frei, 15360 gebraucht, trotzdem kein zusammenhaengender Block):

```
E ESP_AE_RATE_CVT: Failed to allocate memory for 'coefficients matrix'(15360)
```

Deshalb macht der Datei-Zweig Rate und Bittiefe jetzt selbst:

* `aud_lin_resample` ist zu einer konfigurierbaren Instanz erweitert
  (`aud_lin_resample_cfg_t`: tag, in_bits, in_rate, out_rate).
* `aud_lin_resample_file` ist die Datei-Instanz: 16 Bit, Rate 0 =
  "aus der Toninformation der Quelle", Ausgang 48000.
* Kette: `{aud_dec, aud_lin_resample_file, aud_ch_cvt_file}`.
* Das Element uebernimmt **Rate und Kanalzahl** aus der Toninformation. Die
  Kanalzahl ist wichtig: eine Mono-WAV wurde vorher als Stereoframe gelesen
  (zwei Monosamples = ein Frame) und lief dadurch doppelt so schnell.
* GMF braucht kein PSRAM - der lineare Umsetzer braucht ein paar Dutzend Byte.

### 9.2 Fehler, die dabei gefunden und behoben wurden (jeweils mit Beleg)

| Version | Symptom | Ursache | Behebung |
|---|---|---|---|
| 0.9.25 -> 0.9.26 | `LoadProhibited`, EXCVADDR 0x80092968, Absturz beim Pipeline-Aufbau | `esp_gmf_obj_set_config` merkt sich nur den **Zeiger** (esp_gmf_obj.c:71); die Stack-Kopie war nach dem Return tot, `esp_gmf_obj_dupl` reichte sie an `new_obj` weiter (addr2line: esp_gmf_obj_set_tag <- lin_resample_new <- esp_gmf_obj_dupl) | Heap-Kopie der cfg, freigegeben in `del_obj` |
| 0.9.26 -> 0.9.27 | Konsole tot, kein `version` mehr | `esp_gmf_pipeline_stop` auf einen **schon angehaltenen** Datei-Zweig: esp_gmf_task.c:720 wartet nach dem Timeout mit `0xFFFFFFFF` **unendlich** auf das STOP-Bit | Stop nur bei RUNNING/PAUSED |
| 0.9.27 -> 0.9.28 | Dateien verzerrt | Ausgangspuffer mit `in_frames * ratio` statt `in_frames / ratio` angefordert -> jeder Block brach zu frueh ab, Restposition lief ins Minus, ab -1 Frame lieferte das Element **nichts** mehr (`out_samples=0 (Ausgang leer!)`) | Teilen statt multiplizieren, auf 1024 Byte aufrunden |
| 0.9.28 -> 0.9.29 | Ton faellt nach ein paar Sekunden ganz aus | BT-Stack ohne Speicher: `E BT_OSI: calloc failed (size=622)`, `Failed to send frame batch: ESP_ERR_NO_MEM` - bei jedem Block andere Puffergroesse -> Heap zersplittert (27 KB frei, groesster Block 1,4 KB) | Groesse auf 1024 Byte aufrunden; Pool registriert nur die Elemente des Messfalls |
| 0.9.30 -> 0.9.31 | Watchdog-Neustart nach einem Dateifehler (`rst:0x8 TG1WDT_SYS_RESET`) | `esp_bt_audio_media_stop()` **direkt im GMF-Event-Callback** -> Ringschluss (GMF-Task wartet auf BT, BT wartet auf GMF-Task) | nur vormerken, im `stream_proc_task` ausfuehren |
| 0.9.30 | `E WAV_Parser: Not a WAV file` -> Job failed -> ERROR | `playfile` traf den Datei-Zweig mitten im Zustand OPENING und baute den Decoder darunter um | bis zu 1,5 s auf das Ende des Oeffnens warten |

### 9.3 Offen: der MP3-Decoder braucht 28 KB am Stueck

Espressif gibt fuer den MP3-Decoder 28 KB Speicher an (esp_audio_codec,
Tabelle "Decoder"). Gemessen mit dem erweiterten `free`-Befehl:

| Zustand | frei | groesster zusammenhaengender Block |
|---|---|---|
| nach dem Start, kein Stream | 119716 B | 63488 B |
| Stream laeuft, MP3 spielt | 27632 / 27264 / 27544 B | 2432 / 1472 / 2176 B |
| nach MP3-Ende + WDT-Neustart (leer) | 119796 B | 63488 B |

Folge: die **erste** Datei nach `start_media` spielt (da ist noch ein grosser
Block frei), jede weitere scheitert:

```
E ESP_MP3_DEC: Fail to init MP3 decoder ret 10   (20x)
E AUD_SDEC: Decode error reach limited 20
E ESP_GMF_TASK: Job failed [... aud_dec_proc]
I STREAM_PROC: [a2dp source pipeline] state => ERROR(7)
```

Der Decoder belegt beim Oeffnen 28 KB und gibt sie am Dateiende frei; der
Bluetooth-Stack fordert waehrenddessen laufend ~620-Byte-Puffer an und setzt
sie in genau diese Luecke.

**Massnahme 0.9.31** (im Quelltext, Build steht aus):
* `io_i2s`-Datenbus 12 KB -> 6 KB (`i2s_input.c`)
* Datei-Ringpuffer 12 KB -> 4 KB (`FILE_DB_ITEMS 4`), I2S-Ring bleibt 12 KB
* Mischer-Kette ohne `aud_rate_cvt`/`aud_bit_cvt` (beide waren Durchlaeufer:
  esp_gmf_rate_cvt.c:61 setzt `bypass = src_rate == dest_rate`)
* `free` gibt zusaetzlich mit `heap_caps_print_heap_info()` jeden
  Heap-Bereich einzeln aus

### 9.4 Bedienhinweise, die sich bewaehrt haben

* Nach jedem Flashen: **Stromzyklus von Hand** (der Chip bleibt im Bootloader).
* `playfile <name>` braucht einen **laufenden** Stream, sonst fuellt der
  Datei-Zweig nur seinen Ringpuffer und der Mischer zieht nichts.
* Dateien immer mit Abstand senden (>= 9 s), sonst trifft der naechste Befehl
  ein laufendes Oeffnen.
* Neue Messhilfen: `free` (frei, Minimum, groesster Block, Bereiche),
  `i2sstat`, `bufstat [reset]`, `tasks`.

---

## 10. Stand 03.10.2026 spaet (0.9.31 bis 0.9.39) - Datei-Zweig laeuft sauber

Ergebnis: **MP3 und WAV klingen sauber, auch im schnellen Wechsel (6 s Takt,
8 Titel), die Vampire ist durchgehend zu hoeren. Im ganzen Lauf 0 Resets,
0 Job-Fehler.**

### 10.1 Die vier echten Ursachen (jeweils mit Messbeleg)

| Version | Symptom | Ursache | Behebung |
|---|---|---|---|
| 0.9.31 | Ton faellt nach Sekunden aus | `esp_bt_audio_media_stop()` **direkt im GMF-Event-Callback** -> Ringschluss GMF-Task <-> BT -> `rst:0x8 TG1WDT_SYS_RESET` | nur vormerken, im `stream_proc_task` ausfuehren |
| 0.9.34 | Dateien verzerrt/kratzig | Uebergangswert an der Blockgrenze war das **vorletzte** Sample (`last_idx = (pos - ratio_fixed) >> 16`) statt des **letzten** (`in_frames-1`). Jeder Block verwarf damit sein letztes Sample und wiederholte das vorherige | Cache immer aus `in_frames-1` fuellen |
| 0.9.35/0.9.37 | WAV klingt **stotternd** (Sinus) | Der Mischer holt je Aufruf `10 ms * rate * kanaele * bits / 8000` = **1920 Byte** (esp_gmf_mixer.c:24) und fuellt den Rest mit **Nullen** (Zeile 216). Der Datei-Zweig lieferte nur 1024 Byte **und** kam nur auf **163000 Byte/s** statt 192000, weil `FILE_IO_CFG_DEFAULT()` **cache_size = 0** setzt (jeder 512-Byte-Lesevorgang ging direkt auf die SD-Karte) | Mischer-Blockgroesse auf 1024 Byte; `cache_size = 4096` fuer die Datei-IO |
| 0.9.38/0.9.39 | Neustart beim Titelwechsel | Zwei Aufgaben bauten gleichzeitig um (Konsolen-`playfile` und `stream_proc` bei Dateiende) **und** die Zustandspruefung war falsch: `esp_gmf_pipeline_t::state` stand auf **OPENING**, waehrend ein 5-Minuten-MP3 lief -> der Zweig wurde nicht gestoppt, der Decoder mitten im Titel umkonfiguriert (`E WAV_Parser: Not a WAV file`) | Sperre (Mutex) fuer beide Wege **und** eigener Merker `s_local2bt_laeuft` statt des Zustandsfelds |

### 10.2 Die Messungen, die den Weg gewiesen haben

**Durchsatz des Datei-Zweigs** (Soll bei 48 kHz stereo: 192000 Byte/s):

| Version | Datei-Zweig | I2S-Zweig | Folge |
|---|---|---|---|
| 0.9.36 (ohne Datei-Cache) | **158702 / 162279 / 163766** | 186959 | Datei stottert |
| 0.9.37 (Cache 4096) | Ring nie unter 3684 Byte | - | Datei sauber |

**Ringpuffer im 2-ms-Raster** (der Mischer braucht immer 1024 Byte am Stueck):

```
0.9.37 waehrend WAV: 197 Messungen in 2 s | Minimum 3684, Maximum 8192 | 0 mal unter 1024 (0.0 %)
0.9.36 vorher      : Durchsatz zu klein -> Puffer laeuft leer -> Nullen im Mischer
```

**Selbsttest 1:1** (unser Umsetzer gegen die Originaldatei
`ESP32-I2S-to-BT/test_tone_48k.wav`, 440-Hz-Sinus, 48 kHz, mono):

```
Datei:  s[254]=14438  s[255]=13969  s[256]=13453
0.9.33: erstes Ausgangssample von Block 2 = 14438   (falsch, = s[254])
0.9.34: erstes Ausgangssample von Block 2 = 13969   (richtig, = s[255])
```

**Speicher** (WROVER, COM3, kein PSRAM, Flash 40 MHz):

| Zustand | frei | groesster Block |
|---|---|---|
| 0.9.30 waehrend MP3 | 27632 B | **1472 B** -> MP3-Decoder (28 KB) scheiterte |
| 0.9.39 waehrend Stream | 71804 B | **32768 B** -> reicht |

**CPU-Last 0.9.39** (Stream laeuft): Kern0 33-41 %, Kern1 79-80 %;
mixer_task 23 %, i2s2bt_task 8,5 %, io_i2s 4,6 %.

### 10.3 Was am Datei-Zweig jetzt anders ist

* Kette: `{aud_dec, aud_lin_resample_file}` - der fremde Kanalwandler ist weg.
* `aud_lin_resample_file` gibt **immer stereo** aus und kopiert Mono selbst auf
  beide Kanaele; Rate und Kanalzahl kommen aus der Toninformation der Quelle.
* Datei-Ringpuffer 8 KB (> ein Block: 1152 Frames = 5016 Byte nach der
  Ratenwandlung) und wird **vor jedem Titel geleert**.
* Datei-IO mit 4 KB Cache.
* Mischer-Blockgroesse fest 1024 Byte statt 1920 (10 ms).
* Wechsel laufen ueber einen eigenen Merker + Sperre, nie ueber das
  Zustandsfeld der Pipeline.

### 10.4 Offene Punkte

* Die Diagnosezeilen (`Block n: in_frames=...`, Durchsatz, 2-ms-Raster) sind
  noch aktiv. Sie kosten UART-Zeit im Audio-Task und sollten fuer den
  Dauerbetrieb abgeschaltet werden (Schalter oder Log-Level).
* `test.mp3` ist 5 Minuten lang (4630501 Byte) - gut fuer Dauertests.
* Der MP3-Decoder braucht 28 KB am Stueck; mit 32 KB groesstem Block ist der
  Abstand jetzt komfortabel, ein Ballon (Reservierung) ist nicht noetig.
* Noch offen: I2C-Protokoll fuer die V4, EQ/Hall, SBC-Rate 44100 vs 48000.
