#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QImage>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QStyle>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <cmath>
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
#include "ui/theme/themeresolver.h"
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
// Covers: cpuLevel's thresholds (amber over the budget, red past the VBlank
// window whatever the budget) and the engine-tick bar lengths;
// AudioEngine::cpuSnapshot invalid right after a song loads, valid within a
// few callbacks, its total always the sum of its three terms, the sequencer
// term marked estimated; while the song plays the peak never decreases and
// never sits under the last frame; a pause and resume, and a seek while
// playing, keep it, a play from stop resets it (D8), resetCpuPeak lowers
// it. The status bar's CPU item: shown with the meter, a nonzero percent
// while playing, "--" stopped; amber (cpu_meter_warn_text, through the
// cpuLevel property its style sheet matches) under a budget below the
// reading, plain above it and at exactly the reading's cycles, no colour
// left after a stop; the tooltip names
// the three terms, marks the estimate, never mentions sound effects (D4).
// The song as declared to the engine's sequencer estimate: started with
// exactly its note tracks, continued (no start-of-song work) on a resume
// and a seek, the meter's bar length and phase after a mid-bar seek, kept
// across a timeline swap, the loop start's meter saved when the start lies
// before it in the same bar and when it lies past it, and restored by the
// GOTO at the wrap. Rendering with the hooks on and off is byte-identical.
// The CPU dock (step 6, redesigned 2026-10-29): on its own with synthetic
// readings, every state's status pill, large percent and cycles line, the
// linear 0..140 % meter (segments, budget and VBlank lines, the peak marker,
// the clamp and chevron past the scale), its captions never overlapping at
// any budget and width, the history's caption and dimming, the table, the
// peak card (hidden with no peak, "at m:ss.s"), the legend, Reset. With the
// song: hidden by default, View → CPU shows it and the saved window state
// brings it back; while playing its percent is the status bar's, its meter
// proportional to the three terms; the budget line follows the budget and
// the readout its colour; AudioEngine's history fills a frame at a time with
// monotonic song positions, survives a stop and is cleared by a play from
// stop and resetCpuPeak; the peak's song position is a history frame's; Reset
// lowers the peak; no label or tooltip mentions sound effects (D4).

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

// The meter's x for `cycles`, worked out here in floating point: one
// linear scale from 0 to 140 % of the VBlank window across the track,
// inset 4 px at both ends.
int expectedX(const QRect &track, int64_t cycles)
{
    const double scale = M4A_VBLANK_CYCLES * 1.4;
    const double c = std::clamp<double>(double(cycles), 0.0, scale);
    return track.left() + 4 + int(std::lround(c / scale * (track.width() - 8)));
}

// The meter against the panel's reading: mixer, sequencer and CGB end to
// end from the scale's 0 in that order, every segment edge at expectedX of
// its cumulative cycles, the three adding up to the total's width; the
// budget and VBlank lines at theirs.
bool meterMatchesReading(const CpuPanel &panel)
{
    const AudioEngine::CpuSnapshot &cpu = panel.reading().cpu;
    const QRect track = panel.meterTrackRect();
    if (track.width() <= 0 || panel.vblankMarkX() != expectedX(track, cpu.vblankCycles) ||
        panel.budgetMarkX() != expectedX(track, panel.reading().budgetCycles))
        return false;
    int64_t cumulative = 0;
    int edge = expectedX(track, 0), sum = 0;
    for (const int term :
         {int(AudioEngine::CpuMixer), int(AudioEngine::CpuSequencer), int(AudioEngine::CpuCgb)}) {
        const QRect r = panel.segmentRect(term);
        cumulative += cpu.last[term];
        if (r.left() != edge || r.left() + r.width() != expectedX(track, cumulative))
            return false;
        edge = r.left() + r.width();
        sum += r.width();
    }
    return sum == expectedX(track, cpu.last[AudioEngine::CpuTotal]) - expectedX(track, 0);
}

// The meter's captions clear of each other and inside the meter; the
// sections clear of each other.
bool captionsClear(const CpuPanel &panel)
{
    const QList<CpuPanel::Caption> captions = panel.meterCaptions();
    const QRect local(QPoint(0, 0), panel.meterRect().size());
    if (captions.size() < 2)
        return false;
    for (int i = 0; i < captions.size(); i++) {
        if (!local.contains(captions[i].rect))
            return false;
        for (int j = i + 1; j < captions.size(); j++)
            if (captions[i].rect.intersects(captions[j].rect))
                return false;
    }
    const QList<QRect> sections = panel.sectionRects();
    for (int i = 0; i < sections.size(); i++)
        for (int j = i + 1; j < sections.size(); j++)
            if (sections[i].intersects(sections[j]))
                return false;
    return true;
}

bool hasCaption(const CpuPanel &panel, const QString &text)
{
    for (const CpuPanel::Caption &c : panel.meterCaptions())
        if (c.text == text)
            return true;
    return false;
}

