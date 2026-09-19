// Scale Snap: a song.changed *reactor*. While "Snap new notes" is on,
// every note the user paints outside the chosen scale is moved to the
// nearest scale degree, as its own undo entry right after the user's.
//
// The listener has two halves. The observer half runs on every event and
// keeps `known` (the ids the song had at the last event) current, so the
// next event can tell which notes are new. The reactor half runs only for
// origin "user": reacting to "script" would loop on this plugin's own
// snap (the host faults a plugin that does), and reacting to "history"
// would destroy the redo entry the user just made.

var NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
var SCALES = {
    "Major": [0, 2, 4, 5, 7, 9, 11],
    "Natural minor": [0, 2, 3, 5, 7, 8, 10],
    "Harmonic minor": [0, 2, 3, 5, 7, 8, 11],
    "Pentatonic major": [0, 2, 4, 7, 9],
    "Pentatonic minor": [0, 3, 5, 7, 10],
    "Blues": [0, 3, 5, 6, 7, 10]
};
var SCALE_NAMES = Object.keys(SCALES);

var state = { root: 0, scale: "Major", snap: false };
var snapItem = null;
var known = {};   // note id -> true, as of the last song.changed

function load() {
    if (porydaw.song.loaded && porydaw.project.isOpen) {
        state.root = porydaw.storage.song.get("root", 0);
        state.scale = porydaw.storage.song.get("scale", "Major");
        if (!SCALES[state.scale]) state.scale = "Major";
    }
    // Snapping is a session choice, deliberately off at every song open.
    state.snap = false;
    if (snapItem) snapItem.checked = false;
    remember();
}

function save() {
    if (!porydaw.song.loaded || porydaw.song.readOnly || !porydaw.project.isOpen) return;
    porydaw.storage.song.set("root", state.root);
    porydaw.storage.song.set("scale", state.scale);
}

function remember() {
    known = {};
    if (!porydaw.song.loaded) return;
    var notes = porydaw.song.notes();
    for (var i = 0; i < notes.length; i++) known[notes[i].id] = true;
}

// Signed semitone step to the nearest scale degree (0 when in scale).
function snapDelta(key) {
    var steps = SCALES[state.scale];
    for (var d = 0; d <= 6; d++) {
        if (steps.indexOf(((key + d - state.root) % 12 + 12) % 12) >= 0) return d;
        if (steps.indexOf(((key - d - state.root) % 12 + 12) % 12) >= 0) return -d;
    }
    return 0;
}

function onChanged(e) {
    if (!porydaw.song.loaded) { known = {}; return; }
    var notes = porydaw.song.notes();
    var fresh = [];
    for (var i = 0; i < notes.length; i++) {
        if (!known[notes[i].id]) fresh.push(notes[i]);
    }
    remember();
    // Reactor half: only what the user just did.
    if (e.origin !== "user" || !state.snap || !fresh.length) return;
    var byDelta = {};
    for (var j = 0; j < fresh.length; j++) {
        var d = snapDelta(fresh[j].key);
        if (d === 0) continue;
        (byDelta[d] = byDelta[d] || []).push(fresh[j].id);
    }
    var deltas = Object.keys(byDelta);
    if (!deltas.length) return;
    // The roll selects a painted note by its (tick, key), which the snap is
    // about to change; ids survive the move, so re-select afterwards.
    var selected = porydaw.selection.notes().map(function (n) { return n.id; });
    porydaw.edit.transaction("Snap to scale", function () {
        for (var k = 0; k < deltas.length; k++)
            porydaw.edit.moveNotes(byDelta[deltas[k]], 0, Number(deltas[k]));
    });
    if (selected.length) porydaw.selection.setNotes(selected);
}

function chooseScale() {
    var answers = porydaw.ui.dialog.form({
        title: "Scale snap",
        fields: [
            { key: "root", label: "Root", type: "combo", items: NAMES, value: state.root },
            { key: "scale", label: "Scale", type: "combo", items: SCALE_NAMES,
              value: SCALE_NAMES.indexOf(state.scale) }
        ]
    });
    if (!answers) return;
    state.root = answers.root;
    state.scale = SCALE_NAMES[answers.scale];
    save();
    porydaw.ui.statusMessage("Snapping to " + NAMES[state.root] + " " + state.scale);
}

function setSnap(on) {
    state.snap = on;
    if (on) remember();
    if (snapItem && snapItem.checked !== on) snapItem.checked = on;
    porydaw.ui.statusMessage(on ? "Scale snap on" : "Scale snap off");
}

export function activate() {
    // The toggle is both a menu item and a bindable command.
    porydaw.actions.register({ id: "toggle", name: "Toggle scale snap", context: "global",
                               run: function () { setSnap(!state.snap); } });
    var menu = porydaw.ui.menu();
    menu.addItem({ label: "Choose scale…", run: chooseScale });
    snapItem = menu.addItem({ label: "Snap new notes", checkable: true, checked: false,
                              run: setSnap });
    porydaw.song.on("changed", onChanged);
    porydaw.song.on("activated", load);
    load();
}

export function deactivate() {}
