"""make_tone48k.py - erzeugt einen 440-Hz-Ton mit 48 kHz Abtastrate.

Zweck: der A2DP-Source des ESP32 ist fest auf 44100 Hz verdrahtet
(btc_a2dp_source.c). Eine 48-kHz-Datei kommt deshalb mit 44100/48000 =
91,875 % Geschwindigkeit heraus - aus 440 Hz werden rund 404 Hz, also gut
eineinhalb Halbtoene zu tief. Der vorhandene 44,1-kHz-Ton kann das nicht
zeigen, weil er genau passt.

Gleiche Parameter wie test_tone_440.wav (3 s, 16 Bit, mono), nur 48 kHz.
"""
import math
import struct
import wave

PATH = r"D:\programmier_ordner\ESP-IDF\ESP32-I2S-to-BT\test_tone_48k.wav"
RATE = 48000
SECONDS = 3.0
FREQ = 440.0
AMPL = 0.5

n = int(RATE * SECONDS)
frames = bytearray()
for i in range(n):
    v = int(AMPL * 32767.0 * math.sin(2.0 * math.pi * FREQ * i / RATE))
    frames += struct.pack("<h", v)

with wave.open(PATH, "wb") as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(RATE)
    w.writeframes(bytes(frames))

import os
size = os.path.getsize(PATH)
print(f"{PATH}")
print(f"{size} Byte, {n} Samples, {RATE} Hz, {SECONDS:.0f} s, {FREQ:.0f} Hz mono 16 Bit")
print(f"Erwartet bei falscher Wiedergabe: {FREQ * 44100 / RATE:.1f} Hz "
      f"({44100 * 10000 // RATE / 100:.2f} % Geschwindigkeit)")
