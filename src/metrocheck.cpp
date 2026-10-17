#include <QByteArray>
#include <QString>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "audio/audioengine.h"
#include "audio/midiinput.h"
#include "core/captureevent.h"
#include "core/clickgrid.h"
#include "core/miditimeline.h"
#include "core/smf.h"

// --metrocheck: the metronome (docs/metronome/PLAN.md step 1), fully
// self-contained (no project needed).
//  - The click grid against the timeline: songs with tempo changes off the
//    beat (three on one tick: the last wins) and meter changes (one
//    mid-bar), at several tick rates and sample rates, carry a note on every
//    beat line stepGridLines finds; every click must land on exactly its
//    note's sample, accented exactly on bar lines, and clicksIn over
//    arbitrary chunk boundaries must give the same list as one query. A
//    capped query returns exactly the first `cap` clicks; a query far into
//    the unbounded last meter still finds its beats.
//  - The engine, device held: a silent looping song whose loop is not
//    beat-aligned clicks at the expected output frames across wraps,
//    accents bar lines, never reaches the audio tap, is silent when off,
//    and is cut by Stop, a seek and a song switch. Turning it off mid-click
//    lets that click finish and clicks no more. A loop shorter than a
//    callback (several wraps per render) clicks once per pass. The
//    auto-stop after the song's tail keeps the clicks its callback rendered
//    before the stop.
//  - Record's count-in (step 2): its length and beat lines for cursors on
//    and off bar lines and near tick 0; through the engine, its clicks with
//    the metronome off, the song starting sample-exactly after it, the
//    take's carried keys, Stop and rapid Stop/Record/Play; a tempo change
//    on the cursor's tick, a seek during it, a start past the loop end.

namespace {

int g_failures = 0;

void fail(const char *fmt, ...) Q_ATTRIBUTE_FORMAT_PRINTF(1, 2);
void fail(const char *fmt, ...)
{
    std::fprintf(stderr, "metrocheck: FAIL: ");
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "\n");
    g_failures++;
}

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

SmfEvent tempoUsEvent(uint64_t tick, uint32_t us)
{
    const char b[3] = {char((us >> 16) & 0xFF), char((us >> 8) & 0xFF), char(us & 0xFF)};
    return metaEvent(tick, 0x51, QByteArray(b, 3));
}

SmfEvent tempoEvent(uint64_t tick, double bpm)
{
    return tempoUsEvent(tick, uint32_t(60000000.0 / bpm + 0.5));
}

SmfEvent meterEvent(uint64_t tick, int numerator, int denomPow2)
{
    const char b[4] = {char(numerator), char(denomPow2), 24, 8};
    return metaEvent(tick, 0x58, QByteArray(b, 4));
}

// A conductor with tempo and meter changes at tpb-relative ticks, and one
// channel track (a program change only) for the notes the caller adds.
SmfFile gridSong(uint32_t tpb)
{
    SmfFile smf;
    smf.format = 1;
    smf.division = uint16_t(tpb);
    smf.tracks.resize(2);
    SmfTrack &c = smf.tracks[0];
    c.events.push_back(tempoEvent(0, 120.0));
    c.events.push_back(meterEvent(0, 4, 2));
    c.events.push_back(tempoEvent(tpb * 5 / 2 + 1, 97.0)); // off the beat
    c.events.push_back(meterEvent(tpb * 8, 6, 3));         // bar 3: 6/8
    c.events.push_back(tempoEvent(tpb * 9, 151.0));
    // 6/8 bars are 3 quarters; 3/4 starts mid-way through bar 5's 6/8.
    c.events.push_back(meterEvent(tpb * 8 + tpb * 3 + tpb / 2 * 3, 3, 2));
    c.events.push_back(tempoEvent(tpb * 16, 133.0));
    // Three tempos on one tick, off the beat: the last in the file wins.
    c.events.push_back(tempoEvent(tpb * 20 + tpb / 3, 80.0));
    c.events.push_back(tempoEvent(tpb * 20 + tpb / 3, 177.0));
    c.events.push_back(tempoEvent(tpb * 20 + tpb / 3, 140.0));
    c.endTick = tpb * 40;
    smf.tracks[1].events.push_back(channelEvent(0, 0xC0, 0, 0));
    smf.tracks[1].endTick = tpb * 40;
    return smf;
}

