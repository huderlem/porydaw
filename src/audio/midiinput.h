#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>

#include "audio/midiroutes.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

class QTimer;
class RtMidiIn;

// One channel-voice message from a MIDI keyboard, already validated by
// MidiInput::parse: note off (0x8n; a velocity-0 note on arrives as one),
// note on (0x9n, d2 >= 1), control change (0xBn) or pitch bend (0xEn,
// d1 = LSB, d2 = MSB). The channel stays in the status byte; data bytes are
// 7-bit.
struct MidiMsg {
    uint8_t status = 0;
    uint8_t d1 = 0;
    uint8_t d2 = 0;
    // When it arrived (midiClockNs), so the audio thread can tell how long
    // it waited for a callback; 0 = unknown, taken as no wait.
    int64_t arrivalNs = 0;
};

// The clock MidiMsg::arrivalNs is read on (steady, any thread).
int64_t midiClockNs();

// A bounded multi-producer, single-consumer ring of T. Producers (RtMidi's
// callback threads, one per open port, and the test injector on the UI
// thread) serialise on a short mutex; the one consumer never locks: pop()
// is wait-free. A full ring drops the new message and counts it.
template <typename T, uint32_t Capacity>
class MidiRing
{
  public:
    static constexpr uint32_t kCapacity = Capacity; // power of two
    static_assert((kCapacity & (kCapacity - 1)) == 0, "ring size must be a power of two");

    // Any thread. False (and dropped() grows) when the ring is full.
    bool push(const T &msg)
    {
        std::lock_guard<std::mutex> lock(m_producer);
        const uint32_t write = m_write.load(std::memory_order_relaxed);
        const uint32_t read = m_read.load(std::memory_order_acquire);
        if (write - read >= kCapacity) {
            m_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        m_slots[write & (kCapacity - 1)] = msg;
        m_write.store(write + 1, std::memory_order_release);
        return true;
    }
    // The one consumer thread only. False when empty.
    bool pop(T *out)
    {
        const uint32_t read = m_read.load(std::memory_order_relaxed);
        const uint32_t write = m_write.load(std::memory_order_acquire);
        if (read == write)
            return false;
        *out = m_slots[read & (kCapacity - 1)];
        m_read.store(read + 1, std::memory_order_release);
        return true;
    }
    // Approximate from any thread; exact on the consumer thread.
    uint32_t size() const
    {
        return m_write.load(std::memory_order_acquire) - m_read.load(std::memory_order_acquire);
    }
    uint64_t dropped() const { return m_dropped.load(std::memory_order_relaxed); }

  private:
    std::mutex m_producer;
    std::array<T, kCapacity> m_slots{};
    std::atomic<uint32_t> m_write{0}; // producers (under m_producer) increment
    std::atomic<uint32_t> m_read{0};  // the consumer increments
    std::atomic<uint64_t> m_dropped{0};
};

// The input ring between the MIDI threads and the audio thread
// (docs/midi-input/PLAN.md §2 stance 1); its consumer is the audio callback.
class MidiInputRing : public MidiRing<MidiMsg, 256>
{};

// One channel-voice message as a port sent it, for porydaw.midi
// (docs/midi-input/PLAN.md step 6): unlike MidiMsg nothing is normalised
// (a velocity-0 note on stays 0x9n) and every channel-voice type is kept,
// program change and both aftertouches included. One-data-byte types
// (0xCn, 0xDn) have d2 = 0. port is MidiInput's token for the port's
// identity (portForToken); 0 = none (an injected message without a port).
struct MidiTapMsg {
    uint8_t status = 0;
    uint8_t d1 = 0;
    uint8_t d2 = 0;
    uint16_t port = 0;
};

// The script tap: MIDI threads to the UI thread, drained there once per
// queued hop (MidiInput::tapReady). Separate from the input ring, so the
// audio path neither sees nor waits for it.
class MidiTapRing : public MidiRing<MidiTapMsg, 1024>
{};

// MIDI keyboard input (docs/midi-input/PLAN.md, step 1): owns the RtMidi
// ports, the input ring they feed, and the MIDI preferences. Lives on the UI
// thread (owned by MainWindow); only the RtMidi callbacks and the ring's
// consumer run elsewhere.
//
// Ports are identified by a stable name, not by RtMidi's position-dependent
// one (portIdentities): the device's own name ("Keystation 49"), and
// "Name (2)", "Name (3)", ... for further devices of the same name, in the
// backend's order. Every port is enabled unless its identity is in
// midi/disabledPorts, so a newly plugged keyboard plays at once. A 2 s poll
// reopens enabled ports as they appear and closes the ones that vanish.
//
// Without a usable backend (a build without RtMidi or with only its dummy
// API, or a system where the backend fails to start — WSL has no ALSA
// sequencer) everything still works and lists no ports; backendStatus()
// says why.
class MidiInput : public QObject
{
    Q_OBJECT

