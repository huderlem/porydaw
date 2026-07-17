#include "smf.h"

#include <QFile>
#include <algorithm>
#include <array>
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

    bool readBlob(uint32_t n, QByteArray *out)
    {
        if (pos + n > size)
            return false;
        *out = QByteArray(reinterpret_cast<const char *>(data + pos), int(n));
        pos += n;
        return true;
    }
};

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

// Parses one MTrk chunk body. r.pos is at the chunk data; end is one past it.
bool parseTrack(Reader &r, size_t end, int trackIndex, SmfTrack *track, QString *error)
{
    uint64_t tick = 0;
    uint8_t runningStatus = 0;
    bool sawEot = false;

    while (r.pos < end) {
        uint32_t delta;
        if (!r.readVlq(&delta))
            return fail(error, QStringLiteral("Track %1: truncated delta time").arg(trackIndex));
        tick += delta;

        uint8_t b;
        if (!r.readByte(&b))
            return fail(error, QStringLiteral("Track %1: truncated event").arg(trackIndex));

        if (b == 0xFF) {
            runningStatus = 0;
            uint8_t metaType;
            uint32_t metaLen;
            if (!r.readByte(&metaType) || !r.readVlq(&metaLen))
                return fail(error,
                            QStringLiteral("Track %1: truncated meta event").arg(trackIndex));
            if (metaType == 0x2F) {
                if (r.pos + metaLen > end)
                    return fail(error,
                                QStringLiteral("Track %1: truncated end-of-track").arg(trackIndex));
                r.pos += metaLen;
                track->endTick = tick;
                sawEot = true;
                break; // anything after EOT in the chunk is ignored, as mid2agb does
            }
            SmfEvent ev;
            ev.tick = tick;
            ev.status = 0xFF;
            ev.metaType = metaType;
            if (!r.readBlob(metaLen, &ev.blob))
                return fail(error,
                            QStringLiteral("Track %1: truncated meta payload").arg(trackIndex));
            track->events.push_back(std::move(ev));
        } else if (b == 0xF0 || b == 0xF7) {
            runningStatus = 0;
            uint32_t sysexLen;
            SmfEvent ev;
            ev.tick = tick;
            ev.status = b;
            if (!r.readVlq(&sysexLen) || !r.readBlob(sysexLen, &ev.blob))
                return fail(error,
                            QStringLiteral("Track %1: truncated SysEx event").arg(trackIndex));
            track->events.push_back(std::move(ev));
        } else {
            uint8_t status, data0;
            if (b & 0x80) {
                status = b;
                runningStatus = b;
                if (!r.readByte(&data0))
                    return fail(error,
                                QStringLiteral("Track %1: truncated event data").arg(trackIndex));
            } else {
                if (!runningStatus)
                    return fail(error,
                                QStringLiteral("Track %1: data byte with no running status")
                                    .arg(trackIndex));
                status = runningStatus;
                data0 = b;
            }
            if (status >= 0xF0)
                return fail(error, QStringLiteral("Track %1: unexpected status 0x%2")
                                       .arg(trackIndex)
                                       .arg(status, 2, 16, QLatin1Char('0')));

            SmfEvent ev;
            ev.tick = tick;
            ev.status = status;
            ev.data0 = data0;
            const uint8_t type = status >> 4;
            if (type != 0xC && type != 0xD) { // all others carry two data bytes
                if (!r.readByte(&ev.data1))
                    return fail(error,
                                QStringLiteral("Track %1: truncated event data").arg(trackIndex));
            }
            track->events.push_back(std::move(ev));
        }
    }

    if (!sawEot)
        track->endTick = tick;
    return true;
}

void writeVlq(QByteArray &out, uint32_t value)
{
    uint8_t buf[4];
    int n = 0;
    do {
        buf[n++] = value & 0x7F;
        value >>= 7;
    } while (value && n < 4);
    while (n > 1)
        out.append(char(buf[--n] | 0x80));
    out.append(char(buf[0]));
}

void writeU32(QByteArray &out, uint32_t v)
{
    out.append(char(v >> 24));
    out.append(char(v >> 16));
    out.append(char(v >> 8));
    out.append(char(v));
}

void writeU16(QByteArray &out, uint16_t v)
{
    out.append(char(v >> 8));
    out.append(char(v));
}

} // namespace

bool SmfFile::read(const QByteArray &bytes, SmfFile *out, QString *error)
{
    Reader r{reinterpret_cast<const uint8_t *>(bytes.constData()),
             static_cast<size_t>(bytes.size())};

    if (r.size < 14 || memcmp(r.data, "MThd", 4) != 0)
        return fail(error, QStringLiteral("Not a Standard MIDI File"));
    r.pos = 4;

    uint32_t hdrLen;
    uint16_t format, numTracks, division;
    if (!r.readU32(&hdrLen) || !r.readU16(&format) || !r.readU16(&numTracks)
        || !r.readU16(&division))
        return fail(error, QStringLiteral("Invalid MIDI header"));
    if (hdrLen < 6)
        return fail(error, QStringLiteral("Invalid MIDI header length"));
    r.pos += hdrLen - 6;

    if (format > 1)
        return fail(error, QStringLiteral("Unsupported MIDI format %1 (only 0 and 1 supported)")
                               .arg(format));
    if (division & 0x8000)
        return fail(error, QStringLiteral("SMPTE time division is not supported"));
    if (division == 0)
        return fail(error, QStringLiteral("Invalid time division 0"));

    out->format = format;
    out->division = division;
    out->tracks.clear();
    out->tracks.reserve(numTracks);

    for (uint16_t t = 0; t < numTracks; t++) {
        if (r.pos + 8 > r.size || memcmp(r.data + r.pos, "MTrk", 4) != 0)
            return fail(error, QStringLiteral("Missing MTrk chunk for track %1 of %2")
                                   .arg(t + 1)
                                   .arg(numTracks));
        r.pos += 4;
        uint32_t trackLen;
        if (!r.readU32(&trackLen) || r.pos + trackLen > r.size)
            return fail(error, QStringLiteral("Truncated track %1").arg(t));
        const size_t trackEnd = r.pos + trackLen;

        SmfTrack track;
        if (!parseTrack(r, trackEnd, t, &track, error))
            return false;
        out->tracks.push_back(std::move(track));
        r.pos = trackEnd;
    }
    return true;
}

