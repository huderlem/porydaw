# Sample Studio — DSP Specification

Signal-processing design for the sample pipeline. No DSP code exists in
porydaw or pory4a today; everything here is net-new, implemented from scratch
(no external DSP libraries), living in `src/audio/sampledsp.{h,cpp}` as pure
stateless functions. Formulas are normative; parameter values are the
defaults to ship.

Domain conventions: float32 mono working buffers on the canonical scale
`value = s8 / 128` (full scale = 1.0 ⇔ 128); **double** for accumulators and
kernel generation. All "LSB" figures are in the signed-8 output domain.

## 1. Pipeline order (normative)

```
decode → downmix to mono → float32 canonical
  [1] crop/trim (attack + tail)          — source-rate domain
  [2] DC removal                          — default ON for looped, OFF for one-shots
  [3] resample to target rate             — enters the FINAL sample grid
  [4] pitch detection                     — analysis only, no audio change
  [5] loop search / snap / refine         — on the final grid
  [6] normalize                           — pure gain, peak-capped
  [7] optional crossfade bake at the seam
  [8] micro fade-in / fade-out
  [9] quantize s8 (once)                  → export (.wav + smpl + agbp + agbl)
```

Why this order:

- **Crop before resample** — don't spend filter support on discarded audio;
  the filter's edge behavior lands on the real attack, not leading garbage.
- **Loop search after resample** (key decision) — the click a player hears is
  a discontinuity **on the final grid**. Searching on the source grid and
  mapping through a non-integer ratio (44100→13379 ≈ 3.296) shifts the seam
  by up to half an output sample and smears it through the anti-alias filter.
  Post-resample search makes seam metrics exact by construction.
- **Normalize after loop search** — the seam metric (NCC) is level-invariant,
  but computing gain from the final-rate loop region makes the reported
  loop-RMS the true exported one; gain commutes with crossfade.
- **Quantize last, once** — exactly one precision-losing step.

The editing model is non-destructive (PLAN.md §3): an immutable decoded
source + a parameter set; any parameter change re-renders the whole chain
(samples are seconds long — full re-render is trivial; no intermediate
caching).

**Marker bookkeeping**: positions live in the current stage's domain and
transform stage-by-stage — crop subtracts the crop offset; resample
multiplies by the ratio with `round()` (never load-bearing: the stage-5
refine pass re-seats loop markers); gain/fades/crossfade/quantize don't move
markers. Positions become authoritative integers only after stage 5.

## 2. Import conversion & quantization

- u8: `(x − 128)/128`; s16: `x/32768`; s24/s32 analogous; float: pass
  through, clamp to ±1 with a warning if exceeded.
- Stereo downmix: arithmetic channel mean. If full-file L/R correlation < 0
  (phase-cancelling), warn and offer "left channel only"; the mean stays the
  only automatic behavior.
- Quantizer (bit-match wav2agb): `s = clamp(floor(x · 128), −128, 127)`,
  computed in double. With the /128 canonical scale this makes u8 imports
  round-trip **bit-exactly** through a no-op pipeline (for any integer s,
  `floor((s/128)·128) = s`, negatives included — s/128 is exactly
  representable).
- **Dither: OFF by default.** All reference tooling (wav2agb, every shipped
  sample) truncates; matching it keeps porydaw byte-comparable to reference
  output. At 8 bits, TPDF raises the noise floor ~4.8 dB, and inside a loop
  the dither becomes a *periodic* noise pattern — audible pitched buzz every
  pass, worse than the truncation distortion it removes. The GBA's PWM DAC
  and the engine's `(s8·envVol)>>8` scaling add downstream quantization that
  per-sample dither doesn't survive. Advanced toggle: TPDF ±1 LSB with a
  **fixed PRNG seed per render** (exports must stay deterministic — harness
  requirement).

## 3. Resampler

**Direct-convolution windowed-sinc, Kaiser window, kernel evaluated per
output sample.** Rejected: polyphase (its only benefit is CPU efficiency at
the cost of phase-table granularity and ratio bookkeeping — speed is a
non-issue offline); linear/cubic interpolation (no anti-aliasing: at 3.3:1
decimation, 6.7–22 kHz content folds straight into the audible band); FFT
overlap-add (more code, block-edge artifacts, no benefit); external
libraries (scratch-build policy).

