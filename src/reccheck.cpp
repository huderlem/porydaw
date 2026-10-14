#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <QUndoStack>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <vector>

#include "audio/midiinput.h"
#include "core/miditimeline.h"
#include "core/recordingtake.h"
#include "mainwindow.h"
#include "ui/keymap.h"
#include "ui/songview.h"
#include "ui/theme/themeruntime.h"

// --reccheck <projectRoot> <songLabel>: MIDI recording
// (docs/midi-input/PLAN.md step 3). QSettings is redirected into a temp dir
// first. Writes into the project (a bundle export under the temp dir only,
// but the window opens the project): run it on a scratch copy.
//
// The RecordingTake rules first, on fabricated sample stamps against a
// hand-built tempo map (1000 samples per tick, then 2000 after a tempo
// change): ticks through the tempo map, nearest-tick rounding, the 1-tick
// minimum, held-at-stop, a note held across one or more loop wraps ending at
// the loop end, the latency offset clamped at punch-in and at the loop
// start (and never moving an end the take imposed), later passes winning
// overlapping notes by the document's own rules, unmatched note-offs, cuts,
// a lost Boundary (and a Boundary after one), another arm's events; bend and
// lane moves (MOD, VOL, PAN, BENDR, LFOS) as lane writes, in ascending CC
// with BEND last, each ending at its segment's end (a Boundary, a lost one,
// the stop) with the song's value (or the engine's track-init value)
// restored there.
//
// Then Record end to end in the window, with the audio device parked and the
// real callback pumped (AudioEngine::holdDeviceForTest/pumpForTest), every
// message injected through MidiInput::injectForTest: a fresh start records
// from the edit cursor and Stop commits one undo step that undo removes; R
// in the roll toggles Record, which has no window shortcut; Pause commits;
// a take ended inside an open edit group lands after it as its own step; a
// take's pitch bend and mod wheel land on its BEND and MOD lanes, the song's
// value back at the stop (a take of moves alone too, and Reset All
// Controllers' centred bend and reset routed lanes); routed VOL, PAN, BENDR
// and LFOS knobs land on their lanes as heard and play back as they
// sounded, a loop pass's VOL restored at the loop end; a
// track the song doesn't have can't record, and a take that can't be
// written isn't reported as recorded; an empty take pushes nothing; punch
// in and out while playing keeps playing,
// ignores a key held at punch-in and clamps the offset at punch-in; the
// target stays locked through a selection change; the sustain pedal extends
// a note; Fixed velocity is recorded; Play incoming notes off still records;
// MIDI: All Notes Off ends a held note unshifted; Auto takes the device
// latency; a loop wrap, a seek and a 2-pass overdub, and a take started at
// the loop end; a document edit mid-take; the capture ring overflowing; the
// song ending by itself; closing another tab, closing, destroying and
// reloading the take's own; a tab switch committing to the take's own tab;
// Record disabled on a read-only bundle tab; the ghosts (checkRecordingDisplay,
// checkRecordingLaneDisplay, checkRecordingCcLaneDisplay); a 3-knob flood,
// drained and with the UI stalled (checkRecordingFlood); the latch, song
// events of a moved lane held back during a take and back after each way it
// ends (checkRecordingLatch); program changes onto the Voice lane
// (checkRecordingVoice).

namespace {

int g_failures = 0;

bool check(bool ok, const char *what)
{
    if (!ok) {
        std::fprintf(stderr, "reccheck: FAIL: %s\n", what);
        g_failures++;
    }
    return ok;
}

using NewNote = SongDocument::NewNote;

bool sameNotes(const std::vector<NewNote> &got, std::initializer_list<NewNote> want)
{
    if (got.size() != want.size())
        return false;
    size_t i = 0;
    for (const NewNote &w : want) {
        const NewNote &g = got[i++];
        if (g.tick != w.tick || g.key != w.key || g.duration != w.duration ||
            g.velocity != w.velocity)
            return false;
    }
    return true;
}

void printNotes(const char *what, const std::vector<NewNote> &notes)
{
    std::fprintf(stderr, "reccheck:   %s:", what);
    for (const NewNote &n : notes)
        std::fprintf(stderr, " {%llu k%d d%u v%d}", (unsigned long long)n.tick, n.key, n.duration,
                     n.velocity);
    std::fprintf(stderr, "\n");
}

// RecordingTake::toNotes as step 4 first wrote it, with a plain O(n^2)
// later-wins pass a key: the reference the indexed pass must match exactly.
std::vector<NewNote> referenceToNotes(const RecordingTake &take, const MidiTimeline &timeline,
                                      uint64_t endSample, uint32_t endSegment,
                                      uint64_t latencySamples)
{
    const auto tickAt = [&](uint64_t sample) {
        return uint64_t(std::max<long long>(0, std::llround(timeline.tickForSample(sample))));
    };
    struct Span {
        uint64_t tick, end;
        uint8_t key, velocity;
        bool kept;
    };
    std::vector<Span> spans;
    const uint32_t mask = RecordingTake::kSegmentMask;
    for (const RecordingTake::Note &note : take.notes()) {
        uint64_t off = note.offSample;
        bool played = note.playedEnd;
        if (note.open) {
            const bool same =
                (note.segment & mask) == (endSegment & mask) && note.segment == take.segment();
            off = std::max(note.onSample,
                           same && endSample >= note.onSample ? endSample : take.lastSample());
            played = false;
        }
        const auto earlier = [](uint64_t sample, uint64_t by) {
            return sample > by ? sample - by : 0;
        };
        const uint64_t on =
            std::max(note.floorSample, earlier(note.onSample, latencySamples + note.onLate));
        if (played)
            off = std::max(on, earlier(off, latencySamples + note.offLate));
        const uint64_t tick = tickAt(on);
        spans.push_back({tick, std::max(tick + 1, tickAt(off)), note.key, note.velocity, true});
    }
    for (size_t i = 0; i < spans.size(); i++) {
        const Span &w = spans[i];
        for (size_t j = 0; j < i; j++) {
            Span &s = spans[j];
            if (s.key != w.key || !s.kept || s.end <= w.tick || s.tick >= w.end)
                continue;
            if (s.tick < w.tick)
                s.end = w.tick;
            else if (s.end > w.end)
                s.tick = w.end;
            else
                s.kept = false;
        }
    }
    std::vector<NewNote> out;
    for (const Span &s : spans) {
        if (s.kept)
            out.push_back({s.tick, s.key, uint32_t(s.end - s.tick), s.velocity});
    }
    std::stable_sort(out.begin(), out.end(), [](const NewNote &a, const NewNote &b) {
        return a.tick != b.tick ? a.tick < b.tick : a.key < b.key;
    });
    return out;
}

// Feeds one arm's events into a take, the way the audio thread stamps them.
struct Feed {
    RecordingTake &take;
    uint32_t generation;
    uint32_t segment = 0;
    uint32_t late = 0; // stamped on note ons and offs, bends and mods

    void push(uint8_t kind, uint64_t pos, uint8_t key = 0, uint8_t velocity = 0,
              uint64_t resume = 0)
    {
        CaptureEvent ev;
        ev.kind = kind;
        ev.samplePos = pos;
        ev.resumePos = resume;
        ev.generation = generation;
        ev.segment = segment;
        ev.key = key;
        ev.velocity = velocity;
        if (kind != CaptureEvent::Begin && kind != CaptureEvent::Cut &&
            kind != CaptureEvent::Boundary)
            ev.lateSamples = late;
        take.push(ev);
    }
    void control(uint8_t kind, uint64_t pos, int16_t value, uint8_t cc = 0, uint8_t flags = 0)
    {
        CaptureEvent ev;
        ev.kind = kind;
        ev.samplePos = pos;
        ev.generation = generation;
        ev.segment = segment;
        ev.key = cc;
        ev.value = value;
        ev.lateSamples = late;
        ev.flags = flags;
        take.push(ev);
    }
    // Reset All Controllers with the default routes, as the engine captures
    // it: the bend centred, then MOD, VOL and PAN at rest, each flagged.
    void resetAll(uint64_t pos)
    {
        control(CaptureEvent::Bend, pos, 0, 0, CaptureEvent::Reset);
        for (const uint8_t cc : {uint8_t(0x01), uint8_t(0x07), uint8_t(0x0A)})
            control(CaptureEvent::Control, pos, int16_t(RecordingTake::restValue(cc)), cc,
                    CaptureEvent::Reset);
    }
    void bend(uint64_t pos, int16_t value) { control(CaptureEvent::Bend, pos, value); }
    // A routed lane's move: cc is the lane's (0x01 MOD, 0x07 VOL, ...).
    void lane(uint8_t cc, uint64_t pos, int16_t value)
    {
        control(CaptureEvent::Control, pos, value, cc);
    }
    void mod(uint64_t pos, int16_t value) { lane(0x01, pos, value); }
    // A program change: value is the voice slot.
    void program(uint64_t pos, int16_t voice) { control(CaptureEvent::Program, pos, voice); }
    void begin(uint64_t pos) { push(CaptureEvent::Begin, pos); }
    void on(uint64_t pos, uint8_t key, uint8_t velocity = 100)
    {
        push(CaptureEvent::NoteOn, pos, key, velocity);
    }
    void off(uint64_t pos, uint8_t key) { push(CaptureEvent::NoteOff, pos, key); }
    void cut(uint64_t pos) { push(CaptureEvent::Cut, pos); }
    void jump(uint64_t from, uint64_t to)
    {
        segment++;
        push(CaptureEvent::Boundary, from, 0, 0, to);
    }
};

// Answers every "Unsaved Changes" prompt that comes up while it lives.
class PromptAnswer
{
  public:
    explicit PromptAnswer(QMessageBox::StandardButton button)
    {
        m_timer.setInterval(10);
        QObject::connect(&m_timer, &QTimer::timeout, [this, button] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box)
                return;
            QAbstractButton *b = box->button(button);
            if (!b)
                b = box->button(QMessageBox::Cancel);
            if (b) {
                m_answered++;
                b->click();
            } else {
                box->reject();
            }
        });
        m_timer.start();
    }
    PromptAnswer(const PromptAnswer &) = delete;
    PromptAnswer &operator=(const PromptAnswer &) = delete;
    int answered() const { return m_answered; }

  private:
    QTimer m_timer;
    int m_answered = 0;
};

// 48 kHz, 24 ticks per beat at 120 bpm: 1000 samples per tick. With
// tempoChange the tempo halves at tick 96 (sample 96000): 2000 per tick.
MidiTimeline makeTimeline(bool tempoChange)
{
    MidiTimeline tl;
    tl.sampleRate = 48000.0;
    tl.ticksPerBeat = 24;
    tl.tempoMap.push_back({0, 0, 120.0});
    if (tempoChange)
        tl.tempoMap.push_back({96, 96000, 60.0});
    return tl;
}

