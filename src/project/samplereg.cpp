#include "samplereg.h"

#include "sidecar.h"
#include "voicegroupsource.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <cmath>

namespace {

QByteArray readAllBytes(const QString &path, bool *ok = nullptr)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (ok)
            *ok = false;
        return QByteArray();
    }
    if (ok)
        *ok = true;
    return file.readAll();
}

// The project's make files that can hold the sample build rules: the root
// Makefile and every root-level .mk (pokeemerald keeps them in
// audio_rules.mk, included from the Makefile).
QStringList makeFilePaths(const QString &projectRoot)
{
    QStringList paths = {projectRoot + QStringLiteral("/Makefile"),
                         projectRoot + QStringLiteral("/makefile")};
    QDirIterator it(projectRoot, {QStringLiteral("*.mk")}, QDir::Files);
    while (it.hasNext())
        paths.append(it.next());
    return paths;
}

// Whether any make file has a pattern rule "%.bin: ...%.<srcExt>" whose
// recipe mentions the given tool (as $(VAR) or a literal path).
bool hasPatternRule(const QStringList &makeFiles, const char *srcExt,
                    const char *tool)
{
    const QRegularExpression targetRe(
        QStringLiteral(R"(%\.bin\s*:(?!=).*%\.%1\s*$)")
            .arg(QLatin1String(srcExt)));
    for (const QString &path : makeFiles) {
        bool ok = false;
        const QByteArray content = readAllBytes(path, &ok);
        if (!ok)
            continue;
        const QList<QByteArray> lines = content.split('\n');
        for (int i = 0; i < lines.size(); i++) {
            QByteArray line = lines.at(i);
            if (line.endsWith('\r'))
                line.chop(1);
            if (!targetRe.match(QString::fromUtf8(line)).hasMatch())
                continue;
            // Recipe lines (tab-indented) follow until the next non-recipe.
            for (int j = i + 1; j < lines.size(); j++) {
                const QByteArray recipe = lines.at(j);
                if (!recipe.startsWith('\t'))
                    break;
                if (recipe.toLower().contains(tool))
                    return true;
            }
        }
    }
    return false;
}

} // namespace

SampleFormatProbe SampleRegistrar::probeSampleFormat(const QString &projectRoot)
{
    SampleFormatProbe probe;
    probe.incPath = projectRoot + QStringLiteral("/sound/direct_sound_data.inc");
    probe.samplesDir = projectRoot + QStringLiteral("/sound/direct_sound_samples");
    if (!QFile::exists(probe.incPath)) {
        probe.refusal = QStringLiteral(
            "cannot find sound/direct_sound_data.inc — samples are registered "
            "there. Set up pret's sample layout, then import again.");
        return probe;
    }
    const QStringList makeFiles = makeFilePaths(projectRoot);
    if (hasPatternRule(makeFiles, "wav", "wav2agb")) {
        probe.pipeline = SampleFormatProbe::Wav2Agb;
        return probe;
    }
    if (hasPatternRule(makeFiles, "aif", "aif2pcm")) {
        probe.pipeline = SampleFormatProbe::LegacyAif;
        probe.refusal = QStringLiteral(
            "this project predates wav2agb: its samples build from .aif "
            "sources via aif2pcm. Port the sample pipeline to wav2agb "
            "(pret's current layout), then import again.");
        return probe;
    }
    probe.refusal = QStringLiteral(
        "cannot find a wav2agb build rule (%.bin: %.wav) in the project's "
        "make files; add pret's audio_rules.mk pattern rule, then import "
        "again.");
    return probe;
}

QString SampleRegistrar::sanitizeSampleName(const QString &raw)
{
    QString name;
    bool pendingSep = false;
    for (const QChar c : raw.toLower()) {
        if ((c >= QLatin1Char('a') && c <= QLatin1Char('z'))
            || (c >= QLatin1Char('0') && c <= QLatin1Char('9'))) {
            if (pendingSep && !name.isEmpty())
                name += QLatin1Char('_');
            pendingSep = false;
            name += c;
        } else {
            pendingSep = true;
        }
    }
    return name;
}

