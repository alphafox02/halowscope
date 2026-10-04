// HaLowScope web UI: spectrum, waterfall, sweep, receiver control and device
// status, served by the board over Wi-Fi HaLow.
//
// Adapted from eSpDR's iqstream web UI (https://github.com/h0m3us3r/eSpDR,
// 0BSD). Changes: a sweep mode, a device panel in place of the FPGA and
// stream panels, FFT sizes up to 8192. Copyright 2026 CEMAXECUTER LLC.
//
// The server sends spectrum frames already fitted to this view (8-bit steps
// of 0.5 dB) and a status message a few times a second; the page asks for
// the range and resolution it shows, acknowledges every frame (which paces
// the server on a slow link) and reconnects by itself when the link drops.
import {Renderer, NO_DATA, PALETTES, paletteCss} from './render.js';

const $ = (id) => document.getElementById(id);
const clamp = (x, low, high) => Math.min(high, Math.max(low, x));

// ---- preferences (kept in this browser) --------------------------------------------------------

const DEFAULTS = {
    top: -20, range: 80, contrast: 0,       // dBFS at the top of the spectrum, dB shown, waterfall shift
    spectrumAttack: 0.9, spectrumDecay: 0.7, // SDR#'s defaults
    waterfallAttack: 1.0, waterfallDecay: 0.9,
    palette: 'sdrsharp', fill: true, peakHold: false, channels: 'none',
    split: 0.42, maxFps: 0, maxKbps: 0, maxBins: -1,  // -1: automatic resolution
    sidebar: window.innerWidth > 900,
    open: {sweep: true, receiver: true, fft: false, display: false, device: true, link: false},
};
const prefs = (() => {
    try {
        const saved = JSON.parse(localStorage.getItem('halowscope') || '{}');
        return {...DEFAULTS, ...saved, open: {...DEFAULTS.open, ...(saved.open || {})}};
    } catch {
        return {...DEFAULTS};
    }
})();
let saveTimer = 0;
function savePrefs() {
    clearTimeout(saveTimer);
    saveTimer = setTimeout(() => {
        try {
            localStorage.setItem('halowscope', JSON.stringify(prefs));
        } catch { /* private mode: preferences last for this visit */ }
    }, 300);
}

// ---- state ---------------------------------------------------------------------------------------

let status = null;          // last status message
let windows = [];           // FFT windows the server offers: {name, bandwidth (bins)}
let maxBins = 4096;         // most bins the server sends in a frame
const spectrum = {key: '', start: 0, stop: 0, bins: 0, target: null, display: null, peak: null, settled: true};
const waterfall = {key: '', row: null};
const view = {offset: 0, span: 0};     // relative to the LO: centre offset and width, Hz
const pointer = {x: -1, y: -1, inside: false};
let marker = null;                     // Hz
let pendingLo = null, pendingLoAt = 0;
const dirty = {plot: true, overlay: true};
const stats = {frames: 0, merged: 0, bytes: 0, renders: 0, draws: [], age: 0, renderFps: 0, dataFps: 0};
window.halowscope = {stats, prefs, get status() { return status; }, get link() { return link.state; }};  // for inspection

const canvases = $('canvases');
let renderer;
try {
    renderer = new Renderer($('gl'), $('overlay'));
} catch (e) {
    showMessage(e.message);
    throw e;
}
$('gl').addEventListener('webglcontextlost', (e) => e.preventDefault());
$('gl').addEventListener('webglcontextrestored', () => {
    renderer.setup();
    waterfall.key = '';
    dirty.plot = true;
});

// ---- connection -------------------------------------------------------------------------------------

const link = {ws: null, state: 'connecting', retries: 0, retryAt: 0, timer: 0, last: 0, rtt: null,
              clockOffset: 0, replaced: false};

