#include "recordingtake.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>

#include "core/miditimeline.h"
#include "core/timelineplayer.h"

void RecordingTake::begin(int track, uint32_t generation)
{
    clear();
    m_track = track;
    m_generation = generation;
}

void RecordingTake::setPunchWindow(uint64_t in, uint64_t out)
{
    m_windowIn = in;
    m_windowOut = std::max(in, out);
}

void RecordingTake::clear()
{
    m_track = -1;
    m_generation = 0;
    m_windowIn = 0;
    m_windowOut = 0;
    m_started = false;
    m_punchIn = 0;
    m_segment = 0;
    m_segmentStart = 0;
    m_lastSample = 0;
    m_unmatchedOffs = 0;
    m_notes.clear();
    m_controls.clear();
    std::fill(std::begin(m_moved), std::end(m_moved), false);
    m_segmentEnds.clear();
    m_spans.clear();
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

namespace {

// A sample's tick on the timeline, rounded, never before tick 0.
uint64_t tickAt(const MidiTimeline &timeline, uint64_t sample)
{
    return uint64_t(std::max<long long>(0, std::llround(timeline.tickForSample(sample))));
}

// Adds [start, end) to a sorted, merged span list, merging what it touches.
void addSpan(std::vector<std::pair<uint64_t, uint64_t>> &spans, uint64_t start, uint64_t end)
{
    if (end <= start)
        return;
    auto it = std::lower_bound(spans.begin(), spans.end(), std::make_pair(start, start));
    if (it != spans.begin() && std::prev(it)->second >= start)
        --it;
    while (it != spans.end() && it->first <= end) {
        start = std::min(start, it->first);
        end = std::max(end, it->second);
        it = spans.erase(it);
    }
    spans.insert(it, {start, end});
}

} // namespace

void RecordingTake::leaveSegment(uint64_t end)
{
    m_segmentEnds[m_segment] = end;
    addSpan(m_spans, m_segmentStart, std::max(end, m_segmentStart));
}

std::vector<std::pair<uint64_t, uint64_t>> RecordingTake::recordedSpans(uint64_t endSample,
                                                                        uint32_t endSegment) const
{
    if (!m_started)
        return {};
    std::vector<std::pair<uint64_t, uint64_t>> spans = m_spans;
    // As toNotes ends an open note: at endSample when it was read in the
    // current segment, else at the last position the take saw.
    const bool sameSegment = (endSegment & kSegmentMask) == (m_segment & kSegmentMask);
    const uint64_t end = sameSegment && endSample >= m_segmentStart ? endSample : m_lastSample;
    addSpan(spans, m_segmentStart, std::max(end, m_lastSample));
    return spans;
}

std::vector<std::pair<uint64_t, uint64_t>>
RecordingTake::recordedTickSpans(const MidiTimeline &timeline, uint64_t endSample,
                                 uint32_t endSegment) const
{
    std::vector<std::pair<uint64_t, uint64_t>> spans;
    for (const auto &[start, end] : recordedSpans(endSample, endSegment)) {
        uint64_t from = tickAt(timeline, start), to = tickAt(timeline, end);
        if (punching()) {
            from = std::max(from, m_windowIn);
            to = std::min(to, m_windowOut);
        }
        addSpan(spans, from, to);
    }
    return spans;
}

std::vector<std::pair<uint64_t, uint64_t>> RecordingTake::punchedSpans(const MidiTimeline &timeline,
                                                                       uint64_t endSample,
                                                                       uint32_t endSegment) const
{
    if (!punching())
        return {};
    return recordedTickSpans(timeline, endSample, endSegment);
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
            // where the segment did. Past a lost Boundary, the position this
            // one ended at belongs to a segment the take never saw: the last
            // position seen is the bound, as below.
            const uint64_t end = ev.segment == m_segment + 1 ? ev.samplePos : m_lastSample;
            closeOpen(end, /*played=*/false);
            leaveSegment(end);
            enterSegment(ev.segment, ev.resumePos);
        }
        return;
    }
    if (ev.segment > m_segment) {
        // The segment's Boundary was dropped (a full ring). Its true end is
        // unknown; the last position seen in it is the best bound.
        closeOpen(m_lastSample, /*played=*/false);
        leaveSegment(m_lastSample);
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
            prev.offLate = ev.lateSamples;
            prev.open = false;
            prev.playedEnd = true;
        }
        Note note;
        note.onSample = ev.samplePos;
        note.onLate = ev.lateSamples;
        note.floorSample = m_segmentStart;
        note.carried = (ev.flags & CaptureEvent::Carried) != 0;
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
            note.offLate = ev.lateSamples;
            note.open = false;
            note.playedEnd = true;
            m_open[key] = -1;
        }
        break;
    case CaptureEvent::Cut:
        closeOpen(ev.samplePos, /*played=*/false);
        break;
    case CaptureEvent::Bend:
    case CaptureEvent::Control:
    case CaptureEvent::Program: {
        Control control;
        control.sample = ev.samplePos;
        control.floorSample = m_segmentStart;
        control.late = ev.lateSamples;
        control.segment = m_segment;
        // A Control's lane is its key (its 7 bits: never DOC_CC_BEND); a
        // program change's is the Voice lane.
        control.cc = ev.kind == CaptureEvent::Bend      ? DOC_CC_BEND
                     : ev.kind == CaptureEvent::Program ? DOC_CC_VOICE
                                                        : key;
        // A CC121 reset on a lane this take never moved isn't recorded:
        // the lane keeps the song's automation (J5).
        if (ev.flags & CaptureEvent::Reset) {
            if (!m_moved[control.cc])
                break;
        } else {
            m_moved[control.cc] = true;
        }
        control.reset = (ev.flags & CaptureEvent::Reset) != 0;
        control.value = ev.kind == CaptureEvent::Bend ? std::clamp<int>(ev.value, -8192, 8191)
                                                      : std::clamp<int>(ev.value, 0, 127);
        m_controls.push_back(control);
        break;
    }
    default:
        break;
    }
}

