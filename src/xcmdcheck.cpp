#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
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

// --xcmdcheck: pseudo-echo XCMD playback check, fully self-contained (no
// project needed). mid2agb turns a CC 0x1E (select) + CC 0x1D/0x1F (fire)
// pair into one XCMD xIECV / xIECL command while it compiles, so the pairing
// is compile-time state: it never rewinds at a loop or a seek, and the
// selector stash leaks from one printed track into the next. Synthesizes
// songs for each of those rules and asserts on the engine's track state
// after chase and after a looped render, plus an end-to-end render proving a
// released note rings on at the echo volume for the echo length.
//
// With a directory argument it also checks the pairing against mid2agb
// itself (see checkCorpus).

namespace {

// 24 ticks per quarter, 120 BPM at 48kHz: one tick is exactly 1000 samples.
constexpr uint32_t kDivision = 24;
constexpr double kSampleRate = 48000.0;
constexpr uint64_t kSamplesPerTick = 1000;

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

SmfFile emptySong(int chunks, uint64_t endTick)
{
    SmfFile smf;
    smf.format = 1;
    smf.division = kDivision;
    smf.tracks.resize(size_t(chunks));
    smf.tracks[0].events.push_back(metaEvent(0, 0x51, QByteArray("\x07\xA1\x20", 3)));
    for (SmfTrack &track : smf.tracks)
        track.endTick = endTick;
    return smf;
}

void addNote(SmfTrack &track, uint8_t channel, uint64_t on, uint64_t off, uint8_t key = 60)
{
    track.events.push_back(channelEvent(on, 0x90 | channel, key, 100));
    track.events.push_back(channelEvent(off, 0x80 | channel, key, 0));
}

void addXcmd(SmfTrack &track, uint8_t channel, uint64_t tick, int select, uint8_t fireCc,
             uint8_t value)
{
    if (select >= 0)
        track.events.push_back(channelEvent(tick, 0xB0 | channel, 0x1E, uint8_t(select)));
    track.events.push_back(channelEvent(tick, 0xB0 | channel, fireCc, value));
}

// One looped PCM square wave on every program, with a release quick enough
// that a note is long silent by the time the echo assertions listen.
struct TestVoicegroup {
    int8_t sample[65]; // 64 + the guard byte the interpolating mixer reads
    WaveData wave;
    ToneData voices[128];

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
            v.attack = 255;
            v.decay = 0;
            v.sustain = 255;
            v.release = 128; // halves every frame: gone in well under 0.2s
        }
    }
};

int checkEcho(const M4AEngine &engine, int track, int volume, int length, const char *what)
{
    const M4ATrack &t = engine.tracks[track];
    if (t.pseudoEchoVolume == volume && t.pseudoEchoLength == length)
        return 0;
    std::fprintf(stderr,
                 "xcmdcheck: FAIL: %s: track %d pseudo-echo is vol %d len %d, expected vol %d "
                 "len %d\n",
                 what, track, t.pseudoEchoVolume, t.pseudoEchoLength, volume, length);
    return 1;
}

