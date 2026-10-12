#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <iterator>
#include <thread>
#include <vector>

#include "audio/midiinput.h"
#include "audio/midiroutes.h"
#include "mainwindow.h"
#include "ui/midisettingspage.h"
#include "ui/settingsdialog.h"
#include "ui/songview.h"

// --midicheck <projectRoot> <songLabel> [shot.png]: MIDI keyboard input
// (docs/midi-input/PLAN.md). No MIDI hardware is ever assumed — WSL and CI
// have no MIDI system at all — so every message enters through
// MidiInput::injectForTest, which takes the same path as a port's RtMidi
// callback. QSettings is redirected into a temp dir first.
//
// With a shot path, the MIDI page (with test ports) is saved as
// <shot>-page.png.
//
// Step 1 covers: the parse table (a velocity-0 note on is a note off;
// aftertouch and system messages are dropped, a program change is kept),
// the input
// ring (FIFO, overflow counted, several producers at once against a
// concurrent consumer), port-list plumbing on whatever backend this machine
// has (without asserting that any device exists), the injector reaching the
// ring and the throttled activity signal, the Settings → MIDI page
// persisting its controls and port toggles, and the window's Settings
// section with a song open. Port identities (the stable names that
// survive hot-plug renumbering, every backend's spelling) are checked through
// the pure helpers, and the no-backend page's Try Again through a simulated
// backend.
//
// Step 2 (live play) parks the audio device and drives the real callback
// itself (AudioEngine::pumpForTest), so time is simulated and every check is
// deterministic: held keys sound on the selected track and outlive the
// band-sweep's timed cap, note-off and the sustain pedal release them, fixed
// velocity applies, a selection change releases and follows, bend and mod
// act on the whole target track and are re-chased when it is left, held
// notes win steal pressure from timed previews, a cut clears the published
// held mask and a key released after it sends no stray note-off, and live
// play off / no target / panic drain silently. Panic, CC123 and CC121 lift
// the sustain pedal, so a lost pedal-up can't defer every later note-off.
//
// docs/midi-controls/PLAN.md step 1 adds the route table (defaults,
// reserved-CC refusal, move-on-conflict, both encodings), its persistence
// in midi/routes, Learn through the injector (first assignable CC, swallowed
// from live play while armed, cancel, a shortened timeout), the Settings
// page's Controls table (spinners skip reserved and held CCs, a typed held
// CC moves at Enter, the wheel needs focus, Learn, a learn started elsewhere
// shows, second click and Esc cancel inside a dialog that stays open), and MOD following its routed CC live and into the capture
// ring while the CC it left does nothing.
//
// Its step 2 covers live play of every routed lane (VOL, PAN and LFOS at
// the knob's value, BENDR scaled to 0..24, on the target track only), a
// program change switching the target's voice, one past the voicegroup's
// last voice ignored and reported once, CC121 resetting only the routed
// lanes (and bend) and never the voice, and a target change handing every
// touched lane and the voice back to the song. The handback's "untouched
// lanes stay untouched" half is --primecheck's (TimelinePlayer::chaseTouched
// on a synthetic song).

namespace {

int g_failures = 0;

bool check(bool ok, const char *what)
{
    if (!ok) {
        std::fprintf(stderr, "midicheck: FAIL: %s\n", what);
        g_failures++;
    }
    return ok;
}

void wait(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

bool same(const MidiMsg &a, const MidiMsg &b)
{
    return a.status == b.status && a.d1 == b.d1 && a.d2 == b.d2;
}

bool parsed(std::vector<unsigned char> bytes, MidiMsg expect)
{
    MidiMsg out;
    return MidiInput::parse(bytes.data(), bytes.size(), &out) && same(out, expect);
}

bool rejected(std::vector<unsigned char> bytes)
{
    MidiMsg out;
    return !MidiInput::parse(bytes.data(), bytes.size(), &out);
}

void drain(MidiInputRing &ring)
{
    MidiMsg msg;
    while (ring.pop(&msg)) {
    }
}

void checkParseTable()
{
    check(parsed({0x90, 60, 100}, {0x90, 60, 100}), "note on did not parse as itself");
    check(parsed({0x93, 60, 0}, {0x83, 60, 0}),
          "a velocity-0 note on did not become a note off on its channel");
    check(parsed({0x80, 60, 64}, {0x80, 60, 64}), "note off did not parse as itself");
    check(parsed({0xB2, 64, 127}, {0xB2, 64, 127}), "control change did not parse as itself");
    check(parsed({0xE0, 0x00, 0x40}, {0xE0, 0x00, 0x40}), "pitch bend did not parse as itself");
    check(parsed({0x90, 0xBC, 0xE4}, {0x90, 0x3C, 0x64}), "data bytes were not masked to 7 bits");
    check(rejected({0xA0, 60, 10}), "polyphonic aftertouch was not dropped");
    check(rejected({0xD0, 10}), "channel aftertouch was not dropped");
    check(parsed({0xC5, 0x85}, {0xC5, 0x05, 0}) && parsed({0xC0, 5, 9}, {0xC0, 5, 0}),
          "a program change did not parse with its one (masked) data byte");
    check(rejected({0xC0}) && rejected({0xB0, 7}), "a truncated message was accepted");
    check(rejected({0xF8}) && rejected({0xFE}) && rejected({0xF0, 0x7E, 0xF7}),
          "clock, active sensing or sysex was not dropped");
    check(rejected({0x90, 60}) && rejected({}), "a truncated message was accepted");
    check(rejected({0x3C, 0x40, 0x00}), "a message without a status byte was accepted");
}

void checkRing()
{
    MidiInputRing ring;
    MidiMsg msg;
    check(!ring.pop(&msg) && ring.size() == 0, "a new ring was not empty");
    for (uint32_t i = 0; i < MidiInputRing::kCapacity; ++i)
        ring.push({0x90, uint8_t(i & 0x7F), uint8_t(i >> 7)});
    check(ring.size() == MidiInputRing::kCapacity && ring.dropped() == 0,
          "the ring did not hold its full capacity");
    check(!ring.push({0x80, 1, 0}) && ring.dropped() == 1,
          "a push into a full ring was not dropped and counted");
    bool fifo = true;
    for (uint32_t i = 0; i < MidiInputRing::kCapacity; ++i)
        fifo &= ring.pop(&msg) && msg.d1 == (i & 0x7F) && msg.d2 == (i >> 7);
    check(fifo && !ring.pop(&msg), "the ring did not return messages in order");
    check(ring.push({0x80, 2, 0}) && ring.pop(&msg) && msg.d1 == 2,
          "the ring did not accept messages again after draining");

    // Several producers (one per port, plus the injector) against one
    // consumer, as on the audio thread: nothing is lost or duplicated —
    // every message is either popped or counted as dropped — and each
    // producer's messages keep their order.
    MidiInputRing shared;
    constexpr int kProducers = 4;
    constexpr int kEach = 16000; // fits the 14 bits two data bytes carry
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&shared, p] {
            for (int i = 0; i < kEach; ++i)
                shared.push({uint8_t(0x90 | p), uint8_t(i & 0x7F), uint8_t((i >> 7) & 0x7F)});
        });
    }
    long popped = 0;
    bool ordered = true;
    int last[kProducers];
    std::fill(std::begin(last), std::end(last), -1);
    std::atomic<bool> done{false};
    std::thread consumer([&] {
        MidiMsg m;
        for (;;) {
            const bool finished = done.load();
            while (shared.pop(&m)) {
                const int p = m.status & 0x0F;
                const int i = m.d1 | (m.d2 << 7);
                if (p >= kProducers || i <= last[p])
                    ordered = false;
                else
                    last[p] = i;
                ++popped;
            }
            if (finished)
                break;
            std::this_thread::yield();
        }
    });
    for (auto &t : producers)
        t.join();
    done.store(true);
    consumer.join();
    check(popped + long(shared.dropped()) == long(kProducers) * kEach,
          "concurrent producers lost or duplicated messages");
    check(ordered, "concurrent producers' messages arrived out of order");
    std::printf("midicheck: concurrent ring: %ld popped, %llu dropped\n", popped,
                static_cast<unsigned long long>(shared.dropped()));
}

// Port identities (stable across hot-plug renumbering), as the pure
// helpers resolve them; no backend needed, so every platform's naming is
// covered here.
void checkPortIdentities()
{
    using Naming = MidiInput::PortNaming;
    const QString keys = QStringLiteral("Keystation 49");
    // WinMM (built without RtMidi's appended index) and CoreMIDI: the
    // device's own name, digits and all; same-named ports are counted.
    check(MidiInput::portIdentities({keys, QStringLiteral("Other"), keys}, Naming::Plain) ==
              QStringList({keys, QStringLiteral("Other"), keys + QStringLiteral(" (2)")}),
          "same-named plain ports were not numbered by occurrence");
    check(MidiInput::portBaseName(keys, Naming::Plain) == keys,
          "a plain port name lost its own trailing number");
    // A device literally named "X (2)" keeps its name; the counted one moves on.
    check(MidiInput::portIdentities({"X (2)", "X", "X"}, Naming::Plain) ==
              QStringList({"X (2)", "X", "X (3)"}),
          "a counted identity collided with a device's literal name");
    // A port that vanished mid-scan reads back empty and is not counted.
    check(MidiInput::portIdentities({"X", "", "X"}, Naming::Plain) ==
              QStringList({"X", "", "X (2)"}),
          "an empty port name was counted or given an identity");
    // ALSA: RtMidi's " client:port" suffix is dropped, so a replug (new
    // numbers) keeps its identity.
    const QString usb = QStringLiteral("USB Keys:USB Keys MIDI 1");
    check(MidiInput::portIdentities(
              {usb + " 20:0", "Midi Through:Midi Through Port-0 14:0", usb + " 24:0"},
              Naming::Alsa) == QStringList({usb, "Midi Through:Midi Through Port-0", usb + " (2)"}),
          "ALSA port names kept their client:port suffix");
    check(MidiInput::portBaseName(usb + " 28:0", Naming::Alsa) == usb &&
              MidiInput::portBaseName(usb + " 20:0", Naming::Alsa) == usb,
          "a replugged ALSA port changed identity");

    // Whether an open identity still names the device it opened.
    check(!MidiInput::identityStable({"K", "K"}, {"K"}, "K", Naming::Plain) &&
              !MidiInput::identityStable({"K", "K"}, {"K"}, "K (2)", Naming::Plain),
          "unplugging one of two identical ports left an identity thought stable");
    check(MidiInput::identityStable({"K", "Other"}, {"K"}, "K", Naming::Plain) &&
              MidiInput::identityStable({"K"}, {"Other", "K"}, "K", Naming::Plain),
          "another device coming or going unsettled a port");
    check(!MidiInput::identityStable({"K", "Other"}, {"Other"}, "K", Naming::Plain),
          "an unplugged port was thought stable");
    check(!MidiInput::identityStable({usb + " 20:0"}, {usb + " 24:0"}, usb, Naming::Alsa),
          "an ALSA port replugged under new numbers was thought stable");
}

