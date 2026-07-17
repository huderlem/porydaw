#include "miditimeline.h"

#include <algorithm>

#include "smf.h"

namespace {

struct RawEvent {
    uint64_t tick;
    uint16_t smfTrack;
    uint8_t type;
    uint8_t data0;
    uint8_t data1;
    int origIndex; // insertion order: stable-sort tiebreaker so setup events
                   // (program change, CC) stay ahead of note-ons at the same tick
};

struct TempoChange {
    uint64_t tick;
    uint32_t usPerBeat;
};

// Parsed-but-not-played data destined for MidiTimeline::otherEvents; the
// engine track is resolved after all tracks are mapped.
struct RawOther {
    uint64_t tick;
    uint16_t smfTrack;
    QString label;
};

// True when buf is exactly the single character `marker` after stripping
// leading/trailing ASCII whitespace.
bool textIsLoopMarker(const char *buf, uint32_t len, char marker)
{
    uint32_t s = 0, e = len;
    while (s < e && (buf[s] == ' ' || buf[s] == '\t' || buf[s] == '\r' || buf[s] == '\n'))
        s++;
    while (e > s && (buf[e - 1] == ' ' || buf[e - 1] == '\t' || buf[e - 1] == '\r' || buf[e - 1] == '\n'))
        e--;
    return (e - s == 1) && buf[s] == marker;
}

// Convert an absolute tick to an absolute sample index via the tempo map.
// Default tempo: 500000 us/beat (120 BPM).
uint64_t tickToSample(uint64_t tick, const std::vector<TempoChange> &tempos,
                      uint32_t tpqn, double sampleRate)
{
    double samples = 0.0;
    uint64_t prevTick = 0;
    double prevTempo = 500000.0;

    for (const TempoChange &tc : tempos) {
        if (tc.tick >= tick)
            break;
        samples += double(tc.tick - prevTick) * prevTempo / double(tpqn) / 1000000.0 * sampleRate;
        prevTick = tc.tick;
        prevTempo = double(tc.usPerBeat);
    }
    samples += double(tick - prevTick) * prevTempo / double(tpqn) / 1000000.0 * sampleRate;
    return static_cast<uint64_t>(samples + 0.5);
}

TimelineEvent makeTempoEvent(uint64_t samplePos, uint64_t tick, double bpm)
{
    int b = static_cast<int>(bpm + 0.5);
    b = std::clamp(b, 1, 0x3FFF);
    TimelineEvent ev;
    ev.samplePos = samplePos;
    ev.tick = static_cast<uint32_t>(tick);
    ev.type = TIMELINE_EVT_TEMPO;
    ev.track = 0;
    ev.data0 = static_cast<uint8_t>(b & 0x7F);
    ev.data1 = static_cast<uint8_t>((b >> 7) & 0x7F);
    return ev;
}

} // namespace

std::unique_ptr<MidiTimeline> MidiTimeline::load(const QString &path, double sampleRate,
                                                 QString *error)
{
    SmfFile smf;
    if (!SmfFile::readFile(path, &smf, error)) // readFile coerces format 0 away
        return nullptr;
    return build(smf, sampleRate);
}

