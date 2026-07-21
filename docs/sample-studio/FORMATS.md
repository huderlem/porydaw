# Sample Studio — Format & Pitch-Math Reference

Byte-level ground truth for everything Sample Studio reads and writes.
Verified 2026-07-19 against `tools/wav2agb` in `/home/huderlem/pokeemerald`
(ipatix wav2agb v1.1, extended) and pory4a's
`external/poryaaaa/plugin/voicegroup_loader.c`. Cite-by-name; line numbers
drift.

## 1. The emitted `.wav` (what porydaw writes)

8-bit **unsigned** PCM mono RIFF/WAVE — the same shape as every shipped
pokeemerald sample. Chunk order: `fmt `, `data`, `smpl`, `agbp`, `agbl`.

| Chunk | Contents |
|---|---|
| `fmt ` | tag=1 (PCM), channels=1, sampleRate=`round(targetRate)` (declared; the *exact* rate lives in `agbp`), blockAlign=1, bits=8 |
| `data` | n bytes, each `s8 + 128` (unsigned offset encoding of the final quantized signed-8 samples) |
| `smpl` | see §1.1 — base key, pitch fraction, one forward loop record (omitted fields zero) |
| `agbp` | u32 LE: exact GBA pitch word = `round(effectiveRate · 1024)` (see §3) |
| `agbl` | u32 LE: exact loop end / sample count = `n` for fresh exports — see §1.2 for the off-by-one trap and why the shipped corpus differs |

RIFF rule: chunks with odd payload length get a pad byte (wav2agb
`wav_file.cpp` honors this when scanning; our 4-byte custom chunks never need
it, but `data` can be odd — pad it).

Rationale for 8-bit output: wav2agb's u8→s8 conversion is a lossless
`x − 128`, so **the bytes porydaw auditioned are bit-identical to the bytes in
the ROM**. Hi-res masters are preserved via the provenance sidecar
(PLAN.md §6), not the committed file. (A 16-bit output toggle is in BACKLOG.)

### 1.1 `smpl` chunk layout (fields wav2agb reads)

All u32 LE, offsets relative to chunk payload start:

| Offset | Field | wav2agb reading (`wav_file.cpp`, `smpl` branch ≈:165-182) |
|---|---|---|
| 12 | dwMIDIUnityNote | `midiKey = min(value, 127)` |
| 16 | dwMIDIPitchFraction | `tuning = value / (2^32 · 100.0)` — **see §3.1 quirk** |
| 28 | cNumSampleLoops | must be 0 or 1 (**>1 is a hard error**) |
| 36+4 | loop type | must be 0 (forward; anything else is a hard error) |
| 36+8 | loop start | sample index, inclusive |
| 36+12 | loop end | sample index, **inclusive** (wav2agb adds +1 on read) |

Manufacturer/product/period/SMPTE fields (offsets 0-11, 20-27) and
sampler-data are written as zero. porydaw writes exactly one loop record when
the sample loops and `cNumSampleLoops = 0` otherwise.

### 1.2 `agbp` / `agbl` custom chunks

From wav2agb `wav_file.cpp` (≈:183-196), comments verbatim from source:

- `agbp` — "exact GBA pitch value (sample_rate * 1024). This allows perfect
  round-trip conversion without period-based precision loss." u32 LE.
