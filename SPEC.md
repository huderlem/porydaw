# porydaw — Design Specification

*Status: v1, approved — 2026-07-03*

porydaw is a purpose-built, cross-platform (Windows/macOS/Linux) DAW for composing and
editing music for Gen 3 Pokémon decompilation projects (pokeemerald, pokefirered,
pokeruby, and forks). It is configured with a single path — the user's decomp project
directory — and from that it knows how to load instruments, samples, and songs, play
them back GBA-accurately, and save drop-in `.mid` files that the project's existing
`mid2agb` build pipeline consumes unchanged.

## 1. Vision

**Primary audience:** ROM hackers who are *not* DAW power users — people who want to
import a MIDI file, tweak an existing song, or compose something simple without
learning Reaper/FL Studio, voicegroup assembly syntax, or the m4a CC conventions.

**Secondary audience:** power users, who keep their preferred DAW + the poryaaaa CLAP
plugin for composition, but may still use porydaw for quick edits, project-aware
auditioning, and its m4a-semantic views.

**Design principle:** the editor's data model *is* the m4a engine's constraint set.
Track count, polyphony, quantization, and parameter semantics are first-class concepts
in the UI, not fights against a general-purpose DAW's assumptions.

### Non-goals

- No audio tracks, recording, or mixing buses. m4a is a MIDI-sequence engine; so is porydaw.
- No plugin hosting (CLAP/VST). There is exactly one instrument: the embedded poryaaaa engine.
  Power users who want plugin routing already have poryaaaa.clap in their DAW of choice.
- Not a replacement for power users' DAWs. Interop with them instead (see §4).
- No editing of project files beyond the song-related set: `.mid` files,
  `sound/songs/midi/midi.cfg`, the registration files, and voicegroup
  `.inc` files (see §6, "song-files write-back").

## 2. Locked decisions

| Decision | Choice | Rationale |
|---|---|---|
| Fork vs. scratch | **Build from scratch** | Existing DAWs (LMMS, Qtractor, Ardour, …) are enormous codebases centered on features we don't need (audio tracks, plugin graphs), while porydaw's value is m4a-native constraints. The hard real-time parts already exist in poryaaaa. |
| UI stack | **Qt 6 / C++** | Same stack as porymap: proven cross-platform shipping to this exact audience, native menus/dialogs/docking, well-trodden piano-roll territory (LMMS/Qtractor are Qt). |
| Song source of truth | **The `.mid` file is canonical** | porydaw edits `sound/songs/midi/*.mid` in place, constrained to the mid2agb-compatible subset. Saving *is* exporting. Perfect interop: the same file opens in any DAW; porydaw can never corrupt a build. |
| Project write-back depth | **All song-related files** *(revised 2026-07-05; originally "songs only" with copy-paste snippets)* | porydaw writes `.mid` files, the song's `midi.cfg` line, and the registration files (`song_table.inc`, `songs.h`, `ld_script.ld`, `charmap.txt`, `src/debug.c`) directly — inserting or correcting only the song's own lines, byte-conservative for everything else (§6.3). Voicegroup `.inc` files: the editor rewrites only the edited voice lines, preserving every other byte (§5.3). Nothing outside this set is ever modified. |
| Repo shape | **New repo; poryaaaa as git submodule** | porydaw is its own CMake project consuming poryaaaa's engine sources (`ENGINE_SOURCES` set). Fixes the engine needs (see §9) are upstreamed to poryaaaa so the CLAP plugin benefits too. |
| Synth | **poryaaaa engine core, statically linked** | `plugin/m4a_engine.{h,c}` + `m4a_channel.c` + `m4a_tables.c` + `m4a_reverb.c` + `voicegroup_loader.c` — a self-contained C11 library with no CLAP/GUI dependency, already proven embeddable by `cmd/poryaaaa_render.c`. |

## 3. Architecture

Four layers; dependencies point downward only.

```
┌─────────────────────────────────────────────────────────┐
│  UI shell (Qt 6 / C++)                                  │
│  main window · track list · piano roll · automation     │
│  lanes · instrument browser · transport · wizards       │
├─────────────────────────────────────────────────────────┤
│  Document + Sequencer (C++)                             │
│  SongDocument (in-memory SMF + m4a semantics) · undo/   │
│  redo · mid2agb simulation (LUTs, CC map) · transport   │
│  state · sample-accurate event scheduler                │
├─────────────────────────────────────────────────────────┤
│  Decomp Project Adapter (C++)                           │
│  project discovery/profile · song list (song_table.inc, │
│  songs.h, midi.cfg) · SMF read/write · midi.cfg write · │
│  registration writer                                    │
├─────────────────────────────────────────────────────────┤
│  poryaaaa engine core (C11, git submodule)              │
│  m4a_engine · m4a_channel · voicegroup_loader ·         │
│  reverb/tables  +  miniaudio for device output          │
└─────────────────────────────────────────────────────────┘
```

### Threading model

- **UI thread:** all Qt widgets, document mutation, undo/redo.
- **Audio thread:** miniaudio (or RtAudio) callback owning the `M4AEngine` instance;
  runs the sequencer, calling `m4a_engine_note_on/off/cc/pitch_bend` and
  `m4a_engine_process(outL, outR, n)`.
- Communication via lock-free SPSC queues: UI → audio (transport commands, immutable
  snapshots of the playable event timeline, live-edit "preview note" events) and
  audio → UI (playhead position, polyphony/overflow telemetry from the engine's
  debug counters — the UI takes a lock-free snapshot copy of the counters, channel
  states, and event ring each tick). The sequencer stamps the engine's
  `polyEventClock` with each note-on's timeline tick so overflow events carry song
  positions; live/preview notes stamp a sentinel instead.
- Document edits during playback swap in a new immutable timeline snapshot; the
  sequencer picks it up at the next tick boundary. No locks on the audio thread.

The engine is multi-instance-safe (all state hangs off a caller-owned `M4AEngine`),
so an optional second instance can serve auditioning (clicking piano keys / previewing
voices) without disturbing playback.

## 4. Data model: the song document

### 4.1 Canonical storage: Standard MIDI File