function connect() {
    clearTimeout(link.timer);
    link.replaced = false;
    link.state = 'connecting';
    $('take-over').hidden = true;
    const ws = new WebSocket((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws');
    ws.binaryType = 'arraybuffer';
    link.ws = ws;
    ws.onopen = () => {
        link.state = 'live';
        link.retries = 0;
        link.last = performance.now();
        sendView();
        ping();
        showLink();
    };
    ws.onmessage = (event) => {
        link.last = performance.now();
        if (typeof event.data === 'string') {
            stats.bytes += event.data.length;
            onMessage(JSON.parse(event.data));
        } else {
            stats.bytes += event.data.byteLength;
            onFrame(event.data);
        }
    };
    ws.onclose = (event) => {
        if (link.ws !== ws) return;
        link.ws = null;
        if (event.code === 4001) {
            link.state = 'replaced';
            link.replaced = true;
            showLink();
        } else {
            reconnectLater();
        }
    };
}

function reconnectLater() {
    const delay = Math.min(8000, 500 * 2 ** link.retries) * (0.75 + 0.5 * Math.random());
    link.retries = Math.min(link.retries + 1, 6);
    link.state = 'reconnecting';
    link.retryAt = performance.now() + delay;
    link.timer = setTimeout(connect, delay);
    showLink();
}

// A link that has gone quiet is dead even if the socket has not noticed.
function abandon() {
    const ws = link.ws;
    if (!ws) return;
    ws.onopen = ws.onmessage = ws.onclose = null;
    try { ws.close(); } catch { /* already gone */ }
    link.ws = null;
    reconnectLater();
}

function send(message) {
    if (!link.ws || link.ws.readyState !== WebSocket.OPEN) return false;
    link.ws.send(JSON.stringify(message));
    return true;
}

function ping() { send({t: 'ping', c: performance.now()}); }

setInterval(() => {
    if (link.state === 'live') {
        if (performance.now() - link.last > 5000) abandon();
        else ping();
    }
    showLink();
}, 2000);
document.addEventListener('visibilitychange', () => {
    if (!document.hidden && link.state === 'reconnecting') connect();
    sendView();  // a hidden page asks for fewer frames
});
window.addEventListener('online', () => { if (link.state === 'reconnecting') connect(); });
$('take-over').addEventListener('click', connect);

function onMessage(m) {
    if (m.t === 'status') onStatus(m);
    else if (m.t === 'hello') onHello(m);
    else if (m.t === 'notice') toast(m.text, 'bad');
    else if (m.t === 'pong') {
        const now = performance.now();
        link.rtt = now - m.c;
        link.clockOffset = m.s - (performance.timeOrigin + (now + m.c) / 2);
    }
}

// ---- frames -----------------------------------------------------------------------------------------

function onFrame(buffer) {
    const d = new DataView(buffer);
    if (d.getUint8(0) !== 1) return;
    const peakMode = d.getUint8(1) & 1, bins = d.getUint16(2, true), sequence = d.getUint32(4, true);
    send({t: 'ack', seq: sequence});
    const start = d.getFloat64(8, true), stop = d.getFloat64(16, true);
    const base = d.getFloat32(24, true), step = d.getFloat32(28, true);
    const merged = d.getUint32(32, true), time = d.getFloat64(40, true);
    const codes = new Uint8Array(buffer, 48, bins);
    const db = new Float32Array(bins);
    for (let i = 0; i < bins; i++) db[i] = base + codes[i] * step;

    const key = `${bins}:${start}:${stop}:${peakMode}`;
    if (key !== spectrum.key) {
        Object.assign(spectrum, {key, start, stop, bins, display: db.slice(), peak: db.slice()});
    } else if (prefs.peakHold) {
        for (let i = 0; i < bins; i++) spectrum.peak[i] = Math.max(spectrum.peak[i], db[i]);
    }
    spectrum.target = db;
    spectrum.settled = false;

    // SDR#'s waterfall smoothing, applied from row to row.
    let row = db;
    if (waterfall.key === key) {
        row = new Float32Array(bins);
        const previous = waterfall.row, up = prefs.waterfallAttack, down = prefs.waterfallDecay;
        for (let i = 0; i < bins; i++) {
            const delta = db[i] - previous[i];
            row[i] = previous[i] + (delta > 0 ? up : down) * delta;
        }
    }
    waterfall.key = key;
    waterfall.row = row;
    renderer.pushRow(row, start, stop, time);

    stats.frames++;
    stats.merged += merged - 1;
    stats.age = performance.timeOrigin + performance.now() + link.clockOffset - time;
    dirty.plot = true;
}

// ---- view ----------------------------------------------------------------------------------------------

function fullSpan() {
    const r = status && status.receiver;
    return r ? {lo: r.lo, rate: r.sample_rate} : null;
}

function minimumSpan() {
    const f = fullSpan(), size = status ? status.fft.size : 4096;
    return f ? Math.max(1000, (f.rate / size) * 8) : 1000;
}

// The visible range in Hz.
function visible() {
    const f = fullSpan();
    if (!f) return spectrum.bins ? {start: spectrum.start, stop: spectrum.stop} : {start: 2.4e9, stop: 2.48e9};
    const span = view.span || f.rate;
    const centre = f.lo + view.offset;
    return {start: centre - span / 2, stop: centre + span / 2};
}

function setView(offset, span) {
    const f = fullSpan();
    if (!f) return;
    span = clamp(span, minimumSpan(), f.rate);
    offset = clamp(offset, -(f.rate - span) / 2, (f.rate - span) / 2);
    if (offset === view.offset && span === view.span) return;
    view.offset = offset;
    view.span = span;
    const zoom = Math.log(f.rate / span) / Math.log(f.rate / minimumSpan());
    $('zoom').value = String(Math.round(1000 * (isFinite(zoom) ? zoom : 0)));
    dirty.plot = dirty.overlay = true;
    sendViewSoon();
}

let viewTimer = 0;
function sendViewSoon() {
    if (!viewTimer) viewTimer = setTimeout(() => { viewTimer = 0; sendView(); }, 90);
}

// Asks for the visible range plus a margin to pan into, at about one bin
// per screen pixel.
function sendView() {
    const f = fullSpan(), v = visible(), width = renderer.canvas.width;
    let start = 0, stop = 0, bins = width;
    if (f) {
        const margin = (v.stop - v.start) * (view.span && view.span < f.rate ? 0.25 : 0);
        start = Math.max(f.lo - f.rate / 2, v.start - margin);
        stop = Math.min(f.lo + f.rate / 2, v.stop + margin);
        bins = Math.round((width * (stop - start)) / (v.stop - v.start));
    }
    const limit = Math.min(renderer.textureWidth, maxBins, prefs.maxBins > 0 ? prefs.maxBins : Infinity);
    send({t: 'view', start, stop, bins: clamp(bins, 16, limit), auto: prefs.maxBins < 0,
          fps: document.hidden ? 2 : prefs.maxFps, kbps: prefs.maxKbps});
}

function frequencyAt(x) {  // x in CSS pixels
    const v = visible();
    return v.start + (x / canvases.clientWidth) * (v.stop - v.start);
}

function zoomAround(x, factor) {
    const f = fullSpan();
    if (!f) return;
    const v = visible(), hz = frequencyAt(x), span = clamp((v.stop - v.start) * factor, minimumSpan(), f.rate);
    const start = hz - (x / canvases.clientWidth) * span;
    setView(start + span / 2 - f.lo, span);
}

function pan(pixels) {
    const v = visible();
    setView(view.offset - (pixels / canvases.clientWidth) * (v.stop - v.start), v.stop - v.start);
}

// Plot layout in CSS pixels.
function layout() {
    const height = canvases.clientHeight, axis = 20;
    const spectrumHeight = Math.round(clamp(prefs.split, 0.12, 0.88) * height);
    return {spectrum: spectrumHeight, axis, waterfall: Math.max(0, height - spectrumHeight - axis)};
}

// ---- pointer and keyboard ------------------------------------------------------------------------

const touches = new Map();
let drag = null;

// Pointer position in the plot, in CSS pixels, whatever element is under it.
function local(e) {
    const box = canvases.getBoundingClientRect();
    return {x: e.clientX - box.left, y: e.clientY - box.top};
}
// The message box and the splitter handle their own pointer events.
const onPlot = (e) => !e.target.closest('#message, #splitter');

canvases.addEventListener('wheel', (e) => {
    if (!onPlot(e)) return;
    e.preventDefault();
    const delta = e.deltaY * (e.deltaMode === 1 ? 33 : e.deltaMode === 2 ? 400 : 1);
    zoomAround(local(e).x, Math.exp(delta * 0.0015));
}, {passive: false});

canvases.addEventListener('pointerdown', (e) => {
    if (!onPlot(e) || e.button > 0) return;
    canvases.setPointerCapture(e.pointerId);
    const p = local(e);
    touches.set(e.pointerId, p);
    drag = touches.size === 1 ? {...p, moved: false, time: performance.now()} : null;
});
canvases.addEventListener('pointermove', (e) => {
    const p = local(e);
    pointer.x = p.x;
    pointer.y = p.y;
    pointer.inside = true;
    dirty.overlay = true;
    const touch = touches.get(e.pointerId);
    if (!touch) return;
    if (touches.size === 2) {
        const [a, b] = [...touches.values()];
        const before = Math.abs(a.x - b.x);
        touch.x = p.x;
        touch.y = p.y;
        const [c, d] = [...touches.values()];
        const after = Math.abs(c.x - d.x);
        if (before > 20 && after > 20) zoomAround((c.x + d.x) / 2, before / after);
        return;
    }
    const dx = p.x - touch.x;
    touch.x = p.x;
    touch.y = p.y;
    if (drag && (drag.moved || Math.hypot(p.x - drag.x, p.y - drag.y) > 4)) {
        drag.moved = true;
        pan(dx);
    }
});
function endPointer(e) {
    if (!touches.has(e.pointerId)) return;
    touches.delete(e.pointerId);
    if (drag && !drag.moved && touches.size === 0 && performance.now() - drag.time < 400) {
        marker = frequencyAt(drag.x);
        dirty.overlay = true;
    }
    drag = null;
}
canvases.addEventListener('pointerup', endPointer);
canvases.addEventListener('pointercancel', endPointer);
canvases.addEventListener('pointerleave', () => {
    pointer.inside = false;
    dirty.overlay = true;
});
canvases.addEventListener('contextmenu', (e) => {
    e.preventDefault();
    marker = null;
    dirty.overlay = true;
});
canvases.addEventListener('dblclick', (e) => {
    if (!onPlot(e)) return;
    tune(Math.round(frequencyAt(local(e).x) / 1000) * 1000, true);
});

$('splitter').addEventListener('pointerdown', (e) => {
    e.stopPropagation();
    const target = $('splitter');
    target.setPointerCapture(e.pointerId);
    const move = (m) => {
        const top = canvases.getBoundingClientRect().top;
        prefs.split = clamp((m.clientY - top - 10) / canvases.clientHeight, 0.12, 0.88);
        placeSplitter();
        dirty.plot = dirty.overlay = true;
        savePrefs();
    };
    const up = () => {
        target.removeEventListener('pointermove', move);
        target.removeEventListener('pointerup', up);
        sendView();
    };
    target.addEventListener('pointermove', move);
    target.addEventListener('pointerup', up);
});

function placeSplitter() {
    const l = layout();
    $('splitter').style.top = `${l.spectrum + l.axis / 2}px`;
}

document.addEventListener('keydown', (e) => {
    if (e.target.closest('input, select, textarea') || e.ctrlKey || e.metaKey || e.altKey) return;
    const middle = canvases.clientWidth / 2;
    if (e.key === '+' || e.key === '=') zoomAround(middle, 1 / 1.5);
    else if (e.key === '-') zoomAround(middle, 1.5);
    else if (e.key === '0') setView(0, Infinity);
    else if (e.key === 'ArrowLeft') pan(canvases.clientWidth / 8);
    else if (e.key === 'ArrowRight') pan(-canvases.clientWidth / 8);
    else if (e.key === 'Escape') { marker = null; dirty.overlay = true; }
    else return;
    e.preventDefault();
});

new ResizeObserver(() => {
    // Beyond two device pixels per CSS pixel the picture looks the same and
    // costs more than twice as much to draw.
    renderer.resize(canvases.clientWidth, canvases.clientHeight, Math.min(2, window.devicePixelRatio || 1));
    placeSplitter();
    dirty.plot = dirty.overlay = true;
    sendViewSoon();
}).observe(canvases);

// ---- drawing ----------------------------------------------------------------------------------------

let traceColumns = new Float32Array(0), peakColumns = new Float32Array(0);

// For each device-pixel column: the maximum of the bins under it, or the
// value interpolated between bins when zoomed in beyond them.
function columns(values, out) {
    const w = out.length, v = visible(), bins = spectrum.bins;
    const scale = bins / (spectrum.stop - spectrum.start), step = (v.stop - v.start) / w;
    for (let c = 0; c < w; c++) {
        const b0 = (v.start + c * step - spectrum.start) * scale, b1 = b0 + step * scale;
        if (b1 <= 0 || b0 >= bins) {
            out[c] = NO_DATA;
        } else if (b1 - b0 >= 1) {
            let peak = -Infinity;
            const last = Math.min(bins, Math.ceil(b1));
            for (let i = Math.max(0, Math.floor(b0)); i < last; i++) peak = Math.max(peak, values[i]);
            out[c] = peak;
        } else {
            const x = clamp((b0 + b1) / 2 - 0.5, 0, bins - 1), i = Math.floor(x), f = x - i;
            out[c] = i + 1 < bins ? values[i] * (1 - f) + values[i + 1] * f : values[i];
        }
    }
}

let lastTime = performance.now(), renderMark = performance.now(), renderCount = 0, dataMark = 0;
function animate(now) {
    requestAnimationFrame(animate);
    const dt = Math.min(0.25, (now - lastTime) / 1000);
    lastTime = now;

    // SDR#'s spectrum attack and decay are per FFT frame; applied here in
    // proportion to the time passed, the trace glides at the display rate.
    if (spectrum.target && !spectrum.settled) {
        const frames = dt * (status ? status.fft.rate : 40);
        const up = 1 - Math.pow(1 - prefs.spectrumAttack, frames), down = 1 - Math.pow(1 - prefs.spectrumDecay, frames);
        const d = spectrum.display, t = spectrum.target;
        let moving = false;
        for (let i = 0; i < d.length; i++) {
            const delta = t[i] - d[i];
            d[i] += (delta > 0 ? up : down) * delta;
            if (delta > 0.05 || delta < -0.05) moving = true;
        }
        spectrum.settled = !moving;
        dirty.plot = true;
    }

    if (dirty.plot) {
        const started = performance.now(), l = layout();
        const width = renderer.canvas.width;
        if (traceColumns.length !== width) {
            traceColumns = new Float32Array(width);
            peakColumns = new Float32Array(width);
        }
        if (spectrum.display) {
            columns(spectrum.display, traceColumns);
            if (prefs.peakHold) columns(spectrum.peak, peakColumns);
        } else {
            traceColumns.fill(NO_DATA);
        }
        renderer.setColumns(traceColumns, prefs.peakHold ? peakColumns : null);
        renderer.draw(l, visible(), {top: prefs.top, bottom: prefs.top - prefs.range, contrast: prefs.contrast / 100,
                                     fill: prefs.fill, peak: prefs.peakHold});
        dirty.plot = false;
        dirty.overlay = true;
        renderCount++;
        stats.draws.push(performance.now() - started);
        if (stats.draws.length > 240) stats.draws.shift();
    }
    if (dirty.overlay) {
        drawOverlay();
        dirty.overlay = false;
    }
    if (now - renderMark >= 1000) {
        stats.renderFps = (renderCount * 1000) / (now - renderMark);
        stats.dataFps = ((stats.frames - dataMark) * 1000) / (now - renderMark);
        dataMark = stats.frames;
        renderCount = 0;
        renderMark = now;
    }
}
requestAnimationFrame(animate);

function niceStep(minimum) {
    const power = Math.pow(10, Math.floor(Math.log10(minimum)));
    for (const m of [1, 2, 5, 10]) if (m * power >= minimum) return m * power;
    return 10 * power;
}

function formatHz(hz, step) {
    const decimals = clamp(Math.ceil(-Math.log10(step / 1e6)), 0, 6);
    return (hz / 1e6).toFixed(decimals);
}

const BLE = Array.from({length: 40}, (_, k) =>
    k === 37 ? 2402e6 : k === 38 ? 2426e6 : k === 39 ? 2480e6 : k <= 10 ? 2404e6 + 2e6 * k : 2428e6 + 2e6 * (k - 11));
const WIFI = Array.from({length: 14}, (_, k) => (k === 13 ? 2484e6 : 2412e6 + 5e6 * k));

function drawOverlay() {
    const ctx = renderer.context2d, r = renderer.ratio || 1;
    const w = canvases.clientWidth, h = canvases.clientHeight, l = layout(), v = visible();
    const x = (hz) => ((hz - v.start) / (v.stop - v.start)) * w;
    const top = prefs.top, bottom = prefs.top - prefs.range;
    const y = (db) => ((top - db) / (top - bottom)) * l.spectrum;
    ctx.setTransform(r, 0, 0, r, 0, 0);
    ctx.clearRect(0, 0, w, h);
    ctx.font = '11px system-ui, sans-serif';
    ctx.textBaseline = 'middle';

    // Level grid and labels.
    const dbStep = niceStep((prefs.range * 26) / Math.max(1, l.spectrum));
    ctx.strokeStyle = 'rgba(160, 180, 200, 0.13)';
    ctx.fillStyle = 'rgba(200, 210, 220, 0.75)';
    ctx.lineWidth = 1;
    ctx.beginPath();
    for (let db = Math.ceil(bottom / dbStep) * dbStep; db <= top; db += dbStep) {
        const yy = Math.round(y(db)) + 0.5;
        ctx.moveTo(0, yy);
        ctx.lineTo(w, yy);
    }
    ctx.stroke();
    ctx.textAlign = 'left';
    for (let db = Math.ceil(bottom / dbStep) * dbStep; db <= top; db += dbStep) {
        const yy = y(db);
        if (yy > 8 && yy < l.spectrum - 6) ctx.fillText(`${db}`, 4, yy);
    }

    // Frequency grid, and labels in the strip between the plots.
    const hzStep = niceStep(((v.stop - v.start) * 90) / Math.max(1, w));
    ctx.beginPath();
    for (let f = Math.ceil(v.start / hzStep) * hzStep; f <= v.stop; f += hzStep) {
        const xx = Math.round(x(f)) + 0.5;
        ctx.moveTo(xx, 0);
        ctx.lineTo(xx, l.spectrum);
    }
    ctx.stroke();
    ctx.fillStyle = '#0d1116';
    ctx.fillRect(0, l.spectrum, w, l.axis);
    ctx.fillStyle = 'rgba(200, 210, 220, 0.85)';
    ctx.textAlign = 'center';
    for (let f = Math.ceil(v.start / hzStep) * hzStep; f <= v.stop; f += hzStep) {
        const xx = x(f);
        if (xx > 24 && xx < w - 24) ctx.fillText(formatHz(f, hzStep), xx, l.spectrum + l.axis / 2);
    }

    // Channel plan.
    if (prefs.channels !== 'none') {
        ctx.textAlign = 'center';
        if (prefs.channels === 'ble') {
            const spacing = (2e6 / (v.stop - v.start)) * w;
            BLE.forEach((f, k) => {
                const xx = x(f);
                if (xx < 0 || xx > w) return;
                const advertising = k >= 37;
                ctx.fillStyle = advertising ? 'rgba(255, 190, 80, 0.14)' : 'rgba(120, 170, 255, 0.07)';
                ctx.fillRect(x(f - 1e6), 0, Math.max(1, spacing), l.spectrum);
                if (spacing > 16 || advertising) {
                    ctx.fillStyle = advertising ? 'rgba(255, 200, 110, 0.95)' : 'rgba(170, 200, 255, 0.8)';
                    ctx.fillText(String(k), xx, 10);
                }
            });
        } else {
            WIFI.forEach((f, k) => {
                const x0 = x(f - 10e6), x1 = x(f + 10e6);
                if (x1 < 0 || x0 > w) return;
                ctx.strokeStyle = 'rgba(120, 200, 160, 0.4)';
                ctx.strokeRect(x0 + 0.5, 3.5 + (k % 3) * 12, x1 - x0, 12);
                ctx.fillStyle = 'rgba(150, 230, 180, 0.9)';
                ctx.fillText(String(k + 1), x(f), 9.5 + (k % 3) * 12);
            });
        }
    }

    // The LO (and the DC artefact that sits on it).
    const f = fullSpan();
    if (f) {
        const xx = Math.round(x(f.lo)) + 0.5;
        ctx.strokeStyle = 'rgba(255, 255, 255, 0.18)';
        ctx.setLineDash([3, 4]);
        ctx.beginPath();
        ctx.moveTo(xx, 0);
        ctx.lineTo(xx, l.spectrum);
        ctx.stroke();
        ctx.setLineDash([]);
    }

    const levelAt = (px) => {
        const c = Math.round(px * r);
        return c >= 0 && c < traceColumns.length && traceColumns[c] > NO_DATA ? traceColumns[c] : null;
    };
    const label = (text, xx, yy, color) => {
        ctx.font = '12px system-ui, sans-serif';
        const width = ctx.measureText(text).width + 12, left = clamp(xx + 10, 2, w - width - 2);
        ctx.fillStyle = 'rgba(10, 13, 17, 0.88)';
        ctx.fillRect(left, yy - 10, width, 20);
        ctx.fillStyle = color;
        ctx.textAlign = 'left';
        ctx.fillText(text, left + 6, yy);
    };
    const digits = clamp(Math.ceil(-Math.log10((v.stop - v.start) / w / 1e6)), 0, 6);

    if (marker !== null) {
        const xx = Math.round(x(marker)) + 0.5;
        if (xx >= 0 && xx <= w) {
            ctx.strokeStyle = 'rgba(255, 190, 70, 0.9)';
            ctx.beginPath();
            ctx.moveTo(xx, 0);
            ctx.lineTo(xx, h);
            ctx.stroke();
            const level = levelAt(xx);
            label(`${(marker / 1e6).toFixed(digits)} MHz${level === null ? '' : `  ${level.toFixed(1)} dBFS`}`,
                  xx, 24, '#ffc766');
        }
    }
    if (pointer.inside && !drag?.moved) {
        const xx = Math.round(pointer.x) + 0.5;
        ctx.strokeStyle = 'rgba(255, 255, 255, 0.35)';
        ctx.beginPath();
        ctx.moveTo(xx, 0);
        ctx.lineTo(xx, h);
        ctx.stroke();
        let text = `${(frequencyAt(pointer.x) / 1e6).toFixed(digits)} MHz`;
        if (pointer.y < l.spectrum) {
            const level = levelAt(pointer.x);
            if (level !== null) text += `  ${level.toFixed(1)} dBFS`;
            if (marker !== null) text += `  Δ ${((frequencyAt(pointer.x) - marker) / 1e3).toFixed(1)} kHz`;
        } else if (pointer.y > l.spectrum + l.axis) {
            const t = renderer.rowTime(Math.floor(pointer.y - l.spectrum - l.axis));
            if (t) text += `  ${((Date.now() + link.clockOffset - t) / 1000).toFixed(1)} s ago`;
        }
        label(text, pointer.x, clamp(pointer.y - 18, 12, h - 12), '#e8eef5');
    }
}

function showMessage(text, takeOver = false) {
    $('message').hidden = !text;
    $('message-text').textContent = text || '';
    $('take-over').hidden = !takeOver;
}

let toastCount = 0;
function toast(text, kind = '') {
    const element = document.createElement('div');
    element.className = `toast ${kind}`;
    element.textContent = text;
    $('toasts').append(element);
    if (++toastCount > 4) $('toasts').firstElementChild?.remove();
    setTimeout(() => { element.remove(); toastCount--; }, 6000);
}

// ---- status ----------------------------------------------------------------------------------------

const setStats = (id, rows) => {
    $(id).replaceChildren(...rows.flatMap(([k, value]) => {
        const dt = document.createElement('dt'), dd = document.createElement('dd');
        dt.textContent = k;
        dd.textContent = value;
        return [dt, dd];
    }));
};

function onHello(m) {
    // A new connection: what the last one said may no longer hold.
    status = null;
    windows = m.windows;
    maxBins = m.max_bins;
    $('fft-window').replaceChildren(...windows.map((w) => new Option(w.name.replace(/-/g, ' '), w.name)));
}

function onStatus(m) {
    // Sections that have not changed since the last status are left out;
    // one that is no longer known comes as null.
    const previous = status;
    const s = status = {...(status || {}), ...m};
    const r = s.receiver, before = previous && previous.receiver;
    if (r && (!before || before.lo !== r.lo || before.sample_rate !== r.sample_rate)) {
        // A new LO keeps the same frequencies in view, as far as the new span
        // reaches; a view of the whole span stays whole at a new rate.
        if (before) view.offset += before.lo - r.lo;
        if (!view.span || (before && view.span >= before.sample_rate)) view.span = r.sample_rate;
        setView(view.offset, view.span);
        dirty.plot = dirty.overlay = true;
        sendView();
    }
    if (!previous || previous.fft.size !== s.fft.size) setView(view.offset, view.span);

    // Top bar.
    $('run').textContent = s.running ? '■' : '▶';
    $('run').classList.toggle('on', s.running);
    $('run').setAttribute('aria-label', s.running ? 'Stop streaming' : 'Start streaming');
    const states = {running: ['running', 'good'], sweeping: ['sweeping', 'good'], stopped: ['stopped', ''],
                    error: ['error', 'bad']};
    const [stateText, stateClass] = states[s.state] || [s.state, ''];
    setPill('pill-state', stateText, stateClass);
    if (pendingLo !== null && r && (Math.abs(r.lo - pendingLo) < 1000 || performance.now() - pendingLoAt > 8000))
        pendingLo = null;
    showFrequency();
    if (r) $('span').textContent = s.sweep && s.sweep.on
        ? `sweep ${(s.sweep.start / 1e6).toFixed(0)}–${(s.sweep.stop / 1e6).toFixed(0)} MHz · ${r.rate} Msps · gain ${r.gain}`
        : `${r.rate} Msps · ${r.width} MHz · gain ${r.gain}`;

    let messageText = '';
    if (s.state === 'error' && s.message) messageText = s.message;
    else if (!s.running) messageText = 'Stopped. Press ▶ to stream.';
    if (!link.replaced) showMessage(messageText);

    showReceiver(r);
    showFft(s);
    showSweep(s.sweep);
    showDevice(s);
    showLinkStats(s.link);
    enableControls();
}

function setPill(id, text, kind) {
    const pill = $(id);
    pill.textContent = text;
    pill.className = `pill ${kind}`;
}

function showLink() {
    const now = performance.now();
    if (link.state === 'live') setPill('pill-link', `live${link.rtt === null ? '' : ` · ${Math.round(link.rtt)} ms`}`, 'good');
    else if (link.state === 'reconnecting') setPill('pill-link', `reconnecting in ${Math.max(0, Math.ceil((link.retryAt - now) / 1000))} s`, 'warn');
    else if (link.state === 'replaced') setPill('pill-link', 'in use elsewhere', 'bad');
    else setPill('pill-link', 'connecting', 'warn');
    if (link.replaced) showMessage('Another browser is using this receiver now.', true);
    if (link.state !== 'live') setPill('pill-state', 'unknown', '');  // until the server is heard again
    enableControls();
}

function showSweep(w) {
    if (!w) return;
    showValue($('sweep-on'), w.on);
    showValue($('sweep-start'), (w.start / 1e6).toFixed(0));
    showValue($('sweep-stop'), (w.stop / 1e6).toFixed(0));
}

function showDevice(s) {
    const d = s.device, sp = s.spectrum;
    if (!d) return;
    setPill('pill-errors', `HaLow ${d.rssi} dBm`, d.rssi > -80 ? 'good' : d.rssi > -95 ? 'warn' : 'bad');
    const kib = (bytes) => `${(bytes / 1024).toFixed(0)} KiB`;
    setStats('device-stats', [
        ['HaLow address', d.ip || '—'],
        ['HaLow RSSI', `${d.rssi} dBm`],
        ['Uptime', `${Math.floor(d.uptime / 3600)} h ${Math.floor((d.uptime % 3600) / 60)} min`],
        ['Internal RAM free', `${kib(d.heap)} (lowest ${kib(d.heap_min)})`],
        ['PSRAM free', kib(d.psram)],
        ['Snapshot + FFTs', `${(sp.snapshot_us / 1000).toFixed(1)} ms`],
        ['Last retune', `${(sp.retune_us / 1000).toFixed(1)} ms`],
        ['Capture failures', String(sp.failures)],
    ]);
}

function showLinkStats(l) {
    const rows = [
        ['State', link.state],
        ['Round trip', link.rtt === null ? '—' : `${link.rtt.toFixed(0)} ms`],
        ['Data age', stats.frames ? `${Math.max(0, stats.age).toFixed(0)} ms` : '—'],
        ['Frames', `${stats.dataFps.toFixed(1)}/s received, ${(l ? l.merged : 0).toFixed(1)}/s merged`],
        ['Drawing', `${stats.renderFps.toFixed(0)} fps, ${percentile(stats.draws, 0.95).toFixed(2)} ms p95`],
    ];
    if (l) {
        rows.push(['Link', `${l.kbps.toFixed(0)} kbit/s used${l.compressed ? ', compressed' : ''}`]);
        rows.push(['Frames delivered', `${l.delivered.toFixed(0)} kbit/s; empty-link round trip ${(1000 * l.rtt_min).toFixed(0)} ms`]);
        rows.push(['In flight', `${l.in_flight} frames, ${(l.in_flight_bytes / 1024).toFixed(1)} of ${(l.window / 1024).toFixed(1)} KiB`]);
        if (l.bins_cap) rows.push(['Resolution', `${l.bins_cap} bins (reduced for the link)`]);
    }
    setStats('link-stats', rows);
}

function percentile(values, p) {
    if (!values.length) return 0;
    const sorted = [...values].sort((a, b) => a - b);
    return sorted[Math.min(sorted.length - 1, Math.floor(p * sorted.length))];
}

// ---- frequency display --------------------------------------------------------------------------

const frequency = $('frequency');
const DIGITS = 10;  // Hz, up to 9.999 999 999 GHz
const digitElements = [];
for (let i = 0; i < DIGITS; i++) {
    if (i && (DIGITS - i) % 3 === 0) {
        const dot = document.createElement('span');
        dot.className = 'dot';
        dot.textContent = '.';
        frequency.append(dot);
    }
    const d = document.createElement('span');
    d.className = 'digit';
    d.dataset.place = String(DIGITS - 1 - i);
    frequency.append(d);
    digitElements.push(d);
}

function currentLo() {
    return pendingLo ?? (status && status.receiver ? status.receiver.lo : null);
}

function showFrequency() {
    const lo = currentLo(), text = lo === null ? '' : String(Math.round(lo)).padStart(DIGITS, '0');
    let leading = true;
    digitElements.forEach((d, i) => {
        d.textContent = text ? text[i] : '-';
        leading = leading && text[i] === '0';
        d.classList.toggle('lead', leading);
    });
    frequency.classList.toggle('pending', pendingLo !== null);
    if (document.activeElement !== $('lo') && lo !== null) $('lo').value = (lo / 1e6).toFixed(6);
}

let tuneTimer = 0;
// Retunes the LO. `centre`: also bring that frequency to the middle of the view.
function tune(hz, centre = false) {
    if (!status || !status.receiver || link.state !== 'live') {
        toast('The receiver is not available.', 'bad');
        return;
    }
    hz = clamp(Math.round(hz), 2210e6, 2790e6);
    pendingLo = hz;
    pendingLoAt = performance.now();
    const f = fullSpan();
    if (centre && f) setView(hz - f.lo, view.span || f.rate);
    showFrequency();
    clearTimeout(tuneTimer);
    tuneTimer = setTimeout(() => {
        if (!send({t: 'set', v: `lo=${hz}`})) toast('Not connected.', 'bad');
    }, centre ? 0 : 350);
}

function stepDigit(place, direction) {
    const lo = currentLo();
    if (lo === null) return;
    const unit = 10 ** place;
    tune(Math.round(lo / unit) * unit + direction * unit);
}

frequency.addEventListener('wheel', (e) => {
    const d = e.target.closest('.digit');
    if (!d) return;
    e.preventDefault();
    stepDigit(Number(d.dataset.place), e.deltaY < 0 ? 1 : -1);
}, {passive: false});
frequency.addEventListener('click', (e) => {
    const d = e.target.closest('.digit');
    if (!d) return;
    const box = d.getBoundingClientRect();
    stepDigit(Number(d.dataset.place), e.clientY < box.top + box.height / 2 ? 1 : -1);
});
// Typing: Enter, or a digit, while the display has focus.
function startEntry(initial) {
    const entry = $('frequency-entry'), lo = currentLo();
    entry.value = initial ?? (lo === null ? '' : (lo / 1e6).toFixed(6));
    frequency.hidden = true;
    entry.hidden = false;
    entry.focus();
    if (initial === undefined) entry.select();
}
frequency.addEventListener('keydown', (e) => {
    if (e.key === 'ArrowUp' || e.key === 'ArrowDown') {
        e.preventDefault();
        stepDigit(6, e.key === 'ArrowUp' ? 1 : -1);
    } else if (e.key === 'Enter') {
        e.preventDefault();
        startEntry();
    } else if (/^[0-9.]$/.test(e.key)) {
        e.preventDefault();
        startEntry(e.key);
    }
});

// "2402", "2402.5M", "2.4G", "2402000k": MHz unless a unit says otherwise.
function parseFrequency(text) {
    const m = /^\s*([0-9]*\.?[0-9]+)\s*([kMG]?)(hz)?\s*$/i.exec(text);
    if (!m) return null;
    const unit = {k: 1e3, m: 1e6, g: 1e9, '': 1e6}[m[2].toLowerCase()];
    return Number(m[1]) * unit;
}

function finishEntry(apply) {
    const entry = $('frequency-entry');
    if (entry.hidden) return;
    const hz = parseFrequency(entry.value);
    entry.hidden = true;
    frequency.hidden = false;
    if (apply && hz !== null) tune(hz);
    else if (apply) toast('Enter a frequency such as 2402 or 2.426G.', 'bad');
}
$('frequency-entry').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') finishEntry(true);
    else if (e.key === 'Escape') finishEntry(false);
});
$('frequency-entry').addEventListener('blur', () => finishEntry(false));
$('lo').addEventListener('change', () => {
    const hz = parseFrequency($('lo').value);
    if (hz === null) toast('Enter a frequency such as 2402 or 2.426G.', 'bad');
    else tune(hz);
});

