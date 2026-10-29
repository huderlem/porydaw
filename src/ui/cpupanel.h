#pragma once

#include <QImage>
#include <QList>
#include <QRect>
#include <QStringList>
#include <QWidget>

#include "audio/audioengine.h"

class QGridLayout;
class QLabel;
class QPushButton;
class QScrollArea;
class CpuBar;

// The CPU dock (SPEC §6.1, docs/cpu-meter/PLAN.md D2): the song engine's
// estimated GBA CPU cost of m4aSoundMain per frame, in percent of the VBlank
// window (D1). The current frame and the peak since playback started from
// stop (D8) in large type, their cycles, a breakdown bar of the sequencer,
// CGB and mixer terms across one whole GBA frame with the budget, VBlank and
// frame-ceiling marks, the engine configuration and a Reset for the peak.
// Fed by MainWindow's uiTick from the status bar's reading (a snapshot copy:
// the panel never reads the engine).
class CpuPanel : public QWidget
{
    Q_OBJECT

  public:
    // One reading, as the status bar's CPU item shows it: a current frame
    // only while the song plays (`cpu.valid` false otherwise), the peak
    // whenever the engine has one.
    struct Reading {
        bool loaded = false;
        bool playing = false;
        AudioEngine::CpuSnapshot cpu;
        int budgetPercent = 100;
        int mixRate = 0;
        int maxPcm = 0;
        bool hqMixer = false;

        bool operator==(const Reading &o) const;
    };

    explicit CpuPanel(QWidget *parent = nullptr);

    void setReading(const Reading &reading);
    const Reading &reading() const { return m_reading; }

    // The readout's colour for a frame of `total` cycles against a budget
    // in percent of the VBlank window (D1): 0 normal, 1 over the budget
    // (amber), 2 at the frame ceiling (red). The status bar uses it too.
    static int level(int32_t total, int budgetPercent);

    // Harness hooks (--cpucheck).
    QString percentText() const;
    QString peakText() const;
    QString cyclesText() const;
    QString peakCyclesText() const;
    QString levelName() const; // the percent label's cpuLevel: "", "warn", "over"
    QColor percentColor() const;
    // The breakdown bar, in the bar widget's coordinates: a term's segment
    // (CpuSequencer, CpuCgb, CpuMixer; empty without a reading), the width
    // standing for the whole frame, and the marks' x positions.
    QRect segmentRect(int term) const;
    int barTrackWidth() const;
    int budgetMarkX() const;
    int vblankMarkX() const;
    int ceilingMarkX() const;
    bool wideLayoutActive() const;
    QRect readoutSectionRect() const;
    QRect breakdownSectionRect() const;
    enum Mark { MarkVblank, MarkBudget, MarkPeak, MarkCeiling };
    QRect markRect(Mark mark) const; // in the bar's coordinates; empty when not drawn
    QColor markColor(Mark mark) const;
    QList<QRect> captionRects() const; // the bar's painted captions, bar coordinates
    QRect barTrackRect() const;        // the track, bar coordinates
    QImage barImage() const;           // the bar as painted
    QRect legendRect() const;          // the term rows, content coordinates
    QString marksText() const;
    int hScrollRange() const; // >0 when the content is wider than the dock
    QRect barRect() const;    // the bar widget in the panel's content coordinates
    // Every label's text and tooltip, the bar's tooltip and the button's.
    QStringList visibleStrings() const;
    void clickReset(); // same path as clicking Reset

  signals:
    void resetRequested();

  protected:
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;

  private:
    void applyFonts();
    void setWideLayout(bool wide);
    void refresh();

    QScrollArea *m_scroll = nullptr;
    QWidget *m_readoutBox = nullptr;
    QWidget *m_breakdownBox = nullptr;
    QGridLayout *m_bodyGrid = nullptr;
    bool m_wideLayout = false;
    QList<QLabel *> m_sectionLabels;
    QLabel *m_percent = nullptr;
    QLabel *m_percentCaption = nullptr;
    QLabel *m_cycles = nullptr;
    QLabel *m_note = nullptr;
    QLabel *m_peak = nullptr;
    QLabel *m_peakCycles = nullptr;
    QPushButton *m_reset = nullptr;
    CpuBar *m_bar = nullptr;
    QLabel *m_termName[3] = {};
    QLabel *m_termCycles[3] = {};
    QLabel *m_termPercent[3] = {};
    QWidget *m_termsBox = nullptr;
    QLabel *m_marks = nullptr;
    QLabel *m_config = nullptr;

    Reading m_reading;
    bool m_haveReading = false; // m_reading was applied at least once
};