  public:
    static constexpr int kPollIntervalMs = 2000;
    // Shortest gap between two activity() signals: a fast run of notes is
    // one signal per gap, not one per message.
    static constexpr int kActivityIntervalMs = 40;
    // latencyMs() value meaning "use the audio device's reported latency".
    static constexpr int kLatencyAuto = -1;
    static constexpr int kLatencyMaxMs = 500;

    // How a backend spells the raw port names RtMidi reports. WinMM (built
    // with RTMIDI_DO_NOT_ENSURE_UNIQUE_PORTNAMES) and CoreMIDI give the
    // device's own name; ALSA appends " client:port", which changes when a
    // device is replugged.
    enum class PortNaming { Plain, Alsa };
    // The raw name without the backend's position-dependent suffix.
    static QString portBaseName(const QString &raw, PortNaming naming);
    // One identity per raw name, in order: the base name for its first
    // occurrence, "Base (n)" for the n-th port of that name. An empty raw
    // name (a port that vanished mid-scan) gives an empty identity.
    static QStringList portIdentities(const QStringList &raw, PortNaming naming);
    // Whether identity id, resolved against the raw list before, still names
    // the same device in now: it is listed, and the ports sharing its base
    // name are unchanged. With two identical keyboards, unplugging either
    // renumbers the other and nothing tells which one left, so neither is
    // stable. A device replugged between two scans at the same place lists
    // exactly as before and reads as stable (Refresh reopens it).
    static bool identityStable(const QStringList &before, const QStringList &now, const QString &id,
                               PortNaming naming);

    explicit MidiInput(QObject *parent = nullptr);
    ~MidiInput() override;

    // True when a real platform backend (WinMM, CoreMIDI, ALSA) is up.
    bool backendAvailable() const { return m_backend != nullptr || m_testBackend; }
    // The backend's display name ("ALSA", "Windows MultiMedia", ...), or
    // empty when none.
    QString backendName() const { return m_backendName; }
    // Why there is no backend (a sentence for the Settings page), or empty.
    QString backendStatus() const { return m_backendStatus; }

    // The identities of the ports seen by the last scan, in the backend's
    // order.
    QStringList ports() const;
    // The ports currently open (receiving), in the order they opened.
    QStringList openPorts() const;
    bool isPortOpen(const QString &name) const;
    bool isPortEnabled(const QString &name) const;
    // An enabled port that is listed but would not open (in use by another
    // program, or a driver error). Retried on Refresh or a re-enable.
    bool portFailed(const QString &name) const;
    // Persists midi/disabledPorts, then opens or closes the port to match.
    void setPortEnabled(const QString &name, bool enabled);
    // Opens a listed port; false (never a throw) when it can't. Open ports
    // are not re-opened.
    bool openPort(const QString &name);
    void closePort(const QString &name);
    // Closes every port. Quietly (app teardown) emits nothing.
    void closeAll(bool quietly = false);
    // The user's Refresh: retries starting the backend if it is down, then
    // closes and reopens every open port (recovering a handle left dead by
    // a quick replug), opens newly listed enabled ports and retries failed
    // ones. The 2 s poll rescans without the reopen.
    void refresh();

