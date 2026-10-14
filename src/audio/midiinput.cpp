#include "midiinput.h"

#include <QDebug>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

#ifdef PORYDAW_MIDI
#include "RtMidi.h"
// Port identities rely on WinMM reporting the device's own name, without
// RtMidi's appended device index (CMakeLists.txt defines this for rtmidi and
// its users).
#if defined(_WIN32) && !defined(RTMIDI_DO_NOT_ENSURE_UNIQUE_PORTNAMES)
#error "RtMidi must be built with RTMIDI_DO_NOT_ENSURE_UNIQUE_PORTNAMES on Windows"
#endif
#else
// Built without RtMidi: the type only has to be complete for unique_ptr.
class RtMidiIn
{};
#endif

namespace {

const QString kDisabledPortsKey = QStringLiteral("midi/disabledPorts");
const QString kSurfacePortsKey = QStringLiteral("midi/surfacePorts");
const QString kKeyboardPortsKey = QStringLiteral("midi/keyboardPorts");
const QString kLivePlayKey = QStringLiteral("midi/livePlay");
const QString kFixedVelocityKey = QStringLiteral("midi/fixedVelocity");
const QString kLatencyKey = QStringLiteral("midi/latencyMs");
const QString kRoutesKey = QStringLiteral("midi/routes");

// The ALSA client name other MIDI tools show for porydaw's connections.
const char *const kClientName = "porydaw";
const char *const kPortName = "porydaw input";

#ifdef PORYDAW_MIDI
// RtMidi reports through this instead of throwing once it is installed.
// It can run on a port's callback thread, so it only logs.
void onRtMidiError(RtMidiError::Type type, const std::string &text, void *)
{
    if (type == RtMidiError::WARNING || type == RtMidiError::DEBUG_WARNING)
        return;
    qWarning("MIDI: %s", text.c_str());
}
#endif

} // namespace

// One open port: its own RtMidiIn client (RtMidi opens one port per
// object), whose callback thread carries this as its user data.
struct MidiInput::OpenPort {
    MidiInput *owner = nullptr;
    QString id;
    // The raw port list it was opened from (see identityStable).
    QStringList rawAtOpen;
    // Set before the client closes; a callback already running on the
    // driver's thread then drops its message (see shutPort).
    std::atomic<bool> closing{false};
    uint16_t token = 0; // portToken(id), for the script tap
    // Its role is Surface. Written on the UI thread (setPortRole), read once
    // per message by the callback, so a role change needs no reopen.
    std::atomic<bool> surface{false};
    std::unique_ptr<RtMidiIn> in;
};

QString MidiInput::portBaseName(const QString &raw, PortNaming naming)
{
    if (naming != PortNaming::Alsa)
        return raw;
    // RtMidi's ALSA names end in " client:port"; the numbers are assigned
    // as devices appear, so a replugged keyboard gets new ones.
    static const QRegularExpression suffix(QStringLiteral(" \\d+:\\d+$"));
    QString base = raw;
    base.remove(suffix);
    return base.isEmpty() ? raw : base;
}

QStringList MidiInput::portIdentities(const QStringList &raw, PortNaming naming)
{
    QStringList ids;
    QHash<QString, int> seen;
    QSet<QString> taken;
    for (const QString &name : raw) {
        if (name.isEmpty()) {
            ids.append(QString());
            continue;
        }
        const QString base = portBaseName(name, naming);
        int n = seen.value(base) + 1;
        QString id = n == 1 ? base : QStringLiteral("%1 (%2)").arg(base).arg(n);
        // A device literally named "Base (2)" keeps its name; the counted
        // one moves on to the next free number.
        while (taken.contains(id))
            id = QStringLiteral("%1 (%2)").arg(base).arg(++n);
        seen.insert(base, n);
        taken.insert(id);
        ids.append(id);
    }
    return ids;
}

