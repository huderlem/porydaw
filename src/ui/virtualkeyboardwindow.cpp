#include "virtualkeyboardwindow.h"

#include "audio/midiinput.h"
#include "layout.h"
#include "theme/themeruntime.h"
#include "ui/m4asemantics.h"

#include <QCloseEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QSettings>
#include <QShowEvent>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace {

const QString kBaseKey = QStringLiteral("virtualKeyboard/base");
const QString kVelocityKey = QStringLiteral("virtualKeyboard/velocity");
const QString kGeometryKey = QStringLiteral("virtualKeyboard/geometry");

// The typing letters of D2 by offset from the base C: the bottom row plays
// the lower octave and its C, the top row the upper octave and on. Comma
// and Q both play the upper C, so that key shows both.
const char *const kLetters[VirtualPianoWidget::kSpan] = {
    "Z", "S", "X", "D", "C", "V", "G", "B", "H", "N", "J", "M", ", Q", "2",
    "W", "3", "E", "R", "5", "T", "6", "Y", "7", "U", "I", "9", "O",   "0",
};

// Each white key's index from the left, by offset (-1 for a black key).
constexpr int kWhiteIndex[VirtualPianoWidget::kSpan] = {
    0, -1, 1, -1, 2,  3,  -1, 4,  -1, 5,  -1, 6,  7,  -1,
    8, -1, 9, 10, -1, 11, -1, 12, -1, 13, 14, -1, 15, -1,
};

// A black key is this much of a white key's width, and this much of the
// piano's height.
constexpr double kBlackWidth = 0.6;
constexpr double kBlackHeight = 0.62;

// A base rounded down to its C (also below zero) and clamped to C-1..C8.
int roundBase(int base)
{
    return std::clamp(base - ((base % 12) + 12) % 12, VirtualKeyboardWindow::kMinBase,
                      VirtualKeyboardWindow::kMaxBase);
}

} // namespace

VirtualPianoWidget::VirtualPianoWidget(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("virtualPiano"));
    // D12: no hover tracking; a move arrives only while a button is down.
    setMouseTracking(false);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    relabel();
    layoutKeys();
}

bool VirtualPianoWidget::isBlack(int offset)
{
    return kWhiteIndex[offset] < 0;
}

void VirtualPianoWidget::setBase(int base)
{
    if (base == m_base)
        return;
    // The key under the mouse is about to stand for another note: let the
    // old one go first (PLAN §2 stance 4).
    releaseMouseKey();
    m_base = base;
    relabel();
    update(); // every label and the disabled keys move
}

bool VirtualPianoWidget::playable(int key) const
{
    return key >= m_base && key < m_base + kSpan && key <= 127;
}

QRect VirtualPianoWidget::keyRect(int key) const
{
    const int offset = key - m_base;
    if (offset < 0 || offset >= kSpan)
        return QRect();
    return m_rects[size_t(offset)];
}

int VirtualPianoWidget::keyAt(const QPoint &pos) const
{
    // Black keys sit over the whites, so they are hit first.
    for (const bool black : {true, false}) {
        for (int offset = 0; offset < kSpan; offset++) {
            if (isBlack(offset) == black && m_rects[size_t(offset)].contains(pos))
                return playable(m_base + offset) ? m_base + offset : -1;
        }
    }
    return -1;
}

void VirtualPianoWidget::releaseMouseKey()
{
    if (m_mouseKey < 0)
        return;
    const int key = m_mouseKey;
    m_mouseKey = -1;
    emit mouseNoteOff(key);
}

void VirtualPianoWidget::forgetMouseKey(int key)
{
    if (key == m_mouseKey)
        m_mouseKey = -1;
}

bool VirtualPianoWidget::isLit(int key) const
{
    return key >= 0 && key < 128 && (m_virtualHeld[size_t(key)] || m_hardwareDown[size_t(key)]);
}

bool VirtualPianoWidget::isVirtualHeld(int key) const
{
    return key >= 0 && key < 128 && m_virtualHeld[size_t(key)];
}

bool VirtualPianoWidget::isHardwareDown(int key) const
{
    return key >= 0 && key < 128 && m_hardwareDown[size_t(key)];
}

