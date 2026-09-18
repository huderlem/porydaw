# Song Bundles (`.porysong`) — Implementation Plan

Status: **decided 2026-09-17, not started.** Phases are ordered; each is
independently landable on branch `song-bundle`, adds a `--bundlecheck`
section, and ends with its acceptance checklist ticked. Facts about the
codebase below were verified against `main` tip `d46706c` on 2026-09-17
(file:line refs may drift; re-grep before editing). Agents: work the phases
in order, update the **Status** line of the phase you land, and record any
deviation from this document in the phase's "Deviations" list rather than
silently changing the design.

## 0. Goal and user-confirmed scope

A standalone, shareable song file that bundles the MIDI + voicegroup +
every instrument sample the song needs, so a song can be listened to and
imported without the source decomp project.

User-confirmed decisions (2026-09-17):

| Item | Decision |
|---|---|
| Opening a bundle | Opens a **read-only tab** (plays, browses, no edits/saves). A button in that tab **imports** the song into the currently-open decomp project. |
| Hi-res sample sources | **Never bundled.** Only the committed GBA-ready sample files. |
| `.porydaw/` content | **Never bundled** (no view sidecars, no provenance sidecars, no registration meta). |
| Unused voicegroup slots | **Ignored.** Top-level slots the song never selects are replaced by a dummy `voice_square_1` line; their samples are not bundled. |
| Extensions / Golden Sun synths | Special care (see §3.5). A bundle always *plays*; importing warns/refuses per the target project's capabilities. |
| Joy feature in scope | **Instant listen, anywhere**: open a bundle with no project open, drag-drop onto the window, CLI positional argument. Everything else brainstormed (cards, UUIDs, audition preview, cover art, bug-report export, listenable-WAV wrapper) is **out of scope** for now. |

## 1. Design stances

1. **The bundle is a miniature decomp project.** poryaaaa's
   `voicegroup_load(root, name, config)` discovers by path-probing under any
   root (`external/poryaaaa/plugin/voicegroup_loader.c:707-802`), so a zip
   whose contents mirror `sound/...` loads from an extracted temp dir with
   **zero poryaaaa changes**. This is the load-bearing simplification; do
   not invent a private serialization of voices/samples.
2. **Zip container, folder-equivalent.** `.porysong` is a plain zip
   (vendored `miniz`, §3.1). A directory with the same layout is also a
   valid bundle for every code path (open/import take a root path; the zip
   layer only extracts/creates). Users can inspect bundles with any zip tool.
3. **Read-only means read-only at the document.** The bundle session's
   `SongDocument` is *locked*: `pushCommand` refuses and the tab shows a
   banner. UI gating (Save/Settings/Register disabled) is in addition to,
   never instead of, the document lock.
4. **Import composes existing writers.** `SampleRegistrar::registerSample`,
   `VoicegroupSource::createVoicegroup/appendIncludeLine/writeSynthDefinitions`,
   `SongRegistry::writeSongFlags/registerSong`. All writes remain
   byte-conservative QSaveFile writes, **not undoable** (same as sample
   registration). Import is a two-step *plan → apply*: the plan is a pure
   function of (bundle, project) and is what the harness asserts on.
