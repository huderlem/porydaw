#include <QByteArray>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

#include "core/miditimeline.h"
#include "core/smf.h"
#include "core/timelineplayer.h"

extern "C" {
#include "m4a_engine.h"
}

// --primecheck: voice-priming check for note auditioning, fully self-contained
// (no project needed). A freshly loaded engine has no instrument on any track
// until playback dispatches the first program change, so previewNote was
// silent until the song had been played a bit. TimelinePlayer::primeVoices
// fixes that by applying each track's first program change up front.
// Synthesizes a song covering the three track shapes — a voice at tick 0, a
// voice only later in the song, and no voice at all — and asserts on the
// engine's track state after chase + primeVoices, plus an end-to-end render
// proving an auditioned note is silent without priming and audible with it.

namespace {

// 24 ticks per quarter, 120 BPM at 48kHz: one tick is exactly 1000 samples.
constexpr uint32_t kDivision = 24;
constexpr double kSampleRate = 48000.0;
constexpr uint64_t kSamplesPerTick = 1000;

constexpr uint8_t kProgAtZero = 5; // engine track 0, tick 0
constexpr uint8_t kProgLater = 9;  // engine track 0, tick 96
constexpr uint8_t kProgTrack1 = 7; // engine track 1, tick 48 (its first)
constexpr uint64_t kLaterTick = 96;

// A VOL ramp on engine track 0, quiet from kLaterTick on: an audition of an
// early note taken while the cursor sits late must still sound at kVolEarly.
constexpr uint8_t kVolEarly = 127;
constexpr uint8_t kVolLate = 32;
// A PAN move on the same track, hard right early and centered from kLaterTick
// on. PAN places the sound and (on a CGB channel) moves the envelope goal, so
// an audition owes the note's pan for the same reason it owes its volume.
constexpr uint8_t kPanEarlyByte = 127; // CC10 byte
constexpr uint8_t kPanLateByte = 64;
constexpr int kPanEarly = int(kPanEarlyByte) - 64; // engine units
constexpr int kPanLate = int(kPanLateByte) - 64;

SmfEvent channelEvent(uint64_t tick, uint8_t status, uint8_t data0, uint8_t data1)
{
    SmfEvent ev;
    ev.tick = tick;
    ev.status = status;
    ev.data0 = data0;
    ev.data1 = data1;
    return ev;
}

SmfEvent metaEvent(uint64_t tick, uint8_t metaType, const QByteArray &blob)
{
    SmfEvent ev;
    ev.tick = tick;
    ev.status = 0xFF;
    ev.metaType = metaType;
    ev.blob = blob;
    return ev;
}

SmfFile buildPrimeSong()
{
    SmfFile smf;
    smf.format = 1;
    smf.division = kDivision;
    smf.tracks.resize(4);

    SmfTrack &conductor = smf.tracks[0];
    conductor.events.push_back(metaEvent(0, 0x51, QByteArray("\x07\xA1\x20", 3)));
    conductor.endTick = 192;

    // Engine track 0: voice at tick 0, replaced later — chase must win over
    // priming.
    SmfTrack &t0 = smf.tracks[1];
    t0.events.push_back(channelEvent(0, 0xC0, kProgAtZero, 0));
    t0.events.push_back(channelEvent(0, 0xB0, 7, kVolEarly));
    t0.events.push_back(channelEvent(0, 0xB0, 10, kPanEarlyByte));
    t0.events.push_back(channelEvent(0, 0x90, 60, 100));
    t0.events.push_back(channelEvent(24, 0x80, 60, 0));
    t0.events.push_back(channelEvent(kLaterTick, 0xC0, kProgLater, 0));
    t0.events.push_back(channelEvent(kLaterTick, 0xB0, 7, kVolLate));
    t0.events.push_back(channelEvent(kLaterTick, 0xB0, 10, kPanLateByte));
    t0.endTick = 192;

    // Engine track 1: first voice only at tick 48 — the priming case.
    SmfTrack &t1 = smf.tracks[2];
    t1.events.push_back(channelEvent(48, 0xC1, kProgTrack1, 0));
    t1.events.push_back(channelEvent(48, 0x91, 62, 100));
    t1.events.push_back(channelEvent(72, 0x81, 62, 0));
    t1.endTick = 192;

    // Engine track 2: notes but no voice anywhere — must stay untouched.
    SmfTrack &t2 = smf.tracks[3];
    t2.events.push_back(channelEvent(0, 0x92, 64, 100));
    t2.events.push_back(channelEvent(24, 0x82, 64, 0));
    t2.endTick = 192;

    return smf;
}

// A looped song for the recording latch (TimelinePlayer::render's LaneLatch,
// docs/midi-controls/PLAN.md §3.4): engine track 0 automates VOL, PAN, MOD,
// the bend and the voice, with a VOL event on the loop start, a PAN event
// only before it, and a VOL event on the loop end; engine track 1 a VOL.
// The loop is ticks kLatchLoopStart..kLatchLoopEnd (set on the timeline).
constexpr uint64_t kLatchLoopStart = 5;
constexpr uint64_t kLatchLoopEnd = 30;

SmfFile buildLatchSong()
{
    SmfFile smf;
    smf.format = 1;
    smf.division = kDivision;
    smf.tracks.resize(3);
    SmfTrack &conductor = smf.tracks[0];
    conductor.events.push_back(metaEvent(0, 0x51, QByteArray("\x07\xA1\x20", 3)));
    conductor.endTick = 48;

    SmfTrack &t0 = smf.tracks[1];
    t0.events.push_back(channelEvent(0, 0xC0, 5, 0));
    t0.events.push_back(channelEvent(0, 0xB0, 7, 100));
    t0.events.push_back(channelEvent(2, 0xB0, 10, 20));
    t0.events.push_back(channelEvent(kLatchLoopStart, 0xB0, 7, 90));
    t0.events.push_back(channelEvent(6, 0x90, 60, 100));
    t0.events.push_back(channelEvent(8, 0x80, 60, 0));
    t0.events.push_back(channelEvent(10, 0xB0, 7, 80));
    t0.events.push_back(channelEvent(12, 0xB0, 1, 9));
    t0.events.push_back(channelEvent(12, 0xE0, 0x00, 72)); // +1024
    t0.events.push_back(channelEvent(12, 0xC0, 9, 0));
    t0.events.push_back(channelEvent(15, 0xB0, 10, 100));
    t0.events.push_back(channelEvent(kLatchLoopEnd, 0xB0, 7, 11));
    t0.endTick = 48;

    SmfTrack &t1 = smf.tracks[2];
    t1.events.push_back(channelEvent(10, 0xB1, 7, 50));
    t1.endTick = 48;
    return smf;
}

// A minimal in-memory voicegroup: one looped PCM square wave shared by every
// program, so any applied voice has a non-null wav (an untouched track's
// zeroed voice does not).
struct TestVoicegroup {
    // 64 samples + the loader's guard byte: the interpolating mixer reads
    // one sample ahead, and voicegroup_loader duplicates the last byte past
    // the end (wd->data[size] = data[size - 1]).
    int8_t sample[65];
    WaveData wave;
    ToneData voices[128];
    // The same programs as square-1 voices: a PSG part is where the audition
    // volume matters most, and its levels are quantized differently.
    ToneData cgbVoices[128];

