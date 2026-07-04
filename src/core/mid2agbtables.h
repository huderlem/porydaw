#pragma once

#include <cstdint>

// mid2agb's note quantization (SPEC.md §4.3, WYHIWYG): what a drawn velocity
// or note length becomes in the compiled song. Values derived from
// tools/mid2agb/tables.cpp (© 2016 YamaArashi, MIT).

// g_noteVelocityLUT: 0 stays 0, everything else rounds up to the next
// multiple of 4, saturating at 127.
inline int mid2agbEffectiveVelocity(int velocity)
{
    if (velocity <= 0)
        return 0;
    const int v = ((velocity + 3) / 4) * 4;
    return v > 127 ? 127 : v;
}

// Note duration in mid2agb clocks (24 or 48 per beat) after ConvertTimes:
// scaled from SMF ticks, zero bumped to 1, then (unless -E exact gate time)
// durations under 96 snapped through g_noteDurationLUT.
int mid2agbEffectiveDuration(int64_t durationTicks, uint32_t division, bool extendedClocks,
                             bool exactGate);