5. **Dedupe by content, never by name.** Samples are identified by SHA-256
   of the committed file bytes (loop/pitch chunks included, so "same audio,
   different loop" is correctly a different sample). Sub-voicegroups,
   keysplit tables, programmable waves and synth definitions are deduped by
   normalized content after symbol remapping.
6. **Never gate on the target's limitations silently.** Extension usage and
   synth voices produce a *warning that names the specifics* and lets the
   user proceed, except where the project physically cannot assemble the
   result (no `set_synth_*` macros, legacy aif2pcm pipeline), where import
   refuses with instructions — matching the existing gates.
7. **Ephemeral extraction.** A bundle is extracted to a `QTemporaryDir`
   owned by its session; nothing under the user's project is touched by
   *opening* a bundle. Zip extraction rejects path traversal (`..`, absolute
   paths, drive letters, symlinks) and enforces a total-size cap.

## 2. Verified codebase facts (do not re-derive)

- **Single global project.** `MainWindow::m_project` (`src/mainwindow.h:365`)
  is the only `DecompProject`; `openProjectDir` (`mainwindow.cpp:1642-1694`)
  tears down every tab. Loader calls, `openVoicegroupSource`, `ViewSidecar`,
  sample registrar all read `m_project.root()` directly
  (`loadVoicegroupFor` `mainwindow.cpp:1715-1728`, `reloadVoicegroupPreview`
  `:3040-3072`). A bundle tab therefore needs its **own root string on
  `SongSession`** and those call sites must read the session's root.
- **`SongSession`** (`src/songsession.h:34-75`): `doc`, `vgSource`,
  `timeline`, `voicegroup`, `synthTones`, `view`, `songId`, applied cfg
  mirrors, `isDirty()`. No read-only concept exists anywhere.
- **Open path:** `MainWindow::loadSong(const SongInfo&, bool newTab)`
  (`mainwindow.cpp:1730-1834`): `loadVoicegroupFor` → `createSession()`
  (`:1205-1287`) → `doc.load(song)` → `buildTimeline` →
  `openVoicegroupSource` (`:3126-3138`, failure degrades to "editing
  unavailable") → `ViewSidecar::load` → `m_tabs->addTab`. Per-tab action
  enablement lives in `activateSession` (`:1326-1345`).
- **Loader failure policy:** only a missing top-level voicegroup or a parse
  error returns NULL (`voicegroup_loader.c:2618-2695`). Unresolved sample /
  wave / sub-voicegroup / keysplit-table symbols degrade to a silent voice
  (`td->wav == NULL` etc., `:2306,:2446,:2544,:2547`). `.incbin` paths are
  stored **verbatim relative to root** (`:904-948`) and `.bin` maps to
  `.wav` then `.aif` then raw `.bin` (`:1599-1628`). Discovery reads
  `sound/direct_sound_data.inc`, `sound/direct_sound_synth_data.inc`,
  `sound/programmable_wave_data.inc`, `sound/keysplit_tables.inc`,
  `sound/voicegroups/` (+`keysplits/`, `drumsets/`), and monolithic
  `sound/voice_groups.inc`. Golden Sun synth macros are resolved inline
  (`symbol_map_find_synth`, `:1731-1813`).
- **Sample registrar** (`src/project/samplereg.h:78-118`):
  `registerSample(root, name, wavBytes, error)` writes
  `sound/direct_sound_samples/<name>.wav` + appends the
  `DirectSoundWaveData_<name>::` block to `sound/direct_sound_data.inc`.
  `validateSampleName` **refuses collisions, never suffixes**.
  `probeSampleFormat` refuses `LegacyAif`. `sourceHashHex(bytes)` is the
  only SHA-256 helper (`samplereg.cpp:381-385`).
- **Voicegroup writer** (`src/project/voicegroupsource.h:183-300`):
  `createVoicegroup(root, name, copyFromFile, copySectionLabel, error)`
  requires `sound/voicegroups/` (per-file layout only; monolithic
  `voice_groups.inc` is read/edit-only). `appendIncludeLine`,
  `writeSynthDefinitions(root, list<pair<symbol, VgSynthDesc>>, error)`
  (equal existing → skip, differing → error, no macros → error).
  `renderPreview()` renders one voicegroup as standalone parseable bytes.
  **porydaw has no keysplit-table writer.** `catalogScan(root)` collects
  keysplit/drumkit instruments and DirectSound symbols.
- **Song registration** (`src/project/songregistry.h`): `makePlan`,
  `registerSong(root, label, constant, player, error, songId)` (song_table,
  songs.h, ld_script, charmap, debug.c; regioned layouts handled),
  `writeSongFlags(midiDir, label, flags, error)` for midi.cfg / songs.mk.
  `SongCfg` (`decompproject.h:8-24`) holds the parsed flags (`-G -V -R -P
  -E -X -N` + `rawFlags`).
- **Program usage:** `MidiTimeline` tracks per-track program changes
  (`src/core/miditimeline.h:11,32`); the set of programs a song selects =
  every program-change value on every track, plus program 0 for any track
  that emits notes before its first program change.
- **Vendoring pattern:** header-only libs are include dirs on `porydaw_app`
  (`CMakeLists.txt:200-205`); implementations are dedicated TUs
  (`src/audio/miniaudio_impl.c`, `src/audio/stb_vorbis_impl.c`). No zip code
  exists anywhere.
- **Harness pattern:** `src/<name>check.cpp` exporting `int
  run<Name>Check(...)`, forward-declared + dispatched in `src/main.cpp`
  (`:8-105`, `:135-260`), listed in `qt_add_executable(porydaw …)`
  (`CMakeLists.txt:246-276`), and added to `tools/run_checks.sh`.
  MainWindow-member harnesses (`runVgSaveCheck`, `src/vgsavecheck.cpp`)
  redirect QSettings to a `QTemporaryDir` and set `m_persistSession=false`.
  Scratch projects are built per-harness (`samplecheck.cpp:294-313`
  `buildWavProject`).
- **Scripting:** `ProjectApi` (`src/scripting/scriptapi.h:483-526`) calls
  `HostBindings` lambdas (`src/scripting/scripthost.h:67-130`) wired in
  MainWindow's ctor (`mainwindow.cpp:288-320`). Docs are generated from
  `docs/scripting/API.md` via `tools/gen_scripting_docs.py --check`.
- **No CLI file args, no drag-drop, no recent list** today (`main.cpp` parses
  only `--version/--selftest/--*check`; no `setAcceptDrops`).
- **File menu actions** attach to `keymap::Registry::instance().attach("file.…")`
  (`mainwindow.cpp:440-470`) — new actions must too.

## 3. Format

### 3.1 Container

- Extension `.porysong`; MIME-ish magic is just "zip containing
  `porysong.json` at the root". Deflate compression (samples are 8-bit PCM
  and compress well). Vendor **miniz** (single `miniz.h`/`miniz.c`, MIT) at
  `external/miniz/`, include dir on `porydaw_app`, compiled as
  `src/project/miniz_impl.c` (mirror `miniaudio_impl.c`). Add the license to
  wherever the other third-party notices live (check `RELEASE-README.txt` /
  `resources/`).
- Extraction guards: reject entries whose normalized path escapes the
  target, absolute paths, `..` segments, backslashes normalized to `/`,
  symlink entries; cap total uncompressed size (64 MiB) and entry count
  (4096); refuse a zip without root `porysong.json`.

### 3.2 Layout inside the archive

```
porysong.json
sound/songs/midi/<label>.mid
sound/songs/midi/midi.cfg                      # exactly one line: this song
sound/voicegroups/<vg>.inc                     # trimmed top-level voicegroup
sound/voicegroups/<sub>.inc …                  # every reachable sub-voicegroup (keysplit/drumkit), verbatim lines
sound/keysplit_tables.inc                      # only referenced tables (omit file when none)
sound/direct_sound_data.inc                    # only referenced symbols
sound/direct_sound_samples/<name>.wav|.aif     # referenced samples, byte-copied from the project
sound/programmable_wave_data.inc               # only referenced waves (omit when none)
sound/direct_sound_synth_data.inc              # only referenced set_synth_* defs (omit when none)
```

Rules:
- The `.incbin` path written into the bundle's `direct_sound_data.inc` is
  `sound/direct_sound_samples/<name>.bin` — the loader maps `.bin`→`.wav`
  →`.aif` itself. `<name>` is the project's symbol name with the
  `DirectSoundWaveData_` prefix stripped, **not** the project's original
  file path (which may live in subdirs like `cries/`). Cries are just
  samples here; DPCM-compressed cries copy verbatim (the loader decodes).
- Sub-voicegroups are emitted **whole** (all their lines; drumkits and
  keysplits select by key so every slot is live). Their DirectSound/wave
  symbols are all bundled.
- Top-level voicegroup: for each of the 128 slots, if the program is in the
  song's *used set* (§2 program usage) emit the line verbatim (after symbol
  renames, if any — export never renames), else emit
  `\tvoice_square_1 60, 0, 0, 2, 0, 0, 15, 0`. Header/label style copied
  from the source (`createVoicegroup` already sniffs both styles; reuse
  that logic). Pokefirered monolithic sections are exported to the per-file
  form.