bool MidiInput::identityStable(const QStringList &before, const QStringList &now, const QString &id,
                               PortNaming naming)
{
    const int at = portIdentities(now, naming).indexOf(id);
    if (at < 0)
        return false;
    const QString base = portBaseName(now[at], naming);
    const auto sameNamed = [&](const QStringList &raw) {
        QStringList ports;
        for (const QString &name : raw) {
            if (!name.isEmpty() && portBaseName(name, naming) == base)
                ports.append(name);
        }
        return ports;
    };
    return sameNamed(before) == sameNamed(now);
}

MidiInput::PortRole MidiInput::defaultRole(const QString &name)
{
    // Letters on neither side, so "DAW" in "KeyLab Essential 61 DAW" or
    // "MIDI DAW Out" counts and "Dawson" or "Pseudawn" don't. Digits and
    // punctuation are boundaries: "MCU1", "LKMK3_DAW".
    static const QRegularExpression surfaceWord(
        QStringLiteral("(?<![A-Za-z])(DAW|MCU|Mackie)(?![A-Za-z])"),
        QRegularExpression::CaseInsensitiveOption);
    if (surfaceWord.match(name).hasMatch())
        return PortRole::Surface;
    // Windows names a device's second port "MIDIIN2 (<device>)". Narrowed to
    // KeyLabs (whose second port is their DAW port): a multi-port
    // interface's second DIN input is named the same way and is a keyboard.
    if (name.startsWith(QStringLiteral("MIDIIN2 ("), Qt::CaseInsensitive) &&
        name.contains(QStringLiteral("KeyLab"), Qt::CaseInsensitive))
        return PortRole::Surface;
    return PortRole::Keyboard;
}

MidiInput::MidiInput(QObject *parent) : QObject(parent)
{
    QSettings settings;
    m_disabled = settings.value(kDisabledPortsKey).toStringList();
    m_surfacePorts = settings.value(kSurfacePortsKey).toStringList();
    m_keyboardPorts = settings.value(kKeyboardPortsKey).toStringList();
    // An identity in both (an edited settings file) is a surface: the safe
    // reading, since it then never plays.
    for (const QString &name : std::as_const(m_surfacePorts))
        m_keyboardPorts.removeAll(name);
    m_livePlay = settings.value(kLivePlayKey, true).toBool();
    m_fixedVelocity = std::clamp(settings.value(kFixedVelocityKey, 0).toInt(), 0, 127);
    m_latencyMs =
        std::clamp(settings.value(kLatencyKey, kLatencyAuto).toInt(), kLatencyAuto, kLatencyMaxMs);
    if (settings.contains(kRoutesKey))
        m_routes = MidiRoutes::fromStrings(settings.value(kRoutesKey).toStringList());

    m_learnPoll = new QTimer(this);
    m_learnPoll->setInterval(kLearnPollMs);
    connect(m_learnPoll, &QTimer::timeout, this, &MidiInput::pollLearn);
    m_learnTimeout = new QTimer(this);
    m_learnTimeout->setSingleShot(true);
    connect(m_learnTimeout, &QTimer::timeout, this, [this] {
        pollLearn(); // a CC that arrived just now still counts
        if (m_learnLane == MidiRoutes::kNone)
            return;
        const int lane = m_learnLane;
        stopLearn();
        emit learnCancelled(lane, /*timedOut=*/true);
    });

    m_activityTimer = new QTimer(this);
    m_activityTimer->setSingleShot(true);
    connect(m_activityTimer, &QTimer::timeout, this, &MidiInput::deliverActivity);

    m_poll = new QTimer(this);
    m_poll->setInterval(kPollIntervalMs);
    // Hot-plug only: a port that failed to open stays closed until it
    // reappears or the user asks again (Refresh, its checkbox), so a device
    // held by another program doesn't log every two seconds.
    connect(m_poll, &QTimer::timeout, this, [this] { rescan(/*userAsked=*/false); });

    if (startBackend()) {
        m_scan = scanPorts();
        for (const ScannedPort &port : m_scan) {
            if (isPortEnabled(port.id))
                openQuietly(port.id);
        }
        m_poll->start();
    }
}

MidiInput::~MidiInput()
{
    // See shutPort for what closing guarantees per backend.
    for (auto &port : m_open)
        shutPort(port);
    m_open.clear();
}

