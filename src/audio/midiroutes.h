#pragma once

#include <QString>
#include <QStringList>

#include <array>
#include <cstdint>

// Which keyboard control drives which automation lane
// (docs/midi-controls/PLAN.md §3.1): for each routable lane (MOD, VOL, PAN,
// BENDR, LFOS, named by their document CC numbers) the MIDI CC number that
// drives it, or none. One CC drives one lane and each lane has at most one
// CC, so the lane → CC form is lossless; laneFor() is the reverse lookup.
//
// Routes are omni (the channel is ignored) and global. CC64 (sustain),
// CC120–127 (channel mode messages), bank select (CC0/32) and data entry
// (CC6/38, which RPN bend-range messages use) are never assignable: neither
// Learn nor the Settings page can route them.
//
// Two encodings: packed() is one 64-bit word for the audio thread (lane i's
// CC in byte i, 0xFF for none), and toStrings()/fromStrings() is the
// QSettings form ("MOD=1", ..., "BENDR=none").
class MidiRoutes
{
  public:
    static constexpr int kLaneCount = 5;
    // The routable lanes, in the Settings table's order.
    static constexpr std::array<uint8_t, kLaneCount> kLanes = {0x01, 0x07, 0x0A, 0x14, 0x15};
    static constexpr int kNone = -1;

    // The defaults: CC1 → MOD, CC7 → VOL, CC10 → PAN, BENDR and LFOS none.
    MidiRoutes();

    // The position of a lane's document CC in kLanes, or -1 when it is not
    // routable.
    static int laneIndex(int lane);
    // "MOD", "VOL", "PAN", "BENDR", "LFOS" (the lane rows' labels and the
    // QSettings keys); empty for a lane that isn't routable.
    static QString laneName(int lane);
    // Whether a MIDI CC number can drive a lane (0–127 and not reserved).
    static bool assignable(int cc);

    // The CC driving lane, or kNone.
    int ccFor(int lane) const;
    // The lane cc drives (a document CC from kLanes), or kNone.
    int laneFor(int cc) const;

    // Routes cc to lane; kNone clears the lane. A cc already driving
    // another lane moves: that lane becomes none, and *movedFrom (when
    // given) names it (kNone when nothing moved). False, with nothing
    // changed, for a lane that isn't routable or a cc that isn't
    // assignable.
    bool assign(int lane, int cc, int *movedFrom = nullptr);

    uint64_t packed() const;
    // The audio thread's decode of packed(): laneOf[cc] is the lane (a
    // document CC from kLanes) cc drives, 0xFF for none. Allocation-free.
    // Bytes that aren't an assignable CC, and a CC a lower byte already
    // took, are skipped, so a malformed word never routes a reserved CC.
    static void unpackLaneTable(uint64_t packed, uint8_t (&laneOf)[128]);

    // Every lane, in kLanes order: "NAME=cc" or "NAME=none".
    QStringList toStrings() const;
    // The reverse. A lane the list doesn't name keeps its default, unless
    // a listed lane took that CC; unknown names, unassignable CCs and a CC
    // listed twice (the later lane) read as none.
    static MidiRoutes fromStrings(const QStringList &strings);

    bool operator==(const MidiRoutes &other) const { return m_cc == other.m_cc; }
    bool operator!=(const MidiRoutes &other) const { return !(*this == other); }

  private:
    // Per lane (kLanes order): the CC, or kNone.
    std::array<int, kLaneCount> m_cc;
};