void checkTakeRules()
{
    const MidiTimeline tl = makeTimeline(false);
    const MidiTimeline tempo = makeTimeline(true);
    RecordingTake take;

    // The basics, and what belongs to no take: events before the Begin and
    // another arm's.
    {
        take.begin(3, 7);
        Feed feed{take, 7};
        feed.on(15000, 61);
        check(!take.started() && take.notes().empty(),
              "take: an event before the Begin was recorded");
        feed.begin(10000);
        feed.on(20000, 60, 100);
        Feed other{take, 6};
        other.on(25000, 62);
        feed.off(30000, 60);
        check(take.started() && take.punchInSample() == 10000 && take.track() == 3,
              "take: the Begin did not set the punch-in");
        const auto notes = take.toNotes(tl, 40000, 0, 0);
        if (!check(sameNotes(notes, {{20, 60, 10, 100}}),
                   "take: a played note did not convert to its ticks (or another arm's leaked)"))
            printNotes("got", notes);
    }
    // Nearest-tick rounding and the 1-tick minimum.
    {
        take.begin(0, 1);
        Feed feed{take, 1};
        feed.begin(0);
        feed.on(20499, 60);
        feed.off(30499, 60);
        feed.on(20500, 62);
        feed.off(20600, 62);
        feed.on(40000, 64);
        feed.off(40000, 64);
        const auto notes = take.toNotes(tl, 50000, 0, 0);
        if (!check(sameNotes(notes, {{20, 60, 10, 100}, {21, 62, 1, 100}, {40, 64, 1, 100}}),
                   "take: rounding to the nearest tick or the 1-tick minimum is wrong"))
            printNotes("got", notes);
    }
    // A tempo change mid-take: ticks follow the tempo map.
    {
        take.begin(0, 2);
        Feed feed{take, 2};
        feed.begin(0);
        feed.on(90000, 60);
        feed.off(110000, 60); // tick 96 + 14000 / 2000
        feed.on(120000, 62);  // tick 108
        feed.off(124000, 62);
        const auto notes = take.toNotes(tempo, 130000, 0, 0);
        if (!check(sameNotes(notes, {{90, 60, 13, 100}, {108, 62, 2, 100}}),
                   "take: a tempo change mid-take was not followed"))
            printNotes("got", notes);
    }
    // Held at stop: the note ends at the stop position. A stop position read
    // in another segment (a wrap the take hasn't seen yet) can't be compared:
    // the note ends at the last position the take saw instead.
    {
        take.begin(0, 3);
        Feed feed{take, 3};
        feed.begin(10000);
        feed.on(50000, 60);
        feed.on(60000, 62);
        feed.off(62000, 62);
        auto notes = take.toNotes(tl, 70000, 0, 0);
        if (!check(sameNotes(notes, {{50, 60, 20, 100}, {60, 62, 2, 100}}),
                   "take: a note held at stop did not end at the stop position"))
            printNotes("got", notes);
        notes = take.toNotes(tl, 90000, 1, 0);
        if (!check(sameNotes(notes, {{50, 60, 12, 100}, {60, 62, 2, 100}}),
                   "take: a stop read in another segment was compared with the note's"))
            printNotes("got", notes);
    }
    // A loop wrap: a note held across it ends at the loop end; its note-off
    // in the next pass matches nothing; the next pass records normally.
    {
        take.begin(0, 4);
        Feed feed{take, 4};
        feed.begin(10000);
        feed.on(80000, 60);
        feed.jump(100000, 40000);
        feed.off(50000, 60);
        feed.on(45000, 62);
        feed.off(47000, 62);
        const auto notes = take.toNotes(tl, 60000, 1, 0);
        if (!check(sameNotes(notes, {{45, 62, 2, 100}, {80, 60, 20, 100}}),
                   "take: a note held across a loop wrap did not end at the loop end"))
            printNotes("got", notes);
        check(take.unmatchedOffs() == 1, "take: the wrapped note's note-off was not ignored");
    }
    // Held across several wraps: still one note, ending at the loop end of
    // the pass it started in.
    {
        take.begin(0, 5);
        Feed feed{take, 5};
        feed.begin(10000);
        feed.on(80000, 60);
        feed.jump(100000, 40000);
        feed.jump(100000, 40000);
        feed.off(70000, 60);
        const auto notes = take.toNotes(tl, 90000, 2, 0);
        if (!check(sameNotes(notes, {{80, 60, 20, 100}}),
                   "take: a note held across two wraps did not end at the first loop end"))
            printNotes("got", notes);
    }
    // Where the take has recorded (the ruler's REC tint): the punched span,
    // each loop pass and seek landing, merged where they overlap; the
    // current one ends at the clock read in its segment, else at the last
    // position seen. Nothing before the Begin.
    {
        using Spans = std::vector<std::pair<uint64_t, uint64_t>>;
        take.begin(0, 14);
        Feed feed{take, 14};
        check(take.recordedSpans(5000, 0).empty(), "take: a take not yet begun has a span");
        feed.begin(50000);
        feed.on(60000, 60);
        check(take.recordedSpans(70000, 0) == Spans{{50000, 70000}},
              "take: the punched span does not run from the punch-in to the clock");
        check(take.recordedSpans(70000, 1) == Spans{{50000, 60000}},
              "take: a clock read in another segment did not stop at the last position");
        feed.jump(100000, 20000);
        check(take.recordedSpans(30000, 1) == Spans{{20000, 30000}, {50000, 100000}},
              "take: a loop pass is not a span of its own beside the first");
        check(take.recordedSpans(60000, 1) == Spans{{20000, 100000}},
              "take: overlapping passes did not merge into one span");
        feed.jump(70000, 5000);
        check(take.recordedSpans(8000, 2) == Spans{{5000, 8000}, {20000, 100000}},
              "take: a seek's landing did not start a span of its own");
    }
    // The latency offset moves played times earlier, clamped at punch-in
    // and at the loop start; an end the take imposed (the wrap) stays put.
    {
        take.begin(0, 6);
        Feed feed{take, 6};
        feed.begin(10000);
        feed.on(10500, 60); // 8500 before punch-in: clamped to 10000
        feed.off(15000, 60);
        feed.on(90000, 62); // held across the wrap: ends at 100000 unshifted
        feed.jump(100000, 40000);
        feed.on(40500, 64); // 38500 before the loop start: clamped to 40000
        feed.off(44000, 64);
        feed.on(50000, 65);
        feed.off(53000, 65);
        const auto notes = take.toNotes(tl, 60000, 1, 2000);
        if (!check(sameNotes(
                       notes,
                       {{10, 60, 3, 100}, {40, 64, 2, 100}, {48, 65, 3, 100}, {88, 62, 12, 100}}),
                   "take: the latency offset was not applied or not clamped"))
            printNotes("got", notes);
    }
    // Later passes win, by SongDocument::resolveNoteOverlaps' rules: overlaps
    // keep the earlier note's head or tail, or remove it when covered; a
    // repeated (tick, key) keeps the earlier note's tail when it outlasts the
    // later one (as a second take over the first would).
    {
        take.begin(0, 8);
        Feed feed{take, 8};
        feed.begin(40000);
        feed.on(50000, 60, 80);
        feed.off(52000, 60);
        feed.on(50000, 62); // head survives
        feed.off(60000, 62);
        feed.on(52000, 64); // covered
        feed.off(54000, 64);
        feed.on(52000, 65); // tail survives
        feed.off(70000, 65);
        feed.jump(100000, 40000);
        feed.on(50000, 60, 120);
        feed.off(51000, 60);
        feed.on(55000, 62);
        feed.off(58000, 62);
        feed.on(50000, 64);
        feed.off(60000, 64);
        feed.on(50000, 65);
        feed.off(60000, 65);
        const auto notes = take.toNotes(tl, 99000, 1, 0);
        if (!check(sameNotes(notes, {{50, 60, 1, 120},
                                     {50, 62, 5, 100},
                                     {50, 64, 10, 100},
                                     {50, 65, 10, 100},
                                     {51, 60, 1, 80},
                                     {55, 62, 3, 100},
                                     {60, 65, 10, 100}}),
                   "take: a later pass did not win over the earlier notes"))
            printNotes("got", notes);
    }
    // A cut ends held notes where it happened (not shifted); their later
    // note-off is unmatched. A re-press with its note-off lost ends the
    // first note at the second.
    {
        take.begin(0, 9);
        Feed feed{take, 9};
        feed.begin(10000);
        feed.on(20000, 60);
        feed.cut(25000);
        feed.off(30000, 60);
        feed.on(40000, 62);
        feed.on(45000, 62);
        check(take.notes().size() == 3 && !take.notes()[1].open,
              "take: a re-pressed key left its first note open");
        feed.off(50000, 62);
        const auto notes = take.toNotes(tl, 60000, 0, 1000);
        if (!check(sameNotes(notes, {{19, 60, 6, 100}, {39, 62, 5, 100}, {44, 62, 5, 100}}),
                   "take: a cut or a lost note-off was mishandled"))
            printNotes("got", notes);
        check(take.unmatchedOffs() == 1, "take: a note-off after a cut was not ignored");
    }
    // A Boundary lost to a full ring: the next segment's first event ends
    // what was held at the last position seen, and starts a segment there
    // (the offset can't move a note before it).
    {
        take.begin(0, 10);
        Feed feed{take, 10};
        feed.begin(10000);
        feed.on(80000, 60);
        feed.on(85000, 61);
        feed.off(86000, 61);
        feed.segment++; // its Boundary never arrives
        feed.on(45000, 62);
        feed.off(49000, 62);
        const auto notes = take.toNotes(tl, 50000, 1, 2000);
        if (!check(sameNotes(notes, {{45, 62, 2, 100}, {78, 60, 8, 100}, {83, 61, 1, 100}}),
                   "take: a lost Boundary did not end the held note at the last position"))
            printNotes("got", notes);
    }
    // A Boundary arriving after an earlier one was lost: what was held ends
    // at the last position seen, not at this Boundary's end (another
    // segment's position).
    {
        take.begin(0, 13);
        Feed feed{take, 13};
        feed.begin(10000);
        feed.on(80000, 60);
        feed.on(85000, 61);
        feed.off(86000, 61);
        feed.segment++;          // a wrap whose Boundary never arrives
        feed.jump(60000, 40000); // then a seek's, ending at 60000
        feed.on(45000, 62);
        feed.off(49000, 62);
        const auto notes = take.toNotes(tl, 50000, 2, 0);
        if (!check(sameNotes(notes, {{45, 62, 4, 100}, {80, 60, 6, 100}, {85, 61, 1, 100}}),
                   "take: a Boundary after a lost one ended the held note at its own position"))
            printNotes("got", notes);
    }
    // A message's wait for its callback moves its time back (the note on's
    // and the played off's each), on top of the offset and still floored at
    // the segment start.
    {
        take.begin(0, 12);
        Feed feed{take, 12};
        feed.begin(10000);
        feed.late = 3000;
        feed.on(20000, 60);
        feed.late = 400;
        feed.off(30000, 60);
        feed.late = 5000;
        feed.on(11000, 61);
        feed.late = 0;
        feed.off(13000, 61);
        auto notes = take.toNotes(tl, 40000, 0, 0);
        if (!check(sameNotes(notes, {{10, 61, 3, 100}, {17, 60, 13, 100}}),
                   "take: a message's wait for its callback did not move it back"))
            printNotes("got", notes);
        notes = take.toNotes(tl, 40000, 0, 1000);
        if (!check(sameNotes(notes, {{10, 61, 2, 100}, {16, 60, 13, 100}}),
                   "take: the wait and the offset did not add up"))
            printNotes("got", notes);
    }
    // The later-wins pass is indexed (a live preview runs it every playhead
    // tick): on randomized takes (loop passes, seeks, lost Boundaries,
    // re-presses, cuts, shared starts, callback waits, held notes, a clock
    // in another segment) it matches the plain pass exactly.
    {
        std::mt19937 rng(20261011);
        const auto pick = [&](uint32_t n) { return uint32_t(rng() % n); };
        int mismatches = 0;
        for (int trial = 0; trial < 400 && mismatches == 0; trial++) {
            take.begin(0, uint32_t(1000 + trial));
            Feed feed{take, uint32_t(1000 + trial)};
            uint64_t pos = 10000 + pick(5000);
            feed.begin(pos);
            const int events = 5 + int(pick(250));
            for (int e = 0; e < events; e++) {
                pos += pick(4) == 0 ? 0 : pick(3000);
                feed.late = pick(4) == 0 ? pick(4000) : 0;
                const uint8_t key = uint8_t(60 + pick(3));
                switch (pick(16)) {
                case 0: // a loop wrap back to an earlier start
                case 1: {
                    const uint64_t to = 5000 + pick(uint32_t(std::max<uint64_t>(pos, 6000) - 5000));
                    feed.jump(pos, to);
                    pos = to;
                    break;
                }
                case 2: // a seek anywhere
                    pos = 5000 + pick(200000);
                    feed.jump(pos + pick(3000), pos);
                    break;
                case 3: // a Boundary lost to a full ring
                    feed.segment++;
                    pos = 5000 + pick(100000);
                    break;
                case 4:
                    feed.cut(pos);
                    break;
                case 5:
                case 6:
                case 7:
                case 8:
                case 9:
                    feed.on(pos, key, uint8_t(1 + pick(127)));
                    break;
                default:
                    feed.off(pos, key);
                    break;
                }
            }
            const uint64_t end = pos + pick(20000);
            const uint32_t endSegment = feed.segment + (pick(4) == 0 ? 1 : 0);
            const uint64_t latency = std::array<uint64_t, 3>{0, 700, 3000}[pick(3)];
            const MidiTimeline &timeline = pick(2) ? tl : tempo;
            const auto got = take.toNotes(timeline, end, endSegment, latency);
            const auto want = referenceToNotes(take, timeline, end, endSegment, latency);
            if (got != want) {
                mismatches++;
                std::fprintf(stderr, "reccheck:   trial %d\n", trial);
                printNotes("got", got);
                printNotes("want", want);
            }
        }
        check(mismatches == 0, "take: the indexed later-wins pass differs from the plain one");
    }
    // A long loop take on one key (5000 notes over 50 passes, each pass
    // shifted a little): the pass still matches, and it visits a few notes
    // per note, not every earlier note on the key.
    {
        take.begin(0, 30);
        Feed feed{take, 30};
        feed.begin(10000);
        int count = 0;
        for (int pass = 0; pass < 50; pass++) {
            const uint64_t start = 10000 + uint64_t(pass % 7) * 300;
            for (int i = 0; i < 100; i++) {
                const uint64_t on = start + uint64_t(i) * 4000;
                feed.on(on, 60, uint8_t(1 + (count % 127)));
                feed.off(on + 2500 + uint64_t(i % 5) * 1000, 60);
                count++;
            }
            feed.jump(start + 100 * 4000, 10000);
        }
        // and one held pass at the end, in one segment
        for (int i = 0; i < 50; i++)
            feed.on(10000 + uint64_t(i) * 7000, 60);
        count += 50;
        const auto got = take.toNotes(tl, 600000, feed.segment, 0);
        const size_t steps = take.overlapSteps();
        check(got == referenceToNotes(take, tl, 600000, feed.segment, 0),
              "take: the long take's later-wins pass differs from the plain one");
        if (!check(steps <= size_t(4 * count),
                   "take: the later-wins pass visited more than a few notes per note"))
            std::fprintf(stderr, "reccheck:   %zu steps for %d notes\n", steps, count);
    }
    // Bend and mod moves become lane writes: one per lane and segment,
    // spanning its first move to the segment's end; nearest-tick rounding,
    // last move on a tick wins, a move the engine can't hear dropped (its
    // value kept by the point it sounds like), the wait and the offset
    // shifted and clamped at the segment start, ticks never running
    // backwards; nothing before the Begin or from another arm.
    {
        using LanePointValue = SongDocument::LanePointValue;
        const auto samePoints = [](const std::vector<LanePointValue> &got,
                                   std::initializer_list<LanePointValue> want) {
            if (got.size() != want.size())
                return false;
            size_t i = 0;
            for (const LanePointValue &w : want) {
                if (got[i].tick != w.tick || got[i].value != w.value)
                    return false;
                i++;
            }
            return true;
        };
        take.begin(0, 12);
        Feed feed{take, 12};
        feed.bend(5000, 100);
        feed.begin(10000);
        Feed other{take, 11};
        other.mod(21000, 9);
        feed.bend(20000, 4096);
        feed.bend(20400, 4100); // same tick: wins
        feed.mod(22000, 50);
        feed.bend(25000, 4101); // the same 7-bit bend: dropped
        feed.bend(30000, 0);
        feed.mod(31000, 20);
        feed.late = 5000;
        feed.mod(31000, 30); // waited back to tick 26: stays after tick 31, wins it
        feed.late = 0;
        check(take.controls().size() == 7,
              "take: a bend or mod move was not kept (or a stray was)");
        feed.jump(40000, 10000);
        feed.mod(10500, 70);
        feed.late = 500;
        feed.mod(15000, 60);
        feed.late = 0;
        // Stopped at 16000 in the second segment: its writes end there, the
        // first segment's at its Boundary (40000), neither shifted.
        const auto writes = take.toLaneWrites(tl, 16000, 1, 2000);
        const bool shape = writes.size() == 3 && writes[0].cc == 0x01 &&
                           writes[1].cc == DOC_CC_BEND && writes[2].cc == 0x01;
        if (check(shape, "take: the lane writes are not one per lane and segment, in order")) {
            // Latency 2000 moves the first segment's moves 2 ticks earlier.
            check(writes[0].tickBegin == 20 && writes[0].tickEnd == 40 && writes[0].restoreEnd &&
                      samePoints(writes[0].points, {{20, 50}, {29, 30}}),
                  "take: the mod moves did not convert (or a reordered move ran backwards)");
            // 4101 sounds as 4100 did: dropped, the point keeping its value.
            check(writes[1].tickBegin == 18 && writes[1].tickEnd == 40 && writes[1].restoreEnd &&
                      samePoints(writes[1].points, {{18, 4101}, {28, 0}}),
                  "take: the bend moves did not convert (same tick, inaudible change)");
            check(writes[2].tickBegin == 10 && writes[2].tickEnd == 16 && writes[2].restoreEnd &&
                      samePoints(writes[2].points, {{10, 70}, {13, 60}}),
                  "take: a later segment's moves were not floored at its start and shifted");
        }
        take.begin(0, 13);
        Feed none{take, 13};
        none.begin(10000);
        none.on(12000, 60);
        check(take.toLaneWrites(tl, 20000, 0, 0).empty(),
              "take: a take without moves wrote lane points");
        // Where a write ends: a Boundary on its last move's tick, a lost
        // Boundary (the last position seen), the stop position read in the
        // current segment or not; an end on or before the last move's tick
        // restores nothing.
        take.begin(0, 14);
        Feed ends{take, 14};
        ends.begin(0);
        ends.bend(3000, 108);
        ends.bend(5000, 0); // the spring back: sounds as 108, rests at 0
        ends.jump(5000, 0);
        ends.bend(1000, 500);
        ends.mod(2000, 10);
        ends.segment++; // its Boundary lost: segment 1 ends at 2000
        ends.mod(8000, 40);
        const auto atStop = take.toLaneWrites(tl, 9000, 2, 0);
        const bool endShape = atStop.size() == 4 && atStop[0].cc == DOC_CC_BEND &&
                              atStop[1].cc == 0x01 && atStop[2].cc == DOC_CC_BEND &&
                              atStop[3].cc == 0x01;
        if (check(endShape, "take: the ended writes are not one per lane and segment")) {
            check(atStop[0].tickEnd == 5 && !atStop[0].restoreEnd &&
                      samePoints(atStop[0].points, {{3, 0}}),
                  "take: a bend's inaudible spring back did not rest at 0 (or a Boundary on "
                  "the last move restored)");
            check(atStop[1].tickEnd == 2 && !atStop[1].restoreEnd,
                  "take: a lost Boundary on the last move's tick restored");
            check(atStop[2].tickBegin == 1 && atStop[2].tickEnd == 2 && atStop[2].restoreEnd,
                  "take: a lost Boundary did not end the segment at the last position seen");
            check(atStop[3].tickBegin == 8 && atStop[3].tickEnd == 9 && atStop[3].restoreEnd,
                  "take: the current segment's write did not end at the stop");
        }
        const auto elsewhere = take.toLaneWrites(tl, 9000, 1, 0);
        const auto early = take.toLaneWrites(tl, 7000, 2, 0);
        check(elsewhere.size() == 4 && elsewhere[3].tickEnd == 8 && !elsewhere[3].restoreEnd &&
                  early.size() == 4 && early[3].tickEnd == 8 && !early[3].restoreEnd,
              "take: a stop read in another segment (or before the last move) still ended "
              "the write there");
        // The restore point: the song's value at the write's end before the
        // take (its last point there or before, else 0), when it sounds
        // different from the take's last.
        {
            const auto pts = [](std::initializer_list<std::pair<uint64_t, int>> list) {
                std::vector<DocLanePoint> out;
                for (const auto &[tick, value] : list) {
                    DocLanePoint pt;
                    pt.tick = tick;
                    pt.value = value;
                    out.push_back(pt);
                }
                return out;
            };
            RecordingTake::LaneWrite w;
            w.cc = DOC_CC_BEND;
            w.tickBegin = 10;
            w.tickEnd = 40;
            w.points = {{10, 4096}, {30, 0}};
            w.restoreEnd = true;
            LanePointValue r{0, 0};
            check(w.restorePoint(pts({{0, 2000}, {20, 1000}, {50, 3000}}), &r) && r.tick == 40 &&
                      r.value == 1000,
                  "take: the end did not restore the song's value there");
            check(w.restorePoint(pts({{0, 2000}, {40, -500}}), &r) && r.tick == 40 &&
                      r.value == -500,
                  "take: a point on the end tick did not come back");
            check(!w.restorePoint(pts({{20, 100}}), &r) && !w.restorePoint(pts({}), &r),
                  "take: an end that sounds like the take's last value got a point");
            w.restoreEnd = false;
            check(!w.restorePoint(pts({{20, 1000}}), &r),
                  "take: a write ending on its last move restored");
            w.cc = 0x01;
            w.points = {{10, 30}};
            w.restoreEnd = true;
            check(w.restorePoint(pts({}), &r) && r.value == 0 &&
                      !w.restorePoint(pts({{5, 30}}), &r),
                  "take: a MOD end did not restore the song's value (0 with no point)");
        }
        // The routed lanes (docs/midi-controls/PLAN.md step 3): VOL, PAN,
        // BENDR and LFOS moves follow MOD's rules, each lane its own write,
        // in ascending CC with BEND last within a segment; a later segment
        // starts the order again.
        take.begin(0, 15);
        Feed cc{take, 15};
        cc.begin(0);
        cc.lane(0x15, 1000, 40);  // LFOS
        cc.bend(1500, 4096);      // BEND
        cc.lane(0x0A, 2000, 0);   // PAN hard left
        cc.lane(0x0A, 2400, 127); // same tick: wins
        cc.lane(0x07, 3000, 90);  // VOL
        cc.lane(0x07, 4000, 90);  // the same VOL again: dropped
        cc.lane(0x07, 5000, 91);  // one step: kept (identity, no 7-bit fold)
        cc.lane(0x14, 6000, 12);  // BENDR, semitones as heard
        cc.lane(0x01, 6500, 5);   // MOD
        cc.lane(0x15, 7000, 41);
        cc.jump(8000, 0);
        cc.lane(0x14, 1000, 24);
        cc.lane(0x07, 2000, 10);
        check(take.controls().size() == 12 && take.controls()[2].cc == 0x0A &&
                  take.controls()[6].cc == 0x07 && take.controls()[7].cc == 0x14 &&
                  take.controls()[7].value == 12,
              "take: a routed lane's move did not keep its lane (the key) and value");
        const auto ccWrites = take.toLaneWrites(tl, 3000, 1, 0);
        const std::vector<uint8_t> wantOrder{0x01, 0x07, 0x0A, 0x14, 0x15, DOC_CC_BEND, 0x07, 0x14};
        std::vector<uint8_t> order;
        for (const RecordingTake::LaneWrite &w : ccWrites)
            order.push_back(w.cc);
        if (check(order == wantOrder, "take: the routed lanes' writes are not in ascending CC, "
                                      "BEND last, segment by segment")) {
            check(ccWrites[0].tickBegin == 7 && samePoints(ccWrites[0].points, {{7, 5}}) &&
                      ccWrites[0].tickEnd == 8 && ccWrites[0].restoreEnd,
                  "take: the MOD write among the routed lanes is off");
            check(ccWrites[1].tickBegin == 3 && ccWrites[1].tickEnd == 8 &&
                      ccWrites[1].restoreEnd && samePoints(ccWrites[1].points, {{3, 90}, {5, 91}}),
                  "take: the VOL moves did not convert (a repeat kept, or a step dropped)");
            check(ccWrites[2].tickBegin == 2 && samePoints(ccWrites[2].points, {{2, 127}}),
                  "take: the PAN moves on one tick did not keep the last");
            check(samePoints(ccWrites[3].points, {{6, 12}}) &&
                      samePoints(ccWrites[4].points, {{1, 40}, {7, 41}}),
                  "take: the BENDR or LFOS moves did not convert");
            check(ccWrites[6].tickBegin == 2 && ccWrites[6].tickEnd == 3 &&
                      samePoints(ccWrites[6].points, {{2, 10}}) &&
                      samePoints(ccWrites[7].points, {{1, 24}}),
                  "take: the second segment's VOL and BENDR writes are off");
        }
        // Each lane's restore reads its own song value, and with no point
        // before, the engine's track-init value: VOL 127, PAN 64, BENDR 2,
        // LFOS 22 (MOD 0, bend centred).
        {
            check(RecordingTake::restValue(0x07) == 127 && RecordingTake::restValue(0x0A) == 64 &&
                      RecordingTake::restValue(0x14) == 2 && RecordingTake::restValue(0x15) == 22 &&
                      RecordingTake::restValue(0x01) == 0 &&
                      RecordingTake::restValue(DOC_CC_BEND) == 0,
                  "take: a lane's rest value is not the engine's track-init value");
            const auto pts = [](std::initializer_list<std::pair<uint64_t, int>> list) {
                std::vector<DocLanePoint> out;
                for (const auto &[tick, value] : list) {
                    DocLanePoint pt;
                    pt.tick = tick;
                    pt.value = value;
                    out.push_back(pt);
                }
                return out;
            };
            RecordingTake::LaneWrite w;
            w.tickBegin = 10;
            w.tickEnd = 40;
            w.restoreEnd = true;
            LanePointValue r{0, 0};
            bool rests = true;
            for (const auto &[lane, rest] :
                 {std::pair{uint8_t(0x07), 127}, std::pair{uint8_t(0x0A), 64},
                  std::pair{uint8_t(0x14), 2}, std::pair{uint8_t(0x15), 22}}) {
                w.cc = lane;
                // Left away from rest: the rest comes back at the end.
                w.points = {{10, rest == 2 ? 24 : 0}};
                rests &= w.restorePoint(pts({}), &r) && r.tick == 40 && r.value == rest;
                // Left at rest: nothing to restore.
                w.points = {{10, 0}, {20, rest}};
                rests &= !w.restorePoint(pts({}), &r);
                // A song point before the end wins over the rest.
                w.points = {{10, rest}};
                rests &= w.restorePoint(pts({{5, 33}}), &r) && r.value == 33;
            }
            check(rests, "take: a routed lane's end did not restore the song's value, or its "
                         "track-init value with no point before");
            // applyLaneWrites keeps to its lane: PAN's write leaves VOL be.
            RecordingTake::LaneWrite pan;
            pan.cc = 0x0A;
            pan.tickBegin = 10;
            pan.tickEnd = 20;
            pan.points = {{10, 0}};
            pan.restoreEnd = true;
            const auto volLane = RecordingTake::applyLaneWrites(pts({{0, 100}}), {pan}, 0x07);
            const auto panLane = RecordingTake::applyLaneWrites(pts({}), {pan}, 0x0A);
            check(samePoints(volLane, {{0, 100}}) && samePoints(panLane, {{10, 0}, {20, 64}}),
                  "take: a PAN write moved another lane, or did not restore centre with no point");
        }
        // CC121's resets (J5, refined after step 3's review) are moves only
        // on a lane the take already moved, in any segment: VOL moved in
        // pass 1 keeps its resets in both passes, bend's counts once it
        // moved in pass 2, and MOD and PAN, never moved, keep the song's.
        take.begin(0, 16);
        Feed reset{take, 16};
        reset.begin(0);
        reset.lane(0x07, 1000, 40);
        reset.resetAll(2000);
        reset.jump(3000, 0);
        reset.bend(500, 4096);
        reset.resetAll(1000);
        {
            std::vector<std::pair<uint8_t, int>> got;
            for (const RecordingTake::Control &c : take.controls())
                got.push_back({c.cc, c.value});
            const std::vector<std::pair<uint8_t, int>> want{
                {0x07, 40}, {0x07, 127}, {DOC_CC_BEND, 4096}, {DOC_CC_BEND, 0}, {0x07, 127}};
            check(got == want && take.controls()[1].segment == 0 && take.controls()[4].segment == 1,
                  "take: a CC121 reset was kept on a lane the take never moved, or dropped on "
                  "one it moved (in this pass or an earlier one)");
        }
        // A new arm starts with no lane moved.
        take.begin(0, 17);
        Feed fresh{take, 17};
        fresh.begin(0);
        fresh.resetAll(1000);
        check(take.controls().empty() && take.toLaneWrites(tl, 2000, 0, 0).empty(),
              "take: a CC121 reset in a take that moved nothing was recorded");
        take.clear();
        check(take.controls().empty(), "take: clear() kept the moves");
        // Program changes (docs/midi-controls/PLAN.md step 5) are Voice
        // lane moves: same-tick last wins, a repeat is dropped, and within a
        // segment the voice writes after BEND.
        {
            take.begin(0, 18);
            Feed v{take, 18};
            v.begin(0);
            v.program(1000, 5);
            v.lane(0x07, 1500, 90);
            v.bend(1600, 4096);
            v.program(2000, 7);
            v.program(2400, 9); // same tick: wins
            v.program(3000, 9); // a repeat: dropped
            v.program(5000, 10);
            v.jump(6000, 0);
            v.program(1000, 11);
            check(take.controls().size() == 8 && take.controls()[0].cc == DOC_CC_VOICE &&
                      take.controls()[0].value == 5 && take.controls()[1].cc == 0x07,
                  "take: a program change did not become a Voice lane move with its slot");
            const auto vw = take.toLaneWrites(tl, 3000, 1, 0);
            std::vector<uint8_t> vorder;
            for (const RecordingTake::LaneWrite &w : vw)
                vorder.push_back(w.cc);
            if (check(vorder == std::vector<uint8_t>{0x07, DOC_CC_BEND, DOC_CC_VOICE, DOC_CC_VOICE},
                      "take: the voice's writes are not after BEND, segment by segment")) {
                check(vw[2].tickBegin == 1 && vw[2].tickEnd == 6 && vw[2].restoreEnd &&
                          samePoints(vw[2].points, {{1, 5}, {2, 9}, {5, 10}}),
                      "take: the voice moves did not convert (same tick, a repeat)");
                check(vw[3].tickBegin == 1 && vw[3].tickEnd == 3 && vw[3].restoreEnd &&
                          samePoints(vw[3].points, {{1, 11}}),
                      "take: the second segment's voice write is off");
            }
            take.clear();
        }
        // The voice's restore: the song's program at or before the end,
        // else its first later one (what primeVoices plays there), else
        // nothing (no event stands for "no voice"). And its lead point: a
        // take that would become the track's first voice puts the song's
        // first one at tick 0.
        {
            const auto pts = [](std::initializer_list<std::pair<uint64_t, int>> list) {
                std::vector<DocLanePoint> out;
                for (const auto &[tick, value] : list) {
                    DocLanePoint pt;
                    pt.tick = tick;
                    pt.value = value;
                    out.push_back(pt);
                }
                return out;
            };
            RecordingTake::LaneWrite w;
            w.cc = DOC_CC_VOICE;
            w.tickBegin = 10;
            w.tickEnd = 40;
            w.points = {{10, 5}, {20, 6}};
            w.restoreEnd = true;
            LanePointValue r{0, 0};
            check(!w.restorePoint(pts({}), &r),
                  "take: a voice end restored with no program change in the song");
            check(w.restorePoint(pts({{0, 3}, {50, 4}}), &r) && r.tick == 40 && r.value == 3,
                  "take: a voice end did not restore the song's program before it");
            check(w.restorePoint(pts({{25, 8}, {50, 4}}), &r) && r.tick == 40 && r.value == 8,
                  "take: a voice end did not restore the program inside its range");
            check(w.restorePoint(pts({{50, 4}, {60, 2}}), &r) && r.tick == 40 && r.value == 4,
                  "take: a voice end with no program before did not restore the first later one");
            check(!w.restorePoint(pts({{0, 6}}), &r) && !w.restorePoint(pts({{50, 6}}), &r),
                  "take: a voice end that already plays the song's voice got a point");
            const auto lead = [&](std::vector<DocLanePoint> before, uint64_t begin,
                                  std::vector<LanePointValue> *out, uint64_t *rangeEnd) {
                RecordingTake::LaneWrite l = w;
                l.tickBegin = begin;
                l.points = {{begin, 5}};
                RecordingTake::LaneWrite later = l;
                later.tickBegin = begin + 100;
                later.tickEnd = begin + 140;
                later.points = {{begin + 100, 7}};
                RecordingTake::LaneWrite vol;
                vol.cc = 0x07;
                vol.tickBegin = 0;
                vol.points = {{0, 1}};
                return RecordingTake::voiceLeadPoint(before, {vol, later, l}, out, rangeEnd);
            };
            std::vector<LanePointValue> ld;
            uint64_t le = 99;
            check(!lead(pts({}), 10, &ld, &le) && !lead(pts({{0, 3}}), 10, &ld, &le) &&
                      !lead(pts({{5, 3}}), 10, &ld, &le) && !lead(pts({{50, 3}}), 0, &ld, &le),
                  "take: a voice lead point was given where the song keeps its first voice");
            check(lead(pts({{10, 3}}), 10, &ld, &le) && samePoints(ld, {{0, 3}}) && le == 0 &&
                      lead(pts({{50, 4}, {60, 2}}), 10, &ld, &le) && samePoints(ld, {{0, 4}}),
                  "take: a take becoming the track's first voice got no lead point (the song's "
                  "first, at tick 0)");
            // The lead is a move: the song's first change goes where the
            // take's restore already plays it (here at the write's end, 40,
            // before the 50), and is left to a write that covers it.
            check(lead(pts({{50, 4}, {60, 2}}), 10, &ld, &le) && le == 50,
                  "take: the voice lead left the song's first change behind after a restore");
            check(lead(pts({{10, 3}}), 10, &ld, &le) && le == 0 &&
                      lead(pts({{40, 3}}), 10, &ld, &le) && le == 0,
                  "take: the voice lead cleared past a write that already covers the song's "
                  "first change");
            // The preview's lane: the lead, the take, the first later
            // program restored at the end; the song's own there has moved.
            const auto voiceLane =
                RecordingTake::applyLaneWrites(pts({{50, 4}}), {w}, DOC_CC_VOICE);
            check(samePoints(voiceLane, {{0, 4}, {10, 5}, {20, 6}, {40, 4}}),
                  "take: the Voice lane preview is not the lead, the take and its restore (the "
                  "song's first voice moved, not copied)");
            check(samePoints(RecordingTake::applyLaneWrites(pts({{0, 4}}), {w}, DOC_CC_VOICE),
                             {{0, 4}, {10, 5}, {20, 6}, {40, 4}}),
                  "take: the Voice lane preview over a tick-0 voice is off");
            // The first change inside the write, on its end, just past it.
            check(samePoints(
                      RecordingTake::applyLaneWrites(pts({{30, 4}, {70, 2}}), {w}, DOC_CC_VOICE),
                      {{0, 4}, {10, 5}, {20, 6}, {40, 4}, {70, 2}}) &&
                      samePoints(RecordingTake::applyLaneWrites(pts({{40, 4}}), {w}, DOC_CC_VOICE),
                                 {{0, 4}, {10, 5}, {20, 6}, {40, 4}}) &&
                      samePoints(RecordingTake::applyLaneWrites(pts({{41, 4}}), {w}, DOC_CC_VOICE),
                                 {{0, 4}, {10, 5}, {20, 6}, {40, 4}}),
                  "take: the Voice lane preview is not minimal with the song's first change "
                  "inside, on or next to the write's end");
            // The take ending on the song's voice (nothing restored): the
            // first change still goes, the take's last already plays it.
            {
                RecordingTake::LaneWrite same = w;
                same.points = {{10, 5}, {20, 4}};
                check(
                    samePoints(RecordingTake::applyLaneWrites(pts({{50, 4}}), {same}, DOC_CC_VOICE),
                               {{0, 4}, {10, 5}, {20, 4}}),
                    "take: a take ending on the song's first voice left it behind");
            }
            // Stopped on the take's last move (no restore): the take's voice
            // runs on to the song's first change, which still changes it.
            {
                RecordingTake::LaneWrite open = w;
                open.tickEnd = 20;
                open.restoreEnd = false;
                std::vector<LanePointValue> lp;
                uint64_t end = 99;
                check(RecordingTake::voiceLeadPoint(pts({{50, 4}}), {open}, &lp, &end) &&
                          end == 0 && samePoints(lp, {{0, 4}}) &&
                          samePoints(
                              RecordingTake::applyLaneWrites(pts({{50, 4}}), {open}, DOC_CC_VOICE),
                              {{0, 4}, {10, 5}, {20, 6}, {50, 4}}),
                      "take: an open take's voice lost the song's first change after it");
                // Several changes on the first tick (last wins): the lead
                // takes what played before them (the first, primeVoices);
                // they go when the take's restore already plays their last,
                // else stay as that last alone.
                check(samePoints(RecordingTake::applyLaneWrites(pts({{50, 4}, {50, 2}}), {open},
                                                                DOC_CC_VOICE),
                                 {{0, 4}, {10, 5}, {20, 6}, {50, 2}}),
                      "take: same-tick first changes after an open take are not their last");
                check(samePoints(RecordingTake::applyLaneWrites(pts({{50, 4}, {50, 2}}), {w},
                                                                DOC_CC_VOICE),
                                 {{0, 4}, {10, 5}, {20, 6}, {40, 4}, {50, 2}}),
                      "take: same-tick first changes after a restore are not reduced to their "
                      "last");
            }
        }
    }
    // A take whose Begin never came records nothing.
    {
        take.begin(0, 11);
        check(take.active() && !take.started() && take.toNotes(tl, 50000, 0, 0).empty(),
              "take: a take that never began produced notes");
        take.clear();
        check(!take.active(), "take: clear() left the take armed");
    }
}

} // namespace

int MainWindow::runRecordWindowCheck(const QString &projectRoot, const QString &songLabel,
                                     const QString &scratchDir)
{
    const int before = g_failures;
    // The undo text's lane list: pitch bend first, the CC lanes ascending,
    // the voice last, and a lane it has no name for still named.
    {
        const auto text = [](std::initializer_list<uint8_t> ccs) {
            std::vector<RecordingTake::LaneWrite> writes;
            for (const uint8_t cc : ccs) {
                RecordingTake::LaneWrite w;
                w.cc = cc;
                writes.push_back(w);
            }
            return takeLanesText(writes);
        };
        check(text({}).isEmpty() && text({0x01}) == QStringLiteral("mod") &&
                  text({0x01, DOC_CC_BEND, 0x01}) == QStringLiteral("pitch bend and mod") &&
                  text({0x0A, 0x07, DOC_CC_BEND, 0x01}) ==
                      QStringLiteral("pitch bend, mod, volume and pan") &&
                  text({DOC_CC_VOICE, 0x15, 0x14}) ==
                      QStringLiteral("bend range, LFO speed and voice") &&
                  text({DOC_CC_VOICE}) == QStringLiteral("voice"),
              "take text: the recorded lanes are not named and ordered as their rows");
        check(text({0x18}) == QStringLiteral("CC 24") &&
                  text({DOC_CC_VOICE, 0x18, 0x07}) == QStringLiteral("volume, CC 24 and voice"),
              "take text: a lane with no name vanished from the list");
    }
    if (!check(openProjectDir(projectRoot, /*interactive=*/false), "the project failed to open"))
        return g_failures - before;
    loadSongByLabel(songLabel);
    if (!m_active || m_active->doc.label() != songLabel) {
        std::fprintf(stderr, "reccheck: song '%s' did not load\n", qUtf8Printable(songLabel));
        check(false, "the song did not load");
        return g_failures - before;
    }
    check(m_recordAction && m_transportToolbar->actions().contains(m_recordAction) &&
              m_recordAction->isCheckable(),
          "Record is not a checkable action on the transport toolbar");
    // No keyboard, no Record button; a connected one brings it back.
    if (!midiInput().openKeyboardPorts().isEmpty()) {
        std::printf("reccheck: no-keyboard checks SKIP (a real MIDI keyboard port is open)\n");
    } else {
        check(!m_recordAction->isVisible(), "with no MIDI input the Record button is shown");
        toggleRecord();
        check(m_takeSession == nullptr, "with no MIDI input Record started a take");
    }
    midiInput().setOpenPortsForTest({QStringLiteral("Test Keyboard")});
    check(m_recordAction->isVisible(), "a connected keyboard did not show the Record button");
    if (!m_audioOk) {
        check(m_recordAction && !m_recordAction->isEnabled(),
              "with no audio device Record is enabled");
        std::printf("reccheck: recording SKIP (no audio device)\n");
        return g_failures - before;
    }
    checkRecordingTakes(scratchDir);
    return g_failures - before;
}

