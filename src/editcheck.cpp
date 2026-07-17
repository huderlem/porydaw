#include <QElapsedTimer>
#include <QString>
#include <cstdio>

#include "core/songdocument.h"
#include "project/decompproject.h"

// --editcheck <projectRoot>: M2 undo-integrity check. For every song with a
// MIDI source, performs a scripted pass over every edit-operation type, then
// verifies that undoing everything restores the SMF byte-for-byte, that redo
// reproduces the edited state deterministically, and that event ticks stay
// sorted after each mutation. Complements --roundtrip (which proves the
// unedited writer) by proving the editing layer can always get back to that
// pristine state.

namespace {

bool tracksSorted(const SmfFile &smf)
{
    for (const SmfTrack &track : smf.tracks) {
        for (size_t i = 1; i < track.events.size(); i++) {
            if (track.events[i].tick < track.events[i - 1].tick)
                return false;
        }
    }
    return true;
}

} // namespace

int runEditCheck(const QString &projectRoot)
{
    DecompProject project;
    QString error;
    if (!project.open(projectRoot, &error)) {
        std::fprintf(stderr, "editcheck: %s\n", qUtf8Printable(error));
        return 1;
    }

    QElapsedTimer timer;
    timer.start();

    int checked = 0, failures = 0;
    for (const SongInfo &song : project.songs()) {
        if (!song.isPlayable())
            continue;

        SongDocument doc;
        if (!doc.load(song, &error)) {
            std::fprintf(stderr, "editcheck: FAIL %s: %s\n", qUtf8Printable(song.label),
                         qUtf8Printable(error));
            failures++;
            continue;
        }
        const QByteArray baseline = doc.smf().write();

        // Pick a track that has notes to edit on.
        int track = -1;
        for (int t = 0; t < doc.engineTrackCount(); t++) {
            if (!doc.notesForTrack(t).empty()) {
                track = t;
                break;
            }
        }

        auto fail = [&](const char *what) {
            std::fprintf(stderr, "editcheck: FAIL %s: %s\n", qUtf8Printable(song.label), what);
            failures++;
        };

        const uint32_t step = doc.ticksPerClock();
        // Edit far past the end of the song so scripted notes and lane points
        // can't collide with (or re-pair against) the song's real content.
        uint64_t base = 0;
        for (const SmfTrack &tr : doc.smf().tracks)
            base = std::max(base, tr.endTick);
        base += step * 100;
        bool ok = true;
        auto mutateAndCheck = [&](const char *what) {
            if (ok && !tracksSorted(doc.smf())) {
                fail(what);
                ok = false;
            }
        };

        if (track >= 0) {
            // Note ops: add, move, resize, re-velocity, delete.
            doc.addNote(track, base, 60, step * 4, 100);
            mutateAndCheck("events unsorted after addNote");
            DocNote note;
            if (ok && !doc.findNote(track, base, 60, &note)) {
                fail("added note not found");
                ok = false;
            }
            if (ok) {
                doc.moveNotes({note}, int64_t(step) * 8, 3);
                mutateAndCheck("events unsorted after moveNotes");
            }
            if (ok && !doc.findNote(track, base + step * 8, 63, &note)) {
                fail("moved note not found");
                ok = false;
            }
            if (ok) {
                doc.resizeNotes({note}, int64_t(step) * 2);
                mutateAndCheck("events unsorted after resizeNotes");
                if (!doc.findNote(track, base + step * 8, 63, &note)
                    || note.duration != step * 6) {
                    fail("resize produced wrong duration");
                    ok = false;
                }
            }
            if (ok) {
                // Left resize: the note-on moves, the note-off stays pinned.
                doc.resizeNotesLeft({note}, -int64_t(step) * 2);
                mutateAndCheck("events unsorted after resizeNotesLeft");
                if (!doc.findNote(track, base + step * 6, 63, &note)
                    || note.duration != step * 8) {
                    fail("left resize produced wrong start/duration");
                    ok = false;
                }
            }
            if (ok) {
                // Dragging the note-on past the note-off clamps to 1 tick left.
                doc.resizeNotesLeft({note}, int64_t(step) * 100);
                mutateAndCheck("events unsorted after clamped resizeNotesLeft");
                if (!doc.findNote(track, base + step * 14 - 1, 63, &note)
                    || note.duration != 1) {
                    fail("left resize not clamped at the note-off");
                    ok = false;
                } else {
                    doc.resizeNotesLeft({note}, -int64_t(step) * 8 + 1);
                    if (!doc.findNote(track, base + step * 6, 63, &note)
                        || note.duration != step * 8) {
                        fail("left resize could not restore the note");
                        ok = false;
                    }
                }
            }
            if (ok) {
                doc.setNotesVelocity({note}, 88);
                if (!doc.findNote(track, base + step * 6, 63, &note) || note.velocity != 88) {
                    fail("velocity edit not applied");
                    ok = false;
                }
            }
            if (ok) {
                doc.nudgeNotesVelocity({note}, -30);
                if (!doc.findNote(track, base + step * 6, 63, &note) || note.velocity != 58) {
                    fail("velocity nudge not applied");
                    ok = false;
                }
            }
            if (ok) {
                doc.nudgeNotesVelocity({note}, 200); // must clamp to 127
                if (!doc.findNote(track, base + step * 6, 63, &note) || note.velocity != 127) {
                    fail("velocity nudge not clamped");
                    ok = false;
                }
            }
            if (ok)
                doc.deleteNotes({note});

            // Batch add (clipboard paste): both notes in one undoable command.
            if (ok) {
                doc.addNotes(track, {{base + step * 20, 64, step * 2, 96},
                                     {base + step * 22, 67, step * 2, 96}});
                mutateAndCheck("events unsorted after addNotes");
                DocNote a, b;
                if (!doc.findNote(track, base + step * 20, 64, &a)
                    || !doc.findNote(track, base + step * 22, 67, &b)) {
                    fail("batch-added notes not found");
                    ok = false;
                } else {
                    doc.undoStack()->undo();
                    if (doc.findNote(track, base + step * 20, 64, &a)
                        || doc.findNote(track, base + step * 22, 67, &b)) {
                        fail("addNotes was not a single undo command");
                        ok = false;
                    } else {
                        doc.undoStack()->redo();
                    }
                }
            }

            // Abutting same-pitch notes, written right-to-left: the left
            // note's end lands at the right note's on tick, and must be
            // ordered before it — pairing (here and in mid2agb) gives every
            // note-on the first same-key end after it, so an end placed
            // after a same-tick note-on makes the left note swallow the
            // right one and orphans the real end when the pair is deleted.
            if (ok) {
                const uint64_t seam = base + step * 72;
                doc.addNote(track, seam, 60, step * 2, 100);
                doc.addNote(track, seam - step * 2, 60, step * 2, 100);
                mutateAndCheck("events unsorted after abutting addNote");
                DocNote leftNote, rightNote;
                if (!doc.findNote(track, seam - step * 2, 60, &leftNote)
                    || !doc.findNote(track, seam, 60, &rightNote)
                    || leftNote.duration != step * 2 || rightNote.duration != step * 2
                    || leftNote.endIndex == rightNote.endIndex) {
                    fail("abutting notes mis-paired (note end after same-tick note-on)");
                    ok = false;
                } else {
                    doc.deleteNotes({leftNote, rightNote});
                    bool leftover = false;
                    for (const SmfEvent &ev :
                         doc.smf().tracks[size_t(leftNote.smfTrack)].events) {
                        leftover |= ev.tick >= seam - step * 2 && ev.isChannel()
                            && (ev.isNoteOn() || ev.isNoteEnd());
                    }
                    if (leftover) {
                        fail("deleting abutting notes left a note event behind");
                        ok = false;
                    }
                }
            }

            // Range edit: a multi-track/multi-lane batch of removals and
            // insertions must land as ONE undoable command.
            if (ok) {
                doc.addNotes(track, {{base + step * 30, 60, step * 2, 90},
                                     {base + step * 32, 62, step * 2, 90}});
                doc.addLanePoint(track, 7, base + step * 30, 80);
                doc.addLanePoint(track, DOC_CC_TEMPO, base + step * 31, 140);
                SongDocument::RangeEdit edit;
                for (const DocNote &n : doc.notesForTrack(track)) {
                    if (n.tick >= base + step * 30 && n.tick < base + step * 34)
                        edit.removeNotes.push_back(n);
                }
                for (const DocLanePoint &p : doc.lanePoints(track, 7)) {
                    if (p.tick == base + step * 30)
                        edit.removePoints.push_back(p);
                }
                for (const DocLanePoint &p : doc.lanePoints(track, DOC_CC_TEMPO)) {
                    if (p.tick == base + step * 31)
                        edit.removePoints.push_back(p);
                }
                edit.addNotes.push_back({track, {{base + step * 40, 65, step * 2, 90}}});
                edit.addPoints.push_back({track, 7, {{base + step * 40, 70}}});
                edit.addPoints.push_back({-1, DOC_CC_TEMPO, {{base + step * 41, 155}}});
                doc.applyRangeEdit(QStringLiteral("range edit"), edit);
                mutateAndCheck("events unsorted after applyRangeEdit");
                DocNote n;
                DocLanePoint p;
                if (doc.findNote(track, base + step * 30, 60, &n)
                    || doc.findNote(track, base + step * 32, 62, &n)
                    || !doc.findNote(track, base + step * 40, 65, &n)
                    || !doc.findLanePoint(track, 7, base + step * 40, &p) || p.value != 70
                    || !doc.findLanePoint(track, DOC_CC_TEMPO, base + step * 41, &p)
                    || p.value != 155) {
                    fail("range edit produced wrong content");
                    ok = false;
                } else {
                    doc.undoStack()->undo();
                    if (!doc.findNote(track, base + step * 30, 60, &n)
                        || doc.findNote(track, base + step * 40, 65, &n)) {
                        fail("applyRangeEdit was not a single undo command");
                        ok = false;
                    } else {
                        doc.undoStack()->redo();
                    }
                }
            }

            // Range move (time-selection nudge): notes plus CC and tempo
            // points shift together by a tick delta as ONE undoable command,
            // with values intact (events move as raw bytes).
            if (ok) {
                doc.addNotes(track, {{base + step * 80, 60, step * 2, 90},
                                     {base + step * 82, 64, step * 2, 90}});
                doc.addLanePoint(track, 7, base + step * 80, 45);
                doc.addLanePoint(track, DOC_CC_TEMPO, base + step * 81, 140);
                std::vector<DocNote> moveNotes;
                for (const DocNote &n : doc.notesForTrack(track)) {
                    if (n.tick >= base + step * 80 && n.tick < base + step * 84)
                        moveNotes.push_back(n);
                }
                std::vector<DocLanePoint> movePoints;
                for (const DocLanePoint &p : doc.lanePoints(track, 7)) {
                    if (p.tick == base + step * 80)
                        movePoints.push_back(p);
                }
                for (const DocLanePoint &p : doc.lanePoints(track, DOC_CC_TEMPO)) {
                    if (p.tick == base + step * 81)
                        movePoints.push_back(p);
                }
                doc.moveRange(moveNotes, movePoints, step * 3);
                mutateAndCheck("events unsorted after moveRange");
                DocNote n;
                DocLanePoint p;
                if (doc.findNote(track, base + step * 80, 60, &n)
                    || !doc.findNote(track, base + step * 83, 60, &n)
                    || n.duration != step * 2
                    || !doc.findNote(track, base + step * 85, 64, &n)
                    || !doc.findLanePoint(track, 7, base + step * 83, &p)
                    || p.value != 45
                    || !doc.findLanePoint(track, DOC_CC_TEMPO, base + step * 84, &p)
                    || p.value != 140) {
                    fail("range move produced wrong content");
                    ok = false;
                }
                if (ok) {
                    doc.moveRange(moveNotes, movePoints, 0); // no-op guard
                    doc.undoStack()->undo();
                    if (!doc.findNote(track, base + step * 80, 60, &n)
                        || doc.findNote(track, base + step * 83, 60, &n)
                        || !doc.findLanePoint(track, 7, base + step * 80, &p)) {
                        fail("moveRange was not a single undo command");
                        ok = false;
                    } else {
                        doc.undoStack()->redo();
                    }
                }
            }

            // Same-key overlap resolution: a written note landing on another
            // note's span trims it (head or tail kept) or removes it when
            // fully covered, in the same undo command — the pairing rule
            // (first same-key end after the on) cannot represent an overlap,
            // which used to silently re-pair the stationary note's end onto
            // the edited note's.
            if (ok) {
                doc.addNote(track, base + step * 90, 71, step * 4, 100); // S 90..94
                doc.addNote(track, base + step * 88, 70, step * 4, 100); // M 88..92
                DocNote m, s;
                // Tail kept: M transposed up onto S's head.
                if (!doc.findNote(track, base + step * 88, 70, &m)) {
                    fail("overlap-scenario notes not found");
                    ok = false;
                } else {
                    doc.moveNotes({m}, 0, 1);
                    mutateAndCheck("events unsorted after overlap transpose");
                    if (!doc.findNote(track, base + step * 88, 71, &m)
                        || m.duration != step * 4
                        || !doc.findNote(track, base + step * 92, 71, &s)
                        || s.duration != step * 2) {
                        fail("transpose onto a note's head did not keep its tail");
                        ok = false;
                    }
                }
                // Fully covered: M resized right across all of S removes it.
                if (ok) {
                    doc.resizeNotes({m}, step * 4); // M 88..96 covers S 92..94
                    mutateAndCheck("events unsorted after overlap resize");
                    if (!doc.findNote(track, base + step * 88, 71, &m)
                        || m.duration != step * 8
                        || doc.findNote(track, base + step * 92, 71, &s)) {
                        fail("resize across a covered note did not remove it");
                        ok = false;
                    }
                }
                // Head kept: a note drawn over M's tail trims M back, and one
                // undo reverts the trim together with the add.
                if (ok) {
                    doc.addNote(track, base + step * 94, 71, step * 4, 100);
                    mutateAndCheck("events unsorted after overlapping addNote");
                    if (!doc.findNote(track, base + step * 88, 71, &m)
                        || m.duration != step * 6
                        || !doc.findNote(track, base + step * 94, 71, &s)
                        || s.duration != step * 4) {
                        fail("overlapping add did not trim the covered tail");
                        ok = false;
                    }
                }
                if (ok) {
                    doc.undoStack()->undo();
                    if (!doc.findNote(track, base + step * 88, 71, &m)
                        || m.duration != step * 8
                        || doc.findNote(track, base + step * 94, 71, &s)) {
                        fail("overlap trim was not part of the edit's own undo");
                        ok = false;
                    } else {
                        doc.undoStack()->redo();
                    }
                }
            }

            // Mergeable moves (keyboard transpose/nudge): consecutive
            // mergeable moveNotes of the same notes collapse into ONE undo
            // command that re-lands from the gesture's start — a neighbor
            // trimmed by a merely-passed-through overlap comes back — and
            // the merge stops at the stack's clean index (a save between
            // presses keeps its own command).
            if (ok) {
                doc.addNote(track, base + step * 100, 70, step * 4, 100); // S
                doc.addNote(track, base + step * 100, 69, step * 2, 100); // M
                const int countBefore = doc.undoStack()->count();
                DocNote m, s;
                if (!doc.findNote(track, base + step * 100, 69, &m)) {
                    fail("merge-scenario notes not found");
                    ok = false;
                }
                if (ok) {
                    doc.moveNotes({m}, 0, 1, true); // M onto S: S trimmed
                    if (!doc.findNote(track, base + step * 102, 70, &s)
                        || s.duration != step * 2) {
                        fail("mergeable transpose did not trim the overlap");
                        ok = false;
                    }
                }
                if (ok) {
                    doc.findNote(track, base + step * 100, 70, &m); // re-resolve
                    doc.moveNotes({m}, 0, 1, true); // past S: merged, +2 total
                    if (doc.undoStack()->count() != countBefore + 1) {
                        fail("consecutive mergeable moves did not merge");
                        ok = false;
                    } else if (!doc.findNote(track, base + step * 100, 71, &m)
                               || m.duration != step * 2
                               || !doc.findNote(track, base + step * 100, 70, &s)
                               || s.duration != step * 4) {
                        fail("merged transpose did not restore the trimmed note");
                        ok = false;
                    }
                }
                if (ok) {
                    doc.undoStack()->undo();
                    if (!doc.findNote(track, base + step * 100, 69, &m)
                        || !doc.findNote(track, base + step * 100, 70, &s)
                        || s.duration != step * 4) {
                        fail("merged move undo did not restore the gesture start");
                        ok = false;
                    } else {
                        doc.undoStack()->redo();
                    }
                }
                if (ok) {
                    doc.findNote(track, base + step * 100, 71, &m);
                    doc.undoStack()->setClean();
                    doc.moveNotes({m}, 0, 1, true);
                    if (doc.undoStack()->count() != countBefore + 2) {
                        fail("mergeable move merged across the clean index");
                        ok = false;
                    }
                }
            }

            // Ripple remove (removeTimeRange): in-range content vanishes,
            // later events shift left by the span, and the last in-range
            // automation point survives at the seam. ONE undoable command.
            if (ok) {
                doc.addNotes(track, {{base + step * 50, 60, step, 90},
                                     {base + step * 52, 62, step, 90},
                                     {base + step * 56, 64, step, 90}});
                doc.addLanePoint(track, 7, base + step * 51, 30);
                doc.addLanePoint(track, 7, base + step * 52, 40);
                SongDocument::RippleScope scope;
                scope.tracks = {track};
                if (!doc.removeTimeRange(base + step * 51, base + step * 54, scope)) {
                    fail("removeTimeRange reported nothing to do");
                    ok = false;
                }
                mutateAndCheck("events unsorted after removeTimeRange");
                DocNote n;
                DocLanePoint p;
                if (ok
                    && (!doc.findNote(track, base + step * 50, 60, &n)
                        || doc.findNote(track, base + step * 52, 62, &n)
                        || !doc.findNote(track, base + step * 53, 64, &n)
                        || !doc.findLanePoint(track, 7, base + step * 51, &p)
                        || p.value != 40)) {
                    fail("ripple remove produced wrong content");
                    ok = false;
                }
                if (ok) {
                    doc.undoStack()->undo();
                    if (!doc.findNote(track, base + step * 56, 64, &n)
                        || !doc.findLanePoint(track, 7, base + step * 52, &p)
                        || p.value != 40) {
                        fail("removeTimeRange was not a single undo command");
                        ok = false;
                    } else {
                        doc.undoStack()->redo();
                    }
                }
            }

            // Whole-song ripple: the globals travel too — a time signature
            // and a tempo change inside the range survive at the seam, later
            // notes shift, loop markers before the range stay put, and the
            // end-of-track ticks close the gap so the song gets shorter.
            if (ok) {
                const auto maxEnd = [&doc] {
                    uint64_t end = 0;
                    for (const SmfTrack &tr : doc.smf().tracks)
                        end = std::max(end, tr.endTick);
                    return end;
                };
                doc.setTimeSig(base + step * 62, 3, 2);
                doc.addLanePoint(track, DOC_CC_TEMPO, base + step * 63, 150);
                doc.addNotes(track, {{base + step * 66, 65, step, 90}});
                const uint64_t endBefore = maxEnd();
                const uint64_t loopStartBefore = doc.loopTick(false);
                SongDocument::RippleScope scope;
                scope.wholeSong = true;
                if (!doc.removeTimeRange(base + step * 61, base + step * 65, scope)) {
                    fail("whole-song removeTimeRange reported nothing to do");
                    ok = false;
                }
                mutateAndCheck("events unsorted after whole-song removeTimeRange");
                DocNote n;
                DocLanePoint p;
                bool sigAtSeam = false;
                for (const DocTimeSig &sig : doc.timeSigs()) {
                    if (sig.tick == base + step * 61 && sig.numerator == 3)
                        sigAtSeam = true;
                }
                if (ok
                    && (!sigAtSeam
                        || !doc.findLanePoint(track, DOC_CC_TEMPO, base + step * 61, &p)
                        || p.value != 150
                        || !doc.findNote(track, base + step * 62, 65, &n)
                        || maxEnd() != endBefore - step * 4
                        || doc.loopTick(false) != loopStartBefore)) {
                    fail("whole-song ripple produced wrong content");
                    ok = false;
                }
                if (ok) {
                    doc.undoStack()->undo();
                    if (!doc.findNote(track, base + step * 66, 65, &n)
                        || maxEnd() != endBefore) {
                        fail("whole-song removeTimeRange was not a single undo command");
                        ok = false;
                    } else {
                        doc.undoStack()->redo();
                    }
                }
            }

            // Voice ops: add, value-only modify (must not reorder within the
            // tick), move to a new tick, delete.
            if (ok) {
                doc.addLanePoint(track, DOC_CC_VOICE, base + step, 5);
                mutateAndCheck("events unsorted after voice add");
                DocLanePoint vc;
                if (!doc.findLanePoint(track, DOC_CC_VOICE, base + step, &vc)
                    || vc.value != 5) {
                    fail("voice change not found after add");
                    ok = false;
                } else {
                    doc.moveLanePoint(track, DOC_CC_VOICE, vc, vc.tick, 9);
                    if (!doc.findLanePoint(track, DOC_CC_VOICE, base + step, &vc)
                        || vc.value != 9) {
                        fail("voice value edit not applied");
                        ok = false;
                    } else {
                        doc.moveLanePoint(track, DOC_CC_VOICE, vc, base + step * 6, 9);
                        mutateAndCheck("events unsorted after voice move");
                        if (!doc.findLanePoint(track, DOC_CC_VOICE, base + step * 6, &vc)) {
                            fail("voice change not found after move");
                            ok = false;
                        } else {
                            doc.deleteLanePoints(track, DOC_CC_VOICE, {vc});
                        }
                    }
                }
            }

            // Automation ops on the volume lane, plus tempo and pitch bend.
            if (ok) {
                doc.addLanePoint(track, 7, base + step * 2, 100);
                doc.addLanePoint(track, DOC_CC_BEND, base + step * 3, -1024);
                doc.addLanePoint(track, DOC_CC_TEMPO, base + step * 4, 150);
                mutateAndCheck("events unsorted after addLanePoint");
                DocLanePoint pt;
                if (!doc.findLanePoint(track, 7, base + step * 2, &pt) || pt.value != 100) {
                    fail("lane point not found after add");
                    ok = false;
                } else {
                    doc.moveLanePoint(track, 7, pt, base + step * 5, 90);
                    mutateAndCheck("events unsorted after moveLanePoint");
                    if (!doc.findLanePoint(track, 7, base + step * 5, &pt) || pt.value != 90) {
                        fail("lane point not found after move");
                        ok = false;
                    } else {
                        std::vector<DocLanePoint> doomed{pt};
                        DocLanePoint bendPt, tempoPt;
                        if (doc.findLanePoint(track, DOC_CC_BEND, base + step * 3, &bendPt))
                            doc.deleteLanePoints(track, DOC_CC_BEND, {bendPt});
                        if (doc.findLanePoint(track, DOC_CC_TEMPO, base + step * 4, &tempoPt))
                            doc.deleteLanePoints(track, DOC_CC_TEMPO, {tempoPt});
                        // Re-resolve: the deletes above shifted indices.
                        if (doc.findLanePoint(track, 7, base + step * 5, &pt))
                            doc.deleteLanePoints(track, 7, {pt});
                    }
                }
            }
        }

        // Track ops: create a track (seeded with its voice), edit on it,
        // delete it again.
        if (ok && doc.canAddTrack()) {
            const int newTrack = doc.addTrack(7);
            if (newTrack < 0) {
                fail("addTrack returned no track with canAddTrack true");
                ok = false;
            } else {
                mutateAndCheck("events unsorted after addTrack");
                const auto seed = doc.lanePoints(newTrack, DOC_CC_VOICE);
                if (ok && (seed.empty() || seed.front().tick != 0 || seed.front().value != 7)) {
                    fail("new track missing its seed voice");
                    ok = false;
                }
                DocNote note;
                if (ok) {
                    doc.addNote(newTrack, base, 72, step * 4, 100);
                    if (!doc.findNote(newTrack, base, 72, &note)) {
                        fail("note on new track not found");
                        ok = false;
                    }
                }
                if (ok) {
                    doc.deleteTrack(newTrack);
                    mutateAndCheck("events unsorted after deleteTrack");
                    if (doc.findNote(newTrack, base, 72, &note)) {
                        fail("deleted track still has its note");
                        ok = false;
                    }
                }
            }
        }

        // Duplicating a song track: the copy lands on a fresh engine slot
        // carrying the same notes as the source.
        if (ok && track >= 0 && doc.canAddTrack()) {
            const auto srcNotes = doc.notesForTrack(track);
            const int copy = doc.duplicateTrack(track);
            mutateAndCheck("events unsorted after duplicateTrack");
            if (copy < 0) {
                fail("duplicateTrack returned no track with canAddTrack true");
                ok = false;
            } else if (copy == track) {
                fail("duplicateTrack returned the source track");
                ok = false;
            } else if (ok) {
                const auto copyNotes = doc.notesForTrack(copy);
                bool same = copyNotes.size() == srcNotes.size();
                for (size_t i = 0; same && i < copyNotes.size(); i++) {
                    same = copyNotes[i].tick == srcNotes[i].tick
                        && copyNotes[i].key == srcNotes[i].key
                        && copyNotes[i].duration == srcNotes[i].duration
                        && copyNotes[i].velocity == srcNotes[i].velocity;
                }
                if (!same) {
                    fail("duplicated track's notes differ from the source");
                    ok = false;
                } else {
                    doc.deleteTrack(copy);
                    mutateAndCheck("events unsorted after deleting the duplicate");
                }
            }
        }

        // Reordering tracks (format 1): the chunk moves with its events and
        // channel bytes untouched, and the seq globals — tempo, time
        // signatures, loop markers — stay with chunk 0 even when the move
        // displaces it (mid2agb and the tempo lane read them only there).
        if (ok && doc.smf().format != 0 && doc.engineTrackCount() >= 2 && track >= 0) {
            doc.addLanePoint(track, DOC_CC_TEMPO, base + step * 110, 145);
            doc.setTimeSig(base + step * 112, 5, 2);
            const uint64_t loopStartBefore = doc.loopTick(false);
            const uint64_t loopEndBefore = doc.loopTick(true);
            const auto srcNotes = doc.notesForTrack(0);
            const uint8_t srcChannel = doc.channelFor(0);
            const int last = doc.engineTrackCount() - 1;
            const int countBefore = doc.undoStack()->count();
            doc.moveTrack(0, 0); // no-op guard
            if (doc.undoStack()->count() != countBefore) {
                fail("moveTrack onto itself pushed a command");
                ok = false;
            }
            auto seqChunkHas = [&doc](uint8_t metaType, uint64_t tick) {
                for (const SmfEvent &ev : doc.smf().tracks[0].events) {
                    if (ev.isMeta() && ev.metaType == metaType && ev.tick == tick)
                        return true;
                }
                return false;
            };
            auto notesMatch = [&doc](int engineTrack,
                                     const std::vector<DocNote> &want) {
                const auto got = doc.notesForTrack(engineTrack);
                if (got.size() != want.size())
                    return false;
                for (size_t i = 0; i < got.size(); i++) {
                    if (got[i].tick != want[i].tick || got[i].key != want[i].key
                        || got[i].duration != want[i].duration
                        || got[i].velocity != want[i].velocity)
                        return false;
                }
                return true;
            };
            if (ok) {
                doc.moveTrack(0, last);
                mutateAndCheck("events unsorted after moveTrack");
            }
            if (ok && doc.undoStack()->count() != countBefore + 1) {
                fail("moveTrack was not a single undo command");
                ok = false;
            }
            if (ok
                && (!notesMatch(last, srcNotes) || doc.channelFor(last) != srcChannel)) {
                fail("moved track's notes or channel changed");
                ok = false;
            }
            if (ok
                && (!seqChunkHas(0x51, base + step * 110)
                    || !seqChunkHas(0x58, base + step * 112))) {
                fail("seq globals did not stay with chunk 0 across the move");
                ok = false;
            }
            if (ok
                && (doc.loopTick(false) != loopStartBefore
                    || doc.loopTick(true) != loopEndBefore)) {
                fail("moveTrack lost the loop markers");
                ok = false;
            }
            if (ok) {
                doc.undoStack()->undo();
                if (!notesMatch(0, srcNotes)) {
                    fail("moveTrack undo did not restore the track order");
                    ok = false;
                } else {
                    doc.undoStack()->redo();
                }
            }
            if (ok) {
                doc.moveTrack(last, 0); // and back again
                mutateAndCheck("events unsorted after moveTrack back");
                if (!notesMatch(0, srcNotes)
                    || !seqChunkHas(0x51, base + step * 110)
                    || !seqChunkHas(0x58, base + step * 112)) {
                    fail("moving the track back did not restore its slot");
                    ok = false;
                }
            }
        }

        // Deleting an original track must not lose the loop markers, even
        // when they live in the removed chunk (they get rescued into the seq
        // chunk). Undone right away so the loop/cfg script below still runs
        // against the full song.
        if (ok && track >= 0) {
            const uint64_t loopStartBefore = doc.loopTick(false);
            const uint64_t loopEndBefore = doc.loopTick(true);
            doc.deleteTrack(track);
            mutateAndCheck("events unsorted after deleteTrack of a song track");
            if (ok
                && (doc.loopTick(false) != loopStartBefore
                    || doc.loopTick(true) != loopEndBefore)) {
                fail("deleteTrack lost the loop markers");
                ok = false;
            }
            doc.undoStack()->undo();
        }

        // Time signatures: create, modify in place, move, delete.
        if (ok) {
            auto findSig = [&doc](uint64_t tick, DocTimeSig *out) {
                for (const DocTimeSig &sig : doc.timeSigs()) {
                    if (sig.tick == tick) {
                        *out = sig;
                        return true;
                    }
                }
                return false;
            };
            const size_t sigsBefore = doc.timeSigs().size();
            doc.setTimeSig(base, 3, 3); // 3/8
            mutateAndCheck("events unsorted after setTimeSig");
            DocTimeSig sig;
            if (ok && (!findSig(base, &sig) || sig.numerator != 3 || sig.denomPow2 != 3)) {
                fail("time signature not found after set");
                ok = false;
            }
            if (ok) {
                doc.setTimeSig(base, 7, 2); // 7/4, replacing in place
                if (!findSig(base, &sig) || sig.numerator != 7 || sig.denomPow2 != 2
                    || doc.timeSigs().size() != sigsBefore + 1) {
                    fail("time signature edit did not replace in place");
                    ok = false;
                }
            }
            if (ok) {
                doc.moveTimeSig(base, base + step * 4);
                mutateAndCheck("events unsorted after moveTimeSig");
                if (findSig(base, &sig) || !findSig(base + step * 4, &sig)
                    || sig.numerator != 7) {
                    fail("time signature not moved");
                    ok = false;
                }
            }
            if (ok) {
                doc.deleteTimeSig(base + step * 4);
                if (findSig(base + step * 4, &sig)) {
                    fail("time signature not deleted");
                    ok = false;
                }
            }
        }

        // Loop markers: move an existing one / create where absent, and cfg.
        const uint64_t loopStart = doc.loopTick(false);
        doc.setLoopTick(false, loopStart == UINT64_MAX ? 0 : int64_t(loopStart + step));
        mutateAndCheck("events unsorted after setLoopTick");
        SongCfg cfg = doc.cfg();
        cfg.masterVolume = cfg.masterVolume == 80 ? 90 : 80;
        doc.setCfg(cfg);

        // Undo everything: the document must be byte-identical to the load.
        while (doc.undoStack()->canUndo())
            doc.undoStack()->undo();
        if (doc.smf().write() != baseline)
            fail("undo-all did not restore the original bytes");
        else if (doc.cfg().masterVolume != song.cfg.masterVolume)
            fail("undo-all did not restore song settings");
        else {
            // Redo everything, then undo again: redo must be deterministic.
            while (doc.undoStack()->canRedo())
                doc.undoStack()->redo();
            const QByteArray redone = doc.smf().write();
            while (doc.undoStack()->canUndo())
                doc.undoStack()->undo();
            if (doc.smf().write() != baseline)
                fail("undo after redo did not restore the original bytes");
            else if (redone == baseline && track >= 0)
                fail("redo-all produced no change (edits were lost)");
        }

        checked++;
    }

    std::printf("editcheck: %d songs in %lld ms\n", checked, (long long)timer.elapsed());
    std::printf("editcheck: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
