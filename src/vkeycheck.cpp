#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QRegion>
#include <QSettings>
#include <QSlider>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QWindow>
#include <cstdio>
#include <ctime>
#include <vector>

#include "audio/midiinput.h"
#include "core/miditimeline.h"
#include "mainwindow.h"
#include "ui/keymap.h"
#include "ui/songview.h"
#include "ui/virtualkeyboardwindow.h"

// --vkeycheck <projectRoot> <songLabel>: the Virtual MIDI Keyboard window
// (docs/virtual-keyboard/PLAN.md step 2), driven offscreen with the song
// open and the audio device parked, so every note the window sends stays in
// MidiInput's input ring to be read back. QSettings is redirected into a
// temp dir first; the project is only read.
//
// Covers: the View action shows and hides the window and follows its close
// button; shown, the Virtual Keyboard port is open and the note monitor on,
// hidden, both off; a click on a white and a black key sends the right note
// on and off at the slider's velocity; a glide releases the last key and
// sounds the next; a release outside the piano sends the note off; the
// octave buttons stop at C-1 and C8, where the keys past G9 are inert; the
// lit keys follow the window's own notes and a hardware keyboard's (a
// hardware release never unlights a key the mouse holds, portsClosed clears
// only hardware keys), and a lit key repaints only its own rectangle;
// hiding with the mouse holding a key releases it once; the octave, the
// velocity, the geometry and the visibility persist; the track-and-voice
// line follows the selection and the edit cursor.
//
// Step 3 (typing): every one of D2's 29 keys by the key() fallback the
// offscreen platform leaves (it has no native codes) plays its note at the
// slider's velocity, Shift+key too; the scan-code tables for xcb, Windows
// and macOS are unit-tested with synthetic native codes; chords, auto-repeat,
// comma + q and mouse + typed holders (one note on, one note off at the last
// release, lit until then), - / = with their clamps and the release before
// the shift, C8's inert keys, deactivation / ActivationChange / a modal
// dialog / hide releasing everything (the mouse key and its drag included),
// no notes while inactive, the ShortcutOverride claims (a main-window
// action bound to S does not fire from the window, Space and Alt+B do) and
// no attached action using Qt::ApplicationShortcut. The window is left open
// when MainWindow is destroyed, so ASAN covers quitting with it shown
// (§4.1 hand-off i).
//
// With PORYDAW_VKEY_CPU=1 it also measures process CPU over 10 s idle with
// the window hidden, shown, and shown with a held chord (D12), and prints
// the numbers; it asserts nothing about them.

namespace {

int g_failures = 0;

bool check(bool ok, const char *what)
{
    if (!ok) {
        std::fprintf(stderr, "vkeycheck: FAIL: %s\n", what);
        g_failures++;
    }
    return ok;
}

bool same(const MidiMsg &a, const MidiMsg &b)
{
    return a.status == b.status && a.d1 == b.d1 && a.d2 == b.d2;
}

std::vector<MidiMsg> take(MidiInputRing &ring)
{
    std::vector<MidiMsg> out;
    MidiMsg msg;
    while (ring.pop(&msg))
        out.push_back(msg);
    return out;
}

bool sameList(const std::vector<MidiMsg> &got, const std::vector<MidiMsg> &want)
{
    if (got.size() != want.size())
        return false;
    for (size_t i = 0; i < got.size(); i++) {
        if (!same(got[i], want[i]))
            return false;
    }
    return true;
}

void mouse(QWidget *widget, QEvent::Type type, const QPoint &pos, Qt::MouseButton button,
           Qt::MouseButtons buttons)
{
    QMouseEvent event(type, QPointF(pos), QPointF(widget->mapToGlobal(pos)), button, buttons,
                      Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}

void press(QWidget *w, const QPoint &pos)
{
    mouse(w, QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton);
}

void drag(QWidget *w, const QPoint &pos)
{
    mouse(w, QEvent::MouseMove, pos, Qt::NoButton, Qt::LeftButton);
}

void release(QWidget *w, const QPoint &pos)
{
    mouse(w, QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton);
}

// A point on key that no other key covers: low on a white key (under the
// black keys), the middle of a black one.
QPoint spot(const VirtualPianoWidget *piano, int key)
{
    const QRect r = piano->keyRect(key);
    return QPoint(r.center().x(), r.bottom() - 2);
}

// Collects the rectangles the piano paints.
class PaintSpy : public QObject
{
  public:
    QRegion painted;
    int paints = 0;

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Paint) {
            painted += static_cast<QPaintEvent *>(event)->region();
            paints++;
        }
        return QObject::eventFilter(watched, event);
    }
};

// A key as a real press reaches the window: QApplication::notify sends a
// non-spontaneous KeyPress's ShortcutOverride first and runs a matching
// shortcut in its place when nobody claims it, then delivers the KeyPress.
void key(QWidget *w, QEvent::Type type, int qtKey, Qt::KeyboardModifiers mods = Qt::NoModifier,
         bool repeat = false)
{
    QKeyEvent event(type, qtKey, mods, QString(), repeat);
    QApplication::sendEvent(w, &event);
}

void tap(QWidget *w, int qtKey, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    key(w, QEvent::KeyPress, qtKey, mods);
    key(w, QEvent::KeyRelease, qtKey, mods);
}

// Whether w claims the key's ShortcutOverride (so no shortcut runs for it).
bool claims(QWidget *w, int qtKey, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QKeyEvent event(QEvent::ShortcutOverride, qtKey, mods);
    event.ignore();
    QApplication::sendEvent(w, &event);
    return event.isAccepted();
}

// The physical key a synthetic event with these native codes names. key()
// is deliberately a key no table maps, so only the native code can decide.
int nativeKey(VirtualKeyboardWindow::KeyTable table, quint32 scanCode, quint32 virtualKey,
              int qtKey = Qt::Key_F35)
{
    QKeyEvent event(QEvent::KeyPress, qtKey, Qt::NoModifier, scanCode, virtualKey, 0);
    return VirtualKeyboardWindow::physicalKey(&event, table);
}

void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

double cpuSecondsOver(int ms)
{
    const std::clock_t start = std::clock();
    spin(ms);
    return double(std::clock() - start) / CLOCKS_PER_SEC;
}

} // namespace