void checkGrid(uint32_t tpb, double sampleRate)
{
    // The beat lines the ruler draws, to tick tpb * 36.
    std::unique_ptr<MidiTimeline> plain = MidiTimeline::build(gridSong(tpb), sampleRate);
    const uint64_t limit = uint64_t(tpb) * 36;
    std::vector<uint64_t> beats;
    for (uint64_t t = 0; t < limit; t = plain->stepGridLines(t, 1, false))
        beats.push_back(t);

    SmfFile smf = gridSong(tpb);
    for (const uint64_t t : beats) {
        smf.tracks[1].events.push_back(channelEvent(t, 0x90, 60, 100));
        smf.tracks[1].events.push_back(channelEvent(t + 1, 0x80, 60, 0));
    }
    std::stable_sort(smf.tracks[1].events.begin(), smf.tracks[1].events.end(),
                     [](const SmfEvent &a, const SmfEvent &b) { return a.tick < b.tick; });
    std::unique_ptr<MidiTimeline> tl = MidiTimeline::build(smf, sampleRate);
    std::vector<uint64_t> noteSamples;
    std::vector<uint64_t> noteTicks;
    for (const TimelineEvent &ev : tl->events) {
        if (ev.type == 0x9) {
            noteSamples.push_back(ev.samplePos);
            noteTicks.push_back(ev.tick);
        }
    }
    if (noteSamples.size() != beats.size()) {
        fail("tpb %u @%.0f: %zu notes built for %zu beats", tpb, sampleRate, noteSamples.size(),
             beats.size());
        return;
    }
    const uint64_t end = tl->clickGrid.sampleAt(limit);
    std::vector<ClickGrid::Click> clicks(beats.size() + 8);
    const size_t n = tl->clickGrid.clicksIn(0, end, clicks.data(), clicks.size());
    clicks.resize(n);
    if (n != beats.size()) {
        fail("tpb %u @%.0f: %zu clicks for %zu beats", tpb, sampleRate, n, beats.size());
        return;
    }
    for (size_t i = 0; i < n; i++) {
        if (clicks[i].sample != noteSamples[i])
            fail("tpb %u @%.0f: beat %zu (tick %llu) clicks at %llu, its note at %llu", tpb,
                 sampleRate, i, (unsigned long long)beats[i], (unsigned long long)clicks[i].sample,
                 (unsigned long long)noteSamples[i]);
        // Playback starts (sampleForTick) land on the same sample.
        if (tl->sampleForTick(noteTicks[i]) != noteSamples[i])
            fail("tpb %u @%.0f: sampleForTick(%llu) = %llu, its note at %llu", tpb, sampleRate,
                 (unsigned long long)noteTicks[i],
                 (unsigned long long)tl->sampleForTick(noteTicks[i]),
                 (unsigned long long)noteSamples[i]);
        const double bar = tl->barPositionForTick(double(beats[i])).bar;
        const bool barLine = bar == std::floor(bar);
        if (clicks[i].accent != barLine)
            fail("tpb %u @%.0f: beat %zu (tick %llu, bar %.3f) accent %d", tpb, sampleRate, i,
                 (unsigned long long)beats[i], bar, int(clicks[i].accent));
    }

    // Arbitrary chunking: half-open spans neither drop nor repeat a beat.
    std::vector<ClickGrid::Click> chunked;
    uint64_t from = 0;
    uint32_t step = 1;
    while (from < end) {
        const uint64_t to = std::min<uint64_t>(end, from + 1 + (step * 7919u) % 6007u);
        ClickGrid::Click buf[64];
        const size_t m = tl->clickGrid.clicksIn(from, to, buf, 64);
        chunked.insert(chunked.end(), buf, buf + m);
        from = to;
        step++;
    }
    if (chunked.size() != clicks.size()) {
        fail("tpb %u @%.0f: chunked queries found %zu clicks, one query %zu", tpb, sampleRate,
             chunked.size(), clicks.size());
    } else {
        for (size_t i = 0; i < chunked.size(); i++) {
            if (chunked[i].sample != clicks[i].sample || chunked[i].accent != clicks[i].accent) {
                fail("tpb %u @%.0f: chunked click %zu differs", tpb, sampleRate, i);
                break;
            }
        }
    }
    // A query starting exactly on a beat includes it; ending on one excludes it.
    if (n > 3) {
        ClickGrid::Click buf[8];
        const size_t m = tl->clickGrid.clicksIn(clicks[2].sample, clicks[3].sample, buf, 8);
        if (m != 1 || buf[0].sample != clicks[2].sample)
            fail("tpb %u @%.0f: [beat 2, beat 3) found %zu clicks", tpb, sampleRate, m);
    }

    // A capped query returns exactly the first `cap` clicks, in order, from
    // any start (n / 2 reaches across meter changes).
    for (const size_t start : {size_t(0), size_t(5)}) {
        for (const size_t cap : {size_t(1), size_t(7), n / 2, n - start - 1}) {
            if (cap == 0 || start + cap >= n)
                continue;
            std::vector<ClickGrid::Click> buf(cap + 4);
            const size_t m = tl->clickGrid.clicksIn(clicks[start].sample, end, buf.data(), cap);
            bool same = m == cap;
            for (size_t i = 0; same && i < m; i++)
                same = buf[i].sample == clicks[start + i].sample &&
                       buf[i].accent == clicks[start + i].accent;
            if (!same)
                fail("tpb %u @%.0f: clicksIn from beat %zu capped at %zu returned %zu clicks%s",
                     tpb, sampleRate, start, cap, m, m == cap ? " that differ" : "");
        }
    }

    // Far into the last meter, which runs forever: its beats k0..k0+3.
    {
        const MidiTimeline::TimeSigSegment last = tl->timeSigSegments().back();
        const uint64_t k0 = 100000;
        const auto tickOf = [&](uint64_t k) { return last.tick + k * last.beatTicks; };
        ClickGrid::Click buf[8];
        const size_t m = tl->clickGrid.clicksIn(tl->clickGrid.sampleAt(tickOf(k0)),
                                                tl->clickGrid.sampleAt(tickOf(k0 + 4)), buf, 8);
        bool same = m == 4;
        for (size_t j = 0; same && j < m; j++)
            same = buf[j].sample == tl->clickGrid.sampleAt(tickOf(k0 + j)) &&
                   buf[j].accent == ((k0 + j) % uint64_t(last.beatsPerBar) == 0);
        if (!same)
            fail("tpb %u @%.0f: %zu clicks (expected 4) at tick %llu in the last meter%s", tpb,
                 sampleRate, m, (unsigned long long)tickOf(k0), m == 4 ? ", wrong samples" : "");
    }
}

// A minimal in-memory voicegroup (transportcheck's recipe); the songs here
// play no notes, so it only has to be valid.
struct TestVoicegroup {
    int8_t sample[65];
    WaveData wave;
    LoadedVoiceGroup vg;

    TestVoicegroup()
    {
        for (int i = 0; i < 64; i++)
            sample[i] = i < 32 ? 100 : -100;
        sample[64] = sample[63];
        std::memset(&wave, 0, sizeof(wave));
        wave.status = 0xC000;
        wave.freq = 8363u * 1024u;
        wave.size = 64;
        wave.data = sample;
        std::memset(&vg, 0, sizeof(vg));
        for (ToneData &v : vg.voices) {
            v.type = VOICE_DIRECTSOUND;
            v.key = 60;
            v.wav = &wave;
            v.attack = 255;
            v.sustain = 255;
            v.release = 200;
        }
    }
};

// A silent song: 120 BPM (or usPerBeat), 4/4, a loop [loopStart, loopEnd)
// in ticks.
SmfFile silentLoopSong(uint32_t tpb, uint64_t loopStart, uint64_t loopEnd,
                       uint32_t usPerBeat = 500000)
{
    SmfFile smf;
    smf.format = 1;
    smf.division = uint16_t(tpb);
    smf.tracks.resize(2);
    SmfTrack &c = smf.tracks[0];
    c.events.push_back(tempoUsEvent(0, usPerBeat));
    c.events.push_back(metaEvent(loopStart, 0x01, QByteArray("[")));
    c.events.push_back(metaEvent(loopEnd, 0x01, QByteArray("]")));
    c.endTick = loopEnd;
    smf.tracks[1].events.push_back(channelEvent(0, 0xC0, 0, 0));
    smf.tracks[1].endTick = loopEnd;
    return smf;
}

constexpr float kSilent = 1e-7f;