// The window half of the file comment.
void MainWindow::checkRecordingTakes(const QString &scratchDir)
{
    MidiInput &midi = midiInput();
    SongSession *session = m_active;
    SongView *view = session->view;
    SongDocument &doc = session->doc;
    QUndoStack *undo = doc.undoStack();
    m_audio.holdDeviceForTest(true);
    {
        MidiMsg msg;
        while (midi.ring().pop(&msg)) {
        }
    }
    const double rate = m_audio.sampleRate();
    const auto pumpMs = [&](int ms) { m_audio.pumpForTest(uint32_t(rate * ms / 1000.0)); };
    const auto send = [&](uint8_t status, uint8_t d1, uint8_t d2) {
        midi.injectForTest({status, d1, d2});
    };
    const auto playhead = [&] { return m_audio.playheadSamples(); };
    // The take's timeline is the session's current one (rebuilt by edits).
    const auto tickOf = [&](uint64_t sample) {
        return uint64_t(std::llround(session->timeline->tickForSample(sample)));
    };
    const auto sampleOf = [&](uint64_t tick) { return session->timeline->sampleForTick(tick); };
    const auto find = [&](int track, int key, uint64_t tick) -> std::optional<DocNote> {
        for (const DocNote &n : doc.notesForTrack(track)) {
            if (n.key == key && n.tick == tick)
                return n;
        }
        return std::nullopt;
    };
    const auto noteList = [&](int track) {
        std::vector<std::array<uint64_t, 4>> out;
        for (const DocNote &n : doc.notesForTrack(track))
            out.push_back({n.tick, n.key, n.duration, n.velocity});
        return out;
    };
    const auto lastUndoText = [&] {
        return undo->index() > 0 ? undo->text(undo->index() - 1) : QString();
    };
    const auto recording = [&] { return m_takeSession != nullptr; };
    const auto message = [&] { return statusBar()->currentMessage(); };
    const auto saysRecordingInto = [&](int track) {
        const QString m = message();
        return m.startsWith(QStringLiteral("Recording into %1: ").arg(track + 1)) ||
               m == QStringLiteral("Recording into Track %1…").arg(track + 1);
    };
    // One audio period in ticks (at the song's opening tempo): the capture
    // jitter the plan allows. Pumped time is exact, so this only bounds it.
    const double samplesPerTick = double(sampleOf(session->timeline->ticksPerBeat)) /
                                  double(std::max<uint32_t>(session->timeline->ticksPerBeat, 1));
    const uint64_t slop =
        1 + uint64_t(std::ceil(double(m_audio.periodSizeFrames()) / samplesPerTick));

    // Two used tracks: the target and a second one for the selection lock.
    int a = -1, b = -1;
    for (int t = 0; t < MAX_TRACKS && b < 0; t++) {
        if (!session->timeline->tracks[t].used)
            continue;
        (a < 0 ? a : b) = t;
    }
    if (!check(a >= 0 && b >= 0, "record: the song has no two used tracks")) {
        m_audio.holdDeviceForTest(false);
        return;
    }
    const uint64_t tpb = session->timeline->ticksPerBeat;
    view->selectTrack(a);
    midi.setLatencyMs(0);
    midi.setFixedVelocity(0);
    midi.setLivePlay(true);
    check(m_recordAction->isEnabled() && !m_recordAction->isChecked(),
          "record: Record is not available on an editable song");
    check(keymap::Registry::instance().bindings(QStringLiteral("transport.record")) ==
              QList<QKeySequence>{QKeySequence(QStringLiteral("R"))},
          "record: transport.record is not bound to R by default");

    // A fresh start: Record plays from the edit cursor and records; Stop
    // commits one undo step, and undo takes every note back out.
    {
        const uint64_t cursor = tpb * 8;
        view->setEditCursorTick(cursor);
        const int undoBefore = undo->index();
        const auto before = noteList(a);
        m_recordAction->trigger();
        check(recording() && m_recordAction->isChecked() &&
                  m_audio.transport() == Transport::Playing,
              "record: Record while stopped did not start recording playback");
        check(message().startsWith(QStringLiteral("Recording into ")),
              "record: the status bar does not say what is recording");
        pumpMs(50);
        synchronizePlayhead(); // the playhead timer's drain
        check(m_take.started() && m_take.punchInSample() == sampleOf(cursor),
              "record: a fresh take did not begin at the edit cursor");
        const uint64_t p1 = playhead();
        check(p1 == sampleOf(cursor) + uint64_t(rate * 0.05),
              "record: pumped time did not advance the playhead as expected");
        send(0x90, 60, 100);
        pumpMs(200);
        const uint64_t p2 = playhead();
        send(0x80, 60, 0);
        pumpMs(100);
        const uint64_t p3 = playhead();
        send(0x90, 62, 90);
        pumpMs(150);
        const uint64_t pStop = playhead();
        m_stopAction->trigger();
        check(!recording() && !m_recordAction->isChecked() &&
                  m_audio.transport() == Transport::Stopped,
              "record: Stop did not end the take");
        check(undo->index() == undoBefore + 1 &&
                  lastUndoText() == QStringLiteral("Record 2 note(s)"),
              "record: Stop did not commit exactly one undo step \"Record 2 note(s)\"");
        check(message() == QStringLiteral("Recorded 2 note(s)."),
              "record: the status bar does not report the recorded notes");
        const std::optional<DocNote> n60 = find(a, 60, tickOf(p1));
        const std::optional<DocNote> n62 = find(a, 62, tickOf(p3));
        check(n60 && n60->duration == tickOf(p2) - tickOf(p1) && n60->velocity == 100,
              "record: a played note did not land on the target track at its ticks");
        check(n62 && n62->tick + n62->duration == tickOf(pStop) && n62->velocity == 90,
              "record: a note held at Stop did not end at the stop position");
        // Played 50 ms after the start: wherever the new key-60 note
        // landed, it is within a period of that.
        const uint64_t intended = tickOf(sampleOf(cursor) + uint64_t(rate * 0.05));
        std::optional<uint64_t> landed;
        for (const DocNote &n : doc.notesForTrack(a)) {
            const std::array<uint64_t, 4> row{n.tick, n.key, n.duration, n.velocity};
            if (n.key == 60 && std::find(before.begin(), before.end(), row) == before.end())
                landed = n.tick;
        }
        check(landed && (*landed > intended ? *landed - intended : intended - *landed) <= slop,
              "record: a played note is more than one period off its time");
        undo->undo();
        check(noteList(a) == before, "record: undo did not remove every recorded note");
        send(0x80, 62, 0);
        pumpMs(30);
    }

    // Pitch bend and the mod wheel record onto the take's BEND and MOD
    // lanes (MOD as heard: the wheel scaled to the lane's range), in the
    // same undo step as the notes, replacing the lane's points from the
    // first move to the stop, where the song's value comes back, and keeping
    // the rest; undo restores them.
    {
        const auto lane = [&](uint8_t cc) {
            std::vector<std::pair<uint64_t, int>> out;
            for (const DocLanePoint &p : doc.lanePoints(a, cc))
                out.push_back({p.tick, p.value});
            return out;
        };
        // What the lane held at tick, before the take (with no point, what
        // the engine starts a track at).
        const auto valueAt = [](uint8_t cc, const std::vector<std::pair<uint64_t, int>> &points,
                                uint64_t tick) {
            int value = RecordingTake::restValue(cc);
            for (const auto &[t, v] : points) {
                if (t <= tick)
                    value = v;
            }
            return value;
        };
        // The take's moves over the song's other points: [first, end]
        // replaced, the song's value back at end when it sounds different.
        const auto expected = [&](uint8_t cc, const std::vector<std::pair<uint64_t, int>> &before,
                                  std::vector<std::pair<uint64_t, int>> moves, uint64_t end) {
            const uint64_t first = moves.front().first;
            const uint64_t last = std::max(end, moves.back().first);
            std::vector<std::pair<uint64_t, int>> want;
            for (const auto &pt : before) {
                if (pt.first < first || pt.first > last)
                    want.push_back(pt);
            }
            const int song = valueAt(cc, before, end);
            if (end > moves.back().first &&
                RecordingTake::audible(cc, song) != RecordingTake::audible(cc, moves.back().second))
                moves.push_back({end, song});
            want.insert(want.end(), moves.begin(), moves.end());
            std::sort(want.begin(), want.end());
            return want;
        };
        const auto sorted = [](std::vector<std::pair<uint64_t, int>> v) {
            std::sort(v.begin(), v.end());
            return v;
        };
        const uint64_t cursor = tpb * 16;
        view->setEditCursorTick(cursor);
        // One bend point inside the range the take will move through, one
        // well after it.
        const uint64_t inside = cursor + tpb / 2;
        doc.addLanePoint(a, DOC_CC_BEND, inside, 1000);
        doc.addLanePoint(a, DOC_CC_BEND, cursor + tpb * 8, 2000);
        const auto bendBefore = lane(DOC_CC_BEND);
        const auto modBefore = lane(0x01);
        const int modMax = view->laneDisplayMax(a, 0x01);
        const int undoBefore = undo->index();
        m_recordAction->trigger();
        pumpMs(20);
        const uint64_t pBend = playhead();
        send(0xE0, 0x00, 0x60); // +4096
        send(0x90, 64, 100);
        pumpMs(40);
        const uint64_t pMod = playhead();
        send(0xB0, 1, 127);
        pumpMs(400);
        const uint64_t pBack = playhead();
        send(0xE0, 0x00, 0x40); // centre
        send(0xB0, 1, 0);
        send(0x80, 64, 0);
        pumpMs(50);
        const uint64_t pStop = playhead();
        m_stopAction->trigger();
        // The setup point sits where the take replaces, and the stop before
        // the point after it.
        check(tickOf(pBend) < inside && inside < tickOf(pBack) && tickOf(pBack) < tickOf(pStop) &&
                  tickOf(pStop) < cursor + tpb * 8,
              "record: the bend take did not span the setup point (timing)");
        check(undo->index() == undoBefore + 1 &&
                  lastUndoText() == QStringLiteral("Record 1 note(s) with pitch bend and mod"),
              "record: a take with bend and mod did not commit one step naming them");
        check(message() == QStringLiteral("Recorded 1 note(s) with pitch bend and mod."),
              "record: the status bar does not report the recorded bend and mod");
        // Here the song's 1000 comes back at the stop.
        const auto wantBend = expected(DOC_CC_BEND, bendBefore,
                                       {{tickOf(pBend), 4096}, {tickOf(pBack), 0}}, tickOf(pStop));
        const auto gotBend = sorted(lane(DOC_CC_BEND));
        if (!check(gotBend == wantBend && std::count(gotBend.begin(), gotBend.end(),
                                                     std::make_pair(tickOf(pStop), 1000)) == 1,
                   "record: the bend lane is not the take's moves over the song's other points, "
                   "the song's value back at the stop")) {
            for (const auto &[tick, value] : gotBend)
                std::fprintf(stderr, "reccheck:   bend %llu %d\n", (unsigned long long)tick, value);
        }
        const auto wantMod =
            expected(0x01, modBefore, {{tickOf(pMod), modMax}, {tickOf(pBack), 0}}, tickOf(pStop));
        check(sorted(lane(0x01)) == wantMod,
              "record: the mod lane is not the take's moves, scaled as heard");
        check(find(a, 64, tickOf(pBend)).has_value(), "record: the note with the bend was lost");
        undo->undo();
        check(lane(DOC_CC_BEND) == bendBefore && lane(0x01) == modBefore,
              "record: undo did not restore the bend and mod lanes");
        undo->undo(); // the two setup points
        undo->undo();

        // Moves alone are a take too.
        const int undoAlone = undo->index();
        m_recordAction->trigger();
        pumpMs(20);
        send(0xB0, 1, 64);
        pumpMs(50);
        m_stopAction->trigger();
        check(undo->index() == undoAlone + 1 && lastUndoText() == QStringLiteral("Record mod"),
              "record: a take of only a mod move did not commit \"Record mod\"");
        check(message() == QStringLiteral("Recorded mod."),
              "record: the status bar does not report a mod-only take");
        undo->undo();
        send(0xB0, 1, 0);
        pumpMs(30);

        // Reset All Controllers (CC121) centres the bend and puts every
        // routed lane back at its track-init value (J5: with the default
        // routes MOD 0, VOL 127 and PAN 64). The take records a reset only
        // on a lane it already moved (bend and VOL here); MOD and PAN,
        // untouched, keep the song's automation (a PAN point here).
        const uint64_t resetCursor = tpb * 20;
        view->setEditCursorTick(resetCursor);
        const auto panBeforeReset = lane(0x0A);
        doc.addLanePoint(a, 0x0A, resetCursor, 20);
        const auto bendReset = lane(DOC_CC_BEND);
        const auto modReset = lane(0x01);
        const auto volReset = lane(0x07);
        const auto panReset = lane(0x0A);
        const auto bendrReset = lane(0x14);
        const int undoReset = undo->index();
        m_recordAction->trigger();
        pumpMs(20);
        const uint64_t pUp = playhead();
        send(0xE0, 0x00, 0x60); // +4096
        pumpMs(40);
        const uint64_t pVol = playhead();
        send(0xB0, 7, 37);
        pumpMs(60);
        const uint64_t pReset = playhead();
        send(0xB0, 121, 0);
        pumpMs(50);
        const uint64_t pEnd = playhead();
        m_stopAction->trigger();
        check(undo->index() == undoReset + 1 &&
                  lastUndoText() == QStringLiteral("Record pitch bend and volume"),
              "record: Reset All Controllers did not record exactly the lanes the take moved");
        check(sorted(lane(DOC_CC_BEND)) == expected(DOC_CC_BEND, bendReset,
                                                    {{tickOf(pUp), 4096}, {tickOf(pReset), 0}},
                                                    tickOf(pEnd)),
              "record: Reset All Controllers did not centre the recorded bend");
        check(sorted(lane(0x07)) == expected(0x07, volReset,
                                             {{tickOf(pVol), 37}, {tickOf(pReset), 127}},
                                             tickOf(pEnd)),
              "record: Reset All Controllers did not record the moved VOL's reset to 127");
        check(lane(0x01) == modReset && lane(0x0A) == panReset,
              "record: Reset All Controllers wrote MOD or PAN, which the take never moved");
        check(lane(0x14) == bendrReset,
              "record: Reset All Controllers recorded BENDR, which has no route");
        undo->undo();
        undo->undo(); // the PAN setup point
        check(lane(0x0A) == panBeforeReset, "record: the CC121 setup point did not undo");

        // Every routed lane records (docs/midi-controls/PLAN.md step 3):
        // VOL, PAN, BENDR and LFOS knobs land on their lanes as heard (BENDR
        // in semitones), in the note's undo step, the song's value back at
        // the stop; played back, each sounds as it did live.
        check(midi.setRoute(0x14, 20) && midi.setRoute(0x15, 21),
              "record: BENDR and LFOS could not be routed");
        const M4AEngine *engine = m_audio.engineForTest();
        enum { Vol, Pan, BendR, Lfos };
        const auto heard = [&] {
            const M4ATrack &tr = engine->tracks[a];
            return std::array<int, 4>{tr.rawVolume, tr.pan, tr.bendRange, tr.lfoSpeed};
        };
        const uint64_t ccCursor = tpb * 24;
        view->setEditCursorTick(ccCursor);
        std::array<std::vector<std::pair<uint64_t, int>>, 4> ccBefore;
        const std::array<uint8_t, 4> ccLanes{0x07, 0x0A, 0x14, 0x15};
        for (size_t i = 0; i < 4; i++)
            ccBefore[i] = lane(ccLanes[i]);
        const int undoCc = undo->index();
        m_recordAction->trigger();
        pumpMs(20);
        const uint64_t pNote = playhead();
        send(0x90, 67, 100);
        pumpMs(20);
        // Values no song is likely to hold, so the restores show.
        std::array<uint64_t, 4> pMove{};
        const std::array<uint8_t, 4> knobCc{7, 10, 20, 21};
        const std::array<uint8_t, 4> knob{37, 113, 106, 77};
        const std::array<int, 4> asHeard{37, 113, 20, 77}; // BENDR: 106 * 24 / 127, rounded
        std::array<int, 4> live{};
        for (size_t i = 0; i < 4; i++) {
            pMove[i] = playhead();
            send(0xB0, knobCc[i], knob[i]);
            pumpMs(40);
            // Latched (step 4): no song event in those 40 ms moved it.
            live[i] = heard()[i];
        }
        pumpMs(100);
        const uint64_t pMid = playhead();
        pumpMs(100);
        send(0x80, 67, 0);
        pumpMs(50);
        const uint64_t pCcStop = playhead();
        m_stopAction->trigger();
        check(live == std::array<int, 4>{37, 113 - 64, 20, 77},
              "record: the routed knobs did not sound at their scales live");
        check(undo->index() == undoCc + 1 &&
                  lastUndoText() ==
                      QStringLiteral("Record 1 note(s) with volume, pan, bend range and LFO speed"),
              "record: a take with every routed lane did not commit one step naming them");
        check(message() ==
                  QStringLiteral("Recorded 1 note(s) with volume, pan, bend range and LFO speed."),
              "record: the status bar does not name the routed lanes recorded");
        check(find(a, 67, tickOf(pNote)).has_value(), "record: the routed lanes' note was lost");
        for (size_t i = 0; i < 4; i++) {
            const auto want = expected(ccLanes[i], ccBefore[i], {{tickOf(pMove[i]), asHeard[i]}},
                                       tickOf(pCcStop));
            const auto got = sorted(lane(ccLanes[i]));
            if (!check(got == want, "record: a routed lane is not the take's move as heard over "
                                    "the song's other points, the song's value back at the stop")) {
                std::fprintf(stderr, "reccheck:   lane 0x%02X:", ccLanes[i]);
                for (const auto &[tick, value] : got)
                    std::fprintf(stderr, " %llu:%d", (unsigned long long)tick, value);
                std::fprintf(stderr, "\n");
            }
        }
        // The song as committed, played from inside the take: the engine
        // gets what it got from the knobs.
        m_audio.seek(sampleOf(tickOf(pMid)));
        pumpMs(1);
        check(heard() == live, "record: a recorded lane does not play back as it sounded live");
        // And past the stop, the song's own values again.
        m_audio.seek(sampleOf(tickOf(pCcStop) + 1));
        pumpMs(1);
        std::array<int, 4> songAfter{};
        for (size_t i = 0; i < 4; i++)
            songAfter[i] = valueAt(ccLanes[i], ccBefore[i], tickOf(pCcStop) + 1);
        songAfter[Pan] -= 64;
        check(heard() == songAfter,
              "record: past the take the lanes do not play the song's values");
        undo->undo();
        bool restored = true;
        for (size_t i = 0; i < 4; i++)
            restored &= lane(ccLanes[i]) == ccBefore[i];
        check(restored, "record: undo did not restore the routed lanes");

        // Loop passes: a VOL move before the wrap is restored at the loop
        // end; a PAN move on the pass after it starts its own write.
        if (session->timeline->hasLoop() &&
            session->timeline->loopEndTick > session->timeline->loopStartTick + 4 * tpb) {
            const uint64_t loopEnd = session->timeline->loopEndTick;
            m_loopAction->setChecked(true);
            view->setEditCursorTick(loopEnd - tpb);
            const auto volLoop = lane(0x07);
            const auto panLoop = lane(0x0A);
            const int undoLoop = undo->index();
            m_recordAction->trigger();
            pumpMs(20);
            const uint64_t pVol = playhead();
            send(0xB0, 7, 41);
            for (int i = 0; i < 1000 && playhead() >= pVol; i++)
                pumpMs(10);
            check(playhead() < pVol, "record: the routed lanes' loop never wrapped");
            pumpMs(20);
            const uint64_t pPan = playhead();
            send(0xB0, 10, 5);
            pumpMs(60);
            const uint64_t pLoopStop = playhead();
            m_stopAction->trigger();
            check(undo->index() == undoLoop + 1 &&
                      lastUndoText() == QStringLiteral("Record volume and pan"),
                  "record: a looped take of VOL and PAN did not commit \"Record volume and pan\"");
            check(sorted(lane(0x07)) == expected(0x07, volLoop, {{tickOf(pVol), 41}}, loopEnd),
                  "record: a VOL move before the wrap did not end (restored) at the loop end");
            check(sorted(lane(0x0A)) ==
                      expected(0x0A, panLoop, {{tickOf(pPan), 5}}, tickOf(pLoopStop)),
                  "record: a PAN move after the wrap is not its own write to the stop");
            undo->undo();
            check(lane(0x07) == volLoop && lane(0x0A) == panLoop,
                  "record: undo did not restore the looped take's lanes");
        }
        // The knobs back where the song is, and the routes at the defaults.
        m_audio.seek(0);
        pumpMs(10);
        check(midi.setRoute(0x14, MidiRoutes::kNone) && midi.setRoute(0x15, MidiRoutes::kNone),
              "record: BENDR and LFOS could not be unrouted");
        check(midi.routes() == MidiRoutes(), "record: the routes are not back at the defaults");
    }

    checkRecordingLatch(a, b);
    checkRecordingVoice(a, b);

    // Record's key is the roll's, like M/S: R in the focused roll starts and
    // ends a take. The action carries no window shortcut, which would fire
    // in place of the song list's, the voicegroup tree's, the event list's
    // or a combo box's type-ahead (none of them claims a plain letter).
    {
        check(m_recordAction->shortcuts().isEmpty(),
              "record: Record carries a window shortcut (it would take R from item views)");
        auto *roll = view->findChild<QWidget *>(QStringLiteral("pianoRoll"));
        const auto pressR = [&] {
            QKeyEvent press(QEvent::KeyPress, Qt::Key_R, Qt::NoModifier, QStringLiteral("r"));
            QApplication::sendEvent(roll, &press);
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_R, Qt::NoModifier, QStringLiteral("r"));
            QApplication::sendEvent(roll, &release);
        };
        if (check(roll != nullptr, "record: the song view has no piano roll")) {
            view->setEditCursorTick(tpb * 4);
            pressR();
            check(recording(), "record: R in the focused roll did not start a take");
            pumpMs(30);
            pressR();
            check(!recording(), "record: R in the focused roll did not end the take");
            m_stopAction->trigger();
            pumpMs(30);
        }
    }

    // Pause commits the take like Stop, and stays paused.
    {
        view->setEditCursorTick(tpb * 4);
        const int undoBefore = undo->index();
        m_recordAction->trigger();
        pumpMs(30);
        const uint64_t p1 = playhead();
        send(0x90, 60, 100);
        pumpMs(100);
        m_pauseAction->trigger();
        check(!recording() && m_audio.transport() == Transport::Paused &&
                  undo->index() == undoBefore + 1 && find(a, 60, tickOf(p1)).has_value(),
              "record: Pause did not commit the take (and stay paused)");
        send(0x80, 60, 0);
        pumpMs(30);
        m_stopAction->trigger();
        undo->undo();
    }

    // A take ended while the document has an edit group open (a script
    // transaction that stops the transport) is not written into that group:
    // it lands once the group has closed, as its own step, even when the
    // group is rolled back.
    {
        view->setEditCursorTick(tpb * 4);
        m_recordAction->trigger();
        pumpMs(30);
        const uint64_t p1 = playhead();
        send(0x90, 60, 100);
        pumpMs(100);
        doc.beginEditGroup(QStringLiteral("Script"));
        doc.addNote(b, tpb * 64, 30, 6, 100);
        m_stopAction->trigger();
        check(!recording() && !find(a, 60, tickOf(p1)).has_value(),
              "record: a take ended inside an open edit group was written into it");
        check(view->recordingPreview().notes.size() == 1 &&
                  view->recordingPreview().notes.front().key == 60 &&
                  view->recordingPreview().spans.empty(),
              "record: a deferred take's ghost notes did not stay (without the REC tint) until "
              "it lands");
        doc.endEditGroup(/*discard=*/true);
        const int undoAfterGroup = undo->index();
        QElapsedTimer waited;
        waited.start();
        while (undo->index() == undoAfterGroup && waited.elapsed() < 2000)
            QApplication::processEvents(QEventLoop::AllEvents, 10);
        check(undo->index() == undoAfterGroup + 1 &&
                  lastUndoText() == QStringLiteral("Record 1 note(s)") &&
                  find(a, 60, tickOf(p1)).has_value() &&
                  message() == QStringLiteral("Recorded 1 note(s)."),
              "record: a take ended inside an edit group did not land as its own step after it");
        check(view->recordingPreview().empty(),
              "record: a deferred take's ghost notes stayed after it landed");
        send(0x80, 60, 0);
        pumpMs(30);
        undo->undo();
    }

    // Two takes ended inside one open edit group both wait for it: the
    // first landing leaves the second's ghosts on view, and they clear when
    // the second lands.
    {
        view->setEditCursorTick(tpb * 4);
        const int undoBefore = undo->index();
        m_recordAction->trigger();
        pumpMs(30);
        send(0x90, 60, 100);
        pumpMs(60);
        send(0x80, 60, 0);
        pumpMs(20);
        doc.beginEditGroup(QStringLiteral("Script"));
        m_stopAction->trigger();
        view->setEditCursorTick(tpb * 12);
        m_recordAction->trigger();
        pumpMs(30);
        send(0x90, 62, 100);
        pumpMs(60);
        send(0x80, 62, 0);
        pumpMs(20);
        synchronizePlayhead();
        m_stopAction->trigger();
        const RecordingPreview second = view->recordingPreview();
        check(!recording() && second.notes.size() == 1 && second.notes.front().key == 62,
              "record: the second deferred take's ghosts are not on view");
        doc.endEditGroup(/*discard=*/true);
        const int undoAfterGroup = undo->index();
        std::vector<RecordingPreview> atLanding;
        const QMetaObject::Connection watch =
            QObject::connect(undo, &QUndoStack::indexChanged, undo,
                             [&] { atLanding.push_back(view->recordingPreview()); });
        QElapsedTimer waited;
        waited.start();
        while (undo->index() < undoAfterGroup + 2 && waited.elapsed() < 2000)
            QApplication::processEvents(QEventLoop::AllEvents, 10);
        QObject::disconnect(watch);
        check(undo->index() == undoAfterGroup + 2 && !atLanding.empty() &&
                  atLanding.front().take == second.take && atLanding.front().notes == second.notes,
              "record: the first deferred take's landing cleared the second take's ghosts");
        check(view->recordingPreview().empty(),
              "record: deferred ghosts stayed after both takes landed");
        while (undo->index() > undoBefore)
            undo->undo();
    }

    // A selection on a track the song doesn't have can't record (the notes
    // would have nowhere to go); a take that can't be written says so
    // rather than "Recorded".
    {
        int missing = -1;
        for (int t = 0; t < MAX_TRACKS && missing < 0; t++) {
            if (doc.smfTrackFor(t) < 0)
                missing = t;
        }
        if (missing >= 0) {
            view->selectTrack(missing);
            check(!m_recordAction->isEnabled() &&
                      m_recordAction->toolTip().contains(QStringLiteral("song's tracks")),
                  "record: Record is available on a track the song doesn't have");
            beginRecording();
            check(!recording(), "record: Record began a take on a track the song doesn't have");
            view->selectTrack(a);
            check(m_recordAction->isEnabled(),
                  "record: Record did not come back with a song track selected");
        } else {
            std::printf("reccheck: missing-track SKIP (the song uses all 16 tracks)\n");
        }
        view->setEditCursorTick(tpb * 4);
        const int undoBefore = undo->index();
        m_recordAction->trigger();
        pumpMs(30);
        send(0x90, 60, 100);
        pumpMs(100);
        doc.setLocked(true);
        m_stopAction->trigger();
        doc.setLocked(false);
        check(undo->index() == undoBefore &&
                  message() ==
                      QStringLiteral("The 1 recorded note(s) could not be added to the song."),
              "record: a take that could not be written was reported as recorded");
        send(0x80, 60, 0);
        pumpMs(30);
    }

    // An empty take pushes no undo entry; punch in and out while playing
    // keeps playing.
    {
        view->setEditCursorTick(tpb * 4);
        startPlayback();
        pumpMs(50);
        const int undoBefore = undo->index();
        m_recordAction->trigger();
        check(recording() && m_audio.transport() == Transport::Playing,
              "record: Record while playing did not punch in");
        pumpMs(50);
        // Even an empty take hands the keyboard back to the selection.
        view->selectTrack(b);
        pumpMs(50);
        m_recordAction->trigger();
        check(!recording() && m_audio.transport() == Transport::Playing,
              "record: Record again did not punch out and keep playing");
        check(undo->index() == undoBefore, "record: an empty take pushed an undo entry");
        check(m_audio.liveTarget() == b, "record: an empty take kept the target locked");
        view->selectTrack(a);
        check(message() == QStringLiteral("Nothing was recorded."),
              "record: the status bar did not say nothing was recorded");

        // Punch in with a key already held: its note-off matches nothing.
        // The offset can't move a note before the punch-in.
        midi.setLatencyMs(200);
        send(0x90, 65, 100);
        pumpMs(30);
        const auto before = noteList(a);
        m_recordAction->trigger();
        pumpMs(30);
        synchronizePlayhead();
        const uint64_t punchIn = m_take.punchInSample();
        const uint64_t p = playhead();
        check(m_take.started() && punchIn < p && p - punchIn < uint64_t(rate * 0.2),
              "record: the punch-in was not where Record was pressed");
        send(0x90, 67, 100);
        pumpMs(100);
        send(0x80, 65, 0);
        send(0x80, 67, 0);
        pumpMs(50);
        m_recordAction->trigger();
        check(!recording() && m_audio.transport() == Transport::Playing,
              "record: a punch-out with notes did not keep playing");
        check(undo->index() == undoBefore + 1 &&
                  lastUndoText() == QStringLiteral("Record 1 note(s)"),
              "record: a key held at punch-in was recorded (or the take was not committed)");
        check(find(a, 67, tickOf(punchIn)).has_value(),
              "record: the recording offset moved a note before the punch-in");
        undo->undo();
        check(noteList(a) == before, "record: undo did not restore the punched range");
        midi.setLatencyMs(0);
        m_stopAction->trigger();
        pumpMs(30);
    }

    // The target is locked at punch-in; the sustain pedal extends a note;
    // Fixed velocity is recorded; with Play incoming notes off it records
    // all the same.
    {
        view->setEditCursorTick(tpb * 4);
        midi.setLivePlay(false);
        midi.setFixedVelocity(40);
        const auto beforeA = noteList(a);
        const auto beforeB = noteList(b);
        m_recordAction->trigger();
        pumpMs(30);
        view->selectTrack(b);
        check(m_audio.liveTarget() == a, "record: a selection change mid-take moved the target");
        const uint64_t p1 = playhead();
        send(0x90, 60, 127);
        pumpMs(100);
        send(0xB0, 64, 127);
        send(0x80, 60, 0);
        pumpMs(200);
        const uint64_t pUp = playhead();
        send(0xB0, 64, 0);
        pumpMs(50);
        m_stopAction->trigger();
        const std::optional<DocNote> n = find(a, 60, tickOf(p1));
        check(n.has_value(), "record: with live play off (or the target changed) nothing landed on "
                             "the take's track");
        check(n && n->tick + n->duration == tickOf(pUp),
              "record: the sustain pedal did not extend the recorded note to pedal-up");
        check(n && n->velocity == 40, "record: Fixed velocity was not recorded");
        check(noteList(b) == beforeB, "record: the take landed on the newly selected track");
        // The lock ends with the take: the target is the selection again.
        check(m_audio.liveTarget() == b, "record: after the take the target did not follow again");
        undo->undo();
        check(noteList(a) == beforeA, "record: undo after the locked take did not restore");
        midi.setLivePlay(true);
        midi.setFixedVelocity(0);
        view->selectTrack(a);
    }

    // A track move mid-take carries the take (and the sound) along to the
    // track's new slot; deleting the take's track ends the recording and
    // drops the take, playback going on.
    {
        view->setEditCursorTick(tpb * 4);
        const int undoBefore = undo->index();
        m_recordAction->trigger();
        pumpMs(30);
        const uint64_t p1 = playhead();
        send(0x90, 60, 100);
        pumpMs(50);
        synchronizePlayhead(); // the ghost shows on the old slot
        check(doc.moveTrack(a, b), "record: the track move mid-take failed");
        const int moved = m_take.track();
        check(recording() && moved != a, "record: the take did not follow its track's move");
        check(m_audio.liveTarget() == moved, "record: the sound did not follow the take's track");
        check(saysRecordingInto(moved),
              "record: the status bar still names the take's old track after the move");
        synchronizePlayhead();
        check(view->recordingPreview().track == moved && view->recordingPreview().notes.size() == 1,
              "record: the ghost notes did not follow the take's track");
        send(0x80, 60, 0);
        pumpMs(30);
        m_stopAction->trigger();
        check(find(moved, 60, tickOf(p1)).has_value() && !find(a, 60, tickOf(p1)).has_value(),
              "record: after a track move the take landed on the wrong track");
        while (undo->index() > undoBefore)
            undo->undo();

        const auto beforeA = noteList(a);
        m_recordAction->trigger();
        pumpMs(30);
        send(0x90, 62, 100);
        pumpMs(50);
        synchronizePlayhead();
        check(view->recordingPreview().notes.size() == 1,
              "record: a held note shows no ghost before its track is deleted");
        const int undoAtDelete = undo->index();
        doc.deleteTrack(a);
        check(view->recordingPreview().empty(), "record: a dropped take left its ghost notes");
        check(!recording() && !m_recordAction->isChecked() &&
                  m_audio.transport() == Transport::Playing,
              "record: deleting the take's track did not end the recording (playback going on)");
        check(message() == QStringLiteral("Recording stopped: its track was deleted."),
              "record: deleting the take's track did not say why the recording stopped");
        send(0x80, 62, 0);
        pumpMs(30);
        m_stopAction->trigger();
        check(undo->index() == undoAtDelete + 1, "record: the dropped take pushed an undo entry");
        undo->undo();
        check(noteList(a) == beforeA, "record: the dropped take landed somewhere");
        while (undo->index() > undoBefore)
            undo->undo();
    }

    // The keyboard going away mid-take keeps the button (the take can still
    // be stopped from it); once the take ends it goes.
    {
        view->setEditCursorTick(tpb * 4);
        m_recordAction->trigger();
        pumpMs(30);
        midi.setOpenPortsForTest({});
        check(recording() && m_recordAction->isVisible() && m_recordAction->isEnabled(),
              "record: the keyboard going away mid-take took the Record button with it");
        m_recordAction->trigger();
        check(!recording() && !m_recordAction->isVisible(),
              "record: with no keyboard left, the Record button stayed after the take");
        midi.setOpenPortsForTest({QStringLiteral("Test Keyboard")});
        m_stopAction->trigger();
    }

    // A release the app imposes (Tools → MIDI: All Notes Off) ends a held
    // note where it happened: unlike a played note-off, the offset doesn't
    // move it.
    {
        midi.setLatencyMs(100);
        view->setEditCursorTick(tpb * 4);
        const int undoBefore = undo->index();
        m_recordAction->trigger();
        pumpMs(300);
        const uint64_t pOn = playhead();
        send(0x90, 60, 100);
        pumpMs(200);
        const uint64_t pPanic = playhead();
        m_midiPanicAction->trigger();
        pumpMs(50);
        send(0x80, 60, 0);
        pumpMs(30);
        m_stopAction->trigger();
        const std::optional<DocNote> n = find(a, 60, tickOf(pOn - recordingLatencySamples()));
        check(undo->index() == undoBefore + 1 && n && n->tick + n->duration == tickOf(pPanic),
              "record: MIDI: All Notes Off ended a held note shifted, like a played note-off");
        undo->undo();
        midi.setLatencyMs(0);
    }

    // Auto offset: the device's output latency (period × periods). A fixed
    // offset is that many ms.
    {
        midi.setLatencyMs(100);
        check(recordingLatencySamples() == uint64_t(std::llround(rate * 0.1)),
              "record: a 100 ms recording offset is not 100 ms of samples");
        midi.setLatencyMs(MidiInput::kLatencyAuto);
        const uint64_t latency = recordingLatencySamples();
        check(latency == uint64_t(m_audio.periodSizeFrames()) * uint64_t(m_audio.periodCount()) &&
                  latency > 0,
              "record: the Auto offset is not the device's output latency");
        view->setEditCursorTick(tpb * 4);
        m_recordAction->trigger();
        pumpMs(500);
        const uint64_t p = playhead();
        send(0x90, 60, 100);
        pumpMs(100);
        send(0x80, 60, 0);
        pumpMs(30);
        m_stopAction->trigger();
        check(find(a, 60, tickOf(p - latency)).has_value(),
              "record: the Auto offset did not move the note earlier by the device latency");
        undo->undo();
        midi.setLatencyMs(0);
    }

    // A message that waited for its callback is placed where it arrived,
    // not where the callback found it; a wait longer than a few periods (a
    // stalled callback) moves it back no further than that.
    {
        const uint64_t period = uint64_t(std::max(m_audio.periodSizeFrames(), 1));
        const uint64_t waited = std::min<uint64_t>(uint64_t(rate * 0.1), 4 * period);
        const auto sendAged = [&](uint8_t status, uint8_t d1, uint8_t d2, double ageSeconds) {
            MidiMsg msg{status, d1, d2};
            msg.arrivalNs = midiClockNs() - int64_t(ageSeconds * 1e9);
            midi.injectForTest(msg);
        };
        view->setEditCursorTick(tpb * 4);
        m_recordAction->trigger();
        pumpMs(500);
        const uint64_t p1 = playhead();
        sendAged(0x90, 60, 100, double(waited) / rate);
        sendAged(0xB0, 7, 33, double(waited) / rate); // a lane move waits back the same
        pumpMs(200);
        const uint64_t p2 = playhead();
        sendAged(0x90, 62, 100, 10.0);
        pumpMs(100);
        send(0x80, 60, 0);
        send(0x80, 62, 0);
        pumpMs(30);
        m_stopAction->trigger();
        // Up to a period of real time passes between the stamp and the pump.
        const auto landedIn = [&](int key, uint64_t latest, uint64_t earliest) {
            for (const DocNote &n : doc.notesForTrack(a)) {
                if (n.key == key && n.tick >= tickOf(earliest) && n.tick <= tickOf(latest))
                    return true;
            }
            return false;
        };
        check(tickOf(p1 - waited) < tickOf(p1) && landedIn(60, p1 - waited, p1 - waited - period),
              "record: a message's wait for its callback did not move it back");
        const std::vector<DocLanePoint> vol = doc.lanePoints(a, 0x07);
        check(std::any_of(vol.begin(), vol.end(),
                          [&](const DocLanePoint &pt) {
                              return pt.value == 33 && pt.tick >= tickOf(p1 - waited - period) &&
                                     pt.tick <= tickOf(p1 - waited);
                          }),
              "record: a lane move's wait for its callback did not move it back");
        // Four periods, or two callbacks (pumpForTest's are 512 frames) when
        // that is more.
        const uint64_t cap = std::max<uint64_t>(4 * period, 2 * 512);
        check(landedIn(62, p2 - cap, p2 - cap - period),
              "record: a stalled callback's wait was not capped at four periods");
        undo->undo();
    }

    // A loop wrap, a seek and a 2-pass overdub in one take: a note held
    // across the wrap ends at the loop end, a note held across the seek ends
    // where it happened, and the second pass's note at the first pass's
    // (tick, key) wins.
    if (session->timeline->hasLoop() &&
        session->timeline->loopEndTick > session->timeline->loopStartTick + 4 * tpb) {
        const uint64_t loopStart = session->timeline->loopStartTick;
        const uint64_t loopEnd = session->timeline->loopEndTick;
        const uint64_t cursor = loopEnd - 2 * tpb;
        m_loopAction->setChecked(true);
        view->setEditCursorTick(cursor);
        const int undoBefore = undo->index();
        const auto before = noteList(a);
        m_recordAction->trigger();
        pumpMs(20);
        const uint64_t p1 = playhead();
        send(0x90, 74, 60);
        pumpMs(60);
        send(0x80, 74, 0);
        pumpMs(40);
        const uint64_t pA = playhead();
        send(0x90, 70, 100);
        // Pump to the wrap (two beats at any tempo the song has is well
        // within ten seconds).
        for (int i = 0; i < 1000 && playhead() >= pA; i++)
            pumpMs(10);
        check(playhead() < pA, "record: the loop never wrapped");
        pumpMs(50);
        send(0x80, 70, 0);
        const uint64_t pB = playhead();
        send(0x90, 72, 100);
        pumpMs(50);
        send(0x80, 72, 0);
        pumpMs(20);
        const uint64_t pS = playhead();
        send(0x90, 76, 100);
        pumpMs(40);
        const uint64_t pSeek = playhead();
        // A seek while playing (what moving the edit cursor does; it is
        // already at the cursor, so seek the engine itself).
        m_audio.seek(sampleOf(cursor));
        pumpMs(20);
        check(playhead() == p1, "record: the seek did not land where the first pass started");
        send(0x80, 76, 0);
        send(0x90, 74, 110);
        pumpMs(60);
        send(0x80, 74, 0);
        pumpMs(20);
        synchronizePlayhead();
        check(m_take.segment() == 2,
              "record: the take did not see the wrap and the seek as two new segments");
        m_stopAction->trigger();
        check(undo->index() == undoBefore + 1 &&
                  lastUndoText() == QStringLiteral("Record 4 note(s)"),
              "record: the looped take did not commit 4 notes in one step");
        const std::optional<DocNote> held = find(a, 70, tickOf(pA));
        check(held && held->tick + held->duration == loopEnd,
              "record: a note held across the loop wrap did not end at the loop end");
        const std::optional<DocNote> pass2 = find(a, 72, tickOf(pB));
        check(pass2 && pass2->tick >= loopStart && pass2->duration > 0,
              "record: the pass after the wrap was not recorded at its own ticks");
        const std::optional<DocNote> seeked = find(a, 76, tickOf(pS));
        check(seeked && seeked->tick + seeked->duration == tickOf(pSeek),
              "record: a note held across a seek did not end where the seek happened");
        int at74 = 0;
        for (const DocNote &n : doc.notesForTrack(a))
            at74 += n.key == 74 && n.tick == tickOf(p1);
        const std::optional<DocNote> merged = find(a, 74, tickOf(p1));
        check(at74 == 1 && merged && merged->velocity == 110,
              "record: the second pass did not replace the first pass's note");
        undo->undo();
        check(noteList(a) == before, "record: undo after the looped take did not restore");

        // Started with the edit cursor at the loop end, the player wraps
        // before it plays a sample: the take begins at the loop start, and
        // a note played at once lands there, not a tick past the loop end.
        const uint64_t loopStartSample = session->timeline->loopStartSample;
        view->setEditCursorTick(loopEnd);
        const int undoAtEnd = undo->index();
        m_recordAction->trigger();
        send(0x90, 77, 100);
        pumpMs(100);
        const uint64_t pOff = playhead();
        send(0x80, 77, 0);
        pumpMs(20);
        synchronizePlayhead();
        check(m_take.started() && m_take.punchInSample() == loopStartSample,
              "record: a take started at the loop end did not begin at the loop start");
        m_stopAction->trigger();
        const std::optional<DocNote> first = find(a, 77, tickOf(loopStartSample));
        check(undo->index() == undoAtEnd + 1 && first &&
                  first->tick + first->duration == tickOf(pOff),
              "record: a note played as a take began at the loop end did not land at the loop "
              "start");
        undo->undo();

        // The preview read across a wrap the drain hasn't seen (a callback
        // crossed it between the playhead timer's drain and the clock read):
        // the held note's ghost and the first pass's span still end at the
        // loop end, not snapped back to where the take last saw the player.
        // Past a lost Boundary (a full ring), the frame stays as it was until
        // the take catches up.
        view->setEditCursorTick(cursor);
        const int undoAtWrap = undo->index();
        m_recordAction->trigger();
        pumpMs(20);
        send(0x90, 70, 100);
        pumpMs(20);
        synchronizePlayhead();
        const uint64_t pHeld = playhead();
        for (int i = 0; i < 1000 && playhead() >= pHeld; i++)
            pumpMs(10); // no drain: the wrap's Boundary waits in the ring
        check(playhead() < pHeld, "record: the loop never wrapped (preview)");
        syncRecordingPreview();
        {
            const RecordingPreview &preview = view->recordingPreview();
            const auto held = std::find_if(preview.notes.begin(), preview.notes.end(),
                                           [](const NewNote &n) { return n.key == 70; });
            check(held != preview.notes.end() && held->tick + held->duration == loopEnd &&
                      std::any_of(preview.spans.begin(), preview.spans.end(),
                                  [&](const auto &span) { return span.second == loopEnd; }),
                  "record: a preview read across an undrained wrap snapped the held note or the "
                  "pass's span back");
        }
        send(0x80, 70, 0);
        pumpMs(20);
        synchronizePlayhead();
        for (int i = 0; i < 24; i++) {
            for (int j = 0; j < int(MidiInputRing::kCapacity) / 2; j++) {
                send(0x90, 60, 100);
                send(0x80, 60, 0);
            }
            m_audio.pumpForTest(512); // one callback, no drain in between
        }
        m_audio.seek(sampleOf(cursor));
        m_audio.pumpForTest(512); // the seek's Boundary is lost to the full ring
        {
            const RecordingPreview before = view->recordingPreview();
            syncRecordingPreview();
            const RecordingPreview &after = view->recordingPreview();
            check(after.notes == before.notes && after.spans == before.spans,
                  "record: a preview read past a lost Boundary did not keep the last frame");
        }
        send(0x90, 72, 100);
        pumpMs(20);
        synchronizePlayhead();
        check(std::any_of(view->recordingPreview().notes.begin(),
                          view->recordingPreview().notes.end(),
                          [](const NewNote &n) { return n.key == 72; }),
              "record: the preview did not resume once the take caught up");
        send(0x80, 72, 0);
        pumpMs(20);
        m_stopAction->trigger();
        while (undo->index() > undoAtWrap)
            undo->undo();
    } else {
        std::printf("reccheck: loop wrap SKIP (the song has no loop of 4+ beats)\n");
    }

    // A document edit mid-take cuts the live notes: the held note ends
    // there, and the edit and the take are separate undo steps.
    {
        view->setEditCursorTick(tpb * 4);
        const int undoBefore = undo->index();
        m_recordAction->trigger();
        pumpMs(30);
        const uint64_t p1 = playhead();
        send(0x90, 64, 100);
        pumpMs(100);
        const uint64_t pEdit = playhead();
        doc.addNote(b, tpb * 64, 30, 6, 100);
        pumpMs(100);
        send(0x80, 64, 0);
        pumpMs(30);
        m_stopAction->trigger();
        const std::optional<DocNote> n = find(a, 64, tickOf(p1));
        check(undo->index() == undoBefore + 2 && n && n->tick + n->duration == tickOf(pEdit),
              "record: a document edit mid-take did not end the held note at the edit");
        undo->undo();
        undo->undo();
        check(undo->index() == undoBefore, "record: the mid-take edit's undo steps are off");
    }

    // The capture ring overflowing: the loss is counted, the take carries on
    // recording, and the status bar says input was lost.
    {
        view->setEditCursorTick(tpb * 4);
        const int undoBefore = undo->index();
        const uint32_t droppedBefore = m_audio.captureDropped();
        m_recordAction->trigger();
        pumpMs(20);
        for (int i = 0; i < 24; i++) {
            for (int j = 0; j < int(MidiInputRing::kCapacity) / 2; j++) {
                send(0x90, 60, 100);
                send(0x80, 60, 0);
            }
            m_audio.pumpForTest(512); // one callback, no drain in between
        }
        check(m_audio.captureDropped() > droppedBefore,
              "record: a full capture ring did not count its drops");
        synchronizePlayhead(); // the drain that makes room again
        pumpMs(30);
        const uint64_t pAfter = playhead();
        send(0x90, 72, 100);
        pumpMs(100);
        const uint64_t pAfterOff = playhead();
        send(0x80, 72, 0);
        pumpMs(30);
        m_stopAction->trigger();
        const std::optional<DocNote> after = find(a, 72, tickOf(pAfter));
        check(undo->index() == undoBefore + 1 && after &&
                  after->tick + after->duration == tickOf(pAfterOff),
              "record: after capture drops the take did not go on recording");
        check(message().contains(QStringLiteral("MIDI input was lost")),
              "record: a take with capture drops did not say input was lost");
        undo->undo();
    }

    // The song ending by itself (loop off) ends the take, on the playhead
    // tick.
    {
        m_loopAction->setChecked(false);
        const uint64_t end = session->timeline->lengthTicks;
        view->setEditCursorTick(end > tpb ? end - tpb : 0);
        const int undoBefore = undo->index();
        m_recordAction->trigger();
        pumpMs(20);
        const uint64_t pOn = playhead();
        send(0x90, 60, 100);
        uint64_t pLast = pOn;
        for (int i = 0; i < 1000 && m_audio.transport() == Transport::Playing; i++) {
            pLast = playhead();
            pumpMs(20);
        }
        check(m_audio.transport() == Transport::Stopped, "record: the song did not end by itself");
        synchronizePlayhead();
        check(!recording() && undo->index() == undoBefore + 1,
              "record: the song ending by itself did not commit the take");
        // Held to the end: it ends where playback stopped, in the last pump.
        const std::optional<DocNote> held = find(a, 60, tickOf(pOn));
        check(held && held->tick + held->duration >= tickOf(pLast) &&
                  held->tick + held->duration <= tickOf(pLast + uint64_t(rate * 0.02)),
              "record: a note held as the song ended did not end where playback stopped");
        send(0x80, 60, 0);
        pumpMs(20);
        undo->undo();
        m_loopAction->setChecked(true);
    }

    // The take's tab going away, on a second song's tab: closing another
    // (clean) tab mid-take keeps recording; closing the take's own tab
    // commits before the prompt (answered Cancel, the tab stays, holding
    // the take); destroying it, or reloading it in place, commits first too.
    {
        const auto usedTrack = [](SongSession *s) {
            for (int t = 0; t < MAX_TRACKS; t++) {
                if (s->timeline->tracks[t].used && s->doc.smfTrackFor(t) >= 0)
                    return t;
            }
            return -1;
        };
        // Another playable song with a track to record into (the first found,
        // then that one again).
        const auto openOther = [&](QString *label) -> SongSession * {
            int tries = 0;
            for (const SongInfo &song : m_project.songs()) {
                if (song.label == doc.label() || !song.isPlayable() ||
                    !song.label.startsWith(QStringLiteral("mus_")) ||
                    (!label->isEmpty() && song.label != *label))
                    continue;
                if (++tries > 10)
                    break;
                loadSongByLabel(song.label, /*newTab=*/true);
                if (!m_active || m_active == session || m_active->doc.label() != song.label)
                    return nullptr;
                if (usedTrack(m_active) >= 0) {
                    *label = song.label;
                    return m_active;
                }
                closeTab(m_tabs->indexOf(m_active->view));
            }
            return nullptr;
        };
        const auto recordOneNote = [&](SongSession *s) {
            s->view->selectTrack(usedTrack(s));
            s->view->setEditCursorTick(0);
            m_recordAction->trigger();
            pumpMs(30);
            send(0x90, 60, 100);
            pumpMs(100);
        };
        QString other;
        SongSession *tab = openOther(&other);
        if (!tab) {
            check(false, "record: a second song did not open in its own tab");
        } else {
            m_tabs->setCurrentWidget(view);
            check(m_active == session, "record: the take's tab did not come back");
            m_recordAction->trigger();
            pumpMs(30);
            closeTab(m_tabs->indexOf(tab->view));
            check(recording() && m_active == session,
                  "record: closing another, clean tab punched the take out");
            m_stopAction->trigger();
            pumpMs(30);

            tab = openOther(&other);
            if (check(tab != nullptr, "record: the second song did not open again")) {
                QUndoStack *tabUndo = tab->doc.undoStack();
                const int tabBefore = tabUndo->index();
                recordOneNote(tab);
                {
                    PromptAnswer cancel(QMessageBox::Cancel);
                    closeTab(m_tabs->indexOf(tab->view));
                    check(cancel.answered() == 1,
                          "record: closing the take's tab did not ask about the take");
                }
                check(!recording() && m_tabs->indexOf(tab->view) >= 0 &&
                          tabUndo->index() == tabBefore + 1 &&
                          tabUndo->text(tabUndo->index() - 1) == QStringLiteral("Record 1 note(s)"),
                      "record: closing the take's own tab did not commit the take first");
                send(0x80, 60, 0);
                pumpMs(30);
                tabUndo->undo();

                recordOneNote(tab);
                destroySession(tab);
                check(!recording() && message() == QStringLiteral("Recorded 1 note(s)."),
                      "record: destroying the take's tab did not commit the take first");
                send(0x80, 60, 0);
                pumpMs(30);
            }

            tab = openOther(&other);
            if (check(tab != nullptr, "record: the second song did not open a third time")) {
                recordOneNote(tab);
                SongInfo info;
                for (const SongInfo &song : m_project.songs()) {
                    if (song.label == other)
                        info = song;
                }
                QString error;
                tab = populateSession(
                    tab, info, nullptr,
                    [&info](SongDocument &d, QString *err) { return d.load(info, err); }, &error);
                // Into the document being replaced: the reloaded one starts
                // with no history.
                check(tab && !recording() && message() == QStringLiteral("Recorded 1 note(s).") &&
                          tab->doc.undoStack()->count() == 0,
                      "record: reloading the take's song in place did not commit the take first");
                send(0x80, 60, 0);
                pumpMs(30);
                if (tab) {
                    PromptAnswer discard(QMessageBox::Discard);
                    closeTab(m_tabs->indexOf(tab->view));
                }
            }
        }
        m_tabs->setCurrentWidget(view);
        check(m_active == session, "record: the song's tab did not come back after the tab checks");
        m_stopAction->trigger();
    }

    // A tab switch mid-take commits to the take's own tab; on a read-only
    // bundle tab Record is unavailable.
    {
        const QString bundle = QDir(scratchDir).filePath(QStringLiteral("reccheck.porysong"));
        QString error;
        int samples = 0;
        check(exportBundleByLabel(doc.label(), bundle, &samples, &error),
              "record: exporting the song as a bundle failed");
        view->setEditCursorTick(tpb * 4);
        const int undoBefore = undo->index();
        m_recordAction->trigger();
        pumpMs(30);
        const uint64_t p1 = playhead();
        send(0x90, 60, 100);
        pumpMs(100);
        check(openBundle(bundle, &error) && m_active && m_active->bundle,
              "record: the bundle tab did not open");
        check(!recording() && undo->index() == undoBefore + 1 &&
                  lastUndoText() == QStringLiteral("Record 1 note(s)") &&
                  find(a, 60, tickOf(p1)).has_value(),
              "record: a tab switch mid-take did not commit to the take's own tab");
        send(0x80, 60, 0);
        pumpMs(30);
        if (m_active && m_active->bundle) {
            updateTransportActions();
            check(!m_recordAction->isEnabled() &&
                      m_recordAction->toolTip().contains(QStringLiteral("read-only")),
                  "record: Record is available on a read-only bundle tab");
            beginRecording();
            check(!recording() && message().contains(QStringLiteral("read-only")),
                  "record: Record on a read-only tab did not say why in the status bar");
            closeTab(m_tabs->indexOf(m_active->view));
        }
        check(m_active == session, "record: closing the bundle tab did not return to the song");
        undo->undo();
    }

    checkRecordingDisplay();
    checkRecordingFlood();
    m_audio.holdDeviceForTest(false);
}

