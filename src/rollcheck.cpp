#include "ui/theme/themeruntime.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QIcon>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QMenu>
#include <QPixmap>
#include <QPushButton>
#include <QPoint>
#include <QToolButton>
#include <QRect>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "core/songdocument.h"
#include "project/decompproject.h"
#include "rollcheckplayhead.h"
#include "ui/songview.h"

// --rollcheck <projectRoot> <song> [shot.png]: piano-roll gesture check.
// Drives the roll widget offscreen with synthesized mouse events: the
// double-click pencil draws at the default velocity (100 on a fresh
// document) and double-clicking an existing note deletes it, the
// Reaper-style latch makes the last clicked or
// velocity-dragged note's velocity the default for the next drawn note,
// and an edge resize snaps to the ruler's absolute grid even when the
// note's own edge sits off-grid. A right-drag band auditions each note
// as it first covers it (Ableton-style; the note's length is the ceiling),
// releases it when the band leaves it or the drag ends, and selects the
// covered notes on release. A plain left press on empty space auditions
// its row at the latched velocity (glissing across rows while held,
// released on mouse-up) and still parks the edit cursor on release; a
// press that grows into a draw does not re-attack the sounding key, and
// any horizontal travel at all grows it (no drag threshold). Holding the
// roll.velocity_drag modifier chord (Ctrl by default) turns a vertical
// drag from anywhere on a note into an Ableton-style velocity drag; a
// modifier click without the drag keeps Ctrl's selection toggle, resolved
// on release. The edge resize grips stay live under Ctrl, but a
// bulk-select click landing on one joins the note to the selection
// instead of replacing it, and the drag resizes the whole selection;
// a Ctrl+velocity drag on an unselected note likewise joins it and
// nudges the whole selection. Ctrl+arrows transpose (Shift: octave)
// and nudge the selection along the same absolute grid — both the roll's
// note selection and a multi-track time selection — and the view follows
// notes moved out of sight with a minimal scroll (flush at the edge, not
// re-centered). The playhead follow-scroll pauses while a mouse gesture
// is held (pan, drag, sweep) and resumes on release. Dragging a track
// header row reorders the tracks, the mute flag following the moved
// track through undo and redo; a right-button release cancels the drag,
// and a drop with a rename editor open commits the typed name first.
// Bare M and S toggle mute/solo over the multi-track scope (mixed state
// resolving toward on), the header buttons following without a rebuild
// and the undo stack untouched.
// The cursor over the roll marks its key row on the keyboard column
// (with a note-name chip) — held through gestures so a drag's target row
// stays readable — cleared when the cursor leaves the widget.
// Undoing every gesture must restore the original bytes.

namespace {

void sendMouse(QWidget *w, QEvent::Type type, QPoint pos,
                              Qt::MouseButton button, Qt::MouseButtons buttons,
                              Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QMouseEvent ev(type, QPointF(pos), QPointF(w->mapToGlobal(pos)), button,
                   buttons, mods);
    QCoreApplication::sendEvent(w, &ev);
}

void sendMouse(QWidget *w, QEvent::Type type, QPointF pos,
                              Qt::MouseButton button, Qt::MouseButtons buttons,
                              Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QMouseEvent ev(type, pos, QPointF(w->mapToGlobal(pos.toPoint())), button,
                                  buttons, mods);
    QCoreApplication::sendEvent(w, &ev);
}

void sendWheel(QWidget *w, QPointF pos, int angleDeltaY, int pixelDeltaY = 0,
               Qt::KeyboardModifiers mods = Qt::ControlModifier,
               int pixelDeltaX = 0) {
    QWheelEvent ev(pos, QPointF(w->mapToGlobal(pos.toPoint())),
                                  QPoint(pixelDeltaX, pixelDeltaY),
                                  QPoint(0, angleDeltaY), Qt::NoButton,
                                  mods, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(w, &ev);
}

// Test-side mirror of the roll's vertical projection. It intentionally
// samples the same independently-snapped half-open row boundaries without
// making the roll's private paint geometry part of SongView's public API.
struct SnappedRows {
    const SongView &view;
    const QWidget &roll;

    qreal dpr() const { return roll.devicePixelRatioF(); }
    qreal pixel() const { return 1.0 / dpr(); }
    qreal edge(int row) const {
        return std::round((row * view.keyHeight() - view.scrollY()) * dpr()) /
                      dpr();
    }
    qreal top(int key) const { return edge(127 - key); }
    qreal bottom(int key) const { return edge(128 - key); }
    int keyAt(qreal y) const {
        for (int row = 0; row < 128; ++row)
            if (y < edge(row + 1))
                return 127 - row;
        return 0;
    }
    int centerY(int key) const { return int(std::floor((top(key) + bottom(key)) / 2)); }
    QRectF noteRect(int x0, int x1, int key) const {
        return QRectF(x0, top(key) + pixel(), std::max(2, x1 - x0),
                                    std::max(2.0 * pixel(), bottom(key) - top(key) - pixel()));
    }
    QRectF noteBox(const QRectF &rect) const {
        return rect.adjusted(0, 0, 0, -pixel());
    }
    int noteTopProbeY(int key) const {
        return int(std::floor(noteRect(0, 1, key).top() + pixel()));
    }
};

void click(QWidget *w, QPoint pos) {
    sendMouse(w, QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton);
    sendMouse(w, QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton);
}

// The pencil gesture: Qt replaces a fast second press with a DblClick event,
// and the note commits on the release that follows.
void drawNote(QWidget *w, QPoint pos) {
    sendMouse(w, QEvent::MouseButtonDblClick, pos, Qt::LeftButton,
                        Qt::LeftButton);
    sendMouse(w, QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton);
}

void sendKey(QWidget *w, int key, Qt::KeyboardModifiers mods) {
    QKeyEvent press(QEvent::KeyPress, key, mods);
    QCoreApplication::sendEvent(w, &press);
    QKeyEvent release(QEvent::KeyRelease, key, mods);
    QCoreApplication::sendEvent(w, &release);
}

} // namespace

int runRollCheck(const QString &projectRoot, const QString &songLabel,
                                  const QString &screenshotPath) {
    // The roll consults keymap::Registry (Ctrl+arrow transposes, the
    // velocity-drag modifier chord), so redirect QSettings into a temp dir
    // first — a user's rebinds must not leak into the gesture assertions.
    QTemporaryDir settingsDir;
    if (settingsDir.isValid()) {
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                           settingsDir.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           settingsDir.path());
        // Registry is initialized during application startup; force the
        // harness's redirected format so it cannot consult a user's registry
        // override for a gesture binding.
        QSettings::setDefaultFormat(QSettings::IniFormat);
    }

    DecompProject project;
    QString error;
    if (!project.open(projectRoot, &error)) {
        std::fprintf(stderr, "rollcheck: %s\n", qUtf8Printable(error));
        return 1;
    }
    const SongInfo *info = nullptr;
    for (const SongInfo &song : project.songs()) {
        if (song.label == songLabel && song.isPlayable())
            info = &song;
    }
    if (!info) {
        std::fprintf(stderr, "rollcheck: no playable song %s\n",
                     qUtf8Printable(songLabel));
        return 1;
    }

    QElapsedTimer timer;
    timer.start();

    SongDocument doc;
    if (!doc.load(*info, &error)) {
        std::fprintf(stderr, "rollcheck: %s\n", qUtf8Printable(error));
        return 1;
    }
    const QByteArray baseline = doc.smf().write();

    auto timeline = doc.buildTimeline(48000.0);
    SongView view;
    if (view.internalWinId() != 0 || view.testAttribute(Qt::WA_NativeWindow)) {
        std::fprintf(stderr, "rollcheck: FAIL %s: SongView constructor forced native window creation\n",
                     qUtf8Printable(songLabel));
        return 1;
    }
    if (qEnvironmentVariableIsSet("PORYDAW_FORCE_UNCACHED_TIMELINE")) {
        // The diagnostic mode invalidates every paint-budget assertion below.
        std::fprintf(stderr,
                     "rollcheck: FAIL %s: unset PORYDAW_FORCE_UNCACHED_TIMELINE "
                     "(diagnostic mode breaks the cache paint budgets)\n",
                     qUtf8Printable(songLabel));
        return 1;
    }
    view.resize(1280, 800);
    view.setSong(timeline.get(), nullptr);
    view.setDocument(&doc);
    // The app rebuilds the timeline after every edit
    // (MainWindow::onDocumentChanged); the roll hit-tests against the view
    // model, so the check must keep it fresh the same way.
    QObject::connect(&doc, &SongDocument::documentChanged, &view, [&] {
        auto rebuilt = doc.buildTimeline(48000.0);
        view.updateSong(rebuilt.get());
        timeline = std::move(rebuilt); // frees the old one after the swap
    });

    // The zoom-adaptive default grid is a 16th note — an 8px cell, too tight
    // for clean center/handle clicks. Floor the grid at quarter notes (the
    // ruler's own control) so cells are a comfortable 32px. Snapping runs
    // one ladder step finer than the drawn grid (the floor is display-only),
    // so the snap grid here is half-beats — 16px snap cells.
    view.setGridMinDenom(4);
    (void)view.grab(); // force layout so child geometry is real

    int failures = 0;
    auto fail = [&](const char *what) {
        std::fprintf(stderr, "rollcheck: FAIL %s: %s\n", qUtf8Printable(songLabel),
                     what);
        failures++;
    };

    auto *roll = view.findChild<QWidget *>(QStringLiteral("pianoRoll"));
    if (!roll || roll->width() <= songview::kKeyboardW || roll->height() <= 0) {
        fail("piano roll not found or not laid out");
        return 1;
    }
    const int track = view.selectedTrack();
    if (doc.engineTrackCount() <= track) {
        fail("no engine track to draw on");
        return 1;
    }

    // The Y camera is continuous: partial wheel deltas are immediately
    // multiplicative, preserve the cursor's content row, and remain precise
    // through the integer-native scrollbar projection.
    {
        const SongView::ViewState original = view.viewState();
        SongView::ViewState zoom = original;
        zoom.keyHeight = 8.0;
        zoom.scrollY = 300.0;
        view.applyViewState(zoom);
        const QPointF anchor(songview::kKeyboardW + 40.0, 200.0);

        for (int i = 0; i < 4; ++i)
            sendWheel(roll, anchor, 30);
        const double partialHeight = view.keyHeight();
        const double partialScroll = view.scrollY();

        view.applyViewState(zoom);
        sendWheel(roll, anchor, 120);
        if (std::abs(view.keyHeight() - partialHeight) > 1e-12 ||
                std::abs(view.scrollY() - partialScroll) > 1e-10)
            fail("four partial Ctrl-wheel deltas differ from one full notch");

        view.applyViewState(zoom);
        const double anchoredRow = (anchor.y() + view.scrollY()) / view.keyHeight();
        sendWheel(roll, anchor, 30);
        if (std::abs((anchor.y() + view.scrollY()) / view.keyHeight()
                                  - anchoredRow) > 1e-12)
            fail("Ctrl-wheel zoom moved the cursor's content row");

        view.applyViewState(zoom);
        for (int i = 0; i < 10; ++i)
            sendWheel(roll, anchor, 120);
        if (std::abs(view.keyHeight() - 16.0) > 1e-12)
            fail("ten Ctrl-wheel notches did not double key height");

        view.applyViewState(zoom);
        sendWheel(roll, anchor, 0, 240);
        if (std::abs(view.keyHeight() - 16.0) > 1e-12)
            fail("240-pixel Ctrl-wheel zoom did not double key height");


        view.applyViewState(zoom);
        const double keyboardScroll = view.scrollY();
        sendWheel(roll, QPointF(songview::kKeyboardW - 1.0, anchor.y()),
                  0, 1, Qt::NoModifier);
        if (std::abs(view.scrollY() - (keyboardScroll - 0.5)) > 1e-12)
            fail("pixel-only wheel over keyboard did not scroll note range");
        view.applyViewState(zoom);
        for (int i = 0; i < 4; ++i)
            sendWheel(roll, anchor, 30);
        for (int i = 0; i < 4; ++i)
            sendWheel(roll, anchor, -30);
        if (std::abs(view.keyHeight() - zoom.keyHeight) > 1e-12 ||
                std::abs(view.scrollY() - zoom.scrollY) > 1e-10)
            fail("equal Ctrl-wheel zoom in/out did not restore the camera");

        zoom.keyHeight = 9.375;
        zoom.scrollY = 257.625;
        view.applyViewState(zoom);
        const SongView::ViewState fractional = view.viewState();
        if (std::abs(fractional.keyHeight - zoom.keyHeight) > 1e-12 ||
                std::abs(fractional.scrollY - zoom.scrollY) > 1e-12)
            fail("fractional vertical view state did not round-trip");

        const int boundaryRow = 40;
        const qreal dpr = roll->devicePixelRatioF();
        const qreal boundary = std::round((boundaryRow * view.keyHeight()
                                                                              - view.scrollY()) * dpr) / dpr;
        sendMouse(roll, QEvent::MouseMove,
                            QPointF(songview::kKeyboardW + 40.0, boundary - 0.25),
                            Qt::NoButton, Qt::NoButton);
        if (roll->property("hoverKey").toInt() != 128 - boundaryRow)
            fail("hovering above a snapped pitch boundary chose the wrong key");
        sendMouse(roll, QEvent::MouseMove,
                            QPointF(songview::kKeyboardW + 40.0, boundary + 0.25),
                            Qt::NoButton, Qt::NoButton);
        if (roll->property("hoverKey").toInt() != 127 - boundaryRow)
            fail("hovering below a snapped pitch boundary chose the wrong key");

        // Integer-valued legacy vertical state still applies unchanged after the
        // type migration.
        zoom.keyHeight = 11;
        zoom.scrollY = 217;
        view.applyViewState(zoom);
        const SongView::ViewState legacy = view.viewState();
        if (legacy.keyHeight != 11.0 || legacy.scrollY != 217.0)
            fail("legacy integer vertical view state no longer applies");
        view.applyViewState(original);
        (void)view.grab(); // consume the restoration repaint before later probes
        QCoreApplication::processEvents();
    }