The document is `sound/songs/midi/<song>.mid`, plus its options line in
`sound/songs/midi/midi.cfg` (e.g. `mus_foo.mid: -E -R50 -G_foo -V080`). porydaw:

- Reads any SMF, but **writes only the mid2agb-compatible subset** (format 1,
  channel-per-track semantics as mid2agb expects, the CC vocabulary below, loop
  markers as `[` / `]` text/marker meta events). A format-0 file is coerced to
  format 1 at the parse layer (`SmfFile::read` → `convertToFormat1`, so an
  unconverted file cannot escape it): a conductor chunk 0 for the non-channel
  metas plus one chunk per used channel in ascending channel order — the order
  mid2agb emits agb tracks for a format-0 file, so the compiled `.s` output is
  unchanged (`--roundtrip` proves it). Channel-Prefix-scoped text metas travel
  to their channel's chunk — except marker text, which stays (prefixed) in the
  conductor chunk where mid2agb reads it, and a prefixed name on a channel
  with no events becomes a name-only chunk rather than lost data. Everything
  past the parse layer deals in format 1 only.
- Preserves unrecognized meta events and any data it doesn't model on round-trip,
  so a file authored in another DAW survives a porydaw edit session.
- Treats the `midi.cfg` flags as song properties editable in a Song Settings panel:
  voicegroup (`-G`), master volume (`-V`), reverb (`-R`), priority (`-P`),
  clocks/beat (`-X`), exact gate (`-E`), compression (`-N`).

### 4.2 The m4a semantic layer (presentation, not storage)

The stored bytes are plain MIDI; the UI *presents* them in m4a terms using mid2agb's
mapping (from `tools/mid2agb/agb.cpp`):

| MIDI event | m4a meaning | UI presentation |
|---|---|---|
| Program change | `VOICE n` | Instrument picker showing the actual voicegroup entry (name, type, ADSR) |
| CC 1 | `MOD` (LFO depth) | "Modulation" lane |
| CC 5 | `PORTAMENTO` (ext) | "Portamento" lane (gated on engine profile) |
| CC 7 / CC 10 | `VOL` / `PAN` | Volume / Pan lanes |
| CC 12–17, 33, 39 | `MEMACC` ops, loop label, `PRIO` | Advanced view only |
| CC 23 / 25 | `PWMC` / `PWMS` (ext) | Pulse-width lanes (gated on engine profile) |
| CC 24 | `TUNE` | Fine-tune lane |
| CC 20 | `BENDR` | Bend-range lane |
| CC 21 / 22 / 26 | `LFOS` / `MODT` / `LFODL` | LFO speed / type / delay lanes |
| CC 29 / 30 / 31 | `XCMD` (pseudo-echo vol/len) | Pseudo-echo lanes |
| Pitch bend | `BEND c_v±` | Pitch-bend lane |
| Tempo meta | `TEMPO` | Tempo track |
| Marker `[` `]` | `GOTO` loop | Loop region overlay in the timeline |

Dedicated automation lanes are offered only for parameters the embedded poryaaaa
engine actually renders — porydaw targets the engine as it exists, not the full m4a
command set. CCs the engine currently treats as no-ops (e.g. `TUNE`, `MODT`,
`LFODL`) and pseudo-echo `XCMD`s get no audible lane; they are still preserved
byte-for-byte on round-trip and visible in the advanced/"other events" view. If
poryaaaa gains support for one later, porydaw simply enables its lane.

### 4.3 WYHIWYG: simulating mid2agb on playback

To guarantee *what you hear is what the ROM plays*, playback and display pass note
data through mid2agb's transforms before it reaches the engine:

- Velocities snapped through `g_noteVelocityLUT`; durations through
  `g_noteDurationLUT` (both in `tools/mid2agb/tables.cpp` — small, portable, and
  reimplemented in the document layer).
- Timing quantized to the song's clock base (24 or 48 clocks/beat per its `-X` flag).
- Master volume (`-V`) and reverb (`-R`) from `midi.cfg` applied exactly as the
  compiled `.s` header would (`VOL * mvl / mxv`, `reverb_set + N`); a song with
  no `-R` flag plays at the vanilla `STD_REVERB` default (50).
- The UI shows both the drawn and the effective (quantized) value — e.g.
  "velocity 93 → GBA plays 90" — so quantization is visible, not mysterious.
- Music-player track budgets honored: the song's player only allocates
  `NUM_TRACKS_*` tracks (`sound/music_player_table.inc`, e.g. BGM = 10 in
  vanilla, engine ceiling 16), and `MPlayStart` silently never starts tracks
  beyond them — so playback mutes those tracks, their headers read "silent
  in-game", and import warns. Parsed from the project's own table, so raising
  `NUM_TRACKS_BGM` in the project raises porydaw's budget with it; editing is
  never gated on the budget.

### 4.4 Sidecar view state

Per-song UI state (zoom, lane visibility, track colors, last edit position) lives in
`<projectroot>/.porydaw/<song>.json`. Sidecars are cosmetic only — deleting them
loses nothing musical — and `.porydaw/` is recommended for the project's
`.gitignore`.

## 5. Decomp project adapter

### 5.1 Project profile

On open, porydaw scans the project root and builds a profile:

- **Layout:** pokeemerald-style (`sound/voicegroups/*.inc`) vs. pokefirered-style
  (monolithic `sound/voice_groups.inc`) — already auto-detected by poryaaaa's
  `voicegroup_loader`.
- **Engine feature set:** stock m4a vs. `m4a_extensions` (detected by probing for
  `PORTAMENTO`/`PWMC` opcodes in `sound/MPlayDef.s` and/or extension fields in
  `m4a_internal.h`). Unsupported automation lanes are shown grayed out with an
  explanatory tooltip, never hidden silently.
- **Song list:** parsed from `sound/song_table.inc` + `include/constants/songs.h`
  (names, IDs, player assignment) cross-referenced with `midi.cfg` and the presence
  of `.mid` sources. Songs that exist only as `.s` (no `.mid`) are listed but marked
  import-only (see agb2mid, §8 M4).
- **Voicegroups, keysplits, samples:** loaded via poryaaaa's `voicegroup_loader`
  (reused unmodified).