// docs/midi-controls/PLAN.md step 4: the latch (D3, J2), as engine state.
// During a take, once the keyboard moves a lane on the sounding target, the
// song's own events in that lane on that track no longer reach the engine,
// while another track's, an unmoved lane's and the notes still do; a
// document edit mid-take keeps the knob's value; CC121 holds only the lanes
// the take moved. The latch ends, the lanes chased back to the song, at a
// disarm (punch-out), Pause, a seek, a panic, the target going and a loop
// wrap (the loop start's value, and pass 2 plays the song's events). No
// take, or live play off: no latch, the song's events win as ever (the
// second still records).
void MainWindow::checkRecordingLatch(int a, int b)
{
    SongSession *session = m_active;
    SongView *view = session->view;
    SongDocument &doc = session->doc;
    QUndoStack *undo = doc.undoStack();
    MidiInput &midi = midiInput();
    const double rate = m_audio.sampleRate();
    const auto pumpMs = [&](int ms) { m_audio.pumpForTest(uint32_t(rate * ms / 1000.0)); };
    const auto send = [&](uint8_t status, uint8_t d1, uint8_t d2) {
        midi.injectForTest({status, d1, d2});
    };
    const auto playhead = [&] { return m_audio.playheadSamples(); };
    const auto sampleOf = [&](uint64_t tick) { return session->timeline->sampleForTick(tick); };
    // The tick whose events the player has dispatched by now.
    const auto tickNow = [&] {
        return uint64_t(std::max(0.0, std::floor(session->timeline->tickForSample(playhead()))));
    };
    // Pumps until the playhead is `ms` past tick.
    const auto pumpPast = [&](uint64_t tick, int ms) {
        const uint64_t target = sampleOf(tick) + uint64_t(rate * ms / 1000.0);
        for (int i = 0; i < 4000 && playhead() < target; i++)
            pumpMs(5);
        return playhead() >= target;
    };
    // The song's value of a lane at tick, as a full chase gives it.
    const auto songAt = [&](int track, uint8_t cc, uint64_t tick) {
        int value = RecordingTake::restValue(cc);
        for (const DocLanePoint &p : doc.lanePoints(track, cc)) {
            if (p.tick <= tick)
                value = p.value;
        }
        return value;
    };
    const M4AEngine *engine = m_audio.engineForTest();
    struct Heard {
        int vol, pan, bend, volB; // a's VOL, PAN (-64..63) and bend (-64..63); b's VOL
        bool operator==(const Heard &o) const
        {
            return vol == o.vol && pan == o.pan && bend == o.bend && volB == o.volB;
        }
    };
    const auto heard = [&] {
        return Heard{engine->tracks[a].rawVolume, engine->tracks[a].pan, engine->tracks[a].bend,
                     engine->tracks[b].rawVolume};
    };
    // What a full play of the song gives at the playhead.
    const auto song = [&] {
        const uint64_t t = tickNow();
        return Heard{songAt(a, 0x07, t), songAt(a, 0x0A, t) - 64, songAt(a, DOC_CC_BEND, t) >> 7,
                     songAt(b, 0x07, t)};
    };
    const auto report = [&](const char *what, const Heard &got, const Heard &want) {
        std::fprintf(
            stderr, "reccheck:   %s: heard vol %d pan %d bend %d volB %d, want %d %d %d %d\n", what,
            got.vol, got.pan, got.bend, got.volB, want.vol, want.pan, want.bend, want.volB);
    };
    const auto expectHeard = [&](const Heard &want, const char *what) {
        const Heard got = heard();
        if (!check(got == want, what))
            report(what, got, want);
    };
    const auto drainRing = [&] {
        CaptureEvent ev;
        while (m_audio.popCapture(&ev)) {
        }
    };
    const uint64_t tpb = session->timeline->ticksPerBeat;
    const bool loopWas = m_loopAction->isChecked();
    m_loopAction->setChecked(false);

    // The song's events the checks pass: on a, VOL, PAN and the bend at t1,
    // VOL and PAN again at t2; on b, VOL at t1.
    const uint64_t cursor = tpb * 32, t1 = cursor + tpb, t2 = cursor + 2 * tpb;
    const int undoStart = undo->index();
    doc.addLanePoint(a, 0x07, t1, 99);
    doc.addLanePoint(a, 0x0A, t1, 30);
    doc.addLanePoint(a, DOC_CC_BEND, t1, 2048); // 16 in the engine
    doc.addLanePoint(b, 0x07, t1, 55);
    doc.addLanePoint(a, 0x07, t2, 88);
    doc.addLanePoint(a, 0x0A, t2, 70);
    const int undoSetup = undo->index();

    // A take: VOL and the bend moved before t1 hold; PAN (unmoved) and b's
    // VOL play the song's t1 events.
    {
        view->setEditCursorTick(cursor);
        m_recordAction->trigger();
        pumpMs(20);
        send(0xB0, 7, 37);
        send(0xE0, 0x00, 0x60); // +4096: 32
        pumpMs(5);
        check(heard().vol == 37 && heard().bend == 32,
              "latch: the knob and the bend did not sound");
        check(pumpPast(t1, 5), "latch: the take never reached t1");
        check(song() == Heard{99, 30 - 64, 16, 55},
              "latch: the fixture's t1 events are not in force");
        expectHeard({37, 30 - 64, 32, 55},
                    "latch: during a take a moved lane's song event reached the engine, or an "
                    "unmoved lane's or another track's did not");
        // A document edit mid-take: the full chase leaves the held lanes at
        // the knob's values, and the rest at the song's.
        doc.addLanePoint(b, 0x0A, t2 + tpb * 8, 10);
        pumpMs(5);
        expectHeard({37, 30 - 64, 32, 55},
                    "latch: a document edit mid-take did not keep the knob's values on the held "
                    "lanes");
        // CC121: VOL and the bend reset and stay held (the take moved them);
        // PAN resets but the take never moved it, so t2's PAN plays.
        send(0xB0, 121, 0);
        pumpMs(5);
        expectHeard({127, 0, 0, 55}, "latch: Reset All Controllers did not reset the lanes");
        check(pumpPast(t2, 5), "latch: the take never reached t2");
        expectHeard({127, 70 - 64, 0, 55},
                    "latch: after CC121 a moved lane's song event reached the engine, or the reset "
                    "held a lane the take never moved");
        m_stopAction->trigger();
        pumpMs(10);
        while (undo->index() > undoSetup)
            undo->undo(); // the take and the mid-take edit
    }

    // A track inserted before the take's mid-take (added, then moved into
    // its slot): the take follows its track to the next index, and so does
    // the latch. VOL and the bend stay held there through t2's VOL, while the
    // new track at the old index plays its own song values.
    if (check(doc.canAddTrack(), "latch: no free track slot to insert")) {
        view->setEditCursorTick(cursor);
        m_recordAction->trigger();
        pumpMs(20);
        send(0xB0, 7, 37);
        send(0xE0, 0x00, 0x60);
        pumpMs(5);
        check(pumpPast(t1, 5), "latch: the take never reached t1 before the insert");
        const int added = doc.addTrack(0);
        const bool moved = added >= 0 && doc.moveTrack(added, a);
        const int now = m_take.track();
        if (check(moved && now == a + 1 && m_audio.liveTarget() == now,
                  "latch: the take did not follow its track past the inserted one")) {
            pumpMs(5);
            const auto at = [&](int track) {
                return Heard{engine->tracks[track].rawVolume, engine->tracks[track].pan,
                             engine->tracks[track].bend, 0};
            };
            const auto songOf = [&](int track) {
                const uint64_t t = tickNow();
                return Heard{songAt(track, 0x07, t), songAt(track, 0x0A, t) - 64,
                             songAt(track, DOC_CC_BEND, t) >> 7, 0};
            };
            const auto expectAt = [&](int track, const Heard &want, const char *what) {
                const Heard got = at(track);
                if (!check(got == want, what))
                    report(what, got, want);
            };
            expectAt(now, {37, songOf(now).pan, 32, 0},
                     "latch: the insert did not keep the knobs on the take's moved track");
            expectAt(a, songOf(a),
                     "latch: the track now at the take's old index did not play "
                     "its own song values");
            check(pumpPast(t2, 5), "latch: the take never reached t2 after the insert");
            check(songOf(now).vol == 88, "latch: t2's VOL did not move with the take's track");
            expectAt(now, {37, songOf(now).pan, 32, 0},
                     "latch: after the insert a held lane's song event reached the take's track");
            expectAt(a, songOf(a),
                     "latch: after the insert the track at the old index did not "
                     "play its own song values");
        }
        m_stopAction->trigger();
        pumpMs(10);
        while (undo->index() > undoSetup)
            undo->undo(); // the take, the move and the add
    }

    // No take: no latch. The knob sounds until the song's next event.
    {
        view->setEditCursorTick(cursor);
        m_playAction->trigger();
        pumpMs(20);
        send(0xB0, 7, 37);
        pumpMs(5);
        check(heard().vol == 37, "latch: with no take the knob did not sound");
        pumpMs(20); // still before t1
        check(heard().vol == 37 && tickNow() < t1,
              "latch: with no take the knob did not last until the song's next event");
        check(pumpPast(t1, 5), "latch: playback never reached t1");
        expectHeard(song(), "latch: with no take a song event did not win over the knob");
        m_stopAction->trigger();
        pumpMs(10);
    }

    // The latch ends at a disarm (a punch-out), at Pause, at Stop, at a seek
    // and when the target goes (live play off), its lanes chased back to the
    // song where the player is; the song's later events then play. A panic
    // and a port closing don't end it: the take goes on recording the
    // knobs. An arm of the harness's own, with no take: the commit's edit
    // would chase the lanes by itself.
    {
        const auto armAndHold = [&](uint32_t generation) {
            m_audio.seek(sampleOf(cursor));
            m_audio.armCapture(generation);
            m_audio.play();
            pumpMs(20);
            send(0xB0, 7, 37);
            send(0xE0, 0x00, 0x60);
            pumpMs(5);
            pumpPast(t1, 5);
            const Heard held = heard();
            if (!check(held.vol == 37 && held.bend == 32, "latch: a private arm did not hold"))
                report("held", held, {37, 30 - 64, 32, 55});
        };
        armAndHold(0x7E58);
        m_audio.armCapture(0);
        pumpMs(1);
        expectHeard(song(), "latch: a disarm did not chase the held lanes back to the song");
        check(pumpPast(t2, 5), "latch: playback never reached t2");
        expectHeard(song(), "latch: after a disarm the song's events did not play");
        m_audio.stop();
        pumpMs(10);

        armAndHold(0x7E59);
        m_audio.pause();
        pumpMs(1);
        expectHeard(song(), "latch: Pause did not chase the held lanes back to the song");
        m_audio.armCapture(0);
        m_audio.stop();
        pumpMs(10);

        // Stop chases where the player stopped, not where it rewinds to.
        armAndHold(0x7E5D);
        {
            const Heard there = song();
            check(there.vol != songAt(a, 0x07, 0) || there.bend != songAt(a, DOC_CC_BEND, 0) >> 7,
                  "latch: the fixture can't tell the stop position from the song's start");
            m_audio.stop();
            pumpMs(1);
            expectHeard(there, "latch: Stop did not chase the held lanes back to the song where "
                               "it stopped");
        }
        m_audio.armCapture(0);
        pumpMs(10);

        armAndHold(0x7E5A);
        m_audio.seek(sampleOf(cursor));
        pumpMs(1);
        check(pumpPast(t1, 5), "latch: playback never reached t1 after the seek");
        expectHeard(song(), "latch: after a seek in the take the song's events did not play");
        m_audio.armCapture(0);
        m_audio.stop();
        pumpMs(10);

        // A take's track move waiting for its rebuilt timeline
        // (followLiveTrack): a target published meanwhile at the new index
        // stays on the old one, so a callback with the old timeline keeps the
        // latch where the track still is; the rebuild then moves both (here
        // the same timeline stands in for it).
        if (a + 1 < MAX_TRACKS) {
            armAndHold(0x7E5F);
            m_audio.followLiveTrack(0x7E5F, a, a + 1);
            m_audio.setLiveTarget(a + 1);
            check(m_audio.liveTarget() == a,
                  "latch: a target published before the track move's timeline left the old index");
            pumpMs(5);
            check(heard().vol == 37 && heard().bend == 32,
                  "latch: a callback before the track move's timeline ended the latch");
            m_audio.updateTimeline(session->timeline.get());
            const LaneLatch moved = m_audio.liveLatchForTest();
            check(m_audio.liveTarget() == a + 1 && moved.track == a + 1 && moved.lanes.bend &&
                      moved.lanes.hasCc(0x07) && engine->tracks[a + 1].rawVolume == 37 &&
                      engine->tracks[a + 1].bend == 32,
                  "latch: the rebuild did not move the target and the latch to the new index");
            m_audio.armCapture(0);
            m_audio.setLiveTarget(a);
            m_audio.stop();
            pumpMs(10);
        }

        // The app's panic and a port closing keep it: the take goes on
        // recording the knobs, so t2's VOL stays held (PAN, never moved,
        // plays it). The disarm then ends it as ever.
        const auto keptThrough = [&](uint32_t generation, const std::function<void()> &panic,
                                     const char *what) {
            armAndHold(generation);
            panic();
            pumpMs(1);
            const Heard held = heard();
            if (!check(held.vol == 37 && held.bend == 32,
                       qPrintable(QStringLiteral("latch: %1 mid-take ended the latch").arg(what))))
                report(what, held, {37, song().pan, 32, song().volB});
            check(pumpPast(t2, 5), "latch: playback never reached t2 after the panic");
            const Heard t2Held{37, song().pan, 32, song().volB};
            if (!check(heard() == t2Held,
                       qPrintable(QStringLiteral("latch: after %1 mid-take a held lane's song "
                                                 "event reached the engine")
                                      .arg(what))))
                report(what, heard(), t2Held);
            m_audio.armCapture(0);
            pumpMs(1);
            expectHeard(song(), "latch: a disarm after a panic did not hand the lanes back");
            m_audio.stop();
            pumpMs(10);
        };
        keptThrough(0x7E5B, [&] { m_midiPanicAction->trigger(); }, "the app's panic");
        keptThrough(0x7E5E, [&] { emit midi.portsClosed(); }, "a port closing");

        // Live play going off (the target goes) ends it.

        armAndHold(0x7E5C);
        midi.setLivePlay(false);
        pumpMs(1);
        expectHeard(song(), "latch: live play going off did not hand the held lanes back");
        check(pumpPast(t2, 5), "latch: playback never reached t2 after live play went off");
        expectHeard(song(), "latch: after live play went off the song's events did not play");
        midi.setLivePlay(true);
        m_audio.armCapture(0);
        m_audio.stop();
        pumpMs(10);
        drainRing();
    }

    // Live play off: the knob isn't heard, so nothing latches and the song
    // plays on; the move is still recorded.
    {
        midi.setLivePlay(false);
        view->setEditCursorTick(cursor);
        const int undoBefore = undo->index();
        m_recordAction->trigger();
        pumpMs(20);
        const int volBefore = heard().vol;
        send(0xB0, 7, 37);
        pumpMs(5);
        check(heard().vol == volBefore, "latch: with live play off the knob sounded");
        check(pumpPast(t1, 5), "latch: the take never reached t1 with live play off");
        expectHeard(song(), "latch: with live play off a song event was held");
        m_stopAction->trigger();
        pumpMs(10);
        const std::vector<DocLanePoint> vol = doc.lanePoints(a, 0x07);
        check(undo->index() == undoBefore + 1 &&
                  std::any_of(vol.begin(), vol.end(),
                              [](const DocLanePoint &p) { return p.value == 37; }),
              "latch: with live play off the knob was not recorded");
        while (undo->index() > undoBefore)
            undo->undo();
        midi.setLivePlay(true);
    }

    // A loop wrap ends the pass, and the latch: the held lanes come back as
    // an unlatched play of the song has them there — the loop end's state
    // carried across the GOTO (VOL), then the loop start's own events (PAN
    // there), which is also what the take's restore point at the loop end
    // commits — and pass 2 plays the song's events until the knob moves
    // again. A lane that wasn't held carries across the wrap as ever.
    const MidiTimeline *tl = session->timeline.get();
    if (tl->hasLoop() && tl->loopEndTick > tl->loopStartTick + 4 * tpb) {
        const uint64_t loopStart = tl->loopStartTick, loopEnd = tl->loopEndTick;
        const uint64_t inside = loopStart + tpb / 2; // reached early in pass 2
        m_loopAction->setChecked(true);
        if (loopStart >= tpb)
            doc.addLanePoint(a, 0x07, loopStart - tpb / 2, 44);
        doc.addLanePoint(a, 0x07, inside, 66);
        doc.addLanePoint(a, 0x0A, loopStart, 20);
        check(songAt(a, 0x07, loopStart) != songAt(a, 0x07, loopEnd) &&
                  songAt(a, 0x0A, loopStart) != songAt(a, 0x0A, loopEnd),
              "latch: the loop fixture can't tell the loop start from the loop end");
        const int undoBefore = undo->index();
        view->setEditCursorTick(loopEnd - tpb / 4);
        m_recordAction->trigger();
        pumpMs(10);
        const uint64_t pStart = playhead();
        const int bendBefore = heard().bend;
        send(0xB0, 7, 37);
        send(0xB0, 10, 100);
        pumpMs(5);
        check(heard().vol == 37 && heard().pan == 100 - 64,
              "latch: the knobs did not sound before the wrap");
        for (int i = 0; i < 1000 && playhead() >= pStart; i++)
            pumpMs(5);
        check(playhead() < pStart && tickNow() < loopStart + tpb / 4,
              "latch: the take's loop never wrapped");
        const Heard wrapped = heard();
        check(pumpPast(inside, 5), "latch: pass 2 never reached the in-loop VOL point");
        check(song().vol == 66, "latch: the in-loop VOL point is not in force on pass 2");
        expectHeard({song().vol, song().pan, bendBefore, song().volB},
                    "latch: pass 2 did not play the song's own events");
        m_stopAction->trigger();
        pumpMs(10);
        while (undo->index() > undoBefore)
            undo->undo();
        // The same stretch played with no take and no knob: where the wrap
        // left the held lanes is where plain playback is (the engine's own
        // rule, not a model of it); VOL there is the loop end's, not the
        // loop start's.
        view->setEditCursorTick(loopEnd - tpb / 4);
        m_playAction->trigger();
        pumpMs(10);
        const uint64_t qStart = playhead();
        for (int i = 0; i < 1000 && playhead() >= qStart; i++)
            pumpMs(5);
        check(playhead() < qStart && tickNow() < loopStart + tpb / 4,
              "latch: the plain play's loop never wrapped");
        const Heard plain = heard();
        check(plain.vol == songAt(a, 0x07, loopEnd) && plain.vol != songAt(a, 0x07, loopStart),
              "latch: plain playback does not carry the loop end's VOL into pass 2");
        if (!check(wrapped == plain, "latch: the wrap did not leave the held lanes where plain "
                                     "playback has them"))
            report("wrapped", wrapped, plain);
        m_stopAction->trigger();
        pumpMs(10);
    } else {
        std::printf("reccheck: latch loop wrap SKIP (the song has no loop of 4+ beats)\n");
    }

    while (undo->index() > undoStart)
        undo->undo();
    m_loopAction->setChecked(loopWas);
    m_audio.seek(0);
    pumpMs(10);
}