    TestVoicegroup()
    {
        for (int i = 0; i < 64; i++)
            sample[i] = i < 32 ? 100 : -100;
        sample[64] = sample[63];
        std::memset(&wave, 0, sizeof(wave));
        wave.status = 0xC000; // looped
        wave.freq = 8363u * 1024u;
        wave.loopStart = 0;
        wave.size = 64;
        wave.data = sample;
        std::memset(voices, 0, sizeof(voices));
        for (ToneData &v : voices) {
            v.type = VOICE_DIRECTSOUND;
            v.key = 60;
            v.wav = &wave;
            v.attack = 255;  // instant
            v.decay = 0;     // straight to sustain
            v.sustain = 255; // hold until note-off
            v.release = 165;
        }
        std::memset(cgbVoices, 0, sizeof(cgbVoices));
        for (ToneData &v : cgbVoices) {
            v.type = VOICE_SQUARE_1;
            v.key = 60;
            v.attack = 0;   // instant, in CGB envelope units
            v.decay = 0;    // straight to sustain
            v.sustain = 15; // hold until note-off
            v.release = 0;
        }
    }
};

// Per-side peaks: pan is half of what an audition owes the note, and a mono
// peak cannot see it move.
struct StereoPeak {
    float l = 0.0f;
    float r = 0.0f;
    float total() const { return l + r; }
    bool sameAs(const StereoPeak &other) const
    {
        return std::fabs(l - other.l) <= 1e-6f && std::fabs(r - other.r) <= 1e-6f;
    }
};

StereoPeak renderPeaks(M4AEngine *engine)
{
    constexpr uint32_t kFrames = 512;
    float bufL[kFrames], bufR[kFrames];
    m4a_engine_process(engine, bufL, bufR, int(kFrames));
    StereoPeak peak;
    for (uint32_t i = 0; i < kFrames; i++) {
        peak.l = std::max(peak.l, std::fabs(bufL[i]));
        peak.r = std::max(peak.r, std::fabs(bufR[i]));
    }
    return peak;
}

float renderPeak(M4AEngine *engine)
{
    return renderPeaks(engine).total();
}

bool rendersAudibly(M4AEngine *engine)
{
    return renderPeak(engine) > 0.0f;
}

int checkTrackProgram(const M4AEngine &engine, int track, uint8_t program, const char *what)
{
    const M4ATrack &t = engine.tracks[track];
    if (t.currentProgram != program || t.currentVoice.wav == nullptr) {
        std::fprintf(stderr,
                     "primecheck: FAIL: %s: track %d has program %d (wav %s), expected %d\n", what,
                     track, t.currentProgram, t.currentVoice.wav ? "set" : "null", program);
        return 1;
    }
    return 0;
}

} // namespace

