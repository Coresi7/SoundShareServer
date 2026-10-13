#pragma once

#include "ring_buffer.h"

// B6 (FR-11): HRESULT for render-exit diagnostics
#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

// ---- T-GPU-AUDIO-GLITCH F4 (FR-05)：消费侧水位门槛 + 软欠载 ----
// 三态机 PREFILL → PLAYING → SOFT_UNDERRUN（与浏览器端 embedded_page.h 同构）：
//   PREFILL：起播前攒水位（不 Pop、ReleaseBuffer(n,0) 喂静音）
//   PLAYING：正常消费
//   SOFT_UNDERRUN：保持最后样本 × 逐帧斜坡淡出（替掉 memset(0) 硬切）
enum class RenderState { PREFILL, PLAYING, SOFT_UNDERRUN };

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

    // ---- F4 (FR-05)：水位参数（单一参数族：start/resume/fade，单位 = 交织样本）----
    // start 默认 7680（80ms stereo@48k）、resume 默认 0.75×start = 5760（60ms）、
    // fade 默认 480（5ms = 每声道 240 帧）。容量上限 capacityCap（2s）在
    // SoundShareClient/main.cpp 的 kRingCapacity，本类不触碰。
    void SetWaterLevels(size_t startSamples, size_t resumeSamples, size_t fadeSamples) {
        m_startWaterSamples = startSamples;
        m_resumeWaterSamples = resumeSamples;
        m_fadeSamples = fadeSamples == 0 ? 1 : fadeSamples;
    }
    uint64_t UnderrunCount() const { return m_underrunCount.load(); }
    size_t MinAvailable() const { return m_minAvailable.load(); }

    // ---- C1/s5：--dump-pcm=<path>（默认关闭；录"实际 Pop 出并送渲染"的样本，
    //      含软欠载填充；每次 Start() 复位缓冲，Stop() 时一次性写 WAV）----
    void SetDumpPcmPath(const std::string& path) { m_dumpPath = path; }

private:
    void RenderLoop();
    void ReleaseResources();
    // B7/B8/B9: injection check + exit recording (stage 1/2/3)
    bool FaultHit(int stage) const;
    void RecordExit(int stage, HRESULT hr);
    // F4：dump 追加（渲染线程内仅内存操作，无磁盘 IO）；Start() 复位，Stop() 落盘
    void AppendDump(const int16_t* samples, size_t count);
    void WriteDumpWav();

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

    // ---- F4 (FR-05)：水位状态机成员（P1-3：m_state / m_lastL / m_lastR 必备）----
    RenderState m_state = RenderState::PREFILL;
    size_t m_startWaterSamples = 7680;    // 80ms（NFR-LAT-2 默认）
    size_t m_resumeWaterSamples = 5760;   // 60ms = 0.75×start（滞回防振荡）
    size_t m_fadeSamples = 480;           // 5ms 交织样本（每声道 240 帧）
    double m_env = 0.0;                   // 包络（0..1；PREFILL 期 0，PLAYING 斜升）
    int16_t m_lastL = 0, m_lastR = 0;     // 保持最后样本（仅正常读取分支更新，m3）
    std::atomic<uint64_t> m_underrunCount{0};
    std::atomic<size_t> m_minAvailable{SIZE_MAX};   // m5：首次 PLAYING 之后才统计
    bool m_started = false;               // m5：谷值统计起点（首次进入 PLAYING）
    bool m_everStarted = false;           // 非首次起播用 resume 水位

    // ---- C1/s5：--dump-pcm ----
    std::string m_dumpPath;               // 空 = 关闭
    std::vector<int16_t> m_dumpBuf;       // 上限 60s（kDumpMaxSamples），超限停录
    static constexpr size_t kDumpMaxSamples = 48000 * 2 * 60;

    void* m_enumerator = nullptr;
    void* m_device = nullptr;
    void* m_audioClient = nullptr;
    void* m_renderClient = nullptr;
    void* m_waveFormat = nullptr;
    void* m_event = nullptr;
    uint32_t m_bufferFrameCount = 0;
};
