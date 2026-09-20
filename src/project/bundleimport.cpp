#include "bundleimport.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <algorithm>

#include "core/smf.h"
#include "project/bundleexport.h"
#include "project/bundlesources.h"
#include "project/decompproject.h"
#include "project/samplereg.h"
#include "project/songregistry.h"

namespace SongBundle {

using namespace Sources;

namespace {

const char kSamplePrefix[] = "DirectSoundWaveData_";
const char kWavePrefix[] = "ProgrammableWaveData_";
const char kSamplesDir[] = "/sound/direct_sound_samples/";
const char kWavesDir[] = "/sound/programmable_wave_samples/";

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
    QSaveFile out(path);
    if (!QDir().mkpath(QFileInfo(path).path()) || !out.open(QIODevice::WriteOnly) ||
        out.write(bytes) != bytes.size() || !out.commit()) {
        if (error)
            *error = QStringLiteral("cannot write %1.").arg(path);
        return false;
    }
    return true;
}

// ---- content hashes, cached for the session ---------------------------------

struct CachedHash {
    qint64 mtime = 0;
    qint64 size = 0;
    QString hash;
};
QMutex g_hashMutex;
QHash<QString, CachedHash> g_hashCache;

QString fileHash(const QString &path)
{
    const QFileInfo info(path);
    const qint64 mtime = info.lastModified().toMSecsSinceEpoch();
    {
        QMutexLocker lock(&g_hashMutex);
        const auto it = g_hashCache.constFind(path);
        if (it != g_hashCache.constEnd() && it->mtime == mtime && it->size == info.size())
            return it->hash;
    }
    bool ok = false;
    const QByteArray bytes = readAllBytes(path, &ok);
    if (!ok)
        return QString();
    const QString hash = SampleRegistrar::sourceHashHex(bytes);
    QMutexLocker lock(&g_hashMutex);
    g_hashCache.insert(path, {mtime, info.size(), hash});
    return hash;
}

// ---- voicegroup source helpers ------------------------------------------------

// A voice line's audible content: comment dropped, whitespace normalized.
QByteArray normalizedVoice(const QByteArray &raw)
{
    const QByteArray text = contentOf(raw).simplified();
    const int space = text.indexOf(' ');
    if (space < 0)
        return text;
    QByteArray out = text.left(space) + ' ';
    const QList<QByteArray> args = text.mid(space + 1).split(',');
    for (int i = 0; i < args.size(); i++)
        out += (i ? "," : "") + args.at(i).trimmed();
    return out;
}

// Replaces whole-word symbols in the line's content (never in its comment).
QByteArray renameSymbols(const QByteArray &raw, const QHash<QString, QString> &renames)
{
    int end = raw.size();
    for (const char *marker : {"@", "//"}) {
        const int at = raw.indexOf(marker);
        if (at >= 0)
            end = std::min(end, at);
    }
    const QString content = QString::fromUtf8(raw.left(end));
    static const QRegularExpression wordRe(QStringLiteral(R"(\w+)"));
    QString out;
    int last = 0;
    QRegularExpressionMatchIterator it = wordRe.globalMatch(content);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const auto found = renames.constFind(m.captured(0));
        if (found == renames.constEnd())
            continue;
        out += content.mid(last, m.capturedStart(0) - last) + found.value();
        last = m.capturedEnd(0);
    }
    if (last == 0)
        return raw;
    return (out + content.mid(last)).toUtf8() + raw.mid(end);
}

// One voicegroup file, reduced to what import compares and copies.
struct GroupSource {
    QString symbol;
    QList<QByteArray> body;     // lines after the declaration, '\r' dropped
    QList<VgSourceLine> voices; // the voice lines, in slot order
    int startingNote = 0;
    // A voice line is VgLineKind::Broken.
    bool suspect = false;
};

QString declaredSymbol(const VgSourceLine &line)
{
    static const QRegularExpression macroRe(QStringLiteral(R"(^voice_group\s+(\w+))"));
    static const QRegularExpression labelRe(QStringLiteral(R"(^(voicegroup\w*)::?)"));
    const QString text = QString::fromUtf8(contentOf(line.raw));
    QRegularExpressionMatch m = macroRe.match(text);
    if (m.hasMatch())
        return QStringLiteral("voicegroup_") + m.captured(1);
    m = labelRe.match(text);
    return m.hasMatch() ? m.captured(1) : QString();
}

// Splits standalone voicegroup bytes (one declaration) into a GroupSource;
// false when the bytes declare no voicegroup or more than one.
bool readGroup(const QByteArray &bytes, GroupSource *out)
{
    const VgParsedSource parsed = VoicegroupSource::parseSource(bytes);
    GroupSource group;
    bool declared = false;
    for (const VgSourceLine &line : parsed.lines) {
        const QString symbol = declaredSymbol(line);
        if (!symbol.isEmpty()) {
            if (declared)
                return false;
            declared = true;
            group.symbol = symbol;
            continue;
        }
        if (!declared)
            continue;
        QByteArray raw = line.raw;
        if (raw.endsWith('\r'))
            raw.chop(1);
        // Only voices, comments and blank lines are carried into a project:
        // anything else after the declaration (an .align before the next
        // group, or a directive a hostile bundle slipped in) is dropped.
        const QByteArray content = contentOf(raw);
        if (line.slot < 0 && !content.isEmpty())
            continue;
        // A voice line the parser can't read is one the loader leaves
        // silent, and its symbols can't be resolved or renamed. That also
        // covers a quote or statement separator, which would smuggle a second
        // directive into the project's sources.
        if (line.kind == VgLineKind::Broken)
            group.suspect = true;
        group.body.append(raw);
        if (line.slot >= 0) {
            if (group.voices.isEmpty())
                group.startingNote = line.slot;
            group.voices.append(line);
        }
    }
    while (!group.body.isEmpty() && group.body.last().trimmed().isEmpty())
        group.body.removeLast();
    if (!declared || group.voices.isEmpty())
        return false;
    *out = group;
    return true;
}

