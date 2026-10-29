#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QScrollArea>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>

#include "audio/cpuprofile.h"
#include "core/miditimeline.h"
#include "mainwindow.h"
#include "songsession.h"
#include "ui/audiosettingspage.h"
#include "ui/settingsdialog.h"
#include "ui/songview.h"
#include "ui/theme/themecontroller.h"
#include "ui/theme/themeruntime.h"

extern "C" {
#include "m4a_engine.h"
#include "m4a_gba.h"
}

// --cpuprofilecheck <projectRoot> <songLabel>: the offline CPU profile and
// its strip under the ruler (docs/cpu-meter/PLAN.md D3, step 7), with the
// song open and the audio device parked. QSettings is redirected into a
// temp dir first. The song must loop (mus_abandoned_ship).
//
// Covers: the strip is on by default (View → CPU Profile checked) and sits
// directly under the ruler; opening the song starts a profile, "profiling"
// meanwhile, done within a time bound; one frame per GBA frame of the song
// once through (to the loop end), each frame's total the sum of its three
// terms, ticks in order inside the span, the max the frames' max; playing
// the song through once, every frame the readout reports is at most the
// profile's max and the readout's peak matches it; the strip paints heat
// over the song and chrome past its end, the max's column in the max's
// colour; the budget moves the colours (amber over it) without a
// recompute; the hover text names the three terms and no sound effects
// (D4), the gutter's too; a note added starts one recompute (two quick
// edits debounce into one) and replaces the profile; a mix-rate change
// recomputes with a higher max; the toggle hides and shows the strip in
// every tab, persists, and the strip's own Hide unchecks it; hidden, no
// profile is made.

using songview::kGutterW;

namespace {

int g_failures = 0;

bool check(bool ok, const char *what)
{
    if (!ok) {
        std::fprintf(stderr, "cpuprofilecheck: FAIL: %s\n", what);
        g_failures++;
    }
    return ok;
}

bool waitFor(const std::function<bool()> &done, int ms)
{
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    return done();
}

bool mentionsEffects(const QString &s)
{
    const QString lower = s.toLower();
    return lower.contains(QStringLiteral("effect")) || lower.contains(QStringLiteral("cry")) ||
           lower.contains(QStringLiteral("sfx"));
}

} // namespace