bool SampleRegistrar::validateSampleName(const QString &projectRoot,
                                         const QString &name,
                                         const QStringList &existingSymbols,
                                         QString *error)
{
    static const QRegularExpression grammarRe(QStringLiteral("^[a-z0-9_]+$"));
    if (name.isEmpty()) {
        if (error)
            *error = QStringLiteral("sample name is empty.");
        return false;
    }
    if (!grammarRe.match(name).hasMatch()) {
        if (error)
            *error = QStringLiteral(
                "sample names use lowercase letters, digits, and underscores "
                "only.");
        return false;
    }
    const QString symbol = QStringLiteral("DirectSoundWaveData_") + name;
    if (existingSymbols.contains(symbol)) {
        if (error)
            *error =
                QStringLiteral("%1 already exists in this project.").arg(symbol);
        return false;
    }
    const QString samplesDir =
        projectRoot + QStringLiteral("/sound/direct_sound_samples/");
    for (const char *ext : {".wav", ".bin", ".aif"}) {
        if (QFile::exists(samplesDir + name + QLatin1String(ext))) {
            if (error)
                *error = QStringLiteral(
                             "%1%2 already exists in sound/direct_sound_samples.")
                             .arg(name, QLatin1String(ext));
            return false;
        }
    }
    return true;
}

// Mirrors load_wav_from_path (external/poryaaaa/plugin/voicegroup_loader.c)
// chunk-for-chunk, plus the wav2agb-only hard errors (multiple smpl loops,
// non-forward loop type) and an explicit mono check.
bool SampleRegistrar::inspectSampleWav(const QByteArray &bytes,
                                       SampleWavInfo *info, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    const auto u16 = [&bytes](qsizetype at) {
        return quint16(quint8(bytes[at])) | quint16(quint8(bytes[at + 1])) << 8;
    };
    const auto u32 = [&bytes](qsizetype at) {
        return quint32(quint8(bytes[at])) | quint32(quint8(bytes[at + 1])) << 8
            | quint32(quint8(bytes[at + 2])) << 16
            | quint32(quint8(bytes[at + 3])) << 24;
    };
    if (bytes.size() < 12 || !bytes.startsWith("RIFF")
        || bytes.mid(8, 4) != "WAVE")
        return fail(QStringLiteral("not a RIFF/WAVE file."));

    SampleWavInfo wav;
    const qsizetype fileEnd =
        qMin<qsizetype>(bytes.size(), 8 + qsizetype(u32(4)));
    bool fmtFound = false, dataFound = false;
    quint32 dataLen = 0, smplLoopType = 0, smplNumLoops = 0;
    qsizetype pos = 12;
    while (pos + 8 <= fileEnd) {
        const QByteArray id = bytes.mid(pos, 4);
        const quint32 chunkLen = u32(pos + 4);
        const qsizetype at = pos + 8;
        if (at + qsizetype(chunkLen) > fileEnd)
            break;
        if (id == "fmt " && chunkLen >= 16) {
            wav.formatTag = u16(at);
            wav.channels = u16(at + 2);
            wav.sampleRate = u32(at + 4);
            wav.bitsPerSample = u16(at + 14);
            fmtFound = true;
        } else if (id == "smpl" && chunkLen >= 32) {
            wav.hasSmpl = true;
            wav.midiKey = qMin<quint32>(u32(at + 12), 127);
            wav.pitchFraction = u32(at + 16);
            smplNumLoops = u32(at + 28);
            if (smplNumLoops == 1 && chunkLen >= 52) {
                smplLoopType = u32(at + 40);
                wav.loopStart = u32(at + 44);
                wav.loopEndIncl = u32(at + 48);
                wav.loopEnabled = true;
            }
        } else if (id == "agbp" && chunkLen >= 4) {
            wav.agbPitch = u32(at);
        } else if (id == "agbl" && chunkLen >= 4) {
            wav.agbLoopEnd = u32(at);
        } else if (id == "data") {
            dataLen = chunkLen;
            dataFound = true;
        }
        pos = at + chunkLen + (chunkLen & 1);
    }
    if (!fmtFound || !dataFound)
        return fail(QStringLiteral("missing fmt or data chunk."));
    if (wav.channels != 1)
        return fail(QStringLiteral(
                        "only mono samples are supported (this file has %1 "
                        "channels).")
                        .arg(wav.channels));
    quint32 bytesPerSample = 0;
    if (wav.formatTag == 1
        && (wav.bitsPerSample == 8 || wav.bitsPerSample == 16
            || wav.bitsPerSample == 24 || wav.bitsPerSample == 32))
        bytesPerSample = quint32(wav.bitsPerSample) / 8;
    else if (wav.formatTag == 3
             && (wav.bitsPerSample == 32 || wav.bitsPerSample == 64))
        bytesPerSample = quint32(wav.bitsPerSample) / 8;
    if (bytesPerSample == 0)
        return fail(QStringLiteral("unsupported sample format (tag %1, %2-bit).")
                        .arg(wav.formatTag)
                        .arg(wav.bitsPerSample));
    if (smplNumLoops > 1)
        return fail(QStringLiteral(
                        "the smpl chunk declares %1 loops; wav2agb supports at "
                        "most one.")
                        .arg(smplNumLoops));
    if (wav.loopEnabled && smplLoopType != 0)
        return fail(QStringLiteral(
                        "the smpl loop is not a forward loop (type %1); "
                        "wav2agb only supports forward loops.")
                        .arg(smplLoopType));
    wav.numSamples = dataLen / bytesPerSample;

    // The derived WaveData header, exactly as the loader computes it.
    quint32 loopEnd = wav.loopEnabled ? wav.loopEndIncl + 1 : wav.numSamples;
    loopEnd = qMin(loopEnd, wav.numSamples);
    if (wav.agbLoopEnd != 0)
        loopEnd = wav.agbLoopEnd;
    wav.waveSize = loopEnd;
    wav.waveLoopStart = wav.loopStart;
    wav.waveLooped = wav.loopEnabled;
    if (wav.agbPitch != 0) {
        wav.waveFreq = wav.agbPitch;
    } else if (wav.midiKey == 60 && wav.pitchFraction == 0) {
        wav.waveFreq = quint32(double(wav.sampleRate) * 1024.0);
    } else {
        const double tuning = double(wav.pitchFraction) / (4294967296.0 * 100.0);
        const double pitch = double(wav.sampleRate)
            * std::pow(2.0, (60.0 - double(wav.midiKey)) / 12.0 + tuning / 1200.0);
        wav.waveFreq = quint32(pitch * 1024.0);
    }
    if (info)
        *info = wav;
    return true;
}

