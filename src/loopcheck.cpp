#include <QString>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/miditimeline.h"
#include "core/smf.h"
#include "core/timelineplayer.h"

extern "C" {
#include "m4a_engine.h"
}

// --loopcheck: loop-wrap playback check, fully self-contained (no project
// needed). Synthesizes a looping song exercising every note/loop-boundary
// relationship — a normal body note, a note ending exactly at the loop end,
// a note spanning the loop end, and a note starting exactly at the loop end
// (the downbeat of the next measure) — then renders across several wraps and
// asserts no note is left keyed on forever. mid2agb sorts same-tick events so
// the loop GOTO lands after note-ends but before note starts, so a note
// starting at the loop end must not sound while looping, while one ending
// there must still release.

namespace {

// 24 ticks per quarter, 4/4, 120 BPM: one tick is 500000/24 us, which at
// 48kHz is exactly 1000 samples — probe positions below rely on this.
constexpr uint32_t kDivision = 24;
constexpr double kSampleRate = 48000.0;
constexpr uint64_t kSamplesPerTick = 1000;

constexpr uint64_t kLoopStartTick = 96;  // measure 2
constexpr uint64_t kLoopEndTick = 288;   // downbeat of measure 4
constexpr uint8_t kBodyKey = 60;     // ticks 96-120, plays every pass
constexpr uint8_t kEndsAtKey = 62;   // ticks 264-288, off exactly at loop end
constexpr uint8_t kSpansKey = 65;    // ticks 240-300, crosses the loop end
constexpr uint8_t kBoundaryKey = 64; // ticks 288-312, the reported stuck note

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

SmfFile buildLoopSong()
{
    SmfFile smf;
    smf.format = 1;
    smf.division = kDivision;
    smf.tracks.resize(2);

    SmfTrack &conductor = smf.tracks[0];
    conductor.events.push_back(metaEvent(0, 0x51, QByteArray("\x07\xA1\x20", 3)));
    conductor.events.push_back(metaEvent(kLoopStartTick, 0x01, QByteArray("[")));
    conductor.events.push_back(metaEvent(kLoopEndTick, 0x01, QByteArray("]")));
    conductor.endTick = 384;

    SmfTrack &notes = smf.tracks[1];
    auto note = [&notes](uint64_t on, uint64_t off, uint8_t key) {
        notes.events.push_back(channelEvent(on, 0x90, key, 100));
        notes.events.push_back(channelEvent(off, 0x80, key, 0));
    };
    notes.events.push_back(channelEvent(0, 0xC0, 0, 0));
    note(96, 120, kBodyKey);
    note(240, 300, kSpansKey);
    note(264, 288, kEndsAtKey);
    note(288, 312, kBoundaryKey);
    std::sort(notes.events.begin(), notes.events.end(),
              [](const SmfEvent &a, const SmfEvent &b) { return a.tick < b.tick; });
    notes.endTick = 384;

    return smf;
}

// A minimal in-memory voicegroup: one looped PCM square wave that sustains at
// full level until its note-off, so a note whose note-off never arrives stays
// keyed on (and audible) forever — exactly the reported symptom.
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
            v.release = 165; // ~0.2 s fade, then the channel frees itself
        }
    }
};

// Keys of PCM channels that are keyed on (started and not yet released).
std::vector<uint8_t> keyedOnKeys(const M4AEngine &engine)
{
    std::vector<uint8_t> keys;
    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++) {
        const M4APCMChannel &ch = engine.pcmChannels[i];
        if ((ch.status & CHN_ON) && !(ch.status & CHN_STOP))
            keys.push_back(ch.midiKey);
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

struct Probe {
    uint64_t samplePos; // in un-wrapped (global) playback time
    std::vector<uint8_t> expectedKeys;
    const char *what;
};

int runProbes(const MidiTimeline &timeline, bool looping, std::vector<Probe> probes)
{
    int failures = 0;
    M4AEngine engine;
    m4a_engine_init(&engine, float(kSampleRate));
    TestVoicegroup vg;
    m4a_engine_set_voicegroup(&engine, vg.voices);

    TimelinePlayer player;
    player.reset();

    constexpr uint32_t kChunk = 512;
    float bufL[kChunk], bufR[kChunk];
    uint64_t rendered = 0;
    size_t next = 0;
    const uint64_t total = probes.back().samplePos + kChunk;
    while (rendered < total && next < probes.size()) {
        const uint32_t n =
            uint32_t(std::min<uint64_t>(kChunk, probes[next].samplePos - rendered));
        player.render(&engine, &timeline, bufL, bufR, n, looping, 0);
        rendered += n;
        if (rendered != probes[next].samplePos)
            continue;

        const Probe &probe = probes[next++];
        const std::vector<uint8_t> keys = keyedOnKeys(engine);
        if (keys != probe.expectedKeys) {
            std::fprintf(stderr,
                         "loopcheck: FAIL: %s (looping=%d, t=%llu): keyed-on notes [",
                         probe.what, int(looping),
                         (unsigned long long)probe.samplePos);
            for (uint8_t k : keys)
                std::fprintf(stderr, " %d", k);
            std::fprintf(stderr, " ], expected [");
            for (uint8_t k : probe.expectedKeys)
                std::fprintf(stderr, " %d", k);
            std::fprintf(stderr, " ]\n");
            failures++;
        }
    }
    m4a_engine_destroy(&engine);
    return failures;
}

} // namespace

int runLoopCheck()
{
    int failures = 0;

    const SmfFile smf = buildLoopSong();
    const auto timeline = MidiTimeline::build(smf, kSampleRate);
    if (!timeline || !timeline->hasLoop()
        || timeline->loopStartSample != kLoopStartTick * kSamplesPerTick
        || timeline->loopEndSample != kLoopEndTick * kSamplesPerTick) {
        std::fprintf(stderr, "loopcheck: synthesized song has wrong loop points\n");
        return 1;
    }
    const uint64_t loopLen = timeline->loopEndSample - timeline->loopStartSample;

    // Looping playback. The quiet-zone probe sits after the body note's
    // release and before the spanning note starts; anything keyed on there is
    // a note the wrap orphaned from its note-off.
    const uint64_t quiet = 200000, bodyStart = 298000;
    failures += runProbes(*timeline, true,
                          {{quiet, {}, "pass-1 quiet zone"},
                           {bodyStart, {kBodyKey}, "pass-2 body note plays, "
                                                   "loop-end note-off honored"},
                           {quiet + loopLen, {}, "pass-2 quiet zone (stuck notes)"},
                           {quiet + 2 * loopLen, {}, "pass-3 quiet zone (stuck notes)"}});

    // Not looping: playback runs straight through, so the note at the loop
    // end and the note spanning it both sound normally.
    failures += runProbes(*timeline, false,
                          {{bodyStart, {kBoundaryKey, kSpansKey},
                            "loop-boundary notes play when not looping"}});

    std::printf("loopcheck: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