Let `r = targetRate / sourceRate` (double; `targetRate` is a double — e.g.
3344.75). For each output index `n`, input-time center `t = n / r`:

```
y[n] = ( Σ_{k = ceil(t−W) .. floor(t+W)}  x[k] · h(k − t) )  /  Σ h(k − t)
h(u) = 2·fc · sinc(2·fc·u) · kaiser(u / W, β)          sinc(v) = sin(πv)/(πv)
fc   = 0.5 · min(1, r) · ρ         ρ = 0.945  (rolloff)      [cycles/input-sample]
W    = L / (2·fc)                  L = 56     (sinc lobes per side)
β    = 8.96                        (Kaiser for A = 90 dB: β = 0.1102·(A − 8.7))
```

(L = 56, not the 24 originally drafted: with L = 24 the Kaiser transition
band — ~±750 Hz at this ratio — straddles the cutoff and drags the sweep top
of §9 item 1 down ~0.9 dB at 6.0 kHz. L = 56 puts the −0.1 dB point above
6.0 kHz and moves the stop edge inside the output Nyquist; measured
−0.0006 dB at 6.0 kHz, −119 dB at 8 kHz. Offline cost is negligible.)

- 44100→13379: `fc ≈ 0.1434`, `W ≈ 195` input samples per side. Upsampling:
  `min(1, r)` pins the cutoff at the source Nyquist.
- **Per-output tap-sum normalization** (the `/Σh` above) forces exact unity
  DC gain at every fractional position and cleanly handles edges.
- `kaiser` via the standard I₀ power series (double, ~25 terms, ~15 lines).
  Direct `sin()` per tap is fine offline.
- **Edges**: zeros before sample 0 (silence-before-attack is truth). For a
  *looped source* whose loop metadata is known, pad past the end by
  **wrapping loop content** (continue reading from `loopStart`) so the filter
  at the loop end sees the continuation the GBA will actually play. One-shot
  tails: zero-pad.
- **Identity bypass**: `|r − 1| < 1e−9` → bit-exact passthrough (required by
  the corpus round-trip test).