- `midi.cfg` line is the project's line for this song with `-G` rewritten
  to the bundle's voicegroup name.
- `.mid` bytes are the **in-memory document** serialized (so unsaved edits
  export, like Export WAV), never `.porydaw`.
- Sample files keep the project's on-disk extension (`.wav` normally;
  `.aif` in legacy forks). `.aif` bundles play but can only be imported
  into… nothing (both pipelines refuse), so **export refuses when any
  referenced sample resolves to `.aif`** with a message naming them and
  pointing at the Sample Studio. Follow-up (not v1): transcode on export.

### 3.3 `porysong.json`

```json
{
  "format": 1,
  "porydaw": "1.2.0",
  "song": { "label": "mus_route101", "voicegroup": "voicegroup_route101",
            "flags": "-E -R50 -G voicegroup_route101 -V080",
            "constant": "MUS_ROUTE101", "player": "MPlayInfo_BGM" },
  "layout": "pokeemerald",
  "requires": { "extensions": ["PORTAMENTO", "PWMC"], "synth": true },
  "samples": [ { "name": "route101_flute", "file": "sound/direct_sound_samples/route101_flute.wav",
                 "sha256": "…" } ],
  "subVoicegroups": ["voicegroup_route101_drums"],
  "keysplitTables": ["KeySplitTable_route101"],
  "waves": ["ProgrammableWaveData_1"],
  "synths": ["DirectSoundSynth_GoldenSun_80_00_00_00"]
}
```

