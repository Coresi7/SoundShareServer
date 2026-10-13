#include "http_server.h"
#include "embedded_page.h"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/err.h>

#include <iostream>
#include <algorithm>
#include <sstream>
#include <cstdlib>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

// ============================================================
// Common: handle HTTP request and produce response
// ============================================================

// A17 (M-7): /_debug/fault/set 的 query 解析（key=..&value=..[&session=N]）
static void ParseFaultQuery(const std::string& target, std::string& key,
                            std::string& value, std::optional<uint64_t>& sessionId) {
    auto qpos = target.find('?');
    if (qpos == std::string::npos) return;
    std::string query = target.substr(qpos + 1);
    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        std::string seg = query.substr(pos, (amp == std::string::npos ? query.size() : amp) - pos);
        pos = (amp == std::string::npos) ? query.size() : amp + 1;
        auto eq = seg.find('=');
        if (eq == std::string::npos) continue;
        std::string name = seg.substr(0, eq);
        std::string val = seg.substr(eq + 1);
        if (name == "key") key = val;
        else if (name == "value") value = val;
        else if (name == "session") {
            char* end = nullptr;
            unsigned long v = std::strtoul(val.c_str(), &end, 10);
            if (end != val.c_str()) sessionId = static_cast<uint64_t>(v);
        }
    }
}

// A17 (M-3): 签名增加第一个形参 HttpServer* server —— /_debug/* 经其取数
static void HandleHttpRequest(
    HttpServer* server,
    http::request<http::string_body>& req,
    std::function<void(http::response<http::string_body>&&)> sendResponse)
{
    auto target = std::string(req.target());

    // T-A1（tester 返修轮 1）：req.target() 含 query 串（如 "/?fallback=1"）。
    // 路由匹配一律基于剥离 query 后的 path 部分（P0-3 同款思路的根因修法）；
    // query 参数（fault/set 的 key/value、hist 的 name）仍从完整 target 解析。
    std::string path = target;
    const auto qmark = target.find('?');
    if (qmark != std::string::npos) path.resize(qmark);

#ifdef SOUNDSHARE_ENABLE_DIAGNOSTICS
    // 探针入口 1（主）：只读会话/写入状态（FR-10；O(1) 按需，零周期输出）
    // T-A1：改用剥离 query 后的 path 精确匹配（原 target 精确相等在带 query
    // 时 404，与根路由同款缺陷；不带 query 行为不变）。
    if (path == "/_debug/sessions") {
        auto body = server->DumpProbeJson();
        http::response<http::string_body> res{http::status::ok, req.version()};
        res.set(http::field::server, "SoundShareServer");
        res.set(http::field::content_type, "application/json");
        res.set(http::field::access_control_allow_origin, "*");
        res.keep_alive(req.keep_alive());
        res.body() = std::move(body);
        res.prepare_payload();
        sendResponse(std::move(res));
        return;
    }

    // T-GPU-AUDIO-GLITCH S0 (F6 / D-11A)：采集观测窗口复位（只读诊断动作）。
    // ★ P1-1：本路由为主通道（DIAGNOSTICS 默认 ON）；注入构建的
    //   /_debug/fault/set?key=stats_reset 为兜底。
    if (target.rfind("/_debug/stats/reset", 0) == 0) {
        server->ResetStatsWindow();
        http::response<http::string_body> res{http::status::ok, req.version()};
        res.set(http::field::server, "SoundShareServer");
        res.set(http::field::content_type, "application/json");
        res.set(http::field::access_control_allow_origin, "*");
        res.keep_alive(req.keep_alive());
        res.body() = "{\"ok\":true,\"action\":\"stats_reset\"}";
        res.prepare_payload();
        sendResponse(std::move(res));
        return;
    }

    // T-GPU-AUDIO-GLITCH S0 (F6 / D-18A)：采集直方图导出（Q2 双峰判读证据）。
    // ★ P0-3：必须前缀匹配（req.target() 含 query 串，精确相等会 404 ⇒ Q2 无
    //   证据）；name 参数自解析（默认 interArrival），与 /_debug/fault/set 同款。
    if (target.rfind("/_debug/capture/hist", 0) == 0) {
        std::string name = "interArrival";
        auto qpos = target.find('?');
        if (qpos != std::string::npos) {
            const std::string query = target.substr(qpos + 1);
            size_t pos = 0;
            while (pos < query.size()) {
                size_t amp = query.find('&', pos);
                std::string seg = query.substr(pos, (amp == std::string::npos ? query.size() : amp) - pos);
                pos = (amp == std::string::npos) ? query.size() : amp + 1;
                auto eq = seg.find('=');
                if (eq == std::string::npos) continue;
                if (seg.substr(0, eq) == "name") name = seg.substr(eq + 1);
            }
        }
        auto body = server->CaptureHist(name);
        http::response<http::string_body> res{http::status::ok, req.version()};
        res.set(http::field::server, "SoundShareServer");
        res.set(http::field::content_type, "application/json");
        res.set(http::field::access_control_allow_origin, "*");
        res.keep_alive(req.keep_alive());
        res.body() = std::move(body);
        res.prepare_payload();
        sendResponse(std::move(res));
        return;
    }
#endif

#ifdef SOUNDSHARE_ENABLE_FAULT_INJECTION
    // 故障注入入口（写端点）：默认 OFF（M-7）——未定义宏时此分支不编译，
    // 请求落入下方 404 分支。
    if (target.rfind("/_debug/fault/set", 0) == 0) {
        std::string key, value;
        std::optional<uint64_t> sessionId;
        ParseFaultQuery(target, key, value, sessionId);
        std::string error;
        const bool ok = server->Faults().Set(key, value, sessionId, error);

        http::response<http::string_body> res{http::status::ok, req.version()};
        res.set(http::field::server, "SoundShareServer");
        res.set(http::field::content_type, "application/json");
        res.set(http::field::access_control_allow_origin, "*");
        res.keep_alive(req.keep_alive());
        res.body() = ok
            ? "{\"ok\":true,\"key\":\"" + key + "\",\"value\":\"" + value + "\"}"
            : "{\"ok\":false,\"error\":\"" + error + "\"}";
        res.prepare_payload();
        sendResponse(std::move(res));
        return;
    }
#endif

    // T-A1：根路由改用剥离 query 后的 path 匹配 —— "/?fallback=1"（V-17 回退
    // 冒烟入口）与 "/index.html?x=1" 之前因 target 含 query 精确相等而 404；
    // 不带 query 的 "/" 与 "/index.html" 行为不变。
    if (path == "/" || path == "/index.html") {
        http::response<http::string_body> res{http::status::ok, req.version()};
        res.set(http::field::server, "SoundShareServer");
        res.set(http::field::content_type, "text/html; charset=utf-8");
        res.set(http::field::access_control_allow_origin, "*");
        res.keep_alive(req.keep_alive());
        res.body() = GetIndexHtml();
        res.prepare_payload();
        sendResponse(std::move(res));
    } else {
        http::response<http::string_body> res{http::status::not_found, req.version()};
        res.set(http::field::server, "SoundShareServer");
        res.set(http::field::content_type, "text/plain");
        res.keep_alive(req.keep_alive());
        res.body() = "Not Found";
        res.prepare_payload();
        sendResponse(std::move(res));
    }
}

