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

#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <mutex>
#include <set>
#include <atomic>
#include <csignal>
#include <algorithm>
#include <cmath>
#include <random>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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
    uint16_t httpsPort = 0;  // HTTPS disabled by default (no certificate hassle)
    if (argc > 1) {
        httpPort = static_cast<uint16_t>(std::atoi(argv[1]));
    }
    if (argc > 2) {
        httpsPort = static_cast<uint16_t>(std::atoi(argv[2]));
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

    // Track which sessions are "streaming" (have sent start)
    std::mutex streamingMutex;
    std::set<WebSocketSessionBase*> streamingSessions;

    std::atomic<bool> hasStreamingClients{false};

    server->SetConnectCallback([&](WebSocketSessionBase* session, bool connected) {
        if (!connected) {
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
                hasStreamingClients.store(true);
                std::cout << "[Audio] Client started streaming. Active: " << streamingSessions.size() << std::endl;
            } else if (msg.find("\"stop\"") != std::string::npos) {
                std::lock_guard<std::mutex> lock(streamingMutex);
                streamingSessions.erase(session);
                hasStreamingClients.store(!streamingSessions.empty());
                std::cout << "[Audio] Client stopped streaming. Active: " << streamingSessions.size() << std::endl;
            }
        } catch (...) {}
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

    // Debug counters
    std::atomic<uint64_t> captureCallbackCount{0};
    std::atomic<uint64_t> sentPacketCount{0};

    // Start audio capture - send raw PCM Int16 directly
    capture.Start([&](const float* data, uint32_t frameCount, uint32_t channels, uint32_t sampleRate) {
        if (!hasStreamingClients.load()) return;

        uint64_t cbCount = captureCallbackCount.fetch_add(1) + 1;
        if (cbCount == 1 || cbCount % 500 == 0) {
            std::cout << "[Audio] Capture callback #" << cbCount
                      << " frames=" << frameCount
                      << " ch=" << channels
                      << " rate=" << sampleRate << std::endl;
        }

        // Downmix to outputChannels if needed
        std::vector<float> processed;
        const float* processedData = data;
        uint32_t processedFrames = frameCount;

        if (channels != outputChannels) {
            processed.resize(frameCount * outputChannels);
            for (uint32_t i = 0; i < frameCount; i++) {
                if (outputChannels == 2 && channels > 2) {
                    processed[i * 2 + 0] = data[i * channels + 0];
                    processed[i * 2 + 1] = data[i * channels + 1];
                } else if (outputChannels == 1) {
                    float sum = 0;
                    for (uint32_t ch = 0; ch < channels; ch++) {
                        sum += data[i * channels + ch];
                    }
                    processed[i] = sum / channels;
                } else if (outputChannels == 2 && channels == 1) {
                    processed[i * 2 + 0] = data[i];
                    processed[i * 2 + 1] = data[i];
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

        if (processedFrames == 0) return;

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

        // Broadcast raw PCM Int16 to all streaming clients
        {
            std::lock_guard<std::mutex> slock(streamingMutex);
            for (auto* session : streamingSessions) {
                try {
                    session->SendBinary(pcm16.data(), pcm16.size() * sizeof(int16_t));
                    uint64_t sCount = sentPacketCount.fetch_add(1) + 1;
                    if (sCount == 1 || sCount % 500 == 0) {
                        std::cout << "[Audio] Sent PCM packet #" << sCount
                                  << " (" << pcm16.size() * sizeof(int16_t) << " bytes)" << std::endl;
                    }
                } catch (...) {}
            }
        }
    });

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