void checkService()
{
    MidiInput midi;
    // Port plumbing on whatever this machine has; no device is assumed.
    check(midi.backendAvailable() == midi.backendStatus().isEmpty(),
          "backend availability and its status sentence disagree");
    if (midi.backendAvailable()) {
        check(!midi.backendName().isEmpty(), "a live backend has no name");
        bool consistent = true;
        for (const QString &name : midi.openPorts())
            consistent &= midi.ports().contains(name);
        for (const QString &name : midi.ports())
            consistent &= midi.isPortOpen(name) || midi.portFailed(name);
        check(consistent, "open ports and enabled listed ports disagree");
    } else {
        check(midi.ports().isEmpty() && midi.openPorts().isEmpty(),
              "ports listed without a backend");
    }
    midi.refresh(); // must not throw or crash, backend or not
    std::printf("midicheck: backend %s; %d port(s), %d open\n",
                midi.backendAvailable() ? qUtf8Printable(midi.backendName())
                                        : qUtf8Printable(midi.backendStatus()),
                int(midi.ports().size()), int(midi.openPorts().size()));

    // Enable state is stored by name; unknown and unopenable names are safe.
    const QString fake = QStringLiteral("midicheck fake port 9:0");
    check(midi.isPortEnabled(fake), "an unseen port did not default to enabled");
    midi.setPortEnabled(fake, false);
    check(!midi.isPortEnabled(fake) &&
              QSettings().value(QStringLiteral("midi/disabledPorts")).toStringList().contains(fake),
          "disabling a port did not persist it in midi/disabledPorts");
    midi.setPortEnabled(fake, true);
    check(midi.isPortEnabled(fake) && !QSettings().contains(QStringLiteral("midi/disabledPorts")),
          "re-enabling the only disabled port did not clear midi/disabledPorts");
    check(!midi.openPort(fake) && !midi.isPortOpen(fake), "a port that isn't listed opened");

    // The injector reaches the ring through the parse path.
    MidiInputRing &ring = midi.ring();
    drain(ring);
    int activity = 0;
    QObject::connect(&midi, &MidiInput::activity, &midi, [&activity] { ++activity; });
    const uint64_t received = midi.receivedCount();
    midi.injectForTest({0x90, 60, 100});
    midi.injectForTest({0x91, 61, 0});
    midi.injectForTest({0xD0, 40, 0}); // channel aftertouch: dropped before the ring
    MidiMsg msg;
    check(ring.size() == 2 && midi.receivedCount() == received + 2,
          "the injector did not put exactly the two kept messages in the ring");
    check(ring.pop(&msg) && same(msg, {0x90, 60, 100}), "the injected note on was not first");
    check(ring.pop(&msg) && same(msg, {0x81, 61, 0}),
          "the injected velocity-0 note on did not arrive as a note off");
    check(activity == 0, "activity was signalled synchronously, not on the UI hop");
    wait(MidiInput::kActivityIntervalMs * 2);
    check(activity == 1, "injected input did not signal activity once");

    // A burst is coalesced into one signal, and input right after a signal
    // waits out the interval: one signal per interval, not per message.
    activity = 0;
    for (int i = 0; i < 100; ++i) {
        midi.injectForTest({0x90, uint8_t(i), 100});
        drain(ring);
    }
    QApplication::processEvents();
    check(activity == 1, "a burst of input was not coalesced into one activity signal");
    midi.injectForTest({0x80, 1, 0});
    drain(ring);
    QApplication::processEvents();
    check(activity == 1, "input right after an activity signal was not held back");
    wait(MidiInput::kActivityIntervalMs * 3);
    check(activity == 2, "held-back input did not signal activity after the interval");

    // Overflow: nobody drains (the audio thread is the consumer from step
    // 2), so past the capacity every message is dropped and counted.
    drain(ring);
    const uint64_t droppedBefore = ring.dropped();
    const uint64_t receivedBefore = midi.receivedCount();
    for (int i = 0; i < 300; ++i)
        midi.injectForTest({0x90, uint8_t(i & 0x7F), 100});
    check(ring.size() == MidiInputRing::kCapacity, "the ring did not fill to capacity");
    check(ring.dropped() == droppedBefore + (300 - MidiInputRing::kCapacity),
          "overflowing the ring did not count every dropped message");
    check(midi.receivedCount() == receivedBefore + MidiInputRing::kCapacity,
          "dropped messages were counted as received");
    drain(ring);
}

// docs/midi-controls/PLAN.md step 1: the route table on its own.
void checkRoutesTable()
{
    const MidiRoutes defaults;
    check(defaults.ccFor(0x01) == 1 && defaults.ccFor(0x07) == 7 && defaults.ccFor(0x0A) == 10 &&
              defaults.ccFor(0x14) == MidiRoutes::kNone &&
              defaults.ccFor(0x15) == MidiRoutes::kNone,
          "routes: the defaults are not CC1 MOD, CC7 VOL, CC10 PAN, BENDR and LFOS none");
    check(defaults.laneFor(1) == 0x01 && defaults.laneFor(10) == 0x0A &&
              defaults.laneFor(74) == MidiRoutes::kNone &&
              defaults.laneFor(-1) == MidiRoutes::kNone,
          "routes: laneFor does not invert the defaults");
    bool reservedOk = true;
    for (int cc = -1; cc <= 128; cc++) {
        const bool reserved = cc < 0 || cc > 127 || cc == 0 || cc == 32 || cc == 6 || cc == 38 ||
                              cc == 64 || cc >= 120;
        reservedOk &= MidiRoutes::assignable(cc) == !reserved;
    }
    check(reservedOk, "routes: assignable() does not refuse exactly CC0/6/32/38/64/120-127");

    // Reserved CCs and lanes that aren't routable are refused unchanged.
    MidiRoutes routes;
    int moved = 99;
    check(!routes.assign(0x01, 64, &moved) && !routes.assign(0x07, 121) &&
              !routes.assign(0x0A, 0) && !routes.assign(0x14, 6) && !routes.assign(0x15, 38) &&
              !routes.assign(0x02, 20) && routes == MidiRoutes() && moved == MidiRoutes::kNone,
          "routes: a reserved CC or an unroutable lane was assigned");
    // One CC drives one lane: assigning a taken CC moves it.
    check(routes.assign(0x14, 7, &moved) && moved == 0x07 && routes.ccFor(0x14) == 7 &&
              routes.ccFor(0x07) == MidiRoutes::kNone && routes.laneFor(7) == 0x14,
          "routes: assigning VOL's CC to BENDR did not move it (VOL to none)");
    check(routes.assign(0x01, 74, &moved) && moved == MidiRoutes::kNone &&
              routes.ccFor(0x01) == 74 && routes.laneFor(1) == MidiRoutes::kNone,
          "routes: remapping MOD to CC74 did not free CC1");
    check(routes.assign(0x0A, MidiRoutes::kNone) && routes.ccFor(0x0A) == MidiRoutes::kNone,
          "routes: a lane could not be set to none");

    // The packed form decodes, through the audio thread's own unpacker, to
    // exactly laneFor; a malformed word (reserved CCs, a CC twice) routes
    // nothing reserved and keeps the first lane naming a CC.
    const auto unpacksTo = [](uint64_t packed, const MidiRoutes &expect) {
        uint8_t laneOf[128];
        std::fill(std::begin(laneOf), std::end(laneOf), uint8_t(0x42));
        MidiRoutes::unpackLaneTable(packed, laneOf);
        bool ok = true;
        for (int cc = 0; cc < 128; cc++) {
            const int lane = expect.laneFor(cc);
            ok &= laneOf[cc] == (lane == MidiRoutes::kNone ? 0xFF : lane);
        }
        return ok;
    };
    check(unpacksTo(routes.packed(), routes) && unpacksTo(defaults.packed(), defaults),
          "routes: the packed form does not unpack to the same lanes");
    {
        // MOD=CC64 (reserved), VOL=CC20, PAN=CC20 again, BENDR=CC121,
        // LFOS=CC0; the unused top bytes are 0xFF.
        const uint64_t malformed = 0xFFFFFF0079141440ull;
        MidiRoutes expect;
        for (int lane : MidiRoutes::kLanes)
            expect.assign(lane, MidiRoutes::kNone);
        expect.assign(0x07, 20);
        check(unpacksTo(malformed, expect),
              "routes: a malformed packed word routed a reserved or repeated CC");
    }
    check(MidiRoutes::fromStrings(routes.toStrings()) == routes &&
              MidiRoutes::fromStrings(defaults.toStrings()) == defaults,
          "routes: the settings form does not round-trip");
    check(defaults.toStrings() ==
              QStringList({"MOD=1", "VOL=7", "PAN=10", "BENDR=none", "LFOS=none"}),
          "routes: the settings form is not NAME=cc per lane");
    // Hand-edited settings: unknown names, reserved and repeated CCs read
    // as none; an unlisted lane keeps its default unless its CC is taken.
    const MidiRoutes edited = MidiRoutes::fromStrings(
        {"vol=64", "PAN=1", "LFOS=1", "BENDR=x", "TEMPO=3", "garbage", "MOD=200"});
    check(edited.ccFor(0x07) == MidiRoutes::kNone && edited.ccFor(0x0A) == 1 &&
              edited.ccFor(0x15) == MidiRoutes::kNone && edited.ccFor(0x14) == MidiRoutes::kNone &&
              edited.ccFor(0x01) == MidiRoutes::kNone,
          "routes: hand-edited settings were not read defensively");
    const MidiRoutes partial = MidiRoutes::fromStrings({"PAN=1"});
    check(partial.ccFor(0x0A) == 1 && partial.ccFor(0x07) == 7 &&
              partial.ccFor(0x01) == MidiRoutes::kNone,
          "routes: an unlisted lane did not keep its default (or kept a taken CC)");
}

