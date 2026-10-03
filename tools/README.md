# tools/ - Arbeitsmittel

Diese Skripte sind die im Projekt benutzten Helfer. Sie erwarten die
Verzeichnisstruktur, in der sie entstanden sind:

```
D:\Coding\ESP-IDF\V4_ESP32        Projekt (dieses Repository)
D:\Coding\ESP-IDF\.tmp\logs       Logdateien, Befehlskanal des Monitors
D:\Coding\ESP-IDF\.espressif      ESP-IDF v6.1 und Toolchain
```

| Skript | Zweck |
|---|---|
| `mess_bauen.ps1` | `-Mode 0..4` baut einen Messfall, `-NoPsram` baut ohne PSRAM (Modul ohne PSRAM), `-Small` laesst `aud_aec`/`aud_asrc` aus dem GMF-Pool weg. Loescht den CMake-Cache (`sdkconfig` wird als `sdkconfig.vorheriger_lauf` gesichert), damit der Modus sicher greift. |
| `flash_only.ps1` | `-Port COMx [-MaxAttempts n] [-ReadSeconds n]`: flasht und liest danach. Der Chip bleibt im Bootloader - **Stromzyklus von Hand noetig**. |
| `monitor_fenster.ps1` | `-Port COMx [-Log datei]`: Monitor mit Auto-Reconnect. Befehle werden ueber `.tmp\logs\monitor_cmd.txt` eingeschoben (eine Zeile je Befehl, der Monitor sendet hoechstens alle 2 s eine). |
| `send_cmd.ps1` | schickt einen einzelnen Befehl an den laufenden Monitor |
| `monitor.ps1` | einfacher Monitor ohne Befehlskanal |
| `idf61_env.ps1` | setzt `IDF_PATH` und die Umgebung fuer ESP-IDF v6.1 |
| `make_tone48k.py` | erzeugt die Testdateien `test_tone_48k.wav` (48 kHz) und `test_tone_440.wav` (44,1 kHz), je 3 s, mono, 440 Hz, halber Pegel |
| `rate_conv_model.py` | Modellrechnung zum Vergleich von GMF-Ratenwandlung und linearer Interpolation |
