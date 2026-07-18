#include <QByteArray>
#include <QElapsedTimer>
#include <QThread>
#include <cstdio>
#include <cstring>
#include <functional>

#include "audio/audioengine.h"
#include "core/miditimeline.h"
#include "core/smf.h"

// --transportcheck: starting playback halts ringing auditions (self-contained,
// needs a working audio output device — SKIPs cleanly without one). A
// slow-release voice keeps ringing long after a timed preview's note-off, and
// without the transport-side hard cut that tail (or a preview still counting
// down) persists into song playback. Plays a synthesized song with NO notes,
// so the engine's active-channel telemetry isolates the previews: after
// play() the count must drop to zero. Covers every entry into Playing — from
// Stopped, and from Paused both as Space reaches it (seek + play with an
// already-released tail ringing; Space toggles pause, so this is how
// playback usually starts) and with a preview still counting down.

namespace {

constexpr uint32_t kDivision = 24;

SmfEvent channelEvent(uint64_t tick, uint8_t status, uint8_t data0, uint8_t data1)
{
    SmfEvent ev;
    ev.tick = tick;
    ev.status = status;
    ev.data0 = data0;
    ev.data1 = data1;
    return ev;
}

// Two voiced tracks and no notes: both use program 0 (release 254, a tail
// that rings for ~12 s — only a hard cut can silence it promptly). The late
// CC stretches lengthSamples so playback doesn't auto-stop under the checks.
SmfFile buildSilentSong()
{
    SmfFile smf;
    smf.format = 1;
    smf.division = kDivision;
    smf.tracks.resize(3);

    SmfTrack &conductor = smf.tracks[0];
    SmfEvent tempo;
    tempo.tick = 0;
    tempo.status = 0xFF;
    tempo.metaType = 0x51;
    tempo.blob = QByteArray("\x07\xA1\x20", 3); // 120 BPM
    conductor.events.push_back(tempo);
    conductor.endTick = 4800;

    SmfTrack &t0 = smf.tracks[1];
    t0.events.push_back(channelEvent(0, 0xC0, 0, 0));
    t0.events.push_back(channelEvent(4800, 0xB0, 7, 100)); // 100 s at 120 BPM
    t0.endTick = 4800;

    SmfTrack &t1 = smf.tracks[2];
    t1.events.push_back(channelEvent(0, 0xC1, 1, 0));
    t1.endTick = 4800;

    return smf;
}

// A minimal in-memory voicegroup (primecheck's recipe): one looped PCM square
// wave, instant attack, full sustain; only the release rates differ.
struct TestVoicegroup {
    int8_t sample[64];
    WaveData wave;
    LoadedVoiceGroup vg;

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
        std::memset(&vg, 0, sizeof(vg));
        for (ToneData &v : vg.voices) {
            v.type = VOICE_DIRECTSOUND;
            v.key = 60;
            v.wav = &wave;
            v.attack = 255;
            v.decay = 0;
            v.sustain = 255;
            v.release = 254; // slow: (env * 254) >> 8 per frame, ~12 s ring
        }
    }
};

// The audio thread runs on the device's own callbacks, so conditions are
// polled in real time.
bool waitFor(const std::function<bool()> &cond, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (cond())
            return true;
        QThread::msleep(10);
    }
    return cond();
}

} // namespace

int runTransportCheck()
{
    int failures = 0;
    auto fail = [&](const char *what) {
        std::fprintf(stderr, "transportcheck: FAIL: %s\n", what);
        failures++;
    };

    TestVoicegroup tvg;
    std::unique_ptr<MidiTimeline> timeline;
    AudioEngine engine;
    QString error;
    if (!engine.init(&error)) {
        std::printf("transportcheck: SKIP (no audio device: %s)\n",
                    qUtf8Printable(error));
        return 0;
    }

    const SmfFile smf = buildSilentSong();
    timeline = MidiTimeline::build(smf, engine.sampleRate());
    if (!timeline || timeline->usedTrackCount != 2) {
        std::fprintf(stderr, "transportcheck: synthesized song built wrong\n");
        return 1;
    }
    engine.loadSong(timeline.get(), &tvg.vg, SongSettings{});

    const auto active = [&] { return engine.activePcmChannels(); };

    // A short audition whose note-off has already gone out, leaving a
    // ringing slow-release tail — the reported symptom. Fails the whole
    // check if the preview never sounds or the tail dies early (the
    // control expectation changed).
    const auto ringingTail = [&](uint8_t track) {
        engine.previewNoteTimed(track, 60, 127, uint32_t(0.15 * engine.sampleRate()));
        if (!waitFor([&] { return active() >= 1; }, 2000)) {
            fail("timed preview never sounded");
            return false;
        }
        QThread::msleep(400); // note-off sent; the slow release rings on
        if (active() < 1) {
            fail("slow-release tail died early (control expectation changed?)");
            return false;
        }
        return true;
    };

    // Stopped → Playing: the ringing tail must be cut when playback starts.
    if (ringingTail(0)) {
        engine.play();
        if (!waitFor([&] { return active() == 0; }, 2000))
            fail("audition tail persisted after playback started from stop");
    }

    // Paused → Playing, the Space path (pause, audition, seek + play):
    // Space toggles pause and restarts from the edit cursor, so this is how
    // playback usually starts — the tail must be cut here too.
    engine.pause();
    if (ringingTail(0)) {
        engine.seek(0);
        engine.play();
        if (!waitFor([&] { return active() == 0; }, 2000))
            fail("audition tail persisted after Space-style seek + play from pause");
    }

    // Paused → Playing with the preview still counting down: resuming must
    // cut it rather than let it sound over the song for the full duration.
    engine.pause();
    engine.previewNoteTimed(1, 64, 127, uint32_t(60.0 * engine.sampleRate()));
    if (!waitFor([&] { return active() >= 1; }, 2000)) {
        fail("timed preview during pause never sounded");
    } else {
        engine.play();
        if (!waitFor([&] { return active() == 0; }, 2000))
            fail("counting-down preview persisted after resuming playback");
    }

    engine.stop();
    engine.unloadSong();

    if (failures == 0)
        std::printf("transportcheck: PASS\n");
    return failures ? 1 : 0;
}
