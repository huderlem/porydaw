# GBA CPU meter: estimated SoundMain cycles per frame

2026-10-29. In pokeemerald `m4aSoundMain` runs inside `VBlankIntr`
(src/main.c), after the GPU register and DMA3 copies. Every cycle it takes
is taken from the frame budget the game's main loop shares with it, and a
SoundMain that is still running when the next VBlank fires is skipped
(the `ident` guard), which drops a frame of audio. ROM hackers raise the
PCM polyphony and the mix rate without seeing this cost. Porydaw should
show it, live, as the song plays, and let the user watch it move as they
change Settings → Audio.

poryaaaa reproduces the game's engine instruction for instruction, so the
cost can be *modeled*, not measured on the host: the engine knows which
code path every channel takes in every frame; a cycle cost per path gives
the frame's total. Nothing is emulated and no ROM is needed.

## What already exists

- **The sequencer term is cycle-exact.** `M4A_CYC_*` in
  plugin/m4a_engine.h is a probe-measured cost of every MPlayMain segment
  (tick loop, chain walk, each `ply_*` handler, the VOLCHG/PITCHG pass, the
  six-player chain, the FIFO DMA stall, ROM vs IWRAM song bytes).
  m4a_sequencer.c sums it while it runs a compiled song and publishes the
  CgbSound entry cycle (`engine->cgbEntryCycles`); docs/ACCURACY.md,
  "SoundMain timing model".
- **CgbSound is modeled to its last write.** `M4A_CYC_CGB_*` and the
  per-channel cursor in `m4a_cgb_channel_tick` place every PSG register
  write on the clock (m4a_engine.c, the CgbSound block of the tick).
- **The measurement rig.** test/accuracy builds pokeemerald's real m4a
  engine into a test ROM (vanilla and ipatix HQ mixer, any sound-mode
  rate) and `test/accuracy/ref/mgba_probe.c` runs it under libmgba's
  debugger, logging the entry cycle of every driver function per frame,
  including `SoundMainRAM_Buffer` and all of the HQ mixer's `C_*` labels
  (`C_main_mixer_return`, `C_downsampler_return` are the HQ exit).
  `tools/probe_corpus.sh` regenerates the 67-log corpus. ~/pokeemerald and
  ~/mgba (libmgba-static built) are present on the dev machine.
  docs/HQ-MIXER-ACCURACY.md already quotes HQ mixer costs from it: 7683
  cycles with no channel, 39874 for littleroot.
- **porydaw plumbing.** `AudioEngine::polySnapshot` is polled by
  `MainWindow::uiTick` (10 Hz playing, 2 Hz stopped) into the status-bar
  polyphony meter (`m_polyMeter`, `updatePolyStatus`) and the Polyphony
  dock; `porydaw.audio.channels()` mirrors it for plugins. Settings →
  Audio owns `EngineSettings` {maxPcmChannels, pcmMixRate, hqMixer}.
- **porydaw drives the engine with MIDI events**, not a compiled song
  (`m4a_engine_note_on` … from `TimelinePlayer`); no `M4ASequencer` runs,
  so today `cgbEntryCycles` is the fixed one-track value in porydaw.

## What is missing

1. **The mixer term** (`SoundMainRAM` / the HQ mixer): the dominant,
   polyphony- and rate-dependent cost. Needs a cycle cost per translated
   block (the vanilla mixer in m4a_mixer.c and the HQ mixer in
   m4a_hq_mixer.c are label-by-label ports, so every block has a place to
   charge) and the probe's mixer *exit* for the vanilla ROM (the HQ exit is
   already a breakpoint).
2. **The CgbSound tail** (last channel → return → SoundMainRAM entry) and
   the SoundMain epilogue: small constants.
3. **A MIDI-path estimate of the sequencer term**: porydaw has no song
   bytes, so MPlayMain's cost must be charged from the events the engine
   receives and the track state it keeps, with the same `M4A_CYC_*`
   constants.
4. **Publication and UI**: a per-frame cost the host can read, a status-bar
   readout, a breakdown, a threshold, docs.

