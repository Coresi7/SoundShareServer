# tests/ — 验收脚手架（手动驱动，**非 CTest 回归套件**）

> ★ M-12 交付标注：本目录下的脚本是为 `docs/tasks/silent-audio-stall/`
> 验收（AC-01 ~ AC-11）手动驱动的工具，**不接入 CTest、不在 CI 中运行**。
> 仓库基线为 0 测试（无 enable_testing/add_test/gtest/catch2），这些脚本
> 不改变该基线，仅提供可控故障注入与按需观测（NFR-TEST-1）。
>
> 使用前需 `pip install websockets`（开发机已装 16.0，Python 3.14）。

## 脚本清单

| 脚本 | 用途 | 关键模式 |
|---|---|---|
| `stub_client.py` | T1 桩客户端 | `normal` / `no-read`（真实 TCP 零窗口，AC-01/03 首选注入）/ `silent`（AC-11a）/ `garbage --size=N`（AC-11b/c）/ `legacy`（AC-08） |
| `mock_ws_server.py` | T2 mock 服务端 | 发 `audio_config` + `--pcm-seconds=N` 秒 PCM 后**静默保持连接**（不发 FIN/RST）→ AC-05/06 的首选注入（不依赖注入构建） |
| `probe.py` | T3 探针 | 轮询 `GET /_debug/sessions`；`--watch N` 持续采样；`--json` 原始输出 |
| `stress_lifecycle.py` | §8.12 生命周期压测 | 4 并发会话（2 normal + 2 no-read）+ 连续 100 次"连接-start-3s-硬断"；断言 `liveSessions - sessions` 恒 0 |
| `forward_forensics.py` | **前向取证装置（复核轮 B-01）** | 见下节"前向取证装置" |
| `idle_log.py` | **T-CAPTURE-IDLE-LOG 主证据脚本（第 12 个测试，只增不减 11→12）** | `--tag baseline\|post\|s0v` × `--mode inject\|inject-idle(natural 别名)\|active`；内置 O-D1 三条反 vacuity 门禁 V-A1/V-A2/V-A3、G-1 W-A′ 语义（注入 ON + 无播放、自 spawn 起算 60s）、G-5 V-23 固化、G-7 启动污染防护（`loop_play` 先启）、G-8 `probe.py --assert-*` 链实跑、P1-2 观测顺序；stdout+stderr 双路合并行计数 |

## 构建前置（目录语义，复核轮 B-02 勘误后）

**构建目录语义（2026-09-24 起）**：

| 目录 | `SOUNDSHARE_ENABLE_FAULT_INJECTION` | 语义 |
|---|---|---|
| `build/` | **ON** | **注入构建（验收主构建）** |
| `build_off/` | OFF | 默认构建（对照） |
| `build_review_on/` | ON | 审查方自建（可复用，不属交付物） |

- 默认构建（`SOUNDSHARE_ENABLE_DIAGNOSTICS=ON`，`SOUNDSHARE_ENABLE_FAULT_INJECTION=OFF`）
  足以验证：**AC-01 / AC-02 / AC-03 / AC-05（T2 路径）/ AC-06（T2 路径）/ AC-08 / AC-11 / §8.12**。
- 需要 `inject_read_error` / `inject_write_error` / `pause_stream` / `read_message_max`
  的场景（**AC-09 / AC-10**、AC-05/06 的备选路径）**必须**以注入构建启动：
  ```
  cmake -S . -B build -D SOUNDSHARE_ENABLE_FAULT_INJECTION=ON
  cmake --build build --config Release
  ```
  默认（OFF）构建下 `/_debug/fault/set` 返回 404 —— 这是设计行为（M-7），不是缺陷。
  （复核轮 B-02 勘误：曾有一段时间 `build/` 缓存被重配为 OFF，2026-09-24 00:09 起已恢复
  语义 `build/` = 注入 ON，实测 `fault/set` → `{"ok":true}`；OFF 对照在 `build_off/`。）

## 前向取证装置（复核轮 B-01；tester 再现静默死亡时使用）

背景：R8 两次服务端静默死亡无法取证（本机 WER 全局禁用——WerSvc 停止且禁用、
`Disabled=1`——LocalDumps 不可用；且 PS 5.1 `Start-Process -Redirect*` 的 `.ExitCode`
即使 `WaitForExit()` 后也为空，旧脚本退出码全部丢失）。等效装置 =
`tests/forward_forensics.py`（python subprocess：`p.wait()` + `p.returncode`，可靠）：

