#pragma once

// The listener page served at "/". Self-contained: connects a WebSocket to /ws,
// receives 16-bit interleaved stereo PCM (or Opus), and plays it through an
// AudioWorklet. Visual design from design_handoff_listener_page (2026-08).
static const char* const kListenerPage = R"HTMLPAGE(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ListenLink</title>
<meta property="og:type" content="website">
<meta property="og:site_name" content="ListenLink">
<meta property="og:title" content="ListenLink">
<meta property="og:description" content="Live from the master bus &mdash; press play.">
<meta property="og:image" content="https://gggaudio.store/img/listenlink-icon@2x.png">
<meta name="twitter:card" content="summary">
<meta name="twitter:title" content="ListenLink">
<meta name="twitter:description" content="Live from the master bus &mdash; press play.">
<meta name="twitter:image" content="https://gggaudio.store/img/listenlink-icon@2x.png">
<link rel="icon" href="https://gggaudio.store/img/listenlink-icon.png">
<link rel="apple-touch-icon" href="https://gggaudio.store/img/listenlink-icon@2x.png">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;500&family=Inter+Tight:wght@400;500;600&display=swap" rel="stylesheet">
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; margin: 0; }
  body { font-family: 'Inter Tight', -apple-system, 'Segoe UI', Helvetica, sans-serif;
         background:#0c0c10; color:#e8e8ec; min-height:100vh; display:flex;
         align-items:center; justify-content:center; padding:28px; }
  .mono { font-family: 'IBM Plex Mono', monospace; }
  .col { width:min(400px, 100%); }

  /* ---- header ---- */
  .hdr { display:flex; align-items:center; justify-content:space-between;
         margin-bottom:22px; padding:0 4px; }
  .brand { display:flex; align-items:center; gap:9px;
           font-size:13px; font-weight:600; letter-spacing:.01em; color:#e8e8ec; }
  .mark { width:18px; height:18px; border:2px solid #4f6bff; border-radius:50%;
          display:flex; align-items:center; justify-content:center; }
  .mark span { width:5px; height:5px; background:#4f6bff; border-radius:50%; }
  .pill { display:flex; align-items:center; gap:7px; font-size:9.5px;
          letter-spacing:.16em; padding:5px 11px; border-radius:999px;
          border:1px solid #23232c; color:#6a6a76; }
  .pill .pdot { width:5px; height:5px; border-radius:50%; background:#4f4f5a; }
  .pill.live { border-color:rgba(61,220,132,.26); color:#3ddc84;
               background:rgba(61,220,132,.06); }
  .pill.live .pdot { background:#3ddc84; animation:lldot 1.6s infinite; }
  @keyframes lldot { 50% { opacity:.28; } }
  @media (prefers-reduced-motion: reduce) {
    .pill.live .pdot { animation:none; }
  }

  /* ---- card ---- */
  .card { background:#16161c; border:1px solid #23232c; border-radius:20px;
          padding:38px 34px 30px; box-shadow:0 30px 60px -34px rgba(0,0,0,.85); }
  .stack { display:flex; flex-direction:column; align-items:center; gap:26px;
           text-align:center; }
  h1 { font-size:22px; font-weight:600; letter-spacing:-.02em; line-height:1.2;
       color:#e8e8ec; }
  .sub { font-size:13.5px; line-height:1.5; color:#8a8a96; text-wrap:pretty;
         margin-top:7px; }

  /* ---- circular action button ---- */
  #btn { width:76px; height:76px; border-radius:50%; border:none; cursor:pointer;
         display:flex; align-items:center; justify-content:center; padding:0;
         background:#4f6bff; color:#fff; box-shadow:0 14px 32px -12px #4f6bff;
         transition:filter 140ms ease, background 200ms ease; }
  #btn:hover { filter:brightness(1.08); }
  #btn.running { background:#22222b; color:#ff5d5d;
                 box-shadow:inset 0 0 0 1px #2c2c37; }
  #btn.running.muted { color:#ffd24a;
                       box-shadow:inset 0 0 0 1px rgba(255,210,74,.3); }
  .tri { width:0; height:0; margin-left:5px; display:block;
         border-left:17px solid currentColor;
         border-top:11px solid transparent; border-bottom:11px solid transparent; }
  .sq { width:15px; height:15px; border-radius:2px; background:currentColor;
        display:block; }

  /* ---- divider + meters ---- */
  .divider { height:1px; background:#20202a; margin:30px -34px 0; }
  .meters { padding-top:22px; }
  .mgrid { display:grid; grid-template-columns:12px 1fr; gap:9px 11px;
           align-items:center; }
  .mlabel { font-size:9.5px; color:#4f4f5a; }
  .track { height:6px; border-radius:3px; background:#20202a; overflow:hidden;
           position:relative; }
  .fill { position:absolute; top:0; bottom:0; left:0; width:0; border-radius:3px;
          background:#3ddc84; transition:width 70ms linear; }
  .scale { margin-left:23px; margin-top:9px; position:relative; height:11px;
           font-size:9px; letter-spacing:.04em; color:#41414b; }
  .scale span { position:absolute; top:0; }

  /* ---- warning banner ---- */
  .warn[hidden] { display:none; }
  .warn { margin-top:20px; display:flex; align-items:flex-start; gap:9px;
          padding:11px 13px; border-radius:11px; background:rgba(255,210,74,.07);
          border:1px solid rgba(255,210,74,.2); }
  .warn .wdot { width:6px; height:6px; border-radius:50%; background:#ffd24a;
                margin-top:6px; flex:none; }
  .warn p { font-size:12.5px; line-height:1.45; color:#e0c880; text-wrap:pretty; }

  /* ---- footer ---- */
  .foot { margin-top:16px; text-align:center; font-size:10px;
          letter-spacing:.14em; color:#5c5c68; text-transform:uppercase; }
</style>
</head>
<body>
<div class="col">
  <div class="hdr">
    <div class="brand"><span class="mark"><span></span></span>ListenLink</div>
    <div class="pill mono" id="pill"><span class="pdot"></span><span id="pillText">OFFLINE</span></div>
  </div>
  <div class="card">
    <div class="stack">
      <div>
        <h1 id="headline">Live from the master bus</h1>
        <p class="sub" id="subline">Press play to join the session. Nothing to install.</p>
      </div>
      <button id="btn" aria-label="Start listening"><span id="icon" class="tri"></span></button>
    </div>
    <div class="divider"></div>
    <div class="meters">
      <div class="mgrid">
        <span class="mlabel mono">L</span>
        <div class="track"><div class="fill" id="fillL"></div></div>
        <span class="mlabel mono">R</span>
        <div class="track"><div class="fill" id="fillR"></div></div>
      </div>
      <div class="scale mono">
        <span style="left:0">-60</span>
        <span style="left:60%;transform:translateX(-50%)">-24</span>
        <span style="left:80%;transform:translateX(-50%)">-12</span>
        <span style="left:90%;transform:translateX(-50%)">-6</span>
        <span style="right:0">0</span>
      </div>
    </div>
    <div class="warn" id="warn" role="status" aria-live="polite" hidden>
      <span class="wdot"></span>
      <p>Your connection is struggling &mdash; audio may drop out while the buffer rebuilds.</p>
    </div>
  </div>
  <div class="foot mono" id="foot">READY</div>
</div>
<script>
"use strict";
const btn = document.getElementById('btn');
const icon = document.getElementById('icon');
const pill = document.getElementById('pill');
const pillText = document.getElementById('pillText');
const headline = document.getElementById('headline');
const subline = document.getElementById('subline');
const fillL = document.getElementById('fillL');
const fillR = document.getElementById('fillR');
const warn = document.getElementById('warn');
const foot = document.getElementById('foot');

let ctx = null, node = null, gain = null, ws = null;
let running = false, live = false, muted = false;
let wsHost = location.host, wsSecure = location.protocol === 'https:';
let streamId = null, wsFails = 0;
let streamRate = 48000;
let codec = 'pcm', opusBitrate = 0, decoder = null, opusTime = 0, forcePcm = false;
let listeners = 0;
let dispL = 0, dispR = 0;
let lastBufMs = 0, lastUnderruns = 0, lastUnderrunAt = 0, bufferSeen = false;
let disconnected = false;

function levelPct(v) {
  const db = 20 * Math.log10(Math.max(v, 1e-4));
  return Math.max(0, Math.min(100, (db + 60) / 60 * 100));
}

function fillColor(pct) {
  return pct > 90 ? '#ff5d5d' : pct > 80 ? '#ffd24a' : '#3ddc84';
}

function setMeters(pkL, pkR) {
  dispL = Math.max(pkL, dispL * 0.86);
  dispR = Math.max(pkR, dispR * 0.86);
  const pL = levelPct(dispL), pR = levelPct(dispR);
  fillL.style.width = pL.toFixed(1) + '%';
  fillR.style.width = pR.toFixed(1) + '%';
  fillL.style.background = fillColor(pL);
  fillR.style.background = fillColor(pR);
}

// ---- UI state: idle / connecting / live (+ muted) ----
function renderUI() {
  const state = !running ? 'idle' : (live && !disconnected) ? 'live' : 'connecting';

  pill.classList.toggle('live', state === 'live');
  pillText.textContent = state === 'live' ? 'LIVE'
                        : state === 'connecting' ? 'CONNECTING' : 'OFFLINE';

  if (state === 'idle') {
    headline.textContent = 'Live from the master bus';
    subline.textContent = 'Press play to join the session. Nothing to install.';
  } else if (state === 'connecting') {
    headline.innerHTML = 'Connecting&hellip;';
    subline.textContent = 'Audio is streaming in real time from the studio.';
  } else if (muted) {
    headline.textContent = 'Muted';
    subline.innerHTML = 'Tap to unmute &mdash; the stream is still running.';
  } else {
    headline.innerHTML = 'You&rsquo;re listening';
    subline.textContent = 'Audio is streaming in real time from the studio.';
  }

  btn.classList.toggle('running', running);
  btn.classList.toggle('muted', running && muted);
  icon.className = (!running || muted) ? 'tri' : 'sq';
  btn.setAttribute('aria-label', !running ? 'Start listening' : muted ? 'Unmute' : 'Mute');

  renderFoot();
}

function renderFoot() {
  if (!live || disconnected) { foot.textContent = 'READY'; return; }
  const q = codec === 'opus' ? 'OPUS ' + Math.round(opusBitrate / 1000)
                             : (streamRate / 1000) + ' KHZ LOSSLESS';
  foot.textContent = q + ' · ' + lastBufMs + ' MS BUFFER';
}

// Debounced connection-quality banner: recent underruns, a starved buffer,
// or an active reconnect.
function renderWarn() {
  const now = performance.now();
  const struggling =
       (now - lastUnderrunAt < 8000 && lastUnderrunAt > 0)
    || (live && !disconnected && bufferSeen && lastBufMs < 50)
    || (running && disconnected);
  warn.hidden = !struggling;
}

function fallbackToPcm() {
  forcePcm = true;
  if (decoder) { try { decoder.close(); } catch(_){} decoder = null; }
  if (ws) { ws.onclose = null; try { ws.close(); } catch(_){} }
  if (running) connect();
}

function postPair(l, r, frames) {
  if (ctx.sampleRate !== streamRate) {
    const ratio = ctx.sampleRate / streamRate;
    const outN = Math.floor(frames * ratio);
    const l2 = new Float32Array(outN), r2 = new Float32Array(outN);
    for (let i = 0; i < outN; i++) {
      const src = i / ratio, i0 = Math.floor(src), f = src - i0,
            i1 = Math.min(i0 + 1, frames - 1);
      l2[i] = l[i0] + (l[i1] - l[i0]) * f;
      r2[i] = r[i0] + (r[i1] - r[i0]) * f;
    }
    l = l2; r = r2;
  }
  node.port.postMessage({ l, r }, [l.buffer, r.buffer]);
}

function onDecoded(ad) {
  try {
    if (!node) return;
    const frames = ad.numberOfFrames;
    const l = new Float32Array(frames), r = new Float32Array(frames);
    ad.copyTo(l, { planeIndex: 0, format: 'f32-planar' });
    if (ad.numberOfChannels > 1) ad.copyTo(r, { planeIndex: 1, format: 'f32-planar' });
    else r.set(l);
    let pkL = 0, pkR = 0;
    for (let i = 0; i < frames; i++) {
      const a = Math.abs(l[i]), b = Math.abs(r[i]);
      if (a > pkL) pkL = a;
      if (b > pkR) pkR = b;
    }
    setMeters(pkL, pkR);
    postPair(l, r, frames);
  } finally { ad.close(); }
}

const workletSrc = `
class PCMPlayer extends AudioWorkletProcessor {
  constructor() {
    super();
    this.chunks = [];
    this.buffered = 0;
    this.playing = false;
    this.target = Math.round(sampleRate * 0.15);
    this.max = Math.round(sampleRate * 0.6);
    this.trimTo = Math.round(sampleRate * 0.25);
    this.underruns = 0;
    this.frameCount = 0;
    this.port.onmessage = (e) => {
      const d = e.data;
      this.chunks.push({ l: d.l, r: d.r, pos: 0 });
      this.buffered += d.l.length;
      if (this.buffered > this.max) {
        let toDrop = this.buffered - this.trimTo;
        while (toDrop > 0 && this.chunks.length) {
          const c = this.chunks[0];
          const n = Math.min(c.l.length - c.pos, toDrop);
          c.pos += n; toDrop -= n; this.buffered -= n;
          if (c.pos >= c.l.length) this.chunks.shift();
        }
      }
    };
  }
  process(inputs, outputs) {
    const out = outputs[0];
    const L = out[0], R = out.length > 1 ? out[1] : out[0];
    const n = L.length;
    if (!this.playing) {
      if (this.buffered >= this.target) this.playing = true;
      else return true;
    }
    if (this.buffered < n) {
      this.playing = false;
      this.underruns++;
      this.port.postMessage({ underruns: this.underruns, buffered: this.buffered });
      return true;
    }
    let i = 0;
    while (i < n) {
      const c = this.chunks[0];
      const take = Math.min(c.l.length - c.pos, n - i);
      L.set(c.l.subarray(c.pos, c.pos + take), i);
      R.set(c.r.subarray(c.pos, c.pos + take), i);
      c.pos += take; i += take; this.buffered -= take;
      if (c.pos >= c.l.length) this.chunks.shift();
    }
    if ((this.frameCount++ & 63) === 0)
      this.port.postMessage({ underruns: this.underruns, buffered: this.buffered });
    return true;
  }
}
registerProcessor('pcm-player', PCMPlayer);
`;

async function initAudio() {
  if (ctx) { try { await ctx.close(); } catch(_){} ctx = null; node = null; }
  ctx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: streamRate });
  const url = URL.createObjectURL(new Blob([workletSrc], { type: 'application/javascript' }));
  await ctx.audioWorklet.addModule(url);
  node = new AudioWorkletNode(ctx, 'pcm-player', { outputChannelCount: [2] });
  node.port.onmessage = (e) => {
    lastBufMs = Math.round(e.data.buffered / ctx.sampleRate * 1000);
    if (lastBufMs >= 100) bufferSeen = true;
    if (e.data.underruns > lastUnderruns) {
      lastUnderruns = e.data.underruns;
      lastUnderrunAt = performance.now();
    }
    renderFoot();
    renderWarn();
  };
  gain = ctx.createGain();
  gain.gain.value = muted ? 0 : 1;
  node.connect(gain);
  gain.connect(ctx.destination);
  await ctx.resume();
}

// The stream moved (tunnel restarted): ask the link service where it lives
// now, then reconnect there. Falls back to plain retries if it can't answer.
function relocate() {
  fetch('https://gggaudio.store/l/' + streamId + '/resolve', { cache: 'no-store' })
    .then(r => r.ok ? r.json() : null)
    .then(j => {
      if (j && j.url) {
        const h = new URL(j.url).host;
        if (h !== wsHost) { wsHost = h; wsSecure = true; }
      }
    })
    .catch(() => {})
    .finally(() => { setTimeout(() => { if (running) connect(); }, 1500); });
}

function connect() {
  const proto = wsSecure ? 'wss://' : 'ws://';
  ws = new WebSocket(proto + wsHost + '/ws' + (forcePcm ? '?fmt=pcm' : ''));
  ws.binaryType = 'arraybuffer';
  ws.onopen = () => { wsFails = 0; };
  ws.onmessage = async (e) => {
    if (typeof e.data === 'string') {
      const msg = JSON.parse(e.data);
      if (msg.streamId) streamId = msg.streamId;
      if (msg.listeners !== undefined) listeners = msg.listeners;
      if (msg.sampleRate) {
        const newCodec = msg.codec || 'pcm';
        if (newCodec === 'opus' && typeof AudioDecoder === 'undefined') {
          fallbackToPcm();   // old browser: ask the server for raw PCM instead
          return;
        }
        const reinit = !ctx || msg.sampleRate !== streamRate || newCodec !== codec;
        codec = newCodec;
        opusBitrate = msg.bitrate || 0;
        if (reinit) {
          streamRate = msg.sampleRate;
          await initAudio();
          if (decoder) { try { decoder.close(); } catch(_){} decoder = null; }
          if (codec === 'opus') {
            opusTime = 0;
            decoder = new AudioDecoder({ output: onDecoded, error: fallbackToPcm });
            decoder.configure({ codec: 'opus', sampleRate: 48000, numberOfChannels: 2 });
          }
        }
        live = true;
        disconnected = false;
        renderUI();
        renderWarn();
      }
      return;
    }
    if (!node) return;
    if (codec === 'opus') {
      if (decoder && decoder.state === 'configured') {
        decoder.decode(new EncodedAudioChunk({
          type: 'key', timestamp: opusTime, duration: 20000, data: e.data }));
        opusTime += 20000;
      }
      return;
    }
    const int16 = new Int16Array(e.data);
    const frames = int16.length / 2;
    const l = new Float32Array(frames), r = new Float32Array(frames);
    let pkL = 0, pkR = 0;
    for (let i = 0, j = 0; i < frames; i++, j += 2) {
      l[i] = int16[j] / 32768; r[i] = int16[j + 1] / 32768;
      const al = Math.abs(l[i]), ar = Math.abs(r[i]);
      if (al > pkL) pkL = al;
      if (ar > pkR) pkR = ar;
    }
    setMeters(pkL, pkR);
    postPair(l, r, frames);
  };
  ws.onclose = () => {
    if (!running) return;
    live = false;
    disconnected = true;
    renderUI();
    renderWarn();
    wsFails++;
    if (wsFails >= 3 && streamId) { relocate(); return; }
    setTimeout(() => { if (running) connect(); }, 2000);
  };
  ws.onerror = () => { try { ws.close(); } catch(_){} };
}

function setMuted(m) {
  muted = m;
  if (gain) gain.gain.setTargetAtTime(m ? 0 : 1, ctx.currentTime, 0.015);
  renderUI();
}

function start() {
  running = true;
  disconnected = false;
  renderUI();
  connect();
}

// Not reachable from the UI; teardown for pagehide only.
function stop() {
  running = false; live = false;
  if (ws) { ws.onclose = null; try { ws.close(); } catch(_){} ws = null; }
  if (decoder) { try { decoder.close(); } catch(_){} decoder = null; }
  if (ctx) { try { ctx.close(); } catch(_){} ctx = null; node = null; gain = null; }
}

btn.addEventListener('click', () => { running ? setMuted(!muted) : start(); });
window.addEventListener('pagehide', stop);
</script>
</body>
</html>
)HTMLPAGE";
