#pragma once

#include "audio/midiinput.h"

#include <optional>

// One Mackie Control (MCU) transport control, as a control surface port
// sends it (docs/control-surface/PLAN.md §3.2).
struct SurfaceEvent {
    enum Kind { Play, Stop, Record, Cycle, Click, Rewind, FastForward, Save, Undo, Jog };
    Kind kind = Play;
    // A button went down (true) or came up (false). A jog turn is always
    // "pressed".
    bool pressed = true;
    // Jog only: detents turned, positive clockwise (forward), never 0.
    int delta = 0;
};

namespace mcu {

// MCU's note numbers for the buttons porydaw uses (channel 1).
constexpr uint8_t kSave = 0x50;
constexpr uint8_t kUndo = 0x51;
constexpr uint8_t kCycle = 0x56;
constexpr uint8_t kClick = 0x59;
constexpr uint8_t kRewind = 0x5B;
constexpr uint8_t kFastForward = 0x5C;
constexpr uint8_t kStop = 0x5D;
constexpr uint8_t kPlay = 0x5E;
constexpr uint8_t kRecord = 0x5F;
// The jog wheel's CC (channel 1), relative: bit 6 set turns backward, the
// low 6 bits count detents.
constexpr uint8_t kJogCc = 0x3C;

// The control a surface message stands for, or nothing for anything else
// (another channel, an unmapped note or CC, other message types, a jog
// message of zero detents). A note on with velocity > 0 is a press; a note
// off (any velocity) or a velocity-0 note on is a release. Pure.
std::optional<SurfaceEvent> decode(const MidiTapMsg &msg);

} // namespace mcu