// Click onsets in interleaved output: the frame before the first sounding
// one after at least `gap` silent frames (a click's first frame is 0: its
// attack starts there). Peak of each click over its first `span` frames.
struct Onset {
    uint64_t frame;
    float peak;
};
std::vector<Onset> findOnsets(const std::vector<float> &out, uint32_t gap, uint32_t span)
{
    std::vector<Onset> onsets;
    const size_t frames = out.size() / 2;
    uint64_t silentRun = gap; // the start counts as silence
    for (size_t f = 0; f < frames; f++) {
        const bool sounding =
            std::fabs(out[f * 2]) > kSilent || std::fabs(out[f * 2 + 1]) > kSilent;
        if (!sounding) {
            silentRun++;
            continue;
        }
        if (silentRun >= gap) {
            float peak = 0.0f;
            for (size_t g = f; g < std::min(frames, f + span); g++)
                peak = std::max(peak, std::fabs(out[g * 2]));
            onsets.push_back({f == 0 ? 0 : f - 1, peak});
        }
        silentRun = 0;
    }
    return onsets;
}

bool allSilent(const float *v, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (std::fabs(v[i]) > kSilent)
            return false;
    }
    return true;
}

void checkEngine()
{
    AudioEngine engine;
    QString error;
    if (!engine.init(&error)) {
        std::printf("metrocheck: engine part SKIPPED (no audio device: %s)\n",
                    qUtf8Printable(error));
        return;
    }
    const double rate = engine.sampleRate();
    TestVoicegroup tvg;

    // 24 tpb, 4/4: beats every 24 ticks, bar lines at 0 and 96. The loop
    // [30, 150) starts and ends off the beat and holds beats 48-144, the
    // bar line at 96 among them.
    {
        const uint64_t loopStartTick = 30, loopEndTick = 150;
        std::unique_ptr<MidiTimeline> tl =
            MidiTimeline::build(silentLoopSong(24, loopStartTick, loopEndTick), rate);
        if (!tl || !tl->hasLoop()) {
            fail("silent loop song built wrong");
            return;
        }
        const ClickGrid &grid = tl->clickGrid;
        // Expected output frames over three passes.
        const uint64_t ls = tl->loopStartSample, le = tl->loopEndSample, len = le - ls;
        std::vector<std::pair<uint64_t, bool>> expected;
        for (uint64_t t = 0; t < loopEndTick; t += 24)
            expected.push_back({grid.sampleAt(t), t % 96 == 0});
        for (int pass = 1; pass <= 2; pass++) {
            for (uint64_t t = 48; t < loopEndTick; t += 24)
                expected.push_back({le + (pass - 1) * len + (grid.sampleAt(t) - ls), t % 96 == 0});
        }
        const uint64_t total = le + 2 * len;

        engine.holdDeviceForTest(true);
        engine.loadSong(tl.get(), &tvg.vg, SongSettings{});
        engine.setOutputGain(1.0f);
        engine.setMetronomeVolume(1.0f);

        // Off: the song is silent, and so is the output.
        engine.setMetronomeEnabled(false);
        engine.play();
        std::vector<float> out;
        engine.pumpForTest(uint32_t(total), &out);
        if (!allSilent(out.data(), out.size()))
            fail("metronome off: output not silent (the silent song or a stray click)");
        engine.stop();
        engine.pumpForTest(512);

        // On.
        engine.setMetronomeEnabled(true);
        engine.play();
        out.clear();
        engine.pumpForTest(uint32_t(total), &out);
        std::vector<float> tap(size_t(engine.tap().capacity()) * 2);
        engine.tap().readLatest(tap.data(), engine.tap().capacity());
        if (!allSilent(tap.data(), tap.size()))
            fail("the audio tap heard the metronome");
        const std::vector<Onset> onsets = findOnsets(out, 64, 400);
        if (onsets.size() != expected.size()) {
            fail("%zu clicks heard, %zu expected", onsets.size(), expected.size());
        } else {
            for (size_t i = 0; i < onsets.size(); i++) {
                if (onsets[i].frame != expected[i].first)
                    fail("click %zu at frame %llu, expected %llu", i,
                         (unsigned long long)onsets[i].frame,
                         (unsigned long long)expected[i].first);
                const bool accent =
                    onsets[i].peak > (ClickVoice::kAccentPeak + ClickVoice::kBeatPeak) / 2;
                if (accent != expected[i].second)
                    fail("click %zu peak %.3f: accent %d, expected %d", i, onsets[i].peak,
                         int(accent), int(expected[i].second));
            }
        }

        // Volume scales the click.
        engine.stop();
        engine.pumpForTest(512);
        engine.setMetronomeVolume(0.5f);
        engine.play();
        out.clear();
        engine.pumpForTest(uint32_t(grid.sampleAt(1)), &out);
        {
            const std::vector<Onset> o = findOnsets(out, 64, 400);
            const float want = ClickVoice::kAccentPeak * 0.5f;
            if (o.empty() || std::fabs(o[0].peak - want) > 0.02f)
                fail("at 50%% volume the first click peaks at %.3f, expected about %.3f",
                     o.empty() ? 0.0f : o[0].peak, want);
        }
        engine.setMetronomeVolume(1.0f);

        // Stop cuts a ringing click: stop 100 frames into the beat at tick 24.
        engine.stop();
        engine.pumpForTest(512);
        engine.play();
        engine.pumpForTest(uint32_t(grid.sampleAt(24) + 100));
        engine.stop();
        out.clear();
        engine.pumpForTest(2048, &out);
        if (!allSilent(out.data(), out.size()))
            fail("Stop left the click ringing");

        // So does a seek, to tick 26: the next beat (48) is far off.
        engine.stop();
        engine.pumpForTest(512);
        engine.play();
        engine.pumpForTest(uint32_t(grid.sampleAt(24) + 100));
        engine.seek(grid.sampleAt(26));
        out.clear();
        engine.pumpForTest(2048, &out);
        if (!allSilent(out.data(), out.size()))
            fail("a seek left the click ringing");

        // Off mid-click: that click finishes, and no other follows.
        engine.stop();
        engine.pumpForTest(512);
        engine.play();
        engine.pumpForTest(uint32_t(grid.sampleAt(24) + 100));
        engine.setMetronomeEnabled(false);
        out.clear();
        engine.pumpForTest(uint32_t(grid.sampleAt(24 * 4) - grid.sampleAt(24)), &out);
        {
            ClickVoice probe;
            probe.setSampleRate(rate);
            const size_t rest = probe.lengthFrames() - 100;
            if (allSilent(out.data(), 128))
                fail("turning the metronome off cut the ringing click");
            if (!allSilent(out.data() + rest * 2, out.size() - rest * 2))
                fail("the metronome clicked after it was turned off");
        }
        engine.setMetronomeEnabled(true);

        // A song switch cuts the click too (loadSong stops without a
        // transition).
        engine.stop();
        engine.pumpForTest(512);
        engine.play();
        engine.pumpForTest(uint32_t(grid.sampleAt(24) + 100));
        engine.loadSong(tl.get(), &tvg.vg, SongSettings{});
        out.clear();
        engine.pumpForTest(2048, &out);
        if (!allSilent(out.data(), out.size()))
            fail("a song switch left the click ringing");

        engine.unloadSong();
        engine.holdDeviceForTest(false);
    }

    // A loop shorter than the callback: 960 tpb, 4/4, a 120-tick loop
    // [900, 1020) holding beat 960. The tempo sets the loop to the click's
    // length plus a silent gap, so with 8192-frame callbacks every render
    // wraps two or more times and every pass clicks once. Above about 65 kHz
    // a click is too long for that: the loop is then shorter than a click
    // (clicks overlap) and only the click count is checked.
    {
        ClickVoice probe;
        probe.setSampleRate(rate);
        const uint64_t clickLen = probe.lengthFrames();
        uint64_t wantLen = clickLen + 320;
        const bool separable = wantLen * 2 <= 8192;
        if (!separable)
            wantLen = 2000;
        // 120 ticks at 960 tpb is 1/8 beat.
        const uint32_t usPerBeat = uint32_t(double(wantLen) * 8.0 * 1000000.0 / rate + 0.5);
        std::unique_ptr<MidiTimeline> tl =
            MidiTimeline::build(silentLoopSong(960, 900, 1020, usPerBeat), rate);
        const uint64_t ls = tl->loopStartSample, le = tl->loopEndSample, len = le - ls;
        const uint64_t beat = tl->clickGrid.sampleAt(960);
        if (len * 2 > 8192) {
            fail("short loop is %llu frames, too long for the multi-wrap case",
                 (unsigned long long)len);
            return;
        }
        engine.holdDeviceForTest(true);
        engine.loadSong(tl.get(), &tvg.vg, SongSettings{});
        engine.setMetronomeEnabled(true);
        const uint64_t before = engine.clicksQueuedForTest();
        engine.play();
        const int passes = 40;
        const uint64_t total = le + uint64_t(passes - 1) * len;
        std::vector<float> out;
        engine.pumpForTest(uint32_t(total), &out, 8192);
        // Beats 0 and 960 in the first pass, then 960 every later pass.
        const uint64_t want = 1 + uint64_t(passes);
        const uint64_t got = engine.clicksQueuedForTest() - before;
        if (got != want)
            fail("short loop: %llu clicks over %d passes, expected %llu", (unsigned long long)got,
                 passes, (unsigned long long)want);
        // And where: tick 0, then the beat in each pass.
        if (!separable) {
            std::printf("metrocheck: short-loop click positions SKIPPED (a click is %llu frames "
                        "at %.0f Hz, too long to fit a multi-wrap loop)\n",
                        (unsigned long long)clickLen, rate);
        } else {
            const std::vector<Onset> onsets = findOnsets(out, 128, 64);
            if (onsets.size() != want) {
                fail("short loop: %zu clicks heard, expected %llu", onsets.size(),
                     (unsigned long long)want);
            } else {
                for (size_t i = 0; i < onsets.size(); i++) {
                    const uint64_t frame = i == 0 ? 0 : beat + (i - 1) * len;
                    if (onsets[i].frame != frame) {
                        fail("short loop: click %zu at frame %llu, expected %llu", i,
                             (unsigned long long)onsets[i].frame, (unsigned long long)frame);
                        break;
                    }
                }
            }
        }
        engine.stop();
        engine.pumpForTest(512);
        engine.unloadSong();
        engine.holdDeviceForTest(false);
    }

    // The auto-stop, 3 s (audioengine.cpp's kTailSeconds) after a song
    // without a loop ends, comes inside a callback's chunk loop: the clicks
    // that callback queued before it still sound. 240 BPM, 24 tpb, one bar.
    // The stop lands at the end of the first 8192-frame render chunk past
    // the tail; it is pumped as the last chunk of one 8-chunk callback,
    // which at 240 BPM holds at least one beat at any rate up to 192 kHz.
    {
        SmfFile smf = silentLoopSong(24, 0, 96, 250000);
        smf.tracks[0].events.resize(1); // the tempo only: no loop
        std::unique_ptr<MidiTimeline> tl = MidiTimeline::build(smf, rate);
        const ClickGrid &grid = tl->clickGrid;
        const uint64_t tail = tl->lengthSamples + uint64_t(3.0 * rate);
        constexpr uint64_t kRender = 8192, kCallback = kRender * 8;
        const uint64_t stopAt = (tail / kRender + 1) * kRender;
        std::vector<uint64_t> expected;
        for (uint64_t t = 0; grid.sampleAt(t) < stopAt; t += 24)
            expected.push_back(grid.sampleAt(t));
        if (expected.empty() || expected.back() < stopAt - kCallback) {
            fail("auto-stop: no beat in the stopping callback; the case tests nothing");
        } else {
            engine.holdDeviceForTest(true);
            engine.loadSong(tl.get(), &tvg.vg, SongSettings{});
            engine.setMetronomeEnabled(true);
            engine.play();
            std::vector<float> out;
            engine.pumpForTest(uint32_t(stopAt - kCallback), &out, uint32_t(kRender));
            engine.pumpForTest(uint32_t(kCallback), &out, uint32_t(kCallback));
            if (engine.transport() != Transport::Stopped)
                fail("auto-stop: the song is still playing at frame %llu",
                     (unsigned long long)stopAt);
            const std::vector<Onset> onsets = findOnsets(out, 64, 64);
            if (onsets.size() != expected.size()) {
                fail("auto-stop: %zu clicks heard, expected %zu", onsets.size(), expected.size());
            } else {
                for (size_t i = 0; i < onsets.size(); i++) {
                    if (onsets[i].frame != expected[i]) {
                        fail("auto-stop: click %zu at frame %llu, expected %llu", i,
                             (unsigned long long)onsets[i].frame, (unsigned long long)expected[i]);
                        break;
                    }
                }
            }
            engine.unloadSong();
            engine.holdDeviceForTest(false);
        }
    }
}

