#include "wasapi_render.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

// ---- F4 (FR-05)：保持最后样本（got 之后的部分填 lastL/lastR 交织）----
// ★ 替掉原 memset(0) 硬切（机制 B 放大器的直接成因）
static void HoldLast(int16_t* buf, size_t got, size_t needed, uint16_t channels,
                     int16_t lastL, int16_t lastR) {
    if (channels == 2) {
        for (size_t i = got; i + 1 < needed; i += 2) {
            buf[i] = lastL;
            buf[i + 1] = lastR;
        }
    } else {
        const int16_t mid = static_cast<int16_t>((static_cast<int32_t>(lastL) + lastR) / 2);
        for (size_t i = got; i < needed; ++i) buf[i] = mid;
    }
}

// ---- T-CLIENT-SILENT-ON-RESUME：三态名与恢复事件日志（FR-06/AC-02/AC-06）----
// FR-06/AC-06：三态名（仅供单行日志；不新增类成员，不改 wasapi_render.h）
static const char* StateName(RenderState s) {
    switch (s) {
    case RenderState::PREFILL:       return "PREFILL";
    case RenderState::PLAYING:       return "PLAYING";
    case RenderState::SOFT_UNDERRUN: return "SOFT_UNDERRUN";
    }
    return "UNKNOWN";
}

// NFR-06 / M-4（§3.4.4 R1，H-08）：恢复事件行 —— 两类迁移【各自独立】1s
// 节流（s_lastTick[2]，slot 0 = SOFT_UNDERRUN->PREFILL，slot 1 =
// PREFILL->PLAYING）⇒ 合计硬上界 ≤2 行/s、稳态 0 行/s；行尾 t= 与周期
// STATS 同一 loopStartTick 时基（M-1③）。static 仅渲染线程访问（R-1 并发论证）。
static void LogResume(const char* trans, size_t avail, uint64_t und,
                      int slot, uint64_t loopStartTick) {
    static uint64_t s_lastTick[2] = {0, 0};
    const uint64_t now = GetTickCount64();
    if (now - s_lastTick[slot] < 1000) return;
    s_lastTick[slot] = now;
    std::cout << "[Audio][RESUME] " << trans << " avail=" << avail
              << " underruns=" << und
              << " t=" << (now - loopStartTick) << "ms" << std::endl;
}

// M-5a（§3.4.4 R2，H-09）：STATS 窗口内出队前水位峰值。仅渲染线程写；
// Stop() 读取时渲染线程已 join（wasapi_render.cpp:298-300）⇒ 无竞态、无新增锁。
static size_t g_availPeak = 0;

WasapiRender::WasapiRender() {}

WasapiRender::~WasapiRender() {
    Stop();
}

const char* WasapiRender::GetExitStageName() const {
    switch (m_exitStage.load()) {
    case 1: return "GetCurrentPadding";
    case 2: return "GetBuffer";
    case 3: return "ReleaseBuffer";
    default: return "none";
    }
}

// B7/B8/B9: application-level fault injection check (default: stage empty = off)
bool WasapiRender::FaultHit(int stage) const {
    if (m_faultStage.empty()) return false;
    switch (stage) {
    case 1: return m_faultStage == "padding";
    case 2: return m_faultStage == "getbuffer";
    case 3: return m_faultStage == "releasebuffer";
    default: return false;
    }
}

void WasapiRender::RecordExit(int stage, HRESULT hr) {
    m_exitStage.store(stage);
    m_exitHr.store(hr);
    // Filled on the render thread, read by the main thread after IsRunning()
    // flips to false (ordering via the atomic flag) -- same pattern as the
    // existing m_lastError usage in Start().
    m_lastError = std::string("render thread exited: stage=") + GetExitStageName();
}

bool WasapiRender::Initialize(uint32_t sampleRate, uint16_t channels) {
    m_sampleRate = sampleRate;
    m_channels = channels;
    return true;
}

void WasapiRender::ReleaseResources() {
    if (m_renderClient) {
        static_cast<IAudioRenderClient*>(m_renderClient)->Release();
        m_renderClient = nullptr;
    }
    if (m_audioClient) {
        auto* client = static_cast<IAudioClient*>(m_audioClient);
        client->Stop();
        client->Release();
        m_audioClient = nullptr;
    }
    if (m_event) {
        CloseHandle(static_cast<HANDLE>(m_event));
        m_event = nullptr;
    }
    if (m_waveFormat) {
        CoTaskMemFree(m_waveFormat);
        m_waveFormat = nullptr;
    }
    if (m_device) {
        static_cast<IMMDevice*>(m_device)->Release();
        m_device = nullptr;
    }
    if (m_enumerator) {
        static_cast<IMMDeviceEnumerator*>(m_enumerator)->Release();
        m_enumerator = nullptr;
    }
}

