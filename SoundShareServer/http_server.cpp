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

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

// ============================================================
// Common: handle HTTP request and produce response
// ============================================================
static void HandleHttpRequest(
    http::request<http::string_body>& req,
    std::function<void(http::response<http::string_body>&&)> sendResponse)
{
    auto target = std::string(req.target());

    if (target == "/" || target == "/index.html") {
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

        HandleHttpRequest(m_req, [this](http::response<http::string_body>&& res) {
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

        HandleHttpRequest(m_req, [this](http::response<http::string_body>&& res) {
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
static const size_t MAX_QUEUE_DEPTH = 200;

static void EnqueueText(
    std::deque<std::shared_ptr<WriteMessage>>& queue,
    std::mutex& mtx,
    bool& writing,
    const std::string& text,
    std::function<void()> doWrite)
{
    auto msg = std::make_shared<WriteMessage>();
    msg->data.assign(text.begin(), text.end());
    msg->isText = true;

    std::lock_guard<std::mutex> lock(mtx);
    queue.push_back(msg);
    if (!writing) {
        writing = true;
        doWrite();
    }
}

static void EnqueueBinary(
    std::deque<std::shared_ptr<WriteMessage>>& queue,
    std::mutex& mtx,
    bool& writing,
    const void* data, size_t size,
    std::function<void()> doWrite)
{
    auto msg = std::make_shared<WriteMessage>();
    msg->data.assign(
        static_cast<const uint8_t*>(data),
        static_cast<const uint8_t*>(data) + size);
    msg->isText = false;

    std::lock_guard<std::mutex> lock(mtx);
    while (queue.size() >= MAX_QUEUE_DEPTH) {
        if (!queue.empty() && !queue.front()->isText) {
            queue.pop_front();
        } else {
            break;
        }
    }
    queue.push_back(msg);
    if (!writing) {
        writing = true;
        doWrite();
    }
}

// ============================================================
// Plain WebSocket Session Implementation
// ============================================================
PlainWebSocketSession::PlainWebSocketSession(beast::tcp_stream&& stream, HttpServer* server)
    : m_ws(std::move(stream)), m_server(server) {}

PlainWebSocketSession::~PlainWebSocketSession() {
    m_server->OnSessionDisconnected(this);
}

void PlainWebSocketSession::Run(http::request<http::string_body> req) {
    websocket::stream_base::timeout opt{};
    opt.handshake_timeout = std::chrono::seconds(30);
    opt.idle_timeout = std::chrono::seconds(300);
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
        std::cerr << "[WS] Accept error: " << ec.message() << std::endl;
        return;
    }
    beast::get_lowest_layer(m_ws).expires_never();
    m_server->OnSessionConnected(this);
    DoRead();
}

void PlainWebSocketSession::DoRead() {
    m_ws.async_read(
        m_buffer,
        [self = shared_from_this()](beast::error_code ec, std::size_t bytes_transferred) {
            self->OnRead(ec, bytes_transferred);
        });
}

void PlainWebSocketSession::OnRead(beast::error_code ec, std::size_t) {
    if (ec) return;

    if (m_ws.got_text()) {
        std::string msg(static_cast<const char*>(m_buffer.data().data()), m_buffer.size());
        m_server->OnSessionMessage(this, msg);
    }

    m_buffer.consume(m_buffer.size());
    DoRead();
}

void PlainWebSocketSession::Send(const std::string& text) {
    EnqueueText(m_writeQueue, m_writeMutex, m_writing, text,
        [self = shared_from_this()]() { self->DoWrite(); });
}

void PlainWebSocketSession::SendBinary(const void* data, size_t size) {
    EnqueueBinary(m_writeQueue, m_writeMutex, m_writing, data, size,
        [self = shared_from_this()]() { self->DoWrite(); });
}

void PlainWebSocketSession::DoWrite() {
    if (m_writeQueue.empty()) {
        m_writing = false;
        return;
    }
    auto msg = m_writeQueue.front();
    m_ws.text(msg->isText);
    m_ws.binary(!msg->isText);
    m_ws.async_write(
        net::buffer(msg->data.data(), msg->data.size()),
        [self = shared_from_this(), msg](beast::error_code ec, std::size_t) {
            self->OnWrite(ec, 0);
        });
}

void PlainWebSocketSession::OnWrite(beast::error_code ec, std::size_t) {
    if (ec) {
        std::lock_guard<std::mutex> lock(m_writeMutex);
        m_writing = false;
        m_writeQueue.clear();
        return;
    }
    std::lock_guard<std::mutex> lock(m_writeMutex);
    if (!m_writeQueue.empty()) {
        m_writeQueue.pop_front();
    }
    DoWrite();
}

bool PlainWebSocketSession::IsOpen() const {
    return m_ws.is_open();
}

void PlainWebSocketSession::Close() {
    try {
        m_ws.close(websocket::close_code::normal);
    } catch (...) {}
}

// ============================================================
// SSL WebSocket Session Implementation
// ============================================================
SslWebSocketSession::SslWebSocketSession(beast::ssl_stream<beast::tcp_stream>&& stream, HttpServer* server)
    : m_ws(std::move(stream)), m_server(server) {}

SslWebSocketSession::~SslWebSocketSession() {
    m_server->OnSessionDisconnected(this);
}

void SslWebSocketSession::Run(http::request<http::string_body> req) {
    websocket::stream_base::timeout opt{};
    opt.handshake_timeout = std::chrono::seconds(30);
    opt.idle_timeout = std::chrono::seconds(300);
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
        std::cerr << "[WS] Accept error: " << ec.message() << std::endl;
        return;
    }
    beast::get_lowest_layer(m_ws).expires_never();
    m_server->OnSessionConnected(this);
    DoRead();
}

void SslWebSocketSession::DoRead() {
    m_ws.async_read(
        m_buffer,
        [self = shared_from_this()](beast::error_code ec, std::size_t bytes_transferred) {
            self->OnRead(ec, bytes_transferred);
        });
}

void SslWebSocketSession::OnRead(beast::error_code ec, std::size_t) {
    if (ec) return;

    if (m_ws.got_text()) {
        std::string msg(static_cast<const char*>(m_buffer.data().data()), m_buffer.size());
        m_server->OnSessionMessage(this, msg);
    }

    m_buffer.consume(m_buffer.size());
    DoRead();
}

void SslWebSocketSession::Send(const std::string& text) {
    EnqueueText(m_writeQueue, m_writeMutex, m_writing, text,
        [self = shared_from_this()]() { self->DoWrite(); });
}

void SslWebSocketSession::SendBinary(const void* data, size_t size) {
    EnqueueBinary(m_writeQueue, m_writeMutex, m_writing, data, size,
        [self = shared_from_this()]() { self->DoWrite(); });
}

void SslWebSocketSession::DoWrite() {
    if (m_writeQueue.empty()) {
        m_writing = false;
        return;
    }
    auto msg = m_writeQueue.front();
    m_ws.text(msg->isText);
    m_ws.binary(!msg->isText);
    m_ws.async_write(
        net::buffer(msg->data.data(), msg->data.size()),
        [self = shared_from_this(), msg](beast::error_code ec, std::size_t) {
            self->OnWrite(ec, 0);
        });
}

void SslWebSocketSession::OnWrite(beast::error_code ec, std::size_t) {
    if (ec) {
        std::lock_guard<std::mutex> lock(m_writeMutex);
        m_writing = false;
        m_writeQueue.clear();
        return;
    }
    std::lock_guard<std::mutex> lock(m_writeMutex);
    if (!m_writeQueue.empty()) {
        m_writeQueue.pop_front();
    }
    DoWrite();
}

bool SslWebSocketSession::IsOpen() const {
    return m_ws.is_open();
}

void SslWebSocketSession::Close() {
    try {
        m_ws.close(websocket::close_code::normal);
    } catch (...) {}
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
    m_ioc.stop();
    for (auto& t : m_threads) {
        if (t.joinable()) t.join();
    }
    m_threads.clear();
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
    std::cout << "[Server] WebSocket client connected. Total: " << m_sessions.size() << std::endl;
    if (m_connectCallback) {
        m_connectCallback(session, true);
    }
}

void HttpServer::OnSessionDisconnected(WebSocketSessionBase* session) {
    {
        std::lock_guard<std::mutex> lock(m_sessionsMutex);
        m_sessions.erase(session);
    }
    std::cout << "[Server] WebSocket client disconnected. Total: " << m_sessions.size() << std::endl;
    if (m_connectCallback) {
        m_connectCallback(session, false);
    }
}

void HttpServer::OnSessionMessage(WebSocketSessionBase* session, const std::string& msg) {
    if (m_messageCallback) {
        m_messageCallback(session, msg);
    }
}

void HttpServer::BroadcastBinary(const void* data, size_t size) {
    std::lock_guard<std::mutex> lock(m_sessionsMutex);
    for (auto* session : m_sessions) {
        try {
            session->SendBinary(data, size);
        } catch (...) {}
    }
}

void HttpServer::BroadcastText(const std::string& text) {
    std::lock_guard<std::mutex> lock(m_sessionsMutex);
    for (auto* session : m_sessions) {
        try {
            session->Send(text);
        } catch (...) {}
    }
}
