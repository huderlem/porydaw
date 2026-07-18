#include "midiimport.h"

#include <QMap>
#include <QObject>
#include <algorithm>

#include "ui/m4asemantics.h"

namespace {

constexpr int kMaxEngineTracks = 16; // m4a MAX_TRACKS
constexpr int kDefaultPcmBudget = 5; // pokeemerald m4aSoundInit maxChans

// Mirrors SongDocument::rebuildTrackMap / MidiTimeline::build: the first 16
// channel-bearing chunks, as chunk indices in file order.
std::vector<int> engineTrackMap(const SmfFile &smf, int *dropped)
{
    std::vector<int> map;
    *dropped = 0;
    for (size_t t = 0; t < smf.tracks.size(); t++) {
        for (const SmfEvent &ev : smf.tracks[t].events) {
            if (!ev.isChannel())
                continue;
            if (int(map.size()) < kMaxEngineTracks)
                map.push_back(int(t));
            else
                (*dropped)++;
            break;
        }
    }
    return map;
}

} // namespace

ImportAnalysis analyzeForImport(const SmfFile &smf)
{
    ImportAnalysis a;
    a.division = smf.division;
    a.smfTrackCount = int(smf.tracks.size());

    const auto map = engineTrackMap(smf, &a.droppedTracks);
    a.mappedTracks = int(map.size());

    QMap<uint8_t, int> ccCounts;
    // (engineTrack << 8 | key) -> depth, so overlapping same-key notes count
    // once per sounding instance.
    QMap<int, int> sounding;
    struct NoteEdge {
        uint64_t tick;
        bool on;
        int track;
        uint8_t key;
    };
    std::vector<NoteEdge> edges;

    for (int et = 0; et < int(map.size()); et++) {
        const int smfTrack = map[et];
        ImportTrackInfo info;
        info.smfTrack = smfTrack;

        // Name rule mirrors trackNameLoc/MidiTimeline: the chunk's first
        // unprefixed 0x03 — a Channel-Prefix-scoped 0x03 is never its name.
        SmfChannelPrefix prefix;
        for (const SmfEvent &ev : smf.tracks[smfTrack].events) {
            prefix.observe(ev);
            if (ev.isMeta() && ev.metaType == 0x03 && info.name.isEmpty()
                && prefix.channel < 0)
                info.name = QString::fromLatin1(ev.blob).trimmed();
            if (!ev.isChannel())
                continue;
            switch (ev.typeNibble()) {
            case 0x9:
                if (ev.data1 != 0) {
                    info.noteCount++;
                    if (info.programs.empty())
                        info.notesBeforeProgram = true;
                    edges.push_back({ev.tick, true, et, ev.data0});
                    break;
                }
                [[fallthrough]];
            case 0x8:
                edges.push_back({ev.tick, false, et, ev.data0});
                break;
            case 0xB:
                ccCounts[ev.data0]++;
                break;
            case 0xC:
                if (std::find(info.programs.begin(), info.programs.end(), ev.data0)
                    == info.programs.end())
                    info.programs.push_back(ev.data0);
                break;
            default:
                break;
            }
        }
        a.tracks.push_back(info);
    }

    // Peak polyphony: note-ends first at equal ticks, as a note retriggered on
    // the same tick replaces rather than stacks.
    std::stable_sort(edges.begin(), edges.end(), [](const NoteEdge &x, const NoteEdge &y) {
        if (x.tick != y.tick)
            return x.tick < y.tick;
        return !x.on && y.on;
    });
    int active = 0;
    for (const NoteEdge &e : edges) {
        const int key = (e.track << 8) | e.key;
        if (e.on) {
            sounding[key]++;
            active++;
            a.peakConcurrentNotes = std::max(a.peakConcurrentNotes, active);
        } else if (sounding.value(key, 0) > 0) {
            sounding[key]--;
            active--;
        }
    }

    for (auto it = ccCounts.constBegin(); it != ccCounts.constEnd(); ++it) {
        const M4aCcInfo info = m4aClassifyCc(it.key());
        ImportCcUsage usage;
        usage.cc = it.key();
        usage.count = it.value();
        usage.audible = info.eventClass == M4aEventClass::AudibleLane;
        usage.label = QStringLiteral("%1 — %2").arg(QLatin1String(info.name),
                                                    QLatin1String(info.display));
        a.ccs.push_back(usage);
    }

    if (a.droppedTracks > 0)
        a.warnings.append(QObject::tr("%1 track(s) beyond the m4a 16-track limit "
                                      "will not play.")
                              .arg(a.droppedTracks));
    if (a.division % 24 != 0)
        a.warnings.append(
            QObject::tr("Division %1 is not a multiple of 24; mid2agb quantizes to "
                        "24 clocks per beat, so timing will shift slightly.")
                .arg(a.division));
    if (a.peakConcurrentNotes > kDefaultPcmBudget)
        a.warnings.append(
            QObject::tr("Up to %1 notes sound at once; the GBA mixes %2 sample-based "
                        "notes (CGB square/wave/noise voices don't count). Extra "
                        "notes will be dropped or stolen.")
                .arg(a.peakConcurrentNotes)
                .arg(kDefaultPcmBudget));
    for (const ImportTrackInfo &t : a.tracks) {
        if (t.noteCount > 0 && t.notesBeforeProgram) {
            a.warnings.append(
                QObject::tr("Some tracks play notes before any program change; those "
                            "notes use voice 0."));
            break;
        }
    }
    return a;
}

void rescaleDivision(SmfFile *smf, uint16_t newDivision)
{
    if (newDivision == 0 || smf->division == 0 || smf->division == newDivision)
        return;
    // Floor scaling is monotonic, so each track's non-decreasing tick order
    // (and same-tick event order) survives the rescale.
    const uint64_t oldDivision = smf->division;
    for (SmfTrack &track : smf->tracks) {
        for (SmfEvent &ev : track.events)
            ev.tick = ev.tick * newDivision / oldDivision;
        track.endTick = track.endTick * newDivision / oldDivision;
    }
    smf->division = newDivision;
}