// The count-in's beat lines against a brute-force walk of the cursor
// meter's grid extended backward (docs/metronome/PLAN.md stance 6).
void checkCountInGrid(double sampleRate)
{
    const uint32_t tpb = 24;
    std::unique_ptr<MidiTimeline> tl = MidiTimeline::build(gridSong(tpb), sampleRate);
    const std::vector<MidiTimeline::TimeSigSegment> segs = tl->timeSigSegments();
    struct Case {
        uint64_t cursor;
        int bars;
    };
    // On tick 0 (the count-in reaches back before the song), mid-bar in
    // 4/4 after the 97 BPM change, mid-bar in 6/8, on the 3/4 change.
    const Case cases[] = {{0, 1}, {0, 2}, {132, 1}, {132, 2}, {240, 1}, {300, 2}, {12, 2}};
    for (const Case &c : cases) {
        const ClickGrid::CountIn ci = tl->clickGrid.countIn(c.cursor, c.bars);
        const MidiTimeline::TimeSigSegment *seg = &segs.front();
        for (const MidiTimeline::TimeSigSegment &s : segs) {
            if (s.tick <= c.cursor)
                seg = &s;
        }
        const TempoPoint *tp = &tl->tempoMap.front();
        for (const TempoPoint &p : tl->tempoMap) {
            if (p.tick <= c.cursor)
                tp = &p;
        }
        const double spt = 60.0 / tp->bpm * sampleRate / double(tpb);
        const int64_t bt = int64_t(seg->beatTicks), bar = bt * seg->beatsPerBar;
        const int64_t length = bar * c.bars;
        if (ci.lengthTicks != uint64_t(length) ||
            std::fabs(double(ci.lengthSamples) - double(length) * spt) > 1.0) {
            fail("count-in at %llu x%d: %llu ticks / %llu samples, expected %lld / ~%.0f",
                 (unsigned long long)c.cursor, c.bars, (unsigned long long)ci.lengthTicks,
                 (unsigned long long)ci.lengthSamples, (long long)length, double(length) * spt);
            continue;
        }
        // Brute force: every count-in tick on the extended grid.
        std::vector<std::pair<int64_t, bool>> want; // count-in tick, accent
        const auto mod = [](int64_t a, int64_t m) { return ((a % m) + m) % m; };
        for (int64_t u = 0; u < length; u++) {
            const int64_t r = int64_t(c.cursor) - length + u - int64_t(seg->tick);
            if (mod(r, bt) == 0)
                want.push_back({u, mod(r, bar) == 0});
        }
        ClickGrid::Click got[256];
        const size_t n = ClickGrid::countInClicksIn(ci, 0, ci.lengthSamples, got, 256);
        if (n != want.size()) {
            fail("count-in at %llu x%d: %zu clicks, expected %zu", (unsigned long long)c.cursor,
                 c.bars, n, want.size());
            continue;
        }
        for (size_t i = 0; i < n; i++) {
            const double at = double(want[i].first) * spt;
            if (std::fabs(double(got[i].sample) - at) > 1.0 || got[i].accent != want[i].second) {
                fail("count-in at %llu x%d: click %zu at %llu accent %d, expected ~%.0f accent %d",
                     (unsigned long long)c.cursor, c.bars, i, (unsigned long long)got[i].sample,
                     int(got[i].accent), at, int(want[i].second));
                break;
            }
        }
        // Chunked queries agree with one.
        std::vector<ClickGrid::Click> chunked;
        for (uint64_t from = 0; from < ci.lengthSamples;) {
            const uint64_t to = std::min<uint64_t>(ci.lengthSamples, from + 777);
            ClickGrid::Click buf[64];
            const size_t m = ClickGrid::countInClicksIn(ci, from, to, buf, 64);
            chunked.insert(chunked.end(), buf, buf + m);
            from = to;
        }
        if (chunked.size() != n)
            fail("count-in at %llu x%d: chunked queries found %zu clicks, one query %zu",
                 (unsigned long long)c.cursor, c.bars, chunked.size(), n);
    }
    // No count-in: nothing.
    if (tl->clickGrid.countIn(96, 0).lengthSamples != 0)
        fail("a 0-bar count-in has a length");
}

