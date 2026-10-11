#pragma once

#include <cstdint>
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
//  - Later notes win: a later note on the same (tick, key) replaces an
//    earlier one, and a later pass trims or removes the earlier notes it
//    overlaps on the same key exactly as SongDocument::resolveNoteOverlaps
//    does against the document (addNotes resolves against the document
//    only, not within its own batch).
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

    static constexpr uint32_t kSegmentMask = 0xFFFFF;

  private:
    void closeOpen(uint64_t samplePos, bool played);
    void enterSegment(uint32_t segment, uint64_t start);

    int m_track = -1;
    uint32_t m_generation = 0;
    bool m_started = false;
    uint64_t m_punchIn = 0;
    uint32_t m_segment = 0;
    uint64_t m_segmentStart = 0;
    uint64_t m_lastSample = 0; // the latest stamp seen in the current segment
    int m_unmatchedOffs = 0;
    std::vector<Note> m_notes;
    int m_open[128];
};