void MidiInput::shutPort(std::unique_ptr<OpenPort> &port)
{
    if (!port)
        return;
    // ALSA's closePort joins its input thread, so no callback runs once it
    // returns. WinMM (midiInReset/midiInClose) and CoreMIDI
    // (MIDIPortDispose) only stop new callbacks; one already running on the
    // driver's thread is not waited for. The flag makes such a callback
    // drop its message instead of producing, which narrows (but cannot
    // close) the window in which it still reads this port.
    port->closing.store(true, std::memory_order_release);
#ifdef PORYDAW_MIDI
    if (port->in)
        port->in->closePort();
#endif
    port.reset();
}

bool MidiInput::startBackend()
{
#ifdef PORYDAW_MIDI
    // RtMidi's dummy API (no platform backend at build time) is never in
    // the compiled list; asking for UNSPECIFIED then throws.
    std::vector<RtMidi::Api> apis;
    RtMidi::getCompiledApi(apis);
    if (apis.empty()) {
        m_backendStatus = tr("This build of porydaw has no MIDI support, so no MIDI "
                             "keyboard can be used.");
        return false;
    }
    try {
        auto in = std::make_unique<RtMidiIn>(RtMidi::UNSPECIFIED, kClientName);
        if (in->getCurrentApi() == RtMidi::RTMIDI_DUMMY) {
            m_backendStatus = tr("This build of porydaw has no MIDI support, so no MIDI "
                                 "keyboard can be used.");
            return false;
        }
        in->setErrorCallback(&onRtMidiError, nullptr);
        m_api = int(in->getCurrentApi());
        m_naming = m_api == int(RtMidi::LINUX_ALSA) ? PortNaming::Alsa : PortNaming::Plain;
        m_backendName = QString::fromStdString(RtMidi::getApiDisplayName(RtMidi::Api(m_api)));
        m_backend = std::move(in);
        m_backendStatus.clear();
        return true;
    } catch (const std::exception &error) {
        // RtMidiError included. Its text names the failing driver call.
        QString detail = QString::fromLocal8Bit(error.what()).trimmed();
        while (detail.endsWith(QLatin1Char('.')))
            detail.chop(1);
        m_backendStatus = tr("No MIDI system is available on this computer, so no MIDI "
                             "keyboard can be used.");
#ifdef Q_OS_LINUX
        m_backendStatus += QLatin1Char(' ') + tr("Porydaw uses the ALSA sequencer "
                                                 "(/dev/snd/seq); WSL does not provide one.");
#endif
        if (!detail.isEmpty())
            m_backendStatus += QStringLiteral("\n\n") + tr("Details: %1").arg(detail);
    }
    return false;
#else
    m_backendStatus =
        tr("This build of porydaw has no MIDI support, so no MIDI keyboard can be used.");
    return false;
#endif
}

std::vector<MidiInput::ScannedPort> MidiInput::scanPorts() const
{
    QStringList raw;
#ifdef PORYDAW_MIDI
    if (!m_backend)
        return {};
    try {
        // A port that vanished between the count and the name reads back
        // empty and is skipped.
        const unsigned int count = m_backend->getPortCount();
        for (unsigned int i = 0; i < count; ++i)
            raw.append(QString::fromStdString(m_backend->getPortName(i)));
    } catch (const std::exception &error) {
        qWarning("MIDI: listing ports failed: %s", error.what());
    }
#endif
    return scanned(raw, m_naming);
}

std::vector<MidiInput::ScannedPort> MidiInput::scanned(const QStringList &raw, PortNaming naming)
{
    std::vector<ScannedPort> ports;
    const QStringList ids = portIdentities(raw, naming);
    for (int i = 0; i < raw.size(); ++i) {
        if (!ids[i].isEmpty())
            ports.push_back({unsigned(i), raw[i], ids[i]});
    }
    return ports;
}

QStringList MidiInput::rawNames(const std::vector<ScannedPort> &scan)
{
    QStringList raw;
    for (const ScannedPort &port : scan)
        raw.append(port.raw);
    return raw;
}

QStringList MidiInput::ports() const
{
    QStringList ids;
    for (const ScannedPort &port : m_scan)
        ids.append(port.id);
    return ids;
}