`format` is bumped on breaking layout changes; readers refuse a newer
`format`. `constant`/`player` are hints that prefill import (may be null).
`requires.extensions` lists the extension opcodes the MIDI actually emits
(computed from the document's automation/CC usage the same way the lane
gating does); `requires.synth` is true iff any bundled voice resolves to a
`set_synth_*` definition. `samples[].sha256` is the hash of the file bytes
as stored; readers **re-hash on import** and treat the manifest value as a
consistency check, not a trust source.

### 3.4 Import resolution rules

Given a bundle B and project P, the **plan** (a value type, §5 Phase 3)
resolves, in this order, because later stages need earlier renames:

1. **Samples.** Hash every `sound/direct_sound_samples/*` in P once
   (recursive; cache the map in memory for the session, keyed by path +
   mtime). For each bundle sample: same hash anywhere in P → *reuse* P's
   symbol; else if `DirectSoundWaveData_<name>` exists in P or the file
   exists → *rename* `<name>_2`, `_3`, … (first free); else *add*.
2. **Programmable waves.** Compare data words. Same content → reuse symbol;
   name clash → rename; else append to `sound/programmable_wave_data.inc`.
3. **Synth definitions.** Symbols are already param-named and value-deduped
   by `writeSynthDefinitions`; rely on it (equal → skip). Refuse the whole
   import when `requires.synth` and P has no `set_synth_*` macros.
