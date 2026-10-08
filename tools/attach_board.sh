#!/bin/sh
# attach_board.sh - haengt das ESP32-Board per usbipd an WSL und wartet auf /dev/ttyUSB*.
#
# WOZU: nach jedem Flashen (und nach jedem USB-Neustart des Boards) faellt die
# usbip-Anbindung ab - Windows zeigt das Geraet dann wieder als COMx, WSL sieht
# nichts. Ohne diesen Schritt scheitert der naechste Monitorlauf mit
# "could not open port /dev/ttyUSB0".
#
# Aufruf: sh tools/attach_board.sh [BUSID]      (Vorgabe 1-5, siehe usbipd list)
#
# Einmalige Einrichtung auf der Windows-Seite: usbipd-win installieren, dann
#   usbipd list
#   usbipd bind --busid <ID>     (einmalig, als Administrator)
# Details in tools/README.md.

USBIPD="/mnt/c/Program Files/usbipd-win/usbipd.exe"
BUSID="${1:-1-5}"

if [ -c /dev/ttyUSB0 ]; then
    echo "/dev/ttyUSB0 ist schon da"
    exit 0
fi

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