    // Preferences (Settings → MIDI). Each setter persists a real change and
    // emits settingsChanged().
    bool livePlay() const { return m_livePlay; } // midi/livePlay, default on
    void setLivePlay(bool on);
    int fixedVelocity() const { return m_fixedVelocity; } // midi/fixedVelocity, 0 = off
    void setFixedVelocity(int velocity);
    int latencyMs() const { return m_latencyMs; } // midi/latencyMs, kLatencyAuto default
    void setLatencyMs(int ms);
    // Which CC drives which lane (midi/routes, MidiRoutes' defaults when
    // unset). setRoute moves a CC already on another lane (that lane goes to
    // none) and is false, changing nothing, for a lane that isn't routable
    // or a CC that isn't assignable (MidiRoutes::assignable).
    const MidiRoutes &routes() const { return m_routes; }
    bool setRoute(int lane, int cc);

    // MIDI Learn (docs/midi-controls/PLAN.md J1): the next assignable CC any
    // port sends is routed to lane (a CC that drove another lane moves).
    // While armed, every assignable CC is kept from live play, so turning
    // the knob doesn't change the song; the script tap still sees it. The
    // MIDI threads fill a one-message slot that a UI timer polls; the result
    // arrives as learnFinished, or learnCancelled when cancelLearn() is
    // called, another lane starts learning, or timeoutMs passes.
    static constexpr int kLearnTimeoutMs = 10000;
    static constexpr int kLearnPollMs = 15;
    void startLearn(int lane, int timeoutMs = kLearnTimeoutMs);
    void cancelLearn();
    // The lane being learned, or MidiRoutes::kNone.
    int learnLane() const { return m_learnLane; }

    // The audio thread's end of the input (step 2): drain with
    // ring().pop(&msg) at the top of each callback.
    MidiInputRing &ring() { return m_ring; }
    const MidiInputRing &ring() const { return m_ring; }

    // One raw message as RtMidi delivers it (complete, running status
    // already expanded) into *out. False for everything porydaw ignores:
    // polyphonic and channel aftertouch, program change, system messages,
    // and truncated input.
    static bool parse(const unsigned char *bytes, size_t size, MidiMsg *out);

    // The script tap (porydaw.midi, docs/midi-input/PLAN.md step 6). While
    // enabled (some plugin listens for midi.message), every channel-voice
    // message a port sends also goes, as sent, into a second ring the UI
    // thread drains: one queued hop at a time posts tapReady(), and the
    // receiver drains everything with popTap. Live play and recording use
    // the input ring and never see this one. Disabled, the MIDI threads skip
    // it entirely; enabling or disabling empties it (nothing stale arrives).
    void setTapEnabled(bool on);
    bool tapEnabled() const { return m_tapEnabled.load(std::memory_order_relaxed); }
    // UI thread only. False when the tap is empty.
    bool popTap(MidiTapMsg *out) { return m_tap.pop(out); }
    // Messages lost to a full tap (the UI thread was busy for 1024 of them).
    uint64_t tapDropped() const { return m_tap.dropped(); }
    // The port identity a MidiTapMsg::port token stands for ("" for 0).
    QString portForToken(uint16_t token) const;
    // The tap's view of one raw message: false for system messages (sysex,
    // clock and active sensing never arrive: ignoreTypes), bare data bytes
    // and truncated input.
    static bool parseTap(const unsigned char *bytes, size_t size, MidiTapMsg *out);

    // Harness entry: the message's bytes take the same path as a port's
    // callback (Learn, parse, ring push, tap, activity), as if port (an identity;
    // empty = none) sent them. Its arrivalNs is kept as given: 0 (the
    // default) means it waited for no callback.
    void injectForTest(const MidiMsg &msg, const QString &port = QString());
    // The same with raw bytes (a two-byte program change, say).
    void injectBytesForTest(const std::vector<unsigned char> &bytes,
                            const QString &port = QString());
    // Harness entry: replaces the listed ports (raw names, plain naming;
    // opening none), so the Settings page's port list can be driven without
    // MIDI hardware.
    void setPortListForTest(const QStringList &names);
    // Ports reported as open (openPorts) without a device behind them, so
    // a harness can show what depends on a keyboard being connected.
    void setOpenPortsForTest(const QStringList &names);
    // Harness entry: reports a backend as up (named name) without one, so
    // the Settings page's no-backend → backend transition can be driven.
    void setBackendForTest(const QString &name);
    // Messages that reached the ring (any thread's count).
    uint64_t receivedCount() const { return m_received.load(std::memory_order_relaxed); }