4. **Keysplit tables.** Normalize (symbol, ordered entries). Same content →
   reuse; name clash → rename; else append to `sound/keysplit_tables.inc`
   in P's observed macro form (pokeemerald `keysplit` macro vs pokefirered
   `.set` form — detect from the existing file; if the file does not
   exist, create it with the pokeemerald form and refuse if the loader's
   discovery would not find it, i.e. verify with a `voicegroup_load`
   round-trip in the plan's dry-run).
5. **Sub-voicegroups.** Render the bundle's sub-voicegroup with renames 1–4
   applied; compare normalized voice lines against every P sub-voicegroup
   (from `catalogScan`). Same content → reuse; name clash → rename `_2`…;
   else create via `createVoicegroup` (copy lines) + `appendIncludeLine`.
6. **Top-level voicegroup.** Apply all renames; name clash → rename; create.
   Never reuse (the trimmed voicegroup is the song's own).
7. **Song.** Label clash in P (`song_table` label, `.mid` present, or
   constant defined) → rename `<label>_2`…; the Register dialog is shown
   prefilled (label, constant, player from manifest hints or the same
   derivation New Song uses) so the user can override. Write `.mid`,
   `writeSongFlags` (with `-G` = final voicegroup name), `registerSong`.
   Then `loadSongByLabel(label, newTab=true)` opens the imported song in a
   normal editable tab; the bundle tab stays open.

The plan is applied **only after every stage succeeds in dry-run**;
application order is 1→7 so a failure midway leaves a project that still
builds (samples/waves/tables/voicegroups without a song are harmless). A
failure message says exactly which step failed and what was already
written.

### 3.5 Extensions and Golden Sun synths

- **Playing** a bundle never depends on the host project: poryaaaa's engine
  implements the extensions and synths unconditionally.
- **Exporting** from a project records `requires` (manifest) — nothing else
  special.
- **Importing** into a stock-engine project with `requires.extensions`
  non-empty: warn (list the opcodes and the tracks/lanes using them),
  proceed on confirm; the existing lane gating then shows them grayed as
  usual. Into a project without `set_synth_*` macros with `requires.synth`:
  **refuse** naming the synth voices (the voicegroup would not assemble).
  Into a legacy aif2pcm project needing ≥1 *net-new* sample: **refuse** via
  the registrar's existing message. Reused-only imports into legacy
  projects are allowed.

### 3.6 Read-only bundle tab

- `SongSession` gains `QString root` (project root for normal tabs, temp
  extraction dir for bundle tabs), `bool bundle`, `QString bundlePath`,
  `std::unique_ptr<QTemporaryDir> bundleDir`, and the parsed manifest.
- `SongDocument` gains `setLocked(bool)`; `pushCommand` on a locked
  document deletes the command and returns without pushing (assert in
  debug builds that no caller depends on the push). Locked documents are
  never dirty.
- The tab shows a top banner: "Song bundle <file> — read-only. [Import into
  project…]" (button disabled with tooltip "Open a decomp project first"
  when `m_project` is closed). Tab title `📦 <label>` (or a plain prefix if
  the font check objects — verify with `--fontcheck`/`--themecheck`).
- Disabled for the bundle tab: Save, Song Settings, Register, Export WAV
  stays **enabled** (listening is the point), voicegroup dock is
  view-only (its edit widgets disabled; auditioning voices still works),
  Sample Studio "Edit sample…" disabled, Merge/track edits blocked by the
  lock. Scripting `edit.*` calls on a locked document fail with a clear
  error (`writeAllowed`-style).
- **No project open:** MainWindow must tolerate a bundle session with
  `m_project` closed (song list panel empty, every project-scoped action
  disabled). `restoreSession` does not persist bundle tabs.
- Closing the tab deletes the temp dir. Opening the same bundle twice
  focuses the existing tab (dedupe by canonical file path).

## 4. Entry points (Phase 2)

- File → **Open Song Bundle…** (`file.open_bundle`, filter `*.porysong`).
- File → **Export Song Bundle…** (`file.export_bundle`, active tab, default
  name `<label>.porysong`, seeded from the last bundle dir QSettings key).