namespace {

uint64_t samplesEarlier(uint64_t sample, uint64_t by)
{
    return sample > by ? sample - by : 0;
}

} // namespace

std::vector<SongDocument::NewNote> RecordingTake::toNotes(const MidiTimeline &timeline,
                                                          uint64_t endSample, uint32_t endSegment,
                                                          uint64_t latencySamples) const
{
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
            off = std::max(note.onSample,
                           sameSegment && endSample >= note.onSample ? endSample : m_lastSample);
            played = false;
        }
        const uint64_t on =
            std::max(note.floorSample, samplesEarlier(note.onSample, latencySamples + note.onLate));
        if (played) {
            off = samplesEarlier(off, latencySamples + note.offLate);
            // A key carried over from a count-in but heard released at or
            // before the song's start: let go in the latency window before
            // the heard downbeat, so to the ear it was a release during the
            // count-in, and dropped as one (docs/metronome/PLAN.md M6), not
            // left as a one-tick blip at the start.
            if (note.carried && off <= note.floorSample)
                continue;
            off = std::max(on, off);
        }
        uint64_t tick = tickAt(timeline, on);
        uint64_t end = std::max(tick + 1, tickAt(timeline, off));
        if (punching()) {
            // Only the window is kept (P5): a key held across punch-in starts
            // there, one held across punch-out ends there.
            if (end <= m_windowIn || tick >= m_windowOut)
                continue;
            tick = std::max(tick, m_windowIn);
            end = std::min(end, m_windowOut);
        }
        spans.push_back({tick, end, note.key, note.velocity, true});
    }

    // Later notes win (D7/D8), with resolveNoteOverlaps' trims: an earlier
    // note keeps its head when the later one starts inside it, keeps its
    // tail when it outlasts the later one (a shared start included), and
    // goes when it is covered. So one take with two passes lands as two
    // takes would.
    // Each trim leaves what is kept on a key disjoint, so the kept notes sit
    // in one map by (key, start) and a later note visits only those it
    // overlaps: its predecessor and the ones starting inside it. Each is
    // trimmed (at most two) or removed for good, so a long take stays
    // O(n log n) at the playhead cadence (a live preview), not O(n^2) a key.
    std::map<std::pair<uint8_t, uint64_t>, size_t> kept;
    size_t steps = 0;
    for (size_t i = 0; i < spans.size(); i++) {
        const Span &w = spans[i];
        auto it = kept.lower_bound({w.key, w.tick});
        if (it != kept.begin()) {
            const auto prev = std::prev(it);
            if (prev->first.first == w.key && spans[prev->second].end > w.tick)
                it = prev;
        }
        while (it != kept.end() && it->first.first == w.key && it->first.second < w.end) {
            steps++;
            Span &s = spans[it->second];
            if (s.tick < w.tick) {
                s.end = w.tick;
                ++it;
            } else if (s.end > w.end) {
                // Its tail outlasts w: it now starts where w ends, past the
                // span this loop walks (nothing else starts inside it).
                const size_t j = it->second;
                it = kept.erase(it);
                s.tick = w.end;
                kept.emplace(std::make_pair(s.key, s.tick), j);
                break;
            } else {
                s.kept = false;
                it = kept.erase(it);
            }
        }
        steps++;
        kept.emplace(std::make_pair(w.key, w.tick), i);
    }
    m_overlapSteps = steps;

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

