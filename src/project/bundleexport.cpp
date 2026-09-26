#include "bundleexport.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>
#include <algorithm>

#include "core/smf.h"
#include "core/songdocument.h"
#include "project/bundlearchive.h"
#include "project/bundlesources.h"
#include "project/samplereg.h"
#include "project/songregistry.h"

// Symbol resolution here mirrors external/poryaaaa/plugin/voicegroup_loader.c
// — the bundle is loaded by that same loader, so what the exporter copies
// must be exactly what the loader would have read from the project:
//  - DirectSound symbols: "Label::" / "Label:" followed by an .incbin (or a
//    set_synth_* macro) in sound/direct_sound_data.inc and
//    sound/direct_sound_synth_data.inc, first definition wins; a ".bin"
//    path maps to the .wav, then the .aif, then the raw .bin
//    (load_wave_data_from_wav); any other path is read raw. A symbol none
//    of that resolves falls back to <symbol>.wav / .aif in a directory under
//    sound/ that holds samples (resolve_and_load_sample). `cry` voices read
//    the raw path only.
//  - Programmable waves: the same label/.incbin form in
//    sound/programmable_wave_data.inc.
//  - Keysplit tables: "keysplit NAME, n" (symbol keysplit_NAME) + "split"
//    lines, or ".set NAME, . - n" + ".byte" lines, in
//    sound/keysplit_tables.inc, then in the table files the loader's scan of
//    sound/ finds (see DeepScan).
//  - Sub-voicegroups: a group shorter than 128 voices keeps filling from
//    whatever is assembled after it (contiguousFill); see continuationLines.

