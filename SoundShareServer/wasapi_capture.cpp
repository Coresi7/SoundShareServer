#include "wasapi_capture.h"
#include "capture_stats.h"   // F6 (S0)：进程级采集观测
#include <avrt.h>            // F2 (FR-02)：MMCSS "Pro Audio" 注册

#include <iostream>
#include <cmath>
#include <cwctype>

// ★ m11（编译级门禁 V-16）：std::clamp / std::min / std::max 双保险 include
#include <algorithm>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

namespace {
// F6 (S0)：QPC 微秒时间戳（采集侧打点；§3.0.4 Win32 原语）
uint64_t QpcNowUs() {
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

// D-15 (F7)：宽字符 → UTF-8（告警输出走 std::cout；服务端已 SetConsoleOutputCP(CP_UTF8)）
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], need, nullptr, nullptr);
    return out;
}
} // namespace

WasapiCapture::WasapiCapture() {}

WasapiCapture::~WasapiCapture() {
    Stop();
    ReleaseDeviceResources();
    // F3 (D-04)：事件句柄唯一关闭点（Stop() join 之后；对象级唯一，重初始化期间
    // 从不关闭，无双重 close / 悬垂句柄）
    if (m_captureEvent) {
        CloseHandle(m_captureEvent);
        m_captureEvent = nullptr;
    }
    if (m_enumerator) {
        m_enumerator->Release();
        m_enumerator = nullptr;
    }
}

void WasapiCapture::ReleaseDeviceResources() {
    // ★ F3 (D-04)：本函数不得触碰 m_captureEvent（重初始化复用同一句柄）。
    if (m_waveFormat) {
        CoTaskMemFree(m_waveFormat);
        m_waveFormat = nullptr;
    }
    if (m_captureClient) {
        m_captureClient->Release();
        m_captureClient = nullptr;
    }
    if (m_audioClient) {
        m_audioClient->Stop();
        m_audioClient->Release();
        m_audioClient = nullptr;
    }
    if (m_device) {
        m_device->Release();
        m_device = nullptr;
    }
}

// ---- F7 (FR-09)：显卡音频端点判定 ----
// 三条硬约束：不阻断采集（无返回值、不改音频路径参数）；不做运行时切换
// （无端点枚举/切换代码）；不替代主线 AC。每次调用都打一行（命中 = 三要素
// 告警；未命中 = 单行观察），端点变化时自然重复打印（建议④）。
void WasapiCapture::ClassifyEndpoint(const std::wstring& name, const char* prefix) {
    std::wstring lower;
    lower.reserve(name.size());
    for (wchar_t c : name) {
        lower += static_cast<wchar_t>(std::towlower(c));
    }
    // 命中判据（需求原四类全部保留，不删减）+ s1 追加项（闸门 03b §4.2 裁决同意）
    static const wchar_t* const kKeywords[] = {
        L"nvidia high definition audio",
        L"amd high definition audio",
        L"hdmi",
        L"displayport",
        L"intel(r) display audio",   // ★ s1：核显 DP 音频不含 HDMI/DisplayPort 字样
    };
    bool gpu = false;
    for (const wchar_t* kw : kKeywords) {
        if (lower.find(kw) != std::wstring::npos) {
            gpu = true;
            break;
        }
    }

    // ★ B-1（reviewer 返工）：写端持 m_deviceInfoMutex 更新（读端 =
    //   DeviceFriendlyName() 由 HTTP/stdin 探针线程调用；原裸写构成 UB 竞争）。
    //   S-5：首次分类（旧名为空）不算 "endpoint changed"，不再误报后缀。
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_deviceInfoMutex);
        changed = !m_deviceFriendlyName.empty() && (name != m_deviceFriendlyName);
        m_deviceFriendlyName = name;
    }
    g_captureStats.endpointIsGpuAudio.store(gpu, std::memory_order_relaxed);

    const std::string utf8 = WideToUtf8(name);
    if (gpu) {
        // 三要素：①显卡音频端点 ②GPU 高负载可能毛刺 ③建议改用主板/USB 声卡
        std::cout << prefix << "[WARN] GPU audio endpoint detected (" << utf8
                  << (changed ? ", endpoint changed" : "")
                  << "): GPU heavy load may cause glitches/clicks on remote playback; "
                     "consider switching to motherboard or USB audio (local monitor is "
                     "unaffected). Continuing capture - NOT blocked." << std::endl;
    } else {
        std::cout << prefix << " endpoint=" << utf8
                  << " gpuAudio=false" << (changed ? " (endpoint changed)" : "") << std::endl;
    }
}

