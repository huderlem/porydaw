#include "sf2reader.h"

#include <QFile>
#include <QFileInfo>

#include <cmath>
#include <cstring>

#include "project/samplereg.h"
#include "sampledata.h"

namespace {

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

QString corruptText()
{
    return QStringLiteral("the SoundFont file is corrupt or truncated.");
}

quint16 leU16(const quint8 *p)
{
    return quint16(p[0] | quint16(p[1]) << 8);
}

quint32 leU32(const quint8 *p)
{
    return quint32(p[0]) | quint32(p[1]) << 8 | quint32(p[2]) << 16
        | quint32(p[3]) << 24;
}

// Fixed 20-byte record name, NUL-trimmed.
QString recName(const quint8 *p)
{
    int len = 0;
    while (len < 20 && p[len] != 0)
        len++;
    return QString::fromLatin1(reinterpret_cast<const char *>(p), len)
        .trimmed();
}

} // namespace

bool sf2Magic(const QByteArray &bytes)
{
    return bytes.size() >= 12 && bytes.startsWith("RIFF")
        && bytes.mid(8, 4) == "sfbk";
}

bool readSf2Bytes(const QByteArray &bytes, const QString &sourcePath,
                  Sf2File *out, QString *error)
{
    *out = Sf2File();
    out->sourcePath = sourcePath;
    if (!sf2Magic(bytes))
        return fail(error, QStringLiteral("not a SoundFont (.sf2) file."));
    const quint8 *data = reinterpret_cast<const quint8 *>(bytes.constData());
    const qint64 total = bytes.size();

    // Locate the consumed sub-chunks (FORMATS.md §5 subset). Sub-chunk ids
    // are unique across the three LISTs, so a flat scan suffices; declared
    // sizes overrunning the buffer are a hard refusal, not a best-effort
    // parse — every record index below trusts them.
    struct Span {
        qint64 at = -1;
        qint64 len = 0;
    };
    Span smpl, phdr, pbag, pgen, inst, ibag, igen, shdr;
    const struct {
        const char *id;
        Span *span;
    } wanted[] = {{"smpl", &smpl}, {"phdr", &phdr}, {"pbag", &pbag},
                  {"pgen", &pgen}, {"inst", &inst}, {"ibag", &ibag},
                  {"igen", &igen}, {"shdr", &shdr}};

    qint64 pos = 12;
    while (pos + 8 <= total) {
        const qint64 chunkLen = leU32(data + pos + 4);
        const qint64 at = pos + 8;
        if (at + chunkLen > total)
            return fail(error, corruptText());
        if (std::memcmp(data + pos, "LIST", 4) == 0 && chunkLen >= 4) {
            qint64 sp = at + 4;
            const qint64 listEnd = at + chunkLen;
            while (sp + 8 <= listEnd) {
                const qint64 subLen = leU32(data + sp + 4);
                if (sp + 8 + subLen > listEnd)
                    return fail(error, corruptText());
                for (const auto &w : wanted) {
                    if (std::memcmp(data + sp, w.id, 4) == 0
                        && w.span->at < 0) {
                        w.span->at = sp + 8;
                        w.span->len = subLen;
                    }
                }
                sp += 8 + subLen + (subLen & 1);
            }
        }
        pos = at + chunkLen + (chunkLen & 1);
    }
    if (smpl.at < 0)
        return fail(error, QStringLiteral(
                        "the SoundFont has no sample data (smpl chunk)."));
    if (shdr.at < 0 || shdr.len < 46)
        return fail(error, QStringLiteral(
                        "the SoundFont has no sample headers (shdr chunk)."));

    out->pool = bytes.mid(smpl.at, smpl.len & ~qint64(1));

    // Grouping labels only: sample → owning instrument via inst/ibag/igen
    // (sampleID generator, oper 53), instrument → preset via phdr/pbag/pgen
    // (instrument generator, oper 41). First reference wins; every index is
    // bounds-guarded because these arrays cross-reference each other.
    const qint64 nShdr = shdr.len / 46;
    const qint64 nInst = inst.at >= 0 ? inst.len / 22 : 0;
    const qint64 nIbag = ibag.at >= 0 ? ibag.len / 4 : 0;
    const qint64 nIgen = igen.at >= 0 ? igen.len / 4 : 0;
    const qint64 nPhdr = phdr.at >= 0 ? phdr.len / 38 : 0;
    const qint64 nPbag = pbag.at >= 0 ? pbag.len / 4 : 0;
    const qint64 nPgen = pgen.at >= 0 ? pgen.len / 4 : 0;

    std::vector<int> sampleInst(size_t(nShdr), -1);
    for (qint64 i = 0; i + 1 < nInst; i++) {
        const qint64 bagLo = leU16(data + inst.at + i * 22 + 20);
        const qint64 bagHi = leU16(data + inst.at + (i + 1) * 22 + 20);
        for (qint64 b = bagLo; b < qMin(bagHi, nIbag - 1); b++) {
            const qint64 genLo = leU16(data + ibag.at + b * 4);
            const qint64 genHi = leU16(data + ibag.at + (b + 1) * 4);
            for (qint64 g = genLo; g < qMin(genHi, nIgen); g++) {
                if (leU16(data + igen.at + g * 4) != 53)
                    continue;
                const quint16 s = leU16(data + igen.at + g * 4 + 2);
                if (s < nShdr && sampleInst[s] < 0)
                    sampleInst[s] = int(i);
            }
        }
    }
    std::vector<int> instPreset(size_t(nInst), -1);
    for (qint64 i = 0; i + 1 < nPhdr; i++) {
        const qint64 bagLo = leU16(data + phdr.at + i * 38 + 24);
        const qint64 bagHi = leU16(data + phdr.at + (i + 1) * 38 + 24);
        for (qint64 b = bagLo; b < qMin(bagHi, nPbag - 1); b++) {
            const qint64 genLo = leU16(data + pbag.at + b * 4);
            const qint64 genHi = leU16(data + pbag.at + (b + 1) * 4);
            for (qint64 g = genLo; g < qMin(genHi, nPgen); g++) {
                if (leU16(data + pgen.at + g * 4) != 41)
                    continue;
                const quint16 target = leU16(data + pgen.at + g * 4 + 2);
                if (target < nInst && instPreset[target] < 0)
                    instPreset[target] = int(i);
            }
        }
    }

    const qint64 poolFrames = out->pool.size() / 2;
    for (qint64 r = 0; r < nShdr; r++) {
        const quint8 *p = data + shdr.at + r * 46;
        Sf2Zone zone;
        zone.name = recName(p);
        zone.start = leU32(p + 20);
        zone.end = leU32(p + 24);
        zone.loopStart = leU32(p + 28);
        zone.loopEndExcl = leU32(p + 32);
        zone.sampleRate = leU32(p + 36);
        zone.originalPitch = p[40];
        zone.pitchCorrection = qint8(p[41]);
        zone.sampleType = leU16(p + 44);
        if (zone.sampleType & 0x8000)
            continue; // ROM sample — skipped (FORMATS.md §5)
        // Also drops the all-zero EOS terminator record.
        if (zone.end <= zone.start || qint64(zone.end) > poolFrames
            || zone.sampleRate == 0)
            continue;
        if (sampleInst[size_t(r)] >= 0) {
            const int i = sampleInst[size_t(r)];
            zone.instrument = recName(data + inst.at + qint64(i) * 22);
            if (instPreset[size_t(i)] >= 0)
                zone.preset = recName(
                    data + phdr.at + qint64(instPreset[size_t(i)]) * 38);
        }
        out->zones.push_back(zone);
    }
    if (out->zones.empty())
        return fail(error, QStringLiteral(
                        "the SoundFont contains no importable samples."));
    return true;
}

