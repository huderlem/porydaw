#include "midisettingspage.h"

#include "audio/midiinput.h"
#include "layout.h"

#include <QApplication>
#include <QCheckBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <utility>

namespace {

constexpr int kPortNameRole = Qt::UserRole;

// A CC number box that never offers a reserved CC (MidiRoutes::assignable):
// the arrows step over them and typing one doesn't commit. -1 is None.
// CC120–127 are reserved too, so they're simply past the maximum.
//
// Stepping (arrows, PageUp/PageDown, the wheel) also steps over the CCs
// other lanes hold (heldElsewhere), so passing through a value never moves
// another lane's route; only a typed CC, once committed, does. The wheel
// steps only while the box has focus, so scrolling past it changes nothing.
class CcSpinBox : public QSpinBox
{
  public:
    CcSpinBox(std::function<bool(int)> heldElsewhere, QWidget *parent)
        : QSpinBox(parent)
        , m_heldElsewhere(std::move(heldElsewhere))
    {
        setRange(MidiRoutes::kNone, 119);
        setSpecialValueText(QCoreApplication::translate("MidiSettingsPage", "None"));
        setPrefix(QStringLiteral("CC "));
        setKeyboardTracking(false);
        setFocusPolicy(Qt::StrongFocus);
    }

    void stepBy(int steps) override
    {
        // A typed number not yet committed is where the step starts, as in
        // QAbstractSpinBox::stepBy, but it isn't committed on its own: typing
        // another lane's CC and pressing Up doesn't move that lane, and with
        // nowhere to step the entry stays pending for Enter. An unfinished
        // entry (a reserved CC) doesn't step, as in Qt.
        int from = value();
        if (!startValue(&from)) {
            setValue(value()); // shows the route that stands
            return;
        }
        const int to = stepped(from, steps);
        if (to != from || from == value())
            setValue(to);
    }

    StepEnabled stepEnabled() const override
    {
        int from = value();
        if (isReadOnly() || !startValue(&from))
            return isReadOnly() ? StepNone : StepUpEnabled | StepDownEnabled;
        StepEnabled enabled = StepNone;
        if (stepped(from, 1) != from)
            enabled |= StepUpEnabled;
        if (stepped(from, -1) != from)
            enabled |= StepDownEnabled;
        return enabled;
    }

  protected:
    void wheelEvent(QWheelEvent *event) override
    {
        if (!hasFocus()) {
            event->ignore(); // the page (or a scroll area) gets it
            return;
        }
        QSpinBox::wheelEvent(event);
    }

    QValidator::State validate(QString &text, int &pos) const override
    {
        const QValidator::State state = QSpinBox::validate(text, pos);
        if (state != QValidator::Acceptable)
            return state;
        const int v = valueFromText(text);
        // Intermediate, not Invalid: "6" may still become "65".
        return v == MidiRoutes::kNone || MidiRoutes::assignable(v) ? state
                                                                   : QValidator::Intermediate;
    }

  private:
    bool offerable(int cc) const
    {
        return cc == MidiRoutes::kNone || (MidiRoutes::assignable(cc) && !m_heldElsewhere(cc));
    }

    // The value the box shows, typed or not; false for an unfinished entry.
    bool startValue(int *v) const
    {
        QString text = lineEdit()->displayText();
        if (text ==
            specialValueText()) { // "None" (Qt's line-edit validator checks it, not validate)
            *v = minimum();
            return true;
        }
        int pos = lineEdit()->cursorPosition();
        if (validate(text, pos) != QValidator::Acceptable)
            return false;
        *v = valueFromText(text);
        return true;
    }

    // from, moved |steps| offerable values up or down (as far as there are).
    int stepped(int from, int steps) const
    {
        const int dir = steps < 0 ? -1 : 1;
        int v = from;
        for (int n = std::abs(steps); n > 0; n--) {
            int next = v + dir;
            while (next >= minimum() && next <= maximum() && !offerable(next))
                next += dir;
            if (next < minimum() || next > maximum())
                break;
            v = next;
        }
        return v;
    }