int RecordingTake::audible(uint8_t cc, int value)
{
    return cc == DOC_CC_BEND ? (value + 8192) >> 7 : value;
}

int RecordingTake::restValue(uint8_t cc)
{
    if (cc == DOC_CC_BEND)
        return 0;
    const int value = TimelinePlayer::ccDefault(cc);
    return value >= 0 ? value : 0;
}

std::vector<RecordingTake::LaneWrite> RecordingTake::toLaneWrites(const MidiTimeline &timeline,
                                                                  uint64_t endSample,
                                                                  uint32_t endSegment,
                                                                  uint64_t latencySamples) const
{
    // Segments only grow, so a map by (segment, lane) keeps segment order.
    std::map<std::pair<uint32_t, int>, LaneWrite> writes;
    std::map<std::pair<uint32_t, int>, uint64_t> lastMove; // its sample, unshifted
    // A punch take's lanes moved inside the window so far: where a CC121
    // reset counts (push's rule, over the window's moves alone).
    bool movedInside[256] = {};
    for (const Control &c : m_controls) {
        const uint64_t sample =
            std::max(c.floorSample, samplesEarlier(c.sample, latencySamples + c.late));
        uint64_t tick = tickAt(timeline, sample);
        if (punching()) {
            // A move the latency shift carried from inside the window to
            // before punch-in (it arrived at or after in) lands on punch-in,
            // as a note sounding across in starts there; one that arrived
            // before in was played before the window.
            if (tick < m_windowIn && tickAt(timeline, c.sample) >= m_windowIn)
                tick = m_windowIn;
            // A move outside the window isn't recorded (P5): the lane keeps
            // the song's value there.
            if (tick < m_windowIn || tick >= m_windowOut)
                continue;
            if (c.reset && !movedInside[c.cc])
                continue;
            if (!c.reset)
                movedInside[c.cc] = true;
        }
        // Within a segment the lanes go in ascending CC, then BEND (0xFF),
        // then the voice.
        auto [it, fresh] =
            writes.try_emplace({c.segment, c.cc == DOC_CC_VOICE ? 0x100 : int(c.cc)});
        LaneWrite &w = it->second;
        if (fresh) {
            w.cc = c.cc;
            w.tickBegin = tick;
        }
        lastMove[it->first] = c.sample;
        // The wait shift can reorder two moves of one callback; they keep
        // the order they were played in.
        tick = std::max(tick, w.tickEnd);
        w.tickEnd = std::max(w.tickEnd, tick);
        if (!w.points.empty() && w.points.back().tick == tick)
            w.points.pop_back();
        if (!w.points.empty() && audible(c.cc, w.points.back().value) == audible(c.cc, c.value)) {
            w.points.back().value = c.value;
            continue;
        }
        w.points.push_back({tick, c.value});
    }
    std::vector<LaneWrite> out;
    out.reserve(writes.size());
    for (auto &[key, w] : writes) {
        // Where the segment ended, as toNotes ends an open note: the stop
        // position when it was read in the take's current segment, else the
        // last position seen; an earlier segment ended at its Boundary.
        uint64_t end = m_lastSample;
        if (key.first == m_segment) {
            const bool sameSegment = (endSegment & kSegmentMask) == (m_segment & kSegmentMask);
            if (sameSegment && endSample >= lastMove[key])
                end = endSample;
        } else if (const auto seg = m_segmentEnds.find(key.first); seg != m_segmentEnds.end()) {
            end = seg->second;
        }
        uint64_t endTick = tickAt(timeline, end);
        // A punch take's write ends at punch-out at the latest: the song's
        // value comes back there.
        if (punching())
            endTick = std::min(endTick, m_windowOut);
        if (endTick > w.tickEnd) {
            w.tickEnd = endTick;
            w.restoreEnd = true;
        }
        out.push_back(std::move(w));
    }
    return out;
}