// A synthetic reading: `total` cycles split 20 % sequencer, 8 % CGB and the
// rest mixer, the peak `peak` (none when 0), a history of `frames` frames
// at `historyTotal`, `over` of them past the budget.
CpuPanel::Reading syntheticReading(int32_t total, int32_t budget, bool playing, int32_t peak,
                                   int frames = 0, int32_t historyTotal = 0, int over = 0)
{
    CpuPanel::Reading r;
    r.loaded = true;
    r.playing = playing;
    r.budgetCycles = budget;
    r.mixRate = 13379;
    r.maxPcm = 5;
    AudioEngine::CpuSnapshot &cpu = r.cpu;
    cpu.valid = playing && total > 0;
    cpu.estimated = true;
    if (cpu.valid) {
        cpu.last[AudioEngine::CpuSequencer] = total / 5;
        cpu.last[AudioEngine::CpuCgb] = total * 2 / 25;
        cpu.last[AudioEngine::CpuMixer] =
            total - cpu.last[AudioEngine::CpuSequencer] - cpu.last[AudioEngine::CpuCgb];
        cpu.last[AudioEngine::CpuTotal] = total;
    }
    if (peak > 0) {
        cpu.peakValid = true;
        cpu.peak[AudioEngine::CpuSequencer] = peak / 5;
        cpu.peak[AudioEngine::CpuCgb] = peak * 2 / 25;
        cpu.peak[AudioEngine::CpuMixer] =
            peak - cpu.peak[AudioEngine::CpuSequencer] - cpu.peak[AudioEngine::CpuCgb];
        cpu.peak[AudioEngine::CpuTotal] = peak;
        cpu.peakAtSeconds = 72.34;
    }
    for (int i = 0; i < frames; i++) {
        AudioEngine::CpuHistoryEntry e;
        const int32_t t = i < over ? budget + 500 : historyTotal;
        e.terms[AudioEngine::CpuMixer] = t;
        e.terms[AudioEngine::CpuTotal] = t;
        e.seconds = i / 59.7275;
        r.history.push_back(e);
    }
    return r;
}

QString cyclesText(int64_t n)
{
    return QLocale().toString(qlonglong(n));
}

QString pct(int64_t cycles)
{
    return QStringLiteral("%1%").arg(std::lround(double(cycles) * 100.0 / M4A_VBLANK_CYCLES));
}

