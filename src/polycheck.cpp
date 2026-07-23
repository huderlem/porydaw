#include <QCoreApplication>
#include <QDockWidget>
#include <QFileInfo>
#include <QImage>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/miditimeline.h"
#include "core/smf.h"
#include "core/timelineplayer.h"
#include "mainwindow.h"
#include "ui/polyphonypanel.h"

extern "C" {
#include "m4a_engine.h"
}

// --polycheck: polyphony-overflow debugger check, fully self-contained (no
// project needed). Stage A synthesizes a song that forces every overflow kind
// against maxPcmChannels = 1 and asserts the engine's per-track counters and
// tick-stamped event ring, plus the invert (solo overflow) mode's audibility:
// silent before the first overflow, the lost sound audible on the shadow pool
// after it. Stage B drives the PolyphonyPanel offscreen: bar:beat formatting
// across a time-signature change, track/voice names, double-click jump ticks
// (with the live-note sentinel suppressed), reset rebase, and the log cap.
// Stage C constructs a real MainWindow (redirected QSettings, no project)
// and asserts the solo-overflow gate: the engine only inverts while the
// checkbox is checked AND the Polyphony dock is visible, so closing the
// dock never leaves playback silently inverted.

namespace {

// 24 ticks per quarter, 4/4, 120 BPM at 48kHz: one tick is exactly 1000
// samples (same arithmetic as loopcheck).
constexpr uint32_t kDivision = 24;
constexpr double kSampleRate = 48000.0;
constexpr uint64_t kSamplesPerTick = 1000;

// Overflow script, all against a 1-channel PCM pool (equal priorities):
//  tick 24: track 1 starts a long note -> takes the only channel
//  tick 96: track 0's note steals it (occupant track 1 >= 0) -> STOLEN
//  tick 120: track 2's note finds occupant track 0 < 2 -> DROPPED
//  tick 144: track 0 releases; tick 148: track 3 reuses the fading channel
//            -> TAIL_CUT
constexpr uint64_t kHoldOnTick = 24, kHoldOffTick = 216;
constexpr uint64_t kStealTick = 96;
constexpr uint64_t kDropTick = 120;
constexpr uint64_t kStealOffTick = 144;
constexpr uint64_t kTailCutTick = 148;
constexpr uint8_t kHoldKey = 62, kStealKey = 60, kDropKey = 64, kTailKey = 65;

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

SmfFile buildOverflowSong()
{
    SmfFile smf;
    smf.format = 1;
    smf.division = kDivision;
    smf.tracks.resize(5);

    SmfTrack &conductor = smf.tracks[0];
    conductor.events.push_back(metaEvent(0, 0x51, QByteArray("\x07\xA1\x20", 3)));
    conductor.endTick = 384;

    // One SMF chunk per engine track so the script's track indices hold.
    struct NoteSpec {
        uint64_t on, off;
        uint8_t key;
    };
    const NoteSpec specs[4] = {
        {kStealTick, kStealOffTick, kStealKey},  // engine track 0
        {kHoldOnTick, kHoldOffTick, kHoldKey},   // engine track 1
        {kDropTick, 200, kDropKey},              // engine track 2
        {kTailCutTick, kHoldOffTick, kTailKey},  // engine track 3
    };
    for (int t = 0; t < 4; t++) {
        SmfTrack &track = smf.tracks[t + 1];
        track.events.push_back(channelEvent(0, 0xC0, 0, 0));
        track.events.push_back(channelEvent(specs[t].on, 0x90, specs[t].key, 100));
        track.events.push_back(channelEvent(specs[t].off, 0x80, specs[t].key, 0));
        track.endTick = 384;
    }
    return smf;
}

// Same minimal voicegroup as loopcheck: a looped PCM square that sustains at
// full level, with a slow enough release that a tail is alive to be cut.
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
            v.attack = 255;
            v.decay = 0;
            v.sustain = 255;
            v.release = 165; // ~0.2 s fade
        }
    }
};

struct RenderResult {
    float maxBeforeSteal = 0.0f; // largest |sample| before the first overflow
    float maxAfterSteal = 0.0f;  // largest |sample| after it
    bool shadowOnAfterSteal = false;
};

