/*
 * Project: tinySpeak - tinyWebSynth
 * Author: Geoff McIntyre (w/ help from Claude)
 * Revision Date: 2/18/26
 * License: GNU General Public License v3.0
 *
 * Description:
 *   Chiptune synthesizer: live piano, 2-track melodic step sequencer,
 *   3-track drum machine (Kick, Snare, Hi-Hat). Controlled via WebSocket
 *   web interface. Songs saved to browser localStorage.
 *
 *   Audio channels:
 *     0: Lead melody  (square/saw/pulse)
 *     1: Bass melody  (square/saw/pulse)
 *     2: Kick drum    (pitch-swept square)
 *     3: Snare drum   (LFSR noise)
 *     4: Hi-Hat       (LFSR noise, fast decay)
 *
 * Requirements:
 *   - Libraries: mathieucarbou/ESP Async WebServer, mathieucarbou/AsyncTCP,
 *                Preferences (built-in ESP32 core)
 *   - Hardware: tinyCore ESP32-S3 + tinySpeak HAT
 *
 * WebSocket Protocol (Browser → ESP32):
 *   PLAY:freq           Live note on (lead channel)
 *   STOP                Live note off
 *   WAVE:n              Set live waveform (0=square 1=saw 2=pulse)
 *   VOL:n               Set volume (0–30000)
 *   SEQ_SET:t,s,f,w     Set melody step: track, step, freq (0=rest), wave
 *   SEQ_DRUM:t,s,v      Set drum step: track, step, 1=on/0=off
 *   SEQ_BPM:n           Set BPM
 *   SEQ_PLAY            Start sequencer
 *   SEQ_STOP            Stop sequencer
 *   SEQ_CLEAR           Clear all patterns
 *
 * WebSocket Protocol (ESP32 → Browser):
 *   STEP:n              Current sequencer step (for grid highlight)
 *
 * Serial Menu:
 *   [i] Show IP    [+/-] Volume    [w] WiFi setup    [e] Export WAV    [?] Menu 
 */

#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <driver/i2s.h>
#include <math.h>
#include <SD.h>
#include <FS.h>

// ─── PINS ─────────────────────────────────────────────────────────────────────
#define I2S_DOUT  8
#define I2S_BCLK  9
#define I2S_LRC   10

// ─── AUDIO CONFIG ─────────────────────────────────────────────────────────────
#define SAMPLE_RATE  44100
#define BUF_SZ       64        // stereo pairs per i2s write
#define NUM_CH       5
#define NUM_STEPS    16
#define NUM_MEL_TRK  2         // Lead, Bass
#define NUM_DRM_TRK  3         // Kick, Snare, Hat

#define CH_LEAD  0
#define CH_BASS  1
#define CH_KICK  2
#define CH_SNARE 3
#define CH_HAT   4

// ─── AUDIO ENGINE ─────────────────────────────────────────────────────────────

struct Channel {
    float    phase    = 0;
    float    curFreq  = 440;
    int      waveType = 0;      // 0=square 1=saw 2=pulse(25%)
    float    envLvl   = 0;      // 0.0–1.0, multiplied by envDcy each sample
    float    envDcy   = 0.9998f;
    bool     active   = false;
    bool     isNoise  = false;
    float    pitchMul = 1.0f;   // per-sample freq multiplier (kick pitch sweep)
};

Channel ch[NUM_CH];
volatile int32_t gAmp = 10000; // global amplitude 0–30000
volatile int liveWave = 0; // waveform used for live piano notes

// 15-bit Galois LFSR — Game Boy-style noise generator
uint16_t nlfsr[2] = {0xACE1, 0x9182}; // separate state for snare vs hat
uint16_t lfsrNext(uint16_t s) { return (s >> 1) ^ ((s & 1) ? 0xB400 : 0); }

// ── Note triggers ─────────────────────────────────────────────────────────────

void trigNote(int c, float freq, int wave) {
    ch[c].phase    = 0;
    ch[c].curFreq  = freq;
    ch[c].waveType = wave;
    ch[c].envLvl   = 1.0f;
    ch[c].envDcy   = 0.9998f;
    ch[c].active   = true;
    ch[c].isNoise  = false;
    ch[c].pitchMul = 1.0f;
}

void releaseNote(int c) {
    ch[c].active = false;
    ch[c].envLvl = 0;
}

void trigKick() {
    // Square wave starting at 200 Hz, pitch drops rapidly to ~30 Hz
    ch[CH_KICK].phase    = 0;
    ch[CH_KICK].curFreq  = 200.0f;
    ch[CH_KICK].waveType = 0;       // square for punch
    ch[CH_KICK].envLvl   = 1.0f;
    ch[CH_KICK].envDcy   = 0.9997f; // amplitude fades over ~200ms
    ch[CH_KICK].active   = true;
    ch[CH_KICK].isNoise  = false;
    ch[CH_KICK].pitchMul = 0.9991f; // drops freq ~65% in 30ms
}

void trigSnare() {
    ch[CH_SNARE].envLvl  = 1.0f;
    ch[CH_SNARE].envDcy  = 0.9988f; // fades over ~100ms
    ch[CH_SNARE].active  = true;
    ch[CH_SNARE].isNoise = true;
    ch[CH_SNARE].pitchMul = 1.0f;
    nlfsr[0] = 0xACE1; // reset for consistent timbre
}

void trigHat() {
    ch[CH_HAT].envLvl  = 1.0f;
    ch[CH_HAT].envDcy  = 0.9970f; // fades very fast ~30ms
    ch[CH_HAT].active  = true;
    ch[CH_HAT].isNoise = true;
    ch[CH_HAT].pitchMul = 1.0f;
    nlfsr[1] = 0x9182;
}

// ── Per-sample synthesis ──────────────────────────────────────────────────────

