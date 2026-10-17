#pragma once

#include <cstdint>

#include "core/miditimeline.h"

extern "C" {
#include "m4a_engine.h"
}

// The lanes a MIDI keyboard moved live on one track
// (docs/midi-controls/PLAN.md §3.2): its pitch bend, any CC lane, and its
// voice (a program change). TimelinePlayer::chaseTouched hands exactly these
// back to the song. Plain data: the audio thread keeps one.
struct TouchedLanes {
    uint64_t cc[2] = {0, 0}; // CC n is bit (n & 63) of cc[n >> 6]
    bool bend = false;
    bool program = false;

    void setCc(uint8_t n) { cc[(n >> 6) & 1] |= uint64_t(1) << (n & 63); }
    bool hasCc(uint8_t n) const { return (cc[(n >> 6) & 1] >> (n & 63)) & 1; }
    bool any() const { return cc[0] || cc[1] || bend || program; }
};

// The song lanes a recording take holds on its track
// (docs/midi-controls/PLAN.md §3.4, D3): once the keyboard moves a lane while
// a take is armed and the move sounds, the song's own events in that lane on
// that track stop playing, so what you hear is what gets committed.
// TimelinePlayer::render skips them; the latch ends with the pass (a loop
// wrap, see render). Notes, other lanes and other tracks are never held.
// Plain data: the audio thread keeps one; offline paths never pass one.
struct LaneLatch {
    int track = -1;
    TouchedLanes lanes;

    bool any() const { return track >= 0 && lanes.any(); }
    // Whether ev is a song event this latch holds back.
    bool holds(const TimelineEvent &ev) const
    {
        if (ev.track != track)
            return false;
        switch (ev.type) {
        case 0xB:
            return lanes.hasCc(ev.data0 & 0x7F);
        case 0xC:
            return lanes.program;
        case 0xE:
            return lanes.bend;
        default:
            return false;
        }
    }
};

// A punch take's silenced notes (docs/punch/PLAN.md P9): while a punch
// take records, the song's notes on its track that start inside the punch
// range don't play (a playback-only mask; the document is untouched until
// the take commits), and the track's notes still sounding when playback
// reaches the range's start are released there, as the commit truncates
// them. The release happens once per pass (cut), re-armed whenever the
// player is before inSample again (a loop wrap or a seek back), and takes
// only notes that started before outTick (what the commit truncates; a TIE
// from after the range carried across a loop wrap keeps sounding) with
// their gate-carried releases. A key the keyboard holds on the track
// (held, filled by the caller before each render) is not released: the
// engine stops channels by track+key, so its note-off would silence the
// player's own note too; the song's note on that key is let go instead
// (no longer keyed on), and ends with the keyboard's release. A song
// note-off on the track for a key the player hasn't keyed on is dropped
// too: it belongs to a silenced (or released) note, and would otherwise
// cut the keyboard's own note on that key. Plain data: the audio thread
// keeps one; offline paths never pass one.
struct NoteMask {
    int track = -1;
    uint64_t inTick = 0, outTick = 0;     // note-ons in [inTick, outTick) are silent
    uint64_t inSample = 0, outSample = 0; // the same range on the timeline
    bool cut = false;                     // this pass released the track at inSample
    uint64_t held[2] = {0, 0};            // keys the keyboard holds on track

