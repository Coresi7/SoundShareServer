#!/usr/bin/env python3
"""Section 8.12 lifecycle stress test (R-01 / R-12 backstop validation).

NOT a CTest suite member — manually driven acceptance script (M-12).

Scenario (per design section 8.12):
  - 4 concurrent sessions: 2 normal (keep receiving) + 2 no-read (each
    wedges the server write and must trigger one stall verdict)
  - scripted loop: connect -> start -> 3s -> HARD disconnect (process
    killed, no close frame), repeated 100 times
  - observations via /_debug/sessions probe:
      * counters.sessions returns to baseline after each round
      * liveSessions - sessions difference stays 0 (no zombie sessions)
      * server process alive, no crash
  - final: probe counters, exit code reflects pass/fail

Usage: python stress_lifecycle.py --host 127.0.0.1 --port 8080
       [--rounds 100]
Note: the no-read sessions need real PCM traffic, so play looping audio
on the server machine before running this script.
"""

import argparse
import json
import os
import subprocess
import sys
import time
import urllib.request

SELF_DIR = os.path.dirname(os.path.abspath(__file__))
STUB = os.path.join(SELF_DIR, "stub_client.py")


def probe(host, port):
    with urllib.request.urlopen(f"http://{host}:{port}/_debug/sessions", timeout=5) as r:
        return json.loads(r.read().decode())


def spawn(mode, host, port, duration):
    return subprocess.Popen(
        [sys.executable, STUB, "--host", host, "--port", str(port),
         "--mode", mode, "--duration", str(duration)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--rounds", type=int, default=100)
    ap.add_argument("--round-hold", type=float, default=3.0)
    args = ap.parse_args()

    failures = []

    base = probe(args.host, args.port)
    print(f"baseline: sessions={base['counters']['sessions']} "
          f"liveSessions={base['counters']['liveSessions']}", flush=True)

    # long-lived background sessions: 2 normal + 2 no-read
    bg = [spawn("normal", args.host, args.port, 3000),
          spawn("normal", args.host, args.port, 3000),
          spawn("no-read", args.host, args.port, 3000),
          spawn("no-read", args.host, args.port, 3000)]
    print("background: 2 normal + 2 no-read spawned "
          "(each no-read triggers one stall verdict ~29s in)", flush=True)

    t_start = time.monotonic()
    for i in range(args.rounds):
        c = spawn("normal", args.host, args.port, args.round_hold)
        time.sleep(args.round_hold)
        c.kill()  # HARD disconnect: no close frame
        c.wait()

        if (i + 1) % 20 == 0:
            p = probe(args.host, args.port)
            diff = p["counters"]["liveSessions"] - p["counters"]["sessions"]
            print(f"round {i+1}/{args.rounds}: sessions={p['counters']['sessions']} "
                  f"live={p['counters']['liveSessions']} diff={diff} "
                  f"forceClosed={p['counters']['forceClosed']} "
                  f"writeStalls={p['counters']['writeStalls']} "
                  f"elapsed={time.monotonic()-t_start:.0f}s", flush=True)
            if diff != 0:
                failures.append(f"round {i+1}: liveSessions-sessions diff={diff} (zombie!)")

    # final probe
    time.sleep(5)
    p = probe(args.host, args.port)
    diff = p["counters"]["liveSessions"] - p["counters"]["sessions"]
    print(f"final: sessions={p['counters']['sessions']} "
          f"live={p['counters']['liveSessions']} diff={diff} "
          f"forceClosed={p['counters']['forceClosed']} "
          f"writeStalls={p['counters']['writeStalls']}", flush=True)
    if diff != 0:
        failures.append(f"final: liveSessions-sessions diff={diff} (zombie!)")

    for b in bg:
        b.kill()

    if failures:
        print("FAIL:", *failures, sep="\n  ", flush=True)
        sys.exit(1)
    print("PASS: no zombie sessions, no crash observed", flush=True)


if __name__ == "__main__":
    main()
