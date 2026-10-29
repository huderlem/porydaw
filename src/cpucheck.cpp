#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QStyle>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "core/miditimeline.h"
#include "core/timelineplayer.h"
#include "mainwindow.h"
#include "songsession.h"
#include "ui/audiosettingspage.h"
#include "ui/cpupanel.h"
#include "ui/settingsdialog.h"
#include "ui/theme/themeruntime.h"

extern "C" {
#include "m4a_engine.h"
}

// --cpucheck <projectRoot> <songLabel>: the GBA CPU readout
// (docs/cpu-meter/PLAN.md step 5), with the song open and the audio device
// parked so the callback runs only when pumped. QSettings is redirected
// into a temp dir first. The song must loop, with its loop start inside
// its first bars (mus_abandoned_ship: 4/4, loop start at tick 36).
//
// Covers: cpuLevel's thresholds (the red frame ceiling included: no stock
// configuration reaches it) and the engine-tick bar lengths;
// AudioEngine::cpuSnapshot invalid right after a song loads, valid within a
// few callbacks, its total always the sum of its three terms, the sequencer
// term marked estimated; while the song plays the peak never decreases and
// never sits under the last frame; a pause and resume, and a seek while
// playing, keep it, a play from stop resets it (D8), resetCpuPeak lowers
// it. The status bar's CPU item: shown with the meter, a nonzero percent
// while playing, "--" stopped; amber (cpu_meter_warn_text, through the
// cpuLevel property its style sheet matches) under a budget below the
// reading, plain above it, no colour left after a stop; the tooltip names
// the three terms, marks the estimate, never mentions sound effects (D4).
// The song as declared to the engine's sequencer estimate: started with
// exactly its note tracks, continued (no start-of-song work) on a resume
// and a seek, the meter's bar length and phase after a mid-bar seek, kept
// across a timeline swap, the loop start's meter saved when the start lies
// before it in the same bar and when it lies past it, and restored by the
// GOTO at the wrap. Rendering with the hooks on and off is byte-identical.
// The CPU dock (step 6): hidden by default, View → CPU shows it and the
// saved window state brings it back; "--" with no song and stopped; while
// playing its percent is the status bar's, its peak at least the current,
// its breakdown bar's segments proportional to the three terms and adding
// up to the total; the budget mark follows the budget and the readout its
// colour; Reset lowers the peak; narrow and wide layouts do not overlap;
// no label or tooltip mentions sound effects (D4).

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
                                                s.last[AudioEngine::CpuCgb] +
                                                s.last[AudioEngine::CpuMixer] &&
           (!s.peakValid || s.peak[AudioEngine::CpuTotal] == s.peak[AudioEngine::CpuSequencer] +
                                                                 s.peak[AudioEngine::CpuCgb] +
                                                                 s.peak[AudioEngine::CpuMixer]);
}

// A meter's bar in engine ticks (24 per quarter), worked out here rather
// than through MidiTimeline::clocksPerBar: 4/4 is 96, 3/4 72, 6/8 72.
int expectedBar(int numerator, int denomPow2)
{
    const int quarterTicks = 24;
    const int beatTicks = quarterTicks * 4 / (1 << denomPow2);
    return numerator * beatTicks;
}

// Where the coming engine tick lies in the estimate's bar (-1: no marks).
int estPhase(const M4ASeqEstimate &est)
{
    if (!est.barLen)
        return -1;
    const uint32_t c = est.clock + est.tickInFrame;
    if (c >= est.barStart)
        return int((c - est.barStart) % est.barLen);
    const uint32_t ahead = (est.barStart - c) % est.barLen;
    return ahead ? int(est.barLen - ahead) : 0;
}

// Two phases within `slack` engine ticks of each other, around the bar.
bool samePhase(int a, int b, int bar, int slack)
{
    if (a < 0 || b < 0 || bar <= 0)
        return false;
    const int d = ((a - b) % bar + bar) % bar;
    return d <= slack || d >= bar - slack;
}

// "37%" -> 37; anything else -> -1.
int percentValue(const QString &text)
{
    static const QRegularExpression re(QStringLiteral("^([0-9]+)%$"));
    const QRegularExpressionMatch m = re.match(text);
    return m.hasMatch() ? m.captured(1).toInt() : -1;
}

bool mentionsEffects(const QStringList &strings)
{
    for (const QString &s : strings) {
        const QString lower = s.toLower();
        if (lower.contains(QStringLiteral("effect")) || lower.contains(QStringLiteral("cry")) ||
            lower.contains(QStringLiteral("sfx")))
            return true;
    }
    return false;
}

