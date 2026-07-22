#include "sampledoc.h"

#include <algorithm>
#include <cmath>

#include "sampledsp.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

} // namespace

SampleDocument::SampleDocument(ImportedSample source)
    : m_source(std::move(source))
{
    m_params = defaultParams(m_source);
}

SampleEditParams SampleDocument::defaultParams(const ImportedSample &source)
{
    SampleEditParams p;
    const qint64 n = source.frameCount();
    p.cropStart = 0;
    // One-shots honor a source agbl override (the engine never plays past
    // it); looped sources keep the full buffer — the loop end trims at
    // export (DSP.md §7).
    p.cropEnd = source.hasLoop ? n : qMin(source.playLength, n);
    if (p.cropEnd <= 0)
        p.cropEnd = n;
    p.loopOn = source.hasLoop;
    p.loopStart = source.loopStart;
    p.loopEnd = source.loopEndIncl;
    p.baseKey = source.baseKey;
    p.fineTuneCents =
        qBound(0.0, source.fracSemitone * 100.0, 99.9999999999);
    // Fresh sources default to the common GBA music rate — a beginner's
    // 44.1 kHz import should not land a 44.1 kHz ROM sample. Sources at or
    // below it keep theirs; prepared files stay byte-faithful (below).
    p.targetRate = source.gbaReady
        ? source.sampleRate
        : qMin(source.sampleRate, kGbaDefaultRate);
    if (source.gbaReady) {
        // Prepared file: byte-faithful no-op pipeline.
        p.normalizeMode = SampleEditParams::NormalizeOff;
        p.dcRemove = SampleEditParams::Off;
        p.fadeIn = false;
        p.fadeOut = false;
        p.exactPitchOverride = source.exactPitch;
    }
    return p;
}

void SampleDocument::setParams(const SampleEditParams &params)
{
    if (params == m_params)
        return;
    m_params = params;
    m_dirty = true;
}

const ProcessedSample &SampleDocument::processed()
{
    if (m_dirty) {
        m_processed = render();
        m_dirty = false;
    }
    return m_processed;
}

