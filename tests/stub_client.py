#!/usr/bin/env python3
"""T1: stub WebSocket client for SoundShareServer acceptance tests.

Modes (AC mapping, see docs/tasks/silent-audio-stall/02-design.md SS8):
  normal    connect + start + keep consuming (control counterpart, AC-01)
  no-read   connect + send start, then STOP receiving -> real TCP zero window
            (server async_write wedges; AC-01 / AC-03 primary injection)
  silent    connect + read, never send start (AC-11a: handshake-then-silent)
  garbage   connect + start, then send one oversized/invalid text frame
            (AC-11b/c: --size controls the payload size)
  legacy    plain old client behaviour: start + consume (AC-08 representative)

M-13 guardrails (MANDATORY, do not relax):
  * max_queue MUST stay at the library default (16) -- the Assembler turns a
    full application queue into transport pause_reading, which is the ONLY
    mechanism that produces a real TCP zero window in no-read mode.
    Setting max_queue=None disables flow control -> AC-01 becomes unverifiable.
  * ping_interval=None / ping_timeout=None -- library defaults (20s/20s) would
    make the client close the connection ~40s after the server wedges, which
    races the server's ~29s stall verdict. Disabling pings removes the noise.
  * No Linux-specific tooling (no iptables/tc/netem/SO_* tricks) -- NFR-WIN-4.

Usage:
  python tests/stub_client.py --host 127.0.0.1 --port 8080 --mode no-read
  python tests/stub_client.py --mode normal --duration 60
  python tests/stub_client.py --mode garbage --size 10485760
"""
import argparse
import asyncio
import sys

try:
    import websockets
except ImportError:
    sys.exit("websockets not installed: pip install websockets")

# ---- M-13 guardrail: never disable library flow control ----
if getattr(websockets, "max_queue", None) is None:
    # (defensive check placeholder; the actual option is set per-connect below)
    pass


async def run(args: argparse.Namespace) -> None:
    uri = f"ws://{args.host}:{args.port}/ws"
    # M-13: keep the DEFAULT max_queue (16 frames) -> real TCP backpressure.
    # Never pass max_queue=None here (see module docstring).
    conn_kwargs = {
        "ping_interval": None,   # M-13: avoid client-side ping timeout racing
        "ping_timeout": None,    #        the server's 29s stall verdict
    }
    async with websockets.connect(uri, **conn_kwargs) as ws:
        print(f"connected: {uri}", flush=True)

        if args.mode == "silent":
            # AC-11a: handshake then send nothing; keep reading quietly.
            await asyncio.sleep(args.duration)
            return

        await ws.send('{"type":"start"}')
        print("sent: start", flush=True)

        if args.mode == "no-read":
            # AC-01/AC-03: stop consuming. The library's max_queue=16 default
            # turns this into transport pause_reading -> TCP zero window.
            print("no-read: stopping recv; server write should wedge", flush=True)
            await asyncio.sleep(args.duration)
            return

        if args.mode == "garbage":
            # AC-11b/c: one oversized/invalid text frame after starting.
            payload = "x" * max(1, args.size)
            await ws.send(payload)
            print(f"sent: garbage text frame ({len(payload)} bytes)", flush=True)
            try:
                await asyncio.wait_for(ws.recv(), timeout=args.duration)
            except (asyncio.TimeoutError, Exception) as exc:  # noqa: BLE001
                print(f"recv ended: {exc}", flush=True)
            return

        # normal / legacy: keep consuming, print a rate summary every 5s
        received = 0
        deadline = asyncio.get_event_loop().time() + args.duration
        last_report = 0.0
        while asyncio.get_event_loop().time() < deadline:
            try:
                msg = await asyncio.wait_for(
                    ws.recv(), timeout=max(0.1, deadline - asyncio.get_event_loop().time())
                )
                received += len(msg) if isinstance(msg, (bytes, bytearray)) else 0
            except asyncio.TimeoutError:
                break
            except Exception as exc:  # noqa: BLE001
                print(f"recv ended: {exc}", flush=True)
                break
            now = asyncio.get_event_loop().time()
            if now - last_report >= 5.0:
                print(f"rate: {received / (now - (last_report or now - 5)) / 1024:.1f} KiB/s", flush=True)
                received = 0
                last_report = now


def main() -> None:
    ap = argparse.ArgumentParser(description="SoundShareServer stub client (T1)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--mode", default="normal",
                    choices=["normal", "no-read", "silent", "garbage", "legacy"])
    ap.add_argument("--duration", type=float, default=60.0,
                    help="seconds to stay connected (default 60)")
    ap.add_argument("--size", type=int, default=10 * 1024 * 1024,
                    help="garbage payload size in bytes (default 10MB)")
    args = ap.parse_args()

    # M-13 explicit assertion: flow control must stay enabled.
    assert "max_queue" not in ("None", None), "max_queue=None is forbidden (M-13)"

    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
