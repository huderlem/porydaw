#include "audio/mcu.h"

namespace mcu {

std::optional<SurfaceEvent> decode(const MidiTapMsg &msg)
{
    // Channel 1 only: MCU's transport section never uses another.
    if ((msg.status & 0x0F) != 0)
        return std::nullopt;
    const uint8_t type = msg.status & 0xF0;
    if (type == 0xB0) {
        if (msg.d1 != kJogCc)
            return std::nullopt;
        const int count = msg.d2 & 0x3F;
        if (count == 0)
            return std::nullopt;
        SurfaceEvent event;
        event.kind = SurfaceEvent::Jog;
        event.delta = (msg.d2 & 0x40) ? -count : count;
        return event;
    }
    if (type != 0x80 && type != 0x90)
        return std::nullopt;
    SurfaceEvent event;
    switch (msg.d1) {
    case kPlay:
        event.kind = SurfaceEvent::Play;
        break;
    case kStop:
        event.kind = SurfaceEvent::Stop;
        break;
    case kRecord:
        event.kind = SurfaceEvent::Record;
        break;
    case kCycle:
        event.kind = SurfaceEvent::Cycle;
        break;
    case kClick:
        event.kind = SurfaceEvent::Click;
        break;
    case kPunchDrop:
    case kPunchReplace:
        event.kind = SurfaceEvent::Punch;
        break;
    case kRewind:
        event.kind = SurfaceEvent::Rewind;
        break;
    case kFastForward:
        event.kind = SurfaceEvent::FastForward;
        break;
    case kSave:
        event.kind = SurfaceEvent::Save;
        break;
    case kUndo:
        event.kind = SurfaceEvent::Undo;
        break;
    default:
        return std::nullopt;
    }
    event.pressed = type == 0x90 && msg.d2 > 0;
    event.note = msg.d1;
    return event;
}

} // namespace mcu