// Routes and Learn through the service (QSettings and the injector).
void checkRoutesService()
{
    MidiInput midi;
    const QString key = QStringLiteral("midi/routes");
    check(midi.routes() == MidiRoutes() && !QSettings().contains(key),
          "routes: a fresh MidiInput does not start on the defaults with no midi/routes");
    int changed = 0;
    QObject::connect(&midi, &MidiInput::settingsChanged, &midi, [&changed] { ++changed; });

    // Reserved CCs are refused without a write or a signal.
    check(!midi.setRoute(0x01, 64) && !midi.setRoute(0x07, 123) && !midi.setRoute(0x0A, 32) &&
              midi.routes() == MidiRoutes() && changed == 0 && !QSettings().contains(key),
          "routes: the service accepted a reserved CC");
    check(midi.setRoute(0x01, 74) && midi.routes().ccFor(0x01) == 74 && changed == 1 &&
              MidiRoutes::fromStrings(QSettings().value(key).toStringList()) == midi.routes(),
          "routes: remapping MOD did not apply, signal and persist");
    check(midi.setRoute(0x15, 7) && midi.routes().ccFor(0x07) == MidiRoutes::kNone &&
              midi.routes().ccFor(0x15) == 7,
          "routes: the service did not move a taken CC");
    {
        MidiInput reread;
        check(reread.routes() == midi.routes(), "routes: a new MidiInput did not read them back");
    }
    // Back to the defaults clears the key.
    midi.setRoute(0x15, MidiRoutes::kNone);
    midi.setRoute(0x07, 7);
    midi.setRoute(0x01, 1);
    check(midi.routes() == MidiRoutes() && !QSettings().contains(key),
          "routes: returning to the defaults did not remove midi/routes");

    MidiInputRing &ring = midi.ring();
    drain(ring);
    std::vector<std::pair<int, int>> learned; // lane, cc
    int movedFrom = -2;
    std::vector<std::pair<int, bool>> cancelled; // lane, timed out
    QObject::connect(&midi, &MidiInput::learnFinished, &midi, [&](int lane, int cc, int moved) {
        learned.push_back({lane, cc});
        movedFrom = moved;
    });
    QObject::connect(&midi, &MidiInput::learnCancelled, &midi,
                     [&](int lane, bool timedOut) { cancelled.push_back({lane, timedOut}); });
    const auto settle = [&] { wait(MidiInput::kLearnPollMs * 4); };

    // Learn takes the first assignable CC and swallows it from live play;
    // reserved CCs (sustain, bank select, data entry, mode messages) pass
    // through untouched and aren't learned.
    midi.startLearn(0x07);
    check(midi.learnLane() == 0x07, "learn: startLearn did not arm the lane");
    midi.injectForTest({0xB0, 64, 127});
    midi.injectForTest({0xB0, 0, 1});
    midi.injectForTest({0xB0, 32, 0});
    midi.injectForTest({0xB0, 6, 2});
    midi.injectForTest({0xB0, 38, 0});
    midi.injectForTest({0xB0, 121, 0});
    midi.injectForTest({0x90, 60, 100});
    check(ring.size() == 7, "learn: reserved CCs or a note were kept from live play");
    drain(ring);
    settle();
    check(learned.empty() && midi.learnLane() == 0x07, "learn: a reserved CC was learned");
    midi.injectForTest({0xB3, 74, 20}); // any channel
    midi.injectForTest({0xB0, 74, 21});
    midi.injectForTest({0xB0, 75, 22}); // a second knob in the same instant
    check(ring.size() == 0, "learn: the CC being learned reached live play");
    settle();
    check(learned.size() == 1 && learned[0] == std::pair(0x07, 74) &&
              movedFrom == MidiRoutes::kNone && midi.learnLane() == MidiRoutes::kNone &&
              midi.routes().ccFor(0x07) == 74 && midi.routes().laneFor(75) == MidiRoutes::kNone,
          "learn: the first CC was not routed to the lane");
    check(MidiRoutes::fromStrings(QSettings().value(key).toStringList()).ccFor(0x07) == 74,
          "learn: the learned route was not persisted");
    // Learned and disarmed: the knob plays again.
    midi.injectForTest({0xB0, 74, 30});
    check(ring.size() == 1, "learn: after learning, the CC still did not reach live play");
    drain(ring);

    // Learning a CC another lane has moves it there.
    midi.startLearn(0x14);
    midi.injectForTest({0xB0, 10, 64});
    settle();
    check(learned.size() == 2 && learned[1] == std::pair(0x14, 10) && movedFrom == 0x0A &&
              midi.routes().ccFor(0x14) == 10 && midi.routes().ccFor(0x0A) == MidiRoutes::kNone,
          "learn: learning PAN's CC for BENDR did not move it (PAN to none)");

    // Cancel: nothing changes, and the CC plays again.
    const MidiRoutes before = midi.routes();
    midi.startLearn(0x15);
    midi.cancelLearn();
    check(cancelled.size() == 1 && cancelled[0] == std::pair(0x15, false) &&
              midi.learnLane() == MidiRoutes::kNone,
          "learn: cancelLearn did not report a cancel");
    midi.injectForTest({0xB0, 80, 5});
    settle();
    check(ring.size() == 1 && midi.routes() == before && learned.size() == 2,
          "learn: a cancelled learn still swallowed or learned a CC");
    drain(ring);
    // Starting another lane cancels the first.
    midi.startLearn(0x15);
    midi.startLearn(0x0A);
    check(cancelled.size() == 2 && cancelled[1] == std::pair(0x15, false) &&
              midi.learnLane() == 0x0A,
          "learn: a second lane's learn did not cancel the first");
    midi.cancelLearn();
    // Timeout (shortened for the harness).
    cancelled.clear();
    midi.startLearn(0x15, 40);
    wait(200);
    check(cancelled.size() == 1 && cancelled[0] == std::pair(0x15, true) &&
              midi.learnLane() == MidiRoutes::kNone && midi.routes() == before,
          "learn: an unanswered learn did not time out");
    midi.injectForTest({0xB0, 80, 5});
    check(ring.size() == 1, "learn: a timed-out learn still swallowed a CC");
    drain(ring);

    // Back to the defaults for the later checks.
    midi.setRoute(0x14, MidiRoutes::kNone);
    midi.setRoute(0x07, 7);
    midi.setRoute(0x0A, 10);
    check(midi.routes() == MidiRoutes() && !QSettings().contains(key),
          "routes: could not return to the defaults");
}

QString g_shotPath;

// With a shot path, saves widget as <shot>-<name>.png for visual review.
void shoot(QWidget *widget, const char *name)
{
    if (g_shotPath.isEmpty() || !widget)
        return;
    QApplication::processEvents();
    QString path = g_shotPath;
    const int dot = path.lastIndexOf(QLatin1Char('.'));
    const QString suffix = QStringLiteral("-%1").arg(QLatin1String(name));
    path = dot > 0 ? path.left(dot) + suffix + path.mid(dot) : path + suffix;
    widget->grab().save(path);
}

