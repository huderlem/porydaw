#pragma once

#include <QByteArray>
#include <QString>

#include <cmath>
#include <vector>

// Pure stateless DSP for the Sample Studio pipeline (docs/sample-studio/
// DSP.md). Phase-2 core: windowed-sinc resampler, wav2agb-bit-matched
// quantizer, zero-crossing snap, marker mapping, and the corpus-measured
// normalization gain. Loop search and pitch detection land in phase 3.
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

} // namespace SampleDsp
