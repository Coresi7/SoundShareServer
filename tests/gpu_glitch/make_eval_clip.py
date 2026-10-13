#!/usr/bin/env python3
"""Generate the fixed 30s evaluation clip (02-design.md §7.6 / C1).

48kHz stereo 16-bit WAV: pink-noise floor + one -6dBFS short transient every
2 seconds (15 countable transients). Used as the loopback playback source for
baseline/post windows and subjective A/B listening (sha256-fixed artifact).
"""

import hashlib
import math
import os
import struct
import sys
import wave

SR = 48000
DUR_S = 30
CH = 2


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "eval-30s.wav"
    n = SR * DUR_S
    # Voss-McCartney-ish simple pink noise via one-pole filters (3 octaves)
    b0 = b1 = b2 = 0.0
    samples = bytearray()
    rnd_state = 0x12345678

    def rnd():
        nonlocal rnd_state
        x = rnd_state
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        rnd_state = x
        return (x / 0xFFFFFFFF) * 2.0 - 1.0

    for i in range(n):
        w = rnd()
        b0 = 0.99765 * b0 + w * 0.0990460
        b1 = 0.96300 * b1 + w * 0.2965164
        b2 = 0.57000 * b2 + w * 1.0526913
        v = (b0 + b1 + b2 + w * 0.1848) * 0.12   # ~-18dBFS floor
        # transient every 2s: 3ms decaying 4kHz ping at -6dBFS
        t_in_cycle = (i % (SR * 2)) / SR
        if t_in_cycle < 0.003:
            env = math.exp(-t_in_cycle / 0.001)
            v += 0.5 * env * math.sin(2 * math.pi * 4000 * t_in_cycle)
        s = max(-1.0, min(1.0, v))
        si = int(s * 32767)
        samples += struct.pack("<hh", si, si)

    with wave.open(out, "wb") as f:
        f.setnchannels(CH)
        f.setsampwidth(2)
        f.setframerate(SR)
        f.writeframes(bytes(samples))

    with open(out, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    print("%s: %d bytes, %ds, sha256=%s" % (out, os.path.getsize(out), DUR_S, digest))


if __name__ == "__main__":
    main()
