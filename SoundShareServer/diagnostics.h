#pragma once

// ============================================================
// A31: 诊断基础设施（M-2 / M-3 / M-7）
//   - CloseReason（A5）：强制清理原因枚举（peer_closed / server_stop 静默）
//   - ProbeEntry（A5/A31）：探针条目（POD，FillProbe 填充）
//   - FaultInjection（★唯一形态 = HttpServer 成员；不存在单例，M-3）
//   - NextSessionId()（M-7：单调递增会话序号，非指针地址）
//   - JSON 序列化声明（A32，diagnostics.cpp 手工拼装，零第三方依赖）
//
// 双开关（M-7，A33）：
//   SOUNDSHARE_ENABLE_DIAGNOSTICS       读（/_debug/sessions + stdin dump +
//                                       DumpProbeJson），默认 ON
//   SOUNDSHARE_ENABLE_FAULT_INJECTION   写（/_debug/fault/set + session 侧
//                                       注入检查点），默认 OFF；关闭时端点
//                                       编译剔除 → 返回 404，注入检查点零开销
// ============================================================

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

// ---- A5: 强制清理原因 ----
enum class CloseReason {
    peer_closed,    // 对端正常 close/eof → ForceClose 全程静默（§6.3）
    write_stall,    // 写看门狗命中 → E-WS-WRITE-STALL + E-WS-FORCE-CLOSE
    write_error,    // async_write 失败 → E-WS-WRITE-FAIL + E-WS-FORCE-CLOSE
    read_error,     // async_read 失败（非 closed/eof）→ E-WS-READ-FAIL + E-WS-FORCE-CLOSE
    read_overflow,  // 读消息超过 read_message_max → E-WS-READ-INVALID + E-WS-FORCE-CLOSE
    server_stop,    // HttpServer::Stop() 关停路径 → 静默（§6.3，关停不是异常）
};

const char* CloseReasonName(CloseReason r);

// 异常原因（打日志）；peer_closed / server_stop 静默（§6.3 静默规则 / F-4）
inline bool IsExceptionReason(CloseReason r) {
    switch (r) {
    case CloseReason::write_stall:
    case CloseReason::write_error:
    case CloseReason::read_error:
    case CloseReason::read_overflow:
        return true;
    default:
        return false;
    }
}

// ---- A5/A31: 探针条目（POD）。FillProbe() 填充（硬约束见 http_server.h 基类注释）----
struct ProbeEntry {
    uint64_t id = 0;
    bool ssl = false;
    bool closed = false;
    bool stalled = false;      // 即时停滞判定（Stalled(now)）
    bool writing = false;      // 写所有权位
    uint64_t idleMs = 0;       // now - lastProgressMs
    uint32_t thresholdMs = 30000;  // 需求口径 T（非 T_eff）
    // 写队列快照
    uint32_t queueDepth = 0;
    uint32_t queueBinary = 0;
    uint32_t queueText = 0;
    uint64_t queueBytes = 0;
    // 每会话计数
    uint64_t enqueued = 0;
    uint64_t dequeued = 0;
    uint64_t droppedTotal = 0;
    // 流状态（A28b：SetStreaming 原子标记）
    bool streaming = false;
};

// ---- 探针 config 段（§5.8.1 JSON 示例）----
struct ProbeConfig {
    uint32_t serverWriteStallMs = 30000;  // T_server_write（需求口径）
    uint32_t effectiveStallMs = 29000;    // T_eff = T − G（D-15 / M-8）
    uint32_t stallGraceMs = 1000;         // G = min(1000, T/10)
    size_t maxBinaryFrames = 200;
    size_t maxQueuedBytes = 8 << 20;
    size_t maxTextFrames = 16;
};

// ---- 服务端级计数（O-v2-2 口径）----
//   liveSessions 只由会话构造 ++ / 析构 -- 维护（OnSessionCreated /
//   OnSessionDestroyed）；forceClosed / writeStalls 是独立事件计数，
//   不得重复增减 liveSessions（否则 §8.12"liveSessions − sessions 恒 0"失效）。
struct ProbeCounters {
    size_t sessions = 0;        // m_sessions.size()（持 L1 期间快照）
    int64_t liveSessions = 0;   // 已构造 − 已析构（R-12 兜底；与 sessions 差值 = 僵尸会话数）
    uint64_t forceClosed = 0;   // ForceClose 执行次数（全部原因，含 server_stop）
    uint64_t writeStalls = 0;   // 写看门狗命中次数
};

