#include "miditimeline.h"

#include <QFile>
#include <algorithm>
#include <cstring>

namespace {

struct Reader {
    const uint8_t *data;
    size_t size;
    size_t pos = 0;

    bool readByte(uint8_t *out)
    {
        if (pos >= size)
            return false;
        *out = data[pos++];
        return true;
    }

    bool readU16(uint16_t *out)
    {
        if (pos + 2 > size)
            return false;
        *out = static_cast<uint16_t>((data[pos] << 8) | data[pos + 1]);
        pos += 2;
        return true;
    }

    bool readU32(uint32_t *out)
    {
        if (pos + 4 > size)
            return false;
        *out = (static_cast<uint32_t>(data[pos]) << 24)
             | (static_cast<uint32_t>(data[pos + 1]) << 16)
             | (static_cast<uint32_t>(data[pos + 2]) << 8)
             | static_cast<uint32_t>(data[pos + 3]);
        pos += 4;
        return true;
    }

    bool skip(uint32_t n)
    {
        if (pos + n > size)
            return false;
        pos += n;
        return true;
    }

    // Variable-length quantity, up to 4 bytes.
    bool readVlq(uint32_t *out)
    {
        uint32_t val = 0;
        for (int i = 0; i < 4; i++) {
            uint8_t b;
            if (!readByte(&b))
                return false;
            val = (val << 7) | (b & 0x7F);
            if (!(b & 0x80)) {
                *out = val;
                return true;
            }
        }
        return false;
    }
};

struct RawEvent {
    uint64_t tick;
    uint8_t channel;
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
    int channel; // MIDI channel, or -1 for metas/sysex
    QString label;
};

// True when buf is exactly the single character `marker` after stripping
// leading/trailing ASCII whitespace.
bool textIsLoopMarker(const uint8_t *buf, uint32_t len, char marker)
{
    uint32_t s = 0, e = len;
    while (s < e && (buf[s] == ' ' || buf[s] == '\t' || buf[s] == '\r' || buf[s] == '\n'))
        s++;
    while (e > s && (buf[e - 1] == ' ' || buf[e - 1] == '\t' || buf[e - 1] == '\r' || buf[e - 1] == '\n'))
        e--;
    return (e - s == 1) && static_cast<char>(buf[s]) == marker;
}

struct TrackParseResult {
    QString name;
    bool hasChannelEvents = false;
};

void parseTrack(Reader &r, uint32_t trackLen, uint16_t trackIndex,
                std::vector<RawEvent> &rawEvents, std::vector<TempoChange> &tempos,
                std::vector<TimeSigPoint> &timeSigs, std::vector<RawOther> &others,
                uint64_t *loopStartTick, uint64_t *loopEndTick, TrackParseResult *result)
{
    const size_t end = r.pos + trackLen;
    uint64_t tick = 0;
    uint8_t runningStatus = 0;

    auto push = [&](uint8_t channel, uint8_t type, uint8_t d0, uint8_t d1) {
        RawEvent ev;
        ev.tick = tick;
        ev.channel = channel;
        ev.smfTrack = trackIndex;
        ev.type = type;
        ev.data0 = d0;
        ev.data1 = d1;
        ev.origIndex = static_cast<int>(rawEvents.size());
        rawEvents.push_back(ev);
        result->hasChannelEvents = true;
    };

    while (r.pos < end) {
        uint32_t delta;
        if (!r.readVlq(&delta))
            break;
        tick += delta;

        uint8_t b;
        if (!r.readByte(&b))
            break;

        if (b == 0xFF) {
            runningStatus = 0;
            uint8_t metaType;
            uint32_t metaLen;
            if (!r.readByte(&metaType) || !r.readVlq(&metaLen))
                break;

            if (metaType == 0x51 && metaLen == 3) {
                uint8_t t0, t1, t2;
                if (!r.readByte(&t0) || !r.readByte(&t1) || !r.readByte(&t2))
                    break;
                tempos.push_back({tick, (static_cast<uint32_t>(t0) << 16)
                                            | (static_cast<uint32_t>(t1) << 8) | t2});
            } else if (metaType == 0x58 && metaLen >= 2) {
                uint8_t num, den;
                if (!r.readByte(&num) || !r.readByte(&den))
                    break;
                if (!r.skip(metaLen - 2))
                    break;
                timeSigs.push_back({tick, num, den});
            } else if (metaType == 0x03 && result->name.isEmpty()) {
                // Track name
                uint32_t readLen = std::min<uint32_t>(metaLen, 64);
                char nameBuf[64];
                for (uint32_t j = 0; j < readLen; j++) {
                    uint8_t c;
                    if (!r.readByte(&c))
                        goto trackDone;
                    nameBuf[j] = static_cast<char>(c);
                }
                if (metaLen > readLen && !r.skip(metaLen - readLen))
                    goto trackDone;
                result->name = QString::fromLatin1(nameBuf, readLen).trimmed();
            } else if (metaType >= 0x01 && metaType <= 0x07) {
                // Text-type meta: check for loop markers ('[' / ']'), most
                // commonly meta 0x01 (Text) or 0x06 (Marker).
                uint32_t readLen = std::min<uint32_t>(metaLen, 32);
                uint8_t textBuf[32];
                for (uint32_t j = 0; j < readLen; j++) {
                    if (!r.readByte(&textBuf[j]))
                        goto trackDone;
                }
                if (metaLen > readLen && !r.skip(metaLen - readLen))
                    goto trackDone;
                if (textIsLoopMarker(textBuf, readLen, '[') && *loopStartTick == UINT64_MAX)
                    *loopStartTick = tick;
                else if (textIsLoopMarker(textBuf, readLen, ']') && *loopEndTick == UINT64_MAX)
                    *loopEndTick = tick;
                else {
                    static const char *const kTextMetaNames[] = {
                        "Text", "Copyright", "Track name", "Instrument",
                        "Lyric", "Marker", "Cue point"};
                    const QString text =
                        QString::fromLatin1(reinterpret_cast<const char *>(textBuf), readLen)
                            .trimmed();
                    if (!text.isEmpty())
                        others.push_back({tick, trackIndex, -1,
                                          QStringLiteral("%1: %2")
                                              .arg(QLatin1String(kTextMetaNames[metaType - 1]),
                                                   text)});
                }
            } else if (metaType == 0x2F) {
                if (!r.skip(metaLen))
                    break;
            } else {
                if (!r.skip(metaLen))
                    break;
                others.push_back({tick, trackIndex, -1,
                                  QStringLiteral("Meta 0x%1 (%2 bytes)")
                                      .arg(metaType, 2, 16, QLatin1Char('0'))
                                      .arg(metaLen)});
            }
        } else if (b == 0xF0 || b == 0xF7) {
            runningStatus = 0;
            uint32_t sysexLen;
            if (!r.readVlq(&sysexLen) || !r.skip(sysexLen))
                break;
            others.push_back({tick, trackIndex, -1,
                              QStringLiteral("SysEx (%1 bytes)").arg(sysexLen)});
        } else {
            uint8_t status, data0;
            if (b & 0x80) {
                status = b;
                runningStatus = b;
                if (!r.readByte(&data0))
                    break;
            } else {
                if (!runningStatus)
                    break;
                status = runningStatus;
                data0 = b;
            }

            const uint8_t type = (status >> 4) & 0x0F;
            const uint8_t chan = status & 0x0F;
            uint8_t data1;

            switch (type) {
            case 0x8: // Note off
                if (!r.readByte(&data1))
                    goto trackDone;
                push(chan, 0x8, data0, data1);
                break;
            case 0x9: // Note on (velocity 0 means note off)
                if (!r.readByte(&data1))
                    goto trackDone;
                push(chan, data1 ? 0x9 : 0x8, data0, data1);
                break;
            case 0xA: // Polyphonic aftertouch: not played
                if (!r.readByte(&data1))
                    goto trackDone;
                others.push_back({tick, trackIndex, chan,
                                  QStringLiteral("Poly aftertouch key %1 = %2")
                                      .arg(data0)
                                      .arg(data1)});
                break;
            case 0xB: // Control change
                if (!r.readByte(&data1))
                    goto trackDone;
                push(chan, 0xB, data0, data1);
                break;
            case 0xC: // Program change (1 data byte)
                push(chan, 0xC, data0, 0);
                break;
            case 0xD: // Channel pressure: not played (data0 already consumed)
                others.push_back({tick, trackIndex, chan,
                                  QStringLiteral("Channel pressure %1").arg(data0)});
                break;
            case 0xE: // Pitch bend (LSB in data0)
                if (!r.readByte(&data1))
                    goto trackDone;
                push(chan, 0xE, data0, data1);
                break;
            default:
                others.push_back({tick, trackIndex, -1,
                                  QStringLiteral("Unrecognized status 0x%1 — rest of SMF track "
                                                 "%2 skipped")
                                      .arg(status, 2, 16, QLatin1Char('0'))
                                      .arg(trackIndex)});
                goto trackDone; // unknown status byte, bail on this track
            }
        }
    }
trackDone:
    r.pos = end;
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
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Cannot open MIDI file: %1").arg(path);
        return nullptr;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    Reader r{reinterpret_cast<const uint8_t *>(bytes.constData()),
             static_cast<size_t>(bytes.size())};

    if (r.size < 14 || memcmp(r.data, "MThd", 4) != 0) {
        if (error)
            *error = QStringLiteral("Not a Standard MIDI File: %1").arg(path);
        return nullptr;
    }
    r.pos = 4;

    uint32_t hdrLen;
    uint16_t format, numTracks, division;
    if (!r.readU32(&hdrLen) || !r.readU16(&format) || !r.readU16(&numTracks)
        || !r.readU16(&division)) {
        if (error)
            *error = QStringLiteral("Invalid MIDI header");
        return nullptr;
    }
    if (hdrLen > 6)
        r.pos += hdrLen - 6;

    if (format > 1) {
        if (error)
            *error = QStringLiteral("Unsupported MIDI format %1 (only 0 and 1 supported)").arg(format);
        return nullptr;
    }
    if (division & 0x8000) {
        if (error)
            *error = QStringLiteral("SMPTE time division is not supported");
        return nullptr;
    }
    const uint32_t tpqn = division;

    std::vector<RawEvent> rawEvents;
    std::vector<TempoChange> tempos;
    std::vector<TimeSigPoint> timeSigs;
    std::vector<RawOther> rawOthers;
    std::vector<TrackParseResult> trackResults(numTracks);
    uint64_t loopStartTick = UINT64_MAX;
    uint64_t loopEndTick = UINT64_MAX;

    for (uint16_t t = 0; t < numTracks; t++) {
        if (r.pos + 8 > r.size)
            break;
        if (memcmp(r.data + r.pos, "MTrk", 4) != 0)
            break;
        r.pos += 4;
        uint32_t trackLen;
        if (!r.readU32(&trackLen))
            break;
        const size_t trackEnd = r.pos + trackLen;
        parseTrack(r, trackLen, t, rawEvents, tempos, timeSigs, rawOthers, &loopStartTick,
                   &loopEndTick, &trackResults[t]);
        r.pos = trackEnd;
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

    // Map SMF tracks (Type 1) or MIDI channels (Type 0) to engine tracks.
    std::vector<int> smfToEngine(numTracks, -1);
    if (format == 1) {
        int next = 0;
        for (uint16_t t = 0; t < numTracks; t++) {
            if (!trackResults[t].hasChannelEvents)
                continue;
            if (next < 16) {
                smfToEngine[t] = next;
                timeline->tracks[next].name = trackResults[t].name;
                next++;
            } else {
                timeline->droppedTracks++;
            }
        }
        timeline->usedTrackCount = next;
    }

    // Convert raw events to sample positions with final engine track indices.
    timeline->events.reserve(rawEvents.size() + tempos.size() + 1);

    std::vector<TimelineEvent> noteEvents;
    noteEvents.reserve(rawEvents.size());
    for (const RawEvent &re : rawEvents) {
        int engineTrack;
        if (format == 1) {
            engineTrack = smfToEngine[re.smfTrack];
            if (engineTrack < 0)
                continue; // beyond 16 usable tracks
        } else {
            engineTrack = re.channel;
        }

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

    if (format == 0) {
        int used = 0;
        for (int i = 0; i < 16; i++) {
            if (timeline->tracks[i].used) {
                if (timeline->tracks[i].name.isEmpty())
                    timeline->tracks[i].name = QStringLiteral("Channel %1").arg(i + 1);
                used = i + 1;
            }
        }
        timeline->usedTrackCount = used;
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
        int engineTrack = -1;
        if (format == 1)
            engineTrack = smfToEngine[ro.smfTrack];
        else if (ro.channel >= 0)
            engineTrack = ro.channel;
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
