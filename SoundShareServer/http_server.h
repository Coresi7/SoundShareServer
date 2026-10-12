#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <memory>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <functional>
#include <set>
#include <iostream>
#include <thread>
#include <optional>
#include <atomic>
#include <cstdint>

// A5: 诊断基础设施（CloseReason / ProbeEntry / FaultInjection / 序列化）
#include "diagnostics.h"
// A30: 写队列 + 写所有权协议（WriteMessage / QueueConfig / WriteQueue）
#include "write_queue.h"

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

// Forward declarations
class WebSocketSessionBase;
class HttpServer;

// Callback for when a client sends a message
using WsMessageCallback = std::function<void(WebSocketSessionBase*, const std::string&)>;
// Callback for client connect/disconnect
using WsConnectCallback = std::function<void(WebSocketSessionBase*, bool connected)>;

// ============================================================
// WebSocket Session Base Interface
// ============================================================
class WebSocketSessionBase {
public:
    virtual ~WebSocketSessionBase() = default;
    virtual void Send(const std::string& text) = 0;
    virtual void SendBinary(const void* data, size_t size) = 0;
    virtual void Close() = 0;
    virtual bool IsOpen() const = 0;

    // ---- A1: 新增纯虚接口 ----
    // 唯一清理入口（FR-02 终态：socket 关闭 + 两集合摘除 + 唯一一次断开语义，
    // CAS 幂等）。
    // ★ 调用线程契约 v2（§5.4.3）：仅 ① 该会话自身 strand（运行期）或
    //   ② HttpServer::Stop() join 全部 io 线程之后的主线程（关停期）。
    //   禁止从采集线程 / 主线程（运行期）/ 其它会话的 strand / HTTP session
    //   strand 调用。
    // ec：触发本次清理的原始错误码（仅用于 E-WS-WRITE-FAIL / E-WS-READ-FAIL
    // 日志文案，§6.3；默认空）。
    virtual void ForceClose(CloseReason r, beast::error_code ec = {}) = 0;
    // 会话序号（M-7：单调递增 uint64，非指针地址）
    virtual uint64_t Id() const = 0;
    // 探针填充（M-2）。★ 硬约束：
    //   - 调用方持 L1（HttpServer::m_sessionsMutex）期间执行；
    //   - 内部只允许取 L3（WriteQueue::GetSnapshot，叶锁）与原子读；
    //   - 严禁触碰 m_ws（调用方可能不在会话 strand 上，m_ws 属
    //     "Shared objects: Unsafe"）；严禁获取 L1/L2；严禁写日志。
    virtual void FillProbe(ProbeEntry& e) const = 0;
    // 流状态原子标记（A28b；探针输出 sessions[].streaming，
    // 避免"两个集合的裸指针做映射"这一 UAF 温床）
    virtual void SetStreaming(bool on) = 0;
};

// ============================================================
// Plain WebSocket Session (no SSL - for HTTP mode)
// ============================================================
class PlainWebSocketSession : public WebSocketSessionBase, public std::enable_shared_from_this<PlainWebSocketSession> {
public:
    PlainWebSocketSession(beast::tcp_stream&& stream, HttpServer* server);
    ~PlainWebSocketSession();

    void Run(http::request<http::string_body> req);
    void Send(const std::string& text) override;
    void SendBinary(const void* data, size_t size) override;
    void Close() override;
    bool IsOpen() const override;
    void ForceClose(CloseReason r, beast::error_code ec = {}) override;  // A1
    uint64_t Id() const override { return m_id; }     // A1
    void FillProbe(ProbeEntry& e) const override;     // A1
    void SetStreaming(bool on) override { m_streaming.store(on, std::memory_order_relaxed); }  // A1/A28b

private:
    void OnAccept(beast::error_code ec);
    void DoRead();
    void OnRead(beast::error_code ec, std::size_t bytes_transferred);
    void DoWrite();
    void OnWrite(beast::error_code ec, std::size_t bytes_transferred);
    // ★ I-W4（O-v2-1）：全仓唯一 async_write 站点，由 DoWrite 与 OnWrite
    //   成功续写分支共同调用；只能在会话 strand 上运行。
    void BeginWrite(const std::shared_ptr<WriteMessage>& msg);
    // E-WS-QUEUE-DROP 丢弃汇总日志（出 L3 后调用；30s 节流）
    void MaybeLogDrop();
    // 写看门狗命中回调（m_wq.Start 注册；会话 strand）→ ForceClose(write_stall)
    void OnWriteStall();

