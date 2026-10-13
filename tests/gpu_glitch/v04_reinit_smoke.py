#!/usr/bin/env python3
"""V-04 (AC-FR03 hard gate): forced-capture-reinit smoke, x5 rounds.

02-design.md §8.2 / 03b §7.4:
  1) force_capture_reinit=1 five times;
  2) each round: captureReinitCount increments and the stream recovers
     (sentPackets keeps growing);
  3) captureEventHandlesCreated stays 1 across ALL re-inits (D-04: object-level
     unique handle, re-created handles would make this counter grow);
  4) after the rounds: a 60s L2 window shows D delta == 0;
  5) liveSessions - sessions == 0 throughout;
  6) P1-8: after V-04 the caller MUST stats_reset before opening the next
     steady window (re-init waits pollute Max) -- the final 60s window here
     starts from a fresh reset, satisfying the ordering.

Usage: python tests/gpu_glitch/v04_reinit_smoke.py --port 18080 [--rounds 5]
Exit code 0 = PASS, 2 = FAIL.
"""

import argparse
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

SELF_DIR = os.path.dirname(os.path.abspath(__file__))
TESTS_DIR = os.path.dirname(SELF_DIR)
STUB = os.path.join(TESTS_DIR, "stub_client.py")

def http_get(url, timeout=5):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.read().decode()


def probe(host, port):
    return json.loads(http_get("http://%s:%d/_debug/sessions" % (host, port)))


def fault_set(host, port, key, value):
    url = "http://%s:%d/_debug/fault/set?key=%s&value=%s" % (host, port, key, value)
    body = http_get(url)
    return json.loads(body).get("ok", False)


def wait_for(predicate, timeout, poll=0.5, what=""):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = predicate()
        if last:
            return True, time.monotonic()
        time.sleep(poll)
    return False, last


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--rounds", type=int, default=5)
    ap.add_argument("--final-window", type=float, default=60.0,
                    help="final L2 observation window seconds (default 60)")
    args = ap.parse_args()

    failures = []
    stub = subprocess.Popen(
        [sys.executable, STUB, "--host", args.host, "--port", str(args.port),
         "--mode", "normal", "--duration", str(args.rounds * 30 + args.final_window + 30)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print("stub streaming client pid %d" % stub.pid)
    try:
        base = probe(args.host, args.port)
        handles0 = base.get("captureEventHandlesCreated", 0)
        print("initial: eventHandlesCreated=%s eventDriven=%s reinit=%s"
              % (handles0, base.get("captureEventDriven"), base.get("captureReinitCount")))
        if base.get("captureEventDriven") is not True:
            failures.append("precondition: captureEventDriven != true (F3 not active)")

        for rnd in range(1, args.rounds + 1):
            before = probe(args.host, args.port)
            reinit_before = before.get("captureReinitCount", 0)
            sent_before = before.get("sentPackets", 0)

            if not fault_set(args.host, args.port, "force_capture_reinit", "1"):
                failures.append("round %d: fault_set failed" % rnd)
                break

            # reinit may take up to ~2s (recovery wait) + rebuild
            ok, _ = wait_for(
                lambda: probe(args.host, args.port).get("captureReinitCount", 0) > reinit_before,
                timeout=15)
            if not ok:
                failures.append("round %d: captureReinitCount did not increment" % rnd)
                continue
            # stream recovery: sentPackets grows again (allow up to 10s)
            time.sleep(3)
            ok2, _ = wait_for(
                lambda: probe(args.host, args.port).get("sentPackets", 0) > sent_before + 5,
                timeout=10)
            p = probe(args.host, args.port)
            handles = p.get("captureEventHandlesCreated", 0)
            diff = p["counters"]["liveSessions"] - p["counters"]["sessions"]
            print("round %d: reinit=%d sentPackets=%d(+%d) eventHandlesCreated=%d "
                  "live-sessions diff=%d streamRecovered=%s"
                  % (rnd, p.get("captureReinitCount"), p.get("sentPackets"),
                     p.get("sentPackets", 0) - sent_before, handles, diff, ok2))
            if not ok2:
                failures.append("round %d: stream did not recover (sentPackets stale)" % rnd)
            if handles != handles0 or handles != 1:
                failures.append("round %d: captureEventHandlesCreated=%d (expected 1, "
                                "D-04 violated)" % (rnd, handles))
            if diff != 0:
                failures.append("round %d: liveSessions-sessions=%d (zombie)" % (rnd, diff))

        # ---- final 60s L2 window (fresh reset first; P1-8 ordering) ----
        http_get("http://%s:%d/_debug/stats/reset" % (args.host, args.port))
        if not fault_set(args.host, args.port, "capture_stall_ms", "40"):
            failures.append("cannot arm capture_stall_ms")
        if not fault_set(args.host, args.port, "capture_stall_prob_ppm", "200000"):
            failures.append("cannot arm ppm")
        print("final L2 window (%ss) after re-init rounds..." % args.final_window)
        t0 = time.monotonic()
        d0 = probe(args.host, args.port).get("captureDiscontinuityCount", 0)
        while time.monotonic() - t0 < args.final_window:
            time.sleep(2)
        fault_set(args.host, args.port, "capture_stall_ms", "0")
        fault_set(args.host, args.port, "capture_stall_prob_ppm", "0")
        end = probe(args.host, args.port)
        d_delta = end.get("captureDiscontinuityCount", 0) - d0
        print("final window: D delta=%d eventHandlesCreated=%d eventDriven=%s"
              % (d_delta, end.get("captureEventHandlesCreated"), end.get("captureEventDriven")))
        if d_delta != 0:
            failures.append("final 60s window after re-inits: D delta=%d (expected 0)" % d_delta)
        if end.get("captureEventHandlesCreated", 0) != 1:
            failures.append("final: captureEventHandlesCreated != 1")
        diff = end["counters"]["liveSessions"] - end["counters"]["sessions"]
        if diff != 0:
            failures.append("final: liveSessions-sessions=%d (zombie)" % diff)
    finally:
        stub.kill()
        stub.wait()

    if failures:
        print("FAIL:", *failures, sep="\n  ")
        sys.exit(2)
    print("PASS: V-04 re-init smoke (handles==1, stream recovered, D==0, no zombies)")
    print("NOTE (P1-8): run stats_reset before opening the next steady window")


if __name__ == "__main__":
    main()
