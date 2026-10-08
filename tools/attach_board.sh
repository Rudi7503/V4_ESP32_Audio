#!/bin/sh
# attach_board.sh - haengt das ESP32-Board per usbipd an WSL und wartet auf /dev/ttyUSB*.
#
# WOZU: nach jedem Flashen (und nach jedem USB-Neustart des Boards) faellt die
# usbip-Anbindung ab - Windows zeigt das Geraet dann wieder als COMx, WSL sieht
# nichts. Ohne diesen Schritt scheitert der naechste Monitorlauf mit
# "could not open port /dev/ttyUSB0".
#
# Die Bus-ID wird AUTOMATISCH gesucht (CP210x = 10c4:ea60). Sie aendert sich,
# sobald das Board umgesteckt wird - am 08.10.2026 von 1-5 auf 1-6, und der fest
# verdrahtete Wert liess den Mitschnitt still scheitern.
#
# Aufruf: sh tools/attach_board.sh [BUSID]      (ohne Angabe: automatisch)
#
# Einmalige Einrichtung auf der Windows-Seite: usbipd-win installieren, dann
#   usbipd list
#   usbipd bind --busid <ID>     (einmalig, als Administrator)
# Details in tools/README.md.

USBIPD="/mnt/c/Program Files/usbipd-win/usbipd.exe"
BUSID="${1:-}"

if [ -c /dev/ttyUSB0 ]; then
    echo "/dev/ttyUSB0 ist schon da"
    exit 0
fi

if [ -z "$BUSID" ]; then
    BUSID=$("$USBIPD" list 2>/dev/null | awk '/10c4:ea60/ {print $1; exit}')
fi

if [ -z "$BUSID" ]; then
    echo "kein CP210x (10c4:ea60) in 'usbipd list' - Board angesteckt?"
    exit 1
fi

echo "Bus-ID: $BUSID"
"$USBIPD" attach --wsl --busid "$BUSID" >/dev/null 2>&1

i=0
while [ ! -c /dev/ttyUSB0 ] && [ "$i" -lt 20 ]; do
    sleep 0.5
    i=$((i + 1))
done

if [ -c /dev/ttyUSB0 ]; then
    echo "/dev/ttyUSB0 angehaengt"
    exit 0
fi

echo "nicht angehaengt - 'usbipd list' pruefen (Zustand muss 'Shared' sein)"
exit 1
