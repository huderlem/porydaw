#pragma once

// The output device's buffering (Settings → Audio → Output device). A
// periodMs of 0 leaves the size to the backend (miniaudio's ~10 ms
// low-latency default, 30 ms under WSL); PORYDAW_AUDIO_PERIOD_MS still
// overrides either. exclusive asks WASAPI for exclusive mode, which skips
// the Windows mixer; its default periods are kExclusiveDefaultPeriodMs. It
// is ignored on other platforms.
struct AudioDeviceConfig {
    static constexpr int kExclusiveDefaultPeriodMs = 3;

    int periodMs = 0;
    bool exclusive = false;

    bool operator==(const AudioDeviceConfig &o) const
    {
        return periodMs == o.periodMs && exclusive == o.exclusive;
    }
    bool operator!=(const AudioDeviceConfig &o) const { return !(*this == o); }
};
