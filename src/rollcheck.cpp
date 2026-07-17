#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPoint>
#include <QString>
#include <QWidget>
#include <cstdio>

#include "core/songdocument.h"
#include "project/decompproject.h"
#include "ui/songview.h"

// --rollcheck <projectRoot> <song> [shot.png]: piano-roll gesture check.
// Drives the roll widget offscreen with synthesized mouse events: the
// double-click pencil draws at the default velocity (100 on a fresh
// document), and the Reaper-style latch makes the last clicked or
// velocity-dragged note's velocity the default for the next drawn note,
// and an edge resize snaps to the ruler's absolute grid even when the
// note's own edge sits off-grid. Ctrl+arrows transpose (Shift: octave)
// and nudge the selection along the same absolute grid — both the roll's
// note selection and a multi-track time selection — and the view follows
// notes moved out of sight with a minimal scroll (flush at the edge, not
// re-centered). The playhead follow-scroll pauses while a mouse gesture
// is held (pan, drag, sweep) and resumes on release. Undoing every
// gesture must restore the original bytes.

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

    const QImage image = view.grab().toImage();
    if (image.isNull())
        fail("offscreen render produced no image");
    if (!screenshotPath.isEmpty()) {
        image.save(screenshotPath);
        std::printf("rollcheck: wrote %s\n", qUtf8Printable(screenshotPath));
    }

    // Fourteen commands: draw, set, draw, nudge, draw, add, two resizes,
    // the three note-selection presses MERGED into one, the off-grid
    // behind-the-back move, Ctrl+Left (all the scroll-follow presses
    // merge into it), two time-selection moves (kept separate by the
    // clean-index save point), and the inline rename. Undoing them all
    // must restore the original bytes.
    int undos = 0;
    while (doc.undoStack()->canUndo() && undos < 100) {
        doc.undoStack()->undo();
        undos++;
    }
    if (undos != 14)
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