    // The X camera follows the same continuous contract as the Y camera:
    // wheel deltas compose, the exact qreal cursor anchor stays pinned, and
    // the integer scrollbar is only a projection of the fractional camera.
    {
        const SongView::ViewState original = view.viewState();
        SongView::ViewState zoom = original;
        zoom.pxPerBeat = 500.125;
        zoom.scrollPx = 23.625;
        const QPointF anchor(songview::kKeyboardW + 73.375, 200.0);
        const qreal anchorContentX = anchor.x() - songview::kKeyboardW;

        view.applyViewState(zoom);
        for (int i = 0; i < 4; ++i)
            sendWheel(roll, anchor, 30, 0, Qt::NoModifier);
        const double partialScale = view.pxPerBeat();
        const double partialScroll = view.viewState().scrollPx;

        view.applyViewState(zoom);
        sendWheel(roll, anchor, 120, 0, Qt::NoModifier);
        const double fullScale = view.pxPerBeat();
        const double fullScroll = view.viewState().scrollPx;
        const double expectedFullScale =
            zoom.pxPerBeat * std::pow(1.0015, 120.0);
        if (std::abs(fullScale - expectedFullScale) > 1e-10)
            fail("timeline-wheel notch changed horizontal zoom sensitivity");
        if (std::abs(fullScale - partialScale) > 1e-12 ||
                std::abs(fullScroll - partialScroll) > 1e-9)
            fail("four partial timeline-wheel deltas differ from one full notch");

        view.applyViewState(zoom);
        sendWheel(roll, anchor, 0, 24, Qt::NoModifier);
        if (std::abs(view.pxPerBeat() - fullScale) > 1e-12 ||
                std::abs(view.viewState().scrollPx - fullScroll) > 1e-9)
            fail("timeline pixel-wheel delta was not consumed continuously");


        view.applyViewState(zoom);
        const double horizontalScroll = view.viewState().scrollPx;
        const double horizontalScale = view.pxPerBeat();
        sendWheel(roll, anchor, 0, 0, Qt::NoModifier, 8);
        if (std::abs(view.viewState().scrollPx - (horizontalScroll - 8.0)) > 1e-12 ||
                std::abs(view.pxPerBeat() - horizontalScale) > 1e-12)
            fail("pixel-only horizontal wheel did not scroll timeline");
        view.applyViewState(zoom);
        const double anchoredTick = view.tickAtContentX(anchorContentX);
        sendWheel(roll, anchor, 30, 0, Qt::NoModifier);
        if (std::abs(view.tickAtContentX(anchorContentX) - anchoredTick) > 1e-9)
            fail("timeline-wheel zoom moved the cursor's fractional anchor tick");

        view.applyViewState(zoom);
        for (int i = 0; i < 4; ++i)
            sendWheel(roll, anchor, 30, 0, Qt::NoModifier);
        for (int i = 0; i < 4; ++i)
            sendWheel(roll, anchor, -30, 0, Qt::NoModifier);
        if (std::abs(view.pxPerBeat() - zoom.pxPerBeat) > 1e-10 ||
                std::abs(view.viewState().scrollPx - zoom.scrollPx) > 1e-9)
            fail("equal timeline-wheel zoom in/out did not restore the camera");

        zoom.pxPerBeat = 311.375;
        zoom.scrollPx = 47.625;
        view.applyViewState(zoom);
        const SongView::ViewState fractional = view.viewState();
        if (std::abs(fractional.pxPerBeat - zoom.pxPerBeat) > 1e-12 ||
                std::abs(fractional.scrollPx - zoom.scrollPx) > 1e-12)
            fail("fractional horizontal view state did not round-trip");

        // Integer-valued legacy horizontal state remains a valid sidecar value
        // after scrollPx becomes fractional.
        zoom.pxPerBeat = 320.0;
        zoom.scrollPx = 17.0;
        view.applyViewState(zoom);
        const SongView::ViewState legacy = view.viewState();
        if (legacy.pxPerBeat != 320.0 || legacy.scrollPx != 17.0)
            fail("legacy integer horizontal view state no longer applies");

        view.applyViewState(original);
        (void)view.grab();
        QCoreApplication::processEvents();
    }

    // Exact tick geometry rounds once, after adding the destination widget's
    // origin. Its affine inverse must still snap every visible snap-grid tick
    // back to itself at both one- and two-device-pixel scaling.
    {
        const SongView::ViewState original = view.viewState();
        const QSize originalSize = view.size();
        view.resize(180, originalSize.height());
        (void)view.grab();
        QCoreApplication::processEvents();

        struct CameraProbe {
            double pxPerBeat;
            double scrollPx;
        };
        const CameraProbe probes[] = {
            {4.125, 0.375},
            {37.375, 13.625},
            {512.5, 71.3125},
        };
        const qreal origins[] = {
            qreal(songview::kKeyboardW),
            qreal(songview::kGutterW) + 0.25,
        };
        const qreal dprs[] = {1.0, 2.0};

        for (const CameraProbe &probe : probes) {
            SongView::ViewState state = original;
            state.pxPerBeat = probe.pxPerBeat;
            state.scrollPx = probe.scrollPx;
            state.gridMinDenom = 0;
            view.applyViewState(state);
            const SongView::ViewState applied = view.viewState();
            if (std::abs(applied.pxPerBeat - probe.pxPerBeat) > 1e-12 ||
                    std::abs(applied.scrollPx - probe.scrollPx) > 1e-12)
                fail("fractional projection camera did not apply exactly");

            const qreal visibleWidth =
                qreal(roll->width() - songview::kKeyboardW);
            uint64_t tick =
                view.snapTickUp(std::max(0.0, view.tickAtContentX(0.0)));
            int visibleTicks = 0;
            bool mappingFailed = false;
            const double affineTick =
                view.tickAtContentX(visibleWidth * 0.371) + 0.375;
            if (std::abs(view.tickAtContentX(view.contentX(affineTick))
                         - affineTick) > 1e-9)
                fail("raw horizontal projection lost fractional tick precision");
            for (int guard = 0; guard < 10000; ++guard) {
                const qreal rawX = view.contentX(double(tick));
                if (rawX > visibleWidth)
                    break;
                if (rawX >= 0.0) {
                    visibleTicks++;
                    for (qreal origin : origins) {
                        for (qreal dpr : dprs) {
                            const qreal displayed =
                                view.displayX(double(tick), origin, dpr);
                            const qreal expected = std::round(
                                (origin + rawX) * dpr) / dpr;
                            if (std::abs(displayed - expected) > 1e-12)
                                mappingFailed = true;
                            const uint64_t roundTrip = view.snapTick(
                                view.tickAtContentX(displayed - origin));
                            if (roundTrip != tick)
                                mappingFailed = true;
                        }
                    }
                }
                const uint64_t next =
                    view.snapTickUp(double(tick) + 1.0);
                if (next <= tick) {
                    mappingFailed = true;
                    break;
                }
                tick = next;
            }
            if (visibleTicks < 2)
                fail("fractional projection camera exposed too few snap ticks");
            if (mappingFailed)
                fail("display/inverse projection changed a visible snap-grid tick");
        }

        view.resize(originalSize);
        (void)view.grab();
        view.applyViewState(original);
        QCoreApplication::processEvents();
    }
    const SnappedRows rows{view, *roll};

    // Hover readout: the cursor anywhere over the roll marks its key row
    // on the keyboard column (mirrored in the hoverKey property); leaving
    // the widget clears the mark.
    {
        const int y = roll->height() / 2;
        const int expected = rows.keyAt(y);
        sendMouse(roll, QEvent::MouseMove, QPoint(songview::kKeyboardW + 40, y),
                            Qt::NoButton, Qt::NoButton);
        if (roll->property("hoverKey").toInt() != expected)
            fail("hovering the notes area did not mark its key row");
        sendMouse(roll, QEvent::MouseMove, QPoint(4, rows.centerY(expected - 1)), Qt::NoButton,
                  Qt::NoButton);
        if (roll->property("hoverKey").toInt() != expected - 1)
            fail("hovering the keyboard column did not follow the key row");
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(roll, &leave);
        if (roll->property("hoverKey").toInt() != -1)
            fail("leaving the roll did not clear the hover mark");
    }

    // A row/cell is taken if a note of the selected track sits within one
    // cell of it (the roll's hit test pads note rects by 2px; a full cell of
    // clearance keeps the check's clicks unambiguous).
    auto occupied = [&](uint64_t tick, uint64_t dur, int key,
                        bool checkAllTracks = false) {
      const int startTrack = checkAllTracks ? 0 : track;
      const int endTrack = checkAllTracks ? doc.engineTrackCount() : track + 1;
      for (int t = startTrack; t < endTrack; ++t) {
        for (const DocNote &note : doc.notesForTrack(t)) {
          if (int(note.key) != key)
            continue;
          const uint64_t end = note.unterminated()
                                   ? UINT64_MAX
                                   : note.tick + note.duration + dur;
          if (note.tick < tick + 2 * dur && end > tick)
            return true;
        }
      }
      return false;
    };

    // A free grid cell with a click target at mid-cell x, mid-row y (the
    // Move zone).
    struct Cell {
      uint64_t tick = 0, dur = 0;
      int key = -1;
      QPoint center;
    };
    auto findFreeCell = [&](int firstProbe = 8,
                            bool checkAllTracks = false) -> Cell {
      Cell cell;
      for (int key = 115; key >= 24; key--) {
            const qreal top = rows.top(key);
            const qreal bottom = rows.bottom(key);
            if (top < 0 || bottom > roll->height())
          continue;
        for (int probe = firstProbe;
             probe < roll->width() - songview::kKeyboardW - 40; probe += 24) {
          const uint64_t tick = view.snapTickDown(view.tickAtContentX(probe));
          const uint64_t dur = view.gridTicksAt(tick);
          const int x0 = songview::kKeyboardW + view.contentX(double(tick));
          const int x1 =
              songview::kKeyboardW + view.contentX(double(tick + dur));
          const int xs = songview::kKeyboardW +
                         view.contentX(double(tick + view.snapTicksAt(tick)));
          // Wide enough that the click target clears the 3px resize
          // edges once a note fills the cell.
          if (x0 < songview::kKeyboardW || x1 - x0 < 12 || xs - x0 < 8 ||
              x1 >= roll->width())
            continue;
          if (occupied(tick, dur, key, checkAllTracks))
            continue;
          cell.tick = tick;
          cell.dur = dur;
          cell.key = key;
          // Mid snap-cell, not mid drawn-cell: snapping is finer than
          // the drawn grid, so a draw at the cell's visual center
          // would anchor at the snap line there, not at cell.tick.
                cell.center = QPoint((x0 + xs) / 2, rows.centerY(key));
          return cell;
        }
      }
      return cell;
    };