// ============================================================
// Plain HTTP Session (no SSL)
// ============================================================
class PlainHttpSession : public std::enable_shared_from_this<PlainHttpSession> {
public:
    PlainHttpSession(beast::tcp_stream&& stream, HttpServer* server)
        : m_stream(std::move(stream)), m_server(server) {}

    void Run() {
        m_stream.expires_after(std::chrono::seconds(30));
        DoRead();
    }

private:
    void DoRead() {
        m_req = {};
        http::async_read(
            m_stream, m_buffer, m_req,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) return;
                self->HandleRequest();
            });
    }

    void HandleRequest() {
        if (websocket::is_upgrade(m_req)) {
            auto ws = std::make_shared<PlainWebSocketSession>(std::move(m_stream), m_server);
            ws->Run(std::move(m_req));
            return;
        }

        HandleHttpRequest(m_server, m_req, [this](http::response<http::string_body>&& res) {
            SendResponse(std::move(res));
        });
    }

    template<class Body>
    void SendResponse(http::response<Body>&& res) {
        auto sp = std::make_shared<http::response<Body>>(std::move(res));
        http::async_write(
            m_stream, *sp,
            [self = shared_from_this(), sp](beast::error_code ec, std::size_t) {
                if (ec) return;
                self->DoRead();
            });
    }

    beast::tcp_stream m_stream;
    beast::flat_buffer m_buffer;
    http::request<http::string_body> m_req;
    HttpServer* m_server;
};

// ============================================================
// SSL HTTP Session (HTTPS)
// ============================================================
class SslHttpSession : public std::enable_shared_from_this<SslHttpSession> {
public:
    SslHttpSession(beast::ssl_stream<beast::tcp_stream>&& stream, HttpServer* server)
        : m_stream(std::move(stream)), m_server(server) {}

    void Run() {
        beast::get_lowest_layer(m_stream).expires_after(std::chrono::seconds(30));
        m_stream.async_handshake(
            ssl::stream_base::server,
            [self = shared_from_this()](beast::error_code ec) {
                if (ec) {
                    std::cerr << "[SSL] Handshake failed: " << ec.message() << std::endl;
                    return;
                }
                beast::get_lowest_layer(self->m_stream).expires_after(std::chrono::seconds(30));
                self->DoRead();
            });
    }

private:
    void DoRead() {
        m_req = {};
        http::async_read(
            m_stream, m_buffer, m_req,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) return;
                self->HandleRequest();
            });
    }

    void HandleRequest() {
        if (websocket::is_upgrade(m_req)) {
            auto ws = std::make_shared<SslWebSocketSession>(std::move(m_stream), m_server);
            ws->Run(std::move(m_req));
            return;
        }

        HandleHttpRequest(m_server, m_req, [this](http::response<http::string_body>&& res) {
            SendResponse(std::move(res));
        });
    }

    template<class Body>
    void SendResponse(http::response<Body>&& res) {
        auto sp = std::make_shared<http::response<Body>>(std::move(res));
        http::async_write(
            m_stream, *sp,
            [self = shared_from_this(), sp](beast::error_code ec, std::size_t) {
                if (ec) return;
                self->DoRead();
            });
    }

    beast::ssl_stream<beast::tcp_stream> m_stream;
    beast::flat_buffer m_buffer;
    http::request<http::string_body> m_req;
    HttpServer* m_server;
};

// ============================================================
// Common write queue helpers (used by both WS session types)
// ============================================================
// A6/A7/A8 (B3): the old MAX_QUEUE_DEPTH / EnqueueText / EnqueueBinary free
// functions (unbounded-growth path: "queue front is text -> break -> still
// push_back") are replaced by WriteQueue (write_queue.h), which implements
// the write-ownership protocol I-W1~I-W6 (§5.4.4) and the dual-capacity
// drop policy with per-class counters (§5.5).

// ============================================================
// Plain WebSocket Session Implementation
// ============================================================
PlainWebSocketSession::PlainWebSocketSession(beast::tcp_stream&& stream, HttpServer* server)
    : m_ws(std::move(stream)), m_server(server), m_wq(m_ws.get_executor()) {
    m_id = m_server->NextSessionId();       // M-7：会话序号
    m_server->OnSessionCreated();           // O-v2-2：liveSessions 构造 ++（唯一增点）
}

PlainWebSocketSession::~PlainWebSocketSession() {
    // A9：断开语义与 ForceClose 竞争同一 m_closed CAS —— 谁先到谁执行一次
    //（D-09 C / §5.3）。已 ForceClose 过 → 此处 CAS 失败 → 不二次摘除。
    if (!m_closed.exchange(true)) {
        m_server->OnSessionDisconnected(this);
    }
    m_server->OnSessionDestroyed();         // O-v2-2：liveSessions 析构 --（唯一减点）
}

void PlainWebSocketSession::Run(http::request<http::string_body> req) {
    websocket::stream_base::timeout opt{};
    opt.handshake_timeout = std::chrono::seconds(30);
    opt.idle_timeout = std::chrono::seconds(120);
    opt.keep_alive_pings = true;

    m_ws.set_option(opt);

    m_ws.set_option(websocket::stream_base::decorator(
        [](websocket::response_type& res) {
            res.set(http::field::server, "SoundShareServer");
        }));

    m_ws.async_accept(
        req,
        [self = shared_from_this()](beast::error_code ec) {
            self->OnAccept(ec);
        });
}

void PlainWebSocketSession::OnAccept(beast::error_code ec) {
    if (ec) {
        // A10：关停期 operation_aborted 静默过滤（§6.3）
        if (ec != net::error::operation_aborted) {
            std::cerr << "[WS] Accept error: " << ec.message() << std::endl;
        }
        return;   // 路径④⑤（§5.4.5）：m_self 尚未赋值 → 依赖析构回收，不泄漏
    }
    // ★ M-4 顺序硬约束：m_self 赋值必须在 if (ec) return; 之后（若在之前，
    //   accept 失败的对象将自持 + 不在 m_sessions + 无 handler 调 ForceClose
    //   → 永久泄漏且探针不可见，正中 R-12）。
    m_self = shared_from_this();             // A10①：自持引用（D-02 A）
    // P1-1（§5.9.2 read_message_max 接线，AC-11b）：读上限 = OnAccept 时
    // Faults().ReadMessageMax() 的会话级快照（注入构建可经 /_debug/fault/set
    // 运行期覆写、仅对新会话生效；默认构建恒 65536，与原硬编码 64*1024
    // 行为一致）。超限走 read_overflow 异常路径并打一次异常日志。
    m_readLimitBytes = m_server->Faults().ReadMessageMax();
    m_ws.read_message_max(m_readLimitBytes); // A10②：超大消息保护（AC-11b）
    beast::get_lowest_layer(m_ws).expires_never();   // 原样不动（NFR-WIN-2）
    m_server->OnSessionConnected(this);      // 原样
    // B5（A10③）：启动写看门狗 —— timer 绑 m_ws.get_executor()（即会话
    // strand，I-W5）；判定命中 → OnWriteStall → ForceClose(write_stall)。
    m_wq.Start(
        shared_from_this(),   // P0-1 收尾：keeper（timer 存活检查 + 断环点统一）
        [self = shared_from_this()] { self->OnWriteStall(); },
#ifdef SOUNDSHARE_ENABLE_FAULT_INJECTION
        // 注入构建：stall_threshold_ms 每窗口刷新（§5.9.2）
        [this]() -> uint32_t { return m_server->Faults().StallThresholdMs(); });
#else
        nullptr);   // 默认构建：恒 30000ms（T_eff=29000，M-8）