// Where the bar should put `cycles`, worked out here in floating point:
// the VBlank window over the first three quarters of the track, linear,
// the rest of the frame over the last quarter.
int expectedX(const QRect &track, const AudioEngine::CpuSnapshot &cpu, int64_t cycles)
{
    const double w = track.width();
    const double w0 = std::lround(0.75 * w);
    const double c = std::clamp<double>(double(cycles), 0.0, cpu.frameCycles);
    const double x = c <= cpu.vblankCycles ? c * w0 / cpu.vblankCycles
                                           : w0 + (c - cpu.vblankCycles) * (w - w0) /
                                                      (cpu.frameCycles - cpu.vblankCycles);
    return track.left() + int(std::lround(x));
}

// The breakdown bar against the panel's reading: every segment edge at
// expectedX of its cumulative cycles, the segments end to end from the
// track's left, the three adding up to the total's width; the VBlank
// boundary at three quarters of the track and the frame end at its end.
bool barMatchesReading(const CpuPanel &panel)
{
    const AudioEngine::CpuSnapshot &cpu = panel.reading().cpu;
    const QRect track = panel.barTrackRect();
    if (track.width() <= 0 || cpu.frameCycles <= cpu.vblankCycles)
        return false;
    if (panel.vblankMarkX() != expectedX(track, cpu, cpu.vblankCycles) ||
        panel.vblankMarkX() - track.left() != int(std::lround(0.75 * track.width())) ||
        panel.ceilingMarkX() != track.left() + track.width())
        return false;
    int64_t cumulative = 0;
    int edge = track.left(), sum = 0;
    for (int term = 0; term < 3; term++) {
        const QRect r = panel.segmentRect(term);
        cumulative += cpu.last[term];
        if (r.left() != edge || r.left() + r.width() != expectedX(track, cpu, cumulative))
            return false;
        edge = r.left() + r.width();
        sum += r.width();
    }
    return sum == expectedX(track, cpu, cpu.last[AudioEngine::CpuTotal]) - track.left();
}

