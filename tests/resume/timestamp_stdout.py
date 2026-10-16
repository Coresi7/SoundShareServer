#!/usr/bin/env python3
"""tests/resume/timestamp_stdout.py — SSC stdout 逐行墙钟包裹器（可选，推荐）

设计依据：02-design.md §7.2.1（"独立包裹器：把 SSC stdout 逐行打墙钟后
透传 ⇒ 真机/人工路径复用同一度量口径"，§7.4.1 步骤 6 / §7.4.2）。

用法（真机取证时）：
  python tests\\resume\\timestamp_stdout.py -- SoundShareClient.exe <ip> <port> ^
      --dump-pcm=C:\\tmp\\real.wav --out timeline-real.json
  （Ctrl+C 结束：向【本脚本】按 Ctrl+C，脚本会转发 CTRL_C_EVENT 给目标并
   在退出前落盘 timeline JSON。）

输出：stdout 透传每行（前缀 [+SSSS.mmm]），同时记录
  (time.monotonic(), line) 到 --out 指定的 JSON（与 run_matrix.py 的
  timeline.json["stdout"] 字段同构）。

NOT a CTest suite member — manually driven acceptance script（M-12 约定）。
"""
import argparse
import json
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path


def send_ctrl_c(target_pid: int) -> bool:
    """复刻 tests/forward_forensics.py 的已验证序列（AttachConsole 模式）。"""
    import ctypes
    k32 = ctypes.windll.kernel32
    k32.FreeConsole()
    if not k32.AttachConsole(ctypes.c_uint32(target_pid)):
        return False
    k32.SetConsoleCtrlHandler(ctypes.c_void_p(0), True)
    ok = bool(k32.GenerateConsoleCtrlEvent(0, 0))
    time.sleep(0.8)
    k32.SetConsoleCtrlHandler(ctypes.c_void_p(0), False)
    k32.FreeConsole()
    return ok


def main() -> int:
    ap = argparse.ArgumentParser(
        description="SSC stdout 逐行墙钟包裹器（真机取证口径统一）")
    ap.add_argument("--out", default="timeline-real.json",
                    help="输出 JSON 路径")
    ap.add_argument("cmd", nargs=argparse.REMAINDER,
                    help="目标命令（建议以 -- 分隔）")
    args = ap.parse_args()
    cmd = [c for c in args.cmd if c != "--"]
    if not cmd:
        ap.error("需要目标命令，例如 -- SoundShareClient.exe 192.168.1.10")
    out_path = Path(args.out)

    proc = subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace", bufsize=1,
        creationflags=subprocess.CREATE_NEW_CONSOLE)
    t0 = time.monotonic()
    lines = []
    stop = threading.Event()

    def reader():
        for line in proc.stdout:
            ts = time.monotonic()
            lines.append([ts, line.rstrip()])
            print(f"[+{ts - t0:9.3f}] {line.rstrip()}", flush=True)

    threading.Thread(target=reader, daemon=True).start()

    def on_sigint(_sig, _frm):
        stop.set()

    signal.signal(signal.SIGINT, on_sigint)
    print(f"[wrap] target pid={proc.pid}; Ctrl+C here to stop target gracefully",
          flush=True)
    while not stop.is_set() and proc.poll() is None:
        time.sleep(0.2)
    if proc.poll() is None:
        ok = send_ctrl_c(proc.pid)
        print(f"[wrap] forwarded Ctrl+C -> {ok}", flush=True)
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    else:
        # 目标自身退出（stall 重连循环中用户手动断开等）
        pass
    # 等 reader 收尾
    time.sleep(0.5)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(
        {"t0": t0, "stdout": lines,
         "exit_code": proc.returncode,
         "note": "真机人工取证：与 run_matrix timeline.json 的 stdout 字段同构"},
        indent=1), encoding="utf-8")
    print(f"[wrap] timeline -> {out_path} (exit={proc.returncode})", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
