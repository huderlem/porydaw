#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <iterator>
#include <thread>
#include <vector>

#include "audio/midiinput.h"
#include "mainwindow.h"
#include "ui/midiactivityled.h"
#include "ui/midisettingspage.h"
#include "ui/settingsdialog.h"
#include "ui/songview.h"

// --midicheck <projectRoot> <songLabel> [shot.png]: MIDI keyboard input
// (docs/midi-input/PLAN.md). No MIDI hardware is ever assumed — WSL and CI
// have no MIDI system at all — so every message enters through
// MidiInput::injectForTest, which takes the same path as a port's RtMidi
// callback. QSettings is redirected into a temp dir first.
//
// With a shot path, the MIDI page (with test ports) and the lit transport
// toolbar are saved as <shot>-page.png and <shot>-toolbar-lit.png.
//
// Step 1 covers: the parse table (a velocity-0 note on is a note off;
// aftertouch, program change and system messages are dropped), the input
// ring (FIFO, overflow counted, several producers at once against a
// concurrent consumer), port-list plumbing on whatever backend this machine
// has (without asserting that any device exists), the injector reaching the
// ring and the throttled activity signal, the Settings → MIDI page
// persisting its controls and port toggles, and the window's MIDI light and
// Settings section with a song open.
//
// Step 2 (live play) parks the audio device and drives the real callback
// itself (AudioEngine::pumpForTest), so time is simulated and every check is
// deterministic: held keys sound on the selected track and outlive the
// band-sweep's timed cap, note-off and the sustain pedal release them, fixed
// velocity applies, a selection change releases and follows, bend and mod
// act on the whole target track and are re-chased when it is left, held
// notes win steal pressure from timed previews, a cut clears the published
// held mask and a key released after it sends no stray note-off, and live
// play off / no target / panic drain silently.

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
    check(rejected({0xC0, 5}), "program change was not dropped");
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
    // waits out the interval: one blink per interval, not per message.
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

} // namespace

// The window's side, with the song open: the transport's MIDI light and the
// Settings window's MIDI section.
int MainWindow::runMidiWindowCheck(const QString &projectRoot, const QString &songLabel)
{
    if (!openProjectDir(projectRoot, /*interactive=*/false)) {
        std::fprintf(stderr, "midicheck: project failed to open\n");
        return 1;
    }
    loadSongByLabel(songLabel);
    if (!m_active || m_active->doc.label() != songLabel) {
        std::fprintf(stderr, "midicheck: song '%s' did not load\n", qUtf8Printable(songLabel));
        return 1;
    }
    const int before = g_failures;
    MidiInput &midi = midiInput();

    // The light sits on the transport toolbar and says what it listens to.
    MidiActivityLed *led = midiActivityLed();
    if (check(led && m_transportToolbar->isAncestorOf(led),
              "the MIDI light is not on the transport toolbar")) {
        const QStringList open = midi.openPorts();
        check(led->isListening() == !open.isEmpty(),
              "the MIDI light's idle look does not match the open ports");
        bool named = !led->toolTip().isEmpty();
        for (const QString &name : open)
            named &= led->toolTip().contains(name);
        if (open.isEmpty())
            named &= led->toolTip().startsWith(QStringLiteral("No MIDI input"));
        check(named, "the MIDI light's tooltip does not name the open ports");

        check(!led->isLit(), "the MIDI light was lit before any input");
        midi.injectForTest({0x90, 60, 100});
        midi.injectForTest({0x80, 60, 0});
        // Polled, not one fixed wait: a loaded machine (ASAN, CI) may
        // overshoot the short flash.
        QElapsedTimer waited;
        waited.start();
        while (!led->isLit() && waited.elapsed() < 2000)
            QApplication::processEvents(QEventLoop::WaitForMoreEvents, 5);
        check(led->isLit(), "injected input did not light the MIDI light");
        shoot(m_transportToolbar, "toolbar-lit");
        wait(MidiActivityLed::kFlashMs * 2);
        check(!led->isLit(), "the MIDI light stayed lit after input stopped");
        drain(midi.ring());
    }

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
        check(!label && ports && ports->count() == int(midi.ports().size()),
              "the MIDI page does not list the backend's ports");
    } else {
        check(label && !ports && label->text() == midi.backendStatus() && !label->text().isEmpty(),
              "without a backend the MIDI page is not one explanatory label");
    }
    dialog->reject();

    checkLivePlay();
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
    send(0xE0, 0, 96); // bend14 = 96 << 7 = 12288, +4096 from centre
    send(0xB0, 1, 100);
    pumpMs(30);
    check(engine->tracks[a].bend == 32, "live play: pitch bend did not reach the target track");
    check(engine->tracks[a].mod == 100, "live play: the mod wheel did not reach the target track");
    check(aBend != 32 && aMod != 100, "live play: the song's own bend/mod hide the restore check");
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
    send(0x80, 60, 0);
    send(0x90, 62, 100);
    pumpMs(30);
    send(0xB3, 123, 0);
    pumpMs(30);
    check(keyed(a).empty(), "live play: CC123 did not release the sustained keys");
    send(0xB0, 64, 0);
    send(0x80, 62, 0);
    pumpMs(30);

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

int runMidiCheck(const QString &projectRoot, const QString &songLabel, const QString &shotPath)
{
    g_shotPath = shotPath;
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "midicheck: no temp dir for settings\n");
        return 1;
    }
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QStandardPaths::setTestModeEnabled(true);

    checkParseTable();
    checkRing();
    checkService();
    checkSettingsPage();
    {
        MainWindow window;
        window.runMidiWindowCheck(projectRoot, songLabel);
    }

    if (g_failures == 0)
        std::printf("midicheck: OK\n");
    return g_failures ? 1 : 0;
}