### 5.2 Reads

- SMF parser/writer (new, in the adapter; the document layer owns the model).
- `midi.cfg` parser (line format: `<file.mid>: <mid2agb flags>`).
- `song_table.inc` / `songs.h` parsers (read-only, for the song browser).

### 5.3 Writes — songs and voicegroups only

porydaw writes exactly three things into the project:

1. `sound/songs/midi/<song>.mid`
2. The song's line in `sound/songs/midi/midi.cfg`
3. Voicegroup `.inc` files: the voicegroup editor rewrites only the edited
   `voice_*` macro lines, preserving every other byte (comments, keysplit
   lines, labels, line endings). *(The first-save permission prompt was
   removed 2026-07-07 — with saving unified it no longer served a purpose.)*
   Saving is unified with the song: to the user the song and
   its voicegroup are one document, so Save Song writes 1–3 together (there
   is no separate voicegroup save). Creating a voicegroup adds
   `sound/voicegroups/<name>.inc` and appends its `.include` line to
   `sound/voice_groups.inc`. Pre-save auditioning never touches project
   files — edits are rendered to `.porydaw/vgpreview/` and loaded through
   the loader's search-path override.

It never touches `song_table.inc`, `include/constants/songs.h`, `ld_script.ld`,
`ld_script_modern.ld`, or samples.

## 6. UI specification

### 6.1 Main window

- **Left dock — Project panel:** song browser (grouped BGM / SE / fanfares, searchable),
  voicegroup browser for the current song's voicegroup (each entry with type icon,
  name, ADSR summary; click to audition). A sample or wave voice's symbol
  field is a picker button (not a combo — the project sample list is huge and
  prefix-heavy): its popup has a search filter, keysplit/sample/phoneme
  sections (the wave list for programmable-wave voices), loop badges with a
  rate/length detail line, and audition-on-highlight — samples and waves play
  through the voice's own envelope, keysplit rows play whichever sub-voice
  middle C resolves to with that sub-voice's envelope; typed symbols the
  project scan didn't list still commit via an explicit fallback row.
- **Center — Arrangement + Piano roll:** track headers (≤ 16 tracks; name, instrument,
  mute/solo, volume/pan mini-controls) beside a shared-timeline piano roll. Selected
  track is editable; other tracks ghosted. `M`/`S` (rebindable) toggle mute/solo over
  the selected track(s) — the whole Ctrl/Shift header scope, mixed state resolving
  toward on. Loop region rendered from `[`/`]` markers.
  The horizontal camera overshoots the song on both sides: a lead pad of dead
  space before tick 0 (10% of the viewport, 48–256 DIPs, drawn as a flat
  dimmed shade with no grid) is the scroll floor, so zooming near the song
  start comes to rest with tick 0 still on screen and the start never pins to
  the viewport edge — fresh songs and transport "go to start" home there; and
  a full viewport of scratch space past the song's end is scrollable at any
  zoom (grid continues), so notes can be pasted or drawn beyond the current
  bounds — edits out there grow the song, renewing the scratch space.
  View → Color Notes by Velocity (app-wide, persisted) swaps the selected track's
  note fills from track-identity color to a velocity hue ramp — purple `#5f44e9`
  (velocity 1) through the spectrum to red `#e90904` (127); ghost notes unchanged.
  View → Show Note Names (app-wide, persisted) labels the selected track's notes
  with their pitch name in a fixed face — the bundled family two layout pixels
  below Caption (SemiBold for 1x legibility; Regular on macOS, where CoreText
  keeps it crisp), even with Use System Font on. Each note decides
  independently: the label draws inside the note face only when the complete
  name plus a small trailing reserve fits the note's width, and it hides —
  never shrinks — when the padded row height misses the face; ghost notes are
  never labeled. Ink is the stronger of the two piano-key colors against the
  note's own fill — softer than pure black/white, and picked per fill so both
  identity and velocity-hue fills stay readable — and all note text sits on a
  plate of that fill so the velocity bar passes behind the glyphs, never
  through them. The
  pencil's pending note is exempt from the fit rules: it always shows a live
  pitch readout while drawing, overrunning the note or the row if it must.
  While the roll.velocity_drag chord is held or a velocity drag is live,
  notes show their velocity value instead of the pitch name, and velocity
  bars appear at any zoom.
