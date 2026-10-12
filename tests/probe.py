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
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--watch", type=float, default=0,
                    help="watch duration in seconds (default: one shot)")
    ap.add_argument("--interval", type=float, default=2.0)
    ap.add_argument("--json", action="store_true", help="print raw JSON per poll")
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
        if time.monotonic() >= deadline:
            break
        time.sleep(min(args.interval, max(0.1, deadline - time.monotonic())))


if __name__ == "__main__":
    main()
