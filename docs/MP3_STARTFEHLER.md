# MP3-Startfehler im Datei-Zweig (Stand 08.10.2026)

**Kurz:** Der erste `playfile` einer MP3 nach dem Start scheitert — der Datei-Zweig
kann seinen Ausgangspuffer nicht vergroessern, der Job bricht ab, die
A2DP-Uebertragung stoppt. WAV-Dateien sind nicht betroffen. **Der Fehler ist
nicht durch die I2C-Bruecke entstanden** (siehe A/B-Test unten).

## Was in 0.9.61 eingebaut ist

1. **Der Datei-Zweig reisst die Uebertragung nicht mehr mit** (`stream_proc.c`).
   In der Fehlerbehandlung des Datei-Pipelines-Events wurde bisher zusaetzlich
   `local2bt_request_media_stop()` gerufen - das stoppte die **ganze**
   A2DP-Uebertragung, obwohl nur der Datei-Zubringer gescheitert war. Der
   Mischer traegt aber weiter den I2S-Ton der Vampire. Der Aufruf, der Merker
   `local2bt_media_stop_requested` und der zugehoerige Block in
   `local2bt_process_stop_request()` sind entfernt; uebrig bleibt
   `local2bt_request_stop()`, das nur den Datei-Zweig stoppt und zuruecksetzt
   (raeumt auch den ERROR-Zustand auf). Ergebnis: ein fehlgeschlagenes
   `playfile` kostet die Datei, nicht den Vampire-Ton - und es gibt keinen
   Watchdog-Neustart mehr aus diesem Pfad.

2. **Der Wandler fordert eine feste Puffergroesse an** (`linear_resample.c`).
   Bisher wurde je Block eine andere Groesse verlangt (auf 1024 aufgerundet,
   beim ersten kleinen Block also 1024 Byte) - genau dabei kam der Port in die
   Reallokation, die fehlschlaegt. Jetzt wird immer
   `LIN_RESAMPLE_OUT_PAYLOAD_MAX` (6144 Byte) angefordert, der berechnete Wert
   bleibt als Sicherung nach oben. Damit gibt es hoechstens einmal eine
   Vergroesserung und danach nie wieder eine Anforderung.

3. **Temporaere Diagnose** im `open` des Wandlers: sie schreibt die
   Ausrichtungsanforderungen beider Ports, den Zustand des Ausgangspuffers, den
   Wert von `esp_gmf_oal_get_spiram_cache_align()` und die freien Bytes je
   Heap-Bereich (`INTERNAL`, `DMA`, `DEFAULT`) ins Log. Sie beantwortet die noch
   offene Frage aus dem Abschnitt "Was noch offen ist" und **wird nach der
   Klaerung wieder entfernt**.

## Speicher nachgerechnet und freigemacht (0.9.62, 08.10.2026)

Mitschnitt `/tmp/heap_mp3.log` (0.9.61, Stream lief, Senke verbunden):

```
>>> free                    (vor dem playfile)
Free heap size: internal 68848      groesster Block: 34816
>>> playfile test2.mp3
E ESP_GMF_PAYLOAD: esp_gmf_payload.c:92 ...: Got NULL Pointer
E BT_OSI: heap info: free=340, largest_block=120        <- Heap wirklich leer
E STREAM_PROC: Fehler im Datei-Zweig - Wiedergabe wird beendet, Stream laeuft weiter
E BT_OSI: heap info: free=38756, largest_block=27648    <- danach wieder frei
>>> free                    (nach dem Fehler)
Free heap size: internal 74580      groesster Block: 28672
```

Rechnung: der Datei-Zweig belegt beim Oeffnen 68 508 Byte (68 848 - 340) und
braucht dann noch die 4608 Byte fuer den Ausgangspuffer des Dekoders
(`Not enough memory for out, need:4608`). Bedarf also ~73 116 Byte, verfuegbar
waren 68 848 - **es fehlten rund 4,3 KB**. Der groesste freie Block (34 816)
war gross genug; es war reine Menge, keine Zersplitterung.

Diese 4,3 KB sind in 0.9.62 freigemacht worden, ohne den Vertrag, die
Aufgaben-Stacks oder die (in docs/MESSREIHE.md begruendeten) Audiopuffer
anzutasten:

| Massnahme | Datei | frei |
|---|---|---|
| FatFs: **ein** gemeinsamer Sektor-Cache statt 512 Byte je offener Datei (`CONFIG_FATFS_PER_FILE_CACHE` aus -> `_FS_TINY=1`) | `sdkconfig`, `sdkconfig.defaults.esp32.nopsram` | ~4,4 KB |
| Geraetetabelle 16 -> 8 Eintraege (`BT_MGR_MAX_DEVICES`; der Vertrag bleibt bei 16, GET_INFO meldet die unterstuetzte Zahl) | `bt_manager.h/.c`, `v4_link.c` | 2,0 KB |
| Sendering der I2C-Bruecke 2048 -> 1152 Byte (Rahmen ist 1036) | `v4_link.h` | 0,9 KB |