// ★ B-1（reviewer 返工）：端点友好名跨线程快照 —— 持 m_deviceInfoMutex 拷贝
//   返回（按值）。调用方 = main.cpp probe hook（HTTP `/_debug/sessions` 线程
//   与 stdin "dump" 线程，经 DumpProbeJson 在释放 L1 之后调用）。锁为叶锁
//   （临界区仅字符串拷贝，见 wasapi_capture.h 成员注释的锁序合规性说明）。
std::wstring WasapiCapture::DeviceFriendlyName() const {
    std::lock_guard<std::mutex> lock(m_deviceInfoMutex);
    return m_deviceFriendlyName;
}

// ---- F1+F3 双路径共用核心 ----
// 在 m_device 上 Activate client、取 MixFormat、按降级阶梯（m12）Initialize、
// 事件接线（F3）、GetBufferSize、GetService、探针记录。
// wantEvent=true：带 AUDCLNT_STREAMFLAGS_EVENTCALLBACK + SetEventHandle；
//   SetEventHandle/创建句柄失败 ⇒ 返回 false（该 client 已带 EVENTCALLBACK 却
//   无事件，Start() 会失败 ⇒ 调用方须以 wantEvent=false 重建降级轮询）。
bool WasapiCapture::ActivateClientAndInitialize(bool wantEvent, const char* logPrefix) {
    // 二次进入（事件模式失败降级重建）时释放上一轮 client/格式
    // （IAudioClient 仅可 Initialize 一次）
    if (m_captureClient) {
        m_captureClient->Release();
        m_captureClient = nullptr;
    }
    if (m_audioClient) {
        m_audioClient->Release();
        m_audioClient = nullptr;
    }
    if (m_waveFormat) {
        CoTaskMemFree(m_waveFormat);
        m_waveFormat = nullptr;
    }

    HRESULT hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    (void**)&m_audioClient);
    if (FAILED(hr)) {
        std::cerr << logPrefix << " Failed to activate audio client: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    hr = m_audioClient->GetMixFormat(&m_waveFormat);
    if (FAILED(hr)) {
        std::cerr << logPrefix << " Failed to get mix format: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }
    m_sampleRate = m_waveFormat->nSamplesPerSec;
    m_channels = m_waveFormat->nChannels;
    std::cout << logPrefix << " Format: " << m_sampleRate << " Hz, " << m_channels
              << " channels, " << m_waveFormat->wBitsPerSample << " bits" << std::endl;

    DWORD streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK;   // LOOPBACK - captures system audio output
    if (wantEvent) {
        streamFlags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;   // F3：事件驱动
    }

    // F1 (m12)：降级阶梯 = m_captureBufferHns → 50ms → 20ms（现状兜底）；
    // 每次成功后成员就地更新为实际生效值 ⇒ 双路径同源恒成立（V-01）
    const REFERENCE_TIME candidates[3] = {
        m_captureBufferHns,
        kMinCaptureBufferHns,
        kLegacyCaptureBufferHns,
    };
    bool initialized = false;
    for (REFERENCE_TIME want : candidates) {
        if (want > kMaxCaptureBufferHns) continue;   // 防御
        hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags,
                                       want, 0, m_waveFormat, nullptr);
        if (SUCCEEDED(hr)) {
            if (want != m_captureBufferHns) {
                std::cout << logPrefix << " Capture buffer " << m_captureBufferHns
                          << " hns rejected; degraded to " << want << " hns" << std::endl;
                m_captureBufferHns = want;   // ★ m12：就地更新（探针暴露实际值，不静默）
            }
            initialized = true;
            break;
        }
        std::cerr << logPrefix << " Initialize(" << want << " hns) failed: 0x"
                  << std::hex << hr << std::dec << std::endl;
    }
    if (!initialized) {
        std::cerr << logPrefix << " Failed to initialize audio client (all buffer sizes)"
                  << std::endl;
        return false;
    }

    if (wantEvent) {
        // F3 (D-04)：仅首次创建（幂等）；对象级唯一句柄，重初始化复用不重建
        if (!m_captureEvent) {
            m_captureEvent = CreateEventW(nullptr, FALSE /*auto-reset*/, FALSE, nullptr);
            if (m_captureEvent) {
                g_captureStats.eventHandlesCreated.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::cerr << logPrefix << " CreateEvent failed: " << GetLastError() << std::endl;
            }
        }
        if (m_captureEvent && SUCCEEDED(m_audioClient->SetEventHandle(m_captureEvent))) {
            ResetEvent(m_captureEvent);   // 丢弃接线前的陈旧信号
            g_captureStats.eventDriven.store(true, std::memory_order_relaxed);
        } else {
            // ★ 降级：保持 Sleep 轮询（本 client 作废，由调用方重建）
            g_captureStats.eventDriven.store(false, std::memory_order_relaxed);
            return false;
        }
    } else {
        g_captureStats.eventDriven.store(false, std::memory_order_relaxed);
    }

    hr = m_audioClient->GetBufferSize(&m_bufferFrameCount);
    if (FAILED(hr)) {
        std::cerr << logPrefix << " Failed to get buffer size: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }
    // F6：实际生效缓冲暴露到探针（captureBufferHns/captureBufferFrames）
    g_captureStats.bufferHns.store(static_cast<uint32_t>(m_captureBufferHns),
                                   std::memory_order_relaxed);
    g_captureStats.bufferFrames.store(m_bufferFrameCount, std::memory_order_relaxed);

    hr = m_audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&m_captureClient);
    if (FAILED(hr)) {
        std::cerr << logPrefix << " Failed to get capture client: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }
    return true;
}

bool WasapiCapture::ReinitializeDevice() {
    // Release old device resources (but keep enumerator; ★ F3: 事件句柄不受影响)
    ReleaseDeviceResources();

    HRESULT hr;

    // Re-create enumerator if needed
    if (!m_enumerator) {
        hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
            (void**)&m_enumerator);
        if (FAILED(hr)) {
            std::cerr << "[WASAPI] Recovery: Failed to create device enumerator: 0x" << std::hex << hr << std::dec << std::endl;
            return false;
        }
    }

    // Get default audio output device
    hr = m_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_device);
    if (FAILED(hr)) {
        std::cerr << "[WASAPI] Recovery: Failed to get default audio endpoint: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    // Print device name + F7 classification (双路径之一)
    IPropertyStore* props = nullptr;
    hr = m_device->OpenPropertyStore(STGM_READ, &props);
    if (SUCCEEDED(hr)) {
        PROPVARIANT varName;
        PropVariantInit(&varName);
        hr = props->GetValue(PKEY_Device_FriendlyName, &varName);
        if (SUCCEEDED(hr)) {
            const std::wstring name = varName.pwszVal ? varName.pwszVal : L"";
            std::wcout << L"[WASAPI] Recovery: Now capturing from: " << name << std::endl;
            // F7：先拷入成员（ClassifyEndpoint 内部）再 PropVariantClear
            ClassifyEndpoint(name, "[WASAPI] Recovery");
            PropVariantClear(&varName);
        }
        props->Release();
    }

    // F1+F3：先事件驱动；不可用则降级轮询重建（双路径同源 helper）
    if (!ActivateClientAndInitialize(true, "[WASAPI] Recovery")) {
        std::cerr << "[WASAPI] Recovery: event-driven init unavailable; falling back to Sleep polling" << std::endl;
        if (!ActivateClientAndInitialize(false, "[WASAPI] Recovery")) {
            std::cerr << "[WASAPI] Recovery: Failed to initialize audio client" << std::endl;
            return false;
        }
    }

    hr = m_audioClient->Start();
    if (FAILED(hr)) {
        std::cerr << "[WASAPI] Recovery: Failed to start capture: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    std::cout << "[WASAPI] Recovery: Device reinitialized successfully. Buffer size: " << m_bufferFrameCount << " frames" << std::endl;
    // F6 (S0)：重初始化计数（探针 captureReinitCount；V-04 判据）
    g_captureStats.reinitCount.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool WasapiCapture::Initialize() {
    HRESULT hr;

    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
        (void**)&m_enumerator);
    if (FAILED(hr)) {
        std::cerr << "[WASAPI] Failed to create device enumerator: 0x" << std::hex << hr << std::endl;
        return false;
    }

    // Get default audio output device (eRender = output, for loopback capture)
    hr = m_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_device);
    if (FAILED(hr)) {
        std::cerr << "[WASAPI] Failed to get default audio endpoint: 0x" << std::hex << hr << std::endl;
        return false;
    }

    // Print device name + F7 classification (双路径之一；落点 = 需求点名的 :158 区域)
    IPropertyStore* props = nullptr;
    hr = m_device->OpenPropertyStore(STGM_READ, &props);
    if (SUCCEEDED(hr)) {
        PROPVARIANT varName;
        PropVariantInit(&varName);
        hr = props->GetValue(PKEY_Device_FriendlyName, &varName);
        if (SUCCEEDED(hr)) {
            const std::wstring name = varName.pwszVal ? varName.pwszVal : L"";
            std::wcout << L"[WASAPI] Capturing from: " << name << std::endl;
            // F7：先拷入成员（ClassifyEndpoint 内部）再 PropVariantClear
            ClassifyEndpoint(name, "[WASAPI]");
            PropVariantClear(&varName);
        }
        props->Release();
    }

    // F1+F3：先事件驱动；不可用则降级轮询重建（双路径同源 helper）
    if (!ActivateClientAndInitialize(true, "[WASAPI]")) {
        std::cerr << "[WASAPI] Event-driven init unavailable; falling back to Sleep polling" << std::endl;
        if (!ActivateClientAndInitialize(false, "[WASAPI]")) {
            std::cerr << "[WASAPI] Failed to initialize audio client" << std::endl;
            return false;
        }
    }

    std::cout << "[WASAPI] Initialized successfully. Buffer size: " << m_bufferFrameCount << " frames" << std::endl;
    return true;
}

