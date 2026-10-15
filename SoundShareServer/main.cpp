// SoundShareServer - Stream PC system audio to browser via WebSocket
//
// Architecture:
//   WASAPI Loopback -> Downmix/Resample -> PCM Int16 -> WebSocket -> Browser (AudioWorklet)
//
// Usage:
//   SoundShareServer.exe [http_port] [https_port]
//   Default: HTTP on 8080, HTTPS disabled (pass 0 to disable either)
//   Open http://<this-pc-ip>:<port>/ in browser on another device

#include "wasapi_capture.h"
#include "http_server.h"

// T-GPU-AUDIO-GLITCH S0 (F6)：进程级采集观测（探针字段 + 注入开关镜像）
#include "capture_stats.h"

#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <mutex>
#include <set>
#include <atomic>
#include <csignal>
#include <cctype>
#include <algorithm>
#include <cmath>
#include <random>
#include <cstdio>
#include <cstdint>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ============================================================
// T-GPU-AUDIO-GLITCH S0 (F6) 辅助
// ============================================================

// QPC 微秒时间戳（C 打点 / 标记注入 t0；§3.0.4 Win32 原语）
static uint64_t QpcNowUs() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(
        (static_cast<double>(c.QuadPart) * 1000000.0) / static_cast<double>(freq.QuadPart));
}

// D-21A (m8)：probe hook 内最小 JSON 转义 —— '\' → "\\，「"」 → \"，
// 控制字符 < 0x20 丢弃，UTF-8 多字节序列原样保留。
static std::string JsonEscapeMinimal(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c < 0x20) { /* 丢弃控制字符 */ }
        else out += static_cast<char>(c);
    }
    return out;
}

// 宽字符 → UTF-8（端点名输出通道 D-15：std::cout + UTF-8）
static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], need, nullptr, nullptr);
    return out;
}

