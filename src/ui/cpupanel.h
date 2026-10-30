#pragma once

#include <QImage>
#include <QList>
#include <QRect>
#include <QStringList>
#include <QWidget>
#include <vector>

#include "audio/audioengine.h"
#include "ui/cpubudget.h"

class QGridLayout;
class QLabel;
class QPushButton;
class QScrollArea;
class CpuStatusPill;
class CpuMeter;
class CpuHistoryChart;
class CpuShareBar;

// The CPU dock (SPEC §6.1, docs/cpu-meter/PLAN.md D2): the song engine's
// estimated GBA CPU cost of m4aSoundMain per frame, in percent of the VBlank
// window (D1). Top to bottom, in one column: a status pill (within budget,
// over budget, past VBlank, stopped); the current frame's percent in large
// type with its cycles; a linear meter from 0 to 140 % of the
// VBlank window with the budget and VBlank lines, the three terms stacked
// and the peak marked; the last ten seconds of playback frame by frame; the
// sequencer, CGB and mixer terms' cycles and share; the peak since playback
// started from stop (D8) with where in the song it was and a Reset; a legend
// of the budget, VBlank and whole-frame lines; the engine configuration.
// Fed by MainWindow's uiTick (a snapshot copy: the panel never reads the
// engine).
class CpuPanel : public QWidget
{
    Q_OBJECT

  public:
    // One reading, as the status bar's CPU item shows it: a current frame
    // only while the song plays (`cpu.valid` false otherwise), the peak
    // whenever one is shown, the history of the last run (kept while
    // stopped, until the next play from stop).
    struct Reading {
        bool loaded = false;
        bool playing = false;
        AudioEngine::CpuSnapshot cpu;
        std::vector<AudioEngine::CpuHistoryEntry> history; // oldest first
        int32_t budgetCycles = cpubudget::kDefault;
        int mixRate = 0;
        int maxPcm = 0;
        bool hqMixer = false;

        bool operator==(const Reading &o) const;
    };

    explicit CpuPanel(QWidget *parent = nullptr);

    void setReading(const Reading &reading);
    const Reading &reading() const { return m_reading; }

    // The readout's colour for a frame of `total` cycles against a budget
    // in cycles (D1, amended 2026-10-29): 0 within the budget, 1 over it
    // (amber, budgetCycles < total <= the VBlank window), 2 past the VBlank
    // window (red), whatever the budget. The status bar and the profile
    // strip use it too.
    static int level(int32_t total, int32_t budgetCycles);
    // A song position as "m:ss.s" (the peak card's "at ...").
    static QString songTime(double seconds);
    // The meter's and the history's scale: 0 to this percent of the VBlank
    // window.
    static constexpr int kScalePercent = 140;

    // Harness hooks (--cpucheck).
    QString statusText() const;  // the pill
    QString percentText() const; // the large percent, "—" without a reading
    QString heroCaptionText() const;
    QString detailText() const; // the line under it, as plain text
    QString levelName() const;  // "", "warn", "over" (the current frame's)
    QColor percentColor() const;
    QColor statusColor() const;
    // The meter, in its own coordinates.
    QRect meterTrackRect() const;
    int meterXFor(int64_t cycles) const;
    int budgetMarkX() const;
    int vblankMarkX() const;
    int peakMarkX() const;             // -1 when no peak is shown
    QRect segmentRect(int term) const; // CpuSequencer, CpuCgb, CpuMixer; empty without a reading
    bool overflowShown() const;        // the current frame is past the scale's end
    QString peakLabelText() const;     // above the marker, "" when hidden
    struct Caption {
        QRect rect;
        QString text;
    };
    QList<Caption> meterCaptions() const; // under the track (and the peak label above it)
    QImage meterImage() const;
    QRect meterRect() const; // the meter in the content's coordinates
    // The history chart.
    QString historyCaption() const;
    int historyPoints() const;
    bool historyDimmed() const;
    QString historyOverlayText() const;
    QImage historyImage() const;
    QRect historyRect() const; // in the content's coordinates
    int historyYFor(int64_t cycles) const;
    // The breakdown table, by term (CpuSequencer, CpuCgb, CpuMixer).
    QString termCyclesText(int term) const;
    QString termShareText(int term) const;
    int termBarFillWidth(int term) const;
    QString sequencerNoteText() const;
    // The peak card.
    bool peakCardShown() const;
    QString peakTitleText() const;
    QString peakText() const; // "" when the card is hidden
    QString peakDetailText() const;
    QColor peakColor() const;
    // The legend's rows (name, description, value) and the configuration.
    QStringList legendTexts() const;
    QString configText() const;
    // The sections' geometry in the content's coordinates, top to bottom
    // (visible ones only).
    QList<QRect> sectionRects() const;
    int hScrollRange() const; // >0 when the content is wider than the dock
    // Every label's text and tooltip, the painted strings, the tooltips of
    // the painted parts and the button's.
    QStringList visibleStrings() const;
    void clickReset(); // same path as clicking Reset

  signals:
    void resetRequested();

  protected:
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;

  private:
    // Narrower than this, the legend's descriptions go under their marks
    // and the peak card's Reset beside its title.
    static constexpr int kNarrowWidth = 400;
    void applyFonts();
    void refresh();
    void setNarrow(bool narrow);
    QLabel *sectionLabel(const QString &text, QWidget *parent);

    QScrollArea *m_scroll = nullptr;
    QWidget *m_content = nullptr;
    QList<QLabel *> m_sectionLabels;
    QList<QLabel *> m_secondaryLabels; // drawn in the secondary text colour
    QList<QLabel *> m_monoLabels;      // fixed pitch at the body size
    QList<QWidget *> m_sections;
    CpuStatusPill *m_pill = nullptr;
    QLabel *m_heroTitle = nullptr;
    QLabel *m_percent = nullptr;
    QLabel *m_heroCaption = nullptr;
    QLabel *m_detail = nullptr;
    CpuMeter *m_meter = nullptr;
    QLabel *m_historyCaption = nullptr;
    CpuHistoryChart *m_chart = nullptr;
    QLabel *m_termName[3] = {};
    QLabel *m_termNote[3] = {};
    CpuShareBar *m_termBar[3] = {};
    QLabel *m_termCycles[3] = {};
    QLabel *m_termShare[3] = {};
    QWidget *m_peakCard = nullptr;
    QLabel *m_peakTitle = nullptr;
    QLabel *m_peak = nullptr;
    QLabel *m_peakDetail = nullptr;
    QPushButton *m_reset = nullptr;
    QGridLayout *m_peakGrid = nullptr;
    QWidget *m_peakLine = nullptr;
    QGridLayout *m_legendGrid = nullptr;
    bool m_narrow = false;
    QWidget *m_legendNameBox[3] = {};
    QLabel *m_legendName[3] = {};
    QLabel *m_legendText[3] = {};
    QLabel *m_legendValue[3] = {};
    QLabel *m_config = nullptr;

    Reading m_reading;
    bool m_haveReading = false; // m_reading was applied at least once
    QString m_levelName;
    QString m_detailPlain;
};