QStringList MidiInput::openPorts() const
{
    QStringList names;
    for (const auto &port : m_open)
        names.append(port->id);
    names.append(m_testOpen);
    return names;
}

QStringList MidiInput::openKeyboardPorts() const
{
    QStringList names;
    for (const QString &name : openPorts()) {
        if (portRole(name) == PortRole::Keyboard)
            names.append(name);
    }
    return names;
}

bool MidiInput::isPortOpen(const QString &name) const
{
    return std::any_of(m_open.begin(), m_open.end(),
                       [&name](const auto &port) { return port->id == name; });
}

bool MidiInput::isPortEnabled(const QString &name) const
{
    return !m_disabled.contains(name);
}

void MidiInput::saveDisabledPorts()
{
    QSettings settings;
    if (m_disabled.isEmpty())
        settings.remove(kDisabledPortsKey);
    else
        settings.setValue(kDisabledPortsKey, m_disabled);
}

void MidiInput::setPortEnabled(const QString &name, bool enabled)
{
    const bool wasEnabled = !m_disabled.contains(name);
    if (wasEnabled != enabled) {
        if (enabled)
            m_disabled.removeAll(name);
        else
            m_disabled.append(name);
        saveDisabledPorts();
    }
    m_failed.removeAll(name);
    if (enabled)
        openQuietly(name);
    else
        closeQuietly(name);
    emit portsChanged(); // the page shows the setting even when nothing opened
}

MidiInput::PortRole MidiInput::portRole(const QString &name) const
{
    if (m_surfacePorts.contains(name))
        return PortRole::Surface;
    if (m_keyboardPorts.contains(name))
        return PortRole::Keyboard;
    return defaultRole(name);
}

void MidiInput::savePortRoles()
{
    QSettings settings;
    if (m_surfacePorts.isEmpty())
        settings.remove(kSurfacePortsKey);
    else
        settings.setValue(kSurfacePortsKey, m_surfacePorts);
    if (m_keyboardPorts.isEmpty())
        settings.remove(kKeyboardPortsKey);
    else
        settings.setValue(kKeyboardPortsKey, m_keyboardPorts);
}

void MidiInput::setPortRole(const QString &name, PortRole role)
{
    if (name.isEmpty())
        return;
    const PortRole was = portRole(name);
    QStringList &mine = role == PortRole::Surface ? m_surfacePorts : m_keyboardPorts;
    QStringList &other = role == PortRole::Surface ? m_keyboardPorts : m_surfacePorts;
    // Kept even when it matches the name default: it is the user's choice
    // now, whatever a later default says.
    if (!mine.contains(name) || other.contains(name)) {
        other.removeAll(name);
        if (!mine.contains(name))
            mine.append(name);
        savePortRoles();
    }
    if (was == role)
        return;
    // An open port flips in place: its callback routes each message by one
    // read of OpenPort::surface. A key it holds as a keyboard would now
    // release into the surface ring, and a held surface button into live
    // play, so report it closed: held notes and a held repeat are let go.
    for (auto &port : m_open) {
        if (port->id == name)
            port->surface.store(role == PortRole::Surface, std::memory_order_release);
    }
    const bool open = openPorts().contains(name);
    emit settingsChanged();
    emit portsChanged(); // the open keyboards changed, and the page shows the role
    if (open)
        emit portsClosed();
}

bool MidiInput::openPort(const QString &name)
{
    if (isPortOpen(name))
        return true;
    // An explicit open is the user asking again: forget an earlier failure.
    m_failed.removeAll(name);
    const bool opened = openQuietly(name);
    emit portsChanged(); // a failure changes what the page shows too
    return opened;
}

