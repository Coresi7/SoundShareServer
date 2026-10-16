#!/usr/bin/env python3
"""tests/resume/run_matrix.py — T-CLIENT-SILENT-ON-RESUME 自动化取证 harness

设计依据：02-design.md §7.2（闸门 APPROVED：03b-gate-review-design.md），
硬门禁 H-12 / H-13 / H-14 / H-15 / H-20。

职责：
  1. 同进程嵌入 mock 服务端（import resume_gap）+ spawn SSC —— mock 与
     stdout 读取共享同一 time.monotonic() 时基（M-1①）。
  2. SSC 以 CREATE_NEW_CONSOLE + stdout=PIPE + bufsize=1 启动，独立线程
     逐行读取并打墙钟（M-1① 硬要求；Ctrl+C 优雅关停复用
     tests/forward_forensics.py 的 AttachConsole 序列）。
  3. 会话结束后对 --dump-pcm WAV 调 analyze_dump 分析，按矩阵×变体评估
     判据，落盘 evidence/<tag>/{timeline,metrics,manifest}.json（§7.2.5）。
  4. --v08-summary：对 baseline 侧 M1..M4 汇总判定 V-08 mock 有效性硬前置
     （四条：M2 静音到底 / M3,M4 恢复==0 且 underruns Δ 按 M1 稳态归一 /
     burst_ge4==0 且 max_frame_interval≤40 / manifest 齐备）。

用法：
  python tests/resume/run_matrix.py --ssc build\\Release\\SoundShareClient.exe \\
      --variant baseline --matrix M2 --tag M2-baseline
  python tests/resume/run_matrix.py --ssc ... --variant post --matrix M3 \\
      --tag M3-post --m1-metrics evidence/M1-baseline/metrics.json
  python tests/resume/run_matrix.py --v08-summary --evidence evidence

NOT a CTest suite member — manually driven acceptance script（M-12 约定）。
"""
import argparse
import asyncio
import hashlib
import json
import os
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import resume_gap  # noqa: E402  (same-dir import; mock server + Timeline)
import analyze_dump  # noqa: E402

ROOT = HERE.parent.parent
EVIDENCE_DEFAULT = ROOT / "docs" / "tasks" / "T-CLIENT-SILENT-ON-RESUME" / "evidence"

# ---------------------------------------------------------------------------
# 矩阵定义（02-design.md §7.2.2）
# ---------------------------------------------------------------------------
# duration: mock 时间轴自然时长（None = 无限推流，由 stop_after 控制）
# stop_after: 时间轴完成后 harness 再等待的尾巴秒数（此后 Ctrl+C）
# dump_full_cycles: dump 60s 上限内可完整覆盖的循环数（M3=4 / M4=7）
MATRIX = {
    "M1": dict(cycles=0, gap=0, mode="steady", gap_mode="nopacket",
               slow_rate=None, hold=None, stop_at_gap=None,
               duration=None, stop_after=45.0, dump_full_cycles=0,
               desc="稳态对照（无间隙 45s）"),
    "M2": dict(cycles=1, gap=5, mode="pause", gap_mode="nopacket",
               slow_rate=None, hold=None, stop_at_gap=None,
               duration=8.0, stop_after=1.0, dump_full_cycles=1,
               desc="单次间隙 5s"),
    "M3": dict(cycles=10, gap=10, mode="pause", gap_mode="nopacket",
               slow_rate=None, hold=None, stop_at_gap=None,
               duration=130.0, stop_after=1.0, dump_full_cycles=4,
               desc="10× 暂停 10s/恢复（每循环 13s）"),
    "M4": dict(cycles=10, gap=5, mode="reopen", gap_mode="nopacket",
               slow_rate=None, hold=None, stop_at_gap=None,
               duration=80.0, stop_after=1.0, dump_full_cycles=7,
               desc="10× 关播放器 5s/重开（每循环 8s）"),
    "M5": dict(cycles=1, gap=20, mode="longgap", gap_mode="nopacket",
               slow_rate=None, hold=None, stop_at_gap=None,
               duration=23.0, stop_after=1.0, dump_full_cycles=1,
               desc="长间隙 20s（≥15s 看门狗重连，AC-07）"),
    "M6": dict(cycles=1, gap=5, mode="silentsynth", gap_mode="silent",
               slow_rate=None, hold=None, stop_at_gap=(1, 3.0),
               duration=None, stop_after=0.0, dump_full_cycles=1,
               desc="零帧间隙（SILENT 合成对照），gap 第 3s 优雅停止"),
    "M7": dict(cycles=1, gap=5, mode="pause", gap_mode="nopacket",
               slow_rate="1/3", hold=1.0, stop_at_gap=None,
               duration=9.0, stop_after=1.0, dump_full_cycles=1,
               desc="慢速恢复 1/3 实时 × 1s（可选，N-4）"),
    "M8": dict(cycles=1, gap=5, mode="pause", gap_mode="nopacket",
               slow_rate="64", hold=75.0, stop_at_gap=None,
               duration=83.0, stop_after=1.0, dump_full_cycles=1,
               desc="主判据正向构造：64 样本/s × 75s = 4800 < 5760"),
}

