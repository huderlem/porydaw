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
// Loop behavior matches the GBA's GOTO command (and poryaaaa_render's event
// expansion): events at the loop end play before the wrap, then events at the
// loop start replay; events positioned after the loop end are unreachable
// while looping.
class TimelinePlayer
{
public:
    void reset()
    {
        m_pos = 0;
        m_cursor = 0;
    }

    uint64_t position() const { return m_pos; }

    // Renders exactly `frames` samples into outL/outR, dispatching every due
    // event. Note-ons for tracks set in muteMask are skipped (note-offs and
    // controllers always pass so state stays consistent).
    void render(M4AEngine *engine, const MidiTimeline *timeline, float *outL, float *outR,
                uint32_t frames, bool looping, uint32_t muteMask);

private:
    static void dispatchEvent(M4AEngine *engine, const TimelineEvent &ev, uint32_t muteMask);

    uint64_t m_pos = 0;
    size_t m_cursor = 0;
};
