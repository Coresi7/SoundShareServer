#!/usr/bin/env python3
"""T-CAPTURE-IDLE-LOG: idle-state log governance acceptance script (12th test).

Implements 02-design.md §3.6 plus the O-D1 verdict frozen in
03b-gate-review-design.md §1 (see board.md "O-D1 裁定结果" and
"developer 硬门禁 G-1~G-15"). NOT a CTest suite member.

Windows console stdout/stderr under pipes are fully buffered; the server
flushes every governed line with std::endl, so line timestamps taken at read
time are reliable (m13). Recovery lines go to stdout, timeout lines to
stderr: both streams are captured and MERGED for counting (dual-path, V-02).

Hard gates implemented here (each failing => exit 2):

  G-1  --mode natural is REDEFINED as inject-idle = capture_idle_sim=1
        (injection ON) + NO loop_play (no playback); the 60s window counts
        FROM the server spawn ("W-A'", the single anchor of the
        ">=6 lines / delta timeouts ~=300" comparison).
  G-2  V-A1 (baseline/s0v, W-A'): timeout lines >= 6 AND delta
        captureEventTimeouts in [250,350].
  G-3  V-A2 (post, W-A'): timeout lines <= 1 AND delta
        captureIdleEpisodes >= 1.
  G-4  V-A3 (W-B): comparison = (baseline_lines >= 1) ?
        (post <= 1 AND post <= baseline) : "N/A (vacuous)".
        "N/A" is never counted as PASS. "Any one of the two windows
        passes" is FORBIDDEN (conjunction, not disjunction).
  G-5  V-23 is measured in S0v BEFORE baseline; the anchor stays W-A'
        regardless of what V-23 sees (frozen verdict file:
        evidence/s0v/v23-frozen.json).
  G-6  The recovery line's `timeouts` field is expected ~=0 in the inject
        window; the ~300 figure describes natural idling, NOT a criterion.
  G-7  loop_play starts BEFORE the server; the >=5s pre-inject sampling
        must assert captureIdleEpisodes == 0 AND captureTotalIdleMs == 0
        (startup idle pollution would fake-fail AC-FR02-1).
  G-8  The probe.py --assert-* chain is executed at least once (episode
        over, BEFORE stats_reset) and must exit 0.

P1-2 observation order (step 9, hard-coded, not configurable):
  episode ends -> read probe (AC-FR02-1 + G-8 chain) -> stats_reset
  -> open the AC-FR03 window.

Usage:
  python tests/idle_log.py --tag baseline --mode inject-idle \
      --exe-bin build_idle_s0/Release/SoundShareServer.exe
  python tests/idle_log.py --tag post --mode inject \
      --exe-bin build/Release/SoundShareServer.exe
  python tests/idle_log.py --tag post --mode active \
      --exe-bin build/Release/SoundShareServer.exe

--tag s0v is the S0 verification pass (V-22/V-23) that MUST run before the
baseline is collected (G-5/G-15); it records but does not apply the FR AC
verdicts (S0 carries no idle logic yet).
"""

import argparse
import csv
import glob
import hashlib
import json
import os
import platform
import re
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

SELF_DIR = os.path.dirname(os.path.abspath(__file__))   # .../tests
TESTS_DIR = SELF_DIR
REPO_ROOT = os.path.dirname(TESTS_DIR)
DEFAULT_EVIDENCE = os.path.join(REPO_ROOT, "docs", "tasks",
                                "T-CAPTURE-IDLE-LOG", "evidence")
LOOP_PLAY = os.path.join(TESTS_DIR, "gpu_glitch", "loop_play.py")
DEFAULT_CLIP = os.path.join(REPO_ROOT, "docs", "tasks", "T-GPU-AUDIO-GLITCH",
                            "evidence", "clip", "eval-30s.wav")
STUB = os.path.join(TESTS_DIR, "stub_client.py")
PROBE = os.path.join(TESTS_DIR, "probe.py")

RE_TIMEOUT = re.compile(
    r"^\[WASAPI\] Capture event timeout #(\d+) \(continuing with defensive drain\)$")
RE_RECOVERY = re.compile(
    r"^\[WASAPI\] Capture idle ended: ep=(\d+) idleMs=(\d+) totalIdleMs=(\d+) timeouts=(\d+)$")

# AC-FR02-1 numeric window (ms): 60s inject +/- 2.0s entry + jitter (m1).
LO_IDLE_MS, HI_IDLE_MS = 55000, 65000

CSV_FIELDS = [
    "t", "captureEventTimeouts", "captureEventWakes",
    "captureIdleEpisodes", "captureLastIdleMs", "captureTotalIdleMs",
    "captureCurrentIdleMs", "captureInterArrivalSamples",
    "captureInterArrivalMsMax", "captureInterArrivalSaturated",
    "captureCallbacks", "captureReinitCount", "idleSim",
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


# ---------------------------------------------------------------------------
# Server process with dual-path (stdout+stderr) line capture (m13 / V-02).
# ---------------------------------------------------------------------------

class ServerProc:
    """Spawns the server, merges stdout+stderr lines with monotonic timestamps."""

    def __init__(self, exe, args):
        self.exe = exe
        self.lines = []            # list of (monotonic_ts, stream, text)
        self._lock = threading.Lock()
        self.spawned_at = None
        extra = args if isinstance(args, list) else args.split()
        self.proc = subprocess.Popen(
            [exe] + extra,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, bufsize=1)
        self.spawned_at = time.monotonic()
        self._t_out = threading.Thread(target=self._reader,
                                       args=(self.proc.stdout, "stdout"), daemon=True)
        self._t_err = threading.Thread(target=self._reader,
                                       args=(self.proc.stderr, "stderr"), daemon=True)
        self._t_out.start()
        self._t_err.start()

    def _reader(self, fh, stream):
        for raw in fh:
            ts = time.monotonic()
            text = raw.rstrip("\r\n")
            with self._lock:
                self.lines.append((ts, stream, text))

    def snapshot(self):
        with self._lock:
            return list(self.lines)

    def stop(self):
        try:
            self.proc.terminate()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=10)
        except Exception:
            try:
                self.proc.kill()
                self.proc.wait(timeout=10)
            except Exception:
                pass
        self._t_out.join(timeout=5)
        self._t_err.join(timeout=5)

    def dump_consoles(self, out_dir):
        snap = self.snapshot()
        with open(os.path.join(out_dir, "console_out.txt"), "w", newline="") as f:
            for _ts, stream, text in snap:
                if stream == "stdout":
                    f.write(text + "\n")
        with open(os.path.join(out_dir, "console_err.txt"), "w", newline="") as f:
            for _ts, stream, text in snap:
                if stream == "stderr":
                    f.write(text + "\n")


