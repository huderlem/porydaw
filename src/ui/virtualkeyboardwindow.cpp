#include "virtualkeyboardwindow.h"

#include "audio/midiinput.h"
#include "layout.h"
#include "theme/themeruntime.h"
#include "ui/m4asemantics.h"

#include <QCloseEvent>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QSettings>
#include <QShowEvent>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#include <cmath>
#ifdef PORYDAW_HAVE_XCB
#include <QtGui/qguiapplication_platform.h>
#include <cstdlib>
#include <cstring>
#include <xcb/xcb.h>
#endif

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

// D3: the physical keys by platform code, in VirtualKeyboardWindow's
// physical-key order (z s x d c v g b h n j m , | q 2 w 3 e r 5 t 6 y 7 u i
// 9 o 0 | - =), each the key at that US-layout position: 13 bottom-row
// keys, 16 top-row keys, then - and =.
constexpr int kTableSize = VirtualKeyboardWindow::kPhysicalKeys;

// PS/2 scan code set 1, as Windows reports it (lParam bits 16–24). Linux
// evdev codes are the same numbers for this block of keys.
constexpr quint32 kSet1[kTableSize] = {
    0x2C, 0x1F, 0x2D, 0x20, 0x2E, 0x2F, 0x22, 0x30, 0x23, 0x31, 0x24, 0x32, 0x33, 0x10, 0x03, 0x11,
    0x04, 0x12, 0x13, 0x06, 0x14, 0x07, 0x15, 0x08, 0x16, 0x17, 0x0A, 0x18, 0x0B, 0x0C, 0x0D,
};
// XKB keycodes (xcb, Wayland) are the evdev code + 8.
constexpr quint32 kXkbOffset = 8;

// macOS kVK_ANSI_* virtual key codes (Carbon's Events.h), which name
// positions on the ANSI keyboard whatever the layout.
constexpr quint32 kMacAnsi[kTableSize] = {
    0x06, 0x01, 0x07, 0x02, 0x08, 0x09, 0x05, 0x0B, 0x04, 0x2D, 0x26, 0x2E, 0x2B, 0x0C, 0x13, 0x0D,
    0x14, 0x0E, 0x0F, 0x17, 0x11, 0x16, 0x10, 0x1A, 0x20, 0x22, 0x19, 0x1F, 0x1D, 0x1B, 0x18,
};

// The fallback: key() on a US layout, unshifted and shifted.
constexpr int kUsKeys[kTableSize] = {
    Qt::Key_Z, Qt::Key_S,     Qt::Key_X,     Qt::Key_D, Qt::Key_C, Qt::Key_V,     Qt::Key_G,
    Qt::Key_B, Qt::Key_H,     Qt::Key_N,     Qt::Key_J, Qt::Key_M, Qt::Key_Comma, Qt::Key_Q,
    Qt::Key_2, Qt::Key_W,     Qt::Key_3,     Qt::Key_E, Qt::Key_R, Qt::Key_5,     Qt::Key_T,
    Qt::Key_6, Qt::Key_Y,     Qt::Key_7,     Qt::Key_U, Qt::Key_I, Qt::Key_9,     Qt::Key_O,
    Qt::Key_0, Qt::Key_Minus, Qt::Key_Equal,
};
// Shift+key's key() on a US layout, where it differs (letters report the
// same key() shifted or not).
constexpr struct {
    int key;
    int physical;
} kUsShifted[] = {
    {Qt::Key_Less, 12},
    {Qt::Key_At, 14},
    {Qt::Key_NumberSign, 16},
    {Qt::Key_Percent, 19},
    {Qt::Key_AsciiCircum, 21},
    {Qt::Key_Ampersand, 23},
    {Qt::Key_ParenLeft, 26},
    {Qt::Key_ParenRight, 28},
    {Qt::Key_Underscore, VirtualKeyboardWindow::kOctaveDownKey},
    {Qt::Key_Plus, VirtualKeyboardWindow::kOctaveUpKey},
};

int findCode(const quint32 (&table)[kTableSize], quint32 code, quint32 offset = 0)
{
    for (int i = 0; i < kTableSize; i++) {
        if (table[i] + offset == code)
            return i;
    }
    return -1;
}

// D4: Ctrl/Alt/Meta make a chord, which is a command, never a note (Alt+B,
// Ctrl+Z). Shift is not one: Shift+Z plays Z, as the physical key is the
// same, so a Shift held while playing neither drops the note nor runs a
// Shift+letter shortcut.
bool commandModifiers(const QKeyEvent *event)
{
    return event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
}

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

void VirtualPianoWidget::endMouseDrag()
{
    releaseMouseKey();
    m_dragEnded = true;
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
    m_dragEnded = false;
    const int key = keyAt(event->position().toPoint());
    if (key >= 0)
        sound(key);
    event->accept();
}

void VirtualPianoWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!(event->buttons() & Qt::LeftButton) || m_dragEnded) {
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
    m_typed.fill(-1);
    // D1: Qt::Tool owned by the main window — above Porydaw's windows,
    // never above other applications, hidden with the app.
    setObjectName(QStringLiteral("virtualKeyboardWindow"));
    setWindowTitle(tr("Virtual MIDI Keyboard"));
    // The window itself is the focus widget (its controls take no focus), so
    // the ShortcutOverride and the typed key events land here, and so it
    // gets the FocusOut a menu popup sends (focusOutEvent). Being the focus
    // widget does not come from StrongFocus as such: with any policy but
    // NoFocus, Qt 6.2's activation falls back to making the window its own
    // focus widget (with NoFocus, focusWidget() stays null and keys arrive
    // only by QWidgetWindow's focus-object fallback, and no popup FocusOut
    // ever comes). event() takes the focus explicitly on WindowActivate, so
    // it never rests on that fallback; StrongFocus adds click-to-focus.
    setFocusPolicy(Qt::StrongFocus);

    m_piano = new VirtualPianoWidget(this);

    // The controls take no focus: the window keeps it, for typing.
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
    // The mouse is one more holder (PLAN §3 step 3): a C4 typed and clicked
    // at once sounds once and stops with the last release.
    connect(m_piano, &VirtualPianoWidget::mouseNoteOn, this, &VirtualKeyboardWindow::hold);
    connect(m_piano, &VirtualPianoWidget::mouseNoteOff, this, &VirtualKeyboardWindow::unhold);
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
    // D5: every held note goes first, typed and clicked; the keys still
    // down sound again only when pressed again.
    releaseAll();
    m_piano->setBase(base);
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

// WSLg (Weston's xwm behind XWayland, with Windows owning activation) breaks
// a Qt::Tool window two ways. Its X11 type, _NET_WM_WINDOW_TYPE_UTILITY, is
// parked at -32768,-32768: the surface still paints over the main window,
// but XWayland maps pointer events through the X window's position, so no
// click ever lands on it and Windows gets no window for it at all. And its
// WM_TRANSIENT_FOR makes the shell treat it as a dialog that keeps the focus
// over its parent: Windows activates the main window on a click, the shell
// hands the focus straight back to the keyboard, and the main window's
// shortcuts never fire again until the keyboard closes. Retyping the window
// NORMAL and pointing WM_TRANSIENT_FOR at the client leader (an X window
// that is never mapped, so it has no surface: the xwm then treats this one
// as an ordinary toplevel, as Qt's own parentless tool windows are) gives a
// window that is placed where it paints and swaps focus with the main
// window like any other. Pointed, not deleted: the xwm keeps the last
// value it read, and a deleted property reads as nothing, so a delete can
// lose the race with an earlier read while a rewrite is picked up by the
// next re-read. Both go on at ShowToParent, after QXcbWindow::show() has
// written its own WM_TRANSIENT_FOR, mapped the window and synced: the xwm
// has already read Qt's values at the MapRequest, but the rewrites dirty
// them, and it reads them again when the window's surface appears (its
// xserver_map_shell_surface), which waits for the first buffer this
// window commits from the event loop, after this returns. That re-read is
// the one that decides the surface's parent and placement.
// The main window's WindowShortcut actions still fire from here: that
// depends on the Qt::Tool flag, not on the X11 properties. WSL only:
// elsewhere UTILITY keeps its usual desktop behavior (above the parent, no
// taskbar entry). Costs under WSLg: the keyboard is a peer window, so the
// main window can cover it, and it has its own taskbar entry.
static bool underWslg()
{
#if defined(PORYDAW_HAVE_XCB) && QT_CONFIG(xcb)
    if (QGuiApplication::platformName() != QLatin1String("xcb"))
        return false;
    // WSL, and WSLg's own X server rather than a Windows one (VcXsrv, X410)
    // reached from WSL: /mnt/wslg is WSLg's mount.
    if (!qEnvironmentVariableIsSet("WSL_DISTRO_NAME") && !qEnvironmentVariableIsSet("WSL_INTEROP"))
        return false;
    return QFileInfo::exists(QStringLiteral("/mnt/wslg"));
#else
    return false;
#endif
}

// Before the map (QEvent::Show): with no transient parent on the QWindow,
// QXcbWindow::show() writes the client leader into WM_TRANSIENT_FOR itself,
// so the xwm never reads the main window there, and Qt no longer lists this
// window among the main window's transient children, whose WM_TRANSIENT_FOR
// it rewrites whenever the main window's native window is recreated (Qt
// 6.9), which would otherwise bring the dialog treatment back silently
// while the keyboard stays open. Under WSLg only. One cost: a WindowModal
// dialog of the main window (the export progress) no longer blocks this
// window there; application-modal ones still do.
static void detachWslgTransientParent(QWidget *window)
{
    if (!underWslg())
        return;
    if (QWindow *handle = window->windowHandle())
        handle->setTransientParent(nullptr);
}

static void applyWslgWindowFixups(QWidget *window)
{
#if defined(PORYDAW_HAVE_XCB) && QT_CONFIG(xcb)
    if (!underWslg())
        return;
    auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    if (!x11)
        return;
    xcb_connection_t *connection = x11->connection();
    if (!connection)
        return;
    // One round trip for the atoms, one for the leader.
    const char *const names[] = {"_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_NORMAL",
                                 "WM_CLIENT_LEADER"};
    xcb_intern_atom_cookie_t cookies[3];
    for (int i = 0; i < 3; i++)
        cookies[i] = xcb_intern_atom(connection, 0, uint16_t(std::strlen(names[i])), names[i]);
    xcb_atom_t atoms[3] = {XCB_ATOM_NONE, XCB_ATOM_NONE, XCB_ATOM_NONE};
    for (int i = 0; i < 3; i++) {
        xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(connection, cookies[i], nullptr);
        if (reply)
            atoms[i] = reply->atom;
        std::free(reply);
    }
    const xcb_atom_t type = atoms[0], normal = atoms[1], clientLeader = atoms[2];
    if (type == XCB_ATOM_NONE || normal == XCB_ATOM_NONE)
        return;
    const auto id = xcb_window_t(window->winId());
    // Qt's client leader, from this window's WM_CLIENT_LEADER; the root
    // window (unknown to the xwm too) if that is somehow missing.
    xcb_window_t leader = xcb_setup_roots_iterator(xcb_get_setup(connection)).data->root;
    if (clientLeader != XCB_ATOM_NONE) {
        const xcb_get_property_cookie_t cookie =
            xcb_get_property(connection, 0, id, clientLeader, XCB_ATOM_WINDOW, 0, 1);
        xcb_get_property_reply_t *reply = xcb_get_property_reply(connection, cookie, nullptr);
        if (reply && reply->type == XCB_ATOM_WINDOW && reply->format == 32 && reply->value_len >= 1)
            leader = *static_cast<xcb_window_t *>(xcb_get_property_value(reply));
        std::free(reply);
    }
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, id, type, XCB_ATOM_ATOM, 32, 1, &normal);
    // Qt wrote the same leader itself (detachWslgTransientParent); this
    // guards the value against any later Qt write of the main window.
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, id, XCB_ATOM_WM_TRANSIENT_FOR,
                        XCB_ATOM_WINDOW, 32, 1, &leader);
    xcb_flush(connection);