    websocket::stream<beast::tcp_stream> m_ws;
    beast::flat_buffer m_buffer;
    HttpServer* m_server;

    // ---- A2：写队列（m_wq 必须位于 m_ws 之后——其构造需 m_ws.get_executor()）----
    WriteQueue m_wq;

    // ---- A2：生命周期与探针状态 ----
    uint64_t m_id = 0;                       // 会话序号（NextSessionId 分配，M-7）
    // P1-1（§5.9.2 read_message_max 接线）：OnAccept 时从 Faults().ReadMessageMax()
    // 取会话级快照，作为 m_ws.read_message_max() 实际入参与 E-WS-READ-INVALID
    // 的 limitBytes 字段来源（默认构建恒 65536，与原硬编码 64*1024 行为一致）
    std::size_t m_readLimitBytes = FaultInjection::kDefaultReadMessageMax;
    std::atomic<bool> m_closed{false};       // ForceClose/析构竞争的同一 CAS 闸门（D-09 C）
    std::atomic<bool> m_streaming{false};    // 探针流状态标记（A28b）
    std::shared_ptr<PlainWebSocketSession> m_self;  // 自持引用（D-02 A；赋值在 OnAccept 的 ec 早退之后，M-4）
};

// ============================================================
// SSL WebSocket Session (for HTTPS mode)
// ============================================================
class SslWebSocketSession : public WebSocketSessionBase, public std::enable_shared_from_this<SslWebSocketSession> {
public:
    SslWebSocketSession(beast::ssl_stream<beast::tcp_stream>&& stream, HttpServer* server);
    ~SslWebSocketSession();

    void Run(http::request<http::string_body> req);
    void Send(const std::string& text) override;
    void SendBinary(const void* data, size_t size) override;
    void Close() override;
    bool IsOpen() const override;
    void ForceClose(CloseReason r, beast::error_code ec = {}) override;  // A1
    uint64_t Id() const override { return m_id; }     // A1
    void FillProbe(ProbeEntry& e) const override;     // A1
    void SetStreaming(bool on) override { m_streaming.store(on, std::memory_order_relaxed); }  // A1/A28b

private:
    void OnAccept(beast::error_code ec);
    void DoRead();
    void OnRead(beast::error_code ec, std::size_t bytes_transferred);
    void DoWrite();
    void OnWrite(beast::error_code ec, std::size_t bytes_transferred);
    // ★ I-W4（O-v2-1）：全仓唯一 async_write 站点（同 Plain）
    void BeginWrite(const std::shared_ptr<WriteMessage>& msg);
    // E-WS-QUEUE-DROP 丢弃汇总日志（同 Plain）
    void MaybeLogDrop();
    // 写看门狗命中回调（同 Plain）
    void OnWriteStall();

    websocket::stream<beast::ssl_stream<beast::tcp_stream>> m_ws;
    beast::flat_buffer m_buffer;
    HttpServer* m_server;

    // ---- A3：写队列（m_wq 必须位于 m_ws 之后）----
    WriteQueue m_wq;

    // ---- A3：生命周期与探针状态（与 Plain 同构，A16）----
    uint64_t m_id = 0;
    // P1-1：读上限会话级快照（同 Plain，见 A2 注释）
    std::size_t m_readLimitBytes = FaultInjection::kDefaultReadMessageMax;
    std::atomic<bool> m_closed{false};
    std::atomic<bool> m_streaming{false};
    std::shared_ptr<SslWebSocketSession> m_self;
};

// ============================================================
// HTTP Server (supports both HTTP and HTTPS)
// ============================================================
class HttpServer : public std::enable_shared_from_this<HttpServer> {
public:
    // httpPort: plain HTTP port (0 = disabled)
    // httpsPort: SSL HTTPS port (0 = disabled)
    HttpServer(uint16_t httpPort, uint16_t httpsPort = 0);
    ~HttpServer();