// Renders the song straight through (no loop), probing output levels around
// the first overflow at kStealTick.
RenderResult renderSong(M4AEngine *engine, const MidiTimeline &timeline)
{
    TimelinePlayer player;
    player.reset();

    RenderResult result;
    constexpr uint32_t kChunk = 500; // divides the probe boundary exactly
    float bufL[kChunk], bufR[kChunk];
    const uint64_t stealSample = kStealTick * kSamplesPerTick;
    const uint64_t total = 220000;
    uint64_t rendered = 0;
    while (rendered < total) {
        player.render(engine, &timeline, bufL, bufR, kChunk, false, 0);
        float peak = 0.0f;
        for (uint32_t i = 0; i < kChunk; i++)
            peak = std::max({peak, std::fabs(bufL[i]), std::fabs(bufR[i])});
        // The chunk containing the overflow itself is skipped on the "before"
        // side; anything at or past it counts as "after".
        if (rendered + kChunk <= stealSample)
            result.maxBeforeSteal = std::max(result.maxBeforeSteal, peak);
        else
            result.maxAfterSteal = std::max(result.maxAfterSteal, peak);
        rendered += kChunk;
        if (rendered == stealSample + 4000) {
            for (int i = MAX_PCM_CHANNELS; i < TOTAL_PCM_CHANNELS; i++) {
                if (engine->pcmChannels[i].status & CHN_ON)
                    result.shadowOnAfterSteal = true;
            }
        }
    }
    return result;
}

int checkEvent(const M4AEngine &engine, int index, uint8_t type, uint8_t track,
               uint8_t key, uint8_t byTrack, uint32_t tick, const char *what)
{
    const M4APolyEvent &ev = engine.polyEvents[index];
    if (ev.type == type && ev.trackIndex == track && ev.midiKey == key
        && ev.byTrack == byTrack && ev.tick == tick)
        return 0;
    std::fprintf(stderr,
                 "polycheck: FAIL: %s: event %d = {type %d, trk %d, key %d, "
                 "by %d, tick %u}, expected {type %d, trk %d, key %d, by %d, "
                 "tick %u}\n",
                 what, index, ev.type, ev.trackIndex, ev.midiKey, ev.byTrack,
                 ev.tick, type, track, key, byTrack, tick);
    return 1;
}