// ---- controls -----------------------------------------------------------------------------------------

const touched = new Map();  // control -> time of the user's last change

// Shows a value in a control unless the user is working it.
function showValue(input, value) {
    if (document.activeElement === input || performance.now() - (touched.get(input) || 0) < 1500) return;
    if (input.type === 'checkbox') input.checked = value;
    else input.value = String(value);
    const output = document.querySelector(`output[for="${input.id}"]`);
    if (output) output.textContent = input.value;
}

function showSegmented(id, value) {
    for (const b of $(id).querySelectorAll('button')) b.classList.toggle('on', b.dataset.v === String(value));
}

// Receiver settings travel as NAME=VALUE; a dragged slider sends at most
// every 250 ms (the server keeps only the latest of each).
const settingTimers = {};
function setReceiver(name, value, immediate) {
    const text = `${name}=${value}`;
    clearTimeout(settingTimers[name]?.timer);
    const due = settingTimers[name]?.due || 0, now = performance.now();
    const go = () => {
        settingTimers[name] = {due: performance.now() + 250};
        if (!send({t: 'set', v: text})) toast('Not connected.', 'bad');
    };
    if (immediate || now >= due) go();
    else settingTimers[name] = {due, timer: setTimeout(go, due - now)};
}

function bindSlider(id, onValue) {
    const input = $(id), output = document.querySelector(`output[for="${id}"]`);
    const update = (immediate) => {
        touched.set(input, performance.now());
        if (output) output.textContent = input.value;
        onValue(Number(input.value), immediate);
    };
    input.addEventListener('input', () => update(false));
    input.addEventListener('change', () => update(true));
}