bool MidiInput::openQuietly(const QString &name)
{
#ifdef PORYDAW_MIDI
    if (!m_backend || isPortOpen(name))
        return false;
    // Looked up afresh: a hot-plug since the last scan moves indices.
    const std::vector<ScannedPort> now = scanPorts();
    const auto found = std::find_if(now.begin(), now.end(),
                                    [&name](const ScannedPort &port) { return port.id == name; });
    if (found == now.end())
        return false;

    auto port = std::make_unique<OpenPort>();
    port->owner = this;
    port->id = name;
    port->rawAtOpen = rawNames(now);
    port->token = portToken(name);
    port->surface.store(portRole(name) == PortRole::Surface, std::memory_order_release);
    try {
        port->in = std::make_unique<RtMidiIn>(RtMidi::Api(m_api), kClientName);
        port->in->setErrorCallback(&onRtMidiError, nullptr);
        // Sysex, clock/timing and active sensing never reach the callback.
        port->in->ignoreTypes(true, true, true);
        // No WinMM sysex buffers either (the count only matters there):
        // sysex is ignored anyway, and closing an unplugged device is where
        // RtMidi's buffer bookkeeping crashed (see external/rtmidi/LICENSES.md).
        port->in->setBufferSize(1024, 0);
        port->in->setCallback(&MidiInput::onMessage, port.get());
        port->in->openPort(found->index, kPortName);
    } catch (const std::exception &error) {
        qWarning("MIDI: opening \"%s\" failed: %s", qUtf8Printable(name), error.what());
        port->in.reset();
    }
    if (port->in && port->in->isPortOpen()) {
        // A device plugged or unplugged between the lookup and the open
        // shifts the indices: make sure the index opened is still the port
        // looked up. Not marked failed, so the next poll tries again.
        QString opened;
        try {
            opened = QString::fromStdString(m_backend->getPortName(found->index));
        } catch (const std::exception &) {
        }
        if (opened != found->raw) {
            qWarning("MIDI: \"%s\" moved while it was being opened; retrying",
                     qUtf8Printable(name));
            shutPort(port);
            return false;
        }
    }
    if (!port->in || !port->in->isPortOpen()) {
        if (!m_failed.contains(name))
            m_failed.append(name);
        return false;
    }
    m_failed.removeAll(name);
    m_open.push_back(std::move(port));
    return true;
#else
    Q_UNUSED(name);
    return false;
#endif
}

void MidiInput::closePort(const QString &name)
{
    m_failed.removeAll(name);
    if (closeQuietly(name))
        emit portsChanged();
}

bool MidiInput::closeQuietly(const QString &name)
{
    const auto it = std::find_if(m_open.begin(), m_open.end(),
                                 [&name](const auto &port) { return port->id == name; });
    if (it == m_open.end())
        return false;
    shutPort(*it);
    m_open.erase(it);
    emit portsClosed();
    return true;
}

void MidiInput::closeAll(bool quietly)
{
    if (m_open.empty())
        return;
    for (auto &port : m_open)
        shutPort(port);
    m_open.clear();
    if (quietly)
        return;
    emit portsClosed();
    emit portsChanged();
}

bool MidiInput::portFailed(const QString &name) const
{
    return m_failed.contains(name);
}

void MidiInput::refresh()
{
    if (!backendAvailable()) {
        if (!startBackend()) {
            emit portsChanged(); // the reason may read differently now
            return;
        }
        m_poll->start();
    }
    rescan(/*userAsked=*/true);
}

void MidiInput::rescan(bool userAsked)
{
    bool closed = false;
    if (userAsked) {
        m_failed.clear();
        // Reopen everything: a device replugged at the same index between
        // two polls lists exactly as before, but its old handle hears
        // nothing.
        closed = !m_open.empty();
        for (auto &port : m_open)
            shutPort(port);
        m_open.clear();
    }
    std::vector<ScannedPort> now = scanPorts();
    const QStringList before = rawNames(m_scan);
    const QStringList raw = rawNames(now);
    bool changed = userAsked || raw != before;
    // An open port stays open while its identity still names the device
    // it opened (identityStable). With two identical keyboards, unplugging
    // either closes both, and the one still there reopens below.
    for (auto it = m_open.begin(); it != m_open.end();) {
        if (identityStable((*it)->rawAtOpen, raw, (*it)->id, m_naming)) {
            ++it;
        } else {
            shutPort(*it);
            it = m_open.erase(it);
            closed = changed = true;
        }
    }
    // A failed identity that vanished, or may be another device now, is
    // forgotten: it is tried again.
    for (auto it = m_failed.begin(); it != m_failed.end();)
        it = identityStable(before, raw, *it, m_naming) ? std::next(it) : m_failed.erase(it);
    m_scan = std::move(now);
    for (const QString &id : ports()) {
        if (isPortEnabled(id) && !isPortOpen(id) && !m_failed.contains(id))
            changed |= openQuietly(id);
    }
    if (closed)
        emit portsClosed();
    if (changed)
        emit portsChanged();
}

