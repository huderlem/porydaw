#pragma once

#include <QWidget>

class MidiInput;
class QCheckBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QSpinBox;

// Settings window, MIDI page (docs/midi-input/PLAN.md step 1): which input
// ports porydaw listens to, and how incoming notes play and record. Like
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

  private:
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
    // Set while the list is rebuilt, so restoring check states never
    // reads as the user toggling a port.
    bool m_loading = false;
};