function showReceiver(r) {
    if (!r) return;
    const [cap, first, length] = r.pll;
    $('lo-note').textContent = `Exactly ${r.exact_lo.toLocaleString('en-US')} Hz · PLL capacitor ${cap} (locks ${first}–${first + length - 1})`;
    showSegmented('rate', r.rate);
    showSegmented('width', r.width);
    showValue($('filter'), r.filter[0]);
    showValue($('filter2'), r.filter[1]);
    if (r.filter[0] !== r.filter[1] && !$('filter-split').checked) {
        $('filter-split').checked = true;
        $('filter2-row').hidden = false;
    }
    showValue($('gain'), r.gain);
    const automatic = (bit) => (r.auto & bit) !== 0;
    showValue($('rf'), r.rf);
    showValue($('rf-auto'), automatic(1));
    showValue($('bb'), r.bb);
    showValue($('bb-auto'), automatic(2));
    for (let k = 0; k < 4; k++) {
        showValue($(`dc${k}`), r.dc[k]);
        showValue($(`dc${k}-auto`), automatic(4 << k));
    }
    showValue($('iq-auto'), automatic(64));
    showValue($('iq-amplitude'), r.iq[0]);
    showValue($('iq-phase'), r.iq[1]);
    enableControls();
}

function enableControls() {
    const r = status && status.receiver, live = link.state === 'live';
    for (const e of $('panel-receiver').querySelectorAll('input, button, select')) e.disabled = !live || !r;
    if (r) {
        $('rf').disabled ||= $('rf-auto').checked;
        $('bb').disabled ||= $('bb-auto').checked;
        for (let k = 0; k < 4; k++) $(`dc${k}`).disabled ||= $(`dc${k}-auto`).checked;
        $('iq-amplitude').disabled ||= $('iq-auto').checked;
        $('iq-phase').disabled ||= $('iq-auto').checked;
    }
    for (const e of $('panel-fft').querySelectorAll('input, button, select')) e.disabled = !live;
    for (const e of $('panel-sweep').querySelectorAll('input, button')) e.disabled = !live || !r;
    $('run').disabled = !live;
}

