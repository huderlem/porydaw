#pragma once

#include <cstdint>

// The CPU budget (docs/cpu-meter/PLAN.md D1, amended 2026-10-29): the CPU
// readout turns amber above this many GBA cycles of m4aSoundMain per frame.
// Up to the whole frame (M4A_FRAME_CYCLES); past the VBlank window the
// readout is red regardless. The default is about 72 % of the VBlank window
// (M4A_VBLANK_CYCLES, 83776). Shared by Settings → Audio, the CPU dock, the
// profile strip and MainWindow.
namespace cpubudget {

constexpr int32_t kMin = 1000;
constexpr int32_t kMax = 280896;
constexpr int32_t kDefault = 60000;
constexpr int32_t kStep = 1000;

} // namespace cpubudget