void VirtualPianoWidget::setVirtualHeld(int key, bool on)
{
    if (key < 0 || key >= 128)
        return;
    const bool was = isLit(key);
    m_virtualHeld[size_t(key)] = on;
    repaintIfChanged(key, was);
}

void VirtualPianoWidget::setHardwareDown(int key, bool on)
{
    if (key < 0 || key >= 128)
        return;
    const bool was = isLit(key);
    m_hardwareDown[size_t(key)] = on;
    repaintIfChanged(key, was);
}

void VirtualPianoWidget::clearVirtualHeld()
{
    for (int key = 0; key < 128; key++) {
        if (m_virtualHeld[size_t(key)])
            setVirtualHeld(key, false);
    }
}

void VirtualPianoWidget::clearHardwareDown()
{
    for (int key = 0; key < 128; key++) {
        if (m_hardwareDown[size_t(key)])
            setHardwareDown(key, false);
    }
}

void VirtualPianoWidget::repaintIfChanged(int key, bool wasLit)
{
    // D12: only the key that changed. A white key's rect takes the black
    // keys over it along (paintEvent draws whatever meets the dirty rect),
    // and a black key's the white strips under it.
    if (isLit(key) != wasLit) {
        const QRect r = keyRect(key);
        if (!r.isEmpty())
            update(r);
    }
}

void VirtualPianoWidget::relabel()
{
    for (int offset = 0; offset < kSpan; offset++) {
        m_letters[size_t(offset)] = QString::fromLatin1(kLetters[offset]);
        m_octaves[size_t(offset)] =
            offset % 12 == 0 && m_base + offset <= 127 ? midiKeyName(m_base + offset) : QString();
    }
}

void VirtualPianoWidget::layoutKeys()
{
    m_labelFont = font();
    m_labelHeight = QFontMetrics(m_labelFont).height();
    // The whites share the width less half a black key, so the top D#
    // (right of the last white) is drawn whole.
    const double whiteW = width() / (kWhiteKeys + kBlackWidth / 2);
    const int blackW = std::max(3, int(std::lround(whiteW * kBlackWidth)));
    const int blackH = int(std::lround(height() * kBlackHeight));
    const auto whiteX = [whiteW](int index) { return int(std::lround(index * whiteW)); };
    for (int offset = 0; offset < kSpan; offset++) {
        const int index = kWhiteIndex[offset];
        if (index >= 0) {
            m_rects[size_t(offset)] =
                QRect(whiteX(index), 0, whiteX(index + 1) - whiteX(index), height());
        } else {
            // Centred on the line between its two whites.
            const int line = whiteX(kWhiteIndex[offset - 1] + 1);
            m_rects[size_t(offset)] = QRect(line - blackW / 2, 0, blackW, blackH);
        }
    }
}

QRect VirtualPianoWidget::labelRect(int key, bool octave) const
{
    const QRect r = keyRect(key);
    if (r.isEmpty())
        return QRect();
    QRect text(r.left(), r.bottom() - layout::space(layout::Space::Half) - m_labelHeight, r.width(),
               m_labelHeight);
    if (octave)
        text.translate(0, -m_labelHeight);
    return text;
}

int VirtualPianoWidget::minimumPianoHeight() const
{
    // A C's octave sits over its letter, both low on the white key, and the
    // black key beside it covers the top kBlackHeight of the piano: the
    // two lines and the bottom padding must fit in the rest, or "C3" runs
    // under the C# (a white key is as tall as the piano).
    const int labels = 2 * m_labelHeight + layout::space(layout::Space::Half);
    // (+2: the black key's rounded height and the key's last pixel row.)
    const int fit = int(std::ceil((labels + 2) / (1.0 - kBlackHeight)));
    return std::max(layout::fontPx(5), fit);
}

QSize VirtualPianoWidget::sizeHint() const
{
    return QSize(kWhiteKeys * layout::fontPx(2.5),
                 std::max(layout::fontPx(8), minimumPianoHeight()));
}

QSize VirtualPianoWidget::minimumSizeHint() const
{
    return QSize(kWhiteKeys * layout::fontPx(1.6), minimumPianoHeight());
}

void VirtualPianoWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    layoutKeys();
}

void VirtualPianoWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange) {
        layoutKeys();
        updateGeometry(); // the minimum height follows the label font
        update();
    }
}

