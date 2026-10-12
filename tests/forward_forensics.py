#!/usr/bin/env python3
"""tests/forward_forensics.py — silent-audio-stall 前向取证装置（复核轮 B-01 落实）

背景（为什么需要本装置）：
  21:30 / 21:38 两次服务端静默死亡无法取证，原因有二（05-code-review.md §5）：
  (a) 本机 WER 全局禁用：HKLM WER Disabled=1、WerSvc 停止且 StartType=Disabled，
      LocalDumps 机制在本机不生效（无 dump 落盘能力，且非管理员无法启用）；
  (b) 旧 PowerShell 脚本退出码捕获缺陷：Start-Process -PassThru 返回的 Process
      对象在【未调用 WaitForExit()】时直接读 .ExitCode → 句柄已失效 → 读到
      空值/异常（fix-p05-stress.ps1:50-52、fix-bisect-b.ps1:26-34 即此模式；
      正常 Ctrl+C 退出上同样复现，见 reviewer-r2v*-console.log 的
      "server-exited: yes exit-code=" 空值行）。
  本装置是"在位可用"的前向取证等效脚本（tests/ 级，代替不可用的 WER LocalDumps）：
    1) 直接模式（默认）：Popen → p.wait() → p.returncode，捕获【真实退出码】
       并自动分类——0xC0000005 等异常码即"真崩溃"铁证，-1 即 TerminateProcess
       （外部误杀特征），0 即正常优雅退出（含 Ctrl+C）。
    2) cdb 模式（--cdb）：cdb 调试器下启动目标，second-chance 未处理异常自动
       .dump /ma 落盘 + kb 40 调用栈——捕获【崩溃 dump 与调用栈】。

用法：
  python tests/forward_forensics.py --exe <exe> [--args a b ...] [--tag NAME]
      [--ctrlc-after N] [--terminate-after N] [--max-wait N] [--cdb]
      [--cdb-path <cdb.exe>] [--logdir <dir>]

  示例（服务端取证，直接模式，4 秒后发 Ctrl+C 优雅关停）：
    python tests/forward_forensics.py --exe build\\Release\\SoundShareServer.exe ^
        --args 18099 0 --tag repro --ctrlc-after 4 --max-wait 60
  示例（怀疑崩溃时，cdb 模式拿 dump）：
    python tests/forward_forensics.py --exe build\\Release\\SoundShareServer.exe ^
        --args 18099 0 --tag repro-crash --cdb
  示例（装置自测 dummy 崩溃，见 logs/fix-b01-selftest.ps1）：
    python tests/forward_forensics.py --exe C:\\Python314\\python.exe ^
        --args tests\\dummy_crash.py --tag selftest-crash --max-wait 30
    python tests/forward_forensics.py --exe C:\\Python314\\python.exe ^
        --args tests\\dummy_crash.py --tag selftest-cdb --cdb

输出（logdir 默认 docs/tasks/silent-audio-stall/logs/）：
  <tag>-out.log / <tag>-err.log   目标进程 stdout / stderr（直接模式）
  <tag>-cdb.log                   cdb 全文日志（--cdb 模式，-logo 落盘）
  dumps/<tag>-<时间戳>.dmp        崩溃 dump（--cdb 模式，second-chance 自动落盘）
  控制台关键行：
  TARGET-PID / EXIT-CODE: <int> (0xXXXXXXXX) / EXIT-CLASS: <分类>
  DUMP-CREATED: yes|no (<路径>) / CDB-LOG: <路径>

tester 期间再现静默死亡时的处置（05-code-review.md 复核轮 §5）：
  1) 直接以本装置重启服务端并复跑同场景（直接模式）——若再现死亡，控制台
     EXIT-CODE / EXIT-CLASS 即第一手定性证据（0xCxxxxxx=崩溃；-1=外部
     TerminateProcess；0=正常）。
  2) 若怀疑崩溃或需调用栈，用 --cdb 模式复跑——dump 落盘后交 reviewer 分析。
"""
import argparse
import subprocess
import sys
import time
from pathlib import Path

DEFAULT_CDB = (r"C:\Program Files\WindowsApps\Microsoft.WinDbg_1.2606.22001.0"
               r"_x64__8wekyb3d8bbwe\amd64\cdb.exe")

# second-chance 捕获的异常码（未处理 = 真崩溃；first-chance 不触发，避免误报
# 被程序正常处理掉的异常）
CAPTURE_EXCEPTIONS = ("c0000005",  # access violation
                      "c0000374",  # heap corruption
                      "c0000409",  # fast fail (__fastfail / stack cookie)
                      "c00000fd")  # stack overflow

