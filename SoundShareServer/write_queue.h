#pragma once

// ============================================================
// A30: 每会话写队列 + 写所有权协议（§5.4.4, M-1, P0）
//      + 自适应 deadline 写看门狗（§5.1 / §5.1.2, M-8）
//
// Plain / Ssl 会话各持一份（D-07）→ NFR-PROTO-3 对两种子类无差别生效。
//
// 锁序约定（§5.4.2）: 本类的 Mutex() 即 L3（叶锁）。
//   【持 L3 期间禁止】获取其它锁（L1 = HttpServer::m_sessionsMutex /
//   L2 = main.cpp streamingMutex）、写日志、发起 IO、任何可能阻塞的调用。
//   L3 的入边只允许 L1（探针 FillProbe → GetSnapshot）与 L2（采集线程
//   SendBinary → EnqueueLocked）；出边为空 → 无环。
//
// 写所有权协议不变式（I-W1~I-W6，reviewer 可逐条 grep 核对）:
//   I-W1  writing==true ⟺ 存在且仅存在一份"写所有权"：
//         要么有一次在途 async_write，要么有一个已 dispatch/内联排队、
//         尚未执行的 DoWrite() 任务。
//   I-W2  writing 的 false→true 只发生在 EnqueueLocked 的 L3 临界区内
//         （唯一取得点）；true→false 只发生在 TakeNextLocked 返回空的
//         同一 L3 临界区内（唯一归还点）。
//   I-W3  std::deque 的 push_back/pop_front/erase/front/empty 只出现在
//         持 L3 的代码中（本文件全部 *Locked 方法均要求调用方已持锁）；
//         锁外只允许 m_ws.text/binary/async_write（http_server.cpp）。
//   I-W4  全仓 async_write 唯一站点 = BeginWrite()（http_server.cpp，
//         O-v2-1 口径），且只在会话 strand 上运行。
//   I-W5  async_write / async_read / ForceClose 的 shutdown+close /
//         stall timer handler 全部在同一会话 strand。
//   I-W6  pending > 0 ⟹ writing == true（TakeNextLocked 与 EnqueueLocked
//         同受 L3 保护）。
//
// v1 错误写法留档（§5.4.4 末尾，防止回退）：
//   v1 曾写 `bool kick = !writing.exchange(true); unlock; if (kick) doWrite();`
//   并让 OnWrite 出锁后调 DoWrite() —— DoWrite 在锁外访问 deque 与采集线程
//   的 push_back 构成数据竞争，且 writing=false 与采集线程 exchange(true)
//   竞争可产生两条并发 async_write（Beast 禁止）。本文件以
//   EnqueueLocked（锁内取得）/ TakeNextLocked（锁内归还）彻底消除。
// ============================================================

#include <boost/asio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace net = boost::asio;
// minimal asio-only dependency: use boost::system::error_code instead of beast::error_code
using boost::system::error_code;

// Message wrapper that explicitly tracks text vs binary mode
// （原 http_server.h 定义移入此处，随队列一起管理）
struct WriteMessage {
    std::vector<uint8_t> data;
    bool isText = false;
};

// A6: 队列配置（§5.5；值与原 MAX_QUEUE_DEPTH=200 保持一致，§11-2 定稿）
struct QueueConfig {
    size_t maxBinaryFrames = 200;       // 二进制帧硬上限
    size_t maxQueuedBytes  = 8 << 20;   // 8MB 内存天花板（NFR-PERF-2）
    size_t maxTextFrames   = 16;        // 文本（控制信令 audio_config 等）上限
    uint32_t dropLogIntervalMs = 30000; // 丢弃汇总日志节流窗口（NFR-LOG-2）
};

inline uint64_t WqNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