    std::function<bool(int)> m_heldElsewhere;
};

QString laneDescription(int lane)
{
    switch (lane) {
    case 0x01:
        return MidiSettingsPage::tr("Modulation depth. The control's full travel covers the MOD "
                                    "lane's range as drawn.");
    case 0x07:
        return MidiSettingsPage::tr("Track volume.");
    case 0x0A:
        return MidiSettingsPage::tr("Track pan.");
    case 0x14:
        return MidiSettingsPage::tr("Pitch-bend range.");
    case 0x15:
        return MidiSettingsPage::tr("LFO speed.");
    }
    return QString();
}

} // namespace

MidiSettingsPage::MidiSettingsPage(MidiInput &midi, QWidget *parent, bool showWithoutBackend)
    : QWidget(parent)
    , m_midi(midi)
    , m_showWithoutBackend(showWithoutBackend)
{
    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);

    // No backend: why, and a way to try again (a MIDI system that starts
    // later, a driver installed meanwhile).
    m_unavailable = new QWidget(this);
    auto *unavailableLayout = new QVBoxLayout(m_unavailable);
    m_unavailableLabel = new QLabel(m_unavailable);
    m_unavailableLabel->setObjectName(QStringLiteral("settingsMidiUnavailable"));
    m_unavailableLabel->setWordWrap(true);
    m_unavailableLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    auto *retry = new QPushButton(tr("Try Again"), m_unavailable);
    retry->setObjectName(QStringLiteral("settingsMidiRetry"));
    retry->setToolTip(tr("Look for the computer's MIDI system again."));
    retry->setAutoDefault(false);
    auto *retryRow = new QHBoxLayout;
    retryRow->setContentsMargins(0, 0, 0, 0);
    retryRow->addWidget(retry);
    retryRow->addStretch(1);
    unavailableLayout->addWidget(m_unavailableLabel);
    unavailableLayout->addLayout(retryRow);
    unavailableLayout->addStretch(1);
    outerLayout->addWidget(m_unavailable, 1);

    // The settings scroll when the window is too short for them, rather
    // than squeezing.
    m_controlsScroll = new QScrollArea(this);
    m_controlsScroll->setFrameShape(QFrame::NoFrame);
    m_controlsScroll->setWidgetResizable(true);
    m_controlsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_controls = new QWidget;
    auto *pageLayout = new QVBoxLayout(m_controls);
    outerLayout->addWidget(m_controlsScroll, 1);

    // Input ports: every port plays unless unchecked, and a keyboard
    // plugged in later joins on its own.
    auto *portsGroup = new QGroupBox(tr("Input ports"), m_controls);
    m_ports = new QListWidget(m_controls);
    m_ports->setObjectName(QStringLiteral("settingsMidiPorts"));
    m_ports->setSelectionMode(QAbstractItemView::NoSelection);
    m_ports->setToolTip(tr("Notes from every checked port play on the selected track, on any "
                           "MIDI channel. New ports are checked when they appear."));
    m_portsStatus = new QLabel(m_controls);
    m_portsStatus->setObjectName(QStringLiteral("settingsMidiPortsStatus"));
    m_portsStatus->setForegroundRole(QPalette::PlaceholderText);
    m_portsStatus->setWordWrap(true);
    auto *refresh = new QPushButton(tr("Refresh"), m_controls);
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
    portsLayout->addWidget(m_ports);
    portsLayout->addLayout(statusRow);

    // Playing and recording.
    auto *playGroup = new QGroupBox(tr("Playing and recording"), m_controls);
    auto *playForm = new QFormLayout(playGroup);
    playForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    m_livePlay = new QCheckBox(tr("Play incoming notes"), m_controls);
    m_livePlay->setObjectName(QStringLiteral("settingsMidiLivePlay"));
    m_livePlay->setToolTip(tr("Sound notes from the keyboard on the selected track, with its "
                              "voice, volume and pan. Off, the keyboard is silent but can "
                              "still record."));
    playForm->addRow(QString(), m_livePlay);

    m_fixedVelocity = new QSpinBox(m_controls);
    m_fixedVelocity->setObjectName(QStringLiteral("settingsMidiFixedVelocity"));
    m_fixedVelocity->setRange(0, 127);
    m_fixedVelocity->setSpecialValueText(tr("Off"));
    m_fixedVelocity->setKeyboardTracking(false);
    m_fixedVelocity->setToolTip(tr("Play and record every note at this velocity instead of "
                                   "how hard the key was struck. Off uses the keyboard's own "
                                   "velocity."));
    playForm->addRow(tr("Fixed &velocity:"), m_fixedVelocity);

    m_latency = new QSpinBox(m_controls);
    m_latency->setObjectName(QStringLiteral("settingsMidiLatency"));
    m_latency->setRange(MidiInput::kLatencyAuto, MidiInput::kLatencyMaxMs);
    m_latency->setSpecialValueText(tr("Auto"));
    m_latency->setSuffix(tr(" ms"));
    m_latency->setKeyboardTracking(false);
    m_latency->setToolTip(tr("How much earlier recorded notes are placed, to make up for the "
                             "delay between the song playing and you hearing it. Auto uses "
                             "the audio device's own latency."));
    playForm->addRow(tr("Recording &offset:"), m_latency);

    // Controls: which knob, slider or button drives each lane.
    auto *controlsGroup = new QGroupBox(tr("Controls"), m_controls);
    auto *controlsLayout = new QVBoxLayout(controlsGroup);
    auto *grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setColumnStretch(3, 1);
    for (int i = 0; i < MidiRoutes::kLaneCount; i++) {
        const int lane = MidiRoutes::kLanes[i];
        const QString name = MidiRoutes::laneName(lane);
        auto *label = new QLabel(name, m_controls);
        label->setToolTip(laneDescription(lane));
        auto *cc = new CcSpinBox(
            [this, lane](int value) {
                const int holder = m_midi.routes().laneFor(value);
                return holder != MidiRoutes::kNone && holder != lane;
            },
            m_controls);
        cc->setObjectName(QStringLiteral("settingsMidiRoute%1").arg(name));
        cc->setToolTip(tr("The MIDI control (CC number, any channel) that drives %1: %2 "
                          "None leaves it to the song.")
                           .arg(name, laneDescription(lane)));
        label->setBuddy(cc);
        auto *learn = new QPushButton(tr("Learn"), m_controls);
        learn->setObjectName(QStringLiteral("settingsMidiLearn%1").arg(name));
        learn->setCheckable(true);
        learn->setAutoDefault(false);
        learn->setToolTip(tr("Press, then move the knob, slider or button on your keyboard "
                             "that should drive %1. Press again or Esc to cancel.")
                              .arg(name));
        grid->addWidget(label, i, 0, Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(cc, i, 1);
        grid->addWidget(learn, i, 2);
        m_routeCc[i] = cc;
        m_learn[i] = learn;
        connect(cc, &QSpinBox::valueChanged, this, [this, lane](int value) {
            // Steps never land on another lane's CC, so only a committed
            // typed one (Enter, or leaving the box) moves a route; it says
            // so, as Learn does.
            int movedFrom = MidiRoutes::kNone;
            if (!m_midi.setRoute(lane, value, &movedFrom)) {
                // Refused (only a reserved CC set from code): show the route
                // that stands.
                loadPreferences();
            } else if (movedFrom != MidiRoutes::kNone) {
                reportRoute(lane, value, movedFrom);
            } else if (m_midi.learnLane() == MidiRoutes::kNone) {
                m_learnStatus->clear(); // an earlier report no longer applies
            }
        });
        connect(learn, &QPushButton::clicked, this,
                [this, lane](bool checked) { learnClicked(lane, checked); });
    }
    m_learnStatus = new QLabel(m_controls);
    m_learnStatus->setObjectName(QStringLiteral("settingsMidiLearnStatus"));
    m_learnStatus->setForegroundRole(QPalette::PlaceholderText);
    m_learnStatus->setWordWrap(true);
    m_learnStatus->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    controlsLayout->addLayout(grid);
    controlsLayout->addWidget(m_learnStatus);

    pageLayout->addWidget(portsGroup);
    pageLayout->addWidget(playGroup);
    pageLayout->addWidget(controlsGroup);
    pageLayout->addStretch(1);
    m_controlsScroll->setWidget(m_controls);
    fitToFont();

    loadPreferences();
    rebuildPorts();
    syncBackend();

    connect(m_ports, &QListWidget::itemChanged, this, &MidiSettingsPage::portItemChanged);
    connect(refresh, &QPushButton::clicked, &m_midi, &MidiInput::refresh);
    connect(retry, &QPushButton::clicked, this, [this] {
        m_midi.refresh();
        syncBackend(); // the reason may read differently even if it failed again
    });
    // Queued: a checkbox toggle reports portsChanged from inside the list's
    // own itemChanged, and the rebuild deletes that item.
    connect(&m_midi, &MidiInput::portsChanged, this, &MidiSettingsPage::rebuildPorts,
            Qt::QueuedConnection);
    connect(&m_midi, &MidiInput::portsChanged, this, &MidiSettingsPage::syncBackend,
            Qt::QueuedConnection);
    connect(&m_midi, &MidiInput::settingsChanged, this, &MidiSettingsPage::loadPreferences);
    connect(m_livePlay, &QCheckBox::toggled, &m_midi, &MidiInput::setLivePlay);
    connect(m_fixedVelocity, &QSpinBox::valueChanged, &m_midi, &MidiInput::setFixedVelocity);
    connect(m_latency, &QSpinBox::valueChanged, &m_midi, &MidiInput::setLatencyMs);
    // A learn started elsewhere (a lane's menu) shows on the buttons too.
    connect(&m_midi, &MidiInput::learnStarted, this, &MidiSettingsPage::syncLearn);
    connect(&m_midi, &MidiInput::learnFinished, this, [this](int lane, int cc, int movedFrom) {
        m_startedLearn = false;
        syncLearn();
        reportRoute(lane, cc, movedFrom);
    });
    connect(&m_midi, &MidiInput::learnCancelled, this, [this](int, bool timedOut) {
        m_startedLearn = false;
        syncLearn();
        m_learnStatus->setText(timedOut ? tr("Nothing was moved on the keyboard, so nothing "
                                             "changed.")
                                        : QString());
    });
    syncLearn();
}