bool WasapiCapture::Start(AudioDataCallback callback) {
    if (m_capturing.load()) return false;

    m_callback = std::move(callback);

    // ★ m9 (F3)：顺序写死 —— ① ResetEvent（丢弃 Start 前的陈旧信号，避免首帧
    //   立即返回）→ ② m_audioClient->Start() → ③ 建线程。句柄为空则跳过①。
    if (m_captureEvent) {
        ResetEvent(m_captureEvent);
    }
    HRESULT hr = m_audioClient->Start();
    if (FAILED(hr)) {
        std::cerr << "[WASAPI] Failed to start capture: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    m_capturing.store(true);
    m_captureThread = std::thread(&WasapiCapture::CaptureThread, this);
    std::cout << "[WASAPI] Capture started" << std::endl;
    return true;
}

void WasapiCapture::Stop() {
    if (!m_capturing.load()) return;

    // ★ m9 (F3)：顺序写死 —— m_capturing=false → SetEvent（唤醒等待中的线程；
    //   auto-reset 信号保持 ⇒ 不丢唤醒）→ join（最坏 200ms 超时 + 一次 drain，
    //   不死锁）→ m_audioClient->Stop()。
    m_capturing.store(false);
    if (m_captureEvent) {
        SetEvent(m_captureEvent);
    }
    if (m_captureThread.joinable()) {
        m_captureThread.join();
    }

    if (m_audioClient) {
        m_audioClient->Stop();
    }
    std::cout << "[WASAPI] Capture stopped" << std::endl;
}

static const char* HrToString(HRESULT hr) {
    switch (hr) {
    case AUDCLNT_E_DEVICE_INVALIDATED:     return "DEVICE_INVALIDATED";
    case AUDCLNT_E_SERVICE_NOT_RUNNING:    return "SERVICE_NOT_RUNNING";
    case AUDCLNT_E_RESOURCES_INVALIDATED:  return "RESOURCES_INVALIDATED";
    case AUDCLNT_E_BUFFER_ERROR:           return "BUFFER_ERROR";
    default:                               return "UNKNOWN";
    }
}

namespace {
// F2 (FR-02)：MMCSS RAII —— 注册失败仅告警不阻断；所有退出路径（正常结束 /
// break / try_recovery 的 break / 异常）都 revert，且 revert 先于 join 完成
// （线程局部句柄，AvRevert 必须由同一线程调用）
struct MmcssScope {
    HANDLE h = nullptr;
    MmcssScope() {
        DWORD idx = 0;
        h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &idx);
    }
    ~MmcssScope() {
        if (h) AvRevertMmThreadCharacteristics(h);
    }
    MmcssScope(const MmcssScope&) = delete;
    MmcssScope& operator=(const MmcssScope&) = delete;
};
} // namespace