int16_t synthSample(int c) {
    if (!ch[c].active || ch[c].envLvl < 0.001f) {
        ch[c].active = false;
        return 0;
    }

    // Each channel contributes gAmp/NUM_CH so summing all 5 can't clip
    int32_t amp = (int32_t)((float)gAmp / NUM_CH * ch[c].envLvl);
    int16_t val = 0;

    if (ch[c].isNoise) {
        int ni = (c == CH_SNARE) ? 0 : 1;
        nlfsr[ni] = lfsrNext(nlfsr[ni]);
        val = (nlfsr[ni] & 1) ? (int16_t)amp : (int16_t)-amp;
    } else {
        switch (ch[c].waveType) {
            case 0: // Square
                val = (ch[c].phase < PI) ? (int16_t)amp : (int16_t)-amp;
                break;
            case 1: // Saw
                val = (int16_t)((ch[c].phase / (2.0f * PI) * 2.0f - 1.0f) * amp);
                break;
            case 2: // Pulse 25%
                val = (ch[c].phase < PI * 0.5f) ? (int16_t)amp : (int16_t)-amp;
                break;
        }
        float phInc = (2.0f * PI * ch[c].curFreq) / SAMPLE_RATE;
        ch[c].phase += phInc;
        if (ch[c].phase >= 2.0f * PI) ch[c].phase -= 2.0f * PI;

        // Kick pitch sweep
        if (ch[c].pitchMul != 1.0f) {
            ch[c].curFreq *= ch[c].pitchMul;
            if (ch[c].curFreq < 30.0f) ch[c].curFreq = 30.0f;
        }
    }

    ch[c].envLvl *= ch[c].envDcy;
    return val;
}

// ─── SEQUENCER ────────────────────────────────────────────────────────────────

struct MelStep { float freq = 0; int wave = 0; }; // freq==0 means rest

MelStep  melPat[NUM_MEL_TRK][NUM_STEPS];  // zeroed by default = all rests
bool     drmPat[NUM_DRM_TRK][NUM_STEPS];  // zeroed by default = all off

volatile bool seqPlay = false;
volatile int  seqBPM  = 120;
int           seqStep = 0;
unsigned long lastStepMs = 0;

void trigSequencerStep(int s) {
    // Melody tracks
    for (int t = 0; t < NUM_MEL_TRK; t++) {
        int c = (t == 0) ? CH_LEAD : CH_BASS;
        if (melPat[t][s].freq > 0.0f)
            trigNote(c, melPat[t][s].freq, melPat[t][s].wave);
        else
            releaseNote(c);
    }
    // Drum tracks
    if (drmPat[0][s]) trigKick();
    if (drmPat[1][s]) trigSnare();
    if (drmPat[2][s]) trigHat();
}

// ─── WEB SERVER ───────────────────────────────────────────────────────────────

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// ─── WIFI + STORAGE ───────────────────────────────────────────────────────────

Preferences prefs;
String wSSID = "", wPass = "";

// ─── HTML (Flash / PROGMEM) ───────────────────────────────────────────────────

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<title>tinyWebSynth</title>
<style>
*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
:root{
  --bg:#08080b;--panel:#0f0f16;--border:#1c1c2a;
  --cyan:#22d3ee;--purple:#a855f7;--green:#4ade80;
  --orange:#f97316;--red:#f43f5e;--yellow:#fbbf24;
  --blue:#60a5fa;--text:#c0c0d0;--muted:#383848;
}
body{font-family:'Courier New',monospace;background:var(--bg);color:var(--text);
  min-height:100vh;display:flex;flex-direction:column;align-items:center;
  padding:20px 12px 48px;user-select:none;-webkit-user-select:none}
h1{font-size:1.9em;background:linear-gradient(90deg,var(--cyan),var(--purple));
  -webkit-background-clip:text;-webkit-text-fill-color:transparent;
  background-clip:text;letter-spacing:3px;margin-bottom:2px}
.sub{font-size:.68em;color:var(--muted);letter-spacing:.12em;margin-bottom:18px}
.section{width:100%;max-width:700px;margin-bottom:14px}
.sec-title{font-size:.7em;color:var(--muted);letter-spacing:.12em;
  text-transform:uppercase;margin-bottom:7px;padding-left:2px}

/* Visualizer */
#viz{width:100%;max-width:700px;height:72px;background:#050507;
  border:1px solid var(--border);border-radius:8px;margin-bottom:14px;display:block}

/* Control bar */
.cbar{display:flex;flex-wrap:wrap;gap:10px;align-items:center;justify-content:center;
  background:var(--panel);border:1px solid var(--border);border-radius:8px;
  padding:9px 14px;margin-bottom:12px}
.wg{display:flex;gap:5px}
.wb{padding:5px 13px;border-radius:99px;border:1px solid var(--border);
  background:var(--panel);color:var(--muted);font-size:.78em;cursor:pointer;font-family:inherit}
.wb.active{border-color:var(--purple);color:var(--purple);background:#16082a}
.vg{display:flex;align-items:center;gap:6px;font-size:.78em}
#volSlider{-webkit-appearance:none;appearance:none;width:100px;height:4px;
  border-radius:2px;background:var(--muted);outline:none;cursor:pointer}
#volSlider::-webkit-slider-thumb{-webkit-appearance:none;width:15px;height:15px;
  border-radius:50%;background:var(--purple);cursor:pointer}
#volSlider::-moz-range-thumb{width:15px;height:15px;border-radius:50%;
  background:var(--purple);border:none;cursor:pointer}
#volLabel{color:var(--cyan);min-width:22px;text-align:right;font-variant-numeric:tabular-nums}

/* Transport */
.transport{display:flex;gap:8px;align-items:center;
  background:var(--panel);border:1px solid var(--border);border-radius:8px;
  padding:9px 14px;margin-bottom:10px;flex-wrap:wrap;justify-content:center}
.play-btn{width:40px;height:40px;background:#0a2010;border:1px solid var(--green);
  border-radius:7px;color:var(--green);font-size:1.2em;cursor:pointer;
  display:flex;align-items:center;justify-content:center;transition:all .1s}