void MidiSettingsPage::reportRoute(int lane, int cc, int movedFrom)
{
    const QString name = MidiRoutes::laneName(lane);
    m_learnStatus->setText(movedFrom == MidiRoutes::kNone
                               ? tr("CC %1 now drives %2.").arg(cc).arg(name)
                               : tr("CC %1 now drives %2 instead of %3.")
                                     .arg(cc)
                                     .arg(name, MidiRoutes::laneName(movedFrom)));
}

void MidiSettingsPage::learnClicked(int lane, bool checked)
{
    if (checked) {
        m_midi.startLearn(lane);
        m_startedLearn = m_midi.learnLane() == lane;
        if (m_startedLearn)
            qApp->installEventFilter(this); // Esc cancels, wherever the focus is
    } else if (m_midi.learnLane() == lane) {
        m_midi.cancelLearn(); // the second click
    }
    syncLearn();
}

void MidiSettingsPage::syncLearn()
{
    const int learning = m_midi.learnLane();
    for (int i = 0; i < MidiRoutes::kLaneCount; i++) {
        const bool on = MidiRoutes::kLanes[i] == learning;
        const QSignalBlocker block(m_learn[i]);
        m_learn[i]->setChecked(on);
        m_learn[i]->setText(on ? tr("Listening…") : tr("Learn"));
    }
    if (learning != MidiRoutes::kNone) {
        // Esc only cancels a learn this page started (see eventFilter).
        m_learnStatus->setText(
            (m_startedLearn ? tr("Move the knob, slider or button that should drive %1. Esc "
                                 "cancels.")
                            : tr("Move the knob, slider or button that should drive %1."))
                .arg(MidiRoutes::laneName(learning)));
    } else {
        qApp->removeEventFilter(this);
    }
}