// Resolution rules, read back through chase at the end of the song.
int checkResolution()
{
    SmfFile smf = emptySong(6, 96);

    // Engine track 0: both commands on one tick, one through each firing CC.
    addXcmd(smf.tracks[1], 0, 0, 8, 0x1D, 40);
    addXcmd(smf.tracks[1], 0, 0, 9, 0x1F, 20);
    addNote(smf.tracks[1], 0, 0, 24);

    // Engine track 1: fires without selecting — inherits track 0's last
    // selector (xIECL), the way mid2agb's stash leaks across tracks.
    addXcmd(smf.tracks[2], 1, 0, -1, 0x1D, 30);
    addNote(smf.tracks[2], 1, 0, 24);

    // Engine track 2: selects but plays no note. mid2agb never prints it, so
    // its selector never reaches the stash (and its own fire does nothing
    // anyone hears, but still resolves nothing here).
    addXcmd(smf.tracks[3], 2, 0, 8, 0x1D, 77);

    // Engine track 3: fires without selecting — still xIECL, not track 2's
    // xIECV. Then a selector mid2agb has no command for: the fire is dropped.
    addXcmd(smf.tracks[4], 3, 0, -1, 0x1D, 50);
    addXcmd(smf.tracks[4], 3, 12, 4, 0x1D, 99);
    addNote(smf.tracks[4], 3, 0, 24);

    // Engine track 4: inherits the dead selector, so nothing; then a late
    // change a seek has to see.
    addXcmd(smf.tracks[5], 4, 0, -1, 0x1D, 66);
    addNote(smf.tracks[5], 4, 0, 24);
    addXcmd(smf.tracks[5], 4, 48, 8, 0x1D, 12);

    const auto timeline = MidiTimeline::build(smf, kSampleRate);
    if (!timeline || timeline->usedTrackCount != 5) {
        std::fprintf(stderr, "xcmdcheck: FAIL: resolution song built wrong\n");
        return 1;
    }

    int failures = 0;
    M4AEngine engine;
    m4a_engine_init(&engine, float(kSampleRate));

    TimelinePlayer::chase(&engine, timeline.get(), 96 * kSamplesPerTick);
    failures += checkEcho(engine, 0, 40, 20, "select + fire pairs");
    failures += checkEcho(engine, 1, 0, 30, "selector inherited from the previous track");
    failures += checkEcho(engine, 2, 0, 0, "note-less track");
    failures += checkEcho(engine, 3, 0, 50, "note-less track's selector must not leak");
    failures += checkEcho(engine, 4, 12, 0, "unknown selector drops the fire");

    // Seeking back before the late change drops it again; chase also clears
    // what a previous position left behind.
    TimelinePlayer::chase(&engine, timeline.get(), 24 * kSamplesPerTick);
    failures += checkEcho(engine, 4, 0, 0, "chase before the late change");

    m4a_engine_destroy(&engine);
    return failures;
}

// The selector is compile-time state: after a loop wrap the fire at the loop
// start is still the xIECV it was compiled as, even though the last selector
// seen before the wrap was xIECL.
int checkLoopWrap()
{
    SmfFile smf = emptySong(2, 96);
    SmfTrack &t = smf.tracks[1];
    t.events.push_back(metaEvent(24, 0x06, QByteArray("[")));
    t.events.push_back(metaEvent(72, 0x06, QByteArray("]")));
    addXcmd(t, 0, 0, 8, 0x1D, 40);
    addNote(t, 0, 0, 12);
    addXcmd(t, 0, 24, -1, 0x1D, 60); // xIECV = 60, at the loop start
    addXcmd(t, 0, 48, 9, 0x1D, 11);  // xIECL = 11
    std::stable_sort(t.events.begin(), t.events.end(),
                     [](const SmfEvent &a, const SmfEvent &b) { return a.tick < b.tick; });

    const auto timeline = MidiTimeline::build(smf, kSampleRate);
    if (!timeline || !timeline->hasLoop()) {
        std::fprintf(stderr, "xcmdcheck: FAIL: loop song built wrong\n");
        return 1;
    }

    M4AEngine engine;
    m4a_engine_init(&engine, float(kSampleRate));
    TimelinePlayer::chase(&engine, timeline.get(), 0);
    TimelinePlayer player;
    player.reset();

    // Through the loop end (tick 72) and 6 ticks into the second pass.
    const uint32_t frames = uint32_t((72 + 6) * kSamplesPerTick);
    std::vector<float> bufL(frames), bufR(frames);
    player.render(&engine, timeline.get(), bufL.data(), bufR.data(), frames, true, 0);

    const int failures = checkEcho(engine, 0, 60, 11, "fire replayed after the loop wrap");
    m4a_engine_destroy(&engine);
    return failures;
}

