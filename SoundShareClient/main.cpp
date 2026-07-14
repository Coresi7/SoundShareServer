// SoundShareClient - Minimal Windows console client for SoundShareServer
//
// Usage:
//   SoundShareClient <ip> [port]
//   Default port: 8080
//
// Protocol: ws://<ip>:<port>/ws
//   Send {"type":"start"} to begin, {"type":"stop"} on exit

#include "ring_buffer.h"
#include "wasapi_render.h"
#include "ws_client.h"

#include <windows.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

static std::atomic<bool> g_running{true};
static std::atomic<int> g_exitCode{0};

void SignalHandler(int) {
    g_running.store(false);
}

static bool ExtractJsonUint(const std::string& json, const char* key, uint32_t& out) {
    std::string needle = std::string("\"") + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    char* end = nullptr;
    unsigned long val = std::strtoul(json.c_str() + pos, &end, 10);
    if (end == json.c_str() + pos) return false;
    out = static_cast<uint32_t>(val);
    return true;
}

static bool ExtractJsonString(const std::string& json, const char* key, std::string& out) {
    std::string needle = std::string("\"") + key + "\":\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos += needle.size();
    auto end = json.find('"', pos);
    if (end == std::string::npos) return false;
    out = json.substr(pos, end - pos);
    return true;
}

static bool ParseAudioConfig(const std::string& msg, uint32_t& sampleRate, uint32_t& channels, std::string& format) {
    if (msg.find("\"audio_config\"") == std::string::npos) return false;
    if (!ExtractJsonUint(msg, "sampleRate", sampleRate)) return false;
    if (!ExtractJsonUint(msg, "channels", channels)) return false;
    if (!ExtractJsonString(msg, "format", format)) return false;
    return true;
}

