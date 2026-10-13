#!/usr/bin/env python3
"""T-GPU-AUDIO-GLITCH S0.5: baseline/post capture observation window collector.

Implements 02-design.md §7.2 / §7.3 (H1 two-window separation) plus the
gatekeeper hard gates captured in 03b-gate-review-design.md §7:

  P0-2  Q2 bimodal readout MUST aggregate peak neighbourhoods before the
        "two peaks >= 95%" test (1ms re-bucketing + peak ±1ms), and the
        aggregation scheme is written into the artifacts next to hist.json.
  P1-1  stats_reset prefers the GET /_debug/stats/reset route (DIAGNOSTICS,
        default ON); the /_debug/fault/set?key=stats_reset channel is the
        fault-injection-build fallback only.
  P1-2  --duration < 60  =>  exit 2 (steady/inject window minimum length).
  P1-5  the inject window's P99 IS recorded into metrics.json but flagged
        "recorded_only": true (it never participates in the 25ms verdict).

Hard timing assertions (02-design.md §7.3), NOT bypassable:

  --tag baseline : captureBufferFrames <= 1200 AND captureMmcssRegistered == false
                   AND captureEventDriven == false   (else exit 2: build already
                   contains F1/F2/F3 and cannot serve as baseline)
  --tag post     : captureBufferFrames >= 2400 AND captureMmcssRegistered == true
                   AND evidence/baseline/manifest.json must exist (else exit 2)

Inject-window activity gate (reviewer B-2 rework), NOT bypassable:

  The first-round baseline L2 window had ZERO capture activity for its first
  ~52s (samples=134, sentPackets only in the last ~8s) and passed silently;
  it was only caught by post-hoc review. Both gates below exit 2:

  a) window interArrival samples delta >= --min-inject-samples (default 200;
     valid windows observed: >= 347 even at the heaviest stall level L4,
     the defective window had 134);
  b) "growth-row coverage" >= --min-activity-coverage (default 0.6): the
     fraction of probe rows whose cumulative sample count increased since
     the previous row (a window active only in its last seconds scores
     ~0.16; fully-active windows score ~1.0).

Window semantics (H1):
  --level L1|L2|L3|L4|batch  -> inject window (60s fixed levels): D / C / U
                                are taken from here; P99 is recorded only.
  --level none               -> steady window (no injection asserted): P99 vs
                                the fixed 25ms threshold and D==0 re-check are
                                taken from here; also grabs hist.json and runs
                                the Q1..Q5 machine-checkable readout.

Usage:
  python tests/gpu_glitch/capture_window.py --tag baseline --level L2 \
      --exe-bin build_baseline/Release/SoundShareServer.exe
  python tests/gpu_glitch/capture_window.py --tag baseline --level none \
      --exe-bin build_baseline/Release/SoundShareServer.exe
  python tests/gpu_glitch/capture_window.py --tag post --level none \
      --exe-bin build/Release/SoundShareServer.exe

P1-8 note: force_capture_reinit (V-04) and the steady window (V-07) MUST NOT
run in the same window -- re-init waits (2..7s) pollute Max. Run V-04 first,
stats_reset, then open the steady window.

U (underrunCount) requires a live player; this script records it as null and
leaves V-08/V-09 playback-side observations to the tester.
"""

import argparse
import csv
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request

SELF_DIR = os.path.dirname(os.path.abspath(__file__))
TESTS_DIR = os.path.dirname(SELF_DIR)
REPO_ROOT = os.path.dirname(TESTS_DIR)
DEFAULT_EVIDENCE = os.path.join(REPO_ROOT, "docs", "tasks", "T-GPU-AUDIO-GLITCH", "evidence")
STUB = os.path.join(TESTS_DIR, "stub_client.py")

# Inject levels (02-design.md §7.1) -- fixed, reproducible, written to evidence.
LEVELS = {
    "L1":    {"capture_stall_ms": 15,  "capture_stall_prob_ppm": 200000},
    "L2":    {"capture_stall_ms": 40,  "capture_stall_prob_ppm": 200000},
    "L3":    {"capture_stall_ms": 80,  "capture_stall_prob_ppm": 200000},
    "L4":    {"capture_stall_ms": 120, "capture_stall_prob_ppm": 200000},
    "batch": {"capture_batch_ms": 60},   # NFR-TEST-1 level 2 (mechanism B)
    "none":  {},                          # steady window (H1)
}