void WasapiCapture::CaptureThread() {
    // F2 (FR-02)：函数体开头注册 MMCSS "Pro Audio"（RAII；注册失败仅告警不阻断）
    MmcssScope mmcss;
    g_captureStats.mmcssRegistered.store(mmcss.h != nullptr, std::memory_order_relaxed);
    if (!mmcss.h) {
        std::cerr << "[WASAPI] MMCSS Pro Audio registration failed (continuing)" << std::endl;
    }

    std::vector<float> convertBuffer;
    int recoveryAttempts = 0;

    auto detectFormat = [this](bool& isFloat, int& bitsPerSample) {
        isFloat = false;
        bitsPerSample = m_waveFormat->wBitsPerSample;
        if (m_waveFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            isFloat = true;
        } else if (m_waveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            WAVEFORMATEXTENSIBLE* wfex = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(m_waveFormat);
            if (IsEqualGUID(wfex->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
                isFloat = true;
            }
        }
    };

    // m10：calcSleepMs 保留（F3 降级轮询路径仍需；不改轮询频率，FR-01 机理约束）
    auto calcSleepMs = [this]() -> DWORD {
        REFERENCE_TIME hnsActualDuration = (REFERENCE_TIME)((double)m_bufferFrameCount * 10000000.0 / m_sampleRate);
        DWORD ms = (DWORD)(hnsActualDuration / 10000 / 2);
        if (ms < 1) ms = 1;
        if (ms > 10) ms = 10;
        return ms;
    };

    bool isFloat = false;
    int bitsPerSample = 0;
    detectFormat(isFloat, bitsPerSample);
    DWORD sleepMs = calcSleepMs();
    if (sleepMs < 1) sleepMs = kCapturePollFallbackMs;   // 防御（calcSleepMs 已保证 ≥1）

    uint64_t lastWakeUs = 0;   // F6 (FR-08)：到达间隔上一打点时刻（µs）
    // ★ S-2（reviewer 建议）：跨 reset 边界首样本防护 —— stats_reset 递增
    //   arrivalEpoch；打点时发现 epoch 变化 ⇒ 丢弃该次间隔（lastWakeUs 属于
    //   上一窗口，不代表本窗口到达分布）。
    uint64_t lastArrivalEpoch = g_captureStats.arrivalEpoch.load(std::memory_order_relaxed);
    uint64_t timeoutLogCounter = 0;   // m2：超时节流日志计数

    while (m_capturing.load()) {
        // ---- F3 (FR-03)：事件驱动等待；降级路径 = Sleep 轮询（行为与现状一致）----
        if (m_captureEvent && g_captureStats.eventDriven.load(std::memory_order_relaxed)) {
            DWORD wr = WaitForSingleObject(m_captureEvent, kCaptureEventTimeoutMs);
            if (!m_capturing.load()) break;   // Stop() 已置位 + SetEvent
            g_captureStats.eventWakeTotal.fetch_add(1, std::memory_order_relaxed);
            if (wr == WAIT_TIMEOUT) {
                // m2：超时计数进探针（识别"SetEventHandle 成功但事件从不触发"）
                g_captureStats.eventTimeoutTotal.fetch_add(1, std::memory_order_relaxed);
                if (++timeoutLogCounter % 50 == 1) {
                    std::cerr << "[WASAPI] Capture event timeout #" << timeoutLogCounter
                              << " (continuing with defensive drain)" << std::endl;
                }
                // 仍走一次防御性 drain（下方公共 drain 路径）
            }
        } else {
            Sleep(sleepMs);   // 降级路径（不计超时）
        }

        const uint64_t wakeUs = QpcNowUs();   // F6：本次唤醒时刻
        uint32_t windowFrames = 0;            // F6：本次唤醒取得的帧数（同次唤醒多 packet 记一个样本）

        UINT32 packetLength = 0;
        HRESULT hr = m_captureClient->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) {
            std::cerr << "[WASAPI] GetNextPacketSize failed: 0x" << std::hex << hr << std::dec
                      << " (" << HrToString(hr) << ")" << std::endl;
            goto try_recovery;
        }

        while (packetLength > 0) {
            BYTE* data = nullptr;
            UINT32 numFramesAvailable = 0;
            DWORD flags = 0;

            hr = m_captureClient->GetBuffer(&data, &numFramesAvailable, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                std::cerr << "[WASAPI] GetBuffer failed: 0x" << std::hex << hr << std::dec
                          << " (" << HrToString(hr) << ")" << std::endl;
                goto try_recovery;
            }

            // ★ m1 (FR-07)：flags 判定位于 GetBuffer 成功之后、
            //   if (numFramesAvailable > 0 && m_callback) 判断【之外】、
            //   ReleaseBuffer 之前 —— 0 帧 / 无回调时也计数，D 才可信。
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                g_captureStats.discontinuityTotal.fetch_add(1, std::memory_order_relaxed);
            }
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                g_captureStats.silentTotal.fetch_add(1, std::memory_order_relaxed);
            }
            g_captureStats.packets.fetch_add(1, std::memory_order_relaxed);
            g_captureStats.frames.fetch_add(numFramesAvailable, std::memory_order_relaxed);
            windowFrames += numFramesAvailable;

            if (numFramesAvailable > 0 && m_callback) {
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    // Send silence
                    convertBuffer.resize(numFramesAvailable * m_channels, 0.0f);
                    m_callback(convertBuffer.data(), numFramesAvailable, m_channels, m_sampleRate);
                } else if (isFloat) {
                    // Data is already float, pass directly
                    m_callback(reinterpret_cast<const float*>(data), numFramesAvailable, m_channels, m_sampleRate);
                } else if (bitsPerSample == 16) {
                    // Convert int16 to float
                    const int16_t* src = reinterpret_cast<const int16_t*>(data);
                    uint32_t totalSamples = numFramesAvailable * m_channels;
                    convertBuffer.resize(totalSamples);
                    for (uint32_t i = 0; i < totalSamples; i++) {
                        convertBuffer[i] = static_cast<float>(src[i]) / 32768.0f;
                    }
                    m_callback(convertBuffer.data(), numFramesAvailable, m_channels, m_sampleRate);
                }
            }

            hr = m_captureClient->ReleaseBuffer(numFramesAvailable);
            if (FAILED(hr)) {
                std::cerr << "[WASAPI] ReleaseBuffer failed: 0x" << std::hex << hr << std::dec
                          << " (" << HrToString(hr) << ")" << std::endl;
                goto try_recovery;
            }

            hr = m_captureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) {
                std::cerr << "[WASAPI] GetNextPacketSize (inner) failed: 0x" << std::hex << hr << std::dec
                          << " (" << HrToString(hr) << ")" << std::endl;
                goto try_recovery;
            }
        }

        // F6 (FR-08)：到达间隔打点 —— 本次唤醒且取到数据 ⇒ 一个样本（§6.2 口径；
        // 不受 hasStreamingClients 影响；Sleep 轮询 / 事件驱动两种等待模式同一点打点）。
        // ★ S-2：epoch 变化（窗口 reset 落在两次唤醒之间）⇒ 丢弃该次跨边界间隔，
        //   以本次唤醒为新基准。
        if (windowFrames > 0) {
            const uint64_t epoch = g_captureStats.arrivalEpoch.load(std::memory_order_relaxed);
            if (lastWakeUs != 0 && epoch == lastArrivalEpoch) {
                g_captureStats.interArrival.AddUs(wakeUs - lastWakeUs);
            }
            lastArrivalEpoch = epoch;
            lastWakeUs = wakeUs;
        }