// Program changes recorded onto the Voice lane (docs/midi-controls/PLAN.md
// step 5): heard and held through the song's own voice changes during a
// take, one undo step with the notes ("... with voice"), the song's voice
// back at the stop, a same-tick note playing the new voice, an
// out-of-range program neither heard nor recorded (reported once), the
// tick-0 voice replaced and never deleted, a take that would become a
// track's first voice keeping the song's at tick 0, ghost chips while
// recording, playback of the recorded voices, undo and redo exact; the
// latch ending at Stop, live play off recording without holding, and a
// loop pass's voice restored at the loop end, the wrap handing the voice
// back as plain playback has it.
void MainWindow::checkRecordingVoice(int a, int b)
{
    SongSession *session = m_active;
    SongView *view = session->view;
    SongDocument &doc = session->doc;
    QUndoStack *undo = doc.undoStack();
    MidiInput &midi = midiInput();
    const double rate = m_audio.sampleRate();
    const auto pumpMs = [&](int ms) { m_audio.pumpForTest(uint32_t(rate * ms / 1000.0)); };
    const auto send = [&](uint8_t status, uint8_t d1, uint8_t d2) {
        midi.injectForTest({status, d1, d2});
    };
    const auto playhead = [&] { return m_audio.playheadSamples(); };
    const auto sampleOf = [&](uint64_t tick) { return session->timeline->sampleForTick(tick); };
    const auto tickOf = [&](uint64_t sample) {
        return uint64_t(std::llround(session->timeline->tickForSample(sample)));
    };
    const auto tickNow = [&] {
        return uint64_t(std::max(0.0, std::floor(session->timeline->tickForSample(playhead()))));
    };
    const auto pumpPast = [&](uint64_t tick, int ms) {
        const uint64_t target = sampleOf(tick) + uint64_t(rate * ms / 1000.0);
        for (int i = 0; i < 4000 && playhead() < target; i++)
            pumpMs(5);
        return playhead() >= target;
    };
    using Points = std::vector<std::pair<uint64_t, int>>;
    const auto voices = [&](int track) {
        Points out;
        for (const DocLanePoint &p : doc.lanePoints(track, DOC_CC_VOICE))
            out.push_back({p.tick, p.value});
        return out;
    };
    // The song's voice at tick, as the app plays it: the last program change
    // there or before, else the track's first (primeVoices).
    const auto songVoice = [&](int track, uint64_t tick) {
        const Points v = voices(track);
        int value = v.empty() ? -1 : v.front().second;
        for (const auto &[t, p] : v)
            if (t <= tick)
                value = p;
        return value;
    };
    const auto chunkEvents = [&](int track) {
        return doc.smf().tracks[size_t(doc.smfTrackFor(track))].events;
    };
    const M4AEngine *engine = m_audio.engineForTest();
    const auto program = [&](int track) { return int(engine->tracks[track].currentProgram); };
    const auto lastUndoText = [&] {
        return undo->index() > 0 ? undo->text(undo->index() - 1) : QString();
    };
    const auto drainRing = [&] {
        CaptureEvent ev;
        while (m_audio.popCapture(&ev)) {
        }
    };
    const uint64_t tpb = session->timeline->ticksPerBeat;
    const int count = m_active->vgSource ? m_active->vgSource->voiceCount() : VOICEGROUP_SIZE;
    // Six voices neither track uses: three to play, three for the song.
    std::vector<int> pick;
    {
        Points used = voices(a);
        const Points ub = voices(b);
        used.insert(used.end(), ub.begin(), ub.end());
        for (int v = 1; v < count && pick.size() < 6; v++)
            if (std::none_of(used.begin(), used.end(),
                             [&](const auto &p) { return p.second == v; }))
                pick.push_back(v);
    }
    if (!check(pick.size() == 6 && count < 128 && !voices(a).empty(),
               "voice: the fixture has no voice on the track, too few free voices, or a full "
               "voicegroup")) {
        std::fprintf(stderr, "reccheck:   %zu free, %d voices, first change at %lld\n", pick.size(),
                     count, voices(a).empty() ? -1LL : (long long)voices(a).front().first);
        return;
    }
    const uint8_t P1 = uint8_t(pick[0]), P2 = uint8_t(pick[1]), P3 = uint8_t(pick[2]);
    const int S1 = pick[3], Sb = pick[4], S2 = pick[5];
    const bool loopWas = m_loopAction->isChecked();
    m_loopAction->setChecked(false);
    const bool lanesWere = view->automationLanesVisible();
    view->setAutomationLanesVisible(true);
    view->selectTrack(a);
    auto *lanes = view->findChild<QWidget *>(QStringLiteral("automationArea"));

    // The song changes voice at t1 on both tracks.
    const uint64_t cursor = tpb * 24, t1 = cursor + tpb;
    const int undoStart = undo->index();
    doc.addLanePoint(a, DOC_CC_VOICE, t1, S1);
    doc.addLanePoint(b, DOC_CC_VOICE, t1, Sb);
    const int undoSetup = undo->index();

    // The song's .s as mid2agb compiles it, VOICE commands counted by
    // voice; empty without the project's mid2agb.
    const QString mid2agb = m_project.root() + QStringLiteral("/tools/mid2agb/mid2agb");
    const auto compiledVoices = [&](const SmfFile &smf) {
        std::map<int, int> counts;
        QTemporaryDir dir;
        const QString mid = dir.filePath(QStringLiteral("reccheck_voice.mid"));
        const QString out = dir.filePath(QStringLiteral("reccheck_voice.s"));
        QString error;
        if (!QFileInfo::exists(mid2agb) || !smf.writeFile(mid, &error))
            return counts;
        QProcess proc;
        proc.start(mid2agb, QStringList() << doc.cfg().rawFlags << mid << out);
        if (!proc.waitForFinished(15000) || proc.exitCode() != 0)
            return counts;
        QFile file(out);
        if (!file.open(QIODevice::ReadOnly))
            return counts;
        const QString text = QString::fromLatin1(file.readAll());
        static const QRegularExpression voiceOp(QStringLiteral("VOICE\\s*,\\s*(\\d+)"));
        for (auto it = voiceOp.globalMatch(text); it.hasNext();)
            counts[it.next().captured(1).toInt()]++;
        return counts;
    };

    // A take with two program changes over two notes.
    {
        const Points before = voices(a);
        const auto eventsBefore = chunkEvents(a);
        const SmfFile smfBefore = doc.smf();
        const int reportsBefore = m_liveIgnoredReports;
        view->setEditCursorTick(cursor);
        m_recordAction->trigger();
        pumpMs(20);
        // Painted once before the change: the chip below must come from
        // the row's repaint, not a first paint.
        synchronizePlayhead();
        if (lanes)
            (void)lanes->grab();
        const uint64_t p1 = playhead();
        send(0xC0, P1, 0);
        pumpMs(5);
        check(program(a) == P1 && program(b) == songVoice(b, cursor),
              "voice: a program change in a take did not sound on the target alone");
        send(0x90, 60, 100);
        pumpMs(40);
        send(0x80, 60, 0);
        pumpMs(20);
        // The ghost: a faded chip at the change, on the Voice row.
        synchronizePlayhead();
        {
            const RecordingPreview::Lane *ghost = view->recordingPreview().lane(a, DOC_CC_VOICE);
            check(!view->model().findLane(a, DOC_CC_VOICE),
                  "voice: the take's voice brought up a CC lane row (it has its own row)");
            check(ghost && ghost->ranges.size() == 1 && ghost->ranges.front().first == tickOf(p1) &&
                      std::any_of(ghost->points.begin(), ghost->points.end(),
                                  [&](const LanePoint &pt) {
                                      return pt.tick == tickOf(p1) && pt.value == P1;
                                  }),
                  "voice: the take's Voice lane ghost does not hold the program change");
            const std::optional<QPointF> at =
                view->recordingPreviewLanePoint(DOC_CC_VOICE, tickOf(p1), 0);
            if (check(lanes && at, "voice: no Voice row to probe")) {
                const qreal dpr = lanes->devicePixelRatioF();
                const QImage image = lanes->grab().toImage();
                const auto pixel = [&](QPointF p) {
                    return image.pixelColor(int(p.x() * dpr), int(p.y() * dpr));
                };
                const QColor color = SongView::trackColor(a);
                const QColor under = pixel(*at - QPointF(0, 6));
                const QColor got = pixel(*at);
                const auto ch = [&](int f, int u) {
                    return (f * songview::kRecordingGhostAlpha +
                            u * (255 - songview::kRecordingGhostAlpha)) /
                           255;
                };
                check(got != under && std::abs(got.red() - ch(color.red(), under.red())) <= 3 &&
                          std::abs(got.green() - ch(color.green(), under.green())) <= 3 &&
                          std::abs(got.blue() - ch(color.blue(), under.blue())) <= 3,
                      "voice: the take's voice chip is not painted in its track's colour at "
                      "reduced alpha");
            }
        }
        const uint64_t p2 = playhead();
        // A note on the change's own callback: one tick, the voice first.
        send(0xC0, P2, 0);
        send(0x90, 62, 90);
        pumpMs(5);
        // Past the voicegroup: ignored (not heard, not recorded), reported
        // once.
        send(0xC0, uint8_t(count), 0);
        pumpMs(5);
        pollLiveKeys();
        check(program(a) == P2 && m_liveIgnoredReports == reportsBefore + 1,
              "voice: an out-of-range program change sounded, or was not reported once");
        // The song's t1 change is held back on the target, not on b.
        check(pumpPast(t1, 5), "voice: the take never reached t1");
        check(program(a) == P2 && program(b) == Sb,
              "voice: during a take the song's voice change reached the moved track, or "
              "another track's did not");
        // A document edit's full chase gives the held voice back to the
        // keyboard's.
        doc.addLanePoint(b, 0x0A, t1 + tpb * 8, 10);
        pumpMs(5);
        check(program(a) == P2, "voice: a document edit mid-take did not keep the held voice");
        pumpMs(30);
        const uint64_t pStop = playhead();
        m_stopAction->trigger();
        send(0x80, 62, 0);
        pumpMs(10);
        const uint64_t tP1 = tickOf(p1), tP2 = tickOf(p2), tStop = tickOf(pStop);
        check(lastUndoText() == QStringLiteral("Record 2 note(s) with voice") &&
                  statusBar()->currentMessage() == QStringLiteral("Recorded 2 note(s) with voice."),
              "voice: the take did not commit as \"Record 2 note(s) with voice\"");
        Points want;
        for (const auto &pt : before)
            if (pt.first < tP1 || pt.first > tStop)
                want.push_back(pt);
        want.push_back({tP1, P1});
        want.push_back({tP2, P2});
        want.push_back({tStop, S1}); // the song's voice there (t1's, inside the take)
        std::sort(want.begin(), want.end());
        const Points after = voices(a);
        if (!check(after == want, "voice: the Voice lane is not the take's changes over the "
                                  "song's, with the song's voice back at the stop")) {
            for (const auto &[t, v] : after)
                std::fprintf(stderr, "reccheck:   got %llu %d\n", (unsigned long long)t, v);
            for (const auto &[t, v] : want)
                std::fprintf(stderr, "reccheck:   want %llu %d\n", (unsigned long long)t, v);
        }
        check(std::none_of(after.begin(), after.end(),
                           [&](const auto &p) { return p.second == count; }),
              "voice: an out-of-range program change was recorded");
        // The same-tick note follows its voice change, in the file (mid2agb
        // keeps file order within a type... and sorts VOICE first anyway) and
        // in the timeline the app plays.
        {
            const auto evs = chunkEvents(a);
            int pc = -1, on = -1;
            for (int i = 0; i < int(evs.size()); i++) {
                if (evs[i].tick != tP2 || !evs[i].isChannel())
                    continue;
                if (evs[i].typeNibble() == 0xC && pc < 0)
                    pc = i;
                if (evs[i].isNoteOn() && evs[i].data0 == 62 && on < 0)
                    on = i;
            }
            int tpc = -1, ton = -1;
            const auto &tev = session->timeline->events;
            for (int i = 0; i < int(tev.size()); i++) {
                if (tev[i].track != a || tev[i].tick != tP2)
                    continue;
                if (tev[i].type == 0xC && tpc < 0)
                    tpc = i;
                if (tev[i].type == 0x9 && tev[i].data0 == 62 && ton < 0)
                    ton = i;
            }
            check(pc >= 0 && on > pc && tpc >= 0 && ton > tpc,
                  "voice: a note on its voice change's tick does not follow it (file or "
                  "timeline)");
        }
        // mid2agb compiles them as mid-song VOICE commands.
        if (QFileInfo::exists(mid2agb)) {
            std::map<int, int> was = compiledVoices(smfBefore), now = compiledVoices(doc.smf());
            check(!was.empty() && now[P1] == was[P1] + 1 && now[P2] == was[P2] + 1,
                  "voice: mid2agb does not compile the recorded program changes as VOICE "
                  "commands");
        } else {
            std::printf("reccheck: voice mid2agb SKIP (no %s)\n", qUtf8Printable(mid2agb));
        }
        // Playback sounds the recorded voices.
        view->setEditCursorTick(cursor);
        m_playAction->trigger();
        pumpMs(5);
        const int atCursor = program(a);
        check(pumpPast(tP1, 2), "voice: playback never reached the first change");
        const int atP1 = program(a);
        check(pumpPast(tP2, 2), "voice: playback never reached the second change");
        const int atP2 = program(a);
        check(pumpPast(tStop, 2), "voice: playback never reached the stop");
        const int atStop = program(a);
        check(atCursor == songVoice(a, cursor) && atP1 == P1 && atP2 == P2 && atStop == S1,
              "voice: playback after the commit does not sound the recorded voices");
        m_stopAction->trigger();
        pumpMs(10);
        undo->undo();
        check(voices(a) == before && chunkEvents(a) == eventsBefore,
              "voice: undo did not restore the track exactly");
        undo->redo();
        check(voices(a) == want, "voice: redo did not put the take's voices back");
        undo->undo();
    }

    // A take at tick 0. With no voice there before (the song's first comes
    // later), the take's becomes the track's initial voice and the song's
    // first one comes back at the stop. With one there, the take replaces
    // it, never deletes it. Undo puts the track back byte for byte.
    const auto voiceIn = [](const Points &points, uint64_t tick) {
        int value = points.empty() ? -1 : points.front().second;
        for (const auto &[t, v] : points)
            if (t <= tick)
                value = v;
        return value;
    };
    for (const bool seeded : {false, true}) {
        const int undoBefore = undo->index();
        if (seeded && voices(a).front().first != 0)
            doc.addLanePoint(a, DOC_CC_VOICE, 0, S2);
        const Points before = voices(a);
        const auto eventsBefore = chunkEvents(a);
        view->setEditCursorTick(0);
        m_recordAction->trigger();
        send(0xC0, P3, 0);
        pumpMs(30);
        const uint64_t tStop = tickOf(playhead());
        m_stopAction->trigger();
        pumpMs(10);
        int atZero = 0, value = -1;
        for (const SmfEvent &ev : chunkEvents(a))
            if (ev.tick == 0 && ev.isChannel() && ev.typeNibble() == 0xC) {
                atZero++;
                value = ev.data0;
            }
        Points want{{0, P3}};
        if (voiceIn(before, tStop) != P3)
            want.push_back({tStop, voiceIn(before, tStop)});
        for (const auto &pt : before)
            if (pt.first > tStop)
                want.push_back(pt);
        if (!check(tStop > 0 && atZero == 1 && value == P3 && voices(a) == want,
                   seeded ? "voice: a take at tick 0 did not replace the initial voice (one "
                            "tick-0 change, the take's), with the song's voice back at the stop"
                          : "voice: a take at tick 0 on a track whose first voice is later did "
                            "not start the track on it, with the song's first voice back at the "
                            "stop")) {
            for (const auto &[t, v] : voices(a))
                std::fprintf(stderr, "reccheck:   got %llu %d\n", (unsigned long long)t, v);
            for (const auto &[t, v] : want)
                std::fprintf(stderr, "reccheck:   want %llu %d\n", (unsigned long long)t, v);
        }
        undo->undo();
        check(chunkEvents(a) == eventsBefore, "voice: undo did not restore the tick-0 voice");
        if (seeded) {
            // writeLanePoints over tick 0 with no point there keeps the
            // initial voice (J4).
            doc.writeLanePoints(a, DOC_CC_VOICE, 0, tpb, {{tpb / 2, P1}});
            const Points kept = voices(a);
            check(kept.size() >= 2 && kept[0] == before.front() &&
                      kept[1] == std::make_pair(tpb / 2, int(P1)),
                  "voice: a voice write over tick 0 deleted the initial voice");
            undo->undo();
        }
        while (undo->index() > undoBefore)
            undo->undo();
    }

    // A track whose first voice change is later: the take's would become
    // its first voice, so the song's moves to tick 0 (the stretch before the
    // take keeps it) and comes back at the stop. A move, not a copy: the
    // later change, which only repeats what the restore plays there, goes,
    // so the moved one is the track's initial voice to the header's voice
    // editor and to Merge.
    if (check(doc.canAddTrack(), "voice: no free track slot")) {
        const int undoBefore = undo->index();
        const bool followWas = m_followPlayheadAction->isChecked();
        m_followPlayheadAction->setChecked(false);
        const int added = doc.addTrack(S2);
        // Near the start, so tick 0 and the song's changes are on screen.
        const uint64_t start = tpb, later = start + tpb * 4, later2 = later + tpb * 2;
        std::vector<DocLanePoint> seed =
            added >= 0 ? doc.lanePoints(added, DOC_CC_VOICE) : std::vector<DocLanePoint>();
        if (check(added >= 0 && seed.size() == 1 && seed.front().tick == 0,
                  "voice: a new track has no tick-0 voice")) {
            doc.moveLanePoint(added, DOC_CC_VOICE, seed.front(), later, S2);
            doc.addLanePoint(added, DOC_CC_VOICE, later2, P3);
            view->selectTrack(added);
            view->setEditCursorTick(start);
            view->ensureTickVisible(0);
            const auto eventsBefore = chunkEvents(added);
            m_recordAction->trigger();
            pumpMs(20);
            const uint64_t tP = tickOf(playhead());
            send(0xC0, P1, 0);
            pumpMs(30);
            synchronizePlayhead();
            // The ghost is the lane as the commit will leave it: the lead
            // at tick 0, the take, no song change left at `later`.
            const RecordingPreview::Lane *ghost =
                view->recordingPreview().lane(added, DOC_CC_VOICE);
            check(ghost && ghost->points.size() == 4 && ghost->points[0].tick == 0 &&
                      ghost->points[0].value == S2 && ghost->points[1].tick == tP &&
                      ghost->points[1].value == P1 && ghost->points[2].value == S2 &&
                      ghost->points[2].tick < later && ghost->points[3].tick == later2,
                  "voice: the Voice ghost is not the moved lead, the take, its restore and the "
                  "song's later change (the moved one gone)");
            const qreal dpr = lanes ? lanes->devicePixelRatioF() : 1.0;
            const auto pixel = [&](const QImage &image, QPointF p) {
                return image.pixelColor(int(p.x() * dpr), int(p.y() * dpr));
            };
            const auto onScreen = [&](const std::optional<QPointF> &at) {
                return lanes && at && at->x() >= songview::kGutterW + 2 &&
                       at->x() <= lanes->width() - 2;
            };
            // The lead is the commit's, not yet the song's: faded, as the
            // take's own chips are.
            const std::optional<QPointF> leadAt =
                view->recordingPreviewLanePoint(DOC_CC_VOICE, 0, 0);
            if (check(onScreen(leadAt), "voice: the lead chip is off screen")) {
                const QImage image = lanes->grab().toImage();
                const QColor color = SongView::trackColor(added);
                const QColor under = pixel(image, *leadAt - QPointF(0, 6));
                const QColor got = pixel(image, *leadAt);
                const auto ch = [&](int f, int u) {
                    return (f * songview::kRecordingGhostAlpha +
                            u * (255 - songview::kRecordingGhostAlpha)) /
                           255;
                };
                check(got != under && std::abs(got.red() - ch(color.red(), under.red())) <= 3 &&
                          std::abs(got.green() - ch(color.green(), under.green())) <= 3 &&
                          std::abs(got.blue() - ch(color.blue(), under.blue())) <= 3,
                      "voice: the take's lead chip is not painted at the ghost alpha");
            }
            // A band slide over the Voice row mid-take still moves the
            // song's changes: its destination previews over the ghost.
            {
                SongView::TimeSelection band;
                // The change mid-band: its destination is inside the landing
                // frame, not on its dashed edge.
                band.startTick = later2 - tpb / 2;
                band.endTick = later2 + tpb / 2;
                band.scope = SongView::TimeSelection::Lanes;
                band.lanes = {{added, DOC_CC_VOICE}};
                view->setTimeSelection(band);
                const double press = double(later2);
                const bool began = view->beginRangeDrag(press, false);
                view->updateRangeDrag(press + double(tpb));
                const int64_t dTick = view->rangeDrag().dTick;
                const std::optional<QPointF> dest = view->recordingPreviewLanePoint(
                    DOC_CC_VOICE, uint64_t(int64_t(later2) + dTick), 0);
                if (check(began && dTick > 0 && onScreen(dest),
                          "voice: no range drag over the Voice row mid-take, or its destination "
                          "is off screen")) {
                    const QImage image = lanes->grab().toImage();
                    const QColor want = themes::color(themes::Role::song_view_edit_preview_outline);
                    const QColor got = pixel(image, *dest);
                    check(std::abs(got.red() - want.red()) <= 3 &&
                              std::abs(got.green() - want.green()) <= 3 &&
                              std::abs(got.blue() - want.blue()) <= 3,
                          "voice: a range drag over the Voice row's ghost shows no preview of the "
                          "song's carried change");
                }
                view->cancelRangeDrag();
                view->clearTimeSelection();
            }
            const uint64_t tStop = tickOf(playhead());
            m_stopAction->trigger();
            pumpMs(10);
            const Points want{{0, S2}, {tP, P1}, {tStop, S2}, {later2, P3}};
            if (!check(tStop < later && voices(added) == want,
                       "voice: a take becoming a track's first voice did not move the song's to "
                       "tick 0 (back at the stop, the moved one gone from its old tick)")) {
                for (const auto &[t, v] : voices(added))
                    std::fprintf(stderr, "reccheck:   got %llu %d\n", (unsigned long long)t, v);
                for (const auto &[t, v] : want)
                    std::fprintf(stderr, "reccheck:   want %llu %d\n", (unsigned long long)t, v);
            }
            const auto eventsAfter = chunkEvents(added);
            undo->undo();
            const bool undone = chunkEvents(added) == eventsBefore;
            undo->redo();
            check(undone && chunkEvents(added) == eventsAfter,
                  "voice: undo/redo of the moved lead is not exact");
            // The header's voice editor edits the moved lead: the start
            // through the take's start, then the take's voice, then the
            // restore.
            {
                QTimer answer;
                answer.setInterval(10);
                int picked = 0;
                QObject::connect(&answer, &QTimer::timeout, [&] {
                    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                    auto *list = dialog ? dialog->findChild<QListWidget *>() : nullptr;
                    if (!list)
                        return;
                    list->setCurrentRow(P2);
                    picked++;
                    dialog->accept();
                });
                answer.start();
                view->editTrackVoice(added);
                answer.stop();
                const Points edited{{0, P2}, {tP, P1}, {tStop, S2}, {later2, P3}};
                check(picked == 1 && voices(added) == edited,
                      "voice: the header's voice edit after a take did not change the voice "
                      "from the start through the take's start alone");
                if (picked == 1)
                    undo->undo();
            }
            // Merge leaves the moved lead behind as the track's identity and
            // carries the take and the song's later change.
            if (a < added) {
                Points merged = voices(a);
                for (const std::pair<uint64_t, int> &pt :
                     Points{{tP, P1}, {tStop, S2}, {later2, P3}}) {
                    merged.erase(std::remove_if(merged.begin(), merged.end(),
                                                [&](const auto &m) { return m.first == pt.first; }),
                                 merged.end());
                    merged.insert(std::upper_bound(merged.begin(), merged.end(), pt,
                                                   [](const auto &x, const auto &y) {
                                                       return x.first < y.first;
                                                   }),
                                  pt);
                }
                check(doc.mergeTrack(added, a, false) && voices(a) == merged,
                      "voice: Merge did not treat the moved lead as the track's identity");
                undo->undo();
            }
        }
        while (undo->index() > undoBefore)
            undo->undo();
        view->selectTrack(a);
        m_followPlayheadAction->setChecked(followWas);
    }

    // Stop ends the latch: the voice goes back to the song's where the
    // player stopped. A private arm, so no commit edit chases it.
    {
        m_audio.seek(sampleOf(cursor));
        m_audio.armCapture(0x7E60);
        m_audio.play();
        pumpMs(20);
        send(0xC0, P1, 0);
        pumpMs(5);
        check(pumpPast(t1, 5) && program(a) == P1, "voice: a private arm did not hold the voice");
        m_audio.stop();
        pumpMs(1);
        check(program(a) == S1, "voice: Stop did not hand the held voice back to the song");
        m_audio.armCapture(0);
        pumpMs(10);
        drainRing();
    }

    // Live play off: not heard, not held, still recorded.
    {
        midi.setLivePlay(false);
        const int undoBefore = undo->index();
        view->setEditCursorTick(cursor);
        m_recordAction->trigger();
        pumpMs(20);
        const int was = program(a);
        send(0xC0, P1, 0);
        pumpMs(5);
        check(program(a) == was, "voice: with live play off the program change sounded");
        // An out-of-range one is still reported: the take would have
        // recorded it.
        const int reports = m_liveIgnoredReports;
        send(0xC0, uint8_t(count), 0);
        pumpMs(5);
        pollLiveKeys();
        check(m_liveIgnoredReports == reports + 1,
              "voice: with live play off an out-of-range program change in a take was not "
              "reported");
        check(pumpPast(t1, 5) && program(a) == S1,
              "voice: with live play off the song's voice change was held");
        m_stopAction->trigger();
        pumpMs(10);
        const Points v = voices(a);
        check(undo->index() == undoBefore + 1 &&
                  std::any_of(v.begin(), v.end(), [&](const auto &p) { return p.second == P1; }),
              "voice: with live play off the program change was not recorded");
        while (undo->index() > undoBefore)
            undo->undo();
        midi.setLivePlay(true);
    }

    // A loop pass: the voice before the wrap is restored at the loop end;
    // the wrap hands the held voice back as plain playback has it there,
    // and pass 2 plays the song's own change until the next press.
    const MidiTimeline *tl = session->timeline.get();
    if (tl->hasLoop() && tl->loopEndTick > tl->loopStartTick + 4 * tpb) {
        const uint64_t loopStart = tl->loopStartTick, loopEnd = tl->loopEndTick;
        const uint64_t inside = loopStart + tpb / 2;
        m_loopAction->setChecked(true);
        doc.addLanePoint(a, DOC_CC_VOICE, inside, S2);
        const Points before = voices(a);
        const int undoBefore = undo->index();
        view->setEditCursorTick(loopEnd - tpb / 4);
        m_recordAction->trigger();
        pumpMs(10);
        const uint64_t pStart = playhead();
        const uint64_t tP1 = tickOf(pStart);
        send(0xC0, P1, 0);
        pumpMs(5);
        check(program(a) == P1, "voice: the program change did not sound before the wrap");
        for (int i = 0; i < 1000 && playhead() >= pStart; i++)
            pumpMs(5);
        check(playhead() < pStart && tickNow() < loopStart + tpb / 4,
              "voice: the take's loop never wrapped");
        const int wrapped = program(a);
        check(pumpPast(inside, 5) && program(a) == S2,
              "voice: pass 2 did not play the song's own voice change");
        const uint64_t tP2 = tickOf(playhead());
        send(0xC0, P2, 0);
        pumpMs(30);
        const uint64_t tStop = tickOf(playhead());
        m_stopAction->trigger();
        pumpMs(10);
        // Pass 1 to the loop end (the song's voice back there), pass 2 to
        // the stop (S2, played on pass 2, back there).
        Points want;
        for (const auto &pt : before)
            if (!(pt.first >= tP1 && pt.first <= loopEnd) &&
                !(pt.first >= tP2 && pt.first <= tStop))
                want.push_back(pt);
        want.push_back({tP1, P1});
        int atEnd = before.front().second;
        for (const auto &[t, v] : before)
            if (t <= loopEnd)
                atEnd = v;
        if (atEnd != P1)
            want.push_back({loopEnd, atEnd});
        want.push_back({tP2, P2});
        want.push_back({tStop, S2});
        std::sort(want.begin(), want.end());
        check(lastUndoText() == QStringLiteral("Record voice") && voices(a) == want,
              "voice: a looped take's voice is not restored at the loop end and the stop");
        while (undo->index() > undoBefore)
            undo->undo();
        // Plain playback through the same wrap.
        view->setEditCursorTick(loopEnd - tpb / 4);
        m_playAction->trigger();
        pumpMs(10);
        const uint64_t qStart = playhead();
        for (int i = 0; i < 1000 && playhead() >= qStart; i++)
            pumpMs(5);
        check(playhead() < qStart && wrapped == program(a) && wrapped != P1,
              "voice: the wrap did not hand the held voice back as plain playback has it");
        m_stopAction->trigger();
        pumpMs(10);
    } else {
        std::printf("reccheck: voice loop SKIP (the song has no loop of 4+ beats)\n");
    }

    while (undo->index() > undoSetup)
        undo->undo();
    while (undo->index() > undoStart)
        undo->undo();
    m_loopAction->setChecked(loopWas);
    view->setAutomationLanesVisible(lanesWere);
    m_audio.seek(0);
    pumpMs(10);
}