static void PrintUsage() {
    std::cout << "Usage: SoundShareClient <ip> [port]" << std::endl;
    std::cout << "  Default port: 8080" << std::endl;
    std::cout << "  Example: SoundShareClient 192.168.1.10" << std::endl;
    std::cout << "           SoundShareClient 192.168.1.10 9090" << std::endl;
}

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    std::string host = argv[1];
    uint16_t port = 8080;
    if (argc >= 3) {
        char* end = nullptr;
        unsigned long p = std::strtoul(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || p == 0 || p > 65535) {
            std::cerr << "Invalid port: " << argv[2] << std::endl;
            return 1;
        }
        port = static_cast<uint16_t>(p);
    }

    std::cout << "======================================" << std::endl;
    std::cout << "  SoundShare Client" << std::endl;
    std::cout << "======================================" << std::endl;
    std::cout << "  Server: ws://" << host << ":" << port << "/ws" << std::endl;
    std::cout << "  Press Ctrl+C to stop." << std::endl;
    std::cout << std::endl;

    // 2 seconds of stereo PCM at 48kHz
    constexpr size_t kRingCapacity = 48000 * 2 * 2;
    RingBuffer ringBuffer(kRingCapacity);

    WasapiRender renderer;
    WsClient client;

    std::atomic<bool> configReceived{false};
    std::atomic<bool> playbackStarted{false};
    std::atomic<bool> streaming{false};

    client.SetErrorHandler([&](const std::string& err) {
        std::cerr << "[Client] Connection error: " << err << std::endl;
        g_exitCode.store(2);
        g_running.store(false);
    });

    client.SetTextHandler([&](const std::string& msg) {
        uint32_t sampleRate = 0, channels = 0;
        std::string format;
        if (!ParseAudioConfig(msg, sampleRate, channels, format)) return;

        if (sampleRate != 48000 || format != "pcm16" || (channels != 1 && channels != 2)) {
            std::cerr << "[Client] Unsupported audio config: "
                      << sampleRate << " Hz, " << channels << " ch, " << format << std::endl;
            g_exitCode.store(3);
            g_running.store(false);
            return;
        }

        configReceived.store(true);
        ringBuffer.Clear();  // Clear any stale data from previous session/idle period
        renderer.SetRingBuffer(&ringBuffer);
        if (!renderer.Initialize(sampleRate, static_cast<uint16_t>(channels))) {
            std::cerr << "[Client] Failed to init renderer" << std::endl;
            g_exitCode.store(3);
            g_running.store(false);
            return;
        }
        if (!renderer.Start()) {
            std::cerr << "[Client] Failed to start playback: " << renderer.GetLastError() << std::endl;
            g_exitCode.store(3);
            g_running.store(false);
            return;
        }

        playbackStarted.store(true);
        streaming.store(true);
        std::cout << "[Client] Playing audio (" << sampleRate << " Hz, "
                  << channels << " ch)" << std::endl;
    });

    client.SetBinaryHandler([&](const uint8_t* data, size_t size) {
        if (!streaming.load()) return;
        if (size % sizeof(int16_t) != 0) {
            std::cerr << "[Client] Invalid PCM frame size: " << size << std::endl;
            g_exitCode.store(3);
            g_running.store(false);
            return;
        }
        const auto* samples = reinterpret_cast<const int16_t*>(data);
        ringBuffer.Push(samples, size / sizeof(int16_t));
    });

    // Retry loop: auto-reconnect on disconnection (same behavior as browser client).
    // WebSocket keep-alive pings (configured server-side) handle NAT/firewall idle timeouts.
    int reconnectAttempt = 0;
    constexpr int kMaxReconnectDelay = 30000; // cap backoff at 30s

    while (g_running.load()) {
        reconnectAttempt++;

        std::cout << "[Client] Connecting..." << std::endl;
        if (!client.Connect(host, port)) {
            std::cerr << "[Client] Connection failed" << std::endl;
            if (!g_running.load()) break;
            int delay = std::min(1000 * reconnectAttempt, kMaxReconnectDelay);
            std::cout << "[Client] Retrying in " << (delay / 1000) << "s..." << std::endl;
            for (int waited = 0; waited < delay && g_running.load(); waited += 100) {
                Sleep(100);
            }
            continue;
        }
        std::cout << "[Client] Connected" << std::endl;

        if (!client.SendText(R"({"type":"start"})")) {
            std::cerr << "[Client] Failed to send start" << std::endl;
            client.Disconnect();
            if (!g_running.load()) break;
            int delay = std::min(1000 * reconnectAttempt, kMaxReconnectDelay);
            std::cout << "[Client] Retrying in " << (delay / 1000) << "s..." << std::endl;
            for (int waited = 0; waited < delay && g_running.load(); waited += 100) {
                Sleep(100);
            }
            continue;
        }

        // Reset reconnect backoff on successful connection
        reconnectAttempt = 0;

        // Wait for audio_config (timeout 10s)
        configReceived.store(false);
        for (int i = 0; i < 100 && g_running.load() && !configReceived.load(); i++) {
            Sleep(100);
        }
        if (!configReceived.load()) {
            std::cerr << "[Client] Timeout waiting for audio_config" << std::endl;
            client.SendText(R"({"type":"stop"})");
            client.Disconnect();
            if (!g_running.load()) break;
            std::cout << "[Client] Reconnecting in 1s..." << std::endl;
            for (int waited = 0; waited < 1000 && g_running.load(); waited += 100) {
                Sleep(100);
            }
            continue;
        }

        // Main streaming loop — wait until disconnected or Ctrl+C
        while (g_running.load() && client.IsConnected()) {
            Sleep(100);
        }

        streaming.store(false);
        if (playbackStarted.load()) {
            renderer.Stop();
            playbackStarted.store(false);
        }

        if (client.IsConnected()) {
            client.SendText(R"({"type":"stop"})");
        }
        client.RequestStop();
        client.Disconnect();

        if (!g_running.load()) break;

        // Disconnected unexpectedly — reconnect
        std::cout << "[Client] Connection lost. Reconnecting in 1s..." << std::endl;
        for (int waited = 0; waited < 1000 && g_running.load(); waited += 100) {
            Sleep(100);
        }
    }

    int code = g_exitCode.load();
    if (code == 0 && !g_running.load()) {
        std::cout << "[Client] Stopped." << std::endl;
    }
    return code;
}