std::unique_ptr<MidiTimeline> MidiTimeline::build(const SmfFile &smf, double sampleRate)
{
    const uint32_t tpqn = smf.division;
    const int numTracks = int(smf.tracks.size());

    std::vector<RawEvent> rawEvents;
    std::vector<TempoChange> tempos;
    std::vector<TimeSigPoint> timeSigs;
    std::vector<RawOther> rawOthers;
    std::vector<bool> trackHasChannelEvents(numTracks, false);
    std::vector<QString> trackNames(numTracks);
    uint64_t loopStartTick = UINT64_MAX;
    uint64_t loopEndTick = UINT64_MAX;

    for (int t = 0; t < numTracks; t++) {
        // Channel Prefix scoping (SmfChannelPrefix, the shared rule):
        // format 0's per-track naming mechanism — conversion rewrites
        // those, but a foreign format-1 file may still carry prefixed
        // 0x03s, and they are never the chunk's name.
        SmfChannelPrefix prefix;
        for (const SmfEvent &sev : smf.tracks[t].events) {
            const uint64_t tick = sev.tick;
            prefix.observe(sev);

            if (sev.isChannel()) {
                const uint8_t type = sev.typeNibble();
                auto push = [&](uint8_t playType) {
                    RawEvent ev;
                    ev.tick = tick;
                    ev.smfTrack = uint16_t(t);
                    ev.type = playType;
                    ev.data0 = sev.data0;
                    ev.data1 = sev.data1;
                    ev.origIndex = static_cast<int>(rawEvents.size());
                    rawEvents.push_back(ev);
                    trackHasChannelEvents[t] = true;
                };
                switch (type) {
                case 0x8:
                    push(0x8);
                    break;
                case 0x9: // note on (velocity 0 means note off)
                    push(sev.data1 ? 0x9 : 0x8);
                    break;
                case 0xA: // polyphonic aftertouch: not played
                    rawOthers.push_back({tick, uint16_t(t),
                                         QStringLiteral("Poly aftertouch key %1 = %2")
                                             .arg(sev.data0)
                                             .arg(sev.data1)});
                    break;
                case 0xB:
                    push(0xB);
                    break;
                case 0xC:
                    push(0xC);
                    break;
                case 0xD: // channel pressure: not played
                    rawOthers.push_back({tick, uint16_t(t),
                                         QStringLiteral("Channel pressure %1").arg(sev.data0)});
                    break;
                case 0xE:
                    push(0xE);
                    break;
                }
            } else if (sev.isSysEx()) {
                rawOthers.push_back({tick, uint16_t(t),
                                     QStringLiteral("SysEx (%1 bytes)").arg(sev.blob.size())});
            } else if (sev.isMeta()) {
                const uint8_t metaType = sev.metaType;
                const QByteArray &blob = sev.blob;
                if (metaType == 0x51 && blob.size() == 3) {
                    const uint8_t *p = reinterpret_cast<const uint8_t *>(blob.constData());
                    tempos.push_back({tick, (static_cast<uint32_t>(p[0]) << 16)
                                                | (static_cast<uint32_t>(p[1]) << 8) | p[2]});
                } else if (metaType == 0x58 && blob.size() >= 2) {
                    timeSigs.push_back({tick, uint8_t(blob[0]), uint8_t(blob[1])});
                } else if (metaType == 0x20 && blob.size() >= 1) {
                    // Channel Prefix: scoping handled by `prefix` above.
                } else if (metaType == 0x03 && prefix.channel >= 0
                           && !smfMetaIsMarker(sev)) {
                    // A channel-scoped name: not this chunk's. Marker text
                    // is exempt — mid2agb reads markers regardless of the
                    // prefix, so it falls through to the marker check.
                } else if (metaType == 0x03 && prefix.channel < 0
                           && trackNames[t].isEmpty()) {
                    const int len = std::min<int>(blob.size(), 64);
                    trackNames[t] = QString::fromLatin1(blob.constData(), len).trimmed();
                } else if (metaType >= 0x01 && metaType <= 0x07) {
                    // Text-type meta: check for loop markers ('[' / ']').
                    const uint32_t len = uint32_t(std::min<int>(blob.size(), 32));
                    if (textIsLoopMarker(blob.constData(), len, '[')
                        && loopStartTick == UINT64_MAX) {
                        loopStartTick = tick;
                    } else if (textIsLoopMarker(blob.constData(), len, ']')
                               && loopEndTick == UINT64_MAX) {
                        loopEndTick = tick;
                    } else {
                        static const char *const kTextMetaNames[] = {
                            "Text", "Copyright", "Track name", "Instrument",
                            "Lyric", "Marker", "Cue point"};
                        const QString text =
                            QString::fromLatin1(blob.constData(), int(len)).trimmed();
                        if (!text.isEmpty())
                            rawOthers.push_back(
                                {tick, uint16_t(t),
                                 QStringLiteral("%1: %2").arg(
                                     QLatin1String(kTextMetaNames[metaType - 1]), text)});
                    }
                } else {
                    rawOthers.push_back({tick, uint16_t(t),
                                         QStringLiteral("Meta 0x%1 (%2 bytes)")
                                             .arg(metaType, 2, 16, QLatin1Char('0'))
                                             .arg(blob.size())});
                }
            }
        }
    }

    std::stable_sort(rawEvents.begin(), rawEvents.end(),
                     [](const RawEvent &a, const RawEvent &b) {
                         if (a.tick != b.tick)
                             return a.tick < b.tick;
                         return a.origIndex < b.origIndex;
                     });
    std::sort(tempos.begin(), tempos.end(),
              [](const TempoChange &a, const TempoChange &b) { return a.tick < b.tick; });

    auto timeline = std::make_unique<MidiTimeline>();
    timeline->sampleRate = sampleRate;
    timeline->ticksPerBeat = tpqn;

    // Map SMF tracks to engine tracks: the first 16 chunks with channel
    // events, in chunk order (loaders coerce format 0 away, so chunk order
    // IS track order).
    std::vector<int> smfToEngine(numTracks, -1);
    int next = 0;
    for (int t = 0; t < numTracks; t++) {
        if (!trackHasChannelEvents[t])
            continue;
        if (next < 16) {
            smfToEngine[t] = next;
            timeline->tracks[next].name = trackNames[t];
            next++;
        } else {
            timeline->droppedTracks++;
        }
    }
    timeline->usedTrackCount = next;

    // Convert raw events to sample positions with final engine track indices.
    timeline->events.reserve(rawEvents.size() + tempos.size() + 1);

    std::vector<TimelineEvent> noteEvents;
    noteEvents.reserve(rawEvents.size());
    for (const RawEvent &re : rawEvents) {
        const int engineTrack = smfToEngine[re.smfTrack];
        if (engineTrack < 0)
            continue; // beyond 16 usable tracks

        TimelineEvent ev;
        ev.samplePos = tickToSample(re.tick, tempos, tpqn, sampleRate);
        ev.tick = static_cast<uint32_t>(re.tick);
        ev.type = re.type;
        ev.track = static_cast<uint8_t>(engineTrack);
        ev.data0 = re.data0;
        ev.data1 = re.data1;
        noteEvents.push_back(ev);

        TimelineTrack &ti = timeline->tracks[engineTrack];
        ti.used = true;
        if (ev.type == 0x9)
            ti.noteCount++;
        if (ev.type == 0xC && ti.firstProgram < 0)
            ti.firstProgram = ev.data0;
    }

    // Tempo events, with the SMF default of 120 BPM prepended when the song
    // doesn't set a tempo at tick 0, so the engine (whose tempo drives LFO and
    // vibrato rates) never runs on its unrelated init default. The same points
    // form the viewer's tempo map.
    std::vector<TimelineEvent> tempoEvents;
    if (tempos.empty() || tempos.front().tick != 0) {
        tempoEvents.push_back(makeTempoEvent(0, 0, 120.0));
        timeline->tempoMap.push_back({0, 0, 120.0});
    }
    for (const TempoChange &tc : tempos) {
        const uint64_t sp = tickToSample(tc.tick, tempos, tpqn, sampleRate);
        const double bpm = 60000000.0 / double(tc.usPerBeat);
        tempoEvents.push_back(makeTempoEvent(sp, tc.tick, bpm));
        timeline->tempoMap.push_back({tc.tick, sp, bpm});
    }

    // Merge, tempo first at equal positions so it takes effect before notes.
    std::merge(tempoEvents.begin(), tempoEvents.end(), noteEvents.begin(), noteEvents.end(),
               std::back_inserter(timeline->events),
               [](const TimelineEvent &a, const TimelineEvent &b) {
                   return a.samplePos < b.samplePos;
               });

    for (const TimelineEvent &ev : timeline->events) {
        timeline->lengthSamples = std::max(timeline->lengthSamples, ev.samplePos);
        timeline->lengthTicks = std::max(timeline->lengthTicks, uint64_t(ev.tick));
    }

    timeline->loopStartTick = loopStartTick;
    timeline->loopEndTick = loopEndTick;
    if (loopStartTick != UINT64_MAX)
        timeline->loopStartSample = tickToSample(loopStartTick, tempos, tpqn, sampleRate);
    if (loopEndTick != UINT64_MAX)
        timeline->loopEndSample = tickToSample(loopEndTick, tempos, tpqn, sampleRate);
    if (timeline->loopEndTick != UINT64_MAX)
        timeline->lengthTicks = std::max(timeline->lengthTicks, timeline->loopEndTick);

    std::sort(timeSigs.begin(), timeSigs.end(),
              [](const TimeSigPoint &a, const TimeSigPoint &b) { return a.tick < b.tick; });
    timeline->timeSigs = std::move(timeSigs);

    // Map the not-played events onto engine tracks (metas keep -1 unless their
    // SMF chunk got an engine slot) and time them for display.
    std::sort(rawOthers.begin(), rawOthers.end(),
              [](const RawOther &a, const RawOther &b) { return a.tick < b.tick; });
    timeline->otherEvents.reserve(rawOthers.size());
    for (RawOther &ro : rawOthers) {
        const int engineTrack = smfToEngine[ro.smfTrack];
        timeline->otherEvents.push_back({ro.tick,
                                         tickToSample(ro.tick, tempos, tpqn, sampleRate),
                                         engineTrack, std::move(ro.label)});
        timeline->lengthTicks = std::max(timeline->lengthTicks, ro.tick);
    }

    return timeline;
}

uint64_t MidiTimeline::sampleForTick(uint64_t tick) const
{
    // tempoMap always has an entry at tick 0.
    const TempoPoint *tp = &tempoMap.front();
    for (const TempoPoint &p : tempoMap) {
        if (p.tick > tick)
            break;
        tp = &p;
    }
    const double samplesPerTick = 60.0 / tp->bpm * sampleRate / double(ticksPerBeat);
    return tp->samplePos + uint64_t(double(tick - tp->tick) * samplesPerTick + 0.5);
}

double MidiTimeline::tickForSample(uint64_t samplePos) const
{
    const TempoPoint *tp = &tempoMap.front();
    for (const TempoPoint &p : tempoMap) {
        if (p.samplePos > samplePos)
            break;
        tp = &p;
    }
    const double samplesPerTick = 60.0 / tp->bpm * sampleRate / double(ticksPerBeat);
    return double(tp->tick) + double(samplePos - tp->samplePos) / samplesPerTick;
}