    // Regression for the complete paint-to-edit path: use the physical-pixel
    // centers of two adjacent displayed snap boundaries, then require the
    // document note to start at that displayed cell rather than a neighbor.
    {
        const SongView::ViewState original = view.viewState();
        const QSize originalSize = view.size();
        view.resize(180, originalSize.height());
        (void)view.grab();
        QCoreApplication::processEvents();

        SongView::ViewState fractional = original;
        fractional.pxPerBeat = 31.375;
        fractional.scrollPx = 0.625;
        view.applyViewState(fractional);
        const SongView::ViewState applied = view.viewState();
        if (std::abs(applied.pxPerBeat - fractional.pxPerBeat) > 1e-12 ||
                std::abs(applied.scrollPx - fractional.scrollPx) > 1e-12)
            fail("fractional edit camera did not apply exactly");

        struct FractionalEditProbe {
            uint64_t tick = 0;
            uint64_t previous = 0;
            uint64_t next = 0;
            int key = -1;
            QPointF center;
        } probe;
        const qreal origin = qreal(songview::kKeyboardW);
        const qreal dpr = roll->devicePixelRatioF();
        const qreal rightLimit = qreal(roll->width()) - 4.0;

        for (int key = 115; key >= 24 && probe.key < 0; --key) {
            const qreal top = rows.top(key);
            const qreal bottom = rows.bottom(key);
            if (top < 0.0 || bottom > roll->height())
                continue;
            uint64_t tick =
                view.snapTickUp(std::max(0.0, view.tickAtContentX(4.0)));
            for (int guard = 0; guard < 1000; ++guard) {
                const uint64_t next =
                    view.snapTickUp(double(tick) + 1.0);
                if (next <= tick)
                    break;
                const qreal leftX = view.displayX(double(tick), origin, dpr);
                const qreal rightX = view.displayX(double(next), origin, dpr);
                if (leftX > rightLimit)
                    break;
                const uint64_t dur = view.gridTicksAt(tick);
                const uint64_t previous =
                    tick == 0 ? tick : view.snapTickDown(double(tick) - 1.0);
                if (leftX >= origin + 4.0 && rightX <= rightLimit
                        && rightX - leftX >= 4.0
                        && !occupied(tick, dur, key)) {
                    const qreal centerX = (leftX + rightX) / 2.0;
                    if (std::abs(centerX - std::round(centerX)) < 1e-12) {
                        tick = next;
                        continue;
                    }
                    const QPointF center(centerX, (top + bottom) / 2.0);
                    if (view.snapTickDown(
                            view.tickAtContentX(center.x() - origin)) == tick) {
                        probe.tick = tick;
                        probe.previous = previous;
                        probe.next = next;
                        probe.key = key;
                        probe.center = center;
                        break;
                    }
                }
                tick = next;
            }
        }

        if (probe.key < 0) {
            fail("no empty fractional displayed cell for edit regression");
        } else {
            const QByteArray beforeProbe = doc.smf().write();
            const int undoIndex = doc.undoStack()->index();
            sendMouse(roll, QEvent::MouseButtonDblClick, probe.center,
                      Qt::LeftButton, Qt::LeftButton);
            sendMouse(roll, QEvent::MouseButtonRelease, probe.center,
                      Qt::LeftButton, Qt::NoButton);

            DocNote exact;
            if (!doc.findNote(track, probe.tick, uint8_t(probe.key), &exact))
                fail("fractional displayed-cell edit saved at the wrong tick");
            DocNote neighbor;
            const bool atPrevious = probe.previous != probe.tick
                && doc.findNote(track, probe.previous, uint8_t(probe.key), &neighbor);
            const bool atNext =
                doc.findNote(track, probe.next, uint8_t(probe.key), &neighbor);
            if (atPrevious || atNext)
                fail("fractional displayed-cell edit saved in a neighboring cell");

            if (doc.undoStack()->index() <= undoIndex)
                fail("fractional displayed-cell edit pushed no undo command");
            while (doc.undoStack()->index() > undoIndex
                   && doc.undoStack()->canUndo())
                doc.undoStack()->undo();

            DocNote residue;
            const bool exactResidue =
                doc.findNote(track, probe.tick, uint8_t(probe.key), &residue);
            const bool previousResidue = probe.previous != probe.tick
                && doc.findNote(track, probe.previous, uint8_t(probe.key), &residue);
            const bool nextResidue =
                doc.findNote(track, probe.next, uint8_t(probe.key), &residue);
            if (exactResidue || previousResidue || nextResidue)
                fail("undo left the fractional displayed-cell probe in the document");
            if (doc.undoStack()->index() != undoIndex
                    || doc.smf().write() != beforeProbe
                    || view.document() != &doc || !view.timeline())
                fail("fractional displayed-cell probe did not restore document state");
        }

        view.resize(originalSize);
        (void)view.grab();
        view.applyViewState(original);
        QCoreApplication::processEvents();
    }

    // Baseline: the pencil draws at velocity 100 on a fresh document.
    const Cell a = findFreeCell(40, true);
    if (a.key < 0) {
      fail("no free grid cell to draw in");
      return 1;
    }
    // Keep timeline overlays away from the note border under test.
    const uint64_t overlayTick = a.tick + 3 * a.dur;
    view.setPlayheadSample(timeline->sampleForTick(overlayTick), false);
    view.setEditCursorTick(overlayTick);
    const QPixmap rollBeforePixmap = roll->grab();
    const QImage rollBeforeDrawing = rollBeforePixmap.toImage();
    const qreal rasterDpr = rollBeforePixmap.devicePixelRatio();
    const auto toRasterPixel = [rasterDpr](qreal position) {
      return qRound(position * rasterDpr);
    };
    drawNote(roll, a.center);
    DocNote noteA;
    if (!doc.findNote(track, a.tick, uint8_t(a.key), &noteA)) {
      fail("pencil draw produced no note");
      return failures;
    }
    if (noteA.velocity != 100)
      fail("fresh document does not draw at velocity 100");

    // The painted box runs flush to the note's right interaction edge
    // (consecutive notes abut with no phantom rest column) but stops one
    // pixel above the bottom edge, whose reserved row must retain the
    // underlying roll. Nothing may paint past the end tick's column.
    view.setEditCursorTick(overlayTick);
    const QPixmap rollAfterPixmap = roll->grab();
    const QImage rollAfterDrawing = rollAfterPixmap.toImage();
    const int noteLeftX =
        songview::kKeyboardW + view.contentX(double(noteA.tick));
    const int noteRightX = songview::kKeyboardW
        + view.contentX(double(noteA.tick + noteA.duration));
    const QRectF noteFrame =
        rows.noteRect(noteLeftX, noteRightX, noteA.key);
    const QRectF paintedNoteBox = rows.noteBox(noteFrame);
    const int noteLeftPixel = toRasterPixel(noteFrame.left());
    const int noteRightPixel = toRasterPixel(noteFrame.right());
    const int noteTopPixel = toRasterPixel(noteFrame.top());
    const int noteFrameBottomPixel = toRasterPixel(noteFrame.bottom());
    const int paintedNoteBottomPixel =
        toRasterPixel(paintedNoteBox.bottom());
    bool paintEscapedInteractionRect = false;
    for (int y = noteTopPixel; y < noteFrameBottomPixel; ++y) {
      paintEscapedInteractionRect |=
          rollAfterDrawing.pixel(noteRightPixel, y)
          != rollBeforeDrawing.pixel(noteRightPixel, y);
    }
    for (int x = noteLeftPixel; x < noteRightPixel; ++x) {
      paintEscapedInteractionRect |=
          rollAfterDrawing.pixel(x, paintedNoteBottomPixel)
          != rollBeforeDrawing.pixel(x, paintedNoteBottomPixel);
    }
    if (paintEscapedInteractionRect)
      fail("note color escaped past its black box");

    const QRectF twoPixelBarNoteRect(
        noteFrame.left(), noteFrame.top(), noteFrame.width(),
        20 * rows.pixel());
    const QRectF twoPixelBarNoteBox = rows.noteBox(twoPixelBarNoteRect);
    const QRectF velocityZeroBar =
        songview::velBarRect(twoPixelBarNoteRect, 0, rows.dpr());
    if (qRound(velocityZeroBar.height() / rows.pixel()) != 2
        || velocityZeroBar.left() < twoPixelBarNoteBox.left()
        || velocityZeroBar.right() > twoPixelBarNoteBox.right()
        || velocityZeroBar.top() < twoPixelBarNoteBox.top()
        || velocityZeroBar.bottom() > twoPixelBarNoteBox.bottom())
      fail("two-pixel velocity-zero bar escaped painted note box");
    // Timeline overlays are composited above notes and can tint frame colors
    // by a few channel values.
    const auto isBlackBorder = [](QRgb pixel) {
      return qRed(pixel) <= 16 && qGreen(pixel) <= 16 && qBlue(pixel) <= 16;
    };
    const QColor selectionRingColor =
        themes::color(themes::Role::item_selected_background);
    const auto isSelectionRingColor = [selectionRingColor](QRgb pixel) {
      const QColor actualColor(pixel);
      return std::abs(actualColor.red() - selectionRingColor.red()) <= 16
          && std::abs(actualColor.green() - selectionRingColor.green()) <= 16
          && std::abs(actualColor.blue() - selectionRingColor.blue()) <= 16;
    };

    const QColor velocityZeroColor = SongView::noteColor(track, 0);
    const QColor velocityMaximumColor = SongView::noteColor(track, 127);
    const QColor velocityMidpointColor = SongView::noteColor(track, 64);
    const QColor velocityZeroThemeColor =
        themes::color(themes::Role::song_view_note_velocity_zero);
    const QColor trackIdentityColor = SongView::trackColor(track);

    if (velocityZeroColor != velocityZeroThemeColor)
      fail("velocity 0 note color does not equal theme neutral");
    if (velocityZeroColor.alpha() != 255)
      fail("velocity 0 note color is not opaque");
    if (velocityMaximumColor != trackIdentityColor)
      fail("velocity 127 note color does not equal track color");
    if (velocityMaximumColor.alpha() != 255)
      fail("velocity 127 note color is not opaque");
    if (velocityMidpointColor.alpha() != 255)
      fail("intermediate velocity note color is not opaque");
    if (velocityMidpointColor == velocityZeroColor
        || velocityMidpointColor == velocityMaximumColor)
      fail("intermediate velocity note color equals endpoint color");

    const QColor expectedNoteColor = SongView::noteColor(track, 100);
    const QPoint noteInteriorSample(
        toRasterPixel(paintedNoteBox.center().x()),
        toRasterPixel(paintedNoteBox.center().y()));
    if (QColor(rollAfterDrawing.pixel(noteInteriorSample))
        != expectedNoteColor)
      fail("note interior color does not match noteColor(track, 100)");

    // A note ending exactly where the next begins must paint every column
    // across the pair — no reserved background column that reads as a rest
    // between them. (findFreeCell guaranteed the adjacent cell is empty.)
    doc.addNote(track, noteA.tick + noteA.duration, noteA.key,
                noteA.duration, 100);
    const int abuttingRightX = songview::kKeyboardW
        + view.contentX(double(noteA.tick + 2 * noteA.duration));
    const QImage abuttingImage = roll->grab().toImage();
    const int abuttingMidY = toRasterPixel(rows.centerY(noteA.key));
    const int abuttingRightPixel = toRasterPixel(abuttingRightX);
    bool restGapFound = false;
    for (int x = noteLeftPixel; x < abuttingRightPixel; ++x) {
      restGapFound |= abuttingImage.pixel(x, abuttingMidY)
          == rollBeforeDrawing.pixel(x, abuttingMidY);
    }
    if (restGapFound)
      fail("abutting notes left an unpainted rest-like gap column");

    // At a key height where only ~3 face pixels remain, the border thins to
    // one pixel instead of vanishing while neighboring larger notes keep
    // theirs.
    {
      const SongView::ViewState originalView = view.viewState();
      SongView::ViewState tinyView = originalView;
      tinyView.keyHeight = 5.0;
      tinyView.scrollY =
          std::max(0.0, (127.5 - double(noteA.key)) * tinyView.keyHeight
                            - roll->height() / 2.0);
      view.applyViewState(tinyView);
      const SnappedRows tinyRows{view, *roll};
      const QRectF tinyBox = tinyRows.noteBox(
          tinyRows.noteRect(noteRightX, abuttingRightX, noteA.key));
      QImage tinyImage(roll->size(), QImage::Format_ARGB32_Premultiplied);
      tinyImage.fill(Qt::transparent);
      roll->render(&tinyImage);
      const int tinyCenterX = qRound(tinyBox.center().x());
      if (!isBlackBorder(
              tinyImage.pixel(tinyCenterX, qRound(tinyBox.top()))))
        fail("tiny note lost its border instead of thinning it");
      if (isBlackBorder(
              tinyImage.pixel(tinyCenterX, qRound(tinyBox.top()) + 1)))
        fail("tiny note border swallowed the note face");
      view.applyViewState(originalView);
    }