bool MainWindow::runCpuProfileCheck(const QString &projectRoot, const QString &songLabel)
{
    check(m_cpuProfileAction && m_cpuProfileAction->isCheckable() &&
              m_cpuProfileAction->isChecked(),
          "View → CPU Profile is missing or not on by default");
    if (!m_audioOk) {
        std::printf("cpuprofilecheck: SKIP (no audio device)\n");
        return true;
    }
    m_audio.holdDeviceForTest(true);
    resize(1400, 900);
    if (!check(openProjectDir(projectRoot, /*interactive=*/false), "the project failed to open"))
        return false;
    const uint64_t jobsBeforeLoad = m_cpuProfiler->jobsStarted();
    loadSongByLabel(songLabel);
    if (!check(m_active && m_active->doc.label() == songLabel, "the song did not load"))
        return false;
    SongSession &session = *m_active;
    SongView &view = *session.view;
    QWidget *strip = view.findChild<QWidget *>(QStringLiteral("cpuProfileStrip"));
    QWidget *ruler = view.findChild<QWidget *>(QStringLiteral("timeRuler"));
    if (!check(strip && ruler, "the CPU profile strip or the ruler is missing"))
        return false;
    QCoreApplication::processEvents();
    check(view.cpuProfileVisible() && strip->isVisible(), "the strip is not shown by default");
    check(strip->mapTo(&view, QPoint(0, 0)).y() ==
              ruler->mapTo(&view, QPoint(0, 0)).y() + ruler->height(),
          "the strip does not sit directly under the ruler");
    check(view.cpuProfiling(), "the view does not show profiling after the song opened");

    const auto ready = [&] { return session.cpuProfile && !session.cpuProfileStale; };
    QElapsedTimer clock;
    clock.start();
    if (!check(waitFor(ready, 20000), "no profile within 20 s of opening the song"))
        return false;
    const qint64 firstMs = clock.elapsed();
    check(m_cpuProfiler->jobsStarted() == jobsBeforeLoad + 1,
          "opening the song started more or fewer than one profile");
    check(!view.cpuProfiling() && view.cpuProfile() == session.cpuProfile.get(),
          "the finished profile is not the view's, or the view still shows profiling");

    // The profile itself.
    // Edits rebuild the session's timeline: re-read it after each.
    const MidiTimeline *timeline = session.timeline.get();
    std::shared_ptr<const CpuProfile> profile = session.cpuProfile;
    const double rate = m_audio.sampleRate();
    {
        check(timeline->hasLoop() && profile->endSample == timeline->loopEndSample &&
                  profile->endTick == timeline->loopEndTick && !profile->skipped,
              "the profile does not span the song to its loop end");
        const double seconds = double(profile->endSample) / rate;
        const double gbaFrames = seconds * double(GBA_CPU_HZ) / double(GBA_CYCLES_PER_FRAME);
        std::printf("cpuprofilecheck: %s: %.1f s of song, %zu frames (expected %.1f), max %d "
                    "cycles (%.0f%%), rendered in %.3f s (%.0fx real time, %.0f frames/s); "
                    "first profile after %lld ms\n",
                    qPrintable(songLabel), seconds, profile->frames.size(), gbaFrames,
                    profile->maxTotal, profile->maxTotal * 100.0 / M4A_VBLANK_CYCLES,
                    profile->renderSeconds, seconds / std::max(profile->renderSeconds, 1e-9),
                    double(profile->frames.size()) / std::max(profile->renderSeconds, 1e-9),
                    (long long)firstMs);
        check(std::abs(double(profile->frames.size()) - gbaFrames) <= 1.5,
              "the profile's frame count is not the song's length in GBA frames (±1)");
        check(profile->renderSeconds > 0.0 && profile->renderSeconds < 5.0,
              "the profile took 5 s or more to render");
        bool sums = true, ordered = true, inSpan = true;
        int32_t max = 0;
        for (size_t i = 0; i < profile->frames.size(); i++) {
            const CpuProfileFrame &f = profile->frames[i];
            sums = sums && f.total == f.sequencer + f.cgb + f.mixer && f.total > 0;
            ordered = ordered && (i == 0 || (f.tick >= profile->frames[i - 1].tick &&
                                             f.samplePos > profile->frames[i - 1].samplePos));
            inSpan = inSpan && f.tick < profile->endTick && f.samplePos < profile->endSample;
            max = std::max(max, f.total);
        }
        check(sums, "a frame's total is not sequencer + CGB + mixer (or not positive)");
        check(ordered, "the frames are not in timeline order");
        check(inSpan, "a frame lies outside the profiled span");
        check(max == profile->maxTotal && profile->frames[profile->maxIndex].total == max,
              "the profile's max is not its frames' max");
        const double samplesPerFrame = rate * double(GBA_CYCLES_PER_FRAME) / double(GBA_CPU_HZ);
        std::printf("cpuprofilecheck: first frame at sample %llu (%.2f frames)\n",
                    (unsigned long long)profile->frames.front().samplePos,
                    double(profile->frames.front().samplePos) / samplesPerFrame);
        // The boot ends just after its last SoundMain (m4a_engine.c's
        // BOOT_END_CYCLE), where sample 0 plays: the first SoundMain the song
        // reaches is one frame in (it runs sample 0's events). Two frames in
        // was the boot not being taken off.
        check(double(profile->frames.front().samplePos) <= 1.05 * samplesPerFrame,
              "the first frame is more than one GBA frame after the song's start");
    }

    // A cost spike lands on its note: the song's controllers without its
    // notes, then a note on every track at 2 s; the first frame that costs
    // more than the silent ones is the SoundMain right after the onset
    // (within one GBA frame of it).
    {
        MidiTimeline quiet = *timeline;
        quiet.events.erase(
            std::remove_if(quiet.events.begin(), quiet.events.end(),
                           [](const TimelineEvent &e) { return e.type == 0x9 || e.type == 0x8; }),
            quiet.events.end());
        const uint64_t onset = uint64_t(2.0 * rate);
        const uint32_t onsetTick = uint32_t(quiet.tickForSample(onset));
        for (int t = 0; t < 16; t++) {
            if (!quiet.tracks[t].used)
                continue;
            TimelineEvent on{};
            on.samplePos = onset;
            on.tick = onsetTick;
            on.type = 0x9;
            on.track = uint8_t(t);
            on.data0 = 60;
            on.data1 = 127;
            quiet.events.push_back(on);
            TimelineEvent off = on;
            off.samplePos = onset + uint64_t(0.5 * rate);
            off.tick = uint32_t(quiet.tickForSample(off.samplePos));
            off.type = 0x8;
            off.data1 = 0;
            quiet.events.push_back(off);
        }
        std::stable_sort(quiet.events.begin(), quiet.events.end(),
                         [](const TimelineEvent &a, const TimelineEvent &b) {
                             return a.samplePos < b.samplePos;
                         });
        quiet.loopStartSample = quiet.loopEndSample = UINT64_MAX;
        quiet.loopStartTick = quiet.loopEndTick = UINT64_MAX;
        quiet.lengthSamples = onset + uint64_t(rate);
        quiet.lengthTicks = uint64_t(quiet.tickForSample(quiet.lengthSamples));
        CpuProfile spike;
        const bool done =
            renderCpuProfile(quiet, session.voicegroup, songSettingsFor(session), nullptr, &spike);
        const double samplesPerFrame = rate * double(GBA_CYCLES_PER_FRAME) / double(GBA_CPU_HZ);
        check(done && spike.endSample == quiet.lengthSamples + uint64_t(3.0 * rate + 0.5),
              "a song without a loop is not profiled through its 3 s release tail");
        int32_t silent = 0;
        for (const CpuProfileFrame &f : spike.frames)
            if (double(f.samplePos) < double(onset) - 4 * samplesPerFrame)
                silent = std::max(silent, f.total);
        const CpuProfileFrame *first = nullptr;
        for (const CpuProfileFrame &f : spike.frames) {
            if (f.total > silent + 2000) {
                first = &f;
                break;
            }
        }
        if (check(first != nullptr, "the onset's notes raised no frame's cost")) {
            const double delta = double(first->samplePos) - double(onset);
            std::printf("cpuprofilecheck: onset spike %+.2f frames from the note (silent max %d, "
                        "spike %d)\n",
                        delta / samplesPerFrame, silent, first->total);
            check(delta >= -samplesPerFrame * 0.05 && delta <= samplesPerFrame,
                  "the onset's cost spike is not within one GBA frame after the note");
        }
    }

    // Played through once, the readout agrees: no frame it reports costs
    // more than the profile's max, and its peak is that max.
    {
        const uint32_t chunk = uint32_t(rate / 100.0); // under a frame: every frame's cost
        m_audio.play();
        int32_t worstLast = 0;
        bool under = true;
        for (uint64_t pos = 0; pos + 2 * chunk < profile->endSample; pos += chunk) {
            m_audio.pumpForTest(chunk, nullptr, chunk);
            const AudioEngine::CpuSnapshot s = m_audio.cpuSnapshot();
            if (!s.valid)
                continue;
            const int32_t last = s.last[AudioEngine::CpuTotal];
            worstLast = std::max(worstLast, last);
            under = under && last <= profile->maxTotal + profile->maxTotal / 50;
        }
        const AudioEngine::CpuSnapshot s = m_audio.cpuSnapshot();
        m_audio.stop();
        m_audio.pumpForTest(chunk, nullptr, chunk);
        std::printf("cpuprofilecheck: played through: peak %d, worst frame %d, profile max %d\n",
                    s.peakValid ? s.peak[AudioEngine::CpuTotal] : -1, worstLast, profile->maxTotal);
        check(under, "the readout reported a frame above the profile's max (2 % slack)");
        check(s.peakValid && std::abs(s.peak[AudioEngine::CpuTotal] - profile->maxTotal) <=
                                 profile->maxTotal / 50,
              "the readout's peak over one pass is not the profile's max (2 % slack)");
    }

    // The strip: zoomed so the whole song and some space past it show.
    {
        const int width = strip->width();
        const double fit = (width - kGutterW) * 0.6 / (double(profile->endTick) * view.pxPerTick());
        view.zoomAroundContentX(fit, 0.0);
        view.ensureTickVisible(0);
        QCoreApplication::processEvents();
        const auto xOf = [&](double tick) {
            return int(std::lround(kGutterW + view.contentX(tick)));
        };
        const int xEnd = xOf(double(profile->endTick));
        if (check(xOf(0.0) >= kGutterW && xEnd + 20 < width,
                  "could not fit the song in the strip for the paint check")) {
            const QImage img = strip->grab().toImage();
            // The grab is in device pixels: sample a logical x's column.
            const qreal dpr = strip->devicePixelRatioF();
            const int y = int(strip->height() / 2 * dpr);
            const auto devX = [&](int logicalX) { return int(std::lround(logicalX * dpr)); };
            const QColor chrome = themes::color(themes::Role::song_view_timeline_chrome_background);
            bool heat = true;
            for (double f : {0.05, 0.25, 0.5, 0.75, 0.95}) {
                const QColor c = img.pixelColor(devX(xOf(f * double(profile->endTick))), y);
                heat = heat && c != chrome;
            }
            check(heat, "the strip has empty columns over the song");
            bool empty = true;
            for (int x = xEnd + 3; x < std::min(width, xEnd + 40); x++)
                empty = empty && img.pixelColor(devX(x), y) == chrome;
            check(empty, "the strip paints past the profiled span");
            const CpuProfileFrame &peak = profile->frames[profile->maxIndex];
            const QColor peakColor = img.pixelColor(devX(xOf(double(peak.tick))), y);
            check(peakColor == view.cpuProfileColor(peak.total),
                  "the max frame's column is not the max's colour");

            // The budget recolours without a recompute: under the max,
            // the max's column turns amber.
            const uint64_t jobs = m_cpuProfiler->jobsStarted();
            const int lowBudget = std::max(int(AudioSettingsPage::kCpuBudgetMin),
                                           int(double(peak.total) * 100.0 / M4A_VBLANK_CYCLES) - 5);
            m_settingsDialog->audioPage()->setCpuBudget(lowBudget);
            QCoreApplication::processEvents();
            check(view.cpuBudgetPercent() == lowBudget, "the budget did not reach the view");
            const QImage amber = strip->grab().toImage();
            check(amber.pixelColor(devX(xOf(double(peak.tick))), y) ==
                      themes::color(themes::Role::cpu_profile_warn_fill),
                  "over the budget the max's column is not amber");
            check(view.cpuProfileColor(M4A_FRAME_CYCLES) ==
                      themes::color(themes::Role::cpu_profile_over_fill),
                  "a frame at the ceiling is not red");
            check(view.cpuProfileColor(0) == themes::color(themes::Role::cpu_profile_low_fill),
                  "an idle frame is not the ramp's neutral end");
            m_settingsDialog->audioPage()->setCpuBudget(AudioSettingsPage::kCpuBudgetDefault);
            QCoreApplication::processEvents();
            check(m_cpuProfiler->jobsStarted() == jobs, "a budget change recomputed the profile");

            // Hover text.
            const QString tip = view.cpuProfileToolTip(xOf(double(peak.tick)) + 0.5);
            check(tip.contains(QStringLiteral("CPU ")) && tip.contains(QStringLiteral("cycles")) &&
                      tip.contains(QStringLiteral("Sequencer")) &&
                      tip.contains(QStringLiteral("CGB")) &&
                      tip.contains(QStringLiteral("Mixer")) && tip.contains(QStringLiteral("bar ")),
                  "the hover text lacks the percent, cycles, bar or the three terms");
            const QString gutterTip = view.cpuProfileToolTip(kGutterW / 2.0);
            check(gutterTip.contains(QStringLiteral("Peak")),
                  "the gutter's hover text does not give the peak");
            check(!mentionsEffects(tip) && !mentionsEffects(gutterTip),
                  "the strip's hover text mentions sound effects (D4)");
            check(view.cpuProfileToolTip(xEnd + 10).isEmpty(),
                  "the strip has hover text past the profiled span");
        }
    }

    // An edit recomputes, debounced: two quick edits, one job.
    {
        const uint64_t jobs = m_cpuProfiler->jobsStarted();
        int track = 0;
        while (track < 16 && !timeline->tracks[track].used)
            track++;
        const uint64_t tick = timeline->loopStartTick + timeline->ticksPerBeat;
        const uint32_t beat = timeline->ticksPerBeat;
        // No event processing between the two edits: the debounce timer
        // cannot fire in between, however slow the build.
        session.doc.addNote(track, tick, 60, beat, 100);
        timeline = session.timeline.get();
        check(session.cpuProfileStale && view.cpuProfiling() && m_cpuProfileTimer->isActive() &&
                  m_cpuProfiler->jobsStarted() == jobs,
              "an edit did not mark the profile stale and arm the debounce");
        session.doc.addNote(track, tick, 64, beat, 100);
        timeline = session.timeline.get();
        check(m_cpuProfileTimer->isActive() && m_cpuProfiler->jobsStarted() == jobs,
              "a second edit started a job before the debounce");
        if (check(waitFor(ready, 20000), "no profile within 20 s of an edit")) {
            check(m_cpuProfiler->jobsStarted() == jobs + 1,
                  "two quick edits did not debounce into one recompute");
            check(session.cpuProfile != profile && view.cpuProfile() == session.cpuProfile.get(),
                  "the edit's profile did not replace the old one");
        }
        profile = session.cpuProfile;
    }

    // A mix-rate change recomputes: a faster mix costs more.
    {
        EngineSettings faster = m_engineSettings;
        faster.pcmMixRate = 21024.0f;
        const int32_t before = profile->maxTotal;
        m_settingsDialog->audioPage()->setEngineSettings(faster);
        QCoreApplication::processEvents();
        if (check(waitFor([&] { return ready() && session.cpuProfile != profile; }, 20000),
                  "no new profile within 20 s of a mix-rate change")) {
            std::printf("cpuprofilecheck: max at 13379 Hz %d, at 21024 Hz %d (rendered in "
                        "%.3f s)\n",
                        before, session.cpuProfile->maxTotal, session.cpuProfile->renderSeconds);
            check(session.cpuProfile->maxTotal > before,
                  "a faster mix rate did not raise the profile's max");
        }
        EngineSettings back = m_engineSettings;
        back.pcmMixRate = 13379.0f;
        m_settingsDialog->audioPage()->setEngineSettings(back);
        waitFor([&] { return ready(); }, 20000);
        profile = session.cpuProfile;
    }

    // The toggle: every tab, persisted; the strip's own Hide; hidden, no
    // profile is made. Shown or hidden, the roll and its track headers start
    // right under the header (ruler, plus the strip while it shows), level
    // with each other.
    {
        QWidget &roll = view.timelineSurfaces().roll.widget;
        QWidget *headers = nullptr;
        for (QWidget *w = &roll; w && w != &view && !headers; w = w->parentWidget())
            if (QScrollArea *area = w->findChild<QScrollArea *>())
                headers = area;
        const auto topOf = [&](const QWidget *w) { return w->mapTo(&view, QPoint(0, 0)).y(); };
        const auto layoutUnder = [&](const QWidget *above) {
            return headers && headers->width() == songview::kHeaderW &&
                   topOf(&roll) == topOf(above) + above->height() && topOf(headers) == topOf(&roll);
        };
        check(layoutUnder(strip), "with the strip shown, the roll or headers are not under it");
        m_cpuProfileAction->trigger();
        QCoreApplication::processEvents();
        check(!view.cpuProfileVisible() && !strip->isVisible(),
              "the toggle did not hide the strip");
        check(layoutUnder(ruler), "with the strip hidden, the roll or headers are not under the "
                                  "ruler");
        check(!QSettings().value(QStringLiteral("cpuProfileStrip"), true).toBool(),
              "hiding the strip did not persist");
        const uint64_t jobs = m_cpuProfiler->jobsStarted();
        const uint32_t beat = timeline->ticksPerBeat;
        session.doc.addNote(0, beat * 2, 67, beat, 100);
        timeline = session.timeline.get();
        waitFor([] { return false; }, 800);
        check(m_cpuProfiler->jobsStarted() == jobs && !view.cpuProfiling(),
              "a hidden strip still profiled after an edit");
        m_cpuProfileAction->trigger();
        QCoreApplication::processEvents();
        check(view.cpuProfileVisible() && strip->isVisible() &&
                  QSettings().value(QStringLiteral("cpuProfileStrip"), false).toBool(),
              "the toggle did not show the strip again, or did not persist");
        check(waitFor(ready, 20000) && m_cpuProfiler->jobsStarted() == jobs + 1,
              "showing the strip again did not profile the edited song once");
        emit view.cpuProfileVisibilityRequested(false);
        QCoreApplication::processEvents();
        check(!m_cpuProfileAction->isChecked() && !view.cpuProfileVisible(),
              "the strip's own Hide did not uncheck View → CPU Profile");
        m_cpuProfileAction->trigger();
        QCoreApplication::processEvents();
    }
    // PORYDAW_CPUPROFILE_SHOT=<png>: the window as checked, to look at, in
    // Vanilla and (as <png>.dark.png) Dark Neutral High.
    const QString shot = qEnvironmentVariable("PORYDAW_CPUPROFILE_SHOT");
    if (!shot.isEmpty()) {
        waitFor(ready, 20000);
        QCoreApplication::processEvents();
        grab().save(shot);
        themes::ThemeSelection dark;
        dark.mode = themes::ThemeMode::DarkNeutralHigh;
        m_themeController->commit(dark);
        QCoreApplication::processEvents();
        grab().save(shot + QStringLiteral(".dark.png"));
    }
    return g_failures == 0;
}

int runCpuProfileCheck(const QString &projectRoot, const QString &songLabel)
{
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "cpuprofilecheck: no temp dir for settings\n");
        return 1;
    }
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QStandardPaths::setTestModeEnabled(true);
    {
        MainWindow window;
        window.show();
        window.runCpuProfileCheck(projectRoot, songLabel);
    }
    if (g_failures == 0)
        std::printf("cpuprofilecheck: OK\n");
    return g_failures ? 1 : 0;
}
