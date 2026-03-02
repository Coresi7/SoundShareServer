#pragma once

// Embedded HTML page served to browsers
// Receives raw PCM Int16 audio over WebSocket and plays via AudioContext
// This approach has maximum browser compatibility (raw PCM over WebSocket)

inline const char* GetIndexHtml() {
    return R"HTML(<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>SoundShare</title>
    <style>
        body {
            font-family: Arial, sans-serif;
            background-color: #1a1a2e;
            color: #eee;
            display: flex;
            justify-content: center;
            align-items: center;
            min-height: 100vh;
            margin: 0;
        }
        .container {
            text-align: center;
            background: #16213e;
            padding: 40px;
            border-radius: 12px;
            box-shadow: 0 4px 20px rgba(0,0,0,0.3);
            max-width: 500px;
            width: 90%;
        }
        h1 { color: #e94560; margin-bottom: 10px; }
        .status {
            padding: 10px;
            margin: 15px 0;
            border-radius: 6px;
            font-size: 14px;
        }
        .status.connected { background: #0f3460; color: #4ecca3; }
        .status.disconnected { background: #3a0000; color: #e94560; }
        .status.streaming { background: #1a4a1a; color: #4ecca3; }
        button {
            padding: 12px 32px;
            font-size: 18px;
            background-color: #e94560;
            color: #fff;
            border: none;
            border-radius: 6px;
            cursor: pointer;
            margin: 5px;
            transition: background-color 0.3s;
        }
        button:hover { background-color: #c73e54; }
        button:disabled { background-color: #555; cursor: not-allowed; }
        .info { font-size: 12px; color: #888; margin-top: 20px; }
        .volume-control { margin-top: 15px; }
        .volume-control input[type=range] { width: 200px; }
    </style>
</head>
<body>
    <div class="container">
        <h1>&#127925; SoundShare</h1>
        <p>Stream PC audio to your browser</p>
        <div id="status" class="status disconnected">Connecting...</div>
        <div>
            <button id="startBtn" disabled onclick="startAudio()">&#9654; Start</button>
            <button id="stopBtn" disabled onclick="stopAudio()">&#9632; Stop</button>
        </div>
        <div class="volume-control">
            <label>Volume: <span id="volLabel">100%</span></label><br>
            <input type="range" id="volume" min="0" max="100" value="100" oninput="setVolume(this.value)">
        </div>
        <div class="info">
            <span id="streamInfo"></span>
        </div>
    </div>

<script>
// ---- Audio Worklet processor code (inline as a Blob URL) ----
const workletCode = `
class PCMPlayerProcessor extends AudioWorkletProcessor {
    constructor() {
        super();
        // Ring buffer: 48000 * 2ch * 4bytes * 2sec = ~768KB, plenty of room
        this.bufferSize = 48000 * 2 * 2; // 2 seconds worth of samples (interleaved)
        this.ringBuffer = new Float32Array(this.bufferSize);
        this.writePos = 0;
        this.readPos = 0;
        this.count = 0; // number of samples available

        this.port.onmessage = (e) => {
            if (e.data.type === 'audio') {
                // e.data.samples is Float32Array, interleaved stereo
                const samples = e.data.samples;
                const len = samples.length;
                for (let i = 0; i < len; i++) {
                    this.ringBuffer[this.writePos] = samples[i];
                    this.writePos = (this.writePos + 1) % this.bufferSize;
                }
                this.count += len;
                if (this.count > this.bufferSize) {
                    // Overflow: discard oldest data
                    this.count = this.bufferSize;
                    this.readPos = this.writePos;
                }
            } else if (e.data.type === 'clear') {
                this.writePos = 0;
                this.readPos = 0;
                this.count = 0;
            }
        };
    }

    process(inputs, outputs, parameters) {
        const output = outputs[0];
        const numChannels = output.length; // typically 2
        const frameCount = output[0].length; // typically 128
        const needed = frameCount * 2; // interleaved stereo samples needed

        if (this.count >= needed) {
            for (let i = 0; i < frameCount; i++) {
                // Read interleaved L, R
                const l = this.ringBuffer[this.readPos];
                this.readPos = (this.readPos + 1) % this.bufferSize;
                const r = this.ringBuffer[this.readPos];
                this.readPos = (this.readPos + 1) % this.bufferSize;

                if (numChannels >= 2) {
                    output[0][i] = l;
                    output[1][i] = r;
                } else {
                    output[0][i] = (l + r) * 0.5;
                }
            }
            this.count -= needed;
        } else {
            // Underrun: output silence
            for (let ch = 0; ch < numChannels; ch++) {
                output[ch].fill(0);
            }
        }
        return true;
    }
}
registerProcessor('pcm-player-processor', PCMPlayerProcessor);
`;

let ws = null;
let audioCtx = null;
let gainNode = null;
let workletNode = null;
let isStreaming = false;
let wasStreaming = false;  // Track if we were streaming before disconnect (for auto-resume)
let sampleRate = 48000;
let channels = 2;
let packetCount = 0;

function connect() {
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    const wsUrl = proto + '//' + location.host + '/ws';
    ws = new WebSocket(wsUrl);
    ws.binaryType = 'arraybuffer';

    ws.onopen = () => {
        document.getElementById('startBtn').disabled = false;
        // Auto-resume if we were streaming before disconnect
        if (wasStreaming && audioCtx && (workletNode || scriptNode)) {
            console.log('[WS] Reconnected, auto-resuming stream...');
            isStreaming = true;
            packetCount = 0;
            // Clear stale audio data in worklet buffer
            if (workletNode) {
                workletNode.port.postMessage({ type: 'clear' });
            }
            if (scriptNode) {
                fallbackBuffer = [];
            }
            ws.send(JSON.stringify({ type: 'start' }));
            document.getElementById('startBtn').disabled = true;
            document.getElementById('stopBtn').disabled = false;
            document.getElementById('status').className = 'status streaming';
            document.getElementById('status').textContent = 'Streaming audio... (reconnected)';
        } else {
            document.getElementById('status').className = 'status connected';
            document.getElementById('status').textContent = 'Connected. Click Start to play audio.';
        }
    };

    ws.onclose = () => {
        document.getElementById('status').className = 'status disconnected';
        document.getElementById('status').textContent = 'Disconnected. Reconnecting...';
        document.getElementById('startBtn').disabled = true;
        document.getElementById('stopBtn').disabled = true;
        // Remember streaming state for auto-resume after reconnect
        if (isStreaming) {
            wasStreaming = true;
        }
        isStreaming = false;
        setTimeout(connect, 1000);
    };

    ws.onerror = () => {
        ws.close();
    };

    ws.onmessage = (evt) => {
        if (typeof evt.data === 'string') {
            try {
                const msg = JSON.parse(evt.data);
                if (msg.type === 'audio_config') {
                    sampleRate = msg.sampleRate || 48000;
                    channels = msg.channels || 2;
                    console.log('[Config] sampleRate=' + sampleRate + ' channels=' + channels);
                    document.getElementById('streamInfo').textContent =
                        'Format: ' + sampleRate + ' Hz, ' + channels + 'ch, PCM16';
                }
            } catch(e) {}
        } else if (evt.data instanceof ArrayBuffer && isStreaming) {
            handlePCMData(evt.data);
        }
    };
}

function handlePCMData(arrayBuffer) {
    if (!workletNode) return;

    packetCount++;
    if (packetCount % 500 === 1) {
        console.log('[WS] PCM packet #' + packetCount + ' bytes=' + arrayBuffer.byteLength);
    }

    // Data is Int16 interleaved PCM from server
    // Convert Int16 -> Float32 for AudioWorklet
    const int16 = new Int16Array(arrayBuffer);
    const float32 = new Float32Array(int16.length);
    for (let i = 0; i < int16.length; i++) {
        float32[i] = int16[i] / 32768.0;
    }

    // Send to AudioWorklet ring buffer
    workletNode.port.postMessage({ type: 'audio', samples: float32 }, [float32.buffer]);
}

async function startAudio() {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;

    try {
        audioCtx = new AudioContext({ sampleRate: sampleRate });
        if (audioCtx.state === 'suspended') {
            await audioCtx.resume();
        }
        console.log('[Audio] AudioContext state=' + audioCtx.state + ' sampleRate=' + audioCtx.sampleRate);

        gainNode = audioCtx.createGain();
        gainNode.connect(audioCtx.destination);
        setVolume(document.getElementById('volume').value);

        // Register AudioWorklet from Blob URL
        const blob = new Blob([workletCode], { type: 'application/javascript' });
        const url = URL.createObjectURL(blob);
        await audioCtx.audioWorklet.addModule(url);
        URL.revokeObjectURL(url);

        workletNode = new AudioWorkletNode(audioCtx, 'pcm-player-processor', {
            outputChannelCount: [2],
            numberOfOutputs: 1,
        });
        workletNode.connect(gainNode);

        console.log('[Audio] AudioWorklet initialized');
    } catch(e) {
        console.error('[Audio] Failed to init AudioWorklet:', e);
        // Fallback: use ScriptProcessorNode (deprecated but widely supported)
        if (!audioCtx) audioCtx = new AudioContext({ sampleRate: sampleRate });
        if (audioCtx.state === 'suspended') await audioCtx.resume();
        if (!gainNode) {
            gainNode = audioCtx.createGain();
            gainNode.connect(audioCtx.destination);
            setVolume(document.getElementById('volume').value);
        }
        initScriptProcessorFallback();
    }

    packetCount = 0;
    isStreaming = true;
    ws.send(JSON.stringify({ type: 'start' }));

    wasStreaming = true;  // Mark that user wants streaming
    document.getElementById('startBtn').disabled = true;
    document.getElementById('stopBtn').disabled = false;
    document.getElementById('status').className = 'status streaming';
    document.getElementById('status').textContent = 'Streaming audio...';

}

// ---- ScriptProcessorNode fallback for older browsers ----
let scriptNode = null;
let fallbackBuffer = [];
let fallbackLock = false;

function initScriptProcessorFallback() {
    console.log('[Audio] Using ScriptProcessorNode fallback');
    fallbackBuffer = [];
    // bufferSize=4096 gives ~85ms at 48kHz, good balance of latency vs stability
    scriptNode = audioCtx.createScriptProcessor(4096, 0, 2);
    scriptNode.onaudioprocess = (e) => {
        const outL = e.outputBuffer.getChannelData(0);
        const outR = e.outputBuffer.getChannelData(1);
        const frameCount = outL.length;
        const needed = frameCount * 2; // interleaved L,R

        if (fallbackBuffer.length >= needed) {
            for (let i = 0; i < frameCount; i++) {
                outL[i] = fallbackBuffer[i * 2];
                outR[i] = fallbackBuffer[i * 2 + 1];
            }
            fallbackBuffer.splice(0, needed);
        } else {
            outL.fill(0);
            outR.fill(0);
        }
    };
    scriptNode.connect(gainNode);

    // Override handlePCMData for fallback mode
    window._originalHandlePCM = handlePCMData;
    handlePCMData = function(arrayBuffer) {
        packetCount++;
        if (packetCount % 500 === 1) {
            console.log('[WS-FB] PCM packet #' + packetCount + ' bytes=' + arrayBuffer.byteLength);
        }
        const int16 = new Int16Array(arrayBuffer);
        for (let i = 0; i < int16.length; i++) {
            fallbackBuffer.push(int16[i] / 32768.0);
        }
        // Prevent buffer from growing too large (keep ~2 seconds max)
        const maxLen = sampleRate * channels * 2;
        if (fallbackBuffer.length > maxLen) {
            fallbackBuffer.splice(0, fallbackBuffer.length - maxLen);
        }
    };
}

function stopAudio() {
    isStreaming = false;
    wasStreaming = false;  // User explicitly stopped, do not auto-resume

    if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ type: 'stop' }));
    }
    if (workletNode) {
        workletNode.port.postMessage({ type: 'clear' });
        workletNode.disconnect();
        workletNode = null;
    }
    if (scriptNode) {
        scriptNode.disconnect();
        scriptNode = null;
        if (window._originalHandlePCM) {
            handlePCMData = window._originalHandlePCM;
            delete window._originalHandlePCM;
        }
    }
    fallbackBuffer = [];
    if (audioCtx) {
        audioCtx.close();
        audioCtx = null;
        gainNode = null;
    }

    document.getElementById('startBtn').disabled = false;
    document.getElementById('stopBtn').disabled = true;
    document.getElementById('status').className = 'status connected';
    document.getElementById('status').textContent = 'Stopped. Click Start to resume.';
}

function setVolume(val) {
    document.getElementById('volLabel').textContent = val + '%';
    if (gainNode) {
        gainNode.gain.value = val / 100.0;
    }
}

connect();
</script>
</body>
</html>
)HTML";
}
