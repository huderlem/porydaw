#include "ui/theme/color_math.h"
#include "ui/theme/themeruntime.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFontMetrics>
#include <QIcon>
#include <QImage>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPoint>
#include <QPushButton>
#include <QRect>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "core/songdocument.h"
#include "project/decompproject.h"
#include "rollcheckplayhead.h"
#include "ui/keymap.h"
#include "ui/layout.h"
#include "ui/songview.h"
#include "ui/typography.h"

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
// a horizontal drag of layout Space::One grows it. Holding the
// roll.velocity_drag modifier chord (Ctrl by default) turns a vertical
// drag from anywhere on a note into an Ableton-style velocity drag; a
// modifier click without the drag keeps Ctrl's selection toggle, resolved
// on release. The edge resize grips stay live under Ctrl, but a
// bulk-select click landing on one joins the note to the selection
// instead of replacing it, and the drag resizes the whole selection;
// a Ctrl+velocity drag on an unselected note likewise joins it and nudges
// the whole selection, except that the first such drag after a completed
// modifier velocity edit in the same uninterrupted chord hold switches to
// its note instead of accumulating it. Ctrl+arrows transpose (Shift:
// octave) and nudge the selection along the same absolute grid — both the
// roll's note selection and a multi-track time selection — and the view follows
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
// The horizontal camera overshoots the song on both sides: the scroll
// floor is a lead pad of flat-shaded dead space before tick 0 (zooming
// near the song start clamps there with tick 0 still on screen; fresh
// songs and go-to-start home there) and the ceiling leaves a full
// viewport of scratch space past the song's end, where a pencil draw
// lands beyond the old length and grows the rebuilt timeline.
// Bare B toggles the automation pencil mode (never on auto-repeat): the
// lanes cursor becomes a pencil, a left press always freehand-draws (a
// press on an existing point's dot sweeps over it instead of grabbing),
// and holding Shift locks the stroke to a horizontal line at the value
// where the lock engaged; with the mode off, the dot grab-move and the
// Shift ramp behave as before. The key is a momentary chord besides the
// tap-toggle: holding it past the threshold, or drawing a lane stroke
// during the hold, reverts the mode on release (hold-to-draw).
// A lane sweep or Shift ramp crossing a time-signature change steps each
// side on its own segment's grid (the drawn grid restarts, and can change
// spacing, at the signature), the ramp's endpoint exact at the release
// tick. Shift on a point's dot grabs it into an axis-locked drag (first
// travel picks the axis, sticky until Shift releases); the ramp starts
// off-dot.
// Undoing every gesture must restore the original bytes.

namespace {

void sendMouse(QWidget *w, QEvent::Type type, QPoint pos, Qt::MouseButton button,
               Qt::MouseButtons buttons, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QMouseEvent ev(type, QPointF(pos), QPointF(w->mapToGlobal(pos)), button, buttons, mods);
    QCoreApplication::sendEvent(w, &ev);
}

void sendMouse(QWidget *w, QEvent::Type type, QPointF pos, Qt::MouseButton button,
               Qt::MouseButtons buttons, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QMouseEvent ev(type, pos, QPointF(w->mapToGlobal(pos.toPoint())), button, buttons, mods);
    QCoreApplication::sendEvent(w, &ev);
}

void sendWheel(QWidget *w, QPointF pos, int angleDeltaY, int pixelDeltaY = 0,
               Qt::KeyboardModifiers mods = Qt::ControlModifier, int pixelDeltaX = 0,
               int angleDeltaX = 0)
{
    QWheelEvent ev(pos, QPointF(w->mapToGlobal(pos.toPoint())), QPoint(pixelDeltaX, pixelDeltaY),
                   QPoint(angleDeltaX, angleDeltaY), Qt::NoButton, mods, Qt::NoScrollPhase, false);
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
    qreal edge(int row) const
    {
        return std::round((row * view.keyHeight() - view.scrollY()) * dpr()) / dpr();
    }
    qreal top(int key) const { return edge(127 - key); }
    qreal bottom(int key) const { return edge(128 - key); }
    int keyAt(qreal y) const
    {
        for (int row = 0; row < 128; ++row)
            if (y < edge(row + 1))
                return 127 - row;
        return 0;
    }
    int centerY(int key) const { return int(std::floor((top(key) + bottom(key)) / 2)); }
    QRectF noteRect(int x0, int x1, int key) const
    {
        return QRectF(x0, top(key) + pixel(), std::max(2, x1 - x0),
                      std::max(2.0 * pixel(), bottom(key) - top(key) - pixel()));
    }
    QRectF noteBox(const QRectF &rect) const { return rect.adjusted(0, 0, 0, -pixel()); }
    int noteTopProbeY(int key) const
    {
        return int(std::floor(noteRect(0, 1, key).top() + pixel()));
    }
};

void click(QWidget *w, QPoint pos)
{
    sendMouse(w, QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton);
    sendMouse(w, QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton);
}

// The pencil gesture: Qt replaces a fast second press with a DblClick event,
// and the note commits on the release that follows.
void drawNote(QWidget *w, QPoint pos)
{
    sendMouse(w, QEvent::MouseButtonDblClick, pos, Qt::LeftButton, Qt::LeftButton);
    sendMouse(w, QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton);
}

void sendKey(QWidget *w, int key, Qt::KeyboardModifiers mods)
{
    QKeyEvent press(QEvent::KeyPress, key, mods);
    QCoreApplication::sendEvent(w, &press);
    QKeyEvent release(QEvent::KeyRelease, key, mods);
    QCoreApplication::sendEvent(w, &release);
}

// Overlay verticals paint over the lane dots (drawOverlays runs after
// paintCurve): a probe dot must stay clear of the edit cursor's dashed
// line and the loop markers' edge/glow bands. The clearances mirror
// SongView's overlay paint geometry, so they live in one place.
bool overlayContestedX(SongView &view, qreal dpr, qreal x)
{
    const qreal cursorX = view.displayX(double(view.editCursorTick()), songview::kGutterW, dpr);
    if (std::abs(cursorX - x) < 12)
        return true;
    const MidiTimeline *tl = view.timeline();
    for (const uint64_t loopTick : {tl->loopStartTick, tl->loopEndTick}) {
        if (loopTick == UINT64_MAX)
            continue;
        const qreal loopX = view.displayX(double(loopTick), songview::kGutterW, dpr);
        if (x > loopX - 52 && x < loopX + 52)
            return true;
    }
    return false;
}

} // namespace

