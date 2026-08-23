#include "midiimport.h"

#include <QMap>
#include <QObject>
#include <algorithm>
#include <map>

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

ImportAnalysis analyzeForImport(const SmfFile &smf, int trackBudget, const QString &playerName)
{
    ImportAnalysis a;
    a.division = smf.division;
    a.sampleNoteLimit = kDefaultPcmBudget;
    a.smfTrackCount = int(smf.tracks.size());

    const auto map = engineTrackMap(smf, &a.droppedTracks);
    a.mappedTracks = int(map.size());
    if (trackBudget >= 0 && trackBudget < kMaxEngineTracks)
        a.silentTracks = std::max(0, a.mappedTracks - trackBudget);

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
            if (ev.isMeta() && ev.metaType == 0x03 && info.name.isEmpty() && prefix.channel < 0)
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
                if (std::find(info.programs.begin(), info.programs.end(), ev.data0) ==
                    info.programs.end())
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
        usage.label =
            QStringLiteral("%1 — %2").arg(QLatin1String(info.name), QLatin1String(info.display));
        a.ccs.push_back(usage);
    }

    if (a.droppedTracks > 0)
        a.warnings.append(
            QObject::tr("Porydaw will not import %1. The MIDI file contains more than 16 tracks.")
                .arg(trackCountPhrase(a.droppedTracks)));
    if (a.silentTracks > 0) {
        const QString displayName = playerName.isEmpty() ? QObject::tr("the selected audio player")
                                                         : playerRoleName(playerName, true);
        a.warnings.append(
            QObject::tr("The game will not play %1 for %2. This player can play %3.")
                .arg(trackCountPhrase(a.silentTracks), displayName, trackCountPhrase(trackBudget)));
    }
    if (a.division % 24 != 0)
        a.warnings.append(
            QObject::tr("Porydaw will adjust the note timing. The source timing value is %1.")
                .arg(a.division));
    if (a.peakConcurrentNotes > kDefaultPcmBudget)
        a.warnings.append(concurrencyNoticeText(a.peakConcurrentNotes, kDefaultPcmBudget));
    for (const ImportTrackInfo &t : a.tracks) {
        if (t.noteCount > 0 && t.notesBeforeProgram) {
            a.warnings.append(instrumentFallbackNoticeText());
            break;
        }
    }
    return a;
}

QString playerRoleName(const QString &symbol, bool includeSymbol)
{
    const QString sePrefix = QStringLiteral("MUSIC_PLAYER_SE");
    QString role;
    if (symbol == QStringLiteral("MUSIC_PLAYER_BGM")) {
        role = QObject::tr("Background music");
    } else if (symbol.startsWith(sePrefix)) {
        const QString number = symbol.mid(sePrefix.size());
        role = number.isEmpty() ? QObject::tr("Sound effect")
                                : QObject::tr("Sound effect %1").arg(number);
    } else {
        return symbol;
    }
    return includeSymbol ? QStringLiteral("%1 (%2)").arg(role, symbol) : role;
}

QString trackCountPhrase(int count)
{
    return count == 1 ? QObject::tr("1 track") : QObject::tr("%1 tracks").arg(count);
}

QString concurrencyNoticeText(int peakNotes, int sampleNoteLimit)
{
    return QObject::tr("%1 notes play at the same time in one part of the song. The Game Boy "
                       "Advance can mix %2 sample notes at the same time. Square, wave, and noise "
                       "sounds do not use this limit. The game can stop some sample notes.")
        .arg(peakNotes)
        .arg(sampleNoteLimit);
}

QString instrumentFallbackNoticeText()
{
    return QObject::tr("Some notes start before the MIDI data selects an instrument. These notes "
                       "use instrument 0.");
}

