#include "midiinput.h"

#include <QDebug>
#include <QSettings>
#include <QTimer>

#include <algorithm>
#include <exception>

#ifdef PORYDAW_MIDI
#include "RtMidi.h"
#else
// Built without RtMidi: the type only has to be complete for unique_ptr.
class RtMidiIn
{};
#endif

namespace {

const QString kDisabledPortsKey = QStringLiteral("midi/disabledPorts");
const QString kLivePlayKey = QStringLiteral("midi/livePlay");
const QString kFixedVelocityKey = QStringLiteral("midi/fixedVelocity");
const QString kLatencyKey = QStringLiteral("midi/latencyMs");

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

bool MidiInputRing::push(const MidiMsg &msg)
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

bool MidiInputRing::pop(MidiMsg *out)
{
    const uint32_t read = m_read.load(std::memory_order_relaxed);
    const uint32_t write = m_write.load(std::memory_order_acquire);
    if (read == write)
        return false;
    *out = m_slots[read & (kCapacity - 1)];
    m_read.store(read + 1, std::memory_order_release);
    return true;
}

uint32_t MidiInputRing::size() const
{
    return m_write.load(std::memory_order_acquire) - m_read.load(std::memory_order_acquire);
}

// One open port: its own RtMidiIn client (RtMidi opens one port per
// object), whose callback thread carries this as its user data.
struct MidiInput::OpenPort {
    MidiInput *owner = nullptr;
    QString name;
    std::unique_ptr<RtMidiIn> in;
};

MidiInput::MidiInput(QObject *parent) : QObject(parent)
{
    QSettings settings;
    m_livePlay = settings.value(kLivePlayKey, true).toBool();
    m_fixedVelocity = std::clamp(settings.value(kFixedVelocityKey, 0).toInt(), 0, 127);
    m_latencyMs =
        std::clamp(settings.value(kLatencyKey, kLatencyAuto).toInt(), kLatencyAuto, kLatencyMaxMs);

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
        m_ports = scanPorts();
        for (const QString &name : m_ports) {
            if (isPortEnabled(name))
                openQuietly(name);
        }
        m_poll->start();
    }
}