int runRollCheck(const QString &projectRoot, const QString &songLabel,
                 const QString &screenshotPath)
{
    // The roll consults keymap::Registry (Ctrl+arrow transposes, the
    // velocity-drag modifier chord), so redirect QSettings into a temp dir
    // first — a user's rebinds must not leak into the gesture assertions.
    QTemporaryDir settingsDir;
    if (settingsDir.isValid()) {
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
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
        std::fprintf(stderr, "rollcheck: no playable song %s\n", qUtf8Printable(songLabel));
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
        std::fprintf(stderr,
                     "rollcheck: FAIL %s: SongView constructor forced native window creation\n",
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
    // The global tempo row is hidden by default (the transport bar's Tempo
    // spinner covers the constant-tempo case); the row-geometry mirrors
    // below count it as row 0, so show it the way the add-lane menu would.
    if (view.tempoLaneVisible()) {
        std::fprintf(stderr, "rollcheck: FAIL %s: the tempo row did not start hidden\n",
                     qUtf8Printable(songLabel));
        return 1;
    }
    view.setTempoLaneVisible(true);
    (void)view.grab(); // force layout so child geometry is real

    int failures = 0;
    auto fail = [&](const char *what) {
        std::fprintf(stderr, "rollcheck: FAIL %s: %s\n", qUtf8Printable(songLabel), what);
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
        if (std::abs((anchor.y() + view.scrollY()) / view.keyHeight() - anchoredRow) > 1e-12)
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
        sendWheel(roll, QPointF(songview::kKeyboardW - 1.0, anchor.y()), 0, 1, Qt::NoModifier);
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
        const qreal boundary =
            std::round((boundaryRow * view.keyHeight() - view.scrollY()) * dpr) / dpr;
        sendMouse(roll, QEvent::MouseMove, QPointF(songview::kKeyboardW + 40.0, boundary - 0.25),
                  Qt::NoButton, Qt::NoButton);
        if (roll->property("hoverKey").toInt() != 128 - boundaryRow)
            fail("hovering above a snapped pitch boundary chose the wrong key");
        sendMouse(roll, QEvent::MouseMove, QPointF(songview::kKeyboardW + 40.0, boundary + 0.25),
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

    // The wheel actions are rebindable (Settings → Keyboard Shortcuts →
    // Mouse Wheel): Alt pans the roll vertically and Shift horizontally by
    // default, a chord matching no action moves nothing, and a rebound
    // chord takes the action with it.
    {
        const SongView::ViewState original = view.viewState();
        SongView::ViewState zoom = original;
        zoom.keyHeight = 8.0;
        zoom.scrollY = 300.0;
        zoom.scrollPx = 200.0;
        view.applyViewState(zoom);
        const QPointF anchor(songview::kKeyboardW + 40.0, 200.0);
        const double scale = view.pxPerBeat();

        sendWheel(roll, anchor, -120, 0, Qt::AltModifier);
        if (std::abs(view.scrollY() - 360.0) > 1e-12)
            fail("Alt-wheel did not pan the roll vertically");
        if (view.keyHeight() != 8.0 || view.pxPerBeat() != scale ||
            view.viewState().scrollPx != 200.0)
            fail("Alt-wheel pan moved a camera axis it does not own");
        sendWheel(roll, anchor, 120, 0, Qt::AltModifier);
        if (std::abs(view.scrollY() - 300.0) > 1e-12)
            fail("equal Alt-wheel pans did not restore the camera");

        // Qt's xcb and Windows plugins deliver Alt + a vertical notch as a
        // horizontal-only angleDelta; the roll must read it as the wheel's
        // own axis, not as a sideways trackpad pan.
        sendWheel(roll, anchor, 0, 0, Qt::AltModifier, 0, -120);
        if (std::abs(view.scrollY() - 360.0) > 1e-12 || view.viewState().scrollPx != 200.0)
            fail("platform-re-axised Alt-wheel did not pan the roll vertically");
        sendWheel(roll, anchor, 0, 0, Qt::AltModifier, 0, 120);
        if (std::abs(view.scrollY() - 300.0) > 1e-12)
            fail("equal re-axised Alt-wheel pans did not restore the camera");

        sendWheel(roll, anchor, -120, 0, Qt::ShiftModifier);
        if (std::abs(view.viewState().scrollPx - 320.0) > 1e-12 || view.pxPerBeat() != scale)
            fail("Shift-wheel did not pan the timeline");
        sendWheel(roll, anchor, 120, 0, Qt::ShiftModifier);

        // Ctrl+Shift matches no default wheel action: nothing may move.
        sendWheel(roll, anchor, 120, 0, Qt::ControlModifier | Qt::ShiftModifier);
        if (view.scrollY() != 300.0 || view.viewState().scrollPx != 200.0 ||
            view.keyHeight() != 8.0 || view.pxPerBeat() != scale)
            fail("a wheel chord bound to no action moved the camera");

        // Rebind: vertical pan moves to Ctrl+Shift, and Alt goes dead.
        auto &registry = keymap::Registry::instance();
        registry.setWheelBinding(QStringLiteral("wheel.pan_vertical"),
                                 Qt::ControlModifier | Qt::ShiftModifier);
        sendWheel(roll, anchor, -120, 0, Qt::AltModifier);
        if (view.scrollY() != 300.0)
            fail("the unbound Alt chord still panned the roll");
        sendWheel(roll, anchor, -120, 0, Qt::ControlModifier | Qt::ShiftModifier);
        if (std::abs(view.scrollY() - 360.0) > 1e-12)
            fail("the rebound wheel chord did not pan the roll");
        registry.resetBinding(QStringLiteral("wheel.pan_vertical"));

        view.applyViewState(original);
        (void)view.grab();
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
        const double expectedFullScale = zoom.pxPerBeat * std::pow(1.0015, 120.0);
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

            const qreal visibleWidth = qreal(roll->width() - songview::kKeyboardW);
            uint64_t tick = view.snapTickUp(std::max(0.0, view.tickAtContentX(0.0)));
            int visibleTicks = 0;
            bool mappingFailed = false;
            const double affineTick = view.tickAtContentX(visibleWidth * 0.371) + 0.375;
            if (std::abs(view.tickAtContentX(view.contentX(affineTick)) - affineTick) > 1e-9)
                fail("raw horizontal projection lost fractional tick precision");
            for (int guard = 0; guard < 10000; ++guard) {
                const qreal rawX = view.contentX(double(tick));
                if (rawX > visibleWidth)
                    break;
                if (rawX >= 0.0) {
                    visibleTicks++;
                    for (qreal origin : origins) {
                        for (qreal dpr : dprs) {
                            const qreal displayed = view.displayX(double(tick), origin, dpr);
                            const qreal expected = std::round((origin + rawX) * dpr) / dpr;
                            if (std::abs(displayed - expected) > 1e-12)
                                mappingFailed = true;
                            const uint64_t roundTrip =
                                view.snapTick(view.tickAtContentX(displayed - origin));
                            if (roundTrip != tick)
                                mappingFailed = true;
                        }
                    }
                }
                const uint64_t next = view.snapTickUp(double(tick) + 1.0);
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

    // The horizontal camera range: a lead pad of dead space before tick 0
    // (the scroll floor, where zooming near the song start comes to rest
    // with tick 0 still on screen) and a full viewport of scratch space
    // past the song's end (the ceiling rests the end at the content area's
    // left edge). The pad region paints as a flat shade distinct from the
    // roll background, and a negative camera round-trips through the
    // sidecar view state.
    {
        const SongView::ViewState original = view.viewState();
        const double pad = view.leadPadPx();
        if (pad <= 0.0)
            fail("lead pad is not positive");

        SongView::ViewState state = original;
        state.scrollPx = -1.0e9;
        view.applyViewState(state);
        if (std::abs(view.viewState().scrollPx + pad) > 1e-9)
            fail("horizontal scroll floor is not the lead pad");

        // Zooming in anchored inside the pad clamps at the floor instead of
        // pushing tick 0 off the left edge.
        sendWheel(roll, QPointF(songview::kKeyboardW + 2.0, 200.0), 120, 0, Qt::NoModifier);
        if (std::abs(view.viewState().scrollPx + pad) > 1e-9)
            fail("zoom near the song start left the lead-pad floor");
        view.applyViewState(state);

        state.scrollPx = 1.0e9;
        view.applyViewState(state);
        const double ceiling = double(timeline->lengthTicks) * view.pxPerTick();
        if (std::abs(view.viewState().scrollPx - ceiling) > 1e-9)
            fail("scroll ceiling is not a full viewport past the song end");

        view.goToStart();
        if (std::abs(view.viewState().scrollPx + pad) > 1e-9 || view.editCursorTick() != 0)
            fail("go-to-start did not home the camera to the lead pad");

        // A pixel wheel pans left into the pad from the classic origin.
        state.scrollPx = 0.0;
        view.applyViewState(state);
        sendWheel(roll, QPointF(songview::kKeyboardW + 40.0, 200.0), 0, 0, Qt::NoModifier, 8);
        if (std::abs(view.viewState().scrollPx + 8.0) > 1e-12)
            fail("wheel pan could not enter the lead pad");

        state.scrollPx = -pad / 2.0;
        view.applyViewState(state);
        if (std::abs(view.viewState().scrollPx + pad / 2.0) > 1e-12)
            fail("negative scroll did not round-trip through view state");

        // The pre-roll shade: flat (same color on natural and accidental
        // rows, hiding the row stripes) and distinct from the plain roll
        // background right of tick 0.
        state.scrollPx = -pad;
        view.applyViewState(state);
        const auto isBlackKey = [](int key) {
            switch (key % 12) {
            case 1:
            case 3:
            case 6:
            case 8:
            case 10:
                return true;
            default:
                return false;
            }
        };
        int naturalKey = -1, accidentalKey = -1;
        const int midKey = rows.keyAt(roll->height() / 2.0);
        for (int key = midKey - 4; key <= midKey + 4; ++key) {
            if (key < 0 || key > 127 || rows.top(key) < 0 || rows.bottom(key) > roll->height())
                continue;
            (isBlackKey(key) ? accidentalKey : naturalKey) = key;
        }
        if (naturalKey < 0 || accidentalKey < 0) {
            fail("no visible natural/accidental row pair for the pre-roll probe");
        } else {
            const QPixmap padPixmap = roll->grab();
            const QImage padImage = padPixmap.toImage();
            const qreal padDpr = padPixmap.devicePixelRatio();
            const auto raster = [padDpr](qreal position) { return qRound(position * padDpr); };
            const qreal inPadX = songview::kKeyboardW + pad / 2.0;
            // Mid snap-cell right of tick 0, clear of the 2px grid lines.
            const qreal outPadX = songview::kKeyboardW + pad + 20.0;
            const QRgb padNatural =
                padImage.pixel(raster(inPadX), raster(rows.centerY(naturalKey)));
            const QRgb padAccidental =
                padImage.pixel(raster(inPadX), raster(rows.centerY(accidentalKey)));
            const QRgb plainNatural =
                padImage.pixel(raster(outPadX), raster(rows.centerY(naturalKey)));
            if (padNatural != padAccidental)
                fail("pre-roll shade is not flat across row stripes");
            if (padNatural == plainNatural)
                fail("pre-roll shade does not differ from the roll background");
        }

        view.applyViewState(original);
        (void)view.grab();
        QCoreApplication::processEvents();
    }

    // The scratch space past the song's end is editable: from the ceiling
    // camera a pencil draw lands beyond the pre-edit song length, and the
    // rebuilt timeline grows to include it (renewing the overshoot range).
    {
        const SongView::ViewState original = view.viewState();
        const uint64_t lengthBefore = timeline->lengthTicks;
        const QByteArray beforeProbe = doc.smf().write();
        const int undoIndex = doc.undoStack()->index();

        SongView::ViewState state = original;
        state.scrollPx = 1.0e9;
        view.applyViewState(state);

        // Mid-viewport at the ceiling is all past the song end.
        const double probeX = songview::kKeyboardW + (roll->width() - songview::kKeyboardW) / 2.0;
        const uint64_t tick = view.snapTickDown(view.tickAtContentX(probeX - songview::kKeyboardW));
        const int key = rows.keyAt(roll->height() / 2.0);
        const qreal x0 = songview::kKeyboardW + view.contentX(double(tick));
        const qreal xs =
            songview::kKeyboardW + view.contentX(double(tick + view.snapTicksAt(tick)));
        drawNote(roll, QPoint(int((x0 + xs) / 2.0), int(rows.centerY(key))));

        DocNote scratch;
        if (tick < lengthBefore)
            fail("ceiling-camera probe cell is not past the song end");
        else if (!doc.findNote(track, tick, uint8_t(key), &scratch))
            fail("pencil draw in the scratch space produced no note");
        else if (timeline->lengthTicks <= lengthBefore)
            fail("scratch-space note did not grow the rebuilt timeline");

        while (doc.undoStack()->index() > undoIndex && doc.undoStack()->canUndo())
            doc.undoStack()->undo();
        if (doc.smf().write() != beforeProbe)
            fail("undo did not restore the document after the scratch-space draw");

        view.applyViewState(original);
        (void)view.grab();
        QCoreApplication::processEvents();
    }

    // Hover readout: the cursor anywhere over the roll marks its key row
    // on the keyboard column (mirrored in the hoverKey property); leaving
    // the widget clears the mark.
    {
        const int y = roll->height() / 2;
        const int expected = rows.keyAt(y);
        sendMouse(roll, QEvent::MouseMove, QPoint(songview::kKeyboardW + 40, y), Qt::NoButton,
                  Qt::NoButton);
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
    auto occupied = [&](uint64_t tick, uint64_t dur, int key, bool checkAllTracks = false) {
        const int startTrack = checkAllTracks ? 0 : track;
        const int endTrack = checkAllTracks ? doc.engineTrackCount() : track + 1;
        for (int t = startTrack; t < endTrack; ++t) {
            for (const DocNote &note : doc.notesForTrack(t)) {
                if (int(note.key) != key)
                    continue;
                const uint64_t end =
                    note.unterminated() ? UINT64_MAX : note.tick + note.duration + dur;
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
    auto findFreeCell = [&](int firstProbe = 8, bool checkAllTracks = false) -> Cell {
        Cell cell;
        for (int key = 115; key >= 24; key--) {
            const qreal top = rows.top(key);
            const qreal bottom = rows.bottom(key);
            if (top < 0 || bottom > roll->height())
                continue;
            for (int probe = firstProbe; probe < roll->width() - songview::kKeyboardW - 40;
                 probe += 24) {
                const uint64_t tick = view.snapTickDown(view.tickAtContentX(probe));
                const uint64_t dur = view.gridTicksAt(tick);
                const int x0 = songview::kKeyboardW + view.contentX(double(tick));
                const int x1 = songview::kKeyboardW + view.contentX(double(tick + dur));
                const int xs =
                    songview::kKeyboardW + view.contentX(double(tick + view.snapTicksAt(tick)));
                // Wide enough that the click target clears the 3px resize
                // edges once a note fills the cell.
                if (x0 < songview::kKeyboardW || x1 - x0 < 12 || xs - x0 < 8 || x1 >= roll->width())
                    continue;
                if (occupied(tick, dur, key, checkAllTracks))
                    continue;
                // Like the parked playhead/edit cursor, the loop markers'
                // opaque lines must stay outside the probed span (which can
                // cover this cell plus an abutting one): an overlay line
                // paints the same color over background and note, reading
                // as an unpainted column to the before/after comparisons.
                const auto markerInSpan = [&](uint64_t markerTick) {
                    return markerTick != UINT64_MAX && markerTick >= tick &&
                           markerTick <= tick + 2 * dur;
                };
                if (markerInSpan(timeline->loopStartTick) || markerInSpan(timeline->loopEndTick))
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
            uint64_t tick = view.snapTickUp(std::max(0.0, view.tickAtContentX(4.0)));
            for (int guard = 0; guard < 1000; ++guard) {
                const uint64_t next = view.snapTickUp(double(tick) + 1.0);
                if (next <= tick)
                    break;
                const qreal leftX = view.displayX(double(tick), origin, dpr);
                const qreal rightX = view.displayX(double(next), origin, dpr);
                if (leftX > rightLimit)
                    break;
                const uint64_t dur = view.gridTicksAt(tick);
                const uint64_t previous = tick == 0 ? tick : view.snapTickDown(double(tick) - 1.0);
                if (leftX >= origin + 4.0 && rightX <= rightLimit && rightX - leftX >= 4.0 &&
                    !occupied(tick, dur, key)) {
                    const qreal centerX = (leftX + rightX) / 2.0;
                    if (std::abs(centerX - std::round(centerX)) < 1e-12) {
                        tick = next;
                        continue;
                    }
                    const QPointF center(centerX, (top + bottom) / 2.0);
                    if (view.snapTickDown(view.tickAtContentX(center.x() - origin)) == tick) {
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
            sendMouse(roll, QEvent::MouseButtonDblClick, probe.center, Qt::LeftButton,
                      Qt::LeftButton);
            sendMouse(roll, QEvent::MouseButtonRelease, probe.center, Qt::LeftButton, Qt::NoButton);

            DocNote exact;
            if (!doc.findNote(track, probe.tick, uint8_t(probe.key), &exact))
                fail("fractional displayed-cell edit saved at the wrong tick");
            DocNote neighbor;
            const bool atPrevious =
                probe.previous != probe.tick &&
                doc.findNote(track, probe.previous, uint8_t(probe.key), &neighbor);
            const bool atNext = doc.findNote(track, probe.next, uint8_t(probe.key), &neighbor);
            if (atPrevious || atNext)
                fail("fractional displayed-cell edit saved in a neighboring cell");

            if (doc.undoStack()->index() <= undoIndex)
                fail("fractional displayed-cell edit pushed no undo command");
            while (doc.undoStack()->index() > undoIndex && doc.undoStack()->canUndo())
                doc.undoStack()->undo();

            DocNote residue;
            const bool exactResidue = doc.findNote(track, probe.tick, uint8_t(probe.key), &residue);
            const bool previousResidue =
                probe.previous != probe.tick &&
                doc.findNote(track, probe.previous, uint8_t(probe.key), &residue);
            const bool nextResidue = doc.findNote(track, probe.next, uint8_t(probe.key), &residue);
            if (exactResidue || previousResidue || nextResidue)
                fail("undo left the fractional displayed-cell probe in the document");
            if (doc.undoStack()->index() != undoIndex || doc.smf().write() != beforeProbe ||
                view.document() != &doc || !view.timeline())
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
    const auto toRasterPixel = [rasterDpr](qreal position) { return qRound(position * rasterDpr); };
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
    const int noteLeftX = songview::kKeyboardW + view.contentX(double(noteA.tick));
    const int noteRightX =
        songview::kKeyboardW + view.contentX(double(noteA.tick + noteA.duration));
    const QRectF noteFrame = rows.noteRect(noteLeftX, noteRightX, noteA.key);
    const QRectF paintedNoteBox = rows.noteBox(noteFrame);
    const int noteLeftPixel = toRasterPixel(noteFrame.left());
    const int noteRightPixel = toRasterPixel(noteFrame.right());
    const int noteTopPixel = toRasterPixel(noteFrame.top());
    const int noteFrameBottomPixel = toRasterPixel(noteFrame.bottom());
    const int paintedNoteBottomPixel = toRasterPixel(paintedNoteBox.bottom());
    bool paintEscapedInteractionRect = false;
    for (int y = noteTopPixel; y < noteFrameBottomPixel; ++y) {
        paintEscapedInteractionRect |=
            rollAfterDrawing.pixel(noteRightPixel, y) != rollBeforeDrawing.pixel(noteRightPixel, y);
    }
    for (int x = noteLeftPixel; x < noteRightPixel; ++x) {
        paintEscapedInteractionRect |= rollAfterDrawing.pixel(x, paintedNoteBottomPixel) !=
                                       rollBeforeDrawing.pixel(x, paintedNoteBottomPixel);
    }
    if (paintEscapedInteractionRect)
        fail("note color escaped past its black box");

    const QRectF twoPixelBarNoteRect(noteFrame.left(), noteFrame.top(), noteFrame.width(),
                                     20 * rows.pixel());
    const QRectF twoPixelBarNoteBox = rows.noteBox(twoPixelBarNoteRect);
    const QRectF velocityZeroBar = songview::velBarRect(twoPixelBarNoteRect, 0, rows.dpr());
    if (qRound(velocityZeroBar.height() / rows.pixel()) != 2 ||
        velocityZeroBar.left() < twoPixelBarNoteBox.left() ||
        velocityZeroBar.right() > twoPixelBarNoteBox.right() ||
        velocityZeroBar.top() < twoPixelBarNoteBox.top() ||
        velocityZeroBar.bottom() > twoPixelBarNoteBox.bottom())
        fail("two-pixel velocity-zero bar escaped painted note box");
    // Timeline overlays are composited above notes and can tint frame colors
    // by a few channel values.
    const auto isBlackBorder = [](QRgb pixel) {
        return qRed(pixel) <= 16 && qGreen(pixel) <= 16 && qBlue(pixel) <= 16;
    };
    const QColor selectionRingColor = themes::color(themes::Role::item_selected_background);
    const auto isSelectionRingColor = [selectionRingColor](QRgb pixel) {
        const QColor actualColor(pixel);
        return std::abs(actualColor.red() - selectionRingColor.red()) <= 16 &&
               std::abs(actualColor.green() - selectionRingColor.green()) <= 16 &&
               std::abs(actualColor.blue() - selectionRingColor.blue()) <= 16;
    };

    const QColor velocityZeroColor = SongView::noteColor(track, 0);
    const QColor velocityMaximumColor = SongView::noteColor(track, 127);
    const QColor velocityMidpointColor = SongView::noteColor(track, 64);
    const QColor velocityZeroThemeColor = themes::color(themes::Role::song_view_note_velocity_zero);
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
    if (velocityMidpointColor == velocityZeroColor || velocityMidpointColor == velocityMaximumColor)
        fail("intermediate velocity note color equals endpoint color");

    const QColor expectedNoteColor = SongView::noteColor(track, 100);
    const QPoint noteInteriorSample(toRasterPixel(paintedNoteBox.center().x()),
                                    toRasterPixel(paintedNoteBox.center().y()));
    if (QColor(rollAfterDrawing.pixel(noteInteriorSample)) != expectedNoteColor)
        fail("note interior color does not match noteColor(track, 100)");

    // A note ending exactly where the next begins must paint every column
    // across the pair — no reserved background column that reads as a rest
    // between them. (findFreeCell guaranteed the adjacent cell is empty.)
    doc.addNote(track, noteA.tick + noteA.duration, noteA.key, noteA.duration, 100);
    const int abuttingRightX =
        songview::kKeyboardW + view.contentX(double(noteA.tick + 2 * noteA.duration));
    const QImage abuttingImage = roll->grab().toImage();
    const int abuttingMidY = toRasterPixel(rows.centerY(noteA.key));
    const int abuttingRightPixel = toRasterPixel(abuttingRightX);
    bool restGapFound = false;
    for (int x = noteLeftPixel; x < abuttingRightPixel; ++x) {
        restGapFound |=
            abuttingImage.pixel(x, abuttingMidY) == rollBeforeDrawing.pixel(x, abuttingMidY);
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
            std::max(0.0, (127.5 - double(noteA.key)) * tinyView.keyHeight - roll->height() / 2.0);
        view.applyViewState(tinyView);
        const SnappedRows tinyRows{view, *roll};
        const QRectF tinyBox =
            tinyRows.noteBox(tinyRows.noteRect(noteRightX, abuttingRightX, noteA.key));
        QImage tinyImage(roll->size(), QImage::Format_ARGB32_Premultiplied);
        tinyImage.fill(Qt::transparent);
        roll->render(&tinyImage);
        const int tinyCenterX = qRound(tinyBox.center().x());
        if (!isBlackBorder(tinyImage.pixel(tinyCenterX, qRound(tinyBox.top()))))
            fail("tiny note lost its border instead of thinning it");
        if (isBlackBorder(tinyImage.pixel(tinyCenterX, qRound(tinyBox.top()) + 1)))
            fail("tiny note border swallowed the note face");
        view.applyViewState(originalView);
    }

    // Probe the selected 3px ring, its 2px black inset, and the unselected
    // bottom edge with the camera centered at a fractional scale.
    {
        const SongView::ViewState originalView = view.viewState();
        SongView::ViewState fractionalView = originalView;
        fractionalView.keyHeight = 16.375;
        fractionalView.scrollY = std::max(
            0.0, (127.5 - double(noteA.key)) * fractionalView.keyHeight - roll->height() / 2.0);
        view.applyViewState(fractionalView);

        const SnappedRows fractionalRows{view, *roll};
        const QRectF fractionalNoteBox =
            fractionalRows.noteBox(fractionalRows.noteRect(noteLeftX, noteRightX, noteA.key));
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
                    selectedNoteImage.pixel(centerPixelX, topPixel + ringPixel)) ||
                !isSelectionRingColor(
                    selectedNoteImage.pixel(centerPixelX, bottomPixel - 1 - ringPixel))) {
                fail("selected note frame is not a contiguous selection ring");
            }
        }
        for (int borderPixel = 0; borderPixel < borderPixels; ++borderPixel) {
            if (!isBlackBorder(
                    selectedNoteImage.pixel(centerPixelX, topPixel + ringPixels + borderPixel)))
                fail("selected note did not have an inset black top border");
            if (!isBlackBorder(selectedNoteImage.pixel(centerPixelX,
                                                       bottomPixel - 1 - ringPixels - borderPixel)))
                fail("selected note did not have an inset black bottom border");
            if (!isBlackBorder(
                    selectedNoteImage.pixel(leftPixel + ringPixels + borderPixel, centerPixelY)))
                fail("selected note did not have an inset black left border");
            if (!isBlackBorder(selectedNoteImage.pixel(rightPixel - 1 - ringPixels - borderPixel,
                                                       centerPixelY)))
                fail("selected note did not have an inset black right border");
        }
        // The ring must stop where the black border starts.
        if (isSelectionRingColor(selectedNoteImage.pixel(centerPixelX, topPixel + ringPixels)))
            fail("selection ring is thicker than its display-scaled weight");

        view.clearSelection();
        const QImage unselectedNoteImage = roll->grab().toImage();
        for (int borderPixel = 0; borderPixel < borderPixels; ++borderPixel) {
            if (!isBlackBorder(
                    unselectedNoteImage.pixel(centerPixelX, bottomPixel - 1 - borderPixel)))
                fail("unselected note lacks its black bottom border");
        }
        if (QColor(unselectedNoteImage.pixel(centerPixelX, bottomPixel)) == expectedNoteColor) {
            fail("unselected note face appears below its black bottom border");
        }

        view.applyViewState(originalView);
        QCoreApplication::processEvents();
    }

    const int selectedTrackBeforeGhostProbe = view.selectedTrack();
    const int ghostTrack = (selectedTrackBeforeGhostProbe + 1) % doc.engineTrackCount();
    view.selectTrack(ghostTrack);
    const QImage ghostNoteRender = roll->grab().toImage();
    const int ghostCenterX = toRasterPixel(paintedNoteBox.center().x());
    const int ghostTopPixel = toRasterPixel(paintedNoteBox.top());
    const int ghostBottomPixel = toRasterPixel(paintedNoteBox.bottom()) - 1;
    const QRgb ghostTopEdge = ghostNoteRender.pixel(ghostCenterX, ghostTopPixel);
    const QRgb ghostTopInterior = ghostNoteRender.pixel(ghostCenterX, ghostTopPixel + 2);
    const QRgb ghostBottomEdge = ghostNoteRender.pixel(ghostCenterX, ghostBottomPixel);
    const QRgb ghostBottomInterior = ghostNoteRender.pixel(ghostCenterX, ghostBottomPixel - 2);

    if (ghostTopEdge != ghostTopInterior || ghostBottomEdge != ghostBottomInterior)
        fail("ghost note face edge does not match adjacent interior pixel");

    // Velocity-color display mode (View menu, app-wide): the active track's
    // note fills take their hue from velocity — exact purple and red
    // endpoints, the hue falling monotonically through the spectrum between
    // — while ghost notes keep the identity rendering byte-for-byte.
    if (SongView::velocityNoteColor(1) != QColor(0x5f, 0x44, 0xe9))
        fail("velocity 1 fill is not the purple endpoint #5f44e9");
    if (SongView::velocityNoteColor(127) != QColor(0xe9, 0x09, 0x04))
        fail("velocity 127 fill is not the red endpoint #e90904");
    if (SongView::velocityNoteColor(0) != themes::color(themes::Role::song_view_note_velocity_zero))
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
    if (ghostVelocityRender.pixel(ghostCenterX, ghostTopPixel) != ghostTopEdge ||
        ghostVelocityRender.pixel(ghostCenterX, ghostTopPixel + 2) != ghostTopInterior ||
        ghostVelocityRender.pixel(ghostCenterX, ghostBottomPixel) != ghostBottomEdge ||
        ghostVelocityRender.pixel(ghostCenterX, ghostBottomPixel - 2) != ghostBottomInterior)
        fail("velocity-color mode changed a ghost note's rendering");

    view.selectTrack(selectedTrackBeforeGhostProbe);
    const QImage velocityModeRender = roll->grab().toImage();
    if (QColor(velocityModeRender.pixel(noteInteriorSample)) != SongView::velocityNoteColor(100))
        fail("velocity-mode note interior does not match velocityNoteColor(100)");

    view.setVelocityColorMode(false);
    const QImage identityRestoredRender = roll->grab().toImage();
    if (QColor(identityRestoredRender.pixel(noteInteriorSample)) != expectedNoteColor)
        fail("disabling velocity-color mode did not restore identity fills");

    // Note-name display mode (View menu, app-wide): with rows tall enough
    // for legible text, each visible active-track note independently carries
    // its pitch name when its face fits the complete name plus two trailing
    // spaces; ghost notes never do; below the key-height floor, labels vanish
    // individually.
    {
        const auto differingPixels = [](const QImage &before, const QImage &after,
                                        const QRect &region) {
            int count = 0;
            for (int y = region.top(); y <= region.bottom(); ++y)
                for (int x = region.left(); x <= region.right(); ++x)
                    count += before.pixel(x, y) != after.pixel(x, y);
            return count;
        };
        const SongView::ViewState viewBeforeNames = view.viewState();
        SongView::ViewState namedState = viewBeforeNames;
        namedState.keyHeight = 24.0;
        namedState.scrollY = std::max(0.0, (127.5 - double(noteA.key)) * namedState.keyHeight -
                                               roll->height() / 2.0);
        view.applyViewState(namedState);
        const SnappedRows namedRows{view, *roll};
        const QRectF namedNoteBox =
            namedRows.noteBox(namedRows.noteRect(noteLeftX, noteRightX, noteA.key));
        const QRect noteARegion(
            QPoint(toRasterPixel(namedNoteBox.left()), toRasterPixel(namedNoteBox.top())),
            QPoint(toRasterPixel(namedNoteBox.right()) - 1,
                   toRasterPixel(namedNoteBox.bottom()) - 1));
        view.setNoteNameMode(true);
        const QImage namesOnRender = roll->grab().toImage();

        // With the other track selected note A is a ghost, and its face must
        // render identically with the mode on or off.
        view.selectTrack(ghostTrack);
        const QImage ghostNamedRender = roll->grab().toImage();
        view.setNoteNameMode(false);
        if (differingPixels(roll->grab().toImage(), ghostNamedRender, noteARegion) != 0)
            fail("note-name mode changed a ghost note's rendering");
        view.setNoteNameMode(true);
        view.selectTrack(selectedTrackBeforeGhostProbe);

        // The fixed label face and its padded height, shared with the
        // short-row probes below (which need a label-wide note in view, so
        // they run inside the width-probe scene).
        const auto labelPadding = layout::space(layout::Space::Half);
        auto fixedLabelFont = typography::noteName(roll->font());
        fixedLabelFont.setPixelSize(std::max(layout::singlePixel(), fixedLabelFont.pixelSize() -
                                                                        2 * layout::singlePixel()));
        const auto fixedLabelMetrics = QFontMetrics(fixedLabelFont);
        const auto fixedLabelHeight = fixedLabelMetrics.ascent() + fixedLabelMetrics.descent();

        // Per-note width probe: an abutting short pair followed by a distant
        // note wide enough for its name. The pair stays unlabeled while the
        // distant wide note keeps its label.
        const qreal pxPerTick = view.contentX(1.0) - view.contentX(0.0);
        const auto closeTicks = uint64_t(std::max(1.0, std::ceil(5.0 / pxPerTick)));
        const auto labelProbeWidth = 3 * layout::space(layout::Space::Eight);
        const auto labelTicks = uint64_t(std::ceil(labelProbeWidth / pxPerTick));
        const auto farTicks = uint64_t(std::ceil(90.0 / pxPerTick));
        const uint64_t runTick2 = a.tick + closeTicks;
        const uint64_t runTick3 = runTick2 + farTicks;
        const uint64_t runTick4 = runTick3 + labelTicks + closeTicks;
        int runKey = -1;
        for (int key = 115; key >= 24 && runKey < 0; --key) {
            if (namedRows.top(key) < 0.0 || namedRows.bottom(key) > roll->height())
                continue;
            if (!occupied(a.tick, 3 * closeTicks + farTicks + 2 * labelTicks, key))
                runKey = key;
        }
        const int stripW = qRound(12.0 * rasterDpr);
        const QRectF runRowBox =
            namedRows.noteBox(namedRows.noteRect(0.0, 1.0, runKey < 0 ? 60 : runKey));
        const int runRowTop = toRasterPixel(runRowBox.top());
        const int runRowBottom = toRasterPixel(runRowBox.bottom()) - 1;
        const auto labelStrip = [&](uint64_t tick, int width) {
            const int left = toRasterPixel(songview::kKeyboardW + view.contentX(double(tick)));
            return QRect(QPoint(left, runRowTop), QPoint(left + width - 1, runRowBottom));
        };
        if (runKey < 0 || closeTicks * pxPerTick > 12.0 ||
            labelStrip(runTick4, stripW).right() >= namesOnRender.width()) {
            fail("no room for the note-name width probe");
        } else {
            const int undoIndexBeforeRun = doc.undoStack()->index();
            doc.addNotes(track, {{a.tick, uint8_t(runKey), uint32_t(closeTicks), 100},
                                 {runTick2, uint8_t(runKey), uint32_t(closeTicks), 100},
                                 {runTick3, uint8_t(runKey), uint32_t(labelTicks), 100},
                                 {runTick4, uint8_t(runKey), uint32_t(labelTicks), 1}});
            const QImage runNamed = roll->grab().toImage();
            view.setNoteNameMode(false);
            const QImage runUnnamed = roll->grab().toImage();
            const QRect firstStrip(QPoint(labelStrip(a.tick, 1).left(), runRowTop),
                                   QPoint(labelStrip(runTick2, 1).left() - 1, runRowBottom));
            if (differingPixels(runUnnamed, runNamed, firstStrip) != 0)
                fail("a short same-pitch note was labeled");
            if (differingPixels(runUnnamed, runNamed, labelStrip(runTick2, stripW)) != 0)
                fail("a short same-pitch note was labeled");
            const auto wideLabelRegion = labelStrip(runTick3, stripW);
            if (differingPixels(runUnnamed, runNamed, wideLabelRegion) == 0) {
                fail("a distant note with enough label width lost its label");
            } else {
                bool wideLabelContrasts = false;
                for (int y = wideLabelRegion.top();
                     y <= wideLabelRegion.bottom() && !wideLabelContrasts; ++y)
                    for (int x = wideLabelRegion.left();
                         x <= wideLabelRegion.right() && !wideLabelContrasts; ++x)
                        wideLabelContrasts = runNamed.pixel(x, y) != runUnnamed.pixel(x, y) &&
                                             themes::contrastRatio(QColor(runNamed.pixel(x, y)),
                                                                   expectedNoteColor) >= 2.5;
                if (!wideLabelContrasts)
                    fail("no clearly contrasting label ink on a wide note face");
            }

            // The width probe's last grab left the mode off.
            view.setNoteNameMode(true);

            // The fixed face labels the wide note at its exact padded fit...
            const auto centeredOnRun = [&](double keyHeight) {
                SongView::ViewState state = namedState;
                state.keyHeight = keyHeight;
                state.scrollY =
                    std::max(0.0, (127.5 - double(runKey)) * keyHeight - roll->height() / 2.0);
                return state;
            };
            view.applyViewState(centeredOnRun(double(fixedLabelHeight + 2 * labelPadding + 1)));
            const QImage fitRowsNamed = roll->grab().toImage();
            view.setNoteNameMode(false);
            const QImage fitRowsUnnamed = roll->grab().toImage();
            if (fitRowsUnnamed == fitRowsNamed)
                fail("no label at the exact padded label fit");
            view.setNoteNameMode(true);

            // At this height the wide note's velocity bar crosses the label
            // rows. The label sits on a plate of the plain fill, so inside
            // the label strip the bar must give way to fill pixels — ink is
            // never read against the bar.
            const SnappedRows fitRowsGrid{view, *roll};
            const QRectF wideRect = fitRowsGrid.noteRect(
                songview::kKeyboardW + view.contentX(double(runTick3)),
                songview::kKeyboardW + view.contentX(double(runTick3 + labelTicks)), runKey);
            const QRectF barRect = songview::velBarRect(wideRect, 100, fitRowsGrid.dpr());
            const QRect stripX = labelStrip(runTick3, stripW);
            bool plateUnderText = false;
            for (int y = toRasterPixel(barRect.top());
                 y < toRasterPixel(barRect.bottom()) && !plateUnderText; ++y)
                for (int x = stripX.left(); x <= stripX.right() && !plateUnderText; ++x)
                    plateUnderText = QColor(fitRowsNamed.pixel(x, y)) == expectedNoteColor &&
                                     QColor(fitRowsUnnamed.pixel(x, y)) != expectedNoteColor;
            if (!plateUnderText)
                fail("no fill plate under the label across the velocity bar");

            // ...and one layout pixel shorter it hides rather than shrinks.
            view.applyViewState(centeredOnRun(double(fixedLabelHeight + 2 * labelPadding)));
            const QImage shortRowsNamed = roll->grab().toImage();
            view.setNoteNameMode(false);
            if (roll->grab().toImage() != shortRowsNamed)
                fail("note names shrank to fit a short row");
            view.setNoteNameMode(true);
            view.applyViewState(namedState);

            // Velocity-color fills span the whole spectrum, so label ink must
            // be picked per fill: both the bright high-velocity note and the
            // dark low-velocity note need clearly readable ink. The piano-key
            // ink pick clears 4:1 on both probed fills (its floor across the
            // whole ramp is ~3.8:1, in the deep reds near velocity 121);
            // either fixed ink drops below 4:1 on one of the two fills.
            view.setVelocityColorMode(true);
            const QImage velNamed = roll->grab().toImage();
            view.setNoteNameMode(false);
            const QImage velUnnamed = roll->grab().toImage();
            view.setNoteNameMode(true);
            view.setVelocityColorMode(false);
            const auto bestInkContrast = [&](const QRect &region, const QColor &fill) {
                double best = 0.0;
                for (int y = region.top(); y <= region.bottom(); ++y)
                    for (int x = region.left(); x <= region.right(); ++x)
                        if (velNamed.pixel(x, y) != velUnnamed.pixel(x, y))
                            best = std::max(
                                best, themes::contrastRatio(QColor(velNamed.pixel(x, y)), fill));
                return best;
            };
            if (bestInkContrast(labelStrip(runTick3, stripW), SongView::velocityNoteColor(100)) <
                4.0)
                fail("label ink is not picked against the bright velocity fill");
            if (bestInkContrast(labelStrip(runTick4, stripW), SongView::velocityNoteColor(1)) < 4.0)
                fail("label ink is not picked against the dark velocity fill");

            while (doc.undoStack()->index() > undoIndexBeforeRun && doc.undoStack()->canUndo())
                doc.undoStack()->undo();
        }
        view.applyViewState(viewBeforeNames);
    }

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
    sendMouse(roll, QEvent::MouseButtonPress, b.center, Qt::RightButton, Qt::RightButton);
    sendMouse(roll, QEvent::MouseButtonRelease, b.center, Qt::RightButton, Qt::NoButton);
    QCoreApplication::processEvents();
    auto *noteMenu = roll->findChild<QMenu *>();
    if (!noteMenu || !noteMenu->isVisible()) {
        fail("right-click did not open the note menu");
    } else {
        const QPoint aGlobal = roll->mapToGlobal(a.center);
        sendMouse(noteMenu, QEvent::MouseButtonPress, noteMenu->mapFromGlobal(aGlobal),
                  Qt::RightButton, Qt::RightButton);
        sendMouse(noteMenu, QEvent::MouseButtonRelease, noteMenu->mapFromGlobal(aGlobal),
                  Qt::RightButton, Qt::NoButton);
        QCoreApplication::processEvents();
        const std::vector<SongView::NoteKey> &selection = view.selection();
        const SongView::NoteKey aId{uint32_t(a.tick), uint8_t(a.key)};
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
        const QPoint clearGlobal = roll->mapToGlobal(QPoint(a.center.x(), rows.centerY(clearKey)));
        sendMouse(noteMenu, QEvent::MouseButtonPress, noteMenu->mapFromGlobal(clearGlobal),
                  Qt::RightButton, Qt::RightButton);
        sendMouse(noteMenu, QEvent::MouseButtonRelease, noteMenu->mapFromGlobal(clearGlobal),
                  Qt::RightButton, Qt::NoButton);
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
    const QPoint bHandle(b.center.x(),
                         qRound(songview::velBarRect(bRect, 73, rows.dpr()).center().y()));
    sendMouse(roll, QEvent::MouseButtonPress, bHandle, Qt::LeftButton, Qt::LeftButton);
    sendMouse(roll, QEvent::MouseMove, bHandle - QPoint(0, 20), Qt::NoButton, Qt::LeftButton);
    // The cursor sits rows above the note now, but the hover mark pins to
    // the note's own pitch for the whole velocity drag.
    if (roll->property("hoverKey").toInt() != b.key)
        fail("velocity drag did not pin the hover mark to the note's key");
    sendMouse(roll, QEvent::MouseButtonRelease, bHandle - QPoint(0, 20), Qt::LeftButton,
              Qt::NoButton);
    DocNote dragged;
    if (!doc.findNote(track, b.tick, uint8_t(b.key), &dragged) || dragged.velocity != 93)
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
        sendMouse(roll, QEvent::MouseButtonPress, bTop, Qt::LeftButton, Qt::LeftButton);
        const QPoint movedTop(bTop.x(), rows.noteTopProbeY(b.key + 2));
        sendMouse(roll, QEvent::MouseMove, movedTop, Qt::NoButton, Qt::LeftButton);
        sendMouse(roll, QEvent::MouseButtonRelease, movedTop, Qt::LeftButton, Qt::NoButton);
        if (doc.findNote(track, b.tick, uint8_t(b.key), &bNow))
            fail("top-of-note drag on a low-velocity note did not move the "
                 "note (velocity handle still on the top strip?)");
        doc.undoStack()->undo(); // the move
        doc.undoStack()->undo(); // the velocity-20 set
        click(roll, b.center);   // re-latch 93 for the sections below
    }

    // Double-click on a note deletes it (the pencil sections above prove
    // the same event still draws over empty space). Note C goes.
    sendMouse(roll, QEvent::MouseButtonDblClick, c.center, Qt::LeftButton, Qt::LeftButton);
    sendMouse(roll, QEvent::MouseButtonRelease, c.center, Qt::LeftButton, Qt::NoButton);
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
        sendMouse(roll, QEvent::MouseButtonPress, sweepStart, Qt::RightButton, Qt::RightButton);
        sendMouse(roll, QEvent::MouseMove, a.center + QPoint(4, 4), Qt::NoButton, Qt::RightButton);
        if (std::find(onKeys.begin(), onKeys.end(), a.key) == onKeys.end())
            fail("sweeping the band over a note did not audition it");
        // Retreat to a band covering nothing: the departed notes' previews
        // must release now, not ring out their durations.
        sendMouse(roll, QEvent::MouseMove, sweepStart + QPoint(4, 4), Qt::NoButton,
                  Qt::RightButton);
        if (std::find(offKeys.begin(), offKeys.end(), a.key) == offKeys.end())
            fail("shrinking the band did not release the departed note");
        sendMouse(roll, QEvent::MouseMove, sweepEnd, Qt::NoButton, Qt::RightButton);
        sendMouse(roll, QEvent::MouseButtonRelease, sweepEnd, Qt::RightButton, Qt::NoButton);
        QObject::disconnect(conn);
        if (std::count(onKeys.begin(), onKeys.end(), a.key) < 2)
            fail("re-covering a note did not re-audition it");
        const std::vector<SongView::NoteKey> &sel = view.selection();
        if (sel.size() < 2 ||
            std::find(sel.begin(), sel.end(),
                      SongView::NoteKey{uint32_t(a.tick), uint8_t(a.key)}) == sel.end() ||
            std::find(sel.begin(), sel.end(),
                      SongView::NoteKey{uint32_t(b.tick), uint8_t(b.key)}) == sel.end())
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
        auto conn =
            QObject::connect(&view, &SongView::auditionNote, &view,
                             [&](int, int key, int velocity) { aud.push_back({key, velocity}); });
        const int preCount = doc.undoStack()->count();
        sendMouse(roll, QEvent::MouseButtonPress, e.center, Qt::LeftButton, Qt::LeftButton);
        if (aud != std::vector<std::pair<int, int>>{{e.key, 93}})
            fail("empty-space press did not audition its row at the latched velocity");
        const QPoint gliss(e.center.x(), rows.centerY(e.key - 1));
        sendMouse(roll, QEvent::MouseMove, gliss, Qt::NoButton, Qt::LeftButton);
        if (aud.empty() || aud.back() != std::make_pair(e.key - 1, 93))
            fail("holding the press across a row did not gliss the preview");
        sendMouse(roll, QEvent::MouseButtonRelease, gliss, Qt::LeftButton, Qt::NoButton);
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
        const QPoint pull = e.center + QPoint(QApplication::startDragDistance() + 8, 0);
        sendMouse(roll, QEvent::MouseButtonPress, e.center, Qt::LeftButton, Qt::LeftButton);
        sendMouse(roll, QEvent::MouseMove, pull, Qt::NoButton, Qt::LeftButton);
        sendMouse(roll, QEvent::MouseButtonRelease, pull, Qt::LeftButton, Qt::NoButton);
        QObject::disconnect(conn);
        if (std::count(aud.begin(), aud.end(), std::make_pair(e.key, 93)) != 1)
            fail("growing the press into a draw re-attacked the sounding key");
        DocNote drawn;
        if (!doc.findNote(track, e.tick, uint8_t(e.key), &drawn))
            fail("the press-grown draw did not commit its note");
    }

    // An audition sounds at the track volume in force where the NOTE is, not
    // wherever the edit cursor happens to sit: park the cursor past a VOL
    // change and the preview must still carry the volume its own side of the
    // change compiles to.
    {
        const Cell g = findFreeCell();
        if (g.key < 0) {
            fail("no free grid cell for the audition-volume probe");
            return failures;
        }
        constexpr int kVolBefore = 100;
        constexpr int kVolAfter = 40;
        const uint64_t changeTick = g.tick + g.dur;
        const int undoBefore = doc.undoStack()->index();
        doc.addLanePoint(track, 7, 0, kVolBefore);
        doc.addLanePoint(track, 7, changeTick, kVolAfter);
        (void)view.grab();
        if (view.trackRawVolumeAt(track, g.tick) != kVolBefore ||
            view.trackRawVolumeAt(track, changeTick) != kVolAfter)
            fail("the probe's VOL ramp did not take");
        // The cursor sits on the quiet side; the probed row is on the loud one.
        view.commitEditCursor(changeTick);
        std::vector<int> auditionVolumes;
        auto conn = QObject::connect(&view, &SongView::auditionNote, &view,
                                     [&](int, int, int velocity, int rawVolume) {
                                         if (velocity > 0)
                                             auditionVolumes.push_back(rawVolume);
                                     });
        sendMouse(roll, QEvent::MouseButtonPress, g.center, Qt::LeftButton, Qt::LeftButton);
        sendMouse(roll, QEvent::MouseButtonRelease, g.center, Qt::LeftButton, Qt::NoButton);
        QObject::disconnect(conn);
        if (auditionVolumes != std::vector<int>{kVolBefore})
            fail("the press audition did not sound at the VOL in force at the note it draws");
        while (doc.undoStack()->index() > undoBefore)
            doc.undoStack()->undo();
        (void)view.grab();
    }

    // ...and it follows the note as the note moves: a purely horizontal drag
    // out of a loud passage into a quiet one must re-attack at the volume it
    // now sits under, not hold the origin's for the rest of the gesture.
    {
        const int undoBefore = doc.undoStack()->index();
        Cell g;
        for (int probe = 8; probe < roll->width() - songview::kKeyboardW - 64; probe += 24) {
            const Cell c = findFreeCell(probe, true);
            if (c.key < 0)
                break;
            // The cell the drag lands on must be free and on screen too.
            const int x2 = songview::kKeyboardW + view.contentX(double(c.tick + 2 * c.dur));
            if (x2 < roll->width() && !occupied(c.tick + c.dur, c.dur, c.key, true)) {
                g = c;
                break;
            }
        }
        if (g.key < 0) {
            fail("no free cell pair for the drag-across-VOL audition probe");
            return failures;
        }
        constexpr int kVolLoud = 100;
        constexpr int kVolQuiet = 40;
        const uint64_t changeTick = g.tick + g.dur;
        // Velocity 1 parks the note's velocity bar at its bottom edge, so a
        // press near the row top grabs the note to MOVE it instead of
        // starting a velocity drag.
        doc.addNote(track, g.tick, uint8_t(g.key), uint32_t(g.dur), 1);
        doc.addLanePoint(track, 7, 0, kVolLoud);
        doc.addLanePoint(track, 7, changeTick, kVolQuiet);
        (void)view.grab();
        const int volBefore = view.trackRawVolumeAt(track, g.tick);
        if (volBefore == kVolQuiet || view.trackRawVolumeAt(track, changeTick) != kVolQuiet) {
            fail("the drag probe's VOL change did not take");
            return failures;
        }
        g.center.setY(int(std::lround((rows.top(g.key) + rows.centerY(g.key)) / 2.0)));
        std::vector<int> auditionVolumes;
        auto conn = QObject::connect(&view, &SongView::auditionNote, &view,
                                     [&](int, int, int velocity, int rawVolume) {
                                         if (velocity > 0)
                                             auditionVolumes.push_back(rawVolume);
                                     });
        const int dx = view.contentX(double(changeTick)) - view.contentX(double(g.tick));
        const QPoint dropped = g.center + QPoint(dx, 0);
        sendMouse(roll, QEvent::MouseButtonPress, g.center, Qt::LeftButton, Qt::LeftButton);
        sendMouse(roll, QEvent::MouseMove, dropped, Qt::NoButton, Qt::LeftButton);
        sendMouse(roll, QEvent::MouseButtonRelease, dropped, Qt::LeftButton, Qt::NoButton);
        QObject::disconnect(conn);
        if (auditionVolumes.size() < 2 || auditionVolumes.front() != volBefore ||
            auditionVolumes.back() != kVolQuiet)
            fail("a horizontal move across a VOL change did not re-audition at the new volume");
        while (doc.undoStack()->index() > undoBefore)
            doc.undoStack()->undo();
        (void)view.grab();
    }

    // The pencil's pitch readout: while a draw gesture is live, the pending
    // note names its pitch even where settled labels hide — painting a note
    // and dragging it to the right pitch depends on the live name.
    {
        const auto readoutPadding = layout::space(layout::Space::Half);
        auto readoutFont = typography::noteName(roll->font());
        readoutFont.setPixelSize(
            std::max(layout::singlePixel(), readoutFont.pixelSize() - 2 * layout::singlePixel()));
        const auto readoutMetrics = QFontMetrics(readoutFont);
        const SongView::ViewState viewBeforeReadout = view.viewState();
        SongView::ViewState readoutShortRows = viewBeforeReadout;
        // Short rows where the fixed face cannot fit, so any painted name is
        // the readout, never a settled label.
        readoutShortRows.keyHeight =
            double(readoutMetrics.ascent() + readoutMetrics.descent() + 2 * readoutPadding);
        view.applyViewState(readoutShortRows);
        view.setNoteNameMode(true);
        const Cell readoutCell = findFreeCell();
        if (readoutCell.key < 0) {
            fail("no free grid cell for the draw readout probe");
        } else {
            const int undoIndexBeforeReadout = doc.undoStack()->index();
            const QPoint readoutEnd =
                readoutCell.center + QPoint(QApplication::startDragDistance() + 8, 0);
            sendMouse(roll, QEvent::MouseButtonPress, readoutCell.center, Qt::LeftButton,
                      Qt::LeftButton);
            sendMouse(roll, QEvent::MouseMove, readoutEnd, Qt::NoButton, Qt::LeftButton);
            const QImage readoutOn = roll->grab().toImage();
            view.setNoteNameMode(false);
            const QImage readoutOff = roll->grab().toImage();
            view.setNoteNameMode(true);
            sendMouse(roll, QEvent::MouseButtonRelease, readoutEnd, Qt::LeftButton, Qt::NoButton);
            if (readoutOn == readoutOff)
                fail("no pitch readout on the pending draw note");
            while (doc.undoStack()->index() > undoIndexBeforeReadout && doc.undoStack()->canUndo())
                doc.undoStack()->undo();
        }
        view.applyViewState(viewBeforeReadout);
    }

    // Drawing begins at a layout Space::One horizontal drag; a shorter
    // gesture remains a click, while one at the threshold creates a
    // one-snap-cell note.
    {
        const int drawStartDistance = layout::space(layout::Space::One);
        const qreal belowDrawStartDistance = std::max(0.0, double(drawStartDistance) - 0.5);
        const Cell f = findFreeCell();
        if (f.key < 0) {
            fail("no free grid cell for the minimum-distance draw");
            return failures;
        }
        const QPointF belowDrawEnd = QPointF(f.center) + QPointF(belowDrawStartDistance, 0.0);
        sendMouse(roll, QEvent::MouseButtonPress, f.center, Qt::LeftButton, Qt::LeftButton);
        sendMouse(roll, QEvent::MouseMove, belowDrawEnd, Qt::NoButton, Qt::LeftButton);
        sendMouse(roll, QEvent::MouseButtonRelease, belowDrawEnd, Qt::LeftButton, Qt::NoButton);
        DocNote tiny;
        if (doc.findNote(track, f.tick, uint8_t(f.key), &tiny))
            fail("a subthreshold horizontal drag drew a note");
        sendMouse(roll, QEvent::MouseButtonPress, f.center, Qt::LeftButton, Qt::LeftButton);
        sendMouse(roll, QEvent::MouseMove, f.center + QPoint(drawStartDistance, 0), Qt::NoButton,
                  Qt::LeftButton);
        sendMouse(roll, QEvent::MouseButtonRelease, f.center + QPoint(drawStartDistance, 0),
                  Qt::LeftButton, Qt::NoButton);
        if (!doc.findNote(track, f.tick, uint8_t(f.key), &tiny))
            fail("a Space::One horizontal drag did not draw a note");
        else if (tiny.duration != view.snapTicksAt(f.tick))
            fail("the minimum-distance note is not one snap cell long");
    }

    // Modifier velocity gesture (Ableton-style): with the roll.velocity_drag
    // chord held (Ctrl by default), a vertical drag from anywhere on note B
    // adjusts its velocity — 1px = 1 step, 15px down lands 93 -> 78 — with
    // the hover mark pinned to the note's row. Keeping the chord held, a
    // velocity drag on the next note replaces the prior selection instead
    // of accumulating it (one-shot, re-armed by each committed drag).
    // Without a preceding drag, Ctrl+click keeps its selection-toggle
    // meaning (deferred to release), and a vertical jitter under the drag
    // threshold is still that click: it toggles, changes no velocity, and
    // pushes no undo command.
    {
        click(roll, b.center); // plain click: select B (velocity 93)
        const int preCount = doc.undoStack()->count();
        sendMouse(roll, QEvent::MouseButtonPress, b.center, Qt::LeftButton, Qt::LeftButton,
                  Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseMove, b.center + QPoint(0, 15), Qt::NoButton, Qt::LeftButton,
                  Qt::ControlModifier);
        if (roll->property("hoverKey").toInt() != b.key)
            fail("modifier velocity drag did not pin the hover mark");
        sendMouse(roll, QEvent::MouseButtonRelease, b.center + QPoint(0, 15), Qt::LeftButton,
                  Qt::NoButton, Qt::ControlModifier);
        DocNote bMod;
        if (!doc.findNote(track, b.tick, uint8_t(b.key), &bMod) || bMod.velocity != 78)
            fail("modifier velocity drag did not land at 78");
        if (doc.undoStack()->count() != preCount + 1)
            fail("modifier velocity drag did not push exactly one command");

        // Clicks keep their ordinary meaning and do not spend the one-shot
        // armed for the next modifier velocity drag.
        const SongView::NoteKey bId{uint32_t(b.tick), uint8_t(b.key)};
        const SongView::NoteKey aId{uint32_t(a.tick), uint8_t(a.key)};
        sendMouse(roll, QEvent::MouseButtonPress, b.center, Qt::LeftButton, Qt::LeftButton,
                  Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseButtonRelease, b.center, Qt::LeftButton, Qt::NoButton,
                  Qt::ControlModifier);
        if (std::find(view.selection().begin(), view.selection().end(), bId) !=
            view.selection().end())
            fail("Ctrl+click after a velocity drag did not keep its toggle meaning");
        click(roll, b.center);
        if (view.selection().size() != 1 || !(view.selection().front() == bId))
            fail("a plain click after a velocity drag did not select its note");

        // The same uninterrupted chord hold on another note is a request to
        // edit that note, not to grow a bulk selection and edit both.
        DocNote aCarryBefore;
        if (!doc.findNote(track, a.tick, uint8_t(a.key), &aCarryBefore))
            fail("note A went missing before the carried modifier velocity drag");
        const int carryCount = doc.undoStack()->count();
        sendMouse(roll, QEvent::MouseButtonPress, a.center, Qt::LeftButton, Qt::LeftButton,
                  Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseMove, a.center + QPoint(0, 15), Qt::NoButton, Qt::LeftButton,
                  Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseButtonRelease, a.center + QPoint(0, 15), Qt::LeftButton,
                  Qt::NoButton, Qt::ControlModifier);
        const std::vector<SongView::NoteKey> &carried = view.selection();
        if (carried.size() != 1 || !(carried.front() == aId))
            fail("a held modifier accumulated the note after a velocity drag");
        DocNote aCarryAfter, bCarryAfter;
        if (!doc.findNote(track, a.tick, uint8_t(a.key), &aCarryAfter) ||
            int(aCarryAfter.velocity) != int(aCarryBefore.velocity) - 15)
            fail("the carried modifier velocity drag did not adjust the next note");
        if (!doc.findNote(track, b.tick, uint8_t(b.key), &bCarryAfter) ||
            bCarryAfter.velocity != 78)
            fail("the carried modifier velocity drag also adjusted the prior note");
        if (doc.undoStack()->count() != carryCount + 1)
            fail("the carried modifier velocity drag did not push exactly one command");

        // Releasing the modifier ends the one-shot; the toggle click and the
        // sub-threshold jitter behave the same on either side of it.
        QKeyEvent modifierRelease(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier);
        QCoreApplication::sendEvent(roll, &modifierRelease);
        click(roll, b.center); // selection = {B}
        const int postCarryCount = doc.undoStack()->count();
        sendMouse(roll, QEvent::MouseButtonPress, b.center, Qt::LeftButton, Qt::LeftButton,
                  Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseButtonRelease, b.center, Qt::LeftButton, Qt::NoButton,
                  Qt::ControlModifier);
        if (std::find(view.selection().begin(), view.selection().end(), bId) !=
            view.selection().end())
            fail("Ctrl+click did not toggle the note out of the selection");

        sendMouse(roll, QEvent::MouseButtonPress, b.center, Qt::LeftButton, Qt::LeftButton,
                  Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseMove, b.center + QPoint(0, 3), Qt::NoButton, Qt::LeftButton,
                  Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseButtonRelease, b.center + QPoint(0, 3), Qt::LeftButton,
                  Qt::NoButton, Qt::ControlModifier);
        if (view.selection().size() != 1 || !(view.selection().front() == bId))
            fail("a sub-threshold Ctrl-jitter did not act as the toggle click");
        if (!doc.findNote(track, b.tick, uint8_t(b.key), &bMod) || bMod.velocity != 78)
            fail("a sub-threshold Ctrl-jitter changed the velocity");
        if (doc.undoStack()->count() != postCarryCount)
            fail("a Ctrl-click or jitter pushed an undo command");

        // Bulk-selection preservation, mirroring the Ctrl+edge grab: with
        // note A selected, a Ctrl+velocity drag on unselected note B joins
        // B to the selection instead of replacing it, and the nudge lands
        // on BOTH notes in one command. Coming after the carried drag and
        // the modifier release, this also proves the release restored the
        // join semantics.
        click(roll, a.center); // selection = {A}
        DocNote aBefore, bBefore;
        if (!doc.findNote(track, a.tick, uint8_t(a.key), &aBefore) ||
            !doc.findNote(track, b.tick, uint8_t(b.key), &bBefore))
            fail("notes A/B went missing before the joined velocity drag");
        const int joinCount = doc.undoStack()->count();
        sendMouse(roll, QEvent::MouseButtonPress, b.center, Qt::LeftButton, Qt::LeftButton,
                  Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseMove, b.center + QPoint(0, 15), Qt::NoButton, Qt::LeftButton,
                  Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseButtonRelease, b.center + QPoint(0, 15), Qt::LeftButton,
                  Qt::NoButton, Qt::ControlModifier);
        const std::vector<SongView::NoteKey> &joined = view.selection();
        if (joined.size() != 2 || std::find(joined.begin(), joined.end(), aId) == joined.end() ||
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

        // Repeating the modifier drag on the same anchor keeps the
        // deliberate bulk selection; the one-shot only suppresses adding
        // another note.
        const int repeatCount = doc.undoStack()->count();
        sendMouse(roll, QEvent::MouseButtonPress, b.center, Qt::LeftButton, Qt::LeftButton,
                  Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseMove, b.center - QPoint(0, 15), Qt::NoButton, Qt::LeftButton,
                  Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseButtonRelease, b.center - QPoint(0, 15), Qt::LeftButton,
                  Qt::NoButton, Qt::ControlModifier);
        const std::vector<SongView::NoteKey> &repeated = view.selection();
        if (repeated.size() != 2 ||
            std::find(repeated.begin(), repeated.end(), aId) == repeated.end() ||
            std::find(repeated.begin(), repeated.end(), bId) == repeated.end())
            fail("repeating a modifier velocity drag on its anchor collapsed the bulk selection");
        DocNote aRepeated, bRepeated;
        if (!doc.findNote(track, a.tick, uint8_t(a.key), &aRepeated) ||
            aRepeated.velocity != aAfter.velocity + 15 ||
            !doc.findNote(track, b.tick, uint8_t(b.key), &bRepeated) ||
            bRepeated.velocity != bAfter.velocity + 15)
            fail("repeating a modifier velocity drag did not nudge the whole selection");
        if (doc.undoStack()->count() != repeatCount + 1)
            fail("the repeated velocity drag did not push exactly one command");

        // A different anchor replaces the prior selection even when that
        // anchor was already part of the bulk selection.
        const int switchCount = doc.undoStack()->count();
        sendMouse(roll, QEvent::MouseButtonPress, a.center, Qt::LeftButton, Qt::LeftButton,
                  Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseMove, a.center + QPoint(0, 15), Qt::NoButton, Qt::LeftButton,
                  Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseButtonRelease, a.center + QPoint(0, 15), Qt::LeftButton,
                  Qt::NoButton, Qt::ControlModifier);
        DocNote aSwitched, bSwitched;
        if (view.selection().size() != 1 || !(view.selection().front() == aId))
            fail("a carried modifier drag kept the prior note selected");
        if (!doc.findNote(track, a.tick, uint8_t(a.key), &aSwitched) ||
            aSwitched.velocity != aRepeated.velocity - 15)
            fail("the carried modifier drag did not adjust its new anchor");
        if (!doc.findNote(track, b.tick, uint8_t(b.key), &bSwitched) ||
            bSwitched.velocity != bRepeated.velocity)
            fail("the carried modifier drag adjusted the prior selected note");
        if (doc.undoStack()->count() != switchCount + 1)
            fail("the switched velocity drag did not push exactly one command");

        // Focus loss is as much an interruption as releasing the chord:
        // the next modifier drag joins again.
        QFocusEvent focusOut(QEvent::FocusOut);
        QCoreApplication::sendEvent(roll, &focusOut);
        const int focusCount = doc.undoStack()->count();
        sendMouse(roll, QEvent::MouseButtonPress, b.center, Qt::LeftButton, Qt::LeftButton,
                  Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseMove, b.center + QPoint(0, 15), Qt::NoButton, Qt::LeftButton,
                  Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseButtonRelease, b.center + QPoint(0, 15), Qt::LeftButton,
                  Qt::NoButton, Qt::ControlModifier);
        const std::vector<SongView::NoteKey> &refocused = view.selection();
        if (refocused.size() != 2 ||
            std::find(refocused.begin(), refocused.end(), aId) == refocused.end() ||
            std::find(refocused.begin(), refocused.end(), bId) == refocused.end())
            fail("focus loss did not disarm the one-shot selection replace");
        DocNote aFocus, bFocus;
        if (!doc.findNote(track, a.tick, uint8_t(a.key), &aFocus) ||
            aFocus.velocity != aSwitched.velocity - 15 ||
            !doc.findNote(track, b.tick, uint8_t(b.key), &bFocus) ||
            bFocus.velocity != bSwitched.velocity - 15)
            fail("the post-focus-loss drag did not nudge the joined selection");
        if (doc.undoStack()->count() != focusCount + 1)
            fail("the post-focus-loss velocity drag did not push exactly one command");

        // The one-shot recognizes its anchor by NoteId, not by {tick,key}:
        // a Ctrl+Down transpose mid-hold re-keys the whole selection, and
        // the next drag on the SAME (moved) anchor must still keep the
        // bulk selection.
        if (occupied(a.tick, a.dur, a.key - 1) || occupied(b.tick, b.dur, b.key - 1))
            fail("no free rows below notes A/B for the transpose probe");
        const int transposeCount = doc.undoStack()->count();
        sendKey(roll, Qt::Key_Down, Qt::ControlModifier);
        const SongView::NoteKey aMoved{uint32_t(a.tick), uint8_t(a.key - 1)};
        const SongView::NoteKey bMoved{uint32_t(b.tick), uint8_t(b.key - 1)};
        const QPoint bMovedCenter(b.center.x(), rows.centerY(b.key - 1));
        sendMouse(roll, QEvent::MouseButtonPress, bMovedCenter, Qt::LeftButton, Qt::LeftButton,
                  Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseMove, bMovedCenter + QPoint(0, 15), Qt::NoButton,
                  Qt::LeftButton, Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseButtonRelease, bMovedCenter + QPoint(0, 15), Qt::LeftButton,
                  Qt::NoButton, Qt::ControlModifier);
        const std::vector<SongView::NoteKey> &transposed = view.selection();
        if (transposed.size() != 2 ||
            std::find(transposed.begin(), transposed.end(), aMoved) == transposed.end() ||
            std::find(transposed.begin(), transposed.end(), bMoved) == transposed.end())
            fail("a mid-hold transpose disguised the anchor and collapsed the bulk selection");
        DocNote aTransposed, bTransposed;
        if (!doc.findNote(track, a.tick, uint8_t(a.key - 1), &aTransposed) ||
            aTransposed.velocity != aFocus.velocity - 15 ||
            !doc.findNote(track, b.tick, uint8_t(b.key - 1), &bTransposed) ||
            bTransposed.velocity != bFocus.velocity - 15)
            fail("the post-transpose anchor drag did not nudge the whole selection");
        if (doc.undoStack()->count() != transposeCount + 2)
            fail("the transpose and its anchor drag did not push exactly two commands");

        // The last drag re-armed the one-shot; release the chord so no
        // hidden state leaks into the sections below.
        QKeyEvent finalRelease(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier);
        QCoreApplication::sendEvent(roll, &finalRelease);

        // Restore both velocities and positions for later checks: undo the
        // post-transpose, transpose, focus-loss, switched, repeated,
        // joined, and carried edits. The first modifier drag stays
        // committed, exactly as before this section grew, so the gesture
        // tally at the end of the run is unchanged.
        for (int i = 0; i < 7; ++i)
            doc.undoStack()->undo();

        // Escape while the modifier press is still undecided cancels it:
        // no promotion on further movement, no toggle on release, no edit.
        const int escapeCount = doc.undoStack()->count();
        sendMouse(roll, QEvent::MouseButtonPress, b.center, Qt::LeftButton, Qt::LeftButton,
                  Qt::ControlModifier);
        sendKey(roll, Qt::Key_Escape, Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseMove, b.center + QPoint(0, 15), Qt::NoButton, Qt::LeftButton,
                  Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseButtonRelease, b.center + QPoint(0, 15), Qt::LeftButton,
                  Qt::NoButton, Qt::ControlModifier);
        DocNote bEscaped;
        if (!doc.findNote(track, b.tick, uint8_t(b.key), &bEscaped) || bEscaped.velocity != 78)
            fail("an Escaped modifier press still adjusted the velocity");
        if (!view.selection().empty())
            fail("an Escaped modifier press still changed the selection");
        if (doc.undoStack()->count() != escapeCount)
            fail("an Escaped modifier press pushed an undo command");
        view.clearSelection();
    }

    // A velocity value on a vertically short note stays inside the note box:
    // the face fits the box rather than the row pitch (which includes the
    // hairline gap and can round up past it), and the plated text clips to
    // the box. Probe pitches where a pitch-fitted face pushed digit ink into
    // the gap row on 1x displays: an integer pitch whose fitted face
    // occupied the whole rounded height, and a fractional pitch that rounds
    // up past the row.
    {
        const SongView::ViewState originalView = view.viewState();
        for (const double shortKeyHeight : {8.6, 9.0}) {
            SongView::ViewState shortView = originalView;
            shortView.keyHeight = shortKeyHeight;
            view.applyViewState(shortView);
            QCoreApplication::processEvents();

            // The probed note stays unselected (values show on every
            // current-track note during a drag), so its interior carries only
            // the 1-DIP black border, not the selection ring — the drag runs
            // on a sacrificial second note, placed FIRST so the isolation
            // scan below keeps the probed note's guarded rows clear of it.
            const int undoIndexBefore = doc.undoStack()->index();
            const Cell dragCell = findFreeCell(8, true);
            if (dragCell.key < 0) {
                fail("no free cell for the short-note velocity drag note");
                continue;
            }
            doc.addNote(track, dragCell.tick, uint8_t(dragCell.key), uint32_t(dragCell.dur), 100);

            // An isolated two-cell span: wide enough for a two-digit value,
            // with the rows above and below empty on every track so the
            // strips beyond the box compare against static background. The
            // parked playhead/edit-cursor column and the loop markers paint
            // identically over background and note, so they must stay a cell
            // clear of the span (findFreeCell dodges too few of these and
            // only one cell, hence the dedicated scan).
            Cell cell;
            const SnappedRows shortRows{view, *roll};
            for (int key = 115; key >= 24 && cell.key < 0; --key) {
                if (shortRows.top(key) < 3.0 || shortRows.bottom(key) > roll->height() - 3.0)
                    continue;
                for (int probe = 8; probe < roll->width() - songview::kKeyboardW - 40;
                     probe += 24) {
                    const uint64_t tick = view.snapTickDown(view.tickAtContentX(probe));
                    const uint64_t dur = view.gridTicksAt(tick);
                    const int x0 = songview::kKeyboardW + view.contentX(double(tick));
                    const int xs =
                        songview::kKeyboardW + view.contentX(double(tick + view.snapTicksAt(tick)));
                    const int x2 = songview::kKeyboardW + view.contentX(double(tick + 2 * dur));
                    if (x0 < songview::kKeyboardW || xs - x0 < 8 || x2 - x0 < 24 ||
                        x2 >= roll->width())
                        continue;
                    bool blocked = false;
                    for (int neighborKey = key - 1; neighborKey <= key + 1; ++neighborKey)
                        blocked = blocked || occupied(tick, 2 * dur, neighborKey, true);
                    const auto nearSpan = [&](uint64_t overlay) {
                        return overlay != UINT64_MAX && overlay + dur >= tick &&
                               overlay <= tick + 3 * dur;
                    };
                    if (blocked || nearSpan(timeline->loopStartTick) ||
                        nearSpan(timeline->loopEndTick) || nearSpan(overlayTick))
                        continue;
                    cell.tick = tick;
                    cell.dur = dur;
                    cell.key = key;
                    cell.center = QPoint((x0 + xs) / 2, shortRows.centerY(key));
                    break;
                }
            }
            if (cell.key < 0) {
                fail("no isolated cell for the short-note velocity value probe");
                while (doc.undoStack()->index() > undoIndexBefore && doc.undoStack()->canUndo())
                    doc.undoStack()->undo();
                continue;
            }

            // Velocity 10 parks the probed note's bar at the box bottom,
            // keeping the upper interior clear for the glyph-ink assertion.
            doc.addNote(track, cell.tick, uint8_t(cell.key), uint32_t(2 * cell.dur), 10);
            QCoreApplication::processEvents();
            const QImage shortIdleImage = roll->grab().toImage();

            sendMouse(roll, QEvent::MouseButtonPress, dragCell.center, Qt::LeftButton,
                      Qt::LeftButton, Qt::ControlModifier);
            sendMouse(roll, QEvent::MouseMove, dragCell.center + QPoint(0, 12), Qt::NoButton,
                      Qt::LeftButton, Qt::ControlModifier);
            const QImage shortDragImage = roll->grab().toImage();
            sendMouse(roll, QEvent::MouseButtonRelease, dragCell.center + QPoint(0, 12),
                      Qt::LeftButton, Qt::NoButton, Qt::ControlModifier);

            const int shortLeftX = songview::kKeyboardW + view.contentX(double(cell.tick));
            const int shortRightX =
                songview::kKeyboardW + view.contentX(double(cell.tick + 2 * cell.dur));
            const QRectF shortRect = shortRows.noteRect(shortLeftX, shortRightX, cell.key);
            const QRectF shortBox = shortRows.noteBox(shortRect);

            // The drag must render a value, or the no-bleed comparison below
            // passes vacuously. Glyph ink is any non-fill pixel in the box
            // interior above the vertical midline: the velocity bar sits at
            // the box bottom at 10, and the unselected note's black border is
            // excluded by margin.
            const QRgb draggedFill = SongView::noteColor(track, 10).rgb();
            const int frameMargin = songview::noteBorderPixels(rasterDpr);
            const int boxTopPixel = toRasterPixel(shortBox.top());
            const int boxBottomPixel = toRasterPixel(shortBox.bottom());
            const int inkTop = boxTopPixel + frameMargin;
            const int inkBottom = (boxTopPixel + boxBottomPixel) / 2;
            if (inkTop >= inkBottom)
                fail("short-note velocity probe has no frame-free interior row");
            bool valueInkFound = false;
            for (int y = inkTop; y < inkBottom; ++y) {
                for (int x = toRasterPixel(shortBox.left()) + frameMargin;
                     x < toRasterPixel(shortBox.right()) - frameMargin; ++x) {
                    valueInkFound |= shortDragImage.pixel(x, y) != draggedFill;
                }
            }
            if (!valueInkFound)
                fail("short-note velocity drag rendered no value ink");

            // No ink outside the box: the gap row under the box and the rows
            // beyond the note rect (below and above) must match the pre-drag
            // image, on the note's span padded past the plate's side bleed.
            const QRect imageBounds = shortDragImage.rect();
            const auto stripsMatch = [&](int firstY, int lastY) {
                // The left clamp also keeps the strip out of the keyboard,
                // whose hover mark legitimately repaints during the drag.
                const int stripLeft = std::max(toRasterPixel(shortBox.left()) - 2,
                                               toRasterPixel(qreal(songview::kKeyboardW)));
                for (int y = std::max(firstY, 0); y <= std::min(lastY, imageBounds.bottom()); ++y) {
                    for (int x = stripLeft;
                         x <= std::min(toRasterPixel(shortBox.right()) + 2, imageBounds.right());
                         ++x) {
                        if (shortDragImage.pixel(x, y) != shortIdleImage.pixel(x, y))
                            return false;
                    }
                }
                return true;
            };
            if (!stripsMatch(boxBottomPixel, toRasterPixel(shortRect.bottom()) + 2))
                fail("short-note velocity value bled below the note box");
            if (!stripsMatch(toRasterPixel(shortRect.top()) - 3,
                             toRasterPixel(shortRect.top()) - 1))
                fail("short-note velocity value bled above the note rect");

            while (doc.undoStack()->index() > undoIndexBefore && doc.undoStack()->canUndo())
                doc.undoStack()->undo();
            view.clearSelection();
        }
        // The Ctrl+velocity drags above re-armed the one-shot selection
        // replace; release the chord so no hidden state leaks into the
        // sections below.
        QKeyEvent shortNoteRelease(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier);
        QCoreApplication::sendEvent(roll, &shortNoteRelease);
        view.applyViewState(originalView);
        QCoreApplication::processEvents();
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
    // Probe 2.8 DIPs inward at both ends on the velocity bar itself. The
    // resize zones must win over the overlapping velocity hover.
    const qreal resizeNoteLeftX =
        view.displayX(double(d.tick), songview::kKeyboardW, roll->devicePixelRatioF());
    const qreal resizeNoteRightX =
        view.displayX(double(d.tick + offDur), songview::kKeyboardW, roll->devicePixelRatioF());
    const int resizeHandleY =
        qRound(songview::velBarRect(rows.noteRect(0, 1, d.key), 100, rows.dpr()).center().y());
    const QPointF leftHandle(resizeNoteLeftX + 2.8, resizeHandleY);
    const QPointF rightHandle(resizeNoteRightX - 2.8, resizeHandleY);
    sendMouse(roll, QEvent::MouseMove, leftHandle, Qt::NoButton, Qt::NoButton, Qt::ControlModifier);
    const QPixmap expectedLeftCursor = QIcon(QStringLiteral(":/cursors/left-drag.png"))
                                           .pixmap(QSize(24, 24), roll->devicePixelRatioF());
    if (roll->cursor().pixmap().devicePixelRatio() != expectedLeftCursor.devicePixelRatio() ||
        roll->cursor().pixmap().toImage() != expectedLeftCursor.toImage())
        fail("left note edge did not show its DPI-matched custom cursor");
    sendMouse(roll, QEvent::MouseMove, rightHandle, Qt::NoButton, Qt::NoButton,
              Qt::ControlModifier);
    const QPixmap expectedRightCursor = QIcon(QStringLiteral(":/cursors/right-drag.png"))
                                            .pixmap(QSize(24, 24), roll->devicePixelRatioF());
    if (roll->cursor().pixmap().devicePixelRatio() != expectedRightCursor.devicePixelRatio() ||
        roll->cursor().pixmap().toImage() != expectedRightCursor.toImage())
        fail("right note edge did not show its custom cursor");
    const QPoint pull(songview::kKeyboardW + view.contentX(double(d.tick) + 1.9 * double(d.dur)),
                      rowY);
    sendMouse(roll, QEvent::MouseButtonPress, rightHandle, Qt::LeftButton, Qt::LeftButton);
    sendMouse(roll, QEvent::MouseMove, pull, Qt::NoButton, Qt::LeftButton);
    sendMouse(roll, QEvent::MouseButtonRelease, pull, Qt::LeftButton, Qt::NoButton);
    DocNote resized;
    if (!doc.findNote(track, d.tick, uint8_t(d.key), &resized) || resized.duration != 2 * d.dur)
        fail("off-grid right-edge drag did not snap the end to the ruler grid");

    // Ctrl+grabbing an edge keeps the bulk selection: with note B
    // selected, a Ctrl+press on note D's right-edge grip joins D to the
    // selection instead of replacing it, and the drag that follows
    // resizes the whole selection — both notes — in one undo command. A
    // stationary Ctrl+edge click just joins, editing nothing.
    {
        const qreal dpr = roll->devicePixelRatioF();
        const QPointF ctrlEdge(
            view.displayX(double(d.tick + 2 * d.dur), songview::kKeyboardW, dpr) - 2.8, rowY);
        click(roll, b.center); // selection = {B}
        const int preCount = doc.undoStack()->count();
        DocNote bBefore;
        if (!doc.findNote(track, b.tick, uint8_t(b.key), &bBefore))
            fail("note B went missing before the Ctrl+edge grab");
        sendMouse(roll, QEvent::MouseButtonPress, ctrlEdge, Qt::LeftButton, Qt::LeftButton,
                  Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseButtonRelease, ctrlEdge, Qt::LeftButton, Qt::NoButton,
                  Qt::ControlModifier);
        const SongView::NoteKey bId{uint32_t(b.tick), uint8_t(b.key)};
        const SongView::NoteKey dId{uint32_t(d.tick), uint8_t(d.key)};
        const std::vector<SongView::NoteKey> &sel = view.selection();
        if (sel.size() != 2 || std::find(sel.begin(), sel.end(), bId) == sel.end() ||
            std::find(sel.begin(), sel.end(), dId) == sel.end())
            fail("a Ctrl+edge click did not join the note to the selection");
        DocNote still;
        if (!doc.findNote(track, d.tick, uint8_t(d.key), &still) || still.duration != 2 * d.dur)
            fail("a stationary Ctrl+edge click resized the note");
        if (doc.undoStack()->count() != preCount)
            fail("a stationary Ctrl+edge click pushed an undo command");
        // Pull the grip one drawn cell further right: both selected notes
        // must grow by that same delta.
        const qreal cellPx = view.displayX(double(d.tick + 3 * d.dur), songview::kKeyboardW, dpr) -
                             view.displayX(double(d.tick + 2 * d.dur), songview::kKeyboardW, dpr);
        const QPointF pull2(ctrlEdge.x() + cellPx, rowY);
        sendMouse(roll, QEvent::MouseButtonPress, ctrlEdge, Qt::LeftButton, Qt::LeftButton,
                  Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseMove, pull2, Qt::NoButton, Qt::LeftButton,
                  Qt::ControlModifier);
        sendMouse(roll, QEvent::MouseButtonRelease, pull2, Qt::LeftButton, Qt::NoButton,
                  Qt::ControlModifier);
        DocNote dAfter, bAfter;
        if (!doc.findNote(track, d.tick, uint8_t(d.key), &dAfter) || dAfter.duration != 3 * d.dur)
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
    const QPoint edge2(songview::kKeyboardW + view.contentX(double(d.tick + 2 * d.dur)), rowY);
    const QPoint overshoot(
        songview::kKeyboardW + view.contentX(double(d.tick) - 0.5 * double(d.dur)), rowY);
    sendMouse(roll, QEvent::MouseButtonPress, edge2, Qt::LeftButton, Qt::LeftButton);
    sendMouse(roll, QEvent::MouseMove, overshoot, Qt::NoButton, Qt::LeftButton);
    sendMouse(roll, QEvent::MouseButtonRelease, overshoot, Qt::LeftButton, Qt::NoButton);
    DocNote collapsed;
    if (!doc.findNote(track, d.tick, uint8_t(d.key), &collapsed) || collapsed.duration != snapCell)
        fail("overshot right-edge drag did not stop at one snap cell");

    // The collapsed note is one snap cell (16 DIPs here) wide. Inside a
    // note that narrow the edge zones shrink to leave a grabbable middle,
    // so 6 DIPs in from the right edge (below the velocity bar) is part of
    // that middle: the hover shows the plain arrow, not a resize cursor.
    const QPointF narrowMiddle(songview::kKeyboardW +
                                   view.contentX(double(d.tick) + double(snapCell)) - 6,
                               rows.bottom(d.key) - 2);
    sendMouse(roll, QEvent::MouseMove, narrowMiddle, Qt::NoButton, Qt::NoButton);
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
        narrowView.scrollPx = std::max(0.0, double(d.tick) * narrowPxPerTick - 100.0);
        view.applyViewState(narrowView);
        const SnappedRows narrowRows{view, *roll};
        const int narrowLeftX = songview::kKeyboardW + view.contentX(double(d.tick));
        const int narrowRightX = songview::kKeyboardW + view.contentX(double(d.tick + snapCell));
        if (narrowRightX - narrowLeftX > 3)
            fail("narrow-zoom fixture note is unexpectedly wide");
        const QRectF narrowBox =
            narrowRows.noteBox(narrowRows.noteRect(narrowLeftX, narrowRightX, d.key));
        QImage narrowImage(roll->size(), QImage::Format_ARGB32_Premultiplied);
        narrowImage.fill(Qt::transparent);
        roll->render(&narrowImage);
        const auto isNarrowBorder = [&](QRgb pixel) {
            return qRed(pixel) <= 16 && qGreen(pixel) <= 16 && qBlue(pixel) <= 16;
        };
        if (!isNarrowBorder(
                narrowImage.pixel(qRound(narrowBox.center().x()), qRound(narrowBox.top()))))
            fail("narrow note shed the border its wide neighbors keep");
        view.applyViewState(originalView);
    }

    // Abutting notes: each side of the shared boundary must resize its own
    // note. The topmost widened hit used to swallow the left note's right
    // grip — a press just left of the boundary grabbed the right note's
    // left edge instead, so the left note could never be resized there.
    {
        const Cell g = findFreeCell();
        if (g.key < 0) {
            fail("no free grid cell for the abutting-notes resize");
            return failures;
        }
        const int undoIndexBefore = doc.undoStack()->index();
        doc.addNote(track, g.tick, uint8_t(g.key), uint32_t(g.dur), 100);
        doc.addNote(track, g.tick + g.dur, uint8_t(g.key), uint32_t(g.dur), 100);
        const qreal gDpr = roll->devicePixelRatioF();
        const uint64_t gSnap = view.snapTicksAt(g.tick);
        const qreal boundaryX = view.displayX(double(g.tick + g.dur), songview::kKeyboardW, gDpr);
        const int gRowY = rows.centerY(g.key);
        const QPointF leftSide(boundaryX - 2.8, gRowY);
        const QPointF rightSide(boundaryX + 2.8, gRowY);

        sendMouse(roll, QEvent::MouseMove, leftSide, Qt::NoButton, Qt::NoButton);
        const QPixmap wantRightGrip =
            QIcon(QStringLiteral(":/cursors/right-drag.png")).pixmap(QSize(24, 24), gDpr);
        if (roll->cursor().pixmap().toImage() != wantRightGrip.toImage())
            fail("left of an abutting boundary is not the left note's right grip");
        sendMouse(roll, QEvent::MouseMove, rightSide, Qt::NoButton, Qt::NoButton);
        const QPixmap wantLeftGrip =
            QIcon(QStringLiteral(":/cursors/left-drag.png")).pixmap(QSize(24, 24), gDpr);
        if (roll->cursor().pixmap().toImage() != wantLeftGrip.toImage())
            fail("right of an abutting boundary is not the right note's left grip");

        // Drag from just left of the boundary: the LEFT note's end shrinks
        // one snap cell; the right note must not move or resize.
        const QPointF pullLeft(
            view.displayX(double(g.tick + g.dur - gSnap), songview::kKeyboardW, gDpr), gRowY);
        sendMouse(roll, QEvent::MouseButtonPress, leftSide, Qt::LeftButton, Qt::LeftButton);
        sendMouse(roll, QEvent::MouseMove, pullLeft, Qt::NoButton, Qt::LeftButton);
        sendMouse(roll, QEvent::MouseButtonRelease, pullLeft, Qt::LeftButton, Qt::NoButton);
        DocNote gLeft, gRight;
        if (!doc.findNote(track, g.tick, uint8_t(g.key), &gLeft) || gLeft.duration != g.dur - gSnap)
            fail("boundary-left drag did not resize the left note's end");
        if (!doc.findNote(track, g.tick + g.dur, uint8_t(g.key), &gRight) ||
            gRight.duration != g.dur)
            fail("boundary-left drag disturbed the right note");

        // Restore the abutment, then drag from just right of the boundary:
        // the RIGHT note's start moves one snap cell in; the left note must
        // stay put.
        doc.undoStack()->undo();
        view.clearSelection();
        const QPointF pullRight(
            view.displayX(double(g.tick + g.dur + gSnap), songview::kKeyboardW, gDpr), gRowY);
        sendMouse(roll, QEvent::MouseButtonPress, rightSide, Qt::LeftButton, Qt::LeftButton);
        sendMouse(roll, QEvent::MouseMove, pullRight, Qt::NoButton, Qt::LeftButton);
        sendMouse(roll, QEvent::MouseButtonRelease, pullRight, Qt::LeftButton, Qt::NoButton);
        if (!doc.findNote(track, g.tick + g.dur + gSnap, uint8_t(g.key), &gRight) ||
            gRight.duration != g.dur - gSnap)
            fail("boundary-right drag did not resize the right note's start");
        if (!doc.findNote(track, g.tick, uint8_t(g.key), &gLeft) || gLeft.duration != g.dur)
            fail("boundary-right drag disturbed the left note");

        while (doc.undoStack()->index() > undoIndexBefore && doc.undoStack()->canUndo())
            doc.undoStack()->undo();
        view.clearSelection();
    }

    // Keyboard transpose/nudge on note D (clicking it selects it):
    // Ctrl+Up is a semitone, Ctrl+Shift+Down an octave, and Ctrl+Right
    // moves one snap cell from an on-grid start.
    const QPoint dCenter(
        songview::kKeyboardW + view.contentX(double(d.tick) + 0.5 * double(snapCell)), rowY);
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
    view.setSelection({{uint32_t(d.tick + snapCell + snapCell / 2), uint8_t(d.key - 11)}});
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
    const qreal cellPx = view.contentX(double(nStart + snapCell)) - view.contentX(double(nStart));
    const int rides = (vw - view.contentX(double(nStart + snapCell))) / cellPx + 2;
    for (int i = 0; i < rides; i++)
        sendKey(roll, Qt::Key_Right, Qt::ControlModifier);
    nStart += uint64_t(rides) * snapCell;
    if (view.displayX(double(nStart + snapCell), 0.0, dpr) != vw - physicalPixel)
        fail("riding the nudge right did not keep the note's end at the right edge");
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
    if (!doc.findNote(track, d.tick + 2 * snapCell, uint8_t(d.key - 10), &transposed))
        fail("time-selection Ctrl+Right did not nudge the covered note");
    if (view.timeSelection().startTick != d.tick + 2 * snapCell)
        fail("time-selection band did not follow the nudge");

    // Mouse gestures on the time selection. The ruler's left drag sweeps a
    // band (the press still parks the edit cursor; a plain click stays a
    // cursor click); with the range.move chord (Alt) a left drag inside
    // the band — roll, ruler or lanes — slides its contents horizontally
    // with the band following, range.duplicate (Ctrl+Alt) drops a copy
    // instead, and Escape mid-drag commits nothing.
    {
        auto *ruler = view.findChild<QWidget *>(QStringLiteral("timeRuler"));
        if (!ruler || ruler->height() <= 0) {
            fail("time ruler not found");
        } else {
            const uint64_t t0 = d.tick + 2 * snapCell; // the nudged note's tick
            const int key = d.key - 10;
            const int noteY = rows.centerY(key);
            auto rollX = [&](uint64_t tick) {
                return view.displayX(double(tick), songview::kKeyboardW, roll->devicePixelRatioF());
            };
            auto rulerX = [&](uint64_t tick) {
                return view.displayX(double(tick), songview::kGutterW, ruler->devicePixelRatioF());
            };
            const int rulerY = ruler->height() - 3; // the tick row
            auto leftDrag = [&](QWidget *w, QPointF from, QPointF to, Qt::KeyboardModifiers mods) {
                sendMouse(w, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton, mods);
                sendMouse(w, QEvent::MouseMove, (from + to) / 2, Qt::NoButton, Qt::LeftButton,
                          mods);
                sendMouse(w, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton, mods);
                sendMouse(w, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton, mods);
            };
            const int before = doc.undoStack()->count();

            // Ruler left sweep from t0 to t0 + 2 cells (and back to a click).
            view.clearTimeSelection();
            leftDrag(ruler, QPointF(rulerX(t0) + 1, rulerY),
                     QPointF(rulerX(t0 + 2 * snapCell) + 1, rulerY), Qt::NoModifier);
            if (!view.timeSelection().active() || view.timeSelection().startTick != t0 ||
                view.timeSelection().endTick != t0 + 2 * snapCell)
                fail("ruler left drag did not sweep a time selection");
            if (view.editCursorTick() != t0)
                fail("ruler left press did not park the edit cursor at the press");
            sendMouse(ruler, QEvent::MouseButtonPress, QPointF(rulerX(t0 + snapCell) + 1, rulerY),
                      Qt::LeftButton, Qt::LeftButton);
            sendMouse(ruler, QEvent::MouseButtonRelease, QPointF(rulerX(t0 + snapCell) + 1, rulerY),
                      Qt::LeftButton, Qt::NoButton);
            if (view.editCursorTick() != t0 + snapCell)
                fail("ruler left click no longer places the edit cursor");
            if (doc.undoStack()->count() != before)
                fail("ruler sweep pushed an undo command");

            // Alt+drag in the roll, pressed ON the covered note: the band
            // wins over the note, and the contents slide 2 cells right.
            SongView::TimeSelection one;
            one.startTick = t0;
            one.endTick = t0 + snapCell;
            view.setTimeSelection(one);
            leftDrag(roll, QPointF(rollX(t0) + 4, noteY),
                     QPointF(rollX(t0 + 2 * snapCell) + 4, noteY), Qt::AltModifier);
            DocNote moved;
            if (doc.findNote(track, t0, uint8_t(key), &moved) ||
                !doc.findNote(track, t0 + 2 * snapCell, uint8_t(key), &moved))
                fail("Alt+drag inside the band did not move the covered note");
            if (view.timeSelection().startTick != t0 ||
                view.timeSelection().endTick != t0 + snapCell)
                fail("band moved with the Alt+drag (it must stay put)");
            // Follow the content for the next gestures.
            one.startTick = t0 + 2 * snapCell;
            one.endTick = t0 + 3 * snapCell;
            view.setTimeSelection(one);
            if (doc.undoStack()->count() != before + 1)
                fail("Alt+drag move was not one undo command");

            // Ctrl+Alt+drag duplicates: the original stays, a copy lands 2
            // cells later and the band moves onto the copy.
            leftDrag(roll, QPointF(rollX(t0 + 2 * snapCell) + 4, noteY),
                     QPointF(rollX(t0 + 4 * snapCell) + 4, noteY),
                     Qt::ControlModifier | Qt::AltModifier);
            if (!doc.findNote(track, t0 + 2 * snapCell, uint8_t(key), &moved) ||
                !doc.findNote(track, t0 + 4 * snapCell, uint8_t(key), &moved))
                fail("Ctrl+Alt+drag inside the band did not duplicate the covered note");
            if (view.timeSelection().startTick != t0 + 2 * snapCell)
                fail("band moved with the duplicate (it must stay put)");
            one.startTick = t0 + 4 * snapCell;
            one.endTick = t0 + 5 * snapCell;
            view.setTimeSelection(one);
            if (doc.undoStack()->count() != before + 2)
                fail("Ctrl+Alt+drag duplicate was not one undo command");

            // The same move from the ruler's band, one cell left.
            leftDrag(ruler, QPointF(rulerX(t0 + 4 * snapCell) + 3, rulerY),
                     QPointF(rulerX(t0 + 3 * snapCell) + 3, rulerY), Qt::AltModifier);
            if (doc.findNote(track, t0 + 4 * snapCell, uint8_t(key), &moved) ||
                !doc.findNote(track, t0 + 3 * snapCell, uint8_t(key), &moved))
                fail("Alt+drag on the ruler band did not move the contents");
            if (view.timeSelection().startTick != t0 + 4 * snapCell)
                fail("band moved with the ruler Alt+drag (it must stay put)");
            one.startTick = t0 + 3 * snapCell;
            one.endTick = t0 + 4 * snapCell;
            view.setTimeSelection(one);
            if (doc.undoStack()->count() != before + 3)
                fail("ruler Alt+drag move was not one undo command");

            // Escape mid-drag: nothing commits, the release is inert.
            sendMouse(roll, QEvent::MouseButtonPress, QPointF(rollX(t0 + 3 * snapCell) + 4, noteY),
                      Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
            sendMouse(roll, QEvent::MouseMove, QPointF(rollX(t0 + 5 * snapCell) + 4, noteY),
                      Qt::NoButton, Qt::LeftButton, Qt::AltModifier);
            if (!view.rangeDrag().active || view.rangeDrag().dTick != int64_t(2 * snapCell))
                fail("Alt+drag did not preview a snapped 2-cell delta");
            sendKey(roll, Qt::Key_Escape, Qt::NoModifier);
            sendMouse(roll, QEvent::MouseButtonRelease,
                      QPointF(rollX(t0 + 5 * snapCell) + 4, noteY), Qt::LeftButton, Qt::NoButton,
                      Qt::AltModifier);
            if (view.rangeDrag().active || doc.undoStack()->count() != before + 3 ||
                !doc.findNote(track, t0 + 3 * snapCell, uint8_t(key), &moved))
                fail("Escape did not cancel the range drag");

            // A one-pixel wobble under the drag threshold is a click and
            // commits nothing — with the band's start off the current grid
            // too, where the old absolute snap would have turned the
            // residue into a move. A real drag from that band still moves
            // by whole cells, never by the residue.
            leftDrag(roll, QPointF(rollX(t0 + 3 * snapCell) + 4, noteY),
                     QPointF(rollX(t0 + 3 * snapCell) + 5, noteY), Qt::AltModifier);
            if (doc.undoStack()->count() != before + 3 ||
                !doc.findNote(track, t0 + 3 * snapCell, uint8_t(key), &moved))
                fail("a sub-threshold Alt wobble inside the band committed a move");
            const uint64_t residue = std::max<uint64_t>(1, snapCell / 4);
            one.startTick = t0 + 3 * snapCell - residue;
            one.endTick = t0 + 4 * snapCell;
            view.setTimeSelection(one);
            leftDrag(roll, QPointF(rollX(t0 + 3 * snapCell) + 4, noteY),
                     QPointF(rollX(t0 + 3 * snapCell) + 5, noteY), Qt::AltModifier);
            if (doc.undoStack()->count() != before + 3 ||
                !doc.findNote(track, t0 + 3 * snapCell, uint8_t(key), &moved))
                fail("a sub-threshold wobble on an off-grid band committed a move");
            leftDrag(roll, QPointF(rollX(t0 + 3 * snapCell) + 4, noteY),
                     QPointF(rollX(t0 + 5 * snapCell) + 4, noteY), Qt::AltModifier);
            if (doc.undoStack()->count() != before + 4 ||
                !doc.findNote(track, t0 + 5 * snapCell, uint8_t(key), &moved))
                fail("an off-grid band did not move its contents by whole cells");
            doc.undoStack()->undo();
            view.clearTimeSelection();
        }
    }

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
        const uint64_t farTick = uint64_t(std::max(0.0, view.tickAtContentX(vw * 2)));
        const QPoint mid(panned->width() / 2, panned->height() / 2);
        sendMouse(panned, QEvent::MouseButtonPress, mid, Qt::MiddleButton, Qt::MiddleButton);
        view.setPlayheadSample(timeline->sampleForTick(farTick), true);
        if (view.contentX(0.0) != home)
            fail("playhead follow-scroll moved the view during a pan gesture");
        sendMouse(panned, QEvent::MouseButtonRelease, mid, Qt::MiddleButton, Qt::NoButton);
        view.setPlayheadSample(timeline->sampleForTick(farTick), true);
        if (view.contentX(0.0) == home)
            fail("playhead follow-scroll did not resume after the pan ended");
        view.setPlayheadSample(0, false);
        view.scrollByPx(view.contentX(0.0) - home); // back where it started
    }

    // Automation pencil mode (automation.pencil_mode, default B): toggled
    // from the lanes focus, never on key auto-repeat, with a bitmap pencil
    // cursor while on. A pencil press always freehand-draws — landing on an
    // existing point's dot sweeps new points over it instead of grabbing —
    // and holding Shift locks the stroke to a horizontal line at the value
    // where the lock engaged. With the mode off, the dot grab-move and the
    // off-dot Shift ramp behave as before.
    {
        const int undoIndex = doc.undoStack()->index();
        const int laneTrack = view.selectedTrack();
        // Test-side mirror of the tempo row's value<->y mapping (row 0 of
        // the lanes at the default 48 px height: plot top 5, bottom 43),
        // like SnappedRows mirrors the roll. The click sanity asserts below
        // fail loudly if the widget's geometry drifts from this.
        const int rowTopPad = 5, rowBottom = 48 - 1 - 4;
        auto tempoMaxV = [&]() {
            int maxV = 200;
            for (const LanePoint &pt : view.model().tempoLane)
                maxV = std::max(maxV, pt.value + 20);
            return maxV;
        };
        auto tempoValueAtY = [&](int y) {
            const int yc = std::clamp(y, rowTopPad, rowBottom);
            return (rowBottom - yc) * tempoMaxV() / (rowBottom - rowTopPad);
        };
        auto tempoValueY = [&](int v) {
            return rowBottom - v * (rowBottom - rowTopPad) / std::max(1, tempoMaxV());
        };
        auto tempoPoints = [&]() { return doc.lanePoints(laneTrack, DOC_CC_TEMPO); };
        auto pointsInSpan = [&](uint64_t a, uint64_t b) {
            size_t n = 0;
            for (const DocLanePoint &pt : tempoPoints())
                if (pt.tick > a && pt.tick <= b)
                    n++;
            return n;
        };

        if (view.automationPencilMode())
            fail("pencil mode should start off");
        sendKey(lanes, Qt::Key_B, Qt::NoModifier);
        if (!view.automationPencilMode())
            fail("B did not enable pencil mode");
        if (lanes->cursor().shape() != Qt::BitmapCursor)
            fail("pencil mode did not install the pencil cursor");
        const QPixmap expectedPencil = QIcon(QStringLiteral(":/cursors/pencil.png"))
                                           .pixmap(QSize(16, 16), lanes->devicePixelRatioF());
        if (lanes->cursor().pixmap().devicePixelRatio() != expectedPencil.devicePixelRatio() ||
            lanes->cursor().pixmap().toImage() != expectedPencil.toImage())
            fail("pencil mode did not install its DPI-matched cursor asset");
        {
            // A held key's auto-repeat presses must not strobe the mode.
            QKeyEvent repeat(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier, QString(), true);
            QCoreApplication::sendEvent(lanes, &repeat);
            if (!view.automationPencilMode())
                fail("auto-repeat B press toggled pencil mode off");
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_B, Qt::NoModifier);
            QCoreApplication::sendEvent(lanes, &release);
        }
        sendKey(lanes, Qt::Key_B, Qt::NoModifier);
        if (view.automationPencilMode())
            fail("B did not disable pencil mode");
        if (lanes->cursor().shape() == Qt::BitmapCursor)
            fail("leaving pencil mode kept the pencil cursor");

        // Park a tempo point with a pencil-mode click at a spot with clear
        // air (no existing dot within grab range in x). The arrow tool's
        // click writes nothing at all now; the pencil's still leaves the
        // single point these fixtures need.
        const qreal dprLanes = lanes->devicePixelRatioF();
        auto dotNear = [&](qreal x) {
            for (const LanePoint &pt : view.model().tempoLane)
                if (std::abs(view.displayX(double(pt.tick), songview::kGutterW, dprLanes) - x) < 24)
                    return true;
            return false;
        };
        qreal x0 = songview::kGutterW + (lanes->width() - songview::kGutterW) * 0.35;
        while (dotNear(x0))
            x0 += 40;
        const int y0 = 15;
        sendKey(lanes, Qt::Key_B, Qt::NoModifier);
        sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(x0), y0), Qt::LeftButton,
                  Qt::LeftButton);
        sendMouse(lanes, QEvent::MouseButtonRelease, QPoint(int(x0), y0), Qt::LeftButton,
                  Qt::NoButton);
        sendKey(lanes, Qt::Key_B, Qt::NoModifier);
        QCoreApplication::processEvents();
        const uint64_t t0 = view.snapTick(view.tickAtContentX(x0 - songview::kGutterW));
        DocLanePoint clickPt;
        if (!doc.findLanePoint(laneTrack, DOC_CC_TEMPO, t0, &clickPt))
            fail("pencil-mode click did not write a tempo point");
        if (clickPt.value != tempoValueAtY(y0))
            fail("test-side tempo value mirror drifted from the widget's mapping");
        const qreal xDot = view.displayX(double(t0), songview::kGutterW, dprLanes);
        const int yDot = tempoValueY(clickPt.value);
        // Only a self-consistency bound on the two lambdas (the widget-drift
        // coverage is the clickPt.value assert above); yDot is load-bearing
        // as the dot-press coordinate below.
        if (std::abs(yDot - y0) > 2)
            fail("tempo value<->y lambdas disagree on the round-trip");
        const uint64_t g = std::max<uint64_t>(1, view.gridTicksAt(t0));
        const qreal xEnd = view.displayX(double(t0 + 4 * g), songview::kGutterW, dprLanes);
        auto dragStroke = [&](int yFrom, int yTo, Qt::KeyboardModifiers mods) {
            sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(xDot), yFrom), Qt::LeftButton,
                      Qt::LeftButton, mods);
            for (int step = 1; step <= 4; step++) {
                const QPoint p(int(xDot + (xEnd - xDot) * step / 4),
                               yFrom + (yTo - yFrom) * step / 4);
                sendMouse(lanes, QEvent::MouseMove, p, Qt::NoButton, Qt::LeftButton, mods);
            }
            sendMouse(lanes, QEvent::MouseButtonRelease, QPoint(int(xEnd), yTo), Qt::LeftButton,
                      Qt::NoButton, mods);
            QCoreApplication::processEvents();
        };

        // Pencil over the dot: draws the crossed cells, leaves the pressed
        // point's tick alone. A stroke that climbs writes a node per cell
        // it crossed.
        sendKey(lanes, Qt::Key_B, Qt::NoModifier);
        dragStroke(yDot, yDot + 20, Qt::NoModifier);
        DocLanePoint probe;
        if (!doc.findLanePoint(laneTrack, DOC_CC_TEMPO, t0, &probe))
            fail("pencil press on a dot grabbed the point instead of drawing");
        if (pointsInSpan(t0, t0 + 4 * g) < 2)
            fail("pencil sweep over the dot did not draw the crossed cells");
        doc.undoStack()->undo();
        QCoreApplication::processEvents();

        // A flat stroke is ONE node, not one per grid cell: the lane holds
        // its value between points, so the cells after the first restate a
        // value that is already in force. The press's own node stays (a
        // pencil press always leaves a point), and the span it swept is
        // still cleared of whatever was there.
        dragStroke(yDot, yDot, Qt::NoModifier);
        if (!doc.findLanePoint(laneTrack, DOC_CC_TEMPO, t0, &probe))
            fail("flat pencil stroke did not leave its press node");
        if (probe.value != tempoValueAtY(yDot))
            fail("flat pencil stroke's node did not take the stroke's value");
        if (pointsInSpan(t0, t0 + 4 * g) != 0)
            fail("flat pencil stroke wrote duplicate nodes across the span");
        doc.undoStack()->undo();
        QCoreApplication::processEvents();

        // A live pencil stroke recolors exactly what it is about to
        // overwrite: the span it has covered so far paints in the
        // edit-preview color, the committed points inside that span stop
        // painting at all, and the curve outside it keeps the lane's own
        // color. Probed mid-stroke, before any release commits anything.
        {
            const QColor tempoColor = themes::color(themes::Role::song_view_automation_tempo_curve);
            const QColor previewColor = themes::color(themes::Role::song_view_edit_preview_outline);
            const int yDoomed = 40, yStroke = 28;
            // The stroke samples a value from the cursor's y, and the curve
            // paints that value's own row back — which the pixel probes want,
            // not the raw cursor row.
            const int yStrokeInk = tempoValueY(tempoValueAtY(yStroke));
            const uint64_t tDoomed = t0 + 4 * g;
            doc.writeLanePoints(laneTrack, DOC_CC_TEMPO, tDoomed, tDoomed,
                                {{tDoomed, tempoValueAtY(yDoomed)}});
            QCoreApplication::processEvents();
            const qreal xFrom = view.displayX(double(t0 + 2 * g), songview::kGutterW, dprLanes);
            const qreal xTo = view.displayX(double(t0 + 6 * g), songview::kGutterW, dprLanes);
            auto laneColorAt = [&](const QImage &img, qreal x, int y) {
                return img.pixelColor(int((x + 0.5) * dprLanes), int((y + 0.5) * dprLanes));
            };
            // Probe columns: the surviving hold between the parked point and
            // the stroke, the stroke's own span, and the doomed point's hold
            // just past its tick. All clear of the overlays' verticals.
            const qreal xSurvives = (xDot + xFrom) / 2;
            const qreal xInside = (xFrom + xTo) / 2;
            const qreal xDoomedHold =
                (view.displayX(double(tDoomed), songview::kGutterW, dprLanes) + xTo) / 2;
            if (overlayContestedX(view, dprLanes, xSurvives) ||
                overlayContestedX(view, dprLanes, xInside) ||
                overlayContestedX(view, dprLanes, xDoomedHold))
                fail("pencil preview setup: an overlay vertical crosses a probe column");
            if (laneColorAt(lanes->grab().toImage(), xDoomedHold, yDoomed) != tempoColor)
                fail("pencil preview setup: the doomed point's hold is not on the probe row");

            sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(xFrom), yStroke), Qt::LeftButton,
                      Qt::LeftButton);
            for (int step = 1; step <= 4; step++)
                sendMouse(lanes, QEvent::MouseMove,
                          QPoint(int(xFrom + (xTo - xFrom) * step / 4), yStroke), Qt::NoButton,
                          Qt::LeftButton);
            QCoreApplication::processEvents();
            {
                const QImage live = lanes->grab().toImage();
                // The stroke's own samples ring the span, so the preview
                // color is looked for around the held row rather than
                // exactly on it — a column that lands on a node shows the
                // ring, not the line.
                auto inColumn = [&](qreal x, int y, const QColor &want) {
                    for (int dy = -4; dy <= 4; dy++)
                        if (laneColorAt(live, x, y + dy) == want)
                            return true;
                    return false;
                };
                if (!inColumn(xInside, yStrokeInk, previewColor))
                    fail("the live pencil span did not paint in the preview color");
                if (inColumn(xInside, yStrokeInk, tempoColor))
                    fail("the live pencil span kept painting the lane's own color");
                if (laneColorAt(live, xSurvives, yDot) != tempoColor)
                    fail("the curve outside the live pencil span lost the lane's color");
                if (laneColorAt(live, xDoomedHold, yDoomed) == tempoColor)
                    fail("a point the live pencil span will overwrite still painted its hold");
            }
            sendMouse(lanes, QEvent::MouseButtonRelease, QPoint(int(xTo), yStroke), Qt::LeftButton,
                      Qt::NoButton);
            QCoreApplication::processEvents();
            doc.undoStack()->undo(); // the stroke
            doc.undoStack()->undo(); // the doomed point
            QCoreApplication::processEvents();
        }

        // Shift pencil stroke: a horizontal line at the pressed value even
        // though the cursor climbs.
        const int y1 = 35, y2 = 15;
        dragStroke(y1, y2, Qt::ShiftModifier);
        const int lockValue = tempoValueAtY(y1);
        for (const DocLanePoint &pt : tempoPoints())
            if (pt.tick >= t0 && pt.tick <= t0 + 4 * g && pt.value != lockValue)
                fail("Shift pencil stroke was not a horizontal line at the pressed value");
        if (!doc.findLanePoint(laneTrack, DOC_CC_TEMPO, t0, &probe) || probe.value != lockValue)
            fail("Shift pencil stroke did not leave its press node at the locked value");
        if (pointsInSpan(t0, t0 + 4 * g) != 0)
            fail("Shift pencil stroke wrote duplicate nodes along its horizontal line");
        doc.undoStack()->undo();
        QCoreApplication::processEvents();

        // Vertical slop: a stroke meant to be horizontal shouldn't pick up
        // the hand's wobble, so a drift shallower than the activation
        // distance (over travel far wider than it is tall) draws the same
        // flat line Shift would have — no lock held, no key pressed.
        const int slop = layout::fontPx(5.0 / 12.0);
        const int yDrift = std::max(1, slop - 2);
        dragStroke(yDot, yDot + yDrift, Qt::NoModifier);
        const int slopValue = tempoValueAtY(yDot);
        for (const DocLanePoint &pt : tempoPoints())
            if (pt.tick >= t0 && pt.tick <= t0 + 4 * g && pt.value != slopValue)
                fail("a sub-slop vertical drift bent the pencil stroke");
        if (pointsInSpan(t0, t0 + 4 * g) != 0)
            fail("a slop-held stroke still wrote a node per cell");
        doc.undoStack()->undo();
        QCoreApplication::processEvents();

        // ...and the resistance is a starting behavior, not a filter: once
        // the stroke commits to real vertical travel it follows the cursor
        // for the rest of its length.
        dragStroke(yDot, yDot + 8 * slop, Qt::NoModifier);
        bool bent = false;
        for (const DocLanePoint &pt : tempoPoints())
            if (pt.tick > t0 && pt.tick <= t0 + 4 * g && pt.value != slopValue)
                bent = true;
        if (!bent)
            fail("the pencil never broke out of its vertical slop");
        doc.undoStack()->undo();
        QCoreApplication::processEvents();

        // Toggling B mid-stroke must not change the in-flight gesture: the
        // press latched the pencil, so the Shift lock keeps holding even
        // after the mode flips off underneath it.
        sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(xDot), y1), Qt::LeftButton,
                  Qt::LeftButton, Qt::ShiftModifier);
        for (int step = 1; step <= 4; step++) {
            if (step == 3)
                sendKey(lanes, Qt::Key_B, Qt::NoModifier);
            const QPoint p(int(xDot + (xEnd - xDot) * step / 4), y1 + (y2 - y1) * step / 4);
            sendMouse(lanes, QEvent::MouseMove, p, Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
        }
        sendMouse(lanes, QEvent::MouseButtonRelease, QPoint(int(xEnd), y2), Qt::LeftButton,
                  Qt::NoButton, Qt::ShiftModifier);
        QCoreApplication::processEvents();
        if (view.automationPencilMode())
            fail("mid-drag B press did not toggle the mode itself");
        for (const DocLanePoint &pt : tempoPoints())
            if (pt.tick >= t0 && pt.tick <= t0 + 4 * g && pt.value != lockValue)
                fail("mid-drag B toggle changed the in-flight Shift-locked stroke");
        doc.undoStack()->undo();
        QCoreApplication::processEvents();

        // Momentary hold: pressing and holding the key is a chord, not a
        // toggle. A stroke drawn during the hold reverts the mode on
        // release even when the hold was quick...
        {
            QKeyEvent press(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier);
            QCoreApplication::sendEvent(lanes, &press);
        }
        if (!view.automationPencilMode())
            fail("pencil-key press did not enter the momentary mode");
        dragStroke(y1, y1, Qt::NoModifier);
        if (!doc.findLanePoint(laneTrack, DOC_CC_TEMPO, t0, &probe) ||
            probe.value != tempoValueAtY(y1))
            fail("hold-to-draw stroke did not draw");
        {
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_B, Qt::NoModifier);
            QCoreApplication::sendEvent(lanes, &release);
        }
        if (view.automationPencilMode())
            fail("drawing during the hold did not make the toggle momentary");
        doc.undoStack()->undo();
        QCoreApplication::processEvents();

        // ...and so does simply holding past the threshold with no stroke.
        {
            QKeyEvent press(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier);
            QCoreApplication::sendEvent(lanes, &press);
        }
        QThread::msleep(520);
        {
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_B, Qt::NoModifier);
            QCoreApplication::sendEvent(lanes, &release);
        }
        if (view.automationPencilMode())
            fail("holding the pencil key past the threshold stayed sticky");
        sendKey(lanes, Qt::Key_B, Qt::NoModifier); // pencil back on

        // Without Shift the same stroke follows the cursor.
        dragStroke(y1, y2, Qt::NoModifier);
        DocLanePoint first, last;
        if (!doc.findLanePoint(laneTrack, DOC_CC_TEMPO, t0, &first) ||
            !doc.findLanePoint(laneTrack, DOC_CC_TEMPO, t0 + 4 * g, &last))
            fail("unlocked pencil stroke did not draw its endpoints");
        else if (first.value == last.value)
            fail("unlocked pencil stroke did not follow the cursor");
        doc.undoStack()->undo();
        QCoreApplication::processEvents();

        // Arrow mode untouched: the dot grab-move and the off-dot Shift
        // ramp (only the press position matters to grabPoint, and the
        // press at y1 sits well outside the dot's 7 px grab slop).
        sendKey(lanes, Qt::Key_B, Qt::NoModifier);
        dragStroke(yDot, yDot, Qt::NoModifier);
        if (doc.findLanePoint(laneTrack, DOC_CC_TEMPO, t0, &probe))
            fail("arrow-mode press on the dot did not grab-move the point");
        doc.undoStack()->undo();
        QCoreApplication::processEvents();
        dragStroke(y1, y2, Qt::ShiftModifier);
        if (!doc.findLanePoint(laneTrack, DOC_CC_TEMPO, t0, &first) ||
            !doc.findLanePoint(laneTrack, DOC_CC_TEMPO, t0 + 4 * g, &last))
            fail("arrow-mode Shift ramp did not write its endpoints");
        else if (first.value == last.value)
            fail("arrow-mode Shift ramp collapsed to a horizontal line");
        while (doc.undoStack()->index() > undoIndex && doc.undoStack()->canUndo())
            doc.undoStack()->undo();
        QCoreApplication::processEvents();
    }

    // Arrow-tool click semantics in the lanes: with the pencil owning
    // freehand drawing, a left click never creates data. On empty lane
    // space it parks the edit cursor and writes nothing; a freehand sweep
    // only starts once the travel clears the activation slop
    // (layout::fontPx(5/12)), and that slop is subtracted so crossing the
    // threshold is not itself motion. On a node the click deletes it in one
    // undo entry (Shift, which means an axis-locked drag, spares it), which
    // spends the pair: the double-click that follows is a no-op. Empty-
    // space double-click still opens the value type-in.
    {
        const int undoIndex = doc.undoStack()->index();
        const int laneTrack = view.selectedTrack();
        const int slop = layout::fontPx(5.0 / 12.0);
        // Test-side mirror of the tempo row's value<->y mapping, as in the
        // pencil section above (row 0, default 48 px height).
        const int rowTopPad = 5, rowBottom = 48 - 1 - 4;
        auto tempoMaxV = [&]() {
            int maxV = 200;
            for (const LanePoint &pt : view.model().tempoLane)
                maxV = std::max(maxV, pt.value + 20);
            return maxV;
        };
        auto tempoValueAtY = [&](int y) {
            const int yc = std::clamp(y, rowTopPad, rowBottom);
            return (rowBottom - yc) * tempoMaxV() / (rowBottom - rowTopPad);
        };
        auto tempoValueY = [&](int v) {
            return rowBottom - v * (rowBottom - rowTopPad) / std::max(1, tempoMaxV());
        };
        auto tempoPoints = [&]() { return doc.lanePoints(laneTrack, DOC_CC_TEMPO); };
        // Any value type-in that opens gets rejected: an unattended modal
        // would wedge the harness, and every probe here asserts on whether
        // one appeared at all.
        bool typeInSeen = false;
        QTimer typeInPoll;
        typeInPoll.setInterval(0);
        QObject::connect(&typeInPoll, &QTimer::timeout, [&] {
            if (auto *dlg = lanes->findChild<QInputDialog *>()) {
                typeInSeen = true;
                dlg->reject();
            }
        });

        const qreal dprLanes = lanes->devicePixelRatioF();
        auto dotNear = [&](qreal x) {
            for (const LanePoint &pt : view.model().tempoLane)
                if (std::abs(view.displayX(double(pt.tick), songview::kGutterW, dprLanes) - x) < 24)
                    return true;
            return false;
        };
        qreal xClick = songview::kGutterW + (lanes->width() - songview::kGutterW) * 0.55;
        while (dotNear(xClick))
            xClick += 40;
        const int yClick = 20;
        const QPoint clickPos(int(xClick), yClick);
        const uint64_t tClick =
            view.snapTick(view.tickAtContentX(int(xClick) - songview::kGutterW));
        auto press = [&](QPoint pos, Qt::KeyboardModifiers mods = Qt::NoModifier) {
            sendMouse(lanes, QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton, mods);
        };
        auto move = [&](QPoint pos, Qt::KeyboardModifiers mods = Qt::NoModifier) {
            sendMouse(lanes, QEvent::MouseMove, pos, Qt::NoButton, Qt::LeftButton, mods);
        };
        auto release = [&](QPoint pos, Qt::KeyboardModifiers mods = Qt::NoModifier) {
            sendMouse(lanes, QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton, mods);
            QCoreApplication::processEvents();
        };

        // A plain click on empty lane space: nothing written, edit cursor
        // parked at the click's snapped tick (which the setup moves away
        // from first, so the assert can't pass by accident).
        view.commitEditCursor(0);
        QCoreApplication::processEvents();
        QByteArray before = doc.smf().write();
        int undoBefore = doc.undoStack()->index();
        uint64_t revisionBefore = doc.revision();
        press(clickPos);
        release(clickPos);
        if (doc.smf().write() != before || doc.undoStack()->index() != undoBefore ||
            doc.revision() != revisionBefore)
            fail("an arrow-tool click on empty lane space wrote to the document");
        if (tClick == 0)
            fail("arrow-click probe landed on tick 0, where the cursor already sat");
        else if (view.editCursorTick() != tClick)
            fail("an arrow-tool click on empty lane space did not park the edit cursor");

        // Hand jitter below the activation distance, and a drag of exactly
        // the activation distance: both are still clicks, so the document
        // stays byte-identical and the edit cursor still parks — the click
        // boundary is continuous.
        for (const int travel : {std::max(0, slop - 1), slop}) {
            view.commitEditCursor(0);
            QCoreApplication::processEvents();
            press(clickPos);
            move(clickPos + QPoint(0, travel));
            release(clickPos + QPoint(0, travel));
            if (doc.smf().write() != before || doc.undoStack()->index() != undoBefore ||
                doc.revision() != revisionBefore)
                fail(travel < slop ? "sub-slop jitter changed the document"
                                   : "the sweep's activation slop counted as motion");
            if (view.editCursorTick() != tClick)
                fail("a sweep that committed nothing did not park the edit cursor");
        }

        // The slop offset is the slop itself, never the whole first sample:
        // mouse moves arrive coalesced, so a fast flick's first event can
        // land far past the threshold, and subtracting all of it would
        // strand the stroke that far from the cursor for its whole length.
        constexpr int kFlick = 20;
        const int flickValue = tempoValueAtY(yClick + kFlick - slop);
        press(clickPos);
        move(clickPos + QPoint(0, kFlick));
        move(clickPos + QPoint(0, kFlick));
        release(clickPos + QPoint(0, kFlick));
        DocLanePoint flicked;
        if (!doc.findLanePoint(laneTrack, DOC_CC_TEMPO, tClick, &flicked))
            fail("the flicked sweep did not draw");
        else if (flicked.value != flickValue)
            fail("the sweep subtracted more than its activation slop");
        doc.undoStack()->undo();
        QCoreApplication::processEvents();

        // The flick's undo restored the bytes but not the revision counter.
        before = doc.smf().write();
        undoBefore = doc.undoStack()->index();
        revisionBefore = doc.revision();

        // One pixel past the slop draws — and only that one pixel of it:
        // the slop offset is subtracted, so the stroke sits at the press's
        // tick with the value one pixel below the press, not slop + 1.
        const size_t pointsBefore = tempoPoints().size();
        const int sweptValue = tempoValueAtY(yClick + 1);
        press(clickPos);
        move(clickPos + QPoint(0, slop));
        move(clickPos + QPoint(0, slop + 1));
        release(clickPos + QPoint(0, slop + 1));
        DocLanePoint swept;
        if (!doc.findLanePoint(laneTrack, DOC_CC_TEMPO, tClick, &swept) ||
            tempoPoints().size() != pointsBefore + 1 ||
            doc.undoStack()->index() != undoBefore + 1 || doc.revision() != revisionBefore + 1)
            fail("a sweep one pixel past the activation slop did not draw exactly one point");
        else if (swept.value != sweptValue)
            fail("the sweep drew the activation slop as movement");

        // A click on that node deletes it: one undo entry, no type-in.
        const QPoint dotPos(int(view.displayX(double(tClick), songview::kGutterW, dprLanes)),
                            tempoValueY(swept.value));
        before = doc.smf().write();
        undoBefore = doc.undoStack()->index();
        revisionBefore = doc.revision();
        typeInSeen = false;
        typeInPoll.start();
        press(dotPos);
        release(dotPos);
        typeInPoll.stop();
        DocLanePoint gone;
        if (doc.findLanePoint(laneTrack, DOC_CC_TEMPO, tClick, &gone) ||
            tempoPoints().size() != pointsBefore || doc.undoStack()->index() != undoBefore + 1 ||
            doc.revision() != revisionBefore + 1)
            fail("an arrow-tool click on a node did not delete it as one undo entry");
        if (typeInSeen)
            fail("the node click opened a value type-in");

        // The pair is spent: the double-click that completes it must not
        // type a value into the empty space the delete just made.
        const QByteArray afterDelete = doc.smf().write();
        typeInSeen = false;
        typeInPoll.start();
        sendMouse(lanes, QEvent::MouseButtonDblClick, dotPos, Qt::LeftButton, Qt::LeftButton);
        release(dotPos);
        typeInPoll.stop();
        if (typeInSeen || doc.smf().write() != afterDelete)
            fail("the double-click after a node click-delete was not a no-op");
        doc.undoStack()->undo(); // the click-delete
        QCoreApplication::processEvents();

        // Shift means an axis-locked drag, so a Shift click spares the node
        // — and its double-click still opens no type-in.
        before = doc.smf().write();
        undoBefore = doc.undoStack()->index();
        typeInSeen = false;
        typeInPoll.start();
        press(dotPos, Qt::ShiftModifier);
        release(dotPos, Qt::ShiftModifier);
        sendMouse(lanes, QEvent::MouseButtonDblClick, dotPos, Qt::LeftButton, Qt::LeftButton);
        release(dotPos);
        typeInPoll.stop();
        if (doc.smf().write() != before || doc.undoStack()->index() != undoBefore)
            fail("a Shift click on a node changed the document");
        if (typeInSeen)
            fail("a double-click on a surviving node opened a value type-in");

        // A drag that cleared the activation slop is a drag for good, even
        // if it wanders back onto the node's own tick and value: it commits
        // nothing rather than reading as a click and deleting the node.
        // The return has to land on a y that maps back to the node's own
        // value, or the drag commits a legitimate one-value nudge and the
        // probe would pass without testing anything.
        int yRound = -1;
        for (int y = rowTopPad; y <= rowBottom && yRound < 0; y++)
            if (tempoValueAtY(y) == swept.value)
                yRound = y;
        const QPoint origin(dotPos.x(), yRound);
        before = doc.smf().write();
        undoBefore = doc.undoStack()->index();
        if (yRound < 0)
            fail("no pixel row maps back to the parked node's value");
        press(dotPos);
        move(origin + QPoint(slop + 20, 0));
        move(origin);
        release(origin);
        if (doc.smf().write() != before || doc.undoStack()->index() != undoBefore)
            fail("a point drag that returned to its origin was treated as a click");

        // Shift usually lands after the button, and a parked Shift press
        // alone pins the drag back to the node's original position — which
        // must not read as a click either.
        before = doc.smf().write();
        undoBefore = doc.undoStack()->index();
        press(dotPos);
        {
            // Press only: releasing Shift mid-drag deliberately frees the
            // drag again (the axis lock's own contract), which would
            // re-derive the value from the pixel and commit a real move.
            QKeyEvent shiftDown(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
            QCoreApplication::sendEvent(lanes, &shiftDown);
        }
        release(dotPos, Qt::ShiftModifier);
        if (doc.smf().write() != before || doc.undoStack()->index() != undoBefore)
            fail("Shift pressed after the button on a node did not spare it");

        // Same-tick duplicates: the click deletes the dot it grabbed, not
        // whichever event the tick resolves to. A raw fractional-BPM meta
        // (120.5) parked after a written point at the same tick is the
        // audible one, so a tick-only aim would delete it instead.
        {
            const uint64_t tDup = tClick;
            constexpr int vGrab = 40;
            doc.writeLanePoints(laneTrack, DOC_CC_TEMPO, tDup, tDup, {{tDup, vGrab}});
            SmfEvent frac;
            frac.tick = tDup;
            frac.status = 0xFF;
            frac.metaType = 0x51;
            frac.blob.resize(3); // 497925 us/beat = 120.5 BPM
            frac.blob[0] = char(0x07);
            frac.blob[1] = char(0x99);
            frac.blob[2] = char(0x05);
            doc.insertRawEvent(0, frac);
            QCoreApplication::processEvents();
            size_t atTick = 0;
            for (const DocLanePoint &pt : tempoPoints())
                if (pt.tick == tDup)
                    atTick++;
            DocLanePoint audible;
            if (atTick != 2 || !doc.findLanePoint(laneTrack, DOC_CC_TEMPO, tDup, &audible) ||
                audible.value == vGrab)
                fail("duplicate-aim setup: the tick does not resolve to the other point");
            const int dupUndo = doc.undoStack()->index();
            press(QPoint(dotPos.x(), tempoValueY(vGrab)));
            release(QPoint(dotPos.x(), tempoValueY(vGrab)));
            DocLanePoint survivor;
            bool grabbedGone = true;
            for (const DocLanePoint &pt : tempoPoints())
                if (pt.tick == tDup && pt.value == vGrab)
                    grabbedGone = false;
            if (!grabbedGone || doc.undoStack()->index() != dupUndo + 1 ||
                !doc.findLanePoint(laneTrack, DOC_CC_TEMPO, tDup, &survivor) ||
                survivor.value != audible.value)
                fail("the click deleted a same-tick sibling instead of the grabbed dot");
            doc.undoStack()->undo(); // the click-delete
            doc.undoStack()->undo(); // the raw fractional-tempo insert
            doc.undoStack()->undo(); // the written point
            QCoreApplication::processEvents();
        }

        // Empty space keeps the double-click type-in — the way a node's
        // exact value is now typed is the point menu's Set value…
        typeInSeen = false;
        typeInPoll.start();
        // The whole pair, since the first click is what clears the spent
        // flag a preceding click-delete left behind.
        press(clickPos + QPoint(0, 12));
        release(clickPos + QPoint(0, 12));
        sendMouse(lanes, QEvent::MouseButtonDblClick, clickPos + QPoint(0, 12), Qt::LeftButton,
                  Qt::LeftButton);
        release(clickPos + QPoint(0, 12));
        typeInPoll.stop();
        if (!typeInSeen)
            fail("a double-click on empty lane space did not open the value type-in");

        while (doc.undoStack()->index() > undoIndex && doc.undoStack()->canUndo())
            doc.undoStack()->undo();
        QCoreApplication::processEvents();
    }

    // Shift axis lock on a point drag: Shift+press on a point's dot grabs
    // the point — the ramp now starts only off-dot — and once the travel
    // passes the activation distance (layout::fontPx(5/12)) the dominant
    // direction locks the drag to one axis. Mostly-horizontal keeps the
    // value exact while the tick follows (SizeHor cursor); mostly-vertical
    // keeps the tick while the value follows (SizeVer). The lock is sticky
    // across a later 45° cross, releasing Shift mid-drag frees the drag,
    // re-pressing re-resolves from the total travel since the press, and
    // the release restores the tool cursor.
    {
        const int undoIndex = doc.undoStack()->index();
        const int laneTrack = view.selectedTrack();
        // Test-side mirror of the tempo row's value<->y mapping, as in the
        // pencil section above (row 0, default 48 px height). All vertical
        // motion here goes downward (values shrink) so no commit disturbs
        // the auto-fit ceiling the mirror bakes in.
        const int rowTopPad = 5, rowBottom = 48 - 1 - 4;
        auto tempoMaxV = [&]() {
            int maxV = 200;
            for (const LanePoint &pt : view.model().tempoLane)
                maxV = std::max(maxV, pt.value + 20);
            return maxV;
        };
        auto tempoValueAtY = [&](int y) {
            const int yc = std::clamp(y, rowTopPad, rowBottom);
            return (rowBottom - yc) * tempoMaxV() / (rowBottom - rowTopPad);
        };
        auto tempoValueY = [&](int v) {
            return rowBottom - v * (rowBottom - rowTopPad) / std::max(1, tempoMaxV());
        };
        auto tempoPointAt = [&](uint64_t tick, DocLanePoint *pt) {
            return doc.findLanePoint(laneTrack, DOC_CC_TEMPO, tick, pt);
        };
        auto pointsInSpan = [&](uint64_t a, uint64_t b) {
            size_t n = 0;
            for (const DocLanePoint &pt : doc.lanePoints(laneTrack, DOC_CC_TEMPO))
                if (pt.tick > a && pt.tick <= b)
                    n++;
            return n;
        };

        // Park a tempo point with a pencil-mode click in clear air (the
        // arrow tool's click writes nothing, as the section above proves).
        const qreal dprLanes = lanes->devicePixelRatioF();
        auto dotNear = [&](qreal x) {
            for (const LanePoint &pt : view.model().tempoLane)
                if (std::abs(view.displayX(double(pt.tick), songview::kGutterW, dprLanes) - x) < 24)
                    return true;
            return false;
        };
        qreal x0 = songview::kGutterW + (lanes->width() - songview::kGutterW) * 0.35;
        while (dotNear(x0))
            x0 += 40;
        const int y0 = 20;
        sendKey(lanes, Qt::Key_B, Qt::NoModifier);
        sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(x0), y0), Qt::LeftButton,
                  Qt::LeftButton);
        sendMouse(lanes, QEvent::MouseButtonRelease, QPoint(int(x0), y0), Qt::LeftButton,
                  Qt::NoButton);
        sendKey(lanes, Qt::Key_B, Qt::NoModifier);
        QCoreApplication::processEvents();
        const uint64_t t0 = view.snapTick(view.tickAtContentX(x0 - songview::kGutterW));
        DocLanePoint parked;
        if (!tempoPointAt(t0, &parked))
            fail("axis-lock setup: pencil click did not write a tempo point");
        if (parked.value != tempoValueAtY(y0))
            fail("test-side tempo value mirror drifted from the widget's mapping");
        const qreal xDot = view.displayX(double(t0), songview::kGutterW, dprLanes);
        const int yDot = tempoValueY(parked.value);
        const uint64_t g = std::max<uint64_t>(1, view.gridTicksAt(t0));
        const uint64_t tEnd = t0 + 4 * g;
        const qreal xEnd = view.displayX(double(tEnd), songview::kGutterW, dprLanes);
        // Comfortably past the lock's activation travel.
        const int arm = layout::fontPx(5.0 / 12.0) + 8;
        DocLanePoint probe;

        // Time lock: Shift+press on the dot, mostly-horizontal first
        // travel. The y wobble on the way must not touch the value.
        sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(xDot), yDot), Qt::LeftButton,
                  Qt::LeftButton, Qt::ShiftModifier);
        sendMouse(lanes, QEvent::MouseMove, QPoint(int(xDot) + arm, yDot + 2), Qt::NoButton,
                  Qt::LeftButton, Qt::ShiftModifier);
        if (lanes->cursor().shape() != Qt::SizeHorCursor)
            fail("time lock did not show the horizontal drag cursor");
        sendMouse(lanes, QEvent::MouseMove, QPoint(int(xEnd), yDot + 6), Qt::NoButton,
                  Qt::LeftButton, Qt::ShiftModifier);
        sendMouse(lanes, QEvent::MouseButtonRelease, QPoint(int(xEnd), yDot + 6), Qt::LeftButton,
                  Qt::NoButton, Qt::ShiftModifier);
        QCoreApplication::processEvents();
        if (tempoPointAt(t0, &probe))
            fail("Shift press on the dot did not grab the point (ramp or copy left at t0)");
        if (!tempoPointAt(tEnd, &probe))
            fail("time lock did not move the point's tick to the cursor");
        else if (probe.value != parked.value)
            fail("time lock did not keep the point's value exact");
        if (lanes->cursor().shape() != Qt::ArrowCursor)
            fail("releasing the locked drag did not restore the arrow cursor");
        doc.undoStack()->undo();
        QCoreApplication::processEvents();

        // Value lock: mostly-vertical first travel freezes the tick; a
        // full grid cell of later horizontal travel stays pinned (sticky).
        sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(xDot), yDot), Qt::LeftButton,
                  Qt::LeftButton, Qt::ShiftModifier);
        sendMouse(lanes, QEvent::MouseMove, QPoint(int(xDot) + 1, yDot + arm), Qt::NoButton,
                  Qt::LeftButton, Qt::ShiftModifier);
        if (lanes->cursor().shape() != Qt::SizeVerCursor)
            fail("value lock did not show the vertical drag cursor");
        const int yValEnd = yDot + arm + 4;
        const qreal xCell = view.displayX(double(t0 + g), songview::kGutterW, dprLanes);
        sendMouse(lanes, QEvent::MouseMove, QPoint(int(xCell), yValEnd), Qt::NoButton,
                  Qt::LeftButton, Qt::ShiftModifier);
        sendMouse(lanes, QEvent::MouseButtonRelease, QPoint(int(xCell), yValEnd), Qt::LeftButton,
                  Qt::NoButton, Qt::ShiftModifier);
        QCoreApplication::processEvents();
        if (!tempoPointAt(t0, &probe))
            fail("value lock did not keep the point's tick frozen");
        else if (probe.value != tempoValueAtY(yValEnd))
            fail("value lock did not follow the cursor's value");
        doc.undoStack()->undo();
        QCoreApplication::processEvents();

        // Sticky time lock: arm horizontally, then pull the total travel
        // back near the origin and well past 45° vertical — the value must
        // stay pinned anyway. The 5 px pull-back must sit inside half a
        // snap cell (the snap grid runs one ladder step finer than the
        // drawn grid) so the release lands back on t0 and a correct lock
        // commits nothing at all; assert the geometry instead of assuming
        // it.
        const qreal xSnapCell =
            view.displayX(double(t0 + view.snapTicksAt(t0)), songview::kGutterW, dprLanes);
        if ((xSnapCell - xDot) / 2 <= 5.0)
            fail("axis-lock probe setup: the pull-back is not inside half a snap cell");
        sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(xDot), yDot), Qt::LeftButton,
                  Qt::LeftButton, Qt::ShiftModifier);
        sendMouse(lanes, QEvent::MouseMove, QPoint(int(xDot) + arm, yDot + 1), Qt::NoButton,
                  Qt::LeftButton, Qt::ShiftModifier);
        sendMouse(lanes, QEvent::MouseMove, QPoint(int(xDot) + 5, yDot + arm + 4), Qt::NoButton,
                  Qt::LeftButton, Qt::ShiftModifier);
        sendMouse(lanes, QEvent::MouseButtonRelease, QPoint(int(xDot) + 5, yDot + arm + 4),
                  Qt::LeftButton, Qt::NoButton, Qt::ShiftModifier);
        QCoreApplication::processEvents();
        if (!tempoPointAt(t0, &probe) || probe.value != parked.value)
            fail("time lock flipped to a value lock after crossing 45 degrees");
        if (doc.undoStack()->index() != undoIndex + 1)
            fail("sticky locked drag back to its origin was not a no-op commit");
        QCoreApplication::processEvents();

        // Releasing Shift mid-drag frees the drag: the value follows the
        // cursor again and the lock cursor yields to the tool cursor.
        sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(xDot), yDot), Qt::LeftButton,
                  Qt::LeftButton, Qt::ShiftModifier);
        sendMouse(lanes, QEvent::MouseMove, QPoint(int(xDot) + arm, yDot + 2), Qt::NoButton,
                  Qt::LeftButton, Qt::ShiftModifier);
        sendMouse(lanes, QEvent::MouseMove, QPoint(int(xEnd), yDot + 8), Qt::NoButton,
                  Qt::LeftButton, Qt::NoModifier);
        if (lanes->cursor().shape() != Qt::ArrowCursor)
            fail("releasing Shift mid-drag kept the lock cursor");
        sendMouse(lanes, QEvent::MouseButtonRelease, QPoint(int(xEnd), yDot + 8), Qt::LeftButton,
                  Qt::NoButton, Qt::NoModifier);
        QCoreApplication::processEvents();
        if (!tempoPointAt(tEnd, &probe))
            fail("freed drag did not move the point's tick to the cursor");
        else if (probe.value != tempoValueAtY(yDot + 8))
            fail("releasing Shift mid-drag did not free the value axis");
        doc.undoStack()->undo();
        QCoreApplication::processEvents();

        // Re-pressing Shift re-resolves from the total travel since the
        // press: net-vertical by then, so the tick pins back to the origin.
        sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(xDot), yDot), Qt::LeftButton,
                  Qt::LeftButton, Qt::ShiftModifier);
        sendMouse(lanes, QEvent::MouseMove, QPoint(int(xDot) + arm, yDot), Qt::NoButton,
                  Qt::LeftButton, Qt::ShiftModifier);
        sendMouse(lanes, QEvent::MouseMove, QPoint(int(xDot) + 2, yDot + arm + 3), Qt::NoButton,
                  Qt::LeftButton, Qt::NoModifier);
        sendMouse(lanes, QEvent::MouseMove, QPoint(int(xDot) + 2, yDot + arm + 4), Qt::NoButton,
                  Qt::LeftButton, Qt::ShiftModifier);
        if (lanes->cursor().shape() != Qt::SizeVerCursor)
            fail("re-pressed Shift did not re-resolve to a value lock");
        sendMouse(lanes, QEvent::MouseButtonRelease, QPoint(int(xDot) + 2, yDot + arm + 4),
                  Qt::LeftButton, Qt::NoButton, Qt::ShiftModifier);
        QCoreApplication::processEvents();
        if (!tempoPointAt(t0, &probe) || probe.value != tempoValueAtY(yDot + arm + 4))
            fail("re-pressed Shift lock did not pin the tick back to the origin");
        doc.undoStack()->undo();
        QCoreApplication::processEvents();

        // Stationary Shift release mid-drag: no mouse move follows the key
        // event, which alone must free the drag (un-pinning the value) and
        // its cursor.
        sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(xDot), yDot), Qt::LeftButton,
                  Qt::LeftButton, Qt::ShiftModifier);
        sendMouse(lanes, QEvent::MouseMove, QPoint(int(xEnd), yDot + 9), Qt::NoButton,
                  Qt::LeftButton, Qt::ShiftModifier);
        if (lanes->cursor().shape() != Qt::SizeHorCursor)
            fail("stationary-release setup: time lock did not engage");
        {
            QKeyEvent shiftUp(QEvent::KeyRelease, Qt::Key_Shift, Qt::NoModifier);
            QCoreApplication::sendEvent(lanes, &shiftUp);
        }
        if (lanes->cursor().shape() != Qt::ArrowCursor)
            fail("stationary Shift release mid-drag kept the lock cursor");
        sendMouse(lanes, QEvent::MouseButtonRelease, QPoint(int(xEnd), yDot + 9), Qt::LeftButton,
                  Qt::NoButton, Qt::NoModifier);
        QCoreApplication::processEvents();
        if (!tempoPointAt(tEnd, &probe) || probe.value != tempoValueAtY(yDot + 9))
            fail("stationary Shift release mid-drag did not free the pinned value");
        doc.undoStack()->undo();
        QCoreApplication::processEvents();

        // ...and a stationary Shift press locks instantly from the total
        // travel, re-pinning the value to the point's original.
        sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(xDot), yDot), Qt::LeftButton,
                  Qt::LeftButton);
        sendMouse(lanes, QEvent::MouseMove, QPoint(int(xEnd), yDot + 7), Qt::NoButton,
                  Qt::LeftButton);
        {
            QKeyEvent shiftDown(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
            QCoreApplication::sendEvent(lanes, &shiftDown);
        }
        if (lanes->cursor().shape() != Qt::SizeHorCursor)
            fail("stationary Shift press mid-drag did not engage the lock");
        sendMouse(lanes, QEvent::MouseButtonRelease, QPoint(int(xEnd), yDot + 7), Qt::LeftButton,
                  Qt::NoButton, Qt::ShiftModifier);
        QCoreApplication::processEvents();
        if (!tempoPointAt(tEnd, &probe) || probe.value != parked.value)
            fail("stationary Shift press mid-drag did not re-pin the value");
        doc.undoStack()->undo();
        QCoreApplication::processEvents();

        // A middle-button pan interrupting a locked drag hands the cursor
        // back to the lock when it ends, not to the idle tool.
        sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(xDot), yDot), Qt::LeftButton,
                  Qt::LeftButton, Qt::ShiftModifier);
        sendMouse(lanes, QEvent::MouseMove, QPoint(int(xEnd), yDot + 5), Qt::NoButton,
                  Qt::LeftButton, Qt::ShiftModifier);
        sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(xEnd), yDot + 5), Qt::MiddleButton,
                  Qt::LeftButton | Qt::MiddleButton, Qt::ShiftModifier);
        sendMouse(lanes, QEvent::MouseButtonRelease, QPoint(int(xEnd), yDot + 5), Qt::MiddleButton,
                  Qt::LeftButton, Qt::ShiftModifier);
        if (lanes->cursor().shape() != Qt::SizeHorCursor)
            fail("middle pan mid-drag stranded the lock cursor");
        sendMouse(lanes, QEvent::MouseButtonRelease, QPoint(int(xEnd), yDot + 5), Qt::LeftButton,
                  Qt::NoButton, Qt::ShiftModifier);
        QCoreApplication::processEvents();
        if (!tempoPointAt(tEnd, &probe) || probe.value != parked.value)
            fail("middle pan mid-drag broke the locked commit");
        doc.undoStack()->undo();
        QCoreApplication::processEvents();

        // A document change mid-drag aborts the gesture through
        // rebuildRows, which must not strand the lock cursor either.
        sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(xDot), yDot), Qt::LeftButton,
                  Qt::LeftButton, Qt::ShiftModifier);
        sendMouse(lanes, QEvent::MouseMove, QPoint(int(xEnd), yDot + 5), Qt::NoButton,
                  Qt::LeftButton, Qt::ShiftModifier);
        if (lanes->cursor().shape() != Qt::SizeHorCursor)
            fail("rebuild-abort setup: time lock did not engage");
        doc.undoStack()->undo(); // takes the parked point away mid-drag
        QCoreApplication::processEvents();
        if (lanes->cursor().shape() != Qt::ArrowCursor)
            fail("document change mid-drag stranded the lock cursor");
        sendMouse(lanes, QEvent::MouseButtonRelease, QPoint(int(xEnd), yDot + 5), Qt::LeftButton,
                  Qt::NoButton, Qt::ShiftModifier);
        QCoreApplication::processEvents();
        doc.undoStack()->redo(); // park the point again for the probes below
        QCoreApplication::processEvents();

        // Off the dot Shift still starts the ramp — and a true ramp: the
        // committed midpoint interpolates the press/release anchors,
        // ignoring the dipped mid-drag sample a sweep would have followed.
        const int yRampFrom = yDot + 12, yRampTo = yDot + 4;
        sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(xDot), yRampFrom), Qt::LeftButton,
                  Qt::LeftButton, Qt::ShiftModifier);
        const qreal xMid = view.displayX(double(t0 + 2 * g), songview::kGutterW, dprLanes);
        sendMouse(lanes, QEvent::MouseMove, QPoint(int(xMid), rowBottom), Qt::NoButton,
                  Qt::LeftButton, Qt::ShiftModifier);
        sendMouse(lanes, QEvent::MouseMove, QPoint(int(xEnd), yRampTo), Qt::NoButton,
                  Qt::LeftButton, Qt::ShiftModifier);
        sendMouse(lanes, QEvent::MouseButtonRelease, QPoint(int(xEnd), yRampTo), Qt::LeftButton,
                  Qt::NoButton, Qt::ShiftModifier);
        QCoreApplication::processEvents();
        const int vRampFrom = tempoValueAtY(yRampFrom);
        const int vRampTo = tempoValueAtY(yRampTo);
        const int vRampMid =
            vRampFrom +
            int(std::llround(double(vRampTo - vRampFrom) * double(2 * g) / double(tEnd - t0)));
        if (!tempoPointAt(t0, &probe) || probe.value != vRampFrom)
            fail("off-dot Shift press did not anchor a ramp at the press value");
        if (!tempoPointAt(t0 + 2 * g, &probe) || probe.value != vRampMid)
            fail("off-dot Shift drag did not commit an interpolated ramp");
        if (!tempoPointAt(tEnd, &probe) || probe.value != vRampTo)
            fail("off-dot Shift ramp endpoint drifted from the release position");
        doc.undoStack()->undo();
        QCoreApplication::processEvents();

        // Pencil mode is a different code path: Shift+press on the dot
        // sweeps with the horizontal stroke lock, never a point grab.
        sendKey(lanes, Qt::Key_B, Qt::NoModifier);
        sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(xDot), yDot), Qt::LeftButton,
                  Qt::LeftButton, Qt::ShiftModifier);
        sendMouse(lanes, QEvent::MouseMove, QPoint(int(xCell), yDot + 6), Qt::NoButton,
                  Qt::LeftButton, Qt::ShiftModifier);
        sendMouse(lanes, QEvent::MouseMove, QPoint(int(xEnd), yDot + 10), Qt::NoButton,
                  Qt::LeftButton, Qt::ShiftModifier);
        sendMouse(lanes, QEvent::MouseButtonRelease, QPoint(int(xEnd), yDot + 10), Qt::LeftButton,
                  Qt::NoButton, Qt::ShiftModifier);
        QCoreApplication::processEvents();
        sendKey(lanes, Qt::Key_B, Qt::NoModifier);
        if (!tempoPointAt(t0, &probe))
            fail("pencil-mode Shift press on the dot grabbed the point instead of drawing");
        if (probe.value != tempoValueAtY(yDot))
            fail("pencil-mode Shift stroke did not draw at the locked press value");
        for (const DocLanePoint &pt : doc.lanePoints(laneTrack, DOC_CC_TEMPO))
            if (pt.tick >= t0 && pt.tick <= tEnd && pt.value != tempoValueAtY(yDot))
                fail("pencil-mode Shift stroke lost its horizontal lock");

        while (doc.undoStack()->index() > undoIndex && doc.undoStack()->canUndo())
            doc.undoStack()->undo();
        QCoreApplication::processEvents();
    }

    // Automation node selection: a lane-scoped time selection derives a
    // node selection — a point is selected iff its row's lane is one of the
    // selection's lanes and startTick <= tick < endTick (half-open).
    // Selected nodes paint a highlight ring (and already do so while the
    // right-drag band is still sweeping, since the band publishes live);
    // with two or more selected, nodes in unselected lanes dim toward
    // palette().mid(). Dragging a selected node moves every selected node
    // (cross-lane) by one shared dTick/dValue as a single undo entry — the
    // shared shift clamped so the earliest node can't go below tick 0, each
    // value clamped to its own row's display range — with a live preview
    // of every affected row and the selection band following the commit. A
    // press outside the selection stays a single-point move, and
    // Delete/Backspace on the lane-scoped selection removes exactly the
    // selected nodes as one entry.
    {
        const int undoIndex = doc.undoStack()->index();
        const int laneTrack = view.selectedTrack();
        const QByteArray sectionBytes = doc.smf().write();
        view.clearTimeSelection();
        // A CC lane with no song data, so every point in it is the probe's
        // own (mus_abandoned_ship's selected track carries PAN data). Every
        // candidate keeps the fixed 0..127 axis the mirrors below assume —
        // MOD's auto-fit axis is deliberately not on the list.
        uint8_t freeCc = 0;
        for (uint8_t cc : {uint8_t(0x14), uint8_t(0x15), uint8_t(0x07), uint8_t(0x0A)}) {
            if (doc.lanePoints(laneTrack, cc).empty()) {
                freeCc = cc;
                break;
            }
        }
        if (!freeCc)
            fail("node-selection setup: no data-free CC lane available");
        if (freeCc) {
            view.addEmptyLane(laneTrack, freeCc);
            QCoreApplication::processEvents();
            // Row geometry mirrors, like the pencil/detent sections: tempo
            // row 0, voice row, then the track's lanes by ascending CC, all
            // at the default 48 px height.
            int ccRowTop = 2 * 48;
            for (const AutoLane &lane : view.model().lanes)
                if (lane.track == laneTrack && lane.cc < freeCc)
                    ccRowTop += 48;
            const int ccTop = ccRowTop + 5, ccBottom = ccRowTop + 48 - 1 - 4;
            auto ccValueAtY = [&](int y) {
                const int yc = std::clamp(y, ccTop, ccBottom);
                return (ccBottom - yc) * 127 / (ccBottom - ccTop);
            };
            auto ccValueY = [&](int v) { return ccBottom - v * (ccBottom - ccTop) / 127; };
            const int tempoTopPad = 5, tempoBottom = 48 - 1 - 4;
            auto tempoMaxV = [&]() {
                int maxV = 200;
                for (const LanePoint &pt : view.model().tempoLane)
                    maxV = std::max(maxV, pt.value + 20);
                return maxV;
            };
            auto tempoValueY = [&](int v) {
                return tempoBottom - v * (tempoBottom - tempoTopPad) / std::max(1, tempoMaxV());
            };

            const qreal dprLanes = lanes->devicePixelRatioF();
            auto dotX = [&](uint64_t t) {
                return view.displayX(double(t), songview::kGutterW, dprLanes);
            };
            auto tempoPointIn = [&](uint64_t a, uint64_t b) {
                for (const DocLanePoint &pt : doc.lanePoints(laneTrack, DOC_CC_TEMPO))
                    if (pt.tick >= a && pt.tick <= b)
                        return true;
                return false;
            };
            auto contestedX = [&](qreal x) { return overlayContestedX(view, dprLanes, x); };
            // Clear air: no song tempo point (and no overlay vertical) may
            // sit anywhere on the probe span.
            qreal xSeek = songview::kGutterW + (lanes->width() - songview::kGutterW) * 0.35;
            uint64_t t0 = view.snapTick(view.tickAtContentX(xSeek - songview::kGutterW));
            uint64_t g = std::max<uint64_t>(1, view.gridTicksAt(t0));
            auto spanContested = [&]() {
                if (tempoPointIn(t0 - std::min(t0, g), t0 + 5 * g))
                    return true;
                for (int k = 0; k <= 4; k++)
                    if (contestedX(dotX(t0 + uint64_t(k) * g)))
                        return true;
                return false;
            };
            while (spanContested()) {
                xSeek += 40;
                t0 = view.snapTick(view.tickAtContentX(xSeek - songview::kGutterW));
                g = std::max<uint64_t>(1, view.gridTicksAt(t0));
            }
            const uint64_t snap = std::max<uint64_t>(1, view.snapTicksAt(t0));
            const uint64_t tA = t0, tB = t0 + 2 * g, tC = t0 + 4 * g, tE = t0 + g;
            constexpr int vA = 40, vB = 80, vC = 55, vE = 150;
            doc.writeLanePoints(laneTrack, freeCc, tA, tC, {{tA, vA}, {tB, vB}, {tC, vC}});
            doc.writeLanePoints(laneTrack, DOC_CC_TEMPO, tE, tE, {{tE, vE}});
            QCoreApplication::processEvents();

            auto lanesImage = [&]() { return lanes->grab().toImage(); };
            auto pixelAt = [&](const QImage &img, qreal x, int y) {
                return img.pixelColor(int((x + 0.5) * dprLanes), int((y + 0.5) * dprLanes));
            };
            const QColor ringColor = lanes->palette().highlight().color();
            const QColor dimColor = lanes->palette().mid().color();
            const QColor ccColor = SongView::trackColor(laneTrack);
            const QColor tempoColor = themes::color(themes::Role::song_view_automation_tempo_curve);
            // The ring is aliased, so its pixels are exact — either the raw
            // highlight, or, inside the band, the highlight under the
            // selection overlay's alpha-30 tint. Let QPainter compute that
            // blend rather than mirroring its rounding.
            const QColor ringTinted = [&] {
                QImage one(1, 1, QImage::Format_ARGB32_Premultiplied);
                one.fill(ringColor);
                QPainter tp(&one);
                QColor f = themes::color(themes::Role::song_view_selection_fill);
                f.setAlpha(30);
                tp.fillRect(0, 0, 1, 1, f);
                tp.end();
                return one.pixelColor(0, 0);
            }();
            auto ringNear = [&](const QImage &img, qreal x, int y) {
                const int cx = int((x + 0.5) * dprLanes), cy = int((y + 0.5) * dprLanes);
                const int r = int(std::ceil(7 * dprLanes));
                for (int dy = -r; dy <= r; dy++)
                    for (int dx = -r; dx <= r; dx++) {
                        const int px = cx + dx, py = cy + dy;
                        if (px < 0 || py < 0 || px >= img.width() || py >= img.height())
                            continue;
                        const QColor c = img.pixelColor(px, py);
                        if (c == ringColor || c == ringTinted)
                            return true;
                    }
                return false;
            };
            // A node's marker stands clear of its curve above the hold line,
            // so ink just up and to the right of the center is ink no curve
            // alone would put there — off the hold line's own rows, and off
            // the grid line the node's tick sits on. The marker is
            // antialiased, so the probe asks whether anything painted, not
            // for an exact color; the curve color is mirrored on the hold
            // lines instead.
            const QColor rowBackground =
                themes::color(themes::Role::song_view_piano_roll_background);
            auto nodePresent = [&](const QImage &img, qreal x, int y) {
                return pixelAt(img, x + 1, y - 2) != rowBackground;
            };
            // A dimmed node fills solid, so its center is exactly the dim
            // color; any node at full strength is a ring around background.
            auto nodeDimmed = [&](const QImage &img, qreal x, int y) {
                return pixelAt(img, x, y) == dimColor;
            };

            // Mirror + clear-air sanity: with no selection, every probe dot
            // must show its exact curve color and nothing highlight-like.
            {
                const QImage base = lanesImage();
                // The hold line each node sits on carries the lane's color,
                // so it is what the color mirror is checked against; the
                // node centers show the background through their rings.
                if (pixelAt(base, dotX(tA + g), ccValueY(vA)) != ccColor)
                    fail("node probe setup: CC dot color mirror drifted");
                if (pixelAt(base, dotX(tE + g), tempoValueY(vE)) != tempoColor)
                    fail("node probe setup: tempo dot color mirror drifted");
                if (!nodePresent(base, dotX(tA), ccValueY(vA)) ||
                    !nodePresent(base, dotX(tC), ccValueY(vC)) ||
                    !nodePresent(base, dotX(tE), tempoValueY(vE)))
                    fail("node probe setup: a node did not paint its marker");
                if (ringNear(base, dotX(tA), ccValueY(vA)) ||
                    ringNear(base, dotX(tE), tempoValueY(vE)))
                    fail("node probe setup: highlight-like pixels near an unselected node");
            }

            // Ring + dim: sweep the band over A and B on the CC row. Nodes
            // ring while the sweep is still live (the band publishes the
            // selection live), C stays full-color and unringed (in range's
            // lane but outside its ticks), and the tempo node — two nodes
            // selected, its lane not covered — dims to palette().mid().
            const int yCcRow = ccRowTop + 24;
            sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(dotX(tA - snap)), yCcRow),
                      Qt::RightButton, Qt::RightButton);
            sendMouse(lanes, QEvent::MouseMove, QPoint(int(dotX(tB + snap)), yCcRow), Qt::NoButton,
                      Qt::RightButton);
            {
                const QImage live = lanesImage();
                if (!view.timeSelection().active() ||
                    view.timeSelection().scope != SongView::TimeSelection::Lanes)
                    fail("right-drag band did not publish a live lane selection");
                if (!ringNear(live, dotX(tA), ccValueY(vA)) ||
                    !ringNear(live, dotX(tB), ccValueY(vB)))
                    fail("nodes under the live band did not paint the provisional ring");
            }
            sendMouse(lanes, QEvent::MouseButtonRelease, QPoint(int(dotX(tB + snap)), yCcRow),
                      Qt::RightButton, Qt::NoButton);
            QCoreApplication::processEvents();
            {
                const QImage img = lanesImage();
                if (!ringNear(img, dotX(tA), ccValueY(vA)) ||
                    !ringNear(img, dotX(tB), ccValueY(vB)))
                    fail("selected nodes did not paint the highlight ring");
                if (ringNear(img, dotX(tC), ccValueY(vC)))
                    fail("a node outside the selected tick range painted a ring");
                if (nodeDimmed(img, dotX(tC), ccValueY(vC)))
                    fail("an unselected node in a selected lane lost its full color");
                if (!nodeDimmed(img, dotX(tE), tempoValueY(vE)))
                    fail("multi-node selection did not dim the unselected lane's node");
            }

            // A single selected node dims nothing: rings alone are enough
            // emphasis, and the other lanes keep their strength.
            {
                SongView::TimeSelection one;
                one.startTick = tA;
                one.endTick = tA + 1;
                one.scope = SongView::TimeSelection::Lanes;
                one.lanes = {{laneTrack, freeCc}};
                view.setTimeSelection(one);
                QCoreApplication::processEvents();
                const QImage img = lanesImage();
                if (!ringNear(img, dotX(tA), ccValueY(vA)))
                    fail("a single selected node did not ring");
                if (nodeDimmed(img, dotX(tE), tempoValueY(vE)))
                    fail("a single-node selection dimmed another lane's node");
                if (ringNear(img, dotX(tB), ccValueY(vB)))
                    fail("a node outside a single-node selection painted a ring");
            }

            // Group drag: press A inside the selection, drag right and up —
            // A and B move by one shared dTick/dValue as ONE undo entry, C
            // stays, the selection band follows, and undo restores the
            // exact pre-drag bytes.
            SongView::TimeSelection groupSel;
            groupSel.startTick = tA;
            groupSel.endTick = tB + 1;
            groupSel.scope = SongView::TimeSelection::Lanes;
            groupSel.lanes = {{laneTrack, freeCc}};
            const QByteArray preDrag = doc.smf().write();
            {
                view.setTimeSelection(groupSel);
                QCoreApplication::processEvents();
                const int undoBefore = doc.undoStack()->index();
                const int yEnd = ccValueY(60);
                sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(dotX(tA)), ccValueY(vA)),
                          Qt::LeftButton, Qt::LeftButton);
                sendMouse(lanes, QEvent::MouseMove, QPoint(int(dotX(tA + 2 * snap)), yEnd),
                          Qt::NoButton, Qt::LeftButton);
                sendMouse(lanes, QEvent::MouseButtonRelease, QPoint(int(dotX(tA + 2 * snap)), yEnd),
                          Qt::LeftButton, Qt::NoButton);
                QCoreApplication::processEvents();
                const int dValue = ccValueAtY(yEnd) - vA;
                if (doc.undoStack()->index() != undoBefore + 1)
                    fail("group drag was not one undo entry");
                DocLanePoint pt;
                if (doc.findLanePoint(laneTrack, freeCc, tA, nullptr) ||
                    doc.findLanePoint(laneTrack, freeCc, tB, nullptr))
                    fail("group drag left the selected nodes at their old ticks");
                if (!doc.findLanePoint(laneTrack, freeCc, tA + 2 * snap, &pt) ||
                    pt.value != vA + dValue)
                    fail("group drag did not move the grabbed node by the drag delta");
                if (!doc.findLanePoint(laneTrack, freeCc, tB + 2 * snap, &pt) ||
                    pt.value != vB + dValue)
                    fail("group drag did not move the sibling node by the shared delta");
                if (!doc.findLanePoint(laneTrack, freeCc, tC, &pt) || pt.value != vC)
                    fail("group drag disturbed a node outside the selection");
                if (!view.timeSelection().active() ||
                    view.timeSelection().startTick != tA + 2 * snap ||
                    view.timeSelection().endTick != tB + 1 + 2 * snap)
                    fail("the selection band did not follow the group drag");
                doc.undoStack()->undo();
                QCoreApplication::processEvents();
                if (doc.smf().write() != preDrag)
                    fail("undo did not restore the exact pre-group-drag bytes");
            }

            // Cross-lane group drag, straight up: the tempo node moves with
            // the CC nodes and every value clamps per row — the tempo node
            // stops at its row's display cap (~200) even though the
            // document itself would accept up to 999, so the row clamp is
            // observable. No tick moves, and the selection stays put.
            // Mid-drag, the tempo row (not the grabbed row) shows the live
            // preview of its pending clamped curve.
            {
                SongView::TimeSelection cross = groupSel;
                cross.lanes = {{laneTrack, freeCc}, {-1, DOC_CC_TEMPO}};
                view.setTimeSelection(cross);
                QCoreApplication::processEvents();
                const int undoBefore = doc.undoStack()->index();
                // The commit clamps against the pre-move display range;
                // capture it now (the landed value itself would raise it).
                // On a song whose own tempos push the cap past the drag's
                // reach (vE + 87), the clamp never engages — expect the
                // uncapped landing instead of failing spuriously.
                const int tempoCap = std::min(tempoMaxV(), vE + (127 - vA));
                sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(dotX(tA)), ccValueY(vA)),
                          Qt::LeftButton, Qt::LeftButton);
                sendMouse(lanes, QEvent::MouseMove, QPoint(int(dotX(tA)), ccTop), Qt::NoButton,
                          Qt::LeftButton);
                {
                    const QImage midDrag = lanesImage();
                    const QColor previewColor =
                        themes::color(themes::Role::song_view_edit_preview_outline);
                    // A width-1 logical line lands on different device rows
                    // at fractional scale factors; scan the short column.
                    const int px = int((dotX(tE) + 5 + 0.5) * dprLanes);
                    const int py = int((tempoValueY(tempoCap) + 0.5) * dprLanes);
                    const int reach = int(std::ceil(dprLanes)) + 1;
                    bool found = false;
                    for (int dy = -reach; dy <= reach && !found; dy++)
                        found = py + dy >= 0 && py + dy < midDrag.height() &&
                                midDrag.pixelColor(px, py + dy) == previewColor;
                    if (!found)
                        fail("group drag did not preview the affected tempo row's curve");
                }
                sendMouse(lanes, QEvent::MouseButtonRelease, QPoint(int(dotX(tA)), ccTop),
                          Qt::LeftButton, Qt::NoButton);
                QCoreApplication::processEvents();
                DocLanePoint pt;
                if (doc.undoStack()->index() != undoBefore + 1)
                    fail("cross-lane group drag was not one undo entry");
                if (!doc.findLanePoint(laneTrack, freeCc, tA, &pt) || pt.value != 127)
                    fail("vertical group drag did not cap the grabbed node at 127");
                if (!doc.findLanePoint(laneTrack, freeCc, tB, &pt) || pt.value != 127)
                    fail("vertical group drag did not cap the sibling node at 127");
                if (!doc.findLanePoint(laneTrack, DOC_CC_TEMPO, tE, &pt) || pt.value != tempoCap)
                    fail("group drag did not clamp the tempo node to its row's display cap");
                if (!view.timeSelection().active() || view.timeSelection().startTick != tA ||
                    view.timeSelection().endTick != tB + 1)
                    fail("a value-only group drag moved the selection band");
                doc.undoStack()->undo();
                QCoreApplication::processEvents();
                if (doc.smf().write() != preDrag)
                    fail("undo did not restore the cross-lane group drag");
            }

            // Earliest-node clamp: with a node parked near tick 0 in the
            // selection, dragging the group far left stops where that node
            // reaches 0 — everyone still shares the one clamped delta.
            {
                const uint64_t tZ = 2 * snap;
                constexpr int vZ = 20;
                doc.writeLanePoints(laneTrack, freeCc, tZ, tZ, {{tZ, vZ}});
                QCoreApplication::processEvents();
                const QByteArray preClamp = doc.smf().write();
                SongView::TimeSelection wide = groupSel;
                wide.startTick = tZ;
                view.setTimeSelection(wide);
                QCoreApplication::processEvents();
                const int undoBefore = doc.undoStack()->index();
                const int yGrab = ccValueY(vA);
                sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(dotX(tA)), yGrab),
                          Qt::LeftButton, Qt::LeftButton);
                sendMouse(lanes, QEvent::MouseMove, QPoint(songview::kGutterW + 1, yGrab),
                          Qt::NoButton, Qt::LeftButton);
                sendMouse(lanes, QEvent::MouseButtonRelease, QPoint(songview::kGutterW + 1, yGrab),
                          Qt::LeftButton, Qt::NoButton);
                QCoreApplication::processEvents();
                const int dValue = ccValueAtY(yGrab) - vA; // pixel re-quantization
                DocLanePoint pt;
                if (doc.undoStack()->index() != undoBefore + 1)
                    fail("clamped group drag was not one undo entry");
                if (!doc.findLanePoint(laneTrack, freeCc, 0, &pt) || pt.value != vZ + dValue)
                    fail("group drag's earliest node did not stop exactly at tick 0");
                if (!doc.findLanePoint(laneTrack, freeCc, tA - tZ, &pt) || pt.value != vA + dValue)
                    fail("earliest-node clamp did not hold the grabbed node to the shared delta");
                if (!doc.findLanePoint(laneTrack, freeCc, tB - tZ, &pt) || pt.value != vB + dValue)
                    fail("earliest-node clamp did not hold the sibling to the shared delta");
                if (!view.timeSelection().active() || view.timeSelection().startTick != 0 ||
                    view.timeSelection().endTick != tB + 1 - tZ)
                    fail("the selection band did not follow the clamped group drag");
                doc.undoStack()->undo();
                QCoreApplication::processEvents();
                if (doc.smf().write() != preClamp)
                    fail("undo did not restore the clamped group drag");
                doc.undoStack()->undo(); // the parked tick-0 neighbor itself
                QCoreApplication::processEvents();
            }

            // A press outside the selection stays a single-point move: only
            // C moves, the selected nodes hold still, and the band stays.
            {
                view.setTimeSelection(groupSel);
                QCoreApplication::processEvents();
                const int undoBefore = doc.undoStack()->index();
                const int yFrom = ccValueY(vC);
                sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(dotX(tC)), yFrom),
                          Qt::LeftButton, Qt::LeftButton);
                sendMouse(lanes, QEvent::MouseMove, QPoint(int(dotX(tC)), yFrom + 8), Qt::NoButton,
                          Qt::LeftButton);
                sendMouse(lanes, QEvent::MouseButtonRelease, QPoint(int(dotX(tC)), yFrom + 8),
                          Qt::LeftButton, Qt::NoButton);
                QCoreApplication::processEvents();
                DocLanePoint pt;
                if (doc.undoStack()->index() != undoBefore + 1)
                    fail("single-point move outside the selection was not one undo entry");
                if (!doc.findLanePoint(laneTrack, freeCc, tC, &pt) ||
                    pt.value != ccValueAtY(yFrom + 8))
                    fail("press outside the selection did not move the pressed node");
                if (!doc.findLanePoint(laneTrack, freeCc, tA, &pt) || pt.value != vA)
                    fail("press outside the selection dragged the selected nodes along");
                if (!view.timeSelection().active() || view.timeSelection().startTick != tA)
                    fail("a single-point move outside the selection moved the band");
                doc.undoStack()->undo();
                QCoreApplication::processEvents();
            }

            // A lone node previews live while it is dragged, and the preview
            // is the whole row as it will stand once the mouse comes up — not
            // the committed curve with a stroke laid over it. The two probes
            // below are the ways that distinction shows: the position the
            // node leaves must close up behind it, and the pair it lands
            // between must give up their old connecting line.
            {
                view.clearTimeSelection();
                QCoreApplication::processEvents();
                const QColor previewColor =
                    themes::color(themes::Role::song_view_edit_preview_outline);
                // A width-2 logical line lands on different device rows at
                // fractional scale factors; scan the short column.
                const int reach = int(std::ceil(dprLanes)) + 1;
                auto inkAt = [&](const QImage &img, qreal x, int y, const QColor &want) {
                    const int px = int((x + 0.5) * dprLanes);
                    const int py = int((y + 0.5) * dprLanes);
                    for (int dy = -reach; dy <= reach; dy++)
                        if (px >= 0 && px < img.width() && py + dy >= 0 && py + dy < img.height() &&
                            img.pixelColor(px, py + dy) == want)
                            return true;
                    return false;
                };
                // The drag's own value chip paints at the node's right, so
                // every probe below stays left of where the node is held.
                // The gesture sees the integer mouse position, so the
                // landing mirror must round the probe's x down the same way.
                auto landingTick = [&](int x) {
                    return view.snapTick(view.tickAtContentX(qreal(x) - songview::kGutterW));
                };

                // Vacated position: drag B off to the right of C and A must
                // hold straight across to C — the lane's own color, all the
                // way over the two segments B used to own.
                {
                    const int xTo = int(dotX(tC + 2 * snap));
                    const uint64_t landed = landingTick(xTo);
                    // Pressing at the node's own row still re-derives the
                    // value from the pixel, so the held value is the row's,
                    // not the node's stored one.
                    const int held = ccValueAtY(ccValueY(vB));
                    const qreal probeX = (dotX(tB) + dotX(tC)) / 2;
                    if (landed <= tC || contestedX(probeX))
                        fail("node drag preview setup: no clear column past the dragged node");
                    sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(dotX(tB)), ccValueY(vB)),
                              Qt::LeftButton, Qt::LeftButton);
                    sendMouse(lanes, QEvent::MouseMove, QPoint(xTo, ccValueY(vB)), Qt::NoButton,
                              Qt::LeftButton);
                    const QImage midDrag = lanesImage();
                    if (!inkAt(midDrag, probeX, ccValueY(vA), ccColor))
                        fail("a dragged node left a gap where it was picked up");
                    if (inkAt(midDrag, probeX, ccValueY(held), ccColor))
                        fail("a dragged node's committed hold stayed painted behind it");
                    sendMouse(lanes, QEvent::MouseButtonRelease, QPoint(xTo, ccValueY(vB)),
                              Qt::LeftButton, Qt::NoButton);
                    QCoreApplication::processEvents();
                    DocLanePoint pt;
                    if (doc.findLanePoint(laneTrack, freeCc, tB, nullptr) ||
                        !doc.findLanePoint(laneTrack, freeCc, landed, &pt) || pt.value != held)
                        fail("the previewed node did not land where the drag left it");
                    doc.undoStack()->undo();
                    QCoreApplication::processEvents();
                }

                // Landing between two nodes: drop C between A and B and the
                // stretch from it to B must show its pending hold — in the
                // edit-preview color — and nothing of A's old hold across to
                // B, which the release replaces.
                {
                    const int xTo = int((dotX(tA) + dotX(tB)) / 2);
                    const uint64_t landed = landingTick(xTo);
                    const int held = ccValueAtY(ccValueY(vC));
                    const qreal probeX = (qreal(xTo) + dotX(tB)) / 2;
                    if (landed <= tA || landed >= tB || contestedX(probeX))
                        fail("node drag preview setup: no clear column between A and B");
                    sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(dotX(tC)), ccValueY(vC)),
                              Qt::LeftButton, Qt::LeftButton);
                    sendMouse(lanes, QEvent::MouseMove, QPoint(xTo, ccValueY(vC)), Qt::NoButton,
                              Qt::LeftButton);
                    const QImage midDrag = lanesImage();
                    if (!inkAt(midDrag, probeX, ccValueY(held), previewColor))
                        fail("a dragged node did not preview its pending hold");
                    if (inkAt(midDrag, probeX, ccValueY(vA), ccColor))
                        fail("a node dragged between two others left their old line painted");
                    sendMouse(lanes, QEvent::MouseButtonRelease, QPoint(xTo, ccValueY(vC)),
                              Qt::LeftButton, Qt::NoButton);
                    QCoreApplication::processEvents();
                    DocLanePoint pt;
                    if (doc.findLanePoint(laneTrack, freeCc, tC, nullptr) ||
                        !doc.findLanePoint(laneTrack, freeCc, landed, &pt) || pt.value != held)
                        fail("the node dropped between two others did not land there");
                    doc.undoStack()->undo();
                    QCoreApplication::processEvents();
                }
            }

            // Half-open boundary: with the selection ending exactly at B's
            // tick, B is not selected — pressing it moves it alone.
            {
                SongView::TimeSelection edge = groupSel;
                edge.endTick = tB;
                view.setTimeSelection(edge);
                QCoreApplication::processEvents();
                {
                    // The paint side honors the same half-open rule: A
                    // rings, B — exactly at endTick — does not.
                    const QImage img = lanesImage();
                    if (!ringNear(img, dotX(tA), ccValueY(vA)))
                        fail("boundary selection did not ring its covered node");
                    if (ringNear(img, dotX(tB), ccValueY(vB)))
                        fail("a node exactly at endTick painted a ring (half-open broken)");
                }
                const int undoBefore = doc.undoStack()->index();
                const int yFrom = ccValueY(vB);
                sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(dotX(tB)), yFrom),
                          Qt::LeftButton, Qt::LeftButton);
                sendMouse(lanes, QEvent::MouseMove, QPoint(int(dotX(tB)), yFrom + 8), Qt::NoButton,
                          Qt::LeftButton);
                sendMouse(lanes, QEvent::MouseButtonRelease, QPoint(int(dotX(tB)), yFrom + 8),
                          Qt::LeftButton, Qt::NoButton);
                QCoreApplication::processEvents();
                DocLanePoint pt;
                if (doc.undoStack()->index() != undoBefore + 1)
                    fail("boundary-node move was not one undo entry");
                if (!doc.findLanePoint(laneTrack, freeCc, tB, &pt) ||
                    pt.value != ccValueAtY(yFrom + 8))
                    fail("a node exactly at endTick did not move as a single point");
                if (!doc.findLanePoint(laneTrack, freeCc, tA, &pt) || pt.value != vA)
                    fail("a node exactly at endTick counted as selected (half-open broken)");
                doc.undoStack()->undo();
                QCoreApplication::processEvents();
            }

            // A zero-motion click on a selected node deletes exactly that
            // node — one undo entry — and leaves the rest of the selection
            // alone, including events stored in non-canonical bytes that a
            // group move's byte-based no-op detection would "heal" into a
            // rewrite. Park a fractional-BPM tempo meta (120.5 BPM; the
            // canonical form of its 121 lane value encodes differently)
            // inside the selection and click.
            {
                SmfEvent frac;
                frac.tick = tA;
                frac.status = 0xFF;
                frac.metaType = 0x51;
                frac.blob.resize(3); // 497925 us/beat = 120.5 BPM
                frac.blob[0] = char(0x07);
                frac.blob[1] = char(0x99);
                frac.blob[2] = char(0x05);
                doc.insertRawEvent(0, frac);
                QCoreApplication::processEvents();
                SongView::TimeSelection cross = groupSel;
                cross.lanes = {{laneTrack, freeCc}, {-1, DOC_CC_TEMPO}};
                view.setTimeSelection(cross);
                QCoreApplication::processEvents();
                const QByteArray preClick = doc.smf().write();
                const int undoBefore = doc.undoStack()->index();
                const size_t selectedBefore = doc.lanePoints(laneTrack, freeCc).size();
                auto fractionalTempoIntact = [&] {
                    for (const SmfEvent &ev : doc.smf().tracks[0].events)
                        if (ev.tick == tA && ev.status == 0xFF && ev.metaType == 0x51 &&
                            ev.blob == frac.blob)
                            return true;
                    return false;
                };
                sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(dotX(tA)), ccValueY(vA)),
                          Qt::LeftButton, Qt::LeftButton);
                sendMouse(lanes, QEvent::MouseButtonRelease, QPoint(int(dotX(tA)), ccValueY(vA)),
                          Qt::LeftButton, Qt::NoButton);
                QCoreApplication::processEvents();
                DocLanePoint gone;
                if (doc.undoStack()->index() != undoBefore + 1 ||
                    doc.lanePoints(laneTrack, freeCc).size() + 1 != selectedBefore ||
                    doc.findLanePoint(laneTrack, freeCc, tA, &gone))
                    fail("a zero-motion click on a selected node did not delete just that node");
                if (!fractionalTempoIntact())
                    fail("the click-delete rewrote another selected lane's non-canonical event");
                doc.undoStack()->undo(); // the click-delete
                QCoreApplication::processEvents();
                if (doc.smf().write() != preClick)
                    fail("undoing the click-delete did not restore the document");
                doc.undoStack()->undo(); // the raw fractional-tempo insert
                QCoreApplication::processEvents();
            }

            // Escape mid-drag cancels a group drag with its selection: the
            // release after it must commit nothing.
            {
                view.setTimeSelection(groupSel);
                QCoreApplication::processEvents();
                const QByteArray preCancel = doc.smf().write();
                const int undoBefore = doc.undoStack()->index();
                sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(dotX(tA)), ccValueY(vA)),
                          Qt::LeftButton, Qt::LeftButton);
                sendMouse(lanes, QEvent::MouseMove, QPoint(int(dotX(tA + 2 * snap)), ccValueY(vA)),
                          Qt::NoButton, Qt::LeftButton);
                sendKey(lanes, Qt::Key_Escape, Qt::NoModifier);
                sendMouse(lanes, QEvent::MouseButtonRelease,
                          QPoint(int(dotX(tA + 2 * snap)), ccValueY(vA)), Qt::LeftButton,
                          Qt::NoButton);
                QCoreApplication::processEvents();
                if (view.timeSelection().active())
                    fail("Escape mid-group-drag did not clear the selection");
                if (doc.undoStack()->index() != undoBefore || doc.smf().write() != preCancel)
                    fail("release after Escape still committed the cancelled group drag");
            }

            // A band re-swept mid-drag is not the gesture's to shift: the
            // latched group still commits, but the new band stays put.
            {
                view.setTimeSelection(groupSel);
                QCoreApplication::processEvents();
                const QByteArray preSwap = doc.smf().write();
                const int undoBefore = doc.undoStack()->index();
                sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(dotX(tA)), ccValueY(vA)),
                          Qt::LeftButton, Qt::LeftButton);
                sendMouse(lanes, QEvent::MouseMove, QPoint(int(dotX(tA + 2 * snap)), ccValueY(vA)),
                          Qt::NoButton, Qt::LeftButton);
                SongView::TimeSelection other = groupSel;
                other.startTick = tC;
                other.endTick = tC + snap;
                view.setTimeSelection(other); // as a mid-drag right sweep would
                QCoreApplication::processEvents();
                sendMouse(lanes, QEvent::MouseButtonRelease,
                          QPoint(int(dotX(tA + 2 * snap)), ccValueY(vA)), Qt::LeftButton,
                          Qt::NoButton);
                QCoreApplication::processEvents();
                if (doc.undoStack()->index() != undoBefore + 1 ||
                    !doc.findLanePoint(laneTrack, freeCc, tA + 2 * snap, nullptr) ||
                    !doc.findLanePoint(laneTrack, freeCc, tB + 2 * snap, nullptr))
                    fail("mid-drag band replacement kept the latched group from committing");
                if (!view.timeSelection().active() || view.timeSelection().startTick != tC ||
                    view.timeSelection().endTick != tC + snap)
                    fail("the group commit shifted a band re-swept mid-drag");
                doc.undoStack()->undo();
                QCoreApplication::processEvents();
                if (doc.smf().write() != preSwap)
                    fail("undo did not restore the mid-drag band-replacement move");
            }

            // Delete/Backspace on the lane-scoped selection removes exactly
            // the selected nodes — cross-lane, one undo entry, out-of-range
            // survivors untouched (main's roll.delete range path already IS
            // the node delete when the scope is Lanes; see SPEC).
            for (const int key : {int(Qt::Key_Delete), int(Qt::Key_Backspace)}) {
                SongView::TimeSelection cross = groupSel;
                cross.lanes = {{laneTrack, freeCc}, {-1, DOC_CC_TEMPO}};
                view.setTimeSelection(cross);
                QCoreApplication::processEvents();
                const QByteArray preDelete = doc.smf().write();
                const int undoBefore = doc.undoStack()->index();
                sendKey(lanes, key, Qt::NoModifier);
                QCoreApplication::processEvents();
                DocLanePoint pt;
                if (doc.undoStack()->index() != undoBefore + 1)
                    fail(key == Qt::Key_Delete ? "Delete on selected nodes was not one undo entry"
                                               : "Backspace on selected nodes was not one entry");
                if (doc.findLanePoint(laneTrack, freeCc, tA, nullptr) ||
                    doc.findLanePoint(laneTrack, freeCc, tB, nullptr) ||
                    doc.findLanePoint(laneTrack, DOC_CC_TEMPO, tE, nullptr))
                    fail("Delete did not remove the selected nodes across lanes");
                if (!doc.findLanePoint(laneTrack, freeCc, tC, &pt) || pt.value != vC)
                    fail("Delete removed a node outside the selected tick range");
                doc.undoStack()->undo();
                QCoreApplication::processEvents();
                if (doc.smf().write() != preDelete)
                    fail("undo did not restore the deleted nodes");
            }

            // Delete honors the half-open boundary too: ending exactly at
            // B's tick leaves B alive.
            {
                SongView::TimeSelection edge = groupSel;
                edge.endTick = tB;
                view.setTimeSelection(edge);
                QCoreApplication::processEvents();
                const int undoBefore = doc.undoStack()->index();
                sendKey(lanes, Qt::Key_Delete, Qt::NoModifier);
                QCoreApplication::processEvents();
                DocLanePoint pt;
                if (doc.undoStack()->index() != undoBefore + 1)
                    fail("boundary Delete was not one undo entry");
                if (doc.findLanePoint(laneTrack, freeCc, tA, nullptr))
                    fail("boundary Delete did not remove the selected node");
                if (!doc.findLanePoint(laneTrack, freeCc, tB, &pt) || pt.value != vB)
                    fail("Delete removed the node exactly at endTick (half-open broken)");
                doc.undoStack()->undo();
                QCoreApplication::processEvents();
            }

            view.clearTimeSelection();
            while (doc.undoStack()->index() > undoIndex && doc.undoStack()->canUndo())
                doc.undoStack()->undo();
            QCoreApplication::processEvents();
            view.removeEmptyLane(laneTrack, freeCc);
            QCoreApplication::processEvents();
            if (doc.smf().write() != sectionBytes)
                fail("node-selection section did not restore the document bytes");
        }
    }

    // Automation point context menu: a right release in place on a point
    // opens a two-action popup (Set value…, Delete) instead of the old
    // instant delete, aimed by a circular nearest-wins hit test against the
    // document — so of same-tick duplicates, the cursor's y picks the point.
    // The aimed node paints the highlight ring while the popup is open; a
    // right-click on another point re-aims the open menu in one gesture
    // (never dismiss-then-reopen), a right-click on nothing falls through
    // and dismisses it. Empty lane space keeps its selection-clear meaning,
    // and a voice-row right release still deletes the marker directly.
    {
        const int undoIndex = doc.undoStack()->index();
        const int laneTrack = view.selectedTrack();
        const QByteArray sectionBytes = doc.smf().write();
        view.clearTimeSelection();
        auto *pointMenu = lanes->findChild<QMenu *>(QStringLiteral("automationPointMenu"));
        if (!pointMenu)
            fail("automation point menu not found");
        // A CC lane with no song data, exactly like the node-selection
        // setup: fixed 0..127 axis, every point in it the probe's own.
        uint8_t freeCc = 0;
        for (uint8_t cc : {uint8_t(0x14), uint8_t(0x15), uint8_t(0x07), uint8_t(0x0A)}) {
            if (doc.lanePoints(laneTrack, cc).empty()) {
                freeCc = cc;
                break;
            }
        }
        if (!freeCc)
            fail("point-menu setup: no data-free CC lane available");
        if (freeCc && pointMenu) {
            view.addEmptyLane(laneTrack, freeCc);
            QCoreApplication::processEvents();
            // Row geometry mirror: tempo row 0, voice row, then the track's
            // lanes by ascending CC, all at the default 48 px height.
            int ccRowTop = 2 * 48;
            for (const AutoLane &lane : view.model().lanes)
                if (lane.track == laneTrack && lane.cc < freeCc)
                    ccRowTop += 48;
            const int ccTop = ccRowTop + 5, ccBottom = ccRowTop + 48 - 1 - 4;
            auto ccValueY = [&](int v) { return ccBottom - v * (ccBottom - ccTop) / 127; };
            const qreal dprLanes = lanes->devicePixelRatioF();
            auto dotX = [&](uint64_t t) {
                return view.displayX(double(t), songview::kGutterW, dprLanes);
            };
            auto contestedX = [&](qreal x) { return overlayContestedX(view, dprLanes, x); };
            qreal xSeek = songview::kGutterW + (lanes->width() - songview::kGutterW) * 0.3;
            uint64_t t0 = view.snapTick(view.tickAtContentX(xSeek - songview::kGutterW));
            uint64_t g = std::max<uint64_t>(1, view.gridTicksAt(t0));
            auto spanContested = [&]() {
                for (int k = 0; k <= 5; k++)
                    if (contestedX(dotX(t0 + uint64_t(k) * g)))
                        return true;
                return false;
            };
            while (spanContested()) {
                xSeek += 40;
                t0 = view.snapTick(view.tickAtContentX(xSeek - songview::kGutterW));
                g = std::max<uint64_t>(1, view.gridTicksAt(t0));
            }
            const uint64_t tA = t0, tB = t0 + 2 * g, tD = t0 + 4 * g;
            constexpr int vA = 40, vB = 80, vHigh = 100, vLow = 30;
            doc.writeLanePoints(laneTrack, freeCc, tA, tB, {{tA, vA}, {tB, vB}});
            doc.writeLanePoints(laneTrack, freeCc, tD, tD, {{tD, vHigh}});
            {
                // The same-tick duplicate porydaw's own editing never
                // produces but imported files carry: a raw CC event on the
                // duplicate tick with a different value.
                SmfEvent dup;
                dup.tick = tD;
                dup.status = char(0xB0 | doc.channelFor(laneTrack));
                dup.data0 = char(freeCc);
                dup.data1 = char(vLow);
                doc.insertRawEvent(doc.smfTrackFor(laneTrack), dup);
            }
            QCoreApplication::processEvents();
            auto duplicatesAt = [&](uint64_t tick) {
                std::vector<DocLanePoint> out;
                for (const DocLanePoint &pt : doc.lanePoints(laneTrack, freeCc))
                    if (pt.tick == tick)
                        out.push_back(pt);
                return out;
            };
            if (duplicatesAt(tD).size() != 2 ||
                duplicatesAt(tD).front().value == duplicatesAt(tD).back().value)
                fail("point-menu setup: the same-tick duplicate did not land");

            auto rightClick = [&](QPoint pos) {
                sendMouse(lanes, QEvent::MouseButtonPress, pos, Qt::RightButton, Qt::RightButton);
                sendMouse(lanes, QEvent::MouseButtonRelease, pos, Qt::RightButton, Qt::NoButton);
                QCoreApplication::processEvents();
            };
            auto menuAction = [&](const char *text) -> QAction * {
                for (QAction *action : pointMenu->actions())
                    if (action->text() == QString::fromUtf8(text))
                        return action;
                return nullptr;
            };
            // Keyboard activation: synthetic clicks hit QMenu's
            // accidental-release guards; Return on the active action does
            // what a real pick does.
            auto activate = [&](QAction *action) {
                if (!action)
                    return;
                pointMenu->setActiveAction(action);
                sendKey(pointMenu, Qt::Key_Return, Qt::NoModifier);
                QCoreApplication::processEvents();
            };
            auto dismissMenu = [&]() {
                if (pointMenu->isVisible()) {
                    sendKey(pointMenu, Qt::Key_Escape, Qt::NoModifier);
                    QCoreApplication::processEvents();
                }
            };
            const QColor ringColor = lanes->palette().highlight().color();
            // The ring is aliased so its pixels are exact (no selection
            // band here, so no tinted variant to blend).
            auto ringNear = [&](const QImage &img, qreal x, int y) {
                const int cx = int((x + 0.5) * dprLanes), cy = int((y + 0.5) * dprLanes);
                const int r = int(std::ceil(7 * dprLanes));
                for (int dy = -r; dy <= r; dy++)
                    for (int dx = -r; dx <= r; dx++) {
                        const int px = cx + dx, py = cy + dy;
                        if (px < 0 || py < 0 || px >= img.width() || py >= img.height())
                            continue;
                        if (img.pixelColor(px, py) == ringColor)
                            return true;
                    }
                return false;
            };
            const QPoint pointA(int(dotX(tA)), ccValueY(vA));
            const QPoint pointB(int(dotX(tB)), ccValueY(vB));

            // Mirror + clear-air sanity: the dots show their exact curve
            // color and nothing highlight-like before any menu opens.
            {
                const QImage base = lanes->grab().toImage();
                const QColor ccColor = SongView::trackColor(laneTrack);
                // The lane's color lives on the hold line the node sits on;
                // the node itself is a hollow ring showing the row
                // background through its center.
                const int cx = int((dotX(tA + g) + 0.5) * dprLanes);
                if (base.pixelColor(cx, int((ccValueY(vA) + 0.5) * dprLanes)) != ccColor)
                    fail("point-menu setup: CC dot color mirror drifted");
                if (base.pixelColor(int((dotX(tA) + 1.5) * dprLanes),
                                    int((ccValueY(vA) + 0.5) * dprLanes) - 2) ==
                    themes::color(themes::Role::song_view_piano_roll_background))
                    fail("point-menu setup: the CC node did not paint its marker");
                if (ringNear(base, dotX(tA), ccValueY(vA)))
                    fail("point-menu setup: highlight-like pixels near an idle node");
            }

            // Empty lane space keeps its old meaning: no menu, no edit.
            // (Same x as point A, but far above its dot: an x-only test
            // would treat this as a point click.)
            {
                const uint64_t preRevision = doc.revision();
                rightClick(QPoint(int(dotX(tA)), ccTop + 1));
                if (pointMenu->isVisible()) {
                    fail("empty-space right-click opened the point menu");
                    dismissMenu();
                }
                if (doc.revision() != preRevision)
                    fail("empty-space right-click edited the document");
            }

            // Right release in place on a point: the two-action menu opens,
            // the aimed node rings, and nothing is edited by the click —
            // the old instant delete is gone.
            {
                const uint64_t preRevision = doc.revision();
                const int preUndo = doc.undoStack()->index();
                rightClick(pointA);
                if (!pointMenu->isVisible())
                    fail("right-click on a point did not open the point menu");
                QStringList labels;
                for (QAction *action : pointMenu->actions())
                    labels.push_back(action->text());
                if (labels !=
                    QStringList{QString::fromUtf8("Set value…"), QStringLiteral("Delete")})
                    fail("point menu actions are not exactly Set value…, Delete");
                if (!doc.findLanePoint(laneTrack, freeCc, tA, nullptr) ||
                    doc.revision() != preRevision || doc.undoStack()->index() != preUndo)
                    fail("opening the point menu edited the document");
                if (!ringNear(lanes->grab().toImage(), dotX(tA), ccValueY(vA)))
                    fail("the aimed point did not ring while the menu is open");

                // Delete removes exactly the aimed point as one undo entry.
                activate(menuAction("Delete"));
                DocLanePoint pt;
                if (pointMenu->isVisible() || doc.findLanePoint(laneTrack, freeCc, tA, nullptr) ||
                    !doc.findLanePoint(laneTrack, freeCc, tB, &pt) || pt.value != vB ||
                    duplicatesAt(tD).size() != 2 || doc.undoStack()->index() != preUndo + 1 ||
                    doc.revision() != preRevision + 1)
                    fail("point menu Delete did not remove exactly the aimed point");
                doc.undoStack()->undo();
                QCoreApplication::processEvents();
            }

            // Same-tick duplicates resolve by the cursor's y: aiming at the
            // first duplicate's dot deletes that one, not the later-in-file
            // one an x-only tie rule would pick.
            {
                const std::vector<DocLanePoint> dups = duplicatesAt(tD);
                const int preUndo = doc.undoStack()->index();
                rightClick(QPoint(int(dotX(tD)), ccValueY(dups.front().value)));
                if (!pointMenu->isVisible())
                    fail("right-click on a same-tick duplicate did not open the menu");
                activate(menuAction("Delete"));
                const std::vector<DocLanePoint> after = duplicatesAt(tD);
                if (after.size() != 1 || after.front().value != dups.back().value ||
                    doc.undoStack()->index() != preUndo + 1)
                    fail("Delete did not remove the same-tick duplicate under the cursor's y");
                doc.undoStack()->undo();
                QCoreApplication::processEvents();
            }

            // Set value commits the typed value on the aimed point, in
            // place, through the same type-in the double-click opens.
            {
                const int preUndo = doc.undoStack()->index();
                const uint64_t preRevision = doc.revision();
                rightClick(pointB);
                if (!pointMenu->isVisible())
                    fail("Set value probe: the menu did not open");
                const int typedShown = 10;
                // PAN stores c_v + 64; the free-CC pick can land on 0x0A.
                const int typedStored = freeCc == 0x0A ? typedShown + 64 : typedShown;
                QTimer poll;
                poll.setInterval(0);
                bool inputSeen = false;
                QObject::connect(&poll, &QTimer::timeout, [&] {
                    if (auto *dlg = lanes->findChild<QInputDialog *>()) {
                        inputSeen = true;
                        dlg->setIntValue(typedShown);
                        dlg->accept();
                    }
                });
                poll.start();
                activate(menuAction("Set value…"));
                poll.stop();
                if (!inputSeen)
                    fail("Set value did not open the numeric type-in");
                DocLanePoint edited;
                if (!doc.findLanePoint(laneTrack, freeCc, tB, &edited) ||
                    edited.value != typedStored || duplicatesAt(tD).size() != 2 ||
                    doc.undoStack()->index() != preUndo + 1 || doc.revision() != preRevision + 1)
                    fail("Set value did not commit the typed value in place");
                doc.undoStack()->undo();
                QCoreApplication::processEvents();
            }

            // Set value on a same-tick duplicate: the dialog is seeded with
            // the aimed point's value (the y pick), and committing heals
            // the tick like every value edit does — one point, the typed
            // value, one undo entry (moveLanePoints' shadow-healing rule).
            {
                const std::vector<DocLanePoint> dups = duplicatesAt(tD);
                const int preUndo = doc.undoStack()->index();
                rightClick(QPoint(int(dotX(tD)), ccValueY(dups.front().value)));
                if (!pointMenu->isVisible())
                    fail("duplicate Set value probe: the menu did not open");
                const int aimedShown =
                    freeCc == 0x0A ? dups.front().value - 64 : dups.front().value;
                const int typedShown = 20;
                const int typedStored = freeCc == 0x0A ? typedShown + 64 : typedShown;
                QTimer poll;
                poll.setInterval(0);
                bool seededWithAim = false;
                QObject::connect(&poll, &QTimer::timeout, [&] {
                    if (auto *dlg = lanes->findChild<QInputDialog *>()) {
                        seededWithAim = dlg->intValue() == aimedShown;
                        dlg->setIntValue(typedShown);
                        dlg->accept();
                    }
                });
                poll.start();
                activate(menuAction("Set value…"));
                poll.stop();
                if (!seededWithAim)
                    fail("duplicate Set value was not seeded with the aimed point's value");
                const std::vector<DocLanePoint> after = duplicatesAt(tD);
                if (after.size() != 1 || after.front().value != typedStored ||
                    doc.undoStack()->index() != preUndo + 1)
                    fail("Set value on a same-tick duplicate did not heal the tick to the typed "
                         "value");
                doc.undoStack()->undo();
                QCoreApplication::processEvents();
                if (duplicatesAt(tD).size() != 2)
                    fail("undo did not restore the healed same-tick duplicate");
            }

            // Retarget: with the menu open on B, a right-click on A re-aims
            // it — the popup never closes, the ring jumps to A, and Delete
            // then removes A only. (A sits left of the popup, which opens
            // at B and extends right, so the click is outside its rect.)
            {
                const int preUndo = doc.undoStack()->index();
                rightClick(pointB);
                if (!pointMenu->isVisible())
                    fail("retarget probe: the menu did not open");
                const QPoint aGlobal = lanes->mapToGlobal(pointA);
                sendMouse(pointMenu, QEvent::MouseButtonPress, pointMenu->mapFromGlobal(aGlobal),
                          Qt::RightButton, Qt::RightButton);
                sendMouse(pointMenu, QEvent::MouseButtonRelease, pointMenu->mapFromGlobal(aGlobal),
                          Qt::RightButton, Qt::NoButton);
                QCoreApplication::processEvents();
                if (!pointMenu->isVisible())
                    fail("retargeting dismissed the open point menu");
                const QImage img = lanes->grab().toImage();
                if (!ringNear(img, dotX(tA), ccValueY(vA)) || ringNear(img, dotX(tB), ccValueY(vB)))
                    fail("retargeting did not move the aimed node's ring");
                activate(menuAction("Delete"));
                if (doc.findLanePoint(laneTrack, freeCc, tA, nullptr) ||
                    !doc.findLanePoint(laneTrack, freeCc, tB, nullptr) ||
                    doc.undoStack()->index() != preUndo + 1)
                    fail("retargeted Delete did not remove the retargeted point only");
                doc.undoStack()->undo();
                QCoreApplication::processEvents();
            }

            // The lanes' right-click precedence holds while the popup is
            // open: a retarget onto a point inside a covering time
            // selection declines (that click belongs to the range menu),
            // so the popup dismisses instead of re-aiming.
            {
                SongView::TimeSelection cover;
                cover.startTick = tA + g; // B covered, A (the open aim) not
                cover.endTick = tD;
                cover.scope = SongView::TimeSelection::Lanes;
                cover.lanes = {{laneTrack, freeCc}};
                view.setTimeSelection(cover);
                QCoreApplication::processEvents();
                const uint64_t preRevision = doc.revision();
                rightClick(pointA);
                if (!pointMenu->isVisible())
                    fail("precedence probe: the menu did not open on the uncovered point");
                const QPoint bGlobal = lanes->mapToGlobal(pointB);
                sendMouse(pointMenu, QEvent::MouseButtonPress, pointMenu->mapFromGlobal(bGlobal),
                          Qt::RightButton, Qt::RightButton);
                sendMouse(pointMenu, QEvent::MouseButtonRelease, pointMenu->mapFromGlobal(bGlobal),
                          Qt::RightButton, Qt::NoButton);
                QCoreApplication::processEvents();
                if (pointMenu->isVisible()) {
                    fail("retarget onto a selection-covered point did not decline and dismiss");
                    dismissMenu();
                }
                if (doc.revision() != preRevision)
                    fail("the declined covered-point retarget edited the document");
                view.clearTimeSelection();
                QCoreApplication::processEvents();
            }

            // A right-click that hits no point falls through to QMenu and
            // dismisses the popup (nothing edited), and the ring lifts with
            // it. The empty spot sits up-left of the popup's corner.
            {
                rightClick(pointA);
                if (!pointMenu->isVisible())
                    fail("dismiss probe: the menu did not open");
                const uint64_t preRevision = doc.revision();
                const QPoint emptyGlobal =
                    lanes->mapToGlobal(QPoint(int(dotX(tA)) - 20, ccTop + 1));
                sendMouse(pointMenu, QEvent::MouseButtonPress,
                          pointMenu->mapFromGlobal(emptyGlobal), Qt::RightButton, Qt::RightButton);
                sendMouse(pointMenu, QEvent::MouseButtonRelease,
                          pointMenu->mapFromGlobal(emptyGlobal), Qt::RightButton, Qt::NoButton);
                QCoreApplication::processEvents();
                if (pointMenu->isVisible()) {
                    fail("outside right-click on nothing did not dismiss the point menu");
                    dismissMenu();
                }
                if (doc.revision() != preRevision)
                    fail("dismissing the point menu edited the document");
                if (ringNear(lanes->grab().toImage(), dotX(tA), ccValueY(vA)))
                    fail("the aim ring survived the menu's dismissal");
            }

            // Voice-row right release in place still deletes the marker
            // directly — the point menu never aims at voice identities.
            {
                uint64_t tV = tB;
                auto voiceContested = [&]() {
                    for (const DocLanePoint &pt : doc.lanePoints(laneTrack, DOC_CC_VOICE))
                        if (std::abs(dotX(pt.tick) - dotX(tV)) < 12)
                            return true;
                    return false;
                };
                while (voiceContested())
                    tV += g;
                doc.addLanePoint(laneTrack, DOC_CC_VOICE, tV, 3);
                QCoreApplication::processEvents();
                const int preUndo = doc.undoStack()->index();
                rightClick(QPoint(int(dotX(tV)), 48 + 24));
                if (pointMenu->isVisible()) {
                    fail("voice-row right-click opened the point menu");
                    dismissMenu();
                }
                if (doc.findLanePoint(laneTrack, DOC_CC_VOICE, tV, nullptr) ||
                    doc.undoStack()->index() != preUndo + 1)
                    fail("voice-marker right-click delete no longer works");
            }

            dismissMenu();
            view.clearTimeSelection();
            while (doc.undoStack()->index() > undoIndex && doc.undoStack()->canUndo())
                doc.undoStack()->undo();
            QCoreApplication::processEvents();
            view.removeEmptyLane(laneTrack, freeCc);
            QCoreApplication::processEvents();
            if (doc.smf().write() != sectionBytes)
                fail("point-menu section did not restore the document bytes");
        }
    }

    // Meter-aware lane stepping: a freehand sweep or a Shift line ramp
    // crossing a time-signature change lands every generated point on the
    // grid of the segment governing it — the drawn grid restarts (and can
    // change spacing) at the signature — instead of carrying the press-time
    // spacing and phase across the boundary. The ramp's final point stays
    // exactly at the release tick. Probed on the tempo row against 4/4->3/2
    // (spacing doubles at the boundary) and against a mid-cell 4/4->4/4
    // change (equal spacing, restarted phase).
    {
        const int undoIndex = doc.undoStack()->index();
        const int laneTrack = view.selectedTrack();
        const qreal dprLanes = lanes->devicePixelRatioF();
        const uint64_t tpb = timeline->ticksPerBeat;
        // Test-side mirror of the tempo row's value<->y mapping, as in the
        // pencil section above (row 0, default 48 px height).
        const int rowTopPad = 5, rowBottom = 48 - 1 - 4;
        auto tempoMaxV = [&]() {
            int maxV = 200;
            for (const LanePoint &pt : view.model().tempoLane)
                maxV = std::max(maxV, pt.value + 20);
            return maxV;
        };
        auto tempoValueAtY = [&](int y) {
            const int yc = std::clamp(y, rowTopPad, rowBottom);
            return (rowBottom - yc) * tempoMaxV() / (rowBottom - rowTopPad);
        };
        auto tempoValueY = [&](int v) {
            return rowBottom - v * (rowBottom - rowTopPad) / std::max(1, tempoMaxV());
        };
        auto tempoPoints = [&]() { return doc.lanePoints(laneTrack, DOC_CC_TEMPO); };
        auto laneX = [&](uint64_t tick) {
            return view.displayX(double(tick), songview::kGutterW, dprLanes);
        };
        // The press must land in clear air (the grab slop is 7 px in x and
        // y); dodge the song's own tempo dots by sliding the probe window.
        auto clearAir = [&](uint64_t tick, int y) {
            for (const LanePoint &pt : view.model().tempoLane)
                if (std::abs(laneX(pt.tick) - laneX(tick)) <= 12 &&
                    std::abs(tempoValueY(pt.value) - y) <= 12)
                    return false;
            return true;
        };
        const int ySweep = 40; // low tempo values, away from typical dots
        // Drag from tA to tB, passing exactly through the given grid ticks
        // so every mouse sample's snapped seed sits on the drawn grid (the
        // per-sample seed phase is part of the sweep contract under test).
        auto dragAcross = [&](uint64_t tA, uint64_t tB, std::vector<uint64_t> via, int yFrom,
                              int yTo, Qt::KeyboardModifiers mods) {
            sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(laneX(tA)), yFrom),
                      Qt::LeftButton, Qt::LeftButton, mods);
            // A freehand sweep only starts once it clears its activation
            // slop, so arm it straight up: the stroke's x — all these
            // probes assert on — is untouched, and the subtracted slop
            // just shifts every sampled value down by the same few pixels.
            // A Shift ramp has no slop (and commits from its anchors), so
            // it needs no arming.
            if (!(mods & Qt::ShiftModifier))
                sendMouse(lanes, QEvent::MouseMove,
                          QPoint(int(laneX(tA)), yFrom - layout::fontPx(5.0 / 12.0)), Qt::NoButton,
                          Qt::LeftButton, mods);
            via.push_back(tB);
            for (size_t i = 0; i < via.size(); i++) {
                const int y = yFrom + (yTo - yFrom) * int(i + 1) / int(via.size());
                sendMouse(lanes, QEvent::MouseMove, QPoint(int(laneX(via[i])), y), Qt::NoButton,
                          Qt::LeftButton, mods);
            }
            sendMouse(lanes, QEvent::MouseButtonRelease, QPoint(int(laneX(tB)), yTo),
                      Qt::LeftButton, Qt::NoButton, mods);
            QCoreApplication::processEvents();
        };
        // Every committed point in [tA, tB] on its side's grid, and the
        // count proves full cell coverage on both sides (a fixed-spacing
        // walk either misfires the phase or skips cells).
        auto checkSpan = [&](uint64_t sigTick, uint64_t tA, uint64_t tB, const char *what) {
            const uint64_t gLeft = std::max<uint64_t>(1, view.gridTicksAt(tA));
            const uint64_t gRight = std::max<uint64_t>(1, view.gridTicksAt(sigTick));
            size_t n = 0;
            bool aligned = true;
            for (const DocLanePoint &pt : tempoPoints()) {
                if (pt.tick < tA || pt.tick > tB)
                    continue;
                n++;
                aligned = aligned && (pt.tick < sigTick ? (pt.tick - tA) % gLeft == 0
                                                        : (pt.tick - sigTick) % gRight == 0);
            }
            const size_t expected =
                size_t((sigTick - tA + gLeft - 1) / gLeft + (tB - sigTick) / gRight + 1);
            if (!aligned || n != expected)
                fail(what);
        };

        for (int scenario = 0; scenario < 2; scenario++) {
            // Scenario 0: 3/2 at a bar line — the beat (and the quarter-note
            // grid floor set above) doubles the spacing at the boundary.
            // Scenario 1: 4/4 half a beat past a bar line — same spacing on
            // both sides, but the grid re-anchors at the signature's tick.
            uint64_t sigTick = 0;
            for (uint64_t bar = 16; bar <= 28 && !sigTick; bar += 4) {
                const uint64_t candidate = bar * tpb + (scenario == 1 ? tpb / 2 : 0);
                // Vet the press tick itself: the sweep starts two beats
                // before the bar line the signature hangs off of.
                if (clearAir((bar - 2) * tpb, ySweep))
                    sigTick = candidate;
            }
            if (!sigTick) {
                fail("no clear air found for the meter-stepping probes");
                break;
            }
            if (scenario == 0)
                doc.setTimeSig(sigTick, 3, 1);
            else
                doc.setTimeSig(sigTick, 4, 2);
            QCoreApplication::processEvents();
            const uint64_t gLeft = std::max<uint64_t>(1, view.gridTicksAt(0));
            const uint64_t gRight = std::max<uint64_t>(1, view.gridTicksAt(sigTick));
            if (gLeft != tpb || gRight != (scenario == 0 ? 2 * tpb : tpb)) {
                fail("meter-stepping probe setup: unexpected grid spacing");
                doc.undoStack()->undo();
                QCoreApplication::processEvents();
                continue;
            }
            const uint64_t tA = sigTick - (scenario == 1 ? tpb / 2 : 0) - 2 * gLeft;
            const uint64_t tB = sigTick + 2 * gRight;

            // Freehand sweep across the boundary in one fast move, so the
            // interpolated cell walk itself crosses the signature (samples
            // parked on the boundary would keep every per-sample walk
            // inside a single segment and mask a fixed-spacing regression).
            dragAcross(tA, tB, {}, ySweep, ySweep, Qt::NoModifier);
            checkSpan(sigTick, tA, tB,
                      scenario == 0 ? "sweep across a meter change left points off the new grid"
                                    : "sweep across a re-anchored grid ignored the new phase");
            doc.undoStack()->undo();
            QCoreApplication::processEvents();

            // Shift line ramp across the same boundary: stepped on each
            // side's grid, endpoint exactly at the release tick and value.
            const int yFrom = 40, yTo = 25;
            dragAcross(tA, tB, {tA + gLeft, sigTick, sigTick + gRight}, yFrom, yTo,
                       Qt::ShiftModifier);
            checkSpan(sigTick, tA, tB, "line ramp across a meter change stepped off-grid");
            const std::vector<DocLanePoint> committed = tempoPoints();
            const DocLanePoint *last = nullptr;
            for (const DocLanePoint &pt : committed)
                if (pt.tick >= tA && pt.tick <= tB)
                    last = &pt;
            if (!last || last->tick != tB)
                fail("line ramp endpoint is not exactly at the release tick");
            else if (last->value != tempoValueAtY(yTo))
                fail("line ramp endpoint value drifted from the release position");
            doc.undoStack()->undo();
            QCoreApplication::processEvents();

            doc.undoStack()->undo(); // the signature change itself
            QCoreApplication::processEvents();
        }
        while (doc.undoStack()->index() > undoIndex && doc.undoStack()->canUndo())
            doc.undoStack()->undo();
        QCoreApplication::processEvents();
    }

    // Ctrl neutral detent: the magnet window is a font-scaled pixel radius
    // (layout::fontPx(2/3), the old hard-coded 8 px at the reference font)
    // converted to value units through the row's height — span * radius /
    // rowHeight — so a resized lane keeps the same on-screen magnet.
    // Probed on a PAN lane (neutral 64) at the default and a doubled row
    // height, from both sides of the neutral, just inside and just outside
    // the window.
    {
        const int undoIndex = doc.undoStack()->index();
        const int laneTrack = view.selectedTrack();
        view.addEmptyLane(laneTrack, 0x0A); // PAN; a no-op if the song has one
        QCoreApplication::processEvents();
        // Row order mirror: tempo, voice (always present with a document),
        // then the selected track's lanes by ascending CC; rows above the
        // PAN row keep the default 48 px height throughout.
        int panRowTop = 2 * 48;
        for (const AutoLane &lane : view.model().lanes)
            if (lane.track == laneTrack && lane.cc < 0x0A)
                panRowTop += 48;
        auto panPoints = [&]() { return doc.lanePoints(laneTrack, 0x0A); };
        const qreal dprLanes = lanes->devicePixelRatioF();
        // A pencil tap: the arrow tool's click writes nothing now, and the
        // detent lives in the shared press-time value mapping, so the
        // pencil's single-point click probes it just as directly.
        auto ctrlTap = [&](qreal x, int y, Qt::KeyboardModifiers mods) {
            sendKey(lanes, Qt::Key_B, Qt::NoModifier);
            sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(x), y), Qt::LeftButton,
                      Qt::LeftButton, mods);
            sendMouse(lanes, QEvent::MouseButtonRelease, QPoint(int(x), y), Qt::LeftButton,
                      Qt::NoButton, mods);
            sendKey(lanes, Qt::Key_B, Qt::NoModifier);
            QCoreApplication::processEvents();
        };
        for (const int rowH : {48, 96}) {
            // The per-row height override travels through the sidecar view
            // state, like a divider drag would set it.
            SongView::ViewState st = view.viewState();
            const QString panKey = QStringLiteral("cc:%1:%2").arg(laneTrack).arg(0x0A);
            if (rowH != 48) {
                st.laneHeights.insert(panKey, rowH);
                view.applyViewState(st);
                QCoreApplication::processEvents();
            }
            const int top = panRowTop + 5, bottom = panRowTop + rowH - 1 - 4;
            auto panValueAtY = [&](int y) {
                const int yc = std::clamp(y, top, bottom);
                return (bottom - yc) * 127 / (bottom - top);
            };
            auto panValueY = [&](int v) { return bottom - v * (bottom - top) / 127; };
            const int window = 127 * layout::fontPx(2.0 / 3.0) / rowH;
            // A press in clear air of any existing dots (grab slop 7 px).
            auto clearX = [&](int y) {
                qreal x = songview::kGutterW + (lanes->width() - songview::kGutterW) * 0.45;
                auto nearDot = [&](qreal probe) {
                    for (const DocLanePoint &pt : panPoints())
                        if (std::abs(view.displayX(double(pt.tick), songview::kGutterW, dprLanes) -
                                     probe) <= 12 &&
                            std::abs(panValueY(pt.value) - y) <= 12)
                            return true;
                    return false;
                };
                while (nearDot(x))
                    x += 40;
                return x;
            };
            // Tap, read back the committed value at the snapped tick, and
            // unwind exactly what the tap pushed (a grab that moved nothing
            // pushes no undo entry, so a blind undo could eat older edits).
            auto tapAndRead = [&](qreal x, int y, Qt::KeyboardModifiers mods) {
                // Truncate once: the tap presses at int(x), so the read-back
                // tick must come from the same pixel or a sub-pixel skew
                // could snap to a neighboring cell.
                const int xPress = int(x);
                const int before = doc.undoStack()->index();
                ctrlTap(xPress, y, mods);
                const uint64_t tick =
                    view.snapTick(view.tickAtContentX(xPress - songview::kGutterW));
                DocLanePoint pt;
                int value = INT_MIN;
                if (doc.findLanePoint(laneTrack, 0x0A, tick, &pt))
                    value = pt.value;
                else
                    fail("Ctrl detent probe press did not write a PAN point");
                while (doc.undoStack()->index() > before && doc.undoStack()->canUndo())
                    doc.undoStack()->undo();
                QCoreApplication::processEvents();
                return value;
            };
            // Mirror sanity, like the tempo mirror above: a plain tap must
            // write exactly the mirrored value.
            {
                const int y = panValueY(100);
                const qreal x = clearX(y);
                if (tapAndRead(x, y, Qt::NoModifier) != panValueAtY(y))
                    fail("test-side PAN value mirror drifted from the widget's mapping");
            }
            for (const int sign : {1, -1}) {
                // Just inside: the largest offset within the window that a
                // pixel row actually hits; just outside: the smallest
                // beyond it.
                int yIn = -1, yOut = -1;
                for (int y = top; y <= bottom; y++) {
                    const int d = (panValueAtY(y) - 64) * sign;
                    if (d > 0 && d <= window && (yIn < 0 || d > (panValueAtY(yIn) - 64) * sign))
                        yIn = y;
                    if (d > window && (yOut < 0 || d < (panValueAtY(yOut) - 64) * sign))
                        yOut = y;
                }
                if (yIn < 0 || yOut < 0) {
                    fail("Ctrl detent probe found no suitable pixel rows");
                    continue;
                }
                if (tapAndRead(clearX(yIn), yIn, Qt::ControlModifier) != 64)
                    fail(rowH == 48 ? "Ctrl press inside the detent window did not snap to center"
                                    : "Ctrl press inside the window did not snap on a tall lane");
                if (tapAndRead(clearX(yOut), yOut, Qt::ControlModifier) != panValueAtY(yOut))
                    fail(rowH == 48 ? "Ctrl press outside the detent window snapped to center"
                                    : "Ctrl press outside the window snapped on a tall lane");
            }
            if (rowH != 48) {
                st.laneHeights.remove(panKey);
                view.applyViewState(st);
                QCoreApplication::processEvents();
            }
        }
        view.removeEmptyLane(laneTrack, 0x0A);
        while (doc.undoStack()->index() > undoIndex && doc.undoStack()->canUndo())
            doc.undoStack()->undo();
        QCoreApplication::processEvents();
    }

    // Lane hiding (gutter menu → Hide lane): the row drops out of the lanes
    // area while its document data and the undo stack stay untouched, and
    // the add-lane menu grows a "Hidden lanes" section whose Show entry
    // restores the row. A hidden CC is never re-offered as a plain add
    // candidate (even a stale hidden key without a model lane), and a
    // non-canonical key spelling never surfaces an entry. Hiding drops the
    // lane out of an active time selection, and Show on a lane whose data
    // vanished while hidden re-adds it as an empty lane. The add-lane strip
    // opens on right-click as well as left, and an added-but-empty lane's
    // gutter opens the same menu. The menus are driven for real: the press
    // blocks in QMenu::exec(), so a zero-delay timer scheduled first fires
    // inside that loop and activates an entry by keyboard.
    {
        const int undoIndex = doc.undoStack()->index();
        const int laneTrack = view.selectedTrack();
        view.addEmptyLane(laneTrack, 0x0A);        // PAN; a no-op if the song has one
        doc.addLanePoint(laneTrack, 0x0A, 0, 100); // hidden data must survive
        QCoreApplication::processEvents();
        const std::vector<DocLanePoint> panBefore = doc.lanePoints(laneTrack, 0x0A);
        const int undoAfterSetup = doc.undoStack()->index();
        auto samePanPoints = [&]() {
            const std::vector<DocLanePoint> now = doc.lanePoints(laneTrack, 0x0A);
            if (now.size() != panBefore.size())
                return false;
            for (size_t i = 0; i < now.size(); i++)
                if (now[i].tick != panBefore[i].tick || now[i].value != panBefore[i].value)
                    return false;
            return true;
        };
        auto isHidden = [&](int track, int cc) {
            return view.viewState().hiddenLanes.contains(
                QStringLiteral("cc:%1:%2").arg(track).arg(cc));
        };
        // Row order mirror, like the detent probe: tempo, voice, then the
        // selected track's visible lanes by ascending CC, all 48 px here.
        auto laneRowTop = [&](uint8_t cc) {
            int top = 2 * 48;
            for (const AutoLane &lane : view.model().lanes)
                if (lane.track == laneTrack && lane.cc < cc && !isHidden(lane.track, lane.cc))
                    top += 48;
            return top;
        };
        auto addStripTop = [&]() {
            int rows = 2;
            for (const AutoLane &lane : view.model().lanes)
                if (lane.track == laneTrack && !isHidden(lane.track, lane.cc))
                    rows++;
            return rows * 48;
        };
        QString menuError;
        auto driveMenu = [&](const std::function<void(QMenu *)> &act) {
            menuError = QStringLiteral("menu did not open");
            QTimer::singleShot(0, [&, act]() {
                QMenu *menu = nullptr;
                for (QWidget *w : QApplication::topLevelWidgets())
                    if (auto *m = qobject_cast<QMenu *>(w); m && m->isVisible())
                        menu = m;
                if (!menu)
                    return;
                menuError.clear();
                act(menu);
                if (menu->isVisible()) // don't wedge exec() on a failed pick
                    sendKey(menu, Qt::Key_Escape, Qt::NoModifier);
            });
        };
        auto chooseAction = [&](QMenu *menu, const std::function<bool(QAction *)> &match,
                                const char *what) {
            for (QAction *action : menu->actions()) {
                if (match(action)) {
                    menu->setActiveAction(action);
                    sendKey(menu, Qt::Key_Return, Qt::NoModifier);
                    return;
                }
            }
            menuError = QStringLiteral("menu is missing %1").arg(QLatin1String(what));
        };
        auto clickGutter = [&](uint8_t cc) {
            const QPoint pos(8, laneRowTop(cc) + 10); // before the menu mutates row order
            sendMouse(lanes, QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton);
            sendMouse(lanes, QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton);
            QCoreApplication::processEvents();
        };

        // Hide the PAN lane through its gutter menu. A lanes-scope time
        // selection covering the row is parked first: it must not survive
        // the hide, or a range Delete would edit an invisible lane's events.
        {
            SongView::TimeSelection sel;
            sel.startTick = 0;
            sel.endTick = 960;
            sel.scope = SongView::TimeSelection::Lanes;
            sel.lanes.push_back({laneTrack, 0x0A});
            view.setTimeSelection(sel);
        }
        const int hBefore = lanes->minimumHeight();
        driveMenu([&](QMenu *menu) {
            chooseAction(
                menu, [](QAction *a) { return a->text() == QStringLiteral("Hide lane"); },
                "Hide lane");
        });
        clickGutter(0x0A);
        if (!menuError.isEmpty())
            fail(qUtf8Printable(QStringLiteral("gutter lane %1").arg(menuError)));
        if (!isHidden(laneTrack, 0x0A))
            fail("Hide lane did not record the hidden lane in the view state");
        if (lanes->minimumHeight() != hBefore - 48)
            fail("hiding a lane did not remove its row from the lanes area");
        if (!samePanPoints())
            fail("hiding a lane touched its document data");
        if (doc.undoStack()->index() != undoAfterSetup)
            fail("hiding a lane pushed an undo entry");
        if (view.timeSelectionCoversRow(laneTrack, 0x0A))
            fail("hiding a lane left it inside the active time selection");

        // A stale hidden key — its lane has no data and no model row — must
        // still keep its CC out of the plain add candidates (it is restored
        // through Show, not re-added as a second empty lane).
        uint8_t staleCc = 0;
        for (const int cc : {0x14, 0x15, 0x01, 0x07})
            if (!view.model().findLane(laneTrack, uint8_t(cc))) {
                staleCc = uint8_t(cc);
                break;
            }
        if (staleCc == 0) {
            fail("no free audible CC for the stale hidden-key probe");
        } else {
            SongView::ViewState st = view.viewState();
            st.hiddenLanes.insert(QStringLiteral("cc:%1:%2").arg(laneTrack).arg(staleCc));
            // A non-canonical spelling of the same identity (hand-edited
            // sidecar): carried in the set but never surfaced as an entry —
            // Show could only ever remove the canonical spelling.
            st.hiddenLanes.insert(QStringLiteral("cc:%1:0%2").arg(laneTrack).arg(staleCc));
            view.applyViewState(st);
            QCoreApplication::processEvents();
        }

        // Restore PAN from the add-lane menu, opened with a right-click on
        // the strip; the hidden entries must be Show actions (data 256+cc),
        // never plain add candidates (data cc), and exactly one per hidden
        // lane identity (no phantom from the non-canonical key).
        bool hiddenOfferedAsAdd = false;
        int showEntries = 0;
        driveMenu([&](QMenu *menu) {
            for (QAction *action : menu->actions()) {
                if (!action->data().isValid())
                    continue;
                if (action->data().toInt() == 0x0A ||
                    (staleCc != 0 && action->data().toInt() == int(staleCc)))
                    hiddenOfferedAsAdd = true;
                if (action->data().toInt() >= 256)
                    showEntries++;
            }
            chooseAction(
                menu,
                [](QAction *a) { return a->data().isValid() && a->data().toInt() == 256 + 0x0A; },
                "the hidden lane's Show entry");
        });
        {
            const QPoint strip(songview::kGutterW + 40, addStripTop() + 4);
            sendMouse(lanes, QEvent::MouseButtonPress, strip, Qt::RightButton, Qt::RightButton);
            sendMouse(lanes, QEvent::MouseButtonRelease, strip, Qt::RightButton, Qt::NoButton);
            QCoreApplication::processEvents();
        }
        if (!menuError.isEmpty())
            fail(qUtf8Printable(QStringLiteral("right-click add-lane %1").arg(menuError)));
        if (hiddenOfferedAsAdd)
            fail("a hidden lane was still offered as a plain add candidate");
        if (staleCc != 0 && showEntries != 2)
            fail("a non-canonical hidden key surfaced a phantom Show entry");
        if (isHidden(laneTrack, 0x0A))
            fail("Show did not clear the hidden lane from the view state");
        if (lanes->minimumHeight() != hBefore)
            fail("Show did not restore the hidden lane's row");
        if (!samePanPoints())
            fail("the hide/show round trip touched the lane data");

        // An added-but-empty lane's gutter opens the same menu.
        if (staleCc != 0) {
            SongView::ViewState st = view.viewState(); // retire the stale key
            st.hiddenLanes.clear();
            view.applyViewState(st);
            view.addEmptyLane(laneTrack, staleCc);
            QCoreApplication::processEvents();
            bool sawRemoveEmpty = false, sawHide = false;
            driveMenu([&](QMenu *menu) {
                for (QAction *action : menu->actions()) {
                    sawRemoveEmpty |= action->text() == QStringLiteral("Remove empty lane");
                    sawHide |= action->text() == QStringLiteral("Hide lane");
                }
            });
            clickGutter(staleCc);
            if (!menuError.isEmpty())
                fail(qUtf8Printable(QStringLiteral("empty lane's gutter %1").arg(menuError)));
            else if (!sawRemoveEmpty || !sawHide)
                fail("empty lane's gutter menu is missing its lane actions");
            view.removeEmptyLane(laneTrack, staleCc);
        }

        // Show on a hidden lane whose data has since vanished (event-list
        // delete, or undo of its insertion) must bring the row back as an
        // empty lane instead of restoring nothing. Probed on staleCc — a
        // lane born from a single point, so undoing it truly leaves the
        // model laneless (the song may carry its own PAN data).
        if (staleCc != 0) {
            doc.addLanePoint(laneTrack, staleCc, 0, 64);
            QCoreApplication::processEvents();
            const int hWithLane = lanes->minimumHeight();
            {
                SongView::ViewState st = view.viewState();
                st.hiddenLanes.insert(QStringLiteral("cc:%1:%2").arg(laneTrack).arg(staleCc));
                view.applyViewState(st);
            }
            doc.undoStack()->undo(); // its only point: the hidden data vanishes
            QCoreApplication::processEvents();
            driveMenu([&](QMenu *menu) {
                chooseAction(
                    menu,
                    [&](QAction *a) {
                        return a->data().isValid() && a->data().toInt() == 256 + int(staleCc);
                    },
                    "the vanished lane's Show entry");
            });
            {
                const QPoint strip(songview::kGutterW + 40, addStripTop() + 4);
                sendMouse(lanes, QEvent::MouseButtonPress, strip, Qt::RightButton, Qt::RightButton);
                sendMouse(lanes, QEvent::MouseButtonRelease, strip, Qt::RightButton, Qt::NoButton);
                QCoreApplication::processEvents();
            }
            if (!menuError.isEmpty())
                fail(qUtf8Printable(QStringLiteral("vanished-lane add-lane %1").arg(menuError)));
            if (isHidden(laneTrack, staleCc))
                fail("Show on a vanished lane kept it hidden");
            if (lanes->minimumHeight() != hWithLane)
                fail("Show on a vanished lane did not bring its row back empty");
            const SongView::ViewState st = view.viewState();
            const std::pair<int, uint8_t> key(laneTrack, staleCc);
            if (std::find(st.emptyLanes.begin(), st.emptyLanes.end(), key) == st.emptyLanes.end())
                fail("Show on a vanished lane did not re-register the empty lane");
            view.removeEmptyLane(laneTrack, staleCc);
        }

        view.removeEmptyLane(laneTrack, 0x0A);
        while (doc.undoStack()->index() > undoIndex && doc.undoStack()->canUndo())
            doc.undoStack()->undo();
        QCoreApplication::processEvents();
    }

    // Automation hover & gesture value chips: hovering a point's dot rings
    // it in the edit-preview color at the point's exact position (the ring
    // uses the left-press grab box, so ring and grab can never disagree —
    // and the pencil never rings, since it never grabs; the tick is
    // mirrored in the hoverNodeTick property). Hovering the Voice row
    // reads out the voice in effect at the cursor's tick, suppressed
    // within the marker hit radius. During any left drag the committed
    // value rides a filled chip clamped inside the row's plot, even
    // dragged to the row's edge. Hover repaints are region-exact: leaving
    // the area restores the pre-hover pixels bit-for-bit.
    {
        const int undoIndex = doc.undoStack()->index();
        const int laneTrack = view.selectedTrack();
        const QByteArray sectionBytes = doc.smf().write();
        view.clearTimeSelection();
        if (!view.viewState().hiddenLanes.isEmpty())
            fail("hover section setup: expected no hidden lanes");

        // A data-free CC lane for the dot probes. PAN is excluded (its c_v
        // text would complicate the chip mirror); every candidate keeps the
        // fixed 0..127 axis the mirrors assume.
        uint8_t freeCc = 0;
        for (uint8_t cc : {uint8_t(0x14), uint8_t(0x15), uint8_t(0x07)}) {
            if (doc.lanePoints(laneTrack, cc).empty()) {
                freeCc = cc;
                break;
            }
        }
        if (!freeCc)
            fail("hover section setup: no data-free CC lane available");
        view.addEmptyLane(laneTrack, freeCc);
        QCoreApplication::processEvents();
        // Row-geometry mirrors, like the pencil and node-selection
        // sections: tempo row 0, voice row, then the track's lanes by
        // ascending CC, all at the default 48 px height.
        int ccRowTop = 2 * 48;
        for (const AutoLane &lane : view.model().lanes)
            if (lane.track == laneTrack && lane.cc < freeCc)
                ccRowTop += 48;
        const int ccTop = ccRowTop + 5, ccBottom = ccRowTop + 48 - 1 - 4;
        auto ccValueAtY = [&](int y) {
            const int yc = std::clamp(y, ccTop, ccBottom);
            return (ccBottom - yc) * 127 / (ccBottom - ccTop);
        };
        auto ccValueY = [&](int v) { return ccBottom - v * (ccBottom - ccTop) / 127; };

        const qreal dprLanes = lanes->devicePixelRatioF();
        auto dotX = [&](uint64_t t) {
            return view.displayX(double(t), songview::kGutterW, dprLanes);
        };
        auto contestedX = [&](qreal x) { return overlayContestedX(view, dprLanes, x); };
        auto voiceMarkerNear = [&](qreal x, qreal within) {
            for (const DocLanePoint &pt : doc.lanePoints(laneTrack, DOC_CC_VOICE))
                if (std::abs(dotX(pt.tick) - x) < within)
                    return true;
            return false;
        };
        auto lanesImage = [&]() { return lanes->grab().toImage(); };
        auto pixelAt = [&](const QImage &img, qreal x, int y) {
            return img.pixelColor(int((x + 0.5) * dprLanes), int((y + 0.5) * dprLanes));
        };
        auto hoverAt = [&](QPoint p) {
            sendMouse(lanes, QEvent::MouseMove, p, Qt::NoButton, Qt::NoButton);
        };
        auto leaveLanes = [&] {
            QEvent leave(QEvent::Leave);
            QCoreApplication::sendEvent(lanes, &leave);
        };
        auto hoverNodeTick = [&]() { return lanes->property("hoverNodeTick").toLongLong(); };

        // Clear air for the probe dot: no overlay vertical at the dot or
        // any of the hover/drag spots to its right.
        qreal xSeek = songview::kGutterW + (lanes->width() - songview::kGutterW) * 0.35;
        uint64_t tA = view.snapTick(view.tickAtContentX(xSeek - songview::kGutterW));
        auto dotContested = [&]() {
            for (qreal off : {0.0, 20.0, 40.0, 60.0})
                if (contestedX(dotX(tA) + off))
                    return true;
            return false;
        };
        while (dotContested()) {
            xSeek += 40;
            tA = view.snapTick(view.tickAtContentX(xSeek - songview::kGutterW));
        }
        constexpr int vA = 64;
        doc.writeLanePoints(laneTrack, freeCc, tA, tA, {{tA, vA}});
        QCoreApplication::processEvents();
        const qreal xDot = dotX(tA);
        const int yDot = ccValueY(vA);
        if (yDot - 7 <= ccRowTop || yDot + 7 >= ccRowTop + 48)
            fail("hover section setup: probe dot too close to the row edge");

        const QColor previewColor = themes::color(themes::Role::song_view_edit_preview_outline);
        // The ring (radius 4.5, pen 2) and the idle marker (radius 3, pen
        // 1) are aliased: a pixel inks iff its center falls inside the
        // stroke, so the ring reliably covers every center in radial
        // (3.5, 5.5) device-scaled and the marker never inks past 3.5.
        // Scanning [4.0, 5.4] bands (width 1.4 — always at least one pixel
        // center at any scale) therefore sees the ring and never the
        // marker. The band right of the dot is skipped: the readout text
        // starts at x + 6 in the same color.
        auto previewInkIn = [&](const QImage &img, qreal lx0, qreal lx1, qreal ly0, qreal ly1) {
            for (int py = int(std::ceil(ly0 * dprLanes)); py <= int(std::floor(ly1 * dprLanes));
                 py++)
                for (int px = int(std::ceil(lx0 * dprLanes)); px <= int(std::floor(lx1 * dprLanes));
                     px++) {
                    if (px < 0 || py < 0 || px >= img.width() || py >= img.height())
                        continue;
                    if (img.pixelColor(px, py) == previewColor)
                        return true;
                }
            return false;
        };
        // full = all three bands ink (a ring is painted); trace = any band
        // inks. Absence probes assert on trace, so a partially-erased ring
        // (an under-sized invalidation leaves arcs outside the stale
        // region) still trips them.
        struct RingInk {
            bool full;
            bool trace;
        };
        auto ringInkAt = [&](const QImage &img, qreal cx, int cy) {
            const bool left = previewInkIn(img, cx - 5.4, cx - 4.0, cy - 1, cy + 1);
            const bool top = previewInkIn(img, cx - 1, cx + 1, cy - 5.4, cy - 4.0);
            const bool bottom = previewInkIn(img, cx - 1, cx + 1, cy + 4.0, cy + 5.4);
            return RingInk{left && top && bottom, left || top || bottom};
        };
        auto ringInkAtDot = [&](const QImage &img) { return ringInkAt(img, xDot, yDot); };

        // On the dot: the readout rings the point and mirrors its tick.
        leaveLanes();
        if (hoverNodeTick() != -1)
            fail("hover node property not cleared at rest");
        hoverAt(QPoint(int(xDot), yDot));
        if (hoverNodeTick() != qlonglong(tA))
            fail("hovering a dot did not register the node hover");
        if (!ringInkAtDot(lanesImage()).full)
            fail("hover ring not painted at the dot");
        // Anywhere inside the grab box rings the dot itself, not the cursor.
        hoverAt(QPoint(int(xDot) + 5, yDot - 5));
        if (hoverNodeTick() != qlonglong(tA))
            fail("hover inside the grab box did not register the node");
        if (!ringInkAtDot(lanesImage()).full)
            fail("grab-box hover did not ring the dot itself");
        // Off the dot: back to the curve readout, no ring.
        hoverAt(QPoint(int(xDot) + 40, yDot));
        if (hoverNodeTick() != -1)
            fail("off-dot hover still registered a node");
        if (ringInkAtDot(lanesImage()).trace)
            fail("off-dot hover left ring ink at the dot");
        // The pencil never grabs, so it must not promise a grab with a
        // ring — including the moment the mode turns on while a ring is
        // showing: the toggle must erase it, not strand it in the content
        // cache (the intermediate grab bakes the ring in first, so the
        // toggle's own invalidation is what's exercised).
        hoverAt(QPoint(int(xDot), yDot));
        (void)lanesImage();
        sendKey(lanes, Qt::Key_B, Qt::NoModifier);
        if (hoverNodeTick() != -1)
            fail("pencil toggle kept the node hover registered");
        if (ringInkAtDot(lanesImage()).trace)
            fail("pencil toggle stranded the hover ring");
        hoverAt(QPoint(int(xDot), yDot));
        if (hoverNodeTick() != -1)
            fail("pencil-mode hover registered a node");
        if (ringInkAtDot(lanesImage()).trace)
            fail("pencil-mode hover painted the grab ring");
        sendKey(lanes, Qt::Key_B, Qt::NoModifier);

        // Voice row: a marker of our own in clear air (no other marker
        // within 60 px of it or of the hover spot, both overlay-free).
        const int yVoice = 48 + 24;
        qreal xvSeek = songview::kGutterW + (lanes->width() - songview::kGutterW) * 0.5;
        uint64_t tM = view.snapTick(view.tickAtContentX(xvSeek - songview::kGutterW));
        auto voiceSpotContested = [&]() {
            const qreal xm = dotX(tM);
            return contestedX(xm) || contestedX(xm + 20) || voiceMarkerNear(xm, 60) ||
                   voiceMarkerNear(xm + 20, 60);
        };
        while (voiceSpotContested()) {
            xvSeek += 40;
            tM = view.snapTick(view.tickAtContentX(xvSeek - songview::kGutterW));
        }
        doc.addLanePoint(laneTrack, DOC_CC_VOICE, tM, 0);
        QCoreApplication::processEvents();
        const qreal xM = dotX(tM);
        leaveLanes();
        const QImage voiceBase = lanesImage();
        hoverAt(QPoint(int(xM) + 20, yVoice));
        if (lanesImage() == voiceBase)
            fail("voice-row hover painted no readout");
        // Directly over the marker the readout is suppressed — and moving
        // there must restore the previous readout's pixels exactly.
        hoverAt(QPoint(int(xM), yVoice));
        if (lanesImage() != voiceBase)
            fail("voice-row hover over a marker was not suppressed");

        // Range-drag preview on the voice row: a track-scope band carrying
        // the marker draws its line at the live delta and no longer at rest
        // (a move), or at both with the copy ghosted (a duplicate) — the
        // commit takes the track's voice changes with everything else, so
        // the preview must show them going.
        {
            leaveLanes();
            auto markerInk = [&](const QImage &img, qreal x) {
                const QColor ink = SongView::trackColor(laneTrack);
                int hits = 0;
                for (int y = yVoice - 8; y <= yVoice + 8; y++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        const QColor c = pixelAt(img, x + dx, y);
                        if (std::abs(c.red() - ink.red()) + std::abs(c.green() - ink.green()) +
                                std::abs(c.blue() - ink.blue()) <
                            60)
                            hits++;
                    }
                }
                return hits;
            };
            SongView::TimeSelection band;
            band.startTick = tM;
            band.endTick = tM + view.snapTicksAt(tM);
            view.setTimeSelection(band);
            const QImage resting = lanesImage();
            const double cursor = view.tickAtContentX(xM + 80 - songview::kGutterW);
            for (const bool duplicate : {false, true}) {
                if (!view.beginRangeDrag(double(tM), duplicate))
                    fail("voice-row preview: range drag did not begin");
                view.updateRangeDrag(cursor);
                const int64_t dTick = view.rangeDrag().dTick;
                if (dTick <= 0)
                    fail("voice-row preview: the drag did not arm");
                const QImage live = lanesImage();
                const qreal xTo = dotX(uint64_t(int64_t(tM) + dTick));
                // The ghost is translucent (alpha 110 over whatever rested
                // there), so it is probed as that blend of the track's ink
                // over the resting pixel — never as "the column changed",
                // which the live drag's own overlay would satisfy.
                auto ghostInk = [&](qreal x) {
                    const QColor ink = SongView::trackColor(laneTrack);
                    int hits = 0;
                    for (int y = yVoice - 8; y <= yVoice + 8; y++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            const QColor bg = pixelAt(resting, x + dx, y);
                            const QColor c = pixelAt(live, x + dx, y);
                            auto blend = [&](int b, int i) { return b + (i - b) * 110 / 255; };
                            if (std::abs(c.red() - blend(bg.red(), ink.red())) +
                                    std::abs(c.green() - blend(bg.green(), ink.green())) +
                                    std::abs(c.blue() - blend(bg.blue(), ink.blue())) <
                                45)
                                hits++;
                        }
                    }
                    return hits;
                };
                if (duplicate ? ghostInk(xTo) < 8 : markerInk(live, xTo) < 8)
                    fail(duplicate ? "voice-row duplicate preview drew no ghost marker"
                                   : "voice-row move preview did not carry the marker");
                if (duplicate ? markerInk(live, xM) < markerInk(resting, xM)
                              : markerInk(live, xM) >= markerInk(resting, xM))
                    fail(duplicate ? "voice-row duplicate preview dropped the original"
                                   : "voice-row move preview left the marker at rest");
                view.cancelRangeDrag();
                if (lanesImage() != resting)
                    fail("cancelling the range drag did not restore the voice row");
            }
            view.clearTimeSelection();
        }

        // Region hygiene: a hover walk across rows, then leaving, restores
        // the resting pixels bit-for-bit — no readout trails — and clears
        // the node-hover mirror. Each step grabs (and discards) an image so
        // every transition repaints incrementally; batching the moves would
        // let their accumulated dirty regions mask a missing invalidation.
        leaveLanes();
        const QImage resting = lanesImage();
        hoverAt(QPoint(int(xDot), yDot));
        (void)lanesImage();
        hoverAt(QPoint(int(xDot) + 40, yDot));
        (void)lanesImage();
        hoverAt(QPoint(int(xM) + 20, yVoice));
        (void)lanesImage();
        hoverAt(QPoint(int(xDot), yDot));
        (void)lanesImage();
        leaveLanes();
        if (hoverNodeTick() != -1)
            fail("leaving the lanes did not clear the node hover");
        if (lanesImage() != resting)
            fail("hover moves left readout trails behind");

        // Over the pre-roll pad every x clamps to tick 0, so a hit test
        // run on an x reconstructed from the tick would land exactly on a
        // tick-0 dot and promise a grab the press won't perform (the press
        // uses the raw x). With the raw cursor x threaded through, the pad
        // never rings.
        {
            const SongView::ViewState savedView = view.viewState();
            SongView::ViewState homeView = savedView;
            homeView.scrollPx = -1e9; // clamped to the camera's minimum
            view.applyViewState(homeView);
            doc.writeLanePoints(laneTrack, freeCc, 0, 0, {{0, 50}});
            QCoreApplication::processEvents();
            const qreal xZero = dotX(0);
            if (xZero - songview::kGutterW < 20)
                fail("hover section setup: no pre-roll pad at home scroll");
            const int yZero = ccValueY(50);
            hoverAt(QPoint(int((songview::kGutterW + xZero) / 2), yZero));
            if (hoverNodeTick() != -1)
                fail("pre-roll pad hover promised a grab on the tick-0 point");
            if (ringInkAt(lanesImage(), xZero, yZero).trace)
                fail("pre-roll pad hover ringed the tick-0 dot");
            leaveLanes();
            doc.undoStack()->undo();
            view.applyViewState(savedView);
            QCoreApplication::processEvents();
        }

        // Scroll under a stationary cursor clears the hover instead of
        // letting the ring ride the content away from the pointer.
        hoverAt(QPoint(int(xDot), yDot));
        if (hoverNodeTick() != qlonglong(tA))
            fail("hover section setup: re-hover before the wheel probe failed");
        sendWheel(lanes, QPointF(xDot, yDot), 120, 0, Qt::ShiftModifier);
        if (hoverNodeTick() != -1)
            fail("wheel scroll kept the node hover");
        sendWheel(lanes, QPointF(xDot, yDot), -120, 0, Qt::ShiftModifier);
        leaveLanes();

        // A row rebuild (track switch, lane hide, any document edit)
        // resets the node-hover mirror even though no mouse move follows.
        hoverAt(QPoint(int(xDot), yDot));
        if (hoverNodeTick() != qlonglong(tA))
            fail("hover section setup: re-hover before the rebuild probe failed");
        view.addEmptyLane(laneTrack, 0x5B); // sorts below freeCc: rows above keep their tops
        QCoreApplication::processEvents();
        if (hoverNodeTick() != -1)
            fail("row rebuild kept the node hover mirror");
        view.removeEmptyLane(laneTrack, 0x5B);
        QCoreApplication::processEvents();
        leaveLanes();

        // Gesture chip: mirror of the widget's chip layout (caption font,
        // text anchored right of and above the pending marker, clamped
        // into the row's plot). The backdrop fill ring at the chip's
        // corner is glyph-free, so it probes as the exact backdrop color.
        const QColor chipColor = themes::color(themes::Role::song_view_piano_roll_accidental_lane);
        const QFont chipFont = typography::caption(lanes->font());
        const QFontMetrics chipFm(chipFont);
        const QRect ccPlot(songview::kGutterW, ccRowTop, lanes->width() - songview::kGutterW, 48);
        auto chipRect = [&](qreal x, int yMark, const QString &text) {
            QRect chip(int(std::ceil(x)) + 6, yMark - 4 - chipFm.height(),
                       chipFm.horizontalAdvance(text), chipFm.height());
            if (chip.right() > ccPlot.right())
                chip.moveRight(ccPlot.right());
            if (chip.left() < ccPlot.left())
                chip.moveLeft(ccPlot.left());
            if (chip.top() < ccPlot.top())
                chip.moveTop(ccPlot.top());
            if (chip.bottom() > ccPlot.bottom())
                chip.moveBottom(ccPlot.bottom());
            return chip;
        };

        // Freehand sweep mid-row: the chip follows the pending value. The
        // arming move clears the sweep's activation slop straight down, so
        // the stroke's x is untouched and every later sample's value is the
        // one that many pixels above the cursor.
        {
            const int slop = layout::fontPx(5.0 / 12.0);
            sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(xDot) + 40, ccRowTop + 30),
                      Qt::LeftButton, Qt::LeftButton);
            sendMouse(lanes, QEvent::MouseMove, QPoint(int(xDot) + 40, ccRowTop + 30 + slop),
                      Qt::NoButton, Qt::LeftButton);
            sendMouse(lanes, QEvent::MouseMove, QPoint(int(xDot) + 60, ccRowTop + 35), Qt::NoButton,
                      Qt::LeftButton);
            const uint64_t tDrag =
                view.snapTick(view.tickAtContentX(int(xDot) + 60 - songview::kGutterW));
            const QRect chip = chipRect(dotX(tDrag), ccValueY(ccValueAtY(ccRowTop + 35 - slop)),
                                        QString::number(ccValueAtY(ccRowTop + 35 - slop)));
            const QImage img = lanesImage();
            if (pixelAt(img, chip.left() - 1, chip.top() + 1) != chipColor ||
                pixelAt(img, chip.left() - 1, chip.bottom() + 1) != chipColor)
                fail("sweep drag did not paint the value chip");
            sendMouse(lanes, QEvent::MouseButtonRelease, QPoint(int(xDot) + 60, ccRowTop + 35),
                      Qt::LeftButton, Qt::NoButton);
            QCoreApplication::processEvents();
        }

        // Point drag to the row's top edge: the unclamped chip would sit
        // above the row; clamping pins it to the plot's top, still fully
        // painted — and nothing chip-colored leaks into the row above.
        {
            sendMouse(lanes, QEvent::MouseButtonPress, QPoint(int(xDot), yDot), Qt::LeftButton,
                      Qt::LeftButton);
            sendMouse(lanes, QEvent::MouseMove, QPoint(int(xDot), ccRowTop + 1), Qt::NoButton,
                      Qt::LeftButton);
            // The move re-snaps the drag tick from the cursor's x, so the
            // mirror derives it the same way instead of assuming tA.
            const uint64_t tDrag =
                view.snapTick(view.tickAtContentX(int(xDot) - songview::kGutterW));
            const QString text = QString::number(ccValueAtY(ccRowTop + 1));
            const QRect chip = chipRect(dotX(tDrag), ccValueY(ccValueAtY(ccRowTop + 1)), text);
            if (chip.top() != ccRowTop)
                fail("hover section setup: top-edge drag did not engage the chip clamp");
            const QImage img = lanesImage();
            if (pixelAt(img, chip.left() - 1, chip.top() + 1) != chipColor ||
                pixelAt(img, chip.left() - 1, chip.bottom() + 1) != chipColor)
                fail("top-edge point drag did not paint the clamped chip");
            bool leaked = false;
            for (int y = ccRowTop - 6; y < ccRowTop && !leaked; y++)
                for (int x = chip.left() - 2; x <= chip.right() + 2 && !leaked; x++)
                    leaked = pixelAt(img, x, y) == chipColor;
            if (leaked)
                fail("chip pixels leaked above the row plot");
            sendMouse(lanes, QEvent::MouseButtonRelease, QPoint(int(xDot), ccRowTop + 1),
                      Qt::LeftButton, Qt::NoButton);
            QCoreApplication::processEvents();
        }

        while (doc.undoStack()->index() > undoIndex && doc.undoStack()->canUndo())
            doc.undoStack()->undo();
        QCoreApplication::processEvents();
        view.removeEmptyLane(laneTrack, freeCc);
        QCoreApplication::processEvents();
        if (doc.smf().write() != sectionBytes)
            fail("hover section did not restore the document bytes");
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
        auto *editor = view.findChild<QLineEdit *>(QStringLiteral("trackRenameEditor"));
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
        auto *row = view.findChild<QWidget *>(QStringLiteral("trackHeaderRow%1").arg(track));
        if (!row) {
            fail("track header row for the edited track not found");
        } else {
            int revealed = -1, reveals = 0;
            const QMetaObject::Connection conn =
                QObject::connect(&view, &SongView::revealVoiceRequested, [&](int program) {
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
            sendMouse(row, QEvent::MouseButtonPress, voicePos, Qt::LeftButton, Qt::LeftButton);
            sendMouse(row, QEvent::MouseMove, voicePos + QPoint(0, 25), Qt::NoButton,
                      Qt::LeftButton);
            sendMouse(row, QEvent::MouseButtonRelease, voicePos + QPoint(0, 25), Qt::LeftButton,
                      Qt::NoButton);
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
                            voiceList->item(0)->isHidden() && !voiceList->item(127)->isHidden();
                        searchField->clear();
                        searchFilteredList &= !voiceList->item(0)->isHidden();
                        voiceList->setCurrentRow(127);
                        searchField->setText(QStringLiteral("1"));
                        searchFilteredList &=
                            voiceList->currentRow() == 1 && !voiceList->item(1)->isHidden() &&
                            !voiceList->item(127)->isHidden() &&
                            dialogButtons->button(QDialogButtonBox::Ok)->isEnabled();
                        searchField->clear();
                        searchFilteredList &= voiceList->currentRow() == 0;
                    }
                    dlg->reject();
                }
            });
            poll.start();
            sendMouse(row, QEvent::MouseButtonDblClick, voicePos, Qt::LeftButton, Qt::LeftButton);
            sendMouse(row, QEvent::MouseButtonRelease, voicePos, Qt::LeftButton, Qt::NoButton);
            QCoreApplication::processEvents(); // the queued picker runs here
            poll.stop();
            if (!pickerSeen)
                fail("voice-line double-click did not open the voice picker");
            if (!searchFilteredList)
                fail("voice picker search did not select and restore its first match");
            auto *renameEditor = view.findChild<QLineEdit *>(QStringLiteral("trackRenameEditor"));
            if (renameEditor && !renameEditor->isHidden())
                fail("voice-line double-click opened the rename editor");
            const QPoint namePos(row->width() / 2, 10);
            sendMouse(row, QEvent::MouseButtonDblClick, namePos, Qt::LeftButton, Qt::LeftButton);
            sendMouse(row, QEvent::MouseButtonRelease, namePos, Qt::LeftButton, Qt::NoButton);
            renameEditor = view.findChild<QLineEdit *>(QStringLiteral("trackRenameEditor"));
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
            sendMouse(row0, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
            sendMouse(row0, QEvent::MouseMove, drop, Qt::NoButton, Qt::LeftButton);
            sendMouse(row0, QEvent::MouseButtonRelease, drop, Qt::RightButton, Qt::LeftButton);
            sendMouse(row0, QEvent::MouseButtonRelease, drop, Qt::LeftButton, Qt::NoButton);
            QCoreApplication::processEvents();
            if (doc.undoStack()->count() != preDragCount)
                fail("right-button release mid-drag committed the reorder");

            // An open rename editor rides along: the drop commits its text
            // Reaper-style (before the move, so it names the right track)
            // instead of silently discarding it with the rebuilt panel.
            view.renameTrack(0);
            auto *editor = view.findChild<QLineEdit *>(QStringLiteral("trackRenameEditor"));
            if (editor && !editor->isHidden()) {
                editor->setText(QStringLiteral("Dragged"));
                dragRenamed = true;
            }

            sendMouse(row0, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
            sendMouse(row0, QEvent::MouseMove, drop, Qt::NoButton, Qt::LeftButton);
            sendMouse(row0, QEvent::MouseButtonRelease, drop, Qt::LeftButton, Qt::NoButton);
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
    sendMouse(roll, QEvent::MouseMove, QPoint(songview::kKeyboardW + 60, roll->height() / 3),
              Qt::NoButton, Qt::NoButton);
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
            if (sel.size() != 1 || !(sel[0] == SongView::NoteKey{target.startTick, target.key}))
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
        auto *row = view.findChild<QWidget *>(QStringLiteral("trackHeaderRow%1").arg(track));
        auto *muteButton =
            row ? row->findChild<QToolButton *>(QStringLiteral("trackMuteButton")) : nullptr;
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
        if (view.findChild<QWidget *>(QStringLiteral("trackHeaderRow%1").arg(other))) {
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

    // Twenty-three commands: draw, set, draw, nudge, draw, the double-click
    // delete, the press-grown draw, the tiny-drag draw, the modifier
    // velocity nudge, the abutting-note fixture add, add, two resizes, the
    // three note-selection presses MERGED into one, the off-grid
    // behind-the-back move, Ctrl+Left (all the scroll-follow presses merge
    // into it), two time-selection moves (kept separate by the clean-index
    // save point), the three mouse range drags (move, duplicate, ruler
    // move), the inline rename, and the mid-song voice change — plus,
    // when the song has a second track, the header-drag track move and the
    // editor commit the drop flushes. Undoing them all must restore the
    // original bytes.
    int undos = 0;
    while (doc.undoStack()->canUndo() && undos < 100) {
        doc.undoStack()->undo();
        undos++;
    }
    if (undos != 23 + (reordered ? (dragRenamed ? 2 : 1) : 0))
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
