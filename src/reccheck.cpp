#include <QAction>
#include <QApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>
#include <QTemporaryDir>
#include <QUndoStack>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <vector>

#include "audio/midiinput.h"
#include "core/miditimeline.h"
#include "core/recordingtake.h"
#include "mainwindow.h"
#include "ui/keymap.h"
#include "ui/songview.h"

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
// same-(tick, key) and overlapping notes, unmatched note-offs, cuts, a lost
// Boundary, another arm's events.
//
// Then Record end to end in the window, with the audio device parked and the
// real callback pumped (AudioEngine::holdDeviceForTest/pumpForTest), every
// message injected through MidiInput::injectForTest: a fresh start records
// from the edit cursor and Stop commits one undo step that undo removes; an
// empty take pushes nothing; punch in and out while playing keeps playing,
// ignores a key held at punch-in and clamps the offset at punch-in; the
// target stays locked through a selection change; the sustain pedal extends
// a note; Fixed velocity is recorded; Play incoming notes off still records;
// Auto takes the device latency; a loop wrap, a seek and a 2-pass overdub;
// a document edit mid-take; the capture ring overflowing; the song ending
// by itself; a tab switch committing to the take's own tab; Record disabled
// on a read-only bundle tab.

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

// Feeds one arm's events into a take, the way the audio thread stamps them.
struct Feed {
    RecordingTake &take;
    uint32_t generation;
    uint32_t segment = 0;

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
        take.push(ev);
    }
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
        if (!check(sameNotes(notes, {{10, 60, 3, 100},
                                     {40, 64, 2, 100},
                                     {48, 65, 3, 100},
                                     {88, 62, 12, 100}}),
                   "take: the latency offset was not applied or not clamped"))
            printNotes("got", notes);
    }
    // Later passes win: a repeated (tick, key) keeps the last; overlaps trim
    // the earlier note's head or tail, or remove it when covered.
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
        // Played 50 ms after the start.
        const uint64_t intended = tickOf(sampleOf(cursor) + uint64_t(rate * 0.05));
        check(n60 && (n60->tick > intended ? n60->tick - intended : intended - n60->tick) <= slop,
              "record: a played note is more than one period off its time");
        undo->undo();
        check(noteList(a) == before, "record: undo did not remove every recorded note");
        send(0x80, 62, 0);
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
        check(doc.moveTrack(a, b), "record: the track move mid-take failed");
        const int moved = m_take.track();
        check(recording() && moved != a, "record: the take did not follow its track's move");
        check(m_audio.liveTarget() == moved, "record: the sound did not follow the take's track");
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
        const int undoAtDelete = undo->index();
        doc.deleteTrack(a);
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

    // The capture ring overflowing: the loss is counted, and the take still
    // commits sane notes.
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
        m_stopAction->trigger();
        bool sane = undo->index() == undoBefore + 1;
        for (const DocNote &n : doc.notesForTrack(a))
            sane &= n.duration >= 1;
        check(sane, "record: after capture drops the take did not commit sane notes");
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
        send(0x90, 60, 100);
        for (int i = 0; i < 1000 && m_audio.transport() == Transport::Playing; i++)
            pumpMs(20);
        check(m_audio.transport() == Transport::Stopped, "record: the song did not end by itself");
        synchronizePlayhead();
        check(!recording() && undo->index() == undoBefore + 1,
              "record: the song ending by itself did not commit the take");
        send(0x80, 60, 0);
        pumpMs(20);
        undo->undo();
        m_loopAction->setChecked(true);
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

    m_audio.holdDeviceForTest(false);
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
