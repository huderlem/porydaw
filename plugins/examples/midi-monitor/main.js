// MIDI Monitor: a dock panel showing what the connected MIDI keyboards send,
// through porydaw.midi. It only listens; the keyboard keeps playing and
// recording into porydaw as usual. Click the panel to clear it.
//
//   porydaw.midi.ports()          the ports, as Settings → MIDI lists them
//   porydaw.midi.on("ports", fn)  the list changed (plugged in, turned off…)
//   porydaw.midi.on("message", fn({status, data1, data2, port}))

var NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
var LOG_SIZE = 12;

var dock = null, canvas = null, offs = [];
var ports = [];
var log = [];        // newest first: {text, detail, port, color}
var held = {};       // key -> velocity, while the key is down
var sustained = {};  // key -> velocity, released under the pedal
var sustain = false;
var bend = 8192, mod = 0, pressure = 0;
var program = null;
var lo = 36, hi = 96; // the keyboard's range (C2..C7), widened to fit

function noteName(key) { return NAMES[key % 12] + (Math.floor(key / 12) - 1); }
function isBlack(key) { return [1, 3, 6, 8, 10].indexOf(key % 12) >= 0; }

function addLog(text, detail, m, color) {
    log.unshift({ text: text, detail: detail, channel: (m.status & 0x0F) + 1, port: m.port, color: color });
    if (log.length > LOG_SIZE) log.length = LOG_SIZE;
}

function fit(key) {
    // Widen by whole octaves so the played key is on the keyboard.
    while (key < lo) lo -= 12;
    while (key > hi) hi += 12;
    lo = Math.max(0, lo);
    hi = Math.min(127, hi);
}

function noteOff(key) {
    if (!(key in held)) return;
    if (sustain) sustained[key] = held[key];
    delete held[key];
}

function onMessage(m) {
    var type = m.status & 0xF0;
    switch (type) {
    case 0x90:
        if (m.data2 > 0) {
            fit(m.data1);
            delete sustained[m.data1];
            held[m.data1] = m.data2;
            addLog("Note on", noteName(m.data1) + "  vel " + m.data2, m, "#3fb950");
            break;
        }
        // A note on with velocity 0 is a note off.
        noteOff(m.data1);
        addLog("Note off", noteName(m.data1) + "  (note on, vel 0)", m, "#8b949e");
        break;
    case 0x80:
        noteOff(m.data1);
        addLog("Note off", noteName(m.data1) + "  vel " + m.data2, m, "#8b949e");
        break;
    case 0xA0:
        addLog("Key pressure", noteName(m.data1) + "  " + m.data2, m, "#a371f7");
        break;
    case 0xB0:
        if (m.data1 === 1) mod = m.data2;
        if (m.data1 === 64) {
            sustain = m.data2 >= 64;
            if (!sustain) sustained = {};
        }
        if (m.data1 === 120 || m.data1 === 123) { held = {}; sustained = {}; }
        addLog("Control", ccName(m.data1) + "  = " + m.data2, m, "#d29922");
        break;
    case 0xC0:
        program = m.data1;
        addLog("Program", String(m.data1), m, "#58a6ff");
        break;
    case 0xD0:
        pressure = m.data1;
        addLog("Pressure", String(m.data1), m, "#a371f7");
        break;
    case 0xE0:
        bend = m.data1 | (m.data2 << 7);
        addLog("Pitch bend", String(bend - 8192), m, "#f778ba");
        break;
    }
    if (canvas) canvas.repaint();
}

function ccName(n) {
    var known = { 1: "Mod wheel", 7: "Volume", 10: "Pan", 11: "Expression", 64: "Sustain",
                  120: "All sound off", 121: "Reset controllers", 123: "All notes off" };
    return known[n] ? "CC " + n + " " + known[n] : "CC " + n;
}

function onPorts(list) {
    ports = list;
    if (canvas) canvas.repaint();
}

// ---- painting ----

function drawPorts(g, t, x, y, w) {
    if (!ports.length) {
        var why = porydaw.midi.available ? "No MIDI keyboard connected." : "No MIDI system on this computer.";
        g.text(x, y, why, t.secondary_text, { baseline: "top" });
        return y + 20;
    }
    var cx = x;
    ports.forEach(function (p) {
        // Green: receiving. Red: enabled but couldn't be opened. Grey: turned off.
        var color = p.open ? "#3fb950" : p.failed ? "#f85149" : t.disabled_text;
        var label = p.name + (p.open ? "" : p.failed ? " (unavailable)" : " (off)");
        var width = g.measureText(label, {}).width + 22;
        if (cx > x && cx + width > x + w) { cx = x; y += 18; }
        g.fillCircle(cx + 5, y + 7, 4, color);
        g.text(cx + 14, y, label, p.open ? t.window_text : t.secondary_text, { baseline: "top" });
        cx += width;
    });
    if (porydaw.midi.dropped > 0)
        g.text(x + w, y, porydaw.midi.dropped + " dropped", "#f85149", { baseline: "top", align: "right" });
    return y + 20;
}

