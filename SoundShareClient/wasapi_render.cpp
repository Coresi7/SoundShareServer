#include "wasapi_render.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>

#include <iostream>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

WasapiRender::WasapiRender() {}

WasapiRender::~WasapiRender() {
    Stop();
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

bool WasapiRender::Start() {
    if (m_running.load()) return true;
    if (!m_ringBuffer) {
        m_lastError = "Ring buffer not set";
        return false;
    }

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

    while (!m_stopRequested.load()) {
        DWORD waitResult = WaitForSingleObject(event, 200);
        if (m_stopRequested.load()) break;
        if (waitResult != WAIT_OBJECT_0) continue;

        UINT32 padding = 0;
        hr = audioClient->GetCurrentPadding(&padding);
        if (FAILED(hr)) break;

        UINT32 availableFrames = m_bufferFrameCount - padding;
        if (availableFrames == 0) continue;

        BYTE* data = nullptr;
        hr = renderClient->GetBuffer(availableFrames, &data);
        if (FAILED(hr)) break;

        size_t samplesNeeded = static_cast<size_t>(availableFrames) * channels;
        size_t got = m_ringBuffer->Pop(temp.data(), samplesNeeded);

        if (got < samplesNeeded) {
            std::memset(temp.data() + got, 0, (samplesNeeded - got) * sizeof(int16_t));
        }
        std::memcpy(data, temp.data(), samplesNeeded * sizeof(int16_t));

        hr = renderClient->ReleaseBuffer(availableFrames, 0);
        if (FAILED(hr)) break;
    }

    audioClient->Stop();
    if (taskHandle) AvRevertMmThreadCharacteristics(taskHandle);
    m_running.store(false);
}