// A punch take's pre-roll start (docs/punch/PLAN.md P4): bars of the meter
// in force at punch-in, whatever lies between, clamped at tick 0. gridSong
// at 24 ticks/beat: 4/4 to tick 192 (96-tick bars), 6/8 from 192 (72-tick
// bars), 3/4 from 300 (72-tick bars).
void checkPreRoll(double sampleRate)
{
    std::unique_ptr<MidiTimeline> tl = MidiTimeline::build(gridSong(24), sampleRate);
    struct Case {
        uint64_t in;
        int bars;
        uint64_t want;
    };
    const Case cases[] = {
        {132, 0, 132},                // off: punch-in itself
        {132, 1, 36},                 // 4/4
        {132, 2, 0},                  // clamped at 0 (two bars would be -60)
        {96, 1, 0},                   // exactly one bar: 0, not clamped
        {0, 2, 0},                    // at the song's start
        {240, 1, 168},                // 6/8 bars, though the second crosses into 4/4
        {240, 2, 96},  {300, 1, 228}, // on the 3/4 change: its meter
        {300, 2, 156}, {299, 1, 227}, // a tick before it: 6/8 (also 72 ticks)
        {191, 1, 95},                 // a tick before 6/8: 4/4
        {192, 1, 120},                // on 6/8
        {250, 2, 106},                // mid-bar in 6/8, two bars back into 4/4
        {330, 2, 186},                // mid-bar in 3/4, back across 6/8 into 4/4
        {150, 1, 54},                 // mid-bar in 4/4
    };
    for (const Case &c : cases) {
        const uint64_t got = tl->preRollStartTick(c.in, c.bars);
        if (got != c.want)
            fail("pre-roll before %llu x%d: starts at %llu, expected %llu",
                 (unsigned long long)c.in, c.bars, (unsigned long long)got,
                 (unsigned long long)c.want);
    }
}

