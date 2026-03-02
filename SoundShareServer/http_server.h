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

// Message wrapper that explicitly tracks text vs binary mode
struct WriteMessage {
    std::vector<uint8_t> data;
    bool isText = false;
};

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

private:
    void OnAccept(beast::error_code ec);
    void DoRead();
    void OnRead(beast::error_code ec, std::size_t bytes_transferred);
    void DoWrite();
    void OnWrite(beast::error_code ec, std::size_t bytes_transferred);

    websocket::stream<beast::tcp_stream> m_ws;
    beast::flat_buffer m_buffer;
    HttpServer* m_server;

    std::mutex m_writeMutex;
    std::deque<std::shared_ptr<WriteMessage>> m_writeQueue;
    bool m_writing = false;
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

private:
    void OnAccept(beast::error_code ec);
    void DoRead();
    void OnRead(beast::error_code ec, std::size_t bytes_transferred);
    void DoWrite();
    void OnWrite(beast::error_code ec, std::size_t bytes_transferred);

    websocket::stream<beast::ssl_stream<beast::tcp_stream>> m_ws;
    beast::flat_buffer m_buffer;
    HttpServer* m_server;

    std::mutex m_writeMutex;
    std::deque<std::shared_ptr<WriteMessage>> m_writeQueue;
    bool m_writing = false;
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

    ssl::context& GetSslContext() { return m_sslCtx; }

private:
    void DoAcceptPlain();
    void DoAcceptSsl();

    // Generate self-signed certificate in memory (only if HTTPS is enabled)
    bool GenerateSelfSignedCert();

    uint16_t m_httpPort;
    uint16_t m_httpsPort;
    net::io_context m_ioc;
    ssl::context m_sslCtx;
    tcp::acceptor m_plainAcceptor;
    std::optional<tcp::acceptor> m_sslAcceptor;

    std::vector<std::thread> m_threads;

    std::mutex m_sessionsMutex;
    std::set<WebSocketSessionBase*> m_sessions;

    WsMessageCallback m_messageCallback;
    WsConnectCallback m_connectCallback;
};