bool RecordingTake::LaneWrite::restorePoint(const std::vector<DocLanePoint> &before,
                                            SongDocument::LanePointValue *out) const
{
    if (!restoreEnd || points.empty())
        return false;
    // The song's value at tickEnd: its last point there or before, or with
    // none the engine's track-init value (restValue: VOL 127, PAN 64, BENDR
    // 2, LFOS 22, MOD 0, bend centred), which is what plays there. A point
    // on tickEnd itself, which the write's range takes, comes back this way
    // (or goes when the take's last value already sounds like it).
    // The voice has no track-init value: with no program change before,
    // the track plays its first later one (TimelinePlayer::primeVoices, and
    // chaseTouched at a wrap), so that is what comes back; with none at
    // all, nothing can stand for "no voice", and the take's last voice
    // stays.
    const bool voice = cc == DOC_CC_VOICE;
    int value = restValue(cc);
    bool known = !voice;
    for (const DocLanePoint &pt : before) {
        if (pt.tick > tickEnd) {
            if (!known) {
                value = pt.value;
                known = true;
            }
            break;
        }
        value = pt.value;
        known = true;
    }
    if (!known)
        return false;
    if (audible(cc, value) == audible(cc, points.back().value))
        return false;
    *out = {tickEnd, value};
    return true;
}

namespace {

const auto byTick = [](const SongDocument::LanePointValue &a,
                       const SongDocument::LanePointValue &b) { return a.tick < b.tick; };

// What writeLanePoints does to lane (sorted by tick): the range
// [tickBegin, tickEnd] goes, points (one a tick, inside the range) take its
// place.
void replaceRange(std::vector<SongDocument::LanePointValue> &lane, uint64_t tickBegin,
                  uint64_t tickEnd, const std::vector<SongDocument::LanePointValue> &points)
{
    const auto first = std::lower_bound(lane.begin(), lane.end(),
                                        SongDocument::LanePointValue{tickBegin, 0}, byTick);
    const auto last =
        std::upper_bound(first, lane.end(), SongDocument::LanePointValue{tickEnd, 0}, byTick);
    lane.insert(lane.erase(first, last), points.begin(), points.end());
}

// lane with this cc's writes applied in order, each with its restore point
// read against before, as MainWindow::commitTake writes them.
void applyWrites(std::vector<SongDocument::LanePointValue> &lane,
                 const std::vector<DocLanePoint> &before,
                 const std::vector<RecordingTake::LaneWrite> &writes, uint8_t cc)
{
    for (const RecordingTake::LaneWrite &w : writes) {
        if (w.cc != cc)
            continue;
        std::vector<SongDocument::LanePointValue> points = w.points;
        SongDocument::LanePointValue restore;
        if (w.restorePoint(before, &restore))
            points.push_back(restore);
        replaceRange(lane, w.tickBegin, w.tickEnd, points);
    }
}

} // namespace

