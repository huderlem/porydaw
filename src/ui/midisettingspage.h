#pragma once

#include <QWidget>

#include <array>

#include "audio/midiroutes.h"

class MidiInput;
class QCheckBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QSpinBox;

// Settings window, MIDI page (docs/midi-input/PLAN.md step 1): which input
// ports porydaw listens to, how incoming notes play and record, and which
// keyboard control drives which lane (the Controls table,
// docs/midi-controls/PLAN.md step 1: a CC or None per lane, and Learn). Like
// every Settings page it applies as controls change; the MidiInput service
// owns persisting (midi/* keys) and reopening ports.
//
// Without a MIDI backend the page is an explanatory label and a "Try Again"
// button; once the backend starts the full controls show. The harness can
// ask for the controls anyway (showWithoutBackend) to drive the preference
// plumbing on a machine with no MIDI system (WSL, CI).
class MidiSettingsPage : public QWidget
{
    Q_OBJECT

  public:
    explicit MidiSettingsPage(MidiInput &midi, QWidget *parent = nullptr,
                              bool showWithoutBackend = false);

    // Names the device latency Auto uses in the offset box ("Auto (21 ms)");
    // a negative value shows plain "Auto".
    void setAutoLatencyMs(int ms);

  protected:
    void hideEvent(QHideEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    // Learn buttons and the status line follow the service's learn state
    // (a learn started elsewhere shows here too).
    void syncLearn();
    void learnClicked(int lane, bool checked);
    void rebuildPorts();
    // Shows the controls or the no-backend message, whichever applies.
    void syncBackend();
    void portItemChanged(QListWidgetItem *item);
    void loadPreferences();

    MidiInput &m_midi;
    bool m_showWithoutBackend = false;
    QWidget *m_controls = nullptr;
    QWidget *m_unavailable = nullptr;
    QLabel *m_unavailableLabel = nullptr;
    QListWidget *m_ports = nullptr;
    QLabel *m_portsStatus = nullptr;
    QCheckBox *m_livePlay = nullptr;
    QSpinBox *m_fixedVelocity = nullptr;
    QSpinBox *m_latency = nullptr;
    // The Controls table, one row per MidiRoutes::kLanes entry.
    std::array<QSpinBox *, MidiRoutes::kLaneCount> m_routeCc{};
    std::array<QPushButton *, MidiRoutes::kLaneCount> m_learn{};
    QLabel *m_learnStatus = nullptr;
    // This page started the learn in progress: hiding the page cancels it,
    // and Esc anywhere in its window does.
    bool m_startedLearn = false;
    // Set while the list is rebuilt, so restoring check states never
    // reads as the user toggling a port.
    bool m_loading = false;
};
