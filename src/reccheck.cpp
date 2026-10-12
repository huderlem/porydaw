#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMessageBox>
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
#include <initializer_list>
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
// mod moves as lane writes, each ending at its segment's end (a Boundary, a
// lost one, the stop) with the song's value restored there.
//
// Then Record end to end in the window, with the audio device parked and the
// real callback pumped (AudioEngine::holdDeviceForTest/pumpForTest), every
// message injected through MidiInput::injectForTest: a fresh start records
// from the edit cursor and Stop commits one undo step that undo removes; R
// in the roll toggles Record, which has no window shortcut; Pause commits;
// a take ended inside an open edit group lands after it as its own step; a
// take's pitch bend and mod wheel land on its BEND and MOD lanes, the song's
// value back at the stop (a take of moves alone too, and Reset All
// Controllers' centred bend and zeroed mod); a
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
// Record disabled on a read-only bundle tab.

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
    void control(uint8_t kind, uint64_t pos, int16_t value)
    {
        CaptureEvent ev;
        ev.kind = kind;
        ev.samplePos = pos;
        ev.generation = generation;
        ev.segment = segment;
        ev.value = value;
        ev.lateSamples = late;
        take.push(ev);
    }
    void bend(uint64_t pos, int16_t value) { control(CaptureEvent::Bend, pos, value); }
    void mod(uint64_t pos, int16_t value) { control(CaptureEvent::Mod, pos, value); }
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
        take.clear();
        check(take.controls().empty(), "take: clear() kept the moves");
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
    if (!midiInput().openPorts().isEmpty()) {
        std::printf("reccheck: no-keyboard checks SKIP (a real MIDI port is open)\n");
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
        // What the lane held at tick, before the take (0 with no point).
        const auto valueAt = [](const std::vector<std::pair<uint64_t, int>> &points,
                                uint64_t tick) {
            int value = 0;
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
            const int song = valueAt(before, end);
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

        // Reset All Controllers (CC121) centres the bend and zeroes the mod,
        // and the take records both.
        const auto bendReset = lane(DOC_CC_BEND);
        const auto modReset = lane(0x01);
        const int undoReset = undo->index();
        m_recordAction->trigger();
        pumpMs(20);
        const uint64_t pUp = playhead();
        send(0xE0, 0x00, 0x60); // +4096
        pumpMs(100);
        const uint64_t pReset = playhead();
        send(0xB0, 121, 0);
        pumpMs(50);
        const uint64_t pEnd = playhead();
        m_stopAction->trigger();
        check(undo->index() == undoReset + 1 &&
                  lastUndoText() == QStringLiteral("Record pitch bend and mod"),
              "record: Reset All Controllers did not record a bend and a mod");
        check(sorted(lane(DOC_CC_BEND)) == expected(DOC_CC_BEND, bendReset,
                                                    {{tickOf(pUp), 4096}, {tickOf(pReset), 0}},
                                                    tickOf(pEnd)),
              "record: Reset All Controllers did not centre the recorded bend");
        check(sorted(lane(0x01)) == expected(0x01, modReset, {{tickOf(pReset), 0}}, tickOf(pEnd)),
              "record: Reset All Controllers did not zero the recorded mod");
        undo->undo();
    }

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
    m_audio.holdDeviceForTest(false);
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
    m_followPlayheadAction->setChecked(followWas);
}

// The take's bend and mod ghosts (the lanes' half of plan step 4): the
// BEND row shows the take's curve as a commit would leave it while the take
// runs, its row appearing for the take when the track had none; the
// rewritten stretch is painted in the track's colour at the ghost notes'
// alpha and turns solid at Stop; a row the take alone showed goes with the
// take (undo, or a dropped take).
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
    const bool lanesWere = view->automationLanesVisible();
    view->setAutomationLanesVisible(true);
    view->selectTrack(track);
    // No bend on the track, so its row is the take's alone.
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

    // Stop: the ghost turns into the real curve, painted solid where it was.
    send(0xE0, 0x00, 0x40);
    pumpMs(30);
    m_stopAction->trigger();
    check(view->recordingPreview().empty() && undo->index() == undoTake + 1 &&
              view->model().findLane(track, LANE_CC_BEND) != nullptr,
          "lane display: after Stop the ghost stayed, or the bend did not land");
    if (at) {
        const QImage committed = lanes->grab().toImage();
        check(pixel(committed, probe) == color,
              "lane display: after Stop the bend is not painted solid where its ghost was");
    }
    undo->undo();
    check(!view->model().findLane(track, LANE_CC_BEND),
          "lane display: undoing the take left its BEND row");

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
    view->setAutomationLanesVisible(lanesWere);
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