void MidiInput::setLivePlay(bool on)
{
    if (m_livePlay == on)
        return;
    m_livePlay = on;
    QSettings().setValue(kLivePlayKey, on);
    emit settingsChanged();
}

void MidiInput::setFixedVelocity(int velocity)
{
    velocity = std::clamp(velocity, 0, 127);
    if (m_fixedVelocity == velocity)
        return;
    m_fixedVelocity = velocity;
    QSettings().setValue(kFixedVelocityKey, velocity);
    emit settingsChanged();
}

void MidiInput::setLatencyMs(int ms)
{
    ms = std::clamp(ms, kLatencyAuto, kLatencyMaxMs);
    if (m_latencyMs == ms)
        return;
    m_latencyMs = ms;
    QSettings().setValue(kLatencyKey, ms);
    emit settingsChanged();
}

void MidiInput::saveRoutes()
{
    QSettings settings;
    if (m_routes == MidiRoutes())
        settings.remove(kRoutesKey);
    else
        settings.setValue(kRoutesKey, m_routes.toStrings());
}

bool MidiInput::setRoute(int lane, int cc, int *movedFrom)
{
    MidiRoutes routes = m_routes;
    if (!routes.assign(lane, cc, movedFrom))
        return false;
    if (routes != m_routes) {
        m_routes = routes;
        saveRoutes();
        emit settingsChanged();
    }
    return true;
}

void MidiInput::startLearn(int lane, int timeoutMs)
{
    if (MidiRoutes::laneIndex(lane) < 0)
        return;
    if (m_learnLane != MidiRoutes::kNone) {
        const int was = m_learnLane;
        stopLearn();
        emit learnCancelled(was, /*timedOut=*/false);
    }
    m_learnLane = lane;
    if (++m_learnGen == 0) // 0 is "disarmed"
        m_learnGen = 1;
    // Emptied before arming; a late CC from an earlier learn that still gets
    // in carries that learn's generation and is dropped by the poll.
    m_learnSlot.store(0, std::memory_order_relaxed);
    m_learnArmedGen.store(m_learnGen, std::memory_order_release);
    m_learnPoll->start();
    m_learnTimeout->start(std::max(timeoutMs, 1));
    emit learnStarted(lane);
}

void MidiInput::cancelLearn()
{
    if (m_learnLane == MidiRoutes::kNone)
        return;
    const int lane = m_learnLane;
    stopLearn();
    emit learnCancelled(lane, /*timedOut=*/false);
}

void MidiInput::stopLearn()
{
    m_learnArmedGen.store(0, std::memory_order_release);
    m_learnSlot.store(0, std::memory_order_relaxed);
    m_learnLane = MidiRoutes::kNone;
    m_learnPoll->stop();
    m_learnTimeout->stop();
}

void MidiInput::pollLearn()
{
    if (m_learnLane == MidiRoutes::kNone)
        return;
    const uint64_t taken = m_learnSlot.exchange(0, std::memory_order_acq_rel);
    if (taken == 0 || uint32_t(taken >> 8) != m_learnGen)
        return; // empty, or a straggler from an earlier learn (see m_learnSlot)
    const int cc = int(taken & 0x7F);
    const int lane = m_learnLane;
    stopLearn();
    MidiRoutes routes = m_routes;
    int movedFrom = MidiRoutes::kNone;
    routes.assign(lane, cc, &movedFrom); // the MIDI threads only take assignable CCs
    if (routes != m_routes) {
        m_routes = routes;
        saveRoutes();
        emit settingsChanged();
    }
    emit learnFinished(lane, cc, movedFrom);
}