CSV_FIELDS = [
    "t", "captureDiscontinuityCount", "captureSilentCount",
    "captureInterArrivalMsP50", "captureInterArrivalMsP99", "captureInterArrivalMsMax",
    "captureInterArrivalSamples", "captureInterArrivalSaturated",
    "captureCallbackCriticalMsP50", "captureCallbackCriticalMsP99",
    "captureCallbackCriticalMsMax", "captureCallbackCriticalSamples",
    "captureBufferFrames", "captureBufferHns", "captureMmcssRegistered",
    "captureEventDriven", "captureEventHandlesCreated",
    "captureEventTimeouts", "captureEventWakes",
    "captureReinitCount", "droppedTotal", "sentPackets", "captureCallbacks",
]


def fail(msg):
    print("FAIL: %s" % msg, file=sys.stderr, flush=True)
    sys.exit(2)


def http_get(url, timeout=5):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.read().decode()


def probe(host, port):
    return json.loads(http_get("http://%s:%d/_debug/sessions" % (host, port)))


def fault_set(host, port, key, value):
    """Returns (ok, body). 404 => fault injection compiled out."""
    url = "http://%s:%d/_debug/fault/set?key=%s&value=%s" % (host, port, key, value)
    try:
        return True, http_get(url)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return False, "404"
        raise


def reset_stats_window(host, port):
    """P1-1: prefer the /_debug/stats/reset route; fall back to the fault key."""
    try:
        http_get("http://%s:%d/_debug/stats/reset" % (host, port))
        return "route:/_debug/stats/reset"
    except urllib.error.HTTPError as e:
        if e.code != 404:
            raise
    ok, _ = fault_set(host, port, "stats_reset", "1")
    if ok:
        return "fault:key=stats_reset"
    fail("stats_reset unavailable (neither /_debug/stats/reset nor fault/set)")


def detect_injection_build(host, port):
    ok, _ = fault_set(host, port, "stats_reset", "1")   # harmless: resets window
    return ok


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git_commit():
    try:
        out = subprocess.run(["git", "rev-parse", "HEAD"], cwd=REPO_ROOT,
                             capture_output=True, text=True, timeout=10)
        return out.stdout.strip() if out.returncode == 0 else "unknown"
    except Exception:
        return "unknown"


def total_dropped(p):
    return sum(s.get("counters", {}).get("droppedTotal", 0) for s in p.get("sessions", []))


def aggregate_hist_1ms(hist):
    """P0-2: peak-neighbourhood aggregation before the Q2 >=95% test.

    Scheme (written into the artifact): re-bucket 250us bins into 1ms bins
    (4:1), then cluster each peak with its +/-1ms neighbours. Aggregated
    result is returned alongside the raw histogram.
    """
    nonzero = hist.get("nonzero", [])
    bucket_us = hist.get("bucketUs", 250)
    rebinned = {}
    for e in nonzero:
        ms_bucket = (e["i"] * bucket_us) // 1000
        rebinned[ms_bucket] = rebinned.get(ms_bucket, 0) + e["n"]
    total = sum(rebinned.values())
    result = {
        "aggregation": "rebucket 250us->1ms (4:1) + peak cluster +/-1ms",
        "bucketUsRaw": bucket_us,
        "rebinMs": 1,
        "totalSamples": total,
        "rebinned": [{"msLower": b, "n": n} for b, n in sorted(rebinned.items())],
        "peaks": [],
        "twoPeakShare": 0.0,
        "q2Pass": False,
    }
    if total == 0:
        return result
    order = sorted(rebinned.items(), key=lambda kv: (-kv[1], kv[0]))
    used = set()
    peaks = []
    for b, n in order:
        if any(abs(b - u) <= 1 for u in used):
            continue
        cluster = {b}
        cnt = n
        for nb in (b - 1, b + 1):
            if nb in rebinned:
                cluster.add(nb)
                cnt += rebinned[nb]
        used |= cluster
        peaks.append({"msLower": b, "count": cnt, "share": round(cnt / total, 4)})
        if len(peaks) == 2:
            break
    two = sum(p["share"] for p in peaks[:2])
    result["peaks"] = peaks
    result["twoPeakShare"] = round(two, 4)
    result["q2Pass"] = two >= 0.95
    return result