    // Probe the selected 3px ring, its 2px black inset, and the unselected
    // bottom edge with the camera centered at a fractional scale.
    {
      const SongView::ViewState originalView = view.viewState();
      SongView::ViewState fractionalView = originalView;
      fractionalView.keyHeight = 16.375;
      fractionalView.scrollY =
          std::max(0.0,
                   (127.5 - double(noteA.key)) * fractionalView.keyHeight
                       - roll->height() / 2.0);
      view.applyViewState(fractionalView);

      const SnappedRows fractionalRows{view, *roll};
      const QRectF fractionalNoteBox = fractionalRows.noteBox(
          fractionalRows.noteRect(noteLeftX, noteRightX, noteA.key));
      const QPixmap selectedNotePixmap = roll->grab();
      const QImage selectedNoteImage = selectedNotePixmap.toImage();
      const qreal devicePixelRatio = selectedNotePixmap.devicePixelRatio();
      const auto toPhysicalPixel = [devicePixelRatio](qreal position) {
        return qRound(position * devicePixelRatio);
      };
      const int leftPixel = toPhysicalPixel(fractionalNoteBox.left());
      const int rightPixel = toPhysicalPixel(fractionalNoteBox.right());
      const int topPixel = toPhysicalPixel(fractionalNoteBox.top());
      const int bottomPixel = toPhysicalPixel(fractionalNoteBox.bottom());
      const int centerPixelX = toPhysicalPixel(fractionalNoteBox.center().x());
      const int centerPixelY = toPhysicalPixel(fractionalNoteBox.center().y());
      // Frame weights scale with the display ratio (1-DIP border, 1.5-DIP
      // ring) — assert exactly the pixel counts the paint code derives.
      const int ringPixels = songview::selectionRingPixels(devicePixelRatio);
      const int borderPixels = songview::noteBorderPixels(devicePixelRatio);
      for (int ringPixel = 0; ringPixel < ringPixels; ++ringPixel) {
        if (!isSelectionRingColor(
                selectedNoteImage.pixel(centerPixelX,
                                        topPixel + ringPixel))
            || !isSelectionRingColor(
                selectedNoteImage.pixel(centerPixelX,
                                        bottomPixel - 1 - ringPixel))) {
          fail("selected note frame is not a contiguous selection ring");
        }
      }
      for (int borderPixel = 0; borderPixel < borderPixels; ++borderPixel) {
        if (!isBlackBorder(selectedNoteImage.pixel(
                centerPixelX, topPixel + ringPixels + borderPixel)))
          fail("selected note did not have an inset black top border");
        if (!isBlackBorder(selectedNoteImage.pixel(
                centerPixelX,
                bottomPixel - 1 - ringPixels - borderPixel)))
          fail("selected note did not have an inset black bottom border");
        if (!isBlackBorder(selectedNoteImage.pixel(
                leftPixel + ringPixels + borderPixel, centerPixelY)))
          fail("selected note did not have an inset black left border");
        if (!isBlackBorder(selectedNoteImage.pixel(
                rightPixel - 1 - ringPixels - borderPixel, centerPixelY)))
          fail("selected note did not have an inset black right border");
      }
      // The ring must stop where the black border starts.
      if (isSelectionRingColor(selectedNoteImage.pixel(
              centerPixelX, topPixel + ringPixels)))
        fail("selection ring is thicker than its display-scaled weight");

      view.clearSelection();
      const QImage unselectedNoteImage = roll->grab().toImage();
      for (int borderPixel = 0; borderPixel < borderPixels; ++borderPixel) {
        if (!isBlackBorder(unselectedNoteImage.pixel(
                centerPixelX, bottomPixel - 1 - borderPixel)))
          fail("unselected note lacks its black bottom border");
      }
      if (QColor(unselectedNoteImage.pixel(centerPixelX, bottomPixel))
          == expectedNoteColor) {
        fail("unselected note face appears below its black bottom border");
      }

      view.applyViewState(originalView);
      QCoreApplication::processEvents();
    }

    const int selectedTrackBeforeGhostProbe = view.selectedTrack();
    const int ghostTrack =
        (selectedTrackBeforeGhostProbe + 1) % doc.engineTrackCount();
    view.selectTrack(ghostTrack);
    const QImage ghostNoteRender = roll->grab().toImage();
    const int ghostCenterX = toRasterPixel(paintedNoteBox.center().x());
    const int ghostTopPixel = toRasterPixel(paintedNoteBox.top());
    const int ghostBottomPixel =
        toRasterPixel(paintedNoteBox.bottom()) - 1;
    const QRgb ghostTopEdge =
        ghostNoteRender.pixel(ghostCenterX, ghostTopPixel);
    const QRgb ghostTopInterior =
        ghostNoteRender.pixel(ghostCenterX, ghostTopPixel + 2);
    const QRgb ghostBottomEdge =
        ghostNoteRender.pixel(ghostCenterX, ghostBottomPixel);
    const QRgb ghostBottomInterior =
        ghostNoteRender.pixel(ghostCenterX, ghostBottomPixel - 2);

    if (ghostTopEdge != ghostTopInterior
        || ghostBottomEdge != ghostBottomInterior)
      fail("ghost note face edge does not match adjacent interior pixel");

    // Velocity-color display mode (View menu, app-wide): the active track's
    // note fills take their hue from velocity — exact purple and red
    // endpoints, the hue falling monotonically through the spectrum between
    // — while ghost notes keep the identity rendering byte-for-byte.
    if (SongView::velocityNoteColor(1) != QColor(0x5f, 0x44, 0xe9))
      fail("velocity 1 fill is not the purple endpoint #5f44e9");
    if (SongView::velocityNoteColor(127) != QColor(0xe9, 0x09, 0x04))
      fail("velocity 127 fill is not the red endpoint #e90904");
    if (SongView::velocityNoteColor(0)
        != themes::color(themes::Role::song_view_note_velocity_zero))
      fail("velocity 0 fill is not the theme neutral");
    for (int velocity = 2; velocity <= 127; ++velocity) {
      const QColor lower = SongView::velocityNoteColor(velocity - 1);
      const QColor upper = SongView::velocityNoteColor(velocity);
      if (upper.alpha() != 255) {
        fail("velocity fill is not opaque");
        break;
      }
      if (upper.hsvHueF() > lower.hsvHueF()) {
        fail("velocity hue does not fall monotonically from purple to red");
        break;
      }
    }

    // noteA is a ghost while ghostTrack is selected: flipping the mode must
    // not move a single sampled ghost pixel.
    view.setVelocityColorMode(true);
    const QImage ghostVelocityRender = roll->grab().toImage();
    if (ghostVelocityRender.pixel(ghostCenterX, ghostTopPixel) != ghostTopEdge
        || ghostVelocityRender.pixel(ghostCenterX, ghostTopPixel + 2)
            != ghostTopInterior
        || ghostVelocityRender.pixel(ghostCenterX, ghostBottomPixel)
            != ghostBottomEdge
        || ghostVelocityRender.pixel(ghostCenterX, ghostBottomPixel - 2)
            != ghostBottomInterior)
      fail("velocity-color mode changed a ghost note's rendering");

    view.selectTrack(selectedTrackBeforeGhostProbe);
    const QImage velocityModeRender = roll->grab().toImage();
    if (QColor(velocityModeRender.pixel(noteInteriorSample))
        != SongView::velocityNoteColor(100))
      fail("velocity-mode note interior does not match velocityNoteColor(100)");

    view.setVelocityColorMode(false);
    const QImage identityRestoredRender = roll->grab().toImage();
    if (QColor(identityRestoredRender.pixel(noteInteriorSample))
        != expectedNoteColor)
      fail("disabling velocity-color mode did not restore identity fills");

    // Click latch: give note A a distinctive velocity behind the view's
    // back, click it, and the next drawn note must inherit it.
    doc.setNotesVelocity({noteA}, 73);
    click(roll, a.center);
    const Cell b = findFreeCell();
    if (b.key < 0) {
        fail("no free grid cell for the click-latch draw");
        return failures;
    }
    drawNote(roll, b.center);
    DocNote noteB;
    if (!doc.findNote(track, b.tick, uint8_t(b.key), &noteB)) {
        fail("click-latch draw produced no note");
        return failures;
    }
    if (noteB.velocity != 73)
        fail("clicked note's velocity did not latch into the next draw");

    // A right-click on another note while the note menu is open replaces the
    // popup in one gesture instead of spending the click only dismissing it.
    sendMouse(roll, QEvent::MouseButtonPress, b.center, Qt::RightButton,
              Qt::RightButton);
    sendMouse(roll, QEvent::MouseButtonRelease, b.center, Qt::RightButton,
              Qt::NoButton);
    QCoreApplication::processEvents();
    auto *noteMenu = roll->findChild<QMenu *>();
    if (!noteMenu || !noteMenu->isVisible()) {
        fail("right-click did not open the note menu");
    } else {
        const QPoint aGlobal = roll->mapToGlobal(a.center);
        sendMouse(noteMenu, QEvent::MouseButtonPress,
                  noteMenu->mapFromGlobal(aGlobal), Qt::RightButton,
                  Qt::RightButton);
        sendMouse(noteMenu, QEvent::MouseButtonRelease,
                            noteMenu->mapFromGlobal(aGlobal), Qt::RightButton, Qt::NoButton);
        QCoreApplication::processEvents();
        const std::vector<SongView::NoteId> &selection = view.selection();
        const SongView::NoteId aId{uint32_t(a.tick), uint8_t(a.key)};
        if (!noteMenu->isVisible())
            fail("retargeting hid the open note menu");
        if (selection.size() != 1 || !(selection.front() == aId))
            fail("retargeting did not select the new note");

        // A right-click that hits no note must fall through to QMenu and
        // dismiss the popup, not be swallowed. The menu hangs below note
        // A's row, so the first clear row above it is outside the popup
        // (rows scrolled off the top are fine — nothing to hit there).
        int clearKey = a.key + 1;
        while (clearKey <= 127 && occupied(a.tick, a.dur, clearKey))
            clearKey++;
        const QPoint clearGlobal =
                roll->mapToGlobal(QPoint(a.center.x(), rows.centerY(clearKey)));
        sendMouse(noteMenu, QEvent::MouseButtonPress,
                  noteMenu->mapFromGlobal(clearGlobal), Qt::RightButton,
                  Qt::RightButton);
        sendMouse(noteMenu, QEvent::MouseButtonRelease,
                  noteMenu->mapFromGlobal(clearGlobal), Qt::RightButton,
                  Qt::NoButton);
        QCoreApplication::processEvents();
        if (noteMenu->isVisible()) {
            fail("empty-space right-click did not dismiss the note menu");
            noteMenu->hide();
            QCoreApplication::processEvents();
        }
    }

    // Drag latch: grab note B's velocity bar and pull 20px up (1px = 1
    // step), 73 -> 93. The latch must follow the dragged value, not the
    // press value.
    const QRectF bRect = rows.noteRect(0, 1, b.key);
    const QPoint bHandle(
            b.center.x(), qRound(songview::velBarRect(bRect, 73, rows.dpr()).center().y()));
    sendMouse(roll, QEvent::MouseButtonPress, bHandle, Qt::LeftButton,
              Qt::LeftButton);
    sendMouse(roll, QEvent::MouseMove, bHandle - QPoint(0, 20), Qt::NoButton,
              Qt::LeftButton);
    // The cursor sits rows above the note now, but the hover mark pins to
    // the note's own pitch for the whole velocity drag.
    if (roll->property("hoverKey").toInt() != b.key)
        fail("velocity drag did not pin the hover mark to the note's key");
    sendMouse(roll, QEvent::MouseButtonRelease, bHandle - QPoint(0, 20),
              Qt::LeftButton, Qt::NoButton);
    DocNote dragged;
    if (!doc.findNote(track, b.tick, uint8_t(b.key), &dragged) ||
            dragged.velocity != 93)
        fail("velocity-handle drag did not land at 93");
    const Cell c = findFreeCell();
    if (c.key < 0) {
        fail("no free grid cell for the drag-latch draw");
        return failures;
    }
    drawNote(roll, c.center);
    DocNote noteC;
    if (!doc.findNote(track, c.tick, uint8_t(c.key), &noteC)) {
        fail("drag-latch draw produced no note");
        return failures;
    }
    if (noteC.velocity != 93)
        fail("dragged velocity did not latch into the next draw");