for (const b of $('rate').querySelectorAll('button')) b.addEventListener('click', () => setReceiver('rate', b.dataset.v, true));
for (const b of $('width').querySelectorAll('button')) b.addEventListener('click', () => setReceiver('width', b.dataset.v, true));
const sendFilter = (immediate) => {
    const a = $('filter').value, b = $('filter-split').checked ? $('filter2').value : a;
    setReceiver('filter', `${a},${b}`, immediate);
};
bindSlider('filter', (_, immediate) => sendFilter(immediate));
bindSlider('filter2', (_, immediate) => sendFilter(immediate));
$('filter-split').addEventListener('change', () => {
    $('filter2-row').hidden = !$('filter-split').checked;
    if (!$('filter-split').checked) sendFilter(true);
});
bindSlider('gain', (v, immediate) => setReceiver('gain', v, immediate));
for (const name of ['rf', 'bb', 'dc0', 'dc1', 'dc2', 'dc3']) {
    bindSlider(name, (v, immediate) => setReceiver(name, v, immediate));
    $(`${name}-auto`).addEventListener('change', () => {
        touched.set($(`${name}-auto`), performance.now());
        setReceiver(name, $(`${name}-auto`).checked ? 'auto' : $(name).value, true);
        enableControls();
    });
}
const sendIq = (immediate) => setReceiver('iq', $('iq-auto').checked ? 'auto' : `${$('iq-amplitude').value},${$('iq-phase').value}`, immediate);
bindSlider('iq-amplitude', (_, immediate) => sendIq(immediate));
bindSlider('iq-phase', (_, immediate) => sendIq(immediate));
$('iq-auto').addEventListener('change', () => {
    touched.set($('iq-auto'), performance.now());
    sendIq(true);
    enableControls();
});
$('run').addEventListener('click', () => send({t: 'run', v: !(status && status.running)}));