RE_FINAL = re.compile(
    r"\[Audio\]\[STATS\] final: underruns=(\d+) minAvail=(-?\d+) samples(.*)$")
RE_STATS = re.compile(
    r"\[Audio\]\[STATS\] underruns=(\d+) \(\+(\d+)\) minAvail=(-?\d+) "
    r"samples avail=(\d+)(.*)$")
RE_RESUME = re.compile(
    r"\[Audio\]\[RESUME\] (SOFT_UNDERRUN->PREFILL|PREFILL->PLAYING) "
    r"avail=(\d+) underruns=(\d+) t=(\d+)ms")
RE_KV = re.compile(r"\b(\w+)=([-\w.]+)")


def parse_kv(tail: str) -> dict:
    return dict(RE_KV.findall(tail))


# ---------------------------------------------------------------------------
# 优雅关停（复刻 tests/forward_forensics.py send_ctrl_c 的已验证序列）
# ---------------------------------------------------------------------------
def send_ctrl_c(target_pid: int) -> bool:
    import ctypes
    k32 = ctypes.windll.kernel32
    k32.FreeConsole()
    if not k32.AttachConsole(ctypes.c_uint32(target_pid)):
        return False
    k32.SetConsoleCtrlHandler(ctypes.c_void_p(0), True)   # self ignores ^C
    ok = bool(k32.GenerateConsoleCtrlEvent(0, 0))         # CTRL_C_EVENT, group 0
    time.sleep(0.8)
    k32.SetConsoleCtrlHandler(ctypes.c_void_p(0), False)
    k32.FreeConsole()
    return ok


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git_info() -> dict:
    def run(cmd):
        try:
            return subprocess.run(cmd, capture_output=True, text=True,
                                  cwd=str(ROOT), timeout=30).stdout.strip()
        except Exception:  # noqa: BLE001
            return ""
    commit = run(["git", "rev-parse", "HEAD"])
    dirty = subprocess.run(["git", "diff", "--quiet", "HEAD"], cwd=str(ROOT),
                           capture_output=True).returncode != 0
    return {"git_commit": commit, "dirty_tracked": dirty}


# ---------------------------------------------------------------------------
# stdout 解析
# ---------------------------------------------------------------------------
class StdoutLog:
    def __init__(self):
        self.lines = []          # [(monotonic_ts, line)]

    def text(self):
        return "\n".join(l for _, l in self.lines)

    def first_ts(self, needle):
        for ts, line in self.lines:
            if needle in line:
                return ts
        return None

    def all_matching(self, pattern):
        return [(ts, line) for ts, line in self.lines if pattern.search(line)]


def parse_stdout(log: StdoutLog):
    finals = log.all_matching(RE_FINAL)
    stats = log.all_matching(RE_STATS)
    resumes = [(ts, RE_RESUME.search(line).group(1),
                parse_kv(line[line.index("avail="):]))
               for ts, line in log.all_matching(RE_RESUME)]
    return {
        "finals": finals,
        "stats": stats,
        "resumes": resumes,
        "playing_ts": [ts for ts, line in log.lines if "Playing audio" in line],
        "stall_lines": [line for _, line in log.lines
                        if "audio stall" in line],
        "lost_lines": [line for _, line in log.lines
                       if "Connection lost" in line],
        "render_exit_lines": [line for _, line in log.lines
                              if "E-CLIENT-RENDER-EXIT" in line
                              or "render thread exited" in line],
    }