## Scale (rough, to be replaced by calibrated numbers)

Frame = 280896 cycles; VBlank = 83776. Vanilla interpolating loop ≈ 22
cycles per output sample per channel (instruction count; ROM waitstates on
sample fetches are what the calibration settles).

| Config | Mixer cycles/frame | Share of frame |
|---|---|---|
| 5 ch @ 13379 Hz (stock, 224 samples) | ~25k | ~9 % |
| 12 ch @ 13379 Hz | ~59k | ~21 % |
| 12 ch @ 21024 Hz (352 samples) | ~93k | ~33 % |
| 12 ch @ 42048 Hz (704 samples) | ~186k | ~66 % |

The HQ mixer costs more per sample and has a fixed downsampler pass
(the 7683 floor).

## Decisions (all confirmed by the user 2026-10-29)

- **D1 What 100 % means (CONFIRMED 2026-10-29: the VBlank window).**
  100 % = the VBlank window, 68 lines × 1232 = **83776 cycles**. The
  hardware does not force SoundMain to end inside VBlank (an interrupt
  handler runs on into the draw period), but everything the game counts on
  doing "during VBlank" shares those cycles, so it is the honest,
  conservative budget and the one hackers reason with. The whole frame
  (280896 cycles = 335 %) is the hard ceiling: past it the next SoundMain
  is skipped (`ident` guard) and audio drops a frame. Readout: percent of
  VBlank; amber above a user-settable **CPU budget** in Settings → Audio
  (default 100 %), red at the frame ceiling. The tooltip shows the raw
  cycles and both lines. A stock 5-channel 13379 Hz song reads ≈ 30 %.
  *2026-10-29 amendment: the budget is in cycles (QSettings
  `engine/cpuBudgetCycles`, 1000..280896), default 60000 (≈ 72 %); amber
  when the frame's cycles exceed it; a stored `engine/cpuBudgetPercent`
  converts once on load.*
  *2026-10-29: red = past VBlank (total > 83776 cycles, whatever the
  budget); the whole frame (280896) is no longer a colour threshold, shown
  for scale only (the dock's legend, the tooltip). Three bands: within the
  budget, over it (amber, up to the VBlank window), past VBlank (red). The
  status bar, the dock and the profile strip all follow `CpuPanel::level`.*
- **D2 Where it shows (CONFIRMED: its own dock).** A new **CPU dock**,
  a sibling of the Polyphony dock (same View-menu placement, same
  show/hide persistence, SPEC §6.1 dock list): current and peak percent,
  the raw cycles, three breakdown bars (sequencer / PSG / mixer), the
  budget and frame-ceiling lines, its own Reset. Plus a compact `CPU n %`
  item in the status-bar meter beside the PCM/PSG counts with the same
  tooltip, so it is visible without the dock. Nothing CPU-related goes into
  the Polyphony dock.
  *2026-10-29: red = past VBlank; frame ceiling shown for scale only; dock
  redesigned: hero, linear 0–140 % meter, 10 s history, breakdown table,
  peak card, legend. One column: a status pill; the frame's percent large
  with its cycles; a linear meter 0..140 % of the VBlank window
  (the two-zone bar is gone) with the budget, VBlank and peak lines; the
  last 10 s of playback frame by frame (poryaaaa's new per-frame
  `costHistory` ring drained by the audio callback into AudioEngine's
  `cpuHistory`; kept through a stop, cleared with the peak); the three terms
  with cycles and share of the frame; a peak card with the song position of
  the peak (`CpuSnapshot::peakAtSeconds`) and Reset peak; a legend of the
  budget, VBlank and whole-frame lines.*
- **D3 Offline per-song profile (CONFIRMED).** A cycles-per-frame curve
  along the timeline (heat strip under the ruler), computed by a background
  render of the song through the same engine, so the user sees *which
  bars* are hot without playing. Last step; the model is shared.
- **D4 Headroom for in-game sound effects and cries (CONFIRMED: not
  mentioned).** No option, no tooltip text. The manual may carry one line
  if the user writes it.
- **D5 Which engine (CONFIRMED).** The song engine only. The live engine
  (`m_liveEngine`, live mode mixes piecewise) and the preview engine
  publish nothing; live mode marks the cost invalid.
- **D6 Scripting (CONFIRMED).** `porydaw.audio.cpu()` → `{cycles, percent,
  peak, peakPercent, sequencer, cgb, mixer, estimated, playing,
  budgetPercent, vblankCycles, frameCycles}` or null, beside `channels()`.
  Step 6. (The shape changed during implementation: `psg` became `cgb`,
  `budget` `budgetPercent`, and `peakPercent`, `estimated`, `playing` were
  added; step 7 added `cpuProfile()`.) *2026-10-29: `budgetCycles` added
  to both; `budgetPercent` now derived from it (rounded).*
- **D7 Reference (CONFIRMED).** mGBA 0.10.5 is the reference, as for every
  other constant in the model (not hardware). mGBA's ROM prefetch model is
  not hardware-perfect; hardware may differ by a few percent. Say so in
  the manual.
- **D8 Peak semantics (CONFIRMED).** Peak since the song last started
  playing, reset by play-from-stop and by the dock's Reset.

## Mechanism

### poryaaaa

**Publication.** `M4AFrameCost { int32_t sequencer, cgb, mixer, total;
bool valid; }` on `M4AEngine`: `frameCost` (the last completed SoundMain)
and `frameCostMax` (the maximum since `m4a_engine_reset_frame_cost_max`),
written at the end of the tick after SoundMainRAM, where `frameEndHook`
runs. `total` is cycles from `m4aSoundMain` entry to its return
(`M4A_CYC_SOUNDMAIN_ENTRY` and the epilogue included). Constants
`M4A_VBLANK_CYCLES 83776` and `M4A_FRAME_CYCLES 280896` sit beside it so a
host shows percent of VBlank (D1) and the frame ceiling. `valid` is false in live mode and until the first full frame.

**Sequencer term.** With an `M4ASequencer`, exactly today's
`frameCycles`. Without one (porydaw, the plugin), a *MIDI-path estimate*:
the engine's tick charges the frame skeleton (`MP_BGM_ENTRY`, `MP_HEAD`,
`MP_EXIT`, the per-track `TICK_*` / `APPLY_*` passes from the tracks it
knows exist, the wait/LFO path from each track's `lfoSpeed`/`mod`/delay
state) and, per event applied this tick, the command cost of what mid2agb
would have emitted: note → `CMD_FETCH + CMD_BYTE(+LOW/RS) + CMD_NOTE` +
ply_note's search/init costs already in the header; CC/program/bend →
`CMD_TABLE` + the handler's constant; the tick's `W##`. Running status is
estimated per track from the previous command. The chain walk is charged
from the engine's real channel list. This is a documented approximation:
step 4 measures it against the exact sequencer on the corpus (both inputs
exist for every song) and records the error distribution in
docs/ACCURACY.md.

