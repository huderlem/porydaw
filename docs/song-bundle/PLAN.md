# Song Bundles (`.porysong`) — Implementation Plan

Status: **decided 2026-09-17; Phase 0 landed 2026-09-17 (committed 1d2da1f), Phase 1 landed 2026-09-18 (committed 4b65f6a), Phase 2 landed 2026-09-18 (root refactor committed 816e771, rest staged).** Phases are ordered; each is
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
Status: **LANDED 2026-09-17 on song-bundle, swept normal + ASAN** (also
builds with PORYDAW_SCRIPTING=OFF). Uncommitted, staged.

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

Deviations (Phase 0):
- Branch `song-bundle` was created from `porysong` (= `main` d46706c + the
  plan-doc commit 0139a5d) rather than bare `main`, because this PLAN.md
  only exists on that branch. `main` is an ancestor, so nothing else differs.
- miniz 3.1.2 (release amalgamation) is vendored at `external/miniz/` with
  its upstream `LICENSE` and an `external/miniz/LICENSES.md` notice that
  mirrors `external/dr_libs/LICENSES.md`; there is no central third-party
  notices file in the repo (`RELEASE-README.txt` carries none).
- miniz configuration (`MINIZ_NO_STDIO`, `MINIZ_NO_TIME`,
  `MINIZ_NO_ZLIB_COMPATIBLE_NAMES`) is set as **PUBLIC compile definitions
  on `porydaw_app`** in CMake, not inside `miniz_impl.c`, so the harness
  (which includes `miniz.h` to forge hostile zips) sees the same config;
  `external/miniz` is on the PUBLIC include list for the same reason. All
  file I/O goes through Qt; miniz only ever sees memory buffers (avoids
  miniz's fopen on non-ASCII Windows paths).
- "Fixed timestamps": rather than miniz's `MINIZ_NO_TIME` zero date (day 0 /
  month 0, which some tools render as a blank date), `createBundle`
  patches every local + central header to a real 1980-01-01 00:00:00 DOS
  stamp after finalizing, so archives are deterministic **and** well-formed
  (Python `zipfile` reads `(1980, 1, 1, 0, 0, 0)`).
- Entry order is the byte order of the UTF-8 names (locale-independent);
  empty directories are not recorded; deflate level 9.
- `createBundle` also refuses a source tree without a root `porysong.json`
  and enforces the same entry/size caps as extraction (a bundle we write
  must be one we can read). Every file name is validated with the same
  rules as extraction.
- Extraction guards beyond §3.1: encrypted entries, unsupported compression
  methods, duplicate (post-normalization) names, a name that is both a file
  and a directory prefix, and an archive *file* larger than the cap plus
  4 MiB of header slack (refused before reading). Directory entries are
  validated but never materialized. All guards run before the destination
  directory is created.
