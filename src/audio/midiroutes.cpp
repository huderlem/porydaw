#include "midiroutes.h"

#include <algorithm>
#include <iterator>

namespace {

const char *const kLaneNames[MidiRoutes::kLaneCount] = {"MOD", "VOL", "PAN", "BENDR", "LFOS"};
constexpr std::array<int, MidiRoutes::kLaneCount> kDefaultCc = {1, 7, 10, MidiRoutes::kNone,
                                                                MidiRoutes::kNone};

} // namespace

MidiRoutes::MidiRoutes() : m_cc(kDefaultCc) {}

int MidiRoutes::laneIndex(int lane)
{
    const auto it = std::find(kLanes.begin(), kLanes.end(), lane);
    return it == kLanes.end() ? -1 : int(it - kLanes.begin());
}

QString MidiRoutes::laneName(int lane)
{
    const int i = laneIndex(lane);
    return i < 0 ? QString() : QString::fromLatin1(kLaneNames[i]);
}

bool MidiRoutes::assignable(int cc)
{
    if (cc < 0 || cc > 127)
        return false;
    switch (cc) {
    case 0:  // bank select MSB
    case 32: // bank select LSB
    case 6:  // data entry MSB (RPN bend range)
    case 38: // data entry LSB
    case 64: // sustain pedal
        return false;
    default:
        return cc < 120; // 120–127: channel mode messages (resets, panic)
    }
}

int MidiRoutes::ccFor(int lane) const
{
    const int i = laneIndex(lane);
    return i < 0 ? kNone : m_cc[i];
}

int MidiRoutes::laneFor(int cc) const
{
    if (cc < 0)
        return kNone;
    for (int i = 0; i < kLaneCount; i++) {
        if (m_cc[i] == cc)
            return kLanes[i];
    }
    return kNone;
}

bool MidiRoutes::assign(int lane, int cc, int *movedFrom)
{
    if (movedFrom)
        *movedFrom = kNone;
    const int i = laneIndex(lane);
    if (i < 0 || (cc != kNone && !assignable(cc)))
        return false;
    if (cc != kNone) {
        for (int j = 0; j < kLaneCount; j++) {
            if (j != i && m_cc[j] == cc) {
                m_cc[j] = kNone;
                if (movedFrom)
                    *movedFrom = kLanes[j];
            }
        }
    }
    m_cc[i] = cc;
    return true;
}

uint64_t MidiRoutes::packed() const
{
    uint64_t word = ~uint64_t(0);
    for (int i = 0; i < kLaneCount; i++) {
        if (m_cc[i] != kNone) {
            word &= ~(uint64_t(0xFF) << (8 * i));
            word |= uint64_t(m_cc[i]) << (8 * i);
        }
    }
    return word;
}

void MidiRoutes::unpackLaneTable(uint64_t packed, uint8_t (&laneOf)[128])
{
    std::fill(std::begin(laneOf), std::end(laneOf), uint8_t(0xFF));
    for (int i = 0; i < kLaneCount; i++) {
        const int cc = int((packed >> (8 * i)) & 0xFF);
        if (assignable(cc) && laneOf[cc] == 0xFF)
            laneOf[cc] = kLanes[i];
    }
}

QStringList MidiRoutes::toStrings() const
{
    QStringList strings;
    for (int i = 0; i < kLaneCount; i++) {
        strings.append(
            QStringLiteral("%1=%2")
                .arg(QLatin1String(kLaneNames[i]))
                .arg(m_cc[i] == kNone ? QStringLiteral("none") : QString::number(m_cc[i])));
    }
    return strings;
}

MidiRoutes MidiRoutes::fromStrings(const QStringList &strings)
{
    MidiRoutes routes;
    std::array<bool, kLaneCount> listed{};
    std::array<int, kLaneCount> cc;
    cc.fill(kNone);
    for (const QString &entry : strings) {
        const qsizetype eq = entry.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        const QString name = entry.left(eq).trimmed().toUpper();
        int i = 0;
        while (i < kLaneCount && name != QLatin1String(kLaneNames[i]))
            i++;
        if (i == kLaneCount || listed[i])
            continue;
        listed[i] = true;
        bool ok = false;
        const int value = entry.mid(eq + 1).trimmed().toInt(&ok);
        if (ok && assignable(value) && std::find(cc.begin(), cc.end(), value) == cc.end())
            cc[i] = value;
    }
    // Lanes the list doesn't name (a hand edit, or settings from a build
    // with fewer lanes) keep their defaults where the CC is still free.
    for (int i = 0; i < kLaneCount; i++) {
        if (!listed[i] && routes.m_cc[i] != kNone &&
            std::find(cc.begin(), cc.end(), routes.m_cc[i]) == cc.end())
            cc[i] = routes.m_cc[i];
    }
    routes.m_cc = cc;
    return routes;
}