void WasapiRender::AppendDump(const int16_t* samples, size_t count) {
    if (m_dumpPath.empty()) return;
    if (m_dumpBuf.size() >= kDumpMaxSamples) return;   // 60s 上限，超限停录（保留最初片段）
    const size_t room = kDumpMaxSamples - m_dumpBuf.size();
    m_dumpBuf.insert(m_dumpBuf.end(), samples, samples + std::min(count, room));
}

void WasapiRender::WriteDumpWav() {
    if (m_dumpPath.empty() || m_dumpBuf.empty()) return;
    FILE* f = nullptr;
    if (fopen_s(&f, m_dumpPath.c_str(), "wb") != 0 || !f) {
        std::cerr << "[Audio] dump-pcm: cannot open " << m_dumpPath << " for write" << std::endl;
        return;
    }
    const uint32_t dataBytes = static_cast<uint32_t>(m_dumpBuf.size() * sizeof(int16_t));
    const uint32_t byteRate = m_sampleRate * m_channels * 2;
    const uint32_t blockAlign = m_channels * 2;
    auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    fwrite("RIFF", 1, 4, f);
    w32(36 + dataBytes);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    w32(16);                       // PCM chunk size
    w16(1);                        // PCM format
    w16(m_channels);
    w32(m_sampleRate);
    w32(byteRate);
    w16(static_cast<uint16_t>(blockAlign));
    w16(16);                       // bits per sample
    fwrite("data", 1, 4, f);
    w32(dataBytes);
    fwrite(m_dumpBuf.data(), sizeof(int16_t), m_dumpBuf.size(), f);
    fclose(f);
    std::cout << "[Audio] dump-pcm: wrote " << m_dumpPath << " ("
              << m_dumpBuf.size() / (m_sampleRate * m_channels) << "s)" << std::endl;
    m_dumpBuf.clear();
}

bool WasapiRender::Start() {
    if (m_running.load()) return true;
    if (!m_ringBuffer) {
        m_lastError = "Ring buffer not set";
        return false;
    }

    // ---- F4 (FR-05)：状态机复位（重连重建 renderer 的场景）----
    m_state = RenderState::PREFILL;
    m_env = 0.0;
    m_lastL = 0;
    m_lastR = 0;
    m_underrunCount.store(0, std::memory_order_relaxed);
    m_minAvailable.store(SIZE_MAX, std::memory_order_relaxed);
    m_started = false;
    // m_everStarted 保留（重连后仍用 resume 水位，避免每次重连都攒满 start 水位）
    // ---- s5 (C1)：dump 缓冲复位（否则多次重连 WAV 首尾拼接、盲听片段不对齐）----
    m_dumpBuf.clear();

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        m_lastError = "Failed to initialize COM";
        return false;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) {
        m_lastError = "Failed to create device enumerator";
        return false;
    }
    m_enumerator = enumerator;

    IMMDevice* device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) {
        m_lastError = "Failed to get default audio output device";
        ReleaseResources();
        return false;
    }
    m_device = device;

    IPropertyStore* props = nullptr;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT varName;
        PropVariantInit(&varName);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName))) {
            std::wcout << L"[Audio] Output device: " << varName.pwszVal << std::endl;
            PropVariantClear(&varName);
        }
        props->Release();
    }

    IAudioClient* audioClient = nullptr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(&audioClient));
    if (FAILED(hr)) {
        m_lastError = "Failed to activate audio client";
        ReleaseResources();
        return false;
    }
    m_audioClient = audioClient;

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = m_channels;
    format.nSamplesPerSec = m_sampleRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(m_channels * 2);
    format.nAvgBytesPerSec = m_sampleRate * format.nBlockAlign;
    format.cbSize = 0;

    WAVEFORMATEX* closest = nullptr;
    hr = audioClient->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &format, &closest);
    if (closest) {
        CoTaskMemFree(closest);
        closest = nullptr;
    }
    if (FAILED(hr)) {
        m_lastError = "Output device does not support requested PCM format";
        ReleaseResources();
        return false;
    }

    DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (hr == S_FALSE) {
        streamFlags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    }

    REFERENCE_TIME bufferDuration = 200000; // 20ms
    hr = audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        streamFlags,
        bufferDuration,
        0,
        &format,
        nullptr);
    if (FAILED(hr)) {
        m_lastError = "Failed to initialize audio client";
        ReleaseResources();
        return false;
    }

    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) {
        m_lastError = "Failed to create event";
        ReleaseResources();
        return false;
    }
    m_event = event;

    hr = audioClient->SetEventHandle(event);
    if (FAILED(hr)) {
        m_lastError = "Failed to set event handle";
        ReleaseResources();
        return false;
    }

    UINT32 bufferFrames = 0;
    hr = audioClient->GetBufferSize(&bufferFrames);
    if (FAILED(hr)) {
        m_lastError = "Failed to get buffer size";
        ReleaseResources();
        return false;
    }
    m_bufferFrameCount = bufferFrames;

    IAudioRenderClient* renderClient = nullptr;
    hr = audioClient->GetService(__uuidof(IAudioRenderClient),
                                 reinterpret_cast<void**>(&renderClient));
    if (FAILED(hr)) {
        m_lastError = "Failed to get render client";
        ReleaseResources();
        return false;
    }
    m_renderClient = renderClient;

    std::cout << "[Audio] Playback: " << m_sampleRate << " Hz, "
              << m_channels << " ch, PCM Int16" << std::endl;

    m_stopRequested.store(false);
    m_running.store(true);
    m_thread = std::thread(&WasapiRender::RenderLoop, this);
    return true;
}