class WriteQueue {
public:
    // 有效阈值 T_eff = T − G，G = min(1000ms, T/10)（D-15 / M-8）。
    // 默认 T=30000 → G=1000 → T_eff=29000；加速档 T=3000 → T_eff=2700。
    // 判定时刻 = lastProgress + T_eff + ε ≤ T（ε ≪ 1s）→ AC-01 墙钟 ≤30s 严格自洽。
    static uint32_t EffectiveThresholdMs(uint32_t thresholdMs) {
        const uint32_t g = std::min<uint32_t>(1000u, thresholdMs / 10u);
        return (thresholdMs > g) ? (thresholdMs - g) : 1u;
    }

    // timer 绑定会话 strand（m_ws.get_executor()），与 async_read/write 同
    // strand（I-W5）。成员声明顺序：会话类中 m_wq 必须位于 m_ws 之后。
    explicit WriteQueue(const net::any_io_executor& ex)
        : m_timer(ex) {}

    // ---- L3（叶锁）。以下 *Locked 方法要求调用方已持锁 ----
    std::mutex& Mutex() { return m_mtx; }

    // 入队（任意线程：采集线程 / io 线程 / HTTP strand；调用方持 L3）。
    // 返回 true = 本次由 false→true 取得写所有权（★ I-W2 唯一取得点），
    // 调用方须把 DoWrite 投递到会话 strand（net::dispatch）；
    // 返回 false = 已有写者（或新帧被丢弃），调用方什么都不做。
    bool EnqueueLocked(bool isText, const void* data, size_t size) {
        auto msg = std::make_shared<WriteMessage>();
        const uint8_t* p = static_cast<const uint8_t*>(data);
        msg->data.assign(p, p + size);
        msg->isText = isText;

        if (isText) {
            // 文本也设上限 → 无界增长路径彻底消灭（FR-06 / D-03 A）
            while (m_textCount >= m_cfg.maxTextFrames) {
                DropOldestTextLocked();
            }
        } else {
            // O-v2-3 守卫 1：单帧自身超过字节上限 → 直接丢弃新帧（不入队，
            // 避免"丢光旧帧仍放不下"的死循环）。当前帧 ~3.8KB 不可达，防御性。
            if (size > m_cfg.maxQueuedBytes) {
                m_droppedTotal.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            // 丢最旧 binary 保持实时性（与原 pop_front 语义一致，D-03 A）。
            // O-v2-3 守卫 2：binaryCount==0 时退出（无可丢的旧帧）。
            while ((m_binaryCount >= m_cfg.maxBinaryFrames ||
                    m_queuedBytes + size > m_cfg.maxQueuedBytes) &&
                   m_binaryCount > 0) {
                DropOldestBinaryLocked();
            }
        }

        m_q.push_back(msg);
        m_queuedBytes += size;
        if (isText) m_textCount++; else m_binaryCount++;
        m_pending.store(m_q.size(), std::memory_order_relaxed);
        m_enqueued.fetch_add(1, std::memory_order_relaxed);

        if (!m_writing.load(std::memory_order_relaxed)) {
            m_writing.store(true, std::memory_order_relaxed);  // ★ I-W2 唯一取得点
            return true;
        }
        return false;
    }

    // 取下一条待写消息（仅会话 strand；调用方持 L3）。
    // 队列空时在同一临界区归还写所有权（true→false，★ I-W2 唯一归还点）
    // 并返回 nullptr —— 取消息与归还不留窗口（M-1）。
    std::shared_ptr<WriteMessage> TakeNextLocked(uint64_t nowMs) {
        if (m_q.empty()) {
            m_writing.store(false, std::memory_order_relaxed);  // ★ 唯一归还点
            m_pending.store(0, std::memory_order_relaxed);
            return nullptr;
        }
        auto m = m_q.front();
        m_q.pop_front();
        if (m->isText) m_textCount--; else m_binaryCount--;
        m_queuedBytes -= m->data.size();
        m_pending.store(m_q.size(), std::memory_order_relaxed);
        m_dequeued.fetch_add(1, std::memory_order_relaxed);
        m_lastProgressMs.store(nowMs, std::memory_order_relaxed);  // 更新点 ①（§5.1.1）
        return m;
    }

    // 写成功进展（仅会话 strand；OnWrite 成功回调内，与下一次
    // TakeNextLocked 在同一 L3 临界区中先后调用，§5.7.2）
    void OnProgressLocked(uint64_t nowMs) {
        m_lastProgressMs.store(nowMs, std::memory_order_relaxed);  // 更新点 ②（§5.1.1）
    }

    // 队列配置（E-WS-QUEUE-DROP 日志输出 maxFrames/maxBytes 用）
    const QueueConfig& Config() const { return m_cfg; }

    // ---- 探针快照（持 L1 期间由 FillProbe 调用；内部短暂取 L3。
    //      L1→L3 为允许嵌套（M-2），L3 是叶锁不成环。★ 严禁触碰 m_ws）----
    struct Snapshot {
        bool writing = false;
        size_t depth = 0;
        size_t binary = 0;
        size_t text = 0;
        uint64_t bytes = 0;
        uint64_t enqueued = 0;
        uint64_t dequeued = 0;
        uint64_t droppedTotal = 0;
    };
    Snapshot GetSnapshot() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        Snapshot s;
        s.writing = m_writing.load(std::memory_order_relaxed);
        s.depth = m_q.size();
        s.binary = m_binaryCount;
        s.text = m_textCount;
        s.bytes = m_queuedBytes;
        s.enqueued = m_enqueued.load(std::memory_order_relaxed);
        s.dequeued = m_dequeued.load(std::memory_order_relaxed);
        s.droppedTotal = m_droppedTotal.load(std::memory_order_relaxed);
        return s;
    }