def wait_probe_ready(host, port, timeout=15.0):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            return probe(host, port)
        except Exception as e:   # noqa: BLE001
            last = e
            time.sleep(0.3)
    fail("server probe not ready within %.0fs (last error: %s)" % (timeout, last))


def preflight(host, port, p):
    """§3.6.2 hard assertions: build gate, new fields present, no residual injection."""
    ok, _ = fault_set(host, port, "stats_reset", "1")   # harmless: resets window
    if not ok:
        fail("fault/set returned 404 -- non-injection build; this script requires "
             "SOUNDSHARE_ENABLE_FAULT_INJECTION=ON (§3.6.2-1)")
    injection_build = True
    inj = p.get("injection", {}) or {}
    for k in ("captureIdleEpisodes", "captureLastIdleMs", "captureTotalIdleMs",
              "captureCurrentIdleMs"):
        if k not in p:
            fail("probe field %s missing (S0 scaffolding not in this binary; §3.6.2-1)" % k)
    if "captureIdleSim" not in inj:
        fail("probe field injection.captureIdleSim missing (S0 scaffolding not in "
             "this binary; §3.6.2-1)")
    if inj.get("captureIdleSim") not in (0, False, None):
        fail("residual injection: injection.captureIdleSim=%r at start (§3.6.2-2)"
             % inj.get("captureIdleSim"))
    return injection_build


def check_activity(p_a, p_b, no_client, where, g7=True):
    """P0-2 activity gate; G-7 startup-pollution guard (g7=True, pre-inject only).

    Callbacks delta > 0 AND interArrival samples delta >= 100 (5s @ ~10ms
    ~= 500); with --no-client the callbacks check degrades to samples-only
    (callbacks need a streaming client due to the hasStreamingClients early
    return). G-7 (g7=True): episodes == 0 AND totalIdleMs == 0, else exit 2.
    The post-disarm re-check calls this with g7=False (episodes/totalMs are
    legitimately non-zero after a completed episode there).
    """
    d_callbacks = (p_b.get("captureCallbacks") or 0) - (p_a.get("captureCallbacks") or 0)
    d_samples = ((p_b.get("captureInterArrivalSamples") or 0)
                 - (p_a.get("captureInterArrivalSamples") or 0))
    episodes = p_b.get("captureIdleEpisodes") or 0
    total_ms = p_b.get("captureTotalIdleMs") or 0
    activity = {
        "where": where,
        "callbacksDelta": d_callbacks,
        "samplesDelta": d_samples,
        "noClient": no_client,
        "g7Applied": g7,
    }
    if g7:
        activity["g7"] = {"idleEpisodes": episodes, "totalIdleMs": total_ms}
    callbacks_ok = no_client or d_callbacks > 0
    if not callbacks_ok or d_samples < 100:
        activity["pass"] = False
        fail("activity gate failed at %s: callbacksDelta=%d samplesDelta=%d "
             "(no-client=%s) -- no active playback; idle exit would never "
             "happen, AC-FR01-2/4 & AC-FR02-1 mechanically unreachable (C-H/P0-2)"
             % (where, d_callbacks, d_samples, no_client))
    if g7 and (episodes != 0 or total_ms != 0):
        activity["pass"] = False
        fail("G-7 startup pollution: captureIdleEpisodes=%d captureTotalIdleMs=%d "
             "at %s -- a startup idle episode accumulated; AC-FR02-1 [55000,65000] "
             "would fake-fail. Start loop_play BEFORE the server." % (episodes, total_ms, where))
    activity["pass"] = True
    return activity


def lines_between(snap, t0, t1, regex):
    """Merged-stream lines in [t0, t1] matching regex -> list of match groups."""
    out = []
    for ts, _stream, text in snap:
        if t0 <= ts <= t1:
            m = regex.match(text)
            if m:
                out.append(m)
    return out


def run_probe_csv(out_dir, rows):
    path = os.path.join(out_dir, "probe.csv")
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=CSV_FIELDS)
        w.writeheader()
        w.writerows(rows)


def probe_row(t0, p):
    inj = p.get("injection", {}) or {}
    row = {"t": round(time.monotonic() - t0, 1)}
    for f in CSV_FIELDS[1:-2]:
        row[f] = p.get(f)
    row["captureReinitCount"] = p.get("captureReinitCount")
    row["idleSim"] = inj.get("captureIdleSim")
    return row


