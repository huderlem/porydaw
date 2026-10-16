#include "clickgrid.h"

#include <algorithm>

ClickGrid::ClickGrid(const std::vector<Tempo> &tempos, std::vector<Meter> meters,
                     uint32_t ticksPerBeat, double sampleRate)
    : m_meters(std::move(meters))
    , m_ticksPerBeat(double(std::max<uint32_t>(ticksPerBeat, 1)))
    , m_sampleRate(sampleRate)
{
    // The same accumulation, in the same order, as tickToSample's loop.
    m_tempo.push_back({0, 0.0, 500000.0});
    for (const Tempo &tc : tempos) {
        const TempoPoint &prev = m_tempo.back();
        const double samples = prev.samples + double(tc.tick - prev.tick) * prev.usPerBeat /
                                                  m_ticksPerBeat / 1000000.0 * m_sampleRate;
        m_tempo.push_back({tc.tick, samples, double(tc.usPerBeat)});
    }
    for (Meter &m : m_meters) {
        m.beatTicks = std::max<uint64_t>(m.beatTicks, 1);
        m.beatsPerBar = std::max(m.beatsPerBar, 1);
    }
}

uint64_t ClickGrid::sampleAt(uint64_t tick) const
{
    if (m_tempo.empty()) // default-constructed: no grid, no time
        return 0;
    // The last point with tick < t; the first point for t == 0.
    auto it = std::lower_bound(m_tempo.begin(), m_tempo.end(), tick,
                               [](const TempoPoint &p, uint64_t t) { return p.tick < t; });
    const TempoPoint &p = it == m_tempo.begin() ? *it : *(it - 1);
    const double samples =
        p.samples + double(tick - p.tick) * p.usPerBeat / m_ticksPerBeat / 1000000.0 * m_sampleRate;
    return static_cast<uint64_t>(samples + 0.5);
}

ClickGrid::CountIn ClickGrid::countIn(uint64_t cursorTick, int bars) const
{
    CountIn ci;
    if (empty() || bars <= 0)
        return ci;
    // The tempo and meter the song starts in at the cursor.
    const auto tempo =
        std::upper_bound(m_tempo.begin(), m_tempo.end(), cursorTick,
                         [](uint64_t t, const TempoPoint &p) { return t < p.tick; }) -
        1;
    const auto meter = std::upper_bound(m_meters.begin(), m_meters.end(), cursorTick,
                                        [](uint64_t t, const Meter &m) { return t < m.tick; }) -
                       1;
    ci.cursorTick = cursorTick;
    ci.samplesPerTick = tempo->usPerBeat / m_ticksPerBeat / 1000000.0 * m_sampleRate;
    ci.gridTick = meter->tick;
    ci.beatTicks = meter->beatTicks;
    ci.beatsPerBar = meter->beatsPerBar;
    ci.lengthTicks = uint64_t(bars) * meter->beatTicks * uint64_t(meter->beatsPerBar);
    ci.lengthSamples = uint64_t(double(ci.lengthTicks) * ci.samplesPerTick + 0.5);
    return ci;
}

size_t ClickGrid::countInClicksIn(const CountIn &ci, uint64_t from, uint64_t to, Click *out,
                                  size_t cap)
{
    if (ci.lengthSamples == 0 || from >= to || cap == 0)
        return 0;
    const auto mod = [](int64_t a, int64_t m) { return ((a % m) + m) % m; };
    const int64_t bt = int64_t(ci.beatTicks);
    const int64_t bar = bt * ci.beatsPerBar;
    // Count-in tick u in [0, lengthTicks) is grid tick r = start + u, the
    // count-in starting `lengthTicks` before the cursor.
    const int64_t start = int64_t(ci.cursorTick - ci.gridTick) - int64_t(ci.lengthTicks);
    size_t n = 0;
    for (int64_t u = mod(-start, bt); u < int64_t(ci.lengthTicks); u += bt) {
        // Positioned back from the end, which is the cursor's sample.
        const uint64_t back =
            uint64_t(double(int64_t(ci.lengthTicks) - u) * ci.samplesPerTick + 0.5);
        const uint64_t s = ci.lengthSamples - std::min(back, ci.lengthSamples);
        if (s < from)
            continue;
        if (s >= to)
            break;
        out[n++] = {s, mod(start + u, bar) == 0};
        if (n == cap)
            break;
    }
    return n;
}

size_t ClickGrid::clicksIn(uint64_t from, uint64_t to, Click *out, size_t cap) const
{
    if (empty() || from >= to || cap == 0)
        return 0;
    // The meter holding `from`: the last one starting at or before it.
    size_t i = 0;
    {
        size_t lo = 0, hi = m_meters.size();
        while (hi - lo > 1) {
            const size_t mid = (lo + hi) / 2;
            if (sampleAt(m_meters[mid].tick) <= from)
                lo = mid;
            else
                hi = mid;
        }
        i = lo;
    }
    size_t n = 0;
    for (; i < m_meters.size(); i++) {
        const Meter &m = m_meters[i];
        if (sampleAt(m.tick) >= to)
            break;
        const bool last = i + 1 == m_meters.size();
        // Beats of this meter: k in [0, count), at m.tick + k * beatTicks.
        constexpr uint64_t kUnbounded = uint64_t(1) << 40;
        const uint64_t count =
            last ? kUnbounded : (m_meters[i + 1].tick - m.tick + m.beatTicks - 1) / m.beatTicks;
        const auto beatSample = [&](uint64_t k) { return sampleAt(m.tick + k * m.beatTicks); };
        // The first beat at or after `from`.
        uint64_t hi = 1;
        while (hi < count && beatSample(hi) < from)
            hi = std::min(hi * 2, count);
        uint64_t lo = 0;
        while (lo < hi) {
            const uint64_t mid = lo + (hi - lo) / 2;
            if (beatSample(mid) < from)
                lo = mid + 1;
            else
                hi = mid;
        }
        for (uint64_t k = lo; k < count; k++) {
            const uint64_t s = beatSample(k);
            if (s >= to)
                return n;
            out[n++] = {s, k % uint64_t(m.beatsPerBar) == 0};
            if (n == cap)
                return n;
        }
    }
    return n;
}
