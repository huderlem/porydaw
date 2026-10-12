#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "core/captureevent.h"
#include "core/songdocument.h"

class MidiTimeline;

// A recording take while it is still UI-side data (§2 stance 3): the target
// track, locked at punch-in, and the notes played so far in sample time.
// Pure logic with no widgets or audio, so the harness can feed it fabricated
// stamps.
//
// Rules, applied when the take is converted to notes:
//  - Ticks come from MidiTimeline::tickForSample (tempo changes included),
//    rounded to the nearest tick; every note lasts at least 1 tick.
//  - A played time first moves back by how long its message waited for
//    the audio callback that stamped it (CaptureEvent::lateSamples).
//  - The latency offset moves played times earlier, but never before the
//    start of the note's segment: punch-in for the first, loop start (or a
//    seek's landing) for a later one. Ends the take imposed (a wrap, a seek,
//    a cut, the stop) are not played times and stay where they are.
//  - A note held across a wrap or seek ends where the segment ended: at the
//    loop end for a wrap, however many passes it was held for. Its eventual
//    note-off matches nothing and is ignored, as is any note-off whose note
//    on predates the take (a key held at punch-in) or was dropped.
//  - Notes still open end at the stop position.
//  - Later notes win: a later note trims or removes the earlier notes it
//    overlaps on the same key exactly as SongDocument::resolveNoteOverlaps
//    does against the document (addNotes resolves against the document
//    only, not within its own batch). So an earlier note that outlasts a
//    later one on the same (tick, key) keeps its tail, and one take of two
//    passes lands as two takes would.
//  - Pitch-bend and mod-wheel moves are played times too (the same wait
//    and offset shifts and segment floor), written as BEND and MOD lane
//    points; where the segment they were played in ends (an imposed end,
//    like an open note's: not shifted), the lane goes back to what the song
//    had there. See toLaneWrites.
class RecordingTake
{
  public:
    struct Note {
        uint64_t onSample = 0;
        uint64_t offSample = 0;   // valid once closed
        uint64_t floorSample = 0; // the start of its segment
        uint32_t onLate = 0;      // how long the note on waited for its callback
        uint32_t offLate = 0;     // ditto the played note off
        uint32_t segment = 0;
        uint8_t key = 0;
        uint8_t velocity = 0;
        bool open = true;
        bool playedEnd = false; // closed by a played note-off (shifted by the offset)
    };

    // A played pitch-bend or mod-wheel move.
    struct Control {
        uint64_t sample = 0;
        uint64_t floorSample = 0; // the start of its segment
        uint32_t late = 0;        // how long its message waited for its callback
        uint32_t segment = 0;
        uint8_t cc = 0; // DOC_CC_BEND, or 0x01 (MOD)
        int value = 0;  // bend -8192..8191; MOD 0-127
    };
    // One lane's moves from one segment (a loop pass, a seek's landing):
    // they replace the lane's points in [tickBegin, tickEnd]
    // (SongDocument::writeLanePoints), from its first move to the segment's
    // end, or to its last move when the segment ended on that tick.
    struct LaneWrite {
        uint8_t cc = 0;
        uint64_t tickBegin = 0;
        uint64_t tickEnd = 0;
        std::vector<SongDocument::LanePointValue> points;
        // tickEnd is the segment's end, past the last move: the commit puts
        // back, at tickEnd, the value the song had there before the take,
        // when it sounds different from the take's last (restorePoint).
        bool restoreEnd = false;
        // The point that does it, given the lane's points before the take
        // (sorted by tick); false when the take's last value already sounds
        // like the song's.
        bool restorePoint(const std::vector<DocLanePoint> &before,
                          SongDocument::LanePointValue *out) const;
    };
    // What the engine plays of a lane value: m4a's BEND keeps the bend's
    // top 7 bits; MOD as it is.
    static int audible(uint8_t cc, int value);
    // The lane `cc` as the commit would leave it: before (the lane's points,
    // sorted by tick) with this cc's writes applied in order, each with its
    // restore point read against before, as MainWindow::commitTake writes
    // them. Sorted by tick. The live preview's curve.
    static std::vector<SongDocument::LanePointValue>
    applyLaneWrites(const std::vector<DocLanePoint> &before, const std::vector<LaneWrite> &writes,
                    uint8_t cc);

