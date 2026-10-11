#include "recordingtake.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "core/miditimeline.h"

void RecordingTake::begin(int track, uint32_t generation)
{
    clear();
    m_track = track;
    m_generation = generation;
}

void RecordingTake::clear()
{
    m_track = -1;
    m_generation = 0;
    m_started = false;
    m_punchIn = 0;
    m_segment = 0;
    m_segmentStart = 0;
    m_lastSample = 0;
    m_unmatchedOffs = 0;
    m_notes.clear();
    std::fill(std::begin(m_open), std::end(m_open), -1);
}

void RecordingTake::closeOpen(uint64_t samplePos, bool played)
{
    for (int &index : m_open) {
        if (index < 0)
            continue;
        Note &note = m_notes[size_t(index)];
        note.offSample = std::max(samplePos, note.onSample);
        note.open = false;
        note.playedEnd = played;
        index = -1;
    }
}

void RecordingTake::enterSegment(uint32_t segment, uint64_t start)
{
    m_segment = segment;
    m_segmentStart = start;
    m_lastSample = start;
}

void RecordingTake::push(const CaptureEvent &ev)
{
    // Another arm's leftovers (a callback still in flight at the last
    // punch-out), or anything before this arm's Begin, belong to no take.
    if (!active() || ev.generation != m_generation)
        return;
    if (ev.kind == CaptureEvent::Begin) {
        if (!m_started) {
            m_started = true;
            m_punchIn = ev.samplePos;
            enterSegment(ev.segment, ev.samplePos);
        }
        return;
    }
    if (!m_started || ev.segment < m_segment)
        return;
    if (ev.kind == CaptureEvent::Boundary) {
        if (ev.segment > m_segment) {
            // The player jumped (a loop wrap, a seek): notes still held end
            // where the segment did.
            closeOpen(ev.samplePos, /*played=*/false);
            enterSegment(ev.segment, ev.resumePos);
        }
        return;
    }
    if (ev.segment > m_segment) {
        // The segment's Boundary was dropped (a full ring). Its true end is
        // unknown; the last position seen in it is the best bound.
        closeOpen(m_lastSample, /*played=*/false);
        enterSegment(ev.segment, ev.samplePos);
    }
    m_lastSample = std::max(m_lastSample, ev.samplePos);

    const uint8_t key = ev.key & 0x7F;
    switch (ev.kind) {
    case CaptureEvent::NoteOn: {
        // The engine releases a re-pressed key before re-keying it, so an
        // open note here means its note-off was dropped: end it now.
        if (m_open[key] >= 0) {
            Note &prev = m_notes[size_t(m_open[key])];
            prev.offSample = std::max(ev.samplePos, prev.onSample);
            prev.open = false;
            prev.playedEnd = true;
        }
        Note note;
        note.onSample = ev.samplePos;
        note.floorSample = m_segmentStart;
        note.segment = m_segment;
        note.key = key;
        note.velocity = std::clamp<uint8_t>(ev.velocity, 1, 127);
        m_open[key] = int(m_notes.size());
        m_notes.push_back(note);
        break;
    }
    case CaptureEvent::NoteOff:
        if (m_open[key] < 0) {
            m_unmatchedOffs++;
            break;
        }
        {
            Note &note = m_notes[size_t(m_open[key])];
            note.offSample = std::max(ev.samplePos, note.onSample);
            note.open = false;
            note.playedEnd = true;
            m_open[key] = -1;
        }
        break;
    case CaptureEvent::Cut:
        closeOpen(ev.samplePos, /*played=*/false);
        break;
    default:
        break;
    }
}

std::vector<SongDocument::NewNote> RecordingTake::toNotes(const MidiTimeline &timeline,
                                                          uint64_t endSample, uint32_t endSegment,
                                                          uint64_t latencySamples) const
{
    const auto tickAt = [&](uint64_t sample) {
        return uint64_t(std::max<long long>(0, std::llround(timeline.tickForSample(sample))));
    };
    struct Span {
        uint64_t tick;
        uint64_t end; // exclusive
        uint8_t key;
        uint8_t velocity;
        bool kept;
    };
    std::vector<Span> spans;
    spans.reserve(m_notes.size());
    for (const Note &note : m_notes) {
        uint64_t off = note.offSample;
        bool played = note.playedEnd;
        if (note.open) {
            // The stop position, when it was read in the note's segment;
            // otherwise the segment's last known position.
            const bool sameSegment = (note.segment & kSegmentMask) == (endSegment & kSegmentMask) &&
                                     note.segment == m_segment;
            off = std::max(note.onSample, sameSegment && endSample >= note.onSample ? endSample
                                                                                    : m_lastSample);
            played = false;
        }
        const uint64_t shiftedOn = note.onSample > latencySamples ? note.onSample - latencySamples : 0;
        const uint64_t on = std::max(note.floorSample, shiftedOn);
        if (played)
            off = std::max(on, off > latencySamples ? off - latencySamples : 0);
        const uint64_t tick = tickAt(on);
        const uint64_t end = std::max(tick + 1, tickAt(off));
        spans.push_back({tick, end, note.key, note.velocity, true});
    }

    // Later notes win (D7/D8), with resolveNoteOverlaps' trims: an earlier
    // note keeps its head when the later one starts inside it, keeps its
    // tail when the later one ends inside it, and goes when it is covered
    // or shares the later one's start.
    // Per key, so a live preview of a long take stays cheap.
    std::vector<size_t> byKey[128];
    for (size_t i = 0; i < spans.size(); i++) {
        const Span &w = spans[i];
        std::vector<size_t> &earlier = byKey[w.key];
        for (size_t j : earlier) {
            Span &s = spans[j];
            if (!s.kept || s.end <= w.tick || s.tick >= w.end)
                continue;
            if (s.tick < w.tick)
                s.end = w.tick;
            else if (s.tick > w.tick && s.end > w.end)
                s.tick = w.end;
            else
                s.kept = false;
        }
        earlier.push_back(i);
    }

    std::vector<SongDocument::NewNote> out;
    for (const Span &s : spans) {
        if (s.kept)
            out.push_back({s.tick, s.key, uint32_t(s.end - s.tick), s.velocity});
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const SongDocument::NewNote &a, const SongDocument::NewNote &b) {
                         return a.tick != b.tick ? a.tick < b.tick : a.key < b.key;
                     });
    return out;
}