int runEngineStage()
{
    int failures = 0;
    auto check = [&failures](bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "polycheck: FAIL: %s\n", what);
            failures++;
        }
    };

    const SmfFile smf = buildOverflowSong();
    const auto timeline = MidiTimeline::build(smf, kSampleRate);
    if (!timeline || timeline->usedTrackCount != 4) {
        std::fprintf(stderr, "polycheck: synthesized song built wrong\n");
        return 1;
    }

    TestVoicegroup vg;

    // ---- Counters, event ring, and tick stamps ----
    M4AEngine engine;
    m4a_engine_init(&engine, float(kSampleRate));
    m4a_engine_set_voicegroup(&engine, vg.voices);
    engine.maxPcmChannels = 1;
    renderSong(&engine, *timeline);

    check(engine.polyEventTotal == 3, "exactly three overflow events");
    check(engine.polyStealCount[1] == 1, "steal counted for track 1");
    check(engine.polyDropCount[2] == 1, "drop counted for track 2");
    check(engine.polyTailCutCount[0] == 1, "tail cut counted for track 0");
    uint64_t others = 0;
    for (int t = 0; t < MAX_TRACKS; t++) {
        others += engine.polyDropCount[t] + engine.polyStealCount[t]
            + engine.polyTailCutCount[t];
    }
    check(others == 3, "no unexpected counters");
    failures += checkEvent(engine, 0, M4A_POLY_STOLEN, 1, kHoldKey, 0,
                           uint32_t(kStealTick), "steal event with thief's tick");
    failures += checkEvent(engine, 1, M4A_POLY_DROPPED, 2, kDropKey, 2,
                           uint32_t(kDropTick), "drop event with its own tick");
    failures += checkEvent(engine, 2, M4A_POLY_TAIL_CUT, 0, kStealKey, 3,
                           uint32_t(kTailCutTick), "tail-cut event with thief's tick");
    m4a_engine_destroy(&engine);

    // ---- Live notes carry the sentinel (host never set the clock) ----
    m4a_engine_init(&engine, float(kSampleRate));
    m4a_engine_set_voicegroup(&engine, vg.voices);
    engine.maxPcmChannels = 1;
    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_program_change(&engine, 1, 0);
    m4a_engine_note_on(&engine, 1, 60, 100);
    m4a_engine_note_on(&engine, 0, 67, 100); // steals
    check(engine.polyEventTotal == 1 && engine.polyEvents[0].tick == M4A_POLY_TICK_NONE,
          "direct note-on records the live sentinel tick");
    m4a_engine_destroy(&engine);

    // ---- Normal playback never touches the shadow pool ----
    m4a_engine_init(&engine, float(kSampleRate));
    m4a_engine_set_voicegroup(&engine, vg.voices);
    engine.maxPcmChannels = 1;
    RenderResult normal = renderSong(&engine, *timeline);
    check(normal.maxBeforeSteal > 1e-4f, "normal: audible before the overflow");
    check(normal.maxAfterSteal > 1e-4f, "normal: audible after the overflow");
    check(!normal.shadowOnAfterSteal, "normal: shadow pool stays off");
    m4a_engine_destroy(&engine);

    // ---- Invert: silent until a sound is lost, then only the lost sound ----
    m4a_engine_init(&engine, float(kSampleRate));
    m4a_engine_set_voicegroup(&engine, vg.voices);
    engine.maxPcmChannels = 1;
    m4a_engine_set_poly_debug_invert(&engine, true);
    // A stale audition flag (say, from a preview struck just before play)
    // must be cleared by the sequenced dispatch path, or this render's
    // pre-overflow silence assertion fails.
    engine.auditionNote = true;
    RenderResult inverted = renderSong(&engine, *timeline);
    check(inverted.maxBeforeSteal < 1e-6f, "invert: silent before the overflow");
    check(inverted.maxAfterSteal > 1e-4f, "invert: lost sound audible after it");
    check(inverted.shadowOnAfterSteal, "invert: shadow channel keyed on");
    m4a_engine_set_poly_debug_invert(&engine, false);
    bool shadowCleared = true;
    for (int i = MAX_PCM_CHANNELS; i < TOTAL_PCM_CHANNELS; i++) {
        if (engine.pcmChannels[i].status != 0)
            shadowCleared = false;
    }
    check(shadowCleared, "disabling invert clears the shadow pool");
    m4a_engine_destroy(&engine);

    // ---- Audition notes stay audible in invert mode (the preview paths
    // set auditionNote so the user can hear the note under investigation) ----
    m4a_engine_init(&engine, float(kSampleRate));
    m4a_engine_set_voicegroup(&engine, vg.voices);
    engine.maxPcmChannels = 1;
    m4a_engine_set_poly_debug_invert(&engine, true);
    m4a_engine_program_change(&engine, 0, 0);
    engine.polyEventClock = M4A_POLY_TICK_NONE;
    engine.auditionNote = true;
    m4a_engine_note_on(&engine, 0, 60, 100);
    {
        float bufL[500], bufR[500], peak = 0.0f;
        for (int chunk = 0; chunk < 8; chunk++) {
            m4a_engine_process(&engine, bufL, bufR, 500);
            for (int i = 0; i < 500; i++)
                peak = std::max({peak, std::fabs(bufL[i]), std::fabs(bufR[i])});
        }
        check(peak > 1e-4f, "invert: audition note stays audible");
    }
    m4a_engine_destroy(&engine);

    return failures;
}

// Timeline for bar:beat formatting: 4/4 until a 3/4 change at tick 192
// (bar 3), so tick 96 reads "2:1.0" and tick 216 reads "3:2.0".
std::unique_ptr<MidiTimeline> buildSigTimeline()
{
    SmfFile smf;
    smf.format = 1;
    smf.division = kDivision;
    smf.tracks.resize(2);
    SmfTrack &conductor = smf.tracks[0];
    conductor.events.push_back(metaEvent(0, 0x51, QByteArray("\x07\xA1\x20", 3)));
    conductor.events.push_back(metaEvent(0, 0x58, QByteArray("\x04\x02\x18\x08", 4)));
    conductor.events.push_back(metaEvent(192, 0x58, QByteArray("\x03\x02\x18\x08", 4)));
    conductor.endTick = 384;
    SmfTrack &notes = smf.tracks[1];
    notes.events.push_back(channelEvent(0, 0xC0, 0, 0));
    notes.events.push_back(channelEvent(0, 0x90, 60, 100));
    notes.events.push_back(channelEvent(24, 0x80, 60, 0));
    notes.endTick = 384;
    return MidiTimeline::build(smf, kSampleRate);
}

AudioEngine::PolySnapshot snapshotWithEvents(std::initializer_list<M4APolyEvent> events,
                                             uint32_t startTotal = 0)
{
    AudioEngine::PolySnapshot snap;
    snap.maxPcmChannels = 5;
    uint32_t total = startTotal;
    for (const M4APolyEvent &ev : events)
        snap.events[total++ % M4A_POLY_EVENT_CAPACITY] = ev;
    snap.eventTotal = total;
    return snap;
}

