# Sample Studio — Implementation Plan

Phased plan for building end-to-end custom DirectSound sample creation in
porydaw. Read [CONTEXT.md](CONTEXT.md) first (verified codebase/format facts
— do not re-derive them); byte-level formats in [FORMATS.md](FORMATS.md);
algorithms in [DSP.md](DSP.md). Each phase is independently landable, adds a
`--samplecheck` section, and ends with the acceptance checklist ticked.

User-confirmed scope: full waveform editor end state; inputs .wav/.aif +
mp3/ogg/flac + .sf2 zones; loop auto-suggest + manual fine-tune with looped
audition; single wav2agb output pipeline (retune via metadata, never DSP).

## 1. Design stances

1. **Non-destructive**: the editor never mutates the decoded buffer. It edits
   a `SampleEditParams` value applied by the deterministic DSP.md pipeline to
   an immutable hi-res `ImportedSample`. Re-render on every change; undo
   inside the editor is a params swap.
2. **Audition == build parity is a hard invariant**: the auditioned byte
   stream, porydaw playback after commit, and the ROM's `.bin` content are
   bit-identical (8-bit output + always-`agbp`, FORMATS.md §1), enforced by
   harness round-trips through pory4a's own `load_wav_from_path`.
3. **Write-through commit**: file writes (sample + registration) are atomic,
   byte-conservative, and **not undoable** — matching `createVoicegroup` /
   `writeSynthDefinitions`. Only the subsequent voice assignment is on the
   undo stack.
4. **Zero poryaaaa changes.** Audition constructs `WaveData`/`ToneData` in
   porydaw-owned memory (the `SynthToneBuf` precedent). If a plugin change
   ever becomes necessary, it lands in `/home/huderlem/m4a_plugin` first and
   is bumped (pory4a pushes FIRST).

## 2. Modules