bool SampleRegistrar::registerSample(const QString &projectRoot,
                                     const QString &name,
                                     const QByteArray &wavBytes, QString *error)
{
    const SampleFormatProbe probe = probeSampleFormat(projectRoot);
    if (!probe.ok()) {
        if (error)
            *error = probe.refusal;
        return false;
    }
    // Symbols are re-scanned here (not taken from the caller's catalog) so a
    // stale cache can never let a duplicate through to the .inc.
    if (!validateSampleName(projectRoot, name,
                            VoicegroupSource::directSoundSymbols(projectRoot),
                            error))
        return false;

    // The .wav first: if the .inc append then fails, an orphan sample file is
    // harmless, while a registered symbol with no file breaks the build.
    if (!QDir().mkpath(probe.samplesDir)) {
        if (error)
            *error = QStringLiteral("cannot create %1.").arg(probe.samplesDir);
        return false;
    }
    const QString wavPath =
        probe.samplesDir + QStringLiteral("/%1.wav").arg(name);
    QSaveFile wavOut(wavPath);
    if (!wavOut.open(QIODevice::WriteOnly)
        || wavOut.write(wavBytes) != wavBytes.size() || !wavOut.commit()) {
        if (error)
            *error = QStringLiteral("cannot write %1.").arg(wavPath);
        return false;
    }

    // Append the registration block, matching the file's line endings and
    // the existing entries' indentation (FORMATS.md §4 grammar).
    const QByteArray content = readAllBytes(probe.incPath);
    const QByteArray eol = content.contains("\r\n") ? "\r\n" : "\n";
    QByteArray alignIndent("\t"), incbinIndent("\t");
    for (const QByteArray &rawLine : content.split('\n')) {
        QByteArray line = rawLine;
        if (line.endsWith('\r'))
            line.chop(1);
        const QByteArray trimmed = line.trimmed();
        if (trimmed.isEmpty())
            continue;
        const QByteArray indent = line.left(line.indexOf(trimmed));
        if (trimmed.startsWith(".align"))
            alignIndent = indent;
        else if (trimmed.startsWith(".incbin"))
            incbinIndent = indent;
    }
    QByteArray block;
    if (!content.isEmpty() && !content.endsWith('\n'))
        block += eol;
    if (!content.isEmpty())
        block += eol; // blank line between entries, as in the shipped layout
    block += alignIndent + ".align 2" + eol;
    block += "DirectSoundWaveData_" + name.toUtf8() + "::" + eol;
    block += incbinIndent + ".incbin \"sound/direct_sound_samples/"
        + name.toUtf8() + ".bin\"" + eol;

    QSaveFile incOut(probe.incPath);
    if (!incOut.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("cannot write %1.").arg(probe.incPath);
        return false;
    }
    incOut.write(content);
    incOut.write(block);
    if (!incOut.commit()) {
        if (error)
            *error = QStringLiteral("cannot write %1.").arg(probe.incPath);
        return false;
    }
    return true;
}