// Sweep: the LO steps across a range and one frame covers all of it.
// Tuning to a frequency (double-click, the digits) returns to a fixed LO.
function sendSweep(on) {
    const start = parseFrequency($('sweep-start').value), stop = parseFrequency($('sweep-stop').value);
    if (start === null || stop === null || stop <= start) {
        toast('Enter a start and a stop frequency, in MHz.', 'bad');
        return;
    }
    touched.set($('sweep-on'), performance.now());
    if (!send({t: 'sweep', on, start, stop})) toast('Not connected.', 'bad');
}
$('sweep-on').addEventListener('change', () => sendSweep($('sweep-on').checked));
for (const id of ['sweep-start', 'sweep-stop']) {
    $(id).addEventListener('change', () => {
        touched.set($(id), performance.now());
        if ($('sweep-on').checked) sendSweep(true);
    });
}
for (const b of $('sweep-presets').querySelectorAll('button')) {
    b.addEventListener('click', () => {
        $('sweep-start').value = b.dataset.start;
        $('sweep-stop').value = b.dataset.stop;
        $('sweep-on').checked = true;
        sendSweep(true);
    });
}

// FFT settings belong to the server (every frame is computed once).
const AVERAGING = [1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 0];
const SIZES = Array.from({length: 5}, (_, k) => 512 * 2 ** k);
$('fft-size').replaceChildren(...SIZES.map((n) => new Option(n.toLocaleString('en-US'), String(n))));
// Only the fields that changed: the server keeps the rest, so quick
// successive changes cannot undo each other.
function sendFft(changes) {
    if (!send({t: 'fft', ...changes})) toast('Not connected.', 'bad');
}
$('fft-size').addEventListener('change', () => sendFft({size: Number($('fft-size').value)}));
$('fft-window').addEventListener('change', () => sendFft({window: $('fft-window').value}));
bindSlider('fft-rate', (v, immediate) => { if (immediate) sendFft({rate: v}); });
bindSlider('fft-averaging', (v, immediate) => {
    $('fft-averaging').nextElementSibling.textContent = AVERAGING[v] || 'all';
    if (immediate) sendFft({averaging: AVERAGING[v]});
});
for (const b of $('fft-detector').querySelectorAll('button'))
    b.addEventListener('click', () => sendFft({peak: b.dataset.v === 'peak'}));