def final_readings(parsed):
    """取第一次 final: 行（Stop() 语义时刻；析构二次打印与首次数值相同，
    S0 实验已证——wasapi_render.cpp:305 与析构 Stop 均打印）。"""
    if not parsed["finals"]:
        return None
    ts, line = parsed["finals"][0]
    m = RE_FINAL.search(line)
    kv = parse_kv(m.group(3))
    return {
        "ts": ts,
        "underruns": int(m.group(1)),
        "minAvail": int(m.group(2)),
        "state": kv.get("state"),
        "env": kv.get("env"),
        "availPeak": int(kv["availPeak"]) if "availPeak" in kv else None,
    }


def stats_readings(parsed):
    out = []
    for ts, line in parsed["stats"]:
        m = RE_STATS.search(line)
        if not m:
            continue
        kv = parse_kv(m.group(5))
        out.append({
            "ts": ts, "underruns": int(m.group(1)), "delta": int(m.group(2)),
            "minAvail": int(m.group(3)), "avail": int(m.group(4)),
            "t_ms": int(kv["t"][:-2]) if "t" in kv else None,
            "state": kv.get("state"), "env": kv.get("env"),
            "availPeak": int(kv["availPeak"]) if "availPeak" in kv else None,
        })
    return out


def pair_resumes(resumes):
    """RESUME 行配对：每个 PREFILL->PLAYING 行与它之前最近（≤2s）的
    SOFT_UNDERRUN->PREFILL 行组成一事件。返回 [(su_pf_ts, pf_pl_ts)]。"""
    pairs = []
    last_su = None
    for ts, kind, _kv in resumes:
        if kind == "SOFT_UNDERRUN->PREFILL":
            last_su = ts
        else:
            if last_su is not None and ts - last_su <= 2.0:
                pairs.append((last_su, ts))
            last_su = None
    return pairs