- **Bottom — Automation lanes** (View → Automation Lanes, or `view.automation_lanes`,
  default **A**, dispatched from the roll/lanes focus like V and M/S/B): per-track,
  addable from the m4a parameter list (§4.2),
  drawn as line/step editors. Visibility is an app-wide preference (QSettings, like
  the velocity lane's and Follow Playhead), shown by default; closing the pane is
  view-only, leaving the rows, their heights, and their points untouched, and it is a
  separate thing from a single lane's "Hide lane" (§6.1), which is per-song. A pane
  reopened after the splitter has sized it comes back where it was; one that was never
  sized — the preference persisted off — borrows its classic height from the roll. With the pencil mode below owning freehand drawing, the
  **arrow tool never creates data on a plain click**: a left click on empty lane space
  parks the edit cursor at the snapped tick and writes nothing, and a left click on a
  point's dot **deletes** that point as one undo entry (of a node selection, only the
  clicked node; of same-tick duplicates, the one the press grabbed). Shift+click on a
  dot spares it — that chord starts the axis-locked drag below, and a modifier slip
  must not destroy a node — and off-dot it is still a line ramp, which commits its
  degenerate single point as it always has. A freehand sweep therefore starts only once
  the drag clears a font-scaled activation distance (`layout::fontPx(5/12)`, the axis
  lock's), and the slop is subtracted from the stroke so crossing the threshold is not
  itself movement: sub-threshold hand jitter leaves the document byte-identical. The
  click spends the pair, so a double-click **on a node** is a no-op; the double-click
  type-in stays on empty lane space, and a node's exact value is typed through the
  point menu's Set value… below. None of this touches the pencil: it draws from the
  first pixel, and a pencil click still leaves a single point.
  Ctrl while dragging magnetizes lanes with a meaningful
  center (pan/tune 64, bend 0) to that neutral inside a font-scaled pixel window
  (`layout::fontPx(2/3)` mapped through the row height into value units), so
  dead-center never needs pixel-perfect aim at any lane height or font scale.
  Dragging an existing point with **Shift** held locks the drag to one axis, picked
  by the dominant direction once the travel passes a font-scaled activation distance
  (`layout::fontPx(5/12)`): mostly-horizontal keeps the value exact while the tick
  follows the cursor (SizeHor cursor), mostly-vertical keeps the tick (SizeVer).
  The lock is sticky once resolved — wobbling past 45° later doesn't flip it;
  releasing Shift mid-drag frees the drag, re-pressing re-resolves from the total
  travel, and the release restores the tool cursor. Shift+press **on** a point's dot
  starts this locked drag; the Shift line ramp starts anywhere off-dot (a constrained
  nudge of a dot is common, a ramp anchored exactly on one is not).
  A pencil mode (`automation.pencil_mode`, default **B**,
  dispatched from the roll/lanes focus like M/S) turns every left drag into a freehand
  draw — point grabs and the Shift ramp are suspended while it is on — and holding
  Shift locks the stroke to a horizontal line at the value where the lock engaged;
  the cursor becomes a pencil. The key is Ableton-style momentary besides the tap
  toggle: a quick tap sticks, while holding it past 500 ms — or drawing a lane
  stroke during the hold — reverts the mode on release, making it a hold-to-draw
  (or, from pencil mode, hold-to-arrow) chord. Sweeps and Shift ramps step on the
  drawn grid per time-signature segment — the grid restarts (and can change spacing)
  at a signature change, and generated points follow it — with the ramp's endpoint
  exact at the release tick. Tempo lane always available at the top level. 0-based CC
  lanes have a zoomable value axis (gutter menu → Value range: auto-fit or a fixed
  0–16/32/64/127 display max, persisted in the sidecar §4.4); MOD auto-fits by default
  since its musical range is roughly 0–20. Display only — event values are untouched,
  and data beyond the chosen range grows the axis rather than clipping.
  CC lanes can also be hidden (gutter menu → Hide lane; Tempo/Voice rows cannot):
  the row leaves the lanes area with its events untouched — a view-only op, no undo
  entry — and comes back through the add-lane menu's "Hidden lanes" section
  ("Show: … (hidden)"). A hidden CC is never re-offered as a plain add candidate.
  The hidden set persists in the sidecar (§4.4) keyed like the row heights, so like
  them it goes harmlessly stale when tracks move. The add-lane strip opens on
  right-click as well as left, and the gutter menu works on an added-but-empty lane.
  A lane-scoped time selection (the right-drag band) derives a **node selection**:
  a point is selected iff its lane is one of the selection's lanes and
  `startTick <= tick < endTick` (half-open) — nothing is stored, so the set can
  never go stale, and nodes highlight live while the band is still sweeping.
  Selected nodes draw a ring in the palette highlight; with two or more selected,
  nodes in unselected lanes dim toward the palette mid color. Dragging a selected
  node moves **every** selected node (cross-lane) by one shared tick/value delta
  from the grabbed node — the shift clamped so the earliest node can't go below
  tick 0, each value clamped to its own row's display range (tempo floored at 1) —
  as a single undo entry, with a live preview of each affected row's pending curve;
  the selection band shifts with the move and republishes. Pressing a node outside
  the selection stays a plain single-point move. Delete/Backspace precedence: with
  a **Lanes**-scope selection the range delete *is* the node delete — it removes
  exactly the covered lane points (voice markers of a covered Voice row included)
  as one undo entry, leaving out-of-range points untouched; with a Tracks-scope
  selection the same shortcut keeps deleting the range contents across notes and
  lanes via the shared `roll.delete` dispatch.
  A right release in place on a lane point opens its context menu — **Set value…**
  and **Delete** — instead of deleting outright (fast removal stays on the Delete
  key via the node selection). Set value… is the same per-row type-in the
  double-click opens (BPM for tempo, signed bend, c_v for pan/tune), seeded with
  the aimed point's value; committing retypes that point and, like every value
  edit on a tick (drags, the double-click write), heals shadowed same-tick
  duplicates — the tick ends up holding one point at the typed value. Delete
  removes exactly the aimed point: of same-tick duplicates, only the one under
  the cursor's y. The menu is aimed by a circular nearest-wins hit test against
  the document (9 px, the old x-only right-click radius; the ±7 px dot box stays
  the left-drag grab rule), so the cursor's y picks among same-tick duplicates,
  and the aimed node paints the highlight ring while the popup is open (at zooms
  where the node dots draw, sharing the selection rings' gate). Right-clicking
  another point while it is open re-aims the menu in one gesture — the roll's
  note-menu retarget, shared through `ui::ContextMenu` — and a right-click on
  nothing falls through and dismisses it. The lanes' right-click precedence
  holds while the popup is open: a retarget onto a selection-covered point
  declines (the range menu owns that click, so the popup just dismisses).
  Inside a covering time selection a fresh right-click keeps opening the range
  menu, a voice-row right release still deletes the marker directly, and empty
  lane space still clears the selection.
- **Between them — Velocity lane** (View → Velocity Lane, or `view.velocity_lane`,
  default **V**, dispatched from the roll/lanes focus like M/S/B): a hidden-by-default
  pane in the same splitter, above the automation lanes, showing the selected track's
  notes on the shared timeline. Each note is a node — a filled circle at (start tick,
  velocity) with a stem across its duration, in the track identity color, the stems a
  third of the way to black. The value ruler covers the whole editable domain 1–127
  and there is no vertical zoom: the pane's height only sets how densely the ruler is
  graduated (five density bands), and dragging the splitter resizes it. The lane
  mirrors the roll's note selection — selected nodes gain a ring and their extreme
  velocities are marked on the ruler; with more than one selected, the rest dim toward
  the palette mid color — and shares the timeline camera: middle-drag pans, the plain
  wheel zooms time at the cursor, Shift (or a horizontal wheel) scrolls, and the wheel
  over the ruler column is left alone. Visibility is an app-wide preference (QSettings,
  like Follow Playhead); the pane's height is per-song sidecar state (§4.4).
- **Lane toggles:** two checkable icon buttons — a two-node step glyph for the
  automation lanes and a handle-on-a-rail glyph for the velocity lane, traced from
  specker's SVG icons. They lead the "other events" strip's gutter, which pushes that
  row's label to the gutter's right edge; the strip is the view's last row, below the
  splitter. The toggles are never hidden, so with both panes closed they are still the
  way back without the View menu. The buttons, the View menu items, and the A/V keys
  are three faces of the same two preferences and stay in sync in every direction.
  The glyphs are painted (no SVG module is linked) and tinted from the button role
  ramp, so they follow the theme and the checked/hover fills.
  Editing is deferred throughout: the pointer only moves a preview, and the
  document mutates once — one undo entry — when the button comes up, refusing the
  write outright if the notes changed underneath ("Velocity edit cancelled because
  notes changed."). A left press grabs the note under it (its node's circle, or
  anywhere along its stem; a node outranks a stem, and a selected note outranks an
  unselected one) — an unselected note's press takes the selection over, Ctrl adds
  to it — and dragging moves the velocity of *every* selected note by the drag's own
  vertical distance, each from its own starting value, so the selection keeps its
  shape and notes that clamp at 1 or 127 come back when the pointer does. Under the
  activation slop the press is a click instead: plain collapses the selection onto
  that note, Ctrl toggles its membership. A drag from empty plot **paints**: it
  brushes the selected notes whose ticks the stroke crosses — and only those,
  never an unselected neighbor under it — taking each note's new velocity from the
  stroke's own height there, starting at the press, so a click straight below a
  selected node sets that one note. A stroke that reaches nothing is a click, and a
  click on empty plot clears the selection (on the release, never on the press).
  **Shift** anywhere in the plot is a **ramp** instead: the straight line from the
  press to the pointer, drawn in the lanes' preview ink, read as the velocity for
  every selected note whose tick it spans. A click on one of the **ruler's printed
  values** sets the whole selection to it in one edit (only the labels are targets,
  and the track-header column beside them is not the lane's). Escape, focus loss, or
  losing the mouse grab abandons the gesture and restores the selection the press
  found. While a gesture runs the status bar reads out the aimed note (key, stored
  velocity, the velocity the engine will really play, length) and playback
  follow-scroll pauses. Hovering a node makes *that* note what the ruler describes
  — its value is marked, and its own voice decides the context below — ahead of the
  selection. **PSG detents** (`core/velocitymodel.h`): when the lane's context is one
  CGB channel — the hovered note's voice, else the selected notes' if they agree on
  one channel *and one volume*, else the voice in effect at the playhead/edit cursor
  — the ruler becomes one labeled row per real loudness level ("Vol 1…N", the active
  levels accented), nodes sit at their level's row rather than their stored velocity,
  the levels paint as lines along those same rows for as long as that voice and
  volume last (`song_view_psg_velocity_levels`) — a line is the rail its nodes sit
  on, not a fence between two of them — and every edit lands on a level: drags move
  whole levels (returning to the level it started in restores the exact velocity it
  found there), ruler clicks and strokes take the level's representative, and each
  note answers to its own voice, so a selection across a voice change still lands
  correctly on both sides. The levels are the engine's, derived per note from
  velocity *and* the compiled VOL byte together (`ChnVolSetAsm` then `CgbModVol`):
  mid2agb folds the song's master volume into every VOL (`vol*mvl/mxv`), so a
  quieter song genuinely has fewer detents rather than the same ones played softer —
  a square channel has 16 levels at full volume, 8 at volume 64, and none below
  about 16, where it is silent whatever the velocity says and the lane keeps the
  plain ruler. The header's **Detents** checkbox (shown only where there are
  detents, and rearmed whenever the context leaves PSG) puts the plain ruler and
  exact values back for the track, and holding `velocity.detent_unlock` (**Ctrl** by
  default, read at the press) does the same for one gesture. The **right button** is
  the lane's marquee: a right drag bands the nodes it covers (previewed as selected
  while it is swept) and replaces the selection with them, Ctrl unions instead;
  released in place it is a click — on a node it selects it, Ctrl toggles its
  membership, and on empty plot it clears the selection. It never touches a velocity,
  and the lane has no context menu.
- **Transport bar:** play/pause/stop, loop toggle, a follow-playhead toggle (also in
  the View menu; off, playback stops scrolling the roll and event list — the camera
  stays where the user put it; app-wide, persisted), position, tempo display, master
  volume, and a **polyphony meter** fed by the engine's overflow-debug counters
  showing DirectSound channel usage against the project's `maxChans` (dropped/stolen
  notes flash a warning — the #1 mystery for newcomers).
- **Polyphony dock** (View → Polyphony Debugger, hidden by default): the full
  overflow debugger, mirroring the poryaaaa plugin's Polyphony tab. A live
  channel-usage grid (real PCM/CGB channels plus the shadow pool of lost sounds), a
  "Solo overflow (invert audio)" toggle that mutes normal playback and plays only
  the sounds lost to the polyphony limit (session-sticky: survives play/stop and
  song switches, never persisted; note auditions are exempt and stay audible, so
  the lost note under investigation can be struck against the inverted output), a per-track overflow table (Dropped / Cut Off /
  Tail Cut, document track names, red flash on increase, Reset), and a recent-events
  log where each event carries its bar:beat song position — double-click jumps the
  edit cursor/playhead there and selects the losing track and the lost note in the
  roll; live preview notes read "live" instead.
- **Song Settings dialog:** the `midi.cfg` flags presented as friendly controls
  (voicegroup dropdown, reverb slider, master volume, priority, exact-gate toggle).
- **MIDI event list** (View → MIDI Event List; swaps in for the piano roll):
  every event of one SMF chunk as an editable table row — tick, type, channel,
  data bytes, meta/sysex payload, decoded summary — with per-category
  filtering, an end-of-track row, and a playhead-following row tint. Cell
  edits go through the document's raw-event API (shared undo stack); inserts
  land at their tick's canonical position (setup events → note ends →
  note-ons, the intra-tick order mid2agb's pairing depends on). Because
  same-tick order is otherwise significant — the file preserves it and
  mid2agb stable-sorts by time+type — rows can be reordered directly: drag a
  row between same-tick siblings (the drop indicator only appears where the
  drop is legal), nudge with Alt+Up/Down (rebindable), or use the context
  menu's "Move up/down within tick". Reorders never cross a tick boundary
  (the Tick cell retimes) and never unpin the canonical class order. The
  list is anchored to its chunk, not to a chunk number: track adds, deletes,
  and moves (interactive or via undo/redo) re-index the chunks and the list
  follows its chunk to the new index; deleting the viewed chunk's own track
  falls back to the selected track's chunk.
- **Typeface preference:** the UI ships with the bundled Atkinson Hyperlegible
  scale (Body 1.25× the platform's base font size, Caption at the base size),
  rendered unhinted and antialiased — hinting distorts the letterforms on
  Windows at 100% scale, and the faces' gasp tables ask for the same
  smoothing from renderers that consult the font.
  Edit → Settings → Appearance → Use the system font (app-wide, persisted)
  swaps the whole scale onto the
  platform font at its native size — Body at the platform size, Caption one
  1.25 step below, Body Mono on the platform's fixed-pitch face — so porydaw
  matches the machine's other Qt applications; toggling re-installs the
  application font and re-asserts inheritance on every polished widget
  directly, never through a stylesheet repolish (a repaint hazard while
  playback is painting on Windows — theme changes still repolish).

### 6.2 Editing behaviors

- Note draw/move/resize/velocity with snapping to the song's clock base; the effective
  quantized velocity/length shown inline (§4.3).
- Modifier velocity drag (`roll.velocity_drag` chord, Ctrl by default): a
  vertical drag from anywhere on a note adjusts its velocity (1 px = 1 step);
  the chord's click without the drag keeps Ctrl's selection toggle. On an
  unselected note the Ctrl-chord drag joins it to the bulk selection and
  nudges the whole selection — except that once a modifier velocity drag has
  committed, the next one during the same uninterrupted chord hold replaces
  the selection with its note instead (one-shot, re-armed by each committed
  drag). Repeating the drag on the same note keeps a deliberate bulk
  selection; releasing any modifier key, or losing focus or the window,
  restores the join behavior.
- Live audition: notes sound (through the correct voice) while being drawn or dragged.
- Undo/redo across all document mutations, including `midi.cfg` property
  changes and voicegroup voice edits — song and voicegroup share one undo
  stack and one dirty/save state (they are one document to the user).
- Undo-stack hygiene: an edit whose clamped result changes nothing (a move
  pinned at tick 0 or a key rail, a velocity already at its target or clamp)
  pushes no command, and a merged keyboard move gesture that returns every
  note to its origin removes its command entirely — Ctrl+Z never replays
  no-ops. Moves and resizes re-insert the note's own events (every unmodeled
  byte survives); Duplicate Track copies only the source engine track's own
  channel from a mixed-channel chunk.
- Document contracts under the editing surfaces: every note-on carries a
  transient `NoteId` (stable across move/resize/velocity/undo — never
  serialized), the document exposes a `revision()` counter bumped once per
  published mutation, batch APIs (`setNotesVelocities` — revision-checked and
  all-or-nothing — and `moveLanePoints`) land as single undo entries, and a
  `tracksRemapped` signal reports every chunk/engine ownership change with
  its old→new map. `--editcheck` and `--noteidcheck` harnesses cover them.
- MIDI file import: open an arbitrary external `.mid`, get a guided analysis pass —
  channels → tracks (warn > 16 or > polyphony budget), unmapped CCs flagged — then
  saved into the project as a new song file. Import silently drops same-tick
  duplicate state-setters (repeated channel-init blocks are a common export
  artifact), keeping the last of each run — the only one mid2agb's output ever
  lets the engine hear; notes, text/marker metas, and coupled-protocol CCs
  (MEMACC, XCMD, the loop Label) always survive. Files already in the project
  are never sanitized on open (the round-trip guarantee): the editing surfaces
  instead resolve a same-tick run last-wins — matching playback — and writing
  onto an occupied tick replaces the run rather than stacking another duplicate.

**Sample Editor (custom DirectSound samples; design: `docs/sample-editor/`):**
Tools → Import Sample (or "New…" / "Edit…" beside the sample picker in the
voicegroup dock) opens a modal editor over any .wav/.aif/.mp3/.flac/.ogg
source or .sf2 zone: non-destructive crop / loop / retune / resample /
normalize on an immutable hi-res decode, with loop auto-suggestion, live
seam-click metrics, pitch-detect prefill, and engine-true audition — the
auditioned bytes, porydaw playback after commit, and the built ROM's `.bin`
are bit-identical (retune rides the `smpl`/`agbp` metadata; porydaw never
resamples for tuning). Committing writes the 8-bit `.wav` into
`sound/direct_sound_samples/` and appends its registration block to
`sound/direct_sound_data.inc` (write-through, not undoable;
wav2agb-pipeline projects only, actionable refusals otherwise) —
browser-initiated imports then point the requesting voice at the new symbol
as an ordinary undoable voice edit. A provenance sidecar
(`.porydaw/samples/<name>.json`: source path + content hash + edit params)
lets "Edit…" reopen the hi-res source exactly where the user left off; a
missing sidecar or changed source falls back to re-importing the committed
8-bit `.wav` (still crop/loop-editable), and edits save over the `.wav` in
place with the registration untouched. `--samplecheck` harness (registrar
refusals, DSP acceptance, audition==build parity, offscreen editor driving,
compressed formats, SoundFont zones, engine loop integration, provenance).

### 6.3 New Song flow (write-through registration)

The "New Song" wizard collects name, voicegroup, player (BGM/SE), and `midi.cfg`
flags; writes the `.mid` and the `midi.cfg` line; then **registers the song
itself**, writing one line into each of the registration files. The
voicegroup picker (both blank and import modes) also offers *"create a new
voicegroup for this song"* on per-file-layout projects: the wizard creates
`sound/voicegroups/<label>.inc` from the dummy template, points the song's
`-G` at it, and the user configures its voices in the Voicegroup dock
afterwards.

```
sound/song_table.inc      →  song mus_foo, MUSIC_PLAYER_BGM, 0
include/constants/songs.h →  #define MUS_FOO 610
ld_script.ld              →  sound/songs/midi/mus_foo.o(.rodata);
charmap.txt               →  MUS_FOO = 62 02
src/debug.c               →  X(MUS_FOO)          \
```

Each line is computed from the parsed project (next free ID, existing
indentation/alignment) and inserted after the file's last matching entry —
except in `charmap.txt`, where the line lands at its position in the sound
section's ID order, so a backfilled song sits between its neighbors; every
other line keeps its exact bytes. `charmap.txt`'s sound section — recognized as
the two-hex-byte entries named by `songs.h` constants — maps each constant to
its ID as little-endian bytes so text control codes like `{PLAY_BGM}` can name
songs. Registration is idempotent — existing entries are left untouched, except
a `songs.h` define or `charmap.txt` entry whose ID drifted from the song's
table index, which is corrected in place. Projects whose `ld_script.ld` has no
per-song object lines skip that file, and likewise a `charmap.txt` with no song
entries (or none at all). On pokeemerald-expansion projects, `src/debug.c`'s
debug-menu sound tester lists every song as an X-macro entry under
`SOUND_LIST_BGM` / `SOUND_LIST_SE`; porydaw inserts the entry in the
prefix-matching list at its ID position (rewiring the macro's `\`
continuations when it lands at a list's end) and removes it on song deletion.
Projects without those lists (vanilla) skip the file.

Some expansion lines bound the constants into contiguous regions closed by
`songs.h` markers — `END_SE`, `START_MUS`, `END_MUS` — and size ID-indexed
arrays from them: pre-#9713 checkouts alias the last constant and size
`src/debug.c`'s sound-tester arrays (`sBGMNames[END_MUS - START_MUS + 1]`),
and the night-music feature re-added value-form markers
(`#define END_MUS 558`) sizing `overworld.c`'s `sNightMusicTable`. Either
way a song appended past the phoneme block sits outside the region — at
best the feature cannot address it, at worst its designated initializer
breaks the build. Whenever an `END_MUS` marker resolves in either form
(modern checkouts have none — PR #9713 deleted all three), registration
turns region-aware: new music inserts at `END_MUS + 1` ahead of the phoneme
block, whose defines and `charmap.txt` values all shift up by one; new
sound effects fill the placeholder (`dummy_song_header`) slots between the
SE region and `START_MUS` — bounded by `END_SE`, or by the highest define
below `START_MUS` on layouts that lost that marker — overflowing into the
music region only when the gap is exhausted (their debug entry lands in
`SOUND_LIST_BGM` only on the pre-#9713 shape, where the two lists feed
separately indexed arrays and routing is functional); each marker follows
its region's last song — alias
markers re-point, value markers renumber — through registration, free-slot
reuse, and deletion alike. A registration stranded past `END_MUS` (left by
a porydaw predating this) is flagged as mis-registered and migrates into
the region on re-register.

If registration fails (e.g. an unwritable file), the chosen constant/player
persist in the sidecar and the song shows a badge in the song browser;
**File → Register Song** (or right-click → **Register Song** in the browser)
retries. The same action registers stray `.mid` files dropped into
`sound/songs/midi/` by hand, and completes *partial* registrations — a song
present in the table but missing an entry elsewhere (e.g. one registered
before porydaw wrote `charmap.txt` entries) is badged "not fully registered",
with per-song gaps audited in one pass over the registration files at project
open.

## 7. Playback integration

- Engine sources built from the poryaaaa submodule as a static lib; device output via
  miniaudio (already vendored in poryaaaa) with a small device-selection settings page.
- The sequencer replicates what `cmd/poryaaaa_render.c` proves out: build an event
  timeline (after mid2agb simulation), drive engine event calls + `m4a_engine_process`
  from the audio callback, honor loop markers with configurable loop behavior.
- GBA-accuracy knobs surfaced in app settings, defaulted to hardware-accurate:
  PCM mix rate (13379 Hz default), analog filter, polyphony (`maxChans`), reverb.
- **Offline WAV export** reuses the same timeline against a faster-than-realtime
  render loop (loop count, fadeout — feature parity with `poryaaaa_render`).

### 7.1 Playhead rendering & timeline paint caching

- One transparent `PlayheadOverlay` widget spans every timeline-aligned surface
  (ruler triangle, roll, automation lanes, other-events strip) and draws the
  glow + 1 px core at a sample-accurate, fractional x.
- **Native compositor paths:** on macOS the playhead is CALayer sublayers of the
  top-level window's content view (masked to the visible surfaces); on Windows a
  DirectComposition visual tree in a transparent `WS_EX_NOREDIRECTIONBITMAP`
  child window. A playhead tick is then a single float position update + commit —
  no Qt repaints at all. Any platform failure logs a warning and falls back
  permanently (for that overlay) to the widget path; the
  `PORYDAW_FORCE_WIDGET_PLAYHEAD` environment variable forces the fallback for
  debugging. Elsewhere (Linux, and whenever the platform path is unavailable)
  the overlay paints antialiased vectors, which keeps the same subpixel motion.
- **Timeline paint caching:** roll, lanes, and strip derive from
  `TimelineSurface`, which rasterizes content into a cached pixmap so the
  playhead sweeping across them costs blits, not note/lane re-rasters. Content
  changes must go through `invalidateContent()` — a plain `update()` repaints
  the stale cache (see the class comment in `src/ui/timelinesurface.h`).
  Partial updates and clips are snapped to a device-aligned logical grid so
  fractional scale factors cannot leave stale boundary pixels;
  `PORYDAW_FORCE_UNCACHED_TIMELINE` disables the cache entirely for
  diagnosing stale-pixel artifacts in the field.
- **Update cadences:** playhead timer 60 Hz while playing; time/polyphony status
  labels 10 Hz while playing, 2 Hz otherwise, writing widgets only on change.

## 8. Roadmap

Each milestone is releasable on all three OSes.

**M0 — Player** *(foundations)*
Repo, CMake + Qt skeleton, poryaaaa submodule, CI builds for Win/macOS/Linux.
Open a project directory → pick any song with a `.mid` → GBA-accurate playback with
transport, per-track mute/solo, and the polyphony meter.
*Accept:* pokeemerald's `mus_abandoned_ship` plays indistinguishably from
`poryaaaa_render` output; project open < 5 s.

**M1 — Viewer**
Read-only piano roll + automation lanes with full m4a-semantic presentation (§4.2),
instrument names from the voicegroup, loop-region display, playhead following.
*Accept:* every event type in every vanilla pokeemerald song renders legibly; no
event is silently invisible (unknown data shows in an "other events" strip).

**M2 — Editor**
Note + automation editing, quantization display, Song Settings (`midi.cfg`), loop
marker editing, undo/redo, save with byte-conservative round-trip.
*Accept:* load → save with no edits is semantically identical through mid2agb
(compiled `.s` diff-clean); edit sessions in Reaper before/after porydaw survive.

**M3 — Onboarding**
New Song wizard + write-through registration (§6.3), external MIDI import with the
guided mapping pass, voicegroup *browser* with audition (still read-only).
*Accept:* a first-time user goes from downloaded `.mid` to hearing their song in-game
without hand-editing any project file.

**M4 — Power polish**
WAV export, keysplit/drumset-aware drum lane (row-per-instrument view for drum
tracks), memacc/loop helpers in an advanced view, `.s`-only song import via
`agb2mid`, app theming, auto-update checks, packaging polish (installer/dmg/AppImage).

**Voicegroup editing (shipped after M3):**
basic voice types (DirectSound variants, square 1/2, programmable wave, noise)
editable in the voicegroup dock with live audition before save — audible
mid-playback via a hot track-instrument refresh; keysplit voices swappable
(the Sample list offers the project's keysplit instruments first, each paired
with its table); byte-conservative dirty-line-only writes (§5.3; the
first-save confirmation prompt was later removed as pointless);
create-voicegroup (copy of an existing one or the dummy
template); `--vgcheck` harness; drumset (keysplit_all) voices selectable and
swappable like keysplits (the Drumkit list offers the project's observed
drumkit sub-voicegroups). Cry voices stay read-only and round-trip verbatim;
keysplit *tables* are not editable. Voice edits ride the song's undo stack
and save with Save Song (no separate save/revert; a `-G` voicegroup switch
keeps unsaved voice edits in the undo history and replays them when the
switch is undone); `--vgsavecheck` harness covers the unified pipeline.
Golden Sun synth instruments (ipatix improved-mixer feature; zero-size
DirectSound samples selecting pulse/saw/triangle) get their own
"Synth (Golden Sun)" type in the dock: waveform + pulse duty-LFO
parameters are editable, and edits resolve to param-named shared
definitions (`DirectSoundSynth_GoldenSun_<params>`) deduplicated by value
across `sound/direct_sound_data.inc` and
`sound/direct_sound_synth_data.inc`. Definitions minted by param edits
stay in memory (pending): the edit is auditioned by patching the
descriptor bytes straight into the loaded tone (live, no reload — the
engine re-reads them every tick), and only the definitions the SAVED
voicegroup references are appended to `direct_sound_synth_data.inc` on
Save Song, so abandoned tweaks never touch disk. The save also wires
`direct_sound_synth_data.inc` into the ROM build (an `.include` inserted
next to `direct_sound_data.inc`'s) when nothing assembles it yet, refusing
with instructions when no anchor is found — porydaw's own loader scans the
file unconditionally, which would otherwise mask an undefined-symbol build
break. The definition dropdown
lists on-disk entries only; a pending symbol shows as the voice's current
value until a save lands it. Gated on the project defining the
`set_synth_*` macros; `--vgcheck` covers naming/scan/dedupe/write and a
loader roundtrip, `--vgsavecheck` the pending-until-save pipeline
end-to-end.

**WAV export (shipped after M3, from M4):**
File → Export WAV renders the loaded song (including unsaved edits) offline
through a private engine instance with §7 semantics — loop count + fadeout
for looping songs, ring-out tail otherwise, selectable sample rate, streamed
16-bit stereo output with progress/cancel; `--exportcheck` harness.

**Sample Editor (shipped after M4):**
Custom DirectSound sample creation end to end — import → crop / retune /
resample / normalize / loop → engine-true audition → write-through commit
and registration, with provenance sidecars for reopening. Details in §6.2;
design and formats in `docs/sample-editor/`.

**Later / opt-in ideas (explicitly out of scope for v1):**
keysplit table *editing*, full project
write-back behind a "let porydaw edit project files" setting, pokeruby profile
validation, MIDI keyboard live input.

## 9. Prerequisites & upstream work in poryaaaa

- **License (release blocker):** poryaaaa has no LICENSE file; porydaw links it
  statically, so poryaaaa needs an explicit license before any porydaw release.
  porydaw itself also needs a license choice at repo creation.
- `voicegroup_loader_set_log_path()` is process-global — make it per-config for a
  multi-project host.
- Optional: a small query API for voice metadata (type/name/ADSR per voicegroup
  entry) if the loader doesn't already expose enough for the instrument browser.

No engine feature work is a prerequisite: porydaw's parameter surface is defined by
what poryaaaa currently supports (§4.2). Engine no-ops simply don't get automation
lanes.

## 10. Risks

| Risk | Mitigation |
|---|---|
| mid2agb round-trip subtleties (compression, exact-gate `gtp`, memacc labels) | M2 acceptance test compiles saved `.mid` through the project's real mid2agb and diffs the `.s`; keep a corpus of all vanilla songs as regression tests. |
| Fork divergence in project layouts (renamed dirs, custom macros) | Project profile + explicit extra-search-path config (loader already supports this); fail with actionable messages, never crash on parse. |
| Engine-extension mismatch (song uses PORTAMENTO, project engine is stock) | Profile detection (§5.1) + save-time validation warning listing unsupported events. |
| Qt licensing/deployment weight | LGPL dynamic linking as with porymap; CI packaging from M0 so deployment pain surfaces immediately. |
| Scope creep toward general DAW features | §1 non-goals are the contract; new features must serve the decomp workflow. |