// The take's live display (plan step 4), on the song's own tab: ghost notes
// in the roll and a REC tint on the ruler, fed once per playhead tick; a
// held note grows with the clock, repainting only a sliver of the roll;
// with Follow Playhead off nothing scrolls; Stop swaps the ghosts for the
// real notes and the tint goes; a dropped take (its track deleted) clears
// them; then the bend and mod ghosts in the automation lanes
// (checkRecordingLaneDisplay). PORYDAW_RECCHECK_SHOT=<file.png> saves the
// view mid-take (and <file>-lanes.png with a bend ghost).
void MainWindow::checkRecordingDisplay()
{
    MidiInput &midi = midiInput();
    SongSession *session = m_active;
    SongView *view = session->view;
    SongDocument &doc = session->doc;
    QUndoStack *undo = doc.undoStack();
    const double rate = m_audio.sampleRate();
    const auto pumpMs = [&](int ms) { m_audio.pumpForTest(uint32_t(rate * ms / 1000.0)); };
    const auto send = [&](uint8_t status, uint8_t d1, uint8_t d2) {
        midi.injectForTest({status, d1, d2});
    };
    const auto tickOf = [&](uint64_t sample) {
        return uint64_t(std::llround(session->timeline->tickForSample(sample)));
    };
    auto *roll = dynamic_cast<songview::TimelineSurface *>(view->findChild<QWidget *>("pianoRoll"));
    auto *ruler = view->findChild<QWidget *>(QStringLiteral("timeRuler"));
    if (!check(roll && ruler, "display: the song view has no piano roll or ruler"))
        return;
    int a = -1;
    for (int t = 0; t < MAX_TRACKS && a < 0; t++) {
        if (session->timeline->tracks[t].used && doc.smfTrackFor(t) >= 0)
            a = t;
    }
    // A pitch no track plays, so the probes see only the ghost (or the
    // recorded note) over the bare row.
    int key = -1;
    for (int k = 96; k >= 36 && key < 0; k--) {
        bool used = false;
        for (int t = 0; t < MAX_TRACKS && !used; t++) {
            for (const DocNote &n : doc.notesForTrack(t))
                used = used || n.key == k;
        }
        if (!used)
            key = k;
    }
    if (!check(a >= 0 && key >= 0, "display: no song track, or every pitch is used"))
        return;

    // A real layout, zoomed in so a short held note is wide.
    resize(1400, 900);
    show();
    QApplication::processEvents();
    view->selectTrack(a);
    SongView::ViewState state = view->viewState();
    state.pxPerBeat = 96.0;
    state.keyHeight = 16.0;
    view->applyViewState(state);
    view->ensureKeyVisible(key);
    QApplication::processEvents();
    const qreal dpr = roll->devicePixelRatioF();
    const auto pixel = [&](const QImage &image, QPointF at) {
        return image.pixelColor(int(at.x() * dpr), int(at.y() * dpr));
    };
    const auto blendsTo = [](const QColor &got, const QColor &fill, int alpha,
                             const QColor &under) {
        const auto ch = [&](int f, int u) { return (f * alpha + u * (255 - alpha)) / 255; };
        return std::abs(got.red() - ch(fill.red(), under.red())) <= 3 &&
               std::abs(got.green() - ch(fill.green(), under.green())) <= 3 &&
               std::abs(got.blue() - ch(fill.blue(), under.blue())) <= 3;
    };
    const QColor rec = themes::color(themes::Role::song_view_playhead);
    const int gridWidth = roll->width() - songview::kKeyboardW;
    // Halfway across the roll: room to grow, and the follow-scroll (which
    // jumps past 85%) is a short pump away.
    const uint64_t cursor = uint64_t(std::max(0.0, view->tickAtContentX(gridWidth * 0.5)));
    const bool followWas = m_followPlayheadAction->isChecked();
    m_followPlayheadAction->setChecked(false);
    const double scrollBefore = view->viewState().scrollPx;
    const int undoBefore = undo->index();
    view->setEditCursorTick(cursor);
    m_recordAction->trigger();
    pumpMs(30);
    synchronizePlayhead();
    check(view->recordingPreview().notes.empty() && view->recordingPreview().spans.size() == 1 &&
              view->recordingPreview().spans.front().first == cursor,
          "display: a fresh take's REC span does not start at the punch-in");

    send(0x90, uint8_t(key), 100);
    pumpMs(150);
    synchronizePlayhead();
    const RecordingPreview first = view->recordingPreview();
    {
        uint64_t pos = 0;
        uint32_t segment = 0;
        m_audio.captureClock(&pos, &segment);
        const auto notes =
            m_take.toNotes(*session->timeline, pos, segment, recordingLatencySamples());
        check(first.track == a && first.take == m_take.generation() && first.notes == notes &&
                  first.notes.size() == 1 && first.notes.front().key == key &&
                  first.notes.front().tick + first.notes.front().duration ==
                      tickOf(m_audio.playheadSamples()),
              "display: the ghost notes are not the take as a commit would write it, held to "
              "the clock");
    }
    if (first.notes.size() != 1) {
        send(0x80, uint8_t(key), 0);
        pumpMs(30);
        m_stopAction->trigger();
        while (undo->index() > undoBefore)
            undo->undo();
        m_followPlayheadAction->setChecked(followWas);
        return;
    }
    const RecordingPreview::Note ghost = first.notes.front();
    const QRectF ghostRect = view->recordingPreviewNoteRect(ghost);
    const QPointF ghostProbe = ghostRect.center();
    check(std::abs(ghostRect.left() -
                   view->displayX(double(ghost.tick), songview::kKeyboardW, dpr)) < 0.01 &&
              std::abs(ghostRect.right() - view->displayX(double(ghost.tick + ghost.duration),
                                                          songview::kKeyboardW, dpr)) < 0.01,
          "display: the ghost is not placed at its ticks");
    const QImage withGhost = roll->grab().toImage();
    const QImage rulerDuring = ruler->grab().toImage();
    // The same frame without the overlay (the next tick feeds it again).
    view->clearRecordingPreview();
    const QImage withoutGhost = roll->grab().toImage();
    view->setRecordingPreview(first);
    check(blendsTo(pixel(withGhost, ghostProbe), view->noteFillColor(a, ghost.velocity),
                   songview::kRecordingGhostAlpha, pixel(withoutGhost, ghostProbe)) &&
              pixel(withGhost, ghostProbe) != pixel(withoutGhost, ghostProbe),
          "display: the held note's ghost is not painted in its track's colour at reduced alpha");
    const QRectF spanRect = view->recordingPreviewSpanRect(first.spans.front());
    check(spanRect.width() > 4 && pixel(rulerDuring, QPointF(spanRect.center().x(), 0.0)) == rec &&
              pixel(rulerDuring,
                    QPointF(view->displayX(double(cursor), songview::kGutterW, dpr) - 4, 0.0)) !=
                  rec,
          "display: the ruler shows no REC tint over the punched span alone");
    // The top strip stays solid over the ruler's bands: a time selection
    // across the span paints its fill there first.
    {
        SongView::TimeSelection sel;
        sel.startTick = first.spans.front().first;
        sel.endTick = first.spans.front().second + 1;
        view->setTimeSelection(sel);
        const QImage rulerSelected = ruler->grab().toImage();
        view->clearTimeSelection();
        check(pixel(rulerSelected, QPointF(spanRect.center().x(), 0.0)) == rec,
              "display: a time selection painted over the REC strip");
    }

    // Growing: the next ticks lengthen the ghost to the clock, and the
    // roll re-rasterizes only around its moving end.
    (void)roll->grab();
    const songview::TimelineSurfaceDiagnostics before = roll->diagnostics();
    pumpMs(150);
    synchronizePlayhead();
    const QImage grown = roll->grab().toImage();
    const songview::TimelineSurfaceDiagnostics after = roll->diagnostics();
    const RecordingPreview second = view->recordingPreview();
    check(second.notes.size() == 1 && second.notes.front().tick == ghost.tick &&
              second.notes.front().duration > ghost.duration &&
              second.notes.front().tick + second.notes.front().duration ==
                  tickOf(m_audio.playheadSamples()),
          "display: the held note's ghost did not grow to the playhead");
    if (second.notes.size() == 1) {
        const QRectF grownRect = view->recordingPreviewNoteRect(second.notes.front());
        const QPointF tailProbe((ghostRect.right() + grownRect.right()) / 2, ghostProbe.y());
        view->clearRecordingPreview();
        const QImage grownWithout = roll->grab().toImage();
        view->setRecordingPreview(second);
        check(grownRect.right() > ghostRect.right() + 8 &&
                  blendsTo(pixel(grown, tailProbe), view->noteFillColor(a, ghost.velocity),
                           songview::kRecordingGhostAlpha, pixel(grownWithout, tailProbe)) &&
                  pixel(grown, tailProbe) != pixel(grownWithout, tailProbe),
              "display: the grown part of the ghost is not painted");
    }
    const quint64 rollPixels = quint64(roll->width() * dpr) * quint64(roll->height() * dpr);
    const quint64 repainted = after.contentPaintPixelCount - before.contentPaintPixelCount;
    if (!check(repainted > 0 && repainted * 8 < rollPixels,
               "display: a growing ghost repainted more than a sliver of the roll"))
        std::fprintf(stderr, "reccheck:   repainted %llu of %llu device pixels\n",
                     (unsigned long long)repainted, (unsigned long long)rollPixels);
    if (const QByteArray shot = qgetenv("PORYDAW_RECCHECK_SHOT"); !shot.isEmpty())
        view->grab().save(QString::fromLocal8Bit(shot));

    // Follow Playhead off: the playhead runs past where following would
    // have scrolled (85% of the roll), and the view stays put.
    for (int i = 0; i < 200 && view->contentX(view->playheadTick()) < gridWidth * 0.95; i++) {
        pumpMs(50);
        synchronizePlayhead();
    }
    check(view->contentX(view->playheadTick()) >= gridWidth * 0.95,
          "display: the playhead never crossed the follow-scroll threshold");
    check(view->viewState().scrollPx == scrollBefore,
          "display: with Follow Playhead off the view scrolled during the take");

    // Stop: the ghosts go, the real note is there, and so is the tint.
    send(0x80, uint8_t(key), 0);
    pumpMs(30);
    m_stopAction->trigger();
    const QImage committed = roll->grab().toImage();
    const QImage rulerAfter = ruler->grab().toImage();
    std::optional<DocNote> landed;
    for (const DocNote &n : doc.notesForTrack(a)) {
        if (n.key == key && n.tick == ghost.tick)
            landed = n;
    }
    check(view->recordingPreview().empty() && undo->index() == undoBefore + 1 && landed,
          "display: after Stop the ghost notes stayed, or the take did not land");
    if (landed) {
        const QRectF real = view->recordingPreviewNoteRect(
            {landed->tick, landed->key, landed->duration, landed->velocity});
        const QPointF at(real.left() + (ghostProbe.x() - ghostRect.left()), real.center().y());
        check(pixel(committed, at) == view->noteFillColor(a, landed->velocity),
              "display: after Stop the recorded note is not painted where its ghost was");
    }
    check(pixel(rulerAfter, QPointF(spanRect.center().x(), 0.0)) != rec,
          "display: the REC tint stayed after Stop");
    undo->undo();

    // A dropped take (its track deleted) clears the display too.
    m_recordAction->trigger();
    pumpMs(30);
    send(0x90, uint8_t(key), 100);
    pumpMs(100);
    synchronizePlayhead();
    const int undoAtDelete = undo->index();
    check(!view->recordingPreview().empty(), "display: the second take shows nothing");
    const RecordingPreview dropped = view->recordingPreview();
    doc.deleteTrack(a);
    const QImage rulerDropped = ruler->grab().toImage();
    check(m_takeSession == nullptr && view->recordingPreview().empty() &&
              (dropped.spans.empty() ||
               pixel(rulerDropped,
                     QPointF(view->recordingPreviewSpanRect(dropped.spans.front()).center().x(),
                             0.0)) != rec),
          "display: a dropped take left its ghost notes or REC tint");
    send(0x80, uint8_t(key), 0);
    pumpMs(30);
    m_stopAction->trigger();
    while (undo->index() > undoAtDelete)
        undo->undo();
    check(undo->index() == undoBefore, "display: undo did not restore the song");

    checkRecordingLaneDisplay(a);
    checkRecordingCcLaneDisplay(a);
    m_followPlayheadAction->setChecked(followWas);
}

