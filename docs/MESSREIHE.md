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

---

## 11. Nachtrag 04.10.2026: leises Knacken im Sekundentakt (0.9.40 -> 0.9.41)

Nach dem Aufraeumen (0.9.40) war der Ton der Dateien sauber, aber die Vampire
hatte ein **leises Knacken etwa jede Sekunde**.

Ursache: die Puffer-Diagnose. `buffer_mon_task` lief im **Sekundentakt** und las
dabei ueber `esp_gmf_db_get_filled_size()` den Fuellstand **beider Ringpuffer** -
auch designigen des I2S-Zweigs. Dieses Lesen nimmt kurz die Sperre des
Ringpuffers. Der I2S-Zubringer (Echtzeit!) musste in dieser Zeit warten, und
diese kleine Luecke war als Knacken zu hoeren. Der Task lief zwar mit niedriger
Prioritaet (2), aber die Sperre wirkt unabhaengig von der Prioritaet.

Behebung: die komplette Puffer-Diagnose ist entfallen - der Task, die
Zaehlertabelle, `bufstat` und die 2-ms-Messung. Ein Ringpuffer wird jetzt nur
noch beim Titelwechsel gelesen (dort, wo er ohnehin geleert wird) und auf
ausdrueckliche Anfrage.

Lehre fuer die Zukunft: **im Audio-Pfad nichts messen.** Ein Ringpuffer, der von
einem Echtzeit-Zweig beschrieben wird, darf nicht nebenher abgefragt werden -
auch nicht "nur lesend" und auch nicht aus einem Task mit niedriger Prioritaet.

Messwerte 0.9.41 (WROVER, kein PSRAM, Soundbar 48000 Hz, Dateien im Wechsel):
Stream laeuft, 0 Resets, 0 Job-Fehler, freier Heap 78 204 Byte, groesster
zusammenhaengender Block 30 720 Byte, CPU Kern0 37-40 %, Kern1 71-73 %.

## 12. Nachtrag SD-Karte (Messungen 0.9.42 - 0.9.46, 04.10.)

Alles hier ist am laufenden Geraet gemessen, nicht aus Quellen geschlossen.

### 12a. Der SD-Takt ist es nicht (0.9.42)

`SD_MAX_FREQ_KHZ` von 20000 auf 4000 und `SD_SPI_MAX_FREQ_KHZ` von 10000 auf
1000 gesenkt, auf den WROOM geflasht. Ergebnis unveraendert:

```
I (46463) SD_CARD: SDMMC-Versuch 1/3 (max 4000 kHz)
E (47468) SD_HOST: sd_host_slot_clock_update_command(993): sd_host_start_command returned 0x107
E (47469) SD_HOST: sd_host_slot_set_card_clk(568): ... returned 0x107, failed to disable clk
```

Flash-Takt im geflashten Image gegengeprueft: `FLASHFREQ_40M` in
`build/config/sdkconfig.h`, Strings `SPI Speed` / `40MHz` im Bootloader-Binary,
`spi_speed` im Image-Header = 0.

### 12b. Karte, Halter und Verkabelung sind es nicht (0.9.42)

- **Karte herausgezogen**: identischer 0x107 im CIU-Clock-Update.
- **SD-Breakout komplett von den ESP32-Pins abgeklemmt**: identischer 0x107.

Der Fehler tritt also auf, bevor ein Bit zur Karte geht. (GPIO34 als Card-Detect
ist dabei wertlos: der Pin floatet ohne externen 10k nach 3,3 V auf LOW, meldet
also immer "Karte gesteckt".)

### 12c. Registerdiagnose `sdreg` (0.9.43 ff.)

Neues CLI-Kommando (main/cmd_reg.c): liest `DPORT_WIFI_CLK_EN`,
`DPORT_CORE_RST_EN` und den SDMMC-Block aus, setzt ein CIU-Update-Clock-Kommando
genau wie `sd_host_slot_clock_update_command` ab und pollt `start_command`.

Befund bei geschlossenem Taktgate (`DPORT_WIFI_CLK_EN` Bit 13 = 0):

```
DPORT_WIFI_CLK_EN = 0xffff8800  SDIO-Host-Takt (Bit13): AUS
SDMMC VERID=0000b7cf HCON=0000b7cf CTRL=0000b7cf CLKDIV=0000b7cf CLKSRC=0000b7cf CLKENA=0000b7cf CLOCK=00020224
```

**Alle Register liefern denselben Wert `0x0000b7cf`, nur `CLOCK` (0x800) nicht.**
Das ist kein Datenfehler, sondern ein nicht getakteter Block: bei fehlendem
Modultakt treibt der Peripherieblock den APB-Bus nicht, die gelesenen Werte sind
Zufall. `VERID` ist deshalb die Lebendigkeitspruefung: **nur `0x5342270a`
(Synopsys-Version) heisst "Block lebt".** Wichtig: bei totem Block ist auch ein
`start_command = 0` kein Erfolg, sondern ein **verlorener Schreibzugriff** - der
erste Testaufbau hat das falsch als "angenommen" gewertet.

Zustaendig fuer das Gate ist `esp_perip_clk_init()`
(`components/esp_system/port/soc/esp32/clk.c:250-256,310`): es loescht beim Start
`DPORT_WIFI_CLK_SDIO_HOST_EN` zusammen mit den WiFi/BT-Bits. Im ganzen IDF
schreiben nur zwei Stellen dieses Bit: diese Initialisierung und der
SDMMC-Treiber (`esp_hal_sd/esp32/include/hal/sdmmc_ll.h:122-130`).

### 12d. Das Gate ist NICHT die Ursache des Mountfehlers (0.9.46)

Der Sampler (zweiter Task, 100-us-Raster) laeuft waehrend `sdmmc_card_init()`
mit. Ergebnis:

```
Gate zwangsweise AUS -> sdmmc_host_init() -> Gate jetzt AN   (Treiber oeffnet das Gate)
Hostteiler 10 (SDMMC.clock 0x800) programmiert -> CIU nimmt an
sdmmc_card_init()  -> 0x107
  danach: start_command=1  STATUS=00000306 (data_busy=1)   Gate die ganze Zeit AN
```

Damit ist belegt: der Treiber oeffnet das Taktgate selbst, das Gate bleibt
waehrend des Fehlers offen, und die Taktprogrammierung ist unschuldig.

### 12e. Der Fehler ist modulabhaengig - Gegenprobe WROVER (0.9.46)