**CgbSound term.** `cursor - cgbEntry` after the last channel plus
`M4A_CYC_CGB_TAIL` (last loop head → return) and `M4A_CYC_SM_TO_RAM`
(SoundMain's `SoundMain_4`/`_5` block → `SoundMainRAM_Buffer`), measured
in step 1. The polyphony shadow pool is not charged (debug only).

**Mixer term, vanilla.** Constants `M4A_MIX_*` charged at the translated
labels of m4a_mixer.c: entry/no-reverb check, the reverb pass per sample
(`_081DCEC4`), per channel the envelope/volume block (`_081DCF6A..`), the
START init, the channel-end store (`_081DCFB0`), then per path: the
fixed-frequency word loop (`_081DD0B0`, 4 samples a word) and its tail
loop (`_081DD0F4`), the interpolating loop (`_081DD1BC`) per output sample
plus per source-sample advance (the `ldrsb` fetches, ROM waitstates), loop
wrap (`_081DD134`/`_081DD164`), `finish_word`, Unk1's CMP forward/reverse
and uncompressed-reverse loops with Unk2's block decode per 64-sample
block, `mix_synth`, the per-word ring load/store, the end of the channel
loop and the epilogue. The FIFO DMA stall (18 cycles per 20064) is charged
across the mixer span with `m4a_gba_fifo_dma_stall`, as the sequencer
does. Live mode's piecewise mixing charges nothing (D5).

Constants come from the ARM instruction timings (IWRAM code, 1-cycle
sequential, multiplies 2–4, `ldrsb` from ROM with WS0 3/1 and prefetch;
the ring and channel structs in IWRAM with `gSoundInfo`, 1-cycle)
and are then **validated against the probe**: per frame, `exit − entry` of
the mixer on every corpus log, vanilla at 13379 and 21024 Hz. Where the
hand count misses (the ROM fetch pattern is the likely place), the constant
is fitted by least squares over the corpus and the fit is recorded. The
per-path micro-tests isolate each constant: `pcm`/`pcm_hi` (interpolating
at step < 1 and > 1), `pcm_fix_loop`/`pcm_drum` (FIX), `pcm_cmp`/cries
(CMP forward), `pcm_rev` and the reversed cries (REV, CMP|REV),
`pcm_reverb`/`pcm_reverb_max` (reverb pass), `pcm_short_loop` (wrap),
`pcm_chord` (4 channels), `synth_*` (HQ only).

**Mixer term, HQ.** The same at the `C_*` labels of m4a_hq_mixer.c
(`C_mixing_setup`, `C_channel_state_loop` per channel, the three fast-loop
blocks by step, `fix_path`, `precache_path` with its block decode and
stack check, `synth_path`, `C_end_mixing`/`C_mixing_end_store`,
`C_downsampler` per output sample with noise shaping). The HQ probe
already logs the exit; the corpus is `build/probe-hq/`.

**Acceptance.** `poryaaaa_render --cost-log <file>` writes one line per
frame (`frame seq cgb mixer total`); a new `tools/cost_diff.py` diffs it
against the probe's `.frames` (which gains a mixer-exit column) frame by
frame. Gate: with `--song` input (exact sequencer) every corpus frame's
`total` within **2 %** or 300 cycles of the probe, whichever is larger,
vanilla and HQ, 13379 and 21024 Hz; the error distribution of the
MIDI-path estimate is reported, not gated. Unit tests in test/test_engine.c:
no channel = the fixed cost; N identical channels scale linearly; each
path is charged its constant for a synthetic one-channel frame; live mode
publishes invalid; `frameCostMax` holds and resets.

### porydaw

- `AudioEngine`: after each `m4a_engine_process` the callback reads
  `engine->frameCost` / `frameCostMax` into atomics (`std::atomic<int32_t>`
  per field, published total-last so a torn read shows a consistent older
  frame) — the UI thread must not read the engine struct while the
  callback runs (polySnapshot does, but those fields are bytes). `cpuSnapshot()`
  returns {last, peak, breakdown, valid, budget cycles}. Peak reset on
  play-from-stop (D8) and on request.
- Status bar (D2): a `CPU n %` label in `m_polyMeter`, same 10 Hz /
  write-on-change path as `updatePolyStatus`; amber/red per D1 via theme
  roles (new `cpu_meter_warn_text` / `cpu_meter_over_text`); tooltip with
  the raw cycles, the breakdown, the budget and the frame ceiling. Hidden
  with the meter when no song is loaded.
- Settings → Audio: `CPU budget` spin box (percent of VBlank, default 100,
  D1), QSettings `engine/cpuBudgetPercent`, Restore Defaults covers it,
  `settingscheck` extended.
- CPU dock (D2): new `src/ui/cpupanel.{h,cpp}`, registered like the
  Polyphony dock (View menu, saved layout, `--cpucheck` drives it):
  current and peak percent in large type, raw cycles, three breakdown bars
  with the budget and frame-ceiling marks, a Reset button. Narrow/wide
  layouts as the Polyphony dock does.
- Scripting (D6): `porydaw.audio.cpu()`; `audio.setEngine` unchanged.
- Harness: `--cpucheck` in-binary check (verify skill): load a fixture,
  play, assert the readout appears and is nonzero, the peak is monotone
  while playing and resets on play-from-stop, the threshold coloring
  switches when the budget is set below the reading, the tooltip names the
  three terms, the CPU dock lays out in both widths and its Reset clears
  the peak.
- Offline profile (D3, step 7): a background `TimelinePlayer::render`-style
  pass over the timeline with a `frameEndHook` collecting `total` per
  frame into a per-tick array; debounced on `song.changed`; painted as a
  heat strip under the ruler (TimelineSurface cache rules apply;
  `invalidateContent()` on new data); hover shows the frame's percent.
  Cancelled and restarted on edits; skipped for songs over N minutes until
  measured.
- Docs: CHANGELOG (simple), docsrc/manual/cpu-meter.md as a TODO outline
  for the user's prose (project convention), SPEC §6.1 (status bar, dock),
  §7 (knobs), mkdocs nav, `mkdocs build --strict`.

## Steps

Each step: implement → Opus review → fix → commit → sweep (normal + ASAN);
poryaaaa steps also run the accuracy harness and `claim_check.sh` (the
vanilla and HQ renders must not move: the cost model only *counts*).
Windows checks at steps 5 and 6. poryaaaa changes land in the pory4a
submodule first (push ordering per conventions), then the bump in porydaw.

0. **Plan** (this file) on branch `cpu-meter`.
1. **poryaaaa: measurement.** mgba_probe gains the vanilla mixer exit
   (breakpoint on the return address after the test ROM's `bl m4aSoundMain`,
   or `SoundMainRAM`'s final `pop`), a `mixer` cost column in `.frames`,
   and `M4A_CYC_CGB_TAIL` / `M4A_CYC_SM_TO_RAM` read off the logs.
   Regenerate `build/probe` and `build/probe-hq` at 13379, plus the 21024
   variants (`probe_corpus.sh`). `tools/cost_diff.py` skeleton. Numbers into
   docs/ACCURACY.md, new section "SoundMain cost model".
2. **poryaaaa: vanilla mixer model + publication.** `M4AFrameCost`,
   `M4A_MIX_*`, charging in m4a_mixer.c, the CgbSound tail, `--cost-log`,
   unit tests, `cost_diff.py` PASS on the vanilla corpus at both rates.
3. **poryaaaa: HQ mixer model.** `M4A_HQMIX_*`, charging in
   m4a_hq_mixer.c, `cost_diff.py` PASS on the HQ corpus at both rates.
4. **poryaaaa: MIDI-path sequencer estimate.** The event-based charge in
   the engine's tick; measured against the exact sequencer over the corpus
   (`--song` vs `--midi --gba-tempo-grid`), distribution recorded. Bump
   the submodule in porydaw.
5. **porydaw: readout.** AudioEngine plumbing, status-bar CPU item,
   tooltip, Settings budget, `--cpucheck`. **Windows check.**
6. **porydaw: CPU dock, scripting, docs.** The CPU dock,
   `porydaw.audio.cpu()`, CHANGELOG, manual stub, SPEC. **Windows check.**
7. **porydaw: offline profile strip** (D3), its own review and Windows
   check.

## Risks and caveats (to state in the UI and manual)

- The number is SoundMain only. The game's VBlank handler and main loop
  share the frame; porydaw cannot know the game's own load (D1's budget is
  the user's judgement).
- Hacks with a modified mixer, a changed `WAITCNT`, samples moved to RAM,
  or an engine other than vanilla/ipatix-HQ drift from the model.
- mGBA, not hardware, is the reference (D7).
- The MIDI-path sequencer estimate is approximate (a few hundred cycles
  per frame at most; the mixer dominates). Step 4 quantifies it.
- Per-sample ROM fetch cost may depend on the sample's address alignment
  and the prefetch buffer; if the residual on `pcm_hi` vs `pcm` is not a
  constant, the fit gets a per-advance term rather than a per-sample one.
- Live mode and the preview engines are out of scope (D5).