#endif
    DoRead();                                // 原样
}

void PlainWebSocketSession::DoRead() {
    m_ws.async_read(
        m_buffer,
        [self = shared_from_this()](beast::error_code ec, std::size_t bytes_transferred) {
            self->OnRead(ec, bytes_transferred);
        });
}

void PlainWebSocketSession::OnRead(beast::error_code ec, std::size_t) {
#ifdef SOUNDSHARE_ENABLE_FAULT_INJECTION
    // P0-2（§5.9.2 inject_read_error 消费点，AC-09）：一次性（命中即清除，
    // 由 FaultInjection::InjectReadError 内部摘除，每会话/每次注入至多命中一次）。
    // 语义 = "下一次 OnRead 以 connection_reset 完成"：命中时覆写 ec →
    // connection_reset 不属于 closed/eof/message_too_big → read_error 分支 →
    // E-WS-READ-FAIL + E-WS-FORCE-CLOSE 恰两行。仅注入构建编译（M-7：默认
    // OFF 时本检查点编译剔除，正常路径零开销）。触发条件 = 该会话下一次读
    // 完成（注入后客户端再发任意数据帧即可）。
    if (m_server->Faults().InjectReadError(m_id)) {
        ec = net::error::connection_reset;
    }
#endif
    if (ec) {
        // A11（FR-07）：读错误可恢复 —— 不再静默 return，走 ForceClose 完整终态。
        CloseReason r;
        if (ec == websocket::error::closed || ec == net::error::eof) {
            r = CloseReason::peer_closed;    // 对端正常关闭 → 静默（§6.3）
        } else if (ec == websocket::error::message_too_big) {
            r = CloseReason::read_overflow;  // → E-WS-READ-INVALID（AC-11b）
        } else {
            r = CloseReason::read_error;     // → E-WS-READ-FAIL（AC-09）
        }
        ForceClose(r, ec);
        return;
    }

    if (m_ws.got_text()) {
        std::string msg(static_cast<const char*>(m_buffer.data().data()), m_buffer.size());
        m_server->OnSessionMessage(this, msg);
    }

    m_buffer.consume(m_buffer.size());
    DoRead();
}

// ---- A12 (B3/M-1)：Send / SendBinary —— 任意线程可调用 ----
// 锁内只动容器与计数；writing 的 false→true 只发生在 EnqueueLocked 的 L3
// 临界区内（I-W2 唯一取得点）；取得所有权的一方（kick==true）以 net::dispatch
// 把 DoWrite 投递到会话 strand。★ 采集线程绝不可直调 DoWrite()（I-W4/I-W5）。
void PlainWebSocketSession::Send(const std::string& text) {
    if (m_closed.load(std::memory_order_relaxed)) return;   // 快速路径（非安全依赖，§5.4.1）
#ifdef SOUNDSHARE_ENABLE_FAULT_INJECTION
    if (m_server->Faults().IsPaused(m_id)) return;          // pause_stream（AC-05/06 备选注入）
#endif
    bool kick = false;
    {
        std::lock_guard<std::mutex> lk(m_wq.Mutex());
        kick = m_wq.EnqueueLocked(true, text.data(), text.size());
    }
    if (kick) {
        net::dispatch(m_ws.get_executor(),
                      [self = shared_from_this()] { self->DoWrite(); });
    }
    MaybeLogDrop();   // E-WS-QUEUE-DROP 节流日志；出 L3 之后才允许写日志（§5.5）
}

void PlainWebSocketSession::SendBinary(const void* data, size_t size) {
    if (m_closed.load(std::memory_order_relaxed)) return;
#ifdef SOUNDSHARE_ENABLE_FAULT_INJECTION
    if (m_server->Faults().IsPaused(m_id)) return;
#endif
    bool kick = false;
    {
        std::lock_guard<std::mutex> lk(m_wq.Mutex());
        kick = m_wq.EnqueueLocked(false, data, size);
    }
    if (kick) {
        net::dispatch(m_ws.get_executor(),
                      [self = shared_from_this()] { self->DoWrite(); });
    }
    MaybeLogDrop();
}

// ---- A13 (B3/M-1)：DoWrite —— 只在会话 strand 上运行（PRECONDITION）----
void PlainWebSocketSession::DoWrite() {
    // PRECONDITION: on session strand (net::dispatch from the Enqueue path,
    // or invoked as OnWrite continuation).
    if (m_closed.load(std::memory_order_relaxed)) return;
    std::shared_ptr<WriteMessage> msg;
    {
        std::lock_guard<std::mutex> lk(m_wq.Mutex());
        msg = m_wq.TakeNextLocked(WqNowMs());   // 取消息与所有权归还同临界区（I-W2）
    }
    if (!msg) return;   // 队列空 → 所有权已在 L3 内归还
    BeginWrite(msg);
}

// ---- O-v2-1 口径：发起片段抽为唯一函数（DoWrite 与 OnWrite 成功续写共用）----
// ★ 全仓唯一 async_write 站点（I-W4），且只在会话 strand 上（I-W5）。
void PlainWebSocketSession::BeginWrite(const std::shared_ptr<WriteMessage>& msg) {
    // PRECONDITION: on session strand; caller holds write ownership.
#ifdef SOUNDSHARE_ENABLE_FAULT_INJECTION
    if (m_server->Faults().InjectWriteStall(m_id)) {
        return;   // 所有权保持、永不发起写 → T_eff 后 stall watcher 命中（AC-01 加速型）
    }
    if (m_server->Faults().InjectWriteError(m_id)) {
        // 注入回调必须走 strand，禁止跨线程直调 OnWrite（AC-10 注入点）
        net::post(m_ws.get_executor(),
                  [self = shared_from_this()] {
                      self->OnWrite(net::error::connection_reset, 0);
                  });
        return;
    }
#endif
    m_ws.text(msg->isText);
    m_ws.binary(!msg->isText);
    m_ws.async_write(
        net::buffer(msg->data.data(), msg->data.size()),
        [self = shared_from_this(), msg](beast::error_code ec, std::size_t) {
            self->OnWrite(ec, 0);
        });
}

// ---- A14 (B3/M-1)：OnWrite —— 成功路径在同一 L3 临界区内 OnProgress+TakeNext 续写 ----
void PlainWebSocketSession::OnWrite(beast::error_code ec, std::size_t) {
    if (m_closed.load(std::memory_order_relaxed)) return;   // ForceClose 已抢先 → 静默（UA-3）
    if (ec) {
        ForceClose(CloseReason::write_error, ec);           // 出锁后清理（锁内不关 socket，DL-2）
        return;
    }
    std::shared_ptr<WriteMessage> msg;
    {
        std::lock_guard<std::mutex> lk(m_wq.Mutex());
        m_wq.OnProgressLocked(WqNowMs());                   // 更新点 ②（§5.1.1）
        msg = m_wq.TakeNextLocked(WqNowMs());               // 续取下一条（与归还同区，无窗口）
    }
    if (!msg) return;
    BeginWrite(msg);   // 本就在 strand 上，无需 dispatch（§5.7.2）
}