# ---------------------------------------------------------------------------
# 单次矩阵运行
# ---------------------------------------------------------------------------
async def run_once(args, mcfg, tag_dir: Path) -> dict:
    # git/sha256 等子进程调用必须在 send_ctrl_c（FreeConsole 会破坏
    # 本进程标准句柄继承）之前完成 —— 在 spawn 前采集。
    gi = git_info()
    ssc_path = Path(args.ssc).resolve()
    bin_info = {
        "binary_path": str(ssc_path),
        "binary_sha256": sha256_file(ssc_path),
        "git_commit": gi["git_commit"],
        "dirty_tracked": gi["dirty_tracked"],
        "build_dir": str(ssc_path.parent.parent),
        "build_config": args.build_config,
    }
    tl = resume_gap.Timeline(
        cycles=mcfg["cycles"], gap=mcfg["gap"], mode=mcfg["mode"],
        gap_mode=mcfg["gap_mode"],
        slow_rate=resume_gap.parse_rate(mcfg["slow_rate"])
        if mcfg["slow_rate"] else None,
        hold=mcfg["hold"], stop_at_gap=mcfg["stop_at_gap"])
    srv = resume_gap.MockServer(tl, host="127.0.0.1", port=0)
    port = await srv.start()

    dump_path = tag_dir / "dump.wav"
    if dump_path.exists():
        dump_path.unlink()
    cmdline = [str(args.ssc), "127.0.0.1", str(port),
               "--dump-pcm=" + str(dump_path)]
    log = StdoutLog()
    t_spawn = time.monotonic()
    proc = subprocess.Popen(
        cmdline, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace", bufsize=1,
        creationflags=subprocess.CREATE_NEW_CONSOLE)

    def reader():
        for line in proc.stdout:
            log.lines.append((time.monotonic(), line.rstrip()))

    threading.Thread(target=reader, daemon=True).start()

    # ---- 会话进行 ----
    crashed_early = None
    hard_limit = (mcfg["duration"] or 60.0) + mcfg["stop_after"] + 90.0
    deadline = time.monotonic() + hard_limit
    last_beat = time.monotonic()
    try:
        if mcfg["stop_at_gap"] is not None:
            while not tl.stop_event.is_set() and time.monotonic() < deadline:
                if proc.poll() is not None:
                    crashed_early = proc.returncode
                    break
                now = time.monotonic()
                if now - last_beat >= 10.0:
                    print(f"[wait] {now - t_spawn:6.1f}s stop_at_gap pending "
                          f"(frames={len(tl.frame_send_ts)})", flush=True)
                    last_beat = now
                await asyncio.sleep(0.05)
        elif mcfg["duration"] is None:
            # M1：无限推流 stop_after 秒后停止
            while time.monotonic() - t_spawn < mcfg["stop_after"]:
                if proc.poll() is not None:
                    crashed_early = proc.returncode
                    break
                now = time.monotonic()
                if now - last_beat >= 10.0:
                    print(f"[wait] {now - t_spawn:6.1f}s steady streaming "
                          f"(frames={len(tl.frame_send_ts)})", flush=True)
                    last_beat = now
                await asyncio.sleep(0.05)
        else:
            while not tl.done.is_set() and time.monotonic() < deadline:
                if proc.poll() is not None:
                    crashed_early = proc.returncode
                    break
                now = time.monotonic()
                if now - last_beat >= 10.0:
                    print(f"[wait] {now - t_spawn:6.1f}s timeline pending "
                          f"(frames={len(tl.frame_send_ts)} "
                          f"gaps={len(tl.gap_start_ts)})", flush=True)
                    last_beat = now
                await asyncio.sleep(0.05)
            tail_end = time.monotonic() + mcfg["stop_after"]
            while time.monotonic() < tail_end:
                if proc.poll() is not None:
                    crashed_early = proc.returncode
                    break
                await asyncio.sleep(0.05)
    finally:
        pass

    # ---- 优雅关停 ----
    t_ctrlc = time.monotonic()
    ctrlc_ok = send_ctrl_c(proc.pid) if proc.poll() is None else True
    got_final = False
    for _ in range(300):                       # ≤15s 等 final 行
        if any("final:" in line for _, line in log.lines):
            got_final = True
            break
        if proc.poll() is not None:
            break
        await asyncio.sleep(0.05)
    try:
        proc.wait(timeout=10)
        exit_code, graceful = proc.returncode, True
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        exit_code, graceful = proc.returncode, False
    await srv.close()
    t_end = time.monotonic()

    # ---- 解析 ----
    parsed = parse_stdout(log)
    final = final_readings(parsed)
    stats = stats_readings(parsed)
    pairs = pair_resumes(parsed["resumes"])
    dump = None
    if dump_path.exists():
        dump = analyze_dump.analyze(dump_path, do_glitch=True)

    # 每循环时延（t_resume_line - t_mock_resume）
    per_cycle_latency = []
    pf_ts_list = [pf for _, pf in pairs]
    for i, pf_ts in enumerate(pf_ts_list):
        if i < len(tl.resume_ts):
            per_cycle_latency.append(round((pf_ts - tl.resume_ts[i]) * 1000.0, 1))

    # dump 覆盖的完整循环（M3 N=4 / M4 N=7）
    full_cycles = None
    if dump and mcfg["cycles"] > 0:
        cycle_len = 1.5 + mcfg["gap"] + 1.5
        # dump 只记 PLAYING 起的样本；静音段 start 判定按时间轴对齐：
        # 第 i 个 gap 的静音段起点 ≈ i*cycle_len + 1.5 + ~0.02（淡出）
        n_full = 0
        for i in range(mcfg["cycles"]):
            seg_end_bound = (i + 1) * cycle_len          # 循环末
            if seg_end_bound <= 59.0:
                n_full += 1
            else:
                break
        full_cycles = n_full
        dump["full_cycles_within_limit"] = n_full

    session_playing_span = None
    if parsed["playing_ts"]:
        session_playing_span = round(t_ctrlc - parsed["playing_ts"][0], 3)

    # 日志量（M-4③ / H-20）
    resume_event_count = len(pairs)
    resume_lines_total = len(parsed["resumes"])
    resume_lines_per_event = (round(resume_lines_total / resume_event_count, 2)
                              if resume_event_count else 0.0)
    ss_span = (t_ctrlc - t_spawn)
    steady_state_lines_per_sec = round(resume_lines_total / ss_span, 4) \
        if mcfg["matrix"] == "M1" else None

    metrics = {
        "matrix": mcfg["matrix"],
        "variant": args.variant,
        "desc": mcfg["desc"],
        "cmdline": cmdline,
        "spawn_at": t_spawn,
        "ctrlc_at": t_ctrlc,
        "end_at": t_end,
        "session_seconds": round(ss_span, 3),
        "session_playing_span_s": session_playing_span,
        "exit_code": exit_code,
        "graceful_exit": graceful,
        "ctrlc_sent": ctrlc_ok,
        "got_final_line": got_final,
        "crashed_early": crashed_early,
        "final": final,
        "stats": stats,
        "stats_lines_total": len(parsed["stats"]),
        "resume_pairs": resume_event_count,
        "resume_lines_total": resume_lines_total,
        "resume_lines_per_event": resume_lines_per_event,
        "steady_state_lines_per_sec": steady_state_lines_per_sec,
        "per_cycle_latency_ms": per_cycle_latency,
        "expected_gap_count": mcfg["cycles"] if mcfg["cycles"] > 0 else 0,
        "mock_gap_count": len(tl.gap_start_ts),
        "mock_resume_count": len(tl.resume_ts),
        "burst_ge4_count": tl.burst_ge4_count,
        "max_frame_interval_ms": tl.max_frame_interval_ms,
        "rebase_count": tl.rebase_count,
        "playing_count": len(parsed["playing_ts"]),
        "stall_lines": len(parsed["stall_lines"]),
        "connection_lost_lines": len(parsed["lost_lines"]),
        "render_exit_lines": len(parsed["render_exit_lines"]),
        "dump": dump,
    }

    # underruns 归一（H-15：相对 M1 稳态的增量）
    m1 = None
    if args.m1_metrics:
        m1 = json.loads(Path(args.m1_metrics).read_text(encoding="utf-8")) \
            .get("final", {}) or {}
    m1_underruns = m1.get("underruns") if m1 else None
    metrics["underruns_final"] = final["underruns"] if final else None
    metrics["m1_final_underruns"] = m1_underruns
    metrics["underruns_delta_rel_m1"] = (
        final["underruns"] - m1_underruns
        if final and m1_underruns is not None else None)
    metrics["final_minAvail"] = final["minAvail"] if final else None

    # M1 minAvail 一致性断言（D-04 约束 ii / H-20）
    if args.compare_minavail:
        base = json.loads(Path(args.compare_minavail).read_text(
            encoding="utf-8")).get("final", {}) or {}
        metrics["minavail_consistent_with_baseline"] = (
            base.get("minAvail") == metrics["final_minAvail"])
        metrics["baseline_minAvail_ref"] = base.get("minAvail")

    # ---- timeline / dump 落盘 ----
    tl_dict = tl.to_dict()
    tl_dict["stdout"] = [[ts, line] for ts, line in log.lines]
    timeline_path = tag_dir / "timeline.json"
    timeline_path.write_text(json.dumps(tl_dict, indent=1), encoding="utf-8")
    metrics_path = tag_dir / "metrics.json"

    # ---- manifest（§7.2.5 / H-12）----
    manifest = {
        "baseline": bin_info if args.variant == "baseline" else None,
        "post": bin_info if args.variant == "post" else None,
        "mock": {
            "script_sha256": sha256_file(HERE / "resume_gap.py"),
            "mock_version": resume_gap.MOCK_VERSION,
            "cycles": mcfg["cycles"], "gap": mcfg["gap"], "mode": mcfg["mode"],
            "gap_mode": mcfg["gap_mode"], "frame_ms": resume_gap.FRAME_MS,
            "amplitude_hz": resume_gap.TONE_HZ, "tcp_nodelay": True,
            "rebase_policy": "no-catchup",
            "slow_rate": mcfg["slow_rate"], "hold": mcfg["hold"],
            "stop_at_gap": mcfg["stop_at_gap"],
        },
        "ssc": {
            "cmdline": cmdline,
            "dump_path": str(dump_path),
            "dump_truncated": bool(dump and dump["dump_truncated"]),
            "dump_seconds": dump["seconds"] if dump else 0,
        },
        "timeline": {"timeline_sha256": None},   # filled below
    }
    manifest["timeline"]["timeline_sha256"] = sha256_file(timeline_path)
    (tag_dir / "manifest.json").write_text(json.dumps(manifest, indent=1),
                                           encoding="utf-8")
    metrics_path.write_text(json.dumps(metrics, indent=1), encoding="utf-8")
    return metrics


