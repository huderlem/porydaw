# Sample Studio — Context Dossier

This is the verified-facts reference for implementing **Sample Studio**: end-to-end
custom DirectSound instrument sample creation inside porydaw (import → crop /
retune / resample / normalize / loop → audition → commit into the decomp
project). Everything here was verified against real code on 2026-07-19; line
numbers are cited alongside function names because line numbers drift — trust
the names, re-grep when in doubt.

Companion documents:
- [PLAN.md](PLAN.md) — architecture, module contracts, phased implementation plan.
- [FORMATS.md](FORMATS.md) — byte-level file format and pitch-math reference.
- [DSP.md](DSP.md) — signal-processing algorithms and acceptance criteria.

## 1. What the feature replaces

The manual workflow for adding a custom instrument sample to a pokeemerald-style
project (external tools: Wavosaur, etc.):

1. Obtain a sample.
2. Retune it to middle C.
3. Resample to a low rate (e.g. 6689 or 13379 Hz) — higher rates are wasted
   because of the m4a engine's mixing rate.
4. Crop the attack (and tail for one-shots).
5. Normalize volume to an unstated "right" level.
6. Fiddle with loop points until the loop doesn't click.
7. Save the .wav into the samples directory and hand-register it.

Sample Studio does all of this in porydaw. Two of the steps dissolve entirely:

- **Retuning needs no DSP.** The `smpl` chunk of the .wav carries the base MIDI
  key, and wav2agb bakes it into the sample's pitch word at build time (see
  FORMATS.md §3). porydaw *never* resamples audio for tuning.
- **The "magic" normalization level is now measured**: the sc88pro reference
  corpus is peak-normalized to ≈ −0.3 dBFS with looped material at loop-region
  RMS ≈ −9 dBFS (see DSP.md §5).

## 2. Decomp-side ground truth

### 2.1 The sample pipeline is .wav + wav2agb

**Upstream/vanilla pret pokeemerald uses `.wav` sources compiled by
`tools/wav2agb` — NOT `.aif`/aif2pcm.** (Authoritative user correction;
`.aif` checkouts on disk such as `pokeemerald-pokezelda` are old forks that
predate pret's wav2agb migration. Legacy `.aif` projects get an actionable
refusal in phase 1 and a documented fallback path — FORMATS.md appendix A.)

Reference checkout: `/home/huderlem/pokeemerald` (branch `m4a_extensions`).
`sound/direct_sound_samples/` holds 544 paired `.wav` (source) / `.bin`
(build artifact) files, including `phonemes/` and `cries/` subdirectories
(cries are compressed via a different build rule and are **out of scope**).

### 2.2 Build rules (`audio_rules.mk`)

```make
# Uncompressed sounds (audio_rules.mk ~:25-27)
$(SOUND_BIN_DIR)/%.bin: sound/%.wav
	$(WAV2AGB) -b $< $@

# Compressed cries — OUT OF SCOPE (DPCM, different flags)
$(CRY_BIN_DIR)/%.bin: $(CRY_SUBDIR)/%.wav
	$(WAV2AGB) -b -c -l 1 --no-pad $< $@
```

So a new sample needs **no per-file build wiring** — dropping `name.wav` into
`sound/direct_sound_samples/` and `.incbin`-ing `name.bin` is enough, provided
the pattern rule exists. The registrar must verify this rule (grep the
project's `*.mk`/`Makefile` for a `wav2agb`-based `%.bin: %.wav` rule) and
refuse actionably if absent.

### 2.3 Registration grammar (`sound/direct_sound_data.inc`)

One block per sample, appended in file order:

```asm
	.align 2
DirectSoundWaveData_<name>::
	.incbin "sound/direct_sound_samples/<name>.bin"
```

Symbol convention: `DirectSoundWaveData_<basename>`. Voicegroups reference the
label via macros in `asm/macros/music_voice.inc`:

```
voice_directsound             base_midi_key, pan, sample_ptr, attack, decay, sustain, release   @ type 0
voice_directsound_no_resample ...                                                               @ type 8
voice_directsound_alt         ...                                                               @ type 16
```

### 2.4 Engine facts that shape the design

- Default m4a mixing rate is **13379 Hz**; shipped sample rates cluster on
  13379 and its ×/÷2 ladder (1672, 3344, 6689, 13379 — 17 of 45 sc88pro
  files — 26758, 53516) plus stragglers (11025, 22050, 44100). Fractional
  true rates exist (e.g. 3344.75 Hz, carried exactly by the `agbp` chunk).
- The fork's engine patch (`m4a_1.s`, commit `d9bc5632ed`) makes the voice's
  `base_midi_key` transpose DirectSound notes; vanilla ignores it for
  DirectSound. This is **orthogonal** to the sample's own base key (which
  wav2agb bakes into the pitch word and every engine honors) — treat voice
  `base_midi_key` semantics as project-dependent and keep emitting the
  project-conventional value (60) in generated voices.

