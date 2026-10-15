#!/usr/bin/env python3
"""T3: probe client for GET /_debug/sessions (FR-10 / O-4).

Polls the read-only probe endpoint and prints/asserts:
  sessions[].{id, stalled, writing, queue.depth, streaming},
  streaming.count, counters.{sessions, liveSessions, forceClosed, writeStalls,
  captureCallbacks, sentPackets}. Session ids are monotonic sequence numbers
  (M-7), never pointer addresses.

NOT a CTest suite member -- manually driven acceptance script (M-12).

Usage:
  python probe.py --host 127.0.0.1 --port 8080            # one shot
  python probe.py --port 8080 --watch 60                  # poll every 2s for 60s
  python probe.py --port 8080 --watch 40 --json           # raw JSON lines

T-CAPTURE-IDLE-LOG (§3.5 / m15 / P1-6): extended from print-only to
read-and-assert. Existing output lines keep their prefixes (S-2); an
`idle:` line and `idleSim=` are appended. Optional assertion flags (default
off, so the snapshot role is preserved):

  --assert-idle-episodes-ge N         captureIdleEpisodes >= N
  --assert-last-idle-ms LO,HI         LO <= captureLastIdleMs <= HI
  --assert-total-idle-ms LO,HI        LO <= captureTotalIdleMs <= HI
  --assert-current-idle-ms-zero       captureCurrentIdleMs == 0
  --assert-interarrival-max-lt MS     captureInterArrivalMsMax < MS
  --assert-interarrival-samples-ge N  captureInterArrivalSamples >= N

Any failing assertion prints "FAIL: ..." to stderr and exits 2 (style:
capture_window.py fail()). m12: since T-CAPTURE-IDLE-LOG the
captureInterArrivalSaturated field no longer contains idle-boundary samples.
"""

import argparse
import json
import sys
import time
import urllib.request


def fetch(host, port):
    with urllib.request.urlopen("http://%s:%d/_debug/sessions" % (host, port), timeout=5) as r:
        return json.loads(r.read().decode())


def summary(p):
    lines = []
    for s in p.get("sessions", []):
        lines.append(
            "  session id=%d ssl=%d closed=%d stalled=%d writing=%d "
            "idleMs=%d/%d queue=%d(%db+%dt,%dB) enq=%d deq=%d drop=%d streaming=%d"
            % (s["id"], int(s["ssl"]), int(s["closed"]), int(s["stalled"]),
               int(s["writing"]), s["idleMs"], s["thresholdMs"],
               s["queue"]["depth"], s["queue"]["binary"], s["queue"]["text"],
               s["queue"]["bytes"], s["counters"]["enqueued"],
               s["counters"]["dequeued"], s["counters"]["droppedTotal"],
               int(s["streaming"])))
    c = p["counters"]
    lines.append(
        "  streaming.count=%s counters: sessions=%d live=%d forceClosed=%d "
        "writeStalls=%d captureCallbacks=%s sentPackets=%s"
        % (p.get("streaming", {}).get("count"), c["sessions"], c["liveSessions"],
           c["forceClosed"], c["writeStalls"], p.get("captureCallbacks"),
           p.get("sentPackets")))
    # T-GPU-AUDIO-GLITCH S0 (F6): capture observation line (02-design.md §6.4).
    # Appended only — existing output lines unchanged (backward compatible).
    # Missing keys (older server builds) render as "n/a".
    if any(k in p for k in ("captureDiscontinuityCount", "captureInterArrivalMsP99")):
        inj = p.get("injection", {}) or {}
        lines.append(
            "  capture: D=%s interArrival P50/P99/Max=%s/%s/%s n=%s sat=%s"
            % (p.get("captureDiscontinuityCount", "n/a"),
               p.get("captureInterArrivalMsP50", "n/a"),
               p.get("captureInterArrivalMsP99", "n/a"),
               p.get("captureInterArrivalMsMax", "n/a"),
               p.get("captureInterArrivalSamples", "n/a"),
               p.get("captureInterArrivalSaturated", "n/a")))
        lines.append(
            "         C P50/P99=%s/%s n=%s buf=%sf(%shns) mmcss=%s evt=%s handles=%s "
            "evtTimeout=%s wakes=%s reinit=%s"
            % (p.get("captureCallbackCriticalMsP50", "n/a"),
               p.get("captureCallbackCriticalMsP99", "n/a"),
               p.get("captureCallbackCriticalSamples", "n/a"),
               p.get("captureBufferFrames", "n/a"), p.get("captureBufferHns", "n/a"),
               p.get("captureMmcssRegistered", "n/a"), p.get("captureEventDriven", "n/a"),
               p.get("captureEventHandlesCreated", "n/a"),
               p.get("captureEventTimeouts", "n/a"),
               p.get("captureEventWakes", "n/a"),
               p.get("captureReinitCount", "n/a")))
        lines.append(
            "         endpoint=%s(gpu=%s) markerSeq=%s inject=(stall=%s/%s, batch=%s, skip=%s, idleSim=%s)"
            % (p.get("captureEndpointName", "n/a"),
               p.get("captureEndpointIsGpuAudio", "n/a"),
               p.get("markerSeq", "n/a"),
               inj.get("captureStallMs", "n/a"), inj.get("captureStallProbPpm", "n/a"),
               inj.get("captureBatchMs", "n/a"), inj.get("captureSkipPackets", "n/a"),
               inj.get("captureIdleSim", "n/a")))
        # T-CAPTURE-IDLE-LOG §3.5: idle line (appended only; existing lines keep
        # their prefixes, S-2). m12: since T-CAPTURE-IDLE-LOG, saturated no longer
        # contains idle-boundary samples (idle-exit first frames skip AddUs, FR-03).
        lines.append(
            "  idle: ep=%s lastMs=%s totalMs=%s curMs=%s (m12: sat excludes idle-boundary samples since T-CAPTURE-IDLE-LOG)"
            % (p.get("captureIdleEpisodes", "n/a"),
               p.get("captureLastIdleMs", "n/a"),
               p.get("captureTotalIdleMs", "n/a"),
               p.get("captureCurrentIdleMs", "n/a")))
    return "\n".join(lines)


