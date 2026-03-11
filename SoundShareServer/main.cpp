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

// Resampler: simple linear interpolation for sample rate conversion
class SimpleResampler {
public:
    void Configure(uint32_t inputRate, uint32_t outputRate, uint32_t channels) {
        m_inputRate = inputRate;
        m_outputRate = outputRate;
        m_channels = channels;
        m_ratio = static_cast<double>(outputRate) / inputRate;
        m_residual.clear();
    }

    // Resample interleaved float data
    std::vector<float> Process(const float* input, uint32_t inputFrames) {
        // Prepend residual from last call
        std::vector<float> combined;
        combined.reserve(m_residual.size() + inputFrames * m_channels);
        combined.insert(combined.end(), m_residual.begin(), m_residual.end());
        combined.insert(combined.end(), input, input + inputFrames * m_channels);
        m_residual.clear();

        uint32_t totalInputFrames = static_cast<uint32_t>(combined.size() / m_channels);
        uint32_t outputFrames = static_cast<uint32_t>(totalInputFrames * m_ratio);

        std::vector<float> output(outputFrames * m_channels);

        for (uint32_t i = 0; i < outputFrames; i++) {
            double srcPos = i / m_ratio;
            uint32_t srcIdx = static_cast<uint32_t>(srcPos);
            double frac = srcPos - srcIdx;

            if (srcIdx + 1 >= totalInputFrames) {
                // Save remaining as residual
                output.resize(i * m_channels);
                uint32_t residualStart = srcIdx * m_channels;
                if (residualStart < combined.size()) {
                    m_residual.assign(combined.begin() + residualStart, combined.end());
                }
                break;
            }

            for (uint32_t ch = 0; ch < m_channels; ch++) {
                float s0 = combined[srcIdx * m_channels + ch];
                float s1 = combined[(srcIdx + 1) * m_channels + ch];
                output[i * m_channels + ch] = static_cast<float>(s0 + frac * (s1 - s0));
            }
        }

        return output;
    }

private:
    uint32_t m_inputRate = 0;
    uint32_t m_outputRate = 0;
    uint32_t m_channels = 0;
    double m_ratio = 1.0;
    std::vector<float> m_residual;
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
    SimpleResampler resampler;
    bool needResample = (captureRate != outputRate);
    if (needResample) {
        resampler.Configure(captureRate, outputRate, outputChannels);
        std::cout << "[Audio] Resampling from " << captureRate << " Hz to " << outputRate << " Hz" << std::endl;
    }

    std::cout << "[Audio] Output: " << outputRate << " Hz, " << outputChannels << " ch, PCM Int16" << std::endl;

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

        // Convert float32 to Int16 PCM
        uint32_t totalSamples = processedFrames * outputChannels;
        std::vector<int16_t> pcm16(totalSamples);
        for (uint32_t i = 0; i < totalSamples; i++) {
            float s = processedData[i];
            // Clamp to [-1.0, 1.0]
            if (s > 1.0f) s = 1.0f;
            if (s < -1.0f) s = -1.0f;
            pcm16[i] = static_cast<int16_t>(s * 32767.0f);
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