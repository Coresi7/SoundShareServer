#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

// ★ m11（编译级门禁 V-16）：SetCaptureBufferHns 内联函数体使用 std::clamp，
//   而 main.cpp 先包含本头、后才包含 <algorithm> ⇒ 本头必须自带。
#include <algorithm>

#include <atomic>
#include <functional>
#include <thread>
#include <vector>
#include <string>
#include <mutex>

// Callback: receives interleaved float samples, channel count, and sample rate
using AudioDataCallback = std::function<void(const float* data, uint32_t frameCount, uint32_t channels, uint32_t sampleRate)>;

// Callback: notifies about capture errors and recovery attempts
// Parameters: errorCode (HRESULT), message, recovered (true if successfully recovered)
using AudioErrorCallback = std::function<void(long errorCode, const std::string& message, bool recovered)>;

class WasapiCapture {
public:
    WasapiCapture();
    ~WasapiCapture();

    bool Initialize();
    bool Start(AudioDataCallback callback);
    void Stop();
    bool IsCapturing() const { return m_capturing.load(); }

    uint32_t GetSampleRate() const { return m_sampleRate; }
    uint32_t GetChannels() const { return m_channels; }

    // F6/F7：当前采集端点友好名（Initialize/ReinitializeDevice 双路径更新；
    // 探针 captureEndpointName 数据源）。
    // ★ B-1（reviewer 返工）：跨线程同步访问 —— 写方 = 采集线程
    //   （ReinitializeDevice → ClassifyEndpoint）与主线程 Initialize（服务器
    //   启动前，无并发）；读方 = HTTP/stdin 探针线程（main.cpp probe hook）。
    //   按**值**返回（内部持 m_deviceInfoMutex 拷贝），不再返回成员引用
    //   （引用返回 + 采集线程并发写 = UB 数据竞争）。
    std::wstring DeviceFriendlyName() const;
    // （删除 EndpointIsGpuAudio()/m_endpointIsGpuAudio，S-7：全仓无调用方且
    //   非原子；探针 captureEndpointIsGpuAudio 走 g_captureStats 原子镜像）

    // ---- F1 (FR-01)：采集缓冲单一常量来源（Initialize 与 ReinitializeDevice
    //      双路径共用，杜绝双路径漂移）。单位 REFERENCE_TIME = 100ns：
    //      100ms = 1,000,000 hns（★ 历史错误备忘：100000 是 10ms，绝不能写）。
    static constexpr REFERENCE_TIME kDefaultCaptureBufferHns = 1000000;  // 100ms
    static constexpr REFERENCE_TIME kMinCaptureBufferHns     = 500000;   // 50ms（区间下限）
    static constexpr REFERENCE_TIME kMaxCaptureBufferHns     = 1000000;  // 100ms（区间上限）
    static constexpr REFERENCE_TIME kLegacyCaptureBufferHns  = 200000;   // 20ms（降级阶梯最后兜底 = 现状值）
    // 钳制 [kMin, kMax]；m_configuredBufferHns 另存显式请求值（仅日志对照，m12）
    void SetCaptureBufferHns(REFERENCE_TIME hns) {
        m_configuredBufferHns = hns;
        m_captureBufferHns = std::clamp(hns, kMinCaptureBufferHns, kMaxCaptureBufferHns);
    }
    REFERENCE_TIME CaptureBufferHns() const { return m_captureBufferHns; }
    REFERENCE_TIME ConfiguredBufferHns() const { return m_configuredBufferHns; }

    // Set a callback to be notified of capture errors and recovery
    void SetErrorCallback(AudioErrorCallback callback) { m_errorCallback = std::move(callback); }

private:
    void CaptureThread();
    void ReleaseDeviceResources();
    bool ReinitializeDevice();
    // F7 (FR-09)：显卡音频端点判定（双路径调用；纯日志+标志位，无返回值、
    // 不阻断采集、不做运行时切换）
    void ClassifyEndpoint(const std::wstring& name, const char* prefix);
    // F1+F3 双路径共用核心：Activate + GetMixFormat + 按降级阶梯 Initialize
    //（m12 就地更新成员）+ 事件接线（wantEvent）+ GetBufferSize + GetService +
    // 探针记录。wantEvent=true 时 SetEventHandle 失败返回 false 供降级重建
    //（带 EVENTCALLBACK 却未 SetEventHandle 的 client Start() 会失败）。
    bool ActivateClientAndInitialize(bool wantEvent, const char* logPrefix);