- Drag-and-drop of `.porysong` files onto the main window
  (`setAcceptDrops` + `dragEnterEvent`/`dropEvent`; also accept a directory
  that contains `porysong.json`).
- CLI: `porydaw <file.porysong>` (positional args after harness parsing;
  opened after `restoreSession`).
- OS file association (Windows installer / macOS `Info.plist`
  `CFBundleDocumentTypes` + `QFileOpenEvent`): Phase 4, optional.

## 5. Phases

### Phase 0 — Container + manifest + harness skeleton
Status: **not started**

- Vendor miniz; add `src/project/bundlearchive.{h,cpp}`:
  `bool extractBundle(zipPath, destDir, error)`, `bool createBundle(srcDir,
  zipPath, error)` (deterministic entry order: sorted paths, fixed
  timestamps so re-export of identical content is byte-identical), guards
  per §3.1.
- `src/project/songbundle.{h,cpp}`: `struct BundleManifest` + JSON
  read/write (`QJsonDocument`), `format` check, `isBundleDir(root)`.
- `src/bundlecheck.cpp` + `--bundlecheck <scratch-dir>` dispatch + CMake +
  `tools/run_checks.sh` entry. Sections: zip round-trip, traversal/size
  rejection, manifest round-trip, newer-`format` refusal.

Acceptance: `--bundlecheck` PASS normal + ASAN; `clang-format` clean;
OFF/Windows-irrelevant.

### Phase 1 — Export
Status: **not started**

- `SongBundle::Exporter`: input (project root, `SongSession&`); computes
  the used-program set from the document, walks the voicegroup (parse via
  `VoicegroupSource` lines — do not re-parse the loader's binary result),
  collects the closure (§3.2), stages files into a temp dir, writes the
  manifest, zips. Refusals: `.aif` samples (§3.2), unresolvable symbols
  (name them — the loader would have played them silent, but a bundle
  must be complete), voicegroup not editable (`vgSource` null).
- File → Export Song Bundle… action + keymap id + QSettings last dir.
- Harness: build a scratch wav2agb project (extend `samplecheck`'s
  `buildWavProject` into a shared helper `src/checkfixtures.{h,cpp}` if it
  stays small; otherwise copy) with: 2 used DirectSound voices, 1 unused
  DirectSound voice, 1 keysplit (sub-voicegroup + table), 1 drumkit, 1
  programmable wave, 1 synth voice, 1 cry, a song using programs 0, 5
  (keysplit), 9 (drumkit), 20 (wave), 30 (synth), 40 (cry), with a
  portamento CC. Assert: exact file list in the zip, unused sample absent,
  dummy square line in the unused slot, manifest `requires`, and that
  `voicegroup_load(extractedRoot, vg, nullptr)` yields non-NULL `wav`/
  `subGroup`/`keySplitTable`/`wavePointer` for every used slot. Also:
  re-export → byte-identical zip.

### Phase 2 — Read-only bundle tab + instant listen
Status: **not started**

- `SongSession::root` refactor: every `m_project.root()` read inside
  per-session paths (`loadVoicegroupFor`, `reloadVoicegroupPreview`,
  `openVoicegroupSource`, `ViewSidecar::load/save`, synth preview, sample
  audition lookups) becomes `session.root`. Normal tabs set it from
  `m_project.root()` at creation. Mechanical, land first as its own commit
  with the full sweep green before any bundle behavior.
- `SongDocument::setLocked` + banner + action gating + no-project
  tolerance (§3.6). `MainWindow::openBundle(path)` → extract → manifest →
  synthesize `SongInfo`/`SongCfg` from the bundle's `midi.cfg` line (reuse
  `DecompProject::parseMidiCfg`'s line parser; factor it out if it is
  private) → `loadSong`-equivalent path → lock.