void VirtualPianoWidget::paintEvent(QPaintEvent *event)
{
    using themes::Role;
    QPainter p(this);
    const QRect dirty = event->rect();
    // The sliver right of the last white key, under half the top D#.
    p.fillRect(dirty, themes::color(Role::window_background));
    p.setFont(m_labelFont);
    const QColor &natural = themes::color(Role::song_view_piano_keyboard_natural_key);
    const QColor &black = themes::color(Role::song_view_piano_keyboard_black_key);
    const QColor &active = themes::color(Role::song_view_piano_keyboard_active_key);
    const QColor &separator = themes::color(Role::song_view_piano_keyboard_separator);
    const QColor &label = themes::color(Role::song_view_piano_keyboard_label);
    const QColor &disabledText = themes::color(Role::disabled_text);
    const int line = layout::singlePixel();
    for (const bool blackPass : {false, true}) {
        for (int offset = 0; offset < kSpan; offset++) {
            const QRect &r = m_rects[size_t(offset)];
            if (isBlack(offset) != blackPass || !r.intersects(dirty))
                continue;
            const int key = m_base + offset;
            const bool enabled = key <= 127;
            // D5: past G9 a key is drawn faded and plays nothing.
            QColor fill = isLit(key) ? active : blackPass ? black : natural;
            if (!enabled) {
                fill = blackPass ? separator : themes::color(Role::window_background);
            }
            p.fillRect(r, fill);
            if (!blackPass) {
                p.fillRect(QRect(r.right() - line + 1, 0, line, r.height()), separator);
                p.fillRect(QRect(r.left(), r.bottom() - line + 1, r.width(), line), separator);
                if (offset == 0)
                    p.fillRect(QRect(0, 0, line, r.height()), separator);
            }
            p.setPen(!enabled ? disabledText : blackPass && !isLit(key) ? natural : label);
            p.drawText(labelRect(key, false), Qt::AlignHCenter | Qt::AlignVCenter,
                       m_letters[size_t(offset)]);
            if (!m_octaves[size_t(offset)].isEmpty()) {
                p.drawText(labelRect(key, true), Qt::AlignHCenter | Qt::AlignVCenter,
                           m_octaves[size_t(offset)]);
            }
        }
    }
}

void VirtualPianoWidget::sound(int key)
{
    m_mouseKey = key;
    emit mouseNoteOn(key);
}

void VirtualPianoWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    // One mouse note at a time (D7): a press never stacks on a held one.
    releaseMouseKey();
    const int key = keyAt(event->position().toPoint());
    if (key >= 0)
        sound(key);
    event->accept();
}

void VirtualPianoWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!(event->buttons() & Qt::LeftButton)) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    // A glide (D7): another playable key under the pointer takes over.
    // Off the piano, a gap or a faded key keeps the last one sounding, so
    // a drag that overshoots the edge doesn't stutter; the release stops it
    // wherever it lands (the press grabbed the mouse).
    const int key = keyAt(event->position().toPoint());
    if (key >= 0 && key != m_mouseKey) {
        releaseMouseKey();
        sound(key);
    }
    event->accept();
}

void VirtualPianoWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    releaseMouseKey();
    event->accept();
}