# ---- T-CAPTURE-IDLE-LOG §3.5: optional assertions (default off) ----

def fail(msg):
    print("FAIL: %s" % msg, file=sys.stderr, flush=True)
    sys.exit(2)


def _num(p, key):
    v = p.get(key)
    if not isinstance(v, (int, float)):
        fail("assertion on %s: field missing/not numeric (got %r)" % (key, v))
    return v


def _range(arg):
    parts = arg.split(",")
    if len(parts) != 2:
        fail("expected LO,HI range, got %r" % arg)
    try:
        return float(parts[0]), float(parts[1])
    except ValueError:
        fail("expected LO,HI range, got %r" % arg)


def run_assertions(p, args):
    """Evaluate the --assert-* chain against the latest poll. Exit 2 on failure."""
    results = []
    if args.assert_idle_episodes_ge is not None:
        n = _num(p, "captureIdleEpisodes")
        ok = n >= args.assert_idle_episodes_ge
        results.append(("assert-idle-episodes-ge %d" % args.assert_idle_episodes_ge,
                        ok, "captureIdleEpisodes=%s" % n))
    if args.assert_last_idle_ms:
        lo, hi = _range(args.assert_last_idle_ms)
        v = _num(p, "captureLastIdleMs")
        ok = lo <= v <= hi
        results.append(("assert-last-idle-ms %s" % args.assert_last_idle_ms,
                        ok, "captureLastIdleMs=%s" % v))
    if args.assert_total_idle_ms:
        lo, hi = _range(args.assert_total_idle_ms)
        v = _num(p, "captureTotalIdleMs")
        ok = lo <= v <= hi
        results.append(("assert-total-idle-ms %s" % args.assert_total_idle_ms,
                        ok, "captureTotalIdleMs=%s" % v))
    if args.assert_current_idle_ms_zero:
        v = _num(p, "captureCurrentIdleMs")
        ok = (v == 0)
        results.append(("assert-current-idle-ms-zero", ok, "captureCurrentIdleMs=%s" % v))
    if args.assert_interarrival_max_lt is not None:
        v = _num(p, "captureInterArrivalMsMax")
        ok = v < args.assert_interarrival_max_lt
        results.append(("assert-interarrival-max-lt %s" % args.assert_interarrival_max_lt,
                        ok, "captureInterArrivalMsMax=%s" % v))
    if args.assert_interarrival_samples_ge is not None:
        v = _num(p, "captureInterArrivalSamples")
        ok = v >= args.assert_interarrival_samples_ge
        results.append(("assert-interarrival-samples-ge %d" % args.assert_interarrival_samples_ge,
                        ok, "captureInterArrivalSamples=%s" % v))
    for name, ok, detail in results:
        print("ASSERT %s: %s (%s)" % (name, "PASS" if ok else "FAIL", detail), flush=True)
        if not ok:
            fail("assertion %s failed (%s)" % (name, detail))
    if results:
        print("ASSERT chain: all %d PASS" % len(results), flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--watch", type=float, default=0,
                    help="watch duration in seconds (default: one shot)")
    ap.add_argument("--interval", type=float, default=2.0)
    ap.add_argument("--json", action="store_true", help="print raw JSON per poll")
    # T-CAPTURE-IDLE-LOG §3.5 assertions (default off; keeps the snapshot role)
    ap.add_argument("--assert-idle-episodes-ge", type=int, default=None, metavar="N")
    ap.add_argument("--assert-last-idle-ms", default=None, metavar="LO,HI")
    ap.add_argument("--assert-total-idle-ms", default=None, metavar="LO,HI")
    ap.add_argument("--assert-current-idle-ms-zero", action="store_true")
    ap.add_argument("--assert-interarrival-max-lt", type=float, default=None, metavar="MS")
    ap.add_argument("--assert-interarrival-samples-ge", type=int, default=None, metavar="N")
    args = ap.parse_args()

    deadline = time.monotonic() + args.watch
    while True:
        try:
            p = fetch(args.host, args.port)
        except Exception as e:
            print("probe failed: %s" % e, file=sys.stderr)
            sys.exit(2)
        if args.json:
            print(json.dumps(p), flush=True)
        else:
            print("probe @ %s" % time.strftime("%H:%M:%S"), flush=True)
            print(summary(p), flush=True)
        run_assertions(p, args)
        if time.monotonic() >= deadline:
            break
        time.sleep(min(args.interval, max(0.1, deadline - time.monotonic())))


if __name__ == "__main__":
    main()