Identischer Build (NoPsram, Flash 40 MHz, SD-Takt 4000 kHz) auf beide Module:

| Modul | Ergebnis |
|---|---|
| WROVER (COM3, `68:fe:71:91:08:8c`) | `I (1125) mounted at /sdcard (SDMMC 1 Bit)`, Name USDU1, SDHC - **103 ms** |
| WROOM (COM7, `ec:c9:ff:fd:60:c0`) | `0x107` im ersten CIU-Clock-Update, Timeout nach 1 s |

**Damit ist die Firmware als Ursache widerlegt** (der WROVER mountet mit
demselben Build) und der Rest ist modul-/platinenspezifisch.

Registervergleich am selben Codepunkt (`sdreg` auf beiden Modulen):

| | WROVER | WROOM |
|---|---|---|
| `DPORT_WIFI_CLK_EN` | `0xffffabc9` (Bit13 AN) | `0xffff8800` (Bit13 AUS, nach Fehlversuch) |
| WiFi/BT-Common-Bits (0,3,6,7,9) | gesetzt (Karte war gemountet) | nicht gesetzt - **Messartefakt, siehe 12n** |
| `SDMMC VERID` / `HCON` | `5342270a` / `03c44c83` | identisch |
| CIU-Test mit 400-kHz-Teilern | angenommen (1 us) | angenommen (1 us) |

Die CIU antwortet auf **von Hand** abgesetzte Kommandos auf beiden Modulen
gleich - der Unterschied liegt also nicht im Registerverhalten des Blocks.

### 12f. Was als Naechstes zu pruefen ist

1. **Pull-ups - geprueft am 04.10.: ERLEDIGT, nicht die Ursache.** CLK (GPIO14),
   CMD (GPIO15) und DAT0 (GPIO2) haben je 10k nach 3,3 V. Ohnehin entlastet
   dadurch, dass der Fehler auch bei **komplett abgeklemmtem SD-Modul** auftrat,
   also ganz ohne Leitungen und Pull-ups.
2. **PSRAM-Konfiguration als 1-Variable-Test.** Die Taktquelle des SDMMC ist
   `SOC_MOD_CLK_PLL_F160M` (`soc/esp32/include/soc/clk_tree_defs.h:460-468`),
   bei CPU 240 MHz also 160 MHz - in beiden Builds gleich. Ein PSRAM-Build auf
   dem WROOM (`SPIRAM` + `SPIRAM_IGNORE_NOTFOUND`, laeuft auch ohne PSRAM-Chip)
   testet die Vermutung trotzdem direkt.
3. **Bisect im Treiber:** `sd_host_slot_set_card_clk()` direkt aufrufen und ihre
   Schritte einzeln durch eigene Registerzugriffe ersetzen. Offen ist, warum
   derselbe Registerinhalt von eigenem Code angenommen wird, vom Treiberpfad
   aber nicht.

### 12g. PSRAM ist es auch nicht (0.9.47, 04.10.)

Die letzte systematische Konfigurationsdifferenz zwischen WROVER und WROOM ist
PSRAM - und dafuer gibt es sogar einen Mechanismus: mit PSRAM laeuft beim Start
die MSPI-Timing-Abstimmung, die Flash/PSRAM-Takt und damit die **SPLL**
konfiguriert; die SDMMC-Quelle `PLL_F160M` ist SPLL/3
(`soc/esp32/include/soc/clk_tree_defs.h:460-468`). Genau diese Kopplung steckt
auch hinter dem alten Befund "Flash 80 MHz -> SD faellt aus".

Test als 1-Variable-Experiment: PSRAM-Build auf den WROOM. Der Build laeuft dort
auch ohne PSRAM-Chip, weil `CONFIG_SPIRAM_IGNORE_NOTFOUND=y` gesetzt ist
(verifiziert im gebauten `build/config/sdkconfig.h`):

```
#define CONFIG_SPIRAM 1
#define CONFIG_SPIRAM_SPEED_40M 1
#define CONFIG_SPIRAM_BOOT_INIT 1
#define CONFIG_SPIRAM_IGNORE_NOTFOUND 1
#define CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 240
#define CONFIG_ESPTOOLPY_FLASHFREQ "40m"
```

Ergebnis auf dem WROOM (0.9.47):

```
E (899) quad_psram: PSRAM ID read error: 0xffffffff, PSRAM chip not found or not supported
I (1084) SD_CARD: SDMMC-Versuch 1/3 (max 4000 kHz)
W (2125) SD_CARD: SDMMC-Versuch 1 fehlgeschlagen: ESP_ERR_TIMEOUT
```

**Unveraendert 0x107.** Zusammen mit der WROVER-Zeile "PSRAM aus, 40 MHz Flash,
SD laeuft" (Abschnitt 6) ist PSRAM damit in beide Richtungen widerlegt: die
Konfiguration ist es nicht, und das Vorhandensein des PSRAM-Chips kann es nicht
sein, weil der WROVER auch ohne PSRAM-Konfiguration mountet.

### 12h. Zwei Fehler auf demselben Board - einer davon war unserer

Auf dem WROOM fallen **beide** Wege aus:

| Weg | Peripherie | Fehler |
|---|---|---|
| SDMMC 1 Bit | SDMMC-Host, IOMUX 14/15/2 | 0x107 im CIU-Clock-Update |
| SPI | SPI2-Host, 14/15/2 + CS 13 | `send_if_cond (1) returned 0x108` - CMD8 ohne Antwort |

**KORREKTUR (04.10., vom Betreiber der Platine): GPIO13 ist auf dieser Platine
NICHT verdrahtet.** Damit ist der SPI-Fehler vollstaendig erklaert und die
Deutung "zwei unabhaengige Fehler, also die Kartenanbindung" war falsch: der
SPI-Weg benutzt GPIO13 als CS (`sd_card.c`, `SD_PIN_CS`), die Karte bekommt also
nie ein Chip-Select. `send_if_cond (1) returned 0x108` ist die zwangslaeufige
Folge. Der SPI-Fallback bleibt als Absicherung im Code, kann auf DIESER Platine
aber prinzipbedingt nicht funktionieren.

Damit bleibt als einziger echter Fehler der SDMMC-Weg (Abschnitt 12j/12k).

### 12i. Taktlage gemessen - auch nicht die Ursache (0.9.48)

