#pragma once

#include <QString>
#include <cstdint>
#include <memory>
#include <vector>

// Event type codes: MIDI status nibbles 0x8 (note off), 0x9 (note on),
// 0xB (CC), 0xC (program change), 0xE (pitch bend), plus a synthetic tempo
// change. Real channel-voice events use nibbles 0x8..0xE, so 0x1 is free.
// A tempo event carries the target BPM as a 14-bit value across data0 (low
// 7 bits) and data1 (high 7 bits).
constexpr uint8_t TIMELINE_EVT_TEMPO = 0x1;

struct TimelineEvent {
    uint64_t samplePos;
    uint32_t tick;  // absolute SMF tick (viewer grid position)
    uint8_t type;
    uint8_t track;  // engine track index (0-15), already mapped from SMF track/channel
    uint8_t data0;
    uint8_t data1;
};

struct TimelineTrack {
    QString name;      // from SMF track name meta event, if any
    bool used = false; // has at least one channel event
    int noteCount = 0;
    int firstProgram = -1; // first program change seen, -1 if none
};

// Tempo map entry (viewer data; playback uses the merged TIMELINE_EVT_TEMPO
// events). The map always has an entry at tick 0.
struct TempoPoint {
    uint64_t tick;
    uint64_t samplePos;
    double bpm;
};

// Time signature change from SMF meta 0x58 (viewer grid data; 4/4 assumed
// when absent).
struct TimeSigPoint {
    uint64_t tick;
    uint8_t numerator;
    uint8_t denomPow2; // denominator = 1 << denomPow2
};

// Anything parsed out of the file that playback ignores (aftertouch, channel
// pressure, sysex, non-loop text metas, ...). Kept so the viewer can show
// every event: nothing in the file is silently invisible.
struct OtherEvent {
    uint64_t tick;
    uint64_t samplePos;
    int track; // engine track (0-15), or -1 for file-level metas
    QString label;
};

// An immutable, sample-positioned event timeline parsed from a Standard MIDI
// File. Sample positions are computed against a fixed sample rate using the
// file's tempo map (following poryaaaa_render's approach). Once built, the
// timeline is read-only and safe to hand to the audio thread.
//
// Engine track mapping: for SMF Type 1 files, each MTrk chunk that contains
// channel events gets its own engine track (0-15) in file order, so tracks
// sharing a MIDI channel stay separate (mid2agb semantics). Chunks with no
// channel events (e.g. a conductor track) don't consume a slot. For Type 0,
// the MIDI channel is the engine track.
class MidiTimeline
{
public:
    static std::unique_ptr<MidiTimeline> load(const QString &path, double sampleRate,
                                              QString *error);

    std::vector<TimelineEvent> events; // sorted by samplePos
    TimelineTrack tracks[16];
    int usedTrackCount = 0;

    double sampleRate = 0.0;
    uint64_t lengthSamples = 0; // sample position of the last event
    uint64_t loopStartSample = UINT64_MAX;
    uint64_t loopEndSample = UINT64_MAX;
    int droppedTracks = 0; // SMF tracks beyond the 16 engine tracks

    // Viewer data (never touched by the audio thread).
    uint32_t ticksPerBeat = 24;
    uint64_t lengthTicks = 0;
    uint64_t loopStartTick = UINT64_MAX;
    uint64_t loopEndTick = UINT64_MAX;
    std::vector<TempoPoint> tempoMap;   // sorted by tick, first entry at tick 0
    std::vector<TimeSigPoint> timeSigs; // sorted by tick, may be empty
    std::vector<OtherEvent> otherEvents; // sorted by tick

    bool hasLoop() const
    {
        return loopStartSample != UINT64_MAX && loopEndSample != UINT64_MAX
            && loopEndSample > loopStartSample;
    }

    // Tick <-> sample conversion through the tempo map (viewer/UI thread).
    uint64_t sampleForTick(uint64_t tick) const;
    double tickForSample(uint64_t samplePos) const;
};
