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
// while looping. Notes follow mid2agb's same-tick ordering (the GOTO lands
// after note-ends but before note-starts) and the hardware channel gate:
//  - a note starting at the loop end never sounds — its note-off lies beyond
//    the loop end, which looping playback never reaches;
//  - a note crossing the loop end that mid2agb emits as a direct note command
//    (<= 96 clocks) keeps its channel gate counting across the wrap — the
//    GOTO touches no channel state — so its release is rescheduled at the
//    wrapped position (gate-carry);
//  - a longer crossing note becomes TIE + EOT with the EOT beyond the loop
//    end, unreachable, so it is held forever and a fresh instance stacks
//    every pass — exactly as on hardware.
class TimelinePlayer
{
public:
    void reset()
    {
        m_pos = 0;
        m_cursor = 0;
        clearKeyedOn();
    }

    // A note-off rescheduled past a loop wrap (gate-carry), tracked in both
    // tick and sample domains so later wraps can re-wrap it through the
    // tempo map, like the channel gate it models.
    struct PendingOff {
        uint64_t samplePos;
        uint64_t tick;
        uint8_t track;
        uint8_t key;
    };

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

    // Primes voices for auditioning: each track with no program change at or
    // before `pos` gets its first program change from later in the timeline,
    // so notes previewed outside playback (piano-key clicks, note-draw) sound
    // before the track's voice event has ever been dispatched. Call after
    // chase() — a chase-applied program is never overridden — and only for
    // the interactive engine: playback re-dispatches the real event when it
    // reaches it, but an offline render must keep the engine's authentic
    // uninitialized-track silence.
    static void primeVoices(M4AEngine *engine, const MidiTimeline *timeline, uint64_t pos);

    // Renders exactly `frames` samples into outL/outR, dispatching every due
    // event. Note-ons for tracks set in muteMask are skipped (note-offs and
    // controllers always pass so state stays consistent).
    void render(M4AEngine *engine, const MidiTimeline *timeline, float *outL, float *outR,
                uint32_t frames, bool looping, uint32_t muteMask);

private:
    static void dispatchEvent(M4AEngine *engine, const TimelineEvent &ev, uint32_t muteMask);

    void wrapNotes(M4AEngine *engine, const MidiTimeline *timeline);

    void clearKeyedOn()
    {
        for (auto &track : m_keyedOn)
            for (bool &key : track)
                key = false;
        m_pendingOffCount = 0;
    }

    uint64_t m_pos = 0;
    size_t m_cursor = 0;
    // Notes keyed on with no note-off dispatched yet, and the tick each
    // started at, so the loop wrap can classify notes crossing the loop end.
    bool m_keyedOn[16][128] = {};
    uint32_t m_keyedOnTick[16][128] = {};
    // Gate-carried releases in flight. One entry per crossing (track, key)
    // per wrap; only a loop shorter than the note itself can accumulate more.
    static constexpr int kMaxPendingOffs = 128;
    PendingOff m_pendingOffs[kMaxPendingOffs];
    int m_pendingOffCount = 0;
};