#else
    Q_UNUSED(window);
#endif
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
    m_holders.fill(0);
    m_typed.fill(-1);
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
    releaseAll();
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
        // The window's own note off comes with the key's holders already at
        // zero, so this forgets only what something else let go.
        if (!on && key >= 0 && key < 128) {
            m_piano->forgetMouseKey(key);
            m_holders[size_t(key)] = 0;
            std::replace(m_typed.begin(), m_typed.end(), key, -1);
        }
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

void VirtualKeyboardWindow::hold(int key)
{
    if (key < 0 || key >= 128)
        return;
    if (m_holders[size_t(key)]++ == 0)
        send(0x90, key, m_velocity->value());
}

void VirtualKeyboardWindow::unhold(int key)
{
    if (key < 0 || key >= 128 || m_holders[size_t(key)] == 0)
        return;
    if (--m_holders[size_t(key)] == 0)
        send(0x80, key, 0);
}

void VirtualKeyboardWindow::releaseAll()
{
    // The bookkeeping goes first: every note off below signals noteState
    // back synchronously.
    m_typed.fill(-1);
    m_piano->endMouseDrag(); // the mouse key's unhold
    for (int key = 0; key < 128; key++) {
        if (m_holders[size_t(key)] > 0) {
            m_holders[size_t(key)] = 0;
            send(0x80, key, 0);
        }
    }
}

VirtualKeyboardWindow::KeyTable VirtualKeyboardWindow::platformKeyTable()
{
    // The platform plugin cannot change at run time.
    static const KeyTable table = [] {
        const QString platform = QGuiApplication::platformName();
        if (platform == QLatin1String("xcb") || platform.startsWith(QLatin1String("wayland")))
            return KeyTable::Xkb;
        if (platform == QLatin1String("windows"))
            return KeyTable::Windows;
        if (platform == QLatin1String("cocoa"))
            return KeyTable::Mac;
        return KeyTable::None;
    }();
    return table;
}

