#include "core/timelineplayer.h"

#include <algorithm>

#include "core/mid2agbtables.h"

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

void TimelinePlayer::primeVoices(M4AEngine *engine, const MidiTimeline *timeline, uint64_t pos)
{
    bool chased[16] = {}; // program at or before pos: chase already applied it
    const TimelineEvent *first[16] = {};
    for (const TimelineEvent &ev : timeline->events) {
        if (ev.type != 0xC)
            continue;
        if (ev.samplePos <= pos)
            chased[ev.track] = true;
        else if (!first[ev.track])
            first[ev.track] = &ev;
    }
    for (int track = 0; track < 16; track++)
        if (!chased[track] && first[track])
            m4a_engine_program_change(engine, track, first[track]->data0);
}

void TimelinePlayer::seek(uint64_t pos, const MidiTimeline *timeline)
{
    // Both seek callers release the engine's sounding notes around the seek
    // (their note-offs may be behind the new position or gone entirely), so
    // nothing stays keyed on across it.
    clearKeyedOn();
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
            // Gate-carried releases fire before regular events at the same
            // position: on hardware the channel gate hits zero as the clock
            // advances, before the track's commands at that clock run.
            for (int i = 0; i < m_pendingOffCount;) {
                if (m_pendingOffs[i].samplePos <= m_pos) {
                    m4a_engine_note_off(engine, m_pendingOffs[i].track, m_pendingOffs[i].key);
                    m_keyedOn[m_pendingOffs[i].track][m_pendingOffs[i].key] = false;
                    m_pendingOffs[i] = m_pendingOffs[--m_pendingOffCount];
                } else {
                    i++;
                }
            }
            while (m_cursor < timeline->events.size()
                   && timeline->events[m_cursor].samplePos <= m_pos) {
                const TimelineEvent &ev = timeline->events[m_cursor];
                m_cursor++;
                // While looping, a note starting at the loop end must not
                // sound: the wrap replaces it with the loop-start events, and
                // its note-off lies beyond the loop end where playback never
                // goes (mid2agb orders the GOTO before same-tick note starts,
                // so it is unreachable on hardware too).
                if (loop && ev.type == 0x9 && ev.samplePos >= timeline->loopEndSample)
                    continue;
                if (ev.type == 0x9 && !((muteMask >> ev.track) & 1)) {
                    m_keyedOn[ev.track][ev.data0 & 0x7F] = true;
                    m_keyedOnTick[ev.track][ev.data0 & 0x7F] = ev.tick;
                } else if (ev.type == 0x8) {
                    m_keyedOn[ev.track][ev.data0 & 0x7F] = false;
                }
                dispatchEvent(engine, ev, muteMask);
            }
            if (loop && m_pos >= timeline->loopEndSample) {
                wrapNotes(engine, timeline);
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

        // Render up to the next event, pending release, or the loop end.
        uint64_t next = UINT64_MAX;
        if (m_cursor < timeline->events.size())
            next = timeline->events[m_cursor].samplePos;
        for (int i = 0; i < m_pendingOffCount; i++)
            next = std::min(next, m_pendingOffs[i].samplePos);
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

// Applies mid2agb's note semantics at the loop wrap. A note still keyed on
// here crosses the loop end: its note-off lies beyond it, where looping
// playback never goes. mid2agb emits notes of <= 96 clocks as direct note
// commands whose gate lives on the sound channel — the GOTO doesn't touch
// channel state, so on hardware the note keeps sounding and releases at its
// written duration, now at the wrapped position (gate-carry). Longer notes
// become TIE + EOT; the EOT is beyond the loop end and unreachable, so the
// note is held forever, a fresh instance stacking every pass.
void TimelinePlayer::wrapNotes(M4AEngine *engine, const MidiTimeline *timeline)
{
    // A gate-carried release this pass never reached keeps counting through
    // the wrap, like the channel gate it models.
    for (int i = 0; i < m_pendingOffCount; i++) {
        PendingOff &p = m_pendingOffs[i];
        p.tick = timeline->loopStartTick + (p.tick - timeline->loopEndTick);
        p.samplePos = timeline->sampleForTick(p.tick);
    }

    for (int track = 0; track < 16; track++) {
        for (int key = 0; key < 128; key++) {
            if (!m_keyedOn[track][key])
                continue;
            bool carried = false;
            for (int i = 0; i < m_pendingOffCount && !carried; i++)
                carried = m_pendingOffs[i].track == track && m_pendingOffs[i].key == key;
            if (carried)
                continue;

            // The note's off event, somewhere beyond the loop end (first
            // matching off, which is also how mid2agb pairs notes).
            const TimelineEvent *off = nullptr;
            for (size_t i = m_cursor; i < timeline->events.size(); i++) {
                const TimelineEvent &e = timeline->events[i];
                if (e.type == 0x8 && e.track == track && (e.data0 & 0x7F) == key) {
                    off = &e;
                    break;
                }
            }
            if (off) {
                // exactGate=true: only the raw clock count matters here — the
                // duration LUT never moves a note across the 96-clock line.
                const int clocks = mid2agbEffectiveDuration(
                    int64_t(off->tick) - int64_t(m_keyedOnTick[track][key]),
                    timeline->ticksPerBeat, timeline->extendedClocks, true);
                if (clocks > 96)
                    continue; // TIE + EOT: the EOT is unreachable — held forever
                if (m_pendingOffCount < kMaxPendingOffs) {
                    const uint64_t tick =
                        timeline->loopStartTick + (uint64_t(off->tick) - timeline->loopEndTick);
                    m_pendingOffs[m_pendingOffCount++] = {timeline->sampleForTick(tick), tick,
                                                          uint8_t(track), uint8_t(key)};
                    continue;
                }
            }
            // No note-off in the file (or no room to carry one): release now.
            m4a_engine_note_off(engine, track, uint8_t(key));
            m_keyedOn[track][key] = false;
        }
    }
}