bool MidiInput::parse(const unsigned char *bytes, size_t size, MidiMsg *out)
{
    if (!bytes || size < 2)
        return false; // every type porydaw keeps has a data byte
    const uint8_t status = bytes[0];
    if (status < 0x80)
        return false; // RtMidi expands running status; a bare data byte is garbage
    const uint8_t d1 = bytes[1] & 0x7F;
    switch (status & 0xF0) {
    case 0xC0: // program change: the target track's voice (one data byte)
        *out = {status, d1, 0};
        return true;
    case 0x90:
    case 0x80:
    case 0xB0:
    case 0xE0:
        break;
    default:
        // 0xA0 polyphonic and 0xD0 channel aftertouch, 0xF0 system.
        return false;
    }
    if (size < 3)
        return false; // these have two data bytes
    const uint8_t d2 = bytes[2] & 0x7F;
    if ((status & 0xF0) == 0x90 && d2 == 0) { // note on with velocity 0 is a note off
        *out = {uint8_t(0x80 | (status & 0x0F)), d1, 0};
        return true;
    }
    *out = {status, d1, d2};
    return true;
}

void MidiInput::onMessage(double, std::vector<unsigned char> *message, void *userData)
{
    auto *port = static_cast<OpenPort *>(userData);
    if (!port || port->closing.load(std::memory_order_acquire))
        return;
    if (message && !message->empty())
        port->owner->receive(message->data(), message->size(), midiClockNs(), port->token,
                             port->surface.load(std::memory_order_acquire));
}

void MidiInput::injectForTest(const MidiMsg &msg, const QString &port)
{
    const unsigned char bytes[3] = {msg.status, msg.d1, msg.d2};
    receive(bytes, sizeof bytes, msg.arrivalNs, port.isEmpty() ? 0 : portToken(port),
            !port.isEmpty() && portRole(port) == PortRole::Surface);
}

void MidiInput::injectBytesForTest(const std::vector<unsigned char> &bytes, const QString &port)
{
    receive(bytes.data(), bytes.size(), 0, port.isEmpty() ? 0 : portToken(port),
            !port.isEmpty() && portRole(port) == PortRole::Surface);
}

bool MidiInput::parseTap(const unsigned char *bytes, size_t size, MidiTapMsg *out)
{
    if (!bytes || size < 1)
        return false;
    const uint8_t status = bytes[0];
    if (status < 0x80 || status >= 0xF0)
        return false; // a bare data byte, or a system message
    const uint8_t type = status & 0xF0;
    const size_t dataBytes = (type == 0xC0 || type == 0xD0) ? 1 : 2;
    if (size < 1 + dataBytes)
        return false;
    out->status = status;
    out->d1 = bytes[1] & 0x7F;
    out->d2 = dataBytes == 2 ? uint8_t(bytes[2] & 0x7F) : uint8_t(0);
    return true;
}

void MidiInput::setTapEnabled(bool on)
{
    if (m_tapEnabled.load(std::memory_order_relaxed) == on)
        return;
    m_tapEnabled.store(on, std::memory_order_release);
    // Empty either way: switched off, nobody will read it; switched on,
    // what a producer slipped in around the flip belongs to no listener.
    MidiTapMsg discard;
    while (m_tap.pop(&discard)) {
    }
}

QString MidiInput::portForToken(uint16_t token) const
{
    return token < m_tokenNames.size() ? m_tokenNames[token] : QString();
}

uint16_t MidiInput::portToken(const QString &id)
{
    if (id.isEmpty())
        return 0;
    const qsizetype at = m_tokenNames.indexOf(id);
    if (at > 0)
        return uint16_t(at);
    if (m_tokenNames.size() > 0xFFFF)
        return 0; // 65535 distinct identities: report later ones as none
    m_tokenNames.append(id);
    return uint16_t(m_tokenNames.size() - 1);
}

void MidiInput::deliverTap()
{
    // Cleared first: a message pushed while the receiver drains posts a
    // new hop, so nothing waits for the next one. An exchange, not a
    // store: the drain's later read of the ring can't move ahead of it, so
    // either it sees a producer's push or that producer sees false and
    // posts (a plain store-then-load could strand a message until the next).
    m_tapPosted.exchange(false, std::memory_order_acq_rel);
    emit tapReady();
}