.play-btn.on{background:var(--green);color:#000}
.stop-btn{width:40px;height:40px;background:var(--panel);border:1px solid var(--border);
  border-radius:7px;color:var(--text);font-size:1.1em;cursor:pointer;
  display:flex;align-items:center;justify-content:center}
.bpm-in{width:50px;text-align:center;background:var(--bg);border:1px solid var(--border);
  color:var(--text);border-radius:4px;padding:4px;font-family:inherit;font-size:.9em;outline:none}
.clr-btn{padding:7px 13px;background:var(--panel);border:1px solid var(--border);
  border-radius:6px;color:var(--muted);font-family:inherit;font-size:.72em;cursor:pointer;letter-spacing:.05em}
.clr-btn:hover{border-color:var(--red);color:var(--red)}

/* Sequencer */
.sg{width:100%;overflow-x:auto}
.str{display:flex;align-items:center;gap:3px;margin-bottom:3px}
.tlbl{min-width:58px;font-size:.65em;color:var(--muted);text-align:right;padding-right:6px;letter-spacing:.04em}
.sb{flex:1;min-width:30px;height:30px;background:var(--panel);border:1px solid var(--border);
  border-radius:4px;cursor:pointer;font-family:inherit;font-size:.5em;color:var(--muted);
  display:flex;align-items:center;justify-content:center;transition:background .08s;
  white-space:nowrap;overflow:hidden;-webkit-tap-highlight-color:transparent}
.sb.m0{background:#122018;border-color:var(--green);color:var(--green)}
.sb.m1{background:#0c1820;border-color:var(--blue);color:var(--blue)}
.sb.d0{background:#201008;border-color:var(--orange);color:var(--orange)}
.sb.d1{background:#200810;border-color:var(--red);color:var(--red)}
.sb.d2{background:#1e1a06;border-color:var(--yellow);color:var(--yellow)}
.sb.pn{box-shadow:0 0 7px 1px currentColor}
.sdiv{border:none;border-top:1px solid var(--border);margin:6px 0}

/* Piano */
.piano-wrap{width:100%;max-width:700px;background:var(--panel);
  border:1px solid var(--border);border-radius:8px;padding:14px 14px 10px;margin-bottom:10px}
.piano-topbar{display:flex;align-items:center;justify-content:space-between;
  margin-bottom:10px;flex-wrap:wrap;gap:8px}
.note-status{font-size:.75em;color:var(--muted)}
.note-status em{color:var(--purple);font-style:normal;font-size:1.1em}
.rel-btn{padding:5px 13px;border-radius:6px;border:1px solid var(--border);
  background:var(--bg);color:var(--muted);font-family:inherit;font-size:.72em;cursor:pointer;
  transition:all .15s}
.rel-btn:hover,.rel-btn.lit{border-color:var(--cyan);color:var(--cyan)}
.pw{overflow-x:auto;overflow-y:hidden;padding-bottom:6px}
.piano{position:relative;display:flex;flex-shrink:0;height:160px}
.wk{width:48px;height:160px;background:linear-gradient(180deg,#d0d0d0,#fff);
  border:1px solid #888;border-radius:0 0 8px 8px;cursor:pointer;position:relative;
  display:flex;align-items:flex-end;justify-content:center;padding-bottom:7px;
  font-size:.58em;color:#777;transition:background .05s;flex-shrink:0;
  -webkit-tap-highlight-color:transparent;touch-action:manipulation}
.wk.active{background:linear-gradient(180deg,#9ab8ff,#6090ff) !important;
  border-color:#2a5acc;box-shadow:0 0 12px #6090ff60}
.bk{width:28px;height:100px;background:linear-gradient(180deg,#1a1a1a,#000);
  border:1px solid #000;border-top:none;border-radius:0 0 6px 6px;
  cursor:pointer;position:absolute;z-index:2;top:0;flex-shrink:0;
  display:flex;align-items:flex-end;justify-content:center;padding-bottom:5px;
  font-size:.5em;color:#333;-webkit-tap-highlight-color:transparent;touch-action:manipulation}
.bk.active{background:linear-gradient(180deg,#3a1a5a,#180838) !important;
  box-shadow:0 0 10px #a855f760}
.piano-hint{font-size:.62em;color:var(--muted);text-align:center;margin-top:8px;letter-spacing:.04em}

/* Songs collapsible */
.songs-toggle{width:100%;max-width:700px;display:flex;align-items:center;
  justify-content:space-between;background:var(--panel);border:1px solid var(--border);
  border-radius:8px;padding:10px 16px;cursor:pointer;font-size:.8em;
  color:var(--muted);letter-spacing:.08em;margin-bottom:4px;transition:all .15s}
.songs-toggle:hover{border-color:var(--purple);color:var(--purple)}
.songs-toggle .arr{transition:transform .2s}
.songs-toggle.open .arr{transform:rotate(180deg)}
.songs-panel{display:none;width:100%;max-width:700px;background:var(--panel);
  border:1px solid var(--border);border-top:none;border-radius:0 0 8px 8px;
  padding:14px;margin-bottom:14px}
.songs-panel.open{display:block}
.sname{width:100%;background:var(--bg);border:1px solid var(--border);color:var(--text);
  border-radius:6px;padding:8px 10px;font-family:inherit;font-size:.85em;outline:none;margin-bottom:8px}
.save-btn{width:100%;padding:9px;background:#16082a;border:1px solid var(--purple);
  border-radius:6px;color:var(--purple);font-family:inherit;font-size:.82em;cursor:pointer;margin-bottom:14px}
.si{display:flex;align-items:center;gap:6px;background:var(--bg);
  border:1px solid var(--border);border-radius:6px;padding:8px 10px;margin-bottom:5px;font-size:.8em}
.si-name{flex:1;color:var(--text)}
.si-bpm{color:var(--muted);font-size:.88em}
.smbtn{padding:4px 9px;border-radius:4px;font-family:inherit;font-size:.75em;
  cursor:pointer;border:1px solid var(--border);background:var(--panel);color:var(--muted)}
.smbtn.ld{border-color:var(--cyan);color:var(--cyan)}
.smbtn.dl{border-color:var(--red);color:var(--red)}
.hint{font-size:.67em;color:var(--muted);text-align:center;margin-top:7px;letter-spacing:.04em}
#songStatus{font-size:.67em;color:var(--muted);text-align:center;margin-top:7px;letter-spacing:.04em}
</style>
</head>
<body>
<h1>tinyWebSynth</h1>
<p class="sub">ESP32-S3 &middot; tinySpeak HAT</p>

<canvas id="viz"></canvas>

<!-- Controls: waveform + volume -->
<div class="cbar">
  <div class="wg" id="waveGroup">
    <button class="wb active" onclick="setWave(0)">&#9646; Square</button>
    <button class="wb"        onclick="setWave(1)">/ Saw</button>
    <button class="wb"        onclick="setWave(2)">&#8718; Pulse</button>
  </div>
  <div class="vg">
    <span>&#128264;</span>
    <input id="volSlider" type="range" min="0" max="30" value="10"
           oninput="setVol(this.value)">
    <span>&#128266;</span>
    <span id="volLabel">10</span>
  </div>
</div>

<!-- Sequencer -->
<div class="section">
  <div class="sec-title">&#127925; Sequencer</div>
  <div class="transport">
    <button class="play-btn" id="playBtn" onclick="togglePlay()">&#9654;</button>
    <button class="stop-btn" onclick="stopSeq()">&#9209;</button>
    <label style="font-size:.75em;color:var(--muted)">BPM</label>
    <input class="bpm-in" id="bpmIn" type="number" value="120" min="40" max="240"
           onchange="setBPM(this.value)">
    <button class="clr-btn" onclick="clearAll()">CLEAR ALL</button>
  </div>
  <div class="sg" id="seqGrid"></div>
  <p class="hint">Click melody cell to cycle notes &nbsp;&middot;&nbsp; Right-click to clear &nbsp;&middot;&nbsp; Waveform from selector above</p>
</div>

<!-- Piano — always visible below sequencer -->
<div class="piano-wrap">
  <div class="sec-title">&#127929; Piano</div>
  <div class="piano-topbar">
    <div class="note-status">Held: <em id="noteLabel">&#8212;</em></div>
    <!-- Release button: stops the sustained note -->
    <button class="rel-btn" id="relBtn" onclick="releaseHeld()">&#9646; Release</button>
  </div>
  <div class="pw"><div class="piano" id="piano"></div></div>
  <p class="piano-hint">Tap a key to sustain &nbsp;&middot;&nbsp; Tap again or press Release to stop &nbsp;&middot;&nbsp; Tap another key to switch</p>
</div>

<!-- Songs (collapsible) -->
<button class="songs-toggle" id="songsToggle" onclick="toggleSongs()">
  <span>&#128190; Songs</span>
  <span class="arr">&#9660;</span>
</button>
<div class="songs-panel" id="songsPanel">
  <input class="sname" id="songName" type="text" placeholder="Song name&#8230;" maxlength="32">
  <button class="save-btn" onclick="saveSong()">&#128190; Save Current Pattern</button>
  <div id="songList"></div>
  <p id="songStatus"></p>
</div>

<script>
// ── WebSocket ──────────────────────────────────────────────────────────────
let ws, connected = false;
function wsConnect() {
  ws = new WebSocket('ws://' + location.host + '/ws');
  ws.onopen    = () => { connected = true; };
  ws.onclose   = () => { connected = false; setTimeout(wsConnect, 1500); };
  ws.onerror   = () => ws.close();
  ws.onmessage = e => {
    if (e.data.startsWith('STEP:')) highlightStep(parseInt(e.data.slice(5)));
  };
}
wsConnect();
function send(m) { if (connected && ws.readyState === 1) ws.send(m); }

// ── State ──────────────────────────────────────────────────────────────────
let curWave = 0, seqRunning = false, curStep = -1;

// ── Songs toggle ───────────────────────────────────────────────────────────
function toggleSongs() {
  document.getElementById('songsToggle').classList.toggle('open');
  document.getElementById('songsPanel').classList.toggle('open');
}

// ── Piano — sustain mode ───────────────────────────────────────────────────
// heldKey: the DOM element of the currently sustained key (or null)
let heldKey  = null;
let heldFreq = 0;

function pressKey(el, freq, label) {
  if (heldKey === el) {
    // Same key tapped again → release
    releaseHeld();
    return;
  }
  // Switch to new key (or start fresh)
  if (heldKey) heldKey.classList.remove('active');
  heldKey  = el;
  heldFreq = freq;
  el.classList.add('active');
  document.getElementById('noteLabel').textContent = label;
  document.getElementById('relBtn').classList.add('lit');
  send('PLAY:' + freq);
}

function releaseHeld() {
  if (heldKey) {
    heldKey.classList.remove('active');
    heldKey  = null;
    heldFreq = 0;
  }
  document.getElementById('noteLabel').textContent = '\u2014';
  document.getElementById('relBtn').classList.remove('lit');
  send('STOP');
}

// Build piano — 3 octaves C3–C6
const WKS = [
  {f:131,l:'C3'},{f:147,l:'D3'},{f:165,l:'E3'},{f:175,l:'F3'},
  {f:196,l:'G3'},{f:220,l:'A3'},{f:247,l:'B3'},
  {f:262,l:'C4'},{f:294,l:'D4'},{f:330,l:'E4'},{f:349,l:'F4'},
  {f:392,l:'G4'},{f:440,l:'A4'},{f:494,l:'B4'},
  {f:523,l:'C5'},{f:587,l:'D5'},{f:659,l:'E5'},{f:698,l:'F5'},
  {f:784,l:'G5'},{f:880,l:'A5'},{f:988,l:'B5'},
  {f:1047,l:'C6'}
];
const BKS = [
  {f:139,l:'C#3',a:0},{f:156,l:'D#3',a:1},{f:185,l:'F#3',a:3},{f:208,l:'G#3',a:4},{f:233,l:'A#3',a:5},
  {f:277,l:'C#4',a:7},{f:311,l:'D#4',a:8},{f:370,l:'F#4',a:10},{f:415,l:'G#4',a:11},{f:466,l:'A#4',a:12},
  {f:554,l:'C#5',a:14},{f:622,l:'D#5',a:15},{f:740,l:'F#5',a:17},{f:831,l:'G#5',a:18},{f:932,l:'A#5',a:19},
];
const WW = 48, BW = 28;
const pianoEl = document.getElementById('piano');

WKS.forEach(n => {
  const k = document.createElement('div');
  k.className = 'wk'; k.textContent = n.l;
  // Touch: fire on touchstart only — no touchend/touchcancel (sustain)
  k.addEventListener('mousedown',  e => { e.preventDefault(); pressKey(k, n.f, n.l); });
  k.addEventListener('touchstart', e => { e.preventDefault(); pressKey(k, n.f, n.l); }, {passive:false});
  pianoEl.appendChild(k);
});
BKS.forEach(n => {
  const k = document.createElement('div');
  k.className = 'bk'; k.textContent = n.l;
  k.style.left = ((n.a + 1) * WW - BW / 2) + 'px';
  k.addEventListener('mousedown',  e => { e.preventDefault(); pressKey(k, n.f, n.l); });
  k.addEventListener('touchstart', e => { e.preventDefault(); pressKey(k, n.f, n.l); }, {passive:false});
  pianoEl.appendChild(k);
});
document.addEventListener('contextmenu', e => e.preventDefault());

// ── Controls ───────────────────────────────────────────────────────────────
function setWave(w) {
  curWave = w;
  document.querySelectorAll('.wb').forEach((b, i) => b.classList.toggle('active', i === w));
  send('WAVE:' + w);
}
function setVol(v) {
  document.getElementById('volLabel').textContent = v;
  send('VOL:' + (v * 1000));
}

// ── Sequencer ──────────────────────────────────────────────────────────────
const NOTES = [
  null,
  {n:'C3',f:131},{n:'D3',f:147},{n:'E3',f:165},{n:'F3',f:175},{n:'G3',f:196},{n:'A3',f:220},{n:'B3',f:247},
  {n:'C4',f:262},{n:'D4',f:294},{n:'E4',f:330},{n:'F4',f:349},{n:'G4',f:392},{n:'A4',f:440},{n:'B4',f:494},
  {n:'C5',f:523},{n:'D5',f:587},{n:'E5',f:659},{n:'F5',f:698},{n:'G5',f:784},{n:'A5',f:880},{n:'B5',f:988},
  {n:'C6',f:1047}
];
const melPat = [
  Array.from({length:16}, () => ({ni:0, wave:0})),
  Array.from({length:16}, () => ({ni:0, wave:1})),
];
const drmPat = [Array(16).fill(false), Array(16).fill(false), Array(16).fill(false)];
const TRACKS = [
  {type:'m', ti:0, lbl:'LEAD',   cls:'m0'},
  {type:'m', ti:1, lbl:'BASS',   cls:'m1'},
  {type:'d', ti:0, lbl:'KICK',   cls:'d0'},
  {type:'d', ti:1, lbl:'SNARE',  cls:'d1'},
  {type:'d', ti:2, lbl:'HI-HAT', cls:'d2'},
];
const sBtns = [];

function buildGrid() {
  const grid = document.getElementById('seqGrid');
  grid.innerHTML = '';
  TRACKS.forEach((td, row) => {
    if (row === 2) { const hr = document.createElement('hr'); hr.className = 'sdiv'; grid.appendChild(hr); }
    const tr = document.createElement('div'); tr.className = 'str';
    const lbl = document.createElement('div'); lbl.className = 'tlbl'; lbl.textContent = td.lbl;
    tr.appendChild(lbl);
    sBtns[row] = [];
    for (let s = 0; s < 16; s++) {
      const btn = document.createElement('div'); btn.className = 'sb';
      sBtns[row][s] = btn;
      if (td.type === 'm') {
        btn.addEventListener('click',       () => cycleMel(row, s, td));
        btn.addEventListener('contextmenu', e  => { e.preventDefault(); clearMel(row, s, td); });
        renderMelBtn(btn, melPat[td.ti][s], td.cls);
      } else {
        btn.addEventListener('click',       () => toggleDrm(row, s, td));
        btn.addEventListener('contextmenu', e  => e.preventDefault());
        renderDrmBtn(btn, drmPat[td.ti][s], td.cls);
      }
      tr.appendChild(btn);
    }
    grid.appendChild(tr);
  });
}

function renderMelBtn(btn, step, cls) {
  const n = NOTES[step.ni];
  btn.className = 'sb' + (n ? ' ' + cls : '');
  btn.textContent = n ? n.n : '';
}
function renderDrmBtn(btn, on, cls) {
  btn.className = 'sb' + (on ? ' ' + cls : '');
  btn.textContent = on ? '\u25CF' : '';
}
function cycleMel(row, s, td) {
  const step = melPat[td.ti][s];
  step.ni = (step.ni + 1) % NOTES.length;
  step.wave = curWave;
  renderMelBtn(sBtns[row][s], step, td.cls);
  sendMelStep(td.ti, s, step);
}
function clearMel(row, s, td) {
  melPat[td.ti][s] = {ni:0, wave:curWave};
  renderMelBtn(sBtns[row][s], melPat[td.ti][s], td.cls);
  sendMelStep(td.ti, s, melPat[td.ti][s]);
}
function toggleDrm(row, s, td) {
  drmPat[td.ti][s] = !drmPat[td.ti][s];
  renderDrmBtn(sBtns[row][s], drmPat[td.ti][s], td.cls);
  send('SEQ_DRUM:' + td.ti + ',' + s + ',' + (drmPat[td.ti][s] ? 1 : 0));
}
function sendMelStep(ti, s, step) {
  const n = NOTES[step.ni];
  send('SEQ_SET:' + ti + ',' + s + ',' + (n ? n.f : 0) + ',' + step.wave);
}
function togglePlay() {
  if (seqRunning) {
    send('SEQ_STOP'); seqRunning = false;
    document.getElementById('playBtn').classList.remove('on');
    document.getElementById('playBtn').innerHTML = '&#9654;';
    clearHighlight();
  } else {
    send('SEQ_PLAY'); seqRunning = true;
    document.getElementById('playBtn').classList.add('on');
    document.getElementById('playBtn').innerHTML = '&#9646;&#9646;';
  }
}
function stopSeq() {
  send('SEQ_STOP'); seqRunning = false;
  document.getElementById('playBtn').classList.remove('on');
  document.getElementById('playBtn').innerHTML = '&#9654;';
  clearHighlight();
}
function setBPM(v) { send('SEQ_BPM:' + parseInt(v)); }
function clearAll() {
  melPat.forEach((t, ti) => t.forEach((_, s) => { t[s] = {ni:0, wave: ti===1?1:0}; }));
  drmPat.forEach(t => t.fill(false));
  buildGrid();
  send('SEQ_CLEAR');
}
function highlightStep(step) {
  TRACKS.forEach((_, row) => {
    if (curStep >= 0 && sBtns[row]?.[curStep]) sBtns[row][curStep].classList.remove('pn');
    if (sBtns[row]?.[step]) sBtns[row][step].classList.add('pn');
  });
  curStep = step;
}
function clearHighlight() {
  TRACKS.forEach((_, row) => {
    if (curStep >= 0 && sBtns[row]?.[curStep]) sBtns[row][curStep].classList.remove('pn');
  });
  curStep = -1;
}

// ── Songs ──────────────────────────────────────────────────────────────────
const LS_KEY = 'tinyWebSynth_songs';
const getSongs = () => { try { return JSON.parse(localStorage.getItem(LS_KEY) || '[]'); } catch { return []; } };
const putSongs = a => localStorage.setItem(LS_KEY, JSON.stringify(a));
function saveSong() {
  const name = document.getElementById('songName').value.trim();
  if (!name) { setSongStatus('Enter a name first.'); return; }
  const bpm  = parseInt(document.getElementById('bpmIn').value) || 120;
  const song = { name, bpm,
    mel: melPat.map(t => t.map(s => ({...s}))),
    drm: drmPat.map(t => [...t])
  };
  const list = getSongs();
  const idx  = list.findIndex(s => s.name === name);
  if (idx >= 0) list[idx] = song; else list.push(song);
  putSongs(list);
  renderSongList();
  setSongStatus('Saved \u201c' + name + '\u201d');
}
function loadSong(song) {
  if (seqRunning) stopSeq();
  document.getElementById('bpmIn').value = song.bpm;
  send('SEQ_BPM:' + song.bpm);
  song.mel.forEach((track, ti) =>
    track.forEach((step, s) => { melPat[ti][s] = {...step}; sendMelStep(ti, s, melPat[ti][s]); }));
  song.drm.forEach((track, ti) =>
    track.forEach((val, s) => { drmPat[ti][s] = val; send('SEQ_DRUM:' + ti + ',' + s + ',' + (val ? 1 : 0)); }));
  buildGrid();
  setSongStatus('Loaded \u201c' + song.name + '\u201d');
  // Close songs panel and scroll to sequencer
  document.getElementById('songsToggle').classList.remove('open');
  document.getElementById('songsPanel').classList.remove('open');
  document.getElementById('seqGrid').scrollIntoView({behavior:'smooth', block:'start'});
}
function deleteSong(name) {
  putSongs(getSongs().filter(s => s.name !== name));
  renderSongList();
  setSongStatus('Deleted \u201c' + name + '\u201d');
}
function renderSongList() {
  const songs = getSongs();
  const el    = document.getElementById('songList');
  el.innerHTML = '';
  if (!songs.length) {
    el.innerHTML = '<p style="color:var(--muted);font-size:.78em;text-align:center;margin-top:12px">No saved songs yet.</p>';
    return;
  }
  songs.forEach(s => {
    const row = document.createElement('div'); row.className = 'si';
    const nm  = document.createElement('span'); nm.className = 'si-name'; nm.textContent = s.name;
    const bm  = document.createElement('span'); bm.className = 'si-bpm';  bm.textContent = s.bpm + ' BPM';
    const ld  = document.createElement('button'); ld.className = 'smbtn ld'; ld.textContent = '\u25B6 Load'; ld.onclick = () => loadSong(s);
    const dl  = document.createElement('button'); dl.className = 'smbtn dl'; dl.textContent = '\u2715'; dl.onclick = () => deleteSong(s.name);
    row.append(nm, bm, ld, dl);
    el.appendChild(row);
  });
}
function setSongStatus(m) { document.getElementById('songStatus').textContent = m; }

// ── Visualizer ─────────────────────────────────────────────────────────────
const canvas = document.getElementById('viz');
const ctx    = canvas.getContext('2d');
let vPhase   = 0;
function resizeCanvas() { canvas.width = canvas.offsetWidth; canvas.height = canvas.offsetHeight; }
resizeCanvas(); window.addEventListener('resize', resizeCanvas);
function drawViz() {
  const W = canvas.width, H = canvas.height;
  ctx.clearRect(0, 0, W, H);
  const on = (heldKey !== null) || seqRunning;
  if (!on) {
    ctx.strokeStyle = '#18182a'; ctx.lineWidth = 1.5;
    ctx.beginPath(); ctx.moveTo(0, H/2); ctx.lineTo(W, H/2); ctx.stroke();
    requestAnimationFrame(drawViz); return;
  }
  const g = ctx.createLinearGradient(0, 0, W, 0);
  g.addColorStop(0,'#22d3ee'); g.addColorStop(.5,'#a855f7'); g.addColorStop(1,'#22d3ee');
  ctx.strokeStyle = g; ctx.lineWidth = 2; ctx.shadowBlur = 14; ctx.shadowColor = '#a855f7';
  const CY = 4, AMP = H / 2 - 5;
  ctx.beginPath();
  for (let x = 0; x < W; x++) {
    const p = (vPhase + (x / W) * CY * Math.PI * 2) % (Math.PI * 2);
    let y;
    switch (curWave) {
      case 0: y = H/2 - (p < Math.PI ? 1 : -1) * AMP; break;
      case 1: y = H/2 - (p / (Math.PI*2) * 2 - 1) * AMP; break;
      case 2: y = H/2 - (p < Math.PI*.5 ? 1 : -1) * AMP; break;
    }
    x === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
  }
  ctx.stroke(); ctx.shadowBlur = 0;
  vPhase = (vPhase + 0.06) % (Math.PI * 2);
  requestAnimationFrame(drawViz);
}

// ── Init ───────────────────────────────────────────────────────────────────
buildGrid();
renderSongList();
drawViz();
</script>
</body>
</html>
)rawliteral";

// ─── I2S SETUP ────────────────────────────────────────────────────────────────

void setupI2S() {
    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate          = SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 8,
        .dma_buf_len          = 64,
        .use_apll             = false,
        .tx_desc_auto_clear   = true
    };
    i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
    i2s_pin_config_t pins = {
        .bck_io_num   = I2S_BCLK,
        .ws_io_num    = I2S_LRC,
        .data_out_num = I2S_DOUT,
        .data_in_num  = I2S_PIN_NO_CHANGE
    };
    i2s_set_pin(I2S_NUM_0, &pins);
}

// ─── SERIAL HELPERS ───────────────────────────────────────────────────────────

String readSerialLine(unsigned long timeoutMs = 30000) {
    String r = "";
    unsigned long t = millis();
    while (millis() - t < timeoutMs) {
        if (Serial.available()) {
            char c = Serial.read();
            if (c == '\n') break;
            if (c != '\r') r += c;
        }
    }
    // Only strip \r — do NOT trim() so trailing spaces in SSIDs are preserved
    r.replace("\r", "");
    return r;
}

void promptAndSaveWiFi() {
    Serial.println("\n=== WiFi Setup ===");
    Serial.println("Enter SSID (30s timeout):");
    String s = readSerialLine(30000);
    if (s.isEmpty()) { Serial.println("No input — keeping existing."); return; }
    Serial.println("Enter Password (30s timeout):");
    String p = readSerialLine(30000);
    prefs.begin("wifi", false);
    prefs.putString("ssid", s);
    prefs.putString("pass", p);
    prefs.end();
    wSSID = s; wPass = p;
    Serial.println("Saved.");
}

void printMenu() {
    Serial.println("\n--- tinyWebSynth ---");
    Serial.printf( "    IP:     %s\n", WiFi.localIP().toString().c_str());
    Serial.printf( "    Volume: %d / 30\n", (int)(gAmp / 1000));
    Serial.println("[i] Show IP");
    Serial.println("[+] Volume up");
    Serial.println("[-] Volume down");
    Serial.println("[w] Update WiFi credentials");
    Serial.println("[e] Export pattern to WAV on SD card");
    Serial.println("[?] Show menu");
    Serial.println("--------------------");
}

// ─── WEBSOCKET HANDLER ────────────────────────────────────────────────────────
// Runs in AsyncTCP task (Core 0) — volatile vars protect shared audio state

void onWsEvent(AsyncWebSocket* srv, AsyncWebSocketClient* client,
               AwsEventType type, void* arg, uint8_t* data, size_t len) {

    if (type != WS_EVT_DATA) return;
    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    if (info->opcode != WS_TEXT) return;

    String msg = "";
    msg.reserve(len + 1);
    for (size_t i = 0; i < len; i++) msg += (char)data[i];

    if (msg.startsWith("PLAY:")) {
        trigNote(CH_LEAD, msg.substring(5).toFloat(), liveWave);

    } else if (msg == "STOP") {
        releaseNote(CH_LEAD);

    } else if (msg.startsWith("WAVE:")) {
        liveWave = constrain(msg.substring(5).toInt(), 0, 2);

    } else if (msg.startsWith("VOL:")) {
        gAmp = constrain((int32_t)msg.substring(4).toInt(), 0, 30000);

    } else if (msg.startsWith("SEQ_SET:")) {
        // SEQ_SET:track,step,freq,wave
        String body = msg.substring(8);
        int c1 = body.indexOf(','), c2 = body.indexOf(',', c1+1), c3 = body.indexOf(',', c2+1);
        if (c1<0||c2<0||c3<0) return;
        int   t = constrain(body.substring(0,c1).toInt(),   0, NUM_MEL_TRK-1);
        int   s = constrain(body.substring(c1+1,c2).toInt(),0, NUM_STEPS-1);
        float f = body.substring(c2+1,c3).toFloat();
        int   w = constrain(body.substring(c3+1).toInt(),   0, 2);
        melPat[t][s].freq = f;
        melPat[t][s].wave = w;

    } else if (msg.startsWith("SEQ_DRUM:")) {
        // SEQ_DRUM:track,step,val
        String body = msg.substring(9);
        int c1 = body.indexOf(','), c2 = body.indexOf(',', c1+1);
        if (c1<0||c2<0) return;
        int t = constrain(body.substring(0,c1).toInt(),   0, NUM_DRM_TRK-1);
        int s = constrain(body.substring(c1+1,c2).toInt(),0, NUM_STEPS-1);
        drmPat[t][s] = body.substring(c2+1).toInt() != 0;

    } else if (msg.startsWith("SEQ_BPM:")) {
        seqBPM = constrain(msg.substring(8).toInt(), 40, 240);

    } else if (msg == "SEQ_PLAY") {
        seqStep    = 0;
        lastStepMs = millis();
        seqPlay    = true;

    } else if (msg == "SEQ_STOP") {
        seqPlay = false;
        releaseNote(CH_LEAD);
        releaseNote(CH_BASS);

    } else if (msg == "SEQ_CLEAR") {
        memset(melPat, 0, sizeof(melPat));
        memset(drmPat, 0, sizeof(drmPat));
        releaseNote(CH_LEAD);
        releaseNote(CH_BASS);
    }
}

// ─── WAV EXPORT ───────────────────────────────────────────────────────────────
// Renders the current pattern to a WAV file on the SD card.
// numLoops: how many times to repeat the 16-step pattern in the file.
// Typical render: 4 loops @ 120 BPM ≈ 1.4MB, takes a few seconds.

void writeU16LE(File& f, uint16_t v) { f.write((uint8_t*)&v, 2); }
void writeU32LE(File& f, uint32_t v) { f.write((uint8_t*)&v, 4); }

void writeWAVHeader(File& f, uint32_t totalSamples) {
    uint32_t dataBytes  = totalSamples * 4; // stereo 16-bit = 4 bytes/sample
    uint32_t chunkSize  = 36 + dataBytes;

    f.write((uint8_t*)"RIFF", 4);
    writeU32LE(f, chunkSize);
    f.write((uint8_t*)"WAVE", 4);
    f.write((uint8_t*)"fmt ", 4);
    writeU32LE(f, 16);           // PCM subchunk size
    writeU16LE(f, 1);            // AudioFormat = PCM
    writeU16LE(f, 2);            // NumChannels = stereo
    writeU32LE(f, SAMPLE_RATE);
    writeU32LE(f, SAMPLE_RATE * 4); // ByteRate = SampleRate * NumCh * BitsPerSample/8
    writeU16LE(f, 4);            // BlockAlign
    writeU16LE(f, 16);           // BitsPerSample
    f.write((uint8_t*)"data", 4);
    writeU32LE(f, dataBytes);
}

void exportPatternToWAV(String filename, int numLoops) {
    if (!SD.begin()) {
        Serial.println("Export failed: SD not available.");
        return;
    }

    String path = "/" + filename;
    if (SD.exists(path)) SD.remove(path); // overwrite if exists
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        Serial.printf("Export failed: could not open %s\n", path.c_str());
        return;
    }

    // Compute sizes
    unsigned long stepDurMs    = 60000UL / (unsigned long)seqBPM / 4; // 16th note
    unsigned long samplesPerStep = ((unsigned long)SAMPLE_RATE * stepDurMs) / 1000UL;
    unsigned long totalSamples   = samplesPerStep * NUM_STEPS * numLoops;

    Serial.printf("Exporting '%s' — %d loops @ %d BPM, ~%lu samples (~%lu KB)...\n",
                  filename.c_str(), numLoops, seqBPM,
                  totalSamples, (totalSamples * 4) / 1024);

    writeWAVHeader(f, totalSamples);

    // Reset all channel state so export always starts clean
    for (int c = 0; c < NUM_CH; c++) {
        ch[c] = Channel(); // zero-initialise
    }

    // Write buffer — accumulate 256 stereo samples before flushing to SD
    // to avoid hammering SD with single-sample writes (very slow)
    const int WBUF_SZ = 256;
    int16_t wbuf[WBUF_SZ * 2];
    int wbufPos = 0;

    auto flushBuf = [&]() {
        f.write((uint8_t*)wbuf, wbufPos * 4);
        wbufPos = 0;
    };

    for (int loop = 0; loop < numLoops; loop++) {
        for (int step = 0; step < NUM_STEPS; step++) {
            trigSequencerStep(step);

            for (unsigned long s = 0; s < samplesPerStep; s++) {
                int32_t mix = 0;
                for (int c = 0; c < NUM_CH; c++) mix += synthSample(c);
                int16_t out = (int16_t)constrain(mix, -32767, 32767);

                wbuf[wbufPos * 2]     = out; // Left
                wbuf[wbufPos * 2 + 1] = out; // Right
                wbufPos++;

                if (wbufPos >= WBUF_SZ) flushBuf();
            }
        }
        Serial.printf("  Loop %d/%d done\n", loop + 1, numLoops);
    }
    if (wbufPos > 0) flushBuf(); // flush remainder

    f.close();
    Serial.printf("Export complete: %s (%lu KB)\n", path.c_str(), (totalSamples * 4) / 1024);
}

// ─── SETUP ────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(2000);
    setupI2S();

    if (!SD.begin()) {
        Serial.println("SD card not found — WAV export unavailable.");
    } else {
        Serial.println("SD card mounted.");
    }

    prefs.begin("wifi", true);
    wSSID = prefs.getString("ssid", "");
    wPass = prefs.getString("pass", "");
    prefs.end();

    if (wSSID.isEmpty()) {
        Serial.println("No WiFi credentials stored.");
        promptAndSaveWiFi();
    }

    while (true) {
        Serial.printf("Connecting to: \"%s\"\n", wSSID.c_str());
        WiFi.disconnect(true);
        delay(100);
        WiFi.begin(wSSID.c_str(), wPass.c_str());
        int tries = 0;
        while (WiFi.status() != WL_CONNECTED && tries < 24) { delay(500); Serial.print('.'); tries++; }
        Serial.println();
        if (WiFi.status() == WL_CONNECTED) break;
        Serial.println("Connection failed. Check credentials and try again.");
        promptAndSaveWiFi();
    }

    Serial.printf("Connected! Open: http://%s\n", WiFi.localIP().toString().c_str());

    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send_P(200, "text/html", index_html);
    });
    server.begin();
    printMenu();
}

// ─── LOOP ─────────────────────────────────────────────────────────────────────

void loop() {
    ws.cleanupClients();

    // ── Sequencer step advance ──────────────────────────────────────────────
    if (seqPlay) {
        // Drift-corrected: advance lastStepMs by exactly one step duration rather
        // than resetting to now, so timing errors don't accumulate over time.
        unsigned long stepDur = 60000UL / (unsigned long)seqBPM / 4; // 16th note ms
        unsigned long now     = millis();
        if (now - lastStepMs >= stepDur) {
            lastStepMs += stepDur;
            trigSequencerStep(seqStep);
            ws.textAll("STEP:" + String(seqStep)); // highlight current step in browser
            seqStep = (seqStep + 1) % NUM_STEPS;
        }
    }

    // ── Audio synthesis (always running) ───────────────────────────────────
    {
        int16_t buf[BUF_SZ * 2]; // interleaved L+R
        size_t  bw;

        for (int i = 0; i < BUF_SZ; i++) {
            int32_t mix = 0;
            for (int c = 0; c < NUM_CH; c++) mix += synthSample(c);
            // Hard clamp — with NUM_CH divisor per channel this should rarely clip
            int16_t out = (int16_t)constrain(mix, -32767, 32767);
            buf[i * 2]     = out; // Left
            buf[i * 2 + 1] = out; // Right
        }
        i2s_write(I2S_NUM_0, buf, sizeof(buf), &bw, portMAX_DELAY);
    }

    // ── Serial menu ─────────────────────────────────────────────────────────
    if (Serial.available()) {
        char c = Serial.read();
        switch (c) {
            case 'i': Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str()); break;
            case '+': gAmp = min(gAmp + 1000, (int32_t)30000); Serial.printf("Volume: %d\n", (int)(gAmp/1000)); break;
            case '-': gAmp = max(gAmp - 1000, (int32_t)0);     Serial.printf("Volume: %d\n", (int)(gAmp/1000)); break;
            case 'w':
                promptAndSaveWiFi();
                Serial.println("Reconnecting...");
                WiFi.disconnect(true); delay(100); WiFi.begin(wSSID.c_str(), wPass.c_str());
                { int t=0; while(WiFi.status()!=WL_CONNECTED && t<24){delay(500);Serial.print('.');t++;} Serial.println(); }
                if (WiFi.status()==WL_CONNECTED) Serial.printf("Reconnected! IP: %s\n", WiFi.localIP().toString().c_str());
                else Serial.println("Failed. Try [w] again.");
                break;
            case 'e': {
                // Prompt for filename and loop count
                Serial.println("Export filename (no spaces, e.g. song1.wav):");
                String fname = readSerialLine(20000);
                if (fname.isEmpty()) { Serial.println("Cancelled."); break; }
                if (!fname.endsWith(".wav")) fname += ".wav";
                Serial.println("Number of loops to render (1-8):");
                String loopsStr = readSerialLine(10000);
                int loops = constrain(loopsStr.toInt(), 1, 8);
                if (loops == 0) loops = 2;
                exportPatternToWAV(fname, loops);
                break;
            }
            case '?': printMenu(); break;
        }
    }
}