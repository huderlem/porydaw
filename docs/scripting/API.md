# porydaw scripting API v1

Plugins are written in JavaScript. Everything runs on the UI thread.

`docs/scripting/porydaw.d.ts` declares types for the entire scripting API. For editors that support TypeScript declarations, add `/// <reference path="…/porydaw.d.ts" />` or
point `jsconfig.json` at it to get autocomplete.

## Plugin layout

Installing, enabling and reloading plugins is covered in the manual's
[Plugins](../../docsrc/manual/plugins.md) page. A plugin is one folder in the
plugins folder:

```
plugins/
  select-same-pitch/
    plugin.json
    main.js
```

`plugin.json`:

```json
{ "id": "select-same-pitch", "name": "Select Same Pitch", "version": "1.0.0",
  "api": 1, "main": "main.js", "description": "…" }
```

- `id`: lowercase letters, digits, `-`, `_`; must exactly match the folder name.
- `api`: the API major version this plugin was written for. Porydaw will refuse to load any non-matching plugin api version.
- `main` (default `main.js`): an ES module exporting `activate(ctx)` and,
  optionally, `deactivate()`.

Editing any `.js`/`plugin.json` file in the folder causes porydaw to automatically reload the plugin.

`main.js`:

```js
export function activate(ctx) {  // ctx = {id, name, version, dir}
    porydaw.actions.register({
        id: "select", name: "Select notes of the same pitch",
        context: "roll", default: "Ctrl+Shift+A",
        run() {
          // Code goes here.
          // Use API calls like porydaw.selection, porydaw.edit, ...
        }
    });
}
export function deactivate() {}
```

## `porydaw`

`porydaw` is the top-level global object exposed to the scripting API.

| Member | Meaning |
|---|---|
| `porydaw.version` | porydaw's version string |
| `porydaw.api.version` / `.major` | API version (`"1.0.0"`, `1`) |
| `porydaw.plugin` | `{id, name, version, dir}` of the plugin |
| `porydaw.log(...)`, `.warn(...)`, `.error(...)` | Script Console lines (objects are JSON-stringified). `console.log` etc. alias these. |
| `porydaw.ui.statusMessage(text)` | Status-bar message |

### `porydaw.project`

`porydaw.project` exposes and operates on the currently-opened decomp project.

| Member | |
|---|---|
| `isOpen`, `root` | whether a project is open, and its folder |
| `songs()` | `[{id, label, constant, player, midPath, hasMid, registered, registrationGaps, settings}]`, every song of the project. |
| `song(label)` | one entry of `songs()`, or `null` |
| `open(label, {newTab?})` | opens the song in the active tab (or a new tab) |
| `registration(label)` | `{complete, inSongTable, inSongsH, inLdScript, inCharmap, inDebugMenu, gaps}` |
| `registerSong(label, {constant?, player?})` | same as `File → Register Songs` |
| `unregisterSong(label)` | |
| `reload()` | re-reads the project's music data |
| `musicPlayers()` | `[{name, number, trackCount}]` |
| `voicegroups()` | `[{arg, name}]`, every voicegroup, as its `-G` arg (`"_abandoned_ship"`) and display name (`"abandoned_ship"`) |
| `createVoicegroup(name, {copyFrom?})` | writes `sound/voicegroups/<name>.inc` (can be copied from an existing voicegroup) |

### `porydaw.song`

`porydaw.song` exposes a read-only view of the currently-active song tab. (See `porydaw.edit` for modifying the song.)

"Ticks" are the smallest resolution of time in porydaw (`ticksPerBeat` per quarter note). Note ids are unique to each note and valid for the entire life of the song document.

