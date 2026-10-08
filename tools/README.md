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
| `attach_board.sh` | **Linux-Seite** (WSL): haengt das Board nach einem Flashen/USB-Neustart per `usbipd` wieder an WSL (Vorgabe Bus-ID 1-5) und wartet auf `/dev/ttyUSB0`. Nach jedem Flashen faellt die Anbindung ab - ohne diesen Schritt scheitert der naechste Monitorlauf. |
| `monitor.py` | **Linux-Seite** (WSL): liest den seriellen Monitor und schickt Befehle. `monitor.py 75 version free v4_bus v4_selftest`. Setzt DTR/RTS bewusst auf False (DTR haengt auf GPIO0, RTS auf EN) und versucht einen Reset in den Laufmodus ueber einen RTS-Puls. |

## Flashen und Lesen unter Linux (WSL)

Seit dem 08.10. haengt das Board per `usbipd-win` an WSL, damit unter Linux
gebaut **und** geflasht werden kann — ohne Windows-Umweg. Ablauf:

```bash
# einmalig, Windows als Administrator
winget install --interactive --exact dorssel.usbipd-win
# einmalig, WSL (Passwort noetig)
sudo apt install -y linux-tools-virtual hwdata
sudo update-alternatives --install /usr/local/bin/usbip usbip \
     $(ls /usr/lib/linux-tools/*/usbip | tail -n1) 20
sudo modprobe vhci-hcd

# pro Verbindung: Board anstecken, dann
usbipd list                       # Bus-ID des CP210x suchen
usbipd bind --busid <ID>          # nur beim ersten Mal, als Administrator
usbipd attach --wsl --busid <ID>  # danach /dev/ttyUSB0 in WSL

# flashen (Parameter wie flash_only.ps1: kein Reset durch esptool)
cd build
ARGS=$(grep -v '^\s*#' flash_args | tr '\n' ' ')
python -m esptool --chip esp32 -p /dev/ttyUSB0 -b 460800 \
       --before no-reset --after no-reset write-flash $ARGS

# lesen und bedienen
python ../tools/monitor.py 75 version free v4_bus v4_selftest
```

Zwei Eigenheiten: der Chip bleibt nach dem Flashen im Bootloader, und der
Reset-Puls ueber RTS hilft nicht in jedem Fall — dann EN druecken. Wird das
Board vom USB getrennt, faellt die Anbindung ab und `usbipd attach` muss
wiederholt werden. Und: solange das Geraet an WSL haengt, ist es fuer Windows
weg (COM7 verschwindet), die PowerShell-Skripte laufen dann nicht.