    IMMDeviceEnumerator* m_enumerator = nullptr;
    IMMDevice* m_device = nullptr;
    IAudioClient* m_audioClient = nullptr;
    IAudioCaptureClient* m_captureClient = nullptr;

    WAVEFORMATEX* m_waveFormat = nullptr;
    uint32_t m_sampleRate = 0;
    uint32_t m_channels = 0;
    uint32_t m_bufferFrameCount = 0;

    std::atomic<bool> m_capturing{ false };
    std::thread m_captureThread;
    AudioDataCallback m_callback;
    AudioErrorCallback m_errorCallback;

    // F6/F7：端点友好名（双路径拷入，供探针/告警）。
    // ★ B-1（reviewer 返工）：m_deviceFriendlyName 由 m_deviceInfoMutex 保护
    //   （写 = ClassifyEndpoint【采集线程/主线程 Initialize】；读 =
    //   DeviceFriendlyName【HTTP/stdin 探针线程】）。
    //   锁序合规性（对照 write_queue.h 头部 L1/L2/L3 约定）：
    //   - 本锁临界区只含 std::wstring 的比较/赋值/拷贝 —— 纯内存操作，
    //     不获取其它锁、不做 IO、不打日志 ⇒ 出边为空 ⇒ **叶锁**（与 L3 同类）；
    //   - 写方 ClassifyEndpoint 的调用链（ReinitializeDevice ← CaptureThread
    //     try_recovery / Initialize ← main 启动）不持 L1/L2/L3；
    //   - 读方 probe hook 由 DumpProbeJson() 在**释放 L1 之后**调用
    //     （http_server.cpp DumpProbeJson ①持 L1 → ②出 L1 才调 hook），hook
    //     内部先短暂取 L2 取计数并立即释放，之后才读本快照 ⇒ 读取点不持
    //     L1/L2/L3；
    //   - 叶锁无出边 ⇒ 与 L1/L2/L3 的任何实际嵌套都不构成环；且当前所有
    //     获取点均为零锁持有 ⇒ 实际上连嵌套都不存在。
    mutable std::mutex m_deviceInfoMutex;
    std::wstring m_deviceFriendlyName;   // guarded by m_deviceInfoMutex

    // F1 (FR-01)：音频路径只认 m_captureBufferHns（降级后就地更新 = 实际生效值，
    // 双路径同源恒成立）；m_configuredBufferHns 仅日志对照（m12）
    REFERENCE_TIME m_captureBufferHns = kDefaultCaptureBufferHns;
    REFERENCE_TIME m_configuredBufferHns = kDefaultCaptureBufferHns;

    // ---- F3 (FR-03)：事件驱动 ----
    // ★ D-04：对象级唯一事件句柄 —— 仅首次创建（eventHandlesCreated 计数），
    //   重初始化复用（不重建不关闭）；唯一关闭点 = ~WasapiCapture()（Stop()
    //   join 之后）。g_captureStats.eventDriven 是运行时判定源（降级标志）。
    HANDLE m_captureEvent = nullptr;
    static constexpr DWORD kCaptureEventTimeoutMs = 200;   // 与播放端 wasapi_render.cpp 同构
    static constexpr DWORD kCapturePollFallbackMs = 10;    // 事件不可用时的降级轮询

    // Auto-recovery settings
    static const int MAX_RECOVERY_ATTEMPTS = 10;
    static const int RECOVERY_WAIT_MS = 2000;     // Wait between retry attempts
    static const int RECOVERY_BACKOFF_MS = 5000;   // Additional backoff after multiple failures
};
