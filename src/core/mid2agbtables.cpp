#include "mid2agbtables.h"

namespace {

// tools/mid2agb/tables.cpp g_noteDurationLUT (© 2016 YamaArashi, MIT):
// clock counts 0-96; identity below 24, then progressively coarser buckets.
const uint8_t kDurationLUT[97] = {
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 24, 24, 24, 28, 28, 30, 30,
    32, 32, 32, 32, 36, 36, 36, 36, 40, 40, 42, 42, 44, 44, 44, 44,
    48, 48, 48, 48, 52, 52, 54, 54, 56, 56, 56, 56, 60, 60, 60, 60,
    64, 64, 66, 66, 68, 68, 68, 68, 72, 72, 72, 72, 76, 76, 78, 78,
    80, 80, 80, 80, 84, 84, 84, 84, 88, 88, 90, 90, 92, 92, 92, 92,
    96,
};

} // namespace

int mid2agbEffectiveDuration(int64_t durationTicks, uint32_t division, bool extendedClocks,
                             bool exactGate)
{
    const int64_t clocksPerBeat = extendedClocks ? 48 : 24;
    int64_t duration = division ? (clocksPerBeat * durationTicks) / division : durationTicks;
    if (duration <= 0)
        duration = 1;
    if (!exactGate && duration < 96)
        duration = kDurationLUT[duration];
    return int(duration);
}