void checkSettingsPage()
{
    {
        MidiInput midi;
        // The controls are built even without a backend, so the plumbing is
        // testable here; the window's real page shows only a label then.
        MidiSettingsPage page(midi, nullptr, /*showWithoutBackend=*/true);
        auto *ports = page.findChild<QListWidget *>(QStringLiteral("settingsMidiPorts"));
        auto *refresh = page.findChild<QPushButton *>(QStringLiteral("settingsMidiRefresh"));
        auto *live = page.findChild<QCheckBox *>(QStringLiteral("settingsMidiLivePlay"));
        auto *velocity = page.findChild<QSpinBox *>(QStringLiteral("settingsMidiFixedVelocity"));
        auto *latency = page.findChild<QSpinBox *>(QStringLiteral("settingsMidiLatency"));
        if (!check(ports && refresh && live && velocity && latency, "MIDI page controls not found"))
            return;
        check(live->isChecked() && velocity->value() == 0 && velocity->text() == "Off" &&
                  latency->value() == MidiInput::kLatencyAuto && latency->text() == "Auto",
              "MIDI page did not start on play-on, velocity Off, offset Auto");
        check(!QSettings().contains(QStringLiteral("midi/livePlay")),
              "building the MIDI page wrote QSettings");

        live->setChecked(false);
        check(!midi.livePlay() && QSettings().contains(QStringLiteral("midi/livePlay")) &&
                  !QSettings().value(QStringLiteral("midi/livePlay")).toBool(),
              "Play incoming notes did not apply and persist");
        velocity->setValue(100);
        check(midi.fixedVelocity() == 100 &&
                  QSettings().value(QStringLiteral("midi/fixedVelocity")).toInt() == 100,
              "Fixed velocity did not apply and persist");
        latency->setValue(25);
        check(midi.latencyMs() == 25 &&
                  QSettings().value(QStringLiteral("midi/latencyMs")).toInt() == 25,
              "Recording offset did not apply and persist");

        // Port toggles, through a test port list (no hardware here).
        midi.setPortListForTest({QStringLiteral("Keys A"), QStringLiteral("Keys B")});
        QApplication::processEvents();
        if (check(ports->count() == 2, "the port list did not show the listed ports")) {
            check(ports->item(0)->checkState() == Qt::Checked &&
                      ports->item(1)->checkState() == Qt::Checked,
                  "new ports were not listed as enabled");
            ports->item(1)->setCheckState(Qt::Unchecked);
            QApplication::processEvents();
            check(!midi.isPortEnabled(QStringLiteral("Keys B")) &&
                      QSettings().value(QStringLiteral("midi/disabledPorts")).toStringList() ==
                          QStringList{QStringLiteral("Keys B")},
                  "unchecking a port did not persist it as disabled");
            check(ports->count() == 2 && ports->item(1)->checkState() == Qt::Unchecked &&
                      ports->item(0)->checkState() == Qt::Checked,
                  "the port list did not keep showing the toggled state");
            page.resize(560, 480);
            shoot(&page, "page");
        }

        // Two identical keyboards are two identities; a toggle persists the
        // one shown, not a position.
        midi.setPortListForTest({QStringLiteral("Twin"), QStringLiteral("Twin")});
        QApplication::processEvents();
        if (check(ports->count() == 2 && ports->item(0)->text() == QStringLiteral("Twin") &&
                      ports->item(1)->text() == QStringLiteral("Twin (2)") &&
                      midi.ports() == QStringList({"Twin", "Twin (2)"}),
                  "two same-named ports were not listed as Name and Name (2)")) {
            ports->item(1)->setCheckState(Qt::Unchecked);
            QApplication::processEvents();
            check(!midi.isPortEnabled(QStringLiteral("Twin (2)")) &&
                      midi.isPortEnabled(QStringLiteral("Twin")) &&
                      QSettings()
                          .value(QStringLiteral("midi/disabledPorts"))
                          .toStringList()
                          .contains(QStringLiteral("Twin (2)")),
                  "disabling the second same-named port did not persist its identity");
            midi.setPortEnabled(QStringLiteral("Twin (2)"), true);
        }
    }

    // Without a backend the window's page explains why and offers to try
    // again; a backend that comes up turns it into the full page.
    {
        MidiInput midi;
        if (!midi.backendAvailable()) {
            MidiSettingsPage page(midi);
            auto *label = page.findChild<QLabel *>(QStringLiteral("settingsMidiUnavailable"));
            auto *retry = page.findChild<QPushButton *>(QStringLiteral("settingsMidiRetry"));
            auto *ports = page.findChild<QListWidget *>(QStringLiteral("settingsMidiPorts"));
            if (check(label && retry && ports, "the no-backend MIDI page's widgets not found")) {
                check(label->isVisibleTo(&page) && retry->isVisibleTo(&page) &&
                          !ports->isVisibleTo(&page) && label->text() == midi.backendStatus(),
                      "without a backend the MIDI page is not the reason and Try Again");
                retry->click();
                QApplication::processEvents();
                check(midi.backendAvailable()
                          ? ports->isVisibleTo(&page)
                          : label->isVisibleTo(&page) && !ports->isVisibleTo(&page),
                      "Try Again did not leave the MIDI page matching the backend");
                if (!midi.backendAvailable()) {
                    midi.setBackendForTest(QStringLiteral("midicheck MIDI"));
                    QApplication::processEvents();
                    check(ports->isVisibleTo(&page) && !label->isVisibleTo(&page),
                          "the MIDI page did not show its controls once the backend came up");
                    midi.setBackendForTest(QString());
                    QApplication::processEvents();
                    check(!ports->isVisibleTo(&page) && label->isVisibleTo(&page) &&
                              !label->text().isEmpty(),
                          "the MIDI page did not return to the reason when the backend left");
                }
            }
        }
    }

    // A relaunch reads every preference back, into the service and the page.
    {
        MidiInput midi;
        MidiSettingsPage page(midi, nullptr, /*showWithoutBackend=*/true);
        auto *live = page.findChild<QCheckBox *>(QStringLiteral("settingsMidiLivePlay"));
        auto *velocity = page.findChild<QSpinBox *>(QStringLiteral("settingsMidiFixedVelocity"));
        auto *latency = page.findChild<QSpinBox *>(QStringLiteral("settingsMidiLatency"));
        check(!midi.livePlay() && midi.fixedVelocity() == 100 && midi.latencyMs() == 25 &&
                  !midi.isPortEnabled(QStringLiteral("Keys B")) &&
                  midi.isPortEnabled(QStringLiteral("Keys A")),
              "a new MidiInput did not load the persisted preferences");
        check(live && velocity && latency && !live->isChecked() && velocity->value() == 100 &&
                  latency->value() == 25,
              "a new MIDI page did not show the persisted preferences");
        // Back to the defaults for the window check.
        live->setChecked(true);
        velocity->setValue(0);
        latency->setValue(MidiInput::kLatencyAuto);
        midi.setPortEnabled(QStringLiteral("Keys B"), true);
        check(midi.livePlay() && midi.fixedVelocity() == 0 &&
                  midi.latencyMs() == MidiInput::kLatencyAuto &&
                  !QSettings().contains(QStringLiteral("midi/disabledPorts")),
              "the MIDI page did not return to the defaults");
    }
}

// The Settings page's Controls table (docs/midi-controls/PLAN.md step 1).
// Keys as a real press arrives: the shortcut map's ShortcutOverride to the
// focus widget first (ignored until someone claims it), then the KeyPress,
// which QApplication propagates up the parents to the dialog. Returns
// whether the override was claimed.
bool pressKey(QWidget *window, int key, const QString &text = QString())
{
    QWidget *focus = QApplication::focusWidget();
    if (!focus || focus->window() != window)
        focus = window;
    QKeyEvent override(QEvent::ShortcutOverride, key, Qt::NoModifier, text);
    override.ignore();
    QApplication::sendEvent(focus, &override);
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, text);
    QApplication::sendEvent(focus, &press);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, text);
    QApplication::sendEvent(focus, &release);
    return override.isAccepted();
}

// Replaces what box shows with digits, typed (not committed: the boxes
// commit on Enter or focus loss).
void typeInto(QSpinBox *box, const QString &digits)
{
    box->setFocus();
    box->selectAll(); // the number, not the "CC " prefix
    for (const QChar c : digits)
        pressKey(box->window(), Qt::Key_0 + (c.unicode() - '0'), QString(c));
}