    // The handle rides the velocity bar, not the note's top strip: with
    // note B's bar parked low (velocity 20), a drag from the note's top
    // row must Move the note off its key, not change its velocity.
    // (Skipped when the drag above already displaced note B.)
    DocNote bNow;
    if (doc.findNote(track, b.tick, uint8_t(b.key), &bNow)) {
        doc.setNotesVelocity({bNow}, 20);
        const QPoint bTop(b.center.x(), rows.noteTopProbeY(b.key));
        sendMouse(roll, QEvent::MouseButtonPress, bTop, Qt::LeftButton,
                  Qt::LeftButton);
        const QPoint movedTop(bTop.x(), rows.noteTopProbeY(b.key + 2));
        sendMouse(roll, QEvent::MouseMove, movedTop, Qt::NoButton,
                            Qt::LeftButton);
        sendMouse(roll, QEvent::MouseButtonRelease, movedTop,
                  Qt::LeftButton, Qt::NoButton);
        if (doc.findNote(track, b.tick, uint8_t(b.key), &bNow))
            fail("top-of-note drag on a low-velocity note did not move the "
                 "note (velocity handle still on the top strip?)");
        doc.undoStack()->undo(); // the move
        doc.undoStack()->undo(); // the velocity-20 set
        click(roll, b.center);   // re-latch 93 for the sections below
    }

    // Double-click on a note deletes it (the pencil sections above prove
    // the same event still draws over empty space). Note C goes.
    sendMouse(roll, QEvent::MouseButtonDblClick, c.center, Qt::LeftButton,
              Qt::LeftButton);
    sendMouse(roll, QEvent::MouseButtonRelease, c.center, Qt::LeftButton,
              Qt::NoButton);
    if (doc.findNote(track, c.tick, uint8_t(c.key), &noteC))
        fail("double-click on a note did not delete it");

    // Band-sweep audition: notes audition (self-releasing, duration in
    // samples) as the right-drag rubber band first covers them, release
    // early when the band leaves them (velocity-0 emission), re-audition on
    // re-entry, all release at the drag's end, and no undo commands.
    {
        std::vector<int> onKeys, offKeys;
        quint32 minDur = UINT32_MAX;
        auto conn = QObject::connect(&view, &SongView::auditionNoteTimed, &view,
            [&](int, int key, int velocity, quint32 dur) {
                if (velocity > 0) {
                    onKeys.push_back(key);
                    minDur = std::min(minDur, dur);
                } else {
                    offKeys.push_back(key);
                }
            });
        const int preBandCount = doc.undoStack()->count();
        const QPoint sweepStart(songview::kKeyboardW + 1, 0);
        const QPoint sweepEnd(std::max(a.center.x(), b.center.x()) + 4,
                              std::max(a.center.y(), b.center.y()) + 4);
        sendMouse(roll, QEvent::MouseButtonPress, sweepStart, Qt::RightButton,
                  Qt::RightButton);
        sendMouse(roll, QEvent::MouseMove, a.center + QPoint(4, 4), Qt::NoButton,
                  Qt::RightButton);
        if (std::find(onKeys.begin(), onKeys.end(), a.key) == onKeys.end())
            fail("sweeping the band over a note did not audition it");
        // Retreat to a band covering nothing: the departed notes' previews
        // must release now, not ring out their durations.
        sendMouse(roll, QEvent::MouseMove, sweepStart + QPoint(4, 4), Qt::NoButton,
                            Qt::RightButton);
        if (std::find(offKeys.begin(), offKeys.end(), a.key) == offKeys.end())
            fail("shrinking the band did not release the departed note");
        sendMouse(roll, QEvent::MouseMove, sweepEnd, Qt::NoButton, Qt::RightButton);
        sendMouse(roll, QEvent::MouseButtonRelease, sweepEnd, Qt::RightButton,
                  Qt::NoButton);
        QObject::disconnect(conn);
        if (std::count(onKeys.begin(), onKeys.end(), a.key) < 2)
            fail("re-covering a note did not re-audition it");
        const std::vector<SongView::NoteId> &sel = view.selection();
        if (sel.size() < 2 ||
                std::find(sel.begin(), sel.end(),
                                    SongView::NoteId{uint32_t(a.tick), uint8_t(a.key)}) ==
                        sel.end() ||
                std::find(sel.begin(), sel.end(),
                                    SongView::NoteId{uint32_t(b.tick), uint8_t(b.key)}) ==
                        sel.end())
            fail("band release did not select the swept notes");
        // Every key that auditioned was eventually released (mid-drag or at
        // the drag's end).
        auto keySet = [](std::vector<int> keys) {
            std::sort(keys.begin(), keys.end());
            keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
            return keys;
        };
        if (keySet(onKeys) != keySet(offKeys))
            fail("band sweep left auditioned keys unreleased");
        if (!onKeys.empty() && minDur == 0)
            fail("band sweep auditioned a zero-length note");
        if (doc.undoStack()->count() != preBandCount)
            fail("band sweep pushed an undo command");
        view.clearSelection(); // the sections below manage their own
    }

    // Empty-space press audition: a plain left press sounds its row at the
    // latched velocity right away, glisses when the held cursor crosses
    // rows, and releases on mouse-up — while the release in place still
    // parks the edit cursor without touching the document. A press that
    // grows into a draw keeps the already-sounding key ringing instead of
    // re-attacking it.
    {
        const Cell e = findFreeCell();
        if (e.key < 0) {
            fail("no free grid cell for the press audition");
            return failures;
        }
        std::vector<std::pair<int, int>> aud; // key, velocity
        auto conn = QObject::connect(
            &view, &SongView::auditionNote, &view,
            [&](int, int key, int velocity) { aud.push_back({key, velocity}); });
        const int preCount = doc.undoStack()->count();
        sendMouse(roll, QEvent::MouseButtonPress, e.center, Qt::LeftButton,
                  Qt::LeftButton);
        if (aud != std::vector<std::pair<int, int>>{{e.key, 93}})
            fail(
                    "empty-space press did not audition its row at the latched velocity");
        const QPoint gliss(e.center.x(), rows.centerY(e.key - 1));
        sendMouse(roll, QEvent::MouseMove, gliss, Qt::NoButton,
                            Qt::LeftButton);
        if (aud.empty() || aud.back() != std::make_pair(e.key - 1, 93))
            fail("holding the press across a row did not gliss the preview");
        sendMouse(roll, QEvent::MouseButtonRelease, gliss,
                  Qt::LeftButton, Qt::NoButton);
        if (aud.empty() || aud.back().second != 0)
            fail("releasing the press did not release the preview");
        if (doc.undoStack()->count() != preCount)
            fail("a plain empty-space click edited the document");
        if (view.editCursorTick() !=
                view.snapTick(view.tickAtContentX(e.center.x() - songview::kKeyboardW)))
            fail("the press audition broke the click's edit-cursor park");
        // Draw growth: press the still-free cell again and drag right past
        // the drag threshold; the press's preview must carry into the draw
        // with no second attack on the same key.
        aud.clear();
        const QPoint pull =
            e.center + QPoint(QApplication::startDragDistance() + 8, 0);
        sendMouse(roll, QEvent::MouseButtonPress, e.center, Qt::LeftButton,
                  Qt::LeftButton);
        sendMouse(roll, QEvent::MouseMove, pull, Qt::NoButton, Qt::LeftButton);
        sendMouse(roll, QEvent::MouseButtonRelease, pull, Qt::LeftButton,
                  Qt::NoButton);
        QObject::disconnect(conn);
        if (std::count(aud.begin(), aud.end(), std::make_pair(e.key, 93)) != 1)
            fail("growing the press into a draw re-attacked the sounding key");
        DocNote drawn;
        if (!doc.findNote(track, e.tick, uint8_t(e.key), &drawn))
            fail("the press-grown draw did not commit its note");
    }

    // Any horizontal travel starts the draw — no drag threshold — and the
    // note first appears at its minimum length, one snap cell.
    {
        const Cell f = findFreeCell();
        if (f.key < 0) {
            fail("no free grid cell for the tiny-drag draw");
            return failures;
        }
        sendMouse(roll, QEvent::MouseButtonPress, f.center, Qt::LeftButton,
                  Qt::LeftButton);
        sendMouse(roll, QEvent::MouseMove, f.center + QPoint(2, 0), Qt::NoButton,
                            Qt::LeftButton);
        sendMouse(roll, QEvent::MouseButtonRelease, f.center + QPoint(2, 0),
                  Qt::LeftButton, Qt::NoButton);
        DocNote tiny;
        if (!doc.findNote(track, f.tick, uint8_t(f.key), &tiny))
            fail("a tiny horizontal drag did not draw a note");
        else if (tiny.duration != view.snapTicksAt(f.tick))
            fail("the tiny-drag note is not one snap cell long");
    }

    // Modifier velocity gesture (Ableton-style): with the roll.velocity_drag
    // chord held (Ctrl by default), a vertical drag from anywhere on note B
    // adjusts its velocity — 1px = 1 step, 15px down lands 93 -> 78 — with
    // the hover mark pinned to the note's row. Without the drag the
    // Ctrl+click keeps its selection-toggle meaning (deferred to release),
    // and a vertical jitter under the drag threshold is still that click:
    // it toggles, changes no velocity, and pushes no undo command.
    {
        click(roll, b.center); // plain click: select B (velocity 93)
        const int preCount = doc.undoStack()->count();
        sendMouse(roll, QEvent::MouseButtonPress, b.center, Qt::LeftButton,
                  Qt::LeftButton, Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseMove, b.center + QPoint(0, 15), Qt::NoButton,
                            Qt::LeftButton, Qt::ControlModifier);
        if (roll->property("hoverKey").toInt() != b.key)
            fail("modifier velocity drag did not pin the hover mark");
        sendMouse(roll, QEvent::MouseButtonRelease, b.center + QPoint(0, 15),
                  Qt::LeftButton, Qt::NoButton, Qt::ControlModifier);
        DocNote bMod;
        if (!doc.findNote(track, b.tick, uint8_t(b.key), &bMod) ||
                bMod.velocity != 78)
            fail("modifier velocity drag did not land at 78");
        if (doc.undoStack()->count() != preCount + 1)
            fail("modifier velocity drag did not push exactly one command");

        const SongView::NoteId bId{uint32_t(b.tick), uint8_t(b.key)};
        sendMouse(roll, QEvent::MouseButtonPress, b.center, Qt::LeftButton,
                  Qt::LeftButton, Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseButtonRelease, b.center, Qt::LeftButton,
                  Qt::NoButton, Qt::ControlModifier);
        if (std::find(view.selection().begin(), view.selection().end(), bId) !=
                view.selection().end())
            fail("Ctrl+click did not toggle the note out of the selection");

        sendMouse(roll, QEvent::MouseButtonPress, b.center, Qt::LeftButton,
                  Qt::LeftButton, Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseMove, b.center + QPoint(0, 3), Qt::NoButton,
                            Qt::LeftButton, Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseButtonRelease, b.center + QPoint(0, 3),
                  Qt::LeftButton, Qt::NoButton, Qt::ControlModifier);
        if (view.selection().size() != 1 || !(view.selection().front() == bId))
            fail("a sub-threshold Ctrl-jitter did not act as the toggle click");
        if (!doc.findNote(track, b.tick, uint8_t(b.key), &bMod) ||
                bMod.velocity != 78)
            fail("a sub-threshold Ctrl-jitter changed the velocity");
        if (doc.undoStack()->count() != preCount + 1)
            fail("a Ctrl-click or jitter pushed an undo command");

        // Bulk-selection preservation, mirroring the Ctrl+edge grab: with
        // note A selected, a Ctrl+velocity drag on unselected note B joins
        // B to the selection instead of replacing it, and the nudge lands
        // on BOTH notes in one command.
        click(roll, a.center); // selection = {A}
        DocNote aBefore, bBefore;
        if (!doc.findNote(track, a.tick, uint8_t(a.key), &aBefore) ||
                !doc.findNote(track, b.tick, uint8_t(b.key), &bBefore))
            fail("notes A/B went missing before the joined velocity drag");
        const int joinCount = doc.undoStack()->count();
        sendMouse(roll, QEvent::MouseButtonPress, b.center, Qt::LeftButton,
                  Qt::LeftButton, Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseMove, b.center + QPoint(0, 15),
                  Qt::NoButton, Qt::LeftButton, Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseButtonRelease, b.center + QPoint(0, 15),
                  Qt::LeftButton, Qt::NoButton, Qt::ControlModifier);
        const SongView::NoteId aId{uint32_t(a.tick), uint8_t(a.key)};
        const std::vector<SongView::NoteId> &joined = view.selection();
        if (joined.size() != 2 ||
                std::find(joined.begin(), joined.end(), aId) == joined.end() ||
                std::find(joined.begin(), joined.end(), bId) == joined.end())
            fail("a Ctrl+velocity drag replaced the bulk selection");
        DocNote aAfter, bAfter;
        if (!doc.findNote(track, a.tick, uint8_t(a.key), &aAfter) ||
                aAfter.velocity != aBefore.velocity - 15)
            fail("the joined Ctrl+velocity drag did not nudge the other note");
        if (!doc.findNote(track, b.tick, uint8_t(b.key), &bAfter) ||
                bAfter.velocity != bBefore.velocity - 15)
            fail("the joined Ctrl+velocity drag did not nudge the grabbed note");
        if (doc.undoStack()->count() != joinCount + 1)
            fail("the joined velocity drag did not push exactly one command");
        doc.undoStack()->undo(); // restore both velocities for later checks
        view.clearSelection();
    }

