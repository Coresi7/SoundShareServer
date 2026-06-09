#pragma once

#include "ring_buffer.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

class WasapiRender {
public:
    WasapiRender();
    ~WasapiRender();

    bool Initialize(uint32_t sampleRate, uint16_t channels);
    void SetRingBuffer(RingBuffer* buffer) { m_ringBuffer = buffer; }
    bool Start();
    void Stop();

    bool IsRunning() const { return m_running.load(); }
    const std::string& GetLastError() const { return m_lastError; }

private:
    void RenderLoop();
    void ReleaseResources();

    RingBuffer* m_ringBuffer = nullptr;
    uint32_t m_sampleRate = 48000;
    uint16_t m_channels = 2;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    std::thread m_thread;
    std::string m_lastError;

    void* m_enumerator = nullptr;
    void* m_device = nullptr;
    void* m_audioClient = nullptr;
    void* m_renderClient = nullptr;
    void* m_waveFormat = nullptr;
    void* m_event = nullptr;
    uint32_t m_bufferFrameCount = 0;
};