STATUS_NAMES = {
    0x00000000: "NORMAL (graceful exit; incl. Ctrl+C handled shutdown)",
    0xFFFFFFFF: ("TERMINATED (TerminateProcess code -1; e.g. PowerShell "
                 "Stop-Process -Force / taskkill /f)"),
    0xC0000005: "CRASH: ACCESS_VIOLATION (0xC0000005)",
    0xC0000374: "CRASH: HEAP_CORRUPTION (0xC0000374)",
    0xC0000409: "CRASH: FAST_FAIL (0xC0000409)",
    0xC00000FD: "CRASH: STACK_OVERFLOW (0xC00000FD)",
    0xC000013A: "CONTROL_C_EXIT (0xC000013A; Ctrl+C default termination, no handler)",
    0x80000003: "BREAKPOINT (0x80000003)",
    0xC0000142: "CRASH: DLL_INIT_FAILED (0xC0000142)",
}


def classify(u32: int) -> str:
    if u32 in STATUS_NAMES:
        return STATUS_NAMES[u32]
    if 0xC0000000 <= u32 < 0xC0001000:
        return "CRASH: NTSTATUS facility 0xC (0x%08X)" % u32
    if u32 >= 0x80000000:
        return ("OTHER-ABNORMAL (possibly TerminateProcess with custom code) "
                "(0x%08X)" % u32)
    return "OTHER (code %d)" % u32


def send_ctrl_c(target_pid: int) -> None:
    """向目标进程的控制台发送 CTRL_C_EVENT（复刻 logs/fix-send-ctrlc.ps1 的
    已验证序列：FreeConsole → AttachConsole(pid) → 本进程忽略 Ctrl+C →
    GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0) → FreeConsole）。"""
    import ctypes
    k32 = ctypes.windll.kernel32
    k32.FreeConsole()
    if not k32.AttachConsole(ctypes.c_uint32(target_pid)):
        print("CTRLC-SEND: FAILED (AttachConsole GLE=%d; 目标可能已退出)"
              % ctypes.get_last_error(), flush=True)
        return
    k32.SetConsoleCtrlHandler(ctypes.c_void_p(0), True)  # 本进程忽略 Ctrl+C
    ok = k32.GenerateConsoleCtrlEvent(0, 0)              # CTRL_C_EVENT, 组 0
    time.sleep(0.8)
    k32.SetConsoleCtrlHandler(ctypes.c_void_p(0), False)
    k32.FreeConsole()
    print("CTRLC-SEND: %s (pid=%d)" % ("OK" if ok else "FAILED", target_pid),
          flush=True)


def terminate_like_stop_process(target_pid: int) -> None:
    """与 PowerShell Stop-Process -Force 完全同源的强杀：
    TerminateProcess(handle, 0xFFFFFFFF) —— 用于 TerminateProcess 样本自测。"""
    import ctypes
    k32 = ctypes.windll.kernel32
    PROCESS_TERMINATE = 0x0001
    h = k32.OpenProcess(PROCESS_TERMINATE, False, ctypes.c_uint32(target_pid))
    if not h:
        print("TERMINATE: OpenProcess failed (目标可能已退出)", flush=True)
        return
    ok = k32.TerminateProcess(h, 0xFFFFFFFF)
    k32.CloseHandle(h)
    print("TERMINATE: %s (TerminateProcess code 0xFFFFFFFF)" % ("OK" if ok else "FAILED"),
          flush=True)


def wait_or_action(p, args):
    """先等待可选动作时间（期间轮询目标是否提前死亡——正是取证场景），
    然后执行动作（Ctrl+C / 强杀），最后 wait() 拿真实退出码。"""
    action_at = None
    if args.ctrlc_after is not None:
        action_at = ("ctrlc", args.ctrlc_after)
    elif args.terminate_after is not None:
        action_at = ("terminate", args.terminate_after)

    if action_at:
        kind, delay = action_at
        deadline = time.monotonic() + delay
        while time.monotonic() < deadline:
            if p.poll() is not None:
                print("NOTE: 目标在动作定时前已退出（提前死亡被捕获）", flush=True)
                break
            time.sleep(0.5)
        else:
            if kind == "ctrlc":
                send_ctrl_c(p.pid)
            else:
                terminate_like_stop_process(p.pid)

    try:
        p.wait(timeout=args.max_wait if args.max_wait > 0 else None)
    except subprocess.TimeoutExpired:
        print("RESULT: TIMEOUT (目标 %d 秒内未退出)" % args.max_wait, flush=True)
        return 3
    return 0


def run_direct(args, logdir: Path) -> int:
    out_path = logdir / ("%s-out.log" % args.tag)
    err_path = logdir / ("%s-err.log" % args.tag)
    with open(out_path, "wb") as fo, open(err_path, "wb") as fe:
        p = subprocess.Popen([args.exe] + list(args.args),
                             stdout=fo, stderr=fe,
                             creationflags=subprocess.CREATE_NEW_CONSOLE)
        print("TARGET-PID: %d" % p.pid, flush=True)
        rc_tool = wait_or_action(p, args)
        if rc_tool != 0:
            return rc_tool
        rc = p.returncode  # p.wait() 之后读取——必然有真实值（修复点）
    u = rc & 0xFFFFFFFF
    print("EXIT-CODE: %d (0x%08X)" % (rc, u), flush=True)
    print("EXIT-CLASS: %s" % classify(u), flush=True)
    print("STDOUT-LOG: %s" % out_path, flush=True)
    print("STDERR-LOG: %s" % err_path, flush=True)
    return 0