- `agbl` — "exact loop end override (handles off-by-one from original game)."
  u32 LE. In the converter and in pory4a's loader
  (`voicegroup_loader.c` ≈:1139-1142) a nonzero `agbl` **overrides the final
  `size`** — the count of samples actually played (loop plays
  `loopStart..size`, wrap-interpolating `s[size−1] → s[loopStart]`).
  - **porydaw fresh exports: `agbl = n`** (the exported sample count) for
    looped and one-shot alike, so every exported sample plays. This agrees
    with the `smpl` loop end `n − 1` inclusive (+1 on read = n).
  - **The shipped corpus carries `agbl = n − 1`** (measured across sc88pro
    files, looped and one-shot: data has n samples, `smpl` end = n−1,
    `agbl` = n−1). That is a ROM-round-trip artifact: the original game's
    `.bin` had `size` = n−1 (aif2pcm's `size = loopEnd − 1` off-by-one), the
    rip kept one extra trailing sample in the `.wav`, and `agbl` pins `size`
    back to the original so rebuilt ROMs are byte-identical. Do **not**
    imitate this for new samples — it would silently drop the final sample.
- wav2agb itself post-annotates source .wavs with an `agbl` chunk when
  missing (`write_wav_with_agbl_chunk`, `wav_file.cpp` ≈:310-340) — do not be
  surprised when committed files gain the chunk after a build.
- Unknown-chunk safety: RIFF parsers skip unrecognized chunks, so `agbp`/
  `agbl` never invalidate the file for third-party tools; a wav2agb build
  without agbp support would fall back to `smpl`-derived pitch (integer-key
  accurate; fractional tuning lost — see §3.1).

**Always emit both chunks.** The loader treats `agbp` as authoritative
(pitch word verbatim), which sidesteps both float rounding and the §3.1
fraction quirk.

## 2. The `.bin` build artifact / in-memory `WaveData`

wav2agb `-b` output (`converter.cpp`, binary branch ≈:404-420) — 16-byte
header + signed-8 data, identical layout to the engine struct
`WaveData` (`m4a_engine.h:70-77`) and GBA `struct WaveData`:

| Bytes | Field | Notes |
|---|---|---|
| 0-3 | flags (u32 LE) | bit 0: 0 = PCM8, 1 = DPCM (never emit DPCM); bit 30 (`0x40000000`): loop enabled. Maps onto `type`/`status` u16 pair; loader sets `status = 0x4000` when looped |
| 4-7 | pitch (u32 LE) | `effectiveRate · 1024` (Q10 fixed point) — §3 |
| 8-11 | loopStart (u32 LE) | sample index |
| 12-15 | loopEnd / size (u32 LE) | sample count; loop plays `loopStart..size` forever (`m4a_channel.c`: `loopLen = size − loopStart`) |
| 16- | samples | s8 = `clamp(floor(x · 128), −128, 127)`; wav2agb pads the *file* to 4-byte alignment after data (padding is past `size`, never played). porydaw does not pad the .wav; UI shows ROM cost as `16 + align4(n)` bytes |

## 3. Pitch math (retune without DSP)

wav2agb `converter.cpp` (≈:385-396):

```cpp
if (midiKey == 60 && tuning == 0.0)
    pitch = sampleRate;
else
    pitch = sampleRate * pow(2.0, (60.0 - midiKey) / 12.0 + tuning / 1200.0);
pitch_value = (agbPitch != 0) ? agbPitch : uint32_t(pitch * 1024.0);
```

`pitch_value` is "the sample-playback rate at middle C, ×1024". At playback,
`MidiKeyToFreq` scales it through geometric 12-TET tables so key 60 plays the
sample at `pitch_value/1024` samples/sec and each semitone multiplies by
2^(1/12). **Therefore: to retune, set the unity note — never resample.**

porydaw computes, for detected/entered exact key `m_exact`
(`= 69 + 12·log2(f0/440)`):

```
unity        = floor(m_exact)                     → smpl dwMIDIUnityNote
fracSemitone = m_exact − unity        ∈ [0, 1)
effectiveRate = targetRate · 2^((60 − (unity + fracSemitone)) / 12)
agbp          = round(effectiveRate · 1024)
```

(`targetRate` is a double — fractional rates like 3344.75 Hz are normal and
carried exactly by `agbp`.) This matches pory4a's loader math
(`voicegroup_loader.c` ≈:1147-1153) bit-for-bit when `agbp` is present,
because both sides then use the same integer verbatim.

### 3.1 The dwMIDIPitchFraction quirk (why `agbp` is mandatory)

Standard `smpl` semantics: dwMIDIPitchFraction is the fraction of a semitone
as an unsigned 32-bit fixed-point value (0x80000000 = 50 cents).

wav2agb (`wav_file.cpp` ≈:170) instead computes
`tuning = frac / (2^32 · 100.0)` and later uses `tuning / 1200.0` in the
exponent — treating `tuning` as cents but scaling the raw field down by an
extra ×100. Net effect: the honored magnitude is **1/10,000 of the standard
semantics**. Expressing even one real cent through this path would need
`frac = 100·2^32`, which overflows u32. pory4a's loader
(`voicegroup_loader.c` ≈:1150) reproduces the same math intentionally
(A/B parity with the build tool).

Consequences:

- **Fractional tuning must travel via `agbp`.** The `smpl` fraction is
  effectively decorative to wav2agb.
- porydaw still writes dwMIDIPitchFraction with **standard** semantics
  (`round(fracSemitone · 2^32)`) so third-party tools read the file
  correctly; wav2agb sees ≈0 from it, and `agbp` overrides anyway.
- Worst-case if some wav2agb build lacked `agbp` support: pitch would be
  integer-unity-note accurate, off by `fracSemitone` (< 100 cents). Files
  remain valid; nothing breaks.

## 4. Registration in the project

Block appended to `sound/direct_sound_data.inc` (grammar per shipped file):

```asm
	.align 2
DirectSoundWaveData_<name>::
	.incbin "sound/direct_sound_samples/<name>.bin"
```

- `<name>`: lowercase `[a-z0-9_]+`, must not collide with existing symbols
  (`VoicegroupSource::directSoundSymbols()`) or existing files
  (`<name>.wav`/`.bin` in the samples dir).
- The `.incbin` targets the **`.bin`** (built artifact); the build's pattern
  rule (`audio_rules.mk`: `$(SOUND_BIN_DIR)/%.bin: sound/%.wav` →
  `$(WAV2AGB) -b $< $@`) produces it from our committed `.wav`. The registrar
  must verify a wav2agb `%.bin: %.wav`-shaped rule exists in the project
  (`audio_rules.mk`, `Makefile`, or included `.mk`s) and refuse actionably
  otherwise.
- pory4a needs no registration beyond this: it parses `Label::`/`.incbin`
  pairs and resolves the symbol to our source `.wav` first
  (resolution order `.wav → .aif → .bin`), so porydaw playback picks the
  sample up immediately — even before the project is built.
- Usable voice line (generated on commit-from-browser):
  `voice_directsound 60, 0, DirectSoundWaveData_<name>, <A>, <D>, <S>, <R>`
  with `typicalAdsr()` defaults. Keep base_midi_key = 60 (project
  convention; sample tuning is in the sample — the fork's voice-key
  transposition patch is orthogonal, see CONTEXT.md §2.4).

## 5. SoundFont (.sf2) subset consumed

RIFF form `sfbk`; Sample Studio reads only:

- `sdta` → `smpl` sub-chunk: 16-bit LE PCM sample pool. (`sm24` extension
  chunk: ignored.)
- `pdta` → `shdr` records (46 bytes each): name[20], start, end, loopStart,
  loopEnd (pool indices; sf2 loop end is **exclusive**), sampleRate,
  originalPitch (MIDI key), pitchCorrection (signed cents), sampleLink,
  sampleType.
- `pdta` → `phdr`/`inst`/`ibag`/`igen` (plus the `pbag`/`pgen` index arrays,
  structurally required to link presets to instruments) only as far as
  grouping sample names under instrument/preset labels for the picker UI.
  Generator/modulator semantics, velocity layers, and per-zone tuning
  overrides are deliberately ignored.
- sampleType: mono (1) imported directly; left/right-linked (4/2) import this
  channel and flag "stereo pair — imported one channel" in the UI; ROM
  samples (0x8000 bit) skipped.

Zone → `ImportedSample` mapping: loop points copy over (convert exclusive →
inclusive convention at the boundary), `unity = originalPitch`,
`fracSemitone` from `pitchCorrection/100` (renormalize so frac ∈ [0,1) by
borrowing from unity).

## Appendix A — Legacy `.aif`/aif2pcm forks (fallback, not mainline)

Pre-wav2agb forks (e.g. `pokeemerald-pokezelda`) build `.aif` with
`tools/aif2pcm`, which derives pitch **only** from the AIFF COMM declared
sample rate (`pitch_adjust = sampleRate · 1024`) — the INST base note is read
but unused.

Retuning there is still possible with zero DSP: write the COMM 80-bit
extended-float sample rate as `effectiveRate` (i.e.
`actualRate · 2^((60−key)/12 + frac/12)`), leaving the audio untouched; write
MARK/INST sustain-loop markers as pret `.aif`s do. The declared rate then
intentionally differs from the true capture rate (this matches how pre-tuned
pret samples already behave in spirit).

Phase 1 ships only detection + an actionable refusal for these projects
("this project predates wav2agb"). The `.aif` writer above is an optional
stretch behind the same registrar interface (see BACKLOG). If implemented,
parity-test against `load_aif_from_path()` exactly as the `.wav` path tests
against `load_wav_from_path()`.
