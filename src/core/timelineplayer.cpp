#include "core/timelineplayer.h"

#include <algorithm>

void TimelinePlayer::dispatchEvent(M4AEngine *engine, const TimelineEvent &ev,
                                   uint32_t muteMask)
{
    switch (ev.type) {
    case TIMELINE_EVT_TEMPO:
        // Drives the engine's LFO/vibrato rate.
        m4a_engine_set_tempo_bpm(engine, double((int(ev.data1) << 7) | ev.data0));
        break;
    case 0x8:
        m4a_engine_note_off(engine, ev.track, ev.data0);
        break;
    case 0x9:
        if (!((muteMask >> ev.track) & 1))
            m4a_engine_note_on(engine, ev.track, ev.data0, ev.data1);
        break;
    case 0xB:
        m4a_engine_cc(engine, ev.track, ev.data0, ev.data1);
        break;
    case 0xC:
        m4a_engine_program_change(engine, ev.track, ev.data0);
        break;
    case 0xE:
        m4a_engine_pitch_bend(engine, ev.track,
                              int16_t(((int(ev.data1) << 7) | ev.data0) - 8192));
        break;
    }
}

void TimelinePlayer::render(M4AEngine *engine, const MidiTimeline *timeline, float *outL,
                            float *outR, uint32_t frames, bool looping, uint32_t muteMask)
{
    const bool loop = looping && timeline->hasLoop();
    uint32_t done = 0;

    while (done < frames) {
        // Dispatch every event due at the current position, wrapping the loop
        // as needed.
        for (;;) {
            while (m_cursor < timeline->events.size()
                   && timeline->events[m_cursor].samplePos <= m_pos) {
                dispatchEvent(engine, timeline->events[m_cursor], muteMask);
                m_cursor++;
            }
            if (loop && m_pos >= timeline->loopEndSample) {
                m_pos = timeline->loopStartSample;
                m_cursor = size_t(std::lower_bound(timeline->events.begin(),
                                                   timeline->events.end(), m_pos,
                                                   [](const TimelineEvent &e, uint64_t pos) {
                                                       return e.samplePos < pos;
                                                   })
                                  - timeline->events.begin());
                continue;
            }
            break;
        }

        // Render up to the next event or the loop end.
        uint64_t next = UINT64_MAX;
        if (m_cursor < timeline->events.size())
            next = timeline->events[m_cursor].samplePos;
        if (loop)
            next = std::min(next, timeline->loopEndSample);

        uint32_t n = frames - done;
        if (next != UINT64_MAX)
            n = uint32_t(std::min<uint64_t>(n, next - m_pos));
        if (n == 0)
            n = 1; // defensive; boundaries at m_pos were handled above

        m4a_engine_process(engine, outL + done, outR + done, int(n));
        m_pos += n;
        done += n;
    }
}