VirtualKeyboardWindow::VirtualKeyboardWindow(MidiInput *midi, QWidget *parent)
    : QWidget(parent, Qt::Tool)
    , m_midi(midi)
{
    // D1: Qt::Tool owned by the main window — above Porydaw's windows,
    // never above other applications, hidden with the app.
    setObjectName(QStringLiteral("virtualKeyboardWindow"));
    setWindowTitle(tr("Virtual MIDI Keyboard"));

    m_piano = new VirtualPianoWidget(this);

    // The controls take no focus: the window keeps it, for typing (step 3).
    m_down = new QToolButton(this);
    m_down->setObjectName(QStringLiteral("virtualKeyboardOctaveDown"));
    m_down->setArrowType(Qt::LeftArrow);
    m_down->setFocusPolicy(Qt::NoFocus);
    m_down->setToolTip(tr("One octave down"));
    m_up = new QToolButton(this);
    m_up->setObjectName(QStringLiteral("virtualKeyboardOctaveUp"));
    m_up->setArrowType(Qt::RightArrow);
    m_up->setFocusPolicy(Qt::NoFocus);
    m_up->setToolTip(tr("One octave up"));
    m_octave = new QLabel(this);
    m_octave->setObjectName(QStringLiteral("virtualKeyboardOctave"));
    m_octave->setAlignment(Qt::AlignCenter);
    m_octave->setToolTip(tr("The lowest key's note"));
    // Wide enough for the widest name, so the arrows never move.
    m_octave->setMinimumWidth(m_octave->fontMetrics().horizontalAdvance(QStringLiteral("C-1")) +
                              layout::space(layout::Space::Two));

    auto *velocityLabel = new QLabel(tr("Velocity"), this);
    m_velocity = new QSlider(Qt::Horizontal, this);
    m_velocity->setObjectName(QStringLiteral("virtualKeyboardVelocity"));
    m_velocity->setRange(1, 127);
    m_velocity->setFocusPolicy(Qt::NoFocus);
    m_velocity->setToolTip(tr("How hard clicked and typed notes are struck. Settings → MIDI's "
                              "Fixed velocity overrides it."));
    m_velocity->setMinimumWidth(layout::fontPx(8));
    m_velocityValue = new QLabel(this);
    m_velocityValue->setObjectName(QStringLiteral("virtualKeyboardVelocityValue"));
    m_velocityValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_velocityValue->setMinimumWidth(
        m_velocityValue->fontMetrics().horizontalAdvance(QStringLiteral("127")));

    m_target = new QLabel(this);
    m_target->setObjectName(QStringLiteral("virtualKeyboardTarget"));
    m_target->setForegroundRole(QPalette::PlaceholderText);
    m_target->setToolTip(tr("The selected track, which the keyboard plays and records into"));
    m_target->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(0, 0, 0, 0);
    bar->addWidget(m_down);
    bar->addWidget(m_octave);
    bar->addWidget(m_up);
    bar->addSpacing(layout::space(layout::Space::Four));
    bar->addWidget(velocityLabel);
    bar->addWidget(m_velocity);
    bar->addWidget(m_velocityValue);
    bar->addSpacing(layout::space(layout::Space::Four));
    bar->addWidget(m_target, 1);
    auto *column = new QVBoxLayout(this);
    column->addLayout(bar);
    column->addWidget(m_piano, 1);

    QSettings settings;
    m_piano->setBase(roundBase(settings.value(kBaseKey, kDefaultBase).toInt()));
    m_velocity->setValue(
        std::clamp(settings.value(kVelocityKey, kDefaultVelocity).toInt(), 1, 127));
    m_velocityValue->setNum(m_velocity->value());
    m_savedVelocity = settings.contains(kVelocityKey) ? m_velocity->value() : -1;
    syncOctave();
    resize(sizeHint());
    restoreGeometry(settings.value(kGeometryKey).toByteArray());

    connect(m_down, &QToolButton::clicked, this, [this] { shiftOctave(-1); });
    connect(m_up, &QToolButton::clicked, this, [this] { shiftOctave(1); });
    // Saved when it settles: on the release of a drag (not at each step of
    // it), on any other change (a click on the groove, setVelocity), and
    // on hide.
    connect(m_velocity, &QSlider::valueChanged, this, [this](int value) {
        m_velocityValue->setNum(value);
        if (!m_velocity->isSliderDown())
            saveVelocity();
    });
    connect(m_velocity, &QSlider::sliderReleased, this, &VirtualKeyboardWindow::saveVelocity);
    connect(m_piano, &VirtualPianoWidget::mouseNoteOn, this,
            [this](int key) { send(0x90, key, m_velocity->value()); });
    connect(m_piano, &VirtualPianoWidget::mouseNoteOff, this,
            [this](int key) { send(0x80, key, 0); });
    if (m_midi) {
        connect(m_midi, &MidiInput::noteStateChanged, this, &VirtualKeyboardWindow::noteState);
        // D9 / step 1 judgment call 8: a closing port never sends its
        // note-offs, and the monitor has reported everything before this
        // and restarted from every key up.
        connect(m_midi, &MidiInput::portsClosed, m_piano, &VirtualPianoWidget::clearHardwareDown);
    }
}

