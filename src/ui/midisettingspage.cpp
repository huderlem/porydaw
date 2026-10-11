#include "midisettingspage.h"

#include "audio/midiinput.h"
#include "layout.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {

constexpr int kPortNameRole = Qt::UserRole;

} // namespace

MidiSettingsPage::MidiSettingsPage(MidiInput &midi, QWidget *parent, bool showWithoutBackend)
    : QWidget(parent)
    , m_midi(midi)
{
    auto *pageLayout = new QVBoxLayout(this);
    if (!midi.backendAvailable() && !showWithoutBackend) {
        auto *label = new QLabel(midi.backendStatus(), this);
        label->setObjectName(QStringLiteral("settingsMidiUnavailable"));
        label->setWordWrap(true);
        label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        pageLayout->addWidget(label);
        pageLayout->addStretch(1);
        return;
    }

    // Input ports: every port plays unless unchecked, and a keyboard
    // plugged in later joins on its own.
    auto *portsGroup = new QGroupBox(tr("Input ports"), this);
    m_ports = new QListWidget(this);
    m_ports->setObjectName(QStringLiteral("settingsMidiPorts"));
    m_ports->setSelectionMode(QAbstractItemView::NoSelection);
    m_ports->setToolTip(tr("Notes from every checked port play on the selected track, on any "
                           "MIDI channel. New ports are checked when they appear."));
    m_portsStatus = new QLabel(this);
    m_portsStatus->setObjectName(QStringLiteral("settingsMidiPortsStatus"));
    m_portsStatus->setForegroundRole(QPalette::PlaceholderText);
    m_portsStatus->setWordWrap(true);
    auto *refresh = new QPushButton(tr("Refresh"), this);
    refresh->setObjectName(QStringLiteral("settingsMidiRefresh"));
    refresh->setToolTip(tr("List the MIDI ports again (porydaw also checks every few seconds) "
                           "and retry any that could not be opened."));
    // Not the window's default button: Enter in a spinbox commits it.
    refresh->setAutoDefault(false);
    auto *statusRow = new QHBoxLayout;
    statusRow->setContentsMargins(0, 0, 0, 0);
    statusRow->addWidget(m_portsStatus, 1);
    statusRow->addWidget(refresh);
    auto *portsLayout = new QVBoxLayout(portsGroup);
    portsLayout->addWidget(m_ports, 1);
    portsLayout->addLayout(statusRow);

    // Playing and recording.
    auto *playGroup = new QGroupBox(tr("Playing and recording"), this);
    auto *playForm = new QFormLayout(playGroup);
    playForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    m_livePlay = new QCheckBox(tr("Play incoming notes"), this);
    m_livePlay->setObjectName(QStringLiteral("settingsMidiLivePlay"));
    m_livePlay->setToolTip(tr("Sound notes from the keyboard on the selected track, with its "
                              "voice, volume and pan. Off, the keyboard is silent but can "
                              "still record."));
    playForm->addRow(QString(), m_livePlay);

    m_fixedVelocity = new QSpinBox(this);
    m_fixedVelocity->setObjectName(QStringLiteral("settingsMidiFixedVelocity"));
    m_fixedVelocity->setRange(0, 127);
    m_fixedVelocity->setSpecialValueText(tr("Off"));
    m_fixedVelocity->setKeyboardTracking(false);
    m_fixedVelocity->setToolTip(tr("Play and record every note at this velocity instead of "
                                   "how hard the key was struck. Off uses the keyboard's own "
                                   "velocity."));
    playForm->addRow(tr("Fixed &velocity:"), m_fixedVelocity);

    m_latency = new QSpinBox(this);
    m_latency->setObjectName(QStringLiteral("settingsMidiLatency"));
    m_latency->setRange(MidiInput::kLatencyAuto, MidiInput::kLatencyMaxMs);
    m_latency->setSpecialValueText(tr("Auto"));
    m_latency->setSuffix(tr(" ms"));
    m_latency->setKeyboardTracking(false);
    m_latency->setToolTip(tr("How much earlier recorded notes are placed, to make up for the "
                             "delay between the song playing and you hearing it. Auto uses "
                             "the audio device's own latency."));
    playForm->addRow(tr("Recording &offset:"), m_latency);

    pageLayout->addWidget(portsGroup, 1);
    pageLayout->addWidget(playGroup);

    loadPreferences();
    rebuildPorts();

    connect(m_ports, &QListWidget::itemChanged, this, &MidiSettingsPage::portItemChanged);
    connect(refresh, &QPushButton::clicked, &m_midi, &MidiInput::refresh);
    // Queued: a checkbox toggle reports portsChanged from inside the list's
    // own itemChanged, and the rebuild deletes that item.
    connect(&m_midi, &MidiInput::portsChanged, this, &MidiSettingsPage::rebuildPorts,
            Qt::QueuedConnection);
    connect(&m_midi, &MidiInput::settingsChanged, this, &MidiSettingsPage::loadPreferences);
    connect(m_livePlay, &QCheckBox::toggled, &m_midi, &MidiInput::setLivePlay);
    connect(m_fixedVelocity, &QSpinBox::valueChanged, &m_midi, &MidiInput::setFixedVelocity);
    connect(m_latency, &QSpinBox::valueChanged, &m_midi, &MidiInput::setLatencyMs);
}

// Controls follow the service (startup values, or a change made elsewhere);
// blocked so a reload never echoes back as an edit.
void MidiSettingsPage::loadPreferences()
{
    const QSignalBlocker live(m_livePlay);
    const QSignalBlocker velocity(m_fixedVelocity);
    const QSignalBlocker latency(m_latency);
    m_livePlay->setChecked(m_midi.livePlay());
    m_fixedVelocity->setValue(m_midi.fixedVelocity());
    m_latency->setValue(m_midi.latencyMs());
}

void MidiSettingsPage::rebuildPorts()
{
    m_loading = true;
    m_ports->clear();
    const QStringList ports = m_midi.ports();
    for (const QString &name : ports) {
        auto *item = new QListWidgetItem(m_ports);
        item->setData(kPortNameRole, name);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        const bool enabled = m_midi.isPortEnabled(name);
        item->setCheckState(enabled ? Qt::Checked : Qt::Unchecked);
        if (enabled && m_midi.portFailed(name)) {
            item->setText(tr("%1 (unavailable)").arg(name));
            item->setToolTip(tr("This port could not be opened. Another program may be using "
                                "it; close that program, then press Refresh."));
        } else {
            item->setText(name);
        }
    }
    m_loading = false;

    if (!m_midi.backendAvailable())
        m_portsStatus->setText(m_midi.backendStatus());
    else if (ports.isEmpty())
        m_portsStatus->setText(tr("No MIDI input ports found. Connect a keyboard; it appears "
                                  "here on its own."));
    else
        m_portsStatus->setText(
            tr("Listening to %n port(s) through %1.", nullptr, int(m_midi.openPorts().size()))
                .arg(m_midi.backendName()));
}

void MidiSettingsPage::portItemChanged(QListWidgetItem *item)
{
    if (m_loading || !item)
        return;
    const QString name = item->data(kPortNameRole).toString();
    const bool on = item->checkState() == Qt::Checked;
    m_midi.setPortEnabled(name, on);
}