```powershell
# 直接模式：真实退出码 + 自动判读（NORMAL / CRASH-NTSTATUS / TERMINATED / OTHER）
python tests\forward_forensics.py --exe build\Release\SoundShareServer.exe --args 8080 0 --tag repro

# cdb 模式：second-chance 未处理异常自动 .dump /ma 落盘（logs\dumps\）+ kb 40 调用栈
python tests\forward_forensics.py --exe build\Release\SoundShareServer.exe --args 8080 0 --tag repro-crash --cdb

# 装置在位自测（四样本，约 40s，全 PASS 即可用）
powershell -NoProfile -ExecutionPolicy Bypass -File docs\tasks\silent-audio-stall\logs\fix-b01-selftest.ps1
```

判读口径：`EXIT-CODE 0` = 正常（含 Ctrl+C 优雅关停）；`0xC0000005` 等 = **真崩溃**
（0xC0000005 访问冲突 / 0xC0000374 堆损坏 / 0xC0000409 fastfail）；`0xFFFFFFFF` =
TerminateProcess（外部强杀，如 Stop-Process -Force / taskkill /f）。工具还支持
`--ctrlc-after N`（定时优雅关停）与 `--terminate-after N`（定时强杀）两类样本注入。
依赖：Python 3.14 + WinDbg 商店版 cdb。

## 典型用法

```powershell
# AC-01 / AC-03：真实零窗口（需先在服务端机器上播放循环音频）
python tests/stub_client.py --port 8080 --mode no-read --duration 60
python tests/probe.py     --port 8080 --watch 40

# AC-05：客户端看门狗（mock 服务端 5 秒 PCM 后静默）
python tests/mock_ws_server.py --port 59997 --pcm-seconds 5
SoundShareClient.exe 127.0.0.1 59997

# AC-09 / AC-10（需注入构建）：注入后客户端再发任意一帧触发
curl "http://127.0.0.1:8080/_debug/fault/set?key=inject_read_error&value=1&session=7"

# §8.12 压测
python tests/stress_lifecycle.py --port 8080 --rounds 100
```

## M-13 实现护栏（gate §D7.4，勿移除）

1. `stub_client.py` **禁止 `max_queue=None`**：`websockets` 默认 16 帧 +
   `Assembler(pause=transport.pause_reading, ...)` 的流控是 `no-read` 模式产生
   真实 TCP 零窗口的唯一机制。
2. 所有脚本默认 `ping_interval=None, ping_timeout=None`：避免客户端在服务端
   写卡死后 ~t+40s 主动断开，与服务端 ~29s 判定产生 10s 竞态。
3. 仅用应用层手段（websockets / HTTP），无 iptables/tc/netem（NFR-WIN-4）。

## T-CAPTURE-IDLE-LOG 用法（idle_log.py，`build/` 为注入 ON 构建）

```powershell
# S0v（V-22/V-23，先于 baseline）：W-A′ 验证刷屏与 HEAD 一致 + 注入窗 timeout 实测固化
python tests\idle_log.py --tag s0v --mode inject-idle --exe-bin build\Release\SoundShareServer.exe
python tests\idle_log.py --tag s0v --mode inject     --exe-bin build\Release\SoundShareServer.exe

# baseline（S0 二进制留存于 build_idle_s0\，evidence 落 docs\tasks\T-CAPTURE-IDLE-LOG\evidence）
python tests\idle_log.py --tag baseline --mode inject-idle --exe-bin build_idle_s0\Release\SoundShareServer.exe
python tests\idle_log.py --tag baseline --mode inject     --exe-bin build_idle_s0\Release\SoundShareServer.exe
python tests\idle_log.py --tag baseline --mode active     --exe-bin build_idle_s0\Release\SoundShareServer.exe

# post（实现完成后重编 build\）
python tests\idle_log.py --tag post --mode inject-idle --exe-bin build\Release\SoundShareServer.exe
python tests\idle_log.py --tag post --mode inject     --exe-bin build\Release\SoundShareServer.exe
python tests\idle_log.py --tag post --mode active     --exe-bin build\Release\SoundShareServer.exe

# probe.py 断言链（G-8；episode 结束后、stats_reset 之前）
python tests\probe.py --port 8080 --assert-idle-episodes-ge 1 --assert-last-idle-ms 55000,65000 --assert-total-idle-ms 55000,65000
```

测试总数：**12**（原 11 + `idle_log.py`，只增不减）。