VirtualKeyboardWindow::~VirtualKeyboardWindow()
{
    // ~QWidget hides a shown window without reaching hideEvent here.
    detach();
}

void VirtualKeyboardWindow::detach()
{
    if (m_open)
        saveGeometryNow();
    closePort();
    if (m_midi)
        disconnect(m_midi, nullptr, this, nullptr);
    if (m_midi)
        disconnect(m_midi, nullptr, m_piano, nullptr);
    m_midi = nullptr;
}

void VirtualKeyboardWindow::setBase(int base)
{
    base = roundBase(base);
    if (base == m_piano->base())
        return;
    m_piano->setBase(base); // releases the mouse key first
    syncOctave();
    QSettings().setValue(kBaseKey, base);
}

int VirtualKeyboardWindow::velocity() const
{
    return m_velocity->value();
}

void VirtualKeyboardWindow::setVelocity(int velocity)
{
    m_velocity->setValue(velocity);
}

void VirtualKeyboardWindow::setTarget(const QString &text)
{
    if (m_target->text() != text)
        m_target->setText(text);
    m_target->setToolTip(text);
}

QString VirtualKeyboardWindow::target() const
{
    return m_target->text();
}

void VirtualKeyboardWindow::syncOctave()
{
    const int base = m_piano->base();
    m_octave->setText(midiKeyName(base));
    m_down->setEnabled(base > kMinBase);
    m_up->setEnabled(base < kMaxBase);
}

void VirtualKeyboardWindow::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    openPort();
}

void VirtualKeyboardWindow::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    saveGeometryNow();
    saveVelocity();
    closePort();
}

void VirtualKeyboardWindow::closeEvent(QCloseEvent *event)
{
    QWidget::closeEvent(event);
    // Only the user's close (the title bar's button, a spontaneous event)
    // hides it for good. Quitting closes it too: Qt's last-window-closed
    // quit calls close() on a still-visible tool window after the main
    // window has gone, which must keep "shown" for the next launch.
    if (event->isAccepted() && event->spontaneous() && parentWidget() &&
        parentWidget()->isVisible())
        emit closeRequested();
}

void VirtualKeyboardWindow::openPort()
{
    if (m_open || m_closing || !m_midi)
        return;
    m_open = true;
    // Nothing the window sent is held yet (stale bits could only come from
    // a note sent while it was hidden, e.g. midicheck opening the port).
    m_piano->clearVirtualHeld();
    // The port first, so Record appears with the window; the monitor's
    // hardware keys light from here on.
    m_midi->setVirtualOpen(true);
    m_midi->setNoteMonitorEnabled(true);
}

void VirtualKeyboardWindow::closePort()
{
    if (!m_open || m_closing)
        return;
    // Hand-off ii: the mouse key's note off and setVirtualOpen(false)'s
    // releases signal noteState synchronously, from inside here.
    m_closing = true;
    m_piano->releaseMouseKey();
    if (m_midi) {
        m_midi->setVirtualOpen(false);
        m_midi->setNoteMonitorEnabled(false);
    }
    // Hidden, nothing is reported: whatever was lit is stale.
    m_piano->clearVirtualHeld();
    m_piano->clearHardwareDown();
    m_open = false;
    m_closing = false;
}

void VirtualKeyboardWindow::send(int status, int key, int velocity)
{
    if (!m_midi)
        return;
    MidiMsg msg;
    msg.status = uint8_t(status);
    msg.d1 = uint8_t(key);
    msg.d2 = uint8_t(velocity);
    m_midi->sendVirtual(msg);
}

void VirtualKeyboardWindow::noteState(int key, bool on, bool fromVirtual)
{
    if (fromVirtual) {
        m_piano->setVirtualHeld(key, on);
        // Released elsewhere (the port closing): no second note off later.
        if (!on)
            m_piano->forgetMouseKey(key);
    } else {
        m_piano->setHardwareDown(key, on);
    }
}

void VirtualKeyboardWindow::saveGeometryNow()
{
    QSettings().setValue(kGeometryKey, saveGeometry());
}

void VirtualKeyboardWindow::saveVelocity()
{
    const int value = m_velocity->value();
    if (value != m_savedVelocity) {
        m_savedVelocity = value;
        QSettings().setValue(kVelocityKey, value);
    }
}
