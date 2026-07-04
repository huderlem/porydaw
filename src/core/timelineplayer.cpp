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

void TimelinePlayer::chase(M4AEngine *engine, const MidiTimeline *timeline, uint64_t pos)
{
    // Most recent state-bearing event per slot at or before pos.
    const TimelineEvent *cc[16][128] = {};
    const TimelineEvent *bend[16] = {};
    const TimelineEvent *program[16] = {};
    const TimelineEvent *tempo = nullptr;

    for (const TimelineEvent &ev : timeline->events) {
        if (ev.samplePos > pos)
            break;
        switch (ev.type) {
        case 0xB: cc[ev.track][ev.data0 & 0x7F] = &ev; break;
        case 0xC: program[ev.track] = &ev; break;
        case 0xE: bend[ev.track] = &ev; break;
        case TIMELINE_EVT_TEMPO: tempo = &ev; break;
        }
    }

    // Engine reset defaults for the stateful controllers (the track-init
    // values in m4a_engine.c). The opt-in ones — portamento, PWM — are
    // no-ops in the engine while their feature is disabled.
    static constexpr struct { uint8_t cc; uint8_t value; } kCcDefaults[] = {
        {0x01, 0},   // MOD
        {0x05, 0},   // PORTAMENTO
        {0x07, 127}, // VOL
        {0x0A, 64},  // PAN (center)
        {0x14, 2},   // BENDR
        {0x15, 22},  // LFOS
        {0x17, 0},   // PWMC
        {0x19, 0},   // PWMS
    };

    for (int track = 0; track < 16; track++) {
        if (program[track])
            dispatchEvent(engine, *program[track], 0);
        for (int n = 0; n < 128; n++)
            if (cc[track][n])
                dispatchEvent(engine, *cc[track][n], 0);
        for (const auto &d : kCcDefaults)
            if (!cc[track][d.cc])
                m4a_engine_cc(engine, track, d.cc, d.value);
        if (bend[track])
            dispatchEvent(engine, *bend[track], 0);
        else
            m4a_engine_pitch_bend(engine, track, 0);
    }
    if (tempo)
        dispatchEvent(engine, *tempo, 0);
    else
        m4a_engine_set_tempo_bpm(engine, 150); // engine reset default
}

void TimelinePlayer::seek(uint64_t pos, const MidiTimeline *timeline)
{
    m_pos = pos;
    m_cursor = size_t(std::lower_bound(timeline->events.begin(), timeline->events.end(), pos,
                                       [](const TimelineEvent &e, uint64_t p) {
                                           return e.samplePos < p;
                                       })
                      - timeline->events.begin());
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