int runWidgetStage(const QString &screenshotPath)
{
    int failures = 0;
    auto check = [&failures](bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "polycheck: FAIL: %s\n", what);
            failures++;
        }
    };

    const auto timeline = buildSigTimeline();
    if (!timeline || timeline->timeSigs.size() != 2) {
        std::fprintf(stderr, "polycheck: sig timeline built wrong\n");
        return 1;
    }

    PolyphonyPanel panel;
    panel.resize(380, 760);
    panel.setTimeline(timeline.get());
    QStringList trackNames;
    for (int t = 0; t < 16; t++)
        trackNames.append(QString());
    trackNames[2] = QStringLiteral("Brass");
    panel.setTrackNames(trackNames);
    QStringList voiceNames;
    for (int v = 0; v < 128; v++)
        voiceNames.append(QString());
    voiceNames[5] = QStringLiteral("voice_piano");
    panel.setVoiceNames(voiceNames);

    uint64_t jumpedTo = UINT64_MAX;
    int jumpTrack = -1, jumpKey = -1, jumps = 0;
    QObject::connect(&panel, &PolyphonyPanel::jumpToEvent,
                     [&](uint64_t tick, int track, int midiKey) {
                         jumpedTo = tick;
                         jumpTrack = track;
                         jumpKey = midiKey;
                         jumps++;
                     });

    // Three events: a positioned steal, a positioned tail cut past the sig
    // change, and a live (sentinel) drop.
    AudioEngine::PolySnapshot snap = snapshotWithEvents({
        {M4A_POLY_STOLEN, 2, 60, 4, 5, 96},
        {M4A_POLY_TAIL_CUT, 2, 72, 0, 5, 216},
        {M4A_POLY_DROPPED, 1, 67, 1, 0, M4A_POLY_TICK_NONE},
    });
    snap.steal[2] = 1;
    snap.tailCut[2] = 1;
    snap.drop[1] = 1;
    snap.invert = true;
    snap.pcm[0] = {true, false, 2, 60};
    snap.pcm[MAX_PCM_CHANNELS] = {true, false, 4, 72}; // shadow cell
    panel.updateSnapshot(snap);

    check(panel.logRowCount() == 3, "log has three rows");
    const QString newest = panel.logRowText(0);
    const QString middle = panel.logRowText(1);
    const QString oldest = panel.logRowText(2);
    check(newest.contains(QStringLiteral("live"))
              && newest.contains(QStringLiteral("dropped")),
          "newest row first: the live drop");
    check(middle.contains(QStringLiteral("3:2.0"))
              && middle.contains(QStringLiteral("tail cut")),
          "bar:beat honors the mid-song 3/4 change");
    check(oldest.contains(QStringLiteral("2:1.0"))
              && oldest.contains(QStringLiteral("Trk 3"))
              && oldest.contains(QStringLiteral("C4"))
              && oldest.contains(QStringLiteral("voice_piano"))
              && oldest.contains(QStringLiteral("cut off by Trk 5")),
          "steal row: position, track, note, voice name, thief");

    panel.activateLogRow(2);
    check(jumps == 1 && jumpedTo == 96 && jumpTrack == 2 && jumpKey == 60,
          "double-click jumps with the event's tick, track, and key");
    panel.activateLogRow(0);
    check(jumps == 1, "live rows don't jump");

    // Reset rebase: a smaller total clears the log before draining.
    AudioEngine::PolySnapshot after = snapshotWithEvents({
        {M4A_POLY_DROPPED, 0, 60, 0, 0, 48},
    });
    after.drop[0] = 1;
    panel.updateSnapshot(after);
    check(panel.logRowCount() == 1, "reset rebases and clears the log");

    // Log cap: 600 events over ten polls leave exactly 500 rows.
    uint32_t total = after.eventTotal;
    for (int poll = 0; poll < 10; poll++) {
        AudioEngine::PolySnapshot burst;
        burst.maxPcmChannels = 5;
        for (int i = 0; i < 60; i++)
            burst.events[total++ % M4A_POLY_EVENT_CAPACITY] =
                {M4A_POLY_DROPPED, 0, 60, 0, 0, uint32_t(total)};
        burst.eventTotal = total;
        panel.updateSnapshot(burst);
    }
    check(panel.logRowCount() == 500, "log capped at 500 rows");

    // Paint smoke test. Re-feed the first snapshot so a saved screenshot
    // shows real rows and the shadow grid (snap.invert). Shown + settled
    // first: the scroll area only lays its content out through real layout
    // passes, which hidden widgets don't get.
    panel.updateSnapshot(snapshotWithEvents({}));
    panel.updateSnapshot(snap);
    panel.show();
    auto settle = [] {
        for (int i = 0; i < 3; i++)
            QCoreApplication::processEvents();
    };
    settle();
    QImage image(panel.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    panel.render(&image);
    check(!image.isNull(), "panel renders offscreen");
    if (!screenshotPath.isEmpty())
        image.save(screenshotPath);

    // Responsive layout: at a sliver width the sections stack and the
    // channel grid still gets the full height its wrapped cell rows need
    // (its hint used to be pinned to the five-per-row width, clipping the
    // extra rows); at a wide width the overflow table moves beside the grid
    // instead of stretching across.
    panel.resize(180, 980);
    settle();
    check(!panel.wideLayoutActive(), "narrow panel stacks the sections");
    check(panel.overflowSectionRect().top() >= panel.usageSectionRect().bottom(),
          "stacked: overflow table sits below the channel grid");
    check(panel.gridFullyVisible(),
          "narrow grid tall enough for all wrapped cell rows");
    panel.resize(900, 600);
    settle();
    check(panel.wideLayoutActive(), "wide panel goes side by side");
    check(panel.overflowSectionRect().left() >= panel.usageSectionRect().right(),
          "wide: overflow table sits right of the channel grid");
    check(panel.gridFullyVisible(), "wide grid shows all cell rows");
    if (!screenshotPath.isEmpty()) {
        QImage wideImage(panel.size(), QImage::Format_ARGB32_Premultiplied);
        wideImage.fill(Qt::white);
        panel.render(&wideImage);
        QFileInfo info(screenshotPath);
        wideImage.save(info.path() + QLatin1Char('/') + info.completeBaseName()
                       + QStringLiteral("-wide.") + info.suffix());
    }
    // Short window: once the sections are squeezed to their minimums the
    // panel scrolls instead of clipping the bottom off-window; tall windows
    // never show the scrollbar.
    panel.resize(380, 240);
    settle();
    check(panel.gridFullyVisible(), "short window: grid keeps its height");
    check(panel.vScrollRange() > 0, "short window: panel scrolls to the bottom");
    if (!screenshotPath.isEmpty()) {
        QImage shortImage(panel.size(), QImage::Format_ARGB32_Premultiplied);
        shortImage.fill(Qt::white);
        panel.render(&shortImage);
        QFileInfo info(screenshotPath);
        shortImage.save(info.path() + QLatin1Char('/') + info.completeBaseName()
                        + QStringLiteral("-short.") + info.suffix());
    }
    panel.resize(380, 980);
    settle();
    check(panel.vScrollRange() == 0, "tall window: no scrollbar");

    return failures;
}

} // namespace

