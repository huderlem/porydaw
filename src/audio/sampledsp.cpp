#include "sampledsp.h"

#include <algorithm>
#include <cstdlib>

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

// Kaiser window as a function of arg = 1 − v², linearly interpolated from a
// table: w(v) = I₀(β·sqrt(arg))/I₀(β) = g(arg). Tabulating over arg needs no
// per-tap sqrt or I₀ series — the render budget is interactive (the dialog
// re-renders on every control change), and per-tap I₀ dominated it ~10:1.
// β is a compile-time constant, so the table is built once.
constexpr int kKaiserTableN = 8192;

double kaiserFromArg(double arg)
{
    static const std::vector<double> table = [] {
        std::vector<double> t(kKaiserTableN + 1);
        const double i0Beta = besselI0(kBeta);
        for (int i = 0; i <= kKaiserTableN; i++)
            t[size_t(i)] =
                besselI0(kBeta * std::sqrt(double(i) / kKaiserTableN))
                / i0Beta;
        return t;
    }();
    const double x = arg * kKaiserTableN;
    const int i = int(x) >= kKaiserTableN ? kKaiserTableN - 1 : int(x);
    const double f = x - double(i);
    return table[size_t(i)] * (1.0 - f) + table[size_t(i) + 1] * f;
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
    // sin(2π·fc·u) advances by a constant phase step as k walks the taps —
    // one rotation per tap replaces a libm sin() call (double keeps the
    // accumulated error ~1e-14 over the ~400-tap window).
    const double stepSin = std::sin(2.0 * kPi * fc);
    const double stepCos = std::cos(2.0 * kPi * fc);

    for (qint64 out = 0; out < outCount; out++) {
        const double t = double(out) / ratio;
        const qint64 k0 = qint64(std::ceil(t - W));
        const qint64 k1 = qint64(std::floor(t + W));
        double u = double(k0) - t;
        double s = std::sin(2.0 * kPi * fc * u);
        double c = std::cos(2.0 * kPi * fc * u);
        double num = 0.0, den = 0.0;
        for (qint64 k = k0; k <= k1; k++, u += 1.0) {
            const double v = u / W;
            const double arg = 1.0 - v * v;
            if (arg >= 0.0) {
                // h(u) = 2fc·sinc(2fc·u)·kaiser = sin(2π·fc·u)/(π·u)·kaiser.
                const double h = (std::abs(u) < 1e-9
                                      ? 2.0 * fc
                                      : s / (kPi * u))
                    * kaiserFromArg(arg);
                num += sample(k) * h;
                den += h;
            }
            const double sn = s * stepCos + c * stepSin;
            c = c * stepCos - s * stepSin;
            s = sn;
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

namespace {

// The YIN difference function at a fractional lag, with linear interpolation
// of the delayed signal. Integer YIN quantizes the period to whole samples —
// at high f₀ (small τ) that alone costs tens of cents, so the minimum is
// re-sought on this continuous function (DSP.md §4's parabolic-interpolation
// step, made exact enough for the ±5-cent acceptance bound).
double yinDifferenceFrac(const float *x, qint64 window, double tau)
{
    const qint64 t0 = qint64(std::floor(tau));
    const double f = tau - double(t0);
    double sum = 0.0;
    for (qint64 j = 0; j < window; j++) {
        const double delayed = double(x[j + t0]) * (1.0 - f)
            + double(x[j + t0 + 1]) * f;
        const double d = double(x[j]) - delayed;
        sum += d * d;
    }
    return sum;
}

// Windowed seam NCC in the float domain: window a leads out of the loop end
// (real tail samples), window b leads out of the loop start. Shared by the
// suggestion scorer, the refine pass, and the crossfade-law pick.
double floatSeamNcc(const float *x, qint64 n, qint64 S, qint64 E, qint64 W)
{
    W = std::min({W, S, n - 1 - E});
    if (W < 2)
        return 0.0;
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (qint64 i = 0; i < 2 * W; i++) {
        const double a = double(x[E + 1 - W + i]);
        const double b = double(x[S - W + i]);
        ab += a * b;
        aa += a * a;
        bb += b * b;
    }
    return (aa > 0.0 && bb > 0.0) ? ab / std::sqrt(aa * bb) : 0.0;
}

} // namespace

PitchResult detectPitchYin(const float *x, qint64 n, double rate)
{
    PitchResult result;
    constexpr qint64 kFrame = 4096;
    constexpr qint64 kWindow = kFrame / 2;
    constexpr double kThreshold = 0.10;
    constexpr double kConfidentCmnd = 0.20;
    if (rate <= 0.0 || n < kFrame)
        return result;

    const qint64 tauMin =
        std::max<qint64>(2, qint64(std::floor(rate / 2000.0)));
    const qint64 tauMax =
        std::min<qint64>(kWindow - 2, qint64(std::ceil(rate / 40.0)));
    if (tauMax <= tauMin)
        return result;

    std::vector<double> d(size_t(tauMax) + 1, 0.0);
    std::vector<double> cum(size_t(tauMax) + 1, 0.0);
    std::vector<double> cmnd(size_t(tauMax) + 1, 1.0);
    struct Frame {
        double f0;
        double confidence;
    };
    std::vector<Frame> frames;

    for (qint64 at = 0; at + kFrame <= n; at += kFrame / 2) {
        const float *frame = x + at;
        for (qint64 tau = 1; tau <= tauMax; tau++) {
            double sum = 0.0;
            for (qint64 j = 0; j < kWindow; j++) {
                const double diff =
                    double(frame[j]) - double(frame[j + tau]);
                sum += diff * diff;
            }
            d[size_t(tau)] = sum;
            cum[size_t(tau)] = cum[size_t(tau) - 1] + sum;
            cmnd[size_t(tau)] = cum[size_t(tau)] > 0.0
                ? sum * double(tau) / cum[size_t(tau)]
                : 1.0;
        }

        // First minimum below the threshold (descend to its local bottom),
        // else the global minimum.
        qint64 best = -1;
        for (qint64 tau = tauMin; tau <= tauMax; tau++) {
            if (cmnd[size_t(tau)] < kThreshold) {
                while (tau + 1 <= tauMax
                       && cmnd[size_t(tau + 1)] < cmnd[size_t(tau)])
                    tau++;
                best = tau;
                break;
            }
        }
        if (best < 0) {
            best = tauMin;
            for (qint64 tau = tauMin + 1; tau <= tauMax; tau++) {
                if (cmnd[size_t(tau)] < cmnd[size_t(best)])
                    best = tau;
            }
        }
        if (cmnd[size_t(best)] >= kConfidentCmnd)
            continue;

        // Fractional-lag refinement of the difference function around the
        // integer minimum: 0.1-lag grid, then a parabola on the best triple.
        const double lo = std::max<double>(tauMin, double(best) - 1.0);
        const double hi = std::min<double>(tauMax, double(best) + 1.0);
        double bestTau = double(best);
        double bestVal = yinDifferenceFrac(frame, kWindow, bestTau);
        for (double tau = lo; tau <= hi + 1e-9; tau += 0.1) {
            const double v = yinDifferenceFrac(frame, kWindow, tau);
            if (v < bestVal) {
                bestVal = v;
                bestTau = tau;
            }
        }
        if (bestTau - 0.1 >= lo && bestTau + 0.1 <= hi) {
            const double va =
                yinDifferenceFrac(frame, kWindow, bestTau - 0.1);
            const double vb = bestVal;
            const double vc =
                yinDifferenceFrac(frame, kWindow, bestTau + 0.1);
            const double denom = va - 2.0 * vb + vc;
            if (denom > 0.0)
                bestTau += 0.1 * 0.5 * (va - vc) / denom;
        }

        // Octave-error guard: at small lags the true-period V is narrow, so
        // the integer global-minimum fallback often lands on a *multiple* of
        // the period (a wider V closer to an integer). A subharmonic whose
        // refined difference is also ~zero IS the true period — adopt the
        // smallest such lag.
        const auto cumAt = [&](double tau) {
            const qint64 t0 =
                qBound<qint64>(1, qint64(std::floor(tau)), tauMax - 1);
            const double f = tau - double(t0);
            return cum[size_t(t0)] * (1.0 - f) + cum[size_t(t0) + 1] * f;
        };
        const auto cmndAt = [&](double tau, double dv) {
            const double c = cumAt(tau);
            return c > 0.0 ? dv * tau / c : 1.0;
        };
        double bestCmnd = cmndAt(bestTau, bestVal);
        for (int k = 4; k >= 2; k--) {
            const double tc = bestTau / double(k);
            if (tc < 1.0 || tc < double(tauMin) - 0.6)
                continue;
            double bt = 0.0, bv = 0.0;
            bool have = false;
            for (double tau = std::max(1.0, tc - 0.6); tau <= tc + 0.6;
                 tau += 0.05) {
                const double v = yinDifferenceFrac(frame, kWindow, tau);
                if (!have || v < bv) {
                    have = true;
                    bv = v;
                    bt = tau;
                }
            }
            if (have) {
                // Sub-step parabola: the 0.05 grid alone quantizes small
                // lags by several cents.
                if (bt - 0.05 >= 1.0) {
                    const double va =
                        yinDifferenceFrac(frame, kWindow, bt - 0.05);
                    const double vc =
                        yinDifferenceFrac(frame, kWindow, bt + 0.05);
                    const double denom = va - 2.0 * bv + vc;
                    if (denom > 0.0)
                        bt += 0.05 * 0.5 * (va - vc) / denom;
                }
                const double c = cmndAt(bt, bv);
                if (c < std::max(kThreshold, bestCmnd * 1.5)) {
                    bestTau = bt;
                    bestVal = bv;
                    bestCmnd = c;
                    break;
                }
            }
        }

        const double f0 = rate / bestTau;
        if (f0 >= 40.0 && f0 <= 2000.0)
            frames.push_back({f0, 1.0 - cmnd[size_t(best)]});
    }

    if (frames.size() < 3)
        return result;
    std::vector<double> f0s;
    f0s.reserve(frames.size());
    for (const Frame &f : frames)
        f0s.push_back(f.f0);
    std::sort(f0s.begin(), f0s.end());
    const size_t mid = f0s.size() / 2;
    const double medianF0 = f0s.size() & 1
        ? f0s[mid]
        : 0.5 * (f0s[mid - 1] + f0s[mid]);
    for (const double f0 : f0s) {
        const double cents = 1200.0 * std::log2(f0 / medianF0);
        if (std::abs(cents) > 50.0)
            return result; // percussion, noise, chords
    }
    std::vector<double> confs;
    confs.reserve(frames.size());
    for (const Frame &f : frames)
        confs.push_back(f.confidence);
    std::sort(confs.begin(), confs.end());
    result.pitched = true;
    result.f0 = medianF0;
    result.confidence = confs[confs.size() / 2];
    return result;
}

std::vector<LoopCandidate> suggestLoop(const float *x, qint64 n, double rate,
                                       double period, qint64 regionA,
                                       qint64 regionB)
{
    std::vector<LoopCandidate> out;
    if (!x || n < 256 || rate <= 0.0)
        return out;

    const bool pitched = period > 0.0;
    const qint64 W = pitched
        ? qBound<qint64>(qint64(128), qint64(std::llround(2.0 * period)),
                       qint64(512))
        : 128;

    // The b window needs S − W ≥ 0 and the a window reads W real samples
    // past E (the untrimmed tail). A buffer that can't fit both windows
    // (large pitched W on a short crop) has no candidates — and would break
    // the qBound below, whose min must not exceed its max.
    if (W >= n - 1 - W)
        return out;
    regionA = qBound<qint64>(W, regionA, n - 1);
    const qint64 Bmax = std::min(regionB, n - 1 - W);
    if (Bmax <= regionA)
        return out;
    const qint64 regionLen = Bmax - regionA;

    // Candidate loop lengths (DSP.md §6): pitched — integer multiples of the
    // period from L_min = max(2P, 30 ms); unpitched — a ×1.12 geometric
    // ladder from 50 ms.
    std::vector<qint64> lengths;
    if (pitched) {
        const double lMin =
            std::max(2.0 * period, 0.030 * rate);
        for (qint64 k = qint64(std::ceil(lMin / period));; k++) {
            const qint64 L = qint64(std::llround(double(k) * period));
            if (L > regionLen)
                break;
            if (L >= 16 && (lengths.empty() || L != lengths.back()))
                lengths.push_back(L);
        }
    } else {
        for (double L = 0.050 * rate; L <= double(regionLen); L *= 1.12) {
            const qint64 Li = qint64(std::llround(L));
            if (Li >= 16 && (lengths.empty() || Li != lengths.back()))
                lengths.push_back(Li);
        }
    }
    if (lengths.empty())
        return out;

    // Energy prefix for all RMS gates and NCC denominators.
    std::vector<double> e2(size_t(n) + 1, 0.0);
    for (qint64 i = 0; i < n; i++)
        e2[size_t(i) + 1] = e2[size_t(i)] + double(x[i]) * double(x[i]);
    const auto energy = [&](qint64 from, qint64 toExcl) {
        return e2[size_t(toExcl)] - e2[size_t(from)];
    };

    // Coarse pass: half-window NCC on a ×4 end grid. For a fixed L both seam
    // windows slide together, so the numerator is a running sum of
    // x[j]·x[j−L] — O(1) per grid step.
    struct Coarse {
        double ncc;
        qint64 L;
        qint64 E;
    };
    // Gate-passing survivors, worst-first, capped at 200; gate-failing ones
    // go to a small best-effort pool used only when nothing passes anywhere
    // ("no clean loop found — consider crossfade bake").
    std::vector<Coarse> top;
    std::vector<Coarse> fallback;
    constexpr size_t kTopKeep = 200;
    constexpr size_t kFallbackKeep = 40;
    const qint64 wCoarse = std::max<qint64>(2, W / 2);
    for (const qint64 L : lengths) {
        const qint64 eStart = std::max(regionA + L, W + L - 1);
        if (eStart > Bmax)
            continue;
        // numer(E) = Σ_{j=E+1−w}^{E+w} x[j]·x[j−L]
        double numer = 0.0;
        for (qint64 j = eStart + 1 - wCoarse; j <= eStart + wCoarse; j++)
            numer += double(x[j]) * double(x[j - L]);
        const qint64 tenth = std::max<qint64>(1, L / 10);
        for (qint64 E = eStart; E <= Bmax; E += 4) {
            const double aa = energy(E + 1 - wCoarse, E + wCoarse + 1);
            const double bb = energy(E + 1 - wCoarse - L, E + wCoarse + 1 - L);
            const double ncc = (aa > 0.0 && bb > 0.0)
                ? numer / std::sqrt(aa * bb)
                : 0.0;
            // Pre-gate the survivor pool with the (O(1), prefix-summed)
            // level gates: on material where near-perfect NCC is abundant
            // (steady tones), the 200 coarse survivors would otherwise be an
            // arbitrary slice that can consist entirely of level-mismatched
            // candidates the fine pass then gates away.
            const qint64 S = E + 1 - L;
            const double aaF = energy(E + 1 - W, E + W + 1);
            const double bbF = energy(S - W, S + W);
            const double head = energy(S, S + tenth);
            const double tail = energy(E + 1 - tenth, E + 1);
            const bool preGate = aaF > 0.0 && bbF > 0.0 && head > 0.0
                && tail > 0.0
                && std::abs(10.0 * std::log10(aaF / bbF)) <= 1.5
                && std::abs(10.0 * std::log10(head / tail)) <= 1.0;
            std::vector<Coarse> &pool = preGate ? top : fallback;
            const size_t cap = preGate ? kTopKeep : kFallbackKeep;
            if (pool.size() < cap || ncc > pool.front().ncc) {
                Coarse c{ncc, L, E};
                const auto pos = std::lower_bound(
                    pool.begin(), pool.end(), c,
                    [](const Coarse &a, const Coarse &b) {
                        return a.ncc < b.ncc;
                    });
                pool.insert(pos, c);
                if (pool.size() > cap)
                    pool.erase(pool.begin());
            }
            // Slide the window forward 4 samples.
            for (qint64 j = E + wCoarse + 1;
                 j <= std::min(E + wCoarse + 4, n - 1); j++)
                numer += double(x[j]) * double(x[j - L]);
            for (qint64 j = E + 1 - wCoarse; j <= E + 4 - wCoarse; j++)
                numer -= double(x[j]) * double(x[j - L]);
        }
    }

    // Fine pass: full-W NCC at 1-sample resolution over ±4 per survivor,
    // then the gates and the score.
    struct Scored {
        LoopCandidate cand;
        double dRmsDb;
    };
    std::vector<Scored> scored;
    const double logLMax = std::log(double(regionLen));
    if (top.empty())
        top = std::move(fallback);
    for (const Coarse &c : top) {
        qint64 bestE = -1;
        double bestNcc = -2.0;
        for (qint64 E = c.E - 4; E <= c.E + 4; E++) {
            const qint64 S = E + 1 - c.L;
            if (S < W || E > n - 1 - W || E < S + 15)
                continue;
            const double ncc = floatSeamNcc(x, n, S, E, W);
            if (ncc > bestNcc) {
                bestNcc = ncc;
                bestE = E;
            }
        }
        if (bestE < 0)
            continue;
        const qint64 S = bestE + 1 - c.L;
        // Level-match gate: seam windows within 1.5 dB.
        const double aa = energy(bestE + 1 - W, bestE + W + 1);
        const double bb = energy(S - W, S + W);
        if (aa <= 0.0 || bb <= 0.0)
            continue;
        const double dRmsDb = std::abs(10.0 * std::log10(aa / bb));
        // Anti-pump gate: the loop's first vs last 10% RMS within 1.0 dB.
        const qint64 tenth = std::max<qint64>(1, c.L / 10);
        const double head = energy(S, S + tenth);
        const double tail = energy(bestE + 1 - tenth, bestE + 1);
        const bool gates = dRmsDb <= 1.5 && head > 0.0 && tail > 0.0
            && std::abs(10.0 * std::log10(head / tail)) <= 1.0;
        LoopCandidate cand;
        cand.loopStart = S;
        cand.loopEnd = bestE;
        cand.ncc = bestNcc;
        cand.passedGates = gates;
        cand.score = 0.6 * bestNcc
            + 0.2 * (1.0 - std::min(dRmsDb, 1.5) / 1.5)
            + 0.2 * (logLMax > 0.0 ? std::log(double(c.L)) / logLMax : 0.0);
        scored.push_back({cand, dRmsDb});
    }

    // Gate-passing candidates first, best score first; dedupe candidates
    // whose S and E both sit within P/2 of a better one.
    std::sort(scored.begin(), scored.end(),
              [](const Scored &a, const Scored &b) {
                  if (a.cand.passedGates != b.cand.passedGates)
                      return a.cand.passedGates;
                  return a.cand.score > b.cand.score;
              });
    const qint64 dedupe = pitched
        ? std::max<qint64>(qint64(std::llround(period / 2.0)), 8)
        : 64;
    for (const Scored &s : scored) {
        bool duplicate = false;
        for (const LoopCandidate &kept : out) {
            if (std::llabs(kept.loopStart - s.cand.loopStart) <= dedupe
                && std::llabs(kept.loopEnd - s.cand.loopEnd) <= dedupe) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            out.push_back(s.cand);
            if (out.size() >= 5)
                break;
        }
    }
    return out;
}

void refineLoop(const float *x, qint64 n, double period, qint64 *loopStart,
                qint64 *loopEnd)
{
    if (!x || !loopStart || !loopEnd || n < 32)
        return;
    const qint64 W = period > 0.0
        ? qBound<qint64>(qint64(128), qint64(std::llround(2.0 * period)),
                       qint64(512))
        : 128;
    qint64 bestS = *loopStart, bestE = *loopEnd;
    double bestNcc = -2.0;
    for (qint64 dS = -8; dS <= 8; dS++) {
        for (qint64 dE = -8; dE <= 8; dE++) {
            const qint64 S = *loopStart + dS;
            const qint64 E = *loopEnd + dE;
            if (S < 1 || E > n - 2 || E < S + 15)
                continue;
            const double ncc = floatSeamNcc(x, n, S, E, W);
            if (ncc > bestNcc) {
                bestNcc = ncc;
                bestS = S;
                bestE = E;
            }
        }
    }
    if (bestNcc > -2.0) {
        *loopStart = bestS;
        *loopEnd = bestE;
    }
}

SeamMetrics seamMetricsAt(const qint8 *s8, qint64 n, qint64 loopStart,
                          qint64 loopEnd)
{
    SeamMetrics seam;
    const qint64 S = loopStart, E = loopEnd;
    const qint64 loopLen = E + 1 - S;
    if (!s8 || loopLen < 2 || S < 0 || E >= n || S + 1 > E)
        return seam;
    // Playback order across the wrap is … s[E−1], s[E], s[S], s[S+1] …
    // Indices past E read real tail samples when the buffer has them
    // (candidate evaluation), else wrap into the loop (the exported shape).
    const auto at = [&](qint64 i) {
        if (i >= n)
            i = S + (i - S) % loopLen;
        return double(s8[i]);
    };
    seam.valid = true;
    seam.ampLsb = int(std::abs(at(S) - (at(E) + (at(E) - at(E - 1)))));
    seam.derivLsb = int(std::abs((at(S + 1) - at(S)) - (at(E) - at(E - 1))));

    qint64 W = std::min<qint64>(128, loopLen / 2);
    W = std::min(W, S); // the pre-loop-start window needs S − W ≥ 0
    if (W >= 2) {
        double ab = 0.0, aa = 0.0, bb = 0.0;
        for (qint64 i = 0; i < 2 * W; i++) {
            const double a = at(E + 1 - W + i);
            const double b = at(S - W + i);
            ab += a * b;
            aa += a * a;
            bb += b * b;
        }
        if (aa > 0.0 && bb > 0.0) {
            seam.ncc = ab / std::sqrt(aa * bb);
            seam.nccValid = true;
        }
    }
    return seam;
}

void PeakPyramid::build(const float *x, qint64 n)
{
    levels.clear();
    frameCount = n;
    if (!x || n <= 0)
        return;
    qint64 block = kBlock;
    while (true) {
        const qint64 buckets = (n + block - 1) / block;
        std::vector<MinMax> level(static_cast<size_t>(buckets));
        if (levels.empty()) {
            for (qint64 b = 0; b < buckets; b++) {
                const qint64 from = b * block;
                const qint64 to = std::min(n, from + block);
                float lo = x[from], hi = x[from];
                for (qint64 i = from + 1; i < to; i++) {
                    lo = std::min(lo, x[i]);
                    hi = std::max(hi, x[i]);
                }
                level[size_t(b)] = {lo, hi};
            }
        } else {
            const std::vector<MinMax> &prev = levels.back();
            for (qint64 b = 0; b < buckets; b++) {
                const size_t from = size_t(b) * kBlock;
                const size_t to =
                    std::min(prev.size(), from + size_t(kBlock));
                MinMax m = prev[from];
                for (size_t i = from + 1; i < to; i++) {
                    m.lo = std::min(m.lo, prev[i].lo);
                    m.hi = std::max(m.hi, prev[i].hi);
                }
                level[size_t(b)] = m;
            }
        }
        levels.push_back(std::move(level));
        if (buckets <= kBlock)
            break;
        block *= kBlock;
    }
}

PeakPyramid::MinMax PeakPyramid::query(const float *x, qint64 from,
                                       qint64 to) const
{
    from = std::max<qint64>(0, from);
    to = std::min(to, frameCount);
    if (!x || from >= to)
        return {};
    // Pick the deepest level whose block still subdivides the span, then
    // stitch the partial blocks at the edges from raw samples.
    int lvl = -1;
    qint64 block = kBlock;
    for (size_t k = 0; k < levels.size(); k++) {
        if (block * 4 > to - from)
            break;
        lvl = int(k);
        block *= kBlock;
    }
    MinMax m{x[from], x[from]};
    const auto fold = [&m](float lo, float hi) {
        m.lo = std::min(m.lo, lo);
        m.hi = std::max(m.hi, hi);
    };
    if (lvl < 0) {
        for (qint64 i = from; i < to; i++)
            fold(x[i], x[i]);
        return m;
    }
    qint64 lvlBlock = kBlock;
    for (int k = 0; k < lvl; k++)
        lvlBlock *= kBlock;
    const qint64 bFirst = (from + lvlBlock - 1) / lvlBlock;
    const qint64 bLast = to / lvlBlock; // exclusive bucket bound
    for (qint64 i = from; i < std::min(to, bFirst * lvlBlock); i++)
        fold(x[i], x[i]);
    const std::vector<MinMax> &level = levels[size_t(lvl)];
    for (qint64 b = bFirst; b < bLast; b++)
        fold(level[size_t(b)].lo, level[size_t(b)].hi);
    for (qint64 i = std::max(from, bLast * lvlBlock); i < to; i++)
        fold(x[i], x[i]);
    return m;
}

} // namespace SampleDsp
