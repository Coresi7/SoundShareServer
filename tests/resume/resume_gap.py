#!/usr/bin/env python3
"""tests/resume/resume_gap.py — T-CLIENT-SILENT-ON-RESUME mock WS 音频服务端

设计依据：docs/tasks/T-CLIENT-SILENT-ON-RESUME/02-design.md §7.2.1
（闸门 APPROVED：03b-gate-review-design.md，硬门禁 H-13/H-16）。

场景模型（"无包间隙"复现缺陷，M-3 主取证路径）：
  [tone 1.5s] → [gap G s：完全不发送 / 全零帧，保持连接] → [tone 1.5s] × --cycles
  --cycles 0 = 持续推流（M1 稳态对照），直到外部停止。

加固（M-2② / H-16，全部写入 timeline.json）：
  - TCP_NODELAY（显式 setsockopt 确认；asyncio Windows 默认已开，此处幂等）
  - 绝对时间调度：落后 > 1 帧周期即 rebase（重设基准、不补发追帧）
    ⇒ 结构性不可能产生 ≥4 帧/20ms 突发（反例 (a) 源头消除）
  - 逐帧墙钟 frame_send_ts[] + frame_kind[]
  - max_frame_interval_ms：仅统计相邻 tone 帧之间的间隔（gap 边界与
    slow/silent 段不计，阈值 ≤40ms = 2×帧周期）
  - burst_ge4_count：20ms 滑窗内发送帧数 ≥4 ⇒ +1（反例 (a) 探测器；
    能让 baseline 逃逸的突发 ≥7680 样本 = 4 帧，必被捕获）

慢速恢复（M7/M8，--slow-rate/--hold）：
  第一个 gap 结束后进入 slow 段，以 --slow-rate 样本/s（交织样本）发送
  --hold 秒，随后恢复正常 tone 尾奏。
  打包规则：rate ≥ 4800 ⇒ 20ms 帧 × round(rate*0.02) 样本；
            rate <  4800 ⇒ 1s 帧 × rate 样本（M8: 64/s × 75s = 4800 < 5760）。

--stop-at-gap K S：第 K 个 gap 进行到第 S 秒时置 stop_event 并停发
（保持连接）——供 harness 优雅停止 SSC 以确定性取得间隙中期 final: 行
（间隙类型判别，设计 §3.4.2.1 / D-14）。

可独立运行（手动取证），也可被 run_matrix.py import（同进程、同一
time.monotonic() 时基 —— M-1① 逐行墙钟硬要求）。

NOT a CTest suite member — manually driven acceptance script（沿袭
tests/README.md M-12 约定）。
"""
import argparse
import asyncio
import json
import math
import struct
import sys
import time
from pathlib import Path

try:
    from websockets.asyncio.server import serve
except ImportError:
    print("ERROR: 'websockets' not installed. Run: pip install websockets",
          file=sys.stderr)
    sys.exit(2)

MOCK_VERSION = 1
SR = 48000
CH = 2
TONE_HZ = 1000.0
TONE_AMP = 0.707                # -3 dBFS
FRAME_MS = 20
FRAME_SAMPLES = SR * CH // 50   # 1920 interleaved samples / 20ms frame
PRELUDE_S = 1.5                 # 每循环前奏
TAIL_S = 1.5                    # 尾奏


