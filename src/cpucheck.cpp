#include <QApplication>
#include <QLabel>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QStyle>
#include <QTemporaryDir>
#include <algorithm>
#include <cstdio>

#include "core/m4a.h"
#include "mainwindow.h"
#include "songsession.h"
#include "ui/audiosettingspage.h"
#include "ui/settingsdialog.h"
#include "ui/theme/themeruntime.h"

// --cpucheck <projectRoot> <songLabel>: the GBA CPU readout
// (docs/cpu-meter/PLAN.md step 5), with the song open and the audio device
// parked so the callback runs only when pumped. QSettings is redirected
// into a temp dir first.
//
// Covers: AudioEngine::cpuSnapshot is invalid right after a song loads and
// valid within a few callbacks, its total always the sum of its three
// terms, and the sequencer term marked estimated (porydaw drives the engine
// with MIDI events); while the song plays, the peak never decreases and
// never sits under the last frame; a play from stop resets it (D8), and so
// does resetCpuPeak; the status bar's CPU item is shown with the meter and
// reads a nonzero percent while playing and "--" stopped; a budget under
// the reading turns it amber (the cpu_meter_warn_text role, through the
// cpuLevel property its style sheet matches), one over it plain again;
// stopping leaves no colour behind; the tooltip names the three terms,
// marks the estimate, and never mentions sound effects (D4). cpuLevel's
// thresholds are unit-tested, the red frame ceiling included (no stock
// configuration reaches it).

namespace {

int g_failures = 0;

bool check(bool ok, const char *what)
{
    if (!ok) {
        std::fprintf(stderr, "cpucheck: FAIL: %s\n", what);
        g_failures++;
    }
    return ok;
}

bool sumsUp(const AudioEngine::CpuSnapshot &s)
{
    return s.last[AudioEngine::CpuTotal] == s.last[AudioEngine::CpuSequencer] +
                                                s.last[AudioEngine::CpuPsg] +
                                                s.last[AudioEngine::CpuMixer] &&
           (!s.peakValid || s.peak[AudioEngine::CpuTotal] == s.peak[AudioEngine::CpuSequencer] +
                                                                 s.peak[AudioEngine::CpuPsg] +
                                                                 s.peak[AudioEngine::CpuMixer]);
}

} // namespace