| Member | |
|---|---|
| `loaded` | whether any song is currently loaded |
| `revision` | increments on every edit/undo/redo to the song |
| `label` | the song's name/label |
| `midPath` | the filepath to the song's midi file |
| `ticksPerBeat` | number of ticks per beat |
| `ticksPerClock` | |
| `startTempo` | tempo at the start of the song (beats per minute) |
| `trackCount` | number of tracks |
| `trackBudget` | maximum number of playable tracks |
| `endTick` | the end-of-song tick, everything in the song lives at ticks before this |
| `settings()` | the song's `midi.cfg` settings: `{voicegroup, voicegroupName, masterVolume, reverb, priority, exactGate, extendedClocks, noCompression, flags}` — `voicegroup` is the `-G` arg, `reverb` is `null` while the `-R` flag is absent (the build then uses 50), `flags` the raw flag list |
| `save()` | same as `File → Save` |
| `loop()` | gets the loop region. `{start, end}` or `null` |
| `timeSigs()` | gets the time signature events `[{tick, numerator, denominator}]` |
| `tracks()` | gets the current state of the tracks `[{index, name, chunk, channel, muted, soloed, voice}]`. `chunk` is the track's SMF chunk index. |
| `notes({track?, from?, to?, selectedOnly?})` | gets a list of notes from the song. This is pretty flexible beacuse you get get the notes from a single or multiple tracks, optionally in a range, and optionally only the selected notes. `[{id, track, tick, key, len, vel}]`. `from`/`to` are ticks. |
| `note(id)` | get a note by id. Returns `null` if not found. |
| `lanePoints(track, cc, {from?, to?})` | Get a track's automation lane events. `[{tick, value}]`; `cc` is 0–127 or `song.CC.BEND` / `.TEMPO` / `.VOICE` / `.MOD` / `.VOLUME` / `.PAN` / `.BEND_RANGE` / `.LFO_SPEED` |
| `CC` | lane constants: the controllers porydaw draws as automation lanes, `MOD` (1), `VOLUME` (7), `PAN` (10), `BEND_RANGE` (20), `LFO_SPEED` (21), plus the pseudo-CCs for event-backed lanes, `BEND` (pitch bend), `TEMPO` (song-level, `track: -1`), `VOICE` (program changes). Anywhere a `cc` is accepted |
| `on("changed", fn({revision, origin}))` | event handler that's called whenever the song changed from an edit. `origin` is `"user"`, `"script"` or `"history"` (Edit → Undo/Redo). `revision` is the song's revision as the event reaches you. To make song edits in response to this event, you should read [Reacting to edits](#reacting-to-edits) first! |
| `on("activated", fn({label} \| null))` | the active song tab changed |
| `chunkCount`, `chunkTrack(chunk)`, `chunkEndTick(chunk)` | the file's MTrk chunks: the engine track a chunk is (-1 for the seq/tempo chunk and other trackless chunks) and its end-of-track tick |
| `rawEvents(chunk, {from?, to?})` | the chunk's MIDI events as they are in the file: `[{index, tick, status, type, channel, data0, data1, metaType, blob, text}]`. `type` is `noteOn`, `noteOff` (a note-on with velocity 0 too), `cc`, `program`, `bend`, `aftertouch`, `pressure`, `meta` or `sysex`; `blob` (bytes) and `text` (text metas 1–7) only for metas/sysex. `index` is the position in the chunk right now — it shifts with every edit |

Every `on()` returns a function that unsubscribes the listener. Calling `porydaw.song.off(event, fn)` also works.

### `porydaw.selection`

`porydaw.selection` exposes functionality related to the different kind sof selections.

| Member | |
|---|---|
| `track` | the selected track |
| `trackMask` | the multi-track selection as a bitmask |
| `notes()` | the selected notes, same shape as `porydaw.song.notes` |
| `setNotes(idsOrNotes)` | replaces the note selection with the given notes |
| `clear()` | clears the selection |
| `selectTrack(i)` | selects a track |
| `time()` | the time selection: `{start, end, scope: "tracks"\|"lanes", lanes: [{track, cc}]}`, or `null` |
| `setTime({start, end, scope?, lanes?})` | sets the time selection; `scope` is `"tracks"` (covers the header-selected tracks) or `"lanes"` with `lanes: [{track, cc}]` (`cc` may be `song.CC.TEMPO` with `track: -1`) |
| `clearTime()` | clears the time selection |

### `porydaw.cursor`

`porydaw.cursor` exposes functionality for the edit cursor and the piano roll's snap grid.

| Member | |
|---|---|
| `tick` | the edit cursor's tick position |
| `set(tick)` | moves the cursor (seeks while playing/paused) |
| `snap(tick, "nearest"\|"down"\|"up")` | gets the resulting tick after snapping it to the grid |
| `grid(tick?)` | the grid cell at `tick`. Returns `{start, next, beatTicks, feel, minDenom}` |

### `porydaw.view`

`porydaw.view` exposes functionality for the piano roll's camera and lane visibility.

| Member | |
|---|---|
| `visibleTicks()` | `{from, to}`, or `null` |
| `revealTick(tick)`, `revealRange(from, to)`, `revealKey(key)` | scroll so the tick, span or key row is on screen |
| `revealNote(idOrNote)` | scrolls to the given note |
| `velocityLane`, `automationLanes`, `tempoLane`, `eventList` | read/write booleans, whether each panel is currently being shown |
| `pxPerBeat` | pixel width per beat |
| `keyHeight` | pixel height of each key row |

### `porydaw.edit`

`porydaw.edit` is how you actually make edits to the song.

Every mutation happens inside a **transaction**:

```js
var ids = porydaw.edit.transaction("Insert chord", function () {
    var ids = porydaw.edit.addNotes(track, [{tick: 0, key: 60, len: 24, vel: 100},
                                            {tick: 0, key: 64, len: 24, vel: 100}]);
    porydaw.edit.moveNotes(ids, 48, 0);
    return ids;
});
```

- One transaction results in **one entry in Edit → Undo**.
- Calling any edit outside a transaction throws an error.

| Member | |
|---|---|
| `addNotes(track, [{tick, key, len, vel}])` | Adds a list of new notes to the track. Returns the resulting note ids, in order. |
| `deleteNotes(notes)` | Deletes the given note(s). |
| `moveNotes(notes, dTick, dKey)` | Moves the note(s) by delta ticks and delta keys. |
| `resizeNotes(notes, dLen, {fromLeft?})` | Resizes note(s). `dLen` is the change in length; `fromLeft` moves the start of the note instead of the end |
| `setVelocity(notes, vel)` / `setVelocity(notes, fn(note) → vel)` | Sets the velocity of the given note(s). |
| `nudgeVelocity(notes, delta)` | Changes the velocity of the note(s) by a delta. |
| `addLanePoint(track, cc, tick, value)` | Sets a CC value at the specified tick position. It replaces a point already at that tick. (To edit tempo: use -1 for `track`) |
| `writeLanePoints(track, cc, from, to, [{tick, value}])` | Replaces every CC point of the automation lane in `[from, to]` with the list (doesn't work with the Voice lane) |
| `moveLanePoints(track, cc, [{tick, newTick?, newValue?}])` | Moves the list of CC points, each to a new tick and value |
| `deleteLanePoints(track, cc, [ticks])` | Deletes the list of CC points |
| `setSettings({...})` | Partial update of `song.settings()` |
| `setVoice(slot, {...})` | Partial update of one voice in the song's voicegroup (`porydaw.voicegroup` below) |
| `setStartTempo(bpm)` | Sets the tempo at the start of the song (beats per minute) |
| `setLoop(start, end)` | Sets the loop markers. (`null` means "remove the marker") |
| `setTimeSig(tick, numerator, denominator)` | Sets the time signature at the given tick |
| `deleteTimeSig(tick)` | Deletes the time signature at the given tick |
| `removeTimeRange(start, end, scope)` | Ripple deletion of a range (anything after the range gets automatically shifted forward). `scope` is `{tracks: [i], lanes: [{track, cc}]}` or `{wholeSong: true}` |
|  `insertTimeRange(at, span, scope)` | Inserts a blank span of time (pushes the existing content to the right). `scope` is `{tracks: [i], lanes: [{track, cc}]}` or `{wholeSong: true}` |
| `addTrack(voice)` | Adds a new track with the given `voice`. Returns the index of the created track. |
| `duplicateTrack(i)` | Duplicates the given track |
| `deleteTrack(i)` | Deletes the given track |
| `mergeTrack(i, target, {notesOnly?})` | Merges track `i` into track `target`, then deletes track `i` |
| `moveTrack(i, target)` | Moves the track to a different index |
| `renameTrack(i, name)` | Renames the track |
| `transposeSelection(dKey)` | Moves the selected notes up or down a set number of keys |
| `nudgeSelection("left"\|"right")` | Nudes the selected notes right or left, identical to using the keyboard Left/Right arrow keys |
| `moveRange(start, end, scope, dTick)` | Moves all notes and events in the given range by `dTick` ticks |
| `duplicateRange(start, end, scope, dTick)` | Duplicate all notes and events in the given range `dTick` ticks from `start` |
| `insertRawEvent(chunk, event)`, `modifyRawEvent(chunk, index, event)`, `deleteRawEvents(chunk, [indices])`, `moveRawEvent(chunk, index, destIndex)`, `setChunkEndTick(chunk, tick)` | You probably don't want these because it's editing the raw MIDI itself!  Raw SMF edits in `song.rawEvents`' address space. `event` is `{tick, status}` or `{tick, type, channel?}` plus `data0`/`data1` (channel events), `metaType` and `blob` or `text` (metas), `blob` (sysex). Bytes are clamped, nothing is validated semantically. An orphan note-on is yours to make. Inserts land at the tick's canonical position; a modify that changes the tick re-inserts, so re-read indices afterwards. `moveRawEvent` reorders within one tick (clamped to what the file's ordering rules allow; → whether it moved). |

The example [`plugins/examples/note-tools`](https://github.com/huderlem/porydaw/blob/main/plugins/examples/note-tools) plugin (Legato, Insert chord,
Humanize, Strum, Quantize) demonstrate good usage of `porydaw.edit` patterns.

### `porydaw.transport`

`porydaw.transport` interacts with playback state. Includes the play/pause/stop state, the playhead, and realtime events that follow it.

| Member | |
|---|---|
| `state` | `"stopped"`, `"paused"` or `"playing"` |
| `playheadTick` | Where playback currently is |
| `sampleRate` | The audio device's rate |
| `loopEnabled` | Whether looping is on |
| `play()`, `pause()`, `stop()` | The transport buttons |
| `seek(tick)` | Moves the playhead to the given `tick` |
| `on("state", fn({state}))` | Callback invoked whenenever the transport state changes |
| `on("tick", fn({state, tick, playing}))` | Callback invoked every frame (~60fps), not literally every tick |
| `on("beat", fn({bar, beat, beatsPerBar, beatTicks, tick, bpm}))` | Callback invoked whenever the playhead enters a new beat of the song's meter (`bar` and `beat` are 0-based) |

Every `on()` returns a function that unsubscribes the listener.

### `porydaw.audio`

`porydaw.audio` exposes the final stereo mix as the device hears it. It also provides a WAV render, and the GBA engine settings from Settings → Audio.

| Member | |
|---|---|
| `sampleRate` | The device's sample rate |
| `windowFrames` | The sample analysis window (2048 frames) |
| `peak`, `rms` | The most recent frame's left/right peak and root-mean-squared volumes |
| `pcm()` | `Float32Array` of the newest `windowFrames` pcm frames, interleaved L/R, oldest first |
| `spectrum(bins = 64)` | `Float32Array` of `bins` bands, linear in frequency from 0 to `sampleRate / 2`, each 0..1 (1 = a full-scale sine there); a Hann-windowed FFT of the window's mono mix |
| `channels()` | The current state of the m4a engine's channel pools |
| `on("frame", fn(frame))` | Callback invoked for every audio frame, with `{peak: [l, r], rms: [l, r], frames, sampleRate, playing}`. `peak`/`rms`. `frames` is how many there were. |
| `render(path, {sampleRate?, loopCount?, fadeout?, tail?})` | Renders the active song to a WAV file the same way `File → Export WAV` does. |
| `engine` | The current global m4a engine settings. `{maxPcmChannels, pcmMixRate, analogFilter}` |
| `engineLimits()` | `{maxPcmChannels, mixRates}`. The m4a polyphony ceiling and the selectable sample rates |
| `setEngine({maxPcmChannels?, pcmMixRate?, analogFilter?})` | Partial update the m4a engine settings |
| `on("engine", fn(settings))` | Callback invoked when any of the m4a engine settings are changed |

See these example plugins for reference: [`plugins/examples/vu-meter`](https://github.com/huderlem/porydaw/blob/main/plugins/examples/vu-meter), [`plugins/examples/spectrum`](https://github.com/huderlem/porydaw/blob/main/plugins/examples/spectrum),
[`plugins/examples/dancer`](https://github.com/huderlem/porydaw/blob/main/plugins/examples/dancer) (beats), [`plugins/examples/song-report`](https://github.com/huderlem/porydaw/blob/main/plugins/examples/song-report) (render).

### `porydaw.ui`

`porydaw.ui` exposes porydaw's window, status bar, theme colors, images for canvases, and the visual things a plugin can add, like docks, menus, roll overlays and dialogs.

| Member | |
|---|---|
| `statusMessage(text)` | Shows a status-bar message |
| `theme(role?)` | Gets the color of one theme role |
| `loadImage(path)` | Decodes an image file (PNG, JPEG, …). Returns an image id for use with `g.image()` |
| `imageSize(id)` | Gets the size of an image. `{width, height}`, or `null` |
| `freeImage(id)` | Releases the image |
| `dock(spec)` | Creates a dock panel. Returns the dock handle |
| `menu()` | Gets the plugin's submenu in the **Plugins** menu |
| `contextMenu("notes" \| "range")` | Gets the plugin's entries in the piano roll's context menus (when right-clicking on notes or range) |
| `overlay({id, paint(g, v)})` | Creates an overlay drawing that is painted on top of the piano roll |
| `dialog.alert`, `.confirm`, `.prompt`, `.form`, `.openFile`, `.saveFile`, `.chooseDir` | Shows a modal dialog |

Theme roles:

| Group | Roles |
|---|---|
| Window | `window_background`, `window_text`, `secondary_text`, `disabled_text`, `selection_background`, `selection_text`, `link_text`, `palette_outline` |
| Lists and headers | `item_background`, `item_text`, `item_alternate_background`, `header_background`, `header_text` |
| Buttons and tooltips | `button_background`, `button_text`, `tooltip_background`, `tooltip_text` |
| Polyphony panel | `polyphony_cell_active_background`, `polyphony_cell_releasing_background`, `polyphony_cell_free_background`, `polyphony_cell_shadow_background`, `polyphony_flash_background` |
| Piano roll | `song_view_piano_roll_background`, `song_view_grid`, `song_view_separator`, `song_view_primary_text`, `song_view_secondary_text`, `song_view_selection_fill`, `song_view_selection_edge`, `song_view_playhead`, `song_view_edit_cursor`, `song_view_loop_marker`, `song_view_automation_default_curve`, `song_view_automation_tempo_curve` |
| Sample editor | `sample_waveform_ink` |

#### `porydaw.ui.dock(spec)`

A panel next to Songs / Voicegroup / Polyphony. `spec`:

| Field | Meaning |
|---|---|
| `id` | The id of the dock panel. Allows letters, digits, `_`, `-`. The window remembers the dock's placement per `plugin.<pluginId>.<id>`, and whether the user closed it. |
| `title` | The dock panel's title |
| `area` | `"right"` (default), `"left"`, `"top"`, `"bottom"`. The initial placement. |
| `minWidth`, `minHeight` | Minimum body size (defaults 120 × 60) |
| `paint(g)` | If `paint(g)` is provided, the dock panel is one canvas painted by this function (see below) |
| `build(root)` | If `build(root)` is provided, the dock panel display widgets inside of it, built on `root`, which is a column container (see below) |

The dock handle: `id`, `title` (read/write), `visible` (read/write),
`open` (false after `close()`), `show()`, `hide()`, `raise()`, `close()`,
`root` (the column), `canvas` (the shorthand canvas, else `null`). Docks are
closed automatically on unload/reload; call `close()` from `deactivate()`
anyway so a plugin that stays loaded can tidy up.

Containers (`root`, `addRow()`, `addColumn()`) build children in order:

| Call | Widget / handle |
|---|---|
| `addLabel(text)` | label; `text` |
| `addButton(text, onClick())` | button; `text`, `enabled` |
| `addCheckbox(text, checked, onChange(on))` | checkbox; `checked` |
| `addSlider(min, max, value, onChange(v), {vertical?})` | integer slider; `value` |
| `addCombo(items, index, onChange(i))` | drop-down; `index`, `text`, `setItems(items)` |
| `addCanvas({minWidth?, minHeight?}, paint(g), mouse(ev)?)` | canvas; `repaint()`, `width`, `height` |
| `addRow()`, `addColumn()` | nested containers |
| `addStretch()`, `addSpacing(px)` | layout filler |

Every handle also has `kind`, `visible`, `enabled`, `setMinimumSize(w, h)`,
`setToolTip(text)`. Callbacks (`onChange` etc.) don't fire for changes the
script makes itself. Plugin widgets never take keyboard focus, so the roll's
shortcuts keep working with a panel open.

#### Canvas painting

`paint(g)` runs on the UI thread whenever the canvas repaints, which is after
`repaint()`, on resize, and on theme-change. Coordinates are device-independent pixels from the canvas's top-left (`g.width`, `g.height`, `g.dpr`). Colors are CSS strings (`"#f80"`, `"#ff8800"`, `"#ff880080"`, `"rgb(255,136,0)"`, `"rgba(255,136,0,0.5)"`, names) or `[r, g, b, a?]` arrays.

| `g.` | |
|---|---|
| `clear(color)` | fill everything (ignores transforms) |
| `fillRect(x, y, w, h, color)`, `strokeRect(x, y, w, h, color, lineWidth)`, `fillRoundRect(x, y, w, h, radius, color)` | |
| `line(x1, y1, x2, y2, color, lineWidth)` | |
| `fillCircle(cx, cy, r, color)`, `strokeCircle(cx, cy, r, color, lineWidth)`, `fillEllipse(x, y, w, h, color)` | |
| `fillPolygon(points, color)`, `strokePolyline(points, color, lineWidth, close)` | `points`: `[x0, y0, x1, y1, …]` or `[{x, y}, …]` |
| `text(x, y, str, color, {size?, bold?, align?, baseline?})` | `size` multiplies the UI font (1 = as is); `align` `"left"|"center"|"right"`; `baseline` `"alphabetic"` (default, y is the baseline) `|"top"|"middle"|"bottom"` |
| `measureText(str, opts)` | `{width, height, ascent, descent}` |
| `image(id, dx, dy, dw, dh, sx, sy, sw, sh)` | a `ui.loadImage` image id; `sx, sy, sw, sh` selects a source rectangle from within the image (useful for sprite sheets) |
| `save()`, `restore()`, `translate(dx, dy)`, `rotate(degrees)`, `scale(sx, sy)`, `opacity(0..1)`, `clip(x, y, w, h)`, `antialias(on)` | painter state; unbalanced saves are restored for you |

`mouse(ev)` receives `{type, x, y, button, left, right, middle, deltaX?, deltaY}` with `type`
one of `press`, `move`, `release`, `doubleclick`, `leave`, or `wheel`.

#### Menus

`porydaw.ui.menu()` is the plugin's own submenu in the **Plugins** file menu. `porydaw.ui.contextMenu("notes" | "range")` creates a new context menu whose items are appended to the piano roll's note right-click context menu or the time-selection right-click context menu. Both return a menu handle, with the following members:

| Menu Handle Members | |
|---|---|
| `addItem({label, run(checked), shouldShow()?, action?, checkable?, checked?, enabled?, tooltip?})` | Creates a new menu item. `shouldShow` is queried every time the menu opens, return False to prevent the item from showing. `action` links the item to a command created via `actions.register` (specify its `id`). |
| `addSeparator()` | Appends a horizontal separator bar |
| `addMenu(label)` | Creates a nested menu item (can't use in context menus) |
| `clear()` | removes every entry from the menu |
| `label` | Human-friendly name for the menu |
| `enabled` | Whether the menu is enabled. (When disabled, it's greyed out). |
| `visible` | Whether the menu is visible |

See the example plugin [`plugins/examples/range-tools`](https://github.com/huderlem/porydaw/blob/main/plugins/examples/range-tools) for working examples of menu items.

#### Piano Roll Overlays

`porydaw.ui.overlay({id, paint(g, v)})` creates a drawing overlay that is painted on
top of the active song's note area. `paint` runs whenever the roll repaints
(scroll, zoom, edits, `repaint()`), with the canvas painter `g` (documented above) and the piano roll geometry `v`.

| Overlay Handle Members | |
|---|---|
| `id` | The overlay's id |
| `visible` | Whether the overlay paints (read/write) |
| `active` | Whether the overlay is still attached to the roll (readonly) |
| `repaint()` | Asks the piano roll to repaint |
| `remove()` | Removes the overlay (and sets `active = false`) |

The roll geometry:

| `v.` | |
|---|---|
| `width`, `height` | The note area's size. (0, 0) is the top-left corner |
| `from`, `to` | The visible tick span |
| `x(tick)`, `tick(x)` | Convert between horizontal pixel and tick positions |
| `keyTop(key)`, `keyBottom(key)`, `key(y)` | Convert between vertical pixel and piano key positions |
| `keyHeight` | Vertical pixel height of a single piano key row |
| `pxPerBeat` | Horizontal pixel width of a single beat |
| `track` | The selected track |

Keep paints cheap because they re-run as fast as the user scrolls. See the example plugin [`plugins/examples/scale-guide`](https://github.com/huderlem/porydaw/blob/main/plugins/examples/scale-guide) for a working example of an overlay.

#### Dialogs

`porydaw.ui.dialog` shows modal dialogs, which the user can interact with.

| Call | |
|---|---|
| `dialog.alert(text, {title?, detail?, ok?})` | Shows a simple message |
| `dialog.confirm(text, {title?, detail?, ok?, cancel?})` | Asks a yes/no question. Returns `true` if the user presses the ok button |
| `dialog.prompt(text, {title?, value?, ok?, cancel?})` | Asks for one line of text. Returns the string, or `null` when canceled. |
| `dialog.form({title?, text?, ok?, cancel?, fields})` | Asks for several values. `fields: [{key, label?, type: "text"\|"number"\|"checkbox"\|"combo", value?, min?, max?, step?, decimals?, items?, placeholder?}]`. Returns `{key: value}` (`combo` gives the index) or `null` when canceled. |
| `dialog.openFile({title?, dir?, filter?})` | File picker. Returns the chosen filepath or `null`. |
| `dialog.saveFile({title?, dir?, filter?, name?})` | File save. Returns the chosen filepath or `null`. |
| `dialog.chooseDir({title?, dir?})` | Folder picker. Returns the the chosen directory path or `null`. |

See the example plugins [`plugins/examples/song-report`](https://github.com/huderlem/porydaw/blob/main/plugins/examples/song-report) and [`plugins/examples/range-tools`](https://github.com/huderlem/porydaw/blob/main/plugins/examples/range-tools) for working examples of dialogs.

!!! warning
    Dialogs are NOT allowed inside an edit transaction and from paint callbacks.

### `porydaw.voicegroup`

`porydaw.voicegroup` exposes a readonly view of the current song's voicegroup. (To edit, use `porydaw.edit.setVoice()`, described above.)

| Member | |
|---|---|
| `isOpen`, `arg`, `name`, `file`, `loadName`, `dirty` | the `-G` arg and display name (from the song's settings), the file path, the name the engine loads it by, whether unsaved voice edits exist |
| `voices()` | Gets all 128 voice slots. Each is `{slot, kind}` |
| `voice(slot)` | Gets one voice |
| `symbols()` | Gets the project's instrument symbols: `{directSound, progWave, drumkits, synths, keysplits: [{voicegroup, table}]}` |
| `typicalAdsr(type, symbol?)` | Gets the default `{attack, decay, sustain, release}` ADSR for the specified voice kind |

See example plugin [`plugins/examples/project-tools`](https://github.com/huderlem/porydaw/blob/main/plugins/examples/project-tools) for a working example of `porydaw.voicegroup` interaction.

### `porydaw.io`

`porydaw.io` reads and writes files. It's sandboxed, so a path must be inside the plugin's folder (relative paths count from there), the open project, or something the user picked through `ui.dialog.openFile/saveFile/chooseDir`.

| Member | |
|---|---|
| `pluginDir`, `projectRoot` | The two folders a relative path can resolve against |
| `resolve(path)` | Gets an absolute path, or throws error if it's outside of the sandbox |
| `exists(path)` | Checks if path exists |
| `isDir(path)` | Checks if path is a folder |
| `readText(path)` / `writeText(path, text)` | Reads/writes UTF-8 text. Any necessary parent folders are created on write |
| `readBytes(path)` / `writeBytes(path, bytes)` | Reads/writes a file as `Uint8Array` (or `ArrayBuffer` to write) |
| `list(dir)` | Gets the contents of a folder. Each item is `[{name, dir, size}]` |
| `mkdir(path)` | Creates the folder (and its parents, if needed) |
| `remove(path)` | Deletes a file (files only) |

### `porydaw.actions`

`porydaw.actions` registers commands that show up in `Settings → Keyboard Shortcuts`, so users can bind and rebind them like any built-in command. Registered commands are also what a menu item's `action` links to.

| Member | |
|---|---|
| `register({id, name, context?, default?, run})` | Registers a command. Returns the full `id` of the created command |
| `unregister(fullId)` | Removes a command by `id` |

The `register` spec:

| Field | Meaning |
|---|---|
| `id` | The command's id, unique within the plugin |
| `name` | The display name, shown in `Settings → Keyboard Shortcuts` |
| `context` | `"global"` (window-wide), `"roll"`, `"velocity"`, or `"range"` (only while a time selection is active) |
| `default` | a portable key sequence (`"Ctrl+Shift+A"`) |
| `run()` | called when the command is triggered |

!!! warning
    A `default` keybind that collides with an existing binding in an overlapping context is dropped with a warning, and the command will be unbound to any shortcut. The user can still manually set the keybind, though.

### `porydaw.storage`

`porydaw.storage` is a per-plugin key/value store of JSON-serializable values, persisted in porydaw's settings.

| Member | |
|---|---|
| `get(key, fallback)` | the stored value, or `fallback` when the key is absent |
| `set(key, value)` | stores a JSON-serializable value |
| `remove(key)` | removes the key |
| `keys()` | every key the plugin has stored |

`porydaw.storage.song` has the same four calls for values that belong to the
**song**: they live in the song's sidecar (`<project>/.porydaw/<song>.json`,
under `plugins` → the plugin id), next to the view state, and are written
immediately.

| Member | |
|---|---|
| `song.get(key, fallback)`, `song.set(key, value)`, `song.remove(key)`, `song.keys()` | the same store, scoped to the active song |

See the example plugin [`plugins/examples/scale-guide`](https://github.com/huderlem/porydaw/blob/main/plugins/examples/scale-guide), which remembers each song's scale
via `porydaw.storage`.

## Reacting to edits

A `song.changed` listener that edits the song in response to what the user did (e.g. snapping freshly painted notes to a scale) should start with:

```js
porydaw.song.on("changed", function (e) {
    if (e.origin !== "user") return;
    porydaw.edit.transaction("Snap to scale", function () { /* … */ });
});
```

Without guarding on `e.origin`, the script could enter an infinite loop, where it keeps making edits in response to its *own* edits.
