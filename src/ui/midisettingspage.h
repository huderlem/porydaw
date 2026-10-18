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
class QScrollArea;
class QSpinBox;
class QStyledItemDelegate;

// Settings window, MIDI page (docs/midi-input/PLAN.md step 1): which input
// ports porydaw listens to and what each one is (a keyboard, or a control
// surface: docs/control-surface/PLAN.md), how incoming notes play and
// record, and which keyboard control drives which lane (the Controls table,
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
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    // Learn buttons and the status line follow the service's learn state
    // (a learn started elsewhere shows here too).
    void syncLearn();
    void learnClicked(int lane, bool checked);
    // The status line's "CC 74 now drives VOL (instead of PAN)."
    void reportRoute(int lane, int cc, int movedFrom);
    void rebuildPorts();
    // Sizes that follow the font: the learn status's two lines, the Learn
    // buttons' width, the port list's rows.
    void fitToFont();
    void fitPortList();
    // The scroll area's minimum width: the settings' own plus a scrollbar.
    void fitWidth();
    static constexpr int kMinPortRows = 2;
    static constexpr int kMaxPortRows = 5;
    // Shows the controls or the no-backend message, whichever applies, and
    // the Virtual Keyboard's note while it is open.
    void syncBackend();
    void portItemChanged(QListWidgetItem *item);
    void loadPreferences();

    MidiInput &m_midi;
    bool m_showWithoutBackend = false;
    QScrollArea *m_controlsScroll = nullptr;
    QWidget *m_controls = nullptr;
    QWidget *m_unavailable = nullptr;
    QLabel *m_unavailableLabel = nullptr;
    // "The Virtual Keyboard is open..." while it is (docs/virtual-keyboard/
    // PLAN.md D8): above both views, since it plays without a backend too.
    QLabel *m_virtualNote = nullptr;
    QListWidget *m_ports = nullptr;
    // The port rows' delegate (a PortDelegate: role boxes, elided names).
    QStyledItemDelegate *m_portDelegate = nullptr;
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
