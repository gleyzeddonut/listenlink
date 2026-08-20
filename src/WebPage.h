#pragma once

// The listener page served at "/". Self-contained: connects a WebSocket to /ws,
// receives 16-bit interleaved stereo PCM, and plays it through an AudioWorklet.
static const char* const kListenerPage = R"HTMLPAGE(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ListenLink &mdash; live stream</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; margin: 0; }
  body { font-family: -apple-system, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
         background:#101014; color:#e8e8ec; min-height:100vh; display:flex;
         align-items:center; justify-content:center; }
  .card { background:#1a1a21; border:1px solid #2a2a33; border-radius:16px;
          padding:40px 44px; width:min(420px, 92vw); text-align:center; }
  h1 { font-size:20px; letter-spacing:.3px; margin-bottom:6px; }
  .sub { color:#8a8a96; font-size:13px; margin-bottom:28px; }
  button { font:inherit; font-size:17px; font-weight:600; color:#fff;
           background:#4f6bff; border:none; border-radius:12px; padding:14px 0;
           width:100%; cursor:pointer; }
  button:hover { background:#657eff; }
  button.live { background:#2a2a33; color:#ff5d5d; }
  button.mute { margin-top:10px; font-size:14px; font-weight:500; padding:10px 0;
                background:#26262f; color:#b8b8c2; }
  button.mute:hover { background:#30303b; }
  button.mute.muted { color:#ffd24a; }
  #status { margin-top:18px; font-size:14px; color:#b8b8c2; min-height:20px; }
  #meta { margin-top:6px; font-size:12px; color:#6a6a76; min-height:16px;
          font-variant-numeric: tabular-nums; }
  #meters { margin-top:16px; display:none; }
  .bar { position:relative; height:5px; border-radius:3px; margin-top:4px; overflow:hidden;
         background:linear-gradient(90deg,#3ddc84 0%,#3ddc84 55%,#ffd24a 80%,#ff5d5d 100%); }
  .cover { position:absolute; top:0; right:0; bottom:0; left:0; background:#26262f;
           transition:left 60ms linear; }
  .dot { display:inline-block; width:8px; height:8px; border-radius:50%;
         background:#3ddc84; margin-right:6px; vertical-align:1px;
         animation:pulse 1.2s infinite; }
  @keyframes pulse { 50% { opacity:.35; } }
</style>
</head>
<body>
<div class="card">
  <h1>ListenLink</h1>
  <div class="sub">Live from the master bus</div>
  <button id="btn">&#9654;&nbsp; Listen</button>
  <button id="muteBtn" class="mute" hidden>&#128263;&nbsp; Mute</button>
  <div id="status">Press listen to start</div>
  <div id="meta"></div>
  <div id="meters">
    <div class="bar"><div class="cover" id="covL"></div></div>
    <div class="bar"><div class="cover" id="covR"></div></div>
  </div>
</div>
<script>
"use strict";
const btn = document.getElementById('btn');
const muteBtn = document.getElementById('muteBtn');
const statusEl = document.getElementById('status');
const metaEl = document.getElementById('meta');
const metersEl = document.getElementById('meters');
const covL = document.getElementById('covL');
const covR = document.getElementById('covR');

let ctx = null, node = null, gain = null, ws = null;
let running = false, live = false, muted = false;
let streamRate = 48000;
let codec = 'pcm', opusBitrate = 0, decoder = null, opusTime = 0, forcePcm = false;
let listeners = 0;
let dispL = 0, dispR = 0;
let bytesIn = 0, lastRateT = 0, lastRateBytes = 0;

function levelPct(v) {
  const db = 20 * Math.log10(Math.max(v, 1e-4));
  return Math.max(0, Math.min(100, (db + 60) / 60 * 100));
}

function setMeters(pkL, pkR) {
  dispL = Math.max(pkL, dispL * 0.86);
  dispR = Math.max(pkR, dispR * 0.86);
  covL.style.left = levelPct(dispL).toFixed(1) + '%';
  covR.style.left = levelPct(dispR).toFixed(1) + '%';
}

function renderStatus() {
  if (!live) return;
  const q = codec === 'opus' ? 'Opus ' + Math.round(opusBitrate / 1000) + ' kbps'
                             : (streamRate / 1000) + ' kHz lossless';
  setStatus('<span class="dot"></span>Live &mdash; ' + q
    + (listeners > 1 ? ' &middot; ' + listeners + ' listening' : '')
    + (muted ? ' &middot; muted' : ''));
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

function setStatus(html) { statusEl.innerHTML = html; }

function updateMeta(bufMs, underruns) {
  const now = performance.now();
  if (now - lastRateT > 1500) {
    if (lastRateT > 0)
      metaEl.dataset.kbps = Math.round((bytesIn - lastRateBytes) * 8 / (now - lastRateT));
    lastRateT = now; lastRateBytes = bytesIn;
  }
  metaEl.textContent = (metaEl.dataset.kbps || '–') + ' kbit/s · buffer ' + bufMs + ' ms'
    + (underruns ? ' · ' + underruns + ' dropouts' : '');
}

async function initAudio() {
  if (ctx) { try { await ctx.close(); } catch(_){} ctx = null; node = null; }
  ctx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: streamRate });
  const url = URL.createObjectURL(new Blob([workletSrc], { type: 'application/javascript' }));
  await ctx.audioWorklet.addModule(url);
  node = new AudioWorkletNode(ctx, 'pcm-player', { outputChannelCount: [2] });
  node.port.onmessage = (e) => {
    updateMeta(Math.round(e.data.buffered / ctx.sampleRate * 1000), e.data.underruns);
  };
  gain = ctx.createGain();
  gain.gain.value = muted ? 0 : 1;
  node.connect(gain);
  gain.connect(ctx.destination);
  await ctx.resume();
}

function connect() {
  const proto = location.protocol === 'https:' ? 'wss://' : 'ws://';
  setStatus('Connecting…');
  ws = new WebSocket(proto + location.host + '/ws' + (forcePcm ? '?fmt=pcm' : ''));
  ws.binaryType = 'arraybuffer';
  ws.onmessage = async (e) => {
    if (typeof e.data === 'string') {
      const msg = JSON.parse(e.data);
      if (msg.listeners !== undefined) {
        listeners = msg.listeners;
        renderStatus();
      }
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
        metersEl.style.display = 'block';
        renderStatus();
      }
      return;
    }
    if (!node) return;
    bytesIn += e.data.byteLength;
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
    setStatus('Disconnected — retrying…');
    setTimeout(() => { if (running) connect(); }, 2000);
  };
  ws.onerror = () => { try { ws.close(); } catch(_){} };
}

function setMuted(m) {
  muted = m;
  muteBtn.classList.toggle('muted', m);
  muteBtn.innerHTML = m ? '&#128266;&nbsp; Unmute' : '&#128263;&nbsp; Mute';
  if (gain) gain.gain.setTargetAtTime(m ? 0 : 1, ctx.currentTime, 0.015);
  renderStatus();
}

function start() {
  running = true;
  btn.classList.add('live');
  btn.innerHTML = '&#9632;&nbsp; Stop';
  muteBtn.hidden = false;
  bytesIn = 0; lastRateT = 0; lastRateBytes = 0;
  delete metaEl.dataset.kbps;
  connect();
}

function stop() {
  running = false; live = false;
  btn.classList.remove('live');
  btn.innerHTML = '&#9654;&nbsp; Listen';
  muteBtn.hidden = true;
  setMuted(false);
  if (ws) { ws.onclose = null; try { ws.close(); } catch(_){} ws = null; }
  if (decoder) { try { decoder.close(); } catch(_){} decoder = null; }
  if (ctx) { try { ctx.close(); } catch(_){} ctx = null; node = null; gain = null; }
  setStatus('Stopped');
  metaEl.textContent = '';
  metersEl.style.display = 'none';
  dispL = 0; dispR = 0;
  covL.style.left = '0%'; covR.style.left = '0%';
}

btn.addEventListener('click', () => { running ? stop() : start(); });
muteBtn.addEventListener('click', () => setMuted(!muted));
</script>
</body>
</html>
)HTMLPAGE";
