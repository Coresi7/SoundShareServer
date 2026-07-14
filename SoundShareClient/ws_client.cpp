#include "ws_client.h"

#include <future>
#include <iostream>

WsClient::WsClient() = default;

WsClient::~WsClient() {
    Disconnect();
}

bool WsClient::Connect(const std::string& host, uint16_t port) {
    if (m_connected.load()) return true;

    m_host = host;
    m_port = port;
    m_stopRequested.store(false);

    try {
        m_ioc = std::make_unique<net::io_context>();
        auto resolver = tcp::resolver(*m_ioc);
        auto results = resolver.resolve(host, std::to_string(port));

        beast::tcp_stream stream(*m_ioc);
        stream.expires_after(std::chrono::seconds(10));
        stream.connect(results);

        m_ws = std::make_unique<websocket::stream<beast::tcp_stream>>(std::move(stream));

        // Match server-side timeout settings so the client doesn't time out
        // before the server's keep-alive pings arrive.  Beast's suggested(client)
        // uses a 23s idle timeout with no pings — far too short when the system
        // has no active audio (WASAPI produces no packets, connection goes idle).
        websocket::stream_base::timeout wsTimeout{};
        wsTimeout.handshake_timeout = std::chrono::seconds(30);
        wsTimeout.idle_timeout = std::chrono::seconds(120);
        wsTimeout.keep_alive_pings = true;
        m_ws->set_option(wsTimeout);

        m_ws->set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req) {
                req.set(http::field::user_agent, "SoundShareClient/1.0");
            }));

        std::string target = "/ws";
        m_ws->handshake(host, target);

        // Clear tcp_stream expiry — Beast docs: do not use TCP timeouts
        // on an active WebSocket (server does the same in OnAccept).
        beast::get_lowest_layer(*m_ws).expires_never();

        m_connected.store(true);

        m_ioThread = std::thread(&WsClient::IoThreadMain, this);
        return true;
    } catch (const std::exception& ex) {
        Fail(std::string("Connect failed: ") + ex.what());
        m_ws.reset();
        m_ioc.reset();
        return false;
    }
}

void WsClient::IoThreadMain() {
    DoRead();
    try {
        m_ioc->run();
    } catch (const std::exception& ex) {
        if (!m_stopRequested.load()) {
            Fail(std::string("IO error: ") + ex.what());
        }
    }
}

void WsClient::DoRead() {
    if (!m_ws || m_stopRequested.load()) return;

    m_ws->async_read(
        m_buffer,
        [this](beast::error_code ec, std::size_t bytes) {
            OnRead(ec, bytes);
        });
}

void WsClient::OnRead(beast::error_code ec, std::size_t bytes) {
    if (m_stopRequested.load()) return;

    if (ec) {
        if (ec == beast::error::timeout) {
            Fail("Read failed: connection timed out");
        } else if (ec != websocket::error::closed) {
            Fail(std::string("Read failed: ") + ec.message());
        }
        m_connected.store(false);
        return;
    }

    if (m_ws->got_text()) {
        std::string msg = beast::buffers_to_string(m_buffer.data());
        m_buffer.consume(bytes);
        if (m_onText) m_onText(msg);
    } else {
        auto data = m_buffer.data();
        std::string payload = beast::buffers_to_string(data);
        if (m_onBinary) {
            m_onBinary(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
        }
        m_buffer.consume(bytes);
    }

    DoRead();
}

bool WsClient::SendText(const std::string& text) {
    if (!m_ws || !m_connected.load() || !m_ioc) return false;

    auto promise = std::make_shared<std::promise<bool>>();
    auto future = promise->get_future();
    net::post(m_ioc->get_executor(), [this, text, promise]() {
        try {
            m_ws->write(net::buffer(text));
            promise->set_value(true);
        } catch (const std::exception& ex) {
            Fail(std::string("Send failed: ") + ex.what());
            promise->set_value(false);
        }
    });
    return future.get();
}

void WsClient::RequestStop() {
    m_stopRequested.store(true);
}

void WsClient::Disconnect() {
    m_stopRequested.store(true);
    m_connected.store(false);

    if (m_ioc && m_ws && m_ioThread.joinable()) {
        auto promise = std::make_shared<std::promise<void>>();
        auto future = promise->get_future();
        net::post(m_ioc->get_executor(), [this, promise]() {
            beast::error_code ec;
            m_ws->close(websocket::close_code::normal, ec);
            m_ioc->stop();
            promise->set_value();
        });
        future.wait_for(std::chrono::seconds(2));
    } else if (m_ioc) {
        m_ioc->stop();
    }

    if (m_ioThread.joinable()) {
        m_ioThread.join();
    }

    m_ws.reset();
    m_ioc.reset();
}

void WsClient::Fail(const std::string& reason) {
    if (m_stopRequested.load()) return;
    std::cerr << "[WS] " << reason << std::endl;
    if (m_onError) m_onError(reason);
    m_connected.store(false);
}