QList<QByteArray> normalizedVoices(const GroupSource &group, const QHash<QString, QString> &renames)
{
    QList<QByteArray> out;
    for (const VgSourceLine &line : group.voices)
        out.append(QByteArray::number(line.slot) + ':' +
                   normalizedVoice(renameSymbols(line.raw, renames)));
    return out;
}

// What a group's voice lines reference.
struct References {
    QSet<QString> directSound;
    QSet<QString> cries;
    QSet<QString> waves;
    QSet<QString> tables;
    QStringList subGroups;
};

void collectReferences(const GroupSource &group, References *refs)
{
    for (const VgSourceLine &line : group.voices) {
        if (line.kind == VgLineKind::ReadOnlyVoice && !line.crySymbol.isEmpty())
            refs->cries.insert(line.crySymbol);
        if (line.kind != VgLineKind::Editable)
            continue;
        const VgVoice &v = line.voice;
        switch (v.macro) {
        case VgMacro::DirectSound:
        case VgMacro::DirectSoundNoResample:
        case VgMacro::DirectSoundAlt:
            refs->directSound.insert(v.symbol);
            break;
        case VgMacro::ProgWave:
        case VgMacro::ProgWaveAlt:
            refs->waves.insert(v.symbol);
            break;
        case VgMacro::Keysplit:
            refs->tables.insert(v.keysplitTable);
            [[fallthrough]];
        case VgMacro::KeysplitAll:
            if (!refs->subGroups.contains(v.symbol))
                refs->subGroups.append(v.symbol);
            break;
        default:
            break;
        }
    }
}

QStringList sortedList(const QSet<QString> &set)
{
    QStringList list(set.constBegin(), set.constEnd());
    std::sort(list.begin(), list.end());
    return list;
}

// base, base_2, base_3 … — the first one taken() lets through.
template <typename Taken>
QString firstFree(const QString &base, Taken taken)
{
    if (!taken(base))
        return base;
    for (int n = 2;; n++) {
        const QString candidate = QStringLiteral("%1_%2").arg(base).arg(n);
        if (!taken(candidate))
            return candidate;
    }
}

QString identifierFrom(const QString &raw, const QString &fallback)
{
    QString out;
    for (const QChar c : raw) {
        if (c.isLetterOrNumber() && c.unicode() < 128)
            out += c;
        else if (!out.isEmpty() && !out.endsWith(QLatin1Char('_')))
            out += QLatin1Char('_');
    }
    while (out.endsWith(QLatin1Char('_')))
        out.chop(1);
    return out.isEmpty() ? fallback : out;
}

ImportAction actionFor(const ImportItem &item)
{
    return item.projectSymbol == item.bundleSymbol ? ImportAction::Add : ImportAction::Rename;
}

// ---- byte-conservative appends ------------------------------------------------

// Appends lines as one block after a blank line, in the file's own line
// endings. A missing file is created holding just the block; one that exists
// but can't be read fails rather than being replaced by the block.
bool appendBlock(const QString &path, const QList<QByteArray> &lines, QString *error)
{
    bool readOk = false;
    const QByteArray content = readAllBytes(path, &readOk);
    if (!readOk && QFileInfo::exists(path)) {
        if (error)
            *error = QStringLiteral("cannot read %1.").arg(path);
        return false;
    }
    const QByteArray eol = content.contains("\r\n") ? "\r\n" : "\n";
    QByteArray block;
    if (!content.isEmpty() && !content.endsWith('\n'))
        block += eol;
    if (!content.isEmpty())
        block += eol;
    for (const QByteArray &line : lines)
        block += line + eol;
    return writeBytes(path, content + block, error);
}

