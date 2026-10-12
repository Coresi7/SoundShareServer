#!/usr/bin/env python3
"""T2: mock WebSocket audio server for AC-05 / AC-06 (preferred injection,
per design section 5.9.5 — does NOT need the fault-injection build).

Behavior: accept a client, send audio_config, stream N seconds of PCM16
silence, then go QUIET while keeping the WebSocket connection open (no
FIN/RST) — the client-side stall watchdog must fire and reconnect.

Derived from docs/tasks/soundshareclient-minimal-console/logs/mock_ws_ac004.py
(M-11: reuse existing asset, extended with --pcm-seconds).

NOT a CTest suite member — manually driven acceptance script (M-12).
"""

import argparse
import asyncio
import sys

try:
    from websockets.asyncio.server import serve
except ImportError:
    print("ERROR: 'websockets' not installed. Run: pip install websockets", file=sys.stderr)
    sys.exit(2)


async def handler(websocket, pcm_seconds, sample_rate, channels):
    print(f"client connected path={websocket.request.path}", flush=True)
    msg = await websocket.recv()
    print(f"recv: {msg}", flush=True)

    config = ('{"type":"audio_config","sampleRate":%d,"channels":%d,"format":"pcm16"}'
              % (sample_rate, channels))
    await websocket.send(config)
    print(f"sent: {config}", flush=True)

    # N seconds of silent PCM16 frames (~20ms each)
    frame = b"\x00\x00" * (sample_rate * channels // 50)
    frames = int(pcm_seconds * 50)
    for i in range(frames):
        await websocket.send(frame)
        await asyncio.sleep(0.02)
    print(f"sent {frames} PCM frames over {pcm_seconds}s; going QUIET "
          f"(connection kept open, no FIN/RST)", flush=True)

    # hold the connection in silence forever
    await asyncio.Future()


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=59997)
    ap.add_argument("--pcm-seconds", type=float, default=5.0,
                    help="seconds of PCM before going quiet (default 5)")
    ap.add_argument("--sample-rate", type=int, default=48000)
    ap.add_argument("--channels", type=int, default=2)
    args = ap.parse_args()

    async with serve(lambda ws: handler(ws, args.pcm_seconds, args.sample_rate, args.channels),
                     args.host, args.port, ping_interval=None, ping_timeout=None):
        print(f"mock ws server on ws://{args.host}:{args.port}/ (pcm {args.pcm_seconds}s "
              f"then quiet)", flush=True)
        await asyncio.Future()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