ProcessedSample SampleDocument::render() const
{
    ProcessedSample out;
    const SampleEditParams &p = m_params;
    const qint64 n0 = m_source.frameCount();

    // [1] Crop — source-rate domain; loop markers shift into cropped coords.
    const qint64 cropStart = qBound<qint64>(0, p.cropStart, n0);
    const qint64 cropEnd = qBound<qint64>(cropStart, p.cropEnd, n0);
    std::vector<float> buf(m_source.buffer.begin() + cropStart,
                           m_source.buffer.begin() + cropEnd);
    const qint64 len = qint64(buf.size());
    if (len <= 0) {
        out.warnings += QStringLiteral("the crop selects no samples.");
        return out;
    }
    bool loopOn = p.loopOn;
    qint64 loopStartC = p.loopStart - cropStart;
    qint64 loopEndC = p.loopEnd - cropStart; // inclusive
    if (loopOn) {
        loopStartC = qBound<qint64>(0, loopStartC, len - 1);
        loopEndC = qBound<qint64>(0, loopEndC, len - 1);
        if (loopEndC <= loopStartC) {
            loopOn = false;
            out.warnings += QStringLiteral(
                "loop markers fall outside the crop — loop disabled.");
        }
    }

    // [2] DC removal — default on for looped material (a DC offset shifts
    // the seam and wastes symmetric headroom), off for one-shots.
    const bool dcOn = p.dcRemove == SampleEditParams::On
        || (p.dcRemove == SampleEditParams::Auto && loopOn);
    if (dcOn) {
        double mean = 0.0;
        for (const float v : buf)
            mean += v;
        mean /= double(len);
        if (std::abs(mean) > 1.0 / 256.0) {
            for (float &v : buf)
                v = float(v - mean);
        }
    }

    // [3] Resample — enters the final sample grid. A looped render nudges
    // the ratio so the loop length lands on an integer by construction
    // (DSP.md §3 preserve-imported-loop mode); agbp re-declares the true
    // rate, so tuning stays exact.
    const double srcRate = m_source.sampleRate;
    const double targetRate = p.targetRate > 0.0 ? p.targetRate : srcRate;
    const double r0 = srcRate > 0.0 ? targetRate / srcRate : 1.0;
    const bool identity = std::abs(r0 - 1.0) < 1e-9;
    std::vector<float> grid;
    double outputRate;
    qint64 loopStartOut = 0;
    qint64 nOut;
    if (identity) {
        outputRate = srcRate;
        loopStartOut = loopStartC;
        // The GBA has no post-loop tail: the loop end is the sample end.
        nOut = loopOn ? loopEndC + 1 : len;
        buf.resize(size_t(nOut));
        grid = std::move(buf);
    } else if (loopOn) {
        const qint64 lin = loopEndC + 1 - loopStartC;
        const qint64 lout = qMax<qint64>(1, qint64(std::llround(
                                                double(lin) * r0)));
        const double ratio = double(lout) / double(lin);
        outputRate = srcRate * ratio;
        loopStartOut = qint64(std::llround(double(loopStartC) * ratio));
        nOut = loopStartOut + lout;
        grid = SampleDsp::resampleSinc(buf.data(), len, ratio, nOut,
                                       loopStartC, loopEndC + 1);
    } else {
        outputRate = targetRate;
        nOut = qMax<qint64>(1, qint64(std::llround(double(len) * r0)));
        grid = SampleDsp::resampleSinc(buf.data(), len, r0, nOut);
    }

    // [4] pitch detection and [5] loop search are analysis stages (phase 3);
    // markers are already authoritative integers on the final grid here.

    // [6] Normalize — pure gain, peak-capped (DSP.md §5).
    SampleEditParams::NormalizeMode mode = p.normalizeMode;
    if (mode == SampleEditParams::NormalizeAuto)
        mode = loopOn ? SampleEditParams::NormalizeLooped
                      : SampleEditParams::NormalizeOneShot;
    double gain = 1.0;
    if (p.normalizeMode != SampleEditParams::NormalizeOff) {
        QString warning;
        gain = SampleDsp::normalizeGain(
            grid.data(), nOut, mode == SampleEditParams::NormalizeLooped,
            loopStartOut, &warning);
        if (!warning.isEmpty())
            out.warnings += warning;
        if (gain != 1.0) {
            for (float &v : grid)
                v = float(double(v) * gain);
        }
    }

    // [7] Crossfade bake at the seam (DSP.md §6): the loop end morphs into
    // the material preceding the loop start, so end → start is continuous by
    // construction. The fade length follows the pitch period implied by the
    // unity note (the sample sounds f₀ = 440·2^((key−69)/12) at unity);
    // the law is picked from the pre-bake seam NCC — linear on correlated
    // material (equal-power would bump +3 dB), equal-power otherwise.
    const double exactKey = double(p.baseKey) + p.fineTuneCents / 100.0;
    if (p.crossfadeOn && loopOn) {
        const double f0 = 440.0 * std::pow(2.0, (exactKey - 69.0) / 12.0);
        const double period = f0 > 0.0 ? outputRate / f0 : 0.0;
        const qint64 loopLen = nOut - loopStartOut;
        qint64 F = std::max<qint64>(qint64(std::llround(4.0 * period)), 64);
        F = std::min(F, qint64(std::llround(
                            std::min(double(loopLen) / 4.0,
                                     0.050 * outputRate))));
        F = std::min(F, loopStartOut); // needs F samples before the start
        if (F >= 4) {
            const qint64 S = loopStartOut, E = nOut - 1;
            double ab = 0.0, aa = 0.0, bb = 0.0;
            const qint64 W = std::min<qint64>({128, S, loopLen / 2});
            const auto wrapped = [&](qint64 j) {
                return double(grid[size_t(j >= nOut ? S + (j - nOut) : j)]);
            };
            for (qint64 i = 0; W >= 2 && i < 2 * W; i++) {
                const double a = wrapped(E + 1 - W + i);
                const double b = double(grid[size_t(S - W + i)]);
                ab += a * b;
                aa += a * a;
                bb += b * b;
            }
            const double ncc =
                (aa > 0.0 && bb > 0.0) ? ab / std::sqrt(aa * bb) : 0.0;
            const bool linear = ncc > 0.9;
            for (qint64 i = 0; i < F; i++) {
                // w runs 1 → exactly 0, so the final sample x[E] becomes
                // x[S−1] — the sequence then continues into x[S] exactly as
                // the source did, end → start continuous by construction.
                const double t = double(i + 1) / double(F);
                const double w = linear
                    ? 1.0 - t
                    : std::cos(kPi * t / 2.0) * std::cos(kPi * t / 2.0);
                const qint64 idx = E - F + 1 + i;
                // One loop length back: idx − L = S − F + i.
                const qint64 src = S - F + i;
                grid[size_t(idx)] = float(w * double(grid[size_t(idx)])
                                          + (1.0 - w)
                                              * double(grid[size_t(src)]));
            }
        } else {
            out.warnings += QStringLiteral(
                "loop start too close to the sample start for a crossfade "
                "bake — skipped.");
        }
    }

    // [8] Micro fades (DSP.md §7). The fade-in never reaches into the loop
    // region; the fade-out only exists for one-shots and ends at exactly 0.
    if (p.fadeIn) {
        qint64 f = qint64(std::llround(0.0015 * outputRate));
        f = qMin(f, loopOn ? loopStartOut : nOut);
        for (qint64 i = 0; i < f; i++)
            grid[size_t(i)] =
                float(grid[size_t(i)] * 0.5 * (1.0 - std::cos(kPi * double(i) / double(f))));
    }
    if (p.fadeOut && !loopOn) {
        const qint64 f =
            qMin<qint64>(qint64(std::llround(0.005 * outputRate)), nOut);
        for (qint64 j = 0; j < f; j++)
            grid[size_t(nOut - 1 - j)] = float(
                grid[size_t(nOut - 1 - j)] * 0.5
                * (1.0 - std::cos(kPi * double(j) / double(f))));
    }

    // [9] Quantize once (bit-matches wav2agb; DSP.md §2).
    out.s8 = SampleDsp::quantizeBuffer(grid, p.ditherOn);
    out.size = quint32(nOut);
    out.looped = loopOn;
    out.loopStart = loopOn ? quint32(loopStartOut) : 0;

    // Pitch metadata (FORMATS.md §3): retune via the unity note, never DSP.
    out.outputRate = outputRate;
    out.effectiveRate =
        outputRate * std::pow(2.0, (60.0 - exactKey) / 12.0);
    out.freq = p.exactPitchOverride != 0
        ? p.exactPitchOverride
        : quint32(std::llround(out.effectiveRate * 1024.0));
    out.declaredRate = quint32(std::llround(outputRate));
    out.unityNote = p.baseKey;
    const double fracScaled = (p.fineTuneCents / 100.0) * 4294967296.0;
    out.pitchFraction =
        quint32(qMin<qint64>(qint64(std::llround(fracScaled)), 4294967295LL));
    out.normalizeGain = gain;
    if (loopOn)
        out.seam = SampleDsp::seamMetricsAt(
            reinterpret_cast<const qint8 *>(out.s8.constData()), nOut,
            loopStartOut, nOut - 1);
    out.preview = std::move(grid);
    return out;
}