// The take's bend and mod ghosts (the lanes' half of plan step 4): the
// BEND row shows the take's curve as a commit would leave it while the take
// runs, its row appearing for the take when the track had none (and staying
// through "Remove empty lane" until the take ends); the rewritten stretch is
// painted in the track's colour at the ghost notes' alpha and turns solid at
// Stop; the ghosts follow their track at once when a track is inserted
// before it; a row the take alone showed goes with the take (undo, or a
// dropped take). On a row the track already had, the take's bend joins
// without rebuilding the rows, and the row is read-only while the ghost
// shows (no hover, no click-delete of a node it hides); a take waiting on an
// open edit group re-reads its ghosts against the song.
void MainWindow::checkRecordingLaneDisplay(int track)
{
    MidiInput &midi = midiInput();
    SongSession *session = m_active;
    SongView *view = session->view;
    SongDocument &doc = session->doc;
    QUndoStack *undo = doc.undoStack();
    const double rate = m_audio.sampleRate();
    const auto pumpMs = [&](int ms) { m_audio.pumpForTest(uint32_t(rate * ms / 1000.0)); };
    const auto send = [&](uint8_t status, uint8_t d1, uint8_t d2) {
        midi.injectForTest({status, d1, d2});
    };
    const auto tickOf = [&](uint64_t sample) {
        return uint64_t(std::llround(session->timeline->tickForSample(sample)));
    };
    auto *lanes = view->findChild<QWidget *>(QStringLiteral("automationArea"));
    if (!check(lanes, "lane display: the song view has no automation area"))
        return;
    const qreal dpr = lanes->devicePixelRatioF();
    const auto pixel = [&](const QImage &image, QPointF at) {
        return image.pixelColor(int(at.x() * dpr), int(at.y() * dpr));
    };
    const auto blendsTo = [](const QColor &got, const QColor &fill, int alpha,
                             const QColor &under) {
        const auto ch = [&](int f, int u) { return (f * alpha + u * (255 - alpha)) / 255; };
        return std::abs(got.red() - ch(fill.red(), under.red())) <= 3 &&
               std::abs(got.green() - ch(fill.green(), under.green())) <= 3 &&
               std::abs(got.blue() - ch(fill.blue(), under.blue())) <= 3;
    };
    const auto mouse = [&](QEvent::Type type, QPointF at, Qt::MouseButton button) {
        QMouseEvent ev(type, at, QPointF(lanes->mapToGlobal(at.toPoint())), button,
                       type == QEvent::MouseButtonPress ? Qt::MouseButtons(button) : Qt::NoButton,
                       Qt::NoModifier);
        QCoreApplication::sendEvent(lanes, &ev);
    };
    const auto hoverNodeTick = [&] { return lanes->property("hoverNodeTick").toLongLong(); };
    const auto rebuilds = [&] { return lanes->property("rowRebuilds").toInt(); };
    // The shapes below are in ticks of the injected messages' arrival.
    check(recordingLatencySamples() == 0, "lane display: the checks assume no latency offset");
    const bool lanesWere = view->automationLanesVisible();
    const int selectedWas = view->selectedTrack();
    view->setAutomationLanesVisible(true);
    view->selectTrack(track);
    // No bend on the track, so its row is the take's alone. An empty BEND
    // row the view state holds goes too, and comes back at the end.
    const std::vector<std::pair<int, uint8_t>> emptyLanes = view->viewState().emptyLanes;
    const bool emptyBendWas =
        std::find(emptyLanes.begin(), emptyLanes.end(),
                  std::pair<int, uint8_t>(track, LANE_CC_BEND)) != emptyLanes.end();
    const int undoBefore = undo->index();
    if (const std::vector<DocLanePoint> bends = doc.lanePoints(track, DOC_CC_BEND); !bends.empty())
        doc.deleteLanePoints(track, DOC_CC_BEND, bends);
    if (view->model().findLane(track, LANE_CC_BEND))
        view->removeEmptyLane(track, LANE_CC_BEND);
    QApplication::processEvents();
    check(!view->model().findLane(track, LANE_CC_BEND),
          "lane display: the track's bend lane could not be cleared for the check");

    const int undoTake = undo->index();
    m_recordAction->trigger();
    pumpMs(30);
    synchronizePlayhead();
    check(view->recordingPreview().lanes.empty() && !view->model().findLane(track, LANE_CC_BEND),
          "lane display: a take with no moves shows a lane");
    const uint64_t pBend = m_audio.playheadSamples();
    send(0xE0, 0x00, 0x60); // +4096
    pumpMs(200);
    synchronizePlayhead();
    const uint64_t pNow = m_audio.playheadSamples();
    const RecordingPreview preview = view->recordingPreview();
    const RecordingPreview::Lane *bend = preview.lane(track, LANE_CC_BEND);
    const uint64_t from = tickOf(pBend), to = tickOf(pNow);
    // The take's move, then the song's value (0: no bend) back at the clock.
    const bool shape = bend && preview.lanes.size() == 1 && bend->ranges.size() == 1 &&
                       bend->ranges.front() == std::make_pair(from, to) &&
                       bend->points.size() == 2 && bend->points[0].tick == from &&
                       bend->points[0].value == 4096 && bend->points[1].tick == to &&
                       bend->points[1].value == 0;
    if (!check(shape, "lane display: the bend ghost is not the take's move, restored at the clock"))
        if (bend)
            for (const LanePoint &pt : bend->points)
                std::fprintf(stderr, "reccheck:   ghost %u %d (want %llu..%llu)\n", pt.tick,
                             pt.value, (unsigned long long)from, (unsigned long long)to);
    check(view->model().findLane(track, LANE_CC_BEND) != nullptr,
          "lane display: the take's bend did not bring up a BEND row");
    const std::optional<QPointF> at =
        view->recordingPreviewLanePoint(LANE_CC_BEND, (from + to) / 2, 4096);
    const QColor color = SongView::trackColor(track);
    QPointF probe;
    if (check(at.has_value() && to > from + 4,
              "lane display: no BEND row to probe, or too short")) {
        probe = *at;
        const QImage during = lanes->grab().toImage();
        const QColor under = pixel(during, probe + QPointF(0, 6));
        check(blendsTo(pixel(during, probe), color, songview::kRecordingGhostAlpha, under) &&
                  pixel(during, probe) != under,
              "lane display: the bend ghost is not painted in its track's colour at reduced "
              "alpha");
        if (const QByteArray shot = qgetenv("PORYDAW_RECCHECK_SHOT"); !shot.isEmpty())
            lanes->grab().save(QString::fromLocal8Bit(shot).replace(QStringLiteral(".png"),
                                                                    QStringLiteral("-lanes.png")));
    }

    // Growing: the next frame lengthens the ghost to the clock, the row
    // repainted from around the old end on, not whole.
    if (auto *surface = dynamic_cast<songview::TimelineSurface *>(lanes); at && surface) {
        const std::optional<QPointF> top = view->recordingPreviewLanePoint(LANE_CC_BEND, to, 8191);
        const std::optional<QPointF> bottom =
            view->recordingPreviewLanePoint(LANE_CC_BEND, to, -8192);
        (void)lanes->grab();
        const songview::TimelineSurfaceDiagnostics before = surface->diagnostics();
        pumpMs(150);
        synchronizePlayhead();
        const QImage grown = lanes->grab().toImage();
        const songview::TimelineSurfaceDiagnostics after = surface->diagnostics();
        const RecordingPreview::Lane *g = view->recordingPreview().lane(track, LANE_CC_BEND);
        const uint64_t grownTo = g && g->ranges.size() == 1 ? g->ranges.front().second : to;
        const std::optional<QPointF> tail =
            view->recordingPreviewLanePoint(LANE_CC_BEND, (to + grownTo) / 2, 4096);
        check(grownTo > to + 4 && tail &&
                  blendsTo(pixel(grown, *tail), color, songview::kRecordingGhostAlpha,
                           pixel(grown, *tail + QPointF(0, 6))) &&
                  pixel(grown, *tail) != pixel(grown, *tail + QPointF(0, 6)),
              "lane display: the grown part of the bend ghost is not painted");
        // The row is its plot (top to bottom of the value axis) and 10 px of
        // margin; the old end's step and a node's reach to its left.
        const qreal rowH = top && bottom ? bottom->y() - top->y() + 10 : 0;
        const qreal fromX = std::max<qreal>(songview::kGutterW, top ? top->x() - 16 : 0);
        const quint64 allowed = quint64((lanes->width() - fromX + 4) * dpr * (rowH + 2) * dpr);
        const quint64 repainted = after.contentPaintPixelCount - before.contentPaintPixelCount;
        if (!check(repainted > 0 && repainted <= allowed,
                   "lane display: a growing bend ghost repainted more than the row past its end"))
            std::fprintf(stderr, "reccheck:   repainted %llu, allowed %llu device pixels\n",
                         (unsigned long long)repainted, (unsigned long long)allowed);
    }

    // "Remove empty lane" on the take's row: the take is about to write it.
    view->removeEmptyLane(track, LANE_CC_BEND);
    check(view->model().findLane(track, LANE_CC_BEND) != nullptr &&
              view->recordingPreviewLanePoint(LANE_CC_BEND, from, 4096).has_value(),
          "lane display: removing the take's empty BEND row took it away mid-take");

    // A track inserted before the take's (added, moved into its slot): the
    // ghosts are on the take's track at once, before the next frame, and
    // the slot it left gets no BEND row.
    if (check(doc.canAddTrack(), "lane display: no free track slot to insert")) {
        const int undoAtInsert = undo->index();
        const int added = doc.addTrack(0);
        const bool moved = added >= 0 && doc.moveTrack(added, track);
        check(moved && view->recordingPreview().track == track + 1 &&
                  view->model().findLane(track + 1, LANE_CC_BEND) != nullptr &&
                  !view->model().findLane(track, LANE_CC_BEND),
              "lane display: the bend ghost did not follow its track when one was inserted "
              "before it");
        while (undo->index() > undoAtInsert)
            undo->undo();
        check(view->recordingPreview().track == track &&
                  view->model().findLane(track, LANE_CC_BEND) != nullptr &&
                  !view->model().findLane(track + 1, LANE_CC_BEND),
              "lane display: the bend ghost did not follow its track back on undo");
    }

    // Stop: the ghost turns into the real curve, painted solid where it was.
    send(0xE0, 0x00, 0x40);
    pumpMs(30);
    m_stopAction->trigger();
    check(view->recordingPreview().empty() && undo->index() == undoTake + 1 &&
              view->model().findLane(track, LANE_CC_BEND) != nullptr,
          "lane display: after Stop the ghost stayed, or the bend did not land");
    if (at) {
        const QImage committed = lanes->grab().toImage();
        check(blendsTo(pixel(committed, probe), color, 255, color),
              "lane display: after Stop the bend is not painted solid where its ghost was");
    }
    undo->undo();
    check(!view->model().findLane(track, LANE_CC_BEND),
          "lane display: undoing the take left its BEND row");

    // A row the track already has, with a node where the take will move:
    // the take's bend joins it without a rebuild (which would end a gesture,
    // hover, or point menu there), and while the ghost shows the row is
    // read-only, the node it hides neither hoverable nor click-deletable.
    {
        const int undoAtRow = undo->index();
        m_recordAction->trigger();
        pumpMs(30);
        synchronizePlayhead();
        const uint64_t node = tickOf(m_audio.playheadSamples()) + 2;
        doc.writeLanePoints(track, DOC_CC_BEND, node, node, {{node, -2048}});
        QApplication::processEvents();
        const std::optional<QPointF> nodeAt =
            view->recordingPreviewLanePoint(LANE_CC_BEND, node, -2048);
        if (check(nodeAt.has_value(), "lane display: no BEND row for the node")) {
            mouse(QEvent::MouseMove, *nodeAt, Qt::NoButton);
            check(hoverNodeTick() == qlonglong(node),
                  "lane display: the node is not hoverable before the take moves");
            const int rebuildsBefore = rebuilds();
            send(0xE0, 0x00, 0x60);
            pumpMs(200);
            synchronizePlayhead();
            check(view->recordingPreview().lane(track, LANE_CC_BEND) != nullptr &&
                      rebuilds() == rebuildsBefore,
                  "lane display: the take's bend joining a BEND row the track had rebuilt the "
                  "rows");
            // Back at the clock to the node's value: read against the song
            // as edited mid-take, not as it was at punch-in.
            const RecordingPreview::Lane *joined =
                view->recordingPreview().lane(track, LANE_CC_BEND);
            check(joined && !joined->points.empty() && joined->points.back().value == -2048,
                  "lane display: the bend ghost was not read against the song as edited "
                  "mid-take");
            check(hoverNodeTick() == -1,
                  "lane display: the ghost coming to the hovered row left its node hover");
            mouse(QEvent::MouseMove, *nodeAt, Qt::NoButton);
            check(hoverNodeTick() == -1, "lane display: a node under the take's ghost hovers");
            const uint64_t revision = doc.revision();
            mouse(QEvent::MouseButtonPress, *nodeAt, Qt::LeftButton);
            mouse(QEvent::MouseButtonRelease, *nodeAt, Qt::LeftButton);
            check(doc.revision() == revision,
                  "lane display: a click on a node under the take's ghost edited the lane");
            // An edit while the ghost shows: the next frame has it.
            const uint64_t far = tickOf(m_audio.playheadSamples()) + 200;
            doc.writeLanePoints(track, DOC_CC_BEND, far, far, {{far, 500}});
            pumpMs(30);
            synchronizePlayhead();
            const RecordingPreview::Lane *edited =
                view->recordingPreview().lane(track, LANE_CC_BEND);
            check(edited && std::any_of(edited->points.begin(), edited->points.end(),
                                        [&](const LanePoint &pt) {
                                            return pt.tick == far && pt.value == 500;
                                        }),
                  "lane display: the bend ghost missed an edit made while it shows");
        }

        // Stopped inside an open edit group, the take waits for it; an
        // edit in the group meanwhile shows in its ghost.
        send(0xE0, 0x00, 0x40);
        pumpMs(30);
        synchronizePlayhead();
        const uint64_t later = tickOf(m_audio.playheadSamples()) + 100;
        doc.beginEditGroup(QStringLiteral("Script"));
        m_stopAction->trigger();
        doc.writeLanePoints(track, DOC_CC_BEND, later, later, {{later, 1000}});
        const auto ghostHasLater = [&] {
            const RecordingPreview::Lane *lane = view->recordingPreview().lane(track, LANE_CC_BEND);
            return lane &&
                   std::any_of(lane->points.begin(), lane->points.end(), [&](const LanePoint &pt) {
                       return pt.tick == later && pt.value == 1000;
                   });
        };
        QElapsedTimer waited;
        waited.start();
        while (!ghostHasLater() && waited.elapsed() < 2000)
            QApplication::processEvents(QEventLoop::AllEvents, 10);
        check(ghostHasLater(),
              "lane display: a deferred take's ghost did not follow an edit made while it waits");
        doc.endEditGroup(/*discard=*/true);
        const int undoAfterGroup = undo->index();
        waited.restart();
        while (undo->index() == undoAfterGroup && waited.elapsed() < 2000)
            QApplication::processEvents(QEventLoop::AllEvents, 10);
        check(undo->index() == undoAfterGroup + 1 && view->recordingPreview().empty() &&
                  !ghostHasLater(),
              "lane display: the deferred take did not land after its edit group");
        while (undo->index() > undoAtRow)
            undo->undo();
    }

    // A dropped take takes its row with it.
    m_recordAction->trigger();
    pumpMs(30);
    send(0xE0, 0x00, 0x60);
    pumpMs(100);
    synchronizePlayhead();
    check(view->model().findLane(track, LANE_CC_BEND) != nullptr,
          "lane display: the second take's bend brought up no row");
    const int undoAtDelete = undo->index();
    doc.deleteTrack(track);
    check(m_takeSession == nullptr && view->recordingPreview().empty(),
          "lane display: deleting the take's track did not drop it");
    undo->undo();
    check(undo->index() == undoAtDelete && !view->model().findLane(track, LANE_CC_BEND),
          "lane display: a dropped take left its BEND row");
    send(0xE0, 0x00, 0x40);
    pumpMs(30);
    m_stopAction->trigger();
    while (undo->index() > undoBefore)
        undo->undo();
    check(undo->index() == undoBefore, "lane display: undo did not restore the song");
    if (emptyBendWas)
        view->addEmptyLane(track, LANE_CC_BEND);
    view->selectTrack(selectedWas);
    view->setAutomationLanesVisible(lanesWere);
}