$('fft-dc').addEventListener('change', () => sendFft({dc: $('fft-dc').checked}));

function showFft(s) {
    const f = s.fft;
    showValue($('fft-size'), f.size);
    if (windows.length) showValue($('fft-window'), f.window);
    showValue($('fft-rate'), f.rate);
    const index = AVERAGING.indexOf(f.averaging);
    showValue($('fft-averaging'), index < 0 ? 0 : index);
    if (document.activeElement !== $('fft-averaging')) $('fft-averaging').nextElementSibling.textContent = f.averaging || 'all';
    showSegmented('fft-detector', f.peak ? 'peak' : 'average');
    showValue($('fft-dc'), f.dc);
    const rate = s.receiver ? s.receiver.rate * 1e6 : 80e6, bin = rate / f.size;
    const rbw = bin * (windows.find((w) => w.name === f.window)?.bandwidth || 1);
    const sp = s.spectrum;
    $('fft-note').textContent = `Bins ${(bin / 1e3).toFixed(2)} kHz apart, resolution bandwidth ${(rbw / 1e3).toFixed(2)} kHz. ` +
        (s.sweep && s.sweep.on
            ? `One sweep every ${(sp.sweep_ms / 1000).toFixed(2)} s (retune ${(sp.retune_us / 1000).toFixed(1)} ms per step).`
            : `${sp.fps.toFixed(1)} frames/s from ${(100 * sp.coverage).toFixed(sp.coverage < 0.1 ? 2 : 1)} % of the samples ` +
              '(snapshots, not a continuous stream).');
}

