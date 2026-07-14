#pragma once

#include <cstdint>

#include "core/miditimeline.h"

extern "C" {
#include "m4a_engine.h"
}

// Walks a MidiTimeline, dispatching events to an M4AEngine and rendering
// audio between them. Shared by the real-time audio callback (AudioEngine)
// and the offline render CLI so both play back identically.
//
// Loop behavior matches the GBA's GOTO command: events at the loop end play
// before the wrap, then events at the loop start replay; events positioned
// after the loop end are unreachable while looping. Notes are the exception:
// a note starting at the loop end does not sound (mid2agb orders same-tick
// events so the GOTO lands after note-ends but before note-starts), and any
// note still keyed on at the wrap is released — its note-off lies beyond the
// loop end, so it would otherwise be held forever.
class TimelinePlayer
{
public:
    void reset()
    {
        m_pos = 0;
        m_cursor = 0;
        clearKeyedOn();
    }

    uint64_t position() const { return m_pos; }

    // Repositions onto (a possibly different) timeline without disturbing the
    // engine: keeps the sample position and finds the matching event cursor.
    // Used when an edited document swaps in a rebuilt timeline mid-song.
    void seek(uint64_t pos, const MidiTimeline *timeline);

    // Chases controller state to `pos`: dispatches each track's most recent
    // program/CC/bend/tempo value at or before pos, and the engine's reset
    // default for stateful parameters with no event there — leaving the
    // engine as if this timeline had played linearly from the start. Pair
    // with seek() when swapping in an edited timeline (or, later, jumping
    // the playhead) so deleted or moved automation can't stay latched.
    static void chase(M4AEngine *engine, const MidiTimeline *timeline, uint64_t pos);

    // Renders exactly `frames` samples into outL/outR, dispatching every due
    // event. Note-ons for tracks set in muteMask are skipped (note-offs and
    // controllers always pass so state stays consistent).
    void render(M4AEngine *engine, const MidiTimeline *timeline, float *outL, float *outR,
                uint32_t frames, bool looping, uint32_t muteMask);

private:
    static void dispatchEvent(M4AEngine *engine, const TimelineEvent &ev, uint32_t muteMask);

    void clearKeyedOn()
    {
        for (auto &track : m_keyedOn)
            for (bool &key : track)
                key = false;
    }

    uint64_t m_pos = 0;
    size_t m_cursor = 0;
    // Notes keyed on with no note-off dispatched yet, so the loop wrap can
    // release the ones whose note-off lies beyond the loop end.
    bool m_keyedOn[16][128] = {};
};