// ---- E-WS-QUEUE-DROP（§6.3；首次立即 1 行 + 之后每 30s 至多 1 行）----
void PlainWebSocketSession::MaybeLogDrop() {
    WriteQueue::DropLogEvent ev;
    if (!m_wq.TakeDropLogEvent(WqNowMs(), ev)) return;
    const auto snap = m_wq.GetSnapshot();
    const auto& cfg = m_wq.Config();
    std::cerr << "[WS][WARN] write queue drop: id=" << m_id
              << " dropped=" << ev.droppedInWindow
              << " droppedTotal=" << ev.droppedTotal
              << " depth=" << snap.depth
              << " maxFrames=" << cfg.maxBinaryFrames
              << " bytes=" << snap.bytes
              << " maxBytes=" << cfg.maxQueuedBytes << std::endl;
}

bool PlainWebSocketSession::IsOpen() const {
    return m_ws.is_open();
}

void PlainWebSocketSession::Close() {
    // A15: Close() 语义重定向为 ForceClose(peer_closed)（B-3：原同步
    // m_ws.close() 在卡死连接上会阻塞/抛异常，且无异步收尾与资源回收）。
    // ★ O-v2-4：本方法只允许在会话 strand 上、或 Stop() join 之后的主线程
    //   调用（同 ForceClose 契约）；当前全仓无调用方。
    ForceClose(CloseReason::peer_closed);
}

// ---- A1: 探针填充（M-2 硬约束：只读原子/锁内计数，不触碰 m_ws，不取 L1/L2）----
void PlainWebSocketSession::FillProbe(ProbeEntry& e) const {
    e.id = m_id;
    e.ssl = false;
    e.closed = m_closed.load(std::memory_order_relaxed);
    e.streaming = m_streaming.load(std::memory_order_relaxed);
    // 写队列快照（GetSnapshot 内部取 L3 叶锁；L1→L3 为允许嵌套，M-2）
    const auto s = m_wq.GetSnapshot();
    e.writing = s.writing;
    e.queueDepth = static_cast<uint32_t>(s.depth);
    e.queueBinary = static_cast<uint32_t>(s.binary);
    e.queueText = static_cast<uint32_t>(s.text);
    e.queueBytes = s.bytes;
    e.enqueued = s.enqueued;
    e.dequeued = s.dequeued;
    e.droppedTotal = s.droppedTotal;
    // 即时停滞判定（读原子，无需持锁）
    const uint64_t now = WqNowMs();
    const uint64_t last = m_wq.LastProgressMs();
    e.stalled = m_wq.Stalled(now);
    e.idleMs = (now > last) ? (now - last) : 0;
    e.thresholdMs = m_wq.ThresholdMs();
}

// ---- B5：写看门狗命中（§5.1.3；m_wq 的 onStall 回调，在会话 strand 上执行）----
// E-WS-WRITE-STALL（§6.3；每会话每次卡死 1 行 —— watcher 命中后自停，不重复）。
void PlainWebSocketSession::OnWriteStall() {
    const uint64_t now = WqNowMs();
    const uint64_t last = m_wq.LastProgressMs();
    const uint64_t idle = (now > last) ? (now - last) : 0;
    std::cerr << "[WS][ERROR] write stall detected: id=" << m_id
              << " ssl=0"
              << " writing=" << (m_wq.IsWriting() ? 1 : 0)
              << " pending=" << m_wq.Pending()
              << " idleMs=" << idle
              << " thresholdMs=" << m_wq.ThresholdMs() << std::endl;
    m_server->OnWriteStallDetected();            // counters.writeStalls++（探针）
    ForceClose(CloseReason::write_stall);        // → E-WS-FORCE-CLOSE（第 2 行，AC-01）

    // R2 断环（★ write_stall 路径的专属断环点；其余路径由 ForceClose 步骤 3b
    // 同步断环，P0-1 统一化）：本调用链为 OnTimer→m_onStall→OnWriteStall→
    // ForceClose，此刻正处于 m_onStall 这个 std::function 的执行栈内 —— 若在
    // ForceClose 内同步 ClearCallbacks 会销毁正在执行的 function，属 UB。故
    // 在 strand 下一轮清空回调以断环；析构发生在该 post handler 栈上（持有
    // self），不在本调用链的 m_wq 成员函数栈内 —— 无 UB。
    net::post(m_ws.get_executor(), [self = shared_from_this()] {
        self->m_wq.ClearCallbacks();
    });
}

// ---- A1: 唯一清理入口（FR-02 终态；§5.2 执行顺序）----
void PlainWebSocketSession::ForceClose(CloseReason reason, beast::error_code ec) {
    // ★ 调用线程契约 v2（§5.4.3）：仅 ①会话自身 strand（运行期）或
    //   ②HttpServer::Stop() join 之后的主线程（关停期）。不得持有 L1/L2/L3。
    bool first = !m_closed.exchange(true);   // CAS 闸门 —— 幂等（F-5 / D-09 C）
    if (!first) return;                      // 已清理 → 不二次摘除/不二次打印

    if (IsExceptionReason(reason)) {
        // 事件级异常日志（§6.3；每会话至多一次，由上方 CAS 保证）
        if (reason == CloseReason::write_error) {
            std::cerr << "[WS][ERROR] write failed: id=" << m_id
                      << " ec=" << ec.message() << std::endl;              // E-WS-WRITE-FAIL
        } else if (reason == CloseReason::read_error) {
            std::cerr << "[WS][ERROR] read failed: id=" << m_id
                      << " ec=" << ec.message() << std::endl;              // E-WS-READ-FAIL
        } else if (reason == CloseReason::read_overflow) {
            // P1-1：limitBytes = 该会话 OnAccept 快照（与会话实际生效的
            // read_message_max 一致，而非全局当前值）
            std::cerr << "[WS][ERROR] read rejected: id=" << m_id
                      << " reason=message_too_big limitBytes="
                      << m_readLimitBytes << std::endl;  // E-WS-READ-INVALID
        }
        // E-WS-FORCE-CLOSE（§6.3；独立异常文案，非正常 disconnect）
        std::cerr << "[WS][ERROR] session force-closed: id=" << m_id
                  << " reason=" << CloseReasonName(reason)
                  << " remainingSessions=" << (m_server->SessionCount() - 1)
                  << std::endl;
    }
    // （peer_closed / server_stop 静默，§6.3；write_stall 的 E-WS-WRITE-STALL
    //   由调用方 OnWriteStall 先打，B5）

    m_wq.Stop();   // 3. cancel 看门狗（同 strand；关停期无并发）

    // 3b. ★ P0-1（R2 断环统一化）：任何原因的 ForceClose 都必然断环。
    //     m_wq.m_onStall 的 lambda 捕获 shared_from_this()，与
    //     会话→m_wq→m_onStall→会话 构成循环引用 —— 不清空则 m_self.reset()
    //     后引用计数仍 ≥1，对象永不析构，liveSessions 永不归零（§8.12 的
    //     R-12 判据失效，且为每连接一次的真实泄漏，覆盖
    //     peer_closed/write_error/read_error/read_overflow/server_stop）。
    //     - 非 write_stall 路径：均不在 m_onStall 调用栈内（m_onStall 仅被
    //       OnTimer 调用，而 OnTimer 与本函数同 strand 串行，且上方 m_wq.Stop
    //       已 cancel timer；关停期 join 后无并发）→ 此处同步清空安全。
    //     - write_stall 路径：正处于 m_onStall 执行栈内（OnTimer→m_onStall→
    //       OnWriteStall→ForceClose），同步销毁正在执行的 std::function 属
    //       UB → 由 OnWriteStall 末尾既有的 net::post 延迟断环负责。
    if (reason != CloseReason::write_stall) {
        m_wq.ClearCallbacks();
    }

    // 4. 关闭底层 socket（★ 与 async_write 同 strand ⇒ 无 Shared-object UB）。
    //    只用 error_code 版本；禁止同步 m_ws.close()（会在卡死连接上阻塞）。
    //    shutdown 使挂起的 async_write 立即以错误完成，释放 shared_from_this 持有。
    beast::error_code ignored;
    try {
        beast::get_lowest_layer(m_ws).socket().shutdown(tcp::socket::shutdown_both, ignored);
        beast::get_lowest_layer(m_ws).socket().close(ignored);
    } catch (...) {}   // R-03

    // 5. 摘 m_sessions（锁内 erase）+ connectCallback（→ main.cpp 摘 streamingSessions）
    m_server->OnSessionDisconnected(this);

    // 6. 事件计数（O-v2-2：只 forceClosed++，不动 liveSessions）
    m_server->OnSessionForceClosed();

    // 7. 释放自持引用（B4 起 m_self 在 OnAccept 赋值）。
    //    ★ R-01：必须是最后一条语句 —— reset 之后不得访问任何成员或 this。
    m_self.reset();
}

