#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QEventLoop>
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
    return g_failures - before;
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