// The bar's captions clear of each other, inside the bar, and the bar
// clear of the term rows under it.
bool captionsClear(const CpuPanel &panel)
{
    const QList<QRect> captions = panel.captionRects();
    const QRect bar = panel.barRect();
    const QRect barLocal(QPoint(0, 0), bar.size());
    if (captions.size() < 2)
        return false;
    for (int i = 0; i < captions.size(); i++) {
        if (!barLocal.contains(captions[i]))
            return false;
        for (int j = i + 1; j < captions.size(); j++)
            if (captions[i].intersects(captions[j]))
                return false;
    }
    return !bar.intersects(panel.legendRect());
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
    // The estimate counts engine ticks, 24 per quarter whatever the song's
    // clocks (-X included): the bar of a meter.
    {
        const auto bar = [](uint8_t num, uint8_t den) {
            return MidiTimeline::clocksPerBar(MidiTimeline::MeterMark{0, 0, 0, num, den});
        };
        check(bar(4, 2) == 96 && bar(3, 2) == 72 && bar(6, 3) == 72 && bar(2, 1) == 96 &&
                  bar(5, 2) == 120 && bar(4, 2) == uint32_t(expectedBar(4, 2)),
              "clocksPerBar is not 96 * numerator / denominator engine ticks");
    }

    // The CPU dock: hidden at first, a View-menu toggle, restored with the
    // saved window state.
    {
        QCoreApplication::processEvents();
        QAction *toggle = m_cpuDock->toggleViewAction();
        check(!m_cpuDock->isVisible(), "the CPU dock is shown by default");
        check(m_cpuDock->objectName() == QStringLiteral("cpuDock") &&
                  m_cpuDock->windowTitle() == QStringLiteral("CPU"),
              "the CPU dock's name or title");
        bool inViewMenu = false;
        for (QObject *owner : toggle->associatedObjects()) {
            const auto *menu = qobject_cast<QMenu *>(owner);
            inViewMenu = inViewMenu ||
                         (menu && menu->title().remove(QLatin1Char('&')) == QStringLiteral("View"));
        }
        check(inViewMenu && toggle->text().remove(QLatin1Char('&')) == QStringLiteral("CPU"),
              "the View menu has no CPU entry");
        toggle->trigger();
        QCoreApplication::processEvents();
        check(m_cpuDock->isVisible() && toggle->isChecked(), "View → CPU did not show the dock");
        const QByteArray shown = saveState();
        toggle->trigger();
        QCoreApplication::processEvents();
        check(!m_cpuDock->isVisible(), "View → CPU did not hide the dock again");
        restoreState(shown);
        QCoreApplication::processEvents();
        check(m_cpuDock->isVisible(), "the saved window state did not restore the CPU dock");
        updatePolyStatus();
        updateCpuPanel();
        check(m_cpuPanel->percentText() == QStringLiteral("--") &&
                  m_cpuPanel->peakText() == QStringLiteral("--") &&
                  m_cpuPanel->segmentRect(AudioEngine::CpuMixer).isEmpty(),
              "with no song the CPU dock shows a reading");
        check(!mentionsEffects(m_cpuPanel->visibleStrings()),
              "the CPU dock mentions sound effects with no song (D4)");
    }

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
    const double rate = m_audio.sampleRate();
    const uint32_t chunk = uint32_t(rate / 100.0);
    AudioSettingsPage *page = m_settingsDialog->audioPage();
    const MidiTimeline *timeline = m_audio.timeline();
    const M4ASeqEstimate &est = m_audio.engineForTest()->seqEst;
    const auto cpuText = [this] { return m_cpuValueLabel->text(); };
    const auto cpuLevelProp = [this] { return m_cpuValueLabel->property("cpuLevel").toString(); };
    const auto textColour = [this] {
        return m_cpuValueLabel->palette().color(QPalette::WindowText);
    };
    const auto peakNow = [this] { return m_audio.cpuSnapshot().peak[AudioEngine::CpuTotal]; };
    const auto pumpSeconds = [&](double seconds) {
        m_audio.pumpForTest(uint32_t(seconds * rate), nullptr, chunk);
    };
    if (!check(timeline->hasLoop() && !timeline->meterMarks.empty(),
               "the fixture song has no loop or no meter"))
        return false;

    // The independent expectation of the song's meters: each first-chunk
    // meter mark's bar in engine ticks, and the meter at a tick.
    const auto meterIndexAt = [&](uint64_t tick) {
        size_t i = 0;
        while (i + 1 < timeline->meterMarks.size() && timeline->meterMarks[i + 1].tick <= tick)
            i++;
        return i;
    };
    const auto barAt = [&](uint64_t tick) {
        const MidiTimeline::MeterMark &m = timeline->meterMarks[meterIndexAt(tick)];
        return expectedBar(m.numerator, m.denomPow2);
    };
    // Engine ticks past the bar line at a song tick.
    const auto phaseAt = [&](uint64_t tick) {
        const MidiTimeline::MeterMark &m = timeline->meterMarks[meterIndexAt(tick)];
        const uint64_t smfBar = m.barTicks;
        return int((tick - m.tick) % smfBar * 24 / timeline->ticksPerBeat) % barAt(tick);
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
        check(sumsUp(s), "total != sequencer + CGB + mixer (stopped)");
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
    updateCpuPanel();
    check(m_cpuPanel->percentText() == QStringLiteral("--") && m_cpuPanel->levelName().isEmpty() &&
              m_cpuPanel->segmentRect(AudioEngine::CpuMixer).isEmpty(),
          "stopped, the CPU dock shows a reading, a colour or a bar");

    // 2b. Before the song has ever played the engine's idle SoundMains have
    // a peak, but nothing played it: none is shown (D8).
    check(m_audio.cpuSnapshot().peakValid, "the idle engine has no peak (precondition)");
    check(m_cpuPanel->peakText() == QStringLiteral("--") &&
              m_cpuPanel->markRect(CpuPanel::MarkPeak).isEmpty() &&
              !m_cpuValueLabel->toolTip().contains(QStringLiteral("Peak")),
          "before the first play the CPU readout shows the idle frames' peak");

    // 3. Play two seconds: every frame's cost, the peak monotone.
    m_audio.play();
    int32_t peak = 0;
    int32_t firstPeak = 0;
    bool monotone = true, aboveLast = true, sums = true, estimated = true, valid = true;
    const int calls = int(2.0 * rate / chunk);
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
    check(sums, "total != sequencer + CGB + mixer (playing)");
    check(estimated, "the sequencer term was not marked estimated");
    check(monotone, "the peak decreased while playing");
    check(aboveLast, "the peak sat under the last frame");
    check(peak > firstPeak, "the peak never rose over two seconds of the song");

    // The song as declared: exactly the tracks with notes (up to the BGM
    // player's 10), the meter's bar, the loop start saved once passed.
    {
        uint16_t expectedTracks = 0;
        int counted = 0;
        for (int t = 0; t < 16 && counted < 10; t++) {
            if (timeline->tracks[t].noteCount > 0) {
                expectedTracks |= uint16_t(1u << t);
                counted++;
            }
        }
        check(est.songDeclared && est.playing,
              "playing, the song was not declared to the estimate");
        check(est.declared == expectedTracks && est.exist == expectedTracks,
              "the estimate's song tracks are not exactly the tracks with notes");
        const uint64_t tickNow = uint64_t(timeline->tickForSample(m_audio.playheadSamples()));
        check(est.barLen == barAt(tickNow), "the estimate's bar length is not the song's meter");
        check(est.loopSaved && est.loopBarLen == barAt(timeline->loopStartTick) &&
                  est.loopBarPhase == phaseAt(timeline->loopStartTick),
              "the loop start was passed but its meter not saved");
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

    // The dock while playing: the status bar's percent, a peak at least the
    // current, the bar proportional to the three terms.
    updateCpuPanel();
    {
        const CpuPanel &panel = *m_cpuPanel;
        const AudioEngine::CpuSnapshot &shown = panel.reading().cpu;
        check(panel.percentText() == cpuText() && percentValue(panel.percentText()) > 0,
              "playing, the CPU dock's percent is not the status bar's");
        check(shown.peakValid &&
                  shown.peak[AudioEngine::CpuTotal] >= shown.last[AudioEngine::CpuTotal] &&
                  percentValue(panel.peakText()) >= percentValue(panel.percentText()),
              "the CPU dock's peak is under its current reading");
        check(!panel.cyclesText().isEmpty() && panel.cyclesText() != QStringLiteral("--") &&
                  !panel.peakCyclesText().isEmpty() &&
                  panel.peakCyclesText() != QStringLiteral("--"),
              "the CPU dock does not show the cycles");
        check(barMatchesReading(panel) && panel.segmentRect(AudioEngine::CpuMixer).width() > 0,
              "the CPU dock's bar is not proportional to the sequencer, CGB and mixer terms");
        check(panel.budgetMarkX() == panel.vblankMarkX() &&
                  panel.ceilingMarkX() > panel.vblankMarkX(),
              "at the default budget the budget mark is not the VBlank window's end");
        // The marks: each its own shape and place; the VBlank boundary
        // stays visible over the budget line it coincides with at 100%.
        {
            const QRect vb = panel.markRect(CpuPanel::MarkVblank);
            const QRect bu = panel.markRect(CpuPanel::MarkBudget);
            const QRect pk = panel.markRect(CpuPanel::MarkPeak);
            const QRect ce = panel.markRect(CpuPanel::MarkCeiling);
            const QRect track = panel.barTrackRect();
            check(!vb.isEmpty() && !bu.isEmpty() && !pk.isEmpty() && !ce.isEmpty(),
                  "a mark of the CPU bar is not drawn");
            check(pk.top() > track.bottom() && !pk.intersects(vb) && !pk.intersects(bu) &&
                      pk.size() != vb.size() && vb.size() != bu.size() && ce.left() > vb.right(),
                  "the CPU bar's peak, VBlank and budget marks share a shape or place");
            check(panel.markColor(CpuPanel::MarkVblank) != panel.markColor(CpuPanel::MarkBudget) &&
                      panel.markColor(CpuPanel::MarkCeiling) !=
                          panel.markColor(CpuPanel::MarkBudget) &&
                      panel.markColor(CpuPanel::MarkCeiling) !=
                          panel.markColor(CpuPanel::MarkVblank),
                  "the CPU bar's marks share a colour");
            const QImage bar = panel.barImage();
            const QPoint probe(panel.vblankMarkX(), track.center().y());
            check(bar.rect().contains(probe) &&
                      QColor(bar.pixel(probe)).rgb() == panel.markColor(CpuPanel::MarkVblank).rgb(),
                  "at a 100% budget the VBlank boundary is hidden under the budget line");
            check(captionsClear(panel), "the CPU bar's captions overlap");
        }
        const QStringList strings = panel.visibleStrings();
        check(strings.contains(QStringLiteral("Sequencer (estimated)")) &&
                  strings.contains(QStringLiteral("CGB")) &&
                  strings.contains(QStringLiteral("Mixer")),
              "the CPU dock does not name the three terms or mark the estimate");
        check(!mentionsEffects(strings), "the CPU dock mentions sound effects (D4)");
        std::printf("cpucheck: dock reads %s, peak %s; segments %d/%d/%d px of %d\n",
                    qUtf8Printable(panel.percentText()), qUtf8Printable(panel.peakText()),
                    panel.segmentRect(0).width(), panel.segmentRect(1).width(),
                    panel.segmentRect(2).width(), panel.barTrackWidth());

        // Both layouts, on a standalone panel holding the same reading.
        CpuPanel loose;
        loose.setReading(panel.reading());
        loose.resize(240, 900);
        loose.show();
        QCoreApplication::processEvents();
        check(!loose.wideLayoutActive() &&
                  loose.breakdownSectionRect().top() >= loose.readoutSectionRect().bottom() &&
                  !loose.breakdownSectionRect().intersects(loose.readoutSectionRect()),
              "narrow: the breakdown does not sit below the readout");
        check(barMatchesReading(loose) && loose.barTrackWidth() > 100,
              "narrow: the breakdown bar does not fit the reading");
        check(loose.hScrollRange() == 0, "narrow: the CPU dock's content is wider than the dock");
        check(captionsClear(loose) && !loose.marksText().contains(QStringLiteral("VBlank")),
              "narrow: the bar's captions overlap or the marks line is not the short one");
        const int narrowBar = loose.barTrackWidth();
        loose.resize(820, 500);
        QCoreApplication::processEvents();
        check(loose.wideLayoutActive() &&
                  loose.breakdownSectionRect().left() >= loose.readoutSectionRect().right() &&
                  !loose.breakdownSectionRect().intersects(loose.readoutSectionRect()),
              "wide: the breakdown does not sit right of the readout");
        check(barMatchesReading(loose) && loose.barTrackWidth() > narrowBar &&
                  loose.breakdownSectionRect().contains(loose.barRect()),
              "wide: the breakdown bar does not fit the reading or its section");
        check(captionsClear(loose) && loose.marksText().contains(QStringLiteral("VBlank window")),
              "wide: the bar's captions overlap or the marks line is not the full one");
        QImage image(loose.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        loose.render(&image);
        check(!image.isNull(), "the CPU dock does not render");
        if (const QByteArray shot = qgetenv("PORYDAW_CPUCHECK_SHOT"); !shot.isEmpty()) {
            image.save(QString::fromLocal8Bit(shot) + QStringLiteral("-wide.png"));
            loose.resize(240, 640);
            QCoreApplication::processEvents();
            QImage narrow(loose.size(), QImage::Format_ARGB32_Premultiplied);
            narrow.fill(Qt::white);
            loose.render(&narrow);
            narrow.save(QString::fromLocal8Bit(shot) + QStringLiteral("-narrow.png"));
        }
    }

    // 4. The tooltip: the three terms, the estimate, both budget lines, and
    // nothing about sound effects (D4).
    {
        const QString tip = m_cpuValueLabel->toolTip();
        check(tip == m_cpuCaption->toolTip(), "the caption and the value carry different tooltips");
        check(tip.contains(QStringLiteral("Sequencer")) && tip.contains(QStringLiteral("CGB")) &&
                  tip.contains(QStringLiteral("Mixer")) && !tip.contains(QStringLiteral("PSG")),
              "the tooltip does not name the three terms (CGB, as the status bar does)");
        check(tip.contains(QStringLiteral("(estimated)")),
              "the tooltip does not mark the estimated sequencer term");
        check(tip.contains(QStringLiteral("VBlank window")) &&
                  tip.contains(QStringLiteral("Frame end")) &&
                  tip.contains(QStringLiteral("Budget")) && tip.contains(QStringLiteral("Peak")),
              "the tooltip lacks the budget, VBlank, frame-end or peak line");
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
        const int vblankX = m_cpuPanel->vblankMarkX();
        updateCpuPanel();
        check(m_cpuPanel->budgetMarkX() < vblankX &&
                  m_cpuPanel->levelName() == QStringLiteral("warn") &&
                  m_cpuPanel->percentColor() == amber,
              "a budget under the reading did not move the dock's budget mark or turn it amber");
        page->setCpuBudget(AudioSettingsPage::kCpuBudgetMax);
        updatePolyStatus();
        check(cpuLevelProp().isEmpty() && textColour() == plain,
              "a budget over the reading did not return the CPU item to plain");
        updateCpuPanel();
        check(m_cpuPanel->budgetMarkX() > m_cpuPanel->vblankMarkX() &&
                  m_cpuPanel->budgetMarkX() <= m_cpuPanel->ceilingMarkX() &&
                  m_cpuPanel->levelName().isEmpty() && m_cpuPanel->percentColor() == plain,
              "a budget over the reading did not move the dock's budget mark or clear its colour");
        page->setCpuBudget(std::max(AudioSettingsPage::kCpuBudgetMin, percent / 2));
        updatePolyStatus();
        check(cpuLevelProp() == QStringLiteral("warn"), "the CPU item did not turn amber again");
        page->setCpuBudget(AudioSettingsPage::kCpuBudgetDefault);
    }

    // 6. Pause and resume: the peak stays (D8), the song continues (no
    // start-of-song work: no START init pending).
    {
        const int32_t before = peakNow();
        m_audio.pause();
        pumpSeconds(0.1);
        check(!est.songDeclared, "a pause did not stop the estimate's song");
        m_audio.play();
        m_audio.pumpForTest(chunk, nullptr, chunk);
        check(est.songDeclared && est.playing && est.startPending == 0 &&
                  (est.firstNote & est.declared) == est.declared,
              "a resume did not continue the song (or charged its START init)");
        pumpSeconds(0.2);
        check(peakNow() >= before, "pause and resume reset the peak");
    }

    // 7. A seek into the middle of a bar while playing: the peak stays, the
    // song continues there with its meter in phase, and a timeline swap
    // (an edit) keeps that phase.
    {
        const MidiTimeline::MeterMark &last = timeline->meterMarks.back();
        // Two bars into the last meter: no meter change inside the bar.
        const uint64_t barTick = last.tick + 2 * last.barTicks;
        const uint64_t seekTick = barTick + last.barTicks / 2;
        const int32_t before = peakNow();
        m_audio.seek(timeline->sampleForTick(seekTick));
        m_audio.pumpForTest(chunk, nullptr, chunk);
        check(est.songDeclared && est.startPending == 0 &&
                  (est.firstNote & est.declared) == est.declared,
              "a seek while playing did not continue the song (or charged its START init)");
        check(est.barLen == barAt(seekTick) &&
                  samePhase(estPhase(est), phaseAt(seekTick), barAt(seekTick), 2),
              "a mid-bar seek did not declare the meter in phase");
        std::printf("cpucheck: mid-bar seek to tick %llu: bar %d, phase %d (expected %d)\n",
                    (unsigned long long)seekTick, int(est.barLen), estPhase(est),
                    phaseAt(seekTick));
        const int phaseBefore = estPhase(est);
        m_audio.updateTimeline(timeline, nullptr);
        check(est.barLen == barAt(seekTick) && estPhase(est) == phaseBefore,
              "a timeline swap lost the meter or its phase");
        pumpSeconds(0.2);
        check(peakNow() >= before, "a seek while playing reset the peak");
    }

    // 8. The loop start's meter. (a) A mid-bar start before the loop start,
    // in its bar: the label saves the real meter; (b) a start past the loop
    // start: its meter is declared anyway; and the GOTO restores it.
    const int loopBar = barAt(timeline->loopStartTick);
    const int loopPhase = phaseAt(timeline->loopStartTick);
    const auto loopSavedRight = [&] {
        return est.loopSaved && est.loopBarLen == loopBar && est.loopBarPhase == loopPhase;
    };
    {
        const uint64_t barTicks =
            timeline->meterMarks[meterIndexAt(timeline->loopStartTick)].barTicks;
        const uint64_t halfBeat = std::max<uint32_t>(1, timeline->ticksPerBeat / 2);
        if (check(timeline->loopStartTick > halfBeat,
                  "the fixture's loop start is too early for a start before it")) {
            uint64_t startTick = timeline->loopStartTick - halfBeat;
            if (phaseAt(startTick) == 0 && startTick > 0)
                startTick--; // mid-bar, whatever the loop start's place
            m_audio.stop();
            m_audio.pumpForTest(chunk, nullptr, chunk);
            m_audio.seek(timeline->sampleForTick(startTick));
            m_audio.play();
            m_audio.pumpForTest(chunk, nullptr, chunk);
            check(est.songDeclared && !est.loopSaved && est.barLen == barAt(startTick) &&
                      samePhase(estPhase(est), phaseAt(startTick), barAt(startTick), 2),
                  "(a) a mid-bar start before the loop start did not declare its meter in phase");
            while (m_audio.playheadSamples() <= timeline->loopStartSample + chunk)
                m_audio.pumpForTest(chunk, nullptr, chunk);
            check(loopSavedRight(), "(a) the loop start did not save its real meter");
        }
        // (b) Past the loop start, inside the loop, mid-bar.
        const uint64_t pastTick = timeline->loopStartTick + barTicks + barTicks / 3;
        if (check(timeline->sampleForTick(pastTick) < timeline->loopEndSample,
                  "the fixture's loop is too short to start inside it")) {
            m_audio.seek(timeline->sampleForTick(pastTick));
            m_audio.pumpForTest(chunk, nullptr, chunk);
            check(loopSavedRight(), "(b) a start past the loop start did not declare its meter");
            // The wrap: approach the loop end in small steps, catch the jump.
            m_audio.seek(timeline->loopEndSample - uint64_t(0.2 * rate));
            m_audio.pumpForTest(chunk, nullptr, chunk);
            check(loopSavedRight(), "(b) a seek near the loop end lost the loop start's meter");
            uint64_t prev = m_audio.playheadSamples();
            bool wrapped = false;
            for (int i = 0; i < 4000 && !wrapped; i++) {
                m_audio.pumpForTest(64, nullptr, 64);
                wrapped = m_audio.playheadSamples() < prev;
                prev = m_audio.playheadSamples();
            }
            const int sinceWrap = int((prev - timeline->loopStartSample) * 24.0 *
                                      (timeline->tempoMap.front().bpm / 60.0) / rate) +
                                  1;
            check(wrapped && est.barLen == loopBar &&
                      samePhase(estPhase(est), loopPhase, loopBar, sinceWrap + 1),
                  "the GOTO did not restore the loop start's meter and phase");
            std::printf("cpucheck: loop start bar %d phase %d; after the wrap bar %d phase %d\n",
                        loopBar, loopPhase, int(est.barLen), estPhase(est));
        }
    }

    // 9. Stop: no reading and no colour left behind.
    m_audio.stop();
    m_audio.pumpForTest(chunk, nullptr, chunk);
    updatePolyStatus();
    check(cpuText() == QStringLiteral("--") && cpuLevelProp().isEmpty() && textColour() == plain,
          "stopped, the CPU item kept a reading or a colour");
    updateCpuPanel();
    check(m_cpuPanel->percentText() == QStringLiteral("--") && m_cpuPanel->levelName().isEmpty() &&
              m_cpuPanel->segmentRect(AudioEngine::CpuSequencer).isEmpty() &&
              m_cpuPanel->segmentRect(AudioEngine::CpuMixer).isEmpty(),
          "stopped, the CPU dock kept a reading, a colour or a bar");
    check(!mentionsEffects(m_cpuPanel->visibleStrings()),
          "stopped, the CPU dock mentions sound effects (D4)");
    check(!est.songDeclared && !est.playing, "stopping did not end the estimate's song");

    // 10. Play from stop: the peak restarts (D8). A tenth of a second of the
    // song's start sits under the seconds above.
    peak = peakNow();
    m_audio.play();
    int32_t maxLast = 0;
    bool restartedAbove = true;
    const int startCalls = std::max(4, int(0.1 * rate / chunk));
    for (int i = 0; i < startCalls; i++) {
        m_audio.pumpForTest(chunk, nullptr, chunk);
        const AudioEngine::CpuSnapshot s = m_audio.cpuSnapshot();
        maxLast = std::max(maxLast, s.last[AudioEngine::CpuTotal]);
        if (i > 0)
            restartedAbove =
                restartedAbove && s.peak[AudioEngine::CpuTotal] >= s.last[AudioEngine::CpuTotal];
    }
    {
        const AudioEngine::CpuSnapshot s = m_audio.cpuSnapshot();
        check(s.peakValid && s.peak[AudioEngine::CpuTotal] <= maxLast && restartedAbove,
              "play from stop did not restart the peak at the new playback's frames");
        check(s.peak[AudioEngine::CpuTotal] < peak,
              "after play from stop the peak still holds the earlier playback's maximum");
        // A start leaves each track's implicit VOL to its first note; a
        // continuation marks them all done.
        check(est.songDeclared && (est.firstNote & est.declared) != est.declared,
              "play from stop continued the song instead of starting it");
    }

    // 11. resetCpuPeak: the peak restarts at the next frame, lower than the
    // two seconds' before it.
    pumpSeconds(2.0);
    {
        const int32_t before = peakNow();
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
        const int32_t after = peakNow();
        check(after <= sinceMax && above, "resetCpuPeak did not restart the peak");
        check(after < before, "resetCpuPeak did not lower the peak");
        std::printf("cpucheck: peak %d before resetCpuPeak, %d after\n", before, after);
    }

    // 11b. The dock's Reset: the same, through the button, and the dock
    // shows the lower peak.
    pumpSeconds(2.0);
    {
        updatePolyStatus();
        updateCpuPanel();
        const int32_t before = m_cpuPanel->reading().cpu.peak[AudioEngine::CpuTotal];
        const int beforePercent = percentValue(m_cpuPanel->peakText());
        m_cpuPanel->clickReset();
        for (int i = 0; i < 4; i++)
            m_audio.pumpForTest(chunk, nullptr, chunk);
        updatePolyStatus();
        updateCpuPanel();
        const AudioEngine::CpuSnapshot &shown = m_cpuPanel->reading().cpu;
        check(shown.peakValid && shown.peak[AudioEngine::CpuTotal] < before &&
                  percentValue(m_cpuPanel->peakText()) <= beforePercent &&
                  m_cpuPanel->peakText() ==
                      QStringLiteral("%1%").arg(std::lround(
                          double(shown.peak[AudioEngine::CpuTotal]) * 100.0 / M4A_VBLANK_CYCLES)),
              "the CPU dock's Reset did not lower the peak it shows");
        std::printf("cpucheck: dock peak %s (%d cycles) after Reset, %d before\n",
                    qUtf8Printable(m_cpuPanel->peakText()), shown.peak[AudioEngine::CpuTotal],
                    before);
    }

    // 11c. Stopped, the dock keeps the last playback's peak; a Reset then
    // shows none (not the idle frames' cost) until the song plays again.
    m_audio.stop();
    m_audio.pumpForTest(chunk, nullptr, chunk);
    updatePolyStatus();
    updateCpuPanel();
    check(percentValue(m_cpuPanel->peakText()) > 0 &&
              m_cpuPanel->percentText() == QStringLiteral("--"),
          "stopped, the CPU dock does not keep the last playback's peak");
    m_cpuPanel->clickReset();
    for (int i = 0; i < 4; i++)
        m_audio.pumpForTest(chunk, nullptr, chunk);
    updatePolyStatus();
    updateCpuPanel();
    check(m_cpuPanel->peakText() == QStringLiteral("--") &&
              m_cpuPanel->markRect(CpuPanel::MarkPeak).isEmpty() &&
              !m_cpuValueLabel->toolTip().contains(QStringLiteral("Peak")),
          "a Reset while stopped shows the idle frames' cost as the peak");
    m_audio.play();
    pumpSeconds(0.3);
    updatePolyStatus();
    updateCpuPanel();
    check(percentValue(m_cpuPanel->peakText()) > 0,
          "playing after a Reset while stopped shows no peak");

    // The Settings page's engine change applies after its 200 ms debounce.
    const auto settleEngineApply = [this] {
        QElapsedTimer clock;
        clock.start();
        while ((m_engineApplyTimer->isActive() || clock.elapsed() < 50) && clock.elapsed() < 3000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    };
    // 11d. Stopped, a mix-rate change clears the engine's peak and its idle
    // frames refill it: no peak shown until the song plays again.
    m_audio.stop();
    m_audio.pumpForTest(chunk, nullptr, chunk);
    {
        updatePolyStatus();
        updateCpuPanel();
        check(percentValue(m_cpuPanel->peakText()) > 0,
              "stopped after playing, the dock lost the peak (precondition)");
        EngineSettings faster = m_engineSettings;
        const EngineSettings before = faster;
        faster.pcmMixRate = 21024.0f;
        page->setEngineSettings(faster);
        settleEngineApply();
        for (int i = 0; i < 4; i++)
            m_audio.pumpForTest(chunk, nullptr, chunk);
        updatePolyStatus();
        updateCpuPanel();
        check(m_audio.cpuSnapshot().peakValid && m_cpuPanel->peakText() == QStringLiteral("--") &&
                  !m_cpuValueLabel->toolTip().contains(QStringLiteral("Peak")),
              "a mix-rate change while stopped shows the idle frames' peak");
        page->setEngineSettings(before);
        settleEngineApply();
    }

    m_audio.stop();
    m_audio.pumpForTest(chunk, nullptr, chunk);
    m_audio.holdDeviceForTest(false);

    // 12. The hooks only count: the same span rendered with them on and off
    // is byte-identical (from the start, then across the loop wrap).
    {
        const LoadedVoiceGroup *vg = m_audio.voicegroup();
        const SongSettings settings = songSettingsFor(*m_active);
        std::vector<float> out[2];
        for (int k = 0; k < 2; k++) {
            const auto owned = std::make_unique<M4AEngine>();
            M4AEngine &engine = *owned;
            m4a_engine_init(&engine, float(rate));
            m4a_engine_set_voicegroup(&engine, vg ? const_cast<ToneData *>(vg->voices) : nullptr);
            applySongSettings(&engine, settings);
            const auto ownedPlayer = std::make_unique<TimelinePlayer>();
            TimelinePlayer &player = *ownedPlayer;
            player.setCostHooks(k == 0);
            TimelinePlayer::chase(&engine, timeline, 0);
            player.costSongStart(&engine, timeline, false);
            std::vector<float> l(512), r(512);
            const auto render = [&](double seconds) {
                for (uint32_t done = 0; done < uint32_t(seconds * rate); done += 512) {
                    player.render(&engine, timeline, l.data(), r.data(), 512, true, 0);
                    out[k].insert(out[k].end(), l.begin(), l.end());
                    out[k].insert(out[k].end(), r.begin(), r.end());
                }
            };
            render(2.0);
            const uint64_t pos = timeline->loopEndSample - uint64_t(rate);
            player.seek(pos, timeline);
            TimelinePlayer::chase(&engine, timeline, pos);
            player.costSongStart(&engine, timeline, true);
            render(2.0);
            m4a_engine_destroy(&engine);
        }
        check(out[0].size() == out[1].size() &&
                  std::memcmp(out[0].data(), out[1].data(), out[0].size() * sizeof(float)) == 0,
              "rendering with the cost hooks on differs from rendering with them off");
    }
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