// Record's count-in through the engine: clicks at the count-in's offsets
// with the metronome off, the song held at the cursor until then and
// starting sample-exactly after, the song's clicks carrying on from the
// count-in's with the metronome on, the capture beginning at the song's
// start with only the keys still sounding then, and Stop cancelling.
void checkCountInEngine()
{
    AudioEngine engine;
    QString error;
    if (!engine.init(&error)) {
        std::printf("metrocheck: count-in part SKIPPED (no audio device: %s)\n",
                    qUtf8Printable(error));
        return;
    }
    const double rate = engine.sampleRate();
    TestVoicegroup tvg;
    std::unique_ptr<MidiTimeline> tl = MidiTimeline::build(silentLoopSong(24, 960, 1920), rate);
    const ClickGrid &grid = tl->clickGrid;
    // Mid-bar, between beats 120 and 144.
    const uint64_t cursorTick = 132;
    const uint64_t target = tl->sampleForTick(cursorTick);
    if (target != grid.sampleAt(cursorTick))
        fail("sampleForTick(%llu) = %llu, not the click grid's %llu",
             (unsigned long long)cursorTick, (unsigned long long)target,
             (unsigned long long)grid.sampleAt(cursorTick));
    const ClickGrid::CountIn ci = grid.countIn(cursorTick, 1);
    const uint64_t length = ci.lengthSamples;

    engine.holdDeviceForTest(true);
    engine.loadSong(tl.get(), &tvg.vg, SongSettings{});
    engine.setOutputGain(1.0f);
    engine.setMetronomeVolume(1.0f);

    // Metronome off: only the count-in clicks.
    {
        engine.setMetronomeEnabled(false);
        engine.playWithCountIn(target, ci);
        if (!engine.countingIn())
            fail("playWithCountIn: not counting in");
        std::vector<float> out;
        engine.pumpForTest(uint32_t(length - 1), &out);
        if (!engine.countingIn())
            fail("the count-in ended a frame early");
        if (engine.playheadSamples() != target)
            fail("during the count-in the playhead is at %llu, not the cursor %llu",
                 (unsigned long long)engine.playheadSamples(), (unsigned long long)target);
        const uint32_t after = 3000;
        engine.pumpForTest(uint32_t(after + 1), &out);
        if (engine.countingIn())
            fail("the count-in did not end");
        if (engine.playheadSamples() != target + after)
            fail("the song is at %llu after the count-in plus %u frames, expected %llu",
                 (unsigned long long)engine.playheadSamples(), after,
                 (unsigned long long)(target + after));
        ClickGrid::Click want[64];
        const size_t n = ClickGrid::countInClicksIn(ci, 0, length, want, 64);
        const std::vector<Onset> onsets = findOnsets(out, 64, 400);
        if (onsets.size() != n) {
            fail("count-in: %zu clicks heard, %zu expected", onsets.size(), n);
        } else {
            for (size_t i = 0; i < n; i++) {
                const bool accent =
                    onsets[i].peak > (ClickVoice::kAccentPeak + ClickVoice::kBeatPeak) / 2;
                if (onsets[i].frame != want[i].sample || accent != want[i].accent)
                    fail("count-in click %zu at frame %llu accent %d, expected %llu accent %d", i,
                         (unsigned long long)onsets[i].frame, int(accent),
                         (unsigned long long)want[i].sample, int(want[i].accent));
            }
        }
        std::vector<float> tap(size_t(engine.tap().capacity()) * 2);
        engine.tap().readLatest(tap.data(), engine.tap().capacity());
        if (!allSilent(tap.data(), tap.size()))
            fail("the audio tap heard the count-in");
        engine.stop();
        engine.pumpForTest(512);
    }

    // Metronome on: the song's first click (beat 144) follows the count-in's
    // last (beat 120) one beat later.
    {
        engine.setMetronomeEnabled(true);
        engine.playWithCountIn(target, ci);
        std::vector<float> out;
        engine.pumpForTest(uint32_t(length + (grid.sampleAt(168) - target)), &out);
        const std::vector<Onset> onsets = findOnsets(out, 64, 400);
        const uint64_t firstSong = length + (grid.sampleAt(144) - target);
        if (onsets.size() < 2 || onsets.back().frame != firstSong) {
            fail("the song's first click is at frame %llu, expected %llu",
                 onsets.empty() ? 0ull : (unsigned long long)onsets.back().frame,
                 (unsigned long long)firstSong);
        } else {
            const uint64_t gap = onsets.back().frame - onsets[onsets.size() - 2].frame;
            const double beat = 24 * ci.samplesPerTick;
            if (std::fabs(double(gap) - beat) > 2.0)
                fail("count-in to song: %llu frames between clicks, a beat is %.1f",
                     (unsigned long long)gap, beat);
        }
        engine.stop();
        engine.pumpForTest(512);
    }

    // Capture: key 60 pressed during the count-in and held, key 62 pressed
    // and released during it, key 64 pressed after the song starts.
    {
        engine.setMetronomeEnabled(false);
        MidiInputRing ring;
        engine.setMidiInputRing(&ring);
        engine.setLiveTarget(-1);
        engine.armCapture(41);
        engine.playWithCountIn(target, ci);
        engine.pumpForTest(512);
        ring.push({0x90, 62, 70, 0});
        engine.pumpForTest(uint32_t(length / 3));
        ring.push({0x90, 60, 90, 0});
        engine.pumpForTest(uint32_t(length / 3));
        ring.push({0x80, 62, 0, 0});
        engine.pumpForTest(uint32_t(length - 512 - 2 * (length / 3) + 1000));
        ring.push({0x90, 64, 50, 0});
        engine.pumpForTest(1024);
        ring.push({0x80, 60, 0, 0});
        ring.push({0x80, 64, 0, 0});
        engine.pumpForTest(1024);
        engine.armCapture(0);
        std::vector<CaptureEvent> evs;
        CaptureEvent ev;
        while (engine.popCapture(&ev))
            evs.push_back(ev);
        const auto find = [&](uint8_t kind, uint8_t key) -> const CaptureEvent * {
            for (const CaptureEvent &e : evs) {
                if (e.generation == 41 && e.kind == kind &&
                    (kind == CaptureEvent::Begin || e.key == key))
                    return &e;
            }
            return nullptr;
        };
        const CaptureEvent *begin = find(CaptureEvent::Begin, 0);
        if (!begin || begin->samplePos != target)
            fail("the take begins at %lld, expected the cursor %llu",
                 begin ? (long long)begin->samplePos : -1LL, (unsigned long long)target);
        if (!evs.empty() && evs.front().kind != CaptureEvent::Begin)
            fail("the take's first event is kind %d, not its Begin", int(evs.front().kind));
        const CaptureEvent *held = find(CaptureEvent::NoteOn, 60);
        if (!held || held->samplePos != target || held->velocity != 90 || held->lateSamples != 0)
            fail("the key held through the count-in records at %lld vel %d, expected %llu vel 90",
                 held ? (long long)held->samplePos : -1LL, held ? int(held->velocity) : -1,
                 (unsigned long long)target);
        if (find(CaptureEvent::NoteOn, 62) || find(CaptureEvent::NoteOff, 62))
            fail("a key released during the count-in was recorded");
        const CaptureEvent *later = find(CaptureEvent::NoteOn, 64);
        if (!later || later->samplePos <= target)
            fail("a key pressed after the song started was not recorded after the cursor");
        if (!find(CaptureEvent::NoteOff, 60))
            fail("the held key's release was not recorded");
        engine.stop();
        engine.pumpForTest(512);
        engine.setMidiInputRing(nullptr);
    }

    // Stop during the count-in cancels it and its take; a later Play has none.
    {
        engine.armCapture(42);
        engine.playWithCountIn(target, ci);
        engine.pumpForTest(uint32_t(length / 2));
        engine.stop();
        engine.pumpForTest(512);
        if (engine.countingIn())
            fail("Stop left the count-in pending");
        engine.armCapture(0);
        CaptureEvent ev;
        bool begun = false;
        while (engine.popCapture(&ev))
            begun |= ev.generation == 42 && ev.kind == CaptureEvent::Begin;
        if (begun)
            fail("a take cancelled during its count-in began");
        engine.seek(target);
        engine.play();
        engine.pumpForTest(1024);
        if (engine.playheadSamples() != target + 1024)
            fail("Play after a cancelled count-in did not start at once");
        engine.stop();
        engine.pumpForTest(512);
        // Stopped before the audio thread ever saw the request.
        engine.playWithCountIn(target, ci);
        engine.stop();
        engine.seek(target);
        engine.play();
        engine.pumpForTest(1024);
        if (engine.countingIn() || engine.playheadSamples() != target + 1024)
            fail("a count-in stopped before it began came back on the next Play");
        engine.stop();
        engine.pumpForTest(512);
    }

    // Stop then Record inside one period, from a playing song: the audio
    // thread never sees Stopped, and still counts in.
    {
        engine.seek(target);
        engine.play();
        engine.pumpForTest(1024);
        engine.stop();
        engine.playWithCountIn(target, ci);
        engine.pumpForTest(1024);
        if (!engine.countingIn() || engine.playheadSamples() != target)
            fail("Stop then Record inside one period did not count in (playhead %llu)",
                 (unsigned long long)engine.playheadSamples());
        // And Stop then Play inside one period during a count-in ends it.
        engine.stop();
        engine.seek(target);
        engine.play();
        engine.pumpForTest(1024);
        if (engine.countingIn() || engine.playheadSamples() != target + 1024)
            fail("Stop then Play inside one period kept the count-in (playhead %llu)",
                 (unsigned long long)engine.playheadSamples());
        engine.stop();
        engine.pumpForTest(512);
    }

    engine.unloadSong();
    engine.holdDeviceForTest(false);
}

