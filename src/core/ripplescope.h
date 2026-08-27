#pragma once

#include <cstdint>
#include <utility>
#include <vector>

// Which streams a ripple edit (removeTimeRange / insertTimeRange, the
// insert-space drag) acts on: the engine tracks, the automation lane rows
// (track -1 = tempo), or the whole song — every engine track plus the
// global rows (tempo, time signatures, loop markers, other metas) and each
// chunk's end-of-track tick. Lives outside SongDocument so the views can
// build one without the document header.
struct RippleScope {
    std::vector<int> tracks;                    // engine tracks (ignored when wholeSong)
    std::vector<std::pair<int, uint8_t>> lanes; // (engineTrack, cc); -1 = tempo
    bool wholeSong = false;
};
