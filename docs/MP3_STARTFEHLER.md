# MP3-Startfehler im Datei-Zweig (Stand 08.10.2026)

**Kurz:** Der erste `playfile` einer MP3 nach dem Start scheitert — der Datei-Zweig
kann seinen Ausgangspuffer nicht vergroessern, der Job bricht ab, die
A2DP-Uebertragung stoppt. WAV-Dateien sind nicht betroffen. **Der Fehler ist
nicht durch die I2C-Bruecke entstanden** (siehe A/B-Test unten).

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
