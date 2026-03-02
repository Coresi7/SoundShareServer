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

private:
    void CaptureThread();

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
};