    bool any() const { return track >= 0 && outTick > inTick; }
    bool silences(const TimelineEvent &ev) const
    {
        return ev.type == 0x9 && ev.track == track && ev.tick >= inTick && ev.tick < outTick;
    }
    bool holds(int key) const { return (held[(key >> 6) & 1] >> (key & 63)) & 1; }
};

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
    // Loop wraps rendered so far, ever (never reset): a caller compares it
    // across render() to see the playhead jump back to the loop start.
    uint32_t wrapCount() const { return m_wraps; }

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

    // The narrow version for the lanes a MIDI keyboard moved live on one
    // track (docs/midi-controls/PLAN.md §3.2): each touched CC and the bend
    // get their value at or before pos, or the engine's track-init default
    // (ccDefault; bend centred) when the song has none. A touched voice gets
    // the program at or before pos, or else the track's first one later in
    // the song, as primeVoices would give it, so the track isn't left on the
    // keyboard's voice until playback reaches its own; a track with no
    // program change anywhere keeps the one it has (no event can stand for
    // an unset voice). Every other lane, and every other track, is left
    // exactly as it is, so playback carries on undisturbed.
    static void chaseTouched(M4AEngine *engine, const MidiTimeline *timeline, uint64_t pos,
                             int track, const TouchedLanes &touched);

    // The engine's track-init value for a stateful CC (m4a_engine.c), as
    // chase() restores it where the song has no event; -1 for a CC with no
    // state to restore.
    static int ccDefault(uint8_t cc);

    // Primes voices for auditioning: each track with no program change at or
    // before `pos` gets its first program change from later in the timeline,
    // so notes previewed outside playback (piano-key clicks, note-draw) sound
    // before the track's voice event has ever been dispatched. Call after
    // chase() — a chase-applied program is never overridden — and only for
    // the interactive engine: playback re-dispatches the real event when it
    // reaches it, but an offline render must keep the engine's authentic
    // uninitialized-track silence.
    static void primeVoices(M4AEngine *engine, const MidiTimeline *timeline, uint64_t pos);

    // Keys an audition note at a chosen track VOL and PAN. The engine's track
    // holds the values chased to the playhead, but an auditioned note belongs
    // where it sits in the song, not where the cursor does: rawVolume (0-127,
    // before the song's master volume; negative leaves the track's own VOL
    // alone) and pan (-64..63; anything outside leaves the track's own PAN
    // alone) ride on the channel the note-on starts, so the audition keeps
    // them for its whole life while every other note on the track is
    // unaffected. Both matter to what is heard: PAN places the sound and, on
    // a CGB channel, moves the envelope goal the note sounds at.
    static void auditionNoteOn(M4AEngine *engine, int track, uint8_t key, uint8_t velocity,
                               int rawVolume, int pan);

    // Renders exactly `frames` samples into outL/outR, dispatching every due
    // event. Note-ons for tracks set in muteMask are skipped (note-offs and
    // controllers always pass so state stays consistent).
    //
    // `latch` (live recording only; null everywhere else, which costs one
    // pointer test per event): the song events it holds are skipped. A loop
    // wrap ends it, before the loop start's events dispatch: its lanes are
    // chased to the song's state at the loop end (chaseTouched, the loop
    // end's own events included: what an unlatched pass carries into the
    // next) and it is cleared in place, so the loop start's events apply on
    // top and the next pass plays the song's own events until the keyboard
    // latches a lane again (J2).
    //
    // `mask` (a punch take only; null everywhere else): the notes it silences
    // are skipped, and its track is released where the range starts
    // (NoteMask); a render stops at mask->inSample so that lands exactly.
    void render(M4AEngine *engine, const MidiTimeline *timeline, float *outL, float *outR,
                uint32_t frames, bool looping, uint32_t muteMask, LaneLatch *latch = nullptr,
                NoteMask *mask = nullptr);

    // A position at or past the loop end (a start or seek there, or the
    // last render ending exactly on it) wraps before the next render plays
    // a sample: does that wrap now, exactly as render() would first, so a
    // caller reading position() sees where the sound will come from. A
    // no-op otherwise.
    void settleWrap(M4AEngine *engine, const MidiTimeline *timeline, bool looping,
                    uint32_t muteMask, LaneLatch *latch = nullptr, NoteMask *mask = nullptr);

  private:
    static void dispatchEvent(M4AEngine *engine, const TimelineEvent &ev, uint32_t muteMask);
    // Dispatches every event (and gate-carried release) due at the
    // position, wrapping the loop as needed; a latch and a mask as in render.
    void dispatchDue(M4AEngine *engine, const MidiTimeline *timeline, bool loop, uint32_t muteMask,
                     LaneLatch *latch, NoteMask *mask);
    // The mask's range starts here: its track's song notes release.
    void cutMaskedTrack(M4AEngine *engine, const NoteMask &mask);

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
    uint32_t m_wraps = 0;
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