// The "\t.align 2 / Symbol:: / \t.incbin" entry in the file's observed
// indentation (the registrar's FORMATS.md §4 grammar).
QList<QByteArray> incbinEntry(const QString &incPath, const QString &symbol,
                              const QString &incbinPath)
{
    QByteArray alignIndent("\t"), incbinIndent("\t");
    for (QByteArray line : readAllBytes(incPath).split('\n')) {
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
    return {alignIndent + ".align 2",
            symbol.toUtf8() + "::", incbinIndent + ".incbin \"" + incbinPath.toUtf8() + "\""};
}

// ---- the project side ------------------------------------------------------------

struct ProjectSamples {
    QHash<QString, QString> incbins;   // symbol -> .incbin path
    QHash<qint64, QStringList> bySize; // file size -> symbols with a file of that size
    QHash<QString, QString> wavOf;     // symbol -> absolute .wav (decoded by the loader)
    QHash<QString, QString> rawOf;     // symbol -> absolute raw file (the .incbin itself)
};

ProjectSamples scanProjectSamples(const QString &root)
{
    ProjectSamples out;
    for (const char *file : {"/sound/direct_sound_data.inc", "/sound/direct_sound_synth_data.inc"})
        collectIncbins(readAllBytes(root + QLatin1String(file)), &out.incbins);
    for (auto it = out.incbins.constBegin(); it != out.incbins.constEnd(); ++it) {
        const QString raw = root + QLatin1Char('/') + it.value();
        if (it.value().endsWith(QStringLiteral(".bin"))) {
            const QString wav = raw.left(raw.size() - 4) + QStringLiteral(".wav");
            if (QFile::exists(wav)) {
                out.wavOf.insert(it.key(), wav);
                out.bySize[QFileInfo(wav).size()].append(it.key());
            }
        }
        if (QFile::exists(raw)) {
            out.rawOf.insert(it.key(), raw);
            out.bySize[QFileInfo(raw).size()].append(it.key());
        }
    }
    return out;
}

// Every single-declaration voicegroup file under sound/voicegroups/.
QList<GroupSource> scanProjectGroups(const QString &root)
{
    QStringList paths;
    QDirIterator it(root + QStringLiteral("/sound/voicegroups"), {QStringLiteral("*.inc")},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        paths.append(it.next());
    paths.sort();
    QList<GroupSource> out;
    for (const QString &path : paths) {
        GroupSource group;
        if (readGroup(readAllBytes(path), &group))
            out.append(group);
    }
    return out;
}

bool projectDefinesExtension(const QString &root, const QString &mnemonic)
{
    const QRegularExpression equRe(
        QStringLiteral(R"(^\s*\.equ\s+%1\s*,)").arg(QRegularExpression::escape(mnemonic)),
        QRegularExpression::MultilineOption);
    return equRe.match(QString::fromUtf8(readAllBytes(root + QStringLiteral("/sound/MPlayDef.s"))))
        .hasMatch();
}

// "PORTAMENTO (tracks 1, 3)" for each extension the MIDI emits.
QString extensionUse(const SmfFile &smf, const QString &mnemonic)
{
    static const QHash<QString, int> kCc = {{QStringLiteral("PORTAMENTO"), 0x05},
                                            {QStringLiteral("PWMC"), 0x17},
                                            {QStringLiteral("PWMS"), 0x19}};
    // Numbered like the track headers: only chunks with channel events take
    // a slot (SongDocument's track map), counted from 1; chunks past the
    // engine's 16 tracks have no header.
    const int cc = kCc.value(mnemonic, -1);
    QStringList tracks;
    int slot = 0;
    for (size_t t = 0; t < smf.tracks.size(); t++) {
        bool hasChannel = false;
        bool uses = false;
        for (const SmfEvent &ev : smf.tracks[t].events) {
            if (!ev.isChannel())
                continue;
            hasChannel = true;
            if (ev.typeNibble() == 0xB && ev.data0 == cc) {
                uses = true;
                break;
            }
        }
        if (!hasChannel)
            continue;
        if (++slot > 16)
            break;
        if (uses)
            tracks.append(QString::number(slot));
    }
    if (tracks.isEmpty())
        return mnemonic;
    return QStringLiteral("%1 (track%2 %3)")
        .arg(mnemonic, tracks.size() == 1 ? QString() : QStringLiteral("s"),
             tracks.join(QStringLiteral(", ")));
}

bool keysplitMacroDefined(const QString &root)
{
    QDirIterator it(root + QStringLiteral("/asm/macros"), {QStringLiteral("*.inc")}, QDir::Files);
    static const QRegularExpression macroRe(QStringLiteral(R"(\.macro\s+keysplit\b)"));
    while (it.hasNext()) {
        if (macroRe.match(QString::fromUtf8(readAllBytes(it.next()))).hasMatch())
            return true;
    }
    return false;
}

} // namespace

int ImportPlan::count(const QList<ImportItem> &items, ImportAction action)
{
    return int(std::count_if(items.begin(), items.end(),
                             [action](const ImportItem &item) { return item.action == action; }));
}

void clearImportHashCache()
{
    QMutexLocker lock(&g_hashMutex);
    g_hashCache.clear();
}

ImportPlan makeImportPlan(const QString &bundleRoot, const QString &projectRoot,
                          const ImportOptions &options)
{
    ImportPlan plan;
    plan.bundleRoot = QDir(bundleRoot).absolutePath();
    plan.projectRoot = QDir(projectRoot).absolutePath();
    const QString &B = plan.bundleRoot;
    const QString &P = plan.projectRoot;
    const auto refuse = [&plan](const QString &why) { plan.refusals.append(why); };

    // ---- The bundle, validated (untrusted input) --------------------------------
    SongInfo song;
    QString error;
    if (!readSong(B, &plan.manifest, &song, &error)) {
        refuse(error);
        return plan;
    }
    plan.midSource = song.midPath;
    SmfFile smf;
    if (!SmfFile::read(readAllBytes(song.midPath), &smf, &error)) {
        refuse(QStringLiteral("The bundle's MIDI file can't be read: %1").arg(error));
        return plan;
    }

    DecompProject project;
    if (!project.open(P, &error)) {
        refuse(error);
        return plan;
    }
    if (!QDir(P + QStringLiteral("/sound/voicegroups")).exists()) {
        refuse(QStringLiteral("This project keeps every voicegroup in one file "
                              "(sound/voicegroups/ does not exist); porydaw can only create "
                              "voicegroups in the per-file layout."));
        return plan;
    }

    // ---- The bundle's voicegroups, walked like the exporter walked the project ---
    const QString topSymbol =
        QStringLiteral("voicegroup") +
        (song.cfg.voicegroupArg.isEmpty() ? QStringLiteral("_dummy") : song.cfg.voicegroupArg);
    const auto openGroup = [&](const QString &symbol, GroupSource *group) {
        VoicegroupSource source;
        QString openError;
        return symbol.startsWith(QStringLiteral("voicegroup")) &&
               source.open(B, symbol.mid(10), &openError) && !source.monolithic() &&
               readGroup(source.renderPreview(), group);
    };
    GroupSource top;
    if (!openGroup(topSymbol, &top)) {
        refuse(QStringLiteral("The bundle is incomplete: it has no voicegroup file for %1.")
                   .arg(topSymbol));
        return plan;
    }
    // The flags end up on a make command line in the project.
    static const QRegularExpression flagRe(QStringLiteral("^-[A-Za-z][A-Za-z0-9_]*$"));
    for (const QString &flag : song.cfg.rawFlags) {
        if (!flagRe.match(flag).hasMatch()) {
            refuse(QStringLiteral("The bundle's mid2agb flags contain '%1', which is not a "
                                  "plain option.")
                       .arg(flag));
            return plan;
        }
    }

    References refs;
    collectReferences(top, &refs);
    QList<GroupSource> subs;
    QStringList incomplete;
    QStringList suspect;
    if (top.suspect)
        suspect.append(topSymbol);
    for (int i = 0; i < refs.subGroups.size(); i++) { // grows while iterating
        const QString symbol = refs.subGroups.at(i);
        if (symbol == topSymbol)
            continue;
        GroupSource sub;
        if (!openGroup(symbol, &sub)) {
            incomplete.append(symbol);
            continue;
        }
        sub.symbol = symbol;
        if (sub.suspect)
            suspect.append(symbol);
        collectReferences(sub, &refs);
        subs.append(sub);
    }
    if (!suspect.isEmpty()) {
        refuse(QStringLiteral("The bundle's voicegroup %1 has voice lines that aren't plain "
                              "voice macros; they won't be copied into a project.")
                   .arg(suspect.join(QStringLiteral(", "))));
        return plan;
    }

    QHash<QString, QString> renames; // bundle symbol -> project symbol, where they differ

    // ---- 1. Samples (and 3. synth definitions, which share the symbol space) ----
    QHash<QString, QString> bundleIncbins;
    for (const char *file : {"/sound/direct_sound_data.inc", "/sound/direct_sound_synth_data.inc"})
        collectIncbins(readAllBytes(B + QLatin1String(file)), &bundleIncbins);
    const VgSynthCatalog bundleSynths = VoicegroupSource::synthInstruments(B);
    const VgSynthCatalog projectSynths = VoicegroupSource::synthInstruments(P);
    const ProjectSamples projectSamples = scanProjectSamples(P);
    QHash<QString, QString> manifestHashes; // archive path -> sha256
    for (const BundleSample &sample : plan.manifest.samples)
        manifestHashes.insert(sample.file, sample.sha256);

    // Synths and samples share the DirectSoundWaveData_ namespace, so each
    // side's picks are taken for the other too.
    QSet<QString> takenSampleNames;
    QSet<QString> takenSynthSymbols;
    const auto sampleNameTaken = [&](const QString &name) {
        if (takenSampleNames.contains(name))
            return true;
        const QString symbol = QLatin1String(kSamplePrefix) + name;
        if (takenSynthSymbols.contains(symbol) || projectSamples.incbins.contains(symbol) ||
            projectSynths.find(symbol))
            return true;
        for (const char *ext : {".wav", ".bin", ".aif"}) {
            if (QFile::exists(P + QLatin1String(kSamplesDir) + name + QLatin1String(ext)))
                return true;
        }
        return false;
    };

    for (const QString &symbol : sortedList(refs.directSound + refs.cries)) {
        ImportItem item;
        item.bundleSymbol = symbol;
        if (const VgSynthDesc *desc =
                refs.directSound.contains(symbol) ? bundleSynths.find(symbol) : nullptr) {
            item.synth = *desc;
            const QString existing = projectSynths.symbolFor(*desc);
            if (!existing.isEmpty()) {
                item.projectSymbol = existing;
                item.action = ImportAction::Reuse;
            } else {
                item.projectSymbol = firstFree(symbol, [&](const QString &candidate) {
                    return takenSynthSymbols.contains(candidate) ||
                           (candidate.startsWith(QLatin1String(kSamplePrefix)) &&
                            takenSampleNames.contains(
                                candidate.mid(int(qstrlen(kSamplePrefix))))) ||
                           projectSynths.find(candidate) ||
                           projectSamples.incbins.contains(candidate);
                });
                takenSynthSymbols.insert(item.projectSymbol);
                item.action = actionFor(item);
            }
            if (item.projectSymbol != symbol)
                renames.insert(symbol, item.projectSymbol);
            plan.synths.append(item);
            continue;
        }

        const QString rel = bundleIncbins.value(symbol);
        if (!rel.endsWith(QStringLiteral(".bin"))) {
            incomplete.append(symbol);
            continue;
        }
        const QString stem = rel.left(rel.size() - 4);
        if (QFile::exists(B + QLatin1Char('/') + stem + QStringLiteral(".wav")))
            item.wavSource = B + QLatin1Char('/') + stem + QStringLiteral(".wav");
        if (QFile::exists(B + QLatin1Char('/') + rel))
            item.binSource = B + QLatin1Char('/') + rel;
        if ((item.wavSource.isEmpty() && item.binSource.isEmpty()) ||
            (refs.cries.contains(symbol) && item.binSource.isEmpty())) {
            incomplete.append(symbol);
            continue;
        }
        // The manifest's hashes are a consistency check on the stored bytes.
        bool corrupt = false;
        const auto checkedHash = [&](const QString &source) {
            if (source.isEmpty())
                return QString();
            const QString hash = fileHash(source);
            const QString claimed = manifestHashes.value(QDir(B).relativeFilePath(source));
            if (!claimed.isEmpty() && claimed.compare(hash, Qt::CaseInsensitive) != 0)
                corrupt = true;
            return hash;
        };
        const QString wavHash = checkedHash(item.wavSource);
        const QString binHash = checkedHash(item.binSource);
        if (corrupt) {
            refuse(QStringLiteral("The bundle is damaged: sample %1 doesn't match the hash its "
                                  "manifest records.")
                       .arg(symbol));
            continue;
        }

        // Same bytes behind a project symbol -> reuse that symbol. Candidates
        // come from a size match, so only plausible files are ever hashed.
        QStringList candidates;
        const QString &primary = item.wavSource.isEmpty() ? item.binSource : item.wavSource;
        candidates = projectSamples.bySize.value(QFileInfo(primary).size());
        std::sort(candidates.begin(), candidates.end());
        if (candidates.removeAll(symbol) > 0)
            candidates.prepend(symbol); // the same name wins among equals
        for (const QString &candidate : candidates) {
            const auto matches = [&](const QString &bundleHash, const QString &projectFile) {
                return bundleHash.isEmpty() ||
                       (!projectFile.isEmpty() && fileHash(projectFile) == bundleHash);
            };
            if (matches(wavHash, projectSamples.wavOf.value(candidate)) &&
                matches(binHash, projectSamples.rawOf.value(candidate))) {
                item.projectSymbol = candidate;
                item.action = ImportAction::Reuse;
                break;
            }
        }
        if (item.action != ImportAction::Reuse) {
            QString base = symbol.startsWith(QLatin1String(kSamplePrefix))
                               ? symbol.mid(int(qstrlen(kSamplePrefix)))
                               : symbol;
            static const QRegularExpression grammarRe(QStringLiteral("^[a-z0-9_]+$"));
            if (!grammarRe.match(base).hasMatch())
                base = SampleRegistrar::sanitizeSampleName(base);
            if (base.isEmpty())
                base = QStringLiteral("sample");
            item.name = firstFree(base, sampleNameTaken);
            takenSampleNames.insert(item.name);
            item.projectSymbol = QLatin1String(kSamplePrefix) + item.name;
            item.action = actionFor(item);
            if (item.wavSource.isEmpty())
                plan.warnings.append(
                    QStringLiteral("%1 is raw sample data (a .bin with no .wav source). It is "
                                   "copied as sound/direct_sound_samples/%2.bin — a file your "
                                   "project's .gitignore or `make clean` may treat as a build "
                                   "artifact.")
                        .arg(symbol, item.name));
        }
        if (item.projectSymbol != symbol)
            renames.insert(symbol, item.projectSymbol);
        plan.samples.append(item);
    }

    const bool newSamples =
        ImportPlan::count(plan.samples, ImportAction::Reuse) != plan.samples.size();
    if (newSamples) {
        const SampleFormatProbe probe = SampleRegistrar::probeSampleFormat(P);
        if (!probe.ok())
            refuse(QStringLiteral("This song brings samples the project doesn't have, and they "
                                  "can't be registered: %1")
                       .arg(probe.refusal));
    }
    QStringList newSynths;
    for (const ImportItem &item : plan.synths) {
        if (item.action != ImportAction::Reuse)
            newSynths.append(item.bundleSymbol);
    }
    if (!newSynths.isEmpty() && !projectSynths.creatable())
        refuse(QStringLiteral("This song uses Golden Sun synth voices (%1), but the project "
                              "doesn't define the set_synth_* macros, so its voicegroup would not "
                              "assemble. Add ipatix's improved mixer (with its synth macros) to "
                              "the project first.")
                   .arg(newSynths.join(QStringLiteral(", "))));

    // ---- 2. Programmable waves -------------------------------------------------------
    {
        QHash<QString, QString> bundleWaves, projectWaves;
        collectIncbins(readAllBytes(B + QStringLiteral("/sound/programmable_wave_data.inc")),
                       &bundleWaves);
        const QString projectInc = P + QStringLiteral("/sound/programmable_wave_data.inc");
        collectIncbins(readAllBytes(projectInc), &projectWaves);
        QStringList projectSymbols = projectWaves.keys();
        std::sort(projectSymbols.begin(), projectSymbols.end());
        QSet<QString> takenNames;
        bool anyNew = false;
        for (const QString &symbol : sortedList(refs.waves)) {
            ImportItem item;
            item.bundleSymbol = symbol;
            item.binSource = B + QLatin1Char('/') + bundleWaves.value(symbol);
            bool ok = false;
            const QByteArray data = readAllBytes(item.binSource, &ok);
            if (!bundleWaves.contains(symbol) || !ok) {
                incomplete.append(symbol);
                continue;
            }
            QStringList candidates = projectSymbols;
            if (candidates.removeAll(symbol) > 0)
                candidates.prepend(symbol);
            for (const QString &candidate : candidates) {
                if (readAllBytes(P + QLatin1Char('/') + projectWaves.value(candidate)) == data) {
                    item.projectSymbol = candidate;
                    item.action = ImportAction::Reuse;
                    break;
                }
            }
            if (item.action != ImportAction::Reuse) {
                const QString base = identifierFrom(symbol.startsWith(QLatin1String(kWavePrefix))
                                                        ? symbol.mid(int(qstrlen(kWavePrefix)))
                                                        : symbol,
                                                    QStringLiteral("wave"));
                item.name = firstFree(base, [&](const QString &candidate) {
                    return takenNames.contains(candidate) ||
                           projectWaves.contains(QLatin1String(kWavePrefix) + candidate) ||
                           QFile::exists(P + QLatin1String(kWavesDir) + candidate +
                                         QStringLiteral(".pcm"));
                });
                takenNames.insert(item.name);
                item.projectSymbol = QLatin1String(kWavePrefix) + item.name;
                item.action = actionFor(item);
                anyNew = true;
            }
            if (item.projectSymbol != symbol)
                renames.insert(symbol, item.projectSymbol);
            plan.waves.append(item);
        }
        if (anyNew && !QFile::exists(projectInc))
            refuse(QStringLiteral("This song brings programmable waves, but the project has no "
                                  "sound/programmable_wave_data.inc to register them in."));
    }

    // ---- 4. Keysplit tables ----------------------------------------------------------
    {
        const QHash<QString, QList<QByteArray>> bundleTables =
            collectKeysplitTables(readAllBytes(B + QStringLiteral("/sound/keysplit_tables.inc")));
        const QString projectInc = P + QStringLiteral("/sound/keysplit_tables.inc");
        const bool projectFileExists = QFile::exists(projectInc);
        const QHash<QString, QList<QByteArray>> projectBlocks =
            collectKeysplitTables(readAllBytes(projectInc));
        QHash<QString, KeysplitTable> projectTables;
        bool projectMacroForm = false, formKnown = false;
        QStringList projectSymbols = projectBlocks.keys();
        std::sort(projectSymbols.begin(), projectSymbols.end());
        for (const QString &symbol : projectSymbols) {
            KeysplitTable table;
            if (!parseKeysplitTable(projectBlocks.value(symbol), &table))
                continue;
            projectTables.insert(symbol, table);
            // The file's form: what most of its tables use.
            if (!formKnown) {
                projectMacroForm = table.macroForm;
                formKnown = true;
            }
        }
        if (!formKnown)
            projectMacroForm = keysplitMacroDefined(P);

        QSet<QString> takenSymbols;
        bool anyNew = false;
        for (const QString &symbol : sortedList(refs.tables)) {
            ImportItem item;
            item.bundleSymbol = symbol;
            const auto block = bundleTables.constFind(symbol);
            if (block == bundleTables.constEnd()) {
                incomplete.append(symbol);
                continue;
            }
            KeysplitTable table;
            const bool parsed = parseKeysplitTable(block.value(), &table);
            if (parsed) {
                QStringList candidates = projectSymbols;
                if (candidates.removeAll(symbol) > 0)
                    candidates.prepend(symbol);
                for (const QString &candidate : candidates) {
                    const auto found = projectTables.constFind(candidate);
                    if (found != projectTables.constEnd() && found->sameContent(table)) {
                        item.projectSymbol = candidate;
                        item.action = ImportAction::Reuse;
                        break;
                    }
                }
            }
            if (item.action != ImportAction::Reuse) {
                if (!parsed) {
                    refuse(QStringLiteral("Keysplit table %1 is written in a form porydaw can't "
                                          "read, so it can't be compared or copied.")
                               .arg(symbol));
                    continue;
                }
                // The keysplit macro names its symbol keysplit_<label>.
                QString base = symbol;
                if (projectMacroForm && !base.startsWith(QStringLiteral("keysplit_")))
                    base = QStringLiteral("keysplit_") + identifierFrom(base, QStringLiteral("t"));
                item.projectSymbol = firstFree(base, [&](const QString &candidate) {
                    return takenSymbols.contains(candidate) || projectBlocks.contains(candidate);
                });
                takenSymbols.insert(item.projectSymbol);
                item.action = actionFor(item);
                item.lines = renderKeysplitTable(item.projectSymbol, table, projectMacroForm);
                anyNew = true;
            }
            if (item.projectSymbol != symbol)
                renames.insert(symbol, item.projectSymbol);
            plan.tables.append(item);
        }
        if (anyNew && !projectFileExists)
            plan.warnings.append(
                QStringLiteral("The project has no sound/keysplit_tables.inc; it will be created "
                               "(%1 form). Make sure your build includes it.")
                    .arg(projectMacroForm ? QStringLiteral("keysplit macro")
                                          : QStringLiteral(".set/.byte")));
    }

    if (!incomplete.isEmpty()) {
        std::sort(incomplete.begin(), incomplete.end());
        refuse(QStringLiteral("The bundle is incomplete: it doesn't carry %1.")
                   .arg(incomplete.join(QStringLiteral(", "))));
    }

    // ---- 6 (name only) and 5. Voicegroups ---------------------------------------------
    const QList<GroupSource> projectGroups = scanProjectGroups(P);
    QSet<QString> projectGroupSymbols;
    for (const GroupSource &group : projectGroups)
        projectGroupSymbols.insert(group.symbol);
    for (const QString &arg : SongRegistry::voicegroupArgs(P))
        projectGroupSymbols.insert(QStringLiteral("voicegroup") + arg);
    QSet<QString> takenGroupNames;
    const auto groupNameFor = [&](const QString &symbol) {
        QString base = symbol.mid(10); // past "voicegroup"
        while (base.startsWith(QLatin1Char('_')))
            base.remove(0, 1);
        base = identifierFrom(base, QStringLiteral("imported"));
        const QString name = firstFree(base, [&](const QString &candidate) {
            return takenGroupNames.contains(candidate) ||
                   projectGroupSymbols.contains(QStringLiteral("voicegroup_") + candidate) ||
                   QFile::exists(P + QStringLiteral("/sound/voicegroups/%1.inc").arg(candidate));
        });
        takenGroupNames.insert(name);
        return name;
    };
    const auto renamedBody = [&](const GroupSource &group) {
        QList<QByteArray> body;
        for (const QByteArray &line : group.body)
            body.append(renameSymbols(line, renames));
        return body;
    };

    // The top-level name first: sub-voicegroups may point back at it.
    plan.voicegroup.bundleSymbol = topSymbol;
    plan.voicegroup.name = groupNameFor(topSymbol);
    plan.voicegroup.projectSymbol = QStringLiteral("voicegroup_") + plan.voicegroup.name;
    plan.voicegroup.action = actionFor(plan.voicegroup);
    if (plan.voicegroup.projectSymbol != topSymbol)
        renames.insert(topSymbol, plan.voicegroup.projectSymbol);

    // Sub-voicegroups resolve once everything they reference has: content is
    // compared after renaming. Groups caught in a reference cycle (or naming
    // themselves) can't be compared that way and are always created.
    QList<int> pending;
    for (int i = 0; i < subs.size(); i++)
        pending.append(i);
    QHash<QString, ImportItem> subItems;
    const auto subDependencies = [&](const GroupSource &sub) {
        References own;
        collectReferences(sub, &own);
        return own.subGroups;
    };
    const auto createSub = [&](const GroupSource &sub) {
        ImportItem item;
        item.bundleSymbol = sub.symbol;
        item.name = groupNameFor(sub.symbol);
        item.projectSymbol = QStringLiteral("voicegroup_") + item.name;
        item.action = actionFor(item);
        item.startingNote = sub.startingNote;
        if (item.projectSymbol != sub.symbol)
            renames.insert(sub.symbol, item.projectSymbol);
        return item;
    };
    bool progress = true;
    while (!pending.isEmpty() && progress) {
        progress = false;
        for (int p = 0; p < pending.size(); p++) {
            const GroupSource &sub = subs.at(pending.at(p));
            bool ready = true;
            for (const QString &dep : subDependencies(sub))
                ready = ready && dep != sub.symbol && (dep == topSymbol || subItems.contains(dep));
            if (!ready)
                continue;
            const QList<QByteArray> wanted = normalizedVoices(sub, renames);
            ImportItem item;
            item.bundleSymbol = sub.symbol;
            QList<const GroupSource *> candidates;
            for (const GroupSource &group : projectGroups) {
                if (group.symbol == sub.symbol)
                    candidates.prepend(&group);
                else
                    candidates.append(&group);
            }
            for (const GroupSource *group : candidates) {
                if (group->voices.size() == wanted.size() &&
                    normalizedVoices(*group, {}) == wanted) {
                    item.projectSymbol = group->symbol;
                    item.action = ImportAction::Reuse;
                    if (item.projectSymbol != sub.symbol)
                        renames.insert(sub.symbol, item.projectSymbol);
                    break;
                }
            }
            if (item.action != ImportAction::Reuse)
                item = createSub(sub);
            subItems.insert(sub.symbol, item);
            pending.removeAt(p--);
            progress = true;
        }
    }
    for (int index : pending)
        subItems.insert(subs.at(index).symbol, createSub(subs.at(index)));
    for (const GroupSource &sub : subs) {
        ImportItem item = subItems.value(sub.symbol);
        if (item.action != ImportAction::Reuse)
            item.lines = renamedBody(sub); // every rename is known by now
        plan.subVoicegroups.append(item);
    }
    plan.voicegroup.lines = renamedBody(top);
    plan.voicegroup.startingNote = top.startingNote;

    // ---- 7. The song ----------------------------------------------------------------------
    static const QRegularExpression labelRe(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
    const QString midiDir = P + QStringLiteral("/sound/songs/midi/");
    const QByteArray songsH = readAllBytes(P + QStringLiteral("/include/constants/songs.h"));
    const auto labelTaken = [&](const QString &label) {
        for (const SongInfo &existing : project.songs()) {
            if (existing.label.compare(label, Qt::CaseInsensitive) == 0)
                return true;
        }
        return QFile::exists(midiDir + label + QStringLiteral(".mid")) ||
               QFile::exists(midiDir + label + QStringLiteral(".s"));
    };
    const auto constantTaken = [&](const QString &constant) {
        for (const SongInfo &existing : project.songs()) {
            if (existing.constant == constant)
                return true;
        }
        const QRegularExpression defineRe(
            QStringLiteral(R"(^\s*#\s*define\s+%1\b)").arg(QRegularExpression::escape(constant)),
            QRegularExpression::MultilineOption);
        return defineRe.match(QString::fromUtf8(songsH)).hasMatch();
    };

    if (!options.label.isEmpty()) {
        plan.label = options.label;
        if (!labelRe.match(plan.label).hasMatch())
            refuse(QStringLiteral("'%1' is not a valid song label (letters, digits and "
                                  "underscores).")
                       .arg(plan.label));
        else if (labelTaken(plan.label))
            refuse(QStringLiteral("The project already has a song labelled %1.").arg(plan.label));
    } else {
        plan.label = firstFree(plan.manifest.label, labelTaken);
    }
    if (!options.constant.isEmpty()) {
        plan.constant = options.constant;
        if (!labelRe.match(plan.constant).hasMatch())
            refuse(QStringLiteral("'%1' is not a valid constant name.").arg(plan.constant));
        else if (constantTaken(plan.constant))
            refuse(QStringLiteral("The project already defines %1.").arg(plan.constant));
    } else {
        // The manifest's hint belongs to the bundle's own label; a renamed
        // song derives its constant from the new label instead.
        const QString hinted =
            plan.label == plan.manifest.label && labelRe.match(plan.manifest.constant).hasMatch()
                ? plan.manifest.constant
                : SongRegistry::constantForLabel(plan.label);
        plan.constant = firstFree(hinted, constantTaken);
    }
    QStringList players;
    for (const MusicPlayer &player : project.musicPlayers())
        players.append(player.name);
    if (!options.player.isEmpty()) {
        plan.player = options.player;
        if (!players.contains(plan.player))
            refuse(QStringLiteral("The project has no music player named %1.").arg(plan.player));
    } else if (players.contains(plan.manifest.player)) {
        plan.player = plan.manifest.player;
    } else if (players.contains(QStringLiteral("MUSIC_PLAYER_BGM")) || players.isEmpty()) {
        plan.player = QStringLiteral("MUSIC_PLAYER_BGM");
    } else {
        plan.player = players.first();
    }

    SongCfg cfg = song.cfg;
    cfg.voicegroupArg = plan.voicegroup.projectSymbol.mid(10);
    plan.flags = SongRegistry::mergeCfgFlags(cfg);

    // ---- Engine extensions: warn, never gate (§3.5) -----------------------------------
    QStringList missing;
    for (const QString &mnemonic : usedExtensions(smf)) {
        if (!projectDefinesExtension(P, mnemonic))
            missing.append(extensionUse(smf, mnemonic));
    }
    if (!missing.isEmpty())
        plan.warnings.prepend(
            QStringLiteral("This song uses m4a engine extensions the project doesn't define in "
                           "sound/MPlayDef.s: %1. A stock engine ignores them, so those effects "
                           "won't be heard in-game (porydaw still plays them).")
                .arg(missing.join(QStringLiteral("; "))));
    return plan;
}

bool applyImportPlan(const ImportPlan &plan, QString *error, QStringList *written)
{
    QStringList done;
    const auto finish = [&](bool ok) {
        if (written)
            *written = done;
        return ok;
    };
    const auto fail = [&](const QString &step, const QString &why) {
        if (error) {
            *error = QStringLiteral("Import stopped while %1: %2\n\n").arg(step, why);
            *error += done.isEmpty()
                          ? QStringLiteral("Nothing was written to the project.")
                          : QStringLiteral("Already written (the project still builds; these "
                                           "are simply unused until the import is retried): %1.")
                                .arg(done.join(QStringLiteral(", ")));
        }
        return finish(false);
    };
    if (!plan.ok())
        return fail(QStringLiteral("checking the plan"), plan.refusals.join(QLatin1Char(' ')));

    const QString &P = plan.projectRoot;
    QString why;

    // 1. Samples.
    const QString dsInc = P + QStringLiteral("/sound/direct_sound_data.inc");
    for (const ImportItem &item : plan.samples) {
        if (item.action == ImportAction::Reuse)
            continue;
        const QString step = QStringLiteral("adding sample %1").arg(item.projectSymbol);
        bool ok = false;
        if (!item.wavSource.isEmpty()) {
            const QByteArray wav = readAllBytes(item.wavSource, &ok);
            if (!ok)
                return fail(step, QStringLiteral("cannot read %1.").arg(item.wavSource));
            if (!SampleRegistrar::registerSample(P, item.name, wav, &why))
                return fail(step, why);
            done.append(item.projectSymbol); // on disk from here, whatever follows
        }
        if (!item.binSource.isEmpty()) {
            const QByteArray bin = readAllBytes(item.binSource, &ok);
            if (!ok)
                return fail(step, QStringLiteral("cannot read %1.").arg(item.binSource));
            if (item.wavSource.isEmpty() &&
                !SampleRegistrar::validateSampleName(P, item.name,
                                                     VoicegroupSource::directSoundSymbols(P), &why))
                return fail(step, why);
            const QString incbin =
                QStringLiteral("sound/direct_sound_samples/%1.bin").arg(item.name);
            if (!writeBytes(P + QLatin1Char('/') + incbin, bin, &why))
                return fail(step, why);
            if (!done.contains(item.projectSymbol))
                done.append(item.projectSymbol);
            // With a .wav the registrar already wrote the entry (the .bin is
            // then the build artifact `cry` voices read).
            if (item.wavSource.isEmpty() &&
                !appendBlock(dsInc, incbinEntry(dsInc, item.projectSymbol, incbin), &why))
                return fail(step, why);
        }
    }

    // 2. Programmable waves.
    const QString waveInc = P + QStringLiteral("/sound/programmable_wave_data.inc");
    for (const ImportItem &item : plan.waves) {
        if (item.action == ImportAction::Reuse)
            continue;
        const QString step = QStringLiteral("adding programmable wave %1").arg(item.projectSymbol);
        bool ok = false;
        const QByteArray data = readAllBytes(item.binSource, &ok);
        if (!ok)
            return fail(step, QStringLiteral("cannot read %1.").arg(item.binSource));
        const QString incbin =
            QStringLiteral("sound/programmable_wave_samples/%1.pcm").arg(item.name);
        if (QFile::exists(P + QLatin1Char('/') + incbin))
            return fail(step, QStringLiteral("%1 already exists.").arg(incbin));
        if (!writeBytes(P + QLatin1Char('/') + incbin, data, &why))
            return fail(step, why);
        done.append(item.projectSymbol);
        if (!appendBlock(waveInc, incbinEntry(waveInc, item.projectSymbol, incbin), &why))
            return fail(step, why);
    }

    // 3. Synth definitions (equal ones already on disk are skipped).
    QList<QPair<QString, VgSynthDesc>> synthDefs;
    for (const ImportItem &item : plan.synths) {
        if (item.action != ImportAction::Reuse)
            synthDefs.append({item.projectSymbol, item.synth});
    }
    if (!synthDefs.isEmpty()) {
        if (!VoicegroupSource::writeSynthDefinitions(P, synthDefs, &why))
            return fail(QStringLiteral("adding the synth definitions"), why);
        for (const auto &def : synthDefs)
            done.append(def.first);
    }

    // 4. Keysplit tables.
    const QString tableInc = P + QStringLiteral("/sound/keysplit_tables.inc");
    for (const ImportItem &item : plan.tables) {
        if (item.action == ImportAction::Reuse)
            continue;
        const QString step = QStringLiteral("adding keysplit table %1").arg(item.projectSymbol);
        if (collectKeysplitTables(readAllBytes(tableInc)).contains(item.projectSymbol))
            return fail(step, QStringLiteral("%1 already exists.").arg(item.projectSymbol));
        if (!appendBlock(tableInc, item.lines, &why))
            return fail(step, why);
        done.append(item.projectSymbol);
    }

    // 5. + 6. Sub-voicegroups, then the song's voicegroup.
    QList<ImportItem> groups = plan.subVoicegroups;
    groups.append(plan.voicegroup);
    for (const ImportItem &item : groups) {
        if (item.action == ImportAction::Reuse)
            continue;
        const QString step = QStringLiteral("creating %1").arg(item.projectSymbol);
        if (!VoicegroupSource::createVoicegroupFromLines(P, item.name, item.lines,
                                                         item.startingNote, &why))
            return fail(step, why);
        done.append(item.projectSymbol);
        if (!VoicegroupSource::appendIncludeLine(P, item.name, &why))
            return fail(step, why);
    }

    // 7. The song.
    const QString midiDir = P + QStringLiteral("/sound/songs/midi");
    const QString midPath = midiDir + QStringLiteral("/%1.mid").arg(plan.label);
    {
        const QString step = QStringLiteral("writing %1.mid").arg(plan.label);
        bool ok = false;
        const QByteArray mid = readAllBytes(plan.midSource, &ok);
        if (!ok)
            return fail(step, QStringLiteral("cannot read %1.").arg(plan.midSource));
        if (QFile::exists(midPath))
            return fail(step, QStringLiteral("%1 already exists.").arg(midPath));
        if (!writeBytes(midPath, mid, &why))
            return fail(step, why);
        done.append(QStringLiteral("sound/songs/midi/%1.mid").arg(plan.label));
    }
    if (!SongRegistry::writeSongFlags(midiDir, plan.label, plan.flags, &why))
        return fail(QStringLiteral("writing the song's mid2agb flags"), why);
    done.append(QStringLiteral("the song's mid2agb flags"));
    if (!SongRegistry::registerSong(P, plan.label, plan.constant, plan.player, &why)) {
        // Keep the chosen constant/player so File → Register Song can retry.
        SongRegistry::saveRegistrationMeta(P, plan.label, plan.constant, plan.player);
        return fail(QStringLiteral("registering %1 (File → Register Song can retry this "
                                   "step)")
                        .arg(plan.label),
                    why);
    }
    SongRegistry::clearRegistrationMeta(P, plan.label);
    done.append(QStringLiteral("the registration of %1").arg(plan.label));
    return finish(true);
}

} // namespace SongBundle