int MainWindow::runVirtualKeyboardCheck(const QString &projectRoot, const QString &songLabel)
{
    const int before = g_failures;
    if (!check(openProjectDir(projectRoot, /*interactive=*/false), "the project failed to open"))
        return g_failures - before;
    loadSongByLabel(songLabel);
    if (!m_active || m_active->doc.label() != songLabel) {
        check(false, "the song did not load");
        return g_failures - before;
    }
    MidiInput &midi = midiInput();
    if (!midi.openKeyboardPorts().isEmpty()) {
        std::printf("vkeycheck: SKIP (a real MIDI keyboard port is open)\n");
        return g_failures - before;
    }
    MidiInputRing &ring = midi.ring();
    // Parked, the audio callback never drains the ring: what the window
    // sends stays there to be read.
    if (m_audioOk)
        m_audio.holdDeviceForTest(true);
    take(ring);

    VirtualKeyboardWindow *window = m_virtualKeyboard;
    QAction *action = m_virtualKeyboardAction;
    if (!check(window && action, "no Virtual Keyboard window or View action"))
        return g_failures - before;
    VirtualPianoWidget *piano = window->piano();
    check(!window->isVisible() && !action->isChecked() && !midi.virtualOpen() &&
              !midi.noteMonitorEnabled(),
          "the window, its port or the monitor started open");
    check(window->windowTitle() == tr("Virtual MIDI Keyboard") &&
              window->windowFlags().testFlag(Qt::Tool) && window->parentWidget() == this,
          "the window is not a tool window of the main window titled \"Virtual MIDI Keyboard\"");
    check(action->shortcut() == QKeySequence(QStringLiteral("Alt+B")),
          "View → Virtual MIDI Keyboard's default shortcut is not Alt+B");
    check(window->base() == VirtualKeyboardWindow::kDefaultBase &&
              window->velocity() == VirtualKeyboardWindow::kDefaultVelocity,
          "the window did not start at C3, velocity 100");

    // The View action shows it: the port opens and the monitor starts.
    action->setChecked(true);
    QApplication::processEvents();
    check(window->isVisible() && midi.virtualOpen() && midi.noteMonitorEnabled() &&
              midi.openKeyboardPorts().contains(MidiInput::virtualPortName()),
          "showing the window did not open the Virtual Keyboard port and the note monitor");
    check(QSettings().value(QStringLiteral("virtualKeyboard/visible")).toBool(),
          "showing the window did not persist its visibility");
    // Well inside the offscreen platform's 800x600 screen (and placed so
    // below): newer Qt (CI's 6.9) adjusts restoreGeometry() for frames that
    // leave the screen, and the restore is compared exactly further down
    // (as sessioncheck.cpp's resize(777, 505) notes).
    window->resize(560, 180);
    QApplication::processEvents();

    // At the least height, a C's octave name stays clear of its C# (both
    // label lines sit below the black keys).
    {
        const QSize keep = window->size();
        window->resize(window->minimumSizeHint());
        QApplication::processEvents();
        const QRect octave = piano->labelRect(48, true);
        check(piano->height() == piano->minimumSizeHint().height() && !octave.isEmpty() &&
                  octave.top() > piano->keyRect(49).bottom(),
              "at the least height the C's octave label runs under the C# key");
        window->resize(keep);
        QApplication::processEvents();
    }

    // The track-and-voice line follows the selection.
    {
        SongView *view = m_active->view;
        const MidiTimeline *tl = view->timeline();
        const auto expected = [&](int t) {
            QString name = tl->tracks[t].name;
            if (name.isEmpty())
                name = tr("Track %1").arg(t + 1);
            return QStringLiteral("%1 · %2 — %3").arg(t + 1).arg(name, view->instrumentLabel(t));
        };
        int other = -1;
        for (int t = 0; t < 16 && other < 0; t++) {
            if (tl->tracks[t].used && t != view->selectedTrack())
                other = t;
        }
        check(window->target() == expected(view->selectedTrack()),
              "the window does not name the selected track and its voice");
        if (check(other >= 0, "the song has one track only")) {
            const int was = view->selectedTrack();
            view->selectTrack(other);
            QApplication::processEvents();
            check(window->target() == expected(other) && window->target() != expected(was),
                  "the window's track line did not follow the selection");
            view->selectTrack(was);
        }
        // The voice follows the edit cursor, as the header's does: a voice
        // change somewhere in the song.
        bool tested = false;
        for (const VoiceChange &vc : view->model().voices) {
            if (vc.tick == 0 || view->programAtTick(vc.track, 0) == vc.program)
                continue;
            view->selectTrack(vc.track);
            view->commitEditCursor(0);
            QApplication::processEvents();
            const QString atStart = window->target();
            view->commitEditCursor(vc.tick);
            QApplication::processEvents();
            check(window->target() != atStart &&
                      window->target().endsWith(view->voiceLabel(vc.program)),
                  "the window's voice did not follow the edit cursor across a voice change");
            view->commitEditCursor(0);
            QApplication::processEvents();
            // Playing past the change, the line keeps the edit cursor's
            // voice (the header shows the playhead's), so the stop never
            // leaves it showing the playhead's.
            view->setPlayheadSample(tl->sampleForTick(vc.tick + 1), /*playing=*/true);
            updateVirtualKeyboardTarget();
            check(window->target() == atStart,
                  "the window's voice followed the playhead instead of the edit cursor");
            view->setPlayheadSample(0, /*playing=*/false);
            updateVirtualKeyboardTarget();
            tested = true;
            break;
        }
        if (!tested)
            std::printf("vkeycheck: voice-follows-cursor SKIP (no mid-song voice change)\n");
        take(ring);
    }

    // A white key and a black key, at the slider's velocity.
    window->setVelocity(77);
    check(window->velocitySlider()->value() == 77, "setVelocity did not move the slider");
    take(ring);
    press(piano, spot(piano, 60));
    check(piano->isVirtualHeld(60) && piano->isLit(60) && piano->mouseKey() == 60,
          "a click on C4 did not light it");
    release(piano, spot(piano, 60));
    check(sameList(take(ring), {{0x90, 60, 77}, {0x80, 60, 0}}),
          "a click on the white key C4 did not send note on 60 at 77 and its note off");
    check(!piano->isLit(60) && piano->mouseKey() < 0, "C4 stayed lit after the release");
    {
        const QRect black = piano->keyRect(61);
        press(piano, black.center());
        release(piano, black.center());
        check(sameList(take(ring), {{0x90, 61, 77}, {0x80, 61, 0}}),
              "a click on the black key C#4 did not send note on 61 at 77 and its note off");
        // The white key's upper part under a black key belongs to the black.
        const QRect white = piano->keyRect(62);
        check(piano->keyAt(QPoint(white.left() + 1, 2)) == 61 &&
                  piano->keyAt(QPoint(white.left() + 1, white.bottom() - 2)) == 62,
              "the black key does not cover the white keys' upper part");
    }
    window->setVelocity(VirtualKeyboardWindow::kDefaultVelocity);
    check(QSettings().value(QStringLiteral("virtualKeyboard/velocity")).toInt() ==
              VirtualKeyboardWindow::kDefaultVelocity,
          "setVelocity did not persist the velocity");
    // A drag on the slider saves once, on its release, not at every step.
    {
        QSlider *slider = window->velocitySlider();
        slider->setSliderDown(true);
        slider->setValue(50);
        slider->setValue(51);
        check(window->velocity() == 51 &&
                  QSettings().value(QStringLiteral("virtualKeyboard/velocity")).toInt() ==
                      VirtualKeyboardWindow::kDefaultVelocity,
              "dragging the velocity slider saved before the release");
        slider->setSliderDown(false); // sliderReleased
        check(QSettings().value(QStringLiteral("virtualKeyboard/velocity")).toInt() == 51,
              "releasing the velocity slider did not save it");
        // Hidden mid-drag (no release comes), the hide saves it.
        slider->setSliderDown(true);
        slider->setValue(52);
        action->setChecked(false);
        QApplication::processEvents();
        check(QSettings().value(QStringLiteral("virtualKeyboard/velocity")).toInt() == 52,
              "hiding the window mid-drag did not save the velocity");
        slider->setSliderDown(false);
        action->setChecked(true);
        QApplication::processEvents();
        window->setVelocity(VirtualKeyboardWindow::kDefaultVelocity);
    }

    // A glide: C4 → D4 releases C4 and sounds D4; off the piano D4 keeps
    // sounding; the release outside the widget stops it.
    press(piano, spot(piano, 60));
    drag(piano, spot(piano, 60) + QPoint(1, 0)); // the same key: nothing
    drag(piano, spot(piano, 62));
    check(sameList(take(ring), {{0x90, 60, 100}, {0x80, 60, 0}, {0x90, 62, 100}}),
          "a glide from C4 to D4 did not release C4 and sound D4");
    check(!piano->isLit(60) && piano->isLit(62), "the glide left C4 lit or D4 dark");
    drag(piano, QPoint(-40, -40));
    check(take(ring).empty() && piano->mouseKey() == 62,
          "dragging off the piano released the key (it sounds until the release)");
    release(piano, QPoint(-40, -40));
    check(sameList(take(ring), {{0x80, 62, 0}}),
          "a release outside the piano did not send the note off");
    check(!piano->isLit(62), "D4 stayed lit after a release outside the piano");

    // One mouse note at a time: a second press (its release lost) lets the
    // first key go before it sounds.
    press(piano, spot(piano, 60));
    press(piano, spot(piano, 62));
    release(piano, spot(piano, 62));
    check(sameList(take(ring), {{0x90, 60, 100}, {0x80, 60, 0}, {0x90, 62, 100}, {0x80, 62, 0}}),
          "a press with a mouse key already held did not release it first");

    // Octave buttons: down to C-1 and no further, up to C8 and no further.
    for (int i = 0; i < 12; i++)
        window->octaveDownButton()->click();
    check(window->base() == 0 && !window->octaveDownButton()->isEnabled() &&
              window->findChild<QLabel *>(QStringLiteral("virtualKeyboardOctave"))->text() ==
                  QStringLiteral("C-1"),
          "the octave down button did not stop at C-1 (or its label or button state is wrong)");
    window->shiftOctave(-1);
    check(window->base() == 0, "shiftOctave went below C-1");
    for (int i = 0; i < 12; i++)
        window->octaveUpButton()->click();
    check(window->base() == 108 && !window->octaveUpButton()->isEnabled() &&
              window->octaveDownButton()->isEnabled() &&
              window->findChild<QLabel *>(QStringLiteral("virtualKeyboardOctave"))->text() ==
                  QStringLiteral("C8"),
          "the octave up button did not stop at C8 (or its label or button state is wrong)");
    window->shiftOctave(1);
    check(window->base() == 108, "shiftOctave went above C8");
    take(ring);
    // At C8, G9 (127) plays and G#9 up (128..135) is inert.
    check(piano->playable(127) && !piano->playable(128) && !piano->keyRect(128).isEmpty(),
          "at C8, G9 is not playable or G#9 is not drawn");
    press(piano, spot(piano, 128));
    release(piano, spot(piano, 128));
    press(piano, piano->keyRect(135).center());
    release(piano, piano->keyRect(135).center());
    check(take(ring).empty(), "at C8, a key above 127 sent a note");
    press(piano, spot(piano, 127));
    drag(piano, spot(piano, 129)); // onto an inert key: G9 keeps sounding
    release(piano, spot(piano, 129));
    check(sameList(take(ring), {{0x90, 127, 100}, {0x80, 127, 0}}),
          "at C8, G9 did not play, or a glide onto an inert key changed the note");
    // Shifting with a key held releases it first.
    window->setBase(48);
    press(piano, spot(piano, 60));
    window->shiftOctave(1);
    check(sameList(take(ring), {{0x90, 60, 100}, {0x80, 60, 0}}) && piano->mouseKey() < 0,
          "an octave shift with a mouse key held did not release it");
    release(piano, spot(piano, 72));
    check(take(ring).empty(), "the release after an octave shift sent a second note off");
    window->setBase(48);

    // Lit keys: a hardware keyboard's notes, through the note monitor.
    const QString hardware = QStringLiteral("vkeycheck Keyboard");
    midi.setPortListForTest({hardware});
    midi.setOpenPortsForTest({hardware});
    QApplication::processEvents();
    PaintSpy spy;
    piano->installEventFilter(&spy);
    QApplication::processEvents();
    spy.painted = QRegion();
    spy.paints = 0;
    midi.injectForTest({0x90, 64, 90}, hardware);
    QApplication::processEvents(); // the monitor's hop
    QApplication::processEvents(); // the repaint it asked for
    check(piano->isHardwareDown(64) && piano->isLit(64) && !piano->isVirtualHeld(64),
          "a hardware keyboard's note did not light its key");
    // D12: the repaint is that key's rectangle, not the piano.
    check(spy.paints > 0 && !spy.painted.isEmpty() &&
              piano->keyRect(64).contains(spy.painted.boundingRect()),
          "a lit hardware key repainted more than its own rectangle (or nothing)");
    piano->removeEventFilter(&spy);
    midi.injectForTest({0x80, 64, 0}, hardware);
    QApplication::processEvents();
    check(!piano->isLit(64), "a hardware keyboard's release did not unlight its key");
    // A hardware release never unlights a key the mouse holds.
    press(piano, spot(piano, 64));
    midi.injectForTest({0x90, 64, 90}, hardware);
    QApplication::processEvents();
    midi.injectForTest({0x80, 64, 0}, hardware);
    QApplication::processEvents();
    check(piano->isLit(64) && piano->isVirtualHeld(64) && !piano->isHardwareDown(64),
          "a hardware release unlit a key the mouse holds");
    release(piano, spot(piano, 64));
    check(!piano->isLit(64), "E4 stayed lit after the mouse let go");
    // portsClosed clears the hardware keys, never the mouse's.
    midi.injectForTest({0x90, 65, 90}, hardware);
    QApplication::processEvents();
    press(piano, spot(piano, 67));
    check(piano->isLit(65) && piano->isLit(67), "a hardware key or the mouse key is not lit");
    midi.setPortRole(hardware, MidiInput::PortRole::Surface); // an open port flips: portsClosed
    QApplication::processEvents();
    check(!piano->isLit(65) && piano->isLit(67) && piano->mouseKey() == 67,
          "portsClosed did not clear the hardware keys only");
    midi.setPortRole(hardware, MidiInput::PortRole::Keyboard);
    QApplication::processEvents();
    release(piano, spot(piano, 67));
    take(ring);

    // Hiding with a mouse key held releases it, once, and closes the port
    // and the monitor (the release signals back from inside the hide).
    midi.injectForTest({0x90, 66, 90}, hardware);
    QApplication::processEvents();
    take(ring);
    press(piano, spot(piano, 60));
    check(piano->isLit(60) && piano->isLit(66), "C4 or the hardware F#4 is not lit");
    window->setBase(60); // releases C4; persisted below
    window->setVelocity(33);
    press(piano, spot(piano, 72));
    take(ring);
    action->setChecked(false);
    QApplication::processEvents();
    check(!window->isVisible() && !midi.virtualOpen() && !midi.noteMonitorEnabled(),
          "hiding the window did not close the port and the note monitor");
    check(sameList(take(ring), {{0x80, 72, 0}}),
          "hiding with a mouse key held did not release it exactly once");
    check(piano->mouseKey() < 0 && !piano->isLit(72) && !piano->isLit(66),
          "keys stayed lit, or the mouse key held, after hiding");
    check(!QSettings().value(QStringLiteral("virtualKeyboard/visible")).toBool(),
          "hiding the window did not persist its visibility");
    // A stale mark of the window's own (only a direct port open, as
    // midicheck's, could leave one) is gone on the next show.
    piano->setVirtualHeld(70, true);
    // Hidden: a hardware note lights nothing, and the window sends nothing.
    midi.injectForTest({0x90, 69, 90}, hardware);
    QApplication::processEvents();
    check(!piano->isLit(69), "a hardware note lit a key with the window hidden");
    midi.injectForTest({0x80, 69, 0}, hardware);
    midi.injectForTest({0x80, 66, 0}, hardware);
    QApplication::processEvents();
    take(ring);

    // Persistence: the octave, the velocity and the geometry survive a
    // hide/show, and a new window reads them back.
    window->move(40, 60); // on screen with margin: see the resize above
    action->setChecked(true);
    QApplication::processEvents();
    check(!piano->isLit(70) && !piano->isVirtualHeld(70),
          "a stale virtual key stayed lit across a show");
    check(window->base() == 60 && window->velocity() == 33,
          "the octave or the velocity changed across a hide/show");
    check(QSettings().value(QStringLiteral("virtualKeyboard/base")).toInt() == 60 &&
              QSettings().value(QStringLiteral("virtualKeyboard/velocity")).toInt() == 33,
          "the octave or the velocity was not persisted");
    const QPoint placed = window->pos();
    action->setChecked(false);
    {
        VirtualKeyboardWindow fresh(nullptr, this);
        check(fresh.base() == 60 && fresh.velocity() == 33,
              "a new window did not restore the octave and the velocity");
        // Detached (no MidiInput), it plays nothing and does not crash.
        // Shown, its frame lands where the other's was (before, pos() is
        // the client area's on some platforms).
        fresh.show();
        QApplication::processEvents();
        check(!QSettings()
                      .value(QStringLiteral("virtualKeyboard/geometry"))
                      .toByteArray()
                      .isEmpty() &&
                  fresh.pos() == placed && fresh.size() == window->size(),
              "a new window did not restore the geometry");
        press(fresh.piano(), spot(fresh.piano(), 60));
        release(fresh.piano(), spot(fresh.piano(), 60));
        fresh.hide();
    }
    check(take(ring).empty() && !midi.virtualOpen(),
          "a window without MidiInput sent a note or opened the port");
    // A base off a C lands on the C below.
    window->setBase(65);
    check(window->base() == 60, "setBase did not round to a C");

    // The close button unchecks the action and persists "hidden"; the
    // action shows it again. The title bar's close is a window-system close
    // event, which QWidgetWindow turns into a spontaneous QCloseEvent; a
    // QCloseEvent sent to the QWindow (not QWindow::close(), which marks
    // itself programmatic) takes that same path with public API.
    const auto titleBarClose = [window] {
        QCloseEvent event;
        QApplication::sendEvent(window->windowHandle(), &event);
        QApplication::processEvents();
    };
    action->setChecked(true);
    QApplication::processEvents();
    if (check(window->windowHandle() != nullptr, "the shown window has no QWindow")) {
        titleBarClose();
        check(!window->isVisible() && !action->isChecked() && !midi.virtualOpen(),
              "closing the window did not uncheck View → Virtual MIDI Keyboard");
        check(QSettings().contains(QStringLiteral("virtualKeyboard/visible")) &&
                  !QSettings().value(QStringLiteral("virtualKeyboard/visible")).toBool(),
              "closing the window did not persist it hidden");
    }
    // Quitting closes it too, after the main window has gone: Qt's
    // last-window-closed quit calls close() on the still-visible tool
    // window. That (or any close from code) must keep it "shown" for the
    // next launch, and so must a window-system close once the main window
    // is hidden (a platform quit closing every window).
    action->setChecked(true);
    QApplication::processEvents();
    window->close(); // from code, main window shown
    QApplication::processEvents();
    check(!window->isVisible() && action->isChecked() &&
              QSettings().value(QStringLiteral("virtualKeyboard/visible")).toBool(),
          "a close() from code persisted the window hidden");
    window->show();
    QApplication::processEvents();
    hide(); // the main window goes first, as on quit
    QApplication::processEvents();
    check(window->isVisible(), "hiding the main window hid the tool window");
    window->close();
    QApplication::processEvents();
    check(!window->isVisible() && action->isChecked() &&
              QSettings().value(QStringLiteral("virtualKeyboard/visible")).toBool(),
          "the quit path's close() persisted the window hidden");
    window->show();
    QApplication::processEvents();
    if (window->windowHandle())
        titleBarClose();
    check(!window->isVisible() && action->isChecked() &&
              QSettings().value(QStringLiteral("virtualKeyboard/visible")).toBool(),
          "a window-system close with the main window gone persisted the window hidden");
    show();
    QApplication::processEvents();
    action->setChecked(false);
    QApplication::processEvents();

    // Startup restore: shown when it was shown last, without taking the
    // keys from the main window. (Offscreen activates every window it
    // shows, so this asserts on the path: no activateWindow() on restore,
    // and the show is marked WA_ShowWithoutActivating.)
    activateWindow();
    QApplication::processEvents();
    const int activations = m_virtualKeyboardActivations;
    QSettings().setValue(QStringLiteral("virtualKeyboard/visible"), true);
    restoreVirtualKeyboard();
    QApplication::processEvents();
    check(window->isVisible() && action->isChecked() && midi.virtualOpen(),
          "restoreVirtualKeyboard did not show a window that was shown last time");
    check(m_virtualKeyboardActivations == activations &&
              window->testAttribute(Qt::WA_ShowWithoutActivating),
          "restoreVirtualKeyboard activated the window (it took focus from the main window)");
    // The user's toggle does activate it, ready for typing.
    action->setChecked(false);
    QApplication::processEvents();
    action->setChecked(true);
    QApplication::processEvents();
    check(m_virtualKeyboardActivations == activations + 1 &&
              !window->testAttribute(Qt::WA_ShowWithoutActivating),
          "View → Virtual MIDI Keyboard did not activate the window");

    // ---- Step 3: typing (PLAN §3 step 3, D2–D5) ----
    {
        // D4: a window's ShortcutOverride holds back WindowShortcut actions
        // only; an ApplicationShortcut would need more.
        bool noApplication = true;
        for (QAction *a : keymap::Registry::instance().attachedActions())
            noApplication = noApplication && a->shortcutContext() != Qt::ApplicationShortcut;
        check(noApplication, "an attached QAction uses Qt::ApplicationShortcut");

        // D3: the native tables, by synthetic codes (physical order: z s x d
        // c v g b h n j m , | q 2 w 3 e r 5 t 6 y 7 u i 9 o 0 | - =).
        using KT = VirtualKeyboardWindow::KeyTable;
        const int down = VirtualKeyboardWindow::kOctaveDownKey;
        const int up = VirtualKeyboardWindow::kOctaveUpKey;
        struct Native {
            const char *name;
            quint32 set1;
            quint32 mac;
            int physical;
        };
        const Native natives[] = {
            {"z", 0x2C, 0x06, 0},  {"s", 0x1F, 0x01, 1},    {"q", 0x10, 0x0C, 13},
            {"2", 0x03, 0x13, 14}, {",", 0x33, 0x2B, 12},   {"0", 0x0B, 0x1D, 28},
            {"m", 0x32, 0x2E, 11}, {"-", 0x0C, 0x1B, down}, {"=", 0x0D, 0x18, up},
        };
        bool tables = true;
        for (const Native &n : natives) {
            const bool ok = nativeKey(KT::Windows, n.set1, 0) == n.physical &&
                            nativeKey(KT::Xkb, n.set1 + 8, 0) == n.physical &&
                            nativeKey(KT::Mac, 0, n.mac) == n.physical;
            if (!ok)
                std::fprintf(stderr, "vkeycheck: native code for '%s' is wrong\n", n.name);
            tables = tables && ok;
        }
        check(tables, "a native scan-code table maps a key wrongly");
        // A known native code the table lacks is no key, whatever key() says
        // (AZERTY's A position reports key() Q); zero falls back to key(),
        // except on the Mac, where 0 is kVK_ANSI_A.
        check(nativeKey(KT::Windows, 0x1E, 0, Qt::Key_Q) == -1 &&
                  nativeKey(KT::Xkb, 0x1E + 8, 0, Qt::Key_Q) == -1 &&
                  nativeKey(KT::Mac, 0, 0x00, Qt::Key_Q) == -1,
              "a native code outside the table fell back to key()");
        check(nativeKey(KT::Windows, 0, 0, Qt::Key_Z) == 0 &&
                  nativeKey(KT::Xkb, 0, 0, Qt::Key_Z) == 0 &&
                  nativeKey(KT::None, 0x10, 0x0C, Qt::Key_Z) == 0,
              "with no native code (or no table) the key did not fall back to key()");
        // The native code wins over key() (QWERTZ's Y position reports Z).
        check(nativeKey(KT::Windows, 0x15, 0, Qt::Key_Z) == 22 &&
                  nativeKey(KT::Xkb, 0x15 + 8, 0, Qt::Key_Z) == 22 &&
                  nativeKey(KT::Mac, 0, 0x10, Qt::Key_Z) == 22,
              "key() overrode a native code");
        {
            QKeyEvent keypad(QEvent::KeyPress, Qt::Key_2, Qt::KeypadModifier);
            check(VirtualKeyboardWindow::physicalKey(&keypad, KT::None) == -1,
                  "a keypad digit played as the fallback's top-row digit");
        }
        check(VirtualKeyboardWindow::platformKeyTable() == KT::None,
              "offscreen is not on the key() fallback (the typing checks below assume it)");

        // The window shown, active and at C3, velocity 90. Offscreen never
        // reports a hidden window losing activation, so a window shown again
        // while still Qt's focus window is never activated again (the
        // activation is a no-op change): the main window takes it first.
        const auto activateKeyboard = [this, window] {
            activateWindow();
            QApplication::processEvents();
            window->activateWindow();
            QApplication::processEvents();
        };
        // Where a real key goes: the focus object (the window, once it took
        // the focus on activation).
        const auto focused = [window]() -> QWidget * {
            auto *w = qobject_cast<QWidget *>(QGuiApplication::focusObject());
            return w ? w : window;
        };
        action->setChecked(true);
        activateKeyboard();
        window->setBase(48);
        window->setVelocity(90);
        take(ring);
        // The window is the focus widget, not just the focus object by
        // QWidgetWindow's fallback: only the focus widget gets a popup's
        // FocusOut.
        check(QGuiApplication::focusObject() == window && QApplication::focusWidget() == window,
              "the activated window is not the focus widget");
        // And by its own doing: Qt 6.2's activation also makes a window with
        // a focus policy its own focus widget, so with NoFocus (and the
        // remembered focus cleared) only WindowActivate's setFocus does.
        window->setFocusPolicy(Qt::NoFocus);
        activateWindow();
        QApplication::processEvents();
        window->clearFocus();
        window->activateWindow();
        QApplication::processEvents();
        check(QApplication::focusWidget() == window,
              "the window did not take the focus itself on activation");
        window->setFocusPolicy(Qt::StrongFocus);
        if (check(window->isActiveWindow() && QApplication::activeWindow() == window,
                  "the shown window did not become active (typing checks "
                  "skipped)")) {
            // D2 by the fallback: all 29 keys, at the slider's velocity.
            {
                const struct {
                    int key;
                    int offset;
                } keys[] = {
                    {Qt::Key_Z, 0},      {Qt::Key_S, 1},  {Qt::Key_X, 2},  {Qt::Key_D, 3},
                    {Qt::Key_C, 4},      {Qt::Key_V, 5},  {Qt::Key_G, 6},  {Qt::Key_B, 7},
                    {Qt::Key_H, 8},      {Qt::Key_N, 9},  {Qt::Key_J, 10}, {Qt::Key_M, 11},
                    {Qt::Key_Comma, 12}, {Qt::Key_Q, 12}, {Qt::Key_2, 13}, {Qt::Key_W, 14},
                    {Qt::Key_3, 15},     {Qt::Key_E, 16}, {Qt::Key_R, 17}, {Qt::Key_5, 18},
                    {Qt::Key_T, 19},     {Qt::Key_6, 20}, {Qt::Key_Y, 21}, {Qt::Key_7, 22},
                    {Qt::Key_U, 23},     {Qt::Key_I, 24}, {Qt::Key_9, 25}, {Qt::Key_O, 26},
                    {Qt::Key_0, 27},
                };
                bool all = true;
                for (const auto &k : keys) {
                    const uint8_t note = uint8_t(48 + k.offset);
                    key(focused(), QEvent::KeyPress, k.key);
                    const bool lit = piano->isLit(note) && piano->isVirtualHeld(note);
                    key(focused(), QEvent::KeyRelease, k.key);
                    const bool ok = lit && !piano->isLit(note) &&
                                    sameList(take(ring), {{0x90, note, 90}, {0x80, note, 0}});
                    if (!ok)
                        std::fprintf(stderr, "vkeycheck: typed key 0x%x did not play %d\n", k.key,
                                     note);
                    all = all && ok;
                }
                check(all, "a typed key did not play (and light) its D2 note at "
                           "the slider's velocity");
                check(window->isVisible() && action->isChecked(),
                      "a plain B closed the window (only Alt+B should)");
            }
            // Shift is the plain key (Shift+2 reports Key_At); its release may
            // come without Shift.
            key(focused(), QEvent::KeyPress, Qt::Key_Z, Qt::ShiftModifier);
            key(focused(), QEvent::KeyRelease, Qt::Key_Z, Qt::ShiftModifier);
            key(focused(), QEvent::KeyPress, Qt::Key_At, Qt::ShiftModifier);
            key(focused(), QEvent::KeyRelease, Qt::Key_2);
            key(focused(), QEvent::KeyPress, Qt::Key_Less, Qt::ShiftModifier);
            key(focused(), QEvent::KeyRelease, Qt::Key_Comma);
            check(sameList(take(ring), {{0x90, 48, 90},
                                        {0x80, 48, 0},
                                        {0x90, 61, 90},
                                        {0x80, 61, 0},
                                        {0x90, 60, 90},
                                        {0x80, 60, 0}}),
                  "Shift+Z, Shift+2 or Shift+, did not play as the plain key");
            // Ctrl/Alt/Meta chords are commands, never notes; a note held when
            // Ctrl goes down still stops at its release.
            tap(focused(), Qt::Key_Z, Qt::ControlModifier);
            tap(focused(), Qt::Key_X, Qt::MetaModifier);
            check(take(ring).empty(), "a Ctrl or Meta chord played a note");
            key(focused(), QEvent::KeyPress, Qt::Key_Z);
            key(focused(), QEvent::KeyRelease, Qt::Key_Z, Qt::ControlModifier);
            check(sameList(take(ring), {{0x90, 48, 90}, {0x80, 48, 0}}),
                  "a note's release with Ctrl now held did not stop it");

            // A chord: three ons, three offs.
            key(focused(), QEvent::KeyPress, Qt::Key_Z);
            key(focused(), QEvent::KeyPress, Qt::Key_C);
            key(focused(), QEvent::KeyPress, Qt::Key_B);
            check(piano->isLit(48) && piano->isLit(52) && piano->isLit(55),
                  "a held chord's keys are not all lit");
            key(focused(), QEvent::KeyRelease, Qt::Key_C);
            key(focused(), QEvent::KeyRelease, Qt::Key_Z);
            key(focused(), QEvent::KeyRelease, Qt::Key_B);
            check(sameList(take(ring), {{0x90, 48, 90},
                                        {0x90, 52, 90},
                                        {0x90, 55, 90},
                                        {0x80, 52, 0},
                                        {0x80, 48, 0},
                                        {0x80, 55, 0}}),
                  "a three-key chord did not send three note ons and three note "
                  "offs");

            // Auto-repeat changes nothing: not a held key's, not a stray one's.
            key(focused(), QEvent::KeyPress, Qt::Key_Z);
            for (int i = 0; i < 3; i++) {
                key(focused(), QEvent::KeyRelease, Qt::Key_Z, Qt::NoModifier, true);
                key(focused(), QEvent::KeyPress, Qt::Key_Z, Qt::NoModifier, true);
            }
            key(focused(), QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, true);
            key(focused(), QEvent::KeyRelease, Qt::Key_X, Qt::NoModifier, true);
            check(sameList(take(ring), {{0x90, 48, 90}}) && piano->isLit(48),
                  "auto-repeat sent a note or cut the held one");
            key(focused(), QEvent::KeyRelease, Qt::Key_Z);
            check(sameList(take(ring), {{0x80, 48, 0}}),
                  "the release after auto-repeat did not stop it");
            // A second press of a held key without a release between (its
            // release went elsewhere) is the same holder: one release stops it.
            key(focused(), QEvent::KeyPress, Qt::Key_X);
            key(focused(), QEvent::KeyPress, Qt::Key_X);
            key(focused(), QEvent::KeyRelease, Qt::Key_X);
            check(sameList(take(ring), {{0x90, 50, 90}, {0x80, 50, 0}}) && window->holders(50) == 0,
                  "a second press of a held key stacked a holder (or retriggered it)");

            // Comma and q are one note: the first sounds it, the last stops it.
            key(focused(), QEvent::KeyPress, Qt::Key_Comma);
            key(focused(), QEvent::KeyPress, Qt::Key_Q);
            check(sameList(take(ring), {{0x90, 60, 90}}) && window->holders(60) == 2,
                  "comma then q retriggered the upper C (or did not count two "
                  "holders)");
            key(focused(), QEvent::KeyRelease, Qt::Key_Comma);
            check(take(ring).empty() && piano->isLit(60) && window->holders(60) == 1,
                  "releasing comma with q still down cut or unlit the upper C");
            key(focused(), QEvent::KeyRelease, Qt::Key_Q);
            check(sameList(take(ring), {{0x80, 60, 0}}) && !piano->isLit(60) &&
                      window->holders(60) == 0,
                  "releasing both comma and q did not send one note off");

            // The mouse and a typed key on the same note, both orders.
            press(piano, spot(piano, 60));
            key(focused(), QEvent::KeyPress, Qt::Key_Comma);
            release(piano, spot(piano, 60));
            check(sameList(take(ring), {{0x90, 60, 90}}) && piano->isLit(60),
                  "the mouse letting go of C4 cut it while comma holds it");
            key(focused(), QEvent::KeyRelease, Qt::Key_Comma);
            key(focused(), QEvent::KeyPress, Qt::Key_Q);
            press(piano, spot(piano, 60));
            key(focused(), QEvent::KeyRelease, Qt::Key_Q);
            check(sameList(take(ring), {{0x80, 60, 0}, {0x90, 60, 90}}) && piano->isLit(60) &&
                      piano->mouseKey() == 60,
                  "a typed release cut C4 while the mouse holds it (or a click "
                  "retriggered it)");
            release(piano, spot(piano, 60));
            check(sameList(take(ring), {{0x80, 60, 0}}) && !piano->isLit(60),
                  "the last of the mouse and a typed key did not stop C4");

            // - and =: an octave each, clamped like the buttons.
            tap(focused(), Qt::Key_Equal);
            check(window->base() == 60, "= did not shift up an octave");
            tap(focused(), Qt::Key_Minus);
            tap(focused(), Qt::Key_Minus);
            check(window->base() == 36, "- did not shift down an octave");
            key(focused(), QEvent::KeyPress, Qt::Key_Minus);
            key(focused(), QEvent::KeyPress, Qt::Key_Minus, Qt::NoModifier, true);
            key(focused(), QEvent::KeyRelease, Qt::Key_Minus);
            check(window->base() == 24, "a held - repeated its shift");
            window->setBase(0);
            tap(focused(), Qt::Key_Minus);
            check(window->base() == 0, "- went below C-1");
            window->setBase(108);
            tap(focused(), Qt::Key_Equal);
            check(window->base() == 108, "= went above C8");
            tap(focused(), Qt::Key_Plus, Qt::ShiftModifier); // Shift+=
            check(window->base() == 108, "Shift+= went above C8");
            check(take(ring).empty() && !claims(window, Qt::Key_Equal, Qt::ControlModifier),
                  "an octave key sent a note, or Ctrl+= was claimed");
            // D5 at C8: T is G9 (127); 6, Y and O are past it and inert.
            tap(focused(), Qt::Key_6);
            tap(focused(), Qt::Key_Y);
            tap(focused(), Qt::Key_O);
            tap(focused(), Qt::Key_T);
            check(sameList(take(ring), {{0x90, 127, 90}, {0x80, 127, 0}}),
                  "at C8, typing played a key past G9 or did not play G9");

            // Shifting with keys held lets every note go first, typed and the
            // mouse's; the keys still down stay silent until pressed again, and
            // so does the drag.
            window->setBase(48);
            take(ring);
            key(focused(), QEvent::KeyPress, Qt::Key_Z);
            press(piano, spot(piano, 64));
            tap(focused(), Qt::Key_Equal);
            check(window->base() == 60 &&
                      sameList(take(ring),
                               {{0x90, 48, 90}, {0x90, 64, 90}, {0x80, 64, 0}, {0x80, 48, 0}}) &&
                      window->holders(48) == 0 && window->holders(64) == 0 && !piano->isLit(48) &&
                      !piano->isLit(64) && piano->mouseKey() < 0,
                  "= with a typed and a mouse key held did not release both "
                  "before shifting");
            drag(piano, spot(piano, 67));
            key(focused(), QEvent::KeyRelease, Qt::Key_Z);
            release(piano, spot(piano, 67));
            check(take(ring).empty(),
                  "after a shift, a held key's release or the drag sounded a note");
            tap(focused(), Qt::Key_Z);
            check(sameList(take(ring), {{0x90, 60, 90}, {0x80, 60, 0}}),
                  "after a shift, a fresh press did not play the new octave's key");
            window->setBase(48);

            // D4 ShortcutOverride: the mapped keys and - / = are the window's
            // (Shift too); Space, Escape, Alt+B and chords pass through.
            check(claims(window, Qt::Key_S) && claims(window, Qt::Key_Comma) &&
                      claims(window, Qt::Key_Q) && claims(window, Qt::Key_0) &&
                      claims(window, Qt::Key_Minus) && claims(window, Qt::Key_Equal) &&
                      claims(window, Qt::Key_Z, Qt::ShiftModifier) &&
                      claims(window, Qt::Key_At, Qt::ShiftModifier),
                  "the window did not claim a mapped key's ShortcutOverride");
            check(!claims(window, Qt::Key_Space) && !claims(window, Qt::Key_Escape) &&
                      !claims(window, Qt::Key_B, Qt::AltModifier) &&
                      !claims(window, Qt::Key_Z, Qt::ControlModifier) &&
                      !claims(window, Qt::Key_S, Qt::ControlModifier) &&
                      !claims(window, Qt::Key_S, Qt::MetaModifier) && !claims(window, Qt::Key_A) &&
                      !claims(window, Qt::Key_P),
                  "the window claimed Space, Escape, Alt+B, a chord or an "
                  "unmapped key");
            take(ring);
            // End to end: a main-window action bound to S (a user's rebind of
            // Toggle Loop) stays quiet while S plays from the window; Space
            // still runs play/stop and Alt+B still closes the window.
            {
                keymap::Registry &keys = keymap::Registry::instance();
                keys.setBinding(QStringLiteral("transport.loop"),
                                QKeySequence(QStringLiteral("S")));
                int loops = 0;
                const auto loopSpy =
                    connect(m_loopAction, &QAction::triggered, this, [&loops] { loops++; });
                int plays = 0;
                const auto playSpy =
                    connect(m_playPauseAction, &QAction::triggered, this, [&plays] { plays++; });
                // The control: S from a main-window widget that takes no keys
                // does run it, so the rebind is live.
                key(statusBar(), QEvent::KeyPress, Qt::Key_S);
                key(statusBar(), QEvent::KeyRelease, Qt::Key_S);
                check(loops == 1, "control: S from the main window did not run "
                                  "the action bound to S");
                m_loopAction->trigger(); // back as it was
                loops = 0;
                key(focused(), QEvent::KeyPress, Qt::Key_S);
                key(focused(), QEvent::KeyRelease, Qt::Key_S);
                check(loops == 0, "S in the keyboard window ran the main window's "
                                  "action bound to S");
                check(sameList(take(ring), {{0x90, 49, 90}, {0x80, 49, 0}}),
                      "S in the keyboard window did not play C#3");
                tap(focused(), Qt::Key_Space);
                check(plays == 1, "Space in the keyboard window did not reach play/stop");
                if (m_audio.transport() == Transport::Playing)
                    stopPlayback();
                QApplication::processEvents();
                disconnect(loopSpy);
                disconnect(playSpy);
                keys.resetBinding(QStringLiteral("transport.loop"));
                tap(focused(), Qt::Key_B, Qt::AltModifier);
                QApplication::processEvents();
                check(!window->isVisible() && !action->isChecked(),
                      "Alt+B in the keyboard window did not close it");
                take(ring);
                action->setChecked(true); // shown and active again
                activateKeyboard();
            }

            // Deactivation lets everything go, the mouse key and its drag too,
            // and an inactive window plays nothing typed.
            if (check(window->isActiveWindow(), "the window is not active after Alt+B, View")) {
                key(window, QEvent::KeyPress, Qt::Key_Z);
                key(window, QEvent::KeyPress, Qt::Key_C);
                key(window, QEvent::KeyPress, Qt::Key_B);
                press(piano, spot(piano, 72));
                take(ring);
                activateWindow(); // the main window takes over (as Alt+Tab, a click)
                QApplication::processEvents();
                check(!window->isActiveWindow(), "the keyboard window stayed active");
                check(sameList(take(ring),
                               {{0x80, 72, 0}, {0x80, 48, 0}, {0x80, 52, 0}, {0x80, 55, 0}}),
                      "deactivating did not release the held chord and the mouse "
                      "key");
                check(!piano->isLit(48) && !piano->isLit(72) && piano->mouseKey() < 0 &&
                          window->holders(48) == 0 && window->holders(72) == 0,
                      "keys stayed lit or held after deactivating");
                drag(piano, spot(piano, 74));
                key(window, QEvent::KeyRelease, Qt::Key_Z);
                key(window, QEvent::KeyRelease, Qt::Key_C);
                // B's release goes to the window now active (not sent here).
                release(piano, spot(piano, 74));
                tap(window, Qt::Key_X);
                check(take(ring).empty(), "after deactivating, a release, the "
                                          "drag or a typed key sounded a note");
                // Each release path alone: ActivationChange to inactive, and a
                // WindowDeactivate or WindowBlocked event, with the mouse
                // holding a key (a click plays on an inactive window).
                press(piano, spot(piano, 60));
                QEvent activation(QEvent::ActivationChange);
                QApplication::sendEvent(window, &activation);
                check(sameList(take(ring), {{0x90, 60, 90}, {0x80, 60, 0}}) &&
                          piano->mouseKey() < 0,
                      "an ActivationChange to inactive did not release the mouse "
                      "key");
                release(piano, spot(piano, 60));
                press(piano, spot(piano, 60));
                QEvent deactivate(QEvent::WindowDeactivate);
                QApplication::sendEvent(window, &deactivate);
                check(sameList(take(ring), {{0x90, 60, 90}, {0x80, 60, 0}}),
                      "a WindowDeactivate did not release the mouse key");
                release(piano, spot(piano, 60));
                press(piano, spot(piano, 60));
                QEvent blocked(QEvent::WindowBlocked);
                QApplication::sendEvent(window, &blocked);
                check(sameList(take(ring), {{0x90, 60, 90}, {0x80, 60, 0}}),
                      "a WindowBlocked did not release the mouse key");
                release(piano, spot(piano, 60));
            }
            // Active, an ActivationChange (another window's) releases nothing.
            activateKeyboard();
            if (check(window->isActiveWindow(), "the window did not become active again")) {
                // B, held through the deactivation and let go elsewhere,
                // plays on its next press.
                tap(window, Qt::Key_B);
                check(sameList(take(ring), {{0x90, 55, 90}, {0x80, 55, 0}}),
                      "a key held through a deactivation stayed silent on its next press");
                key(window, QEvent::KeyPress, Qt::Key_Z);
                QEvent activation(QEvent::ActivationChange);
                QApplication::sendEvent(window, &activation);
                check(sameList(take(ring), {{0x90, 48, 90}}) && piano->isLit(48),
                      "an ActivationChange while active released a held key");
                // A modal dialog over it releases what it holds.
                press(piano, spot(piano, 64));
                take(ring);
                {
                    QDialog dialog(this);
                    dialog.setModal(true);
                    dialog.show();
                    QApplication::processEvents();
                    check(sameList(take(ring), {{0x80, 64, 0}, {0x80, 48, 0}}) &&
                              window->holders(48) == 0,
                          "a modal dialog opening did not release the held keys");
                    key(window, QEvent::KeyRelease, Qt::Key_Z);
                    release(piano, spot(piano, 64));
                    dialog.hide();
                }
                check(take(ring).empty(),
                      "a release after the modal dialog sent a second note off");
                activateKeyboard();
                // A menu popup (Alt+V from the window) keeps it active but
                // takes the keys, the releases too: what was held goes.
                check(QApplication::focusWidget() == window,
                      "the window is not the focus widget before the popup");
                key(focused(), QEvent::KeyPress, Qt::Key_Z);
                check(sameList(take(ring), {{0x90, 48, 90}}), "Z did not play before the popup");
                {
                    QMenu menu(this);
                    menu.addAction(QStringLiteral("Item"));
                    menu.popup(window->mapToGlobal(QPoint(0, 0)));
                    QApplication::processEvents();
                    check(menu.isVisible(), "the popup did not show");
                    key(&menu, QEvent::KeyRelease, Qt::Key_Z); // Z's release goes to the menu
                    menu.close();
                    QApplication::processEvents();
                }
                check(sameList(take(ring), {{0x80, 48, 0}}) && window->holders(48) == 0 &&
                          !piano->isLit(48),
                      "a menu popup with Z held did not release it exactly once");
                activateKeyboard();
                tap(focused(), Qt::Key_Z);
                check(sameList(take(ring), {{0x90, 48, 90}, {0x80, 48, 0}}),
                      "after the popup, Z did not play on its next press");
                // Hiding releases typed keys too.
                key(window, QEvent::KeyPress, Qt::Key_V);
                take(ring);
                action->setChecked(false);
                QApplication::processEvents();
                check(sameList(take(ring), {{0x80, 53, 0}}) && window->holders(53) == 0,
                      "hiding the window did not release a typed key");
                key(window, QEvent::KeyRelease, Qt::Key_V);
                action->setChecked(true);
                QApplication::processEvents();
                check(take(ring).empty(), "a typed release after a hide sent a second note off");
            }
        }
        window->setVelocity(VirtualKeyboardWindow::kDefaultVelocity);
    }
    take(ring);

    if (qEnvironmentVariableIntValue("PORYDAW_VKEY_CPU") == 1) {
        // D12: idle cost. The parked device would make the audio thread
        // idle too; release it, so the numbers are the app's real ones.
        if (m_audioOk)
            m_audio.holdDeviceForTest(false);
        action->setChecked(false);
        QApplication::processEvents();
        const double hidden = cpuSecondsOver(10000);
        action->setChecked(true);
        QApplication::processEvents();
        const double shown = cpuSecondsOver(10000);
        for (int key : {60, 64, 67})
            midi.injectForTest({0x90, uint8_t(key), 90}, hardware);
        press(piano, spot(piano, 72));
        QApplication::processEvents();
        const double chord = cpuSecondsOver(10000);
        release(piano, spot(piano, 72));
        for (int key : {60, 64, 67})
            midi.injectForTest({0x80, uint8_t(key), 0}, hardware);
        QApplication::processEvents();
        std::printf("vkeycheck: CPU over 10 s: window hidden %.3f s, shown idle %.3f s, shown "
                    "with a held chord %.3f s\n",
                    hidden, shown, chord);
        if (m_audioOk)
            m_audio.holdDeviceForTest(true);
    }

    midi.setOpenPortsForTest({});
    midi.setPortListForTest({});
    QSettings().remove(QStringLiteral("midi/surfacePorts"));
    QSettings().remove(QStringLiteral("midi/keyboardPorts"));
    // Left shown, with a key held: ~MainWindow must take the window down
    // before MidiInput goes (hand-off i; ASAN covers it).
    press(piano, spot(piano, 60));
    check(window->isVisible() && piano->mouseKey() >= 0, "the window is not left open");
    if (m_audioOk)
        m_audio.holdDeviceForTest(false);
    return g_failures - before;
}

int runVirtualKeyboardCheck(const QString &projectRoot, const QString &songLabel)
{
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "vkeycheck: no temp dir for settings\n");
        return 1;
    }
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QStandardPaths::setTestModeEnabled(true);
    {
        MainWindow window;
        window.show();
        window.runVirtualKeyboardCheck(projectRoot, songLabel);
    }
    if (g_failures == 0)
        std::printf("vkeycheck: OK\n");
    return g_failures ? 1 : 0;
}