- API beyond the spec (all in `BundleArchive`): `listBundle` (archive-order
  entry names — Phase 1's "exact file list in the zip" assertion),
  `extractBundleLimited` (explicit caps for the harness),
  `validateEntryName`.
- `BundleManifest::fromJson` additionally requires a non-empty
  `song.label` and `song.voicegroup`; `constant`/`player` null ↔ empty
  QString; every optional array may be omitted. `toJson` is
  `QJsonDocument::Indented` with QJsonObject's sorted keys (deterministic).
  `BundleManifest::write` creates the bundle root directory if missing.

### Phase 1 — Export
Status: **LANDED 2026-09-18 on song-bundle, swept normal + ASAN.**
Uncommitted, staged.

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

Deviations (Phase 1):
- **Loader facts §2 did not record (verified in `voicegroup_loader.c`), and
  what export does about them:**
  - *Sub-voicegroup contiguity* (`load_sub_voicegroup` / `contiguousFill`,
    `:2089-2166`, `:2259-2279`): a sub-voicegroup shorter than 128 voices
    keeps filling from whatever is assembled after it — the rest of a
    monolithic file, or the next files in `sound/voice_groups.inc` include
    order — until a `voice_group` header. Macro-style projects (modern
    pokeemerald) never continue; label-style ones do, and old drumsets rely
    on it. So "emitted whole" means *own lines + that overflow region*: the
    bundle's sub-voicegroup file is padded out to 128 voices with the lines
    the loader would have reached, and their samples/waves are bundled.
    `voice_keysplit*` lines in the overflow region become a **silent**
    square (`voice_square_1 60, 0, 0, 2, 0, 0, 0, 0` — attack/decay/sustain
    0, the engine ends the note as it starts; fixed 2026-09-18 after review:
    the audible unused-slot dummy made a key beep that the project plays
    silent, since `resolve_voice` returns NULL for a nested keysplit):
    the loader never follows them there, but WOULD from the group's own file
    (and an include-order cycle then recurses until the stack overflows —
    hit and fixed during this phase). Phase 3 consequence: such a padded
    sub-voicegroup never content-matches the target's original, so it is
    always "create".
  - *`cry` / `cry_reverse` voices read the raw `.incbin` path only*
    (`load_wave_data`, no `.bin`→`.wav` mapping, `:2556-2605`), i.e. the
    `.bin` build artifact. Export copies that `.bin` verbatim as
    `sound/direct_sound_samples/<name>.bin` and refuses (unresolved) when
    the project isn't built. A symbol used by both a `cry` and a
    `voice_directsound` line carries both files. stock pokeemerald
    voicegroups contain no `cry` lines (cries play through
    `cry_tables.inc`), so this is an edge, exercised by the fixture only.
  - Sub-voicegroup files are found by the loader as
    `sound/voicegroups/<symbol minus "voicegroup_">.inc` (probe step 1), so
    that is the bundle file name (`route101_drumset.inc`, `voicegroup005.inc`)
    — not the project's `drumsets/route101.inc`, which would collide with
    the top-level `route101.inc` in a flat directory. The top-level file is
    the first `DecompProject::voicegroupCandidates` name.
- **Layout addition:** programmable-wave data files are bundled as
  `sound/programmable_wave_samples/<name>.pcm` (`<name>` = symbol minus
  `ProgrammableWaveData_`); §3.2 listed the `.inc` but not where its
  `.incbin` targets live.
- **`requires.extensions`:** there is no project-capability "lane gating"
  in porydaw to reuse (§3.3/§3.5 assumed one; CC 0x05/0x17/0x19 are merely
  classified `Advanced` in `ui/m4asemantics.cpp`). `usedExtensions()` lists
  the engine's opt-in commands the MIDI emits — `PORTAMENTO` (CC 5), `PWMC`
  (CC 0x17), `PWMS` (CC 0x19), the opt-in set of `kCcDefaults` in
  `core/timelineplayer.cpp`. **Phase 3's "existing lane gating then shows
  them grayed" does not exist either** — the import warning stands alone.
- **`flags` / `midi.cfg`:** the `-G` value keeps mid2agb's real form
  (`-G_route101`; the §3.3 example's `-G voicegroup_route101` is not what
  mid2agb or `parseMidiCfg` accept). Flags are
  `SongRegistry::mergeCfgFlags(doc.cfg())`, so unsaved Song Settings export.
  `manifest.voicegroup` is the full symbol (`voicegroup_route101`).
- **`layout`:** no layout concept exists in the code; written as
  `pokefirered` when the top-level voicegroup is a monolithic section,
  else `pokeemerald`. Informational only.
- **Exporter input** is `(projectRoot, const SongDocument&, const
  VoicegroupSource*)` + `setRegistrationHints` + `setPendingSynths`, not a
  `SongSession&` (`songsession.h` drags `ui/songview.h` into `project/`, and
  the harness needs no MainWindow). Lives in `src/project/bundleexport.{h,cpp}`;
  `stage(dir)` writes the folder form, `exportTo(zip)` = stage + zip.
  Minted-but-unsaved Golden Sun synths (`MainWindow::m_pendingSynths`) are
  honoured, like unsaved voice edits.
- **Voicegroup walk:** new read-only `VoicegroupSource::parseSource(bytes,
  sectionLabel)` exposes the existing `parse()` line classification
  (`VgSourceLine`: raw, kind, slot, voice, cry symbol); the exporter walks
  `renderPreview()` bytes with it. No change to open/edit/save behaviour.
- **Trimming:** only slots that have a source line are rewritten; a
  voicegroup shorter than 128 lines (or with a starting-note header) is not
  padded. A used program with no line / a Broken line is copied as-is (the
  project plays it silent too) — only *symbols* that fail to resolve refuse.
- **Resolution scope (v1):** symbols must be defined in
  `sound/direct_sound_data.inc`, `sound/direct_sound_synth_data.inc`,
  `sound/programmable_wave_data.inc`, `sound/keysplit_tables.inc`. The
  loader's deep-scan fallbacks for nonstandard forks (`<dir>/<symbol>.wav`,
  keysplit data probed in other dirs, config overrides) are not mirrored;
  such symbols refuse as unresolved. A sample committed only as raw `.bin`
  is carried as `.bin`. Bundle file-name clashes (two symbols → one
  case-folded name) refuse.