    // ---- 停滞判定（读原子，无需持锁；§5.1.1 判定信号）----
    // stalled ⟺ (writing || pending>0) && (now - lastProgressMs) >= T_eff
    // 前置条件保证空闲会话（未 start / 无积压）永不判定（R-02 / AC-11a / AC-08）。
    bool Stalled(uint64_t nowMs) const {
        if (!(m_writing.load(std::memory_order_relaxed) ||
              m_pending.load(std::memory_order_relaxed) > 0)) {
            return false;
        }
        return (nowMs - m_lastProgressMs.load(std::memory_order_relaxed)) >=
               EffectiveThresholdMs(m_thresholdMs.load(std::memory_order_relaxed));
    }
    uint64_t LastProgressMs() const { return m_lastProgressMs.load(std::memory_order_relaxed); }
    uint32_t ThresholdMs() const { return m_thresholdMs.load(std::memory_order_relaxed); }
    bool IsWriting() const { return m_writing.load(std::memory_order_relaxed); }
    size_t Pending() const { return m_pending.load(std::memory_order_relaxed); }

    // ---- 丢弃日志节流（E-WS-QUEUE-DROP：首次立即 1 行，之后每 30s 至多 1 行；
    //      出 L3 之后由 Send/SendBinary 调用。并发调用最坏多打一行，无正确性影响）----
    struct DropLogEvent {
        uint64_t droppedInWindow = 0;
        uint64_t droppedTotal = 0;
    };
    bool TakeDropLogEvent(uint64_t nowMs, DropLogEvent& out) {
        const uint64_t total = m_droppedTotal.load(std::memory_order_relaxed);
        const uint64_t sinceLog =
            total - m_droppedAtLastLog.load(std::memory_order_relaxed);
        if (sinceLog == 0) return false;
        const bool first = !m_dropLoggedOnce.load(std::memory_order_relaxed);
        const uint64_t last = m_lastDropLogMs.load(std::memory_order_relaxed);
        if (!first && (nowMs - last) < m_cfg.dropLogIntervalMs) return false;  // 窗口内静默
        m_dropLoggedOnce.store(true, std::memory_order_relaxed);
        m_lastDropLogMs.store(nowMs, std::memory_order_relaxed);
        m_droppedAtLastLog.store(total, std::memory_order_relaxed);
        out.droppedInWindow = sinceLog;
        out.droppedTotal = total;
        return true;
    }