    // Edge resize snaps to the ruler's absolute grid, not to grid-sized
    // offsets from the note's own end: give a note an off-grid duration
    // (1.25 cells) behind the view's back, drag its right edge to 1.9
    // cells, and the end must land on the 2-cell grid line — not at
    // 1.75 cells, the nearest snap-sized offset from the off-grid end.
    const Cell d = findFreeCell();
    if (d.key < 0) {
        fail("no free grid cell for the off-grid resize");
        return failures;
    }
    // The absolute snap grid the edits land on: half a drawn cell.
    const uint64_t snapCell = view.snapTicksAt(d.tick);
    const uint32_t offDur = uint32_t(d.dur + d.dur / 4);
    doc.addNote(track, d.tick, uint8_t(d.key), offDur, 100);
    const int rowY = rows.centerY(d.key);
    // Probe 6.8 DIPs inward at both ends on the velocity bar itself. The
    // resize zones must win over the overlapping velocity hover.
    const qreal resizeNoteLeftX = view.displayX(
        double(d.tick), songview::kKeyboardW, roll->devicePixelRatioF());
    const qreal resizeNoteRightX = view.displayX(
        double(d.tick + offDur), songview::kKeyboardW, roll->devicePixelRatioF());
    const int resizeHandleY = qRound(
        songview::velBarRect(rows.noteRect(0, 1, d.key), 100, rows.dpr())
            .center()
            .y());
    const QPointF leftHandle(resizeNoteLeftX + 6.8, resizeHandleY);
    const QPointF rightHandle(resizeNoteRightX - 6.8, resizeHandleY);
    sendMouse(roll, QEvent::MouseMove, leftHandle, Qt::NoButton, Qt::NoButton,
              Qt::ControlModifier);
    const QPixmap expectedLeftCursor =
        QIcon(QStringLiteral(":/cursors/left-drag.png"))
            .pixmap(QSize(24, 24), roll->devicePixelRatioF());
    if (roll->cursor().pixmap().devicePixelRatio() !=
                    expectedLeftCursor.devicePixelRatio() ||
            roll->cursor().pixmap().toImage() != expectedLeftCursor.toImage())
        fail("left note edge did not show its DPI-matched custom cursor");
    sendMouse(roll, QEvent::MouseMove, rightHandle, Qt::NoButton, Qt::NoButton,
              Qt::ControlModifier);
    const QPixmap expectedRightCursor =
        QIcon(QStringLiteral(":/cursors/right-drag.png"))
            .pixmap(QSize(24, 24), roll->devicePixelRatioF());
    if (roll->cursor().pixmap().devicePixelRatio() !=
                    expectedRightCursor.devicePixelRatio() ||
            roll->cursor().pixmap().toImage() != expectedRightCursor.toImage())
        fail("right note edge did not show its custom cursor");
    const QPoint pull(songview::kKeyboardW +
                                                view.contentX(double(d.tick) + 1.9 * double(d.dur)),
        rowY);
    sendMouse(roll, QEvent::MouseButtonPress, rightHandle, Qt::LeftButton,
              Qt::LeftButton);
    sendMouse(roll, QEvent::MouseMove, pull, Qt::NoButton, Qt::LeftButton);
    sendMouse(roll, QEvent::MouseButtonRelease, pull, Qt::LeftButton,
              Qt::NoButton);
    DocNote resized;
    if (!doc.findNote(track, d.tick, uint8_t(d.key), &resized) ||
            resized.duration != 2 * d.dur)
        fail("off-grid right-edge drag did not snap the end to the ruler grid");

    // Ctrl+grabbing an edge keeps the bulk selection: with note B
    // selected, a Ctrl+press on note D's right-edge grip joins D to the
    // selection instead of replacing it, and the drag that follows
    // resizes the whole selection — both notes — in one undo command. A
    // stationary Ctrl+edge click just joins, editing nothing.
    {
        const qreal dpr = roll->devicePixelRatioF();
        const QPointF ctrlEdge(
            view.displayX(double(d.tick + 2 * d.dur), songview::kKeyboardW,
                          dpr) -
                6.8,
            rowY);
        click(roll, b.center); // selection = {B}
        const int preCount = doc.undoStack()->count();
        DocNote bBefore;
        if (!doc.findNote(track, b.tick, uint8_t(b.key), &bBefore))
            fail("note B went missing before the Ctrl+edge grab");
        sendMouse(roll, QEvent::MouseButtonPress, ctrlEdge, Qt::LeftButton,
                  Qt::LeftButton, Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseButtonRelease, ctrlEdge, Qt::LeftButton,
                  Qt::NoButton, Qt::ControlModifier);
        const SongView::NoteId bId{uint32_t(b.tick), uint8_t(b.key)};
        const SongView::NoteId dId{uint32_t(d.tick), uint8_t(d.key)};
        const std::vector<SongView::NoteId> &sel = view.selection();
        if (sel.size() != 2 ||
                std::find(sel.begin(), sel.end(), bId) == sel.end() ||
                std::find(sel.begin(), sel.end(), dId) == sel.end())
            fail("a Ctrl+edge click did not join the note to the selection");
        DocNote still;
        if (!doc.findNote(track, d.tick, uint8_t(d.key), &still) ||
                still.duration != 2 * d.dur)
            fail("a stationary Ctrl+edge click resized the note");
        if (doc.undoStack()->count() != preCount)
            fail("a stationary Ctrl+edge click pushed an undo command");
        // Pull the grip one drawn cell further right: both selected notes
        // must grow by that same delta.
        const qreal cellPx =
            view.displayX(double(d.tick + 3 * d.dur), songview::kKeyboardW,
                          dpr) -
            view.displayX(double(d.tick + 2 * d.dur), songview::kKeyboardW,
                          dpr);
        const QPointF pull2(ctrlEdge.x() + cellPx, rowY);
        sendMouse(roll, QEvent::MouseButtonPress, ctrlEdge, Qt::LeftButton,
                  Qt::LeftButton, Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseMove, pull2, Qt::NoButton, Qt::LeftButton,
                  Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseButtonRelease, pull2, Qt::LeftButton,
                  Qt::NoButton, Qt::ControlModifier);
        DocNote dAfter, bAfter;
        if (!doc.findNote(track, d.tick, uint8_t(d.key), &dAfter) ||
                dAfter.duration != 3 * d.dur)
            fail("Ctrl+edge drag did not resize the grabbed note");
        if (!doc.findNote(track, b.tick, uint8_t(b.key), &bAfter) ||
                bAfter.duration != bBefore.duration + d.dur)
            fail("Ctrl+edge drag did not resize the rest of the selection");
        if (doc.undoStack()->count() != preCount + 1)
            fail("Ctrl+edge drag did not push exactly one command");
        doc.undoStack()->undo(); // restore both durations for later checks
        view.clearSelection();
    }

    // Overshooting the drag past the note's start must stop at one snap
    // cell, not collapse to the document's 1-tick floor.
    const QPoint edge2(
        songview::kKeyboardW + view.contentX(double(d.tick + 2 * d.dur)), rowY);
    const QPoint overshoot(
            songview::kKeyboardW +
                    view.contentX(double(d.tick) - 0.5 * double(d.dur)),
        rowY);
    sendMouse(roll, QEvent::MouseButtonPress, edge2, Qt::LeftButton,
                        Qt::LeftButton);
    sendMouse(roll, QEvent::MouseMove, overshoot, Qt::NoButton, Qt::LeftButton);
    sendMouse(roll, QEvent::MouseButtonRelease, overshoot, Qt::LeftButton,
              Qt::NoButton);
    DocNote collapsed;
    if (!doc.findNote(track, d.tick, uint8_t(d.key), &collapsed) ||
            collapsed.duration != snapCell)
        fail("overshot right-edge drag did not stop at one snap cell");

    // The collapsed note is one snap cell (16 DIPs here) wide. Inside a
    // note that narrow the edge zones shrink to leave a grabbable middle,
    // so 6 DIPs in from the right edge (below the velocity bar) is part of
    // that middle: the hover shows the plain arrow, not a resize cursor.
    const QPointF narrowMiddle(
        songview::kKeyboardW +
            view.contentX(double(d.tick) + double(snapCell)) - 6,
        rows.bottom(d.key) - 2);
    sendMouse(roll, QEvent::MouseMove, narrowMiddle, Qt::NoButton,
              Qt::NoButton);
    if (roll->cursor().shape() != Qt::ArrowCursor)
        fail("narrow-note middle lost its move target to the edge resize zones");

    // Frame weight is fitted by row height only, so squeezing this
    // one-snap-cell note to ~2px wide at minimum horizontal zoom keeps the
    // same border its wide neighbors have instead of shedding it.
    {
        const SongView::ViewState originalView = view.viewState();
        view.clearSelection(); // the resize press selected note d
        SongView::ViewState narrowView = originalView;
        narrowView.pxPerBeat = 4.0;
        const double narrowPxPerTick = 4.0 / double(timeline->ticksPerBeat);
        narrowView.scrollPx =
            std::max(0.0, double(d.tick) * narrowPxPerTick - 100.0);
        view.applyViewState(narrowView);
        const SnappedRows narrowRows{view, *roll};
        const int narrowLeftX =
            songview::kKeyboardW + view.contentX(double(d.tick));
        const int narrowRightX = songview::kKeyboardW
            + view.contentX(double(d.tick + snapCell));
        if (narrowRightX - narrowLeftX > 3)
            fail("narrow-zoom fixture note is unexpectedly wide");
        const QRectF narrowBox = narrowRows.noteBox(
            narrowRows.noteRect(narrowLeftX, narrowRightX, d.key));
        QImage narrowImage(roll->size(),
                           QImage::Format_ARGB32_Premultiplied);
        narrowImage.fill(Qt::transparent);
        roll->render(&narrowImage);
        const auto isNarrowBorder = [&](QRgb pixel) {
            return qRed(pixel) <= 16 && qGreen(pixel) <= 16
                && qBlue(pixel) <= 16;
        };
        if (!isNarrowBorder(
                narrowImage.pixel(qRound(narrowBox.center().x()),
                                  qRound(narrowBox.top()))))
            fail("narrow note shed the border its wide neighbors keep");
        view.applyViewState(originalView);
    }