## 3. porydaw / pory4a integration inventory

### 3.1 Sample *reading* is already solved (pory4a)

`external/poryaaaa/plugin/voicegroup_loader.c` (submodule; standalone repo at
`/home/huderlem/m4a_plugin`):

- `load_wav_from_path()` (≈:994-1236) — RIFF/WAVE: `fmt ` (8/16/24/32-bit int
  + 32/64-bit float), `smpl` (base key @12, pitch fraction @16, first loop
  record), custom `agbp`/`agbl` chunks. **Downmixes to signed 8-bit** — by
  design (playback), which makes it *unusable for hi-res import*; Sample
  Studio needs its own hi-res readers (PLAN.md §SampleImport).
  Verified internals: loop-end → `size` with `agbl` override (≈:1132-1142);
  pitch = `agbp ?: rate·2^((60−key)/12 + tuning/1200)·1024` (≈:1144-1154);
  `status |= 0x4000` when looped (≈:1162).
- `load_aif_from_path()` (≈:1261-1444) — aif2pcm-identical AIFF path (legacy).
- `load_wave_data()` (≈:1485-1537) — raw `.bin` (16-byte header verbatim).
- Symbol → file resolution order: **`.wav` → `.aif` → `.bin`**
  (`load_wave_data_from_wav()` ≈:1451-1480), driven by `Label::`/`.incbin`
  parsing of `direct_sound_data.inc`-style files.

Playback ground truth in `external/poryaaaa/plugin/m4a_channel.c`:

- `loopLen = wav->size − wav->loopStart` (≈:71) — **the loop end IS the sample
  end**; there is no post-loop tail. `loopLen <= 0` degrades to no-loop (≈:72).
- Sample fetch is **linear-interpolated**, with the interpolation wrapping
  across the loop seam (≈:286-290). Seam-click metrics are defined against
  this (DSP.md §6).
- Per-channel mix contribution is `(s8 · envVol) >> 8` (≈:183-185); master
  volume is folded into `envVol`. Sample headroom does not prevent mix
  clipping — normalization targets corpus consistency, not safety (DSP.md §5).

In-memory structs are plain and public (`plugin/m4a_engine.h:70-96`):
`WaveData { u16 type; u16 status; u32 freq; u32 loopStart; u32 size; s8 *data; }`
and `ToneData` (type/key/panSweep/wav/ADSR). porydaw may construct them in its
own memory and hand them to the engine — `SynthToneBuf`
(`src/songsession.h:33-36`) is the existing precedent.

### 3.2 Project write-back templates (porydaw)

`src/project/voicegroupsource.{h,cpp}` is the model to mirror:

- `ensureSynthDataIncluded()` (≈:249-337) — the canonical "wire a new build
  include" pattern: scan for an existing reference, find the anchor line,
  insert a sibling with matching indentation and EOL, `QSaveFile`-atomic,
  refuse with an actionable message when the anchor is missing.
- `writeSynthDefinitions()` (≈:1020-1128) — append entries to a data `.inc`,
  creating it if missing.
- `symbolsFromFiles()` (≈:129-162) — `Label::` + `.incbin "path"` parsing;
  feeds the catalog static `directSoundSymbols()` (decl `voicegroupsource.h:209`).
- `createVoicegroup()` (≈:1276) + `appendIncludeLine()` (≈:1386) — new-file +
  hub-include creation, matching sibling style/EOL.

House rules embodied by all of the above: **byte-conservative** edits (only
changed lines change bytes), **CRLF-preserving** per line, **QSaveFile-atomic**
writes, **actionable refusals** instead of silent skips or broken builds.