// 两位小数定点（探针毫秒字段口径：§6.1 —— 不出现浮点噪声尾数）
static std::string Fixed2(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

#ifdef SOUNDSHARE_ENABLE_FAULT_INJECTION
// D-14 (C3)：8ms 线性 chirp 1k→8kHz，-6dBFS（抗 Lanczos 重采样、互相关可检出）
// frames = sampleRate * 8 / 1000（48k → 384 帧），调用方钳制 min(frameCount, 384)。
static void WriteChirp(float* data, uint32_t frames, uint32_t channels, uint32_t sampleRate) {
    if (frames == 0) return;
    const double f0 = 1000.0, f1 = 8000.0;
    const double T = static_cast<double>(frames) / sampleRate;
    for (uint32_t i = 0; i < frames; i++) {
        const double t = static_cast<double>(i) / sampleRate;
        const double phase = 2.0 * M_PI * (f0 * t + (f1 - f0) / (2.0 * T) * t * t);
        const float v = 0.5f * static_cast<float>(sin(phase));   // -6dBFS
        for (uint32_t c = 0; c < channels; c++) data[i * channels + c] = v;
    }
}

// capture_stall 命中判定（§3.0.2）：概率 ppm + 最小间隔 kStallMinGapMs=100ms
// （防注入自我螺旋，写死常量）。仅采集回调线程调用。
static bool StallInjectionHit(uint32_t ppm) {
    if (ppm == 0) return true;   // 未设概率 ⇒ 100% 命中（档位表只设 stall_ms 的用法）
    static thread_local std::mt19937 rng{std::random_device{}()};
    static thread_local std::uniform_int_distribution<uint32_t> dist(0, 999999);
    static uint64_t lastHitUs = 0;
    const uint64_t now = QpcNowUs();
    constexpr uint64_t kStallMinGapMs = 100;   // 写死（§3.0.2）
    if (lastHitUs != 0 && now - lastHitUs < kStallMinGapMs * 1000) return false;
    if (dist(rng) >= ppm) return false;
    lastHitUs = now;
    return true;
}
#endif // SOUNDSHARE_ENABLE_FAULT_INJECTION

// ---------------------------------------------------------------------------
// Windowed-Sinc (Lanczos) Resampler — broadcast-quality sample rate conversion
//
// Uses a Lanczos kernel (sinc × sinc window) with configurable lobe count.
// Default LOBES=8 gives ≥120 dB stopband attenuation, suitable for
// professional audio mastering.  The resampler keeps residual input samples
// between calls so it works correctly in a streaming (block-by-block) context.
// ---------------------------------------------------------------------------
class SincResampler {
public:
    // Higher LOBES = better quality but more CPU.
    // 8 lobes ≈ libsamplerate "Best" quality.
    static constexpr int LOBES = 8;

    void Configure(uint32_t inputRate, uint32_t outputRate, uint32_t channels) {
        m_inputRate = inputRate;
        m_outputRate = outputRate;
        m_channels = channels;
        m_ratio = static_cast<double>(outputRate) / inputRate;
        m_residual.clear();
        m_phase = 0.0;

        // Pre-compute the Lanczos kernel table for fast lookup
        BuildKernelTable();
    }

    // Process interleaved float data (streaming-safe: handles residual across calls)
    std::vector<float> Process(const float* input, uint32_t inputFrames) {
        // Prepend residual from previous call
        std::vector<float> combined;
        combined.reserve(m_residual.size() + inputFrames * m_channels);
        combined.insert(combined.end(), m_residual.begin(), m_residual.end());
        combined.insert(combined.end(), input, input + inputFrames * m_channels);
        m_residual.clear();

        uint32_t totalInputFrames = static_cast<uint32_t>(combined.size() / m_channels);

        // We need at least 2*LOBES frames of context for the kernel
        if (totalInputFrames < static_cast<uint32_t>(2 * LOBES + 1)) {
            m_residual.assign(combined.begin(), combined.end());
            return {};
        }

        // Estimate output size
        uint32_t maxOutputFrames = static_cast<uint32_t>(
            std::ceil((totalInputFrames - 2 * LOBES) * m_ratio)) + 1;
        std::vector<float> output;
        output.reserve(maxOutputFrames * m_channels);

        // Walk through output samples using m_phase as the fractional input position
        // Phase starts at LOBES so we have enough left context
        if (m_phase < LOBES) {
            m_phase = static_cast<double>(LOBES);
        }

        while (true) {
            int intPos = static_cast<int>(m_phase);
            double frac = m_phase - intPos;

            // Need LOBES samples on each side
            if (intPos - LOBES < 0 ||
                intPos + LOBES >= static_cast<int>(totalInputFrames)) {
                break;
            }

            for (uint32_t ch = 0; ch < m_channels; ch++) {
                double sum = 0.0;
                for (int j = -LOBES; j < LOBES; j++) {
                    double x = j - frac;
                    double w = LanczosWeight(x);
                    sum += w * combined[(intPos + j) * m_channels + ch];
                }
                output.push_back(static_cast<float>(sum));
            }

            m_phase += 1.0 / m_ratio;
        }

        // Save unconsumed input as residual for next call.
        // Keep everything from (lastConsumedIntPos - LOBES) onward,
        // so the next call has enough left context.
        int lastConsumedPos = static_cast<int>(m_phase);
        int residualStart = lastConsumedPos - LOBES;
        if (residualStart < 0) residualStart = 0;

        uint32_t residualStartSample = static_cast<uint32_t>(residualStart) * m_channels;
        if (residualStartSample < combined.size()) {
            m_residual.assign(combined.begin() + residualStartSample, combined.end());
        }

        // Adjust phase relative to new residual buffer
        m_phase -= residualStart;

        return output;
    }

private:
    uint32_t m_inputRate = 0;
    uint32_t m_outputRate = 0;
    uint32_t m_channels = 0;
    double m_ratio = 1.0;
    double m_phase = 0.0;
    std::vector<float> m_residual;

    // Pre-computed kernel table for fast Lanczos lookup
    static constexpr int TABLE_RESOLUTION = 512;  // sub-sample resolution
    std::vector<double> m_kernelTable;             // size = LOBES*2 * TABLE_RESOLUTION

    // Normalized sinc: sin(pi*x) / (pi*x)
    static double Sinc(double x) {
        if (std::abs(x) < 1e-12) return 1.0;
        double px = M_PI * x;
        return std::sin(px) / px;
    }

    // Lanczos window: sinc(x/LOBES) for |x| < LOBES, else 0
    static double LanczosWindow(double x) {
        if (std::abs(x) >= LOBES) return 0.0;
        return Sinc(x / LOBES);
    }

    // Full Lanczos kernel: sinc(x) * LanczosWindow(x)
    static double LanczosKernel(double x) {
        if (std::abs(x) >= LOBES) return 0.0;
        return Sinc(x) * LanczosWindow(x);
    }

    void BuildKernelTable() {
        int kernelWidth = 2 * LOBES;
        m_kernelTable.resize(kernelWidth * TABLE_RESOLUTION);
        for (int i = 0; i < kernelWidth; i++) {
            for (int s = 0; s < TABLE_RESOLUTION; s++) {
                double frac = static_cast<double>(s) / TABLE_RESOLUTION;
                double x = (i - LOBES) - frac;
                m_kernelTable[i * TABLE_RESOLUTION + s] = LanczosKernel(x);
            }
        }
    }

    // Fast Lanczos weight lookup using pre-computed table
    double LanczosWeight(double x) const {
        if (x <= -LOBES || x >= LOBES) return 0.0;

        // Map x to table coordinates:  x = (j - frac),  j in [-LOBES, LOBES)
        // table index i = j + LOBES,  sub-sample s = frac * TABLE_RESOLUTION
        // But we receive the combined x directly, so:
        double shifted = x + LOBES;  // [0, 2*LOBES)
        int i = static_cast<int>(shifted);
        double subFrac = shifted - i;
        int s = static_cast<int>(subFrac * TABLE_RESOLUTION);
        if (s >= TABLE_RESOLUTION) s = TABLE_RESOLUTION - 1;

        int kernelWidth = 2 * LOBES;
        if (i < 0 || i >= kernelWidth) return 0.0;

        // Linear interpolation between adjacent table entries for extra precision
        int idx = i * TABLE_RESOLUTION + s;
        if (s + 1 < TABLE_RESOLUTION) {
            double w0 = m_kernelTable[idx];
            double w1 = m_kernelTable[idx + 1];
            double t = subFrac * TABLE_RESOLUTION - s;
            return w0 + t * (w1 - w0);
        }
        return m_kernelTable[idx];
    }
};

// ---------------------------------------------------------------------------
// TPDF Dithering for float32 → Int16 quantization
//
// Triangular Probability Density Function dither eliminates quantization
// distortion artifacts (harmonic correlation with signal) by adding a small
// amount of shaped noise before truncation.  This is the standard technique
// used in professional audio mastering (e.g., Apogee UV22, POW-r type 1).
// ---------------------------------------------------------------------------
class TpdfDither {
public:
    TpdfDither() : m_rng(std::random_device{}()), m_dist(-1.0f, 1.0f) {}

    // Convert float [-1,1] to Int16 with TPDF dither
    int16_t Process(float sample) {
        // Generate TPDF noise: sum of two uniform random values → triangular PDF
        float noise = m_dist(m_rng) + m_dist(m_rng);
        // Scale: 1 LSB in Int16 = 1/32767, dither amplitude = 1 LSB peak-to-peak
        float dithered = sample * 32767.0f + noise;
        // Round and clamp
        int32_t rounded = static_cast<int32_t>(std::round(dithered));
        if (rounded > 32767) rounded = 32767;
        if (rounded < -32768) rounded = -32768;
        return static_cast<int16_t>(rounded);
    }

private:
    std::mt19937 m_rng;
    std::uniform_real_distribution<float> m_dist;
};

// Global flag for clean shutdown
static std::atomic<bool> g_running{true};

void SignalHandler(int sig) {
    (void)sig;
    g_running.store(false);
}

int main(int argc, char* argv[]) {
    // Set console to UTF-8
    SetConsoleOutputCP(CP_UTF8);

    uint16_t httpPort = 8080;
    uint16_t httpsPort = 8443;  // HTTPS enabled by default on 8443
    // F1 (FR-01)：--key=value 开关不破坏既有 argv[1]/argv[2] 位置语义 ——
    // 位置参数只认非 "--" 开头的实参
    {
        int positional = 0;
        for (int i = 1; i < argc; i++) {
            const std::string arg = argv[i];
            if (arg.rfind("--", 0) == 0) continue;
            ++positional;
            if (positional == 1) httpPort = static_cast<uint16_t>(std::atoi(arg.c_str()));
            else if (positional == 2) httpsPort = static_cast<uint16_t>(std::atoi(arg.c_str()));
        }
    }

    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    std::cout << "======================================" << std::endl;
    std::cout << "  SoundShare Server (PCM mode)" << std::endl;
    std::cout << "  Stream PC audio to browser" << std::endl;
    std::cout << "======================================" << std::endl;
    std::cout << "  HTTP port:  " << httpPort << (httpPort ? "" : " (disabled)") << std::endl;
    std::cout << "  HTTPS port: " << httpsPort << (httpsPort ? "" : " (disabled)") << std::endl;
    std::cout << std::endl;

    // Initialize COM for WASAPI
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        std::cerr << "Failed to initialize COM" << std::endl;
        return 1;
    }

    // Initialize WASAPI capture
    WasapiCapture capture;

    // F1 (FR-01)：--capture-buffer-ms=<50..100> 运行期旋钮（默认 100ms = 1,000,000 hns；
    // 解析失败/越界 → 打印用法并保留默认，不 exit，不改既有启动语义）
    {
        const std::string kPrefix = "--capture-buffer-ms=";
        for (int i = 1; i < argc; i++) {
            const std::string arg = argv[i];
            if (arg.rfind(kPrefix, 0) != 0) continue;
            const std::string val = arg.substr(kPrefix.size());
            char* end = nullptr;
            const long ms = std::strtol(val.c_str(), &end, 10);
            if (end != val.c_str() && *end == '\0' && ms >= 50 && ms <= 100) {
                capture.SetCaptureBufferHns(static_cast<REFERENCE_TIME>(ms) * 10000);
                std::cout << "[Audio] Capture buffer: " << ms << " ms ("
                          << (ms * 10000) << " hns, configured)" << std::endl;
            } else {
                std::cout << "[Audio] Ignoring invalid " << arg
                          << " (expected --capture-buffer-ms=<50..100>); using default 100 ms"
                          << std::endl;
            }
        }
    }

    if (!capture.Initialize()) {
        std::cerr << "Failed to initialize audio capture" << std::endl;
        CoUninitialize();
        return 1;
    }

    // Set error callback for capture device recovery monitoring
    capture.SetErrorCallback([](long errorCode, const std::string& message, bool recovered) {
        if (recovered) {
            std::cout << "[Audio] " << message << std::endl;
        } else {
            std::cerr << "[Audio] " << message
                      << " (error: 0x" << std::hex << errorCode << std::dec << ")" << std::endl;
        }
    });

    // Audio output parameters

    uint32_t captureRate = capture.GetSampleRate();
    uint32_t captureChannels = capture.GetChannels();
    uint32_t outputRate = 48000;
    uint32_t outputChannels = (captureChannels >= 2) ? 2 : 1;

    // Resampler (if capture rate != output rate)
    SincResampler resampler;
    bool needResample = (captureRate != outputRate);
    if (needResample) {
        resampler.Configure(captureRate, outputRate, outputChannels);
        std::cout << "[Audio] Resampling from " << captureRate << " Hz to " << outputRate
                  << " Hz (Lanczos sinc, " << SincResampler::LOBES << " lobes)" << std::endl;
    }

    // TPDF dithering for float32 -> Int16 quantization
    TpdfDither dither;

    std::cout << "[Audio] Output: " << outputRate << " Hz, " << outputChannels << " ch, PCM Int16 (TPDF dithered)" << std::endl;

    // Start HTTP/WebSocket server
    auto server = std::make_shared<HttpServer>(httpPort, httpsPort);

    // Debug counters（A23/A24：保留为原子计数器，仅由探针读取，不再打印日志）
    std::atomic<uint64_t> captureCallbackCount{0};
    std::atomic<uint64_t> sentPacketCount{0};

    // Track which sessions are "streaming" (have sent start)
    std::mutex streamingMutex;
    std::set<WebSocketSessionBase*> streamingSessions;

    std::atomic<bool> hasStreamingClients{false};

    server->SetConnectCallback([&](WebSocketSessionBase* session, bool connected) {
        if (!connected) {
            // 注意（A28b 偏离说明）：此处只 erase，不调用 session->SetStreaming(false)——
            // 该回调可能由会话析构函数触发（析构中虚调用 = UB / 纯虚崩溃），
            // 且对象即将消亡，streaming 标志随对象消亡无意义。
            std::lock_guard<std::mutex> lock(streamingMutex);
            streamingSessions.erase(session);
            hasStreamingClients.store(!streamingSessions.empty());
        }
    });

    server->SetMessageCallback([&](WebSocketSessionBase* session, const std::string& msg) {

        try {
            if (msg.find("\"start\"") != std::string::npos) {
                // Send audio config (format=pcm16)
                std::string config = "{\"type\":\"audio_config\",\"sampleRate\":"
                    + std::to_string(outputRate) + ",\"channels\":"
                    + std::to_string(outputChannels) + ",\"format\":\"pcm16\"}";
                session->Send(config);

                std::lock_guard<std::mutex> lock(streamingMutex);
                streamingSessions.insert(session);
                session->SetStreaming(true);   // A28b：探针流状态标记
                hasStreamingClients.store(true);
            } else if (msg.find("\"stop\"") != std::string::npos) {
                std::lock_guard<std::mutex> lock(streamingMutex);
                streamingSessions.erase(session);
                session->SetStreaming(false);  // A28b：对象存活（消息回调中），安全
                hasStreamingClients.store(!streamingSessions.empty());
            }
        } catch (...) {}
    });

    // A28: 探针 hook —— 返回顶层 JSON 成员序列（不含外层大括号）。
    // ★ 硬约束（§5.8.4）：本 hook 由 DumpProbeJson() 在【释放 L1 之后】调用，
    //   内部持 L2 只取 count 与计数器、立即出锁后序列化 —— 严禁 L1→L2 嵌套。
    // T-GPU-AUDIO-GLITCH S0 (F6)：追加采集观测字段（§6.1，字段名与需求逐字一致）。
    server->SetProbeHook([&]() -> std::string {
        size_t streamingCount = 0;
        {
            std::lock_guard<std::mutex> lock(streamingMutex);
            streamingCount = streamingSessions.size();
        }
        const CaptureStats& cs = g_captureStats;
        std::string s;
        s += "\"streaming\": { \"count\": " + std::to_string(streamingCount) + " },";
        s += " \"captureCallbacks\": " + std::to_string(captureCallbackCount.load()) + ",";
        s += " \"sentPackets\": " + std::to_string(sentPacketCount.load()) + ",";
        // --- FR-07：D（机制 A 判据）---
        s += " \"captureDiscontinuityCount\": " + std::to_string(cs.discontinuityTotal.load(std::memory_order_relaxed)) + ",";
        s += " \"captureSilentCount\": " + std::to_string(cs.silentTotal.load(std::memory_order_relaxed)) + ",";
        // --- FR-08：P99（稳态窗口判据）/ C（F5 门控）---
        s += " \"captureInterArrivalMsP50\": " + Fixed2(cs.interArrival.QuantileMs(0.50)) + ",";
        s += " \"captureInterArrivalMsP99\": " + Fixed2(cs.interArrival.QuantileMs(0.99)) + ",";
        s += " \"captureInterArrivalMsMax\": " + Fixed2(static_cast<double>(cs.interArrival.maxUs.load(std::memory_order_relaxed)) / 1000.0) + ",";
        // ★ m12（T-CAPTURE-IDLE-LOG FR-03）：自本任务起 captureInterArrivalSaturated
        //   不再含空闲边界样本（空闲退出首帧由 wasapi_capture.cpp 跳过 AddUs）。
        s += " \"captureInterArrivalSamples\": " + std::to_string(cs.interArrival.count.load(std::memory_order_relaxed)) + ",";
        s += " \"captureInterArrivalSaturated\": " + std::to_string(cs.interArrival.saturated.load(std::memory_order_relaxed)) + ",";
        s += " \"captureCallbackCriticalMsP50\": " + Fixed2(cs.callbackCritical.QuantileMs(0.50)) + ",";
        s += " \"captureCallbackCriticalMsP99\": " + Fixed2(cs.callbackCritical.QuantileMs(0.99)) + ",";
        s += " \"captureCallbackCriticalMsMax\": " + Fixed2(static_cast<double>(cs.callbackCritical.maxUs.load(std::memory_order_relaxed)) / 1000.0) + ",";
        s += " \"captureCallbackCriticalSamples\": " + std::to_string(cs.callbackCritical.count.load(std::memory_order_relaxed)) + ",";
        s += " \"captureCallbackCriticalSaturated\": " + std::to_string(cs.callbackCritical.saturated.load(std::memory_order_relaxed)) + ",";
        // --- F1/F2/F3 可观测 ---
        s += " \"captureBufferHns\": " + std::to_string(cs.bufferHns.load(std::memory_order_relaxed)) + ",";
        s += " \"captureBufferFrames\": " + std::to_string(cs.bufferFrames.load(std::memory_order_relaxed)) + ",";
        s += " \"captureMmcssRegistered\": " + std::string(cs.mmcssRegistered.load(std::memory_order_relaxed) ? "true" : "false") + ",";
        s += " \"captureEventDriven\": " + std::string(cs.eventDriven.load(std::memory_order_relaxed) ? "true" : "false") + ",";
        s += " \"captureEventHandlesCreated\": " + std::to_string(cs.eventHandlesCreated.load(std::memory_order_relaxed)) + ",";
        s += " \"captureEventTimeouts\": " + std::to_string(cs.eventTimeoutTotal.load(std::memory_order_relaxed)) + ",";
        s += " \"captureEventWakes\": " + std::to_string(cs.eventWakeTotal.load(std::memory_order_relaxed)) + ",";
        // --- FR-02（T-CAPTURE-IDLE-LOG §3.2.2）：空闲可观测 4 字段（单位 ms，
        //     与 captureEventTimeouts/captureEventWakes 命名/位置/风格完全对齐）---
        s += " \"captureIdleEpisodes\": " + std::to_string(cs.idleEpisodes.load(std::memory_order_relaxed)) + ",";   // 次；进程级单调
        s += " \"captureLastIdleMs\": " + std::to_string(cs.lastIdleMs.load(std::memory_order_relaxed)) + ",";       // ms；最近结束 episode
        s += " \"captureTotalIdleMs\": " + std::to_string(cs.totalIdleMs.load(std::memory_order_relaxed)) + ",";     // ms；进程启动累计（含进行中）
        s += " \"captureCurrentIdleMs\": " + std::to_string(cs.currentIdleMs.load(std::memory_order_relaxed)) + ","; // ms；非 idle 恒 0
        s += " \"captureReinitCount\": " + std::to_string(cs.reinitCount.load(std::memory_order_relaxed)) + ",";
        // --- F7：端点（m8：转义后输出）---
        // ★ B-1（reviewer 返工）：DeviceFriendlyName() 现为持锁按值快照
        //   （原 const 引用返回 + 采集线程 ClassifyEndpoint 并发写 = UB 竞争）。
        //   本调用点不持 L1（DumpProbeJson 已释放 L1 后才调 hook）/ 不持 L2
        //   （上方 L2 临界区已结束）/ 不持 L3 ⇒ 与新叶锁无嵌套。
        s += " \"captureEndpointName\": \"" + JsonEscapeMinimal(WideToUtf8(capture.DeviceFriendlyName())) + "\",";
        s += " \"captureEndpointIsGpuAudio\": " + std::string(cs.endpointIsGpuAudio.load(std::memory_order_relaxed) ? "true" : "false") + ",";
        // --- C3：标记注入 ---
        s += " \"markerSeq\": " + std::to_string(cs.markerSeq.load(std::memory_order_relaxed)) + ",";
        s += " \"markerT0QpcMs\": " + Fixed2(static_cast<double>(cs.markerT0QpcUs.load(std::memory_order_relaxed)) / 1000.0) + ",";
        // --- 注入状态（两种构建都输出；OFF 构建恒 0）---
        s += " \"injection\": { \"captureStallMs\": " + std::to_string(server->Faults().CaptureStallMs());
        s += ", \"captureStallProbPpm\": " + std::to_string(server->Faults().CaptureStallProbPpm());
        s += ", \"captureBatchMs\": " + std::to_string(server->Faults().CaptureBatchMs());
        s += ", \"captureSkipPackets\": " + std::to_string(server->Faults().CaptureSkipPackets());
        // §3.0(e)（T-CAPTURE-IDLE-LOG）：capture_idle_sim 开关状态（两种构建都
        // 输出；OFF 构建恒 0 —— 与既有 4 key 同模式）
        s += ", \"captureIdleSim\": " + std::to_string(server->Faults().CaptureIdleSim()) + " }";
        return s;
    });

    // T-GPU-AUDIO-GLITCH S0 (F6)：观测端点 hooks
    // GET /_debug/stats/reset（D-11A 主通道）+ stdin "reset"（兜底）→ 复位窗口直方图
    server->SetStatsResetHook([]() {
        g_captureStats.ResetWindow();
    });
    // GET /_debug/capture/hist?name=interArrival|callbackCritical（D-18A，Q2 证据）
    server->SetCaptureHistHook([](const std::string& name) -> std::string {
        const TimingHistogram* h = (name == "callbackCritical") ? &g_captureStats.callbackCritical
                                                                : &g_captureStats.interArrival;
        const char* n = (name == "callbackCritical") ? "callbackCritical" : "interArrival";
        std::string s = "{\"name\":\"" + std::string(n) + "\"";
        s += ",\"bucketUs\":" + std::to_string(TimingHistogram::kBucketUs);
        s += ",\"buckets\":" + std::to_string(TimingHistogram::kBuckets);
        s += ",\"count\":" + std::to_string(h->count.load(std::memory_order_relaxed));
        s += ",\"sumUs\":" + std::to_string(h->sumUs.load(std::memory_order_relaxed));
        s += ",\"maxUs\":" + std::to_string(h->maxUs.load(std::memory_order_relaxed));
        s += ",\"saturated\":" + std::to_string(h->saturated.load(std::memory_order_relaxed));
        s += ",\"nonzero\":[";
        bool first = true;
        for (uint32_t i = 0; i < TimingHistogram::kBuckets; ++i) {
            const uint32_t cnt = h->bins[i].load(std::memory_order_relaxed);
            if (cnt == 0) continue;
            if (!first) s += ",";
            first = false;
            s += "{\"i\":" + std::to_string(i) + ",\"n\":" + std::to_string(cnt) + "}";
        }
        s += "]}";
        return s;
    });

    if (!server->Start()) {
        std::cerr << "Failed to start server" << std::endl;
        CoUninitialize();
        return 1;
    }

    // Print access info
    std::cout << std::endl;
    if (httpPort > 0) {
        std::cout << "Open in browser: http://<this-pc-ip>:" << httpPort << "/" << std::endl;
    }
    if (httpsPort > 0) {
        std::cout << "Open in browser: https://<this-pc-ip>:" << httpsPort << "/" << std::endl;
        std::cout << "(Accept the self-signed certificate warning in your browser)" << std::endl;
    }
    std::cout << "Press Ctrl+C to stop." << std::endl;
    std::cout << std::endl;

#ifdef SOUNDSHARE_ENABLE_FAULT_INJECTION
    // 级-2 批量注入缓冲（capture_batch_ms；§3.0.2 —— 攒 PCM16 到 N ms 一次发送）
    std::vector<int16_t> batchPending;
    std::vector<int16_t> batchSwap;
#endif

    // Start audio capture - send raw PCM Int16 directly
    capture.Start([&](const float* data, uint32_t frameCount, uint32_t channels, uint32_t sampleRate) {
        if (!hasStreamingClients.load()) return;   // 现状早退（:387；保持不动，m6 基准）

#ifdef SOUNDSHARE_ENABLE_FAULT_INJECTION
        // ---- 注入开关族检查点（§3.0.2；全部位于 hasStreamingClients 早退【之后】m6）----
        auto& faults = server->Faults();

        // ① capture_skip_packets：丢弃随后 N 个 packet 的【内容】（仍正常
        //    ReleaseBuffer ⇒ 不产生 DATA_DISCONTINUITY）。P1-4：仅"内容缺口
        //    对照"，不得用作级-1 主证据；级-1 主证据 = capture_stall_ms。
        if (faults.ConsumeCaptureSkipPacket()) return;

        // ② capture_stall_ms（级-1 主证据）：采集线程真实卡顿 → 引擎缓冲溢出
        //    → D≥1。位于 C 打点 t0 【之前】⇒ 不计入 C（C 是 F5 门控量，不得
        //    被人为注入污染；§6.3 注入窗口样例 C≈0.78ms 与此口径一致）。
        if (faults.CaptureStallMs() > 0 && StallInjectionHit(faults.CaptureStallProbPpm())) {
            Sleep(faults.CaptureStallMs());
        }
#endif

        // F6 (FR-06/FR-08)：C 打点 t0 —— 过 hasStreamingClients 早退之后取
        // （C 只在"本次回调确实做了处理"时打点，避免空闲回调把门控稀释）
        const uint64_t criticalT0Us = QpcNowUs();

        // A23 (FR-03/NFR-LOG-1): periodic "[Audio] Capture callback #N" log removed;
        // counter kept as atomic, read only by the probe (A28).
        captureCallbackCount.fetch_add(1);

        const float* src = data;   // 可能被标记注入 scratch 重定向（m6：downmix 之前）
#ifdef SOUNDSHARE_ENABLE_FAULT_INJECTION
        // ③ inject_marker（C3 / m6 / D-22A）：一次性；早退之后、downmix 之前
        //    ⇒ E2E 覆盖 downmix→重采样→dither→入队→网络→播放端全链路
        std::vector<float> markerScratch;
        if (g_captureStats.markerArmed.exchange(false, std::memory_order_relaxed)) {
            markerScratch.assign(data, data + (size_t)frameCount * channels);
            WriteChirp(markerScratch.data(), std::min(frameCount, 384u), channels, sampleRate);
            src = markerScratch.data();
            g_captureStats.markerT0QpcUs.store(QpcNowUs(), std::memory_order_relaxed);
            g_captureStats.markerSeq.fetch_add(1, std::memory_order_relaxed);
        }
#endif

        // Downmix to outputChannels if needed
        std::vector<float> processed;
        const float* processedData = src;
        uint32_t processedFrames = frameCount;

        if (channels != outputChannels) {
            processed.resize(frameCount * outputChannels);
            for (uint32_t i = 0; i < frameCount; i++) {
                if (outputChannels == 2 && channels > 2) {
                    processed[i * 2 + 0] = src[i * channels + 0];
                    processed[i * 2 + 1] = src[i * channels + 1];
                } else if (outputChannels == 1) {
                    float sum = 0;
                    for (uint32_t ch = 0; ch < channels; ch++) {
                        sum += src[i * channels + ch];
                    }
                    processed[i] = sum / channels;
                } else if (outputChannels == 2 && channels == 1) {
                    processed[i * 2 + 0] = src[i];
                    processed[i * 2 + 1] = src[i];
                }
            }
            processedData = processed.data();
        }

        // Resample if needed
        std::vector<float> resampled;
        if (needResample) {
            resampled = resampler.Process(processedData, processedFrames);
            processedData = resampled.data();
            processedFrames = static_cast<uint32_t>(resampled.size() / outputChannels);
        }

        if (processedFrames == 0) {
            g_captureStats.callbackCritical.AddUs(QpcNowUs() - criticalT0Us);
            return;
        }

        // Convert float32 to Int16 PCM with TPDF dithering
        uint32_t totalSamples = processedFrames * outputChannels;
        std::vector<int16_t> pcm16(totalSamples);
        for (uint32_t i = 0; i < totalSamples; i++) {
            float s = processedData[i];
            // Clamp to [-1.0, 1.0]
            if (s > 1.0f) s = 1.0f;
            if (s < -1.0f) s = -1.0f;
            pcm16[i] = dither.Process(s);
        }

        // ---- 发送（含级-2 批量注入路径）----
        const int16_t* sendBuf = pcm16.data();
        size_t sendSamples = pcm16.size();
#ifdef SOUNDSHARE_ENABLE_FAULT_INJECTION
        if (faults.CaptureBatchMs() > 0) {
            // 级-2（机制 B）：攒 PCM16 到 N ms 再一次 SendBinary —— 制造到达抖动
            // 但不真丢数据（采集侧 drain/ReleaseBuffer 仍按 packet 及时执行 ⇒ D 恒 0）
            batchPending.insert(batchPending.end(), pcm16.begin(), pcm16.end());
            const size_t batchNeed = static_cast<size_t>(
                (static_cast<double>(outputRate) * faults.CaptureBatchMs() / 1000.0)) *
                outputChannels;
            if (batchPending.size() < batchNeed) {
                g_captureStats.callbackCritical.AddUs(QpcNowUs() - criticalT0Us);
                return;
            }
            batchSwap.assign(batchPending.begin(), batchPending.end());
            batchPending.clear();
            sendBuf = batchSwap.data();
            sendSamples = batchSwap.size();
        }
#endif

        // Broadcast raw PCM Int16 to all streaming clients
        {
            std::lock_guard<std::mutex> slock(streamingMutex);
            for (auto* session : streamingSessions) {
                try {
                    session->SendBinary(sendBuf, sendSamples * sizeof(int16_t));
                    // A24 (FR-03/NFR-LOG-1): periodic "[Audio] Sent PCM packet #N" log
                    // removed; counter kept as atomic, read only by the probe (A28).
                    sentPacketCount.fetch_add(1);
                } catch (...) {}
            }
        }

        // F6 (FR-06/FR-08)：C 打点 t1 —— 持 L2 段结束之后（含发送段）
        g_captureStats.callbackCritical.AddUs(QpcNowUs() - criticalT0Us);
    });

    // A27: stdin "dump" 探针入口（入口 2/3）——独立线程读 stdin，收到
    // dump / p / probe（大小写不敏感）时向 stdout 打印一次探针 JSON。
    // 不阻塞主循环；detach 线程只做 cout，进程退出时随进程终止。
    {
        auto serverCopy = server;  // shared_ptr 副本，避免 main 栈帧悬垂
        std::thread stdinThread([serverCopy]() {
            std::string line;
            while (std::getline(std::cin, line)) {
                std::string t;
                t.reserve(line.size());
                for (char c : line) {
                    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                        t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    }
                }
                if (t == "dump" || t == "p" || t == "probe") {
                    std::cout << serverCopy->DumpProbeJson() << std::endl;
                } else if (t == "reset") {
                    // T-GPU-AUDIO-GLITCH S0（D-11A）：stdin "reset" 兜底通道 ——
                    // 复位采集观测窗口直方图（主通道 = GET /_debug/stats/reset）
                    serverCopy->ResetStatsWindow();
                    std::cout << "{\"ok\":true,\"action\":\"stats_reset\"}" << std::endl;
                }
            }
        });
        stdinThread.detach();
    }

    // Main loop - wait for Ctrl+C
    while (g_running.load()) {
        Sleep(100);
    }

    std::cout << std::endl << "Shutting down..." << std::endl;

    capture.Stop();
    server->Stop();
    CoUninitialize();

    std::cout << "Server stopped." << std::endl;
    return 0;
}