bool MidiSettingsPage::eventFilter(QObject *watched, QEvent *event)
{
    // Only while this page's learn runs (the filter is removed after).
    if ((event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride) &&
        m_startedLearn && m_midi.learnLane() != MidiRoutes::kNone) {
        auto *key = static_cast<QKeyEvent *>(event);
        auto *widget = qobject_cast<QWidget *>(watched);
        if (key->key() == Qt::Key_Escape && widget && widget->window() == window()) {
            // Claimed at the override, so no Esc shortcut takes it, and
            // consumed at the press, so the dialog stays open.
            event->accept();
            if (event->type() == QEvent::KeyPress)
                m_midi.cancelLearn();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void MidiSettingsPage::hideEvent(QHideEvent *event)
{
    // Closing the Settings window mid-learn: the knob shouldn't be learned
    // unseen later. A spontaneous hide (the window minimized) isn't closing.
    if (!event->spontaneous() && m_startedLearn && m_midi.learnLane() != MidiRoutes::kNone)
        m_midi.cancelLearn();
    QWidget::hideEvent(event);
}

void MidiSettingsPage::syncBackend()
{
    const bool controls = m_midi.backendAvailable() || m_showWithoutBackend;
    m_unavailableLabel->setText(m_midi.backendStatus());
    m_unavailable->setVisible(!controls);
    m_controlsScroll->setVisible(controls);
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
    for (int i = 0; i < MidiRoutes::kLaneCount; i++) {
        if (!m_routeCc[i])
            continue; // still building
        const QSignalBlocker block(m_routeCc[i]);
        m_routeCc[i]->setValue(m_midi.routes().ccFor(MidiRoutes::kLanes[i]));
    }
}

void MidiSettingsPage::fitToFont()
{
    // Nothing moves when a learn starts: the status line keeps room for two
    // lines of text, and a Learn button is as wide as "Listening…".
    m_learnStatus->setMinimumHeight(2 * m_learnStatus->fontMetrics().lineSpacing());
    for (QPushButton *learn : m_learn) {
        const QString text = learn->text();
        learn->setMinimumWidth(0);
        int widest = 0;
        for (const QString &t : {tr("Learn"), tr("Listening…")}) {
            learn->setText(t);
            widest = std::max(widest, learn->sizeHint().width());
        }
        learn->setText(text);
        learn->setMinimumWidth(widest);
    }
    fitPortList();
    // It scrolls only vertically, so it's never narrower than the settings.
    m_controlsScroll->setMinimumWidth(
        m_controls->minimumSizeHint().width() +
        m_controlsScroll->style()->pixelMetric(QStyle::PM_ScrollBarExtent));
}

void MidiSettingsPage::fitPortList()
{
    // As tall as its ports, between kMinPortRows and kMaxPortRows rows (it
    // scrolls past that), not the rest of the page.
    const int rows = std::clamp(m_ports->count(), kMinPortRows, kMaxPortRows);
    const int rowH = m_ports->count() > 0
                         ? m_ports->sizeHintForRow(0)
                         : m_ports->fontMetrics().height() + ::layout::space(::layout::Space::One);
    m_ports->setFixedHeight(rows * rowH + 2 * m_ports->frameWidth());
}

void MidiSettingsPage::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    // (m_learnStatus: not during construction.)
    if ((event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) &&
        m_learnStatus)
        fitToFont();
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
    fitPortList();

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

void MidiSettingsPage::setAutoLatencyMs(int ms)
{
    m_latency->setSpecialValueText(ms >= 0 ? tr("Auto (%1 ms)").arg(ms) : tr("Auto"));
}