// Display preferences (this browser only).
function bindPreference(id, key, apply) {
    const input = $(id);
    const read = () => (input.type === 'checkbox' ? input.checked
                        : typeof DEFAULTS[key] === 'number' ? Number(input.value) : input.value);
    if (input.type === 'checkbox') input.checked = prefs[key];
    else input.value = String(prefs[key]);
    const output = document.querySelector(`output[for="${id}"]`);
    if (output) output.textContent = input.value;
    input.addEventListener(input.type === 'range' ? 'input' : 'change', () => {
        prefs[key] = read();
        if (output) output.textContent = input.value;
        savePrefs();
        apply?.();
        dirty.plot = dirty.overlay = true;
    });
}
$('palette').replaceChildren(...Object.keys(PALETTES).map((name) => new Option(
    {sdrsharp: 'SDR#', classic: 'SDR# classic', turbo: 'Turbo', viridis: 'Viridis', gray: 'Gray'}[name], name)));
const applyPalette = () => {
    renderer.setPalette(prefs.palette);
    $('palette-swatch').style.background = paletteCss(prefs.palette);
};
applyPalette();
bindPreference('palette', 'palette', applyPalette);
bindPreference('s-attack', 'spectrumAttack');
bindPreference('s-decay', 'spectrumDecay');
bindPreference('w-attack', 'waterfallAttack');
bindPreference('w-decay', 'waterfallDecay');
bindPreference('channels', 'channels');
bindPreference('fill', 'fill');
bindPreference('peak-hold', 'peakHold', () => { if (spectrum.target) spectrum.peak = spectrum.display.slice(); });
bindPreference('contrast', 'contrast');
bindPreference('range', 'range');
bindPreference('offset', 'top');
bindPreference('max-fps', 'maxFps', sendView);
bindPreference('max-bins', 'maxBins', sendView);
$('peak-reset').addEventListener('click', () => {
    if (spectrum.display) spectrum.peak = spectrum.display.slice();
    dirty.plot = true;
});
$('clear-waterfall').addEventListener('click', () => {
    renderer.clearWaterfall();
    dirty.plot = true;
});
$('auto-levels').addEventListener('click', () => {
    if (!traceColumns.length) return;
    const values = [...traceColumns].filter((v) => v > NO_DATA).sort((a, b) => a - b);
    if (!values.length) return;
    const floor = values[Math.floor(values.length * 0.1)], peak = values[values.length - 1];
    prefs.top = clamp(Math.ceil((peak + 8) / 10) * 10, -160, 20);
    prefs.range = clamp(prefs.top - Math.floor((floor - 15) / 10) * 10, 10, 200);
    $('offset').value = String(prefs.top);
    $('range').value = String(prefs.range);
    savePrefs();
    dirty.plot = dirty.overlay = true;
});
$('zoom').addEventListener('input', () => {
    const f = fullSpan();
    if (!f) return;
    const z = Number($('zoom').value) / 1000;
    setView(view.offset, f.rate * Math.pow(minimumSpan() / f.rate, z));
});

// Sidebar and panels.
// On a narrow screen the controls cover the plot, so they start closed there.
const narrow = window.matchMedia('(max-width: 900px)');
let sidebarOpen = narrow.matches ? false : prefs.sidebar;
function showSidebar() {
    document.body.classList.toggle('sidebar-closed', !sidebarOpen);
    $('menu').setAttribute('aria-expanded', String(sidebarOpen));
}
$('menu').addEventListener('click', () => {
    sidebarOpen = !sidebarOpen;
    if (!narrow.matches) {
        prefs.sidebar = sidebarOpen;
        savePrefs();
    }
    showSidebar();
});
showSidebar();
for (const panel of document.querySelectorAll('details[data-panel]')) {
    panel.open = prefs.open[panel.dataset.panel];
    panel.addEventListener('toggle', () => {
        prefs.open[panel.dataset.panel] = panel.open;
        savePrefs();
    });
}

enableControls();
connect();