// ============================================================
// SSL WebSocket Session Implementation
// ============================================================
SslWebSocketSession::SslWebSocketSession(beast::ssl_stream<beast::tcp_stream>&& stream, HttpServer* server)
    : m_ws(std::move(stream)), m_server(server), m_wq(m_ws.get_executor()) {
    m_id = m_server->NextSessionId();       // M-7：会话序号
    m_server->OnSessionCreated();           // O-v2-2：liveSessions 构造 ++（唯一增点）
}

SslWebSocketSession::~SslWebSocketSession() {
    // A9：断开语义与 ForceClose 竞争同一 m_closed CAS —— 谁先到谁执行一次
    //（D-09 C / §5.3）。已 ForceClose 过 → 此处 CAS 失败 → 不二次摘除。
    if (!m_closed.exchange(true)) {
        m_server->OnSessionDisconnected(this);
    }
    m_server->OnSessionDestroyed();         // O-v2-2：liveSessions 析构 --（唯一减点）
}

void SslWebSocketSession::Run(http::request<http::string_body> req) {
    websocket::stream_base::timeout opt{};
    opt.handshake_timeout = std::chrono::seconds(30);
    opt.idle_timeout = std::chrono::seconds(120);
    opt.keep_alive_pings = true;

    m_ws.set_option(opt);

    m_ws.set_option(websocket::stream_base::decorator(
        [](websocket::response_type& res) {
            res.set(http::field::server, "SoundShareServer");
        }));

    m_ws.async_accept(
        req,
        [self = shared_from_this()](beast::error_code ec) {
            self->OnAccept(ec);
        });
}

void SslWebSocketSession::OnAccept(beast::error_code ec) {
    if (ec) {
        // A10：关停期 operation_aborted 静默过滤（§6.3）
        if (ec != net::error::operation_aborted) {
            std::cerr << "[WS] Accept error: " << ec.message() << std::endl;
        }
        return;   // 路径④⑤（§5.4.5）：m_self 尚未赋值 → 依赖析构回收，不泄漏
    }
    // ★ M-4 顺序硬约束：m_self 赋值必须在 if (ec) return; 之后（若在之前，
    //   accept 失败的对象将自持 + 不在 m_sessions + 无 handler 调 ForceClose
    //   → 永久泄漏且探针不可见，正中 R-12）。
    m_self = shared_from_this();             // A10①：自持引用（D-02 A）
    // P1-1（§5.9.2 read_message_max 接线，AC-11b）：读上限 = OnAccept 时
    // Faults().ReadMessageMax() 的会话级快照（注入构建可经 /_debug/fault/set
    // 运行期覆写、仅对新会话生效；默认构建恒 65536，与原硬编码 64*1024
    // 行为一致）。超限走 read_overflow 异常路径并打一次异常日志。
    m_readLimitBytes = m_server->Faults().ReadMessageMax();
    m_ws.read_message_max(m_readLimitBytes); // A10②：超大消息保护（AC-11b）
    beast::get_lowest_layer(m_ws).expires_never();   // 原样不动（NFR-WIN-2）
    m_server->OnSessionConnected(this);      // 原样
    // B5（A10③）：启动写看门狗 —— timer 绑 m_ws.get_executor()（即会话
    // strand，I-W5）；判定命中 → OnWriteStall → ForceClose(write_stall)。
    m_wq.Start(
        shared_from_this(),   // P0-1 收尾：keeper（timer 存活检查 + 断环点统一）
        [self = shared_from_this()] { self->OnWriteStall(); },
#ifdef SOUNDSHARE_ENABLE_FAULT_INJECTION
        // 注入构建：stall_threshold_ms 每窗口刷新（§5.9.2）
        [this]() -> uint32_t { return m_server->Faults().StallThresholdMs(); });
#else
        nullptr);   // 默认构建：恒 30000ms（T_eff=29000，M-8）
#endif
    DoRead();                                // 原样
}

void SslWebSocketSession::DoRead() {
    m_ws.async_read(
        m_buffer,
        [self = shared_from_this()](beast::error_code ec, std::size_t bytes_transferred) {
            self->OnRead(ec, bytes_transferred);
        });
}

void SslWebSocketSession::OnRead(beast::error_code ec, std::size_t) {
#ifdef SOUNDSHARE_ENABLE_FAULT_INJECTION
    // P0-2（§5.9.2 inject_read_error 消费点，AC-09）：一次性（命中即清除，
    // 由 FaultInjection::InjectReadError 内部摘除，每会话/每次注入至多命中一次）。
    // 语义 = "下一次 OnRead 以 connection_reset 完成"：命中时覆写 ec →
    // connection_reset 不属于 closed/eof/message_too_big → read_error 分支 →
    // E-WS-READ-FAIL + E-WS-FORCE-CLOSE 恰两行。仅注入构建编译（M-7：默认
    // OFF 时本检查点编译剔除，正常路径零开销）。触发条件 = 该会话下一次读
    // 完成（注入后客户端再发任意数据帧即可）。
    if (m_server->Faults().InjectReadError(m_id)) {
        ec = net::error::connection_reset;
    }
#endif
    if (ec) {
        // A11（FR-07）：读错误可恢复 —— 不再静默 return，走 ForceClose 完整终态。
        CloseReason r;
        if (ec == websocket::error::closed || ec == net::error::eof) {
            r = CloseReason::peer_closed;    // 对端正常关闭 → 静默（§6.3）
        } else if (ec == websocket::error::message_too_big) {
            r = CloseReason::read_overflow;  // → E-WS-READ-INVALID（AC-11b）
        } else {
            r = CloseReason::read_error;     // → E-WS-READ-FAIL（AC-09）
        }
        ForceClose(r, ec);
        return;
    }

    if (m_ws.got_text()) {
        std::string msg(static_cast<const char*>(m_buffer.data().data()), m_buffer.size());
        m_server->OnSessionMessage(this, msg);
    }

    m_buffer.consume(m_buffer.size());
    DoRead();
}