- Keysplit tables are copied as source text (declaring line through the
  last data line, comments inside kept, trailing blank/comment/`.align`
  lines dropped), in the project's own macro form. Synth definitions are
  re-rendered canonically (`set_synth_pulse|saw|triangle`), which the
  loader accepts regardless of the project's macro names.
- **Harness:** fixtures are built in `src/bundleexportcheck.cpp`
  (`buildWavProject` writes placeholder `.wav`s the loader can't read, so
  nothing was shareable). Beyond the spec: project-vs-bundle
  `voicegroup_load` equality on every used slot (sub-voicegroups included),
  unsaved-edit + pending-synth export, all refusals, per-file and monolithic
  label-style overflow projects, a second used DirectSound program (1) next
  to the implicit program 0, and — when `PORYDAW_SAMPLE_CORPUS` is set, as
  the sweep does — every 8th playable song of the real tree (60 songs of
  pokeemerald) exported and load-compared. The File-menu action itself
  (dialog + status message) is not driven offscreen.

### Phase 2 — Read-only bundle tab + instant listen
Status: **LANDED 2026-09-18 on song-bundle, swept normal + ASAN.** Root
refactor committed 816e771; the rest (lock, banner, gating, no-project
tolerance, `openBundle`, entry points, harness) staged, uncommitted.

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

Deviations (Phase 2, root refactor):
- `SongSession::root` is set in `createSession()` from `m_project.root()`.
  Converted to the session's root: `loadVoicegroupFor` (now takes `root`
  first; `loadSong` passes `m_project.root()` because the voicegroup loads
  before the session exists), `openVoicegroupSource`,
  `reloadVoicegroupPreview`, `ViewSidecar::load/save` (`loadSong`,
  `saveViewState`), `saveSession`'s `writeSynthDefinitions`, the bundle
  exporter's project root, `synthDescForSymbol(root, symbol)`.
- **The voicegroup catalog + sample batch stay one MainWindow-wide cache,
  now keyed by root** (`vgCatalog(root)`, `ensureSampleSet(root)`,
  `VgCatalog::root`): a call with a different root invalidates and rescans
  (which also frees the sample batch loaded from the old root). Session
  paths pass the session's root (`updateVoicegroupBrowser`, Song Settings,
  synth lookups); browser-signal paths that carry no session (picker
  sample/wave/keysplit audition, the synth-mint lambda) use the new
  `activeRoot()` (active tab's root, else the project's); project-scoped
  callers (New Song wizard, scripting catalog/typicalAdsr, sample import,
  `createVoicegroupNamed`) pass `m_project.root()` explicitly. Cost to know
  about: alternating between a bundle tab and a project tab rescans the
  catalog on each switch. Not made per-session (would be a redesign).