`sdreg` gibt jetzt die tatsaechliche Taktlage aus (`esp_clk_cpu_freq()`,
`esp_clk_apb_freq()`, `esp_clk_tree_src_get_freq_hz` fuer XTAL/PLL_D2/PLL_F160M,
dazu die Flash-Register). WROOM (COM7):

```
Takt: CPU=240000000 Hz, APB=80000000 Hz, XTAL=40000000 Hz,
      PLL_D2=240000000 Hz -> PLL=480000000 Hz, PLL_F160M=160000000 Hz
Flash-Register: SPI0.clock=0x00001001 SPI1.clock=0x00001001
```

**Damit stimmt die Annahme des Treibers exakt.** Wichtig zum Hintergrund: der
SDMMC-Host haengt auf dem ESP32 FEST an PLL_F160M (PLL/3), weil
`sdmmc_ll_select_clk_source()` ein No-Op ist
(`esp_hal_sd/esp32/include/hal/sdmmc_ll.h:218-221`) und
`esp_clk_tree_enable_src()` fuer PLL_F160M nur einen Zaehler fuehrt, ohne ein
Register anzufassen (`esp_hw_support/port/esp32/esp_clk_tree.c:114-153`).
`esp_clk_tree_src_get_freq_hz(PLL_F160M)` liefert nur die **Konstante**
`CLK_LL_PLL_160M_FREQ_MHZ` (ebd.:44-46) - der Wert "src_freq_hz: 160000000" im
Treiberlog ist also eine Annahme. Hier ist er nachgemessen und richtig.

### 12j. Die CIU braucht den Kartentakt - und ein Widerspruch bleibt (0.9.48)

Neuer Test in `sdreg`: ein **CMD8 mit Ruecklesung der Kommando-FSM**. Grund: ein
Update-Clock-Kommando laesst sich nicht von einem verlorenen Schreibzugriff
unterscheiden (beide lassen `start_command = 0` lesen) - ein normales Kommando
mit Antwortpflicht bewegt dagegen die FSM.

```
CMD8 bei Kartentakt AUS: start_command steht noch (5 ms), FSM-Verlauf: idle
CMD8 bei Kartentakt AN : start_command geloescht nach 2 us, FSM-Verlauf: idle
```

Ergebnis: **die CIU verarbeitet ein normales Kommando nur bei laufendem
Kartentakt.** Fuer das Update-Clock-Kommando gilt das nicht - die eigenen
Testkommandos werden auch bei abgeschaltetem Takt angenommen, sind also echte
Annahmen und keine Schreibverluste.

Offen bleibt ein Widerspruch: nach dem fehlgeschlagenen `sdmmc_card_init` steht
`CLKENA = 0x00020000`, also Bit 17 (Kartentakt-Low-Power). Dieses Bit setzt der
Treiber nur in `sd_host_slot_set_card_clk()` **nach** einem erfolgreichen ersten
Update-Kommando (`sd_host_sdmmc.c:599`). Es muss also vorher schon ein Update
durchgelaufen sein - und `sd_host_slot_start_command()` bricht sowohl beim ersten
Warten (auf `start_command == 0`, Zeile 951-960) als auch nach dem Schreiben
(Zeile 967-976) mit demselben `ESP_ERR_TIMEOUT` ab. Der naechste Schritt waere
eine **Registerverlaufsaufzeichnung** (Sampler, der `start_command`, `clkena`,
`clkdiv`, `clock` und `status` waehrend `sdmmc_card_init` auf Aenderungen
mitschreibt) - damit ist sichtbar, welches der drei Update-Kommandos haengen
bleibt.

### 12k. Registerverlauf waehrend sdmmc_card_init (0.9.49)

Der Sampler zeichnet jetzt jede Aenderung von `start_command`, `clkena`,
`clkdiv`, `clock` (0x800) und `status` auf (50-us-Raster, zweiter Kern),
waehrend der Konsolen-Task in `sdmmc_card_init()` blockiert:

```
[sdtrace] Start: Gate=AN
[sdtrace]    404 us: start=0 clkena=00000000 clkdiv=00001400 clock=00020224 status=00000106
[sdtrace]  19654 us: start=0 clkena=00000000 clkdiv=00001400 clock=00021224 status=00000106
[sdtrace]  21068 us: start=1 clkena=00020000 clkdiv=00001400 clock=00129224 status=00000306
E SD_HOST: sd_host_slot_clock_update_command(993): sd_host_start_command returned 0x107
```

Ausgewertet:

- Bei **404 us** ist die Karte **nicht** busy (`status` Bit 9 = 0).
- `clock=00021224` ist ein Zwischenstand der Teilerprogrammierung
  (`sdmmc_ll_set_clock_div` schreibt `div_factor_h`, `_l`, `_n` einzeln), danach
  steht der Endwert `000129224` = Hostteiler 10 = 400 kHz.
- `clkena=00020000` heisst: **Low-Power-Bit (Bit 17) gesetzt** - das setzt der
  Treiber nur in `sd_host_slot_set_card_clk()` nach einem *erfolgreichen* Update
  (`sd_host_sdmmc.c:599`). Der Treiber ist also ueber die Teilerprogrammierung
  hinausgekommen.
- Bei **21 ms** ist `status` Bit 9 = 1: **die Karte zieht DAT0 auf LOW (busy)** -
  und `start_command` bleibt stehen. Der Host verweigert das Kommando, solange
  die Karte busy ist.

Im selben Lauf wurden beide CMD8-Varianten angenommen (18 us / 14 us), im Lauf
davor nur die mit Kartentakt. Die Annahme haengt also **nicht** allein am
Kartentakt, sondern am **busy-Zustand der Karte**.

### 12l. GPIO13 ist auf der Platine nicht verdrahtet

Vom Betreiber der Platine: **CS/DAT3 (GPIO13) ist nicht angeschlossen.** Damit:

- Der **SPI-Fallback** kann dort nicht arbeiten (CS fehlt) - siehe Korrektur in
  12h. Alle SPI-Fehlermeldungen dieser Messreihe sind dadurch erklaert und
  duerfen NICHT als Kartenfehler gedeutet werden.
- Die Massnahme aus 0.9.50 (`sd_dat3_high()`, GPIO13 vor dem SDMMC-Versuch als
  Ausgang HIGH) ist auf dieser Platine wirkungslos. Sie bleibt im Code, weil
  DAT3 im 1-Bit-SD-Betrieb laut Spezifikation HIGH liegen muss und der Treiber
  D1..D3 erst ab 4 Bit Breite anfasst
  (`esp_driver_sdmmc/src/sd_host_sdmmc.c:1376-1385`).