// ---- A12/A16 (B3/M-1)：Send / SendBinary（与 Plain 同构，A16）----
void SslWebSocketSession::Send(const std::string& text) {
    if (m_closed.load(std::memory_order_relaxed)) return;
#ifdef SOUNDSHARE_ENABLE_FAULT_INJECTION
    if (m_server->Faults().IsPaused(m_id)) return;
#endif
    bool kick = false;
    {
        std::lock_guard<std::mutex> lk(m_wq.Mutex());
        kick = m_wq.EnqueueLocked(true, text.data(), text.size());
    }
    if (kick) {
        net::dispatch(m_ws.get_executor(),
                      [self = shared_from_this()] { self->DoWrite(); });
    }
    MaybeLogDrop();
}

void SslWebSocketSession::SendBinary(const void* data, size_t size) {
    if (m_closed.load(std::memory_order_relaxed)) return;
#ifdef SOUNDSHARE_ENABLE_FAULT_INJECTION
    if (m_server->Faults().IsPaused(m_id)) return;
#endif
    bool kick = false;
    {
        std::lock_guard<std::mutex> lk(m_wq.Mutex());
        kick = m_wq.EnqueueLocked(false, data, size);
    }
    if (kick) {
        net::dispatch(m_ws.get_executor(),
                      [self = shared_from_this()] { self->DoWrite(); });
    }
    MaybeLogDrop();
}

// ---- A13/A16 (B3/M-1)：DoWrite —— 只在会话 strand 上运行 ----
void SslWebSocketSession::DoWrite() {
    // PRECONDITION: on session strand.
    if (m_closed.load(std::memory_order_relaxed)) return;
    std::shared_ptr<WriteMessage> msg;
    {
        std::lock_guard<std::mutex> lk(m_wq.Mutex());
        msg = m_wq.TakeNextLocked(WqNowMs());
    }
    if (!msg) return;
    BeginWrite(msg);
}

// ---- O-v2-1：全仓唯一 async_write 站点（与 Plain 同构，I-W4/I-W5）----
void SslWebSocketSession::BeginWrite(const std::shared_ptr<WriteMessage>& msg) {
    // PRECONDITION: on session strand; caller holds write ownership.
#ifdef SOUNDSHARE_ENABLE_FAULT_INJECTION
    if (m_server->Faults().InjectWriteStall(m_id)) {
        return;
    }
    if (m_server->Faults().InjectWriteError(m_id)) {
        net::post(m_ws.get_executor(),
                  [self = shared_from_this()] {
                      self->OnWrite(net::error::connection_reset, 0);
                  });
        return;
    }
#endif
    m_ws.text(msg->isText);
    m_ws.binary(!msg->isText);
    m_ws.async_write(
        net::buffer(msg->data.data(), msg->data.size()),
        [self = shared_from_this(), msg](beast::error_code ec, std::size_t) {
            self->OnWrite(ec, 0);
        });
}

// ---- A14/A16 (B3/M-1)：OnWrite —— 成功路径同临界区续写 ----
void SslWebSocketSession::OnWrite(beast::error_code ec, std::size_t) {
    if (m_closed.load(std::memory_order_relaxed)) return;
    if (ec) {
        ForceClose(CloseReason::write_error, ec);
        return;
    }
    std::shared_ptr<WriteMessage> msg;
    {
        std::lock_guard<std::mutex> lk(m_wq.Mutex());
        m_wq.OnProgressLocked(WqNowMs());
        msg = m_wq.TakeNextLocked(WqNowMs());
    }
    if (!msg) return;
    BeginWrite(msg);
}

// ---- E-WS-QUEUE-DROP（与 Plain 同构）----
void SslWebSocketSession::MaybeLogDrop() {
    WriteQueue::DropLogEvent ev;
    if (!m_wq.TakeDropLogEvent(WqNowMs(), ev)) return;
    const auto snap = m_wq.GetSnapshot();
    const auto& cfg = m_wq.Config();
    std::cerr << "[WS][WARN] write queue drop: id=" << m_id
              << " dropped=" << ev.droppedInWindow
              << " droppedTotal=" << ev.droppedTotal
              << " depth=" << snap.depth
              << " maxFrames=" << cfg.maxBinaryFrames
              << " bytes=" << snap.bytes
              << " maxBytes=" << cfg.maxQueuedBytes << std::endl;
}

bool SslWebSocketSession::IsOpen() const {
    return m_ws.is_open();
}

void SslWebSocketSession::Close() {
    // A15: Close() 语义重定向为 ForceClose(peer_closed)（B-3；同 Plain）。
    // ★ O-v2-4：仅限会话 strand 或 Stop() join 之后的主线程调用；当前全仓无调用方。
    ForceClose(CloseReason::peer_closed);
}

// ---- A1: 探针填充（同 Plain；不触碰 m_ws，不取 L1/L2）----
void SslWebSocketSession::FillProbe(ProbeEntry& e) const {
    e.id = m_id;
    e.ssl = true;
    e.closed = m_closed.load(std::memory_order_relaxed);
    e.streaming = m_streaming.load(std::memory_order_relaxed);
    // 写队列快照（GetSnapshot 内部取 L3 叶锁；L1→L3 为允许嵌套，M-2）
    const auto s = m_wq.GetSnapshot();
    e.writing = s.writing;
    e.queueDepth = static_cast<uint32_t>(s.depth);
    e.queueBinary = static_cast<uint32_t>(s.binary);
    e.queueText = static_cast<uint32_t>(s.text);
    e.queueBytes = s.bytes;
    e.enqueued = s.enqueued;
    e.dequeued = s.dequeued;
    e.droppedTotal = s.droppedTotal;
    const uint64_t now = WqNowMs();
    const uint64_t last = m_wq.LastProgressMs();
    e.stalled = m_wq.Stalled(now);
    e.idleMs = (now > last) ? (now - last) : 0;
    e.thresholdMs = m_wq.ThresholdMs();
}

// ---- B5：写看门狗命中（同 Plain，ssl=1）----
void SslWebSocketSession::OnWriteStall() {
    const uint64_t now = WqNowMs();
    const uint64_t last = m_wq.LastProgressMs();
    const uint64_t idle = (now > last) ? (now - last) : 0;
    std::cerr << "[WS][ERROR] write stall detected: id=" << m_id
              << " ssl=1"
              << " writing=" << (m_wq.IsWriting() ? 1 : 0)
              << " pending=" << m_wq.Pending()
              << " idleMs=" << idle
              << " thresholdMs=" << m_wq.ThresholdMs() << std::endl;
    m_server->OnWriteStallDetected();
    ForceClose(CloseReason::write_stall);

    // R2 断环（write_stall 路径专属断环点；其余路径由 ForceClose 步骤 3b 同步
    // 断环，P0-1 统一化；同 Plain —— 处于 m_onStall 执行栈内，必须延迟断环，
    // 析构发生在 post handler 栈上）。
    net::post(m_ws.get_executor(), [self = shared_from_this()] {
        self->m_wq.ClearCallbacks();
    });
}