namespace {

// The engine slot a pure state-setter writes, or -1 for events where every
// occurrence matters. Slots are per channel; CCs get one slot per controller
// number (the m4a CC vocabulary has no cross-CC coupling outside the excluded
// protocols, and CCs it ignores set no state at all), poly aftertouch one per
// key. CC numbers follow tools/mid2agb/agb.cpp via m4aClassifyCc's table.
int setterSlot(const SmfEvent &ev)
{
    if (ev.isMeta()) {
        // Tempo and time signature; other metas (text, markers, ports) are
        // identities, not values — two on one tick can both be meant.
        if (ev.metaType == 0x51 || ev.metaType == 0x58)
            return 0x10000 | ev.metaType;
        return -1;
    }
    if (!ev.isChannel())
        return -1;
    const int channel = ev.status & 0x0F;
    switch (ev.typeNibble()) {
    case 0xB:
        switch (ev.data0) {
        case 0x0C: // MEMACC plumbing: an op CC fires using state stashed by
        case 0x0D: // its neighbors, and the ops include conditional branches —
        case 0x0E: // every occurrence is an action.
        case 0x0F:
        case 0x10:
        case 0x11: // loop Label
        case 0x1D: // XCMD: same stash-then-fire shape as MEMACC
        case 0x1E:
        case 0x1F:
            return -1;
        default:
            return (0xB << 12) | (channel << 7) | ev.data0;
        }
    case 0xA: // poly aftertouch: one slot per key
        return (0xA << 12) | (channel << 7) | ev.data0;
    case 0xC:
    case 0xD:
    case 0xE:
        return (ev.typeNibble() << 12) | (channel << 7);
    default: // notes
        return -1;
    }
}

} // namespace

int removeRedundantSetterEvents(SmfFile *smf)
{
    int removed = 0;
    for (SmfTrack &track : smf->tracks) {
        std::vector<SmfEvent> &evs = track.events;
        std::vector<char> drop(evs.size(), 0);
        std::map<int, size_t> lastForSlot; // within the current tick only
        uint64_t runTick = 0;
        for (size_t i = 0; i < evs.size(); i++) {
            if (i == 0 || evs[i].tick != runTick) {
                lastForSlot.clear();
                runTick = evs[i].tick;
            }
            const int slot = setterSlot(evs[i]);
            if (slot < 0)
                continue;
            const auto it = lastForSlot.find(slot);
            if (it != lastForSlot.end()) {
                drop[it->second] = 1;
                removed++;
            }
            lastForSlot[slot] = i;
        }
        size_t out = 0;
        for (size_t i = 0; i < evs.size(); i++) {
            if (!drop[i])
                evs[out++] = std::move(evs[i]);
        }
        evs.resize(out);
    }
    return removed;
}

int moveTempoMetasToFirstChunk(SmfFile *smf)
{
    if (smf->tracks.size() < 2)
        return 0;
    std::vector<SmfEvent> moved;
    for (size_t t = 1; t < smf->tracks.size(); t++) {
        std::vector<SmfEvent> &evs = smf->tracks[t].events;
        for (const SmfEvent &ev : evs) {
            if (ev.isMeta() && ev.metaType == 0x51)
                moved.push_back(ev);
        }
        evs.erase(
            std::remove_if(evs.begin(), evs.end(),
                           [](const SmfEvent &ev) { return ev.isMeta() && ev.metaType == 0x51; }),
            evs.end());
    }
    if (moved.empty())
        return 0;
    // Chunks were walked in order and each is tick-sorted, so a stable sort
    // by tick keeps chunk order within a tick (the file-order winner stays
    // last); merge() then places first-chunk events ahead of moved ones at
    // equal ticks for the same reason.
    std::stable_sort(moved.begin(), moved.end(),
                     [](const SmfEvent &a, const SmfEvent &b) { return a.tick < b.tick; });
    SmfTrack &first = smf->tracks.front();
    std::vector<SmfEvent> merged;
    merged.reserve(first.events.size() + moved.size());
    std::merge(first.events.begin(), first.events.end(), moved.begin(), moved.end(),
               std::back_inserter(merged),
               [](const SmfEvent &a, const SmfEvent &b) { return a.tick < b.tick; });
    first.events = std::move(merged);
    first.endTick = std::max(first.endTick, moved.back().tick);
    return int(moved.size());
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
