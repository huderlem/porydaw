#include <QByteArray>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

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

constexpr uint8_t kProgAtZero = 5;  // engine track 0, tick 0
constexpr uint8_t kProgLater = 9;   // engine track 0, tick 96
constexpr uint8_t kProgTrack1 = 7;  // engine track 1, tick 48 (its first)
constexpr uint64_t kLaterTick = 96;

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
    t0.events.push_back(channelEvent(0, 0x90, 60, 100));
    t0.events.push_back(channelEvent(24, 0x80, 60, 0));
    t0.events.push_back(channelEvent(kLaterTick, 0xC0, kProgLater, 0));
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

// A minimal in-memory voicegroup: one looped PCM square wave shared by every
// program, so any applied voice has a non-null wav (an untouched track's
// zeroed voice does not).
struct TestVoicegroup {
    int8_t sample[64];
    WaveData wave;
    ToneData voices[128];

    TestVoicegroup()
    {
        for (int i = 0; i < 64; i++)
            sample[i] = i < 32 ? 100 : -100;
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
    }
};

bool rendersAudibly(M4AEngine *engine)
{
    constexpr uint32_t kFrames = 512;
    float bufL[kFrames], bufR[kFrames];
    m4a_engine_process(engine, bufL, bufR, int(kFrames));
    float peak = 0.0f;
    for (uint32_t i = 0; i < kFrames; i++)
        peak = std::max(peak, std::max(std::fabs(bufL[i]), std::fabs(bufR[i])));
    return peak > 0.0f;
}

int checkTrackProgram(const M4AEngine &engine, int track, uint8_t program, const char *what)
{
    const M4ATrack &t = engine.tracks[track];
    if (t.currentProgram != program || t.currentVoice.wav == nullptr) {
        std::fprintf(stderr,
                     "primecheck: FAIL: %s: track %d has program %d (wav %s), expected %d\n",
                     what, track, t.currentProgram,
                     t.currentVoice.wav ? "set" : "null", program);
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
            std::fprintf(stderr,
                         "primecheck: FAIL: unprimed track 1 audition was audible "
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
            std::fprintf(stderr,
                         "primecheck: FAIL: track with no voice event was primed anyway\n");
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

    std::printf("primecheck: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