bool RecordingTake::voiceLeadPoint(const std::vector<DocLanePoint> &before,
                                   const std::vector<LaneWrite> &writes,
                                   std::vector<SongDocument::LanePointValue> *points,
                                   uint64_t *rangeEnd)
{
    uint64_t first = UINT64_MAX;
    for (const LaneWrite &w : writes)
        if (w.cc == DOC_CC_VOICE && !w.points.empty())
            first = std::min(first, w.tickBegin);
    // Before the take the track played its first program change from the
    // start (primeVoices). When the take's voice would become the first
    // instead, that one goes to tick 0 so the stretch before the take keeps
    // its voice. A track with a voice before the take, or with none at
    // all, needs nothing.
    if (first == UINT64_MAX || first == 0 || before.empty() || before.front().tick < first)
        return false;
    *points = {{0, before.front().value}};
    *rangeEnd = 0;
    // It is a move: the song's first changes (every one on their tick, f)
    // were the track's initial voice, and a copy left behind at f would
    // keep claiming that (SongView::editTrackVoice edits the last change on
    // the first change's tick, SongDocument::mergeTrack leaves those
    // behind) while only repeating what plays there. A write over f takes
    // them anyway. Outside every write they go when what plays just before
    // f, once the take lands, is already their last (same-tick changes are
    // last-wins), as after a restore point at a write's end; otherwise they
    // still change the voice there (the take ended on its own voice, so
    // nothing was restored) and stay, as only that last one.
    const uint64_t f = before.front().tick;
    for (const LaneWrite &w : writes)
        if (w.cc == DOC_CC_VOICE && w.tickBegin <= f && f <= w.tickEnd)
            return true;
    size_t group = 0;
    while (group < before.size() && before[group].tick == f)
        group++;
    const int last = before[group - 1].value;
    std::vector<SongDocument::LanePointValue> lane;
    lane.reserve(before.size() + 1);
    lane.push_back(points->front());
    for (const DocLanePoint &pt : before)
        lane.push_back({pt.tick, pt.value});
    applyWrites(lane, before, writes, DOC_CC_VOICE);
    const auto atF =
        std::lower_bound(lane.begin(), lane.end(), SongDocument::LanePointValue{f, 0}, byTick);
    const int playing = std::prev(atF)->value; // the lead at 0 comes before
    if (audible(DOC_CC_VOICE, playing) == audible(DOC_CC_VOICE, last)) {
        *rangeEnd = f;
    } else if (group > 1) {
        points->push_back({f, last});
        *rangeEnd = f;
    }
    return true;
}

std::vector<SongDocument::LanePointValue>
RecordingTake::applyLaneWrites(const std::vector<DocLanePoint> &before,
                               const std::vector<LaneWrite> &writes, uint8_t cc)
{
    std::vector<SongDocument::LanePointValue> lane;
    lane.reserve(before.size() + 1);
    for (const DocLanePoint &pt : before)
        lane.push_back({pt.tick, pt.value});
    // The voice's lead write goes in first, as MainWindow::commitTake
    // writes it.
    std::vector<SongDocument::LanePointValue> lead;
    uint64_t leadEnd = 0;
    if (cc == DOC_CC_VOICE && voiceLeadPoint(before, writes, &lead, &leadEnd))
        replaceRange(lane, 0, leadEnd, lead);
    applyWrites(lane, before, writes, cc);
    return lane;
}