// The panel on its own, with synthetic readings: every state's strings, the
// meter's geometry and captions, the history, the table, the peak card, the
// legend, Reset.
void checkPanelStandalone()
{
    const int32_t budget = AudioSettingsPage::kCpuBudgetDefault;
    const QColor ok = themes::color(themes::Role::cpu_meter_ok_text);
    const QColor amber = themes::color(themes::Role::cpu_meter_warn_text);
    const QColor red = themes::color(themes::Role::cpu_meter_over_text);
    const QColor plain = themes::color(themes::Role::window_text);

    check(CpuPanel::songTime(0.0) == QStringLiteral("0:00.0") &&
              CpuPanel::songTime(12.34) == QStringLiteral("0:12.3") &&
              CpuPanel::songTime(59.96) == QStringLiteral("1:00.0") &&
              CpuPanel::songTime(72.34) == QStringLiteral("1:12.3") &&
              CpuPanel::songTime(605.0) == QStringLiteral("10:05.0"),
          "CpuPanel::songTime is not m:ss.s");

    CpuPanel panel;
    int resets = 0;
    QObject::connect(&panel, &CpuPanel::resetRequested, [&resets] { resets++; });
    panel.resize(460, 1100);
    panel.show();
    QCoreApplication::processEvents();

    // No song.
    panel.setReading(CpuPanel::Reading());
    check(panel.statusText() == QStringLiteral("No song") &&
              panel.percentText() == QStringLiteral("—") && !panel.peakCardShown() &&
              panel.segmentRect(AudioEngine::CpuMixer).isEmpty() && panel.configText().isEmpty(),
          "standalone: with no song the dock shows a reading");

    // Within budget.
    const int32_t okTotal = 40000, okPeak = 50000;
    panel.setReading(syntheticReading(okTotal, budget, true, okPeak, 300, 45000, 0));
    QCoreApplication::processEvents();
    check(panel.statusText() == QStringLiteral("Within budget") && panel.statusColor() == ok &&
              panel.levelName().isEmpty(),
          "standalone: within budget, the pill");
    check(panel.percentText() == pct(okTotal) && panel.percentColor() == plain &&
              panel.heroCaptionText() == QStringLiteral("of the VBlank window"),
          "standalone: within budget, the large percent");
    check(panel.detailText() == QStringLiteral("%1 cycles").arg(cyclesText(okTotal)),
          "standalone: within budget, the cycles line");
    check(meterMatchesReading(panel) && !panel.overflowShown(),
          "standalone: the meter is not linear on 0..140% or the segments do not add up");
    {
        const QRect track = panel.meterTrackRect();
        const int span = track.width() - 8;
        check(std::abs((panel.vblankMarkX() - track.left() - 4) - int(std::lround(span / 1.4))) <=
                  1,
              "standalone: the VBlank line is not at 100/140 of the scale");
        check(panel.budgetMarkX() < panel.vblankMarkX() && panel.budgetMarkX() > track.left(),
              "standalone: the default budget is not left of the VBlank line");
        check(panel.peakMarkX() == expectedX(track, okPeak) &&
                  panel.peakLabelText() == QStringLiteral("Peak %1").arg(pct(okPeak)),
              "standalone: the peak marker or its label");
        check(
            hasCaption(panel, QStringLiteral("Budget ") + cyclesText(budget)) &&
                hasCaption(panel, QStringLiteral("VBlank end ") + cyclesText(M4A_VBLANK_CYCLES)) &&
                hasCaption(panel, QStringLiteral("0")) && hasCaption(panel, QStringLiteral("140%")),
            "standalone: the meter's captions at 460 px");
        check(captionsClear(panel), "standalone: the meter's captions or the sections overlap");
        // The VBlank line, opaque red across the track.
        const QImage img = panel.meterImage();
        const QPoint probe(panel.vblankMarkX(), track.center().y());
        check(img.rect().contains(probe) && QColor(img.pixel(probe)).rgb() == red.rgb(),
              "standalone: the VBlank line is not drawn red");
        const QPoint budgetProbe(panel.budgetMarkX(), track.center().y());
        check(QColor(img.pixel(budgetProbe)).rgb() == amber.rgb(),
              "standalone: the budget line is not drawn amber");
    }
    check(panel.historyPoints() == 300 && !panel.historyDimmed() &&
              panel.historyCaption() ==
                  QStringLiteral("max %1, never over budget").arg(pct(45000)) &&
              panel.historyOverlayText().isEmpty(),
          "standalone: the history while playing, never over budget");
    check(panel.historyYFor(0) > panel.historyYFor(M4A_VBLANK_CYCLES) &&
              panel.historyYFor(int64_t(M4A_VBLANK_CYCLES * 1.4)) ==
                  panel.historyYFor(int64_t(M4A_VBLANK_CYCLES * 2)),
          "standalone: the history's scale is not 0..140% (clamped)");
    {
        const AudioEngine::CpuSnapshot &cpu = panel.reading().cpu;
        bool rows = true;
        for (int term = 0; term < 3; term++) {
            rows = rows && panel.termCyclesText(term) == cyclesText(cpu.last[term]) &&
                   panel.termShareText(term) ==
                       QStringLiteral("%1%").arg(std::lround(cpu.last[term] * 100.0 / okTotal));
        }
        check(rows && panel.sequencerNoteText() == QStringLiteral("est.") &&
                  panel.termBarFillWidth(AudioEngine::CpuMixer) >
                      panel.termBarFillWidth(AudioEngine::CpuSequencer) &&
                  panel.termBarFillWidth(AudioEngine::CpuSequencer) >
                      panel.termBarFillWidth(AudioEngine::CpuCgb),
              "standalone: the table's cycles, shares, bars or the estimate note");
    }
    check(panel.peakCardShown() && panel.peakTitleText() == QStringLiteral("Peak since Play") &&
              panel.peakText() == pct(okPeak) && panel.peakColor() == plain &&
              panel.peakDetailText() ==
                  QStringLiteral("%1 cycles at 1:12.3").arg(cyclesText(okPeak)),
          "standalone: the peak card while playing");
    check(panel.legendTexts() ==
                  QStringList({QStringLiteral("Budget"),
                               QStringLiteral("recommended ceiling for audio"),
                               QStringLiteral("%1 · %2").arg(cyclesText(budget), pct(budget)),
                               QStringLiteral("VBlank"),
                               QStringLiteral("past this, audio eats into the frame"),
                               QStringLiteral("%1 · 100%").arg(cyclesText(M4A_VBLANK_CYCLES)),
                               QStringLiteral("Frame"), QStringLiteral("whole frame, for scale"),
                               QStringLiteral("%1 · 335%").arg(cyclesText(M4A_FRAME_CYCLES))}) &&
              panel.configText() ==
                  QStringLiteral("13379 Hz mix rate · 5 PCM channels · stock mixer"),
          "standalone: the legend or the configuration line");

    // Over budget, a quarter of the window over it.
    const int32_t amberTotal = 70000;
    panel.setReading(syntheticReading(amberTotal, budget, true, 75000, 400, 50000, 100));
    check(panel.statusText() == QStringLiteral("Over budget") && panel.statusColor() == amber &&
              panel.levelName() == QStringLiteral("warn") && panel.percentColor() == amber,
          "standalone: over budget, the pill and the percent");
    check(panel.detailText() == QStringLiteral("%1 cycles").arg(cyclesText(amberTotal)),
          "standalone: over budget, the cycles line");
    check(panel.historyCaption() ==
              QStringLiteral("max %1, over budget 25% of the time").arg(pct(budget + 500)),
          "standalone: the history's share over budget");
    check(panel.peakColor() == amber, "standalone: an amber peak is not amber");
    // The share never rounds to 0% with a frame over, nor to 100% with one
    // under.
    panel.setReading(syntheticReading(amberTotal, budget, true, 75000, 400, 50000, 1));
    check(panel.historyCaption().endsWith(QStringLiteral("over budget 1% of the time")),
          "standalone: one frame over in 400 does not read 1%");
    panel.setReading(syntheticReading(amberTotal, budget, true, 75000, 400, 50000, 399));
    check(panel.historyCaption().endsWith(QStringLiteral("over budget 99% of the time")),
          "standalone: one frame under in 400 does not read 99%");
    panel.setReading(syntheticReading(amberTotal, budget, true, 75000, 400, 50000, 400));
    check(panel.historyCaption().endsWith(QStringLiteral("over budget 100% of the time")),
          "standalone: every frame over does not read 100%");

    // Past VBlank.
    const int32_t redTotal = 90000;
    panel.setReading(syntheticReading(redTotal, budget, true, 95000, 10, 90000, 10));
    check(panel.statusText() == QStringLiteral("Past VBlank") && panel.statusColor() == red &&
              panel.levelName() == QStringLiteral("over") && panel.percentColor() == red &&
              panel.peakColor() == red,
          "standalone: past VBlank, the pill, the percent or the peak");
    check(panel.detailText() == QStringLiteral("%1 cycles").arg(cyclesText(redTotal)),
          "standalone: past VBlank, the cycles line");
    check(meterMatchesReading(panel) && !panel.overflowShown(),
          "standalone: past VBlank, the meter");

    // Past the scale's end: the segments stop at it, with the chevron.
    const int32_t overflow = int32_t(M4A_VBLANK_CYCLES * 1.5);
    panel.setReading(syntheticReading(overflow, budget, true, overflow));
    {
        const QRect track = panel.meterTrackRect();
        const QRect last = panel.segmentRect(AudioEngine::CpuCgb); // the rightmost
        check(panel.overflowShown() && last.left() + last.width() == track.right() + 1 - 4 &&
                  panel.peakMarkX() == last.left() + last.width() && meterMatchesReading(panel),
              "standalone: past 140% the meter does not clamp at its end with the chevron");
        check(panel.percentText() == pct(overflow), "standalone: past 140% the percent");
    }

    // Stopped: the last run's peak and history kept, dimmed.
    panel.setReading(syntheticReading(0, budget, false, okPeak, 200, 45000, 0));
    check(panel.statusText() == QStringLiteral("Stopped") &&
              panel.percentText() == QStringLiteral("—") &&
              panel.heroCaptionText() == QStringLiteral("not playing") &&
              panel.detailText() ==
                  QStringLiteral("Press Play to measure the sound driver frame by frame.") &&
              panel.levelName().isEmpty(),
          "standalone: stopped, the status and the hero");
    check(panel.segmentRect(AudioEngine::CpuMixer).isEmpty() &&
              panel.termCyclesText(AudioEngine::CpuMixer) == QStringLiteral("—") &&
              panel.termShareText(AudioEngine::CpuMixer) == QStringLiteral("—") &&
              panel.termBarFillWidth(AudioEngine::CpuMixer) == 0,
          "standalone: stopped, the meter or the table shows a frame");
    check(panel.historyDimmed() && panel.historyPoints() == 200 &&
              panel.historyCaption() == QStringLiteral("last run") &&
              panel.historyOverlayText() == QStringLiteral("Last run shown dimmed"),
          "standalone: stopped, the last run's history is not kept dimmed");
    check(panel.peakCardShown() && panel.peakTitleText() == QStringLiteral("Peak in last run"),
          "standalone: stopped, the peak card's title");
    // A peak whose place is unknown.
    {
        CpuPanel::Reading r = syntheticReading(0, budget, false, okPeak);
        r.cpu.peakAtSeconds = -1.0;
        panel.setReading(r);
        check(panel.peakDetailText() == QStringLiteral("%1 cycles").arg(cyclesText(okPeak)),
              "standalone: a peak of unknown place says where");
        check(panel.historyPoints() == 0 && !panel.historyDimmed() &&
                  !panel.historyOverlayText().isEmpty(),
              "standalone: no history, no hint");
    }
    // No peak (before the first play): no card.
    panel.setReading(syntheticReading(0, budget, false, 0));
    check(!panel.peakCardShown() && panel.peakText().isEmpty() && panel.peakMarkX() == -1 &&
              panel.peakLabelText().isEmpty(),
          "standalone: with no peak the card or the marker is shown");

    // Reset → the signal.
    panel.setReading(syntheticReading(okTotal, budget, true, okPeak));
    panel.clickReset();
    check(resets == 1, "standalone: Reset peak did not request a reset");

    // The captions at awkward budgets and narrow widths: never overlapping.
    for (const int width : {460, 300, 280}) {
        panel.resize(width, 1100);
        QCoreApplication::processEvents();
        for (const int32_t b : {int32_t(1000), budget, int32_t(M4A_VBLANK_CYCLES - 2000),
                                int32_t(M4A_VBLANK_CYCLES), int32_t(M4A_VBLANK_CYCLES + 6000),
                                int32_t(M4A_VBLANK_CYCLES * 1.38), int32_t(M4A_FRAME_CYCLES)}) {
            panel.setReading(syntheticReading(okTotal, b, true, okPeak, 50, 45000, 5));
            QCoreApplication::processEvents();
            const bool budgetShown = [&] {
                for (const CpuPanel::Caption &c : panel.meterCaptions())
                    if (c.text.startsWith(QStringLiteral("Budget ")))
                        return true;
                return false;
            }();
            char what[160];
            std::snprintf(what, sizeof what,
                          "standalone: at %d px and a budget of %d the captions overlap or the "
                          "budget's is missing",
                          width, int(b));
            check(captionsClear(panel) && budgetShown && meterMatchesReading(panel), what);
        }
        check(panel.hScrollRange() == 0, "standalone: the dock's content is wider than the dock");
    }
    check(!mentionsEffects(panel.visibleStrings()),
          "standalone: the CPU dock mentions sound effects (D4)");
}

