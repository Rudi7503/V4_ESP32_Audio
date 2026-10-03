# Arbeitsweise in diesem Projekt

## Regel 1: Erst die offiziellen Code-Beispiele lesen, dann Code schreiben

**Immer**, bevor eine Funktion implementiert oder ein Fehler "behoben" wird:
in den offiziellen, geprüften Beispielen nachsehen, wie es dort gelöst ist.

Die Beispiele liegen lokal im Rechner und sind die Referenz:

| Thema | Ort |
|---|---|
| GMF-Grundbeispiele (Pipeline, Datei, Effekte, Loop) | `D:\Coding\ESP-IDF\.espressif\esp-gmf-v1.0\gmf_examples\basic_examples\` |
| Bluetooth-Audio (unser Ausgangsbeispiel) | `D:\Coding\ESP-IDF\.espressif\esp-gmf-v1.0\packages\esp_bt_audio\examples\bt_audio\` |
| Player-Dienst | `D:\Coding\ESP-IDF\.espressif\esp-gmf-v1.0\packages\esp_player\examples\` |
| ADF-Beispiele (altes Projekt, IDF 5.5) | `D:\Coding\ESP-IDF\.espressif\esp-adf_master\adf_examples\` |
| Komponenten-Quelltext (Wahrheit über APIs) | `D:\Coding\ESP-IDF\V4_ESP32\managed_components\` |

Besonders relevante Beispiele für unseren Weg:

- `pipeline_play_sdcard_music` - Datei von SD abspielen, Pipeline am Ende stoppen
- `pipeline_loop_play_no_gap` - Dateiende, naechste Datei, Task-Strategie
- `pipeline_play_embed_music`, `pipeline_play_http_music` - weitere Quellen
- `pipeline_play_multi_source_music` - mehrere Quellen, nahe an unserem Misch-Vorhaben

### Warum diese Regel

Zwei Beispiele aus der Praxis in diesem Projekt:

1. **Brummen am Dateiende.** Ich habe eine eigene Task-Strategie gebaut
   (`esp_gmf_task_set_strategy_func` mit `ACTION_STOP`). Sie griff nicht, weil
   `esp_gmf_task.c:347` die Strategie erst fragt, wenn ALLE Jobs fertig sind.
   Die offiziellen Beispiele loesen es anders und einfacher: Event im Callback
   **merken**, die Pipeline **ausserhalb** des GMF-Tasks stoppen.

2. **Falsche Diagnose.** Ich hatte behauptet, der Encoder reiche das
   `is_done`-Flag nicht durch. Im Quelltext steht das Gegenteil
   (`esp_gmf_audio_enc.c:572` und `:598` liefern bei `in_load->is_done`
   `ESP_GMF_JOB_ERR_DONE`). Ein Blick in den Code haette den Irrweg vermieden.

## Regel 2: Eine Unbekannte pro Aenderung

Nie zwei ungepruefte Aenderungen zusammen einbauen. Erst eine Sache aendern,
auf Hardware pruefen, dann die naechste. (Diese Regel gibt es schon laenger -
sie hat sich beim Rate-Konverter bewaehrt.)

## Regel 3: Die Ausgabe des Werkzeugs lesen, nicht die eigene Zusammenfassung

Exit-Codes von umgeleiteten Kindprozessen sind unzuverlaessig - das Log lesen.
Unterdrueckte Ausgabe hat in diesem Projekt schon mehrfach echte Fehler
versteckt.

## Regel 4: Immer eine Anzeige fuer den Benutzer

Der Benutzer will die serielle Ausgabe **live** sehen. Dafuer gibt es:

- `.tmp\monitor_starten.bat` - eigenes Fenster, laeuft bis Strg+C
- `.tmp\monitor_starten.bat "cmd1|cmd2"` - mit Kommandos
- Der Monitor liest `.tmp\logs\monitor_cmd.txt` ab: eine Zeile hineinschreiben
  schickt sie an den ESP32. So bleiben Anzeige und Steuerung im selben Prozess
  (der serielle Port kann nur von einem Prozess geoeffnet werden).

**Wichtig:** Es darf nur EIN Monitor laufen. Ein zweites Fenster bekommt den
Port nicht und zeigt nur "Zugriff verweigert" - das ist bereits passiert und
hat Zeit gekostet. Der Monitor hat dafuer eine Sperre (benannter Mutex).
Vor einem Neustart also erst das alte Fenster schliessen.
