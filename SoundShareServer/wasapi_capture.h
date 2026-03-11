#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

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

    // Set a callback to be notified of capture errors and recovery
    void SetErrorCallback(AudioErrorCallback callback) { m_errorCallback = std::move(callback); }

private:
    void CaptureThread();
    void ReleaseDeviceResources();
    bool ReinitializeDevice();

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

    // Auto-recovery settings
    static const int MAX_RECOVERY_ATTEMPTS = 10;
    static const int RECOVERY_WAIT_MS = 2000;     // Wait between retry attempts
    static const int RECOVERY_BACKOFF_MS = 5000;   // Additional backoff after multiple failures
};