// ---- A31: 故障注入（运行期开关，全部原子/受 m_mtx 保护；默认全关）----
// 全部基于应用层原语（NFR-WIN-4：禁 iptables/tc 等 Linux 专属手段）。
class FaultInjection {
public:
    static constexpr uint32_t kDefaultStallThresholdMs = 30000;
    static constexpr uint32_t kDefaultReadMessageMax = 65536;

    // ---- 读检查点（session 侧调用；注入 OFF 构建下调用点被编译剔除）----
    // force_write_stall：持续注入（命中不清除，直至 value=0 或会话终结）
    bool InjectWriteStall(uint64_t sessionId);
    // inject_write_error / inject_read_error：一次性（命中即自动清除）
    bool InjectWriteError(uint64_t sessionId);
    bool InjectReadError(uint64_t sessionId);
    // pause_stream：持续（该会话 SendBinary 直接丢弃，连接保持，AC-05/06 备选注入）
    bool IsPaused(uint64_t sessionId) const;

    uint32_t StallThresholdMs() const {
        return m_stallThresholdMs.load(std::memory_order_relaxed);
    }
    uint32_t ReadMessageMax() const {
        return m_readMessageMax.load(std::memory_order_relaxed);
    }

    // ---- 采集侧注入开关（T-GPU-AUDIO-GLITCH NFR-TEST-1 级1/2 + C3；默认全关）----
    // 读检查点在 main.cpp 采集回调内（SOUNDSHARE_ENABLE_FAULT_INJECTION=OFF 时
    // 编译剔除）；成员本身保留（探针 injection 段两种构建都可读，恒 0）。
    uint32_t CaptureStallMs() const {
        return m_captureStallMs.load(std::memory_order_relaxed);
    }
    uint32_t CaptureStallProbPpm() const {
        return m_captureStallProbPpm.load(std::memory_order_relaxed);
    }
    uint32_t CaptureBatchMs() const {
        return m_captureBatchMs.load(std::memory_order_relaxed);
    }
    uint32_t CaptureSkipPackets() const {
        return m_captureSkipPackets.load(std::memory_order_relaxed);
    }
    // capture_skip_packets 一次性计数语义：原子递减；返回 true = 本 packet 丢弃内容
    // （仍正常 ReleaseBuffer ⇒ 不产生 DATA_DISCONTINUITY；P1-4 语义边界）
    bool ConsumeCaptureSkipPacket() {
        uint32_t v = m_captureSkipPackets.load(std::memory_order_relaxed);
        while (v > 0 &&
               !m_captureSkipPackets.compare_exchange_weak(
                   v, v - 1, std::memory_order_relaxed, std::memory_order_relaxed)) {}
        return v > 0;
    }

    // ---- 写端点 /_debug/fault/set?key=..&value=..[&session=N] ----
    // session 参数为会话序号（M-7，非指针）。返回 false 时 error 给出原因。
    bool Set(const std::string& key, const std::string& value,
             const std::optional<uint64_t>& sessionId, std::string& error);

private:
    // 全局开关（无 session 参数时设置）
    std::atomic<bool> m_forceWriteStallAll{false};
    std::atomic<bool> m_injectWriteErrorAll{false};
    std::atomic<bool> m_injectReadErrorAll{false};
    std::atomic<uint32_t> m_stallThresholdMs{kDefaultStallThresholdMs};
    std::atomic<uint32_t> m_readMessageMax{kDefaultReadMessageMax};

    // 定向开关（带 session 参数；读检查点频率低，锁开销可忽略）
    mutable std::mutex m_mtx;
    std::set<uint64_t> m_forceWriteStallSessions;
    std::set<uint64_t> m_injectWriteErrorSessions;
    std::set<uint64_t> m_injectReadErrorSessions;
    std::set<uint64_t> m_pauseStreamSessions;

    // 采集侧注入开关（全局，无 session 维度；默认全关）
    std::atomic<uint32_t> m_captureStallMs{0};        // 0..500
    std::atomic<uint32_t> m_captureStallProbPpm{0};   // 0..1000000
    std::atomic<uint32_t> m_captureBatchMs{0};        // 0..500
    std::atomic<uint32_t> m_captureSkipPackets{0};    // 一次性计数（逐 packet 递减）
};

// ---- M-7: 会话序号（进程内单调递增 uint64，不输出指针地址）----
uint64_t NextSessionId();

// ---- A32: JSON 序列化（diagnostics.cpp 手工拼装）----
// extraMembers：m_probeHook 返回的顶层 JSON 成员序列（不含外层大括号），
// 原样并入顶层对象（如 `"streaming": { "count": 1 }, "captureCallbacks": 9876`）。
std::string SerializeProbeJson(const std::vector<ProbeEntry>& entries,
                               const std::string& extraMembers,
                               const ProbeCounters& counters,
                               const ProbeConfig& cfg);
