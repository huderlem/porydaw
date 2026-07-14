#include "songviewmodel.h"

#include <algorithm>
#include <array>

namespace {

AutoLane &laneFor(SongViewModel &model, uint8_t track, uint8_t cc, M4aLane lane,
                  const QString &name)
{
    for (AutoLane &l : model.lanes)
        if (l.track == track && l.cc == cc)
            return l;
    model.lanes.push_back({track, cc, lane, name, {}});
    return model.lanes.back();
}

} // namespace

SongViewModel buildSongViewModel(const MidiTimeline &tl)
{
    SongViewModel model;

    // Open note indices per (track, key). A note end closes every note still
    // open on its key: mid2agb pairs each note-on with the first same-key end
    // that follows it without consuming it, and SongDocument::notesForTrack
    // mirrors that — pairing the same way here keeps the roll showing exactly
    // what edits operate on and what the build produces (a mis-ordered
    // same-tick end/on pair renders as the zero-length note it really is
    // instead of being papered over).
    std::array<std::vector<size_t>, 16 * 128> open;

    for (const TimelineEvent &ev : tl.events) {
        switch (ev.type) {
        case 0x9: { // note on
            ViewNote note;
            note.startTick = ev.tick;
            note.endTick = ev.tick;
            note.key = ev.data0 & 0x7F;
            note.velocity = ev.data1;
            note.track = ev.track;
            note.unterminated = true;
            open[ev.track * 128 + note.key].push_back(model.notes.size());
            model.notes.push_back(note);
            model.minNoteKey = std::min(model.minNoteKey, int(note.key));
            model.maxNoteKey = std::max(model.maxNoteKey, int(note.key));
            break;
        }
        case 0x8: { // note off
            std::vector<size_t> &stack = open[ev.track * 128 + (ev.data0 & 0x7F)];
            if (stack.empty()) {
                model.orphanNoteOffs++;
                model.strip.push_back(
                    {ev.tick, ev.track,
                     QStringLiteral("Note off (key %1) without a note on").arg(ev.data0)});
                break;
            }
            for (size_t idx : stack) {
                ViewNote &note = model.notes[idx];
                note.endTick = ev.tick;
                note.unterminated = false;
            }
            stack.clear();
            break;
        }
        case 0xB: { // control change
            const M4aCcInfo info = m4aClassifyCc(ev.data0);
            if (info.eventClass == M4aEventClass::AudibleLane) {
                laneFor(model, ev.track, ev.data0, info.lane,
                        QString::fromLatin1(info.display))
                    .points.push_back({ev.tick, int(ev.data1)});
            } else {
                model.strip.push_back(
                    {ev.tick, ev.track, m4aAdvancedCcLabel(ev.data0, ev.data1)});
            }
            break;
        }
        case 0xC: // program change -> VOICE
            model.voices.push_back({ev.tick, ev.track, ev.data0});
            break;
        case 0xE: { // pitch bend, 14-bit centered
            const int bend = (int(ev.data1) << 7 | ev.data0) - 8192;
            laneFor(model, ev.track, LANE_CC_BEND, M4aLane::PitchBend,
                    m4aLaneName(M4aLane::PitchBend))
                .points.push_back({ev.tick, bend});
            break;
        }
        case TIMELINE_EVT_TEMPO:
            break; // synthetic; the tempo lane is built from tl.tempoMap below
        default:
            model.strip.push_back(
                {ev.tick, ev.track,
                 QStringLiteral("Unhandled event type 0x%1").arg(ev.type, 0, 16)});
            break;
        }
    }

    for (const auto &stack : open)
        model.unpairedNoteOns += stack.size();
    // Unterminated notes stay visible: extend them to the end of the song.
    for (ViewNote &note : model.notes)
        if (note.unterminated)
            note.endTick = uint32_t(tl.lengthTicks);

    for (const TempoPoint &tp : tl.tempoMap)
        model.tempoLane.push_back({uint32_t(tp.tick), int(tp.bpm + 0.5)});

    for (const OtherEvent &oe : tl.otherEvents)
        model.strip.push_back({oe.tick, oe.track, oe.label});

    std::stable_sort(model.strip.begin(), model.strip.end(),
                     [](const StripItem &a, const StripItem &b) { return a.tick < b.tick; });
    std::stable_sort(model.voices.begin(), model.voices.end(),
                     [](const VoiceChange &a, const VoiceChange &b) { return a.tick < b.tick; });

    // Stable lane order: by track, then §4.2 table order via CC number
    // (bend sorts last as 0xFF, which matches the table's row order).
    std::stable_sort(model.lanes.begin(), model.lanes.end(),
                     [](const AutoLane &a, const AutoLane &b) {
                         if (a.track != b.track)
                             return a.track < b.track;
                         return a.cc < b.cc;
                     });

    return model;
}
