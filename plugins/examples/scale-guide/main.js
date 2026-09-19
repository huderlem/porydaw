// Scale Guide: a piano-roll overlay that shades every row outside the
// song's scale, so out-of-key notes stand out. The root and scale live in
// the song's sidecar (porydaw.storage.song), chosen from the Plugins menu
// or by right-clicking a note ("Use as scale root").

var NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
var SCALES = {
    "Major": [0, 2, 4, 5, 7, 9, 11],
    "Natural minor": [0, 2, 3, 5, 7, 8, 10],
    "Harmonic minor": [0, 2, 3, 5, 7, 8, 11],
    "Pentatonic major": [0, 2, 4, 7, 9],
    "Pentatonic minor": [0, 3, 5, 7, 10],
    "Blues": [0, 3, 5, 6, 7, 10],
    "Chromatic": [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]
};
var SCALE_NAMES = Object.keys(SCALES);

var overlay = null, showItem = null, state = { root: 0, scale: "Major", show: true };

function load() {
    if (!porydaw.song.loaded || !porydaw.project.isOpen) return;
    state.root = porydaw.storage.song.get("root", 0);
    state.scale = porydaw.storage.song.get("scale", "Major");
    state.show = porydaw.storage.song.get("show", true);
    if (!SCALES[state.scale]) state.scale = "Major";
    if (showItem) showItem.checked = state.show;
    if (overlay) overlay.repaint();
}

function save() {
    if (!porydaw.song.loaded || porydaw.song.readOnly || !porydaw.project.isOpen) return;
    porydaw.storage.song.set("root", state.root);
    porydaw.storage.song.set("scale", state.scale);
    porydaw.storage.song.set("show", state.show);
}

function inScale(key) {
    var steps = SCALES[state.scale];
    return steps.indexOf(((key - state.root) % 12 + 12) % 12) >= 0;
}

function paint(g, v) {
    if (!state.show) return;
    var shade = '#ff0000';
    var topKey = v.key(0), bottomKey = v.key(v.height - 1);
    for (var key = bottomKey; key <= topKey; key++) {
        if (inScale(key)) continue;
        g.opacity(0.5);
        g.fillRect(0, v.keyTop(key), v.width, v.keyBottom(key) - v.keyTop(key), shade);
    }
    g.opacity(0.85);
    g.text(6, 14, NAMES[state.root] + " " + state.scale, porydaw.ui.theme("secondary_text"),
           { size: 0.9, bold: true });
}

function chooseScale() {
    var answers = porydaw.ui.dialog.form({
        title: "Scale guide",
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
    overlay.repaint();
}

function rootFromNote() {
    var notes = porydaw.selection.notes();
    if (!notes.length) return;
    state.root = notes[0].key % 12;
    save();
    overlay.repaint();
    porydaw.ui.statusMessage("Scale root: " + NAMES[state.root]);
}

export function activate() {
    overlay = porydaw.ui.overlay({ id: "scale", paint: paint });
    var menu = porydaw.ui.menu();
    menu.addItem({ label: "Choose scale…", run: chooseScale });
    showItem = menu.addItem({ label: "Show scale guide", checkable: true, checked: true,
                              run: function (checked) {
                                  state.show = checked;
                                  save();
                                  overlay.repaint();
                              } });
    porydaw.ui.contextMenu("notes").addItem({ label: "Use as scale root", run: rootFromNote });
    porydaw.song.on("activated", load);
    load();
}

export function deactivate() {
    overlay.remove();
}