    // Keyboard transpose/nudge on note D (clicking it selects it):
    // Ctrl+Up is a semitone, Ctrl+Shift+Down an octave, and Ctrl+Right
    // moves one snap cell from an on-grid start.
    const QPoint dCenter(
            songview::kKeyboardW +
                    view.contentX(double(d.tick) + 0.5 * double(snapCell)),
                         rowY);
    click(roll, dCenter);
    sendKey(roll, Qt::Key_Up, Qt::ControlModifier);
    DocNote transposed;
    if (!doc.findNote(track, d.tick, uint8_t(d.key + 1), &transposed))
        fail("Ctrl+Up did not transpose up a semitone");
    sendKey(roll, Qt::Key_Down, Qt::ControlModifier | Qt::ShiftModifier);
    if (!doc.findNote(track, d.tick, uint8_t(d.key - 11), &transposed))
        fail("Ctrl+Shift+Down did not transpose down an octave");
    sendKey(roll, Qt::Key_Right, Qt::ControlModifier);
    if (!doc.findNote(track, d.tick + snapCell, uint8_t(d.key - 11), &transposed))
        fail("Ctrl+Right did not nudge one snap cell right");
    // An off-grid selection nudges onto the grid line, not by a whole
    // cell: push the note half a snap cell right behind the view's back
    // (reselecting — the selection keys on the start tick, which moved),
    // and Ctrl+Left must bring it back to the line it left.
    doc.moveNotes({transposed}, int64_t(snapCell / 2), 0);
    view.setSelection(
        {{uint32_t(d.tick + snapCell + snapCell / 2), uint8_t(d.key - 11)}});
    sendKey(roll, Qt::Key_Left, Qt::ControlModifier);
    if (!doc.findNote(track, d.tick + snapCell, uint8_t(d.key - 11), &transposed))
        fail("Ctrl+Left did not snap the off-grid note back to the grid");

    // Keyboard moves keep the notes in view, scrolling just enough rather
    // than re-anchoring. Vertical: park the note's row above the viewport,
    // and Ctrl+Up must land it flush at the top edge.
    const int keyNow = d.key - 11;
    view.scrollRollBy((129 - keyNow) * view.keyHeight() - view.scrollY());
    if ((128 - keyNow) * view.keyHeight() - view.scrollY() > 1e-9)
        fail("could not park the note's row above the viewport");
    sendKey(roll, Qt::Key_Up, Qt::ControlModifier);
    if (std::abs(view.scrollY() - (126 - keyNow) * view.keyHeight()) > 1e-9)
        fail("Ctrl+Up above the viewport did not scroll the row flush to the top");
    sendKey(roll, Qt::Key_Down, Qt::ControlModifier); // undo the extra semitone

    // Horizontal: park the note past the left edge; nudging right must
    // bring its start flush to the left edge (minimal scroll, not the
    // paste jump). Then ride it right across the viewport: once the end
    // crosses the right edge, it must stay flush there.
    uint64_t nStart = d.tick + snapCell;
    const qreal dpr = roll->devicePixelRatioF();
    const qreal physicalPixel = dpr > 0.0 ? 1.0 / dpr : 1.0;
    view.scrollByPx(view.contentX(double(nStart + snapCell)) + 40);
    if (view.displayX(double(nStart + snapCell), 0.0, dpr) >= 0.0)
        fail("could not park the note past the left edge");
    sendKey(roll, Qt::Key_Right, Qt::ControlModifier);
    nStart += snapCell;
    if (view.displayX(double(nStart), 0.0, dpr) != 0.0)
        fail("Ctrl+Right off-screen-left did not scroll the start flush to the "
                  "left edge");
    const qreal vw = std::max(50, roll->width() - songview::kKeyboardW);
    const qreal cellPx =
        view.contentX(double(nStart + snapCell)) - view.contentX(double(nStart));
    const int rides =
            (vw - view.contentX(double(nStart + snapCell))) / cellPx + 2;
    for (int i = 0; i < rides; i++)
        sendKey(roll, Qt::Key_Right, Qt::ControlModifier);
    nStart += uint64_t(rides) * snapCell;
    if (view.displayX(double(nStart + snapCell), 0.0, dpr)
            != vw - physicalPixel)
        fail(
                "riding the nudge right did not keep the note's end at the right edge");
    // Ride back home so the time-selection checks below find the note
    // where they expect it; every press so far merges into one command.
    for (int i = 0; i < rides + 1; i++)
        sendKey(roll, Qt::Key_Left, Qt::ControlModifier);
    if (!doc.findNote(track, d.tick + snapCell, uint8_t(d.key - 11), &transposed))
        fail("the ride right and back did not return the note home");

    // Consecutive keyboard presses on the same notes merge into one undo
    // command; mark a save point so the time-selection presses below get
    // their own commands (merges never cross the stack's clean index).
    doc.undoStack()->setClean();

    // The same shortcuts on a time selection (no notes selected): the band
    // over the note's cell transposes every covered note of the scoped
    // tracks, and a nudge moves the contents with the band following.
    SongView::TimeSelection band;
    band.startTick = d.tick + snapCell;
    band.endTick = d.tick + 2 * snapCell;
    view.setTimeSelection(band);
    sendKey(roll, Qt::Key_Up, Qt::ControlModifier);
    if (!doc.findNote(track, d.tick + snapCell, uint8_t(d.key - 10), &transposed))
        fail("time-selection Ctrl+Up did not transpose the covered note");
    sendKey(roll, Qt::Key_Right, Qt::ControlModifier);
    if (!doc.findNote(track, d.tick + 2 * snapCell, uint8_t(d.key - 10),
                                        &transposed))
        fail("time-selection Ctrl+Right did not nudge the covered note");
    if (view.timeSelection().startTick != d.tick + 2 * snapCell)
        fail("time-selection band did not follow the nudge");

    // Playhead follow-scroll pauses while a mouse gesture is live: with a
    // middle-button pan held in the roll (or the lanes), a playing playhead
    // far past the right edge must not move the view; releasing the button
    // lets the next playhead tick scroll again.
    auto *lanes = view.findChild<QWidget *>(QStringLiteral("automationArea"));
    if (!lanes)
        fail("automation area not found");
    for (QWidget *panned : {roll, lanes}) {
        if (!panned)
            continue;
        const int home = view.contentX(0.0);
        const uint64_t farTick =
            uint64_t(std::max(0.0, view.tickAtContentX(vw * 2)));
        const QPoint mid(panned->width() / 2, panned->height() / 2);
        sendMouse(panned, QEvent::MouseButtonPress, mid, Qt::MiddleButton,
                  Qt::MiddleButton);
        view.setPlayheadSample(timeline->sampleForTick(farTick), true);
        if (view.contentX(0.0) != home)
            fail("playhead follow-scroll moved the view during a pan gesture");
        sendMouse(panned, QEvent::MouseButtonRelease, mid, Qt::MiddleButton,
                  Qt::NoButton);
        view.setPlayheadSample(timeline->sampleForTick(farTick), true);
        if (view.contentX(0.0) == home)
            fail("playhead follow-scroll did not resume after the pan ended");
        view.setPlayheadSample(0, false);
        view.scrollByPx(view.contentX(0.0) - home); // back where it started
    }

    // A stopped playhead is a thin child overlay. Moving it must preserve the
    // timeline parents' backing stores instead of repainting their contents.
    for (const QString &error : playheadOverlayCheckFailures(view, *timeline))
        fail(qUtf8Printable(error));

    // Inline track rename: renameTrack opens a line editor on the header
    // row; Return commits (queued past the panel rebuild), Escape discards,
    // and loop-marker names are refused. isHidden (not isVisible) because
    // the view is never shown offscreen.
    {
        view.renameTrack(track);
        auto *editor =
            view.findChild<QLineEdit *>(QStringLiteral("trackRenameEditor"));
        if (!editor || editor->isHidden()) {
            fail("rename editor did not open");
        } else {
            editor->setText(QStringLiteral("Rolled"));
            sendKey(editor, Qt::Key_Return, Qt::NoModifier);
            QCoreApplication::processEvents(); // the queued document commit
            if (doc.trackName(track) != QStringLiteral("Rolled"))
                fail("inline rename did not apply on Return");
        }
        view.renameTrack(track); // the rebuilt panel carries a fresh editor
        editor = view.findChild<QLineEdit *>(QStringLiteral("trackRenameEditor"));
        if (!editor || editor->isHidden()) {
            fail("rename editor did not reopen after the rebuild");
        } else {
            editor->setText(QStringLiteral("Discarded"));
            sendKey(editor, Qt::Key_Escape, Qt::NoModifier);
            QCoreApplication::processEvents();
            if (doc.trackName(track) != QStringLiteral("Rolled"))
                fail("Escape did not discard the rename");
        }
        view.renameTrack(track);
        editor = view.findChild<QLineEdit *>(QStringLiteral("trackRenameEditor"));
        if (editor && !editor->isHidden()) {
            const int commands = doc.undoStack()->count();
            editor->setText(QStringLiteral("["));
            sendKey(editor, Qt::Key_Return, Qt::NoModifier);
            QCoreApplication::processEvents();
            if (doc.trackName(track) != QStringLiteral("Rolled") ||
                    doc.undoStack()->count() != commands)
                fail("loop-marker name was not refused");
        }
    }

    // The header voice line is live: currentProgram is the last program
    // change at or before the display position — the playhead while playing,
    // the edit cursor otherwise — falling back to the track's first program
    // (which is what primes the engine before any change).
    {
        view.setEditCursorTick(0);
        const int base = view.currentProgram(track);
        const int changed = base == 5 ? 6 : 5;
        const uint64_t vcTick = a.tick + 4 * a.dur;
        doc.addLanePoint(track, DOC_CC_VOICE, vcTick, changed);
        // A track with no program at all adopts the added one everywhere
        // (it becomes the priming first program).
        const int atStart = base < 0 ? changed : base;
        if (view.currentProgram(track) != atStart)
            fail("voice label at the start did not show the priming program");
        view.setEditCursorTick(vcTick);
        if (view.currentProgram(track) != changed)
            fail("voice label did not follow the edit cursor past the change");
        view.setEditCursorTick(0);
        view.setPlayheadSample(timeline->sampleForTick(vcTick), true);
        if (view.currentProgram(track) != changed)
            fail("voice label did not follow the playing playhead");
        view.setPlayheadSample(0, false); // stopped: back to the edit cursor
        if (view.currentProgram(track) != atStart)
            fail("voice label did not return to the edit cursor after stop");
    }

    // Jump-from-context: a completed plain click on a header row's voice
    // line emits revealVoiceRequested with the track's current program (the
    // main window raises the voicegroup dock and selects the slot). A click
    // on the name line stays silent, as does a press there that turns into
    // a reorder drag — and none of it is an edit, so the undo stack must
    // not move.
    {
        (void)view.grab(); // layout pass: rows need real geometry
        auto *row = view.findChild<QWidget *>(
            QStringLiteral("trackHeaderRow%1").arg(track));
        if (!row) {
            fail("track header row for the edited track not found");
        } else {
            int revealed = -1, reveals = 0;
            const QMetaObject::Connection conn = QObject::connect(
                    &view, &SongView::revealVoiceRequested, [&](int program) {
                        revealed = program;
                        reveals++;
                    });
            const int preCount = doc.undoStack()->count();
            const QPoint voicePos(row->width() / 2, 30); // the painted voice line
            click(row, voicePos);
            if (reveals != 1 || revealed != view.currentProgram(track))
                fail("voice-line click did not request the track's program");
            click(row, QPoint(row->width() / 2, 10)); // the name line
            if (reveals != 1)
                fail("a name-line click requested a voice reveal");
            // A press on the voice line that becomes a reorder drag must
            // not reveal on release (adjacent drop slot: no move commits).
            sendMouse(row, QEvent::MouseButtonPress, voicePos, Qt::LeftButton,
                      Qt::LeftButton);
            sendMouse(row, QEvent::MouseMove, voicePos + QPoint(0, 25), Qt::NoButton,
                                Qt::LeftButton);
            sendMouse(row, QEvent::MouseButtonRelease, voicePos + QPoint(0, 25),
                      Qt::LeftButton, Qt::NoButton);
            QCoreApplication::processEvents();
            if (reveals != 1)
                fail("a reorder drag from the voice line requested a reveal");
            // Double-click routing: on the voice line it opens the modal
            // voice picker (rejected here by a zero-timer poll so exec
            // returns), NOT the inline rename; on the name line it still
            // renames. Neither canceled dialog is an edit.
            QTimer poll;
            poll.setInterval(0);
            bool pickerSeen = false;
            bool searchFilteredList = false;
            QObject::connect(&poll, &QTimer::timeout, [&] {
                if (QDialog *dlg = view.findChild<QDialog *>()) {
                    pickerSeen = true;
                    auto *searchField = dlg->findChild<QLineEdit *>();
                    auto *voiceList = dlg->findChild<QListWidget *>();
                    auto *dialogButtons = dlg->findChild<QDialogButtonBox *>();
                    if (searchField && voiceList && dialogButtons) {
                        searchField->setText(QStringLiteral("127  "));
                        searchFilteredList =
                            voiceList->item(0)->isHidden()
                            && !voiceList->item(127)->isHidden();
                        searchField->clear();
                        searchFilteredList &= !voiceList->item(0)->isHidden();
                        voiceList->setCurrentRow(127);
                        searchField->setText(QStringLiteral("1"));
                        searchFilteredList &=
                            voiceList->currentRow() == 1
                            && !voiceList->item(1)->isHidden()
                            && !voiceList->item(127)->isHidden()
                            && dialogButtons->button(QDialogButtonBox::Ok)->isEnabled();
                        searchField->clear();
                        searchFilteredList &= voiceList->currentRow() == 0;
                    }
                    dlg->reject();
                }
            });
            poll.start();
            sendMouse(row, QEvent::MouseButtonDblClick, voicePos, Qt::LeftButton,
                      Qt::LeftButton);
            sendMouse(row, QEvent::MouseButtonRelease, voicePos, Qt::LeftButton,
                      Qt::NoButton);
            QCoreApplication::processEvents(); // the queued picker runs here
            poll.stop();
            if (!pickerSeen)
                fail("voice-line double-click did not open the voice picker");
            if (!searchFilteredList)
                fail("voice picker search did not select and restore its first match");
            auto *renameEditor =
                view.findChild<QLineEdit *>(QStringLiteral("trackRenameEditor"));
            if (renameEditor && !renameEditor->isHidden())
                fail("voice-line double-click opened the rename editor");
            const QPoint namePos(row->width() / 2, 10);
            sendMouse(row, QEvent::MouseButtonDblClick, namePos, Qt::LeftButton,
                      Qt::LeftButton);
            sendMouse(row, QEvent::MouseButtonRelease, namePos, Qt::LeftButton,
                      Qt::NoButton);
            renameEditor =
                view.findChild<QLineEdit *>(QStringLiteral("trackRenameEditor"));
            if (!renameEditor || renameEditor->isHidden())
                fail("name-line double-click no longer opens the rename editor");
            else
                sendKey(renameEditor, Qt::Key_Escape, Qt::NoModifier);
            if (doc.undoStack()->count() != preCount)
                fail("voice navigation touched the undo stack");
            QObject::disconnect(conn);
        }
    }

