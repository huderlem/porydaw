# Scripting / Plugins — Scoping & Plan

Status: **decided 2026-09-02; Phases 0–2 landed (2026-09-03)**. Facts about the
current codebase (file/line refs) were verified against `scripting` branch tip
`52428aa`; the **DECIDE** items below were resolved as follows:

| Item | Decision |
|---|---|
| §2 runtime | **JavaScript via `QJSEngine`** (`Qt6::Qml`). |
| §5 widget layer | **B first**: canvas + widget primitives in Phase 3, no `Qt6::Quick` link. Qt Quick docks (A) may be added in Phase 4 if demand appears; the dancing companion already shipped natively (`b428748`), so archetype 2 no longer needs plugin UI on day one. |
| §3 per-project plugin dir | **No.** Plugins load from the user-global dir only. |
| §4 raw SMF edits | **Not permission-gated**; available like every other undoable edit. The manifest keeps `permissions` only for `io.*`. |
| §9 scheduling | Start now on branch `scripting`. |

Environment facts found while deciding (they correct §2's "already installed"):

- The Linux dev box has Qt **6.2.4** (Ubuntu 22.04) with only `qt6-base-dev`;
  `Qt6::Qml` needs `qt6-declarative-dev` (also 6.2.4). CI installs 6.9 via
  `install-qt-action`, whose default essentials include qtdeclarative.
- The Windows static kit `/mnt/d/Qt6/Static/6.9.0` already ships
  `Qt6Qml`, `Qt6Quick`, `Qt6QuickWidgets` and the `qml/` module plugins, so
  the static-link risk in §9 is mostly retired before the spike; what
  remains to measure is exe size (`libQt6Qml.a` is 31 MB, `libQt6Quick.a`
  35 MB on disk).
- Phase 0 therefore links only `Qt6::Qml` behind `PORYDAW_SCRIPTING`
  (default ON) and proves: evaluate, `Q_INVOKABLE` bridge, error line
  numbers, and `setInterrupted` from a watchdog thread. The Windows static
  build must be done by hand (CI has no static Qt, `docs/RELEASING.md`).

Goal: a plugin system powerful enough for, at minimum, these three archetypes —

1. **Editing macros** — hotkeys that edit notes/automation relative to the edit
   cursor and/or the current note/time selection (e.g. "legato selection",
   "insert chord at cursor", "humanize velocities", "select every C#").
2. **Reactive visuals** — a Fruity-Dance-style avatar that animates to the beat
   / transport state.
3. **Custom analysis widgets** — realtime meters, spectrum, per-channel
   activity, i.e. panels fed by audio-rate data at frame rate.

Plus a fourth the survey makes obvious: **project automation** (batch
voicegroup edits, bulk registration, import/export in other formats).

---

## 1. Design stances

1. **One language, one host.** A single embedded runtime with one API surface
   (`porydaw.*`). No native `.dll/.so` plugins (ABI, crashes, no sandbox);
   no second language "for the easy cases".
2. **Scripts are UI-thread citizens.** All script execution happens on the Qt
   UI thread. The audio callback never touches the script engine; scripts see
   audio only through snapshots copied out of lock-free state (same model as
   `polySnapshot`, `playheadSamples()` today, `src/audio/audioengine.h:47-62`).
3. **Every document edit is an undo step.** Scripts mutate the song only
   through `SongDocument`'s existing public edit API, inside a script
   transaction that collapses to one `QUndoStack` entry named after the
   action. No raw pointers to notes cross the boundary — scripts hold
   `NoteId`s (`src/core/noteid.h`) and re-resolve via `findNote`, exactly
   as the UI does.
4. **Commands are keymap citizens.** A script-registered action becomes a
   normal `keymap::Registry` entry (`plugin.<pluginId>.<actionId>`) with a
   context, category, and optional default binding, so it shows up in
   Settings → Keyboard Shortcuts, participates in conflict detection, and
   dispatches through `SongView::handleEditKey` like every other bare-letter
   command (GROUND-RULES rule: never window shortcuts).
5. **Trusted-but-contained.** Plugins are code the user chose to install (the
   porymap model). We don't promise a security sandbox, but the API is
   capability-shaped: no ambient filesystem/process/network access; file IO is
   an explicit API scoped to the plugin dir and the project root, and
   declared in the manifest so the Plugins page can show it.
6. **Developer loop first.** Hot reload on file change, a script console dock
   (log + errors with stack traces + REPL), and a watchdog for runaway
   scripts. Without these the API is unusable no matter how rich it is.
7. **API versioned from day one.** `porydaw.api.version` (semver), manifest
   declares `"api": ">=1.0"`; breaking changes bump major and the loader
   refuses with a clear message. Ship a `porydaw.d.ts` for editor
   autocompletion and generate the reference docs (`docsrc/`) from it.

## 2. Runtime choice — **DECIDE**

| | JavaScript via `QJSEngine` (Qt::Qml) | Lua 5.4 (vendored, ~30 files, MIT) |
|---|---|---|
| New dependency | none — a Qt module we already have installed (6.9) | one small vendored C lib |
| Static-Qt Windows build | must link `Qt6::Qml` statically — known to work, but Phase 0 must prove it on the local static build (`CMakeLists.txt:236-258` static detection) | zero risk |
| Qt integration | native: `QObject`/`Q_INVOKABLE`/signals/`QJSValue` callbacks bind for free; `QJSEngine::setInterrupted` gives a watchdog | needs a binding layer (sol2 or hand-written) for every type |
| Custom widgets | opens the door to **Qt Quick/QML** for plugin UI (Canvas, AnimatedSprite, animations, layouts) — see §5 | only what we hand-build (canvas API) |
| Audience | JS is the most widely known language; **porymap (same author) already ships a QJSEngine scripting API**, so conventions, docs style and user expectations transfer directly | familiar to modders; PICO-8/Roblox/Renoise/REAPER precedent |
| Performance | fine for macros and 60 Hz callbacks if we pass typed arrays and do FFT/RMS in C++ | faster interpreter, same caveat |
| Risks | QJSEngine is ES7-ish (no modules until 6.x `import` support, which exists but is awkward), GC of `QJSValue` callbacks needs care, `Qt6::Qml` adds ~5 MB | Lua string/table ergonomics for UI-heavy plugins are worse; two "how do I…" doc sets (Lua + our API) |

**Recommendation: JavaScript / `QJSEngine`.** The porymap precedent alone is
worth a lot (users of one will use the other), and it's the only option that
makes "custom widgets" mean real widgets rather than a painter API. The Lua
fallback is kept only if Phase 0 shows static `Qt6::Qml` is unworkable on
Windows.

Rejected: Python (distribution/embedding pain on three platforms, large),
WASM (no ecosystem for this audience), native plugins (stance 1).

## 3. Architecture

New layer between UI shell and Document in the SPEC §3 diagram:

```
UI shell ──▶ ScriptHost (src/scripting/) ──▶ Document / Sequencer / Project adapter
                │
                ├─ PluginManager      discovery, manifest, enable/disable, reload, watchdog
                ├─ Engine             one QJSEngine per plugin (isolation: one plugin's
                │                     exception/hang can't take the others down)
                ├─ Api facades        QObject wrappers exposed as `porydaw.*` (below)
                ├─ ScriptConsole      dock: log/errors/REPL
                └─ PluginsPage        Settings → Plugins
```

Modules (all under `src/scripting/`):

| Module | Responsibility |
|---|---|
| `pluginmanifest.{h,cpp}` | Parse `plugin.json`; validate api range; permissions list. |
| `pluginmanager.{h,cpp}` | Scan plugin dirs, own `Plugin{manifest, QJSEngine, state, errors}`, load/unload/reload, file watcher (`QFileSystemWatcher`), watchdog timer (`setInterrupted` after N ms of a single call; configurable, default 5 s, off while a debugger flag is set). |
| `scriptapi_*.{h,cpp}` | One `QObject` facade per namespace (`app`, `project`, `song`, `edit`, `selection`, `view`, `transport`, `audio`, `ui`, `actions`, `io`, `storage`). Thin — they call into `SongDocument` / `SongView` / `AudioEngine` / `MainWindow`. |
| `scripttransaction.{h,cpp}` | `edit.transaction(name, fn)` — begins a `QUndoStack::beginMacro`, runs `fn`, `endMacro`; rethrows and rolls back (undo the macro) on exception. Nested transactions flatten. |
| `audiotap.{h,cpp}` | Audio→UI ring of the final interleaved mix (post `m_outputGain`) written from `AudioEngine::process`, plus C++ analysis helpers (peak/RMS per channel, N-bin magnitude FFT) computed once per UI frame and shared by all subscribers. |
| `scriptconsole.{h,cpp}` | Dock. |
| `pluginspage.{h,cpp}` | Settings page (list, enable toggle, reload, open folder, per-plugin error badge, "Open plugins folder"). |
| `scriptcheck.cpp` | Harness `--scriptcheck` (see §8). |

Plugin lifetime: each plugin exports `activate(ctx)` / `deactivate()`.
Everything registered in `activate` (actions, docks, subscriptions, menu
items) is tracked by a per-plugin `Disposables` list and torn down on
deactivate/reload — no leaked `QAction`s or keymap entries after a hot
reload (cf. the dangling-`QTreeWidgetItem` gotcha in the keymap page).

Packaging:

```
<QStandardPaths::AppDataLocation>/plugins/<id>/plugin.json   # user-global
<projectRoot>/.porydaw/plugins/<id>/plugin.json              # per-project (Sidecar::ensureDir)
```

```json
{ "id": "legato-tools", "name": "Legato Tools", "version": "1.0.0",
  "api": ">=1.0 <2", "main": "main.js", "ui": ["meter.qml"],
  "permissions": ["io.project", "io.plugin"] }
```

**DECIDE:** per-project plugin dir — yes (song-specific tooling, shareable in
the decomp repo) but it must be opt-in per project ("this project wants to
load 3 plugins — Allow / Never") since opening a stranger's repo shouldn't run
code.

## 4. API surface (v1)

Sketch, not final signatures. Ticks are document ticks (`ticksPerClock`
exposed); tracks are engine track indices as the UI shows them; values are
plain JS objects, never live handles, except `NoteId` (opaque number).

```
porydaw.api.version, porydaw.log(...), porydaw.version

porydaw.project     root, songs[] {id,label,constant,midPath,registered}, open(songId)
porydaw.song        (current tab; also porydaw.songs[] for all open tabs)
  .revision, .ticksPerBeat, .startTempo, .loop{start,end}, .timeSigs[]
  .tracks[] {index, name, voice, muted, soloed}
  .notes({track?, from?, to?, selectedOnly?}) → [{id, track, tick, key, len, vel}]
  .note(id), .lane(track, cc).points({from,to}) → [{tick, value}]
  .rawEvents(track, {from,to})        // read-only view of the SMF, for exotic tools
  .on('changed', fn)                  // documentChanged → (revision)
porydaw.edit        (all inside .transaction(name, fn); throws outside one)
  .addNotes([...]), .deleteNotes([ids]), .moveNotes([ids], dTick, dKey),
  .resizeNotes([ids], dLen, fromLeft?), .setVelocity([{id, vel}]),
  .setLanePoints(track, cc, [{tick,value}], {replaceRange?}),
  .deleteLanePoints(...), .removeTimeRange(...), .duplicateRange(...),
  .addTrack(voice), .renameTrack(i, name), .setStartTempo(bpm), .setLoop(...)
  .rawInsert/Modify/Delete(...)       // gated by "raw" permission; still undoable
porydaw.selection   .notes → ids, .setNotes(ids), .clear(),
  .time → {start,end,scope,lanes}|null, .setTime(...), .track (selected track),
  .on('changed', fn)
porydaw.cursor      .tick (edit cursor), .set(tick), .snap(tick, 'down'|'up'|'nearest'),
  .grid → {ticks, feel, denom}
porydaw.view        .scroll{x,y}, .zoom{h,v}, .visibleTicks{from,to}, .revealTick(),
  .laneVisible(cc), .setLaneVisible(cc,bool), .notesInRect? (later)
porydaw.transport   .state, .play()/.pause()/.stop(), .seek(tick), .playheadTick,
  .tempoAt(tick), .on('tick', fn) (frame rate), .on('beat', fn({bar,beat,tick})),
  .on('state', fn)
porydaw.audio       .sampleRate, .on('frame', fn(frame)) where frame =
  {peak:[l,r], rms:[l,r], pcm: Float32Array (last N interleaved), fft(bins)},
  .channels() → PolySnapshot-shaped {pcm:[{on,releasing,track,key}], cgb:[...]}
  .previewNote(track,key,vel,ms), .previewVoice(...)
porydaw.actions     .register({id, name, category, context:'roll'|'global'|'range'|
  'velocity', default:'Ctrl+Shift+L', run(ctx)}) ; ctx = {song, selection, cursor}
porydaw.ui          .dock({id,title,area, qml|widget}), .menu(path).add(action),
  .contextMenu('roll'|'range'|'track').add(action), .statusMessage(text),
  .dialog.prompt/confirm/form({...}), .toast()
porydaw.io          .readText/writeText/exists/list within plugin dir or project root
porydaw.storage     .plugin (JSON, QSettings-backed) / .song (JSON in the view
                    sidecar, `<root>/.porydaw/<song>.json` "plugins" key)
```

Notes on how these map onto existing code:

- `song.*`/`edit.*` are 1:1 with `SongDocument` (`src/core/songdocument.h`):
  `addNotes/deleteNotes/moveNotes/resizeNotes/setNotesVelocities/writeLanePoints/
  applyRangeEdit/removeTimeRange/duplicateRange/insertRawEvent…`. The
  transaction wrapper is what makes N calls one undo entry; every call still
  goes through `publishMutation`, so revision, `documentChanged`, and the
  timeline snapshot swap for the audio thread all keep working unchanged.
  Optimistic-concurrency variant (`setNotesVelocities(expectedRevision, …)`)
  becomes the default for scripts: a transaction captures `revision()` at
  start and refuses to commit if it moved (a user edit interleaved via a
  nested event loop — e.g. a dialog).
- `selection.*`/`cursor.*`/`view.*` are `SongView` accessors
  (`selection()/setSelection`, `timeSelection`, `editCursorTick/
  commitEditCursor`, `snapTick*`, `gridSegAt`, `viewState`). The range ops
  already living on SongView (`transposeTimeSelection`, `pasteRangeAtEditCursor`,
  …) can be exposed directly; the note-selection ops buried in the private
  `PianoRoll` class (`transposeSelection` songview.cpp:2442, `nudgeSelection`
  :2476) get hoisted to SongView as part of Phase 2 — a good cleanup anyway.
- `transport.*` wraps `AudioEngine` transport atomics + the existing
  `synchronizePlayhead` 17 ms timer (`mainwindow.cpp:274-278`); `on('tick')`
  is driven from that timer, `on('beat')` derives bar/beat from the
  `MidiTimeline` tempo map crossing between two ticks.
- `audio.on('frame')` needs the one engine-side addition: a tap in
  `AudioEngine::process` after gain (a small SPSC ring of the interleaved
  float mix, ~200 ms). Analysis (peak/RMS/FFT) runs once per UI frame in C++
  regardless of subscriber count and is handed to JS as typed arrays. A tiny
  in-house radix-2 FFT (≈80 lines) — no dependency.
- `actions.register` → `keymap::Registry` needs a small extension: today
  `kDefs[]` is a static table (`src/ui/keymap.cpp:30-175`); add
  `registerDynamic(Def)/unregisterDynamic(id)` + the Shortcuts page grouping
  dynamic entries under a "Plugins" category. Bindings persist under the
  same `keymap/<id>` QSettings key so user rebinds survive reload.

## 5. Custom widgets — **DECIDE (gated on Phase 0)**

Two viable designs; both accept a dock spec from `ui.dock(...)` and end up
inside a `QDockWidget` next to Songs/Voicegroup/Polyphony
(`mainwindow.cpp:607-752`), with a plugin-owned `id` so `windowState`
restore works.

**A. Qt Quick (`QQuickWidget`) + QML files shipped by the plugin.** Preferred.
The plugin's JS objects are injected as context properties, so a meter is
literally `Rectangle { height: porydaw.audio.rms[0] * parent.height }` and a
dancing avatar is an `AnimatedSprite` whose `frameRate` is bound to tempo.
Costs: link `Qt6::Quick`/`QuickWidgets` (+ the QML `QtQuick` static plugins
on the Windows static build), ~15 MB, and a QML runtime inside a Widgets app
(fine; QQuickWidget is designed for this). Themeing: expose the resolved
`theme` roles as a context property so plugins can match the current theme.

**B. Widget-primitive + canvas API.** `ui.dock({build(root)})` where `root`
offers `label/button/slider/checkbox/combo/row/column` (mapped to real
`QWidget`s) and `canvas({paint(g, w, h)})` whose `g` is a `QPainter` facade
(`rect/line/text/image/path`, `drawImage` of a pre-decoded sprite sheet).
Repaint on `requestFrame()`. No new Qt modules. Covers both example use cases
adequately, but every future "can I have a…" needs a C++ binding.

Plan: do **A** if Phase 0's static-Qt spike passes; otherwise **B**. Even
under A, ship the `canvas` primitive from B, because a 30-line JS meter that
needs no `.qml` file is the on-ramp most users want.

Piano-roll **overlays** (paint over the roll — scale highlighting, chord
names, per-note annotations) are a separate, later capability
(`ui.overlay('roll', {paint(g, viewport)})`) hooked into `PianoRoll::paintEvent`
after notes and before the playhead. Valuable, but it touches the paint
caching (SPEC §7.1) so it's Phase 4.

## 6. Phases

Each phase is independently landable, adds a `--scriptcheck` section, and
lands a CHANGELOG entry + SPEC §3/§6 update.

**Phase 0 — Spike.** Linux result 2026-09-02: `--scriptcheck` PASS on Qt
6.2.4 (evaluate, ES6 arrows/array methods, `Q_INVOKABLE`/`Q_PROPERTY`
bridge, `QJSValue` callback from C++, bridge survives `collectGarbage`,
error `lineNumber` + URL-ified `fileName` + `stack`, engine usable after an
exception, `setInterrupted` from a `QThread` returns `evaluate()` in ~150 ms,
engine usable after clearing the flag). The watchdog assertion's 5 s cap is
enforced by the watchdog thread itself (`_Exit(1)`), negative-tested by
disabling the interrupt. Size delta on the Linux shared build: +25 KB
(5,896,432 → 5,921,368 bytes; `libQt6Qml.so` is loaded dynamically).
Windows static build measured by the user (2026-09-03): the exe is
about 85 MB with `PORYDAW_SCRIPTING=ON`, a ~20 MB increase over OFF —
judged very reasonable, so the §2 Lua fallback is retired for good. Gotcha: after adding
`src/scriptcheck.cpp` to an already-configured tree, automoc did not scan
it (`scriptcheck.moc: No such file`) until `build/porydaw_autogen` and
`CMakeFiles/porydaw_autogen.dir/ParseCache.txt` were deleted.

Original scope (≈2–3 days). Link `Qt6::Qml` (and `Qt6::Quick` +
`QuickWidgets`) behind `PORYDAW_SCRIPTING=ON`; hello-world `QJSEngine`; one
`QQuickWidget` in a dock; **build and run on the Windows static Qt** and inside
the macOS/Linux release pipelines (`release.yml` uses linuxdeploy-plugin-qt /
macdeployqt — both handle Qml, but the AppImage needs the qml dir). Measure
binary size delta. Decides §2 fallback and §5 A/B. Exit criteria written down
in this doc.

**Phase 1 — Host + read-only API + actions.** LANDED 2026-09-02 on branch
`scripting` (uncommitted): `src/scripting/{pluginmanifest,scripthost,
scriptapi,scriptconsole,pluginspage}` + `prelude.js` (the JS side of the
API, a Qt resource), keymap `registerDynamic/unregisterDynamic` +
`commandsChanged`, `SongView::setPluginKeyHandler`, `--scriptcheck [root]
[label]` (fixture plugins generated into a temp dir: discovery, states,
actions in the keymap/window/key handler/Shortcuts page, console, disable,
hot reload with rebind persistence, watchdog, and the song half). Deviations
from the sketch below: plugins are ES modules (`export function activate
(ctx)`), events use `porydaw.<ns>.on(event, fn)` returning an unsubscribe
function, `song.activated`/`transport.state` events exist, `selection.
setNotes/clear/selectTrack` and `cursor.set` shipped early (view state, no
undo entry), and `ui.dock`/`io.*`/menus wait for Phase 3/4. Bundled example:
`plugins/examples/select-same-pitch`. Reference: `docs/scripting/API.md`.
Code-reviewed 2026-09-02, 8 fixes applied (Plugins-page checkbox
use-after-free → queued rebuild; watchdog = stack of armed calls + wait
condition so nested cross-plugin calls keep their own budgets; key handler
takes the focused surface's context; `song.activated` fires after the
engine swap; plugin defaults also checked against shipped *defaults*
(`Registry::defaultConflicts`); folders that lose their `plugin.json` are
dropped + reload timers freed; script ticks/ids clamped (`clampTick`,
NaN → 0); module load errors reach the console). Open follow-ups: cache
per-id bindings so `Registry::matches` stops hitting QSettings per key
(shipped commands have the same cost); `SelectionApi::notes/setNotes` call
`findNote` per id (rebuilds `notesForTrack` each time) — hoist
`PianoRoll::resolveSelection` to a public SongView helper in Phase 2.

Original scope (≈1–2 weeks).
`PluginManager`, manifest, per-plugin engine, hot reload, watchdog, Script
Console dock, Settings → Plugins page, `porydaw.log/app/project/song(read)/
selection(read)/cursor(read)/transport(read+control)/actions.register/
ui.statusMessage/storage`. Keymap `registerDynamic`. Bundled example:
"Select all notes of same pitch as selection". Harness: load/reload/unload a
fixture plugin, action appears in `Registry`, dispatches through
`handleEditKey`, watchdog interrupts an infinite loop, error surfaces in
console.

**Phase 2 — Editing API.** LANDED 2026-09-03 on branch `scripting`:
`porydaw.edit.*` (`EditApi`), `porydaw.view.*` (`ViewApi`),
`selection.setTime/clearTime`, the transaction machinery
(`ScriptHost::{begin,commit,rollback}Transaction`, `EditTransaction`)
over a new `SongDocument::beginEditGroup/endEditGroup` (one `QUndoStack`
macro; a discarded or empty group is undone and dropped from the redo
side via an obsolete no-op push, so the stack looks untouched), and
`PianoRoll::transposeSelection/nudgeSelection/resolveSelection` hoisted
to public `SongView` methods (the roll keeps its audition/repaint
wrapper). Guards: optimistic revision check per call and at commit,
re-entrancy from `song.changed` listeners refused, transactions from
inside the `song.changed` fan-out refused (without this a plugin
starting one during an undo corrupts the stack — the harness negative
segfaults), one open transaction at a time, forced rollback when the
watchdog interrupts an open one or the owner unloads. Bundled example
`plugins/examples/note-tools` (Legato, Insert chord, Humanize, Strum,
Quantize; Ctrl+Shift+L/K/H/U/G). Harness section `runEditChecks` in
`--scriptcheck`: refusal outside a transaction, one entry per
transaction with byte-identical SMF after undo, empty/nested/swallowed
cases, revision guard (a `harness` bridge injected into the console
engine makes a C++ edit mid-transaction), listener re-entrancy, the
watchdog inside a transaction, the kitchen-sink of every call, and each
Note Tools command; four assertions negative-tested. Found and fixed a
Phase 1 bug: a faulted Script Console never got a fresh engine
(`evalConsole` saw the stale Error state). Code-reviewed 2026-09-03, all
8 findings fixed: the edit-group macro now opens lazily on the first
push (an eager `beginMacro` deleted the user's redo list on a no-op or
failed transaction), Escape refused as a plugin default and ignored by
the plugin key handler, track arguments validated as real integers in
the prelude (`trackArg`) and in C++ scope/lane parsing (`variantTrack`),
`addNotes` rejects non-object entries and resolves same-(tick, key)
batch entries last-wins, `selection.setNotes` accepts a single note,
events skip an interrupted engine (a rollback's `song.changed` fan-out
logged one bogus "Interrupted" per undone child), and `resolveNotes`
does one sweep instead of `findNote` per id. Five more harness
assertions cover these, negative-tested for the lazy macro. Deferred: `moveRange/
duplicateRange` (need SongView's private range gathering), raw SMF
edits, `song.rawEvents` (Phase 4). ← **archetype 1 ships here.**

Original scope (≈1–2 weeks). `edit.transaction` + all mutation
calls, `selection.set*`, `cursor.set`, `view.*`, revision guard, hoist
`PianoRoll::transposeSelection/nudgeSelection` to SongView. Bundled
examples: *Legato*, *Insert chord at cursor*, *Humanize velocities*,
*Strum selection*, *Quantize to grid*. Harness: each example runs against a
fixture song, produces exactly one undo entry, undo restores byte-identical
SMF (`--smfcheck` style), exception mid-transaction leaves the document
unchanged.

**Phase 3 — Realtime + widgets.** LANDED 2026-09-03 on branch `scripting`:
`AudioTap` (src/audio/audiotap.h, always compiled) — a lock-free SPSC ring
of the final stereo mix written at the end of `AudioEngine::process` after
the output gain, plus `AudioAnalyzer` (UI side: peak/RMS over the frames
since the last poll, a 2048-frame window, in-house radix-2 FFT folded into
N bands on demand and cached per poll). `ScriptHost::pumpFrame` runs on the
host's own 17 ms timer only while some plugin listens (the prelude reports
listener counts per event through `host.subscribed`) and emits
`audio.frame` `{peak, rms, frames, sampleRate, playing}`, `transport.tick`
and `transport.beat` (beat index from `MidiTimeline::barPositionForTick`;
re-fires on play start, loop wrap, seek). `porydaw.audio` (`AudioApi`):
`pcm()`/`spectrum(bins)` as `Float32Array` over a `QByteArray`→ArrayBuffer
bridge, `channels()` from `polySnapshot`. `porydaw.ui.dock` (design B,
`scriptwidgets.{h,cpp}`): `DockHandle` + `WidgetHandle` (label/button/
checkbox/slider/combo/row/column/canvas over real QWidgets, no keyboard
focus) + `CanvasWidget` whose `paint(g)` runs through `ScriptHost::invoke`
(watchdog, error logged then held back 1 s) with a `Painter` facade
(rects/lines/circles/polygons/text/images/transforms, CSS colors),
`ui.theme(role)` (curated role table), `ui.loadImage` scoped to the plugin
folder. Docks join the window through `HostBindings::addDock`
(`MainWindow::addPluginDock`: `restoreDockWidget` *before* `addDockWidget`,
title strip, View → Plugin Panels; a user's close is remembered under
`plugins/docks/<objectName>/closed` since Qt restores docks hidden and
reports success even with nothing saved). Examples: `vu-meter`,
`spectrum` (build(root) with a slider), `dancer` (the app logo bouncing on
beats; `dancer.png` is `resources/porydaw-128.png`). Harness: `runTapCheck`
(ring wrap/overrun/padding, FFT of DC, half-scale sine → bin 8 / ~0.5),
`runPanelChecks` (every widget primitive + callbacks, image pixel probe,
placement restore through saveState/restoreState across an unload,
close-button/menu/programmatic-hide persistence), `runDockChecks` (console
dock pixel probe, `g` refused outside paint, mouse + wheel delivery, theme,
five refusals, throwing paint logged once, hung paint → watchdog fault →
docks torn down → console revives), `runRealtimeChecks` (timer gating,
non-zero RMS + ≥2 beats while the fixture song plays through the null
device, typed-array shapes, beat restart on play, examples parked and
re-enabled). ← **archetypes 2 and 3 ship here.**

**Phase 4 — Reach.** First slice LANDED 2026-09-03 on branch `scripting`:
menus (`ui.menu()` → the plugin's submenu of a new menu-bar **Plugins**
menu, hidden while empty; items link to registered commands and show the
binding as a `\t` display hint since a real shortcut on a menu-bar action
would fire window-wide), context menus (`ui.contextMenu("notes"|"range")`:
`SongView::setPluginMenuProvider` appends every plugin's items behind a
separator when the note menu or the time-selection menu opens; the
long-lived note menu drops the previous open's plugin actions first),
dialogs (`ui.dialog.alert/confirm/prompt/form/openFile/saveFile/chooseDir`,
refused inside a transaction and from `song.changed`; `Watchdog::pause/
resume` suspends the innermost armed call's deadline while a modal
dialog or a render runs), `porydaw.io` (sandboxed to the plugin folder,
the project, and dialog-picked paths — `Plugin::grantedPaths`),
`storage.song` (the song sidecar's `plugins`/<id> object, merged with the
view state and registration keys), `song.rawEvents`/`chunkCount`/
`chunkTrack`/`chunkEndTick` + `edit.insertRawEvent/modifyRawEvent/
deleteRawEvents/moveRawEvent/setChunkEndTick` (ungated, per the 2026-09-02
decision), `edit.moveRange/duplicateRange` (the facade gathers notes and
every lane of the scoped tracks itself, so SongView's private gathering
stays private), `project.open(label, {newTab})`, `audio.render(path, opts)`
(the Export WAV path without its dialogs, `MainWindow::renderActiveSongWav`),
and piano-roll overlays (`ui.overlay({id, paint(g, v)})`: `SongView::
setPluginOverlayPainter` is called from `PianoRoll::paintContent` after the
built-in overlays with a `RollOverlayGeometry` of the roll's own snapped
mappings; `OverlayHandle` reuses the canvas `Painter`, now fed its dpr
explicitly). New module `src/scripting/scriptmenus.{h,cpp}`; every handle
is parented under `Plugin::uiRoot`, deleted before the engine on teardown.
Examples: `song-report` (menu + form + saveFile + io + render),
`range-tools` (range context menu, one entry linked to a rebindable
command), `scale-guide` (overlay + per-song storage + note context menu).
Harness `runReachChecks`: drives the Plugins menu, both context menus
(a synthesized right-click on a note opens the real note menu; the
exec()'d range menu is caught by a 0 ms timer — offscreen it dismisses
itself within ~20 ms), every dialog kind (modal driver from a timer; the
watchdog budget is shortened to prove the pause), file pickers (typed
into `fileNameEdit` + `accept()` — `selectFile` leaves a focused field
alone and `done()` reports the directory), the io sandbox (system file,
`..` escape, console-relative), sidecar round trip through a view-state
save, raw inserts/modify/move/delete with byte-identical undo, range
moves on a fresh track, overlay pixel probes and geometry, a real render
sized against `wavExportTotals`, and `project.open` both ways. Negative-
tested: watchdog pause removed, sandbox disabled, overlays never painted.
Full normal + ASAN sweeps PASS; OFF build compiles. Code-reviewed
2026-09-03 (8 findings, all fixed): teardown during a plugin's own nested
event loop deferred via `Plugin::callDepth` (guarded() finishes the
pending teardown/reload when the call unwinds — negative-tested: without
it ASAN SEGVs inside QML on the disable-during-dialog check),
`paintOverlays` iterates a copy (a paint may remove/add overlays;
in-paint invalidations are re-issued next turn since the cache commits
after paintContent), dialogs/render/open refused from paint callbacks
(`m_paintDepth`), `setSession` re-activates on an in-place song swap
(label compare) and `project.open` of the current song is a no-op,
`moveRange/duplicateRange` floor the delta at `-start` and gather only
the lanes a chunk actually has (one scan), `Painter` sizes text from a
fixed base font and restores the painter font (overlay text no longer
inherits the note-name font; overlay `clear()` blends inside the grid
transform), menu `remove()`/`clear()` defer deletes (safe from the entry's
own run()), and the note menu's per-open separator leak is closed.

Second slice LANDED 2026-09-03 (adapter writes + voicegroup API):
`porydaw.project.{song, registration, registerSong, unregisterSong,
reload, musicPlayers, voicegroups, createVoicegroup}` and `songs()[].
{registrationGaps, settings}` — the Register Song / New Voicegroup steps
minus their dialogs, through new `HostBindings` (`registerSong`,
`unregisterSong`, `reloadProject`, `saveSong`, `createVoicegroup`,
`voicegroupCatalog`, `typicalAdsr`, `editVoice`) backed by
`MainWindow::{registerSongByLabel, unregisterSongByLabel,
createVoicegroupNamed, pushVoiceEdit}` (the interactive actions now call
the same helpers; `reloadProject` gained an error out-param);
`song.settings()` / `song.save()`; `edit.setSettings` (partial cfg merge
→ `SongDocument::setCfg`, voicegroup by arg or display name, validated
against the catalog) and `edit.setVoice` (partial `VgVoice` merge, fields
the macro doesn't write refused — a `voice_keysplit_all` slot silently
dropped an `attack` before that rule — family change adopts the typical
envelope, values clamped; pushed through `SongDocument::pushCommand`,
now public, so the transaction macro takes the `VoiceEditCommand` and
rollback/undo cover it; `replayVoiceEdits` now recurses into macro
children, or a transaction's voice edit vanished after a -G switch and
back); new facade `VoicegroupApi` (`porydaw.voicegroup`: source
properties, `voices()`/`voice(slot)` from the session's
`VoicegroupSource`, `symbols()`, `typicalAdsr()`). Example
`project-tools` (registration audit, apply envelope to sample voices).
Harness `runAdapterChecks` (settings round trips, voicegroup switch by
display name with the source following — gated on audio, since
`onDocumentChanged` bails without it — voice edit as one transaction
entry, rollback, structural type change in range, replay across a
switch, validation negatives, `song.save()` writing the voicegroup byte-
identically after the edit back, `createVoicegroup` + switch to the copy,
register/unregister of a copied `.mid` with the active session keeping
its id). Negative-tested: voice push bypassing `pushCommand`, replay
without recursion, settings accepting unknown voicegroups, registerSong
without the reload. `docsrc/reference/scripting.md` is generated from
`API.md` by `tools/gen_scripting_docs.py` (`--check` in
`tools/run_checks.sh` and CI).

Code-reviewed 2026-09-03 (8 findings, all fixed): `replayVoiceEdits` also
walks the OPEN transaction macro (`SongDocument::editGroupMacroOpen()`;
`QUndoStack::index()` doesn't count it, so a `setVoice` before a `-G`
switch and back in the same transaction was lost — negative-tested),
`reverb: undefined` throws instead of dropping `-R` (an invalid QVariant
is not JS `null`; negative-tested), `registerSong({constant: null})`
means the default and the constant must be an identifier, a family
crossing with a partial envelope starts from the typical one and overlays
the given keys, the player check only applies to a player other than the
song's own tabled one (a re-register on an exotic layout never fails on
it) and reads `DecompProject::musicPlayers()` (cached), a reload failure
after a successful registration write is a status-bar message rather than
a failed registration (`reloadProjectOrWarn`), `newVoicegroup` calls
`createVoicegroupNamed`, and `SongRegistry::registrationGaps(status)`
feeds both `SongInfo::registrationGaps` and `project.registration().gaps`.

MIDI input landed later (docs/midi-input/PLAN.md); its step 6 added
`porydaw.midi`: `on("message")` delivers every channel message a keyboard
sends, as sent, with the port's name, through a script tap (a second ring
from the MIDI threads, drained once per queued hop on the UI thread and
running only while some plugin listens); `ports()` and `on("ports")` list
the ports. Input only (no `send`); listeners observe and can't suppress
or alter what live play and recording get.

## 7. Other plugin categories the API should not preclude

- **Generators/transformers**: arpeggiator, chord tools, humanize, strum,
  echo/delay (as note copies), scale-lock, velocity curves, CC LFO drawers.
- **Analysis**: polyphony-overflow finder over the song (uses `polySnapshot`
  history), unused-voice report, mid2agb-compat lint (things the exporter
  will clamp), "which tracks exceed the budget" — surfaced as console output
  or a dock.
- **Import/export**: other trackers' formats, MML/text notation → notes,
  song → text for diffs, batch render via `TimelinePlayer`.
- **Project maintenance**: bulk voicegroup edits, sample dedup reports,
  registration audits — all through the existing adapter APIs.
- **Live input**: MIDI keyboard input through `porydaw.midi.on('message')`
  (chord detectors, MIDI monitors, controller-driven tools).
- **Cosmetic**: theme generators (write `theme/*` QSettings), custom note
  colouring (`ui.overlay`), status-bar widgets.
- **Workflow**: macro recorder (record a sequence of `edit.*` calls into a
  script), per-song "plugin state" (e.g. a chord-progression sketchpad
  stored in the song sidecar).

## 8. Testing

- `src/scriptcheck.cpp`, `--scriptcheck <decomp-root>`, following the
  `check(cond, msg)` / PASS-or-exit-1 convention (`src/keymapcheck.cpp`
  tail), added to `tools/run_checks.sh` and swept under ASAN. Fixture
  plugins live in `src/testdata/plugins/`, loaded from a `QTemporaryDir`
  copy so hot reload can be exercised by rewriting the file.
- Every assertion negative-tested (GROUND-RULES). Watchdog test must prove
  the UI thread is released (run with a wall-clock cap).
- Bundled example plugins double as API smoke tests and as the documentation
  samples — they must stay green in CI or the docs lie.

## 9. Risks and open questions

| Risk | Mitigation |
|---|---|
| Static `Qt6::Qml`/`Quick` on the local Windows build | Phase 0 spike; fallback Lua + widget-primitive API |
| `songview.cpp` churn (10.9k lines, private surface classes) vs. a stable script API | Facades only touch `SongView`'s public accessors; hoisting note-selection ops out of `PianoRoll` reduces coupling |
| Per-frame JS callbacks stuttering the UI | Analysis in C++, typed arrays, subscribers decimated to the 17 ms playhead timer, watchdog, `audio.on('frame')` auto-unsubscribes a plugin that throws repeatedly |
| Scripts mutating during playback | Already handled: every edit swaps an immutable timeline snapshot (SPEC §3) |
| Scripts re-entering via dialogs mid-transaction | Revision guard + transactions are synchronous; `ui.dialog.*` is forbidden inside a transaction (throws) |
| Plugin loaded from a cloned repo | Per-project plugins opt-in with an explicit prompt (§3) |
| API freezes too early | v1 is `0.x` until Phase 3 lands; announce stability with the first release that ships the docs |
| Bundle size / release pipeline | Measure in Phase 0; `PORYDAW_SCRIPTING` CMake option keeps it removable |

**DECIDE list (summary):** runtime (§2, JS recommended); widget layer A vs B
(§5, gated on Phase 0); per-project plugin dir opt-in (§3); whether raw SMF
edits are permission-gated or just available (§4); scheduling — Phase 1+2
before or after the current time-selection/clipboard work is pushed.
