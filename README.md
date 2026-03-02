# SoundShare Server

A lightweight Windows console application that captures system audio output and streams it to any browser on the local network via WebSocket, enabling real-time audio sharing without installing any client software.  
Inspired by https://github.com/RegameDesk/sound_share, but implemented back end by myself (They didn't provide backend ). This version does not depend on ANY other apps.

## Features

- **System Audio Capture** — Uses WASAPI Loopback to capture all audio playing on the PC
- **Browser Playback** — Any device on the LAN can listen via a standard web browser (no plugins needed)
- **Low Latency** — Raw PCM Int16 streaming over WebSocket for minimal delay
- **Auto Reconnect** — Browser client automatically reconnects and resumes playback on disconnection
- **Multi-Client** — Supports multiple simultaneous listeners
- **Dual Protocol** — Supports both HTTP and HTTPS (with auto-generated self-signed certificate)
- **Zero Config** — Just run the executable and open the URL in a browser

## Architecture

```
┌─────────────────┐     WebSocket (PCM Int16)      ┌─────────────────┐
│  Windows PC     │ ──────────────────────────────>│  Browser        │
│                 │                                │                 │
│  WASAPI Loopback│     HTTP (Web Page)            │  AudioWorklet   │
│  → Downmix      │ ──────────────────────────────>│  → Speaker      │
│  → Resample     │                                │                 │
│  → PCM Int16    │                                │                 │
└─────────────────┘                                └─────────────────┘
```

## Quick Start

### Pre-built Binary

1. Download `SoundShareServer.exe` from [Releases](../../releases)
2. Run it — the server starts on HTTP port **8080** by default
3. On another device, open `http://<server-ip>:8080/` in a browser
4. Click **Start** to begin listening

### Command Line Options

```
SoundShareServer.exe [http_port] [https_port]
```

| Example | Description |
|---------|-------------|
| `SoundShareServer.exe` | HTTP on port 8080 (default) |
| `SoundShareServer.exe 9090` | HTTP on port 9090 |
| `SoundShareServer.exe 8080 8443` | HTTP on 8080 + HTTPS on 8443 |
| `SoundShareServer.exe 0 8443` | HTTPS only on 8443 |

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
2. **Processing**: Audio is downmixed to stereo and resampled to 48kHz if necessary
3. **Encoding**: Samples are converted from float32 to PCM Int16
4. **Transport**: Raw PCM data is sent as binary WebSocket frames to all connected clients
5. **Playback**: Browser receives PCM data and plays it through an AudioWorklet with buffering for smooth playback

## Browser Compatibility

Tested and working on:
- Microsoft Edge
- Mozilla Firefox
- Google Chrome

> The client page uses the Web Audio API (`AudioContext` + `AudioWorklet`), which is supported by all modern browsers.

## Network Requirements

- **Bandwidth**: ~192 KB/s per client (48kHz, stereo, 16-bit PCM)
- **Protocol**: WebSocket over HTTP or HTTPS
- **Firewall**: Ensure the chosen port is accessible on the LAN

## License

[MIT License](LICENSE.txt)
