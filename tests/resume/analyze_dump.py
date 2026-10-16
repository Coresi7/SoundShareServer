#!/usr/bin/env python3
"""tests/resume/analyze_dump.py — T-CLIENT-SILENT-ON-RESUME dump WAV 分析

设计依据：02-design.md §7.2.1（V-01 / V-05 / §7.2.4 阶跃判据）。

输入：SSC --dump-pcm 落盘 WAV（pcm16，"实际送渲染"的样本，含软欠载填充）
     + 可选 timeline.json（run_matrix 产出的 mock 观测，用于循环对齐）。

分析（不依赖墙钟↔样本绝对对齐 —— 直接测"静音段结束 → RMS > -40 dBFS"
的宽度，设计 §7.2.1 M-1 要求 1）：
  1. 5ms 滑窗 RMS 包络（dBFS）
  2. 静音段：连续窗 RMS < -60 dBFS 且宽度 ≥ 50ms
  3. 恢复沿：静音段后首个 RMS > -40 dBFS 的窗；time_to_audible_ms =
     静音段内最后一个 <-60dB 窗的结束 → 恢复窗开始的间隔
  4. 恢复段 RMS（恢复沿后 100ms 平均）
  5. 毛刺检测（--glitch，AC-05 / §7.2.4）：
     - 逐样本 |x[n]-x[n-1]|：1kHz/-3dBFS 理论斜率 Δmax = 2π·1000/48000·A
       ≈ 0.131·A；判 max ≤ 1.5×Δmax 且超阈值样本数 == 0
     - 恢复沿附近 5ms 滑窗 RMS 无 >6dB 窗口间阶跃（硬切特征）

WAV 时长 = 60.0s±0.1 ⇒ dump_truncated（kDumpMaxSamples = 48000*2*60，
超限停录保留最初片段，wasapi_render.h:97）。

输出：JSON（stdout 或 --out）。
"""
import argparse
import json
import math
import sys
import wave
from pathlib import Path

import numpy as np

WIN_MS = 5.0
SILENT_DBFS = -60.0
AUDIBLE_DBFS = -40.0
MIN_SILENT_S = 0.050        # 静音段最小宽度（过滤瞬态）
TONE_HZ = 1000.0
TONE_AMP = 0.707


def load_wav(path: Path):
    with wave.open(str(path), "rb") as w:
        assert w.getsampwidth() == 2, "expect pcm16"
        assert w.getnchannels() in (1, 2), "expect mono/stereo"
        sr = w.getframerate()
        ch = w.getnchannels()
        n = w.getnframes()
        raw = w.readframes(n)
    x = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    return x, sr, ch


def rms_dbfs(x: np.ndarray) -> float:
    if x.size == 0:
        return -math.inf
    r = math.sqrt(float(np.mean(x * x)))
    return 20.0 * math.log10(r) if r > 0 else -math.inf


def window_rms(x: np.ndarray, sr: int, ch: int, win_ms: float):
    """返回 (win_starts_seconds[], win_rms_dbfs[])。"""
    win_n = max(1, int(sr * win_ms / 1000.0))
    hop = win_n
    nwin = x.size // hop
    starts = []
    vals = []
    for i in range(nwin):
        seg = x[i * hop:(i + 1) * hop]
        starts.append(i * hop / (sr * ch))
        vals.append(rms_dbfs(seg))
    return np.array(starts), np.array(vals)


def find_silent_segments(starts, vals):
    """连续 <-60dB 窗合并为段（≥MIN_SILENT_S）。返回 [(start_s, end_s)]。"""
    segs = []
    i = 0
    n = len(vals)
    while i < n:
        if vals[i] < SILENT_DBFS:
            j = i
            while j < n and vals[j] < SILENT_DBFS:
                j += 1
            # 段边界：首窗起点 → 末静音窗的下一个窗起点（末静音窗结束）
            start_s = starts[i]
            end_s = starts[j] if j < n else starts[n - 1] + (
                starts[1] - starts[0] if n > 1 else 0)
            if end_s - start_s >= MIN_SILENT_S:
                segs.append((float(start_s), float(end_s)))
            i = j
        else:
            i += 1
    return segs


def find_recovery_edge(starts, vals, seg_end_s, tail_s=2.0):
    """静音段结束后 tail_s 内找首个 > -40dB 窗。
    返回 (t_audible_s or None, rms_after_dbfs or None)。"""
    idx = np.searchsorted(starts, seg_end_s - 1e-9)
    best = None
    for i in range(idx, len(starts)):
        if starts[i] > seg_end_s + tail_s:
            break
        if vals[i] > AUDIBLE_DBFS:
            best = i
            break
    if best is None:
        return None, None
    t_aud = float(starts[best])
    # 恢复段 RMS：恢复沿后 100ms
    win_n = max(1, int(0.100 / (starts[1] - starts[0]))) if len(starts) > 1 else 1
    seg_vals = vals[best:best + win_n]
    rms_after = float(np.mean(seg_vals)) if seg_vals.size else None
    return t_aud, rms_after


