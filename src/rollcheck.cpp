#include <QCoreApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QIcon>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPixmap>
#include <QPoint>
#include <QString>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <cstdio>
#include <vector>

#include "core/songdocument.h"
#include "project/decompproject.h"
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
// covered notes on release. Ctrl+arrows transpose (Shift: octave)
// and nudge the selection along the same absolute grid — both the roll's
// note selection and a multi-track time selection — and the view follows
// notes moved out of sight with a minimal scroll (flush at the edge, not
// re-centered). The playhead follow-scroll pauses while a mouse gesture
// is held (pan, drag, sweep) and resumes on release. Dragging a track
// header row reorders the tracks, the mute flag following the moved
// track through undo and redo; a right-button release cancels the drag,
// and a drop with a rename editor open commits the typed name first.
// Undoing every gesture must restore the original bytes.

namespace {

void sendMouse(QWidget *w, QEvent::Type type, QPoint pos, Qt::MouseButton button,
               Qt::MouseButtons buttons)
{
    QMouseEvent ev(type, QPointF(pos), QPointF(w->mapToGlobal(pos)), button,
                   buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(w, &ev);
}

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

} // namespace

int runRollCheck(const QString &projectRoot, const QString &songLabel,
                 const QString &screenshotPath)
{
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
    // ruler's own control) so cells are a comfortable 32px.
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
    const int keyH = view.keyHeight();

    // A row/cell is taken if a note of the selected track sits within one
    // cell of it (the roll's hit test pads note rects by 2px; a full cell of
    // clearance keeps the check's clicks unambiguous).
    auto occupied = [&](uint64_t tick, uint64_t dur, int key) {
        for (const DocNote &note : doc.notesForTrack(track)) {
            if (int(note.key) != key)
                continue;
            const uint64_t end = note.unterminated()
                                     ? UINT64_MAX
                                     : note.tick + note.duration + dur;
            if (note.tick < tick + 2 * dur && end > tick)
                return true;
        }
        return false;
    };

    // A free grid cell with click targets: mid-cell x, and a y in the Move
    // zone (center) or in the top velocity-handle strip.
    struct Cell {
        uint64_t tick = 0, dur = 0;
        int key = -1;
        QPoint center;
        QPoint handle;
    };
    auto findFreeCell = [&]() -> Cell {
        Cell cell;
        for (int key = 115; key >= 24; key--) {
            const int y = (127 - key) * keyH - view.scrollY();
            if (y < 0 || y + keyH > roll->height())
                continue;
            for (int probe = 8; probe < roll->width() - songview::kKeyboardW - 40;
                 probe += 24) {
                const uint64_t tick = view.snapTickDown(view.tickAtContentX(probe));
                const uint64_t dur = view.gridTicksAt(tick);
                const int x0 = songview::kKeyboardW + view.contentX(double(tick));
                const int x1 =
                    songview::kKeyboardW + view.contentX(double(tick + dur));
                // Wide enough that mid-cell clears the 3px resize edges.
                if (x0 < songview::kKeyboardW || x1 - x0 < 12
                    || x1 >= roll->width())
                    continue;
                if (occupied(tick, dur, key))
                    continue;
                cell.tick = tick;
                cell.dur = dur;
                cell.key = key;
                cell.center = QPoint((x0 + x1) / 2, y + keyH / 2 + 1);
                cell.handle = QPoint((x0 + x1) / 2, y + 2);
                return cell;
            }
        }
        return cell;
    };

    // Baseline: the pencil draws at velocity 100 on a fresh document.
    const Cell a = findFreeCell();
    if (a.key < 0) {
        fail("no free grid cell to draw in");
        return 1;
    }
    drawNote(roll, a.center);
    DocNote noteA;
    if (!doc.findNote(track, a.tick, uint8_t(a.key), &noteA)) {
        fail("pencil draw produced no note");
        return failures;
    }
    if (noteA.velocity != 100)
        fail("fresh document does not draw at velocity 100");

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

    // Drag latch: pull note B's velocity handle 20px up (1px = 1 step),
    // 73 -> 93. The latch must follow the dragged value, not the press value.
    sendMouse(roll, QEvent::MouseButtonPress, b.handle, Qt::LeftButton,
              Qt::LeftButton);
    sendMouse(roll, QEvent::MouseMove, b.handle - QPoint(0, 20), Qt::NoButton,
              Qt::LeftButton);
    sendMouse(roll, QEvent::MouseButtonRelease, b.handle - QPoint(0, 20),
              Qt::LeftButton, Qt::NoButton);
    DocNote dragged;
    if (!doc.findNote(track, b.tick, uint8_t(b.key), &dragged)
        || dragged.velocity != 93)
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
        auto conn = QObject::connect(
            &view, &SongView::auditionNoteTimed, &view,
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
        sendMouse(roll, QEvent::MouseMove, sweepStart + QPoint(4, 4),
                  Qt::NoButton, Qt::RightButton);
        if (std::find(offKeys.begin(), offKeys.end(), a.key) == offKeys.end())
            fail("shrinking the band did not release the departed note");
        sendMouse(roll, QEvent::MouseMove, sweepEnd, Qt::NoButton,
                  Qt::RightButton);
        sendMouse(roll, QEvent::MouseButtonRelease, sweepEnd, Qt::RightButton,
                  Qt::NoButton);
        QObject::disconnect(conn);
        if (std::count(onKeys.begin(), onKeys.end(), a.key) < 2)
            fail("re-covering a note did not re-audition it");
        const std::vector<SongView::NoteId> &sel = view.selection();
        if (sel.size() < 2
            || std::find(sel.begin(), sel.end(),
                         SongView::NoteId{uint32_t(a.tick), uint8_t(a.key)})
                   == sel.end()
            || std::find(sel.begin(), sel.end(),
                         SongView::NoteId{uint32_t(b.tick), uint8_t(b.key)})
                   == sel.end())
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

    // Edge resize snaps to the ruler's absolute grid, not to grid-sized
    // offsets from the note's own end: give a note an off-grid duration
    // (1.25 cells) behind the view's back, drag its right edge to 1.75
    // cells, and the end must land on the 2-cell grid line — not at
    // 1.25 + 1 cells, which is where delta-snapping would put it.
    const Cell d = findFreeCell();
    if (d.key < 0) {
        fail("no free grid cell for the off-grid resize");
        return failures;
    }
    const uint32_t offDur = uint32_t(d.dur + d.dur / 4);
    doc.addNote(track, d.tick, uint8_t(d.key), offDur, 100);
    const int rowY = (127 - d.key) * keyH - view.scrollY() + keyH / 2 + 1;
    const QPoint edge(songview::kKeyboardW + view.contentX(double(d.tick + offDur)),
                      rowY);
    const QPoint leftEdge(songview::kKeyboardW + view.contentX(double(d.tick)), rowY);
    sendMouse(roll, QEvent::MouseMove, leftEdge, Qt::NoButton, Qt::NoButton);
    const QPixmap expectedLeftCursor =
        QIcon(QStringLiteral(":/cursors/left-drag.png"))
            .pixmap(QSize(24, 24), roll->devicePixelRatioF());
    if (roll->cursor().pixmap().devicePixelRatio()
            != expectedLeftCursor.devicePixelRatio()
        || roll->cursor().pixmap().toImage() != expectedLeftCursor.toImage())
        fail("left note edge did not show its DPI-matched custom cursor");
    sendMouse(roll, QEvent::MouseMove, edge, Qt::NoButton, Qt::NoButton);
    const QPixmap expectedRightCursor =
        QIcon(QStringLiteral(":/cursors/right-drag.png"))
            .pixmap(QSize(24, 24), roll->devicePixelRatioF());
    if (roll->cursor().pixmap().devicePixelRatio()
            != expectedRightCursor.devicePixelRatio()
        || roll->cursor().pixmap().toImage() != expectedRightCursor.toImage())
        fail("right note edge did not show its DPI-matched custom cursor");
    const QPoint pull(
        songview::kKeyboardW + view.contentX(double(d.tick) + 1.75 * double(d.dur)),
        rowY);
    sendMouse(roll, QEvent::MouseButtonPress, edge, Qt::LeftButton, Qt::LeftButton);
    sendMouse(roll, QEvent::MouseMove, pull, Qt::NoButton, Qt::LeftButton);
    sendMouse(roll, QEvent::MouseButtonRelease, pull, Qt::LeftButton, Qt::NoButton);
    DocNote resized;
    if (!doc.findNote(track, d.tick, uint8_t(d.key), &resized)
        || resized.duration != 2 * d.dur)
        fail("off-grid right-edge drag did not snap the end to the ruler grid");

    // Overshooting the drag past the note's start must stop at one grid
    // cell, not collapse to the document's 1-tick floor.
    const QPoint edge2(
        songview::kKeyboardW + view.contentX(double(d.tick + 2 * d.dur)), rowY);
    const QPoint overshoot(
        songview::kKeyboardW + view.contentX(double(d.tick) - 0.5 * double(d.dur)),
        rowY);
    sendMouse(roll, QEvent::MouseButtonPress, edge2, Qt::LeftButton, Qt::LeftButton);
    sendMouse(roll, QEvent::MouseMove, overshoot, Qt::NoButton, Qt::LeftButton);
    sendMouse(roll, QEvent::MouseButtonRelease, overshoot, Qt::LeftButton,
              Qt::NoButton);
    DocNote collapsed;
    if (!doc.findNote(track, d.tick, uint8_t(d.key), &collapsed)
        || collapsed.duration != d.dur)
        fail("overshot right-edge drag did not stop at one grid cell");

    // Keyboard transpose/nudge on note D (clicking it selects it):
    // Ctrl+Up is a semitone, Ctrl+Shift+Down an octave, and Ctrl+Right
    // moves one grid cell from an on-grid start.
    const QPoint dCenter(songview::kKeyboardW
                             + view.contentX(double(d.tick) + 0.5 * double(d.dur)),
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
    if (!doc.findNote(track, d.tick + d.dur, uint8_t(d.key - 11), &transposed))
        fail("Ctrl+Right did not nudge one grid cell right");
    // An off-grid selection nudges onto the grid line, not by a whole
    // cell: push the note a quarter cell right behind the view's back
    // (reselecting — the selection keys on the start tick, which moved),
    // and Ctrl+Left must bring it back to the line it left.
    doc.moveNotes({transposed}, int64_t(d.dur / 4), 0);
    view.setSelection(
        {{uint32_t(d.tick + d.dur + d.dur / 4), uint8_t(d.key - 11)}});
    sendKey(roll, Qt::Key_Left, Qt::ControlModifier);
    if (!doc.findNote(track, d.tick + d.dur, uint8_t(d.key - 11), &transposed))
        fail("Ctrl+Left did not snap the off-grid note back to the grid");

    // Keyboard moves keep the notes in view, scrolling just enough rather
    // than re-anchoring. Vertical: park the note's row above the viewport,
    // and Ctrl+Up must land it flush at the top edge.
    const int keyNow = d.key - 11;
    view.scrollRollBy((129 - keyNow) * keyH - view.scrollY());
    if ((128 - keyNow) * keyH - view.scrollY() > 0)
        fail("could not park the note's row above the viewport");
    sendKey(roll, Qt::Key_Up, Qt::ControlModifier);
    if (view.scrollY() != (126 - keyNow) * keyH)
        fail("Ctrl+Up above the viewport did not scroll the row flush to the top");
    sendKey(roll, Qt::Key_Down, Qt::ControlModifier); // undo the extra semitone

    // Horizontal: park the note past the left edge; nudging right must
    // bring its start flush to the left edge (minimal scroll, not the
    // paste jump). Then ride it right across the viewport: once the end
    // crosses the right edge, it must stay flush there.
    uint64_t nStart = d.tick + d.dur;
    view.scrollByPx(view.contentX(double(nStart + d.dur)) + 40);
    if (view.contentX(double(nStart + d.dur)) >= 0)
        fail("could not park the note past the left edge");
    sendKey(roll, Qt::Key_Right, Qt::ControlModifier);
    nStart += d.dur;
    if (view.contentX(double(nStart)) != 0)
        fail("Ctrl+Right off-screen-left did not scroll the start flush to the left edge");
    const int vw = std::max(50, roll->width() - songview::kKeyboardW);
    const int cellPx =
        view.contentX(double(nStart + d.dur)) - view.contentX(double(nStart));
    const int rides = (vw - view.contentX(double(nStart + d.dur))) / cellPx + 2;
    for (int i = 0; i < rides; i++)
        sendKey(roll, Qt::Key_Right, Qt::ControlModifier);
    nStart += uint64_t(rides) * d.dur;
    if (view.contentX(double(nStart + d.dur)) != vw - 1)
        fail("riding the nudge right did not keep the note's end at the right edge");
    // Ride back home so the time-selection checks below find the note
    // where they expect it; every press so far merges into one command.
    for (int i = 0; i < rides + 1; i++)
        sendKey(roll, Qt::Key_Left, Qt::ControlModifier);
    if (!doc.findNote(track, d.tick + d.dur, uint8_t(d.key - 11), &transposed))
        fail("the ride right and back did not return the note home");

    // Consecutive keyboard presses on the same notes merge into one undo
    // command; mark a save point so the time-selection presses below get
    // their own commands (merges never cross the stack's clean index).
    doc.undoStack()->setClean();

    // The same shortcuts on a time selection (no notes selected): the band
    // over the note's cell transposes every covered note of the scoped
    // tracks, and a nudge moves the contents with the band following.
    SongView::TimeSelection band;
    band.startTick = d.tick + d.dur;
    band.endTick = d.tick + 2 * d.dur;
    view.setTimeSelection(band);
    sendKey(roll, Qt::Key_Up, Qt::ControlModifier);
    if (!doc.findNote(track, d.tick + d.dur, uint8_t(d.key - 10), &transposed))
        fail("time-selection Ctrl+Up did not transpose the covered note");
    sendKey(roll, Qt::Key_Right, Qt::ControlModifier);
    if (!doc.findNote(track, d.tick + 2 * d.dur, uint8_t(d.key - 10), &transposed))
        fail("time-selection Ctrl+Right did not nudge the covered note");
    if (view.timeSelection().startTick != d.tick + 2 * d.dur)
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
            if (doc.trackName(track) != QStringLiteral("Rolled")
                || doc.undoStack()->count() != commands)
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
                &view, &SongView::revealVoiceRequested,
                [&](int program) { revealed = program; reveals++; });
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
            sendMouse(row, QEvent::MouseMove, voicePos + QPoint(0, 25),
                      Qt::NoButton, Qt::LeftButton);
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
            QObject::connect(&poll, &QTimer::timeout, [&] {
                if (QDialog *dlg = view.findChild<QDialog *>()) {
                    pickerSeen = true;
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
                same = movedNotes[i].tick == firstNotes[i].tick
                    && movedNotes[i].key == firstNotes[i].key;
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
            if (sel.size() != 1
                || !(sel[0] == SongView::NoteId{target.startTick, target.key}))
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
                if (view.revealNote(target.track, uint8_t(freeKey),
                                    target.startTick))
                    fail("revealNote found a note on an unused key");
                if (view.selectedTrack() != int(target.track))
                    fail("revealNote miss dropped the track selection");
            }
        }
    }

    // Sixteen commands: draw, set, draw, nudge, draw, the double-click
    // delete, add, two resizes, the three note-selection presses MERGED
    // into one, the off-grid behind-the-back move, Ctrl+Left (all the
    // scroll-follow presses merge into it), two time-selection moves
    // (kept separate by the clean-index save point), the inline rename,
    // and the mid-song voice change — plus, when the song has a second
    // track, the header-drag track move and the editor commit the drop
    // flushes. Undoing them all must restore the original bytes.
    int undos = 0;
    while (doc.undoStack()->canUndo() && undos < 100) {
        doc.undoStack()->undo();
        undos++;
    }
    if (undos != 16 + (reordered ? (dragRenamed ? 2 : 1) : 0))
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