void WasapiRender::Stop() {
    m_stopRequested.store(true);
    if (m_event) {
        SetEvent(static_cast<HANDLE>(m_event));
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_running.store(false);
    // ---- F4：终值统计行（tester 可从 stdout 直接读 U 与谷值，V-09）----
    // T-CLIENT-SILENT-ON-RESUME（落点 6 / H-09）：行尾追加 state=/env=/availPeak=
    // （既有字段顺序与分隔符不动；渲染线程已 join ⇒ g_availPeak 读取无竞态）。
    {
        const size_t minAvail = m_minAvailable.load(std::memory_order_relaxed);
        char envBuf[16];
        std::snprintf(envBuf, sizeof envBuf, "%.3f", m_env);
        std::cout << "[Audio][STATS] final: underruns=" << m_underrunCount.load()
                  << " minAvail=" << (minAvail == SIZE_MAX ? -1 : static_cast<long long>(minAvail))
                  << " samples state=" << StateName(m_state)
                  << " env=" << envBuf
                  << " availPeak=" << g_availPeak << std::endl;
        g_availPeak = 0;   // R5：打印后窗口复位
    }
    // ---- C1/s5：dump 落盘（join 之后无并发）----
    WriteDumpWav();
    ReleaseResources();
}

void WasapiRender::RenderLoop() {
    auto* audioClient = static_cast<IAudioClient*>(m_audioClient);
    auto* renderClient = static_cast<IAudioRenderClient*>(m_renderClient);
    auto* event = static_cast<HANDLE>(m_event);

    DWORD taskIndex = 0;
    HANDLE taskHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    HRESULT hr = audioClient->Start();
    if (FAILED(hr)) {
        m_lastError = "Failed to start audio client";
        m_running.store(false);
        if (taskHandle) AvRevertMmThreadCharacteristics(taskHandle);
        return;
    }

    const uint16_t channels = m_channels;
    std::vector<int16_t> temp(m_bufferFrameCount * channels, 0);

    // F4：30s 节流统计（§3.5.4）
    uint64_t lastStatsTick = GetTickCount64();
    uint64_t lastUnderruns = m_underrunCount.load(std::memory_order_relaxed);
    // T-CLIENT-SILENT-ON-RESUME（落点 2，M-1③）：循环起始时基 —— 周期 STATS
    // 行 t= 与 [Audio][RESUME] 行 t= 的同一基准（C5 存活判据，§3.4.2）。
    const uint64_t loopStartTick = GetTickCount64();

    while (!m_stopRequested.load()) {
        DWORD waitResult = WaitForSingleObject(event, 200);
        if (m_stopRequested.load()) break;
        if (waitResult != WAIT_OBJECT_0) continue;

        UINT32 padding = 0;
        hr = audioClient->GetCurrentPadding(&padding);
        // B7: FAILED or --fault-render-fail=padding -> record & break (FR-11)
        if (FAILED(hr) || FaultHit(1)) { RecordExit(1, hr); break; }

        UINT32 availableFrames = m_bufferFrameCount - padding;
        if (availableFrames == 0) continue;

        BYTE* data = nullptr;
        hr = renderClient->GetBuffer(availableFrames, &data);
        // B8: FAILED or --fault-render-fail=getbuffer -> record & break (FR-11)
        if (FAILED(hr) || FaultHit(2)) { RecordExit(2, hr); break; }

        // ===== F4 (FR-05)：三态机（m4：变长量子，samplesNeeded ≤ 960×2 = 1920）=====
        const size_t samplesNeeded = static_cast<size_t>(availableFrames) * channels;
        // m4②：包络斜率按帧折算（每帧 1/240 = 1/(fadeSamples/2)），禁止照抄浏览器
        // 的固定 128 帧假设；原生量子变长（最短 ~2.67ms、最长 20ms 可完成完整淡出）
        const double step = 1.0 / (static_cast<double>(m_fadeSamples) / 2.0);

        // ★ T-CLIENT-SILENT-ON-RESUME（落点 3 / FR-02 / G-2 / H-04）：出队【前】
        //   水位快照（对齐 embedded_page.h:148 `const avail = this.count`）。
        //   逃逸判定（下方 SOFT_UNDERRUN 分支）与 PREFILL 水位判定均用它，
        //   严禁回落到"出队之后"的 Available()（缺陷根因的一半）。
        const size_t availTop = m_ringBuffer->Available();
        if (availTop > g_availPeak) g_availPeak = availTop;   // M-5a（R3）

        if (m_state == RenderState::PREFILL) {
            const size_t w = m_everStarted ? m_resumeWaterSamples : m_startWaterSamples;
            // 落点 4：判定改用出队前快照 availTop（数值等价：PREFILL 期不 Pop，
            // 快照点与判定点之间 Available() 只可能被生产者增大）。
            if (availTop < w) {
                // m4①：PREFILL 期【不 Pop】，但必须 ReleaseBuffer(n, 0)（喂静音）——
                // 不得跳过释放（否则渲染引擎饿死、stall watchdog 误触发）。
                // 起播前静音是"还没开始"，不是硬切。
                std::memset(data, 0, samplesNeeded * sizeof(int16_t));
                m_env = 0.0;
                hr = renderClient->ReleaseBuffer(availableFrames, 0);
                // B9: FAILED or --fault-render-fail=releasebuffer -> record & break (FR-11)
                if (FAILED(hr) || FaultHit(3)) { RecordExit(3, hr); break; }
                continue;
            }
            m_state = RenderState::PLAYING;
            // ★ D-04（§3.4.4 R6 / H-07）：m_minAvailable 语义由"会话最小值"变为
            //   "当前播放段谷值"（PREFILL→PLAYING 晋升点复位为 SIZE_MAX）——
            //   为 AC-02 的"minAvail 回升 > 0"服务；首次起播为 no-op（Start() 已复位）。
            m_minAvailable.store(SIZE_MAX, std::memory_order_relaxed);
            // ★ 落点 4：非首次起播 ⇒ 这是一次"复播"（AC-02 的 PREFILL→PLAYING
            //   证据行，slot 1；独立 1s 节流 ⇒ 不会吞掉同窗口的逃逸行）。
            if (m_everStarted)
                LogResume("PREFILL->PLAYING", availTop,
                          m_underrunCount.load(std::memory_order_relaxed),
                          1, loopStartTick);
            m_everStarted = true;
            m_started = true;   // m5：谷值统计起点（首次进入 PLAYING 之后）
        }

        size_t got = 0;
        bool normal = false;
        {
            if (m_state == RenderState::SOFT_UNDERRUN) {
                // ★ T-CLIENT-SILENT-ON-RESUME（落点 5 / FR-02 / G-2 / H-03）：
                //   SOFT_UNDERRUN 累积窗口 —— 本量子【完全不出队】（got 恒 0），
                //   缓冲以满入队速率 R 累积（5760/96000 = 60ms 必达水位，任意
                //   prod−cons 关系下成立）；语义与既有 PREFILL 低水位分支同构
                //   （:363-372）。本分支不得 continue/break —— 必须落到下方共享
                //   尾部 ReleaseBuffer(:439)（§4.6 喂静音契约，H-05）。
                if (availTop >= m_resumeWaterSamples && m_env <= 0.0) {
                    // ★ FR-01：同连接自举逃逸（H-04：出队【前】availTop 判定，
                    //   位于出队分支之外、之前；原"出队后 Available() ≥ 5760"
                    //   的吸收态判定已删除 —— 等效门槛 7680 且低水位期每量子
                    //   被清零 ⇒ 永不可达）。
                    m_state = RenderState::PREFILL;
                    LogResume("SOFT_UNDERRUN->PREFILL", availTop,
                              m_underrunCount.load(std::memory_order_relaxed),
                              0, loopStartTick);
                }
                // ★ G-4 / H-06：沿用 HoldLast + 逐帧包络斜坡（:409-420 共享尾部），
                //   禁止 memset(0) 硬切（与 :399 同一函数、同一参数形状）。
                HoldLast(temp.data(), 0, samplesNeeded, channels, m_lastL, m_lastR);
            } else {
                // ★ G-1 / H-02：PLAYING 分支 —— 逐字保留原出队行为（零改动）。
                got = m_ringBuffer->Pop(temp.data(), samplesNeeded);
                if (got == samplesNeeded && m_state == RenderState::PLAYING) {
                    normal = true;
                    // ★ m3：lastL/lastR 仅在正常读取分支更新（本块最后一个完整帧）。
                    //   若在欠载分支也更新，"保持最后样本"会退化为"保持 0"。
                    if (channels == 2 && got >= 2) {
                        m_lastL = temp[got - 2];
                        m_lastR = temp[got - 1];
                    } else if (got >= 1) {
                        m_lastL = m_lastR = temp[got - 1];
                    }
                } else {
                    if (m_state == RenderState::PLAYING) {
                        m_state = RenderState::SOFT_UNDERRUN;
                        m_underrunCount.fetch_add(1, std::memory_order_relaxed);
                    }
                    // ★ 软欠载：保持最后样本（got 之后的部分），不得 memset(0)
                    HoldLast(temp.data(), got, samplesNeeded, channels, m_lastL, m_lastR);
                }
            }
        }

        // m4②/m3：包络【逐帧】线性斜坡（消除整块单一增益的 2.67ms 台阶）；
        // 单帧内两声道同一增益（无声道间阶跃），钳制 [0,1]
        const double target = normal ? 1.0 : 0.0;
        for (uint32_t f = 0; f < availableFrames; ++f) {
            m_env = (target > m_env) ? std::min(target, m_env + step)
                                     : std::max(target, m_env - step);
            const float g = static_cast<float>(m_env);
            for (uint16_t c = 0; c < channels; ++c) {
                const size_t idx = static_cast<size_t>(f) * channels + c;
                temp[idx] = static_cast<int16_t>(std::lround(static_cast<float>(temp[idx]) * g));
            }
        }

        std::memcpy(data, temp.data(), samplesNeeded * sizeof(int16_t));

        // C1/s5：录"实际送渲染"的样本（含软欠载填充）
        if (!m_dumpPath.empty()) {
            AppendDump(temp.data(), samplesNeeded);
        }

        // m5：Available() 谷值 —— 仅首次进入 PLAYING 之后统计（PREFILL 期的
        // 0 不得把谷值钉死）
        if (m_started) {
            size_t nowAvail = m_ringBuffer->Available();
            size_t prev = m_minAvailable.load(std::memory_order_relaxed);
            while (nowAvail < prev &&
                   !m_minAvailable.compare_exchange_weak(
                       prev, nowAvail, std::memory_order_relaxed, std::memory_order_relaxed)) {}
        }

        hr = renderClient->ReleaseBuffer(availableFrames, 0);
        // B9: FAILED or --fault-render-fail=releasebuffer -> record & break (FR-11)
        if (FAILED(hr) || FaultHit(3)) { RecordExit(3, hr); break; }

        // ---- F4：30s 节流统计行（V-09：stdout 可读 U 与谷值）----
        // T-CLIENT-SILENT-ON-RESUME（落点 6 / H-09）：行尾追加 state=/env=/t=/availPeak=
        // （顺序与字段分隔符不动 ⇒ 既有解析脚本向后兼容；t= 为 C5 存活判据）。
        {
            const uint64_t now = GetTickCount64();
            if (now - lastStatsTick >= 30000) {
                const uint64_t und = m_underrunCount.load(std::memory_order_relaxed);
                const size_t minAvail = m_minAvailable.load(std::memory_order_relaxed);
                char envBuf[16];
                std::snprintf(envBuf, sizeof envBuf, "%.3f", m_env);
                std::cout << "[Audio][STATS] underruns=" << und
                          << " (+" << (und - lastUnderruns) << ")"
                          << " minAvail=" << (minAvail == SIZE_MAX ? -1 : static_cast<long long>(minAvail))
                          << " samples avail=" << m_ringBuffer->Available()
                          << " state=" << StateName(m_state)
                          << " env=" << envBuf
                          << " t=" << (now - loopStartTick) << "ms"
                          << " availPeak=" << g_availPeak << std::endl;
                lastStatsTick = now;
                lastUnderruns = und;
                g_availPeak = 0;   // R5：打印后窗口复位
            }
        }
    }

    audioClient->Stop();
    if (taskHandle) AvRevertMmThreadCharacteristics(taskHandle);
    m_running.store(false);
}