void MidiInput::deliverSurface()
{
    // As deliverTap.
    m_surfacePosted.exchange(false, std::memory_order_acq_rel);
    emit surfaceReady();
}

void MidiInput::setPortListForTest(const QStringList &names)
{
    m_scan = scanned(names, PortNaming::Plain);
    emit portsChanged();
}

void MidiInput::setOpenPortsForTest(const QStringList &names)
{
    m_testOpen = names;
    emit portsChanged();
}

void MidiInput::setBackendForTest(const QString &name)
{
    m_testBackend = !name.isEmpty();
    if (m_testBackend) {
        m_backendName = name;
        m_backendStatus.clear();
    } else if (!m_backend && startBackend()) {
        m_poll->start();
    }
    emit portsChanged();
}

int64_t midiClockNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void MidiInput::receive(const unsigned char *bytes, size_t size, int64_t arrivalNs, uint16_t port,
                        bool surface)
{
    // A control surface's buttons are notes and CCs too, but they never
    // play, record, light the keyboard or get learned: only the surface
    // ring and the script tap see them (docs/control-surface/PLAN.md J1).
    if (surface) {
        MidiTapMsg msg;
        if (parseTap(bytes, size, &msg)) {
            msg.port = port;
            m_surface.push(msg);
            // One queued hop at a time; deliverSurface re-arms it.
            if (!m_surfacePosted.exchange(true, std::memory_order_acq_rel))
                QMetaObject::invokeMethod(this, &MidiInput::deliverSurface, Qt::QueuedConnection);
        }
    }
    // Learn: an assignable CC while armed is the one to learn (the first
    // to arrive) and never reaches live play, so the knob being learned
    // doesn't move the song. The tap below still sees it. A surface's
    // messages are swallowed outright.
    bool swallow = surface;
    const uint32_t learnGen = m_learnArmedGen.load(std::memory_order_acquire);
    if (!surface && learnGen != 0 && bytes && size >= 3 && (bytes[0] & 0xF0) == 0xB0 &&
        MidiRoutes::assignable(bytes[1] & 0x7F)) {
        uint64_t empty = 0;
        m_learnSlot.compare_exchange_strong(empty, uint64_t(learnGen) << 8 | (bytes[1] & 0x7F),
                                            std::memory_order_acq_rel);
        swallow = true;
    }
    // Live play first: the script tap's lock and queued hop never delay
    // the audio thread's input.
    MidiMsg msg;
    if (!swallow && parse(bytes, size, &msg)) {
        msg.arrivalNs = arrivalNs;
        if (m_ring.push(msg))
            m_received.fetch_add(1, std::memory_order_relaxed);
        // One queued hop at a time; deliverActivity re-arms it.
        if (!m_activityPosted.exchange(true, std::memory_order_acq_rel))
            QMetaObject::invokeMethod(this, &MidiInput::deliverActivity, Qt::QueuedConnection);
    }
    // The script tap, independently: it keeps what live play drops
    // (aftertouch, system messages) and the bytes as sent.
    if (m_tapEnabled.load(std::memory_order_acquire)) {
        MidiTapMsg tap;
        if (parseTap(bytes, size, &tap)) {
            tap.port = port;
            m_tap.push(tap);
            if (!m_tapPosted.exchange(true, std::memory_order_acq_rel))
                QMetaObject::invokeMethod(this, &MidiInput::deliverTap, Qt::QueuedConnection);
        }
    }
}

void MidiInput::deliverActivity()
{
    // Read once: a second read can pass the interval and start the timer
    // with a negative wait, which never fires and strands the posted flag.
    const qint64 since = m_sinceActivity.isValid() ? m_sinceActivity.elapsed() : -1;
    if (since >= 0 && since < kActivityIntervalMs) {
        if (!m_activityTimer->isActive())
            m_activityTimer->start(int(std::max<qint64>(1, kActivityIntervalMs - since)));
        return;
    }
    m_sinceActivity.start();
    m_activityPosted.store(false, std::memory_order_release);
    emit activity();
}