// PORYDAW_CPUCHECK_SHOTS=<dir>: the dock in each state, light and dark, at
// 460 and 300 px, as PNGs.
void savePanelShots(const QString &dir)
{
    auto &app = static_cast<QApplication &>(*QCoreApplication::instance());
    const int32_t budget = AudioSettingsPage::kCpuBudgetDefault;
    const struct {
        const char *name;
        themes::Theme theme;
    } looks[] = {{"light", themes::vanilla()},
                 {"dark", themes::darkNeutralHigh()},
                 {"immaterial", themes::immaterial()}};
    const struct {
        const char *name;
        CpuPanel::Reading reading;
    } states[] = {
        {"ok", syntheticReading(40000, budget, true, 52000, 598, 42000, 0)},
        {"amber", syntheticReading(70000, budget, true, 78000, 598, 52000, 150)},
        {"red", syntheticReading(92000, budget, true, 99000, 598, 60000, 300)},
        {"idle", syntheticReading(0, budget, false, 52000, 598, 42000, 0)},
        {"bigbudget",
         syntheticReading(70000, M4A_VBLANK_CYCLES + 9000, true, 78000, 598, 52000, 0)},
    };
    for (const auto &look : looks) {
        themes::apply(app, look.theme);
        for (const auto &state : states) {
            // A wavy history, so the bands show.
            CpuPanel::Reading r = state.reading;
            for (size_t i = 0; i < r.history.size(); i++) {
                const double t = double(i);
                const double base = r.history[i].terms[AudioEngine::CpuTotal];
                const double v =
                    base * (1.0 + 0.12 * std::sin(t * 0.05) + 0.06 * std::sin(t * 0.31));
                r.history[i].terms[AudioEngine::CpuTotal] = int32_t(v);
            }
            for (const int width : {460, 300}) {
                CpuPanel panel;
                panel.setReading(r);
                panel.resize(width, 980);
                panel.show();
                QCoreApplication::processEvents();
                QImage image(panel.size(), QImage::Format_ARGB32_Premultiplied);
                image.fill(themes::color(themes::Role::window_background));
                panel.render(&image);
                image.save(QStringLiteral("%1/cpu-%2-%3-%4.png")
                               .arg(dir, QLatin1String(look.name), QLatin1String(state.name))
                               .arg(width));
            }
        }
    }
}

} // namespace

