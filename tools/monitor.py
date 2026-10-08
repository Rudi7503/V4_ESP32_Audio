#!/usr/bin/env python3
"""
monitor.py - seriellen Monitor des ESP32 lesen und Konsolenbefehle senden.

Fuer die WSL-Seite, seit das Board per usbip an Linux haengt.

WICHTIG (aus tools/flash_only.ps1 uebernommen): DTR haengt auf GPIO0, RTS auf EN.
Deshalb werden beide Leitungen ausdruecklich NICHT gesetzt, sondern auf False
gelassen - sonst haelt RTS den Chip im Reset und DTR ihn im Download-Modus.
Ein Reset in den LAUFmodus ist genau: DTR=False (GPIO0 hoch) und dann RTS kurz
auf True (EN low) und zurueck.

Aufruf: monitor.py <Lesesekunden> [--reset] [Befehl ...]

Ohne --reset bleibt der Chip unberuehrt (fuer Befehle im laufenden Betrieb).
Mit --reset wird ein Neustart in den Laufmodus versucht, um den Boot-Log zu
sehen.
"""
import sys
import time

import serial

PORT = "/dev/ttyUSB0"
BAUD = 115200


def log(s):
    print(s, flush=True)


def main():
    argv = sys.argv[1:]
    reset = "--reset" in argv
    argv = [a for a in argv if a != "--reset"]
    read_s = float(argv[0]) if argv else 40.0
    cmds = argv[1:]

    ser = serial.Serial()
    ser.port = PORT
    ser.baudrate = BAUD
    ser.bytesize = 8
    ser.parity = serial.PARITY_NONE
    ser.stopbits = serial.STOPBITS_ONE
    ser.timeout = 0.2
    # Beide Leitungen ausdruecklich inaktiv, BEVOR geoeffnet wird.
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.dtr = False
    ser.rts = False
    time.sleep(0.2)

    t0 = time.time()
    log("### Monitor laeuft (%s @ %d) ###" % (PORT, BAUD))

    # Reset in den Laufmodus NUR auf Wunsch (--reset als erstes Argument).
    # Ohne das bleibt der Chip unberuehrt: ein Reset mitten im Betrieb reisst
    # Stream und Wiedergabe ab.
    if reset:
        ser.rts = True
        time.sleep(0.15)
        ser.rts = False
        log("### Reset-Puls ueber RTS gesendet - falls nichts kommt: EN druecken ###")
    else:
        log("### kein Reset (--reset erzwingt einen) ###")

    plan = list(cmds)
    next_cmd_at = 12.0        # erster Befehl nach 12 s
    got_any = False
    buf = b""

    while time.time() - t0 < read_s:
        try:
            data = ser.read(4096)
        except Exception as exc:                      # noqa: BLE001
            log("### Lesefehler: %s ###" % exc)
            break
        if data:
            got_any = True
            buf += data
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()

        now = time.time() - t0
        if plan and now >= next_cmd_at:
            c = plan.pop(0)
            log("\n>>> %s" % c)
            ser.write((c + "\n").encode())
            ser.flush()
            next_cmd_at = now + 9.0

        if not got_any and now > 10.0 and plan:
            pass

    log("\n### Ende nach %.0f s, %d Byte empfangen ###" % (read_s, len(buf)))
    ser.close()


if __name__ == "__main__":
    main()