// A track whose only note has no length is never printed, so its selector
// must not reach the next track.
int checkZeroLengthNoteTrack()
{
    SmfFile smf = emptySong(4, 96);
    addXcmd(smf.tracks[1], 0, 0, 9, 0x1D, 20);
    addNote(smf.tracks[1], 0, 0, 24);
    addXcmd(smf.tracks[2], 1, 0, 8, 0x1D, 77);
    addNote(smf.tracks[2], 1, 0, 0);
    addXcmd(smf.tracks[3], 2, 0, -1, 0x1D, 50);
    addNote(smf.tracks[3], 2, 0, 24);

    const auto timeline = MidiTimeline::build(smf, kSampleRate);
    if (!timeline)
        return 1;
    M4AEngine engine;
    m4a_engine_init(&engine, float(kSampleRate));
    TimelinePlayer::chase(&engine, timeline.get(), 96 * kSamplesPerTick);
    const int failures =
        checkEcho(engine, 2, 0, 50, "selector of a track with only a zero-length note");
    m4a_engine_destroy(&engine);
    return failures;
}

// Peak of `frames` samples `skip` samples further into the song.
float peakAfter(TimelinePlayer &player, M4AEngine *engine, const MidiTimeline *timeline,
                uint32_t skip, uint32_t frames)
{
    std::vector<float> bufL(skip + frames), bufR(skip + frames);
    player.render(engine, timeline, bufL.data(), bufR.data(), skip + frames, false, 0);
    float peak = 0.0f;
    for (uint32_t i = skip; i < skip + frames; i++)
        peak = std::max({peak, std::fabs(bufL[i]), std::fabs(bufR[i])});
    return peak;
}

// End to end: a released note rings on at the echo volume for the echo
// length (30 frames = 0.5s), then stops. Without the XCMDs it is gone as soon
// as its release runs out.
int checkAudibleEcho(bool withEcho)
{
    SmfFile smf = emptySong(2, 96);
    SmfTrack &t = smf.tracks[1];
    t.events.push_back(channelEvent(0, 0xC0, 0, 0));
    if (withEcho) {
        addXcmd(t, 0, 0, 8, 0x1D, 80);
        addXcmd(t, 0, 0, 9, 0x1D, 30);
    }
    addNote(t, 0, 0, 12);

    const auto timeline = MidiTimeline::build(smf, kSampleRate);
    if (!timeline)
        return 1;
    TestVoicegroup vg;
    M4AEngine engine;
    m4a_engine_init(&engine, float(kSampleRate));
    m4a_engine_set_voicegroup(&engine, vg.voices);
    TimelinePlayer::chase(&engine, timeline.get(), 0);
    TimelinePlayer player;
    player.reset();

    // Note-off at 12000 samples. Listen 0.3s after it, then 1s after it.
    const float during = peakAfter(player, &engine, timeline.get(), 12000 + 14400, 2400);
    const float after = peakAfter(player, &engine, timeline.get(), 31200, 2400);
    m4a_engine_destroy(&engine);

    int failures = 0;
    if (withEcho != (during > 0.0f)) {
        std::fprintf(stderr, "xcmdcheck: FAIL: 0.3s after note-off the %s note is %s\n",
                     withEcho ? "echoing" : "plain", during > 0.0f ? "audible" : "silent");
        failures++;
    }
    if (after > 0.0f) {
        std::fprintf(stderr, "xcmdcheck: FAIL: %s note still audible 1s after note-off\n",
                     withEcho ? "echoing" : "plain");
        failures++;
    }
    return failures;
}

using XcmdList = std::vector<std::pair<int, int>>; // (command, value)