Der FatFs-Posten ist der groesste: `vfs_fat.c:203` legt
`sizeof(vfs_fat_ctx_t) + max_files * sizeof(FIL)` an, und `FIL` traegt ohne
`_FS_TINY` einen eigenen 512-Byte-Puffer. `max_files = 8` bleibt trotzdem
richtig (4 Datei-Handles + 2 Verzeichnis-Handles + 1 GMF-Spieler + Luft) - mit
gemeinsamem Cache kostet es fast nichts.

Dazu neu in `v4_bus`: der kleinste Stack-Rest beider Bruecken-Aufgaben
(`uxTaskGetStackHighWaterMark`). Damit wird die Frage "reichen 6144 Byte?"
gemessen statt geschaetzt; gesenkt wurden die Stacks bewusst **nicht**.

## Geloest - auf Hardware bestaetigt (0.9.63, 08.10.2026)

Mitschnitt `/tmp/test0963b.log` (Stream lief, Senke verbunden, `i2s_media` +
`start_media`):

```
>>> free                                   (mit laufendem Stream, vor dem playfile)
Free heap size: internal 87232, min 85724, groesster Block 53248

>>> playfile test2.mp3
W ESP_GMF_ASMP_DEC: Not enough memory for out, need:4608, old: 1024, new: 4608
E LIN_RESAMPLE: DIAG heap: internal frei=48740 | dma 15968 | default frei=15968
I STREAM_PROC: [a2dp source pipeline] state => RUNNING(3)
I (56147) ESP_GMF_FILE: No more data, ret: 0
I (56263) STREAM_PROC: Wiedergabe beendet - stoppe den Datei-Zweig (FINISHED)

>>> playfile test.mp3                       (4,6 MB) lief danach ebenfalls an
E LIN_RESAMPLE: DIAG heap: internal frei=41396 | dma 8624 groesster 5632
I LIN_RESAMPLE: Open: 44100 Hz, 2 ch -> 44100 Hz, 2 ch
I STREAM_PROC: [a2dp source pipeline] state => RUNNING(3)
```

Kein `Got NULL Pointer`, kein `A2DP Source error`, kein Watchdog-Neustart; der
Datei-Zweig endet mit **FINISHED** statt ERROR. Der I2S-Zweig der Vampire lief
durchgehend mit (`I2S-Zweig ausgewaehlt`, Mischer mit zwei Quellen). Der
Grundfuer den Unterschied ist die Spalte "default frei": vorher 340 Byte (bzw.
~3,1 KB), jetzt **15 968 Byte** beim ersten und 8 624 Byte beim zweiten Titel -
die 4608-Byte-Anforderung des Dekoders geht damit durch.

Frei geworden sind mit 0.9.62/0.9.63 zusammen rund 19 KB (87232 statt 68848
Byte freier Heap bei laufendem Stream).

Der Selbsttest der Bruecke lief nach allen Aenderungen erneut durch
(`/tmp/selftest963.log`, FILE_OPEN 772778 Byte, FILE_READ 140/268-Byte-Rahmen,
CRC ok, STOP_PLAY); die Stack-Reserven lagen danach bei 2544 (v4_link) und
5364 Byte (v4_work).

**0.9.64** entfernt die temporaere Diagnosezeile wieder (sonst unveraendert).

## Symptom (Mitschnitt /tmp/monitor_mp3c.log, 0.9.60)

```
I (39755) STREAM_PROC: Datei -> Mischer: file://sdcard/test.mp3 (Eintrag 0)
I (39930) STREAM_PROC: [a2dp source pipeline] state => OPENING(2)
W (39974) ESP_GMF_ASMP_DEC: Not enough memory for out, need:4608, old: 1024, new: 4608
I (40004) LIN_RESAMPLE: Open: 44100 Hz, 2 ch -> 48000 Hz, 2 ch
I (40008) STREAM_PROC: [a2dp source pipeline] state => RUNNING(3)
E (40025) ESP_GMF_PAYLOAD: esp_gmf_payload.c:92 (..._separate_alignment): Got NULL Pointer
E (40038) ESP_GMF_PORT: esp_gmf_port_acquire_out(357): ACQ OUT, reallocate payload
          buffer failed, el:aud_lin_resample_file, sz:4096, new_sz:1024
E (40057) LIN_RESAMPLE: lin_resample_process(386): Failed to acquire out, ret: -1
E (40063) ESP_GMF_TASK: Job failed [... aud_lin_resample_file_proc]
E STREAM_PROC: A2DP Source error        -> Datei-Zweig wird gestoppt
...                                     -> in 0.9.60 zusaetzlich: rst:0x8 (TG1WDT_SYS_RESET)
```

Beim Dekoder (`el:aud_dec`) tritt dieselbe Fehlerzeile mit `sz:512, new_sz:4608`
auf, wenn die MP3 geoeffnet wird; er will seinen Puffer von 512/1024 auf 4608 Byte
(ein MP3-Frame: 1152 Samples x 2 ch x 2 B) vergroessern.

