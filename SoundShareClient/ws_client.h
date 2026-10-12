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
// B1 (FR-08/SS5.10): audio-stall callback.
// * M-6 contract: invoked ON THE IO THREAD -- only non-blocking flag-setting /
//   logging is allowed. Calling Disconnect()/RequestStop()/SendText() here is
//   FORBIDDEN (DL-4/R-08: Disconnect() waits on a future posted to the SAME
//   io_context -> io-thread self-deadlock). Teardown is done by the main loop
//   polling IsStalled() (SS5.10.3).
using WsStallHandler = std::function<void(uint64_t silentMs)>;

class WsClient {
public:
    WsClient();
    ~WsClient();

    void SetTextHandler(WsTextHandler h) { m_onText = std::move(h); }
    void SetBinaryHandler(WsBinaryHandler h) { m_onBinary = std::move(h); }
    void SetErrorHandler(WsErrorHandler h) { m_onError = std::move(h); }
    void SetStallHandler(WsStallHandler h) { m_onStall = std::move(h); }

    bool Connect(const std::string& host, uint16_t port);
    bool SendText(const std::string& text);
    void RequestStop();
    void Disconnect();
    bool IsConnected() const { return m_connected.load(); }
    // B1: stall flag for main-loop polling (FR-08). Reset points (M-6):
    // Connect() entry (authoritative) and Disconnect() end (second).
    bool IsStalled() const { return m_stalled.load(); }

private:
    void IoThreadMain();
    void DoRead();
    void OnRead(beast::error_code ec, std::size_t bytes);
    void Fail(const std::string& reason);
    // B3: watchdog timer (bound to *m_ioc, created inside IoThreadMain)
    void StartWatchdog();
    void OnWatchdogTick(beast::error_code ec);

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
    WsStallHandler m_onStall;

    // ---- B2: stall watchdog state (FR-08 / SS5.10) ----
    // m_armed: set on FIRST binary frame only (B-1: no misfire before any PCM).
    // m_stalled: set once by the watchdog (io thread), reset by Connect/Disconnect.
    // Invariant (M-6, see Connect()): after Connect() returns and until the next
    // stall detection, IsStalled() == false -- otherwise the reconnect loop would
    // exit immediately and AC-05 ("audio restored after reconnect") would fail.
    std::atomic<uint64_t> m_lastBinaryMs{0};
    std::atomic<bool> m_armed{false};
    std::atomic<bool> m_stalled{false};
    std::unique_ptr<net::steady_timer> m_watchdog;
    const uint32_t m_stallMs = 15000;   // T_client_stall (SS11-1)
};