bool MainWindow::runCpuCheck(const QString &projectRoot, const QString &songLabel)
{
    // The thresholds (D1, amended 2026-10-29): amber strictly above the
    // budget, red strictly past the VBlank window whatever the budget; the
    // whole frame is no threshold.
    constexpr int32_t kDefaultBudget = AudioSettingsPage::kCpuBudgetDefault;
    check(kDefaultBudget == 60000 && CpuPanel::level(kDefaultBudget, kDefaultBudget) == 0 &&
              CpuPanel::level(kDefaultBudget + 1, kDefaultBudget) == 1 &&
              CpuPanel::level(M4A_VBLANK_CYCLES, kDefaultBudget) == 1 &&
              CpuPanel::level(M4A_VBLANK_CYCLES + 1, kDefaultBudget) == 2 &&
              CpuPanel::level(M4A_FRAME_CYCLES, kDefaultBudget) == 2,
          "CpuPanel::level at the budget, budget + 1, VBlank, VBlank + 1 and the frame");
    check(cpuLevel(0, kDefaultBudget) == 0 && cpuLevel(M4A_VBLANK_CYCLES, M4A_VBLANK_CYCLES) == 0 &&
              cpuLevel(M4A_VBLANK_CYCLES + 1, M4A_VBLANK_CYCLES) == 2 &&
              cpuLevel(41888, 41888) == 0 && cpuLevel(41889, 41888) == 1 &&
              cpuLevel(M4A_VBLANK_CYCLES, AudioSettingsPage::kCpuBudgetMax) == 0 &&
              cpuLevel(M4A_VBLANK_CYCLES + 1, AudioSettingsPage::kCpuBudgetMax) == 2 &&
              cpuLevel(M4A_FRAME_CYCLES - 1, M4A_FRAME_CYCLES - 2) == 2 &&
              cpuLevel(M4A_FRAME_CYCLES, AudioSettingsPage::kCpuBudgetMin) == 2,
          "cpuLevel's budget and VBlank thresholds");
    checkPanelStandalone();
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
        check(m_cpuPanel->percentText() == QStringLiteral("—") && !m_cpuPanel->peakCardShown() &&
                  m_cpuPanel->segmentRect(AudioEngine::CpuMixer).isEmpty() &&
                  m_cpuPanel->historyPoints() == 0,
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
    check(m_cpuPanel->percentText() == QStringLiteral("—") && m_cpuPanel->levelName().isEmpty() &&
              m_cpuPanel->statusText() == QStringLiteral("Stopped") &&
              m_cpuPanel->segmentRect(AudioEngine::CpuMixer).isEmpty(),
          "stopped, the CPU dock shows a reading, a colour or a bar");

    // 2b. Before the song has ever played the engine's idle SoundMains have
    // a peak, but nothing played it: none is shown (D8).
    check(m_audio.cpuSnapshot().peakValid, "the idle engine has no peak (precondition)");
    check(!m_cpuPanel->peakCardShown() && m_cpuPanel->peakMarkX() == -1 &&
              !m_cpuValueLabel->toolTip().contains(QStringLiteral("Peak")),
          "before the first play the CPU readout shows the idle frames' peak");
    {
        std::vector<AudioEngine::CpuHistoryEntry> history;
        m_audio.cpuHistory(history);
        check(history.empty() && m_cpuPanel->historyPoints() == 0,
              "before the first play the history holds the idle frames");
    }

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

    // The history: a frame at a time (59.7275 a second) over the two
    // seconds, in song order; the peak's place one of its frames'.
    {
        std::vector<AudioEngine::CpuHistoryEntry> history;
        m_audio.cpuHistory(history);
        const double expected = 2.0 * 59.7275;
        bool monotone = true, sums = true;
        for (size_t i = 0; i < history.size(); i++) {
            const AudioEngine::CpuHistoryEntry &e = history[i];
            monotone = monotone && e.seconds >= 0.0 && e.seconds <= 2.05 &&
                       (i == 0 || e.seconds >= history[i - 1].seconds);
            sums = sums && e.terms[AudioEngine::CpuTotal] == e.terms[AudioEngine::CpuSequencer] +
                                                                 e.terms[AudioEngine::CpuCgb] +
                                                                 e.terms[AudioEngine::CpuMixer];
        }
        std::printf("cpucheck: history %zu frames over 2 s (expected ~%.1f), %.3f..%.3f s\n",
                    history.size(), expected, history.empty() ? 0.0 : history.front().seconds,
                    history.empty() ? 0.0 : history.back().seconds);
        check(std::abs(double(history.size()) - expected) <= 3.0,
              "the history did not get one entry per frame over two seconds");
        check(monotone && sums, "the history's positions are not in song order or its terms "
                                "do not add up");
        check(!history.empty() && history.back().seconds > 1.9,
              "the history's last frame is not near the playhead");
        const AudioEngine::CpuSnapshot s = m_audio.cpuSnapshot();
        bool found = false;
        for (const AudioEngine::CpuHistoryEntry &e : history)
            found = found || (e.terms[AudioEngine::CpuTotal] == s.peak[AudioEngine::CpuTotal] &&
                              e.seconds == s.peakAtSeconds);
        check(s.peakAtSeconds >= 0.0 && found,
              "the peak's song position is not the position of a history frame at the peak");
        std::printf("cpucheck: peak %d cycles at %.3f s\n", s.peak[AudioEngine::CpuTotal],
                    s.peakAtSeconds);
    }

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
        check(panel.detailText() == QLocale().toString(shown.last[AudioEngine::CpuTotal]) +
                                        QStringLiteral(" cycles") &&
                  panel.peakDetailText().startsWith(
                      QLocale().toString(shown.peak[AudioEngine::CpuTotal]) +
                      QStringLiteral(" cycles")),
              "the CPU dock does not show the cycles");
        check(meterMatchesReading(panel) && panel.segmentRect(AudioEngine::CpuMixer).width() > 0,
              "the CPU dock's meter is not proportional to the sequencer, CGB and mixer terms");
        check(panel.reading().budgetCycles == kDefaultBudget &&
                  panel.budgetMarkX() > panel.meterTrackRect().left() &&
                  panel.budgetMarkX() < panel.vblankMarkX(),
              "at the default budget the budget line is not left of the VBlank line");
        check(captionsClear(panel), "at the default budget the CPU meter's captions overlap");
        const int lvl = CpuPanel::level(shown.last[AudioEngine::CpuTotal], kDefaultBudget);
        const QString status = lvl == 2   ? QStringLiteral("Past VBlank")
                               : lvl == 1 ? QStringLiteral("Over budget")
                                          : QStringLiteral("Within budget");
        check(panel.statusText() == status, "playing, the dock's status pill is not its level's");
        check(panel.peakCardShown() && panel.peakTitleText() == QStringLiteral("Peak since Play") &&
                  panel.peakDetailText().contains(QStringLiteral(" at 0:0")),
              "playing, the peak card is hidden or does not say where the peak was");
        check(panel.historyPoints() > 100 &&
                  panel.historyCaption().startsWith(QStringLiteral("max ")) &&
                  !panel.historyDimmed(),
              "playing, the dock's history is not filled in");
    }
    // A budget of exactly the VBlank window puts its line on the VBlank
    // boundary.
    page->setCpuBudget(M4A_VBLANK_CYCLES);
    updatePolyStatus();
    updateCpuPanel();
    {
        const CpuPanel &panel = *m_cpuPanel;
        check(panel.budgetMarkX() == panel.vblankMarkX(),
              "at a budget of the VBlank window the budget line is not the window's end");
        // The VBlank line stays visible over the budget line it coincides
        // with here.
        const QRect track = panel.meterTrackRect();
        const QImage meter = panel.meterImage();
        const QPoint probe(panel.vblankMarkX(), track.center().y());
        check(meter.rect().contains(probe) &&
                  QColor(meter.pixel(probe)).rgb() ==
                      themes::color(themes::Role::cpu_meter_over_text).rgb(),
              "at a 100% budget the VBlank line is hidden under the budget line");
        check(captionsClear(panel), "at a 100% budget the CPU meter's captions overlap");
    }
    page->setCpuBudget(AudioSettingsPage::kCpuBudgetDefault);
    updatePolyStatus();
    updateCpuPanel();
    {
        const CpuPanel &panel = *m_cpuPanel;
        const QStringList strings = panel.visibleStrings();
        check(strings.contains(QStringLiteral("Sequencer")) &&
                  panel.sequencerNoteText() == QStringLiteral("est.") &&
                  strings.contains(QStringLiteral("CGB")) &&
                  strings.contains(QStringLiteral("Mixer")),
              "the CPU dock does not name the three terms or mark the estimate");
        check(!mentionsEffects(strings), "the CPU dock mentions sound effects (D4)");
        std::printf("cpucheck: dock reads %s, peak %s; segments %d/%d/%d px of %d\n",
                    qUtf8Printable(panel.percentText()), qUtf8Printable(panel.peakText()),
                    panel.segmentRect(0).width(), panel.segmentRect(1).width(),
                    panel.segmentRect(2).width(), panel.meterTrackRect().width());

        // The live reading at the reference width and a sliver dock.
        CpuPanel loose;
        loose.setReading(panel.reading());
        for (const int width : {460, 300}) {
            loose.resize(width, 1000);
            loose.show();
            // The legend's relayout posts the scroll area's own.
            QCoreApplication::processEvents();
            QCoreApplication::processEvents();
            check(meterMatchesReading(loose) && captionsClear(loose) && loose.hScrollRange() == 0,
                  "the live reading's meter does not fit, its captions overlap or the dock "
                  "scrolls sideways");
        }
        if (const QByteArray shot = qgetenv("PORYDAW_CPUCHECK_SHOT"); !shot.isEmpty()) {
            QImage image(loose.size(), QImage::Format_ARGB32_Premultiplied);
            image.fill(themes::color(themes::Role::window_background));
            loose.render(&image);
            image.save(QString::fromLocal8Bit(shot) + QStringLiteral("-live.png"));
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
                  tip.contains(QStringLiteral("red past it")) &&
                  tip.contains(QStringLiteral("Whole frame")) &&
                  tip.contains(QStringLiteral("for scale")) &&
                  tip.contains(QStringLiteral("Budget")) && tip.contains(QStringLiteral("Peak")) &&
                  !tip.contains(QStringLiteral("skipped")),
              "the tooltip lacks the budget, VBlank, whole-frame or peak line");
        check(tip.contains(QStringLiteral(" at 0:0")),
              "the tooltip's peak line does not say where in the song it was");
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
    const QColor red = themes::color(themes::Role::cpu_meter_over_text);
    const int32_t total = m_lastPolyStatus ? m_lastPolyStatus->cpu.last[AudioEngine::CpuTotal] : 0;
    {
        const int lvl = cpuLevel(total, kDefaultBudget);
        check(lvl == 2   ? cpuLevelProp() == QStringLiteral("over") && textColour() == red
              : lvl == 1 ? cpuLevelProp() == QStringLiteral("warn") && textColour() == amber
                         : cpuLevelProp().isEmpty() && textColour() == plain,
              "at the default budget the CPU item's colour is not its level's");
    }
    if (check(total > 2 * AudioSettingsPage::kCpuBudgetMin,
              "the reading is too low to test the budget against")) {
        page->setCpuBudget(std::max(AudioSettingsPage::kCpuBudgetMin, total / 2));
        check(m_cpuBudgetCycles == page->cpuBudget() && m_cpuBudgetCycles == total / 2,
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
                  m_cpuPanel->budgetMarkX() ==
                      m_cpuPanel->meterXFor(int64_t(M4A_VBLANK_CYCLES * 1.4)) &&
                  m_cpuPanel->levelName().isEmpty() &&
                  m_cpuPanel->percentColor() == themes::color(themes::Role::window_text) &&
                  captionsClear(*m_cpuPanel),
              "a budget past the scale did not put the budget line at its end or clear the "
              "colour");
        // The threshold in cycles: a budget of exactly the reading is not
        // over it, one cycle less is.
        page->setCpuBudget(total);
        updatePolyStatus();
        check(cpuLevelProp().isEmpty() && textColour() == plain,
              "a budget of exactly the reading's cycles turned the CPU item amber");
        page->setCpuBudget(total - 1);
        updatePolyStatus();
        check(cpuLevelProp() == QStringLiteral("warn"),
              "a budget one cycle under the reading did not turn the CPU item amber");
        page->setCpuBudget(std::max(AudioSettingsPage::kCpuBudgetMin, total / 2));
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

            // The history across a wrap inside one callback: the frames
            // before it lie between the callback's start and the loop end,
            // the ones after it between the loop start and the playhead,
            // none in the song's intro.
            const uint64_t from = timeline->loopEndSample - 2048;
            m_audio.seek(from);
            m_audio.pumpForTest(chunk, nullptr, chunk); // the seek lands
            m_audio.seek(from);
            std::vector<AudioEngine::CpuHistoryEntry> before;
            const uint32_t headBefore = uint32_t(m_audio.cpuHistory(before));
            m_audio.pumpForTest(4096, nullptr, 4096);
            std::vector<AudioEngine::CpuHistoryEntry> after;
            const uint32_t added = uint32_t(m_audio.cpuHistory(after)) - headBefore;
            const double loopStartS = double(timeline->loopStartSample) / rate;
            const double loopEndS = double(timeline->loopEndSample) / rate;
            const double fromS = double(from) / rate;
            const double nowS = double(m_audio.playheadSamples()) / rate;
            int pre = 0, post = 0;
            bool placed = added >= 3 && added <= after.size();
            for (size_t k = after.size() - std::min<size_t>(added, after.size()); k < after.size();
                 k++) {
                const double s = after[k].seconds;
                if (s >= fromS - 1e-6 && s <= loopEndS + 1e-6)
                    pre++;
                else if (s >= loopStartS - 1e-6 && s <= nowS + 1e-6)
                    post++;
                else
                    placed = false;
            }
            check(m_audio.playheadSamples() < timeline->loopEndSample && placed && pre > 0 &&
                      post > 0,
                  "across a loop wrap the history placed a frame outside the loop's end and "
                  "start");
            std::printf("cpucheck: wrap in one callback: %u frames, %d before the loop end, %d "
                        "after the loop start\n",
                        added, pre, post);
        }
    }

    // 9. Stop: no reading and no colour left behind.
    m_audio.stop();
    m_audio.pumpForTest(chunk, nullptr, chunk);
    updatePolyStatus();
    check(cpuText() == QStringLiteral("--") && cpuLevelProp().isEmpty() && textColour() == plain,
          "stopped, the CPU item kept a reading or a colour");
    updateCpuPanel();
    check(m_cpuPanel->percentText() == QStringLiteral("—") && m_cpuPanel->levelName().isEmpty() &&
              m_cpuPanel->segmentRect(AudioEngine::CpuSequencer).isEmpty() &&
              m_cpuPanel->segmentRect(AudioEngine::CpuMixer).isEmpty(),
          "stopped, the CPU dock kept a reading, a colour or a bar");
    // The last run's history stays, dimmed, through the stop.
    std::vector<AudioEngine::CpuHistoryEntry> lastRun;
    m_audio.cpuHistory(lastRun);
    pumpSeconds(0.3);
    {
        std::vector<AudioEngine::CpuHistoryEntry> after;
        m_audio.cpuHistory(after);
        updatePolyStatus();
        updateCpuPanel();
        check(!lastRun.empty() && after.size() == lastRun.size() &&
                  after.back().seconds == lastRun.back().seconds,
              "a stop (and the idle frames after it) changed the history");
        check(m_cpuPanel->historyDimmed() &&
                  m_cpuPanel->historyCaption() == QStringLiteral("last run") &&
                  m_cpuPanel->peakCardShown() &&
                  m_cpuPanel->peakTitleText() == QStringLiteral("Peak in last run"),
              "stopped, the dock does not show the last run dimmed with its peak");
    }
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
        std::vector<AudioEngine::CpuHistoryEntry> history;
        m_audio.cpuHistory(history);
        check(!history.empty() &&
                  double(history.size()) <= double(startCalls * chunk) / rate * 59.7275 + 2.0 &&
                  history.front().seconds < 0.05,
              "play from stop did not start the history over");
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
        std::vector<AudioEngine::CpuHistoryEntry> history;
        m_audio.cpuHistory(history);
        check(history.size() <= 4, "resetCpuPeak did not start the history over");
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
              m_cpuPanel->percentText() == QStringLiteral("—"),
          "stopped, the CPU dock does not keep the last playback's peak");
    m_cpuPanel->clickReset();
    for (int i = 0; i < 4; i++)
        m_audio.pumpForTest(chunk, nullptr, chunk);
    updatePolyStatus();
    updateCpuPanel();
    check(!m_cpuPanel->peakCardShown() && m_cpuPanel->peakMarkX() == -1 &&
              !m_cpuValueLabel->toolTip().contains(QStringLiteral("Peak")),
          "a Reset while stopped shows the idle frames' cost as the peak");
    check(m_cpuPanel->historyPoints() == 0, "a Reset while stopped kept the last run's history");
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
        check(m_audio.cpuSnapshot().peakValid && !m_cpuPanel->peakCardShown() &&
                  m_cpuPanel->historyPoints() == 0 &&
                  !m_cpuValueLabel->toolTip().contains(QStringLiteral("Peak")),
              "a mix-rate change while stopped shows the idle frames' peak");
        page->setEngineSettings(before);
        settleEngineApply();
    }

    m_audio.stop();
    m_audio.pumpForTest(chunk, nullptr, chunk);
    m_audio.holdDeviceForTest(false);
    if (const QByteArray dir = qgetenv("PORYDAW_CPUCHECK_SHOTS"); !dir.isEmpty())
        savePanelShots(QString::fromLocal8Bit(dir));

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
