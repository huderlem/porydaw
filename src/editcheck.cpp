#include <QElapsedTimer>
#include <QString>
#include <QTemporaryDir>
#include <cstdio>

#include "core/miditimeline.h"
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

// The document contracts on synthetic fixtures: NoteId minting/preservation,
// the revision counter, publication order (tracksRemapped before
// documentChanged, exactly once per mutation), the batch APIs, and the
// merged-move undo-entry rules.
int documentContractFailures()
{
    int failures = 0;
    auto fail = [&failures](const char *what) {
        std::fprintf(stderr, "editcheck: FAIL document-contracts: %s\n", what);
        failures++;
    };
    auto chEvent = [](uint8_t status, uint64_t tick, uint8_t d0, uint8_t d1) {
        SmfEvent ev;
        ev.tick = tick;
        ev.status = status;
        ev.data0 = d0;
        ev.data1 = d1;
        return ev;
    };
    auto meta = [](uint64_t tick, uint8_t type, QByteArray blob) {
        SmfEvent ev;
        ev.tick = tick;
        ev.status = 0xFF;
        ev.metaType = type;
        ev.blob = std::move(blob);
        return ev;
    };

    // Conductor + a channel-0 chunk whose two note-ons are byte-identical
    // apart from velocity and share pairing territory (both end candidates
    // on one key) + a channel-1 chunk, so the fixture has two engine tracks.
    SmfFile smf;
    smf.format = 1;
    smf.division = 24;
    SmfTrack conductor;
    conductor.events.push_back(meta(0, 0x01, QByteArrayLiteral("contract fixture")));
    conductor.endTick = 48;
    smf.tracks.push_back(conductor);
    SmfTrack first;
    first.events.push_back(chEvent(0xC0, 0, 1, 0));
    first.events.push_back(chEvent(0x90, 0, 60, 100));
    first.events.push_back(chEvent(0x90, 0, 60, 90));
    first.events.push_back(chEvent(0x80, 12, 60, 0));
    first.events.push_back(chEvent(0x80, 24, 60, 0));
    first.endTick = 48;
    smf.tracks.push_back(first);
    SmfTrack second;
    second.events.push_back(chEvent(0xC1, 0, 2, 0));
    second.endTick = 48;
    smf.tracks.push_back(second);

    QTemporaryDir tmp;
    const QString midPath = tmp.path() + QStringLiteral("/contracts.mid");
    QString error;
    SongInfo info;
    info.label = QStringLiteral("contracts");
    info.midPath = midPath;
    info.hasMid = true;
    SongDocument doc;
    std::vector<QString> order;
    std::vector<TrackRemap> remaps;
    QObject::connect(&doc, &SongDocument::tracksRemapped, [&order, &remaps](TrackRemap remap) {
        order.push_back(QStringLiteral("remap"));
        remaps.push_back(std::move(remap));
    });
    QObject::connect(&doc, &SongDocument::documentChanged,
                     [&order] { order.push_back(QStringLiteral("changed")); });
    bool ok = tmp.isValid() && smf.writeFile(midPath, &error) && doc.load(info, &error);
    if (!ok) {
        fail("could not write/load the synthetic file");
        return failures;
    }
    // Load replaces the document without publishing (the owner re-attaches
    // views itself — publishing mid-load would re-enter them), but it must
    // still land on a fresh revision so pre-load state can never match.
    if (doc.revision() != 1 || !order.empty()) {
        fail("load did not bump the revision exactly once without publishing");
        return failures;
    }

    auto clearSignals = [&order, &remaps] {
        order.clear();
        remaps.clear();
    };
    auto expect = [&](bool condition, const char *what) {
        if (!condition) {
            fail(what);
            ok = false;
        }
    };
    auto expectRemap = [&](const char *what, const std::vector<int> &smfMap,
                           const std::vector<int> &engineMap, int newSmfCount, int newEngineCount) {
        expect(remaps.size() == 1 && remaps.front().smfTrackMap == smfMap &&
                   remaps.front().engineTrackMap == engineMap &&
                   remaps.front().newSmfTrackCount == newSmfCount &&
                   remaps.front().newEngineTrackCount == newEngineCount &&
                   order ==
                       std::vector<QString>{QStringLiteral("remap"), QStringLiteral("changed")},
               what);
    };

    // Byte-identical-key duplicate note-ons get distinct identities, and
    // findNote(NoteId) resolves each exactly.
    const auto notes = doc.notesForTrack(0);
    expect(notes.size() == 2 && notes[0].noteId.isAssigned() && notes[1].noteId.isAssigned() &&
               notes[0].noteId != notes[1].noteId,
           "duplicate note-ons did not receive distinct identities");
    if (ok) {
        DocNote byId, bySecondId;
        expect(doc.findNote(notes[0].noteId, &byId) && doc.findNote(notes[1].noteId, &bySecondId) &&
                   byId.onIndex == notes[0].onIndex && bySecondId.noteId == notes[1].noteId,
               "identity lookup did not resolve exact duplicate notes");
    }

    // setNotesVelocities: last entry per note wins, values clamp, ONE undo
    // entry, one publication, and identity survives the write and its
    // undo/redo. Every published step bumps the revision exactly once.
    if (ok) {
        clearSignals();
        const uint64_t before = doc.revision();
        const int undoCount = doc.undoStack()->count();
        const auto result = doc.setNotesVelocities(
            before, {{notes[0].noteId, 0}, {notes[1].noteId, 200}, {notes[0].noteId, 99}});
        DocNote firstNow, secondNow;
        expect(result && *result == before + 1 && doc.revision() == before + 1 &&
                   doc.undoStack()->count() == undoCount + 1 &&
                   doc.findNote(notes[0].noteId, &firstNow) &&
                   doc.findNote(notes[1].noteId, &secondNow) && firstNow.velocity == 99 &&
                   secondNow.velocity == 127 &&
                   order == std::vector<QString>{QStringLiteral("changed")},
               "velocity batch did not last-write, clamp, and commit atomically");
        const uint64_t applied = doc.revision();
        clearSignals();
        doc.undoStack()->undo();
        expect(doc.revision() == applied + 1 && doc.findNote(notes[0].noteId, &firstNow) &&
                   doc.findNote(notes[1].noteId, &secondNow) && firstNow.velocity == 100 &&
                   secondNow.velocity == 90 &&
                   order == std::vector<QString>{QStringLiteral("changed")},
               "velocity batch undo did not restore exact values");
        const uint64_t undone = doc.revision();
        clearSignals();
        doc.undoStack()->redo();
        expect(doc.revision() == undone + 1 && doc.findNote(notes[0].noteId, &firstNow) &&
                   doc.findNote(notes[1].noteId, &secondNow) && firstNow.velocity == 99 &&
                   secondNow.velocity == 127 &&
                   order == std::vector<QString>{QStringLiteral("changed")},
               "velocity batch redo did not preserve duplicate identities");
        if (ok) {
            // The lower clamp rail.
            clearSignals();
            const uint64_t lowerBefore = doc.revision();
            const int lowerUndoCount = doc.undoStack()->count();
            const auto lowerResult = doc.setNotesVelocities(lowerBefore, {{notes[0].noteId, 0}});
            expect(lowerResult && *lowerResult == lowerBefore + 1 &&
                       doc.revision() == lowerBefore + 1 &&
                       doc.undoStack()->count() == lowerUndoCount + 1 &&
                       doc.findNote(notes[0].noteId, &firstNow) && firstNow.velocity == 1 &&
                       order == std::vector<QString>{QStringLiteral("changed")},
                   "velocity batch did not clamp to one");
            clearSignals();
            const uint64_t lowerApplied = doc.revision();
            doc.undoStack()->undo();
            expect(doc.revision() == lowerApplied + 1 &&
                       doc.undoStack()->count() == lowerUndoCount + 1 &&
                       doc.findNote(notes[0].noteId, &firstNow) && firstNow.velocity == 99 &&
                       order == std::vector<QString>{QStringLiteral("changed")},
                   "lower-clamped velocity batch undo did not restore the exact value");
        }
    }

    // setNotesVelocities refusal paths: a stale revision, an unresolvable
    // id anywhere in the batch, and an accepted all-no-op batch must all
    // leave the document byte-identical with nothing pushed or published.
    if (ok) {
        DocNote current;
        doc.findNote(notes[0].noteId, &current);
        const QByteArray unchanged = doc.smf().write();
        const uint64_t before = doc.revision();
        const int undoCount = doc.undoStack()->count();
        clearSignals();
        const auto staleResult = doc.setNotesVelocities(before - 1, {{current.noteId, 42}});
        expect(!staleResult && doc.smf().write() == unchanged && doc.revision() == before &&
                   doc.undoStack()->count() == undoCount && order.empty(),
               "stale expected revision did not refuse the write");
        const auto staleBatch =
            doc.setNotesVelocities(before - 1, {{current.noteId, 42}, {notes[1].noteId, 77}});
        expect(!staleBatch && doc.smf().write() == unchanged && doc.revision() == before &&
                   doc.undoStack()->count() == undoCount && order.empty(),
               "stale velocity batch was not rejected atomically");
        const NoteId invalidId;
        const auto invalidBatch =
            doc.setNotesVelocities(before, {{current.noteId, 42}, {invalidId, 77}});
        expect(!invalidBatch && doc.smf().write() == unchanged && doc.revision() == before &&
                   doc.undoStack()->count() == undoCount && order.empty(),
               "an unresolvable NoteId partially mutated the batch");
        DocNote secondCurrent;
        doc.findNote(notes[1].noteId, &secondCurrent);
        const auto noOp =
            doc.setNotesVelocities(before, {{current.noteId, current.velocity},
                                            {secondCurrent.noteId, secondCurrent.velocity}});
        expect(noOp && *noOp == before && doc.smf().write() == unchanged &&
                   doc.revision() == before && doc.undoStack()->count() == undoCount &&
                   order.empty(),
               "no-op velocity batch changed document state");
    }

    // Undo-stack hygiene: an edit whose clamped result changes nothing must
    // push no command and publish nothing — for moves, velocity writes,
    // velocity nudges, and both resize directions.
    if (ok) {
        DocNote n0;
        doc.findNote(notes[0].noteId, &n0);
        const QByteArray unchanged = doc.smf().write();
        const uint64_t before = doc.revision();
        const int undoCount = doc.undoStack()->count();
        clearSignals();
        doc.moveNotes({n0}, -100, 0); // tick 0: fully absorbed by the clamp
        expect(doc.undoStack()->count() == undoCount && doc.revision() == before && order.empty(),
               "a fully clamped move pushed a command or published");
        doc.setNotesVelocity({n0}, n0.velocity);
        expect(doc.undoStack()->count() == undoCount && doc.revision() == before && order.empty(),
               "an at-target velocity write pushed a command or published");
        doc.resizeNotesLeft({n0}, -100); // tick 0 again
        expect(doc.undoStack()->count() == undoCount && doc.revision() == before && order.empty(),
               "a fully clamped left resize pushed a command or published");
        expect(doc.smf().write() == unchanged, "a no-op edit changed document bytes");
        // The nudge clamp rail and the resize duration floor need notes
        // parked there first; those setup edits undo themselves away.
        doc.setNotesVelocity({n0}, 127);
        DocNote pinned;
        doc.findNote(n0.noteId, &pinned);
        const uint64_t pinnedRevision = doc.revision();
        const int pinnedCount = doc.undoStack()->count();
        clearSignals();
        doc.nudgeNotesVelocity({pinned}, 5); // 127 + 5 clamps to 127
        expect(doc.undoStack()->count() == pinnedCount && doc.revision() == pinnedRevision &&
                   order.empty(),
               "a clamp-pinned velocity nudge pushed a command or published");
        doc.undoStack()->undo();
        doc.findNote(n0.noteId, &pinned);
        doc.resizeNotes({pinned}, -int64_t(pinned.duration)); // to the 1-tick floor
        doc.findNote(n0.noteId, &pinned);
        const uint64_t floorRevision = doc.revision();
        const int floorCount = doc.undoStack()->count();
        clearSignals();
        doc.resizeNotes({pinned}, -100); // already at the floor
        expect(pinned.duration == 1 && doc.undoStack()->count() == floorCount &&
                   doc.revision() == floorRevision && order.empty(),
               "a floor-pinned resize pushed a command or published");
        doc.undoStack()->undo();
        expect(doc.smf().write() == unchanged, "the hygiene probes did not restore their state");
    }

    // Moves re-insert the note's own events: a real note-off (0x80) keeps
    // its form and its release velocity across a move, where a synthesized
    // replacement would write a velocity-0 note-on.
    if (ok) {
        SmfFile fidelity;
        fidelity.format = 1;
        fidelity.division = 24;
        SmfTrack fidelityTrack;
        fidelityTrack.events.push_back(chEvent(0xC0, 0, 1, 0));
        fidelityTrack.events.push_back(chEvent(0x90, 0, 60, 100));
        fidelityTrack.events.push_back(chEvent(0x80, 12, 60, 64)); // release velocity 64
        fidelityTrack.endTick = 48;
        fidelity.tracks.push_back(fidelityTrack);
        const QString fidelityPath = tmp.path() + QStringLiteral("/clone-fidelity.mid");
        SongInfo fidelityInfo = info;
        fidelityInfo.label = QStringLiteral("clone fidelity");
        fidelityInfo.midPath = fidelityPath;
        SongDocument fidelityDoc;
        const bool loaded =
            fidelity.writeFile(fidelityPath, &error) && fidelityDoc.load(fidelityInfo, &error);
        expect(loaded, "could not load the clone-fidelity fixture");
        if (!loaded)
            return failures;
        DocNote note;
        auto endEvent = [&fidelityDoc](const DocNote &n) {
            return fidelityDoc.smf().tracks[size_t(n.smfTrack)].events[n.endIndex];
        };
        const bool resolved = fidelityDoc.findNote(0, 0, 60, &note);
        expect(resolved, "clone-fidelity fixture did not resolve its note");
        const NoteId id = note.noteId;
        bool moved = resolved;
        if (moved) {
            fidelityDoc.moveNotes({note}, 12, 0);
            moved = fidelityDoc.findNote(id, &note) && note.tick == 12 && !note.unterminated();
        }
        expect(moved && endEvent(note).status == 0x80 && endEvent(note).data1 == 64,
               "a move rebuilt the note-off instead of re-inserting it");
        bool leftResized = moved;
        if (leftResized) {
            fidelityDoc.resizeNotesLeft({note}, 6);
            // Re-resolved by IDENTITY: a left resize that synthesized a new
            // note-on would mint a new id here.
            leftResized = fidelityDoc.findNote(id, &note) && note.tick == 18;
        }
        expect(leftResized, "a left resize rebuilt the note-on instead of re-inserting it");
        bool resized = leftResized;
        if (resized) {
            fidelityDoc.resizeNotes({note}, 6);
            resized =
                fidelityDoc.findNote(id, &note) && note.duration == 12 && !note.unterminated();
        }
        expect(resized && endEvent(note).status == 0x80 && endEvent(note).data1 == 64,
               "a right resize rebuilt the note-off instead of re-inserting it");
    }

    // A net-zero merged move must leave the TRACK MAP rebuilt, not just the
    // bytes: reverting a move can change which event is a mixed-channel
    // chunk's first channel event — the one channelFor() derives from — and
    // later edits would land on another track's channel.
    if (ok) {
        SmfFile mixed;
        mixed.format = 1;
        mixed.division = 24;
        SmfTrack mixedTrack;
        mixedTrack.events.push_back(chEvent(0x91, 0, 60, 100)); // ch1 note-on first
        mixedTrack.events.push_back(chEvent(0xB0, 0, 7, 64));   // ch0 CC, same tick
        mixedTrack.endTick = 8;
        mixed.tracks.push_back(mixedTrack);
        const QString mixedPath = tmp.path() + QStringLiteral("/mixed-map.mid");
        SongInfo mixedInfo = info;
        mixedInfo.label = QStringLiteral("mixed map");
        mixedInfo.midPath = mixedPath;
        SongDocument mixedDoc;
        const bool loaded = mixed.writeFile(mixedPath, &error) && mixedDoc.load(mixedInfo, &error);
        expect(loaded, "could not load the mixed-map fixture");
        if (!loaded)
            return failures;
        DocNote note;
        expect(mixedDoc.channelFor(0) == 1 && mixedDoc.findNote(0, 0, 60, &note),
               "mixed-map fixture did not map its first channel event");
        if (ok) {
            mixedDoc.moveNotes({note}, 1, 0, true);
            // The moved note-on lands after the CC, which becomes the
            // chunk's first channel event.
            expect(mixedDoc.channelFor(0) == 0 && mixedDoc.findNote(0, 1, 60, &note),
                   "moving past the CC did not remap the track's channel");
        }
        if (ok) {
            mixedDoc.moveNotes({note}, -1, 0, true); // net zero: command removed
            expect(mixedDoc.undoStack()->count() == 0 && mixedDoc.channelFor(0) == 1,
                   "a net-zero merged move left a stale track map");
        }
    }

    // duplicateTrack mints FRESH identities for the copies, and redo after
    // undo replays those same identities.
    if (ok) {
        const auto source = doc.notesForTrack(0);
        clearSignals();
        const int copy = doc.duplicateTrack(0);
        expect(copy >= 0, "track duplication rejected a duplicable source");
        if (copy >= 0) {
            const auto copied = doc.notesForTrack(copy);
            bool matches = copied.size() == source.size();
            for (size_t i = 0; matches && i < copied.size(); i++) {
                matches = copied[i].tick == source[i].tick && copied[i].key == source[i].key &&
                          copied[i].duration == source[i].duration &&
                          copied[i].velocity == source[i].velocity && copied[i].noteId.isAssigned();
                for (size_t j = 0; matches && j < source.size(); j++)
                    matches = copied[i].noteId != source[j].noteId;
                for (size_t j = 0; matches && j < i; j++)
                    matches = copied[i].noteId != copied[j].noteId;
            }
            expect(matches, "track duplication did not copy notes with fresh identities");
            doc.undoStack()->undo();
            doc.undoStack()->redo();
            const auto redone = doc.notesForTrack(copy);
            bool idsPreserved = redone.size() == copied.size();
            for (size_t i = 0; idsPreserved && i < redone.size(); i++)
                idsPreserved = redone[i].noteId == copied[i].noteId;
            expect(idsPreserved, "track duplication redo did not preserve minted identities");
            doc.undoStack()->undo();
        }
    }

    // tracksRemapped for every ownership change, exactly once per
    // publication, remap before changed, with the undo publishing the
    // inverse map. Chunk layout here: 0 conductor, 1 ch0 (engine 0),
    // 2 ch1 (engine 1).
    if (ok) {
        clearSignals();
        const uint64_t before = doc.revision();
        expect(doc.moveTrack(0, 1) && doc.revision() == before + 1,
               "track move did not increment the revision exactly once");
        expectRemap("track move remap/order was incomplete", {0, 2, 1}, {1, 0}, 3, 2);
        clearSignals();
        doc.undoStack()->undo();
        expectRemap("track move undo remap was incomplete", {0, 2, 1}, {1, 0}, 3, 2);
        clearSignals();
        doc.undoStack()->redo();
        expectRemap("track move redo remap was incomplete", {0, 2, 1}, {1, 0}, 3, 2);
        clearSignals();
        doc.undoStack()->undo(); // back to the original order for the rest
        clearSignals();
    }
    if (ok) {
        clearSignals();
        const int added = doc.addTrack(3);
        expect(added == 2, "track insertion did not produce its new engine slot");
        expectRemap("track insertion remap/order was incomplete", {0, 1, 2}, {0, 1}, 4, 3);
        clearSignals();
        doc.undoStack()->undo();
        expectRemap("track insertion undo remap was incomplete", {0, 1, 2, -1}, {0, 1, -1}, 3, 2);
        clearSignals();
        doc.undoStack()->redo();
        expectRemap("track insertion redo remap was incomplete", {0, 1, 2}, {0, 1}, 4, 3);
        clearSignals();
        doc.deleteTrack(added);
        expectRemap("track deletion remap did not mark deleted owners", {0, 1, 2, -1}, {0, 1, -1},
                    3, 2);
        clearSignals();
        doc.undoStack()->undo();
        expectRemap("track deletion undo remap was incomplete", {0, 1, 2}, {0, 1}, 4, 3);
        clearSignals();
        doc.undoStack()->redo();
        expectRemap("track deletion redo remap was incomplete", {0, 1, 2, -1}, {0, 1, -1}, 3, 2);
    }
    if (ok) {
        clearSignals();
        const int copy = doc.duplicateTrack(0);
        expect(copy == 2, "track duplication did not create its engine slot");
        expectRemap("track duplication remap/order was incomplete", {0, 1, 2}, {0, 1}, 4, 3);
        clearSignals();
        doc.undoStack()->undo();
        expectRemap("track duplication undo remap was incomplete", {0, 1, 2, -1}, {0, 1, -1}, 3, 2);
        clearSignals();
        doc.undoStack()->redo();
        expectRemap("track duplication redo remap was incomplete", {0, 1, 2}, {0, 1}, 4, 3);
    }

    // No remap for edits that leave ownership alone — and no publication at
    // all for a refused no-op.
    if (ok) {
        clearSignals();
        doc.insertRawEvent(0, meta(12, 0x01, QByteArrayLiteral("metadata")));
        expect(remaps.empty() && order == std::vector<QString>{QStringLiteral("changed")},
               "identity raw-metadata edit published a remap");
        clearSignals();
        doc.undoStack()->undo();
        expect(remaps.empty() && order == std::vector<QString>{QStringLiteral("changed")},
               "identity raw-metadata undo published a remap");
        clearSignals();
        const uint64_t before = doc.revision();
        expect(!doc.moveTrack(0, 0) && doc.revision() == before && order.empty(),
               "track move no-op changed state");
    }

    // A raw edit can flip a chunk's ENGINE status without touching chunk
    // structure: chunk 0 gaining its first channel event becomes engine
    // track 0 and every existing engine slot shifts (and back on undo).
    // Chunk layout at this point: 0 conductor, 1 ch0, 2 ch1, 3 the
    // duplicate from above.
    if (ok) {
        clearSignals();
        doc.insertRawEvent(0, chEvent(0xC2, 0, 4, 0));
        expectRemap("metadata-to-engine remap was incomplete", {0, 1, 2, 3}, {1, 2, 3}, 4, 4);
        clearSignals();
        doc.undoStack()->undo();
        expectRemap("metadata-to-engine undo remap was incomplete", {0, 1, 2, 3}, {-1, 0, 1, 2}, 4,
                    3);
        clearSignals();
        doc.undoStack()->redo();
        expectRemap("metadata-to-engine redo remap was incomplete", {0, 1, 2, 3}, {1, 2, 3}, 4, 4);
        size_t programIndex = SIZE_MAX;
        for (size_t i = 0; i < doc.smf().tracks[0].events.size(); i++) {
            const SmfEvent &ev = doc.smf().tracks[0].events[i];
            if (ev.isChannel() && ev.typeNibble() == 0xC && ev.channel() == 2)
                programIndex = i;
        }
        if (programIndex == SIZE_MAX) {
            fail("metadata-to-engine fixture lost its inserted program");
            return failures;
        }
        clearSignals();
        doc.deleteRawEvents(0, {programIndex});
        expectRemap("engine-to-metadata remap was incomplete", {0, 1, 2, 3}, {-1, 0, 1, 2}, 4, 3);
        clearSignals();
        doc.undoStack()->undo();
        expectRemap("engine-to-metadata undo remap was incomplete", {0, 1, 2, 3}, {1, 2, 3}, 4, 4);
        clearSignals();
        doc.undoStack()->redo();
        expectRemap("engine-to-metadata redo remap was incomplete", {0, 1, 2, 3}, {-1, 0, 1, 2}, 4,
                    3);
    }

    // duplicateTrack on a mixed-channel chunk: a foreign file may interleave
    // several channels in one chunk; the duplicate must copy only the
    // source engine track's own channel. (The other channel's notes remain
    // visible on the chunk's engine track, each with its own identity.)
    if (ok) {
        SmfFile collision;
        collision.format = 1;
        collision.division = 24;
        collision.tracks.push_back(conductor);
        SmfTrack sourceTrack;
        sourceTrack.events.push_back(meta(0, 0x03, QByteArrayLiteral("owned channel")));
        sourceTrack.events.push_back(chEvent(0xC1, 0, 7, 0));
        sourceTrack.events.push_back(chEvent(0x91, 0, 60, 100));
        sourceTrack.events.push_back(chEvent(0x90, 24, 60, 90));
        sourceTrack.events.push_back(chEvent(0x81, 24, 60, 0));
        sourceTrack.events.push_back(chEvent(0x80, 48, 60, 0));
        sourceTrack.endTick = 48;
        collision.tracks.push_back(sourceTrack);
        // Fill channels 2-15 so the duplicate must land on the interleaved
        // channel 0 — the collision-prone choice.
        for (int ch = 2; ch < 16; ch++) {
            SmfTrack tr;
            tr.events.push_back(chEvent(uint8_t(0xC0 | ch), 0, uint8_t(ch), 0));
            tr.endTick = 48;
            collision.tracks.push_back(std::move(tr));
        }
        const QString collisionPath = tmp.path() + QStringLiteral("/channel-collision.mid");
        SongInfo collisionInfo = info;
        collisionInfo.label = QStringLiteral("channel collision");
        collisionInfo.midPath = collisionPath;
        SongDocument collisionDoc;
        std::vector<TrackRemap> collisionRemaps;
        QObject::connect(
            &collisionDoc, &SongDocument::tracksRemapped,
            [&collisionRemaps](TrackRemap remap) { collisionRemaps.push_back(std::move(remap)); });
        const bool loaded =
            collision.writeFile(collisionPath, &error) && collisionDoc.load(collisionInfo, &error);
        expect(loaded, "could not load the channel-collision fixture");
        if (!loaded)
            return failures;
        // Engine track 0 is the mixed chunk on channel 1; both channels'
        // notes project onto it, each with its own identity.
        const auto source = collisionDoc.notesForTrack(0);
        DocNote owned, foreign;
        expect(collisionDoc.channelFor(0) == 1 && source.size() == 2 && source[0].tick == 0 &&
                   source[0].channel == 1 && source[0].duration == 24 && source[1].tick == 24 &&
                   source[1].channel == 0 && source[1].duration == 24 &&
                   source[0].noteId.isAssigned() && source[1].noteId.isAssigned() &&
                   source[0].noteId != source[1].noteId &&
                   collisionDoc.findNote(source[0].noteId, &owned) &&
                   collisionDoc.findNote(source[1].noteId, &foreign) &&
                   owned.onIndex == source[0].onIndex && foreign.onIndex == source[1].onIndex,
               "mixed-channel chunk did not project both channels with distinct identities");
        const int copy = collisionDoc.duplicateTrack(0);
        const auto copied = copy >= 0 ? collisionDoc.notesForTrack(copy) : std::vector<DocNote>();
        const int copiedSmfTrack = collisionDoc.smfTrackFor(copy);
        bool copiedEvents = false;
        if (copiedSmfTrack >= 0) {
            // Only the owned channel's three events, rewritten to the free
            // channel 0, with the end-of-track tick preserved.
            const SmfTrack &copiedTrack = collisionDoc.smf().tracks[size_t(copiedSmfTrack)];
            copiedEvents = copiedTrack.endTick == sourceTrack.endTick &&
                           copiedTrack.events.size() == 3 &&
                           copiedTrack.events[0] == chEvent(0xC0, 0, 7, 0) &&
                           copiedTrack.events[1] == chEvent(0x90, 0, 60, 100) &&
                           copiedTrack.events[2] == chEvent(0x80, 24, 60, 0);
        }
        bool visibleMatch = copied.size() == 1;
        if (visibleMatch) {
            visibleMatch = copied[0].tick == source[0].tick && copied[0].key == source[0].key &&
                           copied[0].duration == source[0].duration &&
                           copied[0].velocity == source[0].velocity &&
                           copied[0].noteId.isAssigned() && copied[0].noteId != source[0].noteId &&
                           copied[0].noteId != source[1].noteId;
        }
        bool completeRemap = collisionRemaps.size() == 1 &&
                             collisionRemaps.front().smfTrackMap.size() == 16 &&
                             collisionRemaps.front().engineTrackMap.size() == 15 &&
                             collisionRemaps.front().newSmfTrackCount == 17 &&
                             collisionRemaps.front().newEngineTrackCount == 16;
        for (int i = 0; completeRemap && i < 16; i++)
            completeRemap = collisionRemaps.front().smfTrackMap[i] == i;
        for (int i = 0; completeRemap && i < 15; i++)
            completeRemap = collisionRemaps.front().engineTrackMap[i] == i;
        expect(copy >= 0 && collisionDoc.channelFor(copy) == 0 && copiedEvents && visibleMatch &&
                   completeRemap,
               "track duplication did not isolate the owned channel");
        if (copy >= 0 && !copied.empty()) {
            const NoteId copiedId = copied[0].noteId;
            collisionDoc.undoStack()->undo();
            collisionDoc.undoStack()->redo();
            DocNote redone;
            expect(collisionDoc.findNote(copiedId, &redone) && redone.tick == source[0].tick &&
                       redone.key == source[0].key && redone.duration == source[0].duration &&
                       redone.velocity == source[0].velocity,
                   "channel-isolated duplicate redo did not preserve its identity");
        }
    }

    // duplicateTrack must never copy sequencer globals (tempo, time
    // signature, loop/label markers) even when the source chunk is the seq
    // chunk carrying them — and moving or deleting the duplicate must not
    // disturb the originals, through undo and redo.
    if (ok) {
        SmfFile globals;
        globals.format = 1;
        globals.division = 24;
        SmfTrack globalSource;
        globalSource.events.push_back(meta(0, 0x03, QByteArrayLiteral("lead")));
        globalSource.events.push_back(meta(0, 0x51, QByteArray("\x07\xA1\x20", 3))); // 120 BPM
        globalSource.events.push_back(meta(0, 0x58, QByteArray("\x04\x02\x18\x08", 4)));
        globalSource.events.push_back(chEvent(0xC0, 0, 6, 0));
        globalSource.events.push_back(chEvent(0x90, 0, 60, 100));
        globalSource.events.push_back(meta(4, 0x01, QByteArrayLiteral("global annotation")));
        globalSource.events.push_back(meta(12, 0x06, QByteArrayLiteral("[")));
        globalSource.events.push_back(meta(16, 0x06, QByteArrayLiteral(":")));
        globalSource.events.push_back(chEvent(0x80, 24, 60, 0));
        globalSource.endTick = 48;
        globals.tracks.push_back(globalSource);
        SmfTrack backing;
        backing.events.push_back(chEvent(0xC1, 0, 7, 0));
        backing.endTick = 48;
        globals.tracks.push_back(backing);
        const QString globalsPath = tmp.path() + QStringLiteral("/duplicate-globals.mid");
        SongInfo globalsInfo = info;
        globalsInfo.label = QStringLiteral("duplicate globals");
        globalsInfo.midPath = globalsPath;
        SongDocument globalsDoc;
        const bool loaded =
            globals.writeFile(globalsPath, &error) && globalsDoc.load(globalsInfo, &error);
        expect(loaded, "could not load the duplicate-globals fixture");
        if (!loaded)
            return failures;
        auto activeGlobalsAreOriginal = [&globalsDoc] {
            const auto tempos = globalsDoc.lanePoints(-1, DOC_CC_TEMPO);
            const auto signatures = globalsDoc.timeSigs();
            int tempoEvents = 0, signatureEvents = 0, starts = 0, labels = 0;
            for (const SmfTrack &track : globalsDoc.smf().tracks) {
                for (const SmfEvent &ev : track.events) {
                    if (!ev.isMeta())
                        continue;
                    if (ev.metaType == 0x51 && ev.blob.size() == 3)
                        tempoEvents++;
                    if (ev.metaType == 0x58 && ev.blob.size() >= 2)
                        signatureEvents++;
                    if (ev.metaType == 0x06 && ev.blob == QByteArrayLiteral("["))
                        starts++;
                    if (ev.metaType == 0x06 && ev.blob == QByteArrayLiteral(":"))
                        labels++;
                }
            }
            return tempos.size() == 1 && tempos.front().tick == 0 && tempos.front().value == 120 &&
                   signatures.size() == 1 && signatures.front().tick == 0 &&
                   signatures.front().numerator == 4 && signatures.front().denomPow2 == 2 &&
                   globalsDoc.loopTick(false) == 12 && globalsDoc.loopTick(true) == UINT64_MAX &&
                   tempoEvents == 1 && signatureEvents == 1 && starts == 1 && labels == 1;
        };
        expect(activeGlobalsAreOriginal(), "duplicate-globals fixture did not load canonically");
        const int copy = globalsDoc.duplicateTrack(0);
        bool copiedOnlyOwned = copy >= 0 && globalsDoc.channelFor(copy) == 2;
        if (copiedOnlyOwned) {
            const SmfTrack &copiedTrack = globalsDoc.smf().tracks[globalsDoc.smfTrackFor(copy)];
            copiedOnlyOwned = copiedTrack.endTick == globalSource.endTick &&
                              copiedTrack.events.size() == 3 &&
                              copiedTrack.events[0] == chEvent(0xC2, 0, 6, 0) &&
                              copiedTrack.events[1] == chEvent(0x92, 0, 60, 100) &&
                              copiedTrack.events[2] == chEvent(0x82, 24, 60, 0);
        }
        expect(copiedOnlyOwned && activeGlobalsAreOriginal(),
               "track duplication copied sequencer-global metadata");
        if (copy >= 0) {
            const bool moved = globalsDoc.moveTrack(copy, 0);
            expect(moved && activeGlobalsAreOriginal(),
                   "moving a duplicate activated copied global metadata");
            if (moved) {
                globalsDoc.deleteTrack(0);
                expect(activeGlobalsAreOriginal(),
                       "deleting a moved duplicate changed sequencer-global metadata");
                globalsDoc.undoStack()->undo();
                expect(activeGlobalsAreOriginal(),
                       "undoing duplicate deletion changed sequencer-global metadata");
                globalsDoc.undoStack()->undo();
                expect(activeGlobalsAreOriginal(),
                       "undoing duplicate move changed sequencer-global metadata");
                globalsDoc.undoStack()->undo();
                expect(activeGlobalsAreOriginal(),
                       "undoing track duplication changed sequencer-global metadata");
                globalsDoc.undoStack()->redo();
                expect(activeGlobalsAreOriginal(),
                       "redoing track duplication changed sequencer-global metadata");
                globalsDoc.undoStack()->redo();
                expect(activeGlobalsAreOriginal(),
                       "redoing duplicate move changed sequencer-global metadata");
                globalsDoc.undoStack()->redo();
                expect(activeGlobalsAreOriginal(),
                       "redoing duplicate deletion changed sequencer-global metadata");
            }
        }
    }

    // Start tempo (the transport bar's Tempo spinner): the tick-0 tempo
    // meta read back as BPM (SMF's 120 when the song sets none), written as
    // one "set tempo" undo entry that replaces the tick-0 meta in place —
    // same-tick duplicates included — and never touches later changes.
    if (ok) {
        SmfFile tempoSmf;
        tempoSmf.format = 1;
        tempoSmf.division = 24;
        SmfTrack seq;
        seq.events.push_back(meta(0, 0x51, QByteArray("\x07\xA1\x20", 3)));  // 120 (shadowed)
        seq.events.push_back(meta(0, 0x51, QByteArray("\x06\x1A\x80", 3)));  // 150 (audible)
        seq.events.push_back(meta(48, 0x51, QByteArray("\x0F\x42\x40", 3))); // 60 at tick 48
        seq.events.push_back(chEvent(0xC0, 0, 6, 0));
        seq.events.push_back(chEvent(0x90, 0, 60, 100));
        seq.events.push_back(chEvent(0x80, 24, 60, 0));
        seq.endTick = 96;
        tempoSmf.tracks.push_back(seq);
        // A foreign-file shape: a tempo meta outside the first chunk. The
        // game (mid2agb reads tempo from the first chunk only) and the
        // timeline both ignore it; the document counts it for the warning.
        SmfTrack foreign;
        foreign.events.push_back(meta(0, 0x51, QByteArray("\x04\x93\xE0", 3))); // 200 BPM
        foreign.events.push_back(chEvent(0x91, 0, 64, 100));
        foreign.events.push_back(chEvent(0x81, 24, 64, 0));
        foreign.endTick = 96;
        tempoSmf.tracks.push_back(foreign);
        {
            // The first chunk's three metas (two at tick 0, one at 48) form
            // the tempo map; the foreign 200 BPM must not appear in it, only
            // in the event list's ignored row.
            const auto timeline = MidiTimeline::build(tempoSmf, 48000.0);
            bool listed = false;
            for (const OtherEvent &oe : timeline->otherEvents)
                listed = listed || oe.label.contains(QStringLiteral("ignored"));
            bool foreignPlayed = false;
            for (const TempoPoint &tp : timeline->tempoMap)
                foreignPlayed = foreignPlayed || tp.bpm > 190.0;
            expect(timeline->tempoMap.size() == 3 && !foreignPlayed && listed,
                   "timeline did not ignore (and list) the foreign tempo meta");
        }
        const QString tempoPath = tmp.path() + QStringLiteral("/start-tempo.mid");
        SongInfo tempoInfo = info;
        tempoInfo.label = QStringLiteral("start tempo");
        tempoInfo.midPath = tempoPath;
        SongDocument tempoDoc;
        const bool loaded =
            tempoSmf.writeFile(tempoPath, &error) && tempoDoc.load(tempoInfo, &error);
        expect(loaded, "could not load the start-tempo fixture");
        if (!loaded)
            return failures;
        expect(tempoDoc.startTempo() == 150 && tempoDoc.tempoChangesAfterStart() == 1,
               "start tempo did not read the audible tick-0 meta");
        expect(tempoDoc.tempoMetasOutsideFirstChunk() == 1,
               "foreign tempo meta not counted for the warning");
        const int undoBefore = tempoDoc.undoStack()->index();
        tempoDoc.setStartTempo(150); // already there: nothing to push
        expect(tempoDoc.undoStack()->index() == undoBefore && !tempoDoc.isDirty(),
               "setting the start tempo to its current value pushed an edit");
        tempoDoc.setStartTempo(1200); // clamped like every tempo write
        auto tick0Metas = [&tempoDoc] {
            int count = 0;
            for (const SmfEvent &ev : tempoDoc.smf().tracks[0].events)
                if (ev.tick == 0 && ev.isMeta() && ev.metaType == 0x51)
                    count++;
            return count;
        };
        expect(tempoDoc.startTempo() == 999 && tick0Metas() == 1 &&
                   tempoDoc.tempoChangesAfterStart() == 1 &&
                   tempoDoc.lanePoints(-1, DOC_CC_TEMPO).back().value == 60 &&
                   tempoDoc.undoStack()->index() == undoBefore + 1 &&
                   tempoDoc.undoStack()->text(undoBefore) == QStringLiteral("set tempo"),
               "setStartTempo did not replace the tick-0 metas as one clamped 'set tempo' edit");
        tempoDoc.undoStack()->undo();
        expect(tempoDoc.startTempo() == 150 && tick0Metas() == 2,
               "undoing the start tempo did not restore the tick-0 metas");
        // A song that sets no tempo at all reads as SMF's default; setting
        // one inserts the tick-0 meta.
        SmfFile bare = tempoSmf;
        bare.tracks[0].events.erase(bare.tracks[0].events.begin(),
                                    bare.tracks[0].events.begin() + 3);
        const QString barePath = tmp.path() + QStringLiteral("/no-tempo.mid");
        SongInfo bareInfo = info;
        bareInfo.label = QStringLiteral("no tempo");
        bareInfo.midPath = barePath;
        SongDocument bareDoc;
        const bool bareLoaded = bare.writeFile(barePath, &error) && bareDoc.load(bareInfo, &error);
        expect(bareLoaded, "could not load the no-tempo fixture");
        if (bareLoaded) {
            expect(bareDoc.startTempo() == 120 && bareDoc.tempoChangesAfterStart() == 0,
                   "a song without tempo metas did not read as 120 BPM");
            bareDoc.setStartTempo(90);
            expect(bareDoc.startTempo() == 90 && bareDoc.isDirty() &&
                       bareDoc.lanePoints(-1, DOC_CC_TEMPO).size() == 1,
                   "setStartTempo did not insert a tick-0 tempo meta");
        }
    }

    // Two indistinguishable unterminated notes crossing each other: the
    // merge test is keyed by noteId, so B moving onto A's old spot is a NEW
    // gesture, never merged into A's command.
    if (ok) {
        SmfFile crossing;
        crossing.format = 1;
        crossing.division = 24;
        SmfTrack crossTrack;
        crossTrack.events.push_back(chEvent(0xC0, 0, 1, 0));
        crossTrack.events.push_back(chEvent(0x90, 0, 60, 100)); // A
        crossTrack.events.push_back(chEvent(0x90, 0, 61, 100)); // B
        crossTrack.endTick = 8;
        crossing.tracks.push_back(crossTrack);
        const QString crossingPath = tmp.path() + QStringLiteral("/move-collisions.mid");
        SongInfo crossingInfo = info;
        crossingInfo.label = QStringLiteral("move collisions");
        crossingInfo.midPath = crossingPath;
        SongDocument crossingDoc;
        const bool loaded =
            crossing.writeFile(crossingPath, &error) && crossingDoc.load(crossingInfo, &error);
        expect(loaded, "could not load the move-collision fixture");
        if (!loaded)
            return failures;
        DocNote a, b;
        const bool resolved =
            crossingDoc.findNote(0, 0, 60, &a) && crossingDoc.findNote(0, 0, 61, &b);
        const bool ready = resolved && a.unterminated() && b.unterminated() &&
                           a.velocity == b.velocity && a.noteId.isAssigned() &&
                           b.noteId.isAssigned() && a.noteId != b.noteId;
        expect(ready, "move-collision fixture did not assign distinct identities");
        if (ready) {
            const NoteId aId = a.noteId;
            const NoteId bId = b.noteId;
            auto hasState = [&crossingDoc, aId, bId](uint8_t aKey, uint8_t bKey) {
                DocNote aNow, bNow;
                return crossingDoc.findNote(aId, &aNow) && crossingDoc.findNote(bId, &bNow) &&
                       aNow.key == aKey && bNow.key == bKey;
            };
            const int countBefore = crossingDoc.undoStack()->count();
            const uint64_t beforeFirst = crossingDoc.revision();
            crossingDoc.moveNotes({a}, 0, 1, true); // A onto B's key
            expect(crossingDoc.undoStack()->count() == countBefore + 1 &&
                       crossingDoc.revision() == beforeFirst + 1 && hasState(61, 61),
                   "first move-collision transpose did not preserve A's identity");
            DocNote bNow;
            const bool bResolved = crossingDoc.findNote(bId, &bNow);
            expect(bResolved && bNow.key == 61, "B did not re-resolve by identity");
            if (bResolved) {
                const uint64_t beforeSecond = crossingDoc.revision();
                crossingDoc.moveNotes({bNow}, 0, -1, true); // B away: NOT a merge
                expect(crossingDoc.undoStack()->count() == countBefore + 2 &&
                           crossingDoc.revision() == beforeSecond + 1 && hasState(61, 60),
                       "crossing mergeable moves merged distinct note identities");
                const uint64_t beforeUndo = crossingDoc.revision();
                crossingDoc.undoStack()->undo();
                expect(crossingDoc.revision() == beforeUndo + 1 && hasState(61, 61),
                       "crossing move undo did not restore exact identities");
                const uint64_t beforeSecondUndo = crossingDoc.revision();
                crossingDoc.undoStack()->undo();
                expect(crossingDoc.revision() == beforeSecondUndo + 1 && hasState(60, 61),
                       "first crossing move undo did not restore exact identities");
                const uint64_t beforeRedo = crossingDoc.revision();
                crossingDoc.undoStack()->redo();
                expect(crossingDoc.revision() == beforeRedo + 1 && hasState(61, 61),
                       "first crossing move redo did not preserve exact identities");
                const uint64_t beforeSecondRedo = crossingDoc.revision();
                crossingDoc.undoStack()->redo();
                expect(crossingDoc.revision() == beforeSecondRedo + 1 && hasState(61, 60),
                       "crossing move redo did not preserve exact identities");
            }
        }
    }

    // Merged-move publication and the away-and-back rule: every public
    // moveNotes call and every undo/redo publishes exactly once (no remap —
    // note moves never change ownership), a merge never publishes its
    // provisional pre-merge state, and a merged move that returns every
    // note to its origin removes the command and restores the exact bytes.
    if (ok) {
        SmfFile moves;
        moves.format = 1;
        moves.division = 24;
        SmfTrack moveTrack;
        moveTrack.events.push_back(chEvent(0xC0, 0, 1, 0));
        moveTrack.events.push_back(chEvent(0x90, 0, 70, 100)); // S 0..4
        moveTrack.events.push_back(chEvent(0x90, 0, 69, 100)); // M 0..2
        moveTrack.events.push_back(chEvent(0x80, 2, 69, 0));
        moveTrack.events.push_back(chEvent(0x80, 4, 70, 0));
        moveTrack.endTick = 8;
        moves.tracks.push_back(moveTrack);
        const QString movesPath = tmp.path() + QStringLiteral("/merge-publication.mid");
        SongInfo movesInfo = info;
        movesInfo.label = QStringLiteral("merge publication");
        movesInfo.midPath = movesPath;
        SongDocument movesDoc;
        const bool loaded = moves.writeFile(movesPath, &error) && movesDoc.load(movesInfo, &error);
        expect(loaded, "could not load the merged-move fixture");
        if (!loaded)
            return failures;
        std::vector<QString> moveOrder;
        std::vector<TrackRemap> moveRemaps;
        QObject::connect(&movesDoc, &SongDocument::tracksRemapped,
                         [&moveOrder, &moveRemaps](TrackRemap remap) {
                             moveOrder.push_back(QStringLiteral("remap"));
                             moveRemaps.push_back(std::move(remap));
                         });
        QObject::connect(&movesDoc, &SongDocument::documentChanged,
                         [&moveOrder] { moveOrder.push_back(QStringLiteral("changed")); });
        auto clearMoveSignals = [&moveOrder, &moveRemaps] {
            moveOrder.clear();
            moveRemaps.clear();
        };
        auto expectOnePublication = [&](uint64_t before, const char *what) {
            expect(movesDoc.revision() == before + 1 && moveRemaps.empty() &&
                       moveOrder == std::vector<QString>{QStringLiteral("changed")},
                   what);
        };
        const int countBefore = movesDoc.undoStack()->count();
        const QByteArray reversalBaseline = movesDoc.smf().write();
        DocNote moved, survivor;
        if (!movesDoc.findNote(0, 0, 69, &moved)) {
            fail("merged-move fixture did not resolve the moved note");
            ok = false;
        }
        if (ok) {
            clearMoveSignals();
            const uint64_t before = movesDoc.revision();
            movesDoc.moveNotes({moved}, 0, 1, true); // M onto S: S trimmed
            expect(movesDoc.undoStack()->count() == countBefore + 1,
                   "initial mergeable move did not push its command");
            expectOnePublication(before, "initial mergeable move did not publish exactly once");
            if (!movesDoc.findNote(0, 0, 70, &moved)) {
                fail("initial mergeable move did not resolve its output");
                ok = false;
            }
        }
        if (ok) {
            clearMoveSignals();
            const uint64_t before = movesDoc.revision();
            movesDoc.moveNotes({moved}, 0, -1, true); // back home: net zero
            expectOnePublication(before,
                                 "inverse net-zero mergeable move did not publish exactly once");
            expect(movesDoc.undoStack()->count() == countBefore &&
                       !movesDoc.undoStack()->canUndo() && !movesDoc.undoStack()->canRedo() &&
                       movesDoc.smf().write() == reversalBaseline,
                   "net-zero merged move kept an undo entry or did not restore its original MIDI");
            clearMoveSignals();
            const uint64_t afterInverse = movesDoc.revision();
            movesDoc.undoStack()->undo();
            movesDoc.undoStack()->redo();
            expect(movesDoc.smf().write() == reversalBaseline &&
                       movesDoc.revision() == afterInverse && moveOrder.empty(),
                   "undo or redo after a net-zero merged move mutated the document");
        }
        if (ok && !movesDoc.findNote(0, 0, 69, &moved)) {
            fail("net-zero merged move did not restore its original note");
            ok = false;
        }
        if (ok) {
            clearMoveSignals();
            const uint64_t before = movesDoc.revision();
            movesDoc.moveNotes({moved}, 0, 1, true); // M onto S again
            expectOnePublication(before, "restarted mergeable move did not publish exactly once");
            expect(movesDoc.findNote(0, 0, 70, &moved) && movesDoc.findNote(0, 2, 70, &survivor) &&
                       survivor.duration == 2,
                   "restarted mergeable move did not create its overlap state");
        }
        if (ok) {
            clearMoveSignals();
            const uint64_t before = movesDoc.revision();
            movesDoc.moveNotes({moved}, 0, 1, true); // merged: re-lands from origin
            expect(movesDoc.undoStack()->count() == countBefore + 1,
                   "second mergeable move did not merge");
            expectOnePublication(before,
                                 "second mergeable move published its provisional overlap state");
            expect(movesDoc.findNote(0, 0, 71, &moved) && movesDoc.findNote(0, 0, 70, &survivor) &&
                       survivor.duration == 4,
                   "second mergeable move did not publish its final combined state");
        }
        if (ok) {
            clearMoveSignals();
            const uint64_t before = movesDoc.revision();
            movesDoc.moveNotes({moved}, 0, 1, true);
            expect(movesDoc.undoStack()->count() == countBefore + 1,
                   "later mergeable move did not merge");
            expectOnePublication(before,
                                 "later mergeable move published its provisional overlap state");
            expect(movesDoc.findNote(0, 0, 72, &moved) && movesDoc.findNote(0, 0, 70, &survivor) &&
                       survivor.duration == 4,
                   "later mergeable move did not publish its final combined state");
        }
        if (ok) {
            clearMoveSignals();
            const uint64_t before = movesDoc.revision();
            movesDoc.undoStack()->undo();
            expectOnePublication(before, "merged move undo did not publish exactly once");
            expect(movesDoc.findNote(0, 0, 69, &moved) && movesDoc.findNote(0, 0, 70, &survivor) &&
                       survivor.duration == 4,
                   "merged move undo did not restore its start");
        }
        if (ok) {
            clearMoveSignals();
            const uint64_t before = movesDoc.revision();
            movesDoc.undoStack()->redo();
            expectOnePublication(before, "merged move redo did not publish exactly once");
            expect(movesDoc.findNote(0, 0, 72, &moved),
                   "merged move redo did not restore its final state");
        }
        if (ok) {
            clearMoveSignals();
            const uint64_t before = movesDoc.revision();
            movesDoc.moveNotes({moved}, 0, 1); // not mergeable
            expectOnePublication(before, "ordinary move did not publish exactly once");
            clearMoveSignals();
            const uint64_t undoBefore = movesDoc.revision();
            movesDoc.undoStack()->undo();
            expectOnePublication(undoBefore, "ordinary move undo did not publish exactly once");
            clearMoveSignals();
            const uint64_t redoBefore = movesDoc.revision();
            movesDoc.undoStack()->redo();
            expectOnePublication(redoBefore, "ordinary move redo did not publish exactly once");
        }
    }
    return failures;
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
                if (!doc.findNote(track, base + step * 8, 63, &note) || note.duration != step * 6) {
                    fail("resize produced wrong duration");
                    ok = false;
                }
            }
            if (ok) {
                // Left resize: the note-on moves, the note-off stays pinned.
                doc.resizeNotesLeft({note}, -int64_t(step) * 2);
                mutateAndCheck("events unsorted after resizeNotesLeft");
                if (!doc.findNote(track, base + step * 6, 63, &note) || note.duration != step * 8) {
                    fail("left resize produced wrong start/duration");
                    ok = false;
                }
            }
            if (ok) {
                // Dragging the note-on past the note-off clamps to 1 tick left.
                doc.resizeNotesLeft({note}, int64_t(step) * 100);
                mutateAndCheck("events unsorted after clamped resizeNotesLeft");
                if (!doc.findNote(track, base + step * 14 - 1, 63, &note) || note.duration != 1) {
                    fail("left resize not clamped at the note-off");
                    ok = false;
                } else {
                    doc.resizeNotesLeft({note}, -int64_t(step) * 8 + 1);
                    if (!doc.findNote(track, base + step * 6, 63, &note) ||
                        note.duration != step * 8) {
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
                if (!doc.findNote(track, base + step * 20, 64, &a) ||
                    !doc.findNote(track, base + step * 22, 67, &b)) {
                    fail("batch-added notes not found");
                    ok = false;
                } else {
                    doc.undoStack()->undo();
                    if (doc.findNote(track, base + step * 20, 64, &a) ||
                        doc.findNote(track, base + step * 22, 67, &b)) {
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
                if (!doc.findNote(track, seam - step * 2, 60, &leftNote) ||
                    !doc.findNote(track, seam, 60, &rightNote) || leftNote.duration != step * 2 ||
                    rightNote.duration != step * 2 || leftNote.endIndex == rightNote.endIndex) {
                    fail("abutting notes mis-paired (note end after same-tick note-on)");
                    ok = false;
                } else {
                    doc.deleteNotes({leftNote, rightNote});
                    bool leftover = false;
                    for (const SmfEvent &ev : doc.smf().tracks[size_t(leftNote.smfTrack)].events) {
                        leftover |= ev.tick >= seam - step * 2 && ev.isChannel() &&
                                    (ev.isNoteOn() || ev.isNoteEnd());
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
                if (doc.findNote(track, base + step * 30, 60, &n) ||
                    doc.findNote(track, base + step * 32, 62, &n) ||
                    !doc.findNote(track, base + step * 40, 65, &n) ||
                    !doc.findLanePoint(track, 7, base + step * 40, &p) || p.value != 70 ||
                    !doc.findLanePoint(track, DOC_CC_TEMPO, base + step * 41, &p) ||
                    p.value != 155) {
                    fail("range edit produced wrong content");
                    ok = false;
                } else {
                    doc.undoStack()->undo();
                    if (!doc.findNote(track, base + step * 30, 60, &n) ||
                        doc.findNote(track, base + step * 40, 65, &n)) {
                        fail("applyRangeEdit was not a single undo command");
                        ok = false;
                    } else {
                        doc.undoStack()->redo();
                    }
                }
            }

            // Stripping EVERY channel event from a track (a time-selection
            // delete from tick 0 takes the voice seed too) must not make the
            // track vanish from the engine map: the edit re-seeds a tick-0
            // program change carrying the voice the track had.
            if (ok && !doc.canAddTrack()) {
                fail("keep-alive check needs a free track slot");
                ok = false;
            }
            if (ok) {
                const int before = doc.engineTrackCount();
                const int t = doc.addTrack(42);
                // Deleting the lone tick-0 seed of an empty track would only
                // re-seed it: the document must stay untouched, no command.
                const int undoCount = doc.undoStack()->count();
                doc.deleteLanePoints(t, DOC_CC_VOICE, doc.lanePoints(t, DOC_CC_VOICE));
                DocLanePoint p;
                if (doc.undoStack()->count() != undoCount || doc.smfTrackFor(t) < 0 ||
                    !doc.findLanePoint(t, DOC_CC_VOICE, 0, &p) || p.value != 42) {
                    fail("deleting an empty track's lone voice seed was not a no-op");
                    ok = false;
                }
                doc.addNotes(t, {{base, 60, step, 90}});
                doc.addLanePoint(t, 7, base, 80);
                SongDocument::RangeEdit edit;
                for (const DocNote &n : doc.notesForTrack(t))
                    edit.removeNotes.push_back(n);
                for (uint8_t cc : {uint8_t(7), DOC_CC_VOICE})
                    for (const DocLanePoint &pt : doc.lanePoints(t, cc))
                        edit.removePoints.push_back(pt);
                doc.applyRangeEdit(QStringLiteral("delete range"), edit);
                mutateAndCheck("events unsorted after emptying range edit");
                if (doc.engineTrackCount() != before + 1 || doc.smfTrackFor(t) < 0) {
                    fail("range delete of all channel events dropped the track");
                    ok = false;
                } else if (!doc.notesForTrack(t).empty() || !doc.lanePoints(t, 7).empty() ||
                           !doc.findLanePoint(t, DOC_CC_VOICE, 0, &p) || p.value != 42) {
                    fail("emptied track did not keep a tick-0 voice re-seed");
                    ok = false;
                } else {
                    // Same guarantee through deleteNotes, on a track whose
                    // only channel events are notes (the re-seed itself).
                    for (const DocLanePoint &vp : doc.lanePoints(t, DOC_CC_VOICE))
                        doc.addNotes(t, {{vp.tick + step, 64, step, 90}});
                    std::vector<DocLanePoint> seeds = doc.lanePoints(t, DOC_CC_VOICE);
                    doc.deleteLanePoints(t, DOC_CC_VOICE, seeds);
                    if (doc.smfTrackFor(t) < 0 || doc.findLanePoint(t, DOC_CC_VOICE, 0, &p)) {
                        fail("deleting the voice seed beside a note re-seeded (it should not)");
                        ok = false;
                    }
                    // A program change later in the track is not its initial
                    // voice: the re-seed must not promote it to tick 0.
                    doc.addLanePoint(t, DOC_CC_VOICE, base + step * 3, 30);
                    doc.deleteNotes(doc.notesForTrack(t));
                    for (const DocLanePoint &vp : doc.lanePoints(t, DOC_CC_VOICE))
                        doc.deleteLanePoints(t, DOC_CC_VOICE, {vp});
                    if (doc.engineTrackCount() != before + 1 || doc.smfTrackFor(t) < 0 ||
                        !doc.findLanePoint(t, DOC_CC_VOICE, 0, &p) || p.value != 0 ||
                        doc.lanePoints(t, DOC_CC_VOICE).size() != 1) {
                        fail("deleteNotes/deleteLanePoints of the last channel events dropped "
                             "the track or re-seeded a later voice");
                        ok = false;
                    }
                    // Undo unwinds the re-seed along with the delete (two
                    // commands: the note delete, then the voice delete).
                    doc.undoStack()->undo();
                    doc.undoStack()->undo();
                    if (doc.notesForTrack(t).size() != 1 ||
                        doc.findLanePoint(t, DOC_CC_VOICE, 0, &p) ||
                        doc.lanePoints(t, DOC_CC_VOICE).size() != 1) {
                        fail("undo after keep-alive delete did not restore the note / drop the "
                             "re-seed");
                        ok = false;
                    }
                    doc.undoStack()->redo();
                    doc.undoStack()->redo();
                    // A range delete on a track kept alive by an event the
                    // view has no lane for (portamento, CC5) must still keep
                    // the tick-0 voice — unless the edit writes its own.
                    doc.addNotes(t, {{base, 60, step, 90}});
                    doc.addLanePoint(t, 5, base + step, 3);
                    doc.deleteLanePoints(t, DOC_CC_VOICE, doc.lanePoints(t, DOC_CC_VOICE));
                    doc.addLanePoint(t, DOC_CC_VOICE, 0, 42);
                    SongDocument::RangeEdit sweep;
                    for (const DocNote &n : doc.notesForTrack(t))
                        sweep.removeNotes.push_back(n);
                    for (const DocLanePoint &vp : doc.lanePoints(t, DOC_CC_VOICE))
                        sweep.removePoints.push_back(vp);
                    doc.applyRangeEdit(QStringLiteral("delete range"), sweep);
                    if (!doc.notesForTrack(t).empty() || doc.lanePoints(t, 5).size() != 1 ||
                        doc.lanePoints(t, DOC_CC_VOICE).size() != 1 ||
                        !doc.findLanePoint(t, DOC_CC_VOICE, 0, &p) || p.value != 42) {
                        fail("range delete over a CC5-kept track lost the tick-0 voice");
                        ok = false;
                    }
                    SongDocument::RangeEdit over;
                    for (const DocLanePoint &vp : doc.lanePoints(t, DOC_CC_VOICE))
                        over.removePoints.push_back(vp);
                    over.addPoints.push_back({t, DOC_CC_VOICE, {{0, 50}}});
                    doc.applyRangeEdit(QStringLiteral("paste range"), over);
                    if (doc.lanePoints(t, DOC_CC_VOICE).size() != 1 ||
                        !doc.findLanePoint(t, DOC_CC_VOICE, 0, &p) || p.value != 50) {
                        fail("range paste of a tick-0 voice did not replace the seed");
                        ok = false;
                    }
                    doc.deleteLanePoints(t, 5, doc.lanePoints(t, 5));
                    doc.deleteLanePoints(t, DOC_CC_VOICE, doc.lanePoints(t, DOC_CC_VOICE));
                    // Ripple delete of the whole track content re-seeds too.
                    doc.addNotes(t, {{base, 60, step, 90}});
                    doc.deleteLanePoints(t, DOC_CC_VOICE, doc.lanePoints(t, DOC_CC_VOICE));
                    SongDocument::RippleScope scope;
                    scope.tracks = {t};
                    if (!doc.removeTimeRange(0, base + step * 8, scope)) {
                        fail("keep-alive ripple delete reported nothing to do");
                        ok = false;
                    }
                    if (doc.smfTrackFor(t) < 0 || !doc.notesForTrack(t).empty() ||
                        !doc.findLanePoint(t, DOC_CC_VOICE, 0, &p) || p.value != 0) {
                        fail("ripple delete of the last channel events dropped the track");
                        ok = false;
                    }
                }
                if (ok)
                    doc.deleteTrack(t);
                if (ok && doc.engineTrackCount() != before) {
                    fail("keep-alive track could not be deleted normally");
                    ok = false;
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
                if (doc.findNote(track, base + step * 80, 60, &n) ||
                    !doc.findNote(track, base + step * 83, 60, &n) || n.duration != step * 2 ||
                    !doc.findNote(track, base + step * 85, 64, &n) ||
                    !doc.findLanePoint(track, 7, base + step * 83, &p) || p.value != 45 ||
                    !doc.findLanePoint(track, DOC_CC_TEMPO, base + step * 84, &p) ||
                    p.value != 140) {
                    fail("range move produced wrong content");
                    ok = false;
                }
                if (ok) {
                    doc.moveRange(moveNotes, movePoints, 0); // no-op guard
                    doc.undoStack()->undo();
                    if (!doc.findNote(track, base + step * 80, 60, &n) ||
                        doc.findNote(track, base + step * 83, 60, &n) ||
                        !doc.findLanePoint(track, 7, base + step * 80, &p)) {
                        fail("moveRange was not a single undo command");
                        ok = false;
                    } else {
                        doc.undoStack()->redo();
                    }
                }
                // Range duplicate: the same set copied by a delta as ONE
                // command, originals intact, copies with fresh identities.
                if (ok) {
                    std::vector<DocNote> dupNotes;
                    for (const DocNote &dn : doc.notesForTrack(track)) {
                        if (dn.tick >= base + step * 83 && dn.tick < base + step * 87)
                            dupNotes.push_back(dn);
                    }
                    std::vector<DocLanePoint> dupPoints;
                    for (const DocLanePoint &dp : doc.lanePoints(track, 7)) {
                        if (dp.tick == base + step * 83)
                            dupPoints.push_back(dp);
                    }
                    const int dupBefore = doc.undoStack()->count();
                    doc.duplicateRange(dupNotes, dupPoints, step * 200);
                    mutateAndCheck("events unsorted after duplicateRange");
                    DocNote copy;
                    if (!doc.findNote(track, base + step * 83, 60, &n) ||
                        !doc.findNote(track, base + step * 283, 60, &copy) ||
                        copy.duration != step * 2 || copy.noteId == n.noteId ||
                        !doc.findNote(track, base + step * 285, 64, &copy) ||
                        !doc.findLanePoint(track, 7, base + step * 83, &p) ||
                        !doc.findLanePoint(track, 7, base + step * 283, &p) || p.value != 45 ||
                        doc.undoStack()->count() != dupBefore + 1) {
                        fail("duplicateRange produced wrong content");
                        ok = false;
                    } else {
                        doc.duplicateRange(dupNotes, dupPoints, 0); // no-op guard
                        doc.undoStack()->undo();
                        if (doc.findNote(track, base + step * 283, 60, &copy) ||
                            !doc.findNote(track, base + step * 83, 60, &n)) {
                            fail("duplicateRange was not a single undo command");
                            ok = false;
                        } else {
                            doc.undoStack()->redo();
                        }
                    }
                }
                // Duplicate landing on its own source: the copy trims the
                // original like a drawn note would (the pairing rule cannot
                // hold a same-key overlap) — one command, undo restores it.
                if (ok) {
                    std::vector<DocNote> self;
                    DocNote src;
                    if (!doc.findNote(track, base + step * 83, 60, &src)) {
                        fail("self-overlap source missing");
                        ok = false;
                    } else {
                        self.push_back(src);
                        const int selfBefore = doc.undoStack()->count();
                        doc.duplicateRange(self, {}, step);
                        mutateAndCheck("events unsorted after self-overlapping duplicateRange");
                        DocNote head, copy;
                        if (!doc.findNote(track, base + step * 83, 60, &head) ||
                            head.duration != step || head.noteId != src.noteId ||
                            !doc.findNote(track, base + step * 84, 60, &copy) ||
                            copy.duration != step * 2 || copy.noteId == src.noteId ||
                            doc.undoStack()->count() != selfBefore + 1) {
                            fail("self-overlapping duplicateRange did not trim the source");
                            ok = false;
                        } else {
                            doc.undoStack()->undo();
                            if (!doc.findNote(track, base + step * 83, 60, &head) ||
                                head.duration != step * 2 ||
                                doc.findNote(track, base + step * 84, 60, &copy)) {
                                fail("self-overlapping duplicateRange undo did not restore");
                                ok = false;
                            }
                        }
                    }
                }
                // A run of equal notes duplicated by one step: the copies
                // that coincide with existing notes are not written (those
                // notes keep their identities); only the new one lands.
                if (ok) {
                    doc.addNotes(track, {{base + step * 300, 62, step, 90},
                                         {base + step * 301, 62, step, 90},
                                         {base + step * 302, 62, step, 90}});
                    std::vector<DocNote> run;
                    for (const DocNote &rn : doc.notesForTrack(track)) {
                        if (rn.key == 62 && rn.tick >= base + step * 300 &&
                            rn.tick < base + step * 303)
                            run.push_back(rn);
                    }
                    const int runBefore = doc.undoStack()->count();
                    doc.duplicateRange(run, {}, step);
                    mutateAndCheck("events unsorted after coinciding duplicateRange");
                    int count = 0;
                    bool idsKept = true;
                    for (const DocNote &rn : doc.notesForTrack(track)) {
                        if (rn.key != 62 || rn.tick < base + step * 300)
                            continue;
                        count++;
                        for (const DocNote &orig : run) {
                            if (orig.tick == rn.tick && orig.noteId != rn.noteId)
                                idsKept = false;
                        }
                    }
                    DocNote tail;
                    if (run.size() != 3 || count != 4 || !idsKept ||
                        !doc.findNote(track, base + step * 303, 62, &tail) ||
                        tail.duration != step || doc.undoStack()->count() != runBefore + 1) {
                        fail("coinciding duplicateRange rewrote the notes it landed on");
                        ok = false;
                    } else {
                        doc.undoStack()->undo(); // the duplicate
                        doc.undoStack()->undo(); // the run
                    }
                }
                // Lane points landing on an occupied tick replace the point
                // there (addLanePoint's same-tick rule), for a duplicate and
                // a move alike — never a shadowing same-tick pair.
                if (ok) {
                    doc.addLanePoint(track, 7, base + step * 310, 20);
                    doc.addLanePoint(track, 7, base + step * 312, 90);
                    std::vector<DocLanePoint> lp;
                    for (const DocLanePoint &dp : doc.lanePoints(track, 7)) {
                        if (dp.tick == base + step * 310)
                            lp.push_back(dp);
                    }
                    auto pointsAt = [&](uint64_t tick, int *value) {
                        int found = 0;
                        for (const DocLanePoint &dp : doc.lanePoints(track, 7)) {
                            if (dp.tick == tick) {
                                found++;
                                *value = dp.value;
                            }
                        }
                        return found;
                    };
                    int value = -1;
                    doc.duplicateRange({}, lp, step * 2);
                    mutateAndCheck("events unsorted after lane-landing duplicateRange");
                    if (lp.size() != 1 || pointsAt(base + step * 312, &value) != 1 || value != 20 ||
                        pointsAt(base + step * 310, &value) != 1 || value != 20) {
                        fail("duplicateRange left a same-tick lane duplicate");
                        ok = false;
                    } else {
                        doc.undoStack()->undo();
                        if (pointsAt(base + step * 312, &value) != 1 || value != 90) {
                            fail("lane-landing duplicateRange undo did not restore");
                            ok = false;
                        }
                    }
                    if (ok) {
                        doc.moveRange({}, lp, step * 2);
                        mutateAndCheck("events unsorted after lane-landing moveRange");
                        if (pointsAt(base + step * 312, &value) != 1 || value != 20 ||
                            pointsAt(base + step * 310, &value) != 0) {
                            fail("moveRange left a same-tick lane duplicate");
                            ok = false;
                        } else {
                            doc.undoStack()->undo();
                            if (pointsAt(base + step * 312, &value) != 1 || value != 90 ||
                                pointsAt(base + step * 310, &value) != 1 || value != 20) {
                                fail("lane-landing moveRange undo did not restore");
                                ok = false;
                            }
                        }
                    }
                    if (ok) {
                        doc.undoStack()->undo(); // point at 312
                        doc.undoStack()->undo(); // point at 310
                    }
                }
            }

            // Bulk lane-point move: several automation points shift by
            // independent tick/value deltas as ONE undoable command, and two
            // points converging on one tick resolve last-input-wins.
            if (ok) {
                doc.addLanePoint(track, 7, base + step * 90, 20);
                doc.addLanePoint(track, 7, base + step * 91, 40);
                doc.addLanePoint(track, 7, base + step * 92, 60);
                std::vector<SongDocument::LanePointMove> moves;
                for (const DocLanePoint &p : doc.lanePoints(track, 7)) {
                    if (p.tick == base + step * 90)
                        moves.push_back({track, 7, p, base + step * 93, 25});
                    else if (p.tick == base + step * 91)
                        moves.push_back({track, 7, p, base + step * 94, 45});
                }
                const int undoBefore = doc.undoStack()->count();
                doc.moveLanePoints(moves);
                mutateAndCheck("events unsorted after moveLanePoints");
                DocLanePoint p;
                if (doc.findLanePoint(track, 7, base + step * 90, &p) ||
                    doc.findLanePoint(track, 7, base + step * 91, &p) ||
                    !doc.findLanePoint(track, 7, base + step * 93, &p) || p.value != 25 ||
                    !doc.findLanePoint(track, 7, base + step * 94, &p) || p.value != 45 ||
                    !doc.findLanePoint(track, 7, base + step * 92, &p) || p.value != 60 ||
                    doc.undoStack()->count() != undoBefore + 1) {
                    fail("moveLanePoints produced wrong content or undo count");
                    ok = false;
                } else {
                    doc.undoStack()->undo();
                    if (!doc.findLanePoint(track, 7, base + step * 90, &p) || p.value != 20 ||
                        !doc.findLanePoint(track, 7, base + step * 91, &p) || p.value != 40 ||
                        doc.findLanePoint(track, 7, base + step * 93, &p)) {
                        fail("moveLanePoints was not a single undo command");
                        ok = false;
                    } else {
                        doc.undoStack()->redo();
                    }
                }
                if (ok) {
                    doc.addLanePoint(track, 7, base + step * 95, 70);
                    doc.addLanePoint(track, 7, base + step * 96, 80);
                    DocLanePoint first, second;
                    if (!doc.findLanePoint(track, 7, base + step * 95, &first) ||
                        !doc.findLanePoint(track, 7, base + step * 96, &second)) {
                        fail("converging move fixture was not created");
                        ok = false;
                    } else {
                        const uint64_t destination = base + step * 97;
                        const int convergeBefore = doc.undoStack()->count();
                        doc.moveLanePoints({{track, 7, first, destination, 71},
                                            {track, 7, second, destination, 72}});
                        mutateAndCheck("events unsorted after converging moveLanePoints");
                        int destinationCount = 0;
                        int destinationValue = -1;
                        for (const DocLanePoint &point : doc.lanePoints(track, 7)) {
                            if (point.tick == destination) {
                                destinationCount++;
                                destinationValue = point.value;
                            }
                        }
                        if (doc.findLanePoint(track, 7, base + step * 95, &p) ||
                            doc.findLanePoint(track, 7, base + step * 96, &p) ||
                            destinationCount != 1 || destinationValue != 72 ||
                            doc.undoStack()->count() != convergeBefore + 1) {
                            fail("converging move was not last-input-wins");
                            ok = false;
                        } else {
                            doc.undoStack()->undo();
                            if (!doc.findLanePoint(track, 7, base + step * 95, &p) ||
                                p.value != 70 ||
                                !doc.findLanePoint(track, 7, base + step * 96, &p) ||
                                p.value != 80 || doc.findLanePoint(track, 7, destination, &p)) {
                                fail("converging move undo did not restore both points");
                                ok = false;
                            } else {
                                doc.undoStack()->redo();
                            }
                        }
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
                    if (!doc.findNote(track, base + step * 88, 71, &m) || m.duration != step * 4 ||
                        !doc.findNote(track, base + step * 92, 71, &s) || s.duration != step * 2) {
                        fail("transpose onto a note's head did not keep its tail");
                        ok = false;
                    }
                }
                // Fully covered: M resized right across all of S removes it.
                if (ok) {
                    doc.resizeNotes({m}, step * 4); // M 88..96 covers S 92..94
                    mutateAndCheck("events unsorted after overlap resize");
                    if (!doc.findNote(track, base + step * 88, 71, &m) || m.duration != step * 8 ||
                        doc.findNote(track, base + step * 92, 71, &s)) {
                        fail("resize across a covered note did not remove it");
                        ok = false;
                    }
                }
                // Head kept: a note drawn over M's tail trims M back, and one
                // undo reverts the trim together with the add.
                if (ok) {
                    doc.addNote(track, base + step * 94, 71, step * 4, 100);
                    mutateAndCheck("events unsorted after overlapping addNote");
                    if (!doc.findNote(track, base + step * 88, 71, &m) || m.duration != step * 6 ||
                        !doc.findNote(track, base + step * 94, 71, &s) || s.duration != step * 4) {
                        fail("overlapping add did not trim the covered tail");
                        ok = false;
                    }
                }
                if (ok) {
                    doc.undoStack()->undo();
                    if (!doc.findNote(track, base + step * 88, 71, &m) || m.duration != step * 8 ||
                        doc.findNote(track, base + step * 94, 71, &s)) {
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
                    if (!doc.findNote(track, base + step * 102, 70, &s) || s.duration != step * 2) {
                        fail("mergeable transpose did not trim the overlap");
                        ok = false;
                    }
                }
                if (ok) {
                    doc.findNote(track, base + step * 100, 70, &m); // re-resolve
                    doc.moveNotes({m}, 0, 1, true);                 // past S: merged, +2 total
                    if (doc.undoStack()->count() != countBefore + 1) {
                        fail("consecutive mergeable moves did not merge");
                        ok = false;
                    } else if (!doc.findNote(track, base + step * 100, 71, &m) ||
                               m.duration != step * 2 ||
                               !doc.findNote(track, base + step * 100, 70, &s) ||
                               s.duration != step * 4) {
                        fail("merged transpose did not restore the trimmed note");
                        ok = false;
                    }
                }
                if (ok) {
                    doc.undoStack()->undo();
                    if (!doc.findNote(track, base + step * 100, 69, &m) ||
                        !doc.findNote(track, base + step * 100, 70, &s) || s.duration != step * 4) {
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
                if (ok && (!doc.findNote(track, base + step * 50, 60, &n) ||
                           doc.findNote(track, base + step * 52, 62, &n) ||
                           !doc.findNote(track, base + step * 53, 64, &n) ||
                           !doc.findLanePoint(track, 7, base + step * 51, &p) || p.value != 40)) {
                    fail("ripple remove produced wrong content");
                    ok = false;
                }
                if (ok) {
                    doc.undoStack()->undo();
                    if (!doc.findNote(track, base + step * 56, 64, &n) ||
                        !doc.findLanePoint(track, 7, base + step * 52, &p) || p.value != 40) {
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
                if (ok &&
                    (!sigAtSeam || !doc.findLanePoint(track, DOC_CC_TEMPO, base + step * 61, &p) ||
                     p.value != 150 || !doc.findNote(track, base + step * 62, 65, &n) ||
                     maxEnd() != endBefore - step * 4 || doc.loopTick(false) != loopStartBefore)) {
                    fail("whole-song ripple produced wrong content");
                    ok = false;
                }
                if (ok) {
                    doc.undoStack()->undo();
                    if (!doc.findNote(track, base + step * 66, 65, &n) || maxEnd() != endBefore) {
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
                if (!doc.findLanePoint(track, DOC_CC_VOICE, base + step, &vc) || vc.value != 5) {
                    fail("voice change not found after add");
                    ok = false;
                } else {
                    doc.moveLanePoint(track, DOC_CC_VOICE, vc, vc.tick, 9);
                    if (!doc.findLanePoint(track, DOC_CC_VOICE, base + step, &vc) ||
                        vc.value != 9) {
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
                    same = copyNotes[i].tick == srcNotes[i].tick &&
                           copyNotes[i].key == srcNotes[i].key &&
                           copyNotes[i].duration == srcNotes[i].duration &&
                           copyNotes[i].velocity == srcNotes[i].velocity;
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

        // Reordering tracks: the chunk moves with its events and channel
        // bytes untouched, and the seq globals — tempo, time signatures,
        // loop markers — stay with chunk 0 even when the move displaces it
        // (mid2agb and the tempo lane read them only there).
        if (ok && doc.engineTrackCount() >= 2 && track >= 0) {
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
            auto notesMatch = [&doc](int engineTrack, const std::vector<DocNote> &want) {
                const auto got = doc.notesForTrack(engineTrack);
                if (got.size() != want.size())
                    return false;
                for (size_t i = 0; i < got.size(); i++) {
                    if (got[i].tick != want[i].tick || got[i].key != want[i].key ||
                        got[i].duration != want[i].duration || got[i].velocity != want[i].velocity)
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
            if (ok && (!notesMatch(last, srcNotes) || doc.channelFor(last) != srcChannel)) {
                fail("moved track's notes or channel changed");
                ok = false;
            }
            if (ok &&
                (!seqChunkHas(0x51, base + step * 110) || !seqChunkHas(0x58, base + step * 112))) {
                fail("seq globals did not stay with chunk 0 across the move");
                ok = false;
            }
            if (ok &&
                (doc.loopTick(false) != loopStartBefore || doc.loopTick(true) != loopEndBefore)) {
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
                if (!notesMatch(0, srcNotes) || !seqChunkHas(0x51, base + step * 110) ||
                    !seqChunkHas(0x58, base + step * 112)) {
                    fail("moving the track back did not restore its slot");
                    ok = false;
                }
            }
        }

        // Reordering must not confuse chunk-0 metas that only LOOK like loop
        // markers: a first-0x03 name of "[" is the track's name and travels
        // with its chunk (findLoopMarkerEvent skips it; imported files can
        // carry such names even though renameTrack refuses them), while the
        // combined "][" marker mid2agb reads stays with chunk 0.
        if (ok && doc.engineTrackCount() >= 2 && doc.smfTrackFor(0) == 0) {
            const int last = doc.engineTrackCount() - 1;
            const int indexBefore = doc.undoStack()->index();
            const uint64_t loopStartBefore = doc.loopTick(false);
            const uint64_t loopEndBefore = doc.loopTick(true);
            doc.renameTrack(0, QString()); // the "[" below must be the first 0x03
            SmfEvent name;
            name.tick = 0;
            name.status = 0xFF;
            name.metaType = 0x03;
            name.blob = QByteArrayLiteral("[");
            doc.insertRawEvent(0, name);
            SmfEvent marker;
            marker.tick = base;
            marker.status = 0xFF;
            marker.metaType = 0x06;
            marker.blob = QByteArrayLiteral("][");
            doc.insertRawEvent(0, marker);
            doc.moveTrack(0, last);
            mutateAndCheck("events unsorted after marker-name moveTrack");
            if (ok && doc.trackName(last) != QStringLiteral("[")) {
                fail("a '['-named track lost its name in the move");
                ok = false;
            }
            if (ok &&
                (doc.loopTick(false) != loopStartBefore || doc.loopTick(true) != loopEndBefore)) {
                fail("a '[' track name was misread as a loop marker");
                ok = false;
            }
            bool combinedStayed = false;
            for (const SmfEvent &ev : doc.smf().tracks[0].events) {
                if (ev.isMeta() && ev.metaType == 0x06 && ev.blob == "][")
                    combinedStayed = true;
            }
            if (ok && !combinedStayed) {
                fail("the '][' marker left chunk 0 in the move");
                ok = false;
            }
            while (doc.undoStack()->index() > indexBefore)
                doc.undoStack()->undo();
            mutateAndCheck("events unsorted after marker-name undo");
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
            if (ok &&
                (doc.loopTick(false) != loopStartBefore || doc.loopTick(true) != loopEndBefore)) {
                fail("deleteTrack lost the loop markers");
                ok = false;
            }
            doc.undoStack()->undo();
        }

        // Track rename: set, no-op guard (trimmed match pushes nothing),
        // clear, and undo back through the chunk's Track Name meta (0x03).
        if (ok && track >= 0) {
            doc.renameTrack(track, QStringLiteral("editcheck name"));
            mutateAndCheck("events unsorted after renameTrack");
            if (ok && doc.trackName(track) != QStringLiteral("editcheck name")) {
                fail("rename not applied");
                ok = false;
            }
            // The header paints from the playable projection, not the raw
            // SMF — the new meta must land where MidiTimeline's reader
            // (first 0x03 in the chunk) finds it.
            if (ok) {
                const auto timeline = doc.buildTimeline(48000.0);
                if (!timeline || timeline->tracks[track].name != QStringLiteral("editcheck name")) {
                    fail("renamed track not visible in the timeline projection");
                    ok = false;
                }
            }
            if (ok) {
                const int count = doc.undoStack()->count();
                doc.renameTrack(track, QStringLiteral("  editcheck name  "));
                if (doc.undoStack()->count() != count) {
                    fail("no-op rename pushed an undo command");
                    ok = false;
                }
            }
            if (ok) {
                // mid2agb reads any text meta whose whole text is a marker
                // as a loop/label command; those names must be refused.
                const int count = doc.undoStack()->count();
                doc.renameTrack(track, QStringLiteral("["));
                doc.renameTrack(track, QStringLiteral(" ][ "));
                if (doc.undoStack()->count() != count ||
                    doc.trackName(track) != QStringLiteral("editcheck name")) {
                    fail("loop-marker name was not refused");
                    ok = false;
                }
            }
            if (ok) {
                doc.renameTrack(track, QString());
                if (!doc.trackName(track).isEmpty()) {
                    fail("empty rename did not clear the name");
                    ok = false;
                }
            }
            if (ok) {
                doc.undoStack()->undo();
                if (doc.trackName(track) != QStringLiteral("editcheck name")) {
                    fail("rename undo did not restore the name");
                    ok = false;
                } else {
                    doc.undoStack()->redo();
                }
            }
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
                if (!findSig(base, &sig) || sig.numerator != 7 || sig.denomPow2 != 2 ||
                    doc.timeSigs().size() != sigsBefore + 1) {
                    fail("time signature edit did not replace in place");
                    ok = false;
                }
            }
            if (ok) {
                doc.moveTimeSig(base, base + step * 4);
                mutateAndCheck("events unsorted after moveTimeSig");
                if (findSig(base, &sig) || !findSig(base + step * 4, &sig) || sig.numerator != 7) {
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

    // Format 0 is coerced to format 1 at load (convertToFormat1): the
    // single chunk splits into a conductor chunk 0 carrying every
    // non-channel meta, then one chunk per used channel in ascending
    // channel order — the order mid2agb emits agb tracks for a format-0
    // file, so the build output is unchanged (--roundtrip proves that end
    // to end). Channel-Prefix names (0x20 + 0x03) become ordinary chunk
    // names. Synthetic file — decomp projects are format 1 in practice.
    {
        auto fail0 = [&](const char *what) {
            std::fprintf(stderr, "editcheck: FAIL format0-convert: %s\n", what);
            failures++;
        };
        SmfFile smf;
        smf.format = 0;
        smf.division = 24;
        SmfTrack tr;
        auto chEvent = [](uint8_t status, uint64_t tick, uint8_t d0, uint8_t d1) {
            SmfEvent ev;
            ev.tick = tick;
            ev.status = status;
            ev.data0 = d0;
            ev.data1 = d1;
            return ev;
        };
        auto meta = [](uint64_t tick, uint8_t type, QByteArray blob) {
            SmfEvent ev;
            ev.tick = tick;
            ev.status = 0xFF;
            ev.metaType = type;
            ev.blob = std::move(blob);
            return ev;
        };
        // Global metas, a prefixed per-channel name, and notes on the
        // non-contiguous channels 1, 4, 7 (so slot order = channel order is
        // visible). The unprefixed 0x03 precedes the prefix — after a
        // channel prefix, names are scoped until the next channel event.
        // Three preservation cases ride along: a prefixed non-name text
        // meta (0x04 "Gtr" → travels to channel 4's chunk), a prefixed
        // MARKER-text 0x03 (":" → stays in the conductor chunk with a
        // prefix, where mid2agb reads markers), and a prefixed name on the
        // silent channel 9 ("Ambient" → a name-only chunk rather than
        // silent data loss).
        tr.events.push_back(meta(0, 0x51, QByteArray("\x07\xA1\x20", 3))); // 120 BPM
        tr.events.push_back(meta(0, 0x03, QByteArrayLiteral("Song")));
        tr.events.push_back(meta(0, 0x20, QByteArray(1, char(4))));
        tr.events.push_back(meta(0, 0x03, QByteArrayLiteral("Lead")));
        tr.events.push_back(meta(0, 0x04, QByteArrayLiteral("Gtr")));
        tr.events.push_back(chEvent(0x91, 0, 60, 100));
        tr.events.push_back(chEvent(0x94, 0, 64, 100));
        tr.events.push_back(chEvent(0x97, 0, 67, 100));
        tr.events.push_back(meta(12, 0x06, QByteArrayLiteral("[")));
        tr.events.push_back(meta(12, 0x20, QByteArray(1, char(7))));
        tr.events.push_back(meta(12, 0x03, QByteArrayLiteral(":")));
        tr.events.push_back(chEvent(0x81, 24, 60, 0));
        tr.events.push_back(chEvent(0x84, 24, 64, 0));
        tr.events.push_back(chEvent(0x87, 24, 67, 0));
        tr.events.push_back(meta(36, 0x06, QByteArrayLiteral("]")));
        tr.events.push_back(meta(36, 0x20, QByteArray(1, char(9))));
        tr.events.push_back(meta(36, 0x03, QByteArrayLiteral("Ambient")));
        tr.endTick = 48;
        smf.tracks.push_back(tr);
        const QByteArray originalBytes = smf.write();

        QTemporaryDir tmp;
        const QString midPath = tmp.path() + QStringLiteral("/format0.mid");
        QString werror;
        SongInfo info;
        info.label = QStringLiteral("format0");
        info.midPath = midPath;
        info.hasMid = true;
        SongDocument doc;
        bool ok = tmp.isValid() && smf.writeFile(midPath, &werror) && doc.load(info, &werror);
        if (!ok)
            fail0("could not write/load the synthetic format-0 file");
        if (ok && (doc.smf().format != 1 || !doc.smf().wasFormat0 || doc.smf().tracks.size() != 5 ||
                   doc.engineTrackCount() != 3)) {
            fail0("load did not split into conductor + one chunk per channel");
            ok = false;
        }
        if (ok && (doc.channelFor(0) != 1 || doc.channelFor(1) != 4 || doc.channelFor(2) != 7 ||
                   doc.smfTrackFor(0) != 1)) {
            fail0("converted chunks not in ascending channel order");
            ok = false;
        }
        if (ok) {
            const auto note = [&doc](int track) {
                const auto notes = doc.notesForTrack(track);
                return notes.size() == 1 && notes[0].duration == 24 ? int(notes[0].key) : -1;
            };
            if (note(0) != 60 || note(1) != 64 || note(2) != 67) {
                fail0("notes did not land on their channel's chunk");
                ok = false;
            }
        }
        if (ok && (doc.trackName(1) != QStringLiteral("Lead") || !doc.trackName(0).isEmpty() ||
                   !doc.trackName(2).isEmpty())) {
            fail0("the prefixed name did not become its channel chunk's name");
            ok = false;
        }
        if (ok) {
            auto hasMeta = [](const SmfTrack &track, uint8_t type, const char *text) {
                for (const SmfEvent &ev : track.events)
                    if (ev.isMeta() && ev.metaType == type && ev.blob == text)
                        return true;
                return false;
            };
            const auto &chunks = doc.smf().tracks;
            // Chunk layout: 0 conductor, 1..3 channels 1/4/7, 4 the
            // name-only channel-9 chunk.
            if (!hasMeta(chunks[2], 0x04, "Gtr")) {
                fail0("prefixed instrument-name meta did not travel to its channel chunk");
                ok = false;
            }
            if (ok && (!hasMeta(chunks[0], 0x03, ":") || hasMeta(chunks[3], 0x03, ":"))) {
                fail0("prefixed marker-text meta did not stay in the conductor chunk");
                ok = false;
            }
            if (ok) {
                // ...and it kept a prefix, so no reader mistakes it for the
                // conductor's name.
                bool prefixedMarker = false;
                const auto &evs = chunks[0].events;
                for (size_t i = 1; i < evs.size(); i++) {
                    if (evs[i].isMeta() && evs[i].metaType == 0x03 && evs[i].blob == ":" &&
                        evs[i - 1].isMeta() && evs[i - 1].metaType == 0x20)
                        prefixedMarker = true;
                }
                if (!prefixedMarker) {
                    fail0("the conductor's marker-text meta lost its prefix");
                    ok = false;
                }
            }
            if (ok && !hasMeta(chunks[4], 0x03, "Ambient")) {
                fail0("prefixed name on a silent channel was lost (no name-only chunk)");
                ok = false;
            }
            if (ok) {
                for (const SmfEvent &ev : chunks[4].events) {
                    if (ev.isChannel()) {
                        fail0("the name-only chunk grew channel events");
                        ok = false;
                    }
                }
            }
        }
        if (ok) {
            // Prefixes are rewritten into chunk structure everywhere except
            // the conductor's re-prefixed marker pair (the ":" case above).
            for (size_t t = 0; t < doc.smf().tracks.size(); t++) {
                const SmfTrack &track = doc.smf().tracks[t];
                for (const SmfEvent &ev : track.events) {
                    if (t > 0 && ev.isMeta() && ev.metaType == 0x20) {
                        fail0("a Channel Prefix meta survived in a channel chunk");
                        ok = false;
                    }
                }
                if (track.endTick != 48) {
                    fail0("a converted chunk lost the end-of-track tick");
                    ok = false;
                }
            }
            for (const SmfEvent &ev : doc.smf().tracks[0].events) {
                if (ev.isChannel()) {
                    fail0("a channel event landed in the conductor chunk");
                    ok = false;
                }
            }
        }
        if (ok && (doc.loopTick(false) != 12 || doc.loopTick(true) != 36 ||
                   doc.lanePoints(0, DOC_CC_TEMPO).size() != 1)) {
            fail0("seq globals did not stay readable in chunk 0");
            ok = false;
        }
        if (ok) {
            const auto timeline = doc.buildTimeline(48000.0);
            if (!timeline || timeline->usedTrackCount != 3 ||
                timeline->tracks[1].name != QStringLiteral("Lead") ||
                timeline->loopStartTick != 12) {
                fail0("conversion not reflected in the timeline projection");
                ok = false;
            }
        }
        if (ok && !tracksSorted(doc.smf())) {
            fail0("events unsorted after conversion");
            ok = false;
        }
        const QByteArray converted = ok ? doc.smf().write() : QByteArray();
        if (ok) {
            // SmfFile::read is the conversion choke point: re-reading the
            // original bytes yields the converted file directly, and doing
            // it twice proves determinism (an untouched file re-converts
            // identically next open). Converting the already-converted file
            // is a no-op (fixed point).
            SmfFile redo;
            QString rerror;
            if (!SmfFile::read(originalBytes, &redo, &rerror)) {
                fail0("could not re-read the original bytes");
                ok = false;
            } else {
                if (!redo.wasFormat0 || redo.write() != converted) {
                    fail0("read() did not coerce deterministically");
                    ok = false;
                }
                convertToFormat1(&redo);
                if (ok && redo.write() != converted) {
                    fail0("conversion of a converted file is not a no-op");
                    ok = false;
                }
            }
        }
        if (ok) {
            // The editing layer runs on the converted shape: undo-all
            // restores the converted baseline, not the format-0 bytes.
            doc.renameTrack(0, QStringLiteral("Bass"));
            doc.moveTrack(0, 2);
            if (doc.trackName(2) != QStringLiteral("Bass")) {
                fail0("edits after conversion did not behave as format 1");
                ok = false;
            }
            while (doc.undoStack()->canUndo())
                doc.undoStack()->undo();
            if (ok && doc.smf().write() != converted)
                fail0("undo-all did not restore the converted baseline");
        }
    }

    // A PREFIXED 0x03 carrying marker text has no name position (a chunk's
    // name is its first unprefixed 0x03), so every classifier
    // (MidiTimeline::build, findLoopMarkerEvent, trackNameLoc) reads it as
    // a marker — mid2agb's rule: a foreign format-1 file whose chunk opens
    // with a prefixed 0x03 "[" has a loop the playback timeline, the loop
    // UI, and the compiled ROM all agree on, and renaming the track edits
    // the real name meta, never the marker.
    {
        auto failM = [&](const char *what) {
            std::fprintf(stderr, "editcheck: FAIL marker-vs-name: %s\n", what);
            failures++;
        };
        auto chEvent = [](uint8_t status, uint64_t tick, uint8_t d0, uint8_t d1) {
            SmfEvent ev;
            ev.tick = tick;
            ev.status = status;
            ev.data0 = d0;
            ev.data1 = d1;
            return ev;
        };
        auto meta = [](uint64_t tick, uint8_t type, QByteArray blob) {
            SmfEvent ev;
            ev.tick = tick;
            ev.status = 0xFF;
            ev.metaType = type;
            ev.blob = std::move(blob);
            return ev;
        };
        SmfFile smf;
        smf.format = 1;
        smf.division = 24;
        SmfTrack tr;
        tr.events.push_back(meta(0, 0x20, QByteArray(1, char(0))));
        tr.events.push_back(meta(0, 0x03, QByteArrayLiteral("[")));
        tr.events.push_back(chEvent(0x90, 0, 60, 100)); // clears the prefix
        tr.events.push_back(meta(0, 0x03, QByteArrayLiteral("Real")));
        tr.events.push_back(chEvent(0x80, 24, 60, 0));
        tr.endTick = 24;
        smf.tracks.push_back(tr);

        QTemporaryDir tmp;
        const QString midPath = tmp.path() + QStringLiteral("/marker.mid");
        QString werror;
        SongInfo info;
        info.label = QStringLiteral("marker");
        info.midPath = midPath;
        info.hasMid = true;
        SongDocument doc;
        bool ok = tmp.isValid() && smf.writeFile(midPath, &werror) && doc.load(info, &werror);
        if (!ok)
            failM("could not write/load the synthetic file");
        if (ok && doc.trackName(0) != QStringLiteral("Real")) {
            failM("marker-text 0x03 was mistaken for the track name");
            ok = false;
        }
        if (ok && doc.loopTick(false) != 0) {
            failM("the loop UI did not see the prefixed marker");
            ok = false;
        }
        if (ok) {
            const auto timeline = doc.buildTimeline(48000.0);
            if (!timeline || timeline->loopStartTick != 0) {
                failM("playback did not see the prefixed marker (build/UI disagree)");
                ok = false;
            }
        }
        if (ok) {
            doc.renameTrack(0, QStringLiteral("Renamed"));
            if (doc.trackName(0) != QStringLiteral("Renamed") || doc.loopTick(false) != 0) {
                failM("rename clobbered the loop marker instead of the name");
            }
        }
    }

    // Same-tick duplicate setters (a foreign file's repeated channel-init
    // block): the loader preserves them — sanitizing is the import wizard's
    // job — but every editing surface resolves the run LAST-wins, matching
    // playback, and writing onto an occupied tick replaces what sits there
    // instead of stacking another duplicate. Undo restores the duplicates.
    {
        auto failD = [&](const char *what) {
            std::fprintf(stderr, "editcheck: FAIL same-tick-dup: %s\n", what);
            failures++;
        };
        auto chEvent = [](uint8_t status, uint64_t tick, uint8_t d0, uint8_t d1) {
            SmfEvent ev;
            ev.tick = tick;
            ev.status = status;
            ev.data0 = d0;
            ev.data1 = d1;
            return ev;
        };
        auto meta = [](uint64_t tick, uint8_t type, QByteArray blob) {
            SmfEvent ev;
            ev.tick = tick;
            ev.status = 0xFF;
            ev.metaType = type;
            ev.blob = std::move(blob);
            return ev;
        };
        SmfFile smf;
        smf.format = 1;
        smf.division = 24;
        SmfTrack conductor;
        conductor.events.push_back(meta(0, 0x51, QByteArray("\x07\xA1\x20", 3))); // 120 BPM
        conductor.events.push_back(meta(0, 0x51, QByteArray("\x06\x1A\x80", 3))); // 150 BPM
        conductor.endTick = 96;
        smf.tracks.push_back(conductor);
        SmfTrack ch0;
        ch0.events.push_back(chEvent(0xC0, 0, 5, 0));
        ch0.events.push_back(chEvent(0xB0, 0, 7, 100));
        ch0.events.push_back(chEvent(0xC0, 0, 9, 0));
        ch0.events.push_back(chEvent(0xB0, 0, 7, 80));
        ch0.events.push_back(chEvent(0x90, 0, 60, 100));
        ch0.events.push_back(chEvent(0x80, 96, 60, 0));
        ch0.endTick = 96;
        smf.tracks.push_back(ch0);

        QTemporaryDir tmp;
        const QString midPath = tmp.path() + QStringLiteral("/dups.mid");
        QString werror;
        SongInfo info;
        info.label = QStringLiteral("dups");
        info.midPath = midPath;
        info.hasMid = true;
        SongDocument doc;
        bool ok = tmp.isValid() && smf.writeFile(midPath, &werror) && doc.load(info, &werror);
        if (!ok)
            failD("could not write/load the synthetic file");
        int changedSignals = 0;
        QObject::connect(&doc, &SongDocument::documentChanged,
                         [&changedSignals] { changedSignals++; });
        const QByteArray baseline = ok ? doc.smf().write() : QByteArray();
        const auto ccPointsAt = [&doc](uint8_t cc, uint64_t tick) {
            std::vector<DocLanePoint> at;
            for (const DocLanePoint &pt : doc.lanePoints(0, cc)) {
                if (pt.tick == tick)
                    at.push_back(pt);
            }
            return at;
        };
        if (ok && (doc.lanePoints(0, DOC_CC_VOICE).size() != 2 || ccPointsAt(7, 0).size() != 2)) {
            failD("the loader no longer preserves same-tick duplicates");
            ok = false;
        }
        DocLanePoint pt;
        if (ok && (!doc.findLanePoint(0, 7, 0, &pt) || pt.value != 80)) {
            failD("findLanePoint did not return the last CC at the tick");
            ok = false;
        }
        if (ok && (!doc.findLanePoint(0, DOC_CC_VOICE, 0, &pt) || pt.value != 9)) {
            failD("findLanePoint did not return the last program at the tick");
            ok = false;
        }
        if (ok && (!doc.findLanePoint(0, DOC_CC_TEMPO, 0, &pt) || pt.value != 150)) {
            failD("findLanePoint did not return the last tempo at the tick");
            ok = false;
        }
        if (ok) {
            // Resubmitting the audible value still heals the same-tick
            // shadow under it, so this is a real, exactly-once-published,
            // exactly undoable edit.
            doc.findLanePoint(0, 7, 0, &pt);
            const QByteArray before = doc.smf().write();
            const uint64_t beforeRevision = doc.revision();
            const int beforeUndoCount = doc.undoStack()->count();
            const int beforeUndoIndex = doc.undoStack()->index();
            changedSignals = 0;
            doc.moveLanePoints({{0, 7, pt, pt.tick, pt.value}});
            const auto after = ccPointsAt(7, 0);
            if (after.size() != 1 || after[0].value != 80 || doc.revision() != beforeRevision + 1 ||
                doc.undoStack()->count() != beforeUndoCount + 1 ||
                doc.undoStack()->index() != beforeUndoIndex + 1 || changedSignals != 1) {
                failD("an exact duplicate resubmission did not canonicalize as one edit");
                ok = false;
            }
            const uint64_t canonicalRevision = doc.revision();
            changedSignals = 0;
            doc.undoStack()->undo();
            const auto restored = ccPointsAt(7, 0);
            if (ok &&
                (restored.size() != 2 || restored[0].value != 100 || restored[1].value != 80 ||
                 doc.smf().write() != before || doc.revision() != canonicalRevision + 1 ||
                 doc.undoStack()->count() != beforeUndoCount + 1 ||
                 doc.undoStack()->index() != beforeUndoIndex || changedSignals != 1)) {
                failD("undo did not exactly restore the shadowed duplicate");
                ok = false;
            }
        }
        if (ok) {
            // addLanePoint replaces the whole run on its tick.
            doc.addLanePoint(0, 7, 0, 70);
            if (ccPointsAt(7, 0).size() != 1) {
                failD("addLanePoint stacked another duplicate on the tick");
                ok = false;
            }
            doc.undoStack()->undo();
        }
        if (ok) {
            // A shadow-free point resubmitted unchanged is a no-op: no
            // command, no publication.
            doc.addLanePoint(0, 7, 48, 55);
            if (!doc.findLanePoint(0, 7, 48, &pt) || ccPointsAt(7, 48).size() != 1) {
                failD("controller no-op fixture did not create one point");
                ok = false;
            } else {
                const QByteArray before = doc.smf().write();
                const uint64_t beforeRevision = doc.revision();
                const int beforeUndoCount = doc.undoStack()->count();
                const int beforeUndoIndex = doc.undoStack()->index();
                changedSignals = 0;
                doc.moveLanePoints({{0, 7, pt, pt.tick, pt.value}});
                if (doc.smf().write() != before || doc.revision() != beforeRevision ||
                    doc.undoStack()->count() != beforeUndoCount ||
                    doc.undoStack()->index() != beforeUndoIndex || changedSignals != 0) {
                    failD("an unchanged controller point mutated the document");
                    ok = false;
                }
            }
            if (ok) {
                // A cross-tick move landing on an occupied tick replaces
                // the run there too.
                doc.moveLanePoint(0, 7, pt, 0, 55);
                if (ccPointsAt(7, 0).size() != 1 || !ccPointsAt(7, 48).empty()) {
                    failD("a cross-tick move did not replace the destination run");
                    ok = false;
                }
            }
        }
        if (ok) {
            // The same no-op rule holds for the tempo lane's global metas.
            doc.addLanePoint(0, DOC_CC_TEMPO, 48, 120);
            if (!doc.findLanePoint(0, DOC_CC_TEMPO, 48, &pt) ||
                ccPointsAt(DOC_CC_TEMPO, 48).size() != 1) {
                failD("tempo no-op fixture did not create one point");
                ok = false;
            } else {
                const QByteArray before = doc.smf().write();
                const uint64_t beforeRevision = doc.revision();
                const int beforeUndoCount = doc.undoStack()->count();
                const int beforeUndoIndex = doc.undoStack()->index();
                changedSignals = 0;
                doc.moveLanePoints({{0, DOC_CC_TEMPO, pt, pt.tick, pt.value}});
                if (doc.smf().write() != before || doc.revision() != beforeRevision ||
                    doc.undoStack()->count() != beforeUndoCount ||
                    doc.undoStack()->index() != beforeUndoIndex || changedSignals != 0) {
                    failD("an unchanged global tempo point mutated the document");
                    ok = false;
                }
            }
        }
        if (ok) {
            while (doc.undoStack()->canUndo())
                doc.undoStack()->undo();
            if (doc.smf().write() != baseline)
                failD("undo-all did not restore the duplicated file byte-for-byte");
        }
    }

    failures += documentContractFailures();

    std::printf("editcheck: %d songs in %lld ms\n", checked, (long long)timer.elapsed());
    std::printf("editcheck: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