#ifdef SOUNDSHARE_ENABLE_FAULT_INJECTION
        // NFR-TEST-1 (AC-FR03 / V-04)：force_capture_reinit 一次性触发 →
        // 走真实 try_recovery（hr=S_OK：主动 reinit 非设备错误）
        if (g_captureStats.reinitRequested.exchange(false, std::memory_order_relaxed)) {
            hr = S_OK;
            goto try_recovery;
        }
#endif

        // Reset recovery counter on successful iteration
        recoveryAttempts = 0;
        continue;

    try_recovery:
        // Attempt auto-recovery
        // m10：五项零改动（重试点数 10 / 等待 2000 / 退避 5000 / 错误回调 / 计数复位）
        recoveryAttempts++;
        std::cerr << "[WASAPI] Capture error detected. Recovery attempt "
                  << recoveryAttempts << "/" << MAX_RECOVERY_ATTEMPTS << std::endl;

        if (m_errorCallback) {
            m_errorCallback(hr, "Capture device error, attempting recovery", false);
        }

        if (recoveryAttempts > MAX_RECOVERY_ATTEMPTS) {
            std::cerr << "[WASAPI] Max recovery attempts exceeded. Capture thread exiting." << std::endl;
            if (m_errorCallback) {
                m_errorCallback(hr, "Max recovery attempts exceeded, capture stopped", false);
            }
            break;
        }

        // Wait before retry, with increasing backoff
        int waitMs = RECOVERY_WAIT_MS;
        if (recoveryAttempts > 3) {
            waitMs += RECOVERY_BACKOFF_MS;
        }
        for (int waited = 0; waited < waitMs && m_capturing.load(); waited += 100) {
            Sleep(100);
        }
        if (!m_capturing.load()) break;

        // Try to reinitialize the device
        if (ReinitializeDevice()) {
            std::cout << "[WASAPI] Recovery successful after " << recoveryAttempts << " attempt(s)" << std::endl;
            if (m_errorCallback) {
                m_errorCallback(0, "Capture device recovered successfully", true);
            }
            // m10 新增①：重初始化成功后 ResetEvent（丢弃恢复期的陈旧信号）
            if (m_captureEvent) {
                ResetEvent(m_captureEvent);
            }
            // Re-detect format and sleep time from new device
            detectFormat(isFloat, bitsPerSample);
            sleepMs = calcSleepMs();
            if (sleepMs < 1) sleepMs = kCapturePollFallbackMs;
            recoveryAttempts = 0;
        } else {
            std::cerr << "[WASAPI] Recovery attempt " << recoveryAttempts << " failed, will retry..." << std::endl;
        }
    }

    if (!m_capturing.load()) {
        std::cout << "[WASAPI] Capture thread stopped normally" << std::endl;
    } else {
        std::cerr << "[WASAPI] Capture thread exited due to unrecoverable error" << std::endl;
        m_capturing.store(false);
    }
}
