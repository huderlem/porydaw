#pragma once

#include <QByteArray>
#include <QString>

#include <cmath>
#include <vector>

#include "sampledata.h"

// Pure stateless DSP for the Sample Studio pipeline (docs/sample-studio/
// DSP.md): windowed-sinc resampler, wav2agb-bit-matched quantizer,
// zero-crossing snap, marker mapping, the corpus-measured normalization
// gain, YIN pitch detection, loop-point suggestion, and seam metrics.
namespace SampleDsp {

// Normalization constants (DSP.md §5.2; corpus medians in analyze_samples.py:
// looped loop-region RMS 45.5/128 ≈ −9.0 dBFS, peak 124..125/128 ≈ −0.3 dB).
inline constexpr double kTargetLoopRms = 45.5 / 128.0;
inline constexpr double kPeakCeiling = 125.0 / 128.0;
inline constexpr double kMaxAutoGain = 15.848931924611133; // +24 dB

// Direct-convolution windowed-sinc resampler (DSP.md §3): Kaiser β = 8.96
// (90 dB), L = 56 lobes per side, rolloff ρ = 0.945, per-output tap-sum
// normalization. ratio = outputRate / inputRate. Reads zeros before sample 0.
// When loopWrapExcl > 0 the input wraps past that index back into
// [loopWrapStart, loopWrapExcl) — the continuation the GBA actually plays;
// otherwise it zero-pads past n. |ratio − 1| < 1e-9 is a bit-exact bypass.
std::vector<float> resampleSinc(const float *x, qint64 n, double ratio,
                                qint64 outCount, qint64 loopWrapStart = 0,
                                qint64 loopWrapExcl = 0);

// The wav2agb-bit-matched quantizer: clamp(floor(x·128), −128, 127), in
// double (DSP.md §2).
inline qint8 quantizeToAgb8(double x)
{
    double s = std::floor(x * 128.0);
    if (s < -128.0)
        s = -128.0;
    else if (s > 127.0)
        s = 127.0;
    return qint8(int(s));
}

// Whole-buffer quantize to signed-8 bytes. dither adds TPDF ±1 LSB from a
// fixed-seed LCG so exports stay deterministic (DSP.md §2 advanced toggle).
QByteArray quantizeBuffer(const std::vector<float> &x, bool dither);

// Nearest index to idx where the signal crosses zero (sign change between
// consecutive samples); returns idx when the buffer never crosses.
qint64 nearestZeroCrossing(const float *x, qint64 n, qint64 idx);

// Marker bookkeeping (DSP.md §1): a source-domain position through the crop
// then the resample ratio, rounded (never load-bearing — phase 3's refine
// pass re-seats loop markers).
inline qint64 mapMarker(qint64 srcPos, qint64 cropStart, double ratio)
{
    return qint64(std::llround(double(srcPos - cropStart) * ratio));
}

// Normalization gain (DSP.md §5.2). loopedMode: g = kTargetLoopRms / RMS of
// [loopStart, n), capped so post-gain peak ≤ kPeakCeiling. One-shot: pure
// peak normalize to kPeakCeiling. Guards: near-silent input refuses
// auto-normalize (g = 1 + warning), automatic gain caps at +24 dB.
double normalizeGain(const float *x, qint64 n, bool loopedMode,
                     qint64 loopStart, QString *warning);

// YIN pitch detection (DSP.md §4): difference function → CMND → absolute
// threshold 0.10 → fractional-lag refinement. Frames of 4096 samples at 50%
// hop over the given buffer; f₀ searched in [40 Hz, 2 kHz]. Aggregate is the
// median over frames with CMND < 0.2; unpitched when fewer than 3 frames
// qualify or the inter-frame spread exceeds ±50 cents. Prefill only — the
// result is never silently applied to the pipeline.
struct PitchResult {
    bool pitched = false;
    double f0 = 0.0;         // Hz
    double confidence = 0.0; // median per-frame 1 − CMND(τ)
};
PitchResult detectPitchYin(const float *x, qint64 n, double rate);

// Loop-point suggestion (DSP.md §6) on the FINAL sample grid. Candidates are
// (S, E) pairs where S is the loop start and E the new final sample index
// (audio after E is trimmed at export — the loop end IS the sample end).
// period is the detected pitch period in samples (0 = unpitched, geometric
// length ladder). The search region is [regionA, regionB]; candidate ends
// are additionally kept W samples clear of the buffer end so the seam's
// post-E comparison window reads real tail samples. Returns up to 5
// candidates, gate-passing first, best score first. Candidates that fail the
// level-match / anti-pump gates come back with passedGates = false ("no
// clean loop found — consider crossfade bake"). Buffers shorter than 256
// samples return nothing (auto-search disabled; manual loops stay legal).
struct LoopCandidate {
    qint64 loopStart = 0; // S, final-grid, inclusive
    qint64 loopEnd = 0;   // E, final-grid, inclusive (becomes the sample end)
    double ncc = 0.0;     // full-window seam NCC
    double score = 0.0;
    bool passedGates = false;
};
std::vector<LoopCandidate> suggestLoop(const float *x, qint64 n, double rate,
                                       double period, qint64 regionA,
                                       qint64 regionB);

// Refine pass (shared with DSP.md §3's preserve-imported-loop mode): local
// full-window NCC search over ±8 samples around S and E to re-seat the seam
// on the current grid. Adjusts *loopStart/*loopEnd in place.
void refineLoop(const float *x, qint64 n, double period, qint64 *loopStart,
                qint64 *loopEnd);

// Post-quantize seam click metrics at loop (S, E) in the signed-8 domain
// (DSP.md §6), against the engine's linear-interpolated wrap s[E] → s[S].
// The NCC comparison window past E reads real samples when the buffer still
// has them (candidate evaluation on an untrimmed tail) and wraps into the
// loop otherwise (E == n−1, the exported shape).
SeamMetrics seamMetricsAt(const qint8 *s8, qint64 n, qint64 loopStart,
                          qint64 loopEnd);

// Min/max peak pyramid for the waveform view: level k summarizes blocks of
// 16^(k+1) samples, so a paint pass reads O(width) buckets at any zoom.
struct PeakPyramid {
    static constexpr qint64 kBlock = 16;
    struct MinMax {
        float lo = 0.0f;
        float hi = 0.0f;
    };
    std::vector<std::vector<MinMax>> levels;
    qint64 frameCount = 0;

    void build(const float *x, qint64 n);
    // Extremes over [from, to) — x must be the buffer build() saw.
    MinMax query(const float *x, qint64 from, qint64 to) const;
};

} // namespace SampleDsp