namespace SongBundle {

using namespace Sources;

namespace {

const char kDummyVoice[] = "\tvoice_square_1 60, 0, 0, 2, 0, 0, 15, 0";
// Stands in for a line the project plays silent. Attack, decay and sustain
// all 0: the engine's CGB envelope ends such a note as it starts.
const char kSilentVoice[] = "\tvoice_square_1 60, 0, 0, 2, 0, 0, 0, 0";
const char kSamplePrefix[] = "DirectSoundWaveData_";
const char kWavePrefix[] = "ProgrammableWaveData_";

QByteArray readAllBytes(const QString &path, bool *ok = nullptr)
{
    QFile file(path);
    const bool opened = file.open(QIODevice::ReadOnly);
    if (ok)
        *ok = opened;
    return opened ? file.readAll() : QByteArray();
}

bool writeBytes(const QString &path, const QByteArray &bytes, QString *error)
{
    if (!QDir().mkpath(QFileInfo(path).path())) {
        if (error)
            *error = QStringLiteral("Cannot create %1").arg(QFileInfo(path).path());
        return false;
    }
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit()) {
        if (error)
            *error = QStringLiteral("Cannot write %1").arg(path);
        return false;
    }
    return true;
}

// What the loader's deferred scan of sound/ (discover_scan_tree, which runs
// when a sample or keysplit table misses the standard files) would add.
// Entries are name-sorted where the loader takes readdir order; the two only
// differ for a symbol defined in more than one nonstandard place.
struct DeepScan {
    QStringList sampleDirs; // directories holding a .wav / .aif
    QStringList tableFiles; // keysplit_tables.inc / .s, and keysplits/*.inc / *.s
};

void scanSoundTree(const QString &dirPath, int depth, DeepScan *out)
{
    constexpr int kMaxDepth = 3;
    const QDir dir(dirPath);
    const auto isSource = [](const QString &name) {
        return name.endsWith(QLatin1String(".inc"), Qt::CaseInsensitive) ||
               name.endsWith(QLatin1String(".s"), Qt::CaseInsensitive);
    };
    const QStringList files = dir.entryList(QDir::Files, QDir::Name);
    for (const QString &name : files) {
        if (name.endsWith(QLatin1String(".wav"), Qt::CaseInsensitive) ||
            name.endsWith(QLatin1String(".aif"), Qt::CaseInsensitive)) {
            out->sampleDirs.append(dirPath);
            break;
        }
    }
    for (const char *name : {"keysplit_tables.inc", "keysplit_tables.s"}) {
        if (files.contains(QLatin1String(name)))
            out->tableFiles.append(dir.filePath(QLatin1String(name)));
    }
    const QStringList subdirs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    if (subdirs.contains(QStringLiteral("keysplits"))) {
        const QDir ksDir(dir.filePath(QStringLiteral("keysplits")));
        for (const QString &name : ksDir.entryList(QDir::Files, QDir::Name)) {
            const QString path = ksDir.filePath(name);
            if (isSource(name) && !out->tableFiles.contains(path))
                out->tableFiles.append(path);
        }
    }
    if (depth >= kMaxDepth)
        return;
    for (const QString &name : subdirs) {
        if (!name.startsWith(QLatin1Char('.')))
            scanSoundTree(dir.filePath(name), depth + 1, out);
    }
}

// What the voice lines of the bundle reference.
struct References {
    QSet<QString> directSound; // voice_directsound* symbols (samples or synths)
    QSet<QString> cries;       // cry / cry_reverse symbols
    QSet<QString> waves;
    QSet<QString> tables;
    QStringList subGroups; // discovery order; each appears once
};

// recurse: whether keysplit / drumkit targets are followed (the loader does
// not load nested sub-voicegroups from a continuation region).
void collectLine(const VgSourceLine &line, bool recurse, References *refs)
{
    if (line.kind == VgLineKind::ReadOnlyVoice) {
        if (!line.crySymbol.isEmpty())
            refs->cries.insert(line.crySymbol);
        return;
    }
    if (line.kind != VgLineKind::Editable)
        return;
    const VgVoice &v = line.voice;
    switch (v.macro) {
    case VgMacro::DirectSound:
    case VgMacro::DirectSoundNoResample:
    case VgMacro::DirectSoundAlt:
    case VgMacro::DirectSoundReverse:
        refs->directSound.insert(v.symbol);
        break;
    case VgMacro::DirectSoundCompressed:
    case VgMacro::DirectSoundCompressedReverse:
        refs->cries.insert(v.symbol);
        break;
    case VgMacro::ProgWave:
    case VgMacro::ProgWaveAlt:
        refs->waves.insert(v.symbol);
        break;
    case VgMacro::Keysplit:
    case VgMacro::KeysplitAll:
        if (!recurse)
            break;
        if (v.macro == VgMacro::Keysplit)
            refs->tables.insert(v.keysplitTable);
        if (!refs->subGroups.contains(v.symbol))
            refs->subGroups.append(v.symbol);
        break;
    default:
        break;
    }
}

bool isVoiceLine(const VgSourceLine &line)
{
    return line.slot >= 0;
}

// The voice lines the loader's contiguousFill would append to a
// sub-voicegroup that ends at endSlot < 128: on the GBA indexing past a
// group's end reads the groups assembled after it, and old label-style
// drumsets rely on that. Within a monolithic file that is the rest of the
// file; for a per-file group it is the following files in
// sound/voice_groups.inc include order. A voice_group header ends the fill
// (its label is virtual), so macro-style projects never continue.
QList<VgSourceLine> continuationLines(const QString &root, const VoicegroupSource &sub, int endSlot)
{
    QList<VgSourceLine> out;
    if (endSlot <= 0 || endSlot >= VOICEGROUP_SIZE)
        return out;
    // Appends one parsed file's voices; false once a header stops the fill.
    const auto take = [&](const VgParsedSource &parsed, int fromLine) {
        for (int i = fromLine; i < parsed.lines.size(); i++) {
            const VgSourceLine &line = parsed.lines.at(i);
            if (line.kind == VgLineKind::Header)
                return false;
            if (isVoiceLine(line)) {
                out.append(line);
                if (endSlot + out.size() >= VOICEGROUP_SIZE)
                    return false;
            }
        }
        return true;
    };

    if (sub.monolithic()) {
        const QByteArray content = readAllBytes(sub.filePath());
        const VgParsedSource section = VoicegroupSource::parseSource(content, sub.sectionLabel());
        QByteArray rest;
        const QList<QByteArray> lines = content.split('\n');
        for (int i = section.sectionEnd; i < lines.size(); i++)
            rest += lines.at(i) + '\n';
        take(VoicegroupSource::parseSource(rest), 0);
        return out;
    }

    static const QRegularExpression includeRe(QStringLiteral(R"(^\.include\s+"([^"]+)\")"));
    const QString currentBase = QFileInfo(sub.filePath()).fileName();
    for (const char *index : {"/sound/voice_groups.inc", "/sound/voicegroups.inc"}) {
        bool ok = false;
        const QByteArray hub = readAllBytes(root + QLatin1String(index), &ok);
        if (!ok)
            continue;
        bool foundCurrent = false;
        for (const QByteArray &raw : hub.split('\n')) {
            const QRegularExpressionMatch m = includeRe.match(QString::fromUtf8(contentOf(raw)));
            if (!m.hasMatch())
                continue;
            if (!foundCurrent) {
                foundCurrent = QFileInfo(m.captured(1)).fileName() == currentBase;
                continue;
            }
            const QByteArray next = readAllBytes(root + QLatin1Char('/') + m.captured(1), &ok);
            if (!ok)
                return out; // included file missing: contiguity is unknowable
            const int before = out.size();
            const bool more = take(VoicegroupSource::parseSource(next), 0);
            if (!more || out.size() == before)
                return out;
        }
        if (foundCurrent)
            break;
    }
    return out;
}

QString joinNames(QStringList names)
{
    std::sort(names.begin(), names.end());
    return names.join(QStringLiteral(", "));
}

QStringList sortedList(const QSet<QString> &set)
{
    QStringList list(set.constBegin(), set.constEnd());
    std::sort(list.begin(), list.end());
    return list;
}

QByteArray synthDefinitionText(const QString &symbol, const VgSynthDesc &desc)
{
    QByteArray text = "\t.align 2\n" + symbol.toUtf8() + "::\n\t";
    switch (std::clamp(desc.waveform, 0, 2)) {
    case 0:
        text += QString::asprintf("set_synth_pulse 0x%02X, 0x%02X, 0x%02X, 0x%02X",
                                  uint8_t(desc.baseDuty), uint8_t(desc.dutyStep),
                                  uint8_t(desc.modDepth), uint8_t(desc.phase))
                    .toUtf8();
        break;
    case 1:
        text += "set_synth_saw";
        break;
    default:
        text += "set_synth_triangle";
        break;
    }
    return text + '\n';
}

} // namespace

QSet<int> usedPrograms(const SmfFile &smf)
{
    QSet<int> programs;
    for (const SmfTrack &track : smf.tracks) {
        bool programSeen = false;
        for (const SmfEvent &ev : track.events) {
            if (!ev.isChannel())
                continue;
            if (ev.typeNibble() == 0xC) {
                programs.insert(ev.data0 & 0x7F);
                programSeen = true;
            } else if (ev.isNoteOn() && !programSeen) {
                programs.insert(0);
                programSeen = true; // program 0 recorded; later changes still add
            }
        }
    }
    return programs;
}

QStringList usedExtensions(const SmfFile &smf)
{
    // The opt-in commands of the poryaaaa engine (kCcDefaults in
    // core/timelineplayer.cpp): everything else mid2agb maps is stock m4a.
    static constexpr struct {
        uint8_t cc;
        const char *name;
    } kExtensions[] = {{0x05, "PORTAMENTO"}, {0x17, "PWMC"}, {0x19, "PWMS"}};

    QSet<QString> found;
    for (const SmfTrack &track : smf.tracks) {
        for (const SmfEvent &ev : track.events) {
            if (!ev.isChannel() || ev.typeNibble() != 0xB)
                continue;
            for (const auto &ext : kExtensions) {
                if (ev.data0 == ext.cc)
                    found.insert(QLatin1String(ext.name));
            }
        }
    }
    return sortedList(found);
}

Exporter::Exporter(const QString &projectRoot, const SongDocument &doc,
                   const VoicegroupSource *vgSource)
    : m_root(projectRoot)
    , m_doc(doc)
    , m_vgSource(vgSource)
{}

void Exporter::setRegistrationHints(const QString &constant, const QString &player)
{
    m_constant = constant;
    m_player = player;
}

void Exporter::setPendingSynths(const QHash<QString, VgSynthDesc> &pending)
{
    m_pendingSynths = pending;
}

bool Exporter::stage(const QString &destDir, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!m_vgSource)
        return fail(QStringLiteral("This song's voicegroup source could not be opened, so its "
                                   "instruments can't be collected."));

    const SongCfg &cfg = m_doc.cfg();
    const QString vgSymbol =
        QStringLiteral("voicegroup") +
        (cfg.voicegroupArg.isEmpty() ? QStringLiteral("_dummy") : cfg.voicegroupArg);
    // The name the loader finds the top-level file by: the first
    // DecompProject::voicegroupCandidates entry.
    const auto fileNameFor = [](const QString &symbol) {
        return symbol.startsWith(QStringLiteral("voicegroup_")) ? symbol.mid(11) : symbol;
    };

    // ---- Top-level voicegroup: unused slots become the dummy square ----
    const QSet<int> used = usedPrograms(m_doc.smf());
    References refs;
    QByteArray topBytes;
    // Voice lines the bundle would carry that import refuses. collectLine
    // can't see their samples, even where the loader's laxer sscanf still
    // plays the line.
    QStringList unportable;
    // where: the slot as the song sees it, which for an overflow line is not
    // the line's own (that one counts from the group it was read out of).
    const auto collectVoice = [&](const QString &where, const VgSourceLine &line, bool recurse) {
        if (line.kind == VgLineKind::Broken)
            unportable.append(QStringLiteral("%1:\n    %2")
                                  .arg(where, QString::fromUtf8(contentOf(line.raw)).trimmed()));
        collectLine(line, recurse, &refs);
    };
    const auto voiceAt = [](const QString &symbol, int slot) {
        return QStringLiteral("%1, voice %2").arg(symbol).arg(slot);
    };
    for (const VgSourceLine &line :
         VoicegroupSource::parseSource(m_vgSource->renderPreview()).lines) {
        if (isVoiceLine(line) && !used.contains(line.slot)) {
            topBytes += kDummyVoice;
            if (line.raw.endsWith('\r'))
                topBytes += '\r';
        } else {
            topBytes += line.raw;
            if (isVoiceLine(line))
                collectVoice(voiceAt(vgSymbol, line.slot), line, /*recurse=*/true);
        }
        topBytes += '\n';
    }

    // ---- Sub-voicegroups, whole, plus their contiguity overflow ----
    QStringList unresolved;
    QMap<QString, QByteArray> groupFiles; // file base name -> bytes
    QHash<QString, QString> groupFileFolded;
    const auto addGroupFile = [&](const QString &symbol, const QByteArray &bytes) {
        const QString name = fileNameFor(symbol);
        const QString folded = name.toCaseFolded();
        if (groupFileFolded.contains(folded))
            return false;
        groupFileFolded.insert(folded, symbol);
        groupFiles.insert(name, bytes);
        return true;
    };
    addGroupFile(vgSymbol, topBytes);

    QStringList bundledSubGroups;
    for (int i = 0; i < refs.subGroups.size(); i++) { // grows while iterating
        const QString symbol = refs.subGroups.at(i);
        if (symbol == vgSymbol)
            continue; // self-reference: the (trimmed) top-level file serves it
        VoicegroupSource sub;
        QString subError;
        if (!symbol.startsWith(QStringLiteral("voicegroup")) ||
            !sub.open(m_root, symbol.mid(10), &subError)) {
            unresolved.append(QStringLiteral("%1 (sub-voicegroup)").arg(symbol));
            continue;
        }
        QByteArray bytes = sub.renderPreview();
        int endSlot = 0;
        for (const VgSourceLine &line : VoicegroupSource::parseSource(bytes).lines) {
            if (!isVoiceLine(line))
                continue;
            endSlot = std::max(endSlot, line.slot + 1);
            collectVoice(voiceAt(symbol, line.slot), line, /*recurse=*/true);
        }
        int overflowSlot = endSlot - 1;
        for (const VgSourceLine &line : continuationLines(m_root, sub, endSlot)) {
            overflowSlot++;
            // The loader never follows a keysplit / drumkit out of an
            // overflow region (the hardware doesn't substitute twice, and
            // include-order cycles would recurse forever). Appended to the
            // group's own file the line WOULD be followed. In the project a
            // key landing on it resolves to nothing (nested keysplit) and
            // makes no sound, so it becomes the silent voice instead.
            if (contentOf(line.raw).startsWith("voice_keysplit")) {
                bytes += kSilentVoice;
                bytes += line.raw.endsWith('\r') ? "\r\n" : "\n";
                continue;
            }
            bytes += line.raw + '\n';
            collectVoice(QStringLiteral("%1 (read past the group's end: voice %2 of a "
                                        "voicegroup after it)")
                             .arg(voiceAt(symbol, overflowSlot))
                             .arg(line.slot),
                         line, /*recurse=*/false);
        }
        if (!addGroupFile(symbol, bytes))
            return fail(
                QStringLiteral("Voicegroups %1 and %2 would share a file name in the "
                               "bundle.")
                    .arg(groupFileFolded.value(fileNameFor(symbol).toCaseFolded()), symbol));
        bundledSubGroups.append(symbol);
    }
    if (!unportable.isEmpty())
        return fail(QStringLiteral("Porydaw couldn't parse these voice lines, so the samples "
                                   "they use can't be collected. Check each for a missing "
                                   "comma or a stray argument:\n%1")
                        .arg(unportable.join(QLatin1Char('\n'))));

    // ---- DirectSound samples, synths and cries ----
    QHash<QString, QString> incbins;
    for (const char *file : {"/sound/direct_sound_data.inc", "/sound/direct_sound_synth_data.inc"})
        collectIncbins(readAllBytes(m_root + QLatin1String(file)), &incbins);
    const VgSynthCatalog synthCatalog = VoicegroupSource::synthInstruments(m_root);

    struct SampleFile {
        QString symbol;
        QString source;  // absolute project path
        QString archive; // path inside the bundle
    };
    QList<SampleFile> sampleFiles;
    QMap<QString, VgSynthDesc> synths;
    QMap<QString, QString> sampleIncbins; // symbol -> bundle .incbin path
    QStringList aifSamples;
    QHash<QString, QString> sampleNameFolded;

    // Run on the first miss only, like discovery_ensure_deep_scan.
    DeepScan deepScan;
    bool deepScanned = false;
    const auto ensureDeepScan = [&]() -> const DeepScan & {
        if (!deepScanned)
            scanSoundTree(m_root + QStringLiteral("/sound"), 0, &deepScan);
        deepScanned = true;
        return deepScan;
    };
    // <sample dir>/<symbol>.wav, then .aif; empty when no directory has one.
    const auto sampleDirFile = [&](const QString &symbol) {
        for (const QString &dir : ensureDeepScan().sampleDirs) {
            for (const char *ext : {".wav", ".aif"}) {
                const QString path = dir + QLatin1Char('/') + symbol + QLatin1String(ext);
                if (QFile::exists(path))
                    return path;
            }
        }
        return QString();
    };

    const auto sampleName = [](const QString &symbol) {
        return symbol.startsWith(QLatin1String(kSamplePrefix))
                   ? symbol.mid(int(qstrlen(kSamplePrefix)))
                   : symbol;
    };
    // Registers symbol's bundle name; false on a (case-insensitive) clash
    // with a different symbol.
    const auto claimName = [&](const QString &symbol, QString *name) {
        *name = sampleName(symbol);
        const QString folded = name->toCaseFolded();
        const QString owner = sampleNameFolded.value(folded, symbol);
        sampleNameFolded.insert(folded, owner);
        return owner == symbol;
    };
    // The bundle's .incbin always names <name>.bin, which the loader maps to
    // <name>.wav first: a .wav source keeps its extension, anything else
    // (whatever the project called it) is raw data and becomes the .bin.
    const auto addSampleFile = [&](const QString &symbol, const QString &name,
                                   const QString &source, bool isWav) {
        const QString archive =
            QStringLiteral("sound/direct_sound_samples/%1.%2")
                .arg(name, isWav ? QStringLiteral("wav") : QStringLiteral("bin"));
        for (const SampleFile &existing : sampleFiles) {
            if (existing.archive == archive)
                return;
        }
        sampleFiles.append({symbol, source, archive});
    };

    QStringList allSampleSymbols = sortedList(refs.directSound + refs.cries);
    for (const QString &symbol : allSampleSymbols) {
        if (refs.directSound.contains(symbol)) {
            const auto pending = m_pendingSynths.constFind(symbol);
            if (pending != m_pendingSynths.constEnd()) {
                synths.insert(symbol, pending.value());
                continue;
            }
            if (const VgSynthDesc *desc = synthCatalog.find(symbol)) {
                synths.insert(symbol, *desc);
                continue;
            }
        }
        const QString relPath = incbins.value(symbol);
        const QString rawPath = m_root + QLatin1Char('/') + relPath;
        // resolve_and_load_sample's order: the .wav / .aif beside the
        // .incbin's .bin, the .incbin file itself, then <sample dir>/<symbol>.
        QString source = relPath.isEmpty() ? QString() : rawPath;
        bool converted = false; // a .wav / .aif the loader decodes
        QString besideBin;      // the .bin's own source file
        if (relPath.endsWith(QStringLiteral(".bin"))) {
            const QString stem = rawPath.left(rawPath.size() - 4);
            for (const char *ext : {".wav", ".aif"}) {
                if (QFile::exists(stem + QLatin1String(ext))) {
                    source = besideBin = stem + QLatin1String(ext);
                    converted = true;
                    break;
                }
            }
        }
        if (source.isEmpty() || !QFile::exists(source)) {
            source = sampleDirFile(symbol);
            converted = true;
        }
        // resolve_and_load_compressed_sample: a cry voice plays the built
        // DPCM .bin while that is at least as new as its source, and the
        // source otherwise (an unbuilt project has no cry .bin at all).
        const bool cryPlaysBin =
            refs.cries.contains(symbol) && relPath.endsWith(QStringLiteral(".bin")) &&
            QFile::exists(rawPath) &&
            (besideBin.isEmpty() || QFileInfo(rawPath).lastModified().toSecsSinceEpoch() >=
                                        QFileInfo(besideBin).lastModified().toSecsSinceEpoch());
        const bool needsSource = refs.directSound.contains(symbol) || !cryPlaysBin;
        if (needsSource && source.isEmpty()) {
            unresolved.append(
                relPath.isEmpty()
                    ? QStringLiteral("%1 (sample)").arg(symbol)
                    : QStringLiteral("%1 (sample file %2 is missing)").arg(symbol, relPath));
            continue;
        }
        QString name;
        if (!claimName(symbol, &name))
            return fail(QStringLiteral("Samples %1 and %2 would share a file name in the bundle.")
                            .arg(sampleNameFolded.value(name.toCaseFolded()), symbol));
        if (needsSource) {
            if (converted && source.endsWith(QStringLiteral(".aif")))
                aifSamples.append(symbol);
            else
                addSampleFile(symbol, name, source, converted);
        }
        if (cryPlaysBin)
            addSampleFile(symbol, name, rawPath, /*isWav=*/false);
        sampleIncbins.insert(symbol, QStringLiteral("sound/direct_sound_samples/%1.bin").arg(name));
    }

    // ---- Programmable waves ----
    QHash<QString, QString> waveIncbins;
    collectIncbins(readAllBytes(m_root + QStringLiteral("/sound/programmable_wave_data.inc")),
                   &waveIncbins);
    QMap<QString, QString> waveSources; // symbol -> absolute project path
    QHash<QString, QString> waveNameFolded;
    const auto waveName = [](const QString &symbol) {
        return symbol.startsWith(QLatin1String(kWavePrefix)) ? symbol.mid(int(qstrlen(kWavePrefix)))
                                                             : symbol;
    };
    for (const QString &symbol : sortedList(refs.waves)) {
        const QString relPath = waveIncbins.value(symbol);
        const QString source = m_root + QLatin1Char('/') + relPath;
        if (relPath.isEmpty() || !QFile::exists(source)) {
            unresolved.append(QStringLiteral("%1 (programmable wave)").arg(symbol));
            continue;
        }
        const QString folded = waveName(symbol).toCaseFolded();
        if (waveNameFolded.contains(folded))
            return fail(QStringLiteral("Waves %1 and %2 would share a file name in the bundle.")
                            .arg(waveNameFolded.value(folded), symbol));
        waveNameFolded.insert(folded, symbol);
        waveSources.insert(symbol, source);
    }

    // ---- Keysplit tables ----
    QHash<QString, QList<QByteArray>> projectTables =
        collectKeysplitTables(readAllBytes(m_root + QStringLiteral("/sound/keysplit_tables.inc")));
    QMap<QString, QList<QByteArray>> tables;
    bool tablesRescanned = false;
    for (const QString &symbol : sortedList(refs.tables)) {
        auto it = projectTables.constFind(symbol);
        if (it == projectTables.constEnd() && !tablesRescanned) {
            // keysplit_map_find_or_rescan: tables kept next to the
            // voicegroups (nonstandard layouts); earlier files still win.
            tablesRescanned = true;
            const QString standard = m_root + QStringLiteral("/sound/keysplit_tables.inc");
            for (const QString &path : ensureDeepScan().tableFiles) {
                if (path == standard)
                    continue;
                const auto found = collectKeysplitTables(readAllBytes(path));
                for (auto t = found.constBegin(); t != found.constEnd(); ++t) {
                    if (!projectTables.contains(t.key()))
                        projectTables.insert(t.key(), t.value());
                }
            }
            it = projectTables.constFind(symbol);
        }
        if (it == projectTables.constEnd())
            unresolved.append(QStringLiteral("%1 (keysplit table)").arg(symbol));
        else
            tables.insert(symbol, it.value());
    }

    // ---- Refusals ----
    if (!aifSamples.isEmpty())
        return fail(QStringLiteral("These samples only exist as .aif files, which a bundle can't "
                                   "carry: %1. Convert them to .wav in the Sample Studio first.")
                        .arg(joinNames(aifSamples)));
    if (!unresolved.isEmpty())
        return fail(QStringLiteral("The song's voicegroup references things this project "
                                   "doesn't define, so the bundle would be incomplete: %1. (An "
                                   "unsaved Golden Sun synth voice resolves once the song is "
                                   "saved.)")
                        .arg(joinNames(unresolved)));

    // ---- Write the tree ----
    const QString base = QDir(destDir).absolutePath() + QLatin1Char('/');
    if (!QDir().mkpath(base))
        return fail(QStringLiteral("Cannot create %1").arg(destDir));

    const QString midiDir = base + QStringLiteral("sound/songs/midi/");
    const QString flags = SongRegistry::mergeCfgFlags(cfg).join(QLatin1Char(' '));
    if (!writeBytes(midiDir + m_doc.label() + QStringLiteral(".mid"), m_doc.smf().write(), error) ||
        !writeBytes(midiDir + QStringLiteral("midi.cfg"),
                    QStringLiteral("%1.mid: %2\n").arg(m_doc.label(), flags).toUtf8(), error))
        return false;

    for (auto it = groupFiles.constBegin(); it != groupFiles.constEnd(); ++it) {
        if (!writeBytes(base + QStringLiteral("sound/voicegroups/%1.inc").arg(it.key()), it.value(),
                        error))
            return false;
    }

    BundleManifest manifest;
    manifest.porydaw = QStringLiteral(PORYDAW_VERSION);
    manifest.label = m_doc.label();
    manifest.voicegroup = vgSymbol;
    manifest.flags = flags;
    manifest.constant = m_constant;
    manifest.player = m_player;
    manifest.layout =
        m_vgSource->monolithic() ? QStringLiteral("pokefirered") : QStringLiteral("pokeemerald");
    manifest.extensions = usedExtensions(m_doc.smf());
    manifest.synth = !synths.isEmpty();

    std::sort(sampleFiles.begin(), sampleFiles.end(),
              [](const SampleFile &a, const SampleFile &b) { return a.archive < b.archive; });
    for (const SampleFile &sample : sampleFiles) {
        bool ok = false;
        const QByteArray bytes = readAllBytes(sample.source, &ok);
        if (!ok)
            return fail(QStringLiteral("Cannot read %1").arg(sample.source));
        if (!writeBytes(base + sample.archive, bytes, error))
            return false;
        manifest.samples.append(
            {sampleName(sample.symbol), sample.archive, SampleRegistrar::sourceHashHex(bytes)});
    }
    if (!sampleIncbins.isEmpty()) {
        QByteArray inc;
        for (auto it = sampleIncbins.constBegin(); it != sampleIncbins.constEnd(); ++it) {
            if (!inc.isEmpty())
                inc += '\n';
            inc += "\t.align 2\n" + it.key().toUtf8() + "::\n\t.incbin \"" + it.value().toUtf8() +
                   "\"\n";
        }
        if (!writeBytes(base + QStringLiteral("sound/direct_sound_data.inc"), inc, error))
            return false;
    }

    if (!synths.isEmpty()) {
        QByteArray inc;
        for (auto it = synths.constBegin(); it != synths.constEnd(); ++it) {
            if (!inc.isEmpty())
                inc += '\n';
            inc += synthDefinitionText(it.key(), it.value());
        }
        if (!writeBytes(base + QStringLiteral("sound/direct_sound_synth_data.inc"), inc, error))
            return false;
        manifest.synths = synths.keys();
    }

    if (!waveSources.isEmpty()) {
        QByteArray inc;
        for (auto it = waveSources.constBegin(); it != waveSources.constEnd(); ++it) {
            const QString archive =
                QStringLiteral("sound/programmable_wave_samples/%1.pcm").arg(waveName(it.key()));
            bool ok = false;
            const QByteArray bytes = readAllBytes(it.value(), &ok);
            if (!ok)
                return fail(QStringLiteral("Cannot read %1").arg(it.value()));
            if (!writeBytes(base + archive, bytes, error))
                return false;
            if (!inc.isEmpty())
                inc += '\n';
            inc +=
                "\t.align 2\n" + it.key().toUtf8() + "::\n\t.incbin \"" + archive.toUtf8() + "\"\n";
        }
        if (!writeBytes(base + QStringLiteral("sound/programmable_wave_data.inc"), inc, error))
            return false;
        manifest.waves = waveSources.keys();
    }

    if (!tables.isEmpty()) {
        QByteArray inc;
        for (auto it = tables.constBegin(); it != tables.constEnd(); ++it) {
            if (!inc.isEmpty())
                inc += '\n';
            for (const QByteArray &line : it.value())
                inc += line + '\n';
        }
        if (!writeBytes(base + QStringLiteral("sound/keysplit_tables.inc"), inc, error))
            return false;
        manifest.keysplitTables = tables.keys();
    }

    manifest.subVoicegroups = bundledSubGroups;
    std::sort(manifest.subVoicegroups.begin(), manifest.subVoicegroups.end());
    if (!manifest.write(destDir, error))
        return false;
    m_manifest = manifest;
    return true;
}

bool Exporter::exportTo(const QString &zipPath, QString *error)
{
    QTemporaryDir staging;
    if (!staging.isValid()) {
        if (error)
            *error = QStringLiteral("Cannot create a temporary directory.");
        return false;
    }
    const QString stageDir = staging.filePath(QStringLiteral("bundle"));
    if (!stage(stageDir, error))
        return false;
    // Only once the song staged: a refused export leaves no folder.
    QDir().mkpath(QFileInfo(zipPath).absolutePath());
    return BundleArchive::createBundle(stageDir, zipPath, error);
}

} // namespace SongBundle
