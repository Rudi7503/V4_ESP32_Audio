"""rate_conv_model.py - Validierung der Filterauslegung fuer den Q15-Ratumsetzer.

Modelliert exakt den Algorithmus, der danach in sd_rate_conv.c steht:
  - Fenster-Sinc-Tiefpass, Blackman-gefenstert
  - TAPS Abgriffe, PHASES Phasen, jede Zeile auf Summe 32768 normiert (Q15)
  - Positionsschritt step = in_rate/target_rate in Q32
  - Akkumulation in int64, Ausgabe acc >> 15

Geprueft wird:
  1. Jede Phasenzeile summiert sich exakt auf 32768 (Gleichspannungsverstaerkung 1)
  2. Summe der Betraege < 65536 -> int32 waere auch sicher, int64 ist es garantiert
  3. 48 kHz -> 44,1 kHz: ein 440-Hz-Sinus bleibt 440 Hz mit korrekter Amplitude
  4. 22,05 kHz -> 44,1 kHz: Verdopplung, ebenfalls amplitudentreu
  5. Durchlassdaempfung und Stopbanddaempfung der Filterbank
"""
import math

TAPS = 32
PHASES = 128
TARGET = 44100


def build_table(in_rate):
    """Rueckgabe: [phases][taps] als int, plus Koeffizienten als float."""
    # Grenzfrequenz: bei Downsampling auf die Ausgabe-Nyquist begrenzen.
    fc = min(1.0, TARGET / float(in_rate))

    coef = [[0.0] * TAPS for _ in range(PHASES)]
    for p in range(PHASES):
        frac = p / float(PHASES)
        for j in range(TAPS):
            # Position des Abgriffs relativ zum Ausgabezeitpunkt
            u = (j - TAPS // 2 + 1) - frac
            if abs(u) < 1e-9:
                s = 1.0
            else:
                s = math.sin(math.pi * fc * u) / (math.pi * fc * u)
            # Blackman-Fenster ueber den Abgriffen
            w = (0.42
                 - 0.5 * math.cos(2 * math.pi * j / (TAPS - 1))
                 + 0.08 * math.cos(4 * math.pi * j / (TAPS - 1)))
            coef[p][j] = s * fc * w

        # Auf Gleichspannungsverstaerkung 1 normieren
        total = sum(coef[p])
        for j in range(TAPS):
            coef[p][j] /= total

    q = [[max(-32768, min(32767, int(round(c * 32768)))) for c in row] for row in coef]
    # Rundung auf exakt 32768 nachziehen, damit DC wirklich 1 ist
    for p in range(PHASES):
        diff = 32768 - sum(q[p])
        if diff:
            j = TAPS // 2
            q[p][j] += diff
    return q


def resample(q, x, in_rate):
    step = (in_rate / float(TARGET)) * (1 << 32)
    pos = 0.0
    out = []
    n = len(x)
    while True:
        base = int(pos // (1 << 32))
        if base + TAPS // 2 > n - 1:
            break
        frac = (pos / float(1 << 32)) - base
        phase = min(PHASES - 1, int(frac * PHASES))
        acc = 0
        for j in range(TAPS):
            idx = base - TAPS // 2 + 1 + j
            if idx < 0:
                continue
            acc += q[phase][j] * x[idx]
        out.append(acc >> 15)
        pos += step
    return out


def rms(v):
    return math.sqrt(sum(s * s for s in v) / len(v)) if v else 0.0


def test_sine(in_rate, freq):
    q = build_table(in_rate)
    n = in_rate  # 1 Sekunde
    x = [int(20000 * math.sin(2 * math.pi * freq * i / in_rate)) for i in range(n)]
    y = resample(q, x, in_rate)
    # Frequenz ueber Nulldurchgaenge schaetzen, Amplituden ueber RMS
    zc = sum(1 for i in range(1, len(y)) if (y[i - 1] < 0) != (y[i] < 0))
    f_est = zc / 2.0 / (len(y) / TARGET)
    a_in = rms(x[1000:-1000])
    a_out = rms(y[1000:-1000])
    return f_est, a_in, a_out, len(x), len(y)


print("=== Tabellenpruefung ===")
for rate in (48000, 32000, 22050, 24000):
    q = build_table(rate)
    sums = {sum(row) for row in q}
    absmax = max(sum(abs(c) for c in row) for row in q)
    print(f"  in={rate:6d}  Zeilensummen={sorted(sums)}  max Summe|h|={absmax}"
          f"  <65536: {absmax < 65536}")

print()
print("=== Sinustest (Soll-Amplitude 20000 RMS = 14142) ===")
for rate, freq in ((48000, 440.0), (48000, 1000.0), (22050, 440.0), (32000, 440.0)):
    f_est, a_in, a_out, nin, nout = test_sine(rate, freq)
    expect_n = nin * TARGET // rate
    print(f"  {rate:6d} Hz -> 44100, {freq:7.1f} Hz: "
          f"gemessen {f_est:7.1f} Hz, RMS {a_in:7.0f} -> {a_out:7.0f} "
          f"(Verhaeltnis {a_out / a_in:.4f}), {nin} -> {nout} Samples (Soll ~{expect_n})")

print()
print("=== Durchlass-/Sperrdaempfung der Filterbank (48 kHz) ===")
q = build_table(48000)
for f in (100, 1000, 5000, 10000, 18000, 20000, 22000, 23000, 24000):
    # Antwort der Phasen 0 auf einen komplexen Zeiger
    acc_re = acc_im = 0.0
    for j in range(TAPS):
        idx = -TAPS // 2 + 1 + j
        ph = -2 * math.pi * f * idx / 48000
        acc_re += q[0][j] / 32768.0 * math.cos(ph)
        acc_im += q[0][j] / 32768.0 * math.sin(ph)
    mag = math.hypot(acc_re, acc_im)
    db = 20 * math.log10(mag) if mag > 0 else -99
    mark = "  <-- Ausgabe-Nyquist" if f == 22000 else ""
    print(f"  {f:6d} Hz: {db:+7.2f} dB{mark}")