bool MainWindow::runCpuCheck(const QString &projectRoot, const QString &songLabel)
{
    // The thresholds (D1): amber strictly above the budget, red from the
    // whole frame on, whatever the budget.
    check(cpuLevel(0, 100) == 0 && cpuLevel(M4A_VBLANK_CYCLES, 100) == 0 &&
              cpuLevel(M4A_VBLANK_CYCLES + 1, 100) == 1 && cpuLevel(41888, 50) == 0 &&
              cpuLevel(41889, 50) == 1 && cpuLevel(280649, 335) == 0 &&
              cpuLevel(280650, 335) == 1 && cpuLevel(M4A_FRAME_CYCLES - 1, 335) == 1 &&
              cpuLevel(M4A_FRAME_CYCLES, 335) == 2 && cpuLevel(M4A_FRAME_CYCLES, 10) == 2,
          "cpuLevel's budget and frame-ceiling thresholds");

    if (!m_audioOk) {
        std::printf("cpucheck: SKIP (no audio device)\n");
        return true;
    }
    // Parked before the song loads: nothing runs the engine until pumped.
    m_audio.holdDeviceForTest(true);
    if (!check(openProjectDir(projectRoot, /*interactive=*/false), "the project failed to open"))
        return false;
    loadSongByLabel(songLabel);
    if (!check(m_active && m_active->doc.label() == songLabel, "the song did not load"))
        return false;
    // A callback shorter than a GBA frame (~16.7 ms) runs at most one
    // SoundMain, so the `last` read after each is every frame's cost.
    const uint32_t chunk = uint32_t(m_audio.sampleRate() / 100.0);
    AudioSettingsPage *page = m_settingsDialog->audioPage();
    const auto cpuText = [this] { return m_cpuValueLabel->text(); };
    const auto cpuLevelProp = [this] { return m_cpuValueLabel->property("cpuLevel").toString(); };
    const auto textColour = [this] {
        return m_cpuValueLabel->palette().color(QPalette::WindowText);
    };

    // 1. A fresh song engine publishes nothing until it has costed a frame.
    {
        AudioEngine::CpuSnapshot s = m_audio.cpuSnapshot();
        check(!s.valid && !s.peakValid, "the CPU cost was valid before the song engine ran");
        int calls = 0;
        while (!s.valid && calls < 16) {
            m_audio.pumpForTest(chunk, nullptr, chunk);
            s = m_audio.cpuSnapshot();
            calls++;
        }
        check(s.valid, "the CPU cost did not become valid within 16 callbacks");
        check(sumsUp(s), "total != sequencer + PSG + mixer (stopped)");
        check(s.vblankCycles == 83776 && s.frameCycles == 280896,
              "the snapshot's VBlank and frame budgets");
    }

    // 2. Stopped: the item is there with the meter, without a reading.
    updatePolyStatus();
    check(m_polyMeter->isVisibleTo(this) && m_cpuValueLabel->isVisibleTo(this) &&
              m_cpuCaption->text() == QStringLiteral("CPU"),
          "the CPU item is not shown in the status bar with a song loaded");
    check(cpuText() == QStringLiteral("--") && cpuLevelProp().isEmpty(),
          "stopped, the CPU item shows a reading or a colour");

    // 3. Play two seconds: every frame's cost, the peak monotone.
    m_audio.play();
    int32_t peak = 0;
    int32_t firstPeak = 0;
    bool monotone = true, aboveLast = true, sums = true, estimated = true, valid = true;
    const int calls = int(2.0 * m_audio.sampleRate() / chunk);
    for (int i = 0; i < calls; i++) {
        m_audio.pumpForTest(chunk, nullptr, chunk);
        const AudioEngine::CpuSnapshot s = m_audio.cpuSnapshot();
        valid = valid && s.valid && s.peakValid;
        sums = sums && sumsUp(s);
        estimated = estimated && s.estimated;
        // The first callback's frame may predate the play (the reset lands
        // with the first SoundMain after it).
        if (i > 0) {
            monotone = monotone && s.peak[AudioEngine::CpuTotal] >= peak;
            aboveLast = aboveLast && s.peak[AudioEngine::CpuTotal] >= s.last[AudioEngine::CpuTotal];
        }
        peak = s.peak[AudioEngine::CpuTotal];
        if (i == 0)
            firstPeak = peak;
    }
    check(valid, "the CPU cost went invalid while playing");
    check(sums, "total != sequencer + PSG + mixer (playing)");
    check(estimated, "the sequencer term was not marked estimated");
    check(monotone, "the peak decreased while playing");
    check(aboveLast, "the peak sat under the last frame");
    check(peak > firstPeak, "the peak never rose over two seconds of the song");

    // The song is declared to the engine's sequencer estimate (the host
    // contract): started with its note tracks, its meter's bar marks, its
    // loop start once passed.
    const MidiTimeline *timeline = m_audio.timeline();
    const M4ASeqEstimate &est = m_audio.engineForTest()->seqEst;
    {
        uint16_t noteTracks = 0;
        for (int t = 0; t < 16; t++)
            if (timeline->tracks[t].noteCount > 0)
                noteTracks |= uint16_t(1u << t);
        check(est.songDeclared && est.playing,
              "playing, the song was not declared to the estimate");
        check(est.declared == noteTracks || est.declared == (noteTracks & est.exist),
              "the estimate's song tracks are not the tracks with notes");
        check(!timeline->meterMarks.empty() &&
                  est.barLen == timeline->clocksPerBar(timeline->meterMarks.front()),
              "the estimate's bar length is not the song's meter");
        if (timeline->hasLoop() && timeline->loopStartSample < m_audio.playheadSamples())
            check(est.loopSaved, "the loop start was passed but not declared");
    }

    updatePolyStatus();
    const AudioEngine::CpuSnapshot playing = m_audio.cpuSnapshot();
    const int percent =
        int(std::lround(double(playing.last[AudioEngine::CpuTotal]) * 100.0 / M4A_VBLANK_CYCLES));
    const QRegularExpression reading(QStringLiteral("^([0-9]+)%$"));
    const QRegularExpressionMatch match = reading.match(cpuText());
    check(match.hasMatch() && match.captured(1).toInt() == percent && percent > 0,
          "playing, the CPU item does not read the last frame's nonzero percent");
    std::printf("cpucheck: playing reads %s (last %d cycles, peak %d cycles)\n",
                qUtf8Printable(cpuText()), playing.last[AudioEngine::CpuTotal], peak);

    // 4. The tooltip: the three terms, the estimate, both budget lines, and
    // nothing about sound effects (D4).
    {
        const QString tip = m_cpuValueLabel->toolTip();
        check(tip == m_cpuCaption->toolTip(), "the caption and the value carry different tooltips");
        check(tip.contains(QStringLiteral("Sequencer")) && tip.contains(QStringLiteral("PSG")) &&
                  tip.contains(QStringLiteral("Mixer")),
              "the tooltip does not name the three terms");
        check(tip.contains(QStringLiteral("(estimated)")),
              "the tooltip does not mark the estimated sequencer term");
        check(tip.contains(QStringLiteral("VBlank window")) &&
                  tip.contains(QStringLiteral("Frame ceiling")) &&
                  tip.contains(QStringLiteral("Budget")) && tip.contains(QStringLiteral("Peak")),
              "the tooltip lacks the budget, VBlank, frame-ceiling or peak line");
        check(tip.contains(QStringLiteral("%1 Hz").arg(int(m_engineSettings.pcmMixRate + 0.5f))) &&
                  tip.contains(QStringLiteral("stock mixer")),
              "the tooltip does not say the mix rate and mixer in use");
        const QString lower = tip.toLower();
        check(!lower.contains(QStringLiteral("effect")) && !lower.contains(QStringLiteral("cry")) &&
                  !lower.contains(QStringLiteral("sfx")),
              "the tooltip mentions sound effects (D4)");
    }

    // 5. The budget: under the reading the item turns amber, over it plain.
    const QColor plain = themes::color(themes::Role::polyphony_value_text);
    const QColor amber = themes::color(themes::Role::cpu_meter_warn_text);
    check(cpuLevelProp().isEmpty() && textColour() == plain,
          "at the default budget the CPU item is coloured");
    if (check(percent > AudioSettingsPage::kCpuBudgetMin,
              "the reading is too low to test the budget against")) {
        page->setCpuBudget(std::max(AudioSettingsPage::kCpuBudgetMin, percent / 2));
        check(m_cpuBudgetPercent == page->cpuBudget(),
              "the Settings page's budget did not reach the window");
        updatePolyStatus();
        check(cpuLevelProp() == QStringLiteral("warn") && textColour() == amber && amber != plain,
              "a budget under the reading did not turn the CPU item amber");
        page->setCpuBudget(AudioSettingsPage::kCpuBudgetMax);
        updatePolyStatus();
        check(cpuLevelProp().isEmpty() && textColour() == plain,
              "a budget over the reading did not return the CPU item to plain");
        page->setCpuBudget(std::max(AudioSettingsPage::kCpuBudgetMin, percent / 2));
        updatePolyStatus();
        check(cpuLevelProp() == QStringLiteral("warn"), "the CPU item did not turn amber again");
    }

    // 5b. A seek into the middle of a bar restarts the song there with no
    // bar marks until the meter's next bar line, where they resume.
    {
        const MidiTimeline::MeterMark &meter = timeline->meterMarks.front();
        const uint64_t barTick = meter.tick + 2 * meter.barTicks;
        if (timeline->meterMarks.size() == 1 ||
            timeline->meterMarks[1].tick > barTick + meter.barTicks) {
            m_audio.seek(timeline->sampleForTick(barTick + meter.barTicks / 2));
            m_audio.pumpForTest(chunk, nullptr, chunk);
            check(est.songDeclared && est.barLen == 0,
                  "a mid-bar seek did not hold off the bar marks");
            const uint64_t barLine = timeline->sampleForTick(barTick + meter.barTicks);
            while (m_audio.playheadSamples() <= barLine)
                m_audio.pumpForTest(chunk, nullptr, chunk);
            check(est.barLen == timeline->clocksPerBar(meter),
                  "the bar marks did not resume at the next bar line");
        }
    }

    // 6. Stop: no reading and no colour left behind.
    m_audio.stop();
    m_audio.pumpForTest(chunk, nullptr, chunk);
    updatePolyStatus();
    check(cpuText() == QStringLiteral("--") && cpuLevelProp().isEmpty() && textColour() == plain,
          "stopped, the CPU item kept a reading or a colour");
    check(!est.songDeclared && !est.playing, "stopping did not end the estimate's song");
    page->setCpuBudget(AudioSettingsPage::kCpuBudgetDefault);

    // 7. Play from stop: the peak restarts (D8). A tenth of a second of the
    // song's start sits under the two seconds above.
    m_audio.play();
    int32_t maxLast = 0, laterMin = INT32_MAX;
    bool restartedAbove = true;
    const int startCalls = std::max(4, int(0.1 * m_audio.sampleRate() / chunk));
    for (int i = 0; i < startCalls; i++) {
        m_audio.pumpForTest(chunk, nullptr, chunk);
        const AudioEngine::CpuSnapshot s = m_audio.cpuSnapshot();
        maxLast = std::max(maxLast, s.last[AudioEngine::CpuTotal]);
        if (i > 0) {
            laterMin = std::min(laterMin, s.last[AudioEngine::CpuTotal]);
            restartedAbove =
                restartedAbove && s.peak[AudioEngine::CpuTotal] >= s.last[AudioEngine::CpuTotal];
        }
    }
    {
        const AudioEngine::CpuSnapshot s = m_audio.cpuSnapshot();
        check(s.peakValid && s.peak[AudioEngine::CpuTotal] <= maxLast && restartedAbove,
              "play from stop did not restart the peak at the new playback's frames");
        check(s.peak[AudioEngine::CpuTotal] < peak,
              "after play from stop the peak still holds the earlier playback's maximum");
    }

    // 8. resetCpuPeak: the peak restarts at the next frame.
    m_audio.pumpForTest(uint32_t(2.0 * m_audio.sampleRate()), nullptr, chunk);
    {
        const int32_t before = m_audio.cpuSnapshot().peak[AudioEngine::CpuTotal];
        m_audio.resetCpuPeak();
        int32_t sinceMax = 0;
        bool above = true;
        for (int i = 0; i < 4; i++) {
            m_audio.pumpForTest(chunk, nullptr, chunk);
            const AudioEngine::CpuSnapshot s = m_audio.cpuSnapshot();
            sinceMax = std::max(sinceMax, s.last[AudioEngine::CpuTotal]);
            if (i > 0)
                above = above && s.peak[AudioEngine::CpuTotal] >= s.last[AudioEngine::CpuTotal];
        }
        const int32_t after = m_audio.cpuSnapshot().peak[AudioEngine::CpuTotal];
        check(after <= sinceMax && above, "resetCpuPeak did not restart the peak");
        std::printf("cpucheck: peak %d before resetCpuPeak, %d after\n", before, after);
    }

    m_audio.stop();
    m_audio.pumpForTest(chunk, nullptr, chunk);
    m_audio.holdDeviceForTest(false);
    return g_failures == 0;
}

int runCpuCheck(const QString &projectRoot, const QString &songLabel)
{
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "cpucheck: no temp dir for settings\n");
        return 1;
    }
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QStandardPaths::setTestModeEnabled(true);
    {
        MainWindow window;
        window.show();
        window.runCpuCheck(projectRoot, songLabel);
    }
    if (g_failures == 0)
        std::printf("cpucheck: OK\n");
    return g_failures ? 1 : 0;
}
