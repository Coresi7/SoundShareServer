#pragma once

#include "ring_buffer.h"

// B6 (FR-11): HRESULT for render-exit diagnostics
#include <Windows.h>

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

    // ---- B6 (FR-11 / SS5.12): render-thread failure surfacing ----
    HRESULT GetExitHr() const { return m_exitHr.load(); }
    int GetExitStage() const { return m_exitStage.load(); }
    const char* GetExitStageName() const;
    // B13: --fault-render-fail=<padding|getbuffer|releasebuffer> (default off,
    // application-level injection only, NFR-WIN-4)
    void SetFaultInjectStage(const std::string& stage) { m_faultStage = stage; }

private:
    void RenderLoop();
    void ReleaseResources();
    // B7/B8/B9: injection check + exit recording (stage 1/2/3)
    bool FaultHit(int stage) const;
    void RecordExit(int stage, HRESULT hr);

    RingBuffer* m_ringBuffer = nullptr;
    uint32_t m_sampleRate = 48000;
    uint16_t m_channels = 2;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    std::thread m_thread;
    std::string m_lastError;

    // ---- B6 (FR-11): abnormal-exit diagnostics ----
    std::atomic<long> m_exitHr{0};    // HRESULT of the failing call (0 = none)
    std::atomic<int>  m_exitStage{0}; // 1=GetCurrentPadding 2=GetBuffer 3=ReleaseBuffer
    std::string m_faultStage;         // "" (off) | "padding" | "getbuffer" | "releasebuffer"

    void* m_enumerator = nullptr;
    void* m_device = nullptr;
    void* m_audioClient = nullptr;
    void* m_renderClient = nullptr;
    void* m_waveFormat = nullptr;
    void* m_event = nullptr;
    uint32_t m_bufferFrameCount = 0;
};