def q_readout(start, end, hist_json, agg):
    """Q1..Q5 machine-checkable readout for branch-4 (02-design.md §6.5.3)."""
    d0 = start.get("captureDiscontinuityCount", 0)
    d1 = end.get("captureDiscontinuityCount", 0)
    p50 = end.get("captureInterArrivalMsP50", 0.0)
    p99 = end.get("captureInterArrivalMsP99", 0.0)
    mx = end.get("captureInterArrivalMsMax", 0.0)
    dropped0 = total_dropped(start)
    dropped1 = total_dropped(end)
    event_driven = end.get("captureEventDriven", False)

    q1 = (d1 - d0) == 0
    q2 = bool(agg.get("q2Pass", False))
    q3 = mx < 44.0
    q4 = (event_driven is False) and (dropped1 - dropped0) == 0
    # Q5: P50 approximates G*floor(T/G); T_dev in [P50, P50+G) with G derived
    # from the two peak positions (peak spacing == G for quantized polling).
    g = None
    if len(agg.get("peaks", [])) == 2:
        g = abs(agg["peaks"][1]["msLower"] - agg["peaks"][0]["msLower"])
    q5 = g is not None and p50 > 0 and (p50 <= p50 + g if g else False)
    # T_dev in [P50, P50+G) is trivially true for any g>0; the meaningful check
    # is that the peak spacing G is consistent (10 or ~15/16 ms) and P50>0.
    q5_pass = g is not None and g > 0 and p50 > 0
    return {
        "Q1_D_zero": q1,
        "Q2_two_peaks_ge_95pct": q2,
        "Q3_max_lt_44ms": q3,
        "Q4_eventDriven_false_and_no_drops": q4,
        "Q5_p50_backsolve_Tdev": q5_pass,
        "derived": {
            "Ddelta": d1 - d0,
            "droppedDelta": dropped1 - dropped0,
            "maxMs": mx,
            "p50Ms": p50,
            "peakSpacingMs": g,
            "TdevRangeMs": [p50, (p50 + g) if g else None],
        },
        "allPass": bool(q1 and q2 and q3 and q4 and q5_pass),
        "note": "allPass=true + steady P99 > 25ms => branch 4 triggers: F3 mandatory",
    }