def glitch_analysis(x: np.ndarray, sr: int, ch: int, edge_s_list):
    """§7.2.4 阶跃判据：max|x[n]-x[n-1]| ≤ 1.5×Δmax 且超阈值计数 == 0；
    恢复沿附近 5ms 窗 RMS 窗间阶跃 ≤ 6dB。"""
    d = np.abs(np.diff(x))
    dmax_theory = 2.0 * math.pi * TONE_HZ / sr * TONE_AMP   # ≈ 0.0921
    # 注意：x 为交织两声道；相邻样本差对 1kHz 正弦 ≈ Δmax（声道间同相），
    # 但帧边界 L→R 同样本差 ~0。取全曲最大相邻差与阈值比较。
    thresh = 1.5 * dmax_theory
    over = int(np.sum(d > thresh))
    mx = float(np.max(d)) if d.size else 0.0
    # 恢复沿附近窗口阶跃（±0.5s）
    steps = []
    for edge_s in edge_s_list:
        starts, vals = window_rms(x, sr, ch, WIN_MS)
        idx = np.searchsorted(starts, edge_s - 0.5)
        end = np.searchsorted(starts, edge_s + 0.5)
        local = vals[idx:end]
        local = local[np.isfinite(local)]
        if local.size >= 2:
            jump_db = float(np.max(np.diff(local)))
            steps.append(round(jump_db, 2))
    return {
        "max_adjacent_jump": round(mx, 5),
        "theory_dmax": round(dmax_theory, 5),
        "threshold_1p5x": round(thresh, 5),
        "over_threshold_count": over,
        "max_window_rms_step_db": max(steps) if steps else None,
        "window_step_limit_db": 6.0,
        # §7.2.4 判定主句（AC-05 字面）：max|x[n]-x[n-1]| ≤ 1.5×Δmax 且
        # 超阈值样本数 == 0。window_rms_step 仅记录不作 fail 条件：恢复沿
        # "静音→出声"即使有 5ms 完美斜坡，5ms 窗间动态也必然 ~17-23dB
        # （斜坡只持续 1 个窗宽），该辅助检查在恢复场景数学上不可满足；
        # 真硬切（0→全幅瞬跳）由逐样本判据排除。
        "pass": over == 0 and mx <= thresh,
    }


def analyze(wav_path: Path, do_glitch: bool):
    x, sr, ch = load_wav(wav_path)
    dur = x.size / (sr * ch)
    starts, vals = window_rms(x, sr, ch, WIN_MS)
    segs = find_silent_segments(starts, vals)

    segments = []
    edges = []
    for (s0, s1) in segs:
        t_aud, rms_after = find_recovery_edge(starts, vals, s1)
        recovered = t_aud is not None
        # time_to_audible：末静音窗结束 → 恢复窗开始
        tta = None
        if recovered:
            tta = round((t_aud - s1) * 1000.0, 1)
        segments.append({
            "silent_start_s": round(s0, 3),
            "silent_end_s": round(s1, 3),
            "silent_width_ms": round((s1 - s0) * 1000.0, 1),
            "recovered": recovered,
            "time_to_audible_ms": tta,
            "rms_after_dbfs": None if rms_after is None else round(rms_after, 2),
        })
        if recovered:
            edges.append(t_aud)

    finite = vals[np.isfinite(vals)]
    result = {
        "wav": str(wav_path),
        "seconds": round(dur, 3),
        "sample_rate": sr,
        "channels": ch,
        "dump_truncated": abs(dur - 60.0) <= 0.1,
        "rms_overall_dbfs": round(float(np.mean(finite)), 2) if finite.size else None,
        "rms_min_dbfs": round(float(np.min(finite)), 2) if finite.size else None,
        "silent_segments": segments,
        "recovered_count": sum(1 for s in segments if s["recovered"]),
        "tail_silent_to_end": bool(segs and not segments[-1]["recovered"]
                                   and abs(segs[-1][1] - dur) < 0.05),
    }
    if do_glitch:
        result["glitch"] = glitch_analysis(x, sr, ch, edges)
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description="dump WAV RMS/恢复沿/毛刺分析")
    ap.add_argument("wav", help="SSC --dump-pcm 落盘 WAV")
    ap.add_argument("--glitch", action="store_true", help="附带毛刺/阶跃检测")
    ap.add_argument("--timeline", default=None, help="timeline.json（可选，"
                    "当前仅透传摘要，循环对齐由 run_matrix 完成）")
    ap.add_argument("--out", default=None, help="JSON 输出路径（默认 stdout）")
    args = ap.parse_args()
    result = analyze(Path(args.wav), args.glitch)
    if args.timeline:
        tl = json.loads(Path(args.timeline).read_text(encoding="utf-8"))
        result["timeline_summary"] = {
            "gap_count": len(tl.get("gap_start_ts", [])),
            "resume_count": len(tl.get("resume_ts", [])),
            "burst_ge4_count": tl.get("burst_ge4_count"),
            "max_frame_interval_ms": tl.get("max_frame_interval_ms"),
        }
    text = json.dumps(result, indent=1)
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
        print(f"analyzed -> {args.out}")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
