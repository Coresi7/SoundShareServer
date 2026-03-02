#include "wasapi_capture.h"
#include <iostream>
#include <cmath>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

WasapiCapture::WasapiCapture() {}

WasapiCapture::~WasapiCapture() {
    Stop();

    if (m_waveFormat) {
        CoTaskMemFree(m_waveFormat);
        m_waveFormat = nullptr;
    }
    if (m_captureClient) {
        m_captureClient->Release();
        m_captureClient = nullptr;
    }
    if (m_audioClient) {
        m_audioClient->Release();
        m_audioClient = nullptr;
    }
    if (m_device) {
        m_device->Release();
        m_device = nullptr;
    }
    if (m_enumerator) {
        m_enumerator->Release();
        m_enumerator = nullptr;
    }
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

void WasapiCapture::CaptureThread() {
    // The mix format from WASAPI is typically IEEE float 32-bit
    // WAVEFORMATEXTENSIBLE with SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
    bool isFloat = false;
    int bitsPerSample = m_waveFormat->wBitsPerSample;

    if (m_waveFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        isFloat = true;
    } else if (m_waveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        WAVEFORMATEXTENSIBLE* wfex = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(m_waveFormat);
        if (IsEqualGUID(wfex->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            isFloat = true;
        }
    }

    REFERENCE_TIME hnsActualDuration = (REFERENCE_TIME)((double)m_bufferFrameCount * 10000000.0 / m_sampleRate);
    DWORD sleepMs = (DWORD)(hnsActualDuration / 10000 / 2);
    if (sleepMs < 1) sleepMs = 1;
    if (sleepMs > 10) sleepMs = 10;

    std::vector<float> convertBuffer;

    while (m_capturing.load()) {
        Sleep(sleepMs);

        UINT32 packetLength = 0;
        HRESULT hr = m_captureClient->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) break;

        while (packetLength > 0) {
            BYTE* data = nullptr;
            UINT32 numFramesAvailable = 0;
            DWORD flags = 0;

            hr = m_captureClient->GetBuffer(&data, &numFramesAvailable, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;

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
            if (FAILED(hr)) break;

            hr = m_captureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) break;
        }
    }
}