  signals:
    // The port list or the open set changed.
    void portsChanged();
    // At least one open port closed (unplugged, disabled, reopened by
    // Refresh): whatever keys it held can never send their note-offs.
    void portsClosed();
    // Input arrived (throttled, UI thread).
    void activity();
    // The script tap has messages (UI thread; at most one hop in flight).
    void tapReady();
    void settingsChanged();
    // Learn routed cc to lane; movedFrom is the lane cc drove before, or
    // MidiRoutes::kNone. settingsChanged() has already been emitted.
    void learnFinished(int lane, int cc, int movedFrom);
    // Learn for lane stopped without a CC: cancelled, or timed out.
    void learnCancelled(int lane, bool timedOut);

  private:
    struct OpenPort;
    // One port of a scan: RtMidi's index and name, and porydaw's identity.
    struct ScannedPort {
        unsigned int index = 0;
        QString raw;
        QString id;
    };

    static void onMessage(double timeStamp, std::vector<unsigned char> *message, void *userData);
    // Any thread: parse, push, flag activity. port is the sender's token.
    void receive(const unsigned char *bytes, size_t size, int64_t arrivalNs, uint16_t port);
    void deliverActivity();
    void deliverTap();
    // UI thread: the token for a port identity, assigned on first use and
    // kept (the list only grows, by one per identity ever opened).
    uint16_t portToken(const QString &id);
    bool startBackend();
    // Lists the ports again, closes vanished ones and opens enabled new
    // ones; userAsked also reopens open ports, retries failed ones and
    // always reports.
    void rescan(bool userAsked);
    bool openQuietly(const QString &name);
    bool closeQuietly(const QString &name);
    // Closes the port's RtMidi client and frees it.
    static void shutPort(std::unique_ptr<OpenPort> &port);
    std::vector<ScannedPort> scanPorts() const;
    static std::vector<ScannedPort> scanned(const QStringList &raw, PortNaming naming);
    static QStringList rawNames(const std::vector<ScannedPort> &scan);
    void saveDisabledPorts();
    void saveRoutes();
    // UI thread: takes a learned CC from the slot, if one arrived.
    void pollLearn();
    // Disarms the MIDI threads' side and stops the timers.
    void stopLearn();

    MidiInputRing m_ring;
    MidiTapRing m_tap;
    std::atomic<bool> m_tapEnabled{false};
    // Set by the producer that posts the tap hop, cleared on the UI thread
    // just before tapReady() (so a message pushed during the drain posts
    // again).
    std::atomic<bool> m_tapPosted{false};
    // Token → identity (index 0 = none). UI thread only; tokens reach the
    // MIDI threads through OpenPort::token, set before the port opens.
    QStringList m_tokenNames{QString()};
    // The enumerating client; null without a backend.
    std::unique_ptr<RtMidiIn> m_backend;
    int m_api = 0; // RtMidi::Api of m_backend
    PortNaming m_naming = PortNaming::Plain;
    QString m_backendName;
    QString m_backendStatus;
    bool m_testBackend = false;
    QStringList m_testOpen;
    std::vector<ScannedPort> m_scan;
    std::vector<std::unique_ptr<OpenPort>> m_open;
    QStringList m_failed;
    // midi/disabledPorts, read once and kept in step with every write.
    QStringList m_disabled;
    QTimer *m_poll = nullptr;

    bool m_livePlay = true;
    int m_fixedVelocity = 0;
    int m_latencyMs = kLatencyAuto;
    MidiRoutes m_routes;

    // Learn: armed (UI writes, MIDI threads read) and the first assignable
    // CC to arrive while armed (any producer CASes it in from empty, -1; the
    // UI thread takes it).
    std::atomic<bool> m_learnArmed{false};
    std::atomic<int> m_learnSlot{-1};
    int m_learnLane = MidiRoutes::kNone;
    QTimer *m_learnPoll = nullptr;
    QTimer *m_learnTimeout = nullptr;

    std::atomic<uint64_t> m_received{0};
    // Set by the producer that posts the activity hop; cleared on the UI
    // thread when activity() is emitted, so at most one hop is in flight.
    std::atomic<bool> m_activityPosted{false};
    QTimer *m_activityTimer = nullptr;
    QElapsedTimer m_sinceActivity;
};