// The XCMDs of each track mid2agb printed, in print order.
std::vector<XcmdList> xcmdsInAssembly(const QString &path)
{
    std::vector<XcmdList> tracks;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return tracks;
    // mid2agb's banner, not the track label: a song named like se_foo_1 has a
    // header label shaped exactly like one.
    static const QRegularExpression trackBanner(QStringLiteral("^@\\*+ Track \\d+ "));
    static const QRegularExpression xcmd(QStringLiteral("xIEC([VL])\\s*,\\s*(\\d+)"));
    while (!file.atEnd()) {
        const QString line = QString::fromLatin1(file.readLine());
        if (trackBanner.match(line).hasMatch()) {
            tracks.emplace_back();
            continue;
        }
        const QRegularExpressionMatch m = xcmd.match(line);
        if (m.hasMatch() && !tracks.empty())
            tracks.back().push_back(
                {m.captured(1) == QLatin1String("V") ? M4A_XCMD_IECV : M4A_XCMD_IECL,
                 m.captured(2).toInt()});
    }
    return tracks;
}

// Every X.mid in the directory with an X.s beside it, compiled with -N so no
// PATT call folds a repeated measure's XCMDs away: each track's resolved
// commands must be exactly the XCMD lines mid2agb printed for it. Expects
// one channel per chunk, like every decomp song; mid2agb prints a track only
// when it plays a note.
int checkCorpus(const QString &dirPath)
{
    int failures = 0, songs = 0, commands = 0;
    const QDir dir(dirPath);
    for (const QString &name : dir.entryList({QStringLiteral("*.mid")}, QDir::Files, QDir::Name)) {
        const QString asmPath = dir.filePath(name.chopped(4) + QStringLiteral(".s"));
        if (!QFile::exists(asmPath))
            continue;
        QString error;
        const auto timeline = MidiTimeline::load(dir.filePath(name), kSampleRate, &error);
        if (!timeline) {
            std::fprintf(stderr, "xcmdcheck: FAIL: %s: %s\n", qPrintable(name), qPrintable(error));
            failures++;
            continue;
        }
        std::vector<XcmdList> ours;
        for (int track = 0; track < timeline->usedTrackCount; track++) {
            if (timeline->tracks[track].noteCount == 0)
                continue;
            ours.emplace_back();
            for (const TimelineEvent &ev : timeline->events)
                if (ev.type == TIMELINE_EVT_XCMD && ev.track == track)
                    ours.back().push_back({ev.data0, ev.data1});
        }
        const std::vector<XcmdList> theirs = xcmdsInAssembly(asmPath);
        songs++;
        if (ours.size() != theirs.size()) {
            std::fprintf(stderr, "xcmdcheck: FAIL: %s: %zu tracks here, %zu in the .s\n",
                         qPrintable(name), ours.size(), theirs.size());
            failures++;
            continue;
        }
        for (size_t track = 0; track < ours.size(); track++) {
            commands += int(ours[track].size());
            if (ours[track] != theirs[track]) {
                std::fprintf(stderr,
                             "xcmdcheck: FAIL: %s track %zu: %zu XCMDs resolved, mid2agb "
                             "printed %zu (or they differ)\n",
                             qPrintable(name), track + 1, ours[track].size(), theirs[track].size());
                failures++;
            }
        }
    }
    std::fprintf(stderr, "xcmdcheck: corpus: %d songs, %d XCMDs compared\n", songs, commands);
    if (songs == 0) {
        std::fprintf(stderr, "xcmdcheck: FAIL: no .mid/.s pairs in %s\n", qPrintable(dirPath));
        failures++;
    }
    return failures;
}

} // namespace

int runXcmdCheck(const QString &corpusDir)
{
    int failures = 0;
    failures += checkResolution();
    failures += checkLoopWrap();
    failures += checkZeroLengthNoteTrack();
    failures += checkAudibleEcho(false);
    failures += checkAudibleEcho(true);
    if (!corpusDir.isEmpty())
        failures += checkCorpus(corpusDir);

    if (failures == 0)
        std::fprintf(stderr, "xcmdcheck: PASS\n");
    return failures == 0 ? 0 : 1;
}
