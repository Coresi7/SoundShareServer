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

    std::string host;
    uint16_t port = 8080;

    // T-GPU-AUDIO-GLITCH F4 (FR-05)：水位参数族（NFR-LAT-2 单一参数族）。
    // start 默认 80ms（区间 [60,120]）；resume = 0.75×start（滞回防振荡）；
    // 容量上限 capacityCap = kRingCapacity 维持 2s 不变（不得提高）。
    constexpr uint32_t kDefaultStartWaterMs = 80;
    uint32_t startWaterMs = kDefaultStartWaterMs;
    std::string dumpPcmPath;   // C1：--dump-pcm=<path>（默认关闭）

    // 位置参数只认非 "--" 开关（与既有 <ip> [port] 语义兼容）
    {
        const std::string kWaterPrefix = "--start-water-ms=";
        const std::string kDumpPrefix = "--dump-pcm=";
        const std::string kFaultPrefix = "--fault-render-fail=";
        int positional = 0;
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            if (arg.rfind(kWaterPrefix, 0) == 0) {
                char* end = nullptr;
                const long ms = std::strtol(arg.c_str() + kWaterPrefix.size(), &end, 10);
                if (end && *end == '\0' && ms >= 60 && ms <= 120) {
                    startWaterMs = static_cast<uint32_t>(ms);
                } else {
                    std::cout << "[Audio] Ignoring invalid " << arg
                              << " (expected --start-water-ms=<60..120>); using default "
                              << kDefaultStartWaterMs << " ms" << std::endl;
                }
            } else if (arg.rfind(kDumpPrefix, 0) == 0) {
                dumpPcmPath = arg.substr(kDumpPrefix.size());
            } else if (arg.rfind(kFaultPrefix, 0) == 0) {
                // handled below (kept for positional-skip parity)
            } else if (arg.rfind("--", 0) == 0) {
                // unknown switch: skip from positional parsing
            } else {
                ++positional;
                if (positional == 1) host = arg;
            }
        }
    }

    if (host.empty()) {
        PrintUsage();
        return 1;
    }

    // B13 (FR-05/NFR-WIN-4): --fault-render-fail=<padding|getbuffer|releasebuffer>
    // Application-level render-failure injection for AC-07; default off.
    std::string faultStage;
    {
        const std::string kPrefix = "--fault-render-fail=";
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            if (arg.rfind(kPrefix, 0) == 0) {
                faultStage = arg.substr(kPrefix.size());
            }
        }
    }
    if (argc >= 3) {
        // port may be positional #2 (skip switches)
        int positional = 0;
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            if (arg.rfind("--", 0) == 0) continue;
            ++positional;
            if (positional == 2) {
                char* end = nullptr;
                unsigned long p = std::strtoul(arg.c_str(), &end, 10);
                if (end == arg.c_str() || *end != '\0' || p == 0 || p > 65535) {
                    std::cerr << "Invalid port: " << arg << std::endl;
                    return 1;
                }
                port = static_cast<uint16_t>(p);
            }
        }
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

    if (!faultStage.empty()) {
        if (faultStage != "padding" && faultStage != "getbuffer" && faultStage != "releasebuffer") {
            std::cerr << "Invalid --fault-render-fail stage: " << faultStage
                      << " (expected padding|getbuffer|releasebuffer)" << std::endl;
            return 1;
        }
        renderer.SetFaultInjectStage(faultStage);
        std::cout << "[Audio] Fault injection enabled: render stage '" << faultStage << "'" << std::endl;
    }

    // ---- T-GPU-AUDIO-GLITCH F4 (FR-05)：装配水位参数（在 SetRingBuffer 之后、
    //      Initialize 之前；样本口径 = 交织样本，与 RingBuffer 槽位一致）----
    {
        const size_t startSamples = static_cast<size_t>(startWaterMs) * 48000 / 1000 * 2;
        const size_t resumeSamples = startSamples * 3 / 4;   // 0.75×start（滞回）
        const size_t fadeSamples = 48000 * 5 / 1000 * 2;     // 5ms（每声道 240 帧）
        renderer.SetWaterLevels(startSamples, resumeSamples, fadeSamples);
        if (!dumpPcmPath.empty()) {
            renderer.SetDumpPcmPath(dumpPcmPath);
        }
        std::cout << "[Audio] Water levels: start=" << startWaterMs << "ms ("
                  << startSamples << " samples), resume=" << (startWaterMs * 3 / 4)
                  << "ms (" << resumeSamples << " samples), fade=5ms" << std::endl;
        if (!dumpPcmPath.empty()) {
            std::cout << "[Audio] dump-pcm enabled: " << dumpPcmPath << std::endl;
        }
    }

    // B11 (M-6): stall handler runs on the IO thread; only flag-setting /
    // logging allowed. The E-CLIENT-STALL line is printed inside WsClient
    // (once per stall); teardown is done by the main loop polling IsStalled().
    client.SetStallHandler([](uint64_t /*silentMs*/) {});

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

        // Main streaming loop — exit on: Ctrl+C / disconnect / audio stall /
        // render-thread death (B12, FR-08/FR-11).
        while (g_running.load() && client.IsConnected() && !client.IsStalled()) {
            // FR-11 / SS5.12: render thread exited abnormally -> surface as a
            // recoverable error, fall through to the existing reconnect +
            // renderer rebuild loop (E-CLIENT-RENDER-EXIT, one line).
            if (playbackStarted.load() && !renderer.IsRunning()) {
                std::cerr << "[Audio][ERROR] render thread exited: stage="
                          << renderer.GetExitStageName()
                          << " hr=0x" << std::hex << renderer.GetExitHr() << std::dec
                          << " (" << renderer.GetLastError() << ")" << std::endl;
                break;
            }
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