int runPrimeCheck()
{
    int failures = 0;

    const SmfFile smf = buildPrimeSong();
    const auto timeline = MidiTimeline::build(smf, kSampleRate);
    if (!timeline || timeline->usedTrackCount != 3) {
        std::fprintf(stderr, "primecheck: synthesized song built wrong\n");
        return 1;
    }

    TestVoicegroup vg;

    // Control: chase alone at position 0 leaves track 1 (voice at tick 48)
    // with no instrument, so an auditioned note renders pure silence — the
    // reported symptom.
    {
        M4AEngine engine;
        m4a_engine_init(&engine, float(kSampleRate));
        m4a_engine_set_voicegroup(&engine, vg.voices);
        TimelinePlayer::chase(&engine, timeline.get(), 0);
        m4a_engine_note_on(&engine, 1, 60, 127);
        if (rendersAudibly(&engine)) {
            std::fprintf(stderr, "primecheck: FAIL: unprimed track 1 audition was audible "
                                 "(control expectation changed?)\n");
            failures++;
        }
        m4a_engine_destroy(&engine);
    }

    // The load recipe: chase + primeVoices at position 0.
    {
        M4AEngine engine;
        m4a_engine_init(&engine, float(kSampleRate));
        m4a_engine_set_voicegroup(&engine, vg.voices);
        TimelinePlayer::chase(&engine, timeline.get(), 0);
        TimelinePlayer::primeVoices(&engine, timeline.get(), 0);

        failures += checkTrackProgram(engine, 0, kProgAtZero,
                                      "chase-applied voice not overridden by a later one");
        failures += checkTrackProgram(engine, 1, kProgTrack1, "later voice primed at load");
        if (engine.tracks[2].currentVoice.wav != nullptr) {
            std::fprintf(stderr, "primecheck: FAIL: track with no voice event was primed anyway\n");
            failures++;
        }

        m4a_engine_note_on(&engine, 1, 60, 127);
        if (!rendersAudibly(&engine)) {
            std::fprintf(stderr, "primecheck: FAIL: primed track 1 audition was silent\n");
            failures++;
        }
        m4a_engine_destroy(&engine);
    }

    // Mid-song position (a seek past both of track 0's voice events): chase
    // supplies every track's program, priming adds nothing.
    {
        M4AEngine engine;
        m4a_engine_init(&engine, float(kSampleRate));
        m4a_engine_set_voicegroup(&engine, vg.voices);
        const uint64_t pos = (kLaterTick + 4) * kSamplesPerTick;
        TimelinePlayer::chase(&engine, timeline.get(), pos);
        TimelinePlayer::primeVoices(&engine, timeline.get(), pos);

        failures += checkTrackProgram(engine, 0, kProgLater, "mid-song chase wins");
        failures += checkTrackProgram(engine, 1, kProgTrack1, "mid-song chase supplies track 1");
        m4a_engine_destroy(&engine);
    }

    // TimelinePlayer::chaseTouched (docs/midi-controls/PLAN.md step 2): a
    // track a MIDI keyboard moved gets back exactly the lanes it touched —
    // the song's value at the position, the engine's track-init default where
    // the song has none — and every other lane and track keeps what it holds.
    {
        M4AEngine engine;
        m4a_engine_init(&engine, float(kSampleRate));
        m4a_engine_set_voicegroup(&engine, vg.voices);
        const uint64_t early = 4 * kSamplesPerTick;
        const uint64_t late = (kLaterTick + 4) * kSamplesPerTick;
        TimelinePlayer::chase(&engine, timeline.get(), early);
        TimelinePlayer::primeVoices(&engine, timeline.get(), early);
        // What a keyboard might leave behind, on every lane of tracks 0-2.
        const auto poke = [&](int track) {
            m4a_engine_cc(&engine, track, 0x07, 50);
            m4a_engine_cc(&engine, track, 0x0A, 10);
            m4a_engine_cc(&engine, track, 0x14, 12);
            m4a_engine_cc(&engine, track, 0x15, 99);
            m4a_engine_cc(&engine, track, 0x01, 30);
            m4a_engine_pitch_bend(&engine, track, 4096);
            m4a_engine_program_change(&engine, track, 3);
        };
        struct Lanes {
            int vol, pan, bendr, lfos, mod, bend, program;
            bool operator==(const Lanes &o) const
            {
                return vol == o.vol && pan == o.pan && bendr == o.bendr && lfos == o.lfos &&
                       mod == o.mod && bend == o.bend && program == o.program;
            }
        };
        const auto lanes = [&](int track) {
            const M4ATrack &t = engine.tracks[track];
            return Lanes{t.rawVolume, t.pan,  t.bendRange,     t.lfoSpeed,
                         t.mod,       t.bend, t.currentProgram};
        };
        const Lanes poked{50, 10 - 64, 12, 99, 30, 32, 3};
        const auto expect = [&](bool ok, const char *what) {
            if (!ok) {
                std::fprintf(stderr, "primecheck: FAIL: chaseTouched: %s\n", what);
                failures++;
            }
        };
        for (int t = 0; t < 3; t++)
            poke(t);
        expect(lanes(0) == poked && lanes(1) == poked, "the fixture's pokes did not land");

        // VOL (a song event), BENDR (none: the default), the bend (none:
        // centred) and the voice (at tick 0) on track 0; PAN, LFOS and MOD
        // stay as poked, and so does track 1.
        TouchedLanes touched;
        touched.setCc(0x07);
        touched.setCc(0x14);
        touched.bend = true;
        touched.program = true;
        TimelinePlayer::chaseTouched(&engine, timeline.get(), early, 0, touched);
        expect(lanes(0) == Lanes{kVolEarly, 10 - 64, 2, 99, 30, 0, kProgAtZero},
               "track 0 did not get back exactly VOL, BENDR, bend and voice");
        expect(lanes(1) == poked, "another track was touched");

        // PAN (a song event, late), LFOS and MOD (defaults) late on track 0.
        TouchedLanes rest;
        rest.setCc(0x0A);
        rest.setCc(0x15);
        rest.setCc(0x01);
        TimelinePlayer::chaseTouched(&engine, timeline.get(), late, 0, rest);
        expect(lanes(0) == Lanes{kVolEarly, kPanLate, 2, 22, 0, 0, kProgAtZero},
               "track 0's PAN, LFOS and MOD were not chased late, or VOL was re-chased");

        // A voice with none at or before the position: the track's first one
        // later, as primeVoices gives it. None anywhere: kept.
        TouchedLanes voice;
        voice.program = true;
        TimelinePlayer::chaseTouched(&engine, timeline.get(), early, 1, voice);
        expect(lanes(1) == Lanes{50, 10 - 64, 12, 99, 30, 32, kProgTrack1},
               "track 1's voice was not primed from later, or another lane moved");
        TimelinePlayer::chaseTouched(&engine, timeline.get(), early, 2, voice);
        expect(lanes(2) == poked, "a track with no voice event lost the keyboard's voice");

        // Nothing touched: nothing moves.
        TimelinePlayer::chaseTouched(&engine, timeline.get(), late, 2, TouchedLanes());
        expect(lanes(2) == poked, "an empty mask moved something");
        expect(TimelinePlayer::ccDefault(0x07) == 127 && TimelinePlayer::ccDefault(0x0A) == 64 &&
                   TimelinePlayer::ccDefault(0x14) == 2 && TimelinePlayer::ccDefault(0x15) == 22 &&
                   TimelinePlayer::ccDefault(0x01) == 0 && TimelinePlayer::ccDefault(0x03) == -1,
               "ccDefault does not match the engine's track-init values");
        m4a_engine_destroy(&engine);
    }

    // The recording latch (docs/midi-controls/PLAN.md §3.4): render skips
    // exactly the song events a LaneLatch holds — its lanes, on its track —
    // and a loop wrap ends it, chasing its lanes to the song at the loop
    // start (not the loop end) before the loop start's events play.
    {
        auto timeline = MidiTimeline::build(buildLatchSong(), kSampleRate);
        if (!timeline || timeline->usedTrackCount != 2) {
            std::fprintf(stderr, "primecheck: FAIL: latch: the synthesized song built wrong\n");
            return 1;
        }
        timeline->loopStartTick = kLatchLoopStart;
        timeline->loopEndTick = kLatchLoopEnd;
        timeline->loopStartSample = kLatchLoopStart * kSamplesPerTick;
        timeline->loopEndSample = kLatchLoopEnd * kSamplesPerTick;
        const auto expect = [&](bool ok, const char *what) {
            if (!ok) {
                std::fprintf(stderr, "primecheck: FAIL: latch: %s\n", what);
                failures++;
            }
        };
        struct Lanes {
            int vol, pan, mod, bend, program, vol1;
            bool operator==(const Lanes &o) const
            {
                return vol == o.vol && pan == o.pan && mod == o.mod && bend == o.bend &&
                       program == o.program && vol1 == o.vol1;
            }
        };
        struct Run {
            M4AEngine engine;
            TimelinePlayer player;
            float bufL[512], bufR[512];
            Run(TestVoicegroup &vg, const MidiTimeline *timeline)
            {
                m4a_engine_init(&engine, float(kSampleRate));
                m4a_engine_set_voicegroup(&engine, vg.voices);
                TimelinePlayer::chase(&engine, timeline, 0);
                player.reset();
            }
            ~Run() { m4a_engine_destroy(&engine); }
            // Renders `ticks` ticks of playback time (wraps included).
            void play(const MidiTimeline *timeline, uint64_t ticks, LaneLatch *latch)
            {
                uint64_t left = ticks * kSamplesPerTick;
                while (left > 0) {
                    const uint32_t n = uint32_t(std::min<uint64_t>(left, 512));
                    player.render(&engine, timeline, bufL, bufR, n, true, 0, latch);
                    left -= n;
                }
            }
            Lanes lanes() const
            {
                const M4ATrack &t = engine.tracks[0];
                return Lanes{t.rawVolume,
                             t.pan,
                             t.mod,
                             t.bend,
                             t.currentProgram,
                             engine.tracks[1].rawVolume};
            }
            void poke()
            {
                m4a_engine_cc(&engine, 0, 0x07, 33);
                m4a_engine_cc(&engine, 0, 0x0A, 5);
                m4a_engine_pitch_bend(&engine, 0, 4096);
                m4a_engine_program_change(&engine, 0, 3);
            }
            bool keyed(uint8_t key) const
            {
                for (int i = 0; i < TOTAL_PCM_CHANNELS; i++) {
                    const M4APCMChannel &ch = engine.pcmChannels[i];
                    if ((ch.status & CHN_ON) && !(ch.status & CHN_STOP) && ch.midiKey == key &&
                        ch.trackIndex == 0)
                        return true;
                }
                return false;
            }
        };
        // The song unlatched at tick 16: every event played.
        const Lanes songAt16{80, 100 - 64, 9, 8, 9, 50};
        {
            auto run = std::make_unique<Run>(vg, timeline.get());
            run->play(timeline.get(), 3, nullptr);
            run->poke();
            run->play(timeline.get(), 13, nullptr);
            expect(run->lanes() == songAt16, "the control render did not play the song");
        }
        // A latch with no lanes holds nothing.
        {
            auto run = std::make_unique<Run>(vg, timeline.get());
            LaneLatch none;
            none.track = 0;
            run->play(timeline.get(), 3, &none);
            run->poke();
            run->play(timeline.get(), 13, &none);
            expect(run->lanes() == songAt16, "an empty latch held a song event");
        }
        // VOL, PAN, the bend and the voice latched on track 0, after the
        // keyboard set them at tick 3: their events stay out, while MOD,
        // track 1's VOL and track 0's note play.
        {
            auto run = std::make_unique<Run>(vg, timeline.get());
            LaneLatch latch;
            latch.track = 0;
            latch.lanes.setCc(0x07);
            latch.lanes.setCc(0x0A);
            latch.lanes.bend = true;
            latch.lanes.program = true;
            run->play(timeline.get(), 3, &latch);
            run->poke();
            run->play(timeline.get(), 4, &latch);
            expect(run->keyed(60), "a note on the latched track did not play");
            run->play(timeline.get(), 9, &latch); // tick 16
            expect(run->lanes() == Lanes{33, 5 - 64, 9, 32, 3, 50},
                   "the latch did not hold exactly its lanes' events on its track");
            expect(latch.any(), "the latch ended before the wrap");
            // To the loop end, where its VOL 11 is held back too, then one
            // tick past the wrap: the latch is over, its lanes are the
            // song's at the loop start (VOL 90 from the loop start's own
            // event, PAN 20 from before it, the bend centred, voice 5),
            // never the loop end's (VOL 11, PAN 100, bend +1024, voice 9).
            run->play(timeline.get(), 14, &latch);
            expect(run->lanes().vol == 33, "the latch did not hold the loop end's VOL");
            run->play(timeline.get(), 1, &latch); // tick 6 of pass 2
            expect(!latch.any() && latch.track == -1, "the loop wrap did not end the latch");
            expect(run->lanes() == Lanes{90, 20 - 64, 9, 0, 5, 50},
                   "the wrap did not chase the latched lanes to the loop start");
            // Pass 2 plays the song's own events.
            run->play(timeline.get(), 10, &latch); // tick 16
            expect(run->lanes() == songAt16, "pass 2 did not play the song's events");
        }
        // A latch on another track leaves track 0 alone.
        {
            auto run = std::make_unique<Run>(vg, timeline.get());
            LaneLatch other;
            other.track = 1;
            other.lanes.setCc(0x0A);
            other.lanes.bend = true;
            run->play(timeline.get(), 3, &other);
            run->poke();
            run->play(timeline.get(), 13, &other);
            expect(run->lanes() == songAt16, "a latch on track 1 held track 0's events");
        }
    }

    // An audition belongs to the note, not to the cursor: with the engine
    // chased past the VOL drop, keying at the early note's own VOL must sound
    // exactly as it would have back there — and must leave the track's real
    // VOL, and every note already sounding on it, alone.
    //
    // Held for many frames and with the track's LFO running, because a
    // vibrato lead (the ordinary shape of a PSG square part) recomputes every
    // sounding channel's volume from the track on each LFO tick. An audition
    // that merely *starts* at the right volume is pulled onto the track's
    // current VOL a few milliseconds later — audibly, and the reason the
    // channel carries its audition volume rather than the track borrowing it
    // across the note-on.
    {
        const uint64_t late = (kLaterTick + 4) * kSamplesPerTick;
        const uint64_t early = 4 * kSamplesPerTick;

        struct VoiceKind {
            const char *name;
            const ToneData *voices;
        };
        const VoiceKind kinds[] = {{"directsound", vg.voices}, {"square", vg.cgbVoices}};

        // The note's settled level, not its onset: a peak taken across the
        // whole render would still pass on an audition that starts right and
        // sinks. The voices sustain flat, so the last block is the level the
        // note holds.
        const auto settledPeak = [&](const ToneData *voices, uint64_t pos, int rawVolume, int pan,
                                     bool lfo) {
            M4AEngine engine;
            m4a_engine_init(&engine, float(kSampleRate));
            m4a_engine_set_voicegroup(&engine, const_cast<ToneData *>(voices));
            TimelinePlayer::chase(&engine, timeline.get(), pos);
            if (lfo) {
                m4a_engine_cc(&engine, 0, 0x01, 40); // MOD depth: the LFO runs
                m4a_engine_cc(&engine, 0, 0x15, 60); // LFO speed
            }
            TimelinePlayer::auditionNoteOn(&engine, 0, 60, 127, rawVolume, pan);
            StereoPeak peak;
            for (int i = 0; i < 40; i++)
                peak = renderPeaks(&engine);
            m4a_engine_destroy(&engine);
            return peak;
        };

        for (const VoiceKind &kind : kinds) {
            for (const bool lfo : {false, true}) {
                const StereoPeak earlyPeak =
                    settledPeak(kind.voices, early, -1, M4A_AUDITION_PAN_NONE, lfo);
                const StereoPeak latePeak =
                    settledPeak(kind.voices, late, -1, M4A_AUDITION_PAN_NONE, lfo);
                if (!(earlyPeak.total() > latePeak.total() && latePeak.total() > 0.0f)) {
                    std::fprintf(stderr,
                                 "primecheck: FAIL: the fixture's VOL drop is not audible on a %s "
                                 "voice%s (control expectation changed?)\n",
                                 kind.name, lfo ? " with its LFO running" : "");
                    failures++;
                    continue;
                }
                const StereoPeak auditionPeak =
                    settledPeak(kind.voices, late, kVolEarly, kPanEarly, lfo);
                if (!auditionPeak.sameAs(earlyPeak)) {
                    std::fprintf(stderr,
                                 "primecheck: FAIL: a %s audition%s at the note's own VOL and PAN "
                                 "settled at %.6f/%.6f; the note there settles at %.6f/%.6f (the "
                                 "quiet side is %.6f/%.6f)\n",
                                 kind.name, lfo ? " with its LFO running" : "",
                                 double(auditionPeak.l), double(auditionPeak.r),
                                 double(earlyPeak.l), double(earlyPeak.r), double(latePeak.l),
                                 double(latePeak.r));
                    failures++;
                }
                // The pan is doing real work in that match: keyed at the
                // note's VOL but left on the cursor's PAN, the same audition
                // lands somewhere else entirely.
                const StereoPeak volumeOnlyPeak =
                    settledPeak(kind.voices, late, kVolEarly, M4A_AUDITION_PAN_NONE, lfo);
                if (volumeOnlyPeak.sameAs(earlyPeak)) {
                    std::fprintf(stderr,
                                 "primecheck: FAIL: a %s audition%s ignoring the note's PAN still "
                                 "matched it (control expectation changed?)\n",
                                 kind.name, lfo ? " with its LFO running" : "");
                    failures++;
                }
            }
        }

        // The track itself is untouched: its VOL still reads the one chased to
        // the cursor, and a note already sounding on it keeps its own level.
        M4AEngine engine;
        m4a_engine_init(&engine, float(kSampleRate));
        m4a_engine_set_voicegroup(&engine, vg.voices);
        TimelinePlayer::chase(&engine, timeline.get(), late);
        m4a_engine_note_on(&engine, 0, 48, 127);
        uint8_t heldRight = 0, heldLeft = 0;
        for (int i = 0; i < MAX_PCM_CHANNELS; i++) {
            if ((engine.pcmChannels[i].status & CHN_ON) && engine.pcmChannels[i].midiKey == 48) {
                heldRight = engine.pcmChannels[i].rightVolume;
                heldLeft = engine.pcmChannels[i].leftVolume;
            }
        }
        TimelinePlayer::auditionNoteOn(&engine, 0, 60, 127, kVolEarly, kPanEarly);
        (void)renderPeak(&engine); // let the track's own refreshes run
        if (engine.tracks[0].rawVolume != kVolLate ||
            engine.tracks[0].volume !=
                uint8_t(int(kVolLate) * engine.songMasterVolume / MAX_SONG_VOLUME)) {
            std::fprintf(stderr, "primecheck: FAIL: the audition moved the track's own VOL\n");
            failures++;
        }
        if (engine.tracks[0].pan != kPanLate) {
            std::fprintf(stderr, "primecheck: FAIL: the audition moved the track's own PAN\n");
            failures++;
        }
        bool heldMoved = false;
        for (int i = 0; i < MAX_PCM_CHANNELS; i++) {
            if ((engine.pcmChannels[i].status & CHN_ON) && engine.pcmChannels[i].midiKey == 48 &&
                (engine.pcmChannels[i].rightVolume != heldRight ||
                 engine.pcmChannels[i].leftVolume != heldLeft))
                heldMoved = true;
        }
        if (heldMoved) {
            std::fprintf(stderr, "primecheck: FAIL: the audition moved a note already sounding "
                                 "on the track\n");
            failures++;
        }

        // ...and the audition itself stays where it was keyed. A PAN event
        // arriving while it sounds — the playhead running past one, or the
        // track's LFO recalculating — must not drag it onto the track's new
        // pan, exactly as a VOL event must not drag its volume.
        uint8_t audRight = 0, audLeft = 0;
        for (int i = 0; i < MAX_PCM_CHANNELS; i++) {
            if ((engine.pcmChannels[i].status & CHN_ON) && engine.pcmChannels[i].midiKey == 60) {
                audRight = engine.pcmChannels[i].rightVolume;
                audLeft = engine.pcmChannels[i].leftVolume;
            }
        }
        m4a_engine_cc(&engine, 0, 10, 0); // hard left, the other end from kPanEarly
        m4a_engine_cc(&engine, 0, 7, kVolLate);
        (void)renderPeak(&engine);
        bool auditionMoved = false;
        for (int i = 0; i < MAX_PCM_CHANNELS; i++) {
            if ((engine.pcmChannels[i].status & CHN_ON) && engine.pcmChannels[i].midiKey == 60 &&
                (engine.pcmChannels[i].rightVolume != audRight ||
                 engine.pcmChannels[i].leftVolume != audLeft))
                auditionMoved = true;
        }
        if (!(audRight || audLeft)) {
            std::fprintf(stderr, "primecheck: FAIL: the audition note is not sounding "
                                 "(control expectation changed?)\n");
            failures++;
        }
        if (auditionMoved) {
            std::fprintf(stderr, "primecheck: FAIL: a later PAN/VOL event dragged the sounding "
                                 "audition off the values it was keyed at\n");
            failures++;
        }
        m4a_engine_destroy(&engine);
    }

    std::printf("primecheck: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