    RecordingTake() { clear(); }

    void begin(int track, uint32_t generation);
    void clear();
    // Armed: begin() ran and nothing has cleared it.
    bool active() const { return m_track >= 0; }
    // The audio thread's Begin arrived: the take has a punch-in position.
    bool started() const { return m_started; }
    int track() const { return m_track; }
    // The track moved to another engine slot (a track add, delete, or move).
    void setTrack(int track) { m_track = track; }
    uint32_t generation() const { return m_generation; }
    uint64_t punchInSample() const { return m_punchIn; }
    uint32_t segment() const { return m_segment; }
    // Note-offs that matched no open note (a key held at punch-in, a note
    // already ended by a wrap or cut, a dropped note on).
    int unmatchedOffs() const { return m_unmatchedOffs; }

    void push(const CaptureEvent &ev);
    // In note-on order; open notes have no end yet.
    const std::vector<Note> &notes() const { return m_notes; }

    // The take as document notes, with open notes ending at endSample. That
    // is the stop position at commit, and the playhead for a live preview
    // (step 4); endSegment is the segment it was read in (compared modulo
    // segmentMask, as AudioEngine publishes it), and an open note whose
    // segment doesn't match ends at the last position the take saw instead.
    // Sorted by tick, then key.
    std::vector<SongDocument::NewNote> toNotes(const MidiTimeline &timeline, uint64_t endSample,
                                               uint32_t endSegment, uint64_t latencySamples) const;

    // Where the take has recorded, in sample time: every segment it saw
    // (punch-in, loop passes, seek landings), the current one running to
    // endSample under toNotes' segment rule. Sorted, overlaps merged, [start,
    // end). Empty until started(). The roll's "REC" ruler tint (step 4).
    std::vector<std::pair<uint64_t, uint64_t>> recordedSpans(uint64_t endSample,
                                                             uint32_t endSegment) const;

    // In arrival order.
    const std::vector<Control> &controls() const { return m_controls; }
    // The take's bend and mod moves as lane writes, in segment order (later
    // passes win where they overlap), MOD before BEND within a segment.
    // Ticks round to the nearest and never run backwards within a write; on
    // a shared tick the last move wins, and a move that changes nothing the
    // engine plays (the same MOD, or the same bend to its 7-bit resolution)
    // is dropped, its exact value taken by the point it sounds like (a
    // bend's spring back to centre rests at 0). Each write runs to its
    // segment's end, unshifted: the Boundary that ended it, or for the
    // current segment endSample under toNotes' rule for an open note
    // (endSample when read in it, else the last position seen); there the
    // commit restores the song's value (LaneWrite::restoreEnd). A take with
    // no moves writes nothing, so the lanes keep what the song had.
    std::vector<LaneWrite> toLaneWrites(const MidiTimeline &timeline, uint64_t endSample,
                                        uint32_t endSegment, uint64_t latencySamples) const;

    // The latest position the take saw in its current segment (where an
    // open note read in another segment ends).
    uint64_t lastSample() const { return m_lastSample; }
    // Harness only: how many overlap steps (a kept note visited, a note
    // placed) the last toNotes took; bounded by a few per note.
    size_t overlapSteps() const { return m_overlapSteps; }

    static constexpr uint32_t kSegmentMask = 0xFFFFF;

  private:
    void closeOpen(uint64_t samplePos, bool played);
    void enterSegment(uint32_t segment, uint64_t start);
    // The current segment ended at end: its span joins m_spans.
    void leaveSegment(uint64_t end);

    int m_track = -1;
    uint32_t m_generation = 0;
    bool m_started = false;
    uint64_t m_punchIn = 0;
    uint32_t m_segment = 0;
    uint64_t m_segmentStart = 0;
    uint64_t m_lastSample = 0; // the latest stamp seen in the current segment
    int m_unmatchedOffs = 0;
    std::vector<Note> m_notes;
    std::vector<Control> m_controls;
    // Where each segment left so far ended, by segment.
    std::map<uint32_t, uint64_t> m_segmentEnds;
    // The segments left so far, merged (see recordedSpans).
    std::vector<std::pair<uint64_t, uint64_t>> m_spans;
    int m_open[128];
    mutable size_t m_overlapSteps = 0;
};