bool SampleRegistrar::updateSample(const QString &projectRoot,
                                   const QString &name,
                                   const QByteArray &wavBytes, QString *error)
{
    const SampleFormatProbe probe = probeSampleFormat(projectRoot);
    if (!probe.ok()) {
        if (error)
            *error = probe.refusal;
        return false;
    }
    const QString symbol = QStringLiteral("DirectSoundWaveData_") + name;
    if (!VoicegroupSource::directSoundSymbols(projectRoot).contains(symbol)) {
        if (error)
            *error = QStringLiteral(
                         "%1 is not registered in this project; use Import "
                         "Sample to add new samples.")
                         .arg(symbol);
        return false;
    }
    const QString wavPath =
        probe.samplesDir + QStringLiteral("/%1.wav").arg(name);
    if (!QFile::exists(wavPath)) {
        if (error)
            *error = QStringLiteral(
                         "%1.wav does not exist in sound/direct_sound_samples "
                         "— only samples with a .wav source can be updated.")
                         .arg(name);
        return false;
    }
    QSaveFile wavOut(wavPath);
    if (!wavOut.open(QIODevice::WriteOnly)
        || wavOut.write(wavBytes) != wavBytes.size() || !wavOut.commit()) {
        if (error)
            *error = QStringLiteral("cannot write %1.").arg(wavPath);
        return false;
    }
    return true;
}

QString SampleRegistrar::sampleSidecarPath(const QString &projectRoot,
                                           const QString &name)
{
    return projectRoot + QStringLiteral("/.porydaw/samples/%1.json").arg(name);
}

QString SampleRegistrar::sourceHashHex(const QByteArray &sourceBytes)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(sourceBytes, QCryptographicHash::Sha256)
            .toHex());
}