- Fuer SDMMC bleiben damit genau die drei Leitungen **CLK=GPIO14, CMD=GPIO15,
  D0=GPIO2** - alle drei haben 10k-Pull-ups nach 3,3 V (Abschnitt 12f).

Offen ist damit nur noch: warum haelt die Karte auf dem WROOM DAT0 busy,
waehrend sie auf dem WROVER (gleiche Platine, gleiche Leitungen, gleiche Karte,
gleicher Build) in 103 ms mountet.

### 12m. Registerverlauf WROVER gegen WROOM - die Karte antwortet nur auf dem WROVER (0.9.50)

Versuchsanordnung: **ein Board, ein Kartenslot, eine Karte, ein Build (0.9.50);
nur das ESP32-Modul wird im Sockel getauscht.** Auf beiden Modulen derselbe
Ablauf (`sd_unmount`, dann `sdreg` → Sampler laeuft waehrend
`sdmmc_card_init()`).

**WROVER:**

```
[sdtrace]    419 us: start=0 clkena=00000000 clkdiv=00001400 clock=00020224 status=00000106 (FSM=idle)
[sdtrace]  19482 us: start=0 clkena=00000000 clkdiv=00001400 clock=00129224 status=00000106 (FSM=idle)
[sdtrace]  20835 us: start=0 clkena=00020002 clkdiv=00001400 clock=00129224 status=00000116 (FSM=send_init)
[sdtrace]  33226 us: start=0 clkena=00020002 clkdiv=00001400 clock=00129224 status=00000106 (FSM=idle)
[sdtrace]  45598 us: start=0 clkena=00020002 clkdiv=00001400 clock=00129224 status=0001f906 (FSM=idle)
  sdmmc_card_init():       0x0
```

**WROOM:**

```
[sdtrace]    404 us: start=0 clkena=00000000 clkdiv=00001400 clock=00020224 status=00000106 (FSM=idle)
[sdtrace]  19654 us: start=0 clkena=00000000 clkdiv=00001400 clock=00021224 status=00000106 (FSM=idle)
[sdtrace]  21068 us: start=1 clkena=00020000 clkdiv=00001400 clock=00129224 status=00000306 (FSM=idle)
  sdmmc_card_init():       0x107
```

| Merkmal | WROVER | WROOM |
|---|---|---|
| `start_command` | bleibt 0, CIU nimmt jedes Kommando an | bleibt 1 |
| `clkena` | `0x20002` - Kartentakt **an** (Bit 1) | `0x20000` - Kartentakt **aus** |
| `status` | `0x1f906` - **`response_index` != 0, die Karte antwortet** | `0x306` - `data_busy=1`, `response_index=0` |
| Kommando-FSM | `send_init` -> idle | bleibt idle |
| `sdmmc_card_init()` | **0x0** | **0x107** |

**Schlussfolgerung:** Auf dem WROVER laeuft der Karten-Init normal durch
(Takt an, Kommando raus, Antwort da, `response_index` gesetzt). Auf dem WROOM
**antwortet die Karte nie** - sie geht nur auf busy. Da Board, Slot, Karte,
Leitungen, Pull-ups, Taktlage (12i), Build und Konfiguration identisch sind und
das Modul im Sockel getauscht wird, liegt die Ursache **im Modul bzw. in seinen
Kontakten**, nicht in Firmware oder Konfiguration.

Damit ist die Messreihe zum SD-Ausfall abgeschlossen. Fuer den WROOM bleiben
Hardware-Schritte: Kontaktierung von GPIO14/15/2 im Sockel pruefen (Durchgang
Modulpin -> Kartenslot), Modul fest setzen oder loeten, anderes Modul. Der
WROVER ist die Arbeitsgrundlage.

### 12n. Nachtrag: Artefakt korrigiert, GPIO16/17 frei, Kontaktspur (0.9.50)

Drei Punkte, die den Kreis schliessen:

1. **Der in 12e notierte Registerunterschied zwischen den Modulen war ein
   Messartefakt.** Mit 0.9.50 zeigt der WROVER im gleichen Zustand (nach
   `sd_unmount`) denselben Wert wie der WROOM:

   ```
   WROVER 0.9.50: DPORT_WIFI_CLK_EN = 0xffff8800  (Bit13 AUS)
   WROOM  0.9.49: DPORT_WIFI_CLK_EN = 0xffff8800  (Bit13 AUS)
   ```

   Der alte Wert `0xffffabc9` stammte aus einer Messung mit **gemounteter**
   Karte (dann haelt der Treiber das Taktgate offen). Ebenso identisch ist die
   Taktlage auf beiden Modulen:

   ```
   WROVER: CPU=240 MHz, APB=80 MHz, XTAL=40 MHz, PLL=480 MHz, PLL_F160M=160 MHz
   WROOM : CPU=240 MHz, APB=80 MHz, XTAL=40 MHz, PLL=480 MHz, PLL_F160M=160 MHz
   ```

2. **GPIO16/17 sind auf der Platine frei** (Auskunft des Betreibers). Damit ist
   auch der letzte strukturelle Unterschied zwischen WROOM und WROVER (PSRAM
   belegt dort 16/17) als Ursache ausgeschlossen.

3. **Verbleibende Erklaerung, die ALLE Messungen deckt: eine fehlerhafte
   Verbindung auf einer der drei Leitungen, insbesondere DAT0 (GPIO2).**
   Liegt DAT0 dauerhaft auf LOW (Kurzschluss gegen GND, Kontaktfehler im
   Sockel, defekter Modulpin), dann ergibt sich zwangslaeufig die gemessene
   Kette:

   | Messung | Folge eines DAT0-Kurzschlusses |
   |---|---|
   | `STATUS=0x306`, `data_busy=1` | DAT0 wird als busy gelesen |
   | `start_command` bleibt 1 (0x107) | die CIU sendet kein Kommando, solange die Karte busy ist |
   | `response_index = 0`, Karte antwortet nie | es wird nie ein Kommando gesendet |
   | WROVER laeuft mit demselben Build | dort ist die Verbindung in Ordnung |

   **Pruefungen:** (a) Widerstand jeder der drei Leitungen gegen GND und 3,3 V
   messen, WROOM gegen WROVER vergleichen; (b) Softwaretest `sdpins`: GPIO14,
   GPIO15 und GPIO2 als Ausgang treiben (low/high) und zuruecklesen - folgt der
   Ruecklesewert nicht, ist die Leitung kurzgeschlossen oder zu stark belastet.