**Preserve-imported-loop mode** (curated loops from sf2 or re-imported GBA
samples, when the user hasn't asked for a new loop search): a **ratio
nudge** makes the loop length land on an integer by construction. Given
source loop length `Lin` and nominal ratio `r₀`:

```
r' = round(Lin · r₀) / Lin          (resample with r' instead of r₀)
agbp = round(sourceEffectiveRate · r' · 1024)
```

The whole-sample speed change is `|r'/r₀ − 1| ≤ 0.5/(Lin·r₀)` — under 2
cents for output loops ≥ 500 samples — and `agbp` re-declares the *true*
rate, so tuning is exact even though the ratio was nudged.
`loopStart' = round(loopStart · r')`, then run the §6 **refine** pass
(±8-sample local NCC search) to re-seat the seam on the new grid. (For
multi-period loops any residual snap error is a phase discontinuity, not a
pitch error — refine fixes it.)

## 4. Pitch detection (prefill only — never silently applied)

**YIN**: difference function → cumulative-mean-normalized difference (CMND)
→ absolute threshold → parabolic interpolation of the chosen minimum.
Rejected: raw autocorrelation (octave errors on bright/odd-harmonic material
— exactly what instrument samples are); FFT peak-picking (window-length vs
resolution tension at low f₀, fundamental-vs-harmonic ambiguity).

Parameters: run on **pre-resample** audio (finer period resolution) over the
sustained region (the loop region if set, else the middle 50% after attack
crop). Frames of 4096 samples, 50% hop; search f₀ ∈ [40 Hz, 2 kHz]; CMND
threshold 0.10 (first minimum below threshold, else global minimum);
per-frame confidence = 1 − CMND(τ). Aggregate = **median f₀ over frames with
CMND < 0.2**; report **"unpitched"** if < 3 confident frames or inter-frame
spread > ±50 cents (percussion, noise, chords).

Output feeds two places: the base-key prefill
(`m_exact = 69 + 12·log2(f₀/440)` → unity/fraction per FORMATS.md §3) and
the loop search's period `P = finalRate / f₀` (kept as a double).

## 5. Normalization

### 5.1 Measured reference targets

`tools/analyze_samples.py` over the 45 sc88pro samples (8-bit signed domain,
FS = 128; reproduced 2026-07-19 — re-run the script to verify):

| Population | Metric | Median | IQR | Min–Max |
|---|---|---|---|---|
| all 45 | peak | 124 (−0.28 dBFS) | 117–127 | 77–128 |
| all 45 | full RMS | 44.6 (−9.15 dBFS) | 35.0–50.3 | 20.8–66.8 |
| 35 looped | loop-region RMS | **45.5 (−8.98 dBFS)** | 37.9–50.7 | 13.3–64.5 |
| 35 looped | crest (peak/RMS) | 2.6 | 2.3–3.1 | 1.8–4.9 |
| 10 one-shot | peak | **124 (−0.28 dBFS)** | 120–126 | 115–128 |
| 10 one-shot | full RMS | 30.6 (−12.4 dBFS) | 27.1–44.4 | 20.8–62.8 |

Reading: the corpus is **peak-normalized to ≈ −0.3 dBFS**, and looped/tonal
material additionally clusters at **loop-RMS ≈ −9 dBFS**. Low-RMS files are
either percussive one-shots with near-full peaks (duration-diluted RMS —
conga 20.8 vs organ 52.6, both "correct") or high-crest tonal outliers
(piano1_84 loop-RMS 13.3, harp peak 77) where the peak, not the RMS, was the
binding constraint. That is exactly the two-mode + peak-cap behavior below.

### 5.2 Normalize operation

Constants (one header, measured medians in a comment next to them):

```
kTargetLoopRms = 45.5 / 128        // −9.0 dBFS, corpus looped median
kPeakCeiling   = 125.0 / 128       // −0.2 dB   (integer-safe: post-gain peak ≤ 125)
```

Mode auto-selected by looped/one-shot, user-overridable:

- **Looped/tonal**: `g = kTargetLoopRms / loopRegionRms`, then cap `g` so the
  post-gain peak ≤ `kPeakCeiling`. When the cap engages (high-crest attacks:
  piano, plucked), the sustain lands below target — correct, and exactly how
  the corpus behaves.
- **One-shot/percussive**: pure peak normalize to `kPeakCeiling`. (Loop RMS
  is undefined and full-file RMS of a decaying hit is duration-dependent.)

**No limiter by default** — a pure gain is deterministic, transparent, and
matches reference tooling. Advanced, explicit, previewed option "tame
attack": soft-knee waveshaper above 0.9
(`y = 0.9 + 0.1·tanh((x−0.9)/0.1)`, odd-symmetric), for when a single
transient costs > 4 dB of sustain level.

**Why hot targets are right** (not "safer" lower ones): every 6 dB of sample
headroom costs one of only 8 bits (a −18 dBFS sample has ~31 dB effective
SNR — audible hiss); the engine mixes `(s8·envVol)>>8` per channel and clips
on the *sum* regardless of per-sample headroom (its defenses are envelope,
velocity, master volume); and a sample leveled unlike the sc88pro set sits
wrong in any mix combining them. Target = match the corpus, deterministically.

Guards: peak < 2/128 → refuse auto-normalize ("silent sample"), export still
allowed; automatic gain capped at +24 dB; > 0.1 % of input samples at full
scale → warn "source already clipped". **DC removal** (stage 2): subtract
the mean when `|mean| > 1/256` — default ON for looped (DC shifts the seam
and wastes symmetric headroom), OFF for one-shots (kick drums are
legitimately asymmetric); toggleable.

## 6. Loop-point auto-suggestion

**Search variables.** Engine truth: `loopLen = size − loopStart` — the loop
end *is* the sample end (CONTEXT.md §3.1). So candidates are pairs `(S, E)`
where `S` = loop start and `E` = the new final sample index; audio after `E`
is trimmed at export (this unifies loop-end selection with tail trimming).

Inputs: final-grid float audio; search region `[A, B]` — default
`A = 40 %` of length, `B = end`, user-adjustable by dragging; detected
period `P` (double, §4) or `P = 0` if unpitched.

**Seam model** — playback order is `… x[E−1], x[E], x[S], x[S+1] …` with
linear interpolation across the wrap. Compare windows around the seam:

```
a = x[E+1−W .. E+W]      b = x[S−W .. S+W−1]      W = clamp(round(2P), 128, 512)
```

(`a` needs up to W samples past `E` — they exist, `E` is inside the
untrimmed tail. W = 128 fixed if unpitched.)

**Metric**: normalized cross-correlation
`NCC = Σaᵢbᵢ / sqrt(Σaᵢ²·Σbᵢ²)`. NCC subsumes zero-crossing matching and is
shape-aware; **no hard zero-crossing requirement** (it biases toward
low-energy instants and ignores derivative mismatch). Because NCC is
scale-invariant, add explicit gates:

- level match: `|RMS_dB(a) − RMS_dB(b)| ≤ 1.5 dB`;
- anti-pump: RMS of the loop's first 10 % vs last 10 % within 1.0 dB
  (rejects loops on decaying material that would breathe every pass).

**Candidate generation / pruning**:

1. Loop lengths: pitched — `Lₖ = round(k·P)` for all k with
   `Lₖ ∈ [L_min, B−A]`, `L_min = max(2P, 30 ms)` (short loops are legal but
   buzzy; prefer capturing vibrato/ensemble motion). Unpitched — geometric
   ladder from 50 ms to the region length, ×1.12 steps.
2. Candidate ends: coarse grid every 4 samples over `[A + L, B]`;
   `S = E + 1 − L`.
3. Coarse NCC (window W/2) on the ×4 grid → keep top 200 → full-W NCC at
   1-sample resolution over ±4 around each survivor → apply gates.
4. Score = `0.6·NCC + 0.2·(1 − |ΔRMS_dB|/1.5) + 0.2·log(L)/log(L_max)`
   (mild long-loop preference). Dedupe candidates whose S and E are both
   within P/2 of a better-scoring one. **Return top 5** for the UI.

**Refine pass** (shared with §3's preserve mode): given an existing (S, E),
local NCC search over ±8 samples to re-seat the seam.

**Click metrics — computed post-quantize on the s8 render** (float would
lie: quantization adds ±1 LSB of seam error invisible upstream). Displayed
per candidate and live while dragging handles:

- amplitude delta: `|s[S] − (s[E] + (s[E] − s[E−1]))|` LSB — first-order
  continuation error, matching the engine's linear interpolation;
- derivative delta: `|(s[S+1] − s[S]) − (s[E] − s[E−1])|` LSB/sample;
- NCC as a 0–100 "match" score.

Badges: **green** = amp ≤ 2 LSB AND deriv ≤ 3 LSB/sample; **amber** ≤ double
those; **red** otherwise.

**Crossfade bake (ships with the feature).** The engine cannot crossfade,
but porydaw can pre-bake one — the most effective fix for stubborn material
(ensemble strings, breathy pads). `F = clamp(round(4P), 64,
round(min(L/4, 50 ms)))`; require `S ≥ F` (shrink F otherwise). For
`i ∈ [0, F)`:

```
x[E−F+1+i] ← w(i)·x[E−F+1+i] + (1−w(i))·x[S−F+1+i]
```

— the loop end morphs into the material preceding the loop start, making
end→start continuous by construction. Law auto-selected by seam NCC:
**linear** when NCC > 0.9 (correlated material — equal-power would bump
+3 dB), **equal-power** (`w = cos²`) otherwise. It is a pipeline stage
(destructive-with-preview): undo = toggle off and re-render.

**Edge cases**: total length < 256 samples → auto-search disabled, manual
loop allowed with live metrics (single-cycle chip-style workflow; §3's
ratio-nudge keeps it in tune). Unpitched with nothing passing the gates →
return best-effort candidates flagged "no clean loop found — consider
crossfade bake". UI enforces `L ≥ 16` (engine degrades `loopLen ≤ 0` to
no-loop; don't get near it).

## 7. Crop, fades, end-of-sample rules

- **Attack crop**: auto-suggest the first index where
  `|x| > max(peak·10⁻³, 2/128)`, minus 2 ms pre-roll. Always apply a
  **1.5 ms raised-cosine fade-in** (~20 samples at 13.4 kHz) at the final
  start unless disabled — zero-crossing snap alone leaves a derivative pop;
  1.5 ms is inaudible on any attack. Snap-to-zero-crossing offered as a
  secondary option. Manual mid-waveform crops get the same micro-fade.
- **One-shot tail**: auto-trim below −60 dBFS (post-gain, 5 ms RMS window),
  then a **5 ms raised-cosine fade-out to exactly 0** — a nonzero final
  sample pops when the channel stops.
- **Looped tail**: audio after `E` is discarded at export — the GBA has no
  post-loop tail (loop end = size). A manually-placed early loop end means
  the export is trimmed to it, and the UI says so.
- **Exported metadata** (n = final sample count): `smpl` loop record
  `(S, n−1)` inclusive; `agbl = n`; `agbp` per FORMATS.md §3. (Note the
  shipped corpus carries `agbl = n−1` — a ROM-round-trip artifact, do not
  imitate; FORMATS.md §1.2.)
- **Alignment**: none in the .wav — wav2agb pads the `.bin` after `size`
  (padding unreachable at playback). UI reports ROM cost
  `16 + align4(n)` bytes.

## 8. Preview correctness

**Default: GBA-true.** Render the full pipeline **including s8
quantization**, build an in-memory `WaveData` (freq = the agbp value, loop
flag/loopStart from final markers), and key it through the embedded pory4a
engine (PLAN.md §4) — audition inherits the engine's linear-interp fetch,
loop wrap, envelope, and volume math by construction rather than by
simulation.

- Keyboard audition at arbitrary MIDI keys (exercises base-key/tuning
  metadata end-to-end, not just the waveform).
- Sustained audition holds ≥ 3 loop passes; a "loop seam solo" control
  auditions ±150 ms around the seam on repeat.
- **A/B toggle** to the hi-res float intermediate (pre-quantize,
  pre-resample) so the user can hear what the pipeline costs. Default stays
  GBA-true: seam clicks and the 8-bit noise floor only exist post-quantize;
  a default preview that hides them would defeat the feature.

## 9. `--samplecheck` DSP acceptance criteria

All offline and deterministic (fixed seeds; no wall-clock/randomness):

1. **Passband**: 44100→13379 sine sweep 100 Hz–6.0 kHz (0.9·out-Nyquist):
   output level within ±0.1 dB of unity.
2. **Alias rejection**: input sines at 8, 10, 14 kHz (above out-Nyquist
   6689.5): output RMS ≤ −80 dB re input.
3. **DC / linearity / phase**: constant input → same constant (±1e−4);
   impulse response symmetric (linear phase); 1 kHz in → spectral peak at
   1000 ± 0.5 Hz out.
4. **Identity bypass**: equal rates → bit-exact passthrough.
5. **Quantizer**: matches `clamp(floor(x·128), −128, 127)` on a vector
   including ±1.0, ±127.5/128, ±ε, 0; u8→float→s8 identity across the
   sc88pro corpus (corpus-conditional); no-op-pipeline export byte-equal to
   the paired reference `.bin` data (+128 offset), modulo the corpus's
   `agbl = n−1` final-sample trim.
6. **Normalization**: two runs → identical bytes; synthetic looped tone
   lands within 0.1 dB of `kTargetLoopRms` when the cap is disengaged; cap
   never exceeded; corpus stats within the recorded IQR (drift detection,
   corpus-conditional).
7. **Loop search**: synthetic 440 Hz + 5 Hz vibrato + slow decay: top
   candidate has post-quantize seam amp ≤ 2 LSB, deriv ≤ 3 LSB/sample,
   NCC ≥ 0.95, and L within 1 % of an integer multiple of the period;
   white noise → "unpitched", gates behave.
8. **Loop through engine** (integration): export a looped sample, load via
   `voicegroup_loader`, render ≥ 4 loop wraps through `m4a_channel`; max
   inter-sample step at wraps ≤ max step within the loop body + 2 LSB.
9. **Pitch detection**: sines and sawtooths, f₀ from A1 to A6, at
   8/13.379/22.05/44.1 kHz: within ±5 cents; noise → unpitched.
10. **Metadata round-trip**: exported smpl/agbp/agbl re-parse to identical
    values; `agbp == round(effectiveRate·1024)`; `agbl == n`;
    unity/fraction reconstruct `m_exact` with `frac ∈ [0, 1)`.