// The routed lanes' ghosts (docs/midi-controls/PLAN.md step 3): VOL, BENDR
// and LFOS knobs ghost on their rows while a take runs, BENDR and LFOS on
// rows of their own the take brings up (the track has no points there), each
// curve back at the clock to the song's value or, with no point, the
// engine's track-init value (BENDR 2, LFOS 22); a ghost past a zoomed axis
// grows it; Stop leaves the real curves and undo the rows the take brought.
void MainWindow::checkRecordingCcLaneDisplay(int track)
{
    MidiInput &midi = midiInput();
    SongSession *session = m_active;
    SongView *view = session->view;
    SongDocument &doc = session->doc;
    QUndoStack *undo = doc.undoStack();
    const double rate = m_audio.sampleRate();
    const auto pumpMs = [&](int ms) { m_audio.pumpForTest(uint32_t(rate * ms / 1000.0)); };
    const auto send = [&](uint8_t status, uint8_t d1, uint8_t d2) {
        midi.injectForTest({status, d1, d2});
    };
    const auto tickOf = [&](uint64_t sample) {
        return uint64_t(std::llround(session->timeline->tickForSample(sample)));
    };
    auto *lanes = view->findChild<QWidget *>(QStringLiteral("automationArea"));
    if (!check(lanes, "cc lane display: the song view has no automation area"))
        return;
    const qreal dpr = lanes->devicePixelRatioF();
    const auto pixel = [&](const QImage &image, QPointF at) {
        return image.pixelColor(int(at.x() * dpr), int(at.y() * dpr));
    };
    const auto blendsTo = [](const QColor &got, const QColor &fill, int alpha,
                             const QColor &under) {
        const auto ch = [&](int f, int u) { return (f * alpha + u * (255 - alpha)) / 255; };
        return std::abs(got.red() - ch(fill.red(), under.red())) <= 3 &&
               std::abs(got.green() - ch(fill.green(), under.green())) <= 3 &&
               std::abs(got.blue() - ch(fill.blue(), under.blue())) <= 3;
    };
    const bool lanesWere = view->automationLanesVisible();
    const int selectedWas = view->selectedTrack();
    view->setAutomationLanesVisible(true);
    view->selectTrack(track);
    const std::vector<std::pair<int, uint8_t>> emptyWere = view->viewState().emptyLanes;
    const int undoBefore = undo->index();
    // BENDR and LFOS bare: the take's rows are its own.
    for (const uint8_t cc : {uint8_t(0x14), uint8_t(0x15)}) {
        if (const std::vector<DocLanePoint> pts = doc.lanePoints(track, cc); !pts.empty())
            doc.deleteLanePoints(track, cc, pts);
        if (view->model().findLane(track, cc))
            view->removeEmptyLane(track, cc);
    }
    QApplication::processEvents();
    check(!view->model().findLane(track, 0x14) && !view->model().findLane(track, 0x15),
          "cc lane display: the track's BENDR and LFOS lanes could not be cleared");
    // LFOS zoomed to 0..32: a knob past it grows the axis.
    view->setLaneDisplayRange(track, 0x15, 32);
    check(view->laneDisplayMax(track, 0x15) == 32, "cc lane display: LFOS did not zoom to 32");
    check(midi.setRoute(0x14, 20) && midi.setRoute(0x15, 21),
          "cc lane display: BENDR and LFOS could not be routed");
    // VOL low and flat, so a rising knob raises its ghost's peak (the
    // restore at the clock is the song's 40, not 127).
    if (const std::vector<DocLanePoint> pts = doc.lanePoints(track, 0x07); !pts.empty())
        doc.deleteLanePoints(track, 0x07, pts);
    doc.addLanePoint(track, 0x07, 0, 40);
    QApplication::processEvents();
    const auto volBefore = doc.lanePoints(track, 0x07);

    const int undoTake = undo->index();
    m_recordAction->trigger();
    pumpMs(30);
    synchronizePlayhead();
    const uint64_t pMove = m_audio.playheadSamples();
    send(0xB0, 20, 127); // BENDR 24
    send(0xB0, 21, 20);  // LFOS 20: inside the zoom
    send(0xB0, 7, 50);   // VOL 50
    pumpMs(250);
    synchronizePlayhead();
    (void)lanes->grab(); // painted on the 0..32 axis
    // VOL rising under its fixed 0..127 axis rescales nothing: no row
    // repaints whole for it.
    const auto rescales = [&] { return lanes->property("rescaleRepaints").toInt(); };
    const int rescalesBefore = rescales();
    for (int v = 60; v <= 120; v += 10) {
        send(0xB0, 7, uint8_t(v));
        pumpMs(20);
        synchronizePlayhead();
    }
    check(rescales() == rescalesBefore,
          "cc lane display: a VOL ghost rising under its fixed axis repainted its row whole");
    (void)lanes->grab(); // what the VOL frames left, painted: the LFOS stretch on 0..32
    const uint64_t pLfos = m_audio.playheadSamples();
    send(0xB0, 21, 100); // LFOS 100: past it
    pumpMs(250);
    synchronizePlayhead();
    check(rescales() > rescalesBefore,
          "cc lane display: an LFOS ghost growing its zoomed axis did not repaint its row whole");
    const uint64_t pNow = m_audio.playheadSamples();
    const uint64_t from = tickOf(pMove), to = tickOf(pNow), lfosTick = tickOf(pLfos);
    const RecordingPreview preview = view->recordingPreview();
    std::vector<uint8_t> ghostCcs;
    for (const RecordingPreview::Lane &l : preview.lanes)
        ghostCcs.push_back(l.cc);
    check(ghostCcs == std::vector<uint8_t>{0x07, 0x14, 0x15},
          "cc lane display: the take's ghosts are not VOL, BENDR and LFOS, sorted by cc");
    // The move, then the track-init value back at the clock.
    const auto restsAt = [&](uint8_t cc, int value, int rest) {
        const RecordingPreview::Lane *l = preview.lane(track, cc);
        return l && l->ranges.size() == 1 && l->ranges.front() == std::make_pair(from, to) &&
               l->points.size() == 2 && l->points[0].tick == from && l->points[0].value == value &&
               l->points[1].tick == to && l->points[1].value == rest;
    };
    const RecordingPreview::Lane *lfos = preview.lane(track, 0x15);
    check(restsAt(0x14, 24, 2) && lfos && lfos->ranges.size() == 1 &&
              lfos->ranges.front() == std::make_pair(from, to) && lfos->points.size() == 3 &&
              lfos->points[0].tick == from && lfos->points[0].value == 20 &&
              lfos->points[1].tick == lfosTick && lfos->points[1].value == 100 &&
              lfos->points[2].tick == to && lfos->points[2].value == 22,
          "cc lane display: the BENDR and LFOS ghosts are not the moves, back at the clock to the "
          "track-init values");
    const RecordingPreview::Lane *vol = preview.lane(track, 0x07);
    check(vol &&
              std::any_of(vol->points.begin(), vol->points.end(),
                          [&](const LanePoint &pt) { return pt.tick == from && pt.value == 50; }),
          "cc lane display: the VOL ghost does not hold the knob's move");
    check(view->model().findLane(track, 0x14) && view->model().findLane(track, 0x15),
          "cc lane display: the take's BENDR and LFOS moves did not bring up their rows");
    check(view->laneDisplayMax(track, 0x15) >= 100,
          "cc lane display: an LFOS ghost past the zoomed axis did not grow it");
    const QColor color = SongView::trackColor(track);
    // Probed off the line, below it: LFOS 100 tops its grown axis and
    // BENDR 24 tops its 0..24 one.
    const std::optional<QPointF> lfosAt =
        view->recordingPreviewLanePoint(0x15, (lfosTick + to) / 2, 100);
    // LFOS 20 before the axis grew, where the grown axis draws it: the
    // whole row repainted, not only past the change.
    const std::optional<QPointF> lfosEarly =
        view->recordingPreviewLanePoint(0x15, (from + lfosTick) / 2, 20);
    const std::optional<QPointF> bendrAt =
        view->recordingPreviewLanePoint(0x14, (from + to) / 2, 24);
    if (check(lfosAt && lfosEarly && bendrAt && to > lfosTick + 4 && lfosTick > from + 4,
              "cc lane display: no BENDR or LFOS row to probe, or too short")) {
        const QImage during = lanes->grab().toImage();
        const QColor underL = pixel(during, *lfosAt + QPointF(0, 6));
        const QColor underB = pixel(during, *bendrAt + QPointF(0, 6));
        check(blendsTo(pixel(during, *lfosAt), color, songview::kRecordingGhostAlpha, underL) &&
                  pixel(during, *lfosAt) != underL,
              "cc lane display: the LFOS ghost is not painted in its track's colour at reduced "
              "alpha");
        const QColor underE = pixel(during, *lfosEarly - QPointF(0, 6));
        check(blendsTo(pixel(during, *lfosEarly), color, songview::kRecordingGhostAlpha, underE) &&
                  pixel(during, *lfosEarly) != underE,
              "cc lane display: the LFOS ghost before its axis grew was not repainted on the new "
              "axis");
        check(blendsTo(pixel(during, *bendrAt), color, songview::kRecordingGhostAlpha, underB) &&
                  pixel(during, *bendrAt) != underB,
              "cc lane display: the BENDR ghost is not painted in its track's colour at reduced "
              "alpha");
    }

    pumpMs(30);
    m_stopAction->trigger();
    check(view->recordingPreview().empty() && undo->index() == undoTake + 1 &&
              undo->text(undo->index() - 1) ==
                  QStringLiteral("Record volume, bend range and LFO speed"),
          "cc lane display: Stop did not commit the VOL, BENDR and LFOS take");
    check(view->model().findLane(track, 0x14) && view->model().findLane(track, 0x15) &&
              doc.lanePoints(track, 0x14).size() == 2 && doc.lanePoints(track, 0x15).size() == 3,
          "cc lane display: after Stop the BENDR and LFOS rows do not hold the take");
    if (lfosAt) {
        const QImage committed = lanes->grab().toImage();
        check(blendsTo(pixel(committed, *lfosAt), color, 255, color),
              "cc lane display: after Stop LFOS is not painted solid where its ghost was");
    }
    undo->undo();
    check(!view->model().findLane(track, 0x14) && !view->model().findLane(track, 0x15) &&
              doc.lanePoints(track, 0x07).size() == volBefore.size(),
          "cc lane display: undoing the take left its BENDR or LFOS row (or VOL changed)");

    // Put back the knobs, the routes, the zoom and the song.
    m_audio.seek(0);
    pumpMs(10);
    midi.setRoute(0x14, MidiRoutes::kNone);
    midi.setRoute(0x15, MidiRoutes::kNone);
    view->setLaneDisplayRange(track, 0x15, 127);
    while (undo->index() > undoBefore)
        undo->undo();
    check(undo->index() == undoBefore, "cc lane display: undo did not restore the song");
    for (const std::pair<int, uint8_t> &key : emptyWere) {
        if (key.first == track && (key.second == 0x14 || key.second == 0x15))
            view->addEmptyLane(track, key.second);
    }
    view->selectTrack(selectedWas);
    view->setAutomationLanesVisible(lanesWere);
}

// The plan's knob flood (docs/midi-controls/PLAN.md §5): three knobs at
// 1 kHz each for 10 s, with notes played through it, drained at the
// playhead timer's cadence: nothing is lost, the take commits, and each
// lane holds at most a point a tick (same-tick last wins). Then the UI
// stalls (no drain) under the flood: the ring fills with moves, but the
// slots kept for notes take every note played meanwhile, and the loss is
// reported.
void MainWindow::checkRecordingFlood()
{
    MidiInput &midi = midiInput();
    SongSession *session = m_active;
    SongDocument &doc = session->doc;
    QUndoStack *undo = doc.undoStack();
    const double rate = m_audio.sampleRate();
    const auto pumpMs = [&](int ms) { m_audio.pumpForTest(uint32_t(rate * ms / 1000.0)); };
    const auto send = [&](uint8_t status, uint8_t d1, uint8_t d2) {
        midi.injectForTest({status, d1, d2});
    };
    const auto tickOf = [&](uint64_t sample) {
        return uint64_t(std::llround(session->timeline->tickForSample(sample)));
    };
    const int track = session->view->selectedTrack();
    check(midi.setRoute(0x15, 21), "flood: LFOS could not be routed");
    const uint64_t tpb = session->timeline->ticksPerBeat;
    const bool loopWas = m_loopAction->isChecked();
    m_loopAction->setChecked(false); // one segment, so the point bound is the span's ticks
    session->view->setEditCursorTick(tpb * 4);
    const auto countNew = [&](const std::vector<DocNote> &before) {
        int n = 0;
        for (const DocNote &note : doc.notesForTrack(track)) {
            const bool old = std::any_of(before.begin(), before.end(), [&](const DocNote &b) {
                return b.tick == note.tick && b.key == note.key && b.duration == note.duration;
            });
            n += !old;
        }
        return n;
    };
    // One millisecond of the flood: each knob moves (a sweep apiece), and
    // every 100 ms a note on or off on its own key.
    const auto floodMs = [&](int ms, int *notesOn) {
        send(0xB0, 7, uint8_t((ms * 3) & 0x7F));
        send(0xB0, 10, uint8_t((ms * 5 + 40) & 0x7F));
        send(0xB0, 21, uint8_t((ms * 7 + 80) & 0x7F));
        if (ms % 100 == 0)
            send(0x90, uint8_t(36 + (*notesOn % 48)), 100);
        else if (ms % 100 == 50)
            send(0x80, uint8_t(36 + ((*notesOn)++ % 48)), 0);
        pumpMs(1);
    };

    // Drained every 17 ms, as the playhead timer does.
    {
        const std::vector<DocNote> before = doc.notesForTrack(track);
        const int undoBefore = undo->index();
        const uint32_t droppedBefore = m_audio.captureDropped();
        m_recordAction->trigger();
        pumpMs(20);
        const uint64_t pStart = m_audio.playheadSamples();
        int notes = 0;
        QElapsedTimer frames;
        qint64 slowestFrameNs = 0;
        for (int ms = 0; ms < 10000; ms++) {
            floodMs(ms, &notes);
            if (ms % 17 == 16) {
                frames.start();
                synchronizePlayhead(); // the drain and the ghosts' frame
                slowestFrameNs = std::max(slowestFrameNs, frames.nsecsElapsed());
            }
        }
        synchronizePlayhead();
        const uint64_t pEnd = m_audio.playheadSamples();
        const size_t controls = m_take.controls().size();
        const size_t taken = m_take.notes().size();
        frames.start();
        const size_t writes =
            m_take.toLaneWrites(*session->timeline, pEnd, m_take.segment(), 0).size();
        const qint64 writesNs = frames.nsecsElapsed();
        m_stopAction->trigger();
        check(m_audio.captureDropped() == droppedBefore,
              "flood: a drained 3 kHz knob flood lost capture events");
        check(controls == 30000, "flood: the take did not keep every knob move");
        check(taken == 100 && undo->index() == undoBefore + 1 && countNew(before) == 100,
              "flood: the flooded take did not commit every note in one step");
        const uint64_t span = tickOf(pEnd) - tickOf(pStart) + 2;
        bool bounded = true;
        for (const uint8_t cc : {uint8_t(0x07), uint8_t(0x0A), uint8_t(0x15)}) {
            // The take's stretch: the song keeps its points either side.
            const std::vector<DocLanePoint> all = doc.lanePoints(track, cc);
            std::vector<DocLanePoint> pts;
            std::copy_if(all.begin(), all.end(), std::back_inserter(pts),
                         [&](const DocLanePoint &pt) {
                             return pt.tick >= tickOf(pStart) && pt.tick <= tickOf(pEnd);
                         });
            if (pts.empty() || pts.size() > span) {
                bounded = false;
                std::fprintf(stderr, "reccheck:   lane 0x%02X: %zu points over %llu ticks\n", cc,
                             pts.size(), (unsigned long long)span);
            }
        }
        check(bounded, "flood: a flooded lane holds more than a point a tick");
        check(writes == 3, "flood: the flooded take is not one write a lane");
        std::printf("reccheck: flood: %zu moves over %llu ticks; toLaneWrites %.2f ms, slowest "
                    "drain+preview frame %.2f ms\n",
                    controls, (unsigned long long)(span - 2), double(writesNs) / 1e6,
                    double(slowestFrameNs) / 1e6);
        undo->undo();
    }

    // The UI stalls for 2 s under the flood: the moves overflow the ring,
    // the notes don't.
    {
        const std::vector<DocNote> before = doc.notesForTrack(track);
        const int undoBefore = undo->index();
        const uint32_t droppedBefore = m_audio.captureDropped();
        m_recordAction->trigger();
        pumpMs(20);
        synchronizePlayhead();
        int notes = 0;
        for (int ms = 0; ms < 2000; ms++)
            floodMs(ms, &notes);
        // A bend now, with the ring still full of moves: dropped, so not
        // moved and not latched either (the take never records it), while
        // the flooded knobs, captured before the ring filled, are.
        const uint32_t droppedBeforeBend = m_audio.captureDropped();
        send(0xE0, 0x00, 0x60);
        pumpMs(1);
        const LaneLatch latch = m_audio.liveLatchForTest();
        check(m_audio.captureDropped() > droppedBeforeBend,
              "flood: the bend into the full ring was not dropped");
        check(latch.track == track && latch.lanes.hasCc(0x07),
              "flood: the flooded knobs did not latch");
        check(!latch.lanes.bend, "flood: a move the full ring dropped latched its lane");
        synchronizePlayhead();
        const size_t taken = m_take.notes().size();
        pumpMs(20);
        m_stopAction->trigger();
        check(m_audio.captureDropped() > droppedBefore,
              "flood: a stalled UI under the flood did not overflow the ring");
        check(taken == 20 && undo->index() == undoBefore + 1 && countNew(before) == 20,
              "flood: notes played while the ring overflowed with moves were lost");
        check(statusBar()->currentMessage().contains(QStringLiteral("MIDI input was lost")),
              "flood: the overflow was not reported");
        undo->undo();
    }

    m_audio.seek(0);
    pumpMs(10);
    midi.setRoute(0x15, MidiRoutes::kNone);
    m_loopAction->setChecked(loopWas);
}

int runRecordCheck(const QString &projectRoot, const QString &songLabel)
{
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "reccheck: no temp dir for settings\n");
        return 1;
    }
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QStandardPaths::setTestModeEnabled(true);

    checkTakeRules();
    {
        MainWindow window;
        window.runRecordWindowCheck(projectRoot, songLabel, settingsDir.path());
    }

    if (g_failures == 0)
        std::printf("reccheck: OK\n");
    return g_failures ? 1 : 0;
}