function drawKeyboard(g, t, x, y, w, h) {
    var whites = [];
    for (var k = lo; k <= hi; k++) if (!isBlack(k)) whites.push(k);
    var kw = w / whites.length;
    var at = {}; // white key -> its x
    whites.forEach(function (k, i) {
        at[k] = x + i * kw;
        var vel = held[k], sus = sustained[k];
        var fill = vel !== undefined ? velColor(vel, 1) : sus !== undefined ? velColor(sus, 0.45) : "#f4f4f4";
        g.fillRect(at[k], y, kw - 1, h, fill);
        if (k % 12 === 0 && kw > 9)
            g.text(at[k] + kw / 2, y + h - 3, noteName(k), "#57606a", { size: 0.75, align: "center", baseline: "bottom" });
    });
    for (var b = lo; b <= hi; b++) {
        if (!isBlack(b) || at[b - 1] === undefined) continue;
        var bx = at[b - 1] + kw * 0.65, bw = kw * 0.7;
        var v = held[b], s = sustained[b];
        g.fillRect(bx, y, bw, h * 0.6, v !== undefined ? velColor(v, 1) : s !== undefined ? velColor(s, 0.45) : "#24292f");
    }
    g.strokeRect(x, y, w - 1, h, t.palette_outline, 1);
}

// Soft keys pale, hard keys deep.
function velColor(vel, alpha) {
    var f = vel / 127;
    return "rgba(" + Math.round(255 - 200 * f) + "," + Math.round(200 - 60 * f) + "," + Math.round(120 + 100 * f) + "," + alpha + ")";
}

function drawMeter(g, t, x, y, w, label, value, max, centered, color) {
    g.text(x, y + 6, label, t.secondary_text, { size: 0.85, baseline: "middle" });
    var bx = x + 58, bw = w - 58 - 36;
    g.fillRoundRect(bx, y, bw, 12, 3, t.item_alternate_background);
    if (centered) {
        var mid = bx + bw / 2, end = bx + bw * value / max;
        g.fillRect(Math.min(mid, end), y + 2, Math.abs(end - mid), 8, color);
        g.line(mid, y, mid, y + 12, t.secondary_text, 1);
    } else {
        g.fillRect(bx, y + 2, bw * value / max, 8, color);
    }
    var shown = centered ? value - max / 2 : value;
    g.text(x + w, y + 6, String(shown), t.window_text, { size: 0.85, align: "right", baseline: "middle" });
}

function drawControls(g, t, x, y, w) {
    var half = (w - 12) / 2;
    drawMeter(g, t, x, y, half, "Bend", bend, 16384, true, "#f778ba");
    drawMeter(g, t, x + half + 12, y, half, "Mod", mod, 127, false, "#d29922");
    y += 18;
    drawMeter(g, t, x, y, half, "Pressure", pressure, 127, false, "#a371f7");
    var px = x + half + 12;
    g.fillRoundRect(px, y, 64, 12, 6, sustain ? "#3fb950" : t.item_alternate_background);
    g.text(px + 32, y + 6, "Sustain", sustain ? "#ffffff" : t.secondary_text, { size: 0.8, align: "center", baseline: "middle" });
    if (program !== null)
        g.text(x + w, y + 6, "Program " + program, t.window_text, { size: 0.85, align: "right", baseline: "middle" });
    return y + 22;
}

function drawLog(g, t, x, y, w, bottom) {
    if (!log.length) {
        g.text(x, y, "Play something on your keyboard.", t.secondary_text, { baseline: "top" });
        return;
    }
    var row = 17;
    for (var i = 0; i < log.length && y + row <= bottom; i++, y += row) {
        var e = log[i];
        g.save();
        g.opacity(1 - i / (LOG_SIZE + 2)); // older entries fade
        g.fillCircle(x + 4, y + 7, 3, e.color);
        g.text(x + 12, y, e.text, t.window_text, { baseline: "top", bold: i === 0 });
        g.text(x + 100, y, e.detail, t.window_text, { baseline: "top" });
        g.text(x + w, y, "ch " + e.channel + (e.port ? "  ·  " + e.port : ""), t.secondary_text,
               { baseline: "top", align: "right", size: 0.85 });
        g.restore();
    }
}

function paint(g) {
    var t = porydaw.ui.theme();
    g.clear(t.window_background);
    var pad = 8, x = pad, w = g.width - pad * 2;
    if (w < 120) return;
    var y = drawPorts(g, t, x, pad, w);
    drawKeyboard(g, t, x, y, w, 46);
    y = drawControls(g, t, x, y + 54, w);
    g.line(x, y, x + w, y, t.palette_outline, 1);
    drawLog(g, t, x, y + 6, w, g.height - pad);
}

export function activate() {
    ports = porydaw.midi.ports();
    dock = porydaw.ui.dock({
        id: "monitor", title: "MIDI Monitor", area: "right", minWidth: 300, minHeight: 300,
        paint: paint,
        mouse: function (ev) {
            if (ev.type !== "press") return;
            log = []; held = {}; sustained = {}; sustain = false;
            bend = 8192; mod = 0; pressure = 0; program = null;
            lo = 36; hi = 96;
            canvas.repaint();
        }
    });
    canvas = dock.canvas;
    offs.push(porydaw.midi.on("message", onMessage));
    offs.push(porydaw.midi.on("ports", onPorts));
}

export function deactivate() {
    offs.forEach(function (off) { off(); });
    offs = [];
    if (dock) dock.close();
    dock = canvas = null;
}