bool MainWindow::runPolyGateCheck()
{
    if (!m_audioOk) {
        std::fprintf(stderr, "polycheck: no audio device available\n");
        return false;
    }
    int failures = 0;
    const auto check = [&failures](bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "polycheck: FAIL: %s\n", what);
            failures++;
        }
        return ok;
    };

    show();
    QCoreApplication::processEvents();
    check(!m_polyDock->isVisible(), "poly dock starts hidden");
    check(!m_audio.polyDebugInvert(), "engine invert starts off");

    // The checkbox alone must not invert anything while the dock is hidden.
    m_polyPanel->setInvertChecked(true);
    check(!m_audio.polyDebugInvert(), "hidden dock: checkbox alone is inert");

    m_polyDock->show();
    QCoreApplication::processEvents();
    check(m_audio.polyDebugInvert(), "visible dock + checked box inverts");

    // Closing the dock (its title-bar close button hides it) suspends the
    // mode without losing the checkbox state...
    m_polyDock->close();
    QCoreApplication::processEvents();
    check(!m_audio.polyDebugInvert(), "closing the dock suspends the invert");
    check(m_polyPanel->invertChecked(), "closing the dock keeps the checkbox");

    // ...and re-opening it resumes the mode.
    m_polyDock->show();
    QCoreApplication::processEvents();
    check(m_audio.polyDebugInvert(), "re-opening the dock resumes the invert");

    m_polyPanel->setInvertChecked(false);
    check(!m_audio.polyDebugInvert(), "unchecking turns the invert off");

    return failures == 0;
}

int runPolyCheck(const QString &screenshotPath)
{
    int failures = runEngineStage();
    failures += runWidgetStage(screenshotPath);

    // Stage C: redirected settings so the MainWindow neither reads nor
    // overwrites the user's real window state.
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "polycheck: no temp dir for settings\n");
        failures++;
    } else {
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                           settingsDir.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           settingsDir.path());
        MainWindow window;
        if (!window.runPolyGateCheck())
            failures++;
    }

    std::printf("polycheck: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