int VirtualKeyboardWindow::physicalKey(const QKeyEvent *event, KeyTable table)
{
    switch (table) {
    case KeyTable::Mac:
        return findCode(kMacAnsi, event->nativeVirtualKey());
    case KeyTable::Xkb:
        if (event->nativeScanCode() != 0)
            return findCode(kSet1, event->nativeScanCode(), kXkbOffset);
        break;
    case KeyTable::Windows:
        if (event->nativeScanCode() != 0)
            return findCode(kSet1, event->nativeScanCode());
        break;
    case KeyTable::None:
        break;
    }
    // No native code: the US layout's keys, never the keypad's digits.
    if (event->modifiers() & Qt::KeypadModifier)
        return -1;
    const int key = event->key();
    for (int i = 0; i < kTableSize; i++) {
        if (kUsKeys[i] == key)
            return i;
    }
    for (const auto &shifted : kUsShifted) {
        if (shifted.key == key)
            return shifted.physical;
    }
    return -1;
}

bool VirtualKeyboardWindow::event(QEvent *event)
{
    switch (event->type()) {
    case QEvent::ShortcutOverride: {
        // D4: a typed note (or - / =) is the window's, so the main window's
        // WindowShortcut actions, which do fire from an active Qt::Tool
        // child, never also run for it. Everything else passes through:
        // Space (play/stop), Alt+B (closes this), Escape, Ctrl/Alt/Meta
        // chords.
        const auto *key = static_cast<QKeyEvent *>(event);
        if (!commandModifiers(key) && physicalKey(key, platformKeyTable()) >= 0) {
            event->accept();
            return true;
        }
        break;
    }
    case QEvent::Show:
        // Before show_sys() maps the window; its QWindow exists by now.
        detachWslgTransientParent(this);
        break;
    case QEvent::ShowToParent:
        // After show(): QXcbWindow::show() has set the properties this
        // rewrites and mapped the window (see applyWslgWindowFixups).
        applyWslgWindowFixups(this);
        break;
    case QEvent::WindowActivate:
        // The window is the focus widget while active, not only the focus
        // object by fallback, so a popup's FocusOut reaches focusOutEvent.
        if (!hasFocus())
            setFocus(Qt::ActiveWindowFocusReason);
        break;
    case QEvent::WindowDeactivate:
    case QEvent::WindowBlocked:
        // D4: the key releases go to whichever window is active now, and a
        // modal dialog takes the input: let everything go.
        releaseAll();
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

void VirtualKeyboardWindow::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && !isActiveWindow())
        releaseAll();
}

void VirtualKeyboardWindow::focusOutEvent(QFocusEvent *event)
{
    QWidget::focusOutEvent(event);
    // A menu popup (a menu-bar mnemonic such as Alt+V fires from this
    // window) leaves it active but takes every key event, the releases
    // included: let everything go, as on deactivation. releaseAll is
    // idempotent, so the FocusOut that comes with a deactivation or a hide
    // releases nothing twice.
    releaseAll();
}

void VirtualKeyboardWindow::keyPressEvent(QKeyEvent *event)
{
    const int physical = physicalKey(event, platformKeyTable());
    // D4: only while active (inactive, it gets no key events anyway), and
    // never a Ctrl/Alt/Meta chord.
    if (physical < 0 || !isActiveWindow() || commandModifiers(event)) {
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
    // D3: a held key sounds until released; its repeats change nothing.
    if (event->isAutoRepeat())
        return;
    if (physical == kOctaveDownKey || physical == kOctaveUpKey) {
        shiftOctave(physical == kOctaveUpKey ? 1 : -1); // releases everything first
        return;
    }
    // Already down (its release went elsewhere): it is sounding.
    if (m_typed[size_t(physical)] >= 0)
        return;
    const int key = base() + typingOffset(physical);
    // D5: at C8 the keys past G9 play nothing.
    if (!m_piano->playable(key))
        return;
    m_typed[size_t(physical)] = key;
    hold(key);
}

void VirtualKeyboardWindow::keyReleaseEvent(QKeyEvent *event)
{
    const int physical = physicalKey(event, platformKeyTable());
    if (physical < 0) {
        QWidget::keyReleaseEvent(event);
        return;
    }
    event->accept();
    if (event->isAutoRepeat() || physical >= kNoteKeys)
        return;
    // Whatever the modifiers now (Ctrl pressed while the note was held), the
    // release lets the note go.
    const int key = m_typed[size_t(physical)];
    if (key >= 0) {
        m_typed[size_t(physical)] = -1;
        unhold(key);
    }
}