- `cleanupVgPreview()` removes `.porydaw/vgpreview` under the project root
  **and** every open session's root (previews are written under the
  session root). A locked bundle tab never writes one, so this is only
  belt-and-braces for the temp dir.
- **Deliberately left on `m_project.root()`** (project-scoped writers a
  bundle tab must have disabled, not re-rooted): sample import / Edit
  sample (`importSampleForSlot`, `editSampleForSlot`), register /
  unregister / delete song, `createVoicegroupNamed`, New Song. Also left:
  the scripting side (`scriptapi.cpp` `storage.song` sidecar paths at
  `ViewSidecar::pathFor(p->root(), …)` and `Sidecar::ensureDir(p->root())`)
  — it has no session root plumbing; the rest of Phase 2 must either gate
  those for bundle sessions or route the session root through
  `HostBindings`.
- `loadSong`'s replace-in-place path does not re-assign `session->root`
  (a project session's root cannot differ today). `openBundle` must never
  reuse a project session in place, or must set `root` itself.
- Harness: two assertions added to `--vgsavecheck` (session root == project
  root; voicegroup source opened beneath it). No bundle behaviour yet, so
  no new `--bundlecheck` section in this slice.

Deviations (Phase 2, bundle tab):
- **`openProjectDir` keeps bundle tabs** (§6 default, now asserted):
  `teardownSessions` destroys only project sessions; `openProjectDir` then
  force-re-activates the surviving current tab (currentChanged is
  suppressed during teardown) and refreshes every Import button.
- **Tab title is the plain prefix `[bundle] <label>`**, not `📦`: whether
  the emoji renders depends on a system fallback font that can't be checked
  offscreen, so the safe option §3.6 allowed was taken. Tooltip = the bundle's path; window title = `<label> — <file>`.
- **Banner lives inside `SongView`** (`SongView::setTopBanner`, a strip
  above the ruler), because every harness and `sessionForWidget` identify a
  tab page as `session->view`; wrapping the page would have touched them
  all. The button is `SongSession::bundleImportButton` (objectNames
  `bundleBanner` / `bundleBannerText` / `bundleImportButton`).
- **The Import button is a stub**: enabled iff a project is open (tooltip
  "Open a decomp project first" otherwise); clicking shows "not available
  yet". `MainWindow::importBundle(SongSession&)` is where Phase 3 lands.
- **Lock:** `SongDocument::setLocked` — `pushCommand` deletes the command
  unapplied (every command mutates only in `redo()`, verified: the only
  `m_undoStack.push` sites are `pushCommand` and the edit-group
  `DiscardRedoCommand`), `isDirty()` is false, `save()` refuses,
  `canAddTrack()` is false. No debug assert was added ("assert no caller
  depends on the push"): callers that read a result already handle the
  refused case (`addTrack` → -1); SongView gestures on a bundle tab simply
  do nothing. **SongView itself has no read-only mode** — drags still show
  their live preview and then snap back. Cosmetic; not in the spec.
- **Gating beyond §3.6:** Export Song Bundle is also disabled on a bundle
  tab (you already have the file; re-export from a bundle root is
  untested), the toolbar Volume/Tempo spinners are disabled (they are cfg /
  tempo edits), `VoicegroupBrowser::setViewOnly` disables the selector,
  New…, and the whole voice editor (so the sample picker's browse audition
  is off too; pressing voice rows still auditions). `pushVoiceEdit`,
  `onVoiceEditRequested`, `saveSession`, `openSongSettings`,
  `exportBundle`, `newVoicegroup`, `editSampleForSlot`,
  `importSampleForSlot(slot>=0)` each re-check `session.bundle`/lock.
  No view sidecar is read or written for a bundle tab.
- **Scripting:** `ScriptHost::beginTransaction` refuses a locked document
  ("the song is read-only (a song bundle)…"), which covers every `edit.*`
  call (all go through a transaction). `storage.song.*` refuses on a locked
  document — this is how the root-refactor's open item (scriptapi sidecar
  paths on `p->root()`) was settled: gated, not routed. `song.save` fails
  through `saveSession`. Not covered by a `--scriptcheck` case (needs a
  fixture plugin + bundle; Phase 4 touches scriptcheck anyway).
- **Same label in bundle and project:** `sessionForLabel` skips bundle
  sessions (bundles are found by `sessionForBundlePath`, canonical path),
  `refreshSessionSongIds` leaves a bundle's `songId` at -1, `loadSong`
  never replaces a bundle tab in place (it opens a new tab instead),
  `persistOpenTabs` skips bundle tabs and never records one as the last
  song.
- **Untrusted input:** new `SongBundle::readSong(root, manifest*, SongInfo*,
  error)` (in `project/songbundle`, harness-testable) refuses a label or
  `-G` value that isn't `[A-Za-z0-9_]+`, and any `.incbin`/`.include` target
  in the bundle's `*.inc`/`*.s` that fails `BundleArchive::validateEntryName`
  (absolute, `..`, …) — the loader opens those verbatim relative to root, so
  without this a bundle could make porydaw read files outside its temp dir.
  Phase 3 must call `readSong` (or the same checks) before planning.
- **cfg:** `DecompProject::parseMidiCfgLine` + `cfgFromFlags` are now public
  statics (factored out of `parseMidiCfg`, behaviour unchanged). The bundle's
  own `midi.cfg` line wins; the manifest's `flags` string is the fallback.
  Track budget is left at the document default (no music player table in a
  bundle).
- **Folder bundles** open in place (root = the folder, `bundleDir` null,
  nothing written under it); `.porysong` files extract to
  `<tmp>/porysong-XXXXXX/bundle`, removed when the session dies.
- **CLI:** `MainWindow::openCommandLinePaths(args)` opens every positional
  argument that `isBundlePath` (existing `*.porysong` file or bundle folder);
  anything else — including a mistyped path — is silently ignored. Called
  inside `showCoveredWhileRestoring` right after `restoreSession`.
- **Drop:** the open is deferred with a 0 ms timer so a failure's message
  box never runs inside the source application's drag.
- **Harness** (`src/bundletabcheck.cpp`, run by `runBundleCheck` after the
  export sections against `export_a.porysong` + a copy of the macro fixture
  project given a `song_table.inc`/`midi.cfg`): "non-silent output on used
  programs" is asserted as *transport advances* + *an offline `exportWav`
  render of the bundle tab peaks > 500* (whole song, not per program — the
  per-slot resolution is already Phase 1's load-equality assertion). The
  CLI path is exercised through `openCommandLinePaths`, not by spawning the
  binary. `PORYDAW_BUNDLE_SHOT=<png>` saves the window for visual review.
  The Open Song Bundle… dialog itself is not driven.

### Phase 3 — Import
Status: **not started**

Carry-over from Phase 2 (read before starting):
- **Go through `SongBundle::readSong`** (or apply the same checks) before
  planning anything from a bundle root: it is what refuses a non-identifier
  label / `-G` value and any `.incbin`/`.include` path that escapes the
  root. `makeImportPlan(bundleRoot, …)` must not trust a root that has not
  passed it.
- The Import button is a stub: `MainWindow::importBundle(SongSession&)`
  (shows "not available yet") is the entry point to replace. The session
  carries the parsed `manifest`, `root` (extracted bundle) and `bundlePath`.
- `vgCatalog(root)` returns a reference into ONE MainWindow-wide cache keyed
  by root. Import works with two roots (bundle + project): never hold the
  returned reference across a call made for the other root, and expect a
  rescan on every bundle-tab ↔ project-tab switch.
- `SongView` has no read-only mode: on a bundle tab, note/lane drags show
  their live preview and then snap back (the lock drops the command).
  Cosmetic; fix only if the user asks.
- Positional CLI paths that are not an existing `.porysong` file or bundle
  folder (including mistyped ones) are silently ignored by
  `openCommandLinePaths`.
- The scripting gate on locked documents (`beginTransaction`,
  `storage.song.*`) has no `--scriptcheck` case yet; Phase 4 owns that.

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
