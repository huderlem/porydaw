#pragma once

#include <QElapsedTimer>
#include <QStringList>
#include <QWidget>

#include "audio/audioengine.h"

class QCheckBox;
class QGridLayout;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QScrollArea;
class QTableWidget;
class MidiTimeline;
class PolyChannelGrid;

// The Polyphony dock (SPEC §6.1): visualizes which tracks are losing sound to
// the engine's polyphony limit. Mirrors the poryaaaa CLAP plugin's Polyphony
// tab — solo-overflow (invert audio) toggle, live channel-usage grid with the
// shadow pool, per-track overflow counters with a flash on increase, and a
// recent-events log — plus DAW-only context: document track names, and tick
// timestamps shown as bar:beat with double-click jumping the edit cursor to
// the event's position.
class PolyphonyPanel : public QWidget
{
    Q_OBJECT

public:
    explicit PolyphonyPanel(QWidget *parent = nullptr);

    // Session context, pushed by MainWindow at the same choke points where
    // SongView's borrowed pointers are updated. The timeline is borrowed (the
    // active tab's); names are copied.
    void setTimeline(const MidiTimeline *timeline);
    void setTrackNames(const QStringList &names);
    void setVoiceNames(const QStringList &names);
    void clearSession();

    // Feed one engine snapshot (MainWindow's uiTick, while the dock is
    // visible). Drives the grid, the counter table, and the event log.
    void updateSnapshot(const AudioEngine::PolySnapshot &snap);

    bool invertChecked() const;

    // Harness hooks (--polycheck).
    void setInvertChecked(bool on); // same path as clicking the checkbox
    int logRowCount() const;
    QString logRowText(int row) const;
    void activateLogRow(int row); // same path as a double-click
    bool wideLayoutActive() const;
    QRect usageSectionRect() const;
    QRect overflowSectionRect() const;
    bool gridFullyVisible() const;
    int vScrollRange() const; // >0 when the content scrolls (short window)

signals:
    void invertToggled(bool on);
    void resetRequested();
    // Double-clicked event row: jump the edit cursor to tick and reveal the
    // lost note (track + midiKey identify it; see SongView::revealNote).
    void jumpToEvent(uint64_t tick, int track, int midiKey);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void appendEvent(const M4APolyEvent &ev);
    void refreshTable(const AudioEngine::PolySnapshot &snap);
    void clearRuntimeState();
    // Stacked sections when narrow, overflow table beside the channel grid
    // when the panel is at least kWideLayoutMinWidth wide.
    void setWideLayout(bool wide);

    QCheckBox *m_invert = nullptr;
    PolyChannelGrid *m_grid = nullptr;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_usageBox = nullptr;
    QWidget *m_overflowBox = nullptr;
    QGridLayout *m_bodyGrid = nullptr;
    bool m_wideLayout = false;
    QTableWidget *m_table = nullptr;
    QLabel *m_tableEmpty = nullptr;
    QPushButton *m_reset = nullptr;
    QListWidget *m_log = nullptr;

    const MidiTimeline *m_timeline = nullptr; // not owned (the active tab's)
    QStringList m_trackNames;
    QStringList m_voiceNames;

    // Counter change detection for the row flash, CLAP-style: flash a track's
    // row red for ~1s after any of its counters increases.
    QElapsedTimer m_clock;
    bool m_prevValid = false;
    uint32_t m_prevDrop[MAX_TRACKS] = {};
    uint32_t m_prevSteal[MAX_TRACKS] = {};
    uint32_t m_prevTailCut[MAX_TRACKS] = {};
    qint64 m_flashMs[MAX_TRACKS] = {};

    uint32_t m_lastSeenTotal = 0; // events drained from the engine ring
};
