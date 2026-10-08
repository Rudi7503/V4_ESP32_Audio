#!/bin/bash
# flash_linux.sh - flasht unter Linux (WSL). Gegenstueck zu tools/flash_only.ps1.
#
# WICHTIG: mit bash aufrufen ("bash tools/flash_linux.sh"), nicht mit sh/dash -
# export.sh aus ESP-IDF ist ein Bash-Skript, unter dash kommt die Umgebung
# nicht hoch und es fehlt danach "python".
#
# Aufruf: bash tools/flash_linux.sh [Buildverzeichnis]
#
# Das Buildverzeichnis darf BELIEBIG sein - auch ein fremdes Image zum Vergleich
# (z. B. /mnt/d/Coding/ESP-IDF/V4_ESP32/build fuer den alten Stand 0.9.56).
# Immer dieses Skript nehmen, nie ein eigenes ad-hoc-Skript: nur hier kommt der
# Umsteck-Hinweis nach dem Flashen mit heraus, und der gehoert zu JEDEM Flash.
#         Vorgabe: build  (relativ zum Projektverzeichnis, aus dem aufgerufen wird)
#
# Voraussetzungen:
#   - ESP-IDF v6.1 unter ~/esp-idf (siehe tools/README.md)
#   - Board per usbipd an WSL angehaengt -> /dev/ttyUSB0 (tools/attach_board.sh)
#   - Benutzer in der Gruppe dialout, oder Aufruf ueber 'sg dialout -c ...'
#   - **Chip im Download-Modus**: BOOT halten, EN tippen, BOOT loslassen.
#     Der automatische Reset von esptool funktioniert auf dieser Platine NICHT:
#       ERROR: Failed to connect to ESP32: Wrong boot mode detected (0x1b)!
#     DTR/RTS haengen hier nicht an BOOT/EN. Ein Flash kostet also immer einen
#     Handgriff - alles andere laeuft von allein.
#
# Wichtig: die flash_args der Windows-Ablage haben CRLF. Ohne 'tr -d "\r"'
# scheitert esptool mit "Invalid value for '--flash-size': '4MB\r'".

set -u

BUILD="${1:-build}"
PORT="${PORT:-/dev/ttyUSB0}"
BAUD="${BAUD:-460800}"

if [ ! -f "$BUILD/flash_args" ]; then
    echo "Kein flash_args in '$BUILD' - erst bauen (idf.py build)"
    exit 1
fi
if [ ! -c "$PORT" ]; then
    echo "$PORT fehlt - 'sh tools/attach_board.sh' aufrufen"
    exit 1
fi

# shellcheck disable=SC1090
. "$HOME/esp-idf/export.sh" >/dev/null 2>&1

ARGS=$(tr -d '\r' < "$BUILD/flash_args" | grep -v '^#' | tr '\n' ' ')
echo "Baue aus : $BUILD"
echo "Port     : $PORT"
echo "Parameter: $ARGS"

cd "$BUILD" || exit 1
LOG="${TMPDIR:-/tmp}/flash_linux.log"
python -m esptool --chip esp32 -p "$PORT" -b "$BAUD" \
       --before no-reset --after no-reset \
       write-flash $ARGS 2>&1 | tee "$LOG"

# Erfolg wird aus dem LOG gelesen, nicht aus dem Exit-Code: der Exit-Code einer
# Pipe ist der von tee (docs/ARBEITSWEISE.md, Regel 3).
if grep -q 'Staying in bootloader' "$LOG"; then
    echo
    echo "===================================================================="
    echo " FLASH FERTIG - JETZT UMSTECKEN!"
    echo
    echo "  1. BOOT/GPIO2-Jumper entfernen (bzw. Modul zurueck in den Sockel)."
    echo "  2. Stromzyklus oder EN druecken."
    echo
    echo " Erst danach ist die SD-Karte wieder da: solange GPIO2 (DAT0) auf LOW"
    echo " gehalten wird, scheitert sdmmc_init_ocr mit 0x107 und die Karte"
    echo " mountet nicht - und der Chip startet im falschen Bootmodus."
    echo "===================================================================="
else
    echo
    echo "FLASH FEHLGESCHLAGEN - Log: $LOG"
    echo "Meist: Chip nicht im Download-Modus (BOOT halten, EN tippen, BOOT loslassen)."
    exit 1
fi