// ---- A1: 唯一清理入口（FR-02 终态；§5.2；同 Plain）----
void SslWebSocketSession::ForceClose(CloseReason reason, beast::error_code ec) {
    // ★ 调用线程契约 v2（§5.4.3）：仅 ①会话自身 strand 或 ②Stop() join 后的主线程
    bool first = !m_closed.exchange(true);   // CAS 闸门 —— 幂等（F-5）
    if (!first) return;

    if (IsExceptionReason(reason)) {
        if (reason == CloseReason::write_error) {
            std::cerr << "[WS][ERROR] write failed: id=" << m_id
                      << " ec=" << ec.message() << std::endl;              // E-WS-WRITE-FAIL
        } else if (reason == CloseReason::read_error) {
            std::cerr << "[WS][ERROR] read failed: id=" << m_id
                      << " ec=" << ec.message() << std::endl;              // E-WS-READ-FAIL
        } else if (reason == CloseReason::read_overflow) {
            // P1-1：limitBytes = 该会话 OnAccept 快照（同 Plain）
            std::cerr << "[WS][ERROR] read rejected: id=" << m_id
                      << " reason=message_too_big limitBytes="
                      << m_readLimitBytes << std::endl;  // E-WS-READ-INVALID
        }
        std::cerr << "[WS][ERROR] session force-closed: id=" << m_id
                  << " reason=" << CloseReasonName(reason)
                  << " remainingSessions=" << (m_server->SessionCount() - 1)
                  << std::endl;   // E-WS-FORCE-CLOSE
    }

    m_wq.Stop();   // 3. cancel 看门狗（同 strand；关停期无并发）

    // 3b. ★ P0-1（R2 断环统一化，同 Plain）：任何原因的 ForceClose 都必然断环；
    //     非 write_stall 路径在此同步 ClearCallbacks（安全论证见 Plain 侧注释），
    //     write_stall 路径由 OnWriteStall 末尾的 net::post 延迟断环负责。
    if (reason != CloseReason::write_stall) {
        m_wq.ClearCallbacks();
    }

    beast::error_code ignored;
    try {
        beast::get_lowest_layer(m_ws).socket().shutdown(tcp::socket::shutdown_both, ignored);
        beast::get_lowest_layer(m_ws).socket().close(ignored);
    } catch (...) {}   // R-03

    m_server->OnSessionDisconnected(this);
    m_server->OnSessionForceClosed();        // O-v2-2：只 forceClosed++

    // ★ R-01：最后一条语句，此后不得访问任何成员
    m_self.reset();
}

// ============================================================
// HTTP Server Implementation
// ============================================================
HttpServer::HttpServer(uint16_t httpPort, uint16_t httpsPort)
    : m_httpPort(httpPort)
    , m_httpsPort(httpsPort)
    , m_ioc(4)
    , m_sslCtx(ssl::context::tlsv12)
    , m_plainAcceptor(m_ioc) {}

HttpServer::~HttpServer() {
    Stop();
}

bool HttpServer::GenerateSelfSignedCert() {
    m_sslCtx.set_options(
        ssl::context::default_workarounds |
        ssl::context::no_sslv2 |
        ssl::context::no_sslv3 |
        ssl::context::single_dh_use);

    EVP_PKEY* pkey = nullptr;
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!pctx) {
        std::cerr << "[SSL] Failed to create EVP_PKEY_CTX" << std::endl;
        return false;
    }

    if (EVP_PKEY_keygen_init(pctx) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048) <= 0 ||
        EVP_PKEY_keygen(pctx, &pkey) <= 0) {
        unsigned long err = ERR_get_error();
        char errBuf[256];
        ERR_error_string_n(err, errBuf, sizeof(errBuf));
        std::cerr << "[SSL] Failed to generate RSA key: " << errBuf << std::endl;
        EVP_PKEY_CTX_free(pctx);
        if (pkey) EVP_PKEY_free(pkey);
        return false;
    }
    EVP_PKEY_CTX_free(pctx);

    X509* x509 = X509_new();
    if (!x509) { EVP_PKEY_free(pkey); return false; }

    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 365 * 24 * 3600);

    X509_set_pubkey(x509, pkey);

    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (unsigned char*)"SoundShareServer", -1, -1, 0);
    X509_set_issuer_name(x509, name);

    X509_sign(x509, pkey, EVP_sha256());

    BIO* certBio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(certBio, x509);
    BUF_MEM* certBuf;
    BIO_get_mem_ptr(certBio, &certBuf);
    std::string certPem(certBuf->data, certBuf->length);

    BIO* keyBio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(keyBio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    BUF_MEM* keyBuf;
    BIO_get_mem_ptr(keyBio, &keyBuf);
    std::string keyPem(keyBuf->data, keyBuf->length);

    boost::system::error_code ec;
    m_sslCtx.use_certificate_chain(net::buffer(certPem.data(), certPem.size()), ec);
    if (ec) {
        std::cerr << "[SSL] Failed to load certificate: " << ec.message() << std::endl;
        BIO_free(certBio); BIO_free(keyBio); X509_free(x509); EVP_PKEY_free(pkey);
        return false;
    }

    m_sslCtx.use_private_key(net::buffer(keyPem.data(), keyPem.size()), ssl::context::pem, ec);
    if (ec) {
        std::cerr << "[SSL] Failed to load private key: " << ec.message() << std::endl;
        BIO_free(certBio); BIO_free(keyBio); X509_free(x509); EVP_PKEY_free(pkey);
        return false;
    }

    BIO_free(certBio);
    BIO_free(keyBio);
    X509_free(x509);
    EVP_PKEY_free(pkey);

    std::cout << "[SSL] Self-signed certificate generated successfully" << std::endl;
    return true;
}

bool HttpServer::Start() {
    ERR_load_crypto_strings();
    OpenSSL_add_all_algorithms();

    beast::error_code ec;

    // ---- Start plain HTTP listener ----
    if (m_httpPort > 0) {
        auto endpoint = tcp::endpoint(tcp::v4(), m_httpPort);
        m_plainAcceptor.open(endpoint.protocol(), ec);
        if (ec) {
            std::cerr << "[Server] Failed to open HTTP acceptor: " << ec.message() << std::endl;
            return false;
        }
        m_plainAcceptor.set_option(net::socket_base::reuse_address(true), ec);
        m_plainAcceptor.bind(endpoint, ec);
        if (ec) {
            std::cerr << "[Server] Failed to bind HTTP port " << m_httpPort << ": " << ec.message() << std::endl;
            return false;
        }
        m_plainAcceptor.listen(net::socket_base::max_listen_connections, ec);
        if (ec) {
            std::cerr << "[Server] Failed to listen HTTP: " << ec.message() << std::endl;
            return false;
        }
        DoAcceptPlain();
        std::cout << "[Server] HTTP  listening on http://0.0.0.0:" << m_httpPort << std::endl;
    }

    // ---- Start HTTPS listener (optional) ----
    if (m_httpsPort > 0) {
        if (!GenerateSelfSignedCert()) {
            std::cerr << "[Server] Failed to generate SSL certificate, HTTPS disabled" << std::endl;
        } else {
            m_sslAcceptor.emplace(m_ioc);
            auto endpoint = tcp::endpoint(tcp::v4(), m_httpsPort);
            m_sslAcceptor->open(endpoint.protocol(), ec);
            if (ec) {
                std::cerr << "[Server] Failed to open HTTPS acceptor: " << ec.message() << std::endl;
            } else {
                m_sslAcceptor->set_option(net::socket_base::reuse_address(true), ec);
                m_sslAcceptor->bind(endpoint, ec);
                if (ec) {
                    std::cerr << "[Server] Failed to bind HTTPS port " << m_httpsPort << ": " << ec.message() << std::endl;
                } else {
                    m_sslAcceptor->listen(net::socket_base::max_listen_connections, ec);
                    if (ec) {
                        std::cerr << "[Server] Failed to listen HTTPS: " << ec.message() << std::endl;
                    } else {
                        DoAcceptSsl();
                        std::cout << "[Server] HTTPS listening on https://0.0.0.0:" << m_httpsPort << std::endl;
                    }
                }
            }
        }
    }

    // Start worker threads
    int numThreads = 4;
    for (int i = 0; i < numThreads; i++) {
        m_threads.emplace_back([this]() {
            m_ioc.run();
        });
    }

    return true;
}