bool wheelUp(QSpinBox *box)
{
    const QPointF at(box->width() / 2.0, box->height() / 2.0);
    QWheelEvent wheel(at, box->mapToGlobal(at), QPoint(), QPoint(0, 120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(box, &wheel);
    return wheel.isAccepted();
}

void checkControlsPage()
{
    MidiInput midi;
    // In a dialog, as in Settings, so Esc has a dialog to close.
    QDialog dialog;
    auto *pagePtr = new MidiSettingsPage(midi, &dialog, /*showWithoutBackend=*/true);
    MidiSettingsPage &page = *pagePtr;
    auto *dialogLayout = new QVBoxLayout(&dialog);
    dialogLayout->addWidget(pagePtr);
    int rejected = 0;
    QObject::connect(&dialog, &QDialog::rejected, &dialog, [&rejected] { ++rejected; });
    dialog.show();
    dialog.activateWindow();
    QApplication::processEvents();
    std::array<QSpinBox *, MidiRoutes::kLaneCount> cc{};
    std::array<QPushButton *, MidiRoutes::kLaneCount> learn{};
    bool found = true;
    for (int i = 0; i < MidiRoutes::kLaneCount; i++) {
        const QString name = MidiRoutes::laneName(MidiRoutes::kLanes[i]);
        cc[i] = page.findChild<QSpinBox *>(QStringLiteral("settingsMidiRoute") + name);
        learn[i] = page.findChild<QPushButton *>(QStringLiteral("settingsMidiLearn") + name);
        found &= cc[i] && learn[i];
    }
    auto *status = page.findChild<QLabel *>(QStringLiteral("settingsMidiLearnStatus"));
    if (!check(found && status, "controls: the Controls table's rows were not found"))
        return;
    enum { Mod, Vol, Pan, BendR, Lfos };
    check(cc[Mod]->value() == 1 && cc[Vol]->value() == 7 && cc[Pan]->value() == 10 &&
              cc[BendR]->value() == MidiRoutes::kNone && cc[BendR]->text() == "None" &&
              cc[Lfos]->value() == MidiRoutes::kNone && cc[Mod]->text() == "CC 1",
          "controls: the table did not show the default routes");
    check(!QSettings().contains(QStringLiteral("midi/routes")),
          "controls: building the table wrote QSettings");

    // A spinner applies and persists; the arrows step over reserved CCs.
    cc[Lfos]->setValue(21);
    check(midi.routes().ccFor(0x15) == 21 && QSettings().contains(QStringLiteral("midi/routes")),
          "controls: the LFOS spinner did not apply and persist");
    cc[Lfos]->setValue(63);
    cc[Lfos]->stepBy(1);
    check(cc[Lfos]->value() == 65 && midi.routes().ccFor(0x15) == 65,
          "controls: stepping up from CC63 did not skip the sustain pedal");
    // ...and over the CCs other lanes hold, so stepping never moves a route.
    cc[Lfos]->setValue(MidiRoutes::kNone);
    cc[Lfos]->stepBy(1);
    check(cc[Lfos]->value() == 2 && midi.routes().ccFor(0x15) == 2 &&
              midi.routes().ccFor(0x01) == 1 && cc[Mod]->value() == 1,
          "controls: stepping up from None did not skip CC0 and MOD's CC1");
    cc[Lfos]->stepBy(-1);
    check(cc[Lfos]->value() == MidiRoutes::kNone && midi.routes().ccFor(0x01) == 1,
          "controls: stepping down from CC2 did not skip MOD's CC1 back to None");
    cc[Vol]->stepBy(5); // 8, 9, (PAN's 10), 11, 12, 13
    check(cc[Vol]->value() == 13 && midi.routes().ccFor(0x07) == 13 &&
              midi.routes().ccFor(0x0A) == 10 && cc[Pan]->value() == 10,
          "controls: stepping VOL past PAN's CC10 moved PAN");
    cc[Vol]->setValue(7);
    cc[Lfos]->setValue(119);
    cc[Lfos]->stepBy(1);
    check(cc[Lfos]->value() == 119 && cc[Lfos]->maximum() == 119,
          "controls: the spinner reached the channel mode messages");
    // A typed reserved CC never commits.
    QString typed = QStringLiteral("CC 64");
    int pos = typed.size();
    check(static_cast<QAbstractSpinBox *>(cc[Lfos])->validate(typed, pos) != QValidator::Acceptable,
          "controls: typing CC64 was accepted");
    typed = QStringLiteral("CC 65");
    pos = typed.size();
    check(static_cast<QAbstractSpinBox *>(cc[Lfos])->validate(typed, pos) == QValidator::Acceptable,
          "controls: typing an assignable CC was refused");
    // A reserved value set from code is refused by the service and shown back.
    cc[Vol]->setValue(6);
    check(midi.routes().ccFor(0x07) == 7 && cc[Vol]->value() == 7,
          "controls: a reserved CC set on the spinner was not refused");
    cc[Lfos]->setValue(MidiRoutes::kNone);

    // A typed CC another lane holds applies at Enter, moves that lane, and
    // says so the way Learn does.
    typeInto(cc[Lfos], QStringLiteral("10"));
    check(cc[Lfos]->hasFocus() && midi.routes().ccFor(0x15) == MidiRoutes::kNone &&
              midi.routes().ccFor(0x0A) == 10,
          "controls: a typed CC applied before Enter");
    pressKey(&dialog, Qt::Key_Return);
    check(cc[Lfos]->value() == 10 && midi.routes().ccFor(0x15) == 10 &&
              midi.routes().ccFor(0x0A) == MidiRoutes::kNone &&
              cc[Pan]->value() == MidiRoutes::kNone && rejected == 0 && dialog.isVisible() &&
              status->text() == QStringLiteral("CC 10 now drives LFOS instead of PAN."),
          "controls: a typed held CC did not move at Enter and say so");
    cc[Lfos]->setValue(MidiRoutes::kNone);
    cc[Pan]->setValue(10);
    check(status->text().isEmpty(), "controls: a later route change left a stale status line");
    // Typed, then an arrow: the step starts from the typed number, as in Qt
    // (50, Up: 51), and the typed number isn't committed on its own (PAN's
    // 10, Up: 11 with PAN untouched).
    typeInto(cc[Lfos], QStringLiteral("50"));
    pressKey(&dialog, Qt::Key_Up);
    check(cc[Lfos]->value() == 51 && midi.routes().ccFor(0x15) == 51,
          "controls: typing 50 then Up did not step to 51");
    typeInto(cc[Lfos], QStringLiteral("10"));
    pressKey(&dialog, Qt::Key_Up);
    check(cc[Lfos]->value() == 11 && midi.routes().ccFor(0x15) == 11 &&
              midi.routes().ccFor(0x0A) == 10 && status->text().isEmpty(),
          "controls: typing PAN's CC10 then Up moved PAN, or did not step to 11");
    // The wheel steps only a focused box (and skips held CCs too).
    cc[Lfos]->setValue(9);
    learn[Lfos]->setFocus();
    const bool unfocusedTaken = wheelUp(cc[Lfos]);
    check(!cc[Lfos]->hasFocus() && !unfocusedTaken && cc[Lfos]->value() == 9 &&
              midi.routes().ccFor(0x15) == 9,
          "controls: the wheel stepped an unfocused CC box");
    cc[Lfos]->setFocus();
    wheelUp(cc[Lfos]);
    check(cc[Lfos]->value() == 11 && midi.routes().ccFor(0x0A) == 10,
          "controls: the wheel over a focused box did not step past PAN's CC10");
    cc[Lfos]->setValue(MidiRoutes::kNone);
    check(midi.routes() == MidiRoutes(), "controls: the stepping checks left a route behind");

    // Learn: the button listens, the injected knob lands in its row.
    learn[Pan]->click();
    check(midi.learnLane() == 0x0A && learn[Pan]->isChecked() && !learn[Vol]->isChecked() &&
              status->text().contains(QStringLiteral("Esc")),
          "controls: Learn did not arm PAN and say so");
    midi.injectForTest({0xB0, 7, 99}); // VOL's knob: moves to PAN
    wait(MidiInput::kLearnPollMs * 4);
    check(midi.learnLane() == MidiRoutes::kNone && !learn[Pan]->isChecked() &&
              cc[Pan]->value() == 7 && cc[Vol]->value() == MidiRoutes::kNone &&
              status->text().contains(QStringLiteral("VOL")),
          "controls: a learned CC did not move into the PAN row (VOL to None)");
    // A second click cancels.
    learn[BendR]->click();
    learn[BendR]->click();
    check(midi.learnLane() == MidiRoutes::kNone && !learn[BendR]->isChecked(),
          "controls: a second click did not cancel Learn");
    // A learn started elsewhere (a lane's menu) shows on its button.
    midi.startLearn(0x14);
    check(learn[BendR]->isChecked() && learn[BendR]->text() == QStringLiteral("Listening…") &&
              learn[Pan]->text() == QStringLiteral("Learn") && !status->text().isEmpty() &&
              !status->text().contains(QStringLiteral("Esc")),
          "controls: a learn started outside the page did not show on its button");
    midi.cancelLearn();
    check(!learn[BendR]->isChecked() && learn[BendR]->text() == QStringLiteral("Learn"),
          "controls: a learn cancelled outside the page still showed Listening");
    // Esc, as a real key press, cancels wherever the focus is in the window
    // and leaves the dialog open.
    learn[Lfos]->click();
    learn[Lfos]->setFocus();
    const bool escClaimed = pressKey(&dialog, Qt::Key_Escape);
    check(escClaimed && midi.learnLane() == MidiRoutes::kNone && !learn[Lfos]->isChecked() &&
              rejected == 0 && dialog.isVisible(),
          "controls: Esc on the Learn button did not cancel Learn, or closed the dialog");
    learn[Lfos]->click();
    cc[Vol]->setFocus();
    pressKey(&dialog, Qt::Key_Escape);
    check(midi.learnLane() == MidiRoutes::kNone && rejected == 0 && dialog.isVisible(),
          "controls: Esc in a CC box did not cancel Learn, or closed the dialog");
    midi.injectForTest({0xB0, 90, 1});
    wait(MidiInput::kLearnPollMs * 4);
    check(midi.routes().laneFor(90) == MidiRoutes::kNone, "controls: a cancelled Learn learned");
    // With no learn, Esc closes the dialog as usual.
    pressKey(&dialog, Qt::Key_Escape);
    check(rejected == 1 && !dialog.isVisible(), "controls: Esc without a learn kept the dialog");
    dialog.show();
    dialog.activateWindow();
    QApplication::processEvents();
    // Minimizing the window (a spontaneous hide) keeps the learn; hiding the
    // page (closing Settings, another section) cancels it.
    learn[Lfos]->click();
    dialog.showMinimized();
    QApplication::processEvents();
    check(midi.learnLane() == 0x15, "controls: minimizing the window cancelled Learn");
    dialog.showNormal();
    QApplication::processEvents();
    page.hide();
    check(midi.learnLane() == MidiRoutes::kNone, "controls: hiding the page left Learn armed");
    drain(midi.ring());

    // Back to the defaults.
    cc[Pan]->setValue(10);
    cc[Vol]->setValue(7);
    check(midi.routes() == MidiRoutes() && !QSettings().contains(QStringLiteral("midi/routes")),
          "controls: the table did not return to the defaults");
}

} // namespace

// The window's side, with the song open: the Settings window's MIDI
// section.
int MainWindow::runMidiWindowCheck(const QString &projectRoot, const QString &songLabel)
{
    const int before = g_failures;
    // Counted as failures: "OK" must mean the window half ran.
    if (!check(openProjectDir(projectRoot, /*interactive=*/false), "the project failed to open"))
        return g_failures - before;
    loadSongByLabel(songLabel);
    if (!m_active || m_active->doc.label() != songLabel) {
        std::fprintf(stderr, "midicheck: song '%s' did not load\n", qUtf8Printable(songLabel));
        check(false, "the song did not load");
        return g_failures - before;
    }
    MidiInput &midi = midiInput();

    // Settings → MIDI is the second section; without a backend it is one
    // explanatory label.
    SettingsDialog *dialog = settingsDialog();
    auto *stack = dialog->findChild<QStackedWidget *>(QStringLiteral("settingsStack"));
    dialog->showPage(SettingsDialog::Page::Midi);
    check(dialog->currentPage() == SettingsDialog::Page::Midi && stack &&
              stack->currentWidget() == dialog->midiPage(),
          "Settings → MIDI did not show the MIDI page");
    QWidget *page = dialog->midiPage();
    auto *label = page->findChild<QLabel *>(QStringLiteral("settingsMidiUnavailable"));
    auto *ports = page->findChild<QListWidget *>(QStringLiteral("settingsMidiPorts"));
    if (midi.backendAvailable()) {
        check(label && ports && !label->isVisibleTo(page) && ports->isVisibleTo(page) &&
                  ports->count() == int(midi.ports().size()),
              "the MIDI page does not list the backend's ports");
    } else {
        check(label && ports && label->isVisibleTo(page) && !ports->isVisibleTo(page) &&
                  label->text() == midi.backendStatus() && !label->text().isEmpty(),
              "without a backend the MIDI page does not explain why");
    }
    dialog->reject();

    checkLivePlay();
    checkLiveRoutes();
    checkLiveControls();
    return g_failures - before;
}

// Step 2: live play on the selected track (see the file comment).
void MainWindow::checkLivePlay()
{
    if (!m_audioOk) {
        std::printf("midicheck: live play SKIP (no audio device)\n");
        return;
    }
    MidiInput &midi = midiInput();
    SongView *view = m_active->view;
    const MidiTimeline *timeline = m_active->timeline.get();
    m_audio.holdDeviceForTest(true);
    drain(midi.ring());
    const M4AEngine *engine = m_audio.engineForTest();
    const double rate = m_audio.sampleRate();
    const auto pumpMs = [&](int ms) { m_audio.pumpForTest(uint32_t(rate * ms / 1000.0)); };
    const auto send = [&](uint8_t status, uint8_t d1, uint8_t d2) {
        midi.injectForTest({status, d1, d2});
    };
    // Keys of the channels keyed on (sounding, not yet released) on a track.
    const auto keyed = [&](int track) {
        std::vector<int> keys;
        for (int i = 0; i < MAX_PCM_CHANNELS; i++) {
            const M4APCMChannel &ch = engine->pcmChannels[i];
            if ((ch.status & CHN_ON) && !(ch.status & (CHN_STOP | CHN_IEC)) &&
                ch.trackIndex == track)
                keys.push_back(ch.midiKey);
        }
        for (int i = 0; i < MAX_CGB_CHANNELS; i++) {
            const M4ACGBChannel &ch = engine->cgbChannels[i];
            if ((ch.status & CHN_ON) && !(ch.status & (CHN_STOP | CHN_IEC)) &&
                ch.trackIndex == track)
                keys.push_back(ch.midiKey);
        }
        std::sort(keys.begin(), keys.end());
        return keys;
    };
    const auto has = [](const std::vector<int> &keys, int key) {
        return std::find(keys.begin(), keys.end(), key) != keys.end();
    };
    const auto held = [&] {
        uint64_t lo = 0, hi = 0;
        m_audio.liveHeldKeys(&lo, &hi);
        return std::pair<uint64_t, uint64_t>(lo, hi);
    };
    const auto bits = [](std::initializer_list<int> keys) {
        std::pair<uint64_t, uint64_t> mask{0, 0};
        for (int k : keys)
            (k < 64 ? mask.first : mask.second) |= uint64_t(1) << (k & 63);
        return mask;
    };
    const auto pcmVelocity = [&](int track, int key) {
        for (int i = 0; i < MAX_PCM_CHANNELS; i++) {
            const M4APCMChannel &ch = engine->pcmChannels[i];
            if ((ch.status & CHN_ON) && !(ch.status & (CHN_STOP | CHN_IEC)) &&
                ch.trackIndex == track && ch.midiKey == key)
                return int(ch.velocity);
        }
        return -1;
    };

    // Two used tracks playing a plain DirectSound voice at the playhead (a
    // chord needs PCM channels: a PSG voice has one channel), the first
    // preferably with a looping sample, so it is still sounding after the
    // timed-preview cap.
    int a = -1, b = -1;
    bool aLoops = false;
    for (int t = 0; t < MAX_TRACKS; t++) {
        const ToneData &voice = engine->tracks[t].currentVoice;
        if (!timeline->tracks[t].used || (voice.type & 0xC7) != 0 || !voice.wav)
            continue;
        const bool loops = (voice.wav->status & 0x4000) != 0 && voice.sustain > 0;
        if (a < 0 || (loops && !aLoops)) {
            if (a >= 0 && b < 0)
                b = a;
            a = t;
            aLoops = loops;
        } else if (b < 0) {
            b = t;
        }
    }
    if (!check(a >= 0 && b >= 0, "live play: the song has no two DirectSound tracks to play on")) {
        m_audio.holdDeviceForTest(false);
        return;
    }
    std::printf("midicheck: live play on tracks %d%s and %d\n", a, aLoops ? " (looping)" : "", b);
    view->selectTrack(a);
    check(m_audio.liveTarget() == a, "live play: the selected track is not the live target");

    // Three held keys sound as three channels on the target track.
    send(0x90, 60, 100);
    send(0x90, 64, 100);
    send(0x92, 67, 100); // omni: any channel
    pumpMs(30);
    check(midi.ring().size() == 0, "live play: the audio callback did not drain the input");
    check(keyed(a) == std::vector<int>{60, 64, 67},
          "live play: three held keys are not three channels on the target track");
    check(held() == bits({60, 64, 67}), "live play: the held mask does not show the held keys");
    // The input's activity starts the poll that lights the roll's keyboard.
    QElapsedTimer lit;
    lit.start();
    while (view->liveHeldKeysLo() == 0 && lit.elapsed() < 2000)
        QApplication::processEvents(QEventLoop::WaitForMoreEvents, 5);
    check(std::pair(view->liveHeldKeysLo(), view->liveHeldKeysHi()) == bits({60, 64, 67}),
          "live play: the roll's keyboard did not light the held keys");
    // The roll paints them: every key lit versus none changes the keyboard
    // column, whatever rows are scrolled into view.
    if (auto *roll = view->findChild<QWidget *>(QStringLiteral("pianoRoll"))) {
        const auto column = [&] {
            return roll->grab(QRect(0, 0, 12, roll->height()))
                .toImage()
                .convertToFormat(QImage::Format_RGB32);
        };
        view->setLiveHeldKeys(~uint64_t(0), ~uint64_t(0));
        const QImage allLit = column();
        view->setLiveHeldKeys(0, 0);
        const QImage noneLit = column();
        // Pixel by pixel: QImage's operator== can see bytes no pixel shows.
        int changed = 0;
        for (int y = 0; y < allLit.height() && y < noneLit.height(); y++)
            for (int x = 0; x < allLit.width() && x < noneLit.width(); x++)
                changed += allLit.pixel(x, y) != noneLit.pixel(x, y);
        check(!allLit.isNull() && changed > 0,
              "live play: the roll's keyboard column does not paint MIDI-held keys");
        pollLiveKeys();
    }

    // Held entries never count down: still there past the 10 s timed cap.
    pumpMs(11000);
    check(held() == bits({60, 64, 67}), "live play: held keys expired like a timed preview");
    if (aLoops)
        check(keyed(a) == std::vector<int>{60, 64, 67},
              "live play: held keys stopped sounding before their note-off");

    // Note-off releases them, and the keyboard column goes dark.
    send(0x80, 60, 0);
    send(0x80, 64, 0);
    send(0x82, 67, 0);
    pumpMs(30);
    check(keyed(a).empty(), "live play: note-off did not release the held keys");
    check(held() == bits({}), "live play: the held mask kept released keys");
    pollLiveKeys();
    check(view->liveHeldKeysLo() == 0 && view->liveHeldKeysHi() == 0,
          "live play: the roll's keyboard kept released keys lit");

    // A selection change releases the old track's notes and follows.
    send(0x90, 60, 100);
    pumpMs(30);
    view->selectTrack(b);
    check(m_audio.liveTarget() == b, "live play: a selection change did not move the live target");
    pumpMs(30);
    check(!has(keyed(a), 60), "live play: a selection change left the old track's note sounding");
    check(held() == bits({}), "live play: the held mask kept the old track's key");
    send(0x80, 60, 0); // its late note-off reaches nothing
    send(0x90, 62, 100);
    pumpMs(30);
    check(keyed(b) == std::vector<int>{62} && !has(keyed(a), 62),
          "live play: after a selection change the new track did not play");
    send(0x80, 62, 0);
    pumpMs(30);
    view->selectTrack(a);
    pumpMs(30);

    // Sustain: a released key keeps sounding until pedal-up; a key still
    // held at pedal-up keeps sounding.
    send(0xB0, 64, 127);
    send(0x90, 60, 100);
    send(0x80, 60, 0);
    send(0x90, 64, 100);
    pumpMs(30);
    check(keyed(a) == std::vector<int>{60, 64},
          "live play: the sustain pedal did not hold a released key");
    send(0xB0, 64, 0);
    pumpMs(30);
    check(keyed(a) == std::vector<int>{64},
          "live play: pedal-up did not release only the keys no longer held");
    send(0x80, 64, 0);
    pumpMs(30);
    check(keyed(a).empty(), "live play: a key held through pedal-up did not release");

    // Fixed velocity overrides the keyboard's; off passes it through.
    midi.setFixedVelocity(40);
    send(0x90, 60, 127);
    pumpMs(30);
    check(pcmVelocity(a, 60) == 40, "live play: the fixed velocity was not applied");
    send(0x80, 60, 0);
    midi.setFixedVelocity(0);
    send(0x90, 61, 90);
    pumpMs(30);
    check(pcmVelocity(a, 61) == 90, "live play: the keyboard's velocity was not passed through");
    send(0x80, 61, 0);
    pumpMs(30);

    // Bend and mod act on the whole target track; leaving the track
    // re-chases them at the playhead and leaves the new one alone.
    const int8_t bBend = engine->tracks[b].bend;
    const uint8_t bMod = engine->tracks[b].mod;
    const int8_t aBend = engine->tracks[a].bend;
    const uint8_t aMod = engine->tracks[a].mod;
    // The wheel's travel maps onto the MOD lane's range as drawn.
    const auto wheelMod = [&](int track, int wheel) {
        return (wheel * view->laneDisplayMax(track, 0x01) + 63) / 127;
    };
    // Values the song's chased ones can't equal, so the restore below is
    // seen: a quarter bend up (MSB 96, bend14 = 96 << 7, +4096 from centre:
    // engine bend 32) unless the song sits there, then a quarter down.
    const int bendTo = aBend == 32 ? -32 : 32;
    const int wheel = wheelMod(a, 100) != aMod ? 100 : 50;
    send(0xE0, 0, uint8_t(64 + bendTo));
    send(0xB0, 1, uint8_t(wheel));
    pumpMs(30);
    check(engine->tracks[a].bend == bendTo, "live play: pitch bend did not reach the target track");
    check(engine->tracks[a].mod == wheelMod(a, wheel),
          "live play: the mod wheel did not reach the target track at the lane's scale");
    check(aBend != bendTo && aMod != wheelMod(a, wheel),
          "live play: the song's own bend/mod hide the restore check");
    // Full travel reaches the lane's top; a new range rescales the wheel.
    send(0xB0, 1, 127);
    pumpMs(30);
    check(engine->tracks[a].mod == view->laneDisplayMax(a, 0x01),
          "live play: full mod-wheel travel did not reach the MOD lane's displayed max");
    view->setLaneDisplayRange(a, 0x01, 64);
    send(0xB0, 1, 127);
    pumpMs(30);
    check(view->laneDisplayMax(a, 0x01) >= 64 &&
              engine->tracks[a].mod == view->laneDisplayMax(a, 0x01),
          "live play: changing the MOD lane's range did not rescale the mod wheel");
    view->setLaneDisplayRange(a, 0x01, 0);
    send(0xB0, 1, uint8_t(wheel));
    pumpMs(30);
    view->selectTrack(b);
    pumpMs(30);
    check(engine->tracks[a].bend == aBend && engine->tracks[a].mod == aMod,
          "live play: leaving a track did not restore its chased bend and mod");
    check(engine->tracks[b].bend == bBend && engine->tracks[b].mod == bMod,
          "live play: the new target's bend and mod were disturbed");
    view->selectTrack(a);
    pumpMs(30);

    // Held keys survive steal pressure from a 26-note band sweep, which
    // steals its own timed previews instead.
    send(0x90, 60, 100);
    send(0x90, 64, 100);
    send(0x90, 67, 100);
    pumpMs(30);
    for (int k = 30; k < 56; k++)
        m_audio.previewNoteTimed(uint8_t(b), uint8_t(k), 100, uint32_t(rate * 5));
    pumpMs(30);
    check(held() == bits({60, 64, 67}), "live play: band-sweep previews stole held keys");
    send(0x80, 60, 0);
    send(0x80, 64, 0);
    send(0x80, 67, 0);
    for (int k = 30; k < 56; k++)
        m_audio.previewNoteTimed(uint8_t(b), uint8_t(k), 0, 0);
    pumpMs(30);

    // A transport cut silences held keys and clears the published mask; the
    // key's later note-off finds nothing to release, so it can't stop
    // another note on the same track and key.
    send(0x90, 60, 100);
    pumpMs(30);
    m_audio.play();
    pumpMs(10);
    check(held() == bits({}), "live play: a transport cut left the held mask set");
    pollLiveKeys();
    check(view->liveHeldKeysLo() == 0 && view->liveHeldKeysHi() == 0,
          "live play: a transport cut left the roll's key lit");
    m_audio.stop();
    pumpMs(10);
    m_audio.previewNote(uint8_t(a), 60, 100);
    pumpMs(30);
    const bool previewSounds = has(keyed(a), 60);
    send(0x80, 60, 0);
    pumpMs(30);
    check(previewSounds && has(keyed(a), 60),
          "live play: a key released after a cut sent a stray note-off");
    m_audio.previewNote(uint8_t(a), 60, 0);
    pumpMs(30);

    // Panic (the command, and CC123) releases held and sustained keys.
    send(0xB0, 64, 127);
    send(0x90, 60, 100);
    send(0x90, 64, 100);
    send(0x80, 64, 0);
    pumpMs(30);
    m_midiPanicAction->trigger();
    pumpMs(30);
    check(keyed(a).empty() && held() == bits({}),
          "live play: MIDI: All Notes Off did not release held and sustained keys");
    // The panic lifts the pedal too (its pedal-up may never come, as when
    // the port vanished): a later note-off releases at once. No pedal-up is
    // sent from here on.
    send(0x80, 60, 0);
    send(0x90, 62, 100);
    pumpMs(30);
    send(0x80, 62, 0);
    pumpMs(30);
    check(keyed(a).empty(), "live play: after a panic the pedal still held a released key");
    // CC123 the same, with the pedal down again.
    send(0xB0, 64, 127);
    send(0x90, 62, 100);
    send(0x80, 62, 0);
    pumpMs(30);
    check(keyed(a) == std::vector<int>{62}, "live play: the sustain pedal did not hold a key");
    send(0xB3, 123, 0);
    pumpMs(30);
    check(keyed(a).empty(), "live play: CC123 did not release the sustained keys");
    send(0x90, 63, 100);
    send(0x80, 63, 0);
    pumpMs(30);
    check(keyed(a).empty(), "live play: after CC123 the pedal still held a released key");
    // CC121 (reset all controllers) lifts the pedal, releasing what it held.
    send(0xB0, 64, 127);
    send(0x90, 62, 100);
    send(0x80, 62, 0);
    pumpMs(30);
    send(0xB0, 121, 0);
    pumpMs(30);
    check(keyed(a).empty(), "live play: CC121 did not lift the sustain pedal");
    send(0x90, 63, 100);
    send(0x80, 63, 0);
    pumpMs(30);
    check(keyed(a).empty(), "live play: after CC121 the pedal still held a released key");

    // Live play off, or no target: the input drains and nothing sounds.
    midi.setLivePlay(false);
    send(0x90, 60, 100);
    pumpMs(30);
    check(midi.ring().size() == 0 && keyed(a).empty(),
          "live play: with Play incoming notes off a note still sounded");
    send(0x80, 60, 0);
    midi.setLivePlay(true);
    m_audio.setLiveTarget(-1);
    send(0x90, 60, 100);
    pumpMs(30);
    check(midi.ring().size() == 0 && keyed(a).empty() && held() == bits({}),
          "live play: with no target a note still sounded");
    send(0x80, 60, 0);
    publishLiveTarget();
    pumpMs(30);
    check(m_audio.liveTarget() == a, "live play: the live target was not republished");

    drain(midi.ring());
    m_audio.holdDeviceForTest(false);
}

// docs/midi-controls/PLAN.md step 1: MOD follows its routed CC, live and
// into the capture ring; the CC it left does nothing.
void MainWindow::checkLiveRoutes()
{
    if (!m_audioOk) {
        std::printf("midicheck: live routes SKIP (no audio device)\n");
        return;
    }
    MidiInput &midi = midiInput();
    SongView *view = m_active->view;
    const int a = m_audio.liveTarget();
    if (!check(a >= 0, "live routes: no live target"))
        return;
    m_audio.holdDeviceForTest(true);
    drain(midi.ring());
    const M4AEngine *engine = m_audio.engineForTest();
    const double rate = m_audio.sampleRate();
    const auto pumpMs = [&](int ms) { m_audio.pumpForTest(uint32_t(rate * ms / 1000.0)); };
    const auto send = [&](uint8_t d1, uint8_t d2) { midi.injectForTest({0xB0, d1, d2}); };
    const int modMax = view->laneDisplayMax(a, 0x01);

    check(midi.setRoute(0x01, 74), "live routes: MOD could not be routed to CC74");
    send(74, 0);
    pumpMs(30);
    check(engine->tracks[a].mod == 0, "live routes: CC74 at rest did not set MOD 0");
    send(1, 127); // the old mod wheel: now unrouted
    pumpMs(30);
    check(engine->tracks[a].mod == 0, "live routes: CC1 still drove MOD after it was remapped");
    send(74, 127);
    pumpMs(30);
    check(engine->tracks[a].mod == modMax,
          "live routes: CC74 did not drive MOD at the lane's scale");
    // MOD to none: no CC drives it.
    midi.setRoute(0x01, MidiRoutes::kNone);
    send(74, 0);
    send(1, 0);
    pumpMs(30);
    check(engine->tracks[a].mod == modMax, "live routes: an unrouted MOD still moved");

    // Recording follows the route: a capture sees CC74's moves as MOD, and
    // none from CC1.
    midi.setRoute(0x01, 74);
    const auto mods = [&] {
        std::vector<int> values;
        CaptureEvent ev;
        while (m_audio.popCapture(&ev)) {
            if (ev.kind == CaptureEvent::Control && ev.key == 0x01)
                values.push_back(ev.value);
        }
        return values;
    };
    mods(); // stale
    m_audio.armCapture(0x7E57);
    m_audio.play();
    pumpMs(30);
    send(1, 127);
    pumpMs(30);
    send(74, 127);
    pumpMs(30);
    send(74, 0);
    pumpMs(30);
    m_audio.armCapture(0);
    m_audio.stop();
    pumpMs(30);
    check(mods() == std::vector<int>{modMax, 0},
          "live routes: the capture did not record CC74 (and only CC74) as MOD");

    midi.setRoute(0x01, 1);
    check(midi.routes() == MidiRoutes(), "live routes: could not return to the defaults");
    drain(midi.ring());
    m_audio.holdDeviceForTest(false);
}

// docs/midi-controls/PLAN.md step 2: every routed lane, program changes,
// CC121 and the target-change handback, as engine state.
void MainWindow::checkLiveControls()
{
    if (!m_audioOk) {
        std::printf("midicheck: live controls SKIP (no audio device)\n");
        return;
    }
    MidiInput &midi = midiInput();
    SongView *view = m_active->view;
    const MidiTimeline *timeline = m_active->timeline.get();
    const int a = m_audio.liveTarget();
    int b = -1;
    for (int t = 0; t < MAX_TRACKS && b < 0; t++)
        if (t != a && timeline->tracks[t].used)
            b = t;
    if (!check(a >= 0 && b >= 0, "live controls: no live target and second track"))
        return;
    m_audio.holdDeviceForTest(true);
    drain(midi.ring());
    const M4AEngine *engine = m_audio.engineForTest();
    const double rate = m_audio.sampleRate();
    const auto pumpMs = [&](int ms) { m_audio.pumpForTest(uint32_t(rate * ms / 1000.0)); };
    const auto send = [&](uint8_t d1, uint8_t d2) { midi.injectForTest({0xB0, d1, d2}); };
    // As a keyboard sends it: two bytes.
    const auto program = [&](uint8_t p) { midi.injectBytesForTest({0xC0, p}); };
    enum { Vol, Pan, BendR, Lfos, Mod, Bend, Program };
    const auto lanes = [&](int t) {
        const M4ATrack &tr = engine->tracks[t];
        return std::array<int, 7>{tr.rawVolume, tr.pan,  tr.bendRange,     tr.lfoSpeed,
                                  tr.mod,       tr.bend, tr.currentProgram};
    };

    // BENDR on CC20 and LFOS on CC21 (VOL and PAN keep CC7 and CC10).
    check(midi.setRoute(0x14, 20) && midi.setRoute(0x15, 21),
          "live controls: BENDR and LFOS could not be routed");
    // The song's own state at the playhead: a seek chases every track.
    m_audio.seek(0);
    pumpMs(30);
    const std::array<int, 7> songA = lanes(a);
    const std::array<int, 7> songB = lanes(b);

    // Each lane at its scale (D2), on the target track only. Values the
    // song's chased ones can't equal, so the handback below is seen.
    const int vol = songA[Vol] == 50 ? 51 : 50;
    const int panByte = songA[Pan] == 100 - 64 ? 101 : 100;
    const int lfos = songA[Lfos] == 90 ? 91 : 90;
    send(7, uint8_t(vol));
    pumpMs(30);
    check(lanes(a)[Vol] == vol &&
              engine->tracks[a].volume == uint8_t(vol * engine->songMasterVolume / MAX_SONG_VOLUME),
          "live controls: CC7 did not set the target's VOL to the knob's value");
    send(10, uint8_t(panByte));
    pumpMs(30);
    check(lanes(a)[Pan] == panByte - 64, "live controls: CC10 did not set the target's PAN");
    send(21, uint8_t(lfos));
    pumpMs(30);
    check(lanes(a)[Lfos] == lfos, "live controls: CC21 did not set the target's LFOS");
    // BENDR: the knob's full travel is 0..24 semitones, rounded.
    bool bendrScaled = true;
    for (const auto &[knob, semitones] :
         {std::pair{0, 0}, std::pair{3, 1}, std::pair{127, 24}, std::pair{64, 12}}) {
        send(20, uint8_t(knob));
        pumpMs(30);
        bendrScaled &= lanes(a)[BendR] == semitones;
    }
    check(bendrScaled, "live controls: CC20 did not set BENDR on the 0..24 scale");
    // The BENDR row's axis matches the knob's reach (unless the song's own
    // points go past it).
    int songBendrMax = 0;
    for (const DocLanePoint &pt : m_active->doc.lanePoints(a, 0x14))
        songBendrMax = std::max(songBendrMax, pt.value);
    check(view->laneDisplayMax(a, 0x14) == std::max(24, songBendrMax),
          "live controls: the BENDR row's axis is not 0..24");
    if (songA[BendR] == 12) { // keep the restore visible
        send(20, 127);
        pumpMs(30);
    }
    const int bendrNow = lanes(a)[BendR];
    // An unrouted CC moves nothing.
    send(22, 99);
    pumpMs(30);
    check(lanes(a) == std::array<int, 7>{vol, panByte - 64, bendrNow, lfos, songA[Mod], songA[Bend],
                                         songA[Program]},
          "live controls: the routed lanes did not hold, or an unrouted CC moved one");
    check(lanes(b) == songB, "live controls: a routed knob moved a track other than the target");

    // Program change: the target's voice, the data byte as the slot. The
    // count to expect comes from the voicegroup file itself, not from the
    // source model publishLiveTarget reads: the pinned song's is a per-file
    // voicegroup, one voice macro per line after its voice_group header.
    int voices = -1;
    if (m_active->vgSource && !m_active->vgSource->monolithic()) {
        QFile file(m_active->vgSource->filePath());
        if (file.open(QIODevice::ReadOnly)) {
            voices = 0;
            for (const QByteArray &line : file.readAll().split('\n')) {
                const QByteArray macro = line.trimmed().split(' ').value(0).trimmed();
                if (macro.startsWith("voice_") && macro != "voice_group")
                    voices++;
            }
        }
    }
    check(voices > 10 && voices < VOICEGROUP_SIZE && m_audio.liveVoiceCount() == voices,
          qPrintable(QStringLiteral("live controls: the voice count published (%1) is not the "
                                    "voicegroup file's (%2)")
                         .arg(m_audio.liveVoiceCount())
                         .arg(voices)));
    if (voices <= 10)
        voices = VOICEGROUP_SIZE; // keep the rest meaningful after a failure
    const int prog = (songA[Program] + 1) % voices;
    program(uint8_t(prog));
    pumpMs(30);
    check(lanes(a)[Program] == prog &&
              engine->tracks[a].currentVoice.type == m_active->voicegroup->voices[prog].type,
          "live controls: a program change did not switch the target's voice");
    check(lanes(b) == songB, "live controls: a program change reached another track");

    // Past the voicegroup's last voice: ignored, and reported once per poll
    // however many arrived. The count is shortened so any song shows it.
    pollLiveKeys(); // settle anything earlier
    const int reports = m_liveIgnoredReports;
    m_audio.setLiveVoiceCount(10);
    const int inRange = prog == 9 ? 8 : 9;
    program(10);
    program(120);
    pumpMs(30);
    check(lanes(a)[Program] == prog, "live controls: a program past the last voice was applied");
    // A tab switch publishing another count before the poll: the report
    // still names the range the changes were ignored against.
    m_audio.setLiveVoiceCount(voices);
    pollLiveKeys();
    check(m_liveIgnoredReports == reports + 1 &&
              statusBar()->currentMessage().contains(QStringLiteral("120")) &&
              statusBar()->currentMessage().contains(QStringLiteral("voices are 0–9.")),
          "live controls: ignored program changes were not reported once, against their range");
    pollLiveKeys();
    check(m_liveIgnoredReports == reports + 1, "live controls: an ignored program was re-reported");
    m_audio.setLiveVoiceCount(10);
    program(uint8_t(inRange)); // the last voice still plays
    pumpMs(30);
    pollLiveKeys();
    check(lanes(a)[Program] == inRange && m_liveIgnoredReports == reports + 1,
          "live controls: the voicegroup's last voice was refused");
    publishLiveTarget();
    check(m_audio.liveVoiceCount() == voices, "live controls: the voice count was not republished");
    program(uint8_t(prog));
    pumpMs(30);
    check(lanes(a)[Program] == prog, "live controls: the voice count was not lifted again");

    // CC121 (J5): bend and every routed lane go to the engine's track-init
    // values; a lane routed to none (PAN and MOD here) and the voice stay.
    send(1, 127);
    midi.injectForTest({0xE0, 0, 96});
    pumpMs(30);
    const int modNow = lanes(a)[Mod];
    check(modNow > 0 && lanes(a)[Bend] == 32,
          "live controls: the wheels did not move before CC121");
    midi.setRoute(0x0A, MidiRoutes::kNone);
    midi.setRoute(0x01, MidiRoutes::kNone);
    send(121, 0);
    pumpMs(30);
    check(lanes(a) == std::array<int, 7>{127, panByte - 64, 2, 22, modNow, 0, prog},
          "live controls: CC121 did not reset exactly the routed lanes and bend");
    check(lanes(b) == songB, "live controls: CC121 reached another track");
    midi.setRoute(0x0A, 10);
    midi.setRoute(0x01, 1);

    // LFOS mirrors ply_lfos: a nonzero speed only changes the rate, so a
    // streaming knob keeps the vibrato's phase (lfoSpeedC) and offset
    // (modM); only 0 is clear_modM. One frame applies the input at the
    // callback's start and crosses an LFO tick only rarely, in which case
    // the phase is one step on instead.
    const M4ATrack &lfoTrack = engine->tracks[a];
    send(1, 127);
    send(21, 1);
    pumpMs(30);
    for (int i = 0; i < 200 && !(lfoTrack.lfoSpeedC >= 40 && lfoTrack.lfoSpeedC <= 120); i++)
        pumpMs(30);
    bool lfoKept = lfoTrack.lfoSpeedC >= 40 && lfoTrack.lfoSpeedC <= 120 && lfoTrack.modM != 0;
    for (const int speed : {1, 1, 2, 3}) {
        const int phase = lfoTrack.lfoSpeedC;
        const int offset = lfoTrack.modM;
        send(21, uint8_t(speed));
        m_audio.pumpForTest(1);
        lfoKept &= lfoTrack.lfoSpeed == speed &&
                   ((lfoTrack.lfoSpeedC == phase && lfoTrack.modM == offset) ||
                    lfoTrack.lfoSpeedC == phase + speed);
    }
    const int phaseBefore = lfoTrack.lfoSpeedC;
    pumpMs(30);
    const int advanced = (lfoTrack.lfoSpeedC - phaseBefore) & 0xFF;
    lfoKept &= advanced > 0 && advanced <= 30;
    check(lfoKept, "live controls: a nonzero LFOS restarted the LFO's phase or cleared its offset");
    send(21, 0);
    m_audio.pumpForTest(1);
    check(lfoTrack.lfoSpeed == 0 && lfoTrack.lfoSpeedC == 0 && lfoTrack.modM == 0,
          "live controls: LFOS 0 did not clear the LFO");
    // Off the song's values again, every lane (the resets may sit on them).
    send(7, uint8_t(vol));
    send(20, songA[BendR] == 24 ? 64 : 127);
    send(21, uint8_t(lfos));
    send(1, songA[Mod] == modNow ? 0 : 127);
    midi.injectForTest({0xE0, 0, uint8_t(songA[Bend] == 32 ? 32 : 96)});
    pumpMs(30);
    bool allMoved = true;
    for (int lane = Vol; lane <= Program; lane++)
        allMoved &= lanes(a)[size_t(lane)] != songA[size_t(lane)];
    check(allMoved, "live controls: the song's own values hide the handback check");

    // Leaving the track hands every touched lane and the voice back to the
    // song, as chased at the playhead; the new target is undisturbed.
    view->selectTrack(b);
    pumpMs(30);
    check(m_audio.liveTarget() == b && lanes(a) == songA,
          "live controls: leaving a track did not hand its touched lanes and voice back");
    check(lanes(b) == songB, "live controls: the new target's lanes were disturbed");
    // No target: a program change is ignored, not counted, even one past
    // the last voice (10, with the count shortened as above).
    const uint32_t ignored = m_audio.liveIgnoredProgram();
    m_audio.setLiveTarget(-1);
    m_audio.setLiveVoiceCount(10);
    program(10);
    send(7, 1);
    pumpMs(30);
    check(lanes(a) == songA && lanes(b) == songB && m_audio.liveIgnoredProgram() == ignored,
          "live controls: with no target a program change or knob still acted");
    view->selectTrack(a);
    publishLiveTarget();
    pumpMs(30);

    midi.setRoute(0x14, MidiRoutes::kNone);
    midi.setRoute(0x15, MidiRoutes::kNone);
    check(midi.routes() == MidiRoutes(), "live controls: could not return to the defaults");
    drain(midi.ring());
    m_audio.holdDeviceForTest(false);
}

int runMidiCheck(const QString &projectRoot, const QString &songLabel, const QString &shotPath)
{
    g_shotPath = shotPath;
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "midicheck: no temp dir for settings\n");
        return 1;
    }
    // IniFormat by default: NativeFormat's path is ignored on Windows (the
    // registry) and macOS (plists), where the user's real midi/* keys live.
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QStandardPaths::setTestModeEnabled(true);

    checkParseTable();
    checkPortIdentities();
    checkRing();
    checkService();
    checkRoutesTable();
    checkRoutesService();
    checkSettingsPage();
    checkControlsPage();
    {
        MainWindow window;
        window.runMidiWindowCheck(projectRoot, songLabel);
    }

    if (g_failures == 0)
        std::printf("midicheck: OK\n");
    return g_failures ? 1 : 0;
}
