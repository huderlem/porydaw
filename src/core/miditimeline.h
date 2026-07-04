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

    bool hasLoop() const
    {
        return loopStartSample != UINT64_MAX && loopEndSample != UINT64_MAX
            && loopEndSample > loopStartSample;
    }
};