# ---------------------------------------------------------------------------
# 判据评估（V-01..V-08 / H-14 / H-15 / H-24 分轨）
# ---------------------------------------------------------------------------
def evaluate(m: dict) -> dict:
    mx, var = m["matrix"], m["variant"]
    checks = []

    def ck(name, ok, detail=""):
        checks.append({"check": name, "pass": bool(ok), "detail": str(detail)})

    # 通用：会话完整
    ck("session-complete", m["playing_count"] >= 1 and m["crashed_early"] is None,
       f"playing={m['playing_count']} crashed={m['crashed_early']}")
    ck("no-render-exit", m["render_exit_lines"] == 0)
    ck("mock-burst-free", m["burst_ge4_count"] == 0,
       f"burst_ge4={m['burst_ge4_count']}")
    ck("mock-frame-interval", m["max_frame_interval_ms"] <= 40.0,
       f"max_interval={m['max_frame_interval_ms']}ms")

    dump = m.get("dump")

    if mx == "M1":
        ck("steady-rms", dump is not None and dump["rms_overall_dbfs"] > -40,
           f"rms={dump['rms_overall_dbfs'] if dump else None}")
        ck("log-rate-steady", m["steady_state_lines_per_sec"] in (None, 0.0)
           or m["underruns_final"] in (None, 0),
           f"lines/s={m['steady_state_lines_per_sec']} "
           f"underruns={m['underruns_final']} (口径：仅无欠载稳态要求 0)")
        ck("resume-lines-per-event", m["resume_lines_per_event"] <= 2.0,
           f"{m['resume_lines_per_event']}")
        if "minavail_consistent_with_baseline" in m:
            ck("minavail-consistent-baseline-post",
               m["minavail_consistent_with_baseline"] is True,
               f"baseline={m.get('baseline_minAvail_ref')} "
               f"post={m['final_minAvail']}")

    elif mx == "M2":
        if var == "post":
            # V-01：≤500ms 恢复 + 配对 + RMS + 无断连
            lat = m["per_cycle_latency_ms"]
            ck("resume-pair-present", m["resume_pairs"] == 1,
               f"pairs={m['resume_pairs']}")
            ck("latency-500ms", lat and lat[0] <= 500.0,
               f"lat={lat}")
            ck("dump-recovered-rms",
               dump and dump["recovered_count"] >= 1
               and any(s["recovered"] and s["rms_after_dbfs"] > -40
                       for s in dump["silent_segments"]),
               f"recovered={dump['recovered_count'] if dump else None}")
            ck("no-stall-no-lost", m["stall_lines"] == 0
               and m["connection_lost_lines"] == 0)
            if dump and dump.get("glitch"):
                ck("glitch-free", dump["glitch"]["pass"],
                   dump["glitch"]["max_adjacent_jump"])
        else:
            # baseline：V-08① —— V-06 短会话变体四条
            ck("server-streaming", m["mock_resume_count"] >= 1,
               f"mock_resume={m['mock_resume_count']}")
            ck("underruns-ge1", (m["underruns_final"] or 0) >= 1,
               f"underruns={m['underruns_final']}")
            ck("silent-to-end", dump is not None and dump["recovered_count"] == 0
               and dump["tail_silent_to_end"],
               f"recovered={dump['recovered_count'] if dump else None} "
               f"tail_silent={dump['tail_silent_to_end'] if dump else None}")
            span = m["session_playing_span_s"] or 0
            ck("dump-span-0.9x", dump is not None
               and dump["seconds"] >= 0.9 * span,
               f"dump={dump['seconds'] if dump else None}s span={span}s")

    elif mx in ("M3", "M4"):
        lat = m["per_cycle_latency_ms"]
        n_full = (dump or {}).get("full_cycles_within_limit", 0)
        if var == "post":
            # H-14：RESUME 对数==10 ∧ 每次 ≤500ms ∧ 配套行 ∧ dump 前 N 次
            # RMS ∧ underruns Δ(rel M1)==10±1 ∧ 无累积性失效
            ck("resume-pairs-10", m["resume_pairs"] == m["expected_gap_count"],
               f"pairs={m['resume_pairs']}/{m['expected_gap_count']}")
            ck("latency-500ms-all", len(lat) == m["expected_gap_count"]
               and all(v <= 500.0 for v in lat),
               f"lat={lat}")
            ck("underruns-delta-10", m["underruns_delta_rel_m1"] is not None
               and abs(m["underruns_delta_rel_m1"] - 10) <= 1,
               f"rel={m['underruns_delta_rel_m1']}")
            if dump and n_full:
                segs_in = [s for s in dump["silent_segments"]
                           if s["recovered"]][:n_full]
                ck("dump-full-cycles-recovered", len(segs_in) >= n_full,
                   f"{len(segs_in)}/{n_full}")
            else:
                ck("dump-full-cycles-recovered", True, "no dump coverage")
            if len(lat) >= 10:
                med = lambda v: sorted(v)[len(v) // 2]  # noqa: E731
                drift = med(lat[5:]) - med(lat[:5])
                ck("no-cumulative-degradation", drift <= 50.0,
                   f"drift={drift:.1f}ms")
            ck("no-stall-no-lost", m["stall_lines"] == 0
               and m["connection_lost_lines"] == 0)
        else:
            # baseline：V-08② —— 恢复==0（rel==1 归一）+ dump 覆盖段静音
            ck("resume-pairs-0", m["resume_pairs"] == 0,
               f"pairs={m['resume_pairs']}")
            ck("underruns-rel-1", m["underruns_delta_rel_m1"] == 1,
               f"rel={m['underruns_delta_rel_m1']}")
            ck("dump-covered-silent",
               dump is None or dump["recovered_count"] == 0,
               f"recovered={dump['recovered_count'] if dump else None}")
            ck("no-stall-no-lost", m["stall_lines"] == 0
               and m["connection_lost_lines"] == 0)

    elif mx == "M5":
        # V-07（两变体同判据）：stall 单行 + 重连后 Playing 重现 + dump RMS
        ck("stall-line-once", m["stall_lines"] == 1,
           f"stall={m['stall_lines']}")
        ck("reconnect-playing", m["playing_count"] >= 2,
           f"playing={m['playing_count']}")
        ck("dump-recovered-rms", dump is not None
           and dump["rms_overall_dbfs"] > -40,
           f"rms={dump['rms_overall_dbfs'] if dump else None}")

    elif mx == "M6":
        # 两变体：间隙中期 final —— post state=PLAYING / baseline underruns==0
        if var == "post":
            ck("final-state-playing",
               m["final"] and m["final"]["state"] == "PLAYING",
               f"state={m['final']['state'] if m['final'] else None}")
        else:
            ck("final-underruns-0",
               m["final"] and m["final"]["underruns"] == 0,
               f"underruns={m['final']['underruns'] if m['final'] else None}")
        ck("dump-has-tone-then-silent", dump is not None
           and dump["recovered_count"] == 0
           and dump["rms_overall_dbfs"] > -60,
           f"rms={dump['rms_overall_dbfs'] if dump else None}")

    elif mx == "M7":
        if var == "post":
            lat = m["per_cycle_latency_ms"]
            ck("slow-recover-500ms", lat and lat[0] <= 500.0, f"lat={lat}")
            ck("resume-pair-present", m["resume_pairs"] >= 1)
        else:
            ck("no-recovery", m["resume_pairs"] == 0,
               f"pairs={m['resume_pairs']}")

    elif mx == "M8":
        if var == "post":
            # H-23：连续 ≥2 条 state=SOFT_UNDERRUN ∧ env=0.000 ∧ availPeak>0
            soft = [s for s in m["stats"]
                    if s["state"] == "SOFT_UNDERRUN"
                    and s["env"] in ("0.000", "0.0")
                    and (s["availPeak"] or 0) > 0]
            ck("main-criterion-2-stats", len(soft) >= 2,
               f"soft-stats={len(soft)}")
            ck("final-stays-soft",
               m["final"] and m["final"]["state"] == "SOFT_UNDERRUN"
               and m["final"]["availPeak"] and m["final"]["availPeak"] > 0,
               f"final={m['final']}")
        else:
            ck("m8-baseline-na", True, "baseline 走 M2/M3 替代判据（H-24）")

    passed = all(c["pass"] for c in checks)
    return {"verdict": "PASS" if passed else "FAIL", "checks": checks}


# ---------------------------------------------------------------------------
# V-08 汇总（baseline 侧硬前置门禁）
# ---------------------------------------------------------------------------
def v08_summary(evidence: Path) -> int:
    def load(tag):
        p = evidence / tag / "metrics.json"
        return json.loads(p.read_text(encoding="utf-8")) if p.exists() else None
    m1, m2, m3, m4 = (load("M1-baseline"), load("M2-baseline"),
                      load("M3-baseline"), load("M4-baseline"))
    checks = []

    def ck(name, ok, detail=""):
        checks.append({"check": name, "pass": bool(ok), "detail": str(detail)})

    ck("all-runs-present", all(x for x in (m1, m2, m3, m4)),
       "M1/M2/M3/M4 baseline metrics 齐备")
    if not all(x for x in (m1, m2, m3, m4)):
        out = {"verdict": "INVALID", "checks": checks}
        print(json.dumps(out, indent=1))
        (evidence / "v08-summary.json").write_text(json.dumps(out, indent=1),
                                                   encoding="utf-8")
        return 1
    # ① M2 静音到底（V-06 短会话变体四条由 M2-baseline 判据已逐条检查）
    m2_eval = evaluate(m2)
    ck("V08-1-m2-silent", all(c["pass"] for c in m2_eval["checks"]
                              if c["check"] != "mock-frame-interval"))
    # ② M3/M4 恢复 == 0（rel 归一）
    for tag, mm in (("M3", m3), ("M4", m4)):
        ck(f"V08-2-{tag}-zero-recovery",
           mm["resume_pairs"] == 0
           and mm.get("underruns_delta_rel_m1") == 1,
           f"pairs={mm['resume_pairs']} "
           f"rel={mm.get('underruns_delta_rel_m1')}")
        ck(f"V08-2-{tag}-dump-silent",
           (mm.get("dump") or {}).get("recovered_count", 0) == 0)
    # ③ burst 与帧间隔
    for tag, mm in (("M2", m2), ("M3", m3), ("M4", m4)):
        ck(f"V08-3-{tag}-burst0-interval40",
           mm["burst_ge4_count"] == 0 and mm["max_frame_interval_ms"] <= 40.0,
           f"burst={mm['burst_ge4_count']} "
           f"interval={mm['max_frame_interval_ms']}")
    # ④ manifest 齐备
    for tag in ("M1-baseline", "M2-baseline", "M3-baseline", "M4-baseline"):
        mf = evidence / tag / "manifest.json"
        ok = False
        if mf.exists():
            man = json.loads(mf.read_text(encoding="utf-8"))
            b = man.get("baseline") or {}
            ok = all(b.get(k) for k in
                     ("binary_sha256", "git_commit", "build_dir",
                      "build_config")) and man.get("mock", {}).get("script_sha256")
        ck(f"V08-4-{tag}-manifest", ok)

    passed = all(c["pass"] for c in checks)
    out = {"verdict": "PASS" if passed else "INVALID", "checks": checks}
    print(json.dumps(out, indent=1))
    (evidence / "v08-summary.json").write_text(json.dumps(out, indent=1),
                                               encoding="utf-8")
    return 0 if passed else 1


# ---------------------------------------------------------------------------
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--ssc", help="SoundShareClient.exe 路径")
    ap.add_argument("--variant", choices=["baseline", "post"], default="post")
    ap.add_argument("--matrix", choices=sorted(MATRIX.keys()))
    ap.add_argument("--tag", help="evidence/<tag>/ 目录名（默认 <MX>-<variant>）")
    ap.add_argument("--outdir", default=str(EVIDENCE_DEFAULT))
    ap.add_argument("--build-config", default="Release")
    ap.add_argument("--m1-metrics", default=None,
                    help="M1 同变体 metrics.json（M3/M4 的 underruns Δ 归一）")
    ap.add_argument("--compare-minavail", default=None,
                    help="baseline M1 metrics.json（post M1 的 minAvail 一致性断言）")
    ap.add_argument("--v08-summary", action="store_true")
    ap.add_argument("--evidence", default=None,
                    help="--v08-summary 时的 evidence 目录")
    args = ap.parse_args()

    if args.v08_summary:
        return v08_summary(Path(args.evidence or EVIDENCE_DEFAULT))
    if not args.ssc or not args.matrix:
        ap.error("--ssc 与 --matrix 必填（或使用 --v08-summary）")

    mcfg = dict(MATRIX[args.matrix])
    mcfg["matrix"] = args.matrix
    tag = args.tag or f"{args.matrix}-{args.variant}"
    tag_dir = Path(args.outdir) / tag
    tag_dir.mkdir(parents=True, exist_ok=True)
    print(f"[run] matrix={args.matrix} variant={args.variant} tag={tag} "
          f"desc={mcfg['desc']}", flush=True)
    metrics = asyncio.run(run_once(args, mcfg, tag_dir))
    result = evaluate(metrics)
    print(json.dumps(result, indent=1), flush=True)
    # verdict 回写 metrics.json
    mp = tag_dir / "metrics.json"
    metrics["verdict"] = result["verdict"]
    metrics["checks"] = result["checks"]
    mp.write_text(json.dumps(metrics, indent=1), encoding="utf-8")
    print(f"[run] verdict={result['verdict']}  evidence -> {tag_dir}", flush=True)
    return 0 if result["verdict"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