MidiInput::~MidiInput()
{
    // Closing joins each port's callback thread, so nothing produces into
    // the ring (or touches this) past here.
    m_open.clear();
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

QStringList MidiInput::scanPorts() const
{
    QStringList names;
#ifdef PORYDAW_MIDI
    if (!m_backend)
        return names;
    try {
        const unsigned int count = m_backend->getPortCount();
        for (unsigned int i = 0; i < count; ++i) {
            // A port that vanished between the count and the name reads
            // back empty. Two identical names (rare: Windows and ALSA both
            // number their ports) are one identity; the first one opens.
            const QString name = QString::fromStdString(m_backend->getPortName(i));
            if (!name.isEmpty() && !names.contains(name))
                names.append(name);
        }
    } catch (const std::exception &error) {
        qWarning("MIDI: listing ports failed: %s", error.what());
    }
#endif
    return names;
}

QStringList MidiInput::openPorts() const
{
    QStringList names;
    for (const auto &port : m_open)
        names.append(port->name);
    return names;
}

bool MidiInput::isPortOpen(const QString &name) const
{
    return std::any_of(m_open.begin(), m_open.end(),
                       [&name](const auto &port) { return port->name == name; });
}

QStringList MidiInput::disabledPorts() const
{
    return QSettings().value(kDisabledPortsKey).toStringList();
}

bool MidiInput::isPortEnabled(const QString &name) const
{
    return !disabledPorts().contains(name);
}

void MidiInput::setPortEnabled(const QString &name, bool enabled)
{
    QStringList disabled = disabledPorts();
    const bool wasEnabled = !disabled.contains(name);
    if (wasEnabled != enabled) {
        if (enabled)
            disabled.removeAll(name);
        else
            disabled.append(name);
        QSettings settings;
        if (disabled.isEmpty())
            settings.remove(kDisabledPortsKey);
        else
            settings.setValue(kDisabledPortsKey, disabled);
    }
    m_failed.removeAll(name);
    if (enabled)
        openQuietly(name);
    else
        closeQuietly(name);
    emit portsChanged(); // the page shows the setting even when nothing opened
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
    int index = -1;
    try {
        const unsigned int count = m_backend->getPortCount();
        for (unsigned int i = 0; i < count && index < 0; ++i) {
            if (QString::fromStdString(m_backend->getPortName(i)) == name)
                index = int(i);
        }
    } catch (const std::exception &error) {
        qWarning("MIDI: listing ports failed: %s", error.what());
    }
    if (index < 0)
        return false;

    auto port = std::make_unique<OpenPort>();
    port->owner = this;
    port->name = name;
    try {
        port->in = std::make_unique<RtMidiIn>(RtMidi::Api(m_api), kClientName);
        port->in->setErrorCallback(&onRtMidiError, nullptr);
        // Sysex, clock/timing and active sensing never reach the callback.
        port->in->ignoreTypes(true, true, true);
        port->in->setCallback(&MidiInput::onMessage, port.get());
        port->in->openPort(unsigned(index), kPortName);
    } catch (const std::exception &error) {
        qWarning("MIDI: opening \"%s\" failed: %s", qUtf8Printable(name), error.what());
        port->in.reset();
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
                                 [&name](const auto &port) { return port->name == name; });
    if (it == m_open.end())
        return false;
    m_open.erase(it); // joins the port's callback thread
    return true;
}

void MidiInput::closeAll()
{
    if (m_open.empty())
        return;
    m_open.clear();
    emit portsChanged();
}

bool MidiInput::portFailed(const QString &name) const
{
    return m_failed.contains(name);
}

void MidiInput::refresh()
{
    if (!m_backend) {
        if (!startBackend())
            return;
        m_poll->start();
    }
    rescan(/*userAsked=*/true);
}

void MidiInput::rescan(bool userAsked)
{
    if (userAsked)
        m_failed.clear();
    const QStringList now = scanPorts();
    bool changed = userAsked || now != m_ports;
    m_ports = now;
    for (auto it = m_failed.begin(); it != m_failed.end();)
        it = m_ports.contains(*it) ? std::next(it) : m_failed.erase(it);
    for (auto it = m_open.begin(); it != m_open.end();) {
        if (m_ports.contains((*it)->name)) {
            ++it;
        } else {
            it = m_open.erase(it); // unplugged
            changed = true;
        }
    }
    for (const QString &name : m_ports) {
        if (isPortEnabled(name) && !isPortOpen(name) && !m_failed.contains(name))
            changed |= openQuietly(name);
    }
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

bool MidiInput::parse(const unsigned char *bytes, size_t size, MidiMsg *out)
{
    if (!bytes || size < 3)
        return false; // every type porydaw keeps has two data bytes
    const uint8_t status = bytes[0];
    const uint8_t d1 = bytes[1] & 0x7F;
    const uint8_t d2 = bytes[2] & 0x7F;
    if (status < 0x80)
        return false; // RtMidi expands running status; a bare data byte is garbage
    switch (status & 0xF0) {
    case 0x90:
        if (d2 == 0) { // note on with velocity 0 is a note off
            *out = {uint8_t(0x80 | (status & 0x0F)), d1, 0};
            return true;
        }
        [[fallthrough]];
    case 0x80:
    case 0xB0:
    case 0xE0:
        *out = {status, d1, d2};
        return true;
    default:
        // 0xA0 polyphonic and 0xD0 channel aftertouch, 0xC0 program change
        // (the selected track's voice is what plays), 0xF0 system.
        return false;
    }
}

void MidiInput::onMessage(double, std::vector<unsigned char> *message, void *userData)
{
    auto *port = static_cast<OpenPort *>(userData);
    if (port && message && !message->empty())
        port->owner->receive(message->data(), message->size());
}

void MidiInput::injectForTest(const MidiMsg &msg)
{
    const unsigned char bytes[3] = {msg.status, msg.d1, msg.d2};
    receive(bytes, sizeof bytes);
}

void MidiInput::setPortListForTest(const QStringList &names)
{
    m_ports = names;
    emit portsChanged();
}

void MidiInput::receive(const unsigned char *bytes, size_t size)
{
    MidiMsg msg;
    if (!parse(bytes, size, &msg))
        return;
    if (m_ring.push(msg))
        m_received.fetch_add(1, std::memory_order_relaxed);
    // One queued hop at a time; deliverActivity re-arms it.
    if (!m_activityPosted.exchange(true, std::memory_order_acq_rel))
        QMetaObject::invokeMethod(this, &MidiInput::deliverActivity, Qt::QueuedConnection);
}

void MidiInput::deliverActivity()
{
    if (m_sinceActivity.isValid() && m_sinceActivity.elapsed() < kActivityIntervalMs) {
        if (!m_activityTimer->isActive())
            m_activityTimer->start(int(kActivityIntervalMs - m_sinceActivity.elapsed()));
        return;
    }
    m_sinceActivity.start();
    m_activityPosted.store(false, std::memory_order_release);
    emit activity();
}