void HttpServer::Stop() {
    // A22（M-5）步骤 0：幂等闸门（~HttpServer 与 main.cpp 关停各调一次）
    if (m_stopped.exchange(true)) return;
    // 1. 停止 io_context
    m_ioc.stop();
    // 2. join 全部 io 线程
    for (auto& t : m_threads) {
        if (t.joinable()) t.join();
    }
    m_threads.clear();
    // 3. ★ 此处已【无任何并发】：所有 io 线程已退出，所有 strand 空闲且永不再
    //    运行 —— 契约 v2 (b)（§5.4.3）下主线程 ForceClose(server_stop) 合法。
    for (auto* s : SnapshotSessions()) {
        s->ForceClose(CloseReason::server_stop);   // 静默（§6.3：关停不是异常）
    }
    // 4. 残留检查（E-WS-SHUTDOWN-LEAK；进程生命周期至多 1 行）
    const size_t remaining = SessionCount();
    if (remaining > 0) {
        std::cerr << "[WS][ERROR] shutdown: " << remaining
                  << " session(s) not terminated" << std::endl;
    }
}

// ---- A22：L1 内拷贝会话快照 ----
std::vector<WebSocketSessionBase*> HttpServer::SnapshotSessions() {
    std::lock_guard<std::mutex> lock(m_sessionsMutex);
    return std::vector<WebSocketSessionBase*>(m_sessions.begin(), m_sessions.end());
}

void HttpServer::DoAcceptPlain() {
    m_plainAcceptor.async_accept(
        net::make_strand(m_ioc),
        [this](beast::error_code ec, tcp::socket socket) {
            if (ec) {
                if (ec != net::error::operation_aborted) {
                    std::cerr << "[Server] HTTP accept error: " << ec.message() << std::endl;
                }
                return;
            }

            auto session = std::make_shared<PlainHttpSession>(
                beast::tcp_stream(std::move(socket)),
                this);
            session->Run();

            DoAcceptPlain();
        });
}

void HttpServer::DoAcceptSsl() {
    if (!m_sslAcceptor) return;

    m_sslAcceptor->async_accept(
        net::make_strand(m_ioc),
        [this](beast::error_code ec, tcp::socket socket) {
            if (ec) {
                if (ec != net::error::operation_aborted) {
                    std::cerr << "[Server] HTTPS accept error: " << ec.message() << std::endl;
                }
                return;
            }

            auto session = std::make_shared<SslHttpSession>(
                beast::ssl_stream<beast::tcp_stream>(std::move(socket), m_sslCtx),
                this);
            session->Run();

            DoAcceptSsl();
        });
}

void HttpServer::OnSessionConnected(WebSocketSessionBase* session) {
    {
        std::lock_guard<std::mutex> lock(m_sessionsMutex);
        m_sessions.insert(session);
    }
    // A18 (NFR-LOG-1/F-4): "[Server] WebSocket client connected" removed.
    if (m_connectCallback) {
        m_connectCallback(session, true);
    }
}

void HttpServer::OnSessionDisconnected(WebSocketSessionBase* session) {
    {
        std::lock_guard<std::mutex> lock(m_sessionsMutex);
        m_sessions.erase(session);
    }
    // A19 (NFR-LOG-1/F-4): "[Server] WebSocket client disconnected" removed;
    // disconnect semantics stay silent on ALL paths (section 6.1). The "erase under
    // lock, then callback out of lock" structure is preserved (section 7.1).
    if (m_connectCallback) {
        m_connectCallback(session, false);
    }
}

void HttpServer::OnSessionMessage(WebSocketSessionBase* session, const std::string& msg) {
    if (m_messageCallback) {
        m_messageCallback(session, msg);
    }
}

// ---- A4: 探针实现（三入口共用：/_debug/sessions、stdin dump、进程内调用）----
size_t HttpServer::SessionCount() const {
    std::lock_guard<std::mutex> lock(m_sessionsMutex);
    return m_sessions.size();
}

std::string HttpServer::DumpProbeJson() const {
    std::vector<ProbeEntry> entries;
    size_t sessionCount = 0;

    {   // ① 持 L1 —— ★ 全程持有，直到所有 FillProbe 完成（M-2 / §5.8.4）。
        //   不变式"在 m_sessions 中 ⇒ 未进入析构"只在持 L1 期间成立；
        //   释放 L1 后 ForceClose 可完成 erase → m_self.reset() → 析构，
        //   此后不得再解引用任何 session 指针。
        std::lock_guard<std::mutex> lk(m_sessionsMutex);
        sessionCount = m_sessions.size();
        entries.reserve(sessionCount);
        for (auto* s : m_sessions) {
            ProbeEntry e{};
            s->FillProbe(e);        // 内部取 L3（叶锁）；★ 严禁触碰 m_ws
            entries.push_back(e);   // 只保留 POD，不保留任何指针
        }
    }   // ← 释放 L1

    // ② 出 L1 之后才取 L2（顺序获取，严禁 L1→L2 嵌套）
    std::string extra = m_probeHook ? m_probeHook() : std::string{};

    // ③ 无锁序列化
    ProbeCounters c;
    c.sessions = sessionCount;
    c.liveSessions = m_liveSessions.load(std::memory_order_relaxed);
    c.forceClosed = m_forceClosed.load(std::memory_order_relaxed);
    c.writeStalls = m_writeStalls.load(std::memory_order_relaxed);

    ProbeConfig cfg;
    const uint32_t t = m_faults.StallThresholdMs();
    cfg.serverWriteStallMs = t;
    cfg.stallGraceMs = std::min<uint32_t>(1000u, t / 10u);
    cfg.effectiveStallMs = WriteQueue::EffectiveThresholdMs(t);
    cfg.maxBinaryFrames = QueueConfig{}.maxBinaryFrames;
    cfg.maxQueuedBytes = QueueConfig{}.maxQueuedBytes;
    cfg.maxTextFrames = QueueConfig{}.maxTextFrames;

    return SerializeProbeJson(entries, extra, c, cfg);
}

// ---- T-GPU-AUDIO-GLITCH S0 (F6)：观测端点实现 ----
void HttpServer::ResetStatsWindow() {
    if (m_statsResetHook) m_statsResetHook();
}

std::string HttpServer::CaptureHist(const std::string& name) {
    if (m_captureHistHook) return m_captureHistHook(name);
    return "{\"error\":\"capture hist hook not registered\"}";
}

void HttpServer::BroadcastBinary(const void* data, size_t size) {
    // A20：原实现持 L1 遍历并 SendBinary（L1→L3 违规嵌套，§5.4.2）→
    // 改为锁内拷快照、出锁发送（与采集线程路径同构）。
    auto snapshot = SnapshotSessions();
    for (auto* session : snapshot) {
        try {
            session->SendBinary(data, size);
        } catch (...) {}
    }
}

void HttpServer::BroadcastText(const std::string& text) {
    // A21：同 A20（锁内拷快照、出锁发送）。
    auto snapshot = SnapshotSessions();
    for (auto* session : snapshot) {
        try {
            session->Send(text);
        } catch (...) {}
    }
}