    // ---- 看门狗（Start/Stop 仅会话 strand；OnTimer 经 timer 在会话 strand 回调）----
    // sessionKeeper：会话的 shared_from_this()（以 shared_ptr<void> 形态持有）。
    //   存为 m_keeper —— 与 m_onStall 同属"m_wq 持有的会话强引用"，随
    //   ClearCallbacks 一并释放（P0-1 断环统一化）；timer handler 以其 weak
    //   副本做存活性检查（见 ArmTimer：会话析构后不再触碰 this）。
    // onStall：判定命中回调（→ session::OnWriteStall → ForceClose(write_stall)）。
    // getThresholdMs：返回当前 T_server_write（默认构建传 nullptr → 恒 30000；
    //   注入构建下读 Faults().StallThresholdMs()，每个窗口刷新一次）。
    void Start(std::shared_ptr<void> sessionKeeper,
               std::function<void()> onStall,
               std::function<uint32_t()> getThresholdMs) {
        m_keeper = std::move(sessionKeeper);
        m_onStall = std::move(onStall);
        m_getThreshold = std::move(getThresholdMs);
        if (m_getThreshold) {
            m_thresholdMs.store(m_getThreshold(), std::memory_order_relaxed);
        }
        m_timerActive.store(true, std::memory_order_relaxed);
        ArmTimer();
    }

    // R2 闭环（P0-1 断环统一化）：清空 onStall/getThreshold 回调并释放
    // m_keeper —— 消灭 m_wq 持有的全部会话强引用（打破 会话→m_wq→m_onStall→
    // 会话 循环 —— 不清则 ForceClose 的 m_self.reset() 后对象永不析构，
    // liveSessions 永不归零）。m_keeper 释放后，仍在途/已排队的 timer
    // handler 的 weak 存活检查立即生效（见 ArmTimer）：析构后不触碰 this。
    // 调用语境（二选一，均无并发）：
    //   ① ForceClose 步骤 3b（reason != write_stall）：会话 strand 或关停期
    //      join 后的主线程，且不在 m_onStall 调用栈内 → 同步调用安全；
    //   ② OnWriteStall 末尾 net::post 到 strand 的延迟任务：write_stall 路径
    //      正处于 m_onStall 执行栈内，同步销毁正在执行的 std::function 属
    //      UB → 必须经 post 延迟（析构发生在该 post handler 栈上）。
    void ClearCallbacks() {
        m_onStall = nullptr;
        m_getThreshold = nullptr;
        m_keeper.reset();
    }

