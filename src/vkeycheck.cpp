#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QRegion>
#include <QSettings>
#include <QSlider>
#include <QStandardPaths>
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
// line follows the selection and the edit cursor. The window is left open
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
