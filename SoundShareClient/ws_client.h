#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

using WsTextHandler = std::function<void(const std::string&)>;
using WsBinaryHandler = std::function<void(const uint8_t*, size_t)>;
using WsErrorHandler = std::function<void(const std::string&)>;

class WsClient {
public:
    WsClient();
    ~WsClient();

    void SetTextHandler(WsTextHandler h) { m_onText = std::move(h); }
    void SetBinaryHandler(WsBinaryHandler h) { m_onBinary = std::move(h); }
    void SetErrorHandler(WsErrorHandler h) { m_onError = std::move(h); }

    bool Connect(const std::string& host, uint16_t port);
    bool SendText(const std::string& text);
    void RequestStop();
    void Disconnect();
    bool IsConnected() const { return m_connected.load(); }

private:
    void IoThreadMain();
    void DoRead();
    void OnRead(beast::error_code ec, std::size_t bytes);
    void Fail(const std::string& reason);

    std::string m_host;
    uint16_t m_port = 0;

    std::unique_ptr<net::io_context> m_ioc;
    std::unique_ptr<websocket::stream<beast::tcp_stream>> m_ws;
    std::thread m_ioThread;
    beast::flat_buffer m_buffer;

    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_stopRequested{false};

    WsTextHandler m_onText;
    WsBinaryHandler m_onBinary;
    WsErrorHandler m_onError;
};