// A song with no loop: 120 BPM, then 90 BPM from tick `changeTick`.
SmfFile tempoChangeSong(uint32_t tpb, uint64_t changeTick)
{
    SmfFile smf;
    smf.format = 1;
    smf.division = uint16_t(tpb);
    smf.tracks.resize(2);
    SmfTrack &c = smf.tracks[0];
    c.events.push_back(tempoEvent(0, 120.0));
    c.events.push_back(tempoEvent(changeTick, 90.0));
    c.endTick = tpb * 40;
    smf.tracks[1].events.push_back(channelEvent(0, 0xC0, 0, 0));
    smf.tracks[1].endTick = tpb * 40;
    return smf;
}

// The count-in's edges through the engine: a tempo change on the cursor's
// own tick, a seek during the count-in, and a start past the loop end.
void checkCountInEdges()
{
    AudioEngine engine;
    QString error;
    if (!engine.init(&error)) {
        std::printf("metrocheck: count-in edges SKIPPED (no audio device: %s)\n",
                    qUtf8Printable(error));
        return;
    }
    const double rate = engine.sampleRate();
    TestVoicegroup tvg;
    engine.holdDeviceForTest(true);
    engine.setOutputGain(1.0f);
    engine.setMetronomeVolume(1.0f);

    // A tempo change exactly on the cursor (a bar line): the count-in runs
    // at the new tempo, and the song's first click (the cursor's beat, M9)
    // follows its last one beat of the new tempo later.
    {
        std::unique_ptr<MidiTimeline> tl = MidiTimeline::build(tempoChangeSong(24, 96), rate);
        engine.loadSong(tl.get(), &tvg.vg, SongSettings{});
        const uint64_t target = tl->sampleForTick(96);
        const ClickGrid::CountIn ci = tl->clickGrid.countIn(96, 1);
        const double beat = 24 * (60.0 / 90.0 * rate / 24.0);
        if (std::fabs(ci.samplesPerTick * 24 - beat) > 0.05) // the tempo's rounded µs
            fail("tempo on the cursor: the count-in's beat is %.2f samples, 90 BPM's is %.2f",
                 ci.samplesPerTick * 24, beat);
        engine.setMetronomeEnabled(true);
        engine.playWithCountIn(target, ci);
        std::vector<float> out;
        engine.pumpForTest(uint32_t(ci.lengthSamples + uint64_t(beat) + 200), &out);
        const std::vector<Onset> onsets = findOnsets(out, 64, 400);
        if (onsets.size() != 6) {
            fail("tempo on the cursor: %zu clicks heard, expected 4 + 2", onsets.size());
        } else {
            if (onsets[4].frame != ci.lengthSamples ||
                onsets[4].peak < (ClickVoice::kAccentPeak + ClickVoice::kBeatPeak) / 2)
                fail("tempo on the cursor: the song's first click at frame %llu, expected an "
                     "accent at %llu",
                     (unsigned long long)onsets[4].frame, (unsigned long long)ci.lengthSamples);
            for (size_t i = 1; i < onsets.size(); i++) {
                const double gap = double(onsets[i].frame - onsets[i - 1].frame);
                if (std::fabs(gap - beat) > 2.0)
                    fail("tempo on the cursor: %.0f frames between clicks %zu and %zu, a beat "
                         "is %.1f",
                         gap, i - 1, i, beat);
            }
        }
        engine.stop();
        engine.pumpForTest(512);
        engine.unloadSong();
    }

    std::unique_ptr<MidiTimeline> tl = MidiTimeline::build(silentLoopSong(24, 960, 1920), rate);
    engine.loadSong(tl.get(), &tvg.vg, SongSettings{});
    engine.setMetronomeEnabled(false);
    engine.setLoopEnabled(true);

    // A seek during the count-in moves where the song will start; the
    // count-in runs its length all the same, then the song starts there.
    {
        const uint64_t target = tl->sampleForTick(132);
        const ClickGrid::CountIn ci = tl->clickGrid.countIn(132, 1);
        const uint64_t moved = tl->sampleForTick(480);
        engine.playWithCountIn(target, ci);
        engine.pumpForTest(uint32_t(ci.lengthSamples / 2));
        engine.seek(moved);
        engine.pumpForTest(uint32_t(ci.lengthSamples - ci.lengthSamples / 2 - 1));
        if (!engine.countingIn() || engine.playheadSamples() != moved)
            fail("seek during the count-in: counting in %d, playhead %llu (expected %llu)",
                 int(engine.countingIn()), (unsigned long long)engine.playheadSamples(),
                 (unsigned long long)moved);
        engine.pumpForTest(1001);
        if (engine.countingIn() || engine.playheadSamples() != moved + 1000)
            fail("seek during the count-in: the song is at %llu after the count-in plus 1000 "
                 "frames, expected %llu",
                 (unsigned long long)engine.playheadSamples(), (unsigned long long)(moved + 1000));
        engine.stop();
        engine.pumpForTest(512);
    }

    // A seek or a timeline rebuild during the count-in silences the keys
    // held then: the take doesn't carry them. A key pressed after it is.
    {
        const uint64_t target = tl->sampleForTick(132);
        const ClickGrid::CountIn ci = tl->clickGrid.countIn(132, 1);
        const uint64_t moved = tl->sampleForTick(480);
        std::unique_ptr<MidiTimeline> rebuilt =
            MidiTimeline::build(silentLoopSong(24, 960, 1920), rate);
        MidiInputRing ring;
        engine.setMidiInputRing(&ring);
        engine.setLiveTarget(-1);
        for (const bool rebuild : {false, true}) {
            const uint32_t generation = rebuild ? 45 : 44;
            const char *what = rebuild ? "a timeline update" : "a seek";
            engine.armCapture(generation);
            engine.playWithCountIn(target, ci);
            engine.pumpForTest(512);
            ring.push({0x90, 60, 90, 0});
            engine.pumpForTest(uint32_t(ci.lengthSamples / 2));
            if (rebuild)
                engine.updateTimeline(rebuilt.get());
            else
                engine.seek(moved);
            engine.pumpForTest(512);
            ring.push({0x90, 62, 80, 0});
            engine.pumpForTest(uint32_t(ci.lengthSamples - ci.lengthSamples / 2 - 1024 + 1000));
            ring.push({0x80, 60, 0, 0});
            ring.push({0x80, 62, 0, 0});
            engine.pumpForTest(1024);
            engine.armCapture(0);
            bool begun = false, silenced = false, pressed = false;
            CaptureEvent ev;
            while (engine.popCapture(&ev)) {
                if (ev.generation != generation)
                    continue;
                begun |= ev.kind == CaptureEvent::Begin;
                if (ev.kind == CaptureEvent::NoteOn)
                    (ev.key == 60 ? silenced : pressed) |= (ev.flags & CaptureEvent::Carried) != 0;
            }
            if (!begun || silenced || !pressed)
                fail("%s during the count-in: Begin %d, silenced key carried %d, key pressed "
                     "after it carried %d",
                     what, int(begun), int(silenced), int(pressed));
            engine.stop();
            engine.pumpForTest(512);
            if (rebuild)
                engine.updateTimeline(tl.get());
        }
        engine.setMidiInputRing(nullptr);
    }

    // Held past the loop end (a start the UI didn't move, or a seek during
    // the count-in): the song holds there, unwrapped, until it starts; then
    // it wraps, and the take begins at the loop start with the held key and
    // no Boundary.
    {
        const uint64_t target = tl->sampleForTick(1968);
        const ClickGrid::CountIn ci = tl->clickGrid.countIn(1968, 1);
        MidiInputRing ring;
        engine.setMidiInputRing(&ring);
        engine.setLiveTarget(-1);
        engine.armCapture(43);
        engine.playWithCountIn(target, ci);
        engine.pumpForTest(512);
        ring.push({0x90, 60, 90, 0});
        engine.pumpForTest(uint32_t(ci.lengthSamples - 512 - 1));
        if (!engine.countingIn() || engine.playheadSamples() != target)
            fail("past the loop end: during the count-in the playhead is at %llu, not held at "
                 "%llu",
                 (unsigned long long)engine.playheadSamples(), (unsigned long long)target);
        engine.pumpForTest(1001);
        if (engine.countingIn() || engine.playheadSamples() != tl->loopStartSample + 1000)
            fail("past the loop end: the song is at %llu after the count-in plus 1000 frames, "
                 "expected the loop start + 1000 (%llu)",
                 (unsigned long long)engine.playheadSamples(),
                 (unsigned long long)(tl->loopStartSample + 1000));
        ring.push({0x80, 60, 0, 0});
        engine.pumpForTest(1024);
        engine.armCapture(0);
        bool begun = false, held = false, boundary = false;
        CaptureEvent ev;
        while (engine.popCapture(&ev)) {
            if (ev.generation != 43)
                continue;
            if (ev.kind == CaptureEvent::Begin)
                begun = ev.samplePos == tl->loopStartSample;
            if (ev.kind == CaptureEvent::NoteOn && ev.key == 60)
                held = ev.samplePos == tl->loopStartSample && (ev.flags & CaptureEvent::Carried);
            boundary |= ev.kind == CaptureEvent::Boundary;
        }
        if (!begun || !held || boundary)
            fail("past the loop end: Begin at the loop start %d, carried key there %d, "
                 "Boundary %d",
                 int(begun), int(held), int(boundary));
        engine.stop();
        engine.pumpForTest(512);
        engine.setMidiInputRing(nullptr);
    }

    engine.unloadSong();
    engine.holdDeviceForTest(false);
}

} // namespace

int runMetroCheck()
{
    for (const uint32_t tpb : {24u, 48u, 96u}) {
        for (const double rate : {48000.0, 44100.0})
            checkGrid(tpb, rate);
    }
    checkEngine();
    for (const double rate : {48000.0, 44100.0})
        checkCountInGrid(rate);
    checkPreRoll(48000.0);
    checkCountInEngine();
    checkCountInEdges();
    if (g_failures) {
        std::fprintf(stderr, "metrocheck: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("metrocheck: PASS\n");
    return 0;
}