bool SampleRegistrar::writeSampleSidecar(const QString &projectRoot,
                                         const QString &name,
                                         const SampleSidecar &sidecar,
                                         QString *error)
{
    const QString path = sampleSidecarPath(projectRoot, name);
    Sidecar::ensureDir(projectRoot, QStringLiteral("samples"));
    QJsonObject source;
    source.insert(QStringLiteral("path"), sidecar.sourcePath);
    source.insert(QStringLiteral("sha256"), sidecar.sourceSha256);
    source.insert(QStringLiteral("leftOnly"), sidecar.leftOnly);
    source.insert(QStringLiteral("sf2Zone"), sidecar.sf2Zone);
    const SampleEditParams &p = sidecar.params;
    QJsonObject params;
    params.insert(QStringLiteral("cropStart"), double(p.cropStart));
    params.insert(QStringLiteral("cropEnd"), double(p.cropEnd));
    params.insert(QStringLiteral("loopOn"), p.loopOn);
    params.insert(QStringLiteral("loopStart"), double(p.loopStart));
    params.insert(QStringLiteral("loopEnd"), double(p.loopEnd));
    params.insert(QStringLiteral("baseKey"), p.baseKey);
    params.insert(QStringLiteral("fineTuneCents"), p.fineTuneCents);
    params.insert(QStringLiteral("targetRate"), p.targetRate);
    params.insert(QStringLiteral("normalizeMode"), int(p.normalizeMode));
    params.insert(QStringLiteral("dcRemove"), int(p.dcRemove));
    params.insert(QStringLiteral("fadeIn"), p.fadeIn);
    params.insert(QStringLiteral("fadeOut"), p.fadeOut);
    params.insert(QStringLiteral("crossfadeOn"), p.crossfadeOn);
    params.insert(QStringLiteral("ditherOn"), p.ditherOn);
    params.insert(QStringLiteral("exactPitchOverride"),
                  double(p.exactPitchOverride));
    QJsonObject root;
    root.insert(QStringLiteral("version"), sidecar.version);
    root.insert(QStringLiteral("source"), source);
    root.insert(QStringLiteral("params"), params);

    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) || !out.write(QJsonDocument(root).toJson())
        || !out.commit()) {
        if (error)
            *error = QStringLiteral("cannot write %1.").arg(path);
        return false;
    }
    return true;
}

bool SampleRegistrar::readSampleSidecar(const QString &projectRoot,
                                        const QString &name,
                                        SampleSidecar *sidecar)
{
    QFile in(sampleSidecarPath(projectRoot, name));
    if (!in.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject root = QJsonDocument::fromJson(in.readAll()).object();
    if (root.value(QStringLiteral("version")).toInt() != 1)
        return false;
    const QJsonObject source = root.value(QStringLiteral("source")).toObject();
    const QJsonObject params = root.value(QStringLiteral("params")).toObject();
    if (source.isEmpty() || params.isEmpty())
        return false;
    SampleSidecar sc;
    sc.version = 1;
    sc.sourcePath = source.value(QStringLiteral("path")).toString();
    sc.sourceSha256 = source.value(QStringLiteral("sha256")).toString();
    sc.leftOnly = source.value(QStringLiteral("leftOnly")).toBool();
    sc.sf2Zone = source.value(QStringLiteral("sf2Zone")).toInt(-1);
    if (sc.sourcePath.isEmpty() || sc.sourceSha256.isEmpty())
        return false;
    SampleEditParams &p = sc.params;
    p.cropStart = qint64(params.value(QStringLiteral("cropStart")).toDouble());
    p.cropEnd = qint64(params.value(QStringLiteral("cropEnd")).toDouble());
    p.loopOn = params.value(QStringLiteral("loopOn")).toBool();
    p.loopStart = qint64(params.value(QStringLiteral("loopStart")).toDouble());
    p.loopEnd = qint64(params.value(QStringLiteral("loopEnd")).toDouble());
    p.baseKey = params.value(QStringLiteral("baseKey")).toInt(60);
    p.fineTuneCents = params.value(QStringLiteral("fineTuneCents")).toDouble();
    p.targetRate = params.value(QStringLiteral("targetRate")).toDouble();
    p.normalizeMode = SampleEditParams::NormalizeMode(
        qBound(0, params.value(QStringLiteral("normalizeMode")).toInt(),
               int(SampleEditParams::NormalizeOff)));
    p.dcRemove = SampleEditParams::Toggle(
        qBound(0, params.value(QStringLiteral("dcRemove")).toInt(),
               int(SampleEditParams::Off)));
    p.fadeIn = params.value(QStringLiteral("fadeIn")).toBool(true);
    p.fadeOut = params.value(QStringLiteral("fadeOut")).toBool(true);
    p.crossfadeOn = params.value(QStringLiteral("crossfadeOn")).toBool();
    p.ditherOn = params.value(QStringLiteral("ditherOn")).toBool();
    p.exactPitchOverride = quint32(
        params.value(QStringLiteral("exactPitchOverride")).toDouble());
    if (sidecar)
        *sidecar = sc;
    return true;
}

void SampleRegistrar::removeSampleSidecar(const QString &projectRoot,
                                          const QString &name)
{
    QFile::remove(sampleSidecarPath(projectRoot, name));
}