bool readSf2File(const QString &path, Sf2File *out, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("cannot read %1.").arg(path));
    return readSf2Bytes(file.readAll(), path, out, error);
}

bool extractSf2Zone(const Sf2File &file, int zoneIndex, ImportedSample *out,
                    QString *error)
{
    if (zoneIndex < 0 || zoneIndex >= int(file.zones.size()))
        return fail(error, QStringLiteral("no SoundFont zone selected."));
    const Sf2Zone &zone = file.zones[size_t(zoneIndex)];

    *out = ImportedSample();
    out->sourcePath = file.sourcePath;
    out->suggestedName = SampleRegistrar::sanitizeSampleName(zone.name);
    if (out->suggestedName.isEmpty())
        out->suggestedName = SampleRegistrar::sanitizeSampleName(
            QFileInfo(file.sourcePath).completeBaseName());
    out->sourceKind = ImportedSample::Sf2;
    out->sourceChannels = 1;
    out->sourceBits = 16;

    const qint64 n = zone.frames();
    out->buffer.resize(size_t(n));
    const quint8 *p = reinterpret_cast<const quint8 *>(file.pool.constData())
        + qint64(zone.start) * 2;
    for (qint64 i = 0; i < n; i++)
        out->buffer[size_t(i)] =
            float(double(qint16(leU16(p + i * 2))) / 32768.0);
    out->sampleRate = double(zone.sampleRate);
    out->playLength = n;

    // byOriginalPitch > 127 is the "unpitched" convention: keep the default
    // key and let the editor prefill from pitch detection instead.
    if (zone.originalPitch <= 127) {
        const double exactKey =
            qBound(0.0, double(zone.originalPitch)
                       + double(zone.pitchCorrection) / 100.0,
                   127.99);
        out->baseKey = int(std::floor(exactKey));
        out->fracSemitone = exactKey - std::floor(exactKey);
        out->hasPitchMetadata = true;
    }
    // Exclusive → inclusive loop-end conversion happens here, at the
    // boundary (FORMATS.md §5). Degenerate loop fields are routine on
    // one-shot sf2 zones — no warning, unlike an authored smpl chunk.
    if (zone.hasLoop()) {
        out->hasLoop = true;
        out->loopStart = qint64(zone.loopStart) - qint64(zone.start);
        out->loopEndIncl = qint64(zone.loopEndExcl) - qint64(zone.start) - 1;
    }
    if (zone.stereoPair())
        out->warnings +=
            QStringLiteral("stereo pair — imported one channel.");
    return true;
}