def write_manifest(out_dir, args, exe, injection_build, p, baseline_ref=None,
                   duration_sec=None):
    manifest = {
        "binaryPath": os.path.abspath(exe),
        "binarySha256": sha256_of(exe),
        "gitCommit": git_commit(),
        "buildMacros": {
            "SOUNDSHARE_ENABLE_DIAGNOSTICS": True,
            "SOUNDSHARE_ENABLE_FAULT_INJECTION": injection_build,
        },
        "mode": args.mode,
        "windowType": WINDOW_TYPE[args.mode],
        "durationSec": duration_sec if duration_sec is not None else args.idle_seconds,
        "collectedAt": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "endpointName": p.get("captureEndpointName"),
        "os": platform.platform(),
        "tag": args.tag,
    }
    if baseline_ref:
        manifest["baselineRef"] = baseline_ref
    with open(os.path.join(out_dir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1, ensure_ascii=False)
    return manifest


WINDOW_TYPE = {
    "inject": "W-B (inject ON->OFF, loop_play, from t_inject)",
    "inject-idle": "W-A' (inject ON, no playback, from spawn)",
    "active": "W-C (no injection, loop_play)",
    "natural": "W-A' (inject ON, no playback, from spawn)",   # G-1 redefined alias
}


def require_baseline(args):
    """post side: evidence/baseline/**/manifest.json must exist (P0-1, exit 2)."""
    manifests = sorted(glob.glob(
        os.path.join(args.evidence_root, "baseline", "**", "manifest.json"),
        recursive=True))
    if not manifests:
        fail("post requires a baseline manifest under evidence/baseline/ to exist "
             "first (P0-1; run the S0/baseline passes before post)")
    with open(manifests[0]) as f:
        return json.load(f).get("binarySha256")


def load_baseline_lines(args):
    """V-A3: read the W-B baseline_lines from evidence/baseline/inject-idle."""
    path = os.path.join(args.evidence_root, "baseline", "inject", "metrics.json")
    if not os.path.isfile(path):
        fail("V-A3 requires evidence/baseline/inject/metrics.json (missing)")
    with open(path) as f:
        m = json.load(f)
    lines = m.get("lines", {}).get("all")
    if not isinstance(lines, int):
        fail("V-A3: baseline W-B lines not recorded as int (got %r)" % (lines,))
    return lines


def apply_va3(args, post_lines):
    """G-4: (baseline_lines >= 1) ? (post <= 1 AND post <= baseline) : N/A(vacuous).
    N/A is recorded, never counted as PASS (nor FAIL -> no exit 2 for N/A)."""
    baseline_lines = load_baseline_lines(args)
    if baseline_lines == 0:
        return {
            "rule": "V-A3: (baseline_lines >= 1) ? (post <= 1 && post <= baseline) : N/A (vacuous)",
            "baselineLines": baseline_lines,
            "postLines": post_lines,
            "comparison": "N/A (vacuous)",
            "verdict": None,
            "note": "baseline W-B had zero timeout lines: the line-count comparison "
                    "has no discriminating power here; recorded only, NOT counted "
                    "as PASS (G-4). Anchoring stays W-A' regardless (G-5).",
        }
    ok = post_lines <= 1 and post_lines <= baseline_lines
    return {
        "rule": "V-A3: (baseline_lines >= 1) ? (post <= 1 && post <= baseline) : N/A (vacuous)",
        "baselineLines": baseline_lines,
        "postLines": post_lines,
        "comparison": "PASS" if ok else "FAIL",
        "verdict": "PASS" if ok else "FAIL",
    }


# ---------------------------------------------------------------------------
# W-A' (G-1): injection ON + NO loop_play, window counted from server spawn.
# ---------------------------------------------------------------------------

def run_inject_idle(args):
    injection_build = True
    out_dir = os.path.join(args.evidence_root, args.tag, "inject-idle")
    os.makedirs(out_dir, exist_ok=True)
    if args.idle_seconds < 60:
        fail("--idle-seconds %s < 60 (P1-2-style window minimum)" % args.idle_seconds)

    print("W-A': spawn server (no loop_play, no stub client), window from spawn, "
          "duration=%ss" % args.idle_seconds)
    srv = ServerProc(args.exe_bin, args.args)
    try:
        p0 = wait_probe_ready(args.host, args.port)
        preflight(args.host, args.port, p0)
        base = {
            "timeouts": p0.get("captureEventTimeouts") or 0,
            "wakes": p0.get("captureEventWakes") or 0,
            "episodes": p0.get("captureIdleEpisodes") or 0,
        }
        # arm the injection; the window anchor is srv.spawned_at (G-1)
        ok, body = fault_set(args.host, args.port, "capture_idle_sim", "1")
        if not ok:
            fail("failed to set capture_idle_sim=1: %s" % body)
        t_inject = time.monotonic()
        print("capture_idle_sim=1 armed at +%.1fs from spawn" % (t_inject - srv.spawned_at))

        rows = []
        t0 = srv.spawned_at
        window_end = t0 + args.idle_seconds
        while time.monotonic() < window_end:
            p = probe(args.host, args.port)
            rows.append(probe_row(t0, p))
            time.sleep(min(args.interval, max(0.05, window_end - time.monotonic())))
        p1 = probe(args.host, args.port)
        rows.append(probe_row(t0, p1))

        delta = {
            "eventTimeouts": (p1.get("captureEventTimeouts") or 0) - base["timeouts"],
            "eventWakes": (p1.get("captureEventWakes") or 0) - base["wakes"],
            "idleEpisodes": (p1.get("captureIdleEpisodes") or 0) - base["episodes"],
        }

        # disarm before shutdown (residual-injection hygiene)
        fault_set(args.host, args.port, "capture_idle_sim", "0")
        time.sleep(0.5)
    finally:
        srv.stop()
        srv.dump_consoles(out_dir)
    run_probe_csv(out_dir, rows)

    snap = srv.snapshot()
    window_lines = len(lines_between(snap, srv.spawned_at,
                                     srv.spawned_at + args.idle_seconds, RE_TIMEOUT))
    # recovery lines cannot exist in W-A' (no playback => idle never exits)
    recovery_lines = lines_between(snap, srv.spawned_at,
                                   srv.spawned_at + args.idle_seconds, RE_RECOVERY)
    if recovery_lines:
        fail("W-A' unexpectedly produced %d recovery line(s) -- no playback means "
             "idle must never exit here" % len(recovery_lines))

    comparison = None
    va_verdict = None
    if args.tag in ("s0v", "baseline"):
        # V-A1 (fail-closed). V-22 is the same assertion on the S0 binary:
        # passing it proves S0 still floods like HEAD.
        va_verdict = "V-A1"
        if not (window_lines >= 6 and 250 <= delta["eventTimeouts"] <= 350):
            fail("V-A1 failed (tag=%s): W-A' timeout lines=%d (need >=6), "
                 "delta captureEventTimeouts=%d (need [250,350]). If lines==5, "
                 "recollect with --idle-seconds 70 (O-D1 remark; window counts "
                 "from spawn so 60s => exactly 6 lines)." % (args.tag, window_lines,
                                                             delta["eventTimeouts"]))
        print("V-A1 PASS (tag=%s): lines=%d, deltaTimeouts=%d" %
              (args.tag, window_lines, delta["eventTimeouts"]))
        if args.tag == "s0v":
            print("V-22: S0 binary floods like HEAD under inject+no-playback "
                  "(>=6 lines in 60s) -- baseline is collectable on S0")
    elif args.tag == "post":
        va_verdict = "V-A2"
        if not (window_lines <= 1 and delta["idleEpisodes"] >= 1):
            fail("V-A2 failed: W-A' timeout lines=%d (need <=1), delta "
                 "captureIdleEpisodes=%d (need >=1). episodes>=1 proves the "
                 "capture really entered idle and was silenced (anti-vacuity)."
                 % (window_lines, delta["idleEpisodes"]))
        print("V-A2 PASS: lines=%d, deltaEpisodes=%d" %
              (window_lines, delta["idleEpisodes"]))

    metrics = {
        "tag": args.tag,
        "mode": "inject-idle",
        "windowType": WINDOW_TYPE["inject-idle"],
        "durationSec": args.idle_seconds,
        "collectedAt": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "endpointName": p0.get("captureEndpointName"),
        "lines": {"window": window_lines},
        "deltas": delta,
        "idleFields": {k: p1.get(k) for k in
                       ("captureIdleEpisodes", "captureLastIdleMs",
                        "captureTotalIdleMs", "captureCurrentIdleMs")},
        "vaVerdict": va_verdict,
        "comparison": comparison,
        "anchor": "W-A' = capture_idle_sim=1 + no playback, 60s from server spawn; "
                  "the single anchor of the '>=6 lines / delta timeouts~=300' "
                  "comparison (O-D1(c)); V-23 never participates in anchoring (G-5).",
        "g6Note": "Recovery-line `timeouts` is expected ~=0 in the inject window; "
                  "the ~300 figure in requirements §7.3 describes natural idling "
                  "and is NOT a criterion (G-6).",
    }
    with open(os.path.join(out_dir, "metrics.json"), "w") as f:
        json.dump(metrics, f, indent=1, ensure_ascii=False)

    baseline_ref = require_baseline(args) if args.tag == "post" else None
    write_manifest(out_dir, args, args.exe_bin, injection_build, p0, baseline_ref)

    print("W-A' [%s]: windowLines=%d deltaTimeouts=%d deltaEpisodes=%d"
          % (args.tag, window_lines, delta["eventTimeouts"], delta["idleEpisodes"]))
    print("artifacts: %s" % out_dir)
    print("OK")


# ---------------------------------------------------------------------------
# W-B: injection ON->OFF with loop_play active; the P0-2 main evidence window.
# ---------------------------------------------------------------------------

def run_inject(args):
    injection_build = True
    out_dir = os.path.join(args.evidence_root, args.tag, "inject")
    os.makedirs(out_dir, exist_ok=True)
    if args.idle_seconds < 60:
        fail("--idle-seconds %s < 60 (P1-2-style window minimum)" % args.idle_seconds)
    if args.window_seconds < 60:
        fail("--window-seconds %s < 60 (AC-FR03 window, P1-3)" % args.window_seconds)

    players = []
    stubs = []
    ac = {}
    try:
        # G-7: loop_play starts BEFORE the server (startup pollution guard)
        if not args.no_spawn_play:
            players.append(subprocess.Popen(
                [sys.executable, LOOP_PLAY, args.clip],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
            time.sleep(1.5)   # let the render stream stabilise on the mixer
            print("loop_play spawned (pid %d), clip=%s" % (players[0].pid, args.clip))

        srv = ServerProc(args.exe_bin, args.args)
        try:
            p0 = wait_probe_ready(args.host, args.port)
            preflight(args.host, args.port, p0)
            # step 2: stats_reset (isolate old maxUs) -- §3.6.4 table
            reset_stats_window(args.host, args.port)

            if not args.no_client:
                stubs.append(subprocess.Popen(
                    [sys.executable, STUB, "--host", args.host, "--port", str(args.port),
                     "--mode", "normal",
                     "--duration", str(args.idle_seconds + args.window_seconds + 60)],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
                print("stub streaming client spawned (pid %d)" % stubs[0].pid)
                time.sleep(1.0)

            # step 3: >=5s activity gate + G-7 (P0-2)
            p_a = probe(args.host, args.port)
            time.sleep(5.0)
            p_b = probe(args.host, args.port)
            activity_pre = check_activity(p_a, p_b, args.no_client, "pre-inject")

            # step 4: arm (p_arm anchors the V-23 inject-window deltas)
            ok, body = fault_set(args.host, args.port, "capture_idle_sim", "1")
            if not ok:
                fail("failed to set capture_idle_sim=1: %s" % body)
            t_inject = time.monotonic()
            p_arm = probe(args.host, args.port)
            print("capture_idle_sim=1 armed (t_inject); holding %ss" % args.idle_seconds)

            # step 5 (post only): poll currentIdleMs>0 (2 consecutive samples agree)
            t_idle_on = None
            if args.tag == "post":
                deadline = t_inject + 10.0
                seen = 0
                while time.monotonic() < deadline:
                    p = probe(args.host, args.port)
                    cur = p.get("captureCurrentIdleMs") or 0
                    if cur > 0:
                        seen += 1
                        if seen >= 2:
                            t_idle_on = time.monotonic()
                            break
                    else:
                        seen = 0
                    time.sleep(0.2)
                if t_idle_on is None:
                    fail("idle did not take effect within 10s (currentIdleMs stayed 0)")

            # step 6: hold --idle-seconds from t_inject
            rows = []
            t0 = srv.spawned_at
            hold_until = t_inject + args.idle_seconds
            while time.monotonic() < hold_until:
                p = probe(args.host, args.port)
                rows.append(probe_row(t0, p))
                time.sleep(min(args.interval, max(0.05, hold_until - time.monotonic())))

            # step 7: disarm (p_off closes the V-23 inject window)
            fault_set(args.host, args.port, "capture_idle_sim", "0")
            t_idle_off = time.monotonic()
            p_off = probe(args.host, args.port)
            print("capture_idle_sim=0 (t_idle_off)")

            # step 8 (post): deterministic exit poll (AC-FR01-4)
            t_exit = None
            if args.tag == "post":
                deadline = t_idle_off + 10.0
                while time.monotonic() < deadline:
                    p = probe(args.host, args.port)
                    if ((p.get("captureCurrentIdleMs") or 0) == 0
                            and (p.get("captureLastIdleMs") or 0) > 0):
                        t_exit = time.monotonic()
                        break
                    time.sleep(0.2)
                if t_exit is None:
                    fail("idle did not exit within 10s of disarm "
                         "(currentIdleMs==0 && lastIdleMs>0 unreachable; AC-FR01-4)")
            else:
                # S0/baseline carry no idle logic: after disarm the drain resumes
                t_exit = t_idle_off + 5.0

            # §3.6.2-4: post-disarm activity re-check (5s; g7=False -- after a
            # completed episode the idle fields are legitimately non-zero)
            p_c = probe(args.host, args.port)
            time.sleep(5.0)
            p_d = probe(args.host, args.port)
            activity_post = check_activity(p_c, p_d, args.no_client, "post-disarm",
                                           g7=False)
            p1 = probe(args.host, args.port)
            rows.append(probe_row(t0, p1))

            # step 9 (P1-2, hard-coded): read probe FIRST (AC-FR02-1 + G-8 chain),
            # THEN stats_reset, THEN open the AC-FR03 window.
            ac_fr02_1 = None
            probe_chain = None
            if args.tag == "post":
                episodes = p1.get("captureIdleEpisodes") or 0
                last_ms = p1.get("captureLastIdleMs") or 0
                total_ms = p1.get("captureTotalIdleMs") or 0
                ac_fr02_1 = {
                    "captureIdleEpisodes": episodes,
                    "captureLastIdleMs": last_ms,
                    "captureTotalIdleMs": total_ms,
                }
                if not (episodes >= 1 and LO_IDLE_MS <= last_ms <= HI_IDLE_MS
                        and LO_IDLE_MS <= total_ms <= HI_IDLE_MS):
                    fail("AC-FR02-1 failed: episodes=%d lastMs=%d totalMs=%d "
                         "(need >=1 and both in [%d,%d])"
                         % (episodes, last_ms, total_ms, LO_IDLE_MS, HI_IDLE_MS))
                print("AC-FR02-1 PASS: episodes=%d lastMs=%d totalMs=%d"
                      % (episodes, last_ms, total_ms))

                # G-8: run the probe.py --assert-* chain for real, once, BEFORE reset
                cmd = [sys.executable, PROBE, "--host", args.host, "--port", str(args.port),
                       "--assert-idle-episodes-ge", "1",
                       "--assert-last-idle-ms", "%d,%d" % (LO_IDLE_MS, HI_IDLE_MS),
                       "--assert-total-idle-ms", "%d,%d" % (LO_IDLE_MS, HI_IDLE_MS)]
                r = subprocess.run(cmd, capture_output=True, text=True, timeout=30)

                def _rel_if_under(c):
                    # relpath only when c lives under REPO_ROOT; cross-drive
                    # (e.g. C:\...\python.exe vs D:\repo) raises ValueError.
                    try:
                        rel = os.path.relpath(c, REPO_ROOT)
                    except ValueError:
                        return c
                    return rel if not rel.startswith("..") else c

                probe_chain = {
                    "cmd": " ".join(_rel_if_under(c) for c in cmd),
                    "exitCode": r.returncode,
                    "stdoutTail": (r.stdout or "")[-400:],
                    "stderrTail": (r.stderr or "")[-400:],
                }
                if r.returncode != 0:
                    fail("G-8 probe.py assert chain exited %d (expected 0): %s"
                         % (r.returncode, r.stderr))
                print("G-8 probe.py assert chain EXIT 0")

            # AC-FR02-3 (V-13): snapshot right before/after the window reset
            va13 = None
            reset_stats_window(args.host, args.port)
            p2 = probe(args.host, args.port)
            if args.tag == "post":
                va13 = {
                    "before": {
                        "captureLastIdleMs": p1.get("captureLastIdleMs"),
                        "captureCurrentIdleMs": p1.get("captureCurrentIdleMs"),
                        "captureIdleEpisodes": p1.get("captureIdleEpisodes"),
                        "captureTotalIdleMs": p1.get("captureTotalIdleMs"),
                    },
                    "after": {
                        "captureLastIdleMs": p2.get("captureLastIdleMs"),
                        "captureCurrentIdleMs": p2.get("captureCurrentIdleMs"),
                        "captureIdleEpisodes": p2.get("captureIdleEpisodes"),
                        "captureTotalIdleMs": p2.get("captureTotalIdleMs"),
                    },
                }
                ok13 = (p2.get("captureLastIdleMs") == 0
                        and (p2.get("captureCurrentIdleMs") or 0) == 0
                        and (p2.get("captureIdleEpisodes") or 0) == (p1.get("captureIdleEpisodes") or 0)
                        and (p2.get("captureTotalIdleMs") or 0) == (p1.get("captureTotalIdleMs") or 0)
                        and (p1.get("captureLastIdleMs") or 0) > 0)
                va13["pass"] = bool(ok13)
                ac["AC-FR02-3"] = "PASS" if ok13 else "FAIL"
                if not ok13:
                    fail("AC-FR02-3 (V-13) failed: reset must clear lastMs/curMs "
                         "and KEEP episodes/totalMs; got after=%r" % va13["after"])
                print("AC-FR02-3 (V-13) PASS: lastMs/curMs cleared, episodes/totalMs kept")

            # step 10: open the AC-FR03 window (loop_play keeps supplying)
            d_win_start = p2.get("captureDiscontinuityCount") or 0
            sat_start = p2.get("captureInterArrivalSaturated") or 0
            win_end = time.monotonic() + args.window_seconds
            while time.monotonic() < win_end:
                p = probe(args.host, args.port)
                rows.append(probe_row(t0, p))
                time.sleep(min(args.interval, max(0.05, win_end - time.monotonic())))
            p3 = probe(args.host, args.port)
            rows.append(probe_row(t0, p3))

            if args.tag == "post":
                mx = p3.get("captureInterArrivalMsMax") or 0.0
                p50 = p3.get("captureInterArrivalMsP50") or 0.0
                p99 = p3.get("captureInterArrivalMsP99") or 0.0
                samples = p3.get("captureInterArrivalSamples") or 0
                sat_delta = (p3.get("captureInterArrivalSaturated") or 0) - sat_start
                d_delta = (p3.get("captureDiscontinuityCount") or 0) - d_win_start
                fr03 = {
                    "maxMs": mx, "p50Ms": p50, "p99Ms": p99,
                    "samples": samples, "minSamples": args.min_samples,
                    "saturatedDelta": sat_delta, "dDelta": d_delta,
                    "dNote": "inject-disarm D+1 is the expected side effect "
                              "(§3.0(d)); recorded as such, never a NFR-1 failure (P1-5).",
                }
                ac["AC-FR03-1"] = "PASS" if mx < 5000 else "FAIL"
                ac["AC-FR03-2"] = "PASS" if (9.50 <= p50 <= 10.50 and p99 <= 25.00
                                             and samples >= args.min_samples) else "FAIL"
                ac["AC-FR03-3"] = "PASS" if sat_delta == 0 else "FAIL"
                if ac["AC-FR03-1"] != "PASS":
                    fail("AC-FR03-1 failed: Max=%sms (no sample >5000ms allowed)" % mx)
                if ac["AC-FR03-2"] != "PASS":
                    fail("AC-FR03-2 failed: P50=%s P99=%s samples=%d "
                         "(need [9.50,10.50] / <=25.00 / >=%d)"
                         % (p50, p99, samples, args.min_samples))
                if ac["AC-FR03-3"] != "PASS":
                    fail("AC-FR03-3 failed: saturated delta=%d (need ==0)" % sat_delta)
                print("AC-FR03-1/2/3 PASS: Max=%s P50=%s P99=%s n=%d satDelta=%d dDelta=%d"
                      % (mx, p50, p99, samples, sat_delta, d_delta))

            # disarm hygiene
            fault_set(args.host, args.port, "capture_idle_sim", "0")
            time.sleep(0.5)
        finally:
            srv.stop()
            srv.dump_consoles(out_dir)
    finally:
        for s in stubs:
            s.kill()
            s.wait()
        for pl in players:
            pl.kill()
            pl.wait()
    run_probe_csv(out_dir, rows)

    snap = srv.snapshot()
    lines_all = len(lines_between(snap, t_inject, t_exit, RE_TIMEOUT))
    if t_idle_on is not None:
        lines_sub = len(lines_between(snap, t_idle_on, t_idle_off, RE_TIMEOUT))
    else:
        lines_sub = None   # S0/baseline: no idle logic => no "idle active" interval
    recoveries = lines_between(snap, t_inject, t_exit, RE_RECOVERY)
    recovery_parsed = None
    if recoveries:
        m = recoveries[0]
        recovery_parsed = {
            "ep": int(m.group(1)), "idleMs": int(m.group(2)),
            "totalIdleMs": int(m.group(3)), "timeouts": int(m.group(4)),
            "count": len(recoveries),
        }

    comparison = None
    if args.tag == "post":
        ac["AC-FR01-1-all"] = "PASS" if lines_all <= 1 else "FAIL"
        if lines_all > 1:
            fail("AC-FR01-1(1) failed: %d timeout lines over the whole episode "
                 "(need <=1)" % lines_all)
        if lines_sub is not None:
            ac["AC-FR01-1-sub"] = "PASS" if lines_sub == 0 else "FAIL"
            if lines_sub != 0:
                fail("AC-FR01-1(2) failed: %d lines in the idle-active subinterval "
                     "(need ==0)" % lines_sub)
        if not (recoveries and len(recoveries) == 1):
            fail("AC-FR01-2 failed: expected exactly 1 recovery line, got %d"
                 % len(recoveries))
        ac["AC-FR01-2"] = "PASS"
        # cross-check: recovery line idleMs vs probe captureLastIdleMs
        if recovery_parsed and p1.get("captureLastIdleMs") is not None:
            if recovery_parsed["idleMs"] != p1.get("captureLastIdleMs"):
                fail("AC-FR01-2 cross-check failed: line idleMs=%s != probe "
                     "captureLastIdleMs=%s" % (recovery_parsed["idleMs"],
                                               p1.get("captureLastIdleMs")))
        ac["AC-FR01-4"] = "PASS"
        comparison = apply_va3(args, lines_all)
        if comparison.get("verdict") == "FAIL":
            fail("V-A3 comparison FAIL: %r" % comparison)
        print("V-A3 comparison: %s (baseline=%s post=%s)"
              % (comparison["comparison"], comparison["baselineLines"], lines_all))
    elif args.tag == "s0v":
        comparison = {
            "rule": "V-A3 (baseline side to be collected next)",
            "baselineLines": lines_all,
            "postLines": None,
            "comparison": "pending-baseline",
        }
    else:   # baseline
        comparison = {
            "rule": "V-A3: (baseline_lines >= 1) ? (post <= 1 && post <= baseline) : N/A (vacuous)",
            "baselineLines": lines_all,
            "postLines": None,
            "comparison": "recorded",
        }

    metrics = {
        "tag": args.tag,
        "mode": "inject",
        "windowType": WINDOW_TYPE["inject"],
        "durationSec": args.idle_seconds,
        "collectedAt": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "endpointName": p0.get("captureEndpointName"),
        "lines": {"all": lines_all, "sub": lines_sub},
        "recovery": recovery_parsed,
        # V-23 inject-window deltas ([t_inject, t_idle_off]; event stays set by
        # the engine while a render stream runs, so WAIT_TIMEOUT ~= 0 is the
        # expected outcome here -- recorded, never an anchor (G-5/G-6))
        "deltas": {
            "eventTimeouts": (p_off.get("captureEventTimeouts") or 0)
                             - (p_arm.get("captureEventTimeouts") or 0),
            "eventWakes": (p_off.get("captureEventWakes") or 0)
                          - (p_arm.get("captureEventWakes") or 0),
            "idleEpisodes": (p_off.get("captureIdleEpisodes") or 0)
                            - (p_arm.get("captureIdleEpisodes") or 0),
        },
        "idleFields": {k: p1.get(k) for k in
                       ("captureIdleEpisodes", "captureLastIdleMs",
                        "captureTotalIdleMs", "captureCurrentIdleMs")},
        "activity": {"pre": activity_pre, "post": activity_post},
        "acVerdicts": ac or None,
        "acFR02_1": ac_fr02_1,
        "probePyAssertChain": probe_chain,
        "va13": va13,
        "comparison": comparison,
        "g6Note": "Recovery-line `timeouts` is expected ~=0 in the inject window "
                  "(almost no WAIT_TIMEOUT while a render stream keeps setting the "
                  "event); the ~300 figure in requirements §7.3 describes natural "
                  "idling and is NOT a criterion (G-6).",
    }
    with open(os.path.join(out_dir, "metrics.json"), "w") as f:
        json.dump(metrics, f, indent=1, ensure_ascii=False)

    baseline_ref = require_baseline(args) if args.tag == "post" else None
    write_manifest(out_dir, args, args.exe_bin, injection_build, p0, baseline_ref)

    print("W-B [%s]: linesAll=%d linesSub=%s recovery=%s"
          % (args.tag, lines_all, lines_sub, recovery_parsed))
    print("artifacts: %s" % out_dir)
    print("OK")

    if args.tag == "s0v":
        freeze_v23(args, metrics)


def freeze_v23(args, metrics):
    """G-5: freeze the V-23 measurement BEFORE the baseline is collected.

    V-23 only decides whether W-B carries the conditional line gate
    (delta >= 50 -> expect baseline_lines >= 1); it NEVER participates in
    anchoring (the anchor is W-A', fixed).
    """
    delta_to = metrics["deltas"]["eventTimeouts"]
    out = {
        "frozenAt": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "stage": "S0v (before baseline; G-5)",
        "window": "W-B 60s inject, loop_play active",
        "deltaEventTimeouts": delta_to,
        "deltaEventWakes": metrics["deltas"].get("eventWakes"),
        "timeoutLines": metrics["lines"]["all"],
        "recoveryLines": (metrics.get("recovery") or {}).get("count", 0),
        "wbRule": "V-A3: (baseline_lines >= 1) ? (post <= 1 && post <= baseline) : N/A (vacuous)",
        "wbExpectation": ("W-B expected to carry the conditional line gate "
                          "(delta >= 50 => expect baseline_lines >= 1)")
                          if (delta_to or 0) >= 50
                          else ("W-B expected to be N/A (vacuous) "
                                "(delta < 50 => expect baseline_lines == 0)"),
        "anchorStatement": "The comparison anchor is W-A' (inject ON + no "
                            "playback, 60s from spawn) and stays so regardless "
                            "of this measurement (O-D1(c) / G-5); V-23 never "
                            "participates in anchoring.",
    }
    path = os.path.join(args.evidence_root, "s0v", "v23-frozen.json")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(out, f, indent=1, ensure_ascii=False)
    print("V-23 frozen -> %s (deltaTimeouts=%s => %s)"
          % (path, delta_to, out["wbExpectation"]))


# ---------------------------------------------------------------------------
# W-C: active window (AC-FR01-3 no-false-positive, >= 30s of playback).
# ---------------------------------------------------------------------------

def run_active(args):
    injection_build = True
    out_dir = os.path.join(args.evidence_root, args.tag, "active")
    os.makedirs(out_dir, exist_ok=True)
    if args.window_seconds < 30:
        fail("--window-seconds %s < 30 (AC-FR01-3 minimum)" % args.window_seconds)

    players = []
    stubs = []
    try:
        players.append(subprocess.Popen(
            [sys.executable, LOOP_PLAY, args.clip],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(1.5)
        print("loop_play spawned (pid %d)" % players[0].pid)

        srv = ServerProc(args.exe_bin, args.args)
        try:
            p0 = wait_probe_ready(args.host, args.port)
            preflight(args.host, args.port, p0)
            if not args.no_client:
                stubs.append(subprocess.Popen(
                    [sys.executable, STUB, "--host", args.host, "--port", str(args.port),
                     "--mode", "normal", "--duration", str(args.window_seconds + 60)],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
                time.sleep(1.0)
            p_a = probe(args.host, args.port)
            time.sleep(5.0)
            p_b = probe(args.host, args.port)
            check_activity(p_a, p_b, args.no_client, "active-window-open")

            base_episodes = p_b.get("captureIdleEpisodes") or 0
            rows = []
            t0 = srv.spawned_at
            win_end = time.monotonic() + args.window_seconds
            while time.monotonic() < win_end:
                p = probe(args.host, args.port)
                rows.append(probe_row(t0, p))
                time.sleep(min(args.interval, max(0.05, win_end - time.monotonic())))
            p1 = probe(args.host, args.port)
            rows.append(probe_row(t0, p1))
        finally:
            srv.stop()
            srv.dump_consoles(out_dir)
    finally:
        for s in stubs:
            s.kill()
            s.wait()
        for pl in players:
            pl.kill()
            pl.wait()
    run_probe_csv(out_dir, rows)

    delta_episodes = (p1.get("captureIdleEpisodes") or 0) - base_episodes
    if args.tag == "post":
        if delta_episodes != 0:
            fail("AC-FR01-3 failed: captureIdleEpisodes delta=%d over %ss of "
                 "continuous playback (need ==0)" % (delta_episodes, args.window_seconds))
        print("AC-FR01-3 PASS: episodes delta=%d over %ss active window"
              % (delta_episodes, args.window_seconds))

    metrics = {
        "tag": args.tag,
        "mode": "active",
        "windowType": WINDOW_TYPE["active"],
        "durationSec": args.window_seconds,
        "collectedAt": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "endpointName": p0.get("captureEndpointName"),
        "deltaEpisodes": delta_episodes,
        "idleFields": {k: p1.get(k) for k in
                       ("captureIdleEpisodes", "captureLastIdleMs",
                        "captureTotalIdleMs", "captureCurrentIdleMs")},
    }
    with open(os.path.join(out_dir, "metrics.json"), "w") as f:
        json.dump(metrics, f, indent=1, ensure_ascii=False)

    baseline_ref = require_baseline(args) if args.tag == "post" else None
    write_manifest(out_dir, args, args.exe_bin, injection_build, p0, baseline_ref,
                   duration_sec=args.window_seconds)
    print("W-C [%s]: deltaEpisodes=%d" % (args.tag, delta_episodes))
    print("artifacts: %s" % out_dir)
    print("OK")


# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="T-CAPTURE-IDLE-LOG idle log governance "
                                             "acceptance script (12th test)")
    ap.add_argument("--tag", required=True, choices=["baseline", "post", "s0v"])
    ap.add_argument("--mode", required=True,
                    choices=["inject", "inject-idle", "natural", "active"],
                    help="inject = W-B (loop_play + inject ON->OFF); inject-idle / "
                         "natural = W-A' per G-1 (inject ON + NO playback, window "
                         "from spawn; 'natural' is a G-1-redefined alias); "
                         "active = W-C (no injection, loop_play)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--args", default="8080 0", help="server argv (default '8080 0')")
    ap.add_argument("--exe-bin", required=True, help="server executable")
    ap.add_argument("--idle-seconds", type=float, default=60,
                    help="inject hold duration in seconds (>=60, P1-2 style)")
    ap.add_argument("--window-seconds", type=float, default=60,
                    help="AC-FR03 window duration (>=60 inject, >=30 active)")
    ap.add_argument("--min-samples", type=int, default=1000,
                    help="AC-FR03-2 minimum interArrival samples (P1-3)")
    ap.add_argument("--spawn-play", dest="no_spawn_play", action="store_false",
                    default=False, help="spawn loop_play (default for inject/active)")
    ap.add_argument("--no-spawn-play", dest="no_spawn_play", action="store_true",
                    help="do not spawn loop_play (W-A' never spawns it anyway)")
    ap.add_argument("--clip", default=DEFAULT_CLIP, help="wav clip for loop_play")
    ap.add_argument("--no-client", action="store_true",
                    help="do not spawn the stub streaming client (activity check "
                         "degrades to samples-only)")
    ap.add_argument("--evidence-root", default=DEFAULT_EVIDENCE)
    ap.add_argument("--interval", type=float, default=2.0)
    args = ap.parse_args()

    if not os.path.isfile(args.exe_bin):
        fail("server executable not found: %s" % args.exe_bin)
    if not os.path.isfile(args.clip):
        fail("clip not found: %s (needed by loop_play)" % args.clip)
    if args.mode == "natural":
        args.mode = "inject-idle"   # G-1: redefined semantics, same window

    os.makedirs(args.evidence_root, exist_ok=True)

    if args.mode == "inject-idle":
        run_inject_idle(args)
    elif args.mode == "inject":
        run_inject(args)
    else:
        run_active(args)


if __name__ == "__main__":
    main()
