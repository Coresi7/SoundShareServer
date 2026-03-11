#include "wasapi_capture.h"
#include <iostream>
#include <cmath>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

WasapiCapture::WasapiCapture() {}

WasapiCapture::~WasapiCapture() {
    Stop();
    ReleaseDeviceResources();
    if (m_enumerator) {
        m_enumerator->Release();
        m_enumerator = nullptr;
    }
}

void WasapiCapture::ReleaseDeviceResources() {
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

bool WasapiCapture::ReinitializeDevice() {
    // Release old device resources (but keep enumerator)
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

    // Print device name
    IPropertyStore* props = nullptr;
    hr = m_device->OpenPropertyStore(STGM_READ, &props);
    if (SUCCEEDED(hr)) {
        PROPVARIANT varName;
        PropVariantInit(&varName);
        hr = props->GetValue(PKEY_Device_FriendlyName, &varName);
        if (SUCCEEDED(hr)) {
            std::wcout << L"[WASAPI] Recovery: Now capturing from: " << varName.pwszVal << std::endl;
            PropVariantClear(&varName);
        }
        props->Release();
    }

    hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_audioClient);
    if (FAILED(hr)) {
        std::cerr << "[WASAPI] Recovery: Failed to activate audio client: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    hr = m_audioClient->GetMixFormat(&m_waveFormat);
    if (FAILED(hr)) {
        std::cerr << "[WASAPI] Recovery: Failed to get mix format: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    m_sampleRate = m_waveFormat->nSamplesPerSec;
    m_channels = m_waveFormat->nChannels;

    std::cout << "[WASAPI] Recovery: Format: " << m_sampleRate << " Hz, " << m_channels << " channels, "
              << m_waveFormat->wBitsPerSample << " bits" << std::endl;

    REFERENCE_TIME hnsRequestedDuration = 200000; // 20ms buffer
    hr = m_audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        hnsRequestedDuration,
        0,
        m_waveFormat,
        nullptr);
    if (FAILED(hr)) {
        std::cerr << "[WASAPI] Recovery: Failed to initialize audio client: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    hr = m_audioClient->GetBufferSize(&m_bufferFrameCount);
    if (FAILED(hr)) {
        std::cerr << "[WASAPI] Recovery: Failed to get buffer size: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    hr = m_audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&m_captureClient);
    if (FAILED(hr)) {
        std::cerr << "[WASAPI] Recovery: Failed to get capture client: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    hr = m_audioClient->Start();
    if (FAILED(hr)) {
        std::cerr << "[WASAPI] Recovery: Failed to start capture: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    std::cout << "[WASAPI] Recovery: Device reinitialized successfully. Buffer size: " << m_bufferFrameCount << " frames" << std::endl;
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

    // Print device name
    IPropertyStore* props = nullptr;
    hr = m_device->OpenPropertyStore(STGM_READ, &props);
    if (SUCCEEDED(hr)) {
        PROPVARIANT varName;
        PropVariantInit(&varName);
        hr = props->GetValue(PKEY_Device_FriendlyName, &varName);
        if (SUCCEEDED(hr)) {
            std::wcout << L"[WASAPI] Capturing from: " << varName.pwszVal << std::endl;
            PropVariantClear(&varName);
        }
        props->Release();
    }

    hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_audioClient);
    if (FAILED(hr)) {
        std::cerr << "[WASAPI] Failed to activate audio client: 0x" << std::hex << hr << std::endl;
        return false;
    }

    hr = m_audioClient->GetMixFormat(&m_waveFormat);
    if (FAILED(hr)) {
        std::cerr << "[WASAPI] Failed to get mix format: 0x" << std::hex << hr << std::endl;
        return false;
    }

    m_sampleRate = m_waveFormat->nSamplesPerSec;
    m_channels = m_waveFormat->nChannels;

    std::cout << "[WASAPI] Format: " << m_sampleRate << " Hz, " << m_channels << " channels, "
              << m_waveFormat->wBitsPerSample << " bits" << std::endl;

    // Initialize in loopback mode (AUDCLNT_STREAMFLAGS_LOOPBACK is the key!)
    REFERENCE_TIME hnsRequestedDuration = 200000; // 20ms buffer
    hr = m_audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,  // LOOPBACK mode - captures system audio output
        hnsRequestedDuration,
        0,
        m_waveFormat,
        nullptr);
    if (FAILED(hr)) {
        std::cerr << "[WASAPI] Failed to initialize audio client: 0x" << std::hex << hr << std::endl;
        return false;
    }

    hr = m_audioClient->GetBufferSize(&m_bufferFrameCount);
    if (FAILED(hr)) {
        std::cerr << "[WASAPI] Failed to get buffer size: 0x" << std::hex << hr << std::endl;
        return false;
    }

    hr = m_audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&m_captureClient);
    if (FAILED(hr)) {
        std::cerr << "[WASAPI] Failed to get capture client: 0x" << std::hex << hr << std::endl;
        return false;
    }

    std::cout << "[WASAPI] Initialized successfully. Buffer size: " << m_bufferFrameCount << " frames" << std::endl;
    return true;
}

bool WasapiCapture::Start(AudioDataCallback callback) {
    if (m_capturing.load()) return false;

    m_callback = std::move(callback);

    HRESULT hr = m_audioClient->Start();
    if (FAILED(hr)) {
        std::cerr << "[WASAPI] Failed to start capture: 0x" << std::hex << hr << std::endl;
        return false;
    }

    m_capturing.store(true);
    m_captureThread = std::thread(&WasapiCapture::CaptureThread, this);
    std::cout << "[WASAPI] Capture started" << std::endl;
    return true;
}

void WasapiCapture::Stop() {
    if (!m_capturing.load()) return;

    m_capturing.store(false);
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

void WasapiCapture::CaptureThread() {
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

    while (m_capturing.load()) {
        Sleep(sleepMs);

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

        // Reset recovery counter on successful iteration
        recoveryAttempts = 0;
        continue;

    try_recovery:
        // Attempt auto-recovery
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
            // Re-detect format and sleep time from new device
            detectFormat(isFloat, bitsPerSample);
            sleepMs = calcSleepMs();
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
