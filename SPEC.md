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
  `sound/songs/midi/midi.cfg`, the three registration files, and voicegroup
  `.inc` files (see §6, "song-files write-back").

## 2. Locked decisions

| Decision | Choice | Rationale |
|---|---|---|
| Fork vs. scratch | **Build from scratch** | Existing DAWs (LMMS, Qtractor, Ardour, …) are enormous codebases centered on features we don't need (audio tracks, plugin graphs), while porydaw's value is m4a-native constraints. The hard real-time parts already exist in poryaaaa. |
| UI stack | **Qt 6 / C++** | Same stack as porymap: proven cross-platform shipping to this exact audience, native menus/dialogs/docking, well-trodden piano-roll territory (LMMS/Qtractor are Qt). |
| Song source of truth | **The `.mid` file is canonical** | porydaw edits `sound/songs/midi/*.mid` in place, constrained to the mid2agb-compatible subset. Saving *is* exporting. Perfect interop: the same file opens in any DAW; porydaw can never corrupt a build. |
| Project write-back depth | **All song-related files** *(revised 2026-07-05; originally "songs only" with copy-paste snippets)* | porydaw writes `.mid` files, the song's `midi.cfg` line, and the three registration files (`song_table.inc`, `songs.h`, `ld_script.ld`) directly — inserting or correcting only the song's own lines, byte-conservative for everything else (§6.3). Voicegroup `.inc` files: the editor rewrites only the edited voice lines, preserving every other byte (§5.3). Nothing outside this set is ever modified. |
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
  compiled `.s` header would (`VOL * mvl / mxv`, `reverb_set + N`).
- The UI shows both the drawn and the effective (quantized) value — e.g.
  "velocity 93 → GBA plays 90" — so quantization is visible, not mysterious.

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
  name, ADSR summary; click to audition).
- **Center — Arrangement + Piano roll:** track headers (≤ 16 tracks; name, instrument,
  mute/solo, volume/pan mini-controls) beside a shared-timeline piano roll. Selected
  track is editable; other tracks ghosted. Loop region rendered from `[`/`]` markers.
- **Bottom — Automation lanes:** per-track, addable from the m4a parameter list (§4.2),
  drawn as line/step editors. Tempo lane always available at the top level. 0-based CC
  lanes have a zoomable value axis (gutter menu → Value range: auto-fit or a fixed
  0–16/32/64/127 display max, persisted in the sidecar §4.4); MOD auto-fits by default
  since its musical range is roughly 0–20. Display only — event values are untouched,
  and data beyond the chosen range grows the axis rather than clipping.
- **Transport bar:** play/pause/stop, loop toggle, position, tempo display, master
  volume, and a **polyphony meter** fed by the engine's overflow-debug counters
  showing DirectSound channel usage against the project's `maxChans` (dropped/stolen
  notes flash a warning — the #1 mystery for newcomers).
- **Polyphony dock** (View → Polyphony Debugger, hidden by default): the full
  overflow debugger, mirroring the poryaaaa plugin's Polyphony tab. A live
  channel-usage grid (real PCM/CGB channels plus the shadow pool of lost sounds), a
  "Solo overflow (invert audio)" toggle that mutes normal playback and plays only
  the sounds lost to the polyphony limit (session-sticky: survives play/stop and
  song switches, never persisted), a per-track overflow table (Dropped / Cut Off /
  Tail Cut, document track names, red flash on increase, Reset), and a recent-events
  log where each event carries its bar:beat song position — double-click jumps the
  edit cursor/playhead there; live preview notes read "live" instead.
- **Song Settings dialog:** the `midi.cfg` flags presented as friendly controls
  (voicegroup dropdown, reverb slider, master volume, priority, exact-gate toggle).

### 6.2 Editing behaviors

- Note draw/move/resize/velocity with snapping to the song's clock base; the effective
  quantized velocity/length shown inline (§4.3).
- Live audition: notes sound (through the correct voice) while being drawn or dragged.
- Undo/redo across all document mutations, including `midi.cfg` property
  changes and voicegroup voice edits — song and voicegroup share one undo
  stack and one dirty/save state (they are one document to the user).
- MIDI file import: open an arbitrary external `.mid`, get a guided analysis pass —
  channels → tracks (warn > 16 or > polyphony budget), unmapped CCs flagged — then
  saved into the project as a new song file.

### 6.3 New Song flow (write-through registration)

The "New Song" wizard collects name, voicegroup, player (BGM/SE), and `midi.cfg`
flags; writes the `.mid` and the `midi.cfg` line; then **registers the song
itself**, writing one line into each of the three registration files. The
voicegroup picker (both blank and import modes) also offers *"create a new
voicegroup for this song"* on per-file-layout projects: the wizard creates
`sound/voicegroups/<label>.inc` from the dummy template, points the song's
`-G` at it, and the user configures its voices in the Voicegroup dock
afterwards.

```
sound/song_table.inc      →  song mus_foo, MUSIC_PLAYER_BGM, 0
include/constants/songs.h →  #define MUS_FOO 610
ld_script.ld              →  sound/songs/midi/mus_foo.o(.rodata);
```

Each line is computed from the parsed project (next free ID, existing
indentation/alignment) and inserted after the file's last matching entry; every
other line keeps its exact bytes. Registration is idempotent — existing entries
are left untouched, except a `songs.h` define whose ID drifted from the song's
table index, which is corrected in place. Projects whose `ld_script.ld` has no
per-song object lines skip that file.

If registration fails (e.g. an unwritable file), the chosen constant/player
persist in the sidecar and the song shows a badge in the song browser;
**File → Register Song** retries. The same action registers stray `.mid` files
dropped into `sound/songs/midi/` by hand.

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

**Later / opt-in ideas (explicitly out of scope for v1):**
keysplit table *editing*, sample import (wav2agb semantics), full project
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