bool SmfFile::readFile(const QString &path, SmfFile *out, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("Cannot open MIDI file: %1").arg(path));
    return read(file.readAll(), out, error);
}

QByteArray SmfFile::write() const
{
    QByteArray out;
    out.append("MThd");
    writeU32(out, 6);
    writeU16(out, format);
    writeU16(out, uint16_t(tracks.size()));
    writeU16(out, division);

    for (const SmfTrack &track : tracks) {
        QByteArray body;
        uint64_t tick = 0;
        uint8_t runningStatus = 0;
        for (const SmfEvent &ev : track.events) {
            writeVlq(body, uint32_t(ev.tick - tick));
            tick = ev.tick;
            if (ev.isMeta()) {
                body.append(char(0xFF));
                body.append(char(ev.metaType));
                writeVlq(body, uint32_t(ev.blob.size()));
                body.append(ev.blob);
                runningStatus = 0;
            } else if (ev.isSysEx()) {
                body.append(char(ev.status));
                writeVlq(body, uint32_t(ev.blob.size()));
                body.append(ev.blob);
                runningStatus = 0;
            } else {
                if (ev.status != runningStatus) {
                    body.append(char(ev.status));
                    runningStatus = ev.status;
                }
                body.append(char(ev.data0));
                const uint8_t type = ev.typeNibble();
                if (type != 0xC && type != 0xD)
                    body.append(char(ev.data1));
            }
        }
        // End of track, at its preserved tick (never before the last event).
        const uint64_t eotTick = std::max(track.endTick, tick);
        writeVlq(body, uint32_t(eotTick - tick));
        body.append(char(0xFF));
        body.append(char(0x2F));
        body.append(char(0x00));

        out.append("MTrk");
        writeU32(out, uint32_t(body.size()));
        out.append(body);
    }
    return out;
}

bool SmfFile::writeFile(const QString &path, QString *error) const
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return fail(error, QStringLiteral("Cannot write MIDI file: %1").arg(path));
    const QByteArray bytes = write();
    if (file.write(bytes) != bytes.size())
        return fail(error, QStringLiteral("Short write to %1").arg(path));
    return true;
}

void convertToFormat1(SmfFile *smf)
{
    if (smf->format != 0)
        return;
    smf->format = 1;
    if (smf->tracks.empty())
        return;

    SmfTrack conductor;
    std::array<SmfTrack, 16> channels;
    std::array<bool, 16> used{}; // has channel events (a name alone doesn't
                                 // occupy an engine slot in either format)
    uint64_t endTick = 0;

    // A format-0 file has one chunk; out-of-spec extras get the same
    // treatment, merged into the shared buckets.
    for (const SmfTrack &track : smf->tracks) {
        endTick = std::max(endTick, track.endTick);
        // Channel Prefix (0x20) scopes following name metas to a channel,
        // live until the next channel event or prefix — the readers'
        // (MidiTimeline, trackNameLoc) exact rule.
        int prefixChannel = -1;
        for (const SmfEvent &ev : track.events) {
            if (ev.isChannel()) {
                prefixChannel = -1;
                channels[ev.channel()].events.push_back(ev);
                used[ev.channel()] = true;
            } else if (ev.isMeta() && ev.metaType == 0x20 && ev.blob.size() >= 1) {
                prefixChannel = ev.blob[0] & 0x0F;
            } else if (ev.isMeta() && ev.metaType == 0x03 && prefixChannel >= 0) {
                channels[prefixChannel].events.push_back(ev);
            } else {
                conductor.events.push_back(ev);
            }
        }
    }

    smf->tracks.clear();
    conductor.endTick = endTick;
    std::stable_sort(conductor.events.begin(), conductor.events.end(),
                     [](const SmfEvent &a, const SmfEvent &b) {
                         return a.tick < b.tick;
                     });
    smf->tracks.push_back(std::move(conductor));
    for (int c = 0; c < 16; c++) {
        if (!used[c])
            continue;
        channels[c].endTick = endTick;
        // Already tick-sorted for a single source chunk; a merge of
        // out-of-spec extra chunks may need it (stable: same-tick order
        // within each source chunk survives).
        std::stable_sort(channels[c].events.begin(), channels[c].events.end(),
                         [](const SmfEvent &a, const SmfEvent &b) {
                             return a.tick < b.tick;
                         });
        smf->tracks.push_back(std::move(channels[c]));
    }
}