### 3.3 Undo / save / catalog

- `SongSession` (`src/songsession.h`) owns the song document and the
  `VoicegroupSource` behind **one shared `QUndoStack`**.
- Voice edits are applied by the owner (MainWindow) as `VoiceEditCommand`
  (`src/mainwindow.cpp` ≈:64); widgets are pure views that emit edit-request
  signals (e.g. `voiceEditRequested(slot, voice, structural)`,
  `src/ui/voicegroupbrowser.h` ≈:78-91).
- **Write-through commits stay off the undo stack**: `createVoicegroup`, song
  registration, and synth-definition writes are not undoable. Sample commit
  follows the same rule; only the subsequent *voice assignment* is undoable.
- `MainWindow::VgCatalog` (`src/mainwindow.h` ≈:198-207) caches the six
  project-wide scans (including `directSoundSymbols()`); any project write
  must call `invalidateVgCatalog()` (≈:209).

### 3.4 Audio engine (porydaw side)

`src/audio/audioengine.{h,cpp}`:

- A **second engine instance** `m_previewEngine` (≈.h:96-99, 202) already
  isolates auditions from song playback.
- The established lock-free UI→audio-thread idiom is the **generation-counted
  command atomic** (`m_previewCmd`, `m_previewVoiceCmd`, ≈.h:209-233) plus an
  SPSC ring for polyphonic timed previews. The sample-audition slot design
  (PLAN.md §4) extends this idiom.
- GBA mix-rate presets: `kGbaMixRates[]`
  (`src/ui/enginesettingsdialog.cpp:19-21`), default 13379.
- `src/audio/wavexport.{h,cpp}` is the 16-bit-stereo *song* exporter. Do not
  grow it into a sample writer — the sample writer is a separate module.

**No DSP utilities exist anywhere in porydaw or pory4a** — no resampler, no
normalizer, no loop detection. Everything in DSP.md is net-new.

### 3.5 UI and harness conventions

- Persistent project-state views are docks (VoicegroupBrowser, Polyphony);
  transactional flows are dialogs/wizards where the widget collects choices
  and **MainWindow does the writes** (NewSongWizard). Sample Studio is a
  modal dialog (PLAN.md §5).
- The import wizard's instrument-mapping step was **deliberately removed** —
  never reintroduce program-remap UI in any form.
- Test harnesses are in-binary: `src/<x>check.cpp` exposing
  `int run<X>Check(...)`, forward-declared and dispatched in `src/main.cpp`
  (≈:7-139), listed in `CMakeLists.txt`, run with `QT_QPA_PLATFORM=offscreen`.
  Every flag needs all its arguments; scratch projects must be fully fresh.
  Models: `--vgcheck` (builds fake decomp projects with
  `direct_sound_data.inc`), `--vgsavecheck` (exercises
  `ensureSynthDataIncluded`), `--exportcheck`. Sample Studio adds
  `--samplecheck`, with sections accreting per phase.
- Qt baseline is **6.2**; scratch-build philosophy (no heavyweight deps —
  vendored single-header C libraries are the accepted pattern; the engine
  plugin is plain C).

## 4. Reference corpus

`/home/huderlem/pokeemerald/sound/direct_sound_samples/sc88pro_*.wav` —
45 Roland SC-88 Pro instrument samples, 8-bit unsigned PCM mono, all with
`smpl` (unity note 60, fraction 0 — Sound-Canvas-side pre-tuned; porydaw
samples will carry *real* unity notes instead) and `agbp`/`agbl` chunks.
35 looped, 10 one-shot. Measured statistics and the derived normalization
constants live in DSP.md §5; reproduce them with
`tools/analyze_samples.py`.

## 5. Standing constraints (do not violate)

- Never gate a feature on a file-format limitation — prefer spec mechanism >
  sidecar > conversion.
- Never reintroduce program-remap UI.
- Never emit DPCM (compressed cries pipeline is out of scope).
- Zero poryaaaa/submodule changes are required by this design; if any emerge,
  they must land in `/home/huderlem/m4a_plugin` first and be bumped
  (pory4a pushes FIRST — standing repo rule).
- `midi.cfg` is CRLF; all project text writes preserve per-line EOL.
- Commits that write project files are not undoable; undo covers in-memory
  document/voicegroup state only.