## A/B-Test: 0.9.56 verhaelt sich identisch

Am 08.10. wurde deshalb das **alte Image von `D:\Coding\ESP-IDF\V4_ESP32`**
(sdkconfig ohne PSRAM, 40 MHz - dieselbe Konfiguration) geflasht und dieselbe
Befehlsfolge geschickt (`i2s_media`, `start_media`, zweimal `playfile test2.mp3`):

```
I (990) BT_AUD_EXAMPLE:  V4_ESP32  Version 0.9.56
W (30053) ESP_GMF_ASMP_DEC: Not enough memory for out, need:4608, old: 1024, new: 4608
E (30097) ESP_GMF_PORT: esp_gmf_port.c:284 (esp_gmf_port_acquire_out): Got NULL Pointer
E (30162) STREAM_PROC: A2DP Source error
(zweiter Versuch) -> derselbe Fehler
```

Damit ist ausgeschlossen, dass die I2C-Bruecke (oder das Aufraeumen in
0.9.58/0.9.59) den Fehler verursacht. Im Mitschnitt vom **05.10.**
(`D:\Coding\ESP-IDF\.tmp\logs\eq_01.log`, 0.9.56) steht derselbe Fehler bei
`playfile test.mp3` — dort spielte aber der **zweite** Versuch mit `test2.mp3`
danach. Der Fehler ist also **speicher-/zeitabhaengig**, nicht deterministisch.

## Was ausgeschlossen ist

* **Kein Speichermangel im Groben.** `free` mit laufendem Stream (0.9.60):
  ```
  Free heap size: internal 73692, min 72968
  groesster Block: internal 38912
  DMA-Bereich 0x3ffe4350: 41220 frei, groesster Block 38912
  ```
  Eine 1024-Byte-Anforderung (bzw. 4608 Byte) sollte darin Platz haben.
* **Kein PSRAM-Problem.** `esp_gmf_oal_malloc_align()` nimmt ohne
  `CONFIG_SPIRAM_BOOT_INIT` `MALLOC_CAP_DEFAULT` (esp_gmf_oal_mem.c:47).
* **Kein Ueberlauf der Groessenrechnung.** `realloc_size` ist 1024 bzw. 4608.
  `ESP_GMF_OAL_ALIGN_UP`/`_BYTES_VALID` sind unauffaellig (Makros in
  `oal/include/esp_gmf_oal_mem.h:16`).
* **Kein Verkleinern.** `esp_gmf_port_payload_get_realloc_size()` behaelt immer
  die groessere Laenge (`buf_length > wanted_size -> buf_length`). Die
  Fehlerzeile druckt `sz` = `port->data_length`, nicht die Pufferlaenge - die
  Verwechslung hatte zuerst nach einem Schrumpfen ausgesehen.

## Was noch offen ist

`esp_gmf_payload_realloc_buffer_with_separate_alignment()` ruft
`esp_gmf_oal_malloc_align(resolved_addr, alloc_len)` auf und bekommt NULL.
Weder Groesse noch freier Speicher erklaeren das; es bleibt die
**Ausrichtung** (`resolved_addr`) oder ein Heap-Zustand genau in diesem Moment.
Klaeren laesst sich das nur mit einer Diagnosezeile an dieser Stelle im
(GMF-)Quelltext plus Mitschnitt.

## Fix-Richtung

1. **Puffer vorab anlegen (bevorzugt):** Der Wandler kennt seine maximale
   Ausgangsgroesse (1152 Frames x 48000/44100 x 2 ch x 2 B = 5016 -> aufrunden).
   `esp_gmf_payload_new_with_len()` + `esp_gmf_port_set_payload()` gibt es beide
   (in den Komponenten sonst unbenutzt) - damit braucht der Port NIE
   umzuspeichern. Achtung: die Ausrichtung des Ports (`buf_addr_aligned` des
   Mischer-Eingangs) muss dabei erfuellt sein, sonst fordert der Port weiter um.
2. **Feste Groesse anfordern:** In `linear_resample.c` wird auf 1024 Vielfache
   aufgerundet, in der Annahme "der Port verkleinert nie" (Kommentar bei
   `max_out_bytes`). Das stimmt zwar, aber die Anforderung ist beim ersten,
   kleinen Eingangsblock nur 1024 - also die Groesse, bei der es knallt. Eine
   konstante Maximalgroesse macht das Verhalten einheitlich.
3. **Robuster abstuerzen:** Der Fehler im Datei-Zweig darf die A2DP-Uebertragung
   nicht mitreissen (und in 0.9.60 auch keinen Watchdog-Neustart ausloesen).
   Der Datei-Zweig sollte den Fehler melden, den Stream aber stehen lassen -
   die Vampire laeuft ja.

Punkt 3 ist unabhaengig von 1/2 sinnvoll und der billigste Nutzen: der Ton der
Vampire bleibt hoerbar, auch wenn das Abspielen einer Datei scheitert.
