#include "diagnostics.h"
#include "write_queue.h"

#include <cstdlib>

// ============================================================
// A32: 诊断基础设施实现（JSON 手工拼装，零第三方依赖）
// ============================================================

const char* CloseReasonName(CloseReason r) {
    switch (r) {
    case CloseReason::peer_closed:   return "peer_closed";
    case CloseReason::write_stall:   return "write_stall";
    case CloseReason::write_error:   return "write_error";
    case CloseReason::read_error:    return "read_error";
    case CloseReason::read_overflow: return "read_overflow";
    case CloseReason::server_stop:   return "server_stop";
    }
    return "unknown";
}

uint64_t NextSessionId() {
    static std::atomic<uint64_t> g_nextId{1};
    return g_nextId.fetch_add(1, std::memory_order_relaxed);
}

// ---- FaultInjection 读检查点 ----

bool FaultInjection::InjectWriteStall(uint64_t sessionId) {
    if (m_forceWriteStallAll.load(std::memory_order_relaxed)) return true;
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_forceWriteStallSessions.count(sessionId) != 0;  // 持续注入，命中不清除
}

bool FaultInjection::InjectWriteError(uint64_t sessionId) {
    // 全局开关一次性：命中即清除
    if (m_injectWriteErrorAll.exchange(false, std::memory_order_relaxed)) return true;
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_injectWriteErrorSessions.find(sessionId);
    if (it != m_injectWriteErrorSessions.end()) {
        m_injectWriteErrorSessions.erase(it);  // 一次性：命中即清除
        return true;
    }
    return false;
}

bool FaultInjection::InjectReadError(uint64_t sessionId) {
    if (m_injectReadErrorAll.exchange(false, std::memory_order_relaxed)) return true;
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_injectReadErrorSessions.find(sessionId);
    if (it != m_injectReadErrorSessions.end()) {
        m_injectReadErrorSessions.erase(it);  // 一次性：命中即清除
        return true;
    }
    return false;
}

bool FaultInjection::IsPaused(uint64_t sessionId) const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_pauseStreamSessions.count(sessionId) != 0;
}

// ---- FaultInjection 写端点 ----

bool FaultInjection::Set(const std::string& key, const std::string& value,
                         const std::optional<uint64_t>& sessionId, std::string& error) {
    const bool on = (value == "1" || value == "true" || value == "on");

    if (key == "stall_threshold_ms") {
        char* end = nullptr;
        const unsigned long v = std::strtoul(value.c_str(), &end, 10);
        if (end == value.c_str() || v < 100 || v > 3600000UL) {
            error = "invalid stall_threshold_ms (expected 100..3600000)";
            return false;
        }
        m_stallThresholdMs.store(static_cast<uint32_t>(v), std::memory_order_relaxed);
        return true;
    }
    if (key == "read_message_max") {
        char* end = nullptr;
        const unsigned long v = std::strtoul(value.c_str(), &end, 10);
        if (end == value.c_str() || v < 1024 || v > 100UL * 1024 * 1024) {
            error = "invalid read_message_max (expected 1024..104857600)";
            return false;
        }
        m_readMessageMax.store(static_cast<uint32_t>(v), std::memory_order_relaxed);
        return true;
    }
    if (key == "force_write_stall") {
        if (sessionId) {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (on) m_forceWriteStallSessions.insert(*sessionId);
            else    m_forceWriteStallSessions.erase(*sessionId);
        } else {
            m_forceWriteStallAll.store(on, std::memory_order_relaxed);
        }
        return true;
    }
    if (key == "inject_write_error") {
        if (sessionId) {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (on) m_injectWriteErrorSessions.insert(*sessionId);
            else    m_injectWriteErrorSessions.erase(*sessionId);
        } else {
            m_injectWriteErrorAll.store(on, std::memory_order_relaxed);
        }
        return true;
    }
    if (key == "inject_read_error") {
        if (sessionId) {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (on) m_injectReadErrorSessions.insert(*sessionId);
            else    m_injectReadErrorSessions.erase(*sessionId);
        } else {
            m_injectReadErrorAll.store(on, std::memory_order_relaxed);
        }
        return true;
    }
    if (key == "pause_stream") {
        if (!sessionId) {
            // 定向停流必须指定会话（§5.9.2：pause_stream 必须带 session=）
            error = "pause_stream requires session=<id>";
            return false;
        }
        std::lock_guard<std::mutex> lk(m_mtx);
        if (on) m_pauseStreamSessions.insert(*sessionId);
        else    m_pauseStreamSessions.erase(*sessionId);
        return true;
    }

    error = "unknown key";
    return false;
}

// ---- JSON 序列化（§5.8.1 字段；手工拼装）----

namespace {

inline std::string N(uint64_t v) { return std::to_string(v); }
inline std::string B(bool v) { return v ? "true" : "false"; }

} // namespace

std::string SerializeProbeJson(const std::vector<ProbeEntry>& entries,
                               const std::string& extraMembers,
                               const ProbeCounters& counters,
                               const ProbeConfig& cfg) {
    std::string s;
    s.reserve(512 + entries.size() * 256);

    s += "{\n";
    s += "  \"nowMs\": " + N(WqNowMs()) + ",\n";
    s += "  \"config\": { \"serverWriteStallMs\": " + N(cfg.serverWriteStallMs) +
         ", \"effectiveStallMs\": " + N(cfg.effectiveStallMs) +
         ", \"stallGraceMs\": " + N(cfg.stallGraceMs) +
         ", \"maxBinaryFrames\": " + N(cfg.maxBinaryFrames) +
         ", \"maxQueuedBytes\": " + N(cfg.maxQueuedBytes) +
         ", \"maxTextFrames\": " + N(cfg.maxTextFrames) + " },\n";

    s += "  \"sessions\": [";
    for (size_t i = 0; i < entries.size(); i++) {
        const ProbeEntry& e = entries[i];
        if (i) s += ",";
        s += "\n    { \"id\": " + N(e.id) +
             ", \"ssl\": " + B(e.ssl) +
             ", \"closed\": " + B(e.closed) +
             ", \"stalled\": " + B(e.stalled) +
             ", \"writing\": " + B(e.writing) +
             ", \"idleMs\": " + N(e.idleMs) +
             ", \"thresholdMs\": " + N(e.thresholdMs) +
             ", \"queue\": { \"depth\": " + N(e.queueDepth) +
             ", \"binary\": " + N(e.queueBinary) +
             ", \"text\": " + N(e.queueText) +
             ", \"bytes\": " + N(e.queueBytes) + " }" +
             ", \"counters\": { \"enqueued\": " + N(e.enqueued) +
             ", \"dequeued\": " + N(e.dequeued) +
             ", \"droppedTotal\": " + N(e.droppedTotal) + " }" +
             ", \"streaming\": " + B(e.streaming) + " }";
    }
    s += "\n  ],\n";

    // m_probeHook 的顶层成员序列（顺序获取 L2 之后产生，严禁 L1→L2 嵌套）
    if (!extraMembers.empty()) {
        s += "  " + extraMembers + ",\n";
    }

    s += "  \"counters\": { \"sessions\": " + N(counters.sessions) +
         ", \"liveSessions\": " + N(static_cast<uint64_t>(counters.liveSessions)) +
         ", \"forceClosed\": " + N(counters.forceClosed) +
         ", \"writeStalls\": " + N(counters.writeStalls) + " }\n";
    s += "}\n";
    return s;
}