### 12o. Die Ursache liegt auf GPIO2 = D0 = DAT0 (04.10., Befund des Betreibers)

Der Betreiber der Platine hat den ausschlaggebenden Hinweis geliefert:
**das WROVER-Modul laesst sich in der Platine nicht flashen - wegen GPIO2 -
das WROOM-Modul schon.**

Wichtig: gemeint ist **GPIO2 = D0 = DAT0**, nicht DAT2. Eine fruehere Fassung
dieses Abschnitts hatte DAT2/GPIO12 als Ursache notiert; das war eine falsche
Deutung des Hinweises. Die DAT2/GPIO12-Zusammenhаenge bleiben als Hintergrund
in Abschnitt 12p stehen, sind hier aber NICHT die Ursache.

**GPIO2 ist ein Strapping-Pin** und muss fuer den UART-Download-Modus LOW sein.
Genau daran haengt auch die SD-Karte: D0 (DAT0) ist dieselbe Leitung. Die
offizielle Doku beschreibt diesen Konflikt ausdruecklich
(https://docs.espressif.com/projects/esp-idf/en/release-v4.0/api-reference/peripherals/sd_pullup_requirements.html):

> **GPIO2 Strapping pin:** GPIO2 pin is used as a bootstrapping pin, and should
> be low to enter UART download mode. **You may find it unable to enter the UART
> download mode if you correctly connect the pullup of SD on GPIO2.** For
> WroverKit v3, there are dedicated circuits to pulldown the GPIO2 when
> downloading. ...
> Some boards have pulldown and/or LED on GPIO2. LED is usually ok, but
> **pulldown will interfere with D0 signals and must be removed.** Check the
> schematic of your development board for anything connected to GPIO2.

Damit passen beide Beobachtungen zusammen:

| Modul | Flashen | SD |
|---|---|---|
| WROOM | geht (GPIO2 beim Reset LOW genug) | faellt aus |
| WROVER | **geht nicht** (GPIO2 beim Reset nicht LOW genug) | **laeuft** |

Und es passt exakt auf die SD-Messungen: In allen fehlgeschlagenen Versuchen
meldete der Host **`data_busy = 1`**, also **DAT0 = LOW**, und nahm deshalb kein
Kommando an (`start_command` blieb 1, `response_index = 0`, `0x107`) - siehe
12k/12m. Eine Leitung, die auf D0 festgehalten wird, erzeugt genau dieses Bild;
die Doku nennt fuer solche Faelle ausdruecklich LED/Pulldown auf GPIO2 als
Ursache, die entfernt werden muessen.

**Auskunft des Betreibers (vollstaendig):** An GPIO2/D0 haengt **nur der
10k-Pull-up**, sonst nichts. Die microSD steckt beim Flashen. Und: **das
WROVER-Modul geht in dieser Platine nicht in den Download-Modus, es muss zum
Flashen aus dem Board genommen werden; das WROOM-Modul geht in-circuit.**

Daraus ergibt sich eine geschlossene Erklaerung, in der beide Beobachtungen
dieselbe Leitung betreffen:

| Modul | microSD-Zustand | DAT0/GPIO2 beim Reset | Folge |
|---|---|---|---|
| WROVER | Karte laeuft, DAT0 wird freigegeben | 10k-Pull-up zieht **HIGH** | SD laeuft - aber Download-Modus blockiert (Doku: "unable to enter UART download mode if you correctly connect the pullup of SD on GPIO2") |
| WROOM | Karte haengt **busy** und haelt DAT0 auf LOW | **LOW** | SD faellt aus (Host sieht busy, sendet nichts, 0x107) - Download-Modus funktioniert |

Der Pull-up auf D0 ist also gleichzeitig Voraussetzung fuer die SD-Funktion und
Hindernis fuer den Download-Modus. Die Doku nennt dafuer die Loesungen:
waehrend des Downloads GPIO2 herunterziehen (WROVER-KIT v3 hat dafuer eine
eigene Schaltung; als Behelf GPIO0 und GPIO2 mit einem Jumper verbinden) - ein
DAUERHAFTER Pulldown auf D0 ist dagegen nicht zulaessig ("pulldown will
interfere with D0 signals and must be removed").

**Praktische Konsequenz:** Zum Flashen des WROVER genuegt vermutlich schon das
Herausnehmen der microSD; erst wenn das nicht reicht, das Modul aus dem Sockel.

**Offen bleibt der eigentliche Defekt:** Warum haengt die Karte beim WROOM-Modul
in einem busy-Zustand fest (DAT0 dauerhaft LOW), waehrend sie beim WROVER
normal arbeitet? Das ist modul- bzw. kontaktspezifisch und der verbleibende
Punkt dieser Messreihe. Das Kommando `sdpins` (0.9.51 ff.) treibt GPIO14/15/2
als Ausgange und liest sie zurueck - bleibt der Ruecklesewert bei "high" auf 0,
haelt etwas D0 fest.

> The MTDI strapping pin is incompatible with DAT2 line pull-up by default when
> the code flash is 3.3V.
>
> | Module | Flash voltage | DAT2 connections |
> |---|---|---|
> | Module | Flash voltage | DAT2 connections |
> |---|---|---|
> | Wroom-32 Series | 3.3V | Internal PD, weakly pulled down |
> | Wrover | 1.8V | Internal PU, pullup suggested |
>
> On boards which use the internal regulator and a 3.3V flash chip, **GPIO12
> must be low at reset. This is incompatible with SD card operation.**

### 12p. Hintergrund: DAT2 = GPIO12 = MTDI (nicht die Ursache hier)

Unabhaengig von 12o gilt weiter: **DAT2 der SD-Karte ist beim ESP32 gleichzeitig
MTDI (GPIO12)** und legt beim Reset die Flash-Spannung VDD_SDIO fest. Die
WROVER-Module haben 1,8-V-Flash und einen internen Pull-up auf GPIO12, die
WROOM-32-Module 3,3-V-Flash und einen internen Pulldown - deshalb kann dieselbe
Leitung nicht gleichzeitig SD-DAT2-Pull-up und korrekter Flash-Strapping sein
("incompatible with SD card operation"). Wer DAT2 verdrahtet, muss das
beachten; im 1-Bit-Modus wird DAT2 nicht gebraucht.

Belegstelle mit identischem Symptom ("flashing via serial is not possible until
SD card is removed", Loesung dort: Flash-Spannungs-eFuse brennen - fuer dieses
Board ungeeignet, weil das WROVER-Modul 1,8-V-Flash hat):
https://www.olimex.com/forum/index.php?topic=9267.0
**Konsequenz fuer unseren Fehler:** Die Karte bekam auf DAT2 den falschen Pegel
und antwortete deshalb nie (`response_index = 0`) und ging auf busy
(`data_busy = 1`, Abschnitt 12k/12m). Die Kette aus 12m - Karte antwortet nicht,
Host sendet daher kein Kommando, 0x107 - ist damit erklaert.

### 12q. GEMESSEN: D0 (GPIO2) wird auf LOW gezogen (0.9.53, WROOM, Karte draussen)

`sdpins` liest jede SD-Leitung dreimal: frei, mit internem Pull-up, mit internem
Pull-down. Ergebnis auf dem WROOM (Karte herausgezogen, also ohne Karte):

```
GPIO2  D0 : frei=0, mit Pull-up=0, mit Pull-down=0  -> extern auf LOW gezogen (staerker als 45k)
GPIO14 CLK: frei=1, mit Pull-up=1, mit Pull-down=1  -> extern auf HIGH gezogen (stark)
GPIO15 CMD: frei=1, mit Pull-up=1, mit Pull-down=1  -> extern auf HIGH gezogen (stark)
GPIO4  DAT1: folgt den internen Pulls (extern nichts)
GPIO12 DAT2: folgt den internen Pulls (extern nichts)
GPIO13 DAT3: frei=0  (nicht verdrahtet)
```

Auswertung:

- **CLK und CMD** liegen auch gegen den internen Pull-DOWN auf HIGH -> die
  externen Pull-ups (10k nach 3,3 V) sind vorhanden und kraeftig, wie erwartet.
- **D0 liegt auch gegen den internen Pull-UP auf LOW** -> dort zieht etwas
  **nach GND**, und zwar staerker als die internen ca. 45k. Und das **ohne
  Karte**: es ist also NICHT die Karte.
- DAT1/DAT2 floaten (nichts angeschlossen), DAT3/GPIO13 ist laut Betreiber nicht
  verdrahtet (misst frei=0).

**Damit ist der SD-Ausfall vollstaendig erklaert:** Der Host liest DAT0
dauerhaft als busy (`data_busy = 1`), und die CIU sendet deshalb kein Kommando -
`start_command` bleibt 1, `response_index = 0`, Ergebnis `0x107` (12k/12m). Der
Fehler liegt in der **D0-Leitung**, nicht in Karte, Takt, Gate, PLL, PSRAM oder
Firmware.

**Noch offen - und in einem Zug entscheidbar:** zieht die **Platine** oder das
**WROOM-Modul**? Derselbe `sdpins`-Test auf dem WROVER (gleiches Board, gleiche
Karte, nur Modul getauscht) trennt das:

- D0 auch dort LOW  -> Platinenfehler auf der D0-Leitung
- D0 dort HIGH      -> das WROOM-Modul zieht D0 herunter (Modulfehler)

### 12r. Alle RS232-Leitungen durchgemessen - keine geht auf IO2 (0.9.55)

Verdacht des Betreibers (und berechtigt, weil er alle bisherigen Messungen
entwertet haette): zieht vielleicht DTR oder eine andere Leitung des
USB-Seriell-Wandlers den Pin IO2 herunter?

Dafuer gibt es jetzt ein systematisches Werkzeug:

- **Firmware `scanpins`** (main/cmd_reg.c): liest die Pegel ALLER GPIOs direkt
  aus `GPIO_IN_REG`/`GPIO_IN1_REG` - ohne `gpio_config()`, weil das Pins
  verweigert, die ein Peripherietreiber beansprucht hat. Genau das war im ersten
  Versuch der Fehler: GPIO0, GPIO2, GPIO4 und GPIO5 fehlten im Ergebnis.
- **Host `D:\Coding\ESP-IDF\.tmp\rs232_scan.ps1`**: variiert DTR, RTS und TXD
  (ueber gesendete 0x00/0xFF-Bytes), ruft jeweils `scanpins` auf und vergleicht.

Ergebnis (WROOM, Karte gesteckt, zwei Boards):

```
DTR=0 RTS=0:  GPIO0=1  GPIO2=0  GPIO4=1  GPIO5=1  GPIO12=1  GPIO13=0  GPIO14=1  GPIO15=1
DTR=1 RTS=0:  GPIO0=0  GPIO2=0  GPIO4=1  GPIO5=1  GPIO12=1  GPIO13=0  GPIO14=1  GPIO15=1
TXD=0:        GPIO0=1  GPIO2=0  ...
RTS=1:        keine Antwort (Reset)
```

| RS232-Leitung | ESP32-Pin | Nachweis |
|---|---|---|
| DTR | **GPIO0** | GPIO0 folgt DTR (1 -> 0) |
| RTS | **EN** (Reset) | bei RTS=1 antwortet die Anwendung nicht mehr |
| TXD/RXD | GPIO3/GPIO1 (UART0) | die Konsole laeuft darueber |
| DTR/RTS/TXD auf IO2 | **nein** | GPIO2 bleibt in allen Zustaenden 0 |

**Damit ist ausgeschlossen, dass der Messaufbau den LOW-Pegel auf D0 macht.**
Der Pegel kommt vom Board oder vom Modul; er ist unabhaengig von Karte,
seriellem Port, DTR und RTS.

**Verbleibende Klaerung "Board oder Modul":**
1. Multimeter, Board ohne Modul: Widerstand vom D0-Pad (bzw. DAT0-Kontakt des
   Slots) nach GND. ~10k -> Platine zieht herunter; hochohmig -> Modul.
2. WROVER in dasselbe Board stecken und `sdpins` aufrufen: D0 dort HIGH -> das
   WROOM-Modul zieht D0 herunter.

### 12s. GELOEST: D0-Pull-up hergestellt -> SD laeuft auch auf dem WROOM (0.9.55)

Der Betreiber hat die Platine umgebaut. Danach, mit demselben WROOM-Modul und
demselben Build (0.9.55, NoPsram, Flash 40 MHz, SD-Takt 4000 kHz):

```
I (1010) SD_CARD: Card detect (GPIO34): card inserted
I (1011) SD_CARD: SDMMC-Versuch 1/3 (max 4000 kHz)
I (1114) SD_CARD: mounted at /sdcard (SDMMC 1 Bit)      <- 103 ms, wie auf dem WROVER
Name: USDU1   Type: SDHC
```

Und `sdpins` zeigt die Ursache im Vorher/Nachher-Vergleich eindeutig:

| Messung | vorher (Original) | nachher (umgebaut) |
|---|---|---|
| GPIO2 D0 als Eingang | 0 | **1** |
| D0 frei / Pull-up / Pull-down | 0 / 0 / 0 | **1 / 1 / 1** |
| Bewertung durch `sdpins` | "extern auf LOW gezogen (staerker als 45k)" | **"extern auf HIGH gezogen (stark)"** |
| SDMMC-Versuch 1 | 0x107, Timeout 1 s | **gemountet in 103 ms** |

CLK (GPIO14) und CMD (GPIO15) waren die ganze Zeit unauffaellig (1/1/1); nur D0
war falsch. **Damit ist die Ursache bestaetigt und behoben:**

> Auf der D0-Leitung (GPIO2) fehlte der Pull-up nach 3,3 V. Damit las der
> SDMMC-Host DAT0 dauerhaft als "busy", die CIU sendete kein Kommando
> (`start_command` blieb 1, `response_index = 0`) und der Mount lief in den
> 1-s-Timeout `0x107`. Weil GPIO2 gleichzeitig der Boot-Strapping-Pin ist,
> erklärte derselbe Fehler auch das unterschiedliche Flaschverhalten von WROOM
> (GPIO2 LOW -> Download-Modus geht) und WROVER (GPIO2 HIGH -> Download-Modus
> blockiert, Modul musste heraus).

**Lehre fuer die naechste Platine:** Der Pull-up auf D0 ist nicht optional
(Doku: "CMD and DATA lines D0-D3 should be pulled up by 50KOhm even in 1-bit
mode"). Und weil GPIO2 ein Strapping-Pin ist, muss beim Bestuecken geprueft
werden, dass dieser Pull-up nur nach dem Reset wirkt bzw. dass der
Download-Modus ueber den GPIO0-GPIO2-Jumper oder eine eigene Schaltung
sichergestellt ist (wie beim WROVER-KIT v3).

### 12t. Warnung: `sdpins` NICHT bei gemounteter Karte ausfuehren

`sd_ls` meldete in der 0.9.55-Sitzung "0 entries in /sdcard", obwohl die Karte
vollstaendig ist. Ursache war der Messaufbau: `sdpins` treibt GPIO14/15/2 als
Ausgaenge low/high - auf denselben Leitungen, auf denen die gemountete Karte
gerade arbeitet. Der Verzeichniszustand des FATFS ist danach hin.

Mit 0.9.41 (ohne diesen Eingriff) listet dieselbe Karte korrekt:

```
  UNICORN_11704_x14_b17.jic
  test.mp3
  test2.mp3
73 entries in /sdcard
```

**Regel:** `sdpins`, `scanpins` und `sd_mount_spi` nur benutzen, solange keine
Karte gemountet ist (`sd_unmount` vorher), sonst verfaelscht man sich das
Dateisystem.

### 12u. Mischbetrieb auf dem umgebauten WROOM verifiziert (0.9.41)

Nach dem Hardware-Umbau (D0-Pull-up, Abschnitt 12s) und mit dem verifizierten
Audio-Stand 0.9.41 laeuft die Kernanforderung - "der I2S-Sound von der Vampire
muss immer zu hoeren sein, beim Abspielen von Dateien wird gemischt" - auf dem
WROOM:

**Verbindung und Aufbau**

```
I (1254) STREAM_PROC: I2S-Zweig: 60000 Hz/32 Bit -> 48000 Hz/16 Bit
I (1293) STREAM_PROC: Mischer: 48000 Hz, 16 Bit, 2 ch, 2 Quellen (I2S 1.0, Datei 0.7)
I (213620) BT_AUD_A2D_SRC: A2DP connection state: Connected, addr[66:fe:5a:e3:41:df]
I (213632) BT_AUD_A2D_SRC: A2DP connection handle saved: 65, audio_mtu: 703
I (267511) BT_AUD_A2D_SRC: A2DP audio state: Started (source)
I (267537) BT_AUD_A2D_SRC:   sample_rate: 44100, ch_mode: 2, bitpool: 53
```

**WAV (test_tone_48k.wav, 288 044 Byte)**

```
Datei -> Mischer: test_tone_48k.wav
  (Vampire-Ton laeuft weiter und wird dazugemischt)
I LIN_RESAMPLE: 48000 Hz, 16 Bit, 1 ch -> Ausgang 44100 Hz, 16 Bit, 2 ch
I STREAM_PROC: [a2dp source pipeline] state => RUNNING(3)
CPU-Last: Kern0 46% / Kern1 68%  ->  Kern0 35% / Kern1 64%
```

**MP3 (test2.mp3, 75 531 Byte)**

```
I STREAM_PROC: Decoder auf Dateityp 0x2033504d eingestellt (file://sdcard/test2.mp3)
I LIN_RESAMPLE: 44100 Hz, 16 Bit, 2 ch -> Ausgang 44100 Hz, 16 Bit, 2 ch
I STREAM_PROC: [a2dp source pipeline] state => RUNNING(3)
CPU-Last: Kern0 74% / Kern1 74% (waehrend des Decodierens), danach 35% / 64%
keine Fehlerzeile im Log
```

**Ergebnis (gehoert, nicht nur gemessen):** Der 440-Hz-Ton kommt aus der
Soundbar, die V4 bleibt dabei **lueckenlos** zu hoeren; dasselbe fuer die MP3.
Beim *ersten* Start des Streams (`start_media`) wird der SBC-Encoder
konfiguriert (44100 Hz) - in diesem Moment setzt der BT-Ausgang einmalig kurz
aus. Das ist der Stream-Start, nicht das Mischen; im laufenden Betrieb
(Titelwechsel per `playfile`) bleibt der V4-Sound durchgehend.

Damit ist die Kette **SD -> Decoder -> Mischer -> SBC -> A2DP** auf dem WROOM
vollstaendig belegt, der Datei-Zweig laeuft parallel zum I2S-Zweig der Vampire.

## 13. Equalizer: Einbau und gemessene Kosten (0.9.56)

### 13a. Was eingebaut wurde

- **Element:** `aud_eq` aus `esp_gmf_audio` (baut auf `esp_ae_eq` aus
  `esp_audio_effects` auf). Filtertypen: High-Pass, Low-Pass, Peak, High-Shelf,
  Low-Shelf; 16/24/32 Bit; Parameter zur Laufzeit aenderbar.
- **Ort:** in der Mischer-Pipeline **zwischen `aud_mixer` und `aud_enc_mix`**
  (`stream_proc.c`: `{"aud_mixer", "aud_eq", "aud_enc_mix"}`). Eine Instanz
  formt beide Quellen (Vampire und Datei) - das ist die guenstigste Variante,
  denn der EQ laeuft auf **Kern 1**, wo schon Mischer, Resampler, SBC-Encoder
  und der BT-Sende-Task liegen.
- **Bandzahl:** fest **10** (`MIXER_EQ_BANDS`), alle mit 0 dB = flach.
  Bass-Shelf 100 Hz, acht Peak-Baender (200 Hz ... 10 kHz), Hoehen-Shelf 12 kHz.
- **CLI:** `eq` (auflisten), `eq bands <0..10>` (erste N Baender aktiv),
  `eq set <idx> <typ> <fc> <q> <gain>`.
- **Abtastrate:** das Element passt sich selbst an - `eq_received_event_handler`
  in `esp_gmf_eq.c` setzt bei geaenderter Rate `need_reopen` und oeffnet den EQ
  mit der neuen Rate neu (unsere Kette laeuft je nach Aushandlung mit 44100 oder
  48000 Hz).

### 13b. Gemessene Kosten

Vorher (0.9.41, gleicher Aufbau): Heap 78 284 Byte frei, groesster Block
32 768 Byte. Nach dem Einbau (0.9.56): **75 508 Byte frei, groesster Block
34 816 Byte** -> der EQ kostet rund **2,8 KB**.

CPU-Messreihe (0.9.56, WROOM, `test2.mp3` wiederholt abgespielt, I2S-Eingang der
Vampire aktiv, A2DP-Stream gestartet, 48 kHz Stereo s16). Je Stufe die
Spitzenwerte der 5-s-Mittelwerte:

| Baender | Kern0 (Dekoder) | Kern1 (Mixer+EQ+SBC+BT) |
|---|---|---|
| 0 | 70 % | 60 % |
| 1 | 71 % | 61 % |
| 2 | 71 % | 63 % |
| 5 | 70 % | 65 % |
| 10 | 70 % | **70 %** |

**Ergebnis:** rund **1 % CPU je Band** auf Kern 1; 10 Baender kosten also etwa
**+10 %**. Zwei Dinge sind dabei wichtig:

1. Die Doku-Formel `(rate/8000) * kanaele * baender * base_load` mit
   `base_load = 0,09 %` (s16) ergibt fuer 48 kHz Stereo 1,08 % je Band - **auf
   einem ESP32-S3**. Der ESP32 (LX6, ohne Vektor-SIMD) liegt mit ~1 % je Band
   praktisch gleichauf; meine Vorabschaetzung von 2-3 % je Band war zu
   pessimistisch.
2. **Dekoder und EQ liegen auf verschiedenen Kernen** (Kern 0 bzw. Kern 1) und
   addieren sich daher nicht. Im haertesten gemessenen Fall (MP3-Dekoder +
   I2S + EQ mit 10 Baendern + SBC + A2DP) bleiben auf **beiden** Kernen rund
   30 % Reserve.

Damit sind 10 Baender auf diesem Chip gut vertretbar.

### 13c. Offener Punkt: Start-Reihenfolge des Datei-Zweigs

Beim **ersten** `playfile` nach `start_media` ist der Datei-Zweig einmal
abgestuerzt:

```
W ESP_GMF_ASMP_DEC: Not enough memory for out, need:4608, old: 1024, new: 4608
E ESP_GMF_PORT: esp_gmf_port.c:284 (esp_gmf_port_acquire_out): Got NULL Pointer
E LIN_RESAMPLE: lin_resample_process(386): Failed to acquire out, ret: -1
E ESP_GMF_TASK: Job failed[...aud_lin_resample_file_proc], ret:-1
I STREAM_PROC: [a2dp source pipeline] state => ERROR(7)
```

Der Resampler bekam Daten, bevor sein Ausgangsport am Ringpuffer hing - ein
Start-Reihenfolge-Problem, das mit dem zusaetzlichen Element in der
Mischer-Pipeline auftritt (in 0.9.55 lief dieselbe Folge fehlerfrei). Im
zweiten Durchlauf (15 `playfile`-Aufrufe) trat es **nicht** wieder auf, ist also
selten und haengt am ersten Aufbau.

**Zu beheben:** den Datei-Zweig erst starten, wenn die Mischer-Pipeline
`RUNNING` meldet (bzw. bei `ERROR` einmal neu anstoßen).

### 13d. Hoerprobe: der EQ wirkt deutlich (0.9.56)

Jeweils eine Wiedergabe von `test2.mp3` pro Einstellung, derselbe Aufbau wie in
13b (V4 laeuft ueber I2S mit, Soundbar als A2DP-Empfaenger):

| Einstellung | Kommando | Ergebnis |
|---|---|---|
| flach (Referenz) | `eq bands 0` | normal |
| **nur Bass** | `eq set 0 5 250 0.7 15` + `eq bands 1` | **deutlich** |
| **nur Hoehen** | `eq set 9 4 4000 0.7 15` + `eq bands 10` | **deutlich** |
| beides | beide Baender +15 dB | deutlich |

**Wichtig fuer die Praxis:** Mit den urspruenglich voreingestellten Baendern
(Bass-Shelf **100 Hz**, Hoehen-Shelf **8 kHz**) war der Unterschied nur schwach
zu hoeren - 100 Hz liegt unter dem, was eine Soundbar wiedergibt, und 8 kHz am
oberen Hoerrand. Erst **250 Hz** bzw. **4 kHz** mit **+15 dB** waren klar
hoerbar. Die Standardbaender aus 13a sind deshalb als *Ausgangspunkt* gedacht;
wirksame Hoerproben brauchen tiefere Hoehen- und hoehere Bassfrequenzen.

Damit ist der EQ vollstaendig belegt: eingebaut, in der Kette, messbar
(~1 % CPU je Band) und **hoerbar**.
