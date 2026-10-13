#!/usr/bin/env python3
"""Loop-play the eval clip on the default render device (loopback source).

The capture-side observation windows (capture_window.py) need actual audio
flowing through the system mixer; an idle loopback endpoint stretches packet
periods far beyond the device period and starves the steady window of
samples (see tests/README.md: "play looping audio on the server machine").

Reliable looping on this box (SND_FILENAME+SND_LOOP stops after one pass;
SND_MEMORY+SND_ASYNC raises; SND_MEMORY+SND_LOOP stops silently): plain
synchronous SND_FILENAME playback re-armed in a tight loop -- the inter-pass
gap is a single call boundary (sub-millisecond), negligible against the
device period.

Runs until the process is killed (Ctrl+C / terminate).
"""

import os
import sys
import time
import winsound

SELF_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CLIP = os.path.join(SELF_DIR, "..", "..", "docs", "tasks",
                            "T-GPU-AUDIO-GLITCH", "evidence", "clip", "eval-30s.wav")


def main():
    clip = sys.argv[1] if len(sys.argv) > 1 else os.path.abspath(DEFAULT_CLIP)
    if not os.path.isfile(clip):
        sys.exit("clip not found: %s" % clip)
    print("loop-playing (sync re-arm): %s (kill to stop)" % clip, flush=True)
    try:
        while True:
            winsound.PlaySound(clip, winsound.SND_FILENAME)   # blocks for clip duration
            time.sleep(0.001)
    except KeyboardInterrupt:
        pass
    finally:
        winsound.PlaySound(None, winsound.SND_PURGE)


if __name__ == "__main__":
    main()