    // Header-row drag reorder (format 1 with two or more tracks): press the
    // first row, drag past the second row's center, release — the first two
    // tracks swap slots, the notes and the mute flag following, as ONE undo
    // command (committed queued, so the event loop must spin). A non-left
    // release mid-drag cancels instead of dropping, a rename editor still
    // open at the drop gets its text committed rather than destroyed, and
    // undo/redo re-permute the mute flag along with the tracks.
    bool reordered = false;
    bool dragRenamed = false;
    if (doc.engineTrackCount() >= 2) {
        // The panel was rebuilt by the edits above; force a layout pass so
        // the rows have real positions for the drop-slot hit test.
        (void)view.grab();
        auto *row0 = view.findChild<QWidget *>(QStringLiteral("trackHeaderRow0"));
        auto *row1 = view.findChild<QWidget *>(QStringLiteral("trackHeaderRow1"));
        if (!row0 || !row1) {
            fail("track header rows not found");
        } else {
            const auto firstNotes = doc.notesForTrack(0);
            view.setTrackMute(0, true);
            // Press low in the row, clear of the rename editor overlaying
            // the name line.
            const QPoint start(row0->width() / 2, row0->height() * 3 / 4);
            // Past row 1's center in row-0 coordinates: rows are contiguous
            // and equal-height, so 1.6 row heights lands between row 1's
            // center (1.5) and its bottom.
            const QPoint drop(row0->width() / 2, row0->height() * 8 / 5);

            // A right-button release mid-drag cancels; the left release
            // that follows must not commit either.
            const int preDragCount = doc.undoStack()->count();
            sendMouse(row0, QEvent::MouseButtonPress, start, Qt::LeftButton,
                      Qt::LeftButton);
            sendMouse(row0, QEvent::MouseMove, drop, Qt::NoButton, Qt::LeftButton);
            sendMouse(row0, QEvent::MouseButtonRelease, drop, Qt::RightButton,
                      Qt::LeftButton);
            sendMouse(row0, QEvent::MouseButtonRelease, drop, Qt::LeftButton,
                      Qt::NoButton);
            QCoreApplication::processEvents();
            if (doc.undoStack()->count() != preDragCount)
                fail("right-button release mid-drag committed the reorder");

            // An open rename editor rides along: the drop commits its text
            // Reaper-style (before the move, so it names the right track)
            // instead of silently discarding it with the rebuilt panel.
            view.renameTrack(0);
            auto *editor =
                view.findChild<QLineEdit *>(QStringLiteral("trackRenameEditor"));
            if (editor && !editor->isHidden()) {
                editor->setText(QStringLiteral("Dragged"));
                dragRenamed = true;
            }

            sendMouse(row0, QEvent::MouseButtonPress, start, Qt::LeftButton,
                      Qt::LeftButton);
            sendMouse(row0, QEvent::MouseMove, drop, Qt::NoButton, Qt::LeftButton);
            sendMouse(row0, QEvent::MouseButtonRelease, drop, Qt::LeftButton,
                      Qt::NoButton);
            // The queued rename commit, then the queued moveTrack commit.
            QCoreApplication::processEvents();
            const auto movedNotes = doc.notesForTrack(1);
            bool same = movedNotes.size() == firstNotes.size();
            for (size_t i = 0; same && i < movedNotes.size(); i++) {
                same = movedNotes[i].tick == firstNotes[i].tick &&
                              movedNotes[i].key == firstNotes[i].key;
            }
            if (!same) {
                fail("header drag did not move the track's notes to slot 1");
            } else if (!view.trackMuted(1) || view.trackMuted(0)) {
                fail("header drag did not move the mute flag with the track");
            } else {
                reordered = true;
                if (dragRenamed && doc.trackName(1) != QStringLiteral("Dragged"))
                    fail("the open rename editor's text was lost in the drop");
                // The document's trackMoved signal re-permutes the view
                // state on undo and redo too — the mute bit follows.
                doc.undoStack()->undo();
                if (!view.trackMuted(0) || view.trackMuted(1))
                    fail("undoing the move left the mute flag behind");
                doc.undoStack()->redo();
                if (!view.trackMuted(1) || view.trackMuted(0))
                    fail("redoing the move did not re-move the mute flag");
            }
            view.setTrackMute(1, false);
        }
    }

    const auto screenshotTick =
        uint64_t(std::ceil(std::max(0.0, view.tickAtContentX(view.width() / 2))));
    view.setPlayheadSample(timeline->sampleForTick(screenshotTick), false);
    // Park the cursor mid-roll so the shot shows the hover mark + name chip.
    sendMouse(roll, QEvent::MouseMove,
              QPoint(songview::kKeyboardW + 60, roll->height() / 3), Qt::NoButton,
              Qt::NoButton);
    const QImage image = view.grab().toImage();
    if (image.isNull())
        fail("offscreen render produced no image");
    if (!screenshotPath.isEmpty()) {
        image.save(screenshotPath);
        std::printf("rollcheck: wrote %s\n", qUtf8Printable(screenshotPath));
    }

    // Polyphony-dock jump target: revealNote selects the losing track and
    // the lost note itself (the last note on (track, key) starting at or
    // before the event tick), without touching the undo stack.
    {
        const auto &notes = view.model().notes;
        if (notes.empty()) {
            fail("no notes in the view model for revealNote");
        } else {
            const ViewNote target = notes[notes.size() / 2];
            if (!view.revealNote(target.track, target.key, target.startTick))
                fail("revealNote did not find the note");
            if (view.selectedTrack() != int(target.track))
                fail("revealNote did not select the track");
            const auto &sel = view.selection();
            if (sel.size() != 1 ||
                    !(sel[0] == SongView::NoteId{target.startTick, target.key}))
                fail("revealNote did not select the note");

            // A key the track never plays: no note found, but the track
            // selection sticks (the dock still switches context).
            bool used[128] = {};
            for (const ViewNote &note : notes) {
                if (note.track == target.track)
                    used[note.key] = true;
            }
            int freeKey = -1;
            for (int k = 0; k < 128 && freeKey < 0; k++) {
                if (!used[k])
                    freeKey = k;
            }
            if (freeKey >= 0) {
                if (view.revealNote(target.track, uint8_t(freeKey), target.startTick))
                    fail("revealNote found a note on an unused key");
                if (view.selectedTrack() != int(target.track))
                    fail("revealNote miss dropped the track selection");
            }
        }
    }

    // Keyboard mute/solo: bare M and S toggle the header buttons over the
    // multi-track scope — the selected track alone, or every Ctrl-scoped
    // row — with a mixed scope resolving toward on. View state only: the
    // undo stack must not move, and the header buttons follow the masks
    // without a panel rebuild.
    {
        const int preCount = doc.undoStack()->count();
        const int track = view.selectedTrack();
        if (view.muteMask() != 0 || view.soloMask() != 0)
            fail("mute/solo masks not clean before the keyboard toggles");
        sendKey(roll, Qt::Key_M, Qt::NoModifier);
        if (!view.trackMuted(track))
            fail("M did not mute the selected track");
        auto *row = view.findChild<QWidget *>(
            QStringLiteral("trackHeaderRow%1").arg(track));
        auto *muteButton = row ? row->findChild<QToolButton *>(
                                     QStringLiteral("trackMuteButton"))
                               : nullptr;
        if (!muteButton || !muteButton->isChecked())
            fail("keyboard mute did not check the header button");
        sendKey(roll, Qt::Key_M, Qt::NoModifier);
        if (view.muteMask() != 0)
            fail("second M did not unmute the selected track");
        if (muteButton && muteButton->isChecked())
            fail("keyboard unmute did not uncheck the header button");
        sendKey(roll, Qt::Key_S, Qt::NoModifier);
        if (!view.trackSoloed(track))
            fail("S did not solo the selected track");
        sendKey(roll, Qt::Key_S, Qt::NoModifier);
        if (view.soloMask() != 0)
            fail("second S did not unsolo the selected track");

        // Multi-track scope + mixed state: with another track Ctrl-scoped
        // in and already muted, M mutes the rest (on wins), and the next M
        // clears the whole scope.
        const int other = track == 0 ? 1 : 0;
        if (view.findChild<QWidget *>(
                QStringLiteral("trackHeaderRow%1").arg(other))) {
            view.trackHeaderClicked(other, Qt::ControlModifier);
            view.setTrackMute(other, true);
            sendKey(roll, Qt::Key_M, Qt::NoModifier);
            if (!view.trackMuted(track) || !view.trackMuted(other))
                fail("M over a mixed scope did not mute every scoped track");
            sendKey(roll, Qt::Key_M, Qt::NoModifier);
            if (view.muteMask() != 0)
                fail("second M did not unmute the whole scope");
            view.trackHeaderClicked(track, Qt::NoModifier); // collapse scope
        }
        if (doc.undoStack()->count() != preCount)
            fail("keyboard mute/solo touched the undo stack");
    }

    // Twenty commands: draw, set, draw, nudge, draw, the double-click
    // delete, the press-grown draw, the tiny-drag draw, the modifier
    // velocity nudge, the abutting-note fixture add, add, two resizes, the
    // three note-selection presses MERGED into one, the off-grid
    // behind-the-back move, Ctrl+Left (all the scroll-follow presses merge
    // into it), two time-selection moves (kept separate by the clean-index
    // save point), the inline rename, and the mid-song voice change — plus,
    // when the song has a second track, the header-drag track move and the
    // editor commit the drop flushes. Undoing them all must restore the
    // original bytes.
    int undos = 0;
    while (doc.undoStack()->canUndo() && undos < 100) {
        doc.undoStack()->undo();
        undos++;
    }
    if (undos != 20 + (reordered ? (dragRenamed ? 2 : 1) : 0))
        fail("gesture pass pushed an unexpected number of undo commands");
    if (doc.smf().write() != baseline)
        fail("undoing every gesture did not restore the original bytes");

    view.setDocument(nullptr);
    view.setSong(nullptr, nullptr);

    if (failures == 0)
        std::printf("rollcheck: OK %s (%lld ms)\n", qUtf8Printable(songLabel),
                    (long long)timer.elapsed());
    return failures ? 1 : 0;
}
