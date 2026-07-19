#include "sampledsp.h"

namespace SampleDsp {

namespace {

// Kaiser design per DSP.md §3: A = 90 dB → β = 0.1102·(A − 8.7).
constexpr double kBeta = 8.96;
constexpr double kRolloff = 0.945;
constexpr double kLobesPerSide = 56.0;
constexpr double kPi = 3.14159265358979323846;

// Standard I₀ power series (double, converges in ~25 terms at β ≈ 9).
double besselI0(double x)
{
    const double half = x / 2.0;
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 40; k++) {
        term *= (half / k) * (half / k);
        sum += term;
        if (term < sum * 1e-17)
            break;
    }
    return sum;
}

inline double sinc(double v)
{
    if (std::abs(v) < 1e-12)
        return 1.0;
    const double pv = kPi * v;
    return std::sin(pv) / pv;
}

} // namespace

std::vector<float> resampleSinc(const float *x, qint64 n, double ratio,
                                qint64 outCount, qint64 loopWrapStart,
                                qint64 loopWrapExcl)
{
    std::vector<float> y(size_t(outCount > 0 ? outCount : 0), 0.0f);
    if (n <= 0 || outCount <= 0 || ratio <= 0.0)
        return y;

    const qint64 loopLen =
        loopWrapExcl > loopWrapStart ? loopWrapExcl - loopWrapStart : 0;
    const auto sample = [&](qint64 k) -> double {
        if (k < 0)
            return 0.0; // silence-before-attack is truth
        if (loopLen > 0 && k >= loopWrapExcl)
            k = loopWrapStart + (k - loopWrapStart) % loopLen;
        if (k >= n)
            return 0.0; // one-shot tail: zero-pad
        return double(x[k]);
    };

    // Identity bypass: bit-exact passthrough (required by the corpus
    // round-trip test).
    if (std::abs(ratio - 1.0) < 1e-9) {
        for (qint64 i = 0; i < outCount; i++)
            y[size_t(i)] = float(sample(i));
        return y;
    }

    const double fc = 0.5 * std::min(1.0, ratio) * kRolloff;
    const double W = kLobesPerSide / (2.0 * fc);
    const double i0Beta = besselI0(kBeta);

    for (qint64 out = 0; out < outCount; out++) {
        const double t = double(out) / ratio;
        const qint64 k0 = qint64(std::ceil(t - W));
        const qint64 k1 = qint64(std::floor(t + W));
        double num = 0.0, den = 0.0;
        for (qint64 k = k0; k <= k1; k++) {
            const double u = double(k) - t;
            const double v = u / W;
            const double arg = 1.0 - v * v;
            if (arg < 0.0)
                continue;
            const double h = 2.0 * fc * sinc(2.0 * fc * u)
                * (besselI0(kBeta * std::sqrt(arg)) / i0Beta);
            num += sample(k) * h;
            den += h;
        }
        // Per-output tap-sum normalization: exact unity DC gain at every
        // fractional position, clean edge handling.
        y[size_t(out)] = den != 0.0 ? float(num / den) : 0.0f;
    }
    return y;
}

QByteArray quantizeBuffer(const std::vector<float> &x, bool dither)
{
    QByteArray out(int(x.size()), Qt::Uninitialized);
    // Fixed-seed LCG (Numerical Recipes constants): exports must stay
    // deterministic — the harness diffs repeated renders byte-for-byte.
    quint32 rng = 0x50525944u;
    const auto uniform = [&rng]() {
        rng = rng * 1664525u + 1013904223u;
        return double(rng) / 4294967296.0;
    };
    for (size_t i = 0; i < x.size(); i++) {
        double v = double(x[i]) * 128.0;
        if (dither)
            v += uniform() + uniform() - 1.0; // TPDF ±1 LSB
        double s = std::floor(v);
        if (s < -128.0)
            s = -128.0;
        else if (s > 127.0)
            s = 127.0;
        out[int(i)] = char(qint8(int(s)));
    }
    return out;
}

qint64 nearestZeroCrossing(const float *x, qint64 n, qint64 idx)
{
    if (n < 2)
        return idx;
    idx = qBound(qint64(0), idx, n - 1);
    const auto crossesAt = [&](qint64 i) {
        // A crossing sits between i−1 and i.
        return i > 0 && i < n
            && ((x[i - 1] < 0.0f && x[i] >= 0.0f)
                || (x[i - 1] >= 0.0f && x[i] < 0.0f));
    };
    for (qint64 d = 0; d < n; d++) {
        if (crossesAt(idx - d))
            return idx - d;
        if (crossesAt(idx + d))
            return idx + d;
    }
    return idx;
}

double normalizeGain(const float *x, qint64 n, bool loopedMode,
                     qint64 loopStart, QString *warning)
{
    if (warning)
        warning->clear();
    if (n <= 0)
        return 1.0;
    double peak = 0.0;
    for (qint64 i = 0; i < n; i++)
        peak = std::max(peak, std::abs(double(x[i])));
    if (peak < 2.0 / 128.0) {
        if (warning)
            *warning = QStringLiteral(
                "silent sample — auto-normalize skipped.");
        return 1.0;
    }

    double gain;
    if (loopedMode) {
        loopStart = qBound(qint64(0), loopStart, n - 1);
        double sum = 0.0;
        for (qint64 i = loopStart; i < n; i++)
            sum += double(x[i]) * double(x[i]);
        const double rms = std::sqrt(sum / double(n - loopStart));
        if (rms <= 0.0) {
            if (warning)
                *warning = QStringLiteral(
                    "silent loop region — auto-normalize skipped.");
            return 1.0;
        }
        gain = kTargetLoopRms / rms;
        // Peak cap: high-crest material lands below the RMS target — correct,
        // and exactly how the sc88pro corpus behaves (DSP.md §5.1).
        if (gain * peak > kPeakCeiling)
            gain = kPeakCeiling / peak;
    } else {
        gain = kPeakCeiling / peak;
    }
    if (gain > kMaxAutoGain) {
        gain = kMaxAutoGain;
        if (warning)
            *warning = QStringLiteral("automatic gain capped at +24 dB.");
    }
    return gain;
}

} // namespace SampleDsp
