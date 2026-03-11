# SoundShare Server

A lightweight Windows console application that captures system audio output and streams it to any browser on the local network via WebSocket, enabling real-time audio sharing without installing any client software.  
Inspired by https://github.com/RegameDesk/sound_share, but implemented back end by myself (They didn't provide source code of the backend, use at your own risk). So this version does not depend on **ANY** apps except your web browser.  

## Features

- **System Audio Capture** — Uses WASAPI Loopback to capture all audio playing on the PC
- **Browser Playback** — Any device on the LAN can listen via a standard web browser (no plugins needed)
- **Low Latency** — Raw PCM Int16 streaming over WebSocket for minimal delay
- **Studio-Grade Audio** — Lanczos sinc resampling + TPDF dithering for transparent, artifact-free sound
- **Auto Reconnect** — Browser client automatically reconnects and resumes playback on disconnection
- **Multi-Client** — Supports multiple simultaneous listeners
- **Dual Protocol** — Supports both HTTP and HTTPS (with auto-generated self-signed certificate)
- **Zero Config** — Just run the executable and open the URL in a browser

## Architecture

```
┌─────────────────┐     WebSocket (PCM Int16)      ┌───────────────────────┐
│  Windows PC     │ ──────────────────────────────>│ Another PC/MAC/Phone's│
│                 │                                │      Web Browser      │
│  WASAPI Loopback│     HTTP (Web Page)            │     AudioWorklet      │
│  → Downmix      │ ──────────────────────────────>│     → Speaker         │
│  → Resample     │                                │                       │
│  → PCM Int16    │                                │                       │
└─────────────────┘                                └───────────────────────┘
```

## Quick Start

### Pre-built Binary

1. Download `SoundShareServer.exe` from [Releases](../../releases)
2. Run it — 默认启动 HTTP 端口 **8080** 和 HTTPS 端口 **8443**
3. 在另一台设备上，浏览器打开 `http://<server-ip>:8080/` 或 `https://<server-ip>:8443/`
4. Click **Start** to begin listening

### Command Line Options

```
SoundShareServer.exe [http_port] [https_port]
```

| Example | Description |
|---------|-------------|
| `SoundShareServer.exe` | HTTP 8080 + HTTPS 8443（默认） |
| `SoundShareServer.exe 9090` | HTTP 9090 + HTTPS 8443 |
| `SoundShareServer.exe 8080 0` | 仅 HTTP 8080（禁用 HTTPS） |
| `SoundShareServer.exe 0 8443` | 仅 HTTPS 8443（禁用 HTTP） |

> **Note:** HTTPS mode auto-generates a self-signed certificate. You will need to accept the browser security warning on first visit.

## Building from Source

### Prerequisites

- **Visual Studio 2022** (v143 toolset) or **ABOVE**
- **vcpkg** (with `VCPKG_ROOT` environment variable set)

### Install Dependencies

```bash
vcpkg install boost-beast boost-asio openssl --triplet x64-windows
```

### Build with Visual Studio

1. Open `SoundShareServer.slnx` in Visual Studio 2022 or above
2. Select **Release | x64** configuration
3. Build the solution

### Build with CMake

```bash
cmake --preset vcpkg
cmake --build --preset release
```

## Project Structure

```
SoundShareServer/
├── CMakeLists.txt              # CMake build configuration
├── CMakePresets.json           # CMake presets (vcpkg integration)
├── vcpkg.json                  # vcpkg dependency manifest
├── SoundShareServer.slnx       # Visual Studio solution
├── LICENSE.txt                 # MIT License
└── SoundShareServer/
    ├── main.cpp                # Entry point, audio pipeline orchestration
    ├── wasapi_capture.h/cpp    # WASAPI Loopback audio capture
    ├── http_server.h/cpp       # HTTP + WebSocket server (Boost.Beast)
    └── embedded_page.h         # Embedded HTML/JS client page
```

## How It Works

1. **Audio Capture**: WASAPI Loopback captures all system audio output (what you hear through speakers/headphones)
2. **Processing**: Audio is downmixed to stereo and resampled to 48kHz if necessary (using Lanczos windowed-sinc interpolation, 8 lobes)
3. **Encoding**: Samples are converted from float32 to PCM Int16 with TPDF dithering for optimal quantization quality
4. **Transport**: Raw PCM data is sent as binary WebSocket frames to all connected clients
5. **Playback**: Browser receives PCM data and plays it through an AudioWorklet with buffering for smooth playback

## Browser Compatibility

Tested and working on:
- Microsoft Edge
- Mozilla Firefox
- Google Chrome

> The client page uses the Web Audio API (`AudioContext` + `AudioWorklet`), which is supported by all modern browsers.

### Known issue for Safari
- You need to use HTTPS protocol to make this feature works properly. HTTP will fail.

## Network Requirements

- **Bandwidth**: ~192 KB/s per client (48kHz, stereo, 16-bit PCM)
- **Protocol**: WebSocket over HTTP or HTTPS
- **Firewall**: Ensure the chosen port is accessible on the LAN

## License

[MIT License](LICENSE.txt)