    bool Start();
    void Stop();

    void SetMessageCallback(WsMessageCallback cb) { m_messageCallback = std::move(cb); }
    void SetConnectCallback(WsConnectCallback cb) { m_connectCallback = std::move(cb); }

    // Broadcast binary data to all connected streaming clients
    void BroadcastBinary(const void* data, size_t size);
    // Broadcast text to all connected streaming clients
    void BroadcastText(const std::string& text);

    // Called by WebSocket sessions
    void OnSessionConnected(WebSocketSessionBase* session);
    void OnSessionDisconnected(WebSocketSessionBase* session);
    void OnSessionMessage(WebSocketSessionBase* session, const std::string& msg);

    // ---- A4: 诊断 / 故障注入（M-2 / M-3 / M-7）----
    // 探针 hook：返回顶层 JSON 成员序列（不含外层大括号）。
    // ★ 硬约束：hook 内部取 L2（streamingMutex）的动作由 DumpProbeJson()
    //   在【释放 L1 之后】调用，严禁 L1→L2 嵌套（§5.8.4）。
    void SetProbeHook(std::function<std::string()> hook) { m_probeHook = std::move(hook); }
    // 探针主体（三入口共用：/_debug/sessions、stdin dump、进程内调用）
    std::string DumpProbeJson() const;
    // 故障注入（★唯一形态 = HttpServer 成员，M-3；无单例）
    FaultInjection& Faults() { return m_faults; }
    // 会话序号分配（M-7）
    uint64_t NextSessionId() { return ::NextSessionId(); }
    // 会话存活计数（O-v2-2：liveSessions 只由构造 ++ / 析构 -- 维护）
    void OnSessionCreated()   { m_liveSessions.fetch_add(1, std::memory_order_relaxed); }
    void OnSessionDestroyed() { m_liveSessions.fetch_sub(1, std::memory_order_relaxed); }
    // 事件计数（O-v2-2：ForceClose 步骤 6 只 forceClosed++，不动 liveSessions）
    void OnSessionForceClosed() { m_forceClosed.fetch_add(1, std::memory_order_relaxed); }
    void OnWriteStallDetected() { m_writeStalls.fetch_add(1, std::memory_order_relaxed); }
    // m_sessions 数量（日志辅助；内部取 L1，勿在持锁时调用）
    size_t SessionCount() const;

    ssl::context& GetSslContext() { return m_sslCtx; }

private:
    void DoAcceptPlain();
    void DoAcceptSsl();
    // A22：L1 内拷贝会话指针快照（仅 Stop() join 之后的主线程 / 探针路径使用）
    std::vector<WebSocketSessionBase*> SnapshotSessions();

    // Generate self-signed certificate in memory (only if HTTPS is enabled)
    bool GenerateSelfSignedCert();

    uint16_t m_httpPort;
    uint16_t m_httpsPort;
    net::io_context m_ioc;
    ssl::context m_sslCtx;
    tcp::acceptor m_plainAcceptor;
    std::optional<tcp::acceptor> m_sslAcceptor;

    std::vector<std::thread> m_threads;

    mutable std::mutex m_sessionsMutex;                          // L1（mutable：DumpProbeJson() const 持锁读取）
    std::set<WebSocketSessionBase*> m_sessions;

    WsMessageCallback m_messageCallback;
    WsConnectCallback m_connectCallback;

    // ---- A4: 诊断成员 ----
    std::function<std::string()> m_probeHook;   // main.cpp 注册（streaming 计数等）
    FaultInjection m_faults;                    // ★ 唯一故障注入形态（成员）
    std::atomic<uint64_t> m_forceClosed{0};     // ForceClose 次数（全部原因）
    std::atomic<uint64_t> m_writeStalls{0};     // 写看门狗命中次数
    std::atomic<int64_t>  m_liveSessions{0};    // 构造++/析构--（R-12 兜底）
    std::atomic<bool>     m_stopped{false};     // Stop() 幂等闸门（A22 步骤 0）
};