- Open Song Bundle… action, drag-drop, CLI positional arg.
- Harness (MainWindow-member, model `runVgSaveCheck`): open the Phase 1
  bundle with **no project open**: tab exists, transport plays ≥ N frames
  with non-silent output on used programs (reuse `--audiocheck` /
  `exportcheck` render probes), `pushCommand` is a no-op, Save/Settings
  disabled, Import button disabled; then open a project: Import button
  enabled, bundle tab survives `openProjectDir`? — **No**: decide and
  assert that `openProjectDir` keeps bundle tabs (they are project-
  independent). Drop-event and CLI-arg paths exercised offscreen.

### Phase 3 — Import
Status: **not started**

- `SongBundle::ImportPlan makeImportPlan(bundleRoot, projectRoot, error)`
  (§3.4) and `bool applyImportPlan(plan, error)`. New writers:
  keysplit-table append (`VoicegroupSource::appendKeysplitTable`),
  programmable-wave append, sub-voicegroup create-from-lines (reuse
  `createVoicegroup` copy path with an in-memory line source). Project
  sample hash map with per-session cache.
- Import dialog: summary table (samples: N new / M reused / K renamed;
  waves; tables; sub-voicegroups; voicegroup name; song label/constant/
  player fields prefilled; extension warning block; refusal text when the
  plan cannot apply). OK applies, then opens the imported song in a new
  editable tab.
- Harness: import the Phase 1 bundle into (a) an empty scratch project →
  all "add", registered song loads and plays identically (render compare
  against the bundle tab's render, sample-exact); (b) the *source* project
  → all samples/tables/sub-voicegroups "reuse", only the top-level
  voicegroup + song added with `_2` suffixes, no bytes change in
  `direct_sound_data.inc`; (c) a project with a same-named-different-hash
  sample → rename `_2` and the voicegroup references the renamed symbol;
  (d) a project without `set_synth_*` → refusal naming the synth voice;
  (e) a legacy-aif project → refusal; (f) stock engine → warning lists
  `PORTAMENTO`; (g) mid-apply failure injection (read-only
  `song_table.inc`) → message names the step and what was written. Every
  case re-runs `voicegroup_load` on the *project* afterwards and asserts
  the imported voicegroup resolves fully.

### Phase 4 — Reach + docs
Status: **not started**

- Scripting: `porydaw.project.exportBundle(label, path)` /
  `importBundle(path, {label, constant, player})` via `HostBindings`; update
  `docs/scripting/API.md` + `porydaw.d.ts`, regenerate with
  `tools/gen_scripting_docs.py`, `--scriptcheck` cases.
- Manual page `docsrc/manual/song-bundles.md` (+ nav in `mkdocs.yml`,
  `mkdocs build --strict`), `CHANGELOG.md` Unreleased entries (Added:
  Open/Export Song Bundle; Import).
- Optional: OS file associations (Windows installer, macOS
  `Info.plist`/`QFileOpenEvent`); `.porysong` in the README feature list.

## 6. Open items an implementing agent must not decide alone

- None blocking. If any of the following turns out to be wrong in code,
  stop and ask: the loader tolerating a trimmed voicegroup (§2), keysplit
  table macro forms (§3.4 step 4), whether `openProjectDir` should keep
  bundle tabs (Phase 2 — default: **keep them**).

## 7. Conventions (from memory; see `docs/RELEASING.md`, CLAUDE.md)

- Branch `song-bundle` off `main`; one commit per phase (Phase 2's root
  refactor is its own commit). No pushes without the user.
- Every phase: `clang-format` (CI pins clang-format 22), full
  `tools/run_checks.sh` sweep on the normal build **and** the ASAN build
  (`build-asan`), `mkdocs build --strict` when docs change, scripting docs
  `--check` when API.md changes.
- Never regenerate `samplecheck` fixtures. Never touch
  `external/poryaaaa` for this feature (stance 1).
- Write-through writes are QSaveFile, CRLF/indent-preserving, not undoable.