| Module | Path | Responsibility |
|---|---|---|
| **SampleImport** | `src/audio/sampleimport.{h,cpp}` | Decode any source → `ImportedSample`. Front door `importAudioFile(path, &error)` dispatching on sniffed magic (not extension). wav via vendored **dr_wav** (metadata mode: `smpl` + unknown chunks `agbp`/`agbl`); aif via small in-house reader (crib 80-bit-float/SSND/MARK/INST semantics from pory4a's `load_aif_from_path`, but keep hi-res — the plugin readers downmix to 8-bit and are unusable for import); mp3/flac via **dr_mp3**/**dr_flac**, ogg via **stb_vorbis** (phase 4). Stereo → mean downmix (warn on negative L/R correlation, offer left-only). |
| **Sf2Reader** | `src/audio/sf2reader.{h,cpp}` | Minimal in-house sf2 zone extractor (~300-400 lines): `sdta/smpl` pool + `pdta shdr` (+ `phdr`/`inst`/`ibag`/`igen` only for grouping labels). Subset spec: FORMATS.md §5. Not a synth — TinySoundFont rejected (full renderer, awkward to strip-mine). |
| **SampleDsp** | `src/audio/sampledsp.{h,cpp}` | Pure stateless functions per DSP.md: `resampleSinc`, `suggestLoop` (+ `refineLoop`), `detectPitchYin`, `nearestZeroCrossing`, `normalizeGain`, `quantizeToAgb8`, peak-pyramid builder for the waveform view. |
| **SampleDocument** | `src/audio/sampledoc.{h,cpp}` | `ImportedSample` + `SampleEditParams` → cached render (`ProcessedSample`): final s8 bytes, output-domain loop integers, GBA header fields (freq/loopStart/size/flags), float preview buffer, seam metrics. |
| **SampleWavWriter** | `src/audio/samplewav.{h,cpp}` | `ProcessedSample` → .wav bytes per FORMATS.md §1 (8-bit unsigned fmt/data + smpl + agbp + agbl). Separate from `wavexport.cpp` (the song exporter — do not grow it). |
| **SampleRegistrar** | `src/project/samplereg.{h,cpp}` | `probeSampleFormat(root)`; name/symbol validation vs `directSoundSymbols()` + on-disk collisions; `registerSample(root, name, wavBytes, &error)` — QSaveFile write of `sound/direct_sound_samples/<name>.wav` + CRLF-preserving append of the registration block to `sound/direct_sound_data.inc` (grammar FORMATS.md §4), mirroring `ensureSynthDataIncluded` conventions; verify a wav2agb `%.bin: %.wav` pattern rule exists, refuse actionably otherwise. Separate file — `voicegroupsource.cpp` is already ~1400 lines; reuse its file-local helpers by extraction if needed. |
| **AudioEngine audition** | additions to `src/audio/audioengine.{h,cpp}` | In-memory sample audition on `m_previewEngine` — protocol in §4. |
| **WaveformView** | `src/ui/waveformview.{h,cpp}` | Zoomable waveform (peak pyramid); crop + loop drag handles; zero-cross snap toggle; seam overlay (pre-loop-end vs pre-loop-start windows superimposed); playhead during audition. Pure view: emits param-change signals; owner applies to the document. |
| **SampleEditorDialog** | `src/ui/sampleeditordialog.{h,cpp}` | The "Sample Studio" — §5. |
| **Sf2ZonePicker** | `src/ui/sf2zonepicker.{h,cpp}` | Searchable zone list (grouped by instrument/preset labels), pre-editor step for .sf2 files. |
| **Harness** | `src/samplecheck.cpp` | `int runSampleCheck(...)`; forward-declare + dispatch in `src/main.cpp`; add to `CMakeLists.txt`; `QT_QPA_PLATFORM=offscreen`; builds fully-fresh fake decomp projects (the `--vgcheck` pattern). Sections accrete per phase. |
| **Vendored decoders** | `external/dr_libs/` (dr_wav.h, dr_mp3.h, dr_flac.h), `external/stb/stb_vorbis.c` | Public-domain/MIT-0 single-file C. QAudioDecoder rejected: runtime codec-backend dependency, async API, Qt 6.2 fragility across three OSes. |

Value types (header-only, `src/audio/sampledata.h`):

```
ImportedSample  { float32 buffer (mono, native rate); double sampleRate;
                  int baseKey; double fracSemitone; optional loopStart/End;
                  optional exactPitch (agbp); QString suggestedName, sourcePath;
                  enum sourceKind }
SampleEditParams{ cropStart/End; loopStart/End/loopOn (source coords);
                  baseKey; fineTuneCents; double targetRate;
                  normalizeMode (auto/looped/oneshot/off); fadeIn/fadeOut;
                  crossfadeOn; ditherOn; exactPitchOverride }   // equality-comparable
ProcessedSample { QByteArray s8; uint32 freq, loopStart, size; bool looped;
                  SeamMetrics seam }
```

## 3. Integration contracts

- **Entry points**: Tools-menu **"Import Sample…"** (phase 1, standalone);
  VoicegroupBrowser **"New sample…"** affordance beside the DirectSound
  sample-symbol combo emitting `newSampleRequested(int slot)` (phase 3) —
  the browser stays a pure view; MainWindow opens the dialog.
- **Commit sequence** (MainWindow): `registerSample()` →
  `invalidateVgCatalog()` → (browser-initiated only) build
  `voice_directsound 60, 0, DirectSoundWaveData_<name>, typicalAdsr` and
  apply via the existing `VoiceEditCommand` on the session undo stack —
  pointing the voice is undoable even though file creation is not.
- **No voicegroup dirty-state interaction**: `direct_sound_data.inc` is not a
  voicegroup file; no tab goes dirty, `vgFileTime` logic untouched.
- **Dialog-local undo**: param edits (handle drags, spinboxes) ride a small
  QUndoStack inside the dialog; nothing project-visible exists until commit.
- **Sidecar** (phase 6): `.porydaw/samples/<name>.json` — source absolute
  path + content hash + full `SampleEditParams` + version. Enables "Edit
  sample…" reopen from the hi-res source; the project is canonical without
  it (missing sidecar = re-import the committed 8-bit .wav, still
  crop/loop-editable).

## 4. Audition-slot protocol (highest-risk piece — implement exactly)

Goal: audition an unregistered, in-memory sample through the real engine,
with live re-render during loop-handle drags, without stopping the device.

`AudioEngine` gains a fixed pool of **N = 4 audition slots**, each
`{ ToneData tone; WaveData wave; std::vector<int8_t> bytes; }`, plus:

- `uint64 m_auditionPublish` (atomic, UI-written): packs
  `generation << 16 | slot << 8 | midiKey | flags(noteOn/off)`;
- `uint64 m_auditionAck` (atomic, audio-thread-written): the generation the
  audio thread has adopted, and the slot it most recently **retired**.

Rules:

1. UI thread renders into a slot only if it is *retired* (never used, or its
   generation is acknowledged-superseded via `m_auditionAck`). With N = 4
   and one sounding audition, a free slot always exists; if none is retired
   (pathological), drop the re-render (coalesce — the next drag tick
   retries).
2. UI fills bytes → fills `wave` (freq/loopStart/size/status from
   `ProcessedSample`, `data` → bytes) → fills `tone`
   (`type = DirectSound`, ADSR = fixed sustain A255/S255/R<user> or the
   destination voice's ADSR) → publishes the packed command.
3. Audio thread, at callback boundary: sees a new generation → note-off any
   currently-sounding audition, start the new note against the slot's
   `tone`, write ack (new generation + previously-sounding slot now
   retired). The engine's own `CHN_LOOP` path does the looping.
4. Note-off: publish with the off flag; audio thread releases; the slot
   retires only after release *completes* (envelope done) — track via the
   engine channel status the same way `previewVoice` cleanup does.
5. Shutdown/dialog close: publish all-off, spin-wait (bounded, ~100 ms) for
   ack, then slots may be freed with the engine.

Same idiom as `m_previewCmd`/`m_previewVoiceCmd` — read those first.
**Exit ramp** (acceptable fallback if the handshake fights back): cold-swap
with a brief device pause; the dialog is modal and song playback is
typically stopped. Do not ship a hand-rolled sample-player simulator —
engine fidelity is the point (DSP.md §8).

## 5. Sample Studio dialog

Modal resizable QDialog (transactional flow ending in a commit — dock is
wrong: docks are persistent project-state views; wizard is wrong: one screen
+ commit). Flow: file dialog → (if .sf2) Sf2ZonePicker → editor. Internally
separate widgets so the harness can drive them offscreen.

Layout (revised 2026-07-21 for the beginner-first pass): WaveformView
dominant on top. Below: a checkable "Loop this sample" group whose body
hides entirely for one-shots — first enable on a loop-less sample seeds the
analyzer's best candidate plus a crossfade bake iff its seam isn't clean,
as one undo entry; inside it the green/amber/red seam badge (DSP.md §6),
"Try another loop" (cycles the top-5 candidates), "Refine", loop-range
spins, and "Smooth seam (crossfade)" — the badge's remedy IS the
crossfade checkbox, no separate Fix affordance. Then base key (pitch-detect prefill, note-name spin);
target-rate combo (presets from `kGbaMixRates`, "keep source rate", free
entry — doubles allowed; fresh sources DEFAULT to min(source, 13379) —
`kGbaDefaultRate` — while prepared/gbaReady files keep their source rate
for byte-faithfulness); audition strip (single play/stop toggle — looped
iff the loop is on — audition key selector, "use destination voice ADSR"
when browser-initiated); a one-line friendly summary (duration · ROM cost
`16 + align4(n)` · seam verdict); a collapsed "Advanced" disclosure
(format line, crop spins, fine-tune cents, normalize mode + resulting
gain readout, technical output readout); commit strip (name field with
live validation, "Add to project"). Everything below the waveform except
the dialog buttons rides a frameless squeeze-then-scroll QScrollArea so
short windows scroll rather than squish the frames. No snap-to-zero
toggle (markers land where dragged) and no seam-solo audition — seam
quality is the suggest/refine/crossfade machinery's job.

Never add program-remap UI. Cries/DPCM are out of scope (never emit DPCM).

## 6. Phases

### Phase 1 — Registrar + prepared-file import
Immediate end-to-end value: a user with an already-GBA-ready .wav gets it
into the project without touching an editor.
- `SampleRegistrar` (probe wav2agb pipeline, validate name/symbol/file
  collisions, verify build rule, verbatim-copy the .wav, append registration
  block; legacy-aif and missing-`.inc` → actionable refusals).
- Tools-menu "Import Sample…": file pick, sanitized-name field, read-only
  display of detected smpl/agbp info (light header validation), commit.
- `invalidateVgCatalog()` wiring; new symbol immediately selectable in the
  voicegroup browser and audible via existing `previewVoice`.

*--samplecheck §1*: build a fake wav2agb-flavor decomp project in a fresh
scratch dir (vgcheck pattern: `direct_sound_data.inc`, samples dir,
`audio_rules.mk` stub with the pattern rule, a voicegroup); import a
harness-generated fixture .wav; assert `.inc` bytes (block grammar, CRLF
preservation, single append); `directSoundSymbols()` sees the symbol;
`voicegroup_load` resolves a voicegroup referencing it with expected
`WaveData` freq/loopStart/size/status; collision, aif-only-project, and
missing-rule refusal messages exact-match. *Manual*: import into
`/home/huderlem/pokeemerald`, `make`, hear it in-game; confirm porydaw
playback picks it up pre-build (loader resolves the .wav directly).

### Phase 2 — Headless pipeline + writer (parity)
- Vendor dr_wav. `SampleImport` (wav + aif hi-res), `sampledata.h`,
  `SampleDsp` core (resampler, quantizer, zero-cross, loop mapping;
  `normalizeGain` per DSP.md §5), `SampleDocument`, `SampleWavWriter`.
- Phase-1 dialog upgraded: any wav/aif runs through the pipeline with
  numeric (non-visual) controls for crop/loop/key/rate/normalize.

*--samplecheck §2*: decode fixtures (8/16/24-bit, float, stereo); pipeline
determinism (two renders → identical bytes); **parity matrix** — for a grid
of params, write the .wav into the fixture project and assert
`load_wav_from_path`'s decoded bytes/freq/loopStart/size equal the in-memory
`ProcessedSample` exactly; retune-formula vectors (FORMATS.md §3); DSP.md §9
items 1-6, 10; corpus-conditional: no-op u8 round-trip + reference-`.bin`
byte-equality across sc88pro files.

### Phase 3 — Editor UI + engine audition + loop suggest
- `WaveformView`, `SampleEditorDialog`, audition-slot protocol (§4),
  `suggestLoop`/`refineLoop`/`detectPitchYin` wired to UI,
  `newSampleRequested` browser entry + auto-assign via `VoiceEditCommand`.

*--samplecheck §3*: offscreen widget-driving (rollcheck pattern): simulate
handle drags/spinbox edits → assert params → re-render → seam metrics;
loop-suggest on synthetic signals (DSP.md §9 items 7, 9); audition-slot
unit section — generation retirement invariants, no use-after-reuse, rapid
publish storm doesn't starve; commit into fixture project re-runs §1
assertions; undo count for the dialog-local stack. *Manual checklist*:
looped audition while dragging loop handles (no glitches, no device stop);
retune scrub across keys; audition during song playback doesn't disturb it.
(The "loop seam solo" audition shipped here was removed in the 2026-07-21
beginner-UX pass — it served no real purpose.)

### Phase 4 — Compressed formats
- Vendor dr_mp3/dr_flac/stb_vorbis behind `SampleImport`; extend file
  filters. Nothing downstream changes.

*--samplecheck §4*: tiny harness-generated or in-repo fixtures per format —
decoded length/hash asserts; unsupported-file refusal text.

### Phase 5 — SoundFont
- `Sf2Reader` + `Sf2ZonePicker`; zone loop/originalPitch/pitchCorrection
  pre-fill the editor (exclusive→inclusive loop-end conversion at the
  boundary; FORMATS.md §5).

*--samplecheck §5*: synthesize a minimal .sf2 byte-stream **in the harness**
(RIFF writing is ~100 lines; no binary fixture) — assert zone metadata lands
in `ImportedSample` (incl. stereo-linked flagging, ROM-sample skip); picker
driven offscreen.

### Phase 6 — Provenance + polish
- Sidecar write on commit + "Edit sample…" reopen (hash-mismatch → offer
  re-import); engine-integration loop test (DSP.md §9 item 8) if not landed
  earlier; resolve open questions; optional stretch: legacy `.aif` writer
  (FORMATS.md appendix A) behind the registrar interface.

*--samplecheck §6*: sidecar round-trip — reopen from sidecar, re-render,
hash equals committed bytes.

### Cross-phase docs duty
Whichever phase lands last updates SPEC.md (§6.2 editing behaviors gains a
Sample Studio subsection; §8 marks the roadmap item shipped) and prunes the
BACKLOG entry.

## 7. Open questions (recorded, non-blocking)

- Breadth of `agbp`/`agbl` support across wav2agb versions in the wild —
  verified present in the reference checkout's tool; older ipatix releases
  may lack them. Harmless either way (unknown chunks skipped; smpl fallback
  is integer-key accurate). Revisit only if a user reports a mismatch.
- 16-bit .wav output toggle for users who post-process externally (BACKLOG).
- Offering to install wav2agb into legacy .aif projects (BACKLOG; out of
  scope here).

## 8. Risk register

| Risk | Mitigation |
|---|---|
| Audition-slot concurrency bugs | Protocol pinned in §4; unit section in §3 harness; documented cold-swap exit ramp |
| `.inc`/build-rule variance across forks | Registrar mimics existing entries + verifies the pattern rule; refusals are actionable, never silent |
| Loop clicks at 8-bit/13 kHz on hard material | Post-quantize metrics + crossfade bake (DSP.md §6); "no clean loop" flagging |
| dwMIDIPitchFraction quirk (FORMATS.md §3.1) | Always-emit `agbp`; standard-semantics smpl kept for third-party tools |
| Vendored-decoder licensing | dr_libs/stb are public-domain/MIT-0; record in a LICENSES note under `external/` |
