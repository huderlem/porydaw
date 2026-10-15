#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// The metronome's beat grid (docs/metronome/PLAN.md stance 1): every beat
// line of the song's meter, positioned in samples through the tempo map
// exactly as MidiTimeline::build positions events, so a click and a note on
// the same tick land on the same sample. Built once on the UI thread with
// the timeline and immutable after, so the audio thread reads it lock-free
// like the timeline's events. Lookups never allocate.
class ClickGrid
{
  public:
    // A tempo change from the first MTrk (the SMF's 0x51 metas), sorted by
    // tick as build() sorts them.
    struct Tempo {
        uint64_t tick;
        uint32_t usPerBeat;
    };
    // One meter: a beat line every beatTicks from tick, and every
    // beatsPerBar-th beat line, counting from the one at tick, a bar line
    // (MidiTimeline::TimeSigSegment's grid: lines restart at every meter
    // change).
    struct Meter {
        uint64_t tick;
        uint64_t beatTicks;
        int beatsPerBar;
    };
    struct Click {
        uint64_t sample;
        bool accent; // the first beat of a bar
    };

    // Empty: no clicks anywhere.
    ClickGrid() = default;
    // meters sorted by tick, the first at tick 0.
    ClickGrid(const std::vector<Tempo> &tempos, std::vector<Meter> meters, uint32_t ticksPerBeat,
              double sampleRate);

    bool empty() const { return m_meters.empty(); }

    // The sample of a tick, bit-identical to build()'s tickToSample.
    uint64_t sampleAt(uint64_t tick) const;

    // The beat lines with samples in [from, to), in order, at most cap of
    // them; returns how many were written.
    size_t clicksIn(uint64_t from, uint64_t to, Click *out, size_t cap) const;

  private:
    // tickToSample's running state after each tempo change: a tick t is
    // positioned from the last point with tick < t (the first point, tick
    // 0 at the SMF's default 120 BPM, for t == 0).
    struct TempoPoint {
        uint64_t tick;
        double samples;
        double usPerBeat;
    };
    std::vector<TempoPoint> m_tempo;
    std::vector<Meter> m_meters;
    double m_ticksPerBeat = 1.0;
    double m_sampleRate = 0.0;
};
