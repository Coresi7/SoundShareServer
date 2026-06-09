# SoundShare

A lightweight Windows solution for streaming PC system audio over the local network in real time.

- **SoundShareServer** — Captures system audio and streams it via WebSocket. Listeners can use a web browser (no plugins) or the optional native client below.
- **SoundShareClient** — A minimal Windows console app that connects with `IP` and `port` only. No browser, HTML, or AudioWorklet required.

Inspired by https://github.com/RegameDesk/sound_share, but implemented back end by myself (They didn't provide source code of the backend, use at your own risk).

## Features

- **System Audio Capture** — Uses WASAPI Loopback to capture all audio playing on the PC
- **Browser Playback** — Any device on the LAN can listen via a standard web browser (no plugins needed)
- **Low Latency** — Raw PCM Int16 streaming over WebSocket for minimal delay
- **Studio-Grade Audio** — Lanczos sinc resampling + TPDF dithering for transparent, artifact-free sound
- **Auto Reconnect** — Browser client automatically reconnects and resumes playback on disconnection
- **Multi-Client** — Supports multiple simultaneous listeners
- **Dual Protocol** — Supports both HTTP and HTTPS (with auto-generated self-signed certificate)
- **Zero Config** — Just run the executable and open the URL in a browser
- **Native Client** — Optional `SoundShareClient` for Windows listeners who want a lightweight console player

## Architecture

```
┌─────────────────┐     WebSocket (PCM Int16)      ┌────────────────────────────┐
│  Windows PC     │ ──────────────────────────────>│  Listener (choose one):    │
│                 │                                │  • Web Browser + Web Page  │
│  WASAPI Loopback│     HTTP (Web Page, optional)  │  • SoundShareClient (Win)  │
│  → Downmix      │ ──────────────────────────────>│     → Speaker              │
│  → Resample     │                                │                            │
│  → PCM Int16    │                                │                            │
└─────────────────┘                                └────────────────────────────┘
```

Server and browser client use the same WebSocket protocol. `SoundShareClient` reuses that protocol directly and plays audio through WASAPI on the default output device.

## Quick Start

### Pre-built Binary

1. Download `SoundShareServer.exe` from [Releases](../../releases)
2. Run it — By default, will launch with HTTP port **8080** and HTTPS port **8443**
3. In another device, use web browser to visit `http://<server-ip>:8080/` 或 `https://<server-ip>:8443/`
4. Click **Start** to begin listening

### Command Line Options

```
SoundShareServer.exe [http_port] [https_port]
```

| Example | Description |
|---------|-------------|
| `SoundShareServer.exe` | HTTP 8080 + HTTPS 8443（Default） |
| `SoundShareServer.exe 9090` | HTTP 9090 + HTTPS 8443 |
| `SoundShareServer.exe 8080 0` | ONLY HTTP 8080（Disable HTTPS） |
| `SoundShareServer.exe 0 8443` | ONLY HTTPS 8443（Disable HTTP） |

> **Note:** HTTPS mode auto-generates a self-signed certificate. You will need to accept the browser security warning on first visit.

## SoundShareClient (Native Windows Listener)

Use this when you want to listen on another Windows PC without opening a browser.

### What It Does

- Connects to `ws://<server-ip>:<port>/ws`
- Automatically sends `{"type":"start"}` and begins playback
- Receives `audio_config` and streams PCM Int16 audio to the default output device
- Press **Ctrl+C** to stop; the client sends `{"type":"stop"}` before exiting

### Quick Start

1. Start `SoundShareServer` on the PC that is sharing audio
2. On another Windows machine, run:

```text
SoundShareClient <server-ip> [port]
```

| Example | Description |
|---------|-------------|
| `SoundShareClient 192.168.1.10` | Connect to server at `192.168.1.10:8080` |
| `SoundShareClient 192.168.1.10 9090` | Connect using HTTP port `9090` |

> **Note:** `SoundShareClient` currently supports plain `ws://` only. Use the server's HTTP port (default **8080**). HTTPS / `wss://` is not supported by the native client yet.

### Exit Codes

| Code | Meaning |
|------|---------|
| `0` | Normal exit (Ctrl+C, stop sent) |
| `1` | Invalid command-line arguments |
| `2` | Connection failed |
| `3` | Protocol or audio playback error |

### Client vs Browser

| | Browser | SoundShareClient |
|---|---------|------------------|
| Platform | Any device with a browser | Windows only |
| Setup | Open URL, click Start | One command line |
| HTTPS | Supported | Not supported (`ws://` only) |
| Dependencies | Web Audio API | None (standalone EXE) |

Both can connect to the same server at the same time.

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
3. Build the solution (produces both `SoundShareServer.exe` and `SoundShareClient.exe`)

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
├── SoundShareServer/
│   ├── main.cpp                # Entry point, audio pipeline orchestration
│   ├── wasapi_capture.h/cpp    # WASAPI Loopback audio capture
│   ├── http_server.h/cpp       # HTTP + WebSocket server (Boost.Beast)
│   └── embedded_page.h         # Embedded HTML/JS client page
└── SoundShareClient/
    ├── main.cpp                # Client entry point
    ├── ws_client.h/cpp         # WebSocket client (Boost.Beast)
    ├── wasapi_render.h/cpp     # WASAPI playback to default output device
    └── ring_buffer.h           # PCM ring buffer for smooth playback
```

## How It Works

1. **Audio Capture**: WASAPI Loopback captures all system audio output (what you hear through speakers/headphones)
2. **Processing**: Audio is downmixed to stereo and resampled to 48kHz if necessary (using Lanczos windowed-sinc interpolation, 8 lobes)
3. **Encoding**: Samples are converted from float32 to PCM Int16 with TPDF dithering for optimal quantization quality
4. **Transport**: Raw PCM data is sent as binary WebSocket frames to all connected clients
5. **Playback**: Browser receives PCM data and plays it through an AudioWorklet with buffering for smooth playback; `SoundShareClient` plays the same PCM stream through WASAPI on Windows

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