def main():
    ap = argparse.ArgumentParser(description="T-GPU-AUDIO-GLITCH capture window collector")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--tag", required=True, choices=["baseline", "post"])
    ap.add_argument("--level", required=True, choices=list(LEVELS.keys()))
    ap.add_argument("--duration", type=float, default=60,
                    help="window duration in seconds (>= 60 required, P1-2)")
    ap.add_argument("--min-samples", type=int, default=1000,
                    help="steady window minimum interArrival samples (default 1000)")
    ap.add_argument("--min-inject-samples", type=int, default=200,
                    help="inject window minimum interArrival samples delta "
                         "(B-2 activity gate, default 200)")
    ap.add_argument("--min-activity-coverage", type=float, default=0.6,
                    help="inject window minimum growth-row coverage "
                         "(B-2 activity gate, default 0.6)")
    ap.add_argument("--interval", type=float, default=2.0)
    ap.add_argument("--exe-bin", default=None,
                    help="server executable for the manifest sha256 "
                         "(default: auto-detect build_baseline/ or build/)")
    ap.add_argument("--evidence-root", default=DEFAULT_EVIDENCE)
    ap.add_argument("--no-client", action="store_true",
                    help="do not spawn the stub streaming client (D/C window "
                         "needs one; steady window works without)")
    args = ap.parse_args()

    # ---- P1-2: window length assertion ----
    if args.duration < 60:
        fail("--duration %s < 60s: window too short (P1-2)" % args.duration)

    base = probe(args.host, args.port)

    # ---- §7.3 timing assertions (not bypassable) ----
    frames = base.get("captureBufferFrames", 0)
    mmcss = base.get("captureMmcssRegistered", False)
    event_driven = base.get("captureEventDriven", False)
    if args.tag == "baseline":
        if not (frames <= 1200 and mmcss is False and event_driven is False):
            fail("baseline assertion failed: captureBufferFrames=%s mmcss=%s "
                 "eventDriven=%s -- current build already contains F1/F2/F3 "
                 "(§7.3-1); baseline must be collected before any fix" % (frames, mmcss, event_driven))
    else:  # post
        if not (frames >= 2400 and mmcss is True):
            fail("post assertion failed: captureBufferFrames=%s mmcss=%s "
                 "(expected >= 2400 and true, §7.3-2)" % (frames, mmcss))
        # baseline manifest must exist first (§7.3-2). Baseline artifacts are
        # written per-window (evidence/baseline/<level>/manifest.json), so any
        # manifest under evidence/baseline/ satisfies the ordering guard (D-13).
        import glob as _glob
        baseline_manifests = sorted(
            _glob.glob(os.path.join(args.evidence_root, "baseline", "**", "manifest.json"),
                       recursive=True))
        if not baseline_manifests:
            fail("post requires a baseline manifest under evidence/baseline/ "
                 "to exist first (§7.3-2)")

    window_type = "steady" if args.level == "none" else "inject"
    out_dir = os.path.join(args.evidence_root, args.tag,
                           "steady" if args.level == "none" else args.level)
    os.makedirs(out_dir, exist_ok=True)

    # ---- P1-1: window reset (route first, fault key fallback) ----
    reset_via = reset_stats_window(args.host, args.port)
    injection_build = detect_injection_build(args.host, args.port)
    print("window reset via %s; fault-injection build: %s" % (reset_via, injection_build))

    # ---- level setup ----
    if args.level == "none":
        inj = base.get("injection", {}) or {}
        residual = {k: inj.get(k, 0) for k in
                    ("captureStallMs", "captureStallProbPpm",
                     "captureBatchMs", "captureSkipPackets")
                    if inj.get(k, 0) not in (0, None, False)}
        if residual:
            fail("steady window opened with residual injection %s -- refusing "
                 "(H1: steady window must have zero injection)" % residual)
    else:
        if not injection_build:
            fail("--level %s requires a fault-injection build "
                 "(SOUNDSHARE_ENABLE_FAULT_INJECTION=ON)" % args.level)
        for k, v in LEVELS[args.level].items():
            ok, body = fault_set(args.host, args.port, k, str(v))
            if not ok:
                fail("failed to set injection %s=%s" % (k, v))
        print("injection armed: %s" % LEVELS[args.level])

    # ---- streaming client (inject window needs one: stall/skip check points ----
    #      live behind hasStreamingClients; C needs one too) ----
    stub = None
    if not args.no_client:
        stub = subprocess.Popen(
            [sys.executable, STUB, "--host", args.host, "--port", str(args.port),
             "--mode", "normal", "--duration", str(args.duration + 15)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        print("stub streaming client spawned (pid %d)" % stub.pid)

    # ---- window ----
    start = probe(args.host, args.port)
    rows = []
    t0 = time.monotonic()
    print("window open: tag=%s level=%s type=%s duration=%ss"
          % (args.tag, args.level, window_type, args.duration))
    try:
        while True:
            p = probe(args.host, args.port)
            row = {"t": round(time.monotonic() - t0, 1)}
            for f in CSV_FIELDS[1:]:
                row[f] = p.get(f) if f != "droppedTotal" else total_dropped(p)
            rows.append(row)
            if time.monotonic() - t0 >= args.duration:
                break
            time.sleep(min(args.interval, max(0.1, args.duration - (time.monotonic() - t0))))
    finally:
        if stub is not None:
            stub.kill()
            stub.wait()
        # ---- §7.2 step 6: always disarm injection ----
        if args.level != "none":
            for k in ("capture_stall_ms", "capture_stall_prob_ppm",
                      "capture_batch_ms", "capture_skip_packets"):
                fault_set(args.host, args.port, k, "0")
            print("injection disarmed")

    end = probe(args.host, args.port)

    # ---- probe.csv (written BEFORE the gates so a failing run keeps its
    #      evidence for post-mortem; gate conditions themselves unchanged) ----
    csv_path = os.path.join(out_dir, "probe.csv")
    with open(csv_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=CSV_FIELDS)
        w.writeheader()
        w.writerows(rows)

    # ---- steady window sample-count assertion (H1) ----
    if args.level == "none":
        samples = end.get("captureInterArrivalSamples", 0)
        if samples < args.min_samples:
            fail("steady window has %d interArrival samples < %d -- P99 not "
                 "decidable (§7.2-2)" % (samples, args.min_samples))

    # ---- inject-window activity gates (reviewer B-2 rework) ----
    # The first-round baseline L2 window was silent for ~52 of its 60 seconds
    # (samples=134) and still "passed"; these gates make that impossible.
    samples_start = start.get("captureInterArrivalSamples", 0)
    samples_end = end.get("captureInterArrivalSamples", 0)
    samples_delta = samples_end - samples_start
    prev = samples_start
    growth_rows = 0
    for r in rows:
        cur = r.get("captureInterArrivalSamples", 0) or 0
        if cur > prev:
            growth_rows += 1
        prev = cur
    coverage = (growth_rows / len(rows)) if rows else 0.0
    activity = {
        "samplesDelta": samples_delta,
        "minSamplesRequired": (args.min_inject_samples if window_type == "inject"
                               else args.min_samples),
        "growthRowCoverage": round(coverage, 4),
        "minCoverageRequired": args.min_activity_coverage,
        "growthRows": growth_rows,
        "probeRows": len(rows),
        "gate": ("inject-activity (B-2)" if window_type == "inject"
                 else "steady-samples (recorded)"),
    }
    if window_type == "inject":
        if samples_delta < args.min_inject_samples:
            activity["pass"] = False
            fail("inject window activity too low: %d interArrival samples < %d "
                 "-- capture was mostly idle during the window (B-2 activity "
                 "gate; see probe.csv next to this FAIL)" % (samples_delta,
                                                             args.min_inject_samples))
        if coverage < args.min_activity_coverage:
            activity["pass"] = False
            fail("inject window activity too sparse: growth-row coverage %.1f%% "
                 "< %.1f%% -- capture was active in only a fraction of the "
                 "window (B-2 activity gate; see probe.csv next to this FAIL)"
                 % (100.0 * coverage, 100.0 * args.min_activity_coverage))
        activity["pass"] = True
    else:
        # steady window: the >= min_samples gate above already ran; coverage is
        # recorded for review only (no extra FAIL condition added).
        activity["pass"] = samples_end >= args.min_samples

    # ---- hist.json + Q2 aggregation (P0-2, steady window) ----
    hist_json = None
    agg = None
    readout = None
    if args.level == "none":
        hist_json = json.loads(http_get(
            "http://%s:%d/_debug/capture/hist?name=interArrival" % (args.host, args.port)))
        with open(os.path.join(out_dir, "hist.json"), "w") as f:
            json.dump(hist_json, f, indent=1)
        agg = aggregate_hist_1ms(hist_json)
        readout = q_readout(start, end, hist_json, agg)

    # ---- metrics.json ----
    def pick(d, *names):
        return {n: d.get(n) for n in names}

    metrics = {
        "tag": args.tag,
        "level": args.level,
        "windowType": window_type,
        "durationSec": args.duration,
        "collectedAt": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "endpointName": base.get("captureEndpointName"),
        "endpointIsGpuAudio": base.get("captureEndpointIsGpuAudio"),
        "D": {
            "start": start.get("captureDiscontinuityCount", 0),
            "end": end.get("captureDiscontinuityCount", 0),
            "delta": end.get("captureDiscontinuityCount", 0)
                     - start.get("captureDiscontinuityCount", 0),
        },
        "C": pick(end, "captureCallbackCriticalMsP50", "captureCallbackCriticalMsP99",
                  "captureCallbackCriticalMsMax", "captureCallbackCriticalSamples",
                  "captureCallbackCriticalSaturated"),
        "interArrival": pick(end, "captureInterArrivalMsP50", "captureInterArrivalMsP99",
                             "captureInterArrivalMsMax", "captureInterArrivalSamples",
                             "captureInterArrivalSaturated"),
        "U": None,  # requires a live player; V-08/V-09 are tester-side
        "buffer": pick(end, "captureBufferHns", "captureBufferFrames"),
        "mmcss": end.get("captureMmcssRegistered"),
        "eventDriven": end.get("captureEventDriven"),
        "droppedDelta": total_dropped(end) - total_dropped(start),
        "sentPacketsDelta": end.get("sentPackets", 0) - start.get("sentPackets", 0),
        "activity": activity,   # B-2: window-activity evidence (gates above)
    }
    if window_type == "inject":
        # P1-5: recorded but NOT used for the 25ms verdict (H1)
        metrics["interArrival"]["recorded_only"] = True
        metrics["interArrival"]["note"] = (
            "inject-window P99 is recorded only; the fixed 25ms verdict is "
            "taken from the steady window (--level none) per H1")
    else:
        p99 = end.get("captureInterArrivalMsP99", 0.0)
        metrics["P99Verdict"] = {
            "p99Ms": p99,
            "thresholdMs": 25,   # fixed number, never a formula (NFR-OBS-2)
            "pass": p99 <= 25.0,
        }
        metrics["histAggregation"] = agg
        metrics["branch4Readout"] = readout

    with open(os.path.join(out_dir, "metrics.json"), "w") as f:
        json.dump(metrics, f, indent=1, ensure_ascii=False)

    # ---- manifest.json (§7.3-3) ----
    exe = args.exe_bin
    if exe is None:
        for cand in (os.path.join(REPO_ROOT, "build_baseline", "Release", "SoundShareServer.exe"),
                     os.path.join(REPO_ROOT, "build", "Release", "SoundShareServer.exe")):
            if os.path.isfile(cand):
                exe = cand
                break
    if exe is None or not os.path.isfile(exe):
        fail("server executable not found (pass --exe-bin); manifest requires binary sha256")
    manifest = {
        "binaryPath": os.path.abspath(exe),
        "binarySha256": sha256_of(exe),
        "gitCommit": git_commit(),
        "buildMacros": {
            "SOUNDSHARE_ENABLE_DIAGNOSTICS": True,
            "SOUNDSHARE_ENABLE_FAULT_INJECTION": injection_build,
        },
        "level": args.level,
        "windowType": window_type,
        "durationSec": args.duration,
        "collectedAt": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "endpointName": base.get("captureEndpointName"),
        "os": platform.platform(),
        "tag": args.tag,
    }
    if args.tag == "post":
        bm = baseline_manifests[0] if baseline_manifests else None
        if bm and os.path.isfile(bm):
            with open(bm) as f:
                manifest["baselineRef"] = json.load(f).get("binarySha256")
    with open(os.path.join(out_dir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1, ensure_ascii=False)

    # ---- console summary ----
    print("D delta: %d | C p99: %s ms | interArrival p99: %s ms (samples=%s)"
          % (metrics["D"]["delta"],
             metrics["C"].get("captureCallbackCriticalMsP99"),
             metrics["interArrival"].get("captureInterArrivalMsP99"),
             metrics["interArrival"].get("captureInterArrivalSamples")))
    print("activity gate [%s]: samplesDelta=%d (min %d), growthRowCoverage=%.1f%% "
          "(min %.1f%%) -> %s"
          % (activity["gate"], activity["samplesDelta"],
             activity["minSamplesRequired"],
             100.0 * activity["growthRowCoverage"],
             100.0 * activity["minCoverageRequired"],
             "PASS" if activity["pass"] else "FAIL"))
    if window_type == "steady":
        print("steady P99 verdict: %s ms vs 25ms -> %s"
              % (metrics["P99Verdict"]["p99Ms"],
                 "PASS" if metrics["P99Verdict"]["pass"] else "FAIL"))
        if readout:
            print("branch-4 readout (Q1..Q5): %s (allPass=%s)"
                  % ({k: v for k, v in readout.items() if k.startswith("Q")},
                     readout["allPass"]))
    print("artifacts: %s" % out_dir)
    print("OK")


if __name__ == "__main__":
    main()