def run_cdb(args, logdir: Path) -> int:
    if not Path(args.cdb_path).exists():
        print("CDB-NOT-FOUND: %s（请用 --cdb-path 指定本机 cdb.exe）"
              % args.cdb_path, flush=True)
        return 2
    dumps_dir = logdir / "dumps"
    dumps_dir.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    dmp = dumps_dir / ("%s-%s.dmp" % (args.tag, stamp))
    cdblog = logdir / ("%s-cdb.log" % args.tag)
    # 路径用正斜杠：cdb 命令串解析会吃掉反斜杠（实测 'D:\git\...' 变 'D:git...'）；
    # .dump 用 /ma：本机 WinDbg(1.2606) 用户态不支持 /f，/ma = 完整用户态 dump。
    # !analyze 不放 -c2 串（无符号环境可能极慢导致挂起）；栈由 kb 40 提供，
    # 事后可对 dump 离线 !analyze。
    dmp_posix = str(dmp).replace("\\", "/")
    inner = ".dump /ma %s; kb 40; q" % dmp_posix
    # sn（而非 sxe）：first-chance 只通知不中断（AV first-chance 常为已被正常
    # 处理的异常，中断会卡死非交互场景）；second-chance（未处理=真崩溃）执行
    # -c2 命令：dump + 栈 + 退出。
    # 不使用 -g：-g 会把 -c 初始命令推迟到"第一次中断"（实测为 first-chance
    # AV，此时 sxe/sxn 尚未设置，second-chance 行为不受控）——去掉 -g 使
    # -c 命令在初始断点执行，保证异常设置先于程序运行。
    cmdstr = "; ".join('sxn -c2 "%s" %s' % (inner, exc)
                       for exc in CAPTURE_EXCEPTIONS) + "; g"
    argv = [args.cdb_path, "-G", "-lines", "-logo", str(cdblog),
            "-c", cmdstr, args.exe] + list(args.args)
    print("CDB-DUMP-PLAN: %s" % dmp, flush=True)
    # stdin=DEVNULL：命令异常时 cdb 会停在交互提示符等输入而永久挂起
    # （上一棒 S4 中断现场即此形态的残留 cdb+dummy 进程）——切断 stdin，
    # 并在超时路径上强杀，保证本装置自身绝不残留进程。
    p = subprocess.Popen(argv, stdin=subprocess.DEVNULL)
    try:
        p.wait(timeout=args.max_wait if args.max_wait > 0 else None)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
        print("RESULT: TIMEOUT (cdb %d 秒内未退出，已强杀)" % args.max_wait, flush=True)
        return 3
    print("CDB-EXIT: %d" % p.returncode, flush=True)
    print("DUMP-CREATED: %s (%s)" % ("yes" if dmp.exists() else "no", dmp),
          flush=True)
    print("CDB-LOG: %s" % cdblog, flush=True)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description="silent-audio-stall 前向取证装置（真实退出码 + 崩溃 dump）")
    ap.add_argument("--exe", required=True, help="目标可执行文件")
    ap.add_argument("--args", nargs="*", default=[], help="目标命令行参数")
    ap.add_argument("--tag", default="forensics", help="输出文件名前缀")
    ap.add_argument("--ctrlc-after", type=float, default=None,
                    help="N 秒后向目标控制台发送 CTRL_C_EVENT（优雅关停样本）")
    ap.add_argument("--terminate-after", type=float, default=None,
                    help="N 秒后 TerminateProcess(0xFFFFFFFF) 强杀（同 Stop-Process -Force）")
    ap.add_argument("--max-wait", type=int, default=300,
                    help="最长等待秒数（0=无限；默认 300）")
    ap.add_argument("--cdb", action="store_true",
                    help="cdb 调试器模式：second-chance 崩溃自动 .dump + 调用栈")
    ap.add_argument("--cdb-path", default=DEFAULT_CDB, help="cdb.exe 路径")
    ap.add_argument("--logdir", default=None, help="日志输出目录（默认任务 logs/）")
    args = ap.parse_args()

    if args.logdir:
        logdir = Path(args.logdir)
    else:
        logdir = (Path(__file__).resolve().parent.parent
                  / "docs" / "tasks" / "silent-audio-stall" / "logs")
    logdir.mkdir(parents=True, exist_ok=True)

    print("FORENSICS-MODE: %s" % ("cdb" if args.cdb else "direct"), flush=True)
    print("TARGET-EXE: %s" % args.exe, flush=True)
    if args.cdb:
        return run_cdb(args, logdir)
    return run_direct(args, logdir)


if __name__ == "__main__":
    sys.exit(main())