def make_tone_frame(sample_index: int) -> bytes:
    """1 kHz / -3 dBFS 立体声正弦帧；sample_index 为帧内首样本的全局索引
    （交织样本计数，每帧推进 FRAME_SAMPLES）。"""
    out = bytearray()
    amp = TONE_AMP * 32767.0
    for i in range(FRAME_SAMPLES // 2):   # per-channel frames
        v = int(amp * math.sin(2.0 * math.pi * TONE_HZ
                               * (sample_index + i) / SR))
        out += struct.pack("<hh", v, v)
    return bytes(out)


def make_small_frame(count: int, sample_index: int) -> bytes:
    """慢速段小帧：count 个交织样本（每声道重复同值，保持 1kHz 相位）。"""
    out = bytearray()
    amp = TONE_AMP * 32767.0
    for i in range(count):
        v = int(amp * math.sin(2.0 * math.pi * TONE_HZ
                               * (sample_index + i) / (SR * CH)))
        out += struct.pack("<h", v)
    return bytes(out)


class Timeline:
    """会话时间轴 + 全部观测量（仅事件循环线程访问，无锁）。

    段表（segments）在构造时一次性展开：
      cycles=0            : [(tone, None=∞)]
      cycles>=1, 无 slow  : cycles × [(tone,1.5),(gap,G)] + [(tone,1.5)]
      cycles>=1, 有 slow  : [(tone,1.5),(gap,G),(slow,hold)] + [(tone,1.5)]
    gap 的实际行为由 gap_mode 决定：'nopacket'（本缺陷路径）/ 'silent'（零帧）。
    """

    def __init__(self, cycles: int, gap: float, mode: str, gap_mode: str,
                 slow_rate=None, hold=None, stop_at_gap=None):
        self.cycles = cycles
        self.gap = gap
        self.mode = mode                    # pause | reopen（场景标签，行为同构）
        self.gap_mode = gap_mode            # nopacket | silent
        self.slow_rate = slow_rate          # 样本/s（交织）或 None
        self.hold = hold
        self.stop_at_gap = stop_at_gap      # (K, S) 1-based 或 None

        # ---- 段表 ----
        self.segments = []                  # [(kind, dur, gap_idx)]
        if cycles <= 0:
            self.segments.append(("tone", None, None))
        else:
            for i in range(cycles):
                self.segments.append(("tone", PRELUDE_S, None))
                self.segments.append(("gap", gap, i + 1))
                if slow_rate and i == 0:
                    self.segments.append(("slow", hold, None))
            self.segments.append(("tone", TAIL_S, None))

        # ---- 观测 ----
        self.session_start = None           # 第一帧发送墙钟（时间轴 0 点）
        self.frame_send_ts = []
        self.frame_kind = []                # tone | silent | slow
        self.events = []                    # (name, wall_ts)
        self.gap_start_ts = []
        self.resume_ts = []                 # gap 结束后首帧（tone/slow）墙钟
        self.burst_ge4_count = 0
        self.rebase_count = 0
        self.stop_marker_ts = None
        self.stop_event = asyncio.Event()
        self.done = asyncio.Event()         # 时间轴走完（尾奏结束）
        self._prev_kind = None
        self._prev_gap_idx = None
        self._stop_latched = False

    # -- 时间轴查询 --
    def total_seconds(self):
        if any(d is None for _, d, _ in self.segments):
            return None
        return sum(d for _, d, _ in self.segments)

    def phase_at(self, t: float):
        """t（相对 session_start 秒）→ (kind, gap_idx)。
        kind ∈ tone | gap_nopacket | gap_silent | slow | stopped | done"""
        if self._stop_latched:
            return ("stopped", None)
        acc = 0.0
        for kind, dur, gidx in self.segments:
            if dur is None or t < acc + dur:
                if kind == "gap":
                    if self.stop_at_gap and not self._stop_latched:
                        k, s = self.stop_at_gap
                        if gidx == k and t >= acc + s:
                            return ("stopped", gidx)
                    return (("gap_silent" if self.gap_mode == "silent"
                             else "gap_nopacket"), gidx)
                return (kind, gidx)
            acc += dur
        return ("done", None)

    def _latch_stop(self, t: float, wall: float):
        self._stop_latched = True
        self.stop_marker_ts = wall
        self.events.append(("stop_marker", wall))
        self.stop_event.set()

    # -- 沿事件（由发送循环按 kind 变化驱动）--
    def _record_transition(self, kind, wall):
        if kind == self._prev_kind:
            return
        if kind in ("gap_nopacket", "gap_silent"):
            self.gap_start_ts.append(wall)
            self.events.append(("gap_start", wall))
        elif self._prev_kind in ("gap_nopacket", "gap_silent",
                                 "stopped") and self._prev_kind is not None:
            # gap → (tone|slow)：resume 沿由首帧发送侧调用 record_frame 补记
            pass
        self._prev_kind = kind

    def record_frame(self, kind, wall, samples: int):
        """每帧发送后调用：记录墙钟 / 沿事件 / burst 探测。"""
        prev = self._prev_kind
        if kind != prev:
            if prev in ("gap_nopacket", "gap_silent"):
                self.resume_ts.append(wall)
                self.events.append(("resume", wall))
            self._record_transition(kind, wall)
        self.frame_send_ts.append(wall)
        self.frame_kind.append(kind)
        # burst 探测：20ms 滑窗内帧数 ≥4（含当前帧）
        n = 0
        for ts in reversed(self.frame_send_ts):
            if wall - ts > 0.020:
                break
            n += 1
        if n >= 4:
            self.burst_ge4_count += 1
            self.events.append(("burst_ge4", wall))

    @property
    def max_frame_interval_ms(self):
        """仅统计【同一推流段内】相邻 tone 帧间隔（设计 §7.2.1：间隙边界
        帧不计）。跨段对（gap/slow 边界，间隔秒级）以 >200ms 分界跳过；
        段内异常停顿（40–200ms）仍会被捕获并计入（阈值 ≤40ms）。"""
        mx = 0.0
        prev_ts = None
        for ts, kind in zip(self.frame_send_ts, self.frame_kind):
            if kind != "tone":
                prev_ts = None
                continue
            if prev_ts is not None:
                gap_s = ts - prev_ts
                if gap_s <= 0.200:            # 段内（跨段边界秒级不计）
                    mx = max(mx, gap_s * 1000.0)
            prev_ts = ts
        return round(mx, 3)

    def to_dict(self):
        return {
            "mock_version": MOCK_VERSION,
            "cycles": self.cycles,
            "gap": self.gap,
            "mode": self.mode,
            "gap_mode": self.gap_mode,
            "slow_rate": self.slow_rate,
            "hold": self.hold,
            "stop_at_gap": list(self.stop_at_gap) if self.stop_at_gap else None,
            "frame_ms": FRAME_MS,
            "amplitude_hz": TONE_HZ,
            "sample_rate": SR,
            "channels": CH,
            "tcp_nodelay": True,
            "rebase_policy": "no-catchup",
            "session_start": self.session_start,
            "frame_send_ts": self.frame_send_ts,
            "frame_kind": self.frame_kind,
            "events": self.events,
            "gap_start_ts": self.gap_start_ts,
            "resume_ts": self.resume_ts,
            "burst_ge4_count": self.burst_ge4_count,
            "max_frame_interval_ms": self.max_frame_interval_ms,
            "rebase_count": self.rebase_count,
            "stop_marker_ts": self.stop_marker_ts,
        }


class MockServer:
    """mock WS 服务端：可被 run_matrix.py 嵌入（推荐，同时基）或独立运行。"""

    def __init__(self, tl: Timeline, host="127.0.0.1", port=0):
        self.tl = tl
        self.host = host
        self.port = port
        self._server = None

    async def handler(self, ws):
        # H-16：TCP_NODELAY（幂等确认；asyncio/Windows 默认已开）
        try:
            sock = ws.transport.get_extra_info("socket")
            if sock is not None:
                import socket as _s
                sock.setsockopt(_s.IPPROTO_TCP, _s.TCP_NODELAY, 1)
        except Exception:  # noqa: BLE001 - best effort
            pass
        try:
            msg = await ws.recv()
            cfg = ('{"type":"audio_config","sampleRate":%d,"channels":%d,'
                   '"format":"pcm16"}' % (SR, CH))
            await ws.send(cfg)
            await self.stream(ws)
        except Exception as e:  # noqa: BLE001 - conn teardown is expected
            self.tl.events.append(("conn_end", time.monotonic(),
                                   type(e).__name__))

    async def stream(self, ws):
        tl = self.tl
        tone_idx = 0          # 每连接独立的 tone 相位（交织样本计数）
        next_due = None
        prev_kind = None
        while True:
            now = time.monotonic()
            if tl.session_start is None:
                tl.session_start = now
            t = now - tl.session_start
            kind, _gidx = tl.phase_at(t)

            if kind in ("gap_nopacket", "stopped", "done"):
                next_due = None
                if kind != prev_kind:
                    if kind == "done":
                        tl.done.set()
                    tl._record_transition(kind, now)
                    prev_kind = kind
                # 对端死亡检测：idle 分支不 send，无法靠 send 抛异常退出；
                # transport 关闭（RST/FIN）即返回，防 wait_closed 挂起。
                if ws.transport.is_closing():
                    return
                await asyncio.sleep(0.02)
                continue

            # 计算帧周期与载荷
            if kind == "tone":
                period = FRAME_MS / 1000.0
                payload = make_tone_frame(tone_idx)
                tone_idx += FRAME_SAMPLES
            elif kind == "gap_silent":
                period = FRAME_MS / 1000.0
                payload = b"\x00\x00" * FRAME_SAMPLES
            elif kind == "slow":
                rate = tl.slow_rate
                if rate >= 4800:
                    period = 0.020
                    n = max(1, round(rate * 0.020))
                else:
                    period = 1.0
                    n = max(1, int(rate))
                payload = make_small_frame(n, tone_idx)
                tone_idx += n
            else:  # pragma: no cover
                await asyncio.sleep(0.02)
                continue

            if next_due is None:
                next_due = now
            delay = next_due - now
            if delay > 0:
                await asyncio.sleep(delay)
            elif delay < -period:
                # 落后 > 1 帧：rebase（不补发追帧）—— 结构性消除突发
                tl.rebase_count += 1
                tl.events.append(("rebase", time.monotonic()))
                next_due = time.monotonic()
            send_ts = time.monotonic()
            # stop_at_gap 检查（在发送前判定，可能恰好落在本帧）
            k2, g2 = tl.phase_at(send_ts - tl.session_start)
            if k2 == "stopped":
                tl._latch_stop(send_ts - tl.session_start, send_ts)
                next_due = None
                prev_kind = "stopped"
                continue
            await ws.send(payload)
            tl.record_frame(kind if kind != "gap_silent" else "silent",
                            send_ts, len(payload) // 2)
            if kind != prev_kind:
                prev_kind = kind
            next_due += period

    async def start(self):
        self._server = await serve(self.handler, self.host, self.port,
                                   ping_interval=None, ping_timeout=None)
        # 实际绑定端口（port=0 时由 OS 分配）
        for sock in self._server.sockets:
            if sock.family.name == "AF_INET":
                self.port = sock.getsockname()[1]
                break
        return self.port

    async def close(self):
        if self._server is not None:
            self._server.close()
            try:
                # idle handler（保持连接不发送）无法靠 send 异常退出，
                # 5s 超时兜底；残留 task 由事件循环 teardown 取消。
                await asyncio.wait_for(self._server.wait_closed(), timeout=5.0)
            except asyncio.TimeoutError:
                pass


async def standalone(args) -> int:
    slow_rate = parse_rate(args.slow_rate) if args.slow_rate else None
    stop_at = None
    if args.stop_at_gap:
        k, s = args.stop_at_gap
        stop_at = (int(k), float(s))
    tl = Timeline(args.cycles, args.gap, args.mode, args.gap_mode,
                  slow_rate, args.hold, stop_at)
    srv = MockServer(tl, args.host, args.port)
    port = await srv.start()
    print(f"[mock] ws://{args.host}:{port}/  cycles={args.cycles} gap={args.gap}s "
          f"mode={args.mode} gap_mode={args.gap_mode} slow={slow_rate} "
          f"hold={args.hold} stop_at_gap={stop_at}", flush=True)
    try:
        if tl.total_seconds() is None:
            await asyncio.Future()
        else:
            await tl.done.wait()
            print(f"[mock] timeline done; holding connection (Ctrl+C to exit)",
                  flush=True)
            await asyncio.Future()
    except (KeyboardInterrupt, asyncio.CancelledError):
        pass
    finally:
        if args.timeline:
            Path(args.timeline).parent.mkdir(parents=True, exist_ok=True)
            Path(args.timeline).write_text(
                json.dumps(tl.to_dict(), indent=1), encoding="utf-8")
            print(f"[mock] timeline -> {args.timeline}", flush=True)
        await srv.close()
    return 0


def parse_rate(s: str):
    """'64' => 64 样本/s；'1/3' => 1/3 实时 = 32000 样本/s。"""
    if "/" in s:
        num, den = s.split("/", 1)
        return int(96000 * float(num) / float(den))
    return float(s)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=59990)
    ap.add_argument("--cycles", type=int, default=1)
    ap.add_argument("--gap", type=float, default=5.0,
                    help="gap 秒数（必须 <15s，否则触发客户端看门狗重连）")
    ap.add_argument("--mode", default="pause", choices=["pause", "reopen"],
                    help="场景标签（mock 行为同构；区别仅在语义与时长标注）")
    ap.add_argument("--gap-mode", default="nopacket",
                    choices=["nopacket", "silent"],
                    help="间隙类型：nopacket=完全不发送（本缺陷路径）；"
                         "silent=持续发送全零帧（SILENT 合成对照，M6）")
    ap.add_argument("--slow-rate", default=None,
                    help="恢复段慢速速率：'64'（样本/s）或 '1/3'（实时比例）")
    ap.add_argument("--hold", type=float, default=None,
                    help="慢速段持续秒数")
    ap.add_argument("--stop-at-gap", nargs=2, metavar=("K", "S"), default=None,
                    help="第 K 个 gap 进行到第 S 秒时置 stop_event 并停发"
                         "（harness 据此优雅停止 SSC，读间隙中期 final: 行）")
    ap.add_argument("--timeline", default=None, help="timeline.json 输出路径")
    args = ap.parse_args()
    return asyncio.run(standalone(args))


if __name__ == "__main__":
    sys.exit(main())