    void Stop() {
        m_timerActive.store(false, std::memory_order_relaxed);
        // Boost 1.92: steady_timer::cancel() has no error_code overload;
        // use the no-arg version. try/catch as defensive guard (R-03).
        try { m_timer.cancel(); } catch (...) {}
    }

private:
    // 以下两个 Drop* 仅在持 L3 时调用（I-W3）。
    // 队列前端 text 至多 maxTextFrames(16) 条，扫描代价可忽略。
    void DropOldestBinaryLocked() {
        for (auto it = m_q.begin(); it != m_q.end(); ++it) {
            if (!(*it)->isText) {
                m_queuedBytes -= (*it)->data.size();
                m_binaryCount--;
                m_q.erase(it);
                m_droppedTotal.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    }
    void DropOldestTextLocked() {
        for (auto it = m_q.begin(); it != m_q.end(); ++it) {
            if ((*it)->isText) {
                m_queuedBytes -= (*it)->data.size();
                m_textCount--;
                m_q.erase(it);
                m_droppedTotal.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    }

    // 自适应 deadline 定时（D-01 A-3）。只在会话 strand 上调用。
    void ArmTimer() {
        if (!m_timerActive.load(std::memory_order_relaxed)) return;
        const uint64_t now = WqNowMs();
        // 更新点 ③（§5.1.1）：空闲 ⇒ 无停滞可判，重置基准。
        // ① 吸收空闲期（否则空闲会话 idle 持续累积，一旦开始推流立即误杀）；
        // ② 保持空闲会话 timer 频率仍为 1 次/T_eff（无自旋）。
        if (!(m_writing.load(std::memory_order_relaxed) ||
              m_pending.load(std::memory_order_relaxed) > 0)) {
            m_lastProgressMs.store(now, std::memory_order_relaxed);
        }
        const uint32_t tEff =
            EffectiveThresholdMs(m_thresholdMs.load(std::memory_order_relaxed));
        const uint64_t idle = now - m_lastProgressMs.load(std::memory_order_relaxed);
        const uint64_t wait = (idle >= tEff) ? 1 : (tEff - idle);  // 恒 >0，不可能自旋
        m_timer.expires_after(std::chrono::milliseconds(wait));
        // P0-1 收尾（析构语义复核，审查报告期望改法第 3 点）：handler 捕获
        // m_keeper 的 weak 副本 —— 会话析构（ClearCallbacks 已释放 m_keeper
        // 且无其它强引用）后，已排队/在途的 timer handler 直接 return，不
        // 触碰已析构的 this。修复前"会话永不析构"（泄漏）掩盖了该悬垂；
        // P0-1 使析构必然发生，若无此检查，OnTimer(非 aborted) 将悬垂读
        // m_timerActive/m_onStall（间歇性 UAF：timer 正常 fire 与
        // ForceClose→析构 的竞争窗口，加速档 T=5000 下实测复现过进程死亡）。
        m_timer.async_wait([weakKeeper = std::weak_ptr<void>(m_keeper), this](error_code ec) {
            if (weakKeeper.expired()) return;
            OnTimer(ec);
        });
    }

    void OnTimer(error_code ec) {
        if (ec == net::error::operation_aborted) return;  // Stop() / 重 arm 引起
        if (!m_timerActive.load(std::memory_order_relaxed)) return;
        const uint64_t now = WqNowMs();
        if (Stalled(now)) {
            m_timerActive.store(false, std::memory_order_relaxed);
            if (m_onStall) m_onStall();  // → ForceClose(write_stall)；会话已终结，不再 arm
            return;
        }
        if (m_getThreshold) {  // 运行期注入加速（stall_threshold_ms）下一窗口生效
            m_thresholdMs.store(m_getThreshold(), std::memory_order_relaxed);
        }
        ArmTimer();  // 未命中 → 睡到"刚好到期"
    }

    QueueConfig m_cfg;

    mutable std::mutex m_mtx;  // ★ L3（叶锁）
    std::deque<std::shared_ptr<WriteMessage>> m_q;  // 仅持 L3 访问（I-W3）
    size_t m_binaryCount = 0;
    size_t m_textCount = 0;
    uint64_t m_queuedBytes = 0;

    std::atomic<bool>     m_writing{false};       // 写所有权位（I-W1/I-W2）
    std::atomic<size_t>   m_pending{0};           // q.size() 原子镜像（探针/判定）
    std::atomic<uint64_t> m_lastProgressMs{0};    // 最近一次"有进展"时刻

    std::atomic<uint64_t> m_enqueued{0};
    std::atomic<uint64_t> m_dequeued{0};
    std::atomic<uint64_t> m_droppedTotal{0};

    // 丢弃日志节流状态（仅出 L3 后的 TakeDropLogEvent 访问）
    std::atomic<uint64_t> m_lastDropLogMs{0};
    std::atomic<uint64_t> m_droppedAtLastLog{0};
    std::atomic<bool>     m_dropLoggedOnce{false};

    net::steady_timer m_timer;  // 绑会话 strand（I-W5）
    std::atomic<bool> m_timerActive{false};
    std::atomic<uint32_t> m_thresholdMs{30000};
    std::function<void()> m_onStall;
    std::function<uint32_t()> m_getThreshold;
    // 会话强引用（Start 注入，随 ClearCallbacks 释放 —— P0-1 断环点之一；
    // timer handler 以其 weak 副本检测会话存活性）
    std::shared_ptr<void> m_keeper;
};
