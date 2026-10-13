#pragma once

// Embedded HTML page served to browsers
// Receives raw PCM Int16 audio over WebSocket and plays via AudioContext
// This approach has maximum browser compatibility (raw PCM over WebSocket)
//
// NOTE (T-GPU-AUDIO-GLITCH F4): this page is served as TWO adjacent raw
// string literals concatenated by the compiler. The F4 state-machine change
// pushed the single literal past the MSVC raw-string limit (~16KB, error
// C2026 "string too big"); the split below is byte-identical to a single
// literal (adjacent literals concatenate with no inserted characters).

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
        /* C1 (FR-09): stall state */
        .status.stalled { background: #4a3a00; color: #ffcc4e; }
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
        this.bufferSize = 48000 * 2 * 2; // 2 seconds worth of samples (interleaved) -- capacityCap, DO NOT raise
        this.ringBuffer = new Float32Array(this.bufferSize);
        this.writePos = 0;
        this.readPos = 0;
        this.count = 0; // number of samples available

        // ---- T-GPU-AUDIO-GLITCH F4 (FR-04): watermark + soft-underrun state machine ----
        // Units: interleaved samples (same scale as this.count). Single parameter
        // family (NFR-LAT-2): start 80ms / resume 60ms (0.75x start, hysteresis) /
        // fade 5ms (240 frames per channel). 2s capacity above stays untouched.
        this.startSamples = 48000 * 0.080 * 2;   // 7680  (startWaterLevel)
        this.resumeSamples = 48000 * 0.060 * 2;  // 5760  (resumeWaterLevel = 0.75 x start)
        this.fadeSamples = 48000 * 0.005 * 2;    // 480   (5ms; envelope step = 1/240 per frame)
        this.state = 'PREFILL';
        this.env = 0;                 // 0..1 envelope, per-frame ramped (m3)
        this.underrunCount = 0;       // U: number of entries into SOFT_UNDERRUN
        this.availMin = Infinity;     // m5: updated only outside PREFILL
        this.lastL = 0; this.lastR = 0;   // m3: updated ONLY in the normal-read branch
        this.everStarted = false;     // after first start, resume at resumeSamples
        this.statsTick = 0;

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
                // F4: reset the state machine on clear (stop/reconnect); keep
                // underrunCount/availMin as session-window observations.
                this.state = 'PREFILL';
                this.env = 0;
            }
        };
    }

    process(inputs, outputs, parameters) {
        const output = outputs[0];
        const numChannels = output.length; // typically 2
        const frameCount = output[0].length; // typically 128
        const need = frameCount * 2; // interleaved stereo samples needed
        const avail = this.count;
        // m3: per-FRAME envelope step = 1 / (fadeSamples / 2) = 1/240 (5ms @48k)
        const step = 1 / (this.fadeSamples / 2);

        if (this.state === 'PREFILL') {
            const w = this.everStarted ? this.resumeSamples : this.startSamples;
            if (avail < w) {
                // Not started yet: output silence (this is "not begun", not a hard cut)
                for (let ch = 0; ch < numChannels; ch++) output[ch].fill(0);
                this.env = 0;
                return true;
            }
            this.state = 'PLAYING';
            this.everStarted = true;
        }

        let normal = false;
        if (avail >= need) {
            // Normal read -- the ONLY branch that updates lastL/lastR (m3)
            for (let i = 0; i < frameCount; i++) {
                const l = this.ringBuffer[this.readPos];
                this.readPos = (this.readPos + 1) % this.bufferSize;
                const r = this.ringBuffer[this.readPos];
                this.readPos = (this.readPos + 1) % this.bufferSize;
                if (numChannels >= 2) { output[0][i] = l; output[1][i] = r; }
                else { output[0][i] = (l + r) * 0.5; }
                this.lastL = l; this.lastR = r;
            }
            this.count -= need;
            if (this.state === 'PLAYING') normal = true;
        } else {
            // Soft underrun: HOLD the last samples -- NO fill(0) hard cut (FR-04).
            // lastL/lastR are NOT updated here (updating would degrade
            // "hold last" into "hold 0" since env -> 0, m3).
            if (this.state === 'PLAYING') {
                this.state = 'SOFT_UNDERRUN';
                this.underrunCount++;
            }
            for (let i = 0; i < frameCount; i++) {
                if (numChannels >= 2) { output[0][i] = this.lastL; output[1][i] = this.lastR; }
                else { output[0][i] = (this.lastL + this.lastR) * 0.5; }
            }
        }

        // *** P0-1: SOFT_UNDERRUN -> PREFILL migration MUST live OUTSIDE the
        // avail<need else-branch, evaluated unconditionally after the branch
        // (mirrors the native side). Inside the else it would require
        // avail >= resumeSamples while avail < need(256) -- unreachable, and
        // one underrun would silence the player forever. ***
        if (this.state === 'SOFT_UNDERRUN' && this.env <= 0 && avail >= this.resumeSamples) {
            this.state = 'PREFILL';
        }

        // m3: per-frame linear envelope ramp within the block (a single
        // block-wide gain would leave 0.53x-magnitude steps every 2.67ms)
        const target = normal ? 1 : 0;
        for (let i = 0; i < frameCount; i++) {
            this.env = (target > this.env) ? Math.min(target, this.env + step)
                                           : Math.max(target, this.env - step);
            output[0][i] *= this.env;
            if (numChannels > 1) output[1][i] *= this.env;
        }

        // m5: availability minimum only outside PREFILL (a 0 during prefill
        // would pin the floor to 0 forever)
        if (this.state !== 'PREFILL') {
            this.availMin = Math.min(this.availMin, this.count);
        }

        // F4 observability (AC-FR04): stats message every 512 process() calls
        // (~1.4s at the 128-frame quantum)
        if ((++this.statsTick & 511) === 0) {
            this.port.postMessage({ type: 'stats',
                                    underrunCount: this.underrunCount,
                                    availMin: this.availMin,
                                    availNow: this.count,
                                    state: this.state });
        }
        return true;
    }
}
registerProcessor('pcm-player-processor', PCMPlayerProcessor);
`;

)HTML"
R"HTML(
let ws = null;
let audioCtx = null;
let gainNode = null;
let workletNode = null;
let isStreaming = false;
let wasStreaming = false;  // Track if we were streaming before disconnect (for auto-resume)
let sampleRate = 48000;
let channels = 2;
/* C2 (FR-09 / SS5.11): browser stall watchdog state */
let lastPcmAt = 0;
let pcmArmed = false;
let stallTimer = null;
const BROWSER_STALL_MS = 15000;   // T_browser_stall (SS11-1)
/* T-GPU-AUDIO-GLITCH F4 (FR-04): player stats (underrunCount / availMin) --
   filled from worklet stats messages; readable via window.__playerStats,
   #streamInfo text and the throttled console.log (AC-FR04 observability). */
let fmtText = '';
let lastStatsLogAt = 0;

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
                    fmtText = 'Format: ' + sampleRate + ' Hz, ' + channels + 'ch, PCM16';
                    document.getElementById('streamInfo').textContent = fmtText;
                }
            } catch(e) {}
        } else if (evt.data instanceof ArrayBuffer && isStreaming) {
            // C3 (B-1 arming): arm the watchdog only after the FIRST PCM frame
            lastPcmAt = performance.now();
            if (!pcmArmed) {
                pcmArmed = true;
                startStallWatchdog();
            }
            handlePCMData(evt.data);
        }
    };
}

/* ---- C5 (FR-09): stall watchdog ---- */
function startStallWatchdog() {
    if (stallTimer !== null) clearInterval(stallTimer);
    stallTimer = setInterval(checkStall, 1000);
}

function checkStall() {
    // Judgement: streaming + armed + no PCM for BROWSER_STALL_MS (SS5.11)
    if (!(isStreaming && pcmArmed)) return;
    if (performance.now() - lastPcmAt < BROWSER_STALL_MS) return;
    handleStall();
}

/* ---- C6: stall action -> reuse existing onclose reconnect loop (D-08) ---- */
function handleStall() {
    // 1. one error line (E-BROWSER-STALL, SS6.4)
    console.error('[WS] audio stalled: no PCM for ' +
                  Math.round(performance.now() - lastPcmAt) + 'ms, reconnecting');
    // 2. UI
    const status = document.getElementById('status');
    status.className = 'status stalled';
    status.textContent = 'Audio stalled. Reconnecting...';
    // 3. disarm
    isStreaming = false;
    wasStreaming = true;
    pcmArmed = false;
    if (stallTimer !== null) { clearInterval(stallTimer); stallTimer = null; }
    // 4. reconnect via existing onclose -> setTimeout(connect,1000) -> onopen auto-start
    try {
        ws.close();
    } catch (err) {
        setTimeout(connect, 1000);
    }
}

function handlePCMData(arrayBuffer) {
    if (!workletNode) return;

    // C9 (M-10): every-500-packet console.log removed (high-frequency noise).

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

    // F4 test hook (02-design.md §3.4.4 / V-17): ?fallback=1 forces the
    // ScriptProcessorNode fallback path (regression smoke for N8).
    const forceFallback = new URLSearchParams(location.search).get('fallback') === '1';

    try {
        if (forceFallback) throw new Error('fallback=1 forced (test hook)');

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

        // F4 (FR-04 observability): worklet stats handler (registered only when
        // workletNode exists; the ScriptProcessorNode fallback never reaches here)
        workletNode.port.onmessage = (e) => {
            if (!e.data || e.data.type !== 'stats') return;
            window.__playerStats = e.data;
            const bufMs = (sampleRate && channels)
                ? Math.round(e.data.availNow / (sampleRate * channels) * 1000) : 0;
            const info = document.getElementById('streamInfo');
            if (info) {
                info.textContent = (fmtText ? fmtText + ' | ' : '') +
                    'Underruns: ' + e.data.underrunCount +
                    ' | buf: ' + bufMs + ' ms' +
                    ' | state: ' + e.data.state;
            }
            const now = performance.now();
            if (now - lastStatsLogAt >= 5000) {   // 5s throttled console log
                console.log('[Audio][STATS]', e.data);
                lastStatsLogAt = now;
            }
        };

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

    isStreaming = true;
    ws.send(JSON.stringify({ type: 'start' }));
    lastPcmAt = performance.now();   // C4: reset baseline; NOT armed yet (B-1)

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
        // C10 (M-10): every-500-packet console.log removed (high-frequency noise).
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
    // C8 (FR-09): disarm stall watchdog
    pcmArmed = false;
    if (stallTimer !== null) { clearInterval(stallTimer); stallTimer = null; }

    if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ type: 'stop' }));
    }
    if (workletNode) {
        // F4: clear the stats handler and the stats themselves (no residue)
        workletNode.port.onmessage = null;
        try { delete window.__playerStats; } catch (err) { window.__playerStats = undefined; }
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

/* C7 (R-07): compensate background-tab setInterval throttling -- re-check
   immediately when the tab becomes visible again. */
document.addEventListener('visibilitychange', () => {
    if (!document.hidden) checkStall();
});

connect();
</script>
</body>
</html>
)HTML";
}
