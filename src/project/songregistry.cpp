#include "songregistry.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPair>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>
#include <algorithm>
#include <limits>

#include "core/smf.h"
#include "project/sidecar.h"
#include "project/songsmk.h"
#include "project/voicegroupsource.h"

namespace {

QStringList readLines(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QStringList lines;
    QTextStream in(&file);
    while (!in.atEnd())
        lines.append(in.readLine());
    return lines;
}

// "	song mus_dummy, MUSIC_PLAYER_BGM, 0"
const QRegularExpression &songLineRe()
{
    static const QRegularExpression re(
        QStringLiteral(R"(^(\s*)song\s+(\w+)\s*,\s*(\w+)\s*,\s*(\w+))"));
    return re;
}

// One pass over song_table.inc's lines, everything the planners and the
// delete path need. A free slot is an entry past index 0 bearing entry 0's
// label: entry 0 is the engine's fallback song (mus_dummy), so a later
// duplicate of it is a placeholder by construction — deletion leaves
// exactly that, and hand-written dummy placeholders count too. Free slots
// keep their index (that is their whole point) but never record a label
// occurrence, so the fallback song's own table index stays 0.
struct SongTableScan {
    int count = 0;       // song entries, free slots included
    int labelIndex = -1; // the label's index (last non-free occurrence)
    int labelLine = -1;
    // Every index bearing the label: forks fill new table slots with copies
    // of real songs (vanilla uses a dummy), so one label can own several.
    QVector<int> labelIndices;
    QString labelIndent;
    int freeIndex = -1; // lowest free slot
    int freeLine = -1;
    QVector<int> freeIndices; // every free slot, ascending — region-bounded
                              // layouts may skip ineligible low ones
    QVector<int> entryLines;  // line of every entry, by table index
    QStringList entryLabels;  // label of every entry, by table index
    int lastSongLine = -1;
    QString lastSongLabel; // the final entry's label
    QString indent;        // the last entry's indentation
    // Entry 0's pieces — the template for the free-slot line a deletion
    // leaves behind.
    QString firstLabel, firstPlayer, firstPlayerNum;
};

SongTableScan scanSongTable(const QStringList &lines, const QString &label)
{
    SongTableScan scan;
    for (int i = 0; i < lines.size(); i++) {
        const QRegularExpressionMatch m = songLineRe().match(lines.at(i));
        if (!m.hasMatch())
            continue;
        if (scan.count == 0) {
            scan.firstLabel = m.captured(2);
            scan.firstPlayer = m.captured(3);
            scan.firstPlayerNum = m.captured(4);
        }
        if (scan.count > 0 && m.captured(2) == scan.firstLabel) {
            if (scan.freeLine < 0) {
                scan.freeIndex = scan.count;
                scan.freeLine = i;
            }
            scan.freeIndices.append(scan.count);
        } else if (m.captured(2) == label) {
            scan.labelIndex = scan.count;
            scan.labelLine = i;
            scan.labelIndices.append(scan.count);
            scan.labelIndent = m.captured(1);
        }
        scan.indent = m.captured(1);
        scan.lastSongLine = i;
        scan.lastSongLabel = m.captured(2);
        scan.entryLines.append(i);
        scan.entryLabels.append(m.captured(2));
        scan.count++;
    }
    return scan;
}

// Some pokeemerald-expansion lines keep songs.h's constants in contiguous
// regions closed by markers — "#define END_SE <last SE>",
// "#define START_MUS <first music ID>", "#define END_MUS <last MUS>" — and
// size ID-indexed arrays from them. Pre-#9713 checkouts alias the last
// constant and size src/debug.c's sound-tester arrays
// (sBGMNames[END_MUS - START_MUS + 1]); the night-music line re-added
// value-form markers ("#define END_MUS 558") and sizes overworld.c's
// sNightMusicTable from them. Either way a song appended past the phoneme
// block sits outside the region: at best the feature cannot address it, at
// worst its designated initializer breaks the build ("array index in
// initializer exceeds array bounds"). Whenever an END_MUS marker resolves
// — modern checkouts have none, PR #9713 deleted all three — placement is
// region-aware instead: music inserts at END_MUS + 1 ahead of the phoneme
// block (whose IDs all shift up by one, in songs.h and charmap.txt alike),
// sound effects fill the placeholder slots between the SE region and
// START_MUS (bounded by END_SE, or by the highest define below START_MUS
// when the layout lost that marker), and the marker follows the new last
// song (alias markers re-point, value markers renumber). Which sound list
// a debug.c entry belongs in is only
// functional in the pre-#9713 shape, whose two lists feed separately
// indexed arrays — that shape alone (separateDebugArrays) overrides the
// SE_-prefix routing for songs placed in the music region.
struct RegionMarker {
    int line = -1;    // the "#define END_x <name-or-value>" line in songs.h
    QString referent; // the aliased constant's name; empty in value form
    int value = -1;   // the marker's resolved numeric value
    bool valid() const { return line >= 0 && value >= 0; }
};

struct RegionMarkers {
    RegionMarker endSe, endMus;
    int startMus = -1;                // START_MUS's value, -1 when absent
    bool regioned = false;            // an END_MUS marker resolves (see above)
    bool separateDebugArrays = false; // debug.c consumes END_MUS (pre-#9713)
};

RegionMarkers scanRegionMarkers(const QStringList &songsHLines, const QStringList &debugLines)
{
    static const QRegularExpression markerRe(
        QStringLiteral(R"(^\s*#define\s+(END_SE|END_MUS)\s+([A-Za-z_]\w*|\d+)\s*(//.*)?$)"));
    static const QRegularExpression valueRe(QStringLiteral(R"(^\s*#define\s+(\w+)\s+(\d+)\b)"));
    RegionMarkers m;
    QHash<QString, int> values;
    for (int i = 0; i < songsHLines.size(); i++) {
        const QString &line = songsHLines.at(i);
        const QRegularExpressionMatch v = valueRe.match(line);
        if (v.hasMatch() && !values.contains(v.captured(1)))
            values.insert(v.captured(1), v.captured(2).toInt());
        const QRegularExpressionMatch mk = markerRe.match(line);
        if (!mk.hasMatch())
            continue;
        RegionMarker &marker = mk.captured(1) == QStringLiteral("END_SE") ? m.endSe : m.endMus;
        if (marker.line < 0) {
            marker.line = i;
            bool numeric = false;
            const int value = mk.captured(2).toInt(&numeric);
            if (numeric)
                marker.value = value;
            else
                marker.referent = mk.captured(2);
        }
    }
    if (m.endSe.line >= 0 && !m.endSe.referent.isEmpty())
        m.endSe.value = values.value(m.endSe.referent, -1);
    if (m.endMus.line >= 0 && !m.endMus.referent.isEmpty())
        m.endMus.value = values.value(m.endMus.referent, -1);
    m.startMus = values.value(QStringLiteral("START_MUS"), -1);
    m.regioned = m.endMus.valid();
    if (m.regioned) {
        static const QRegularExpression useRe(QStringLiteral(R"(\bEND_MUS\b)"));
        for (const QString &line : debugLines) {
            if (useRe.match(line).hasMatch()) {
                m.separateDebugArrays = true;
                break;
            }
        }
    }
    return m;
}

// The marker names never renumber and never anchor an ID-ordered insertion:
// a value-form START_MUS mirrors its region's first ID, not a song.
bool isRegionMarkerName(const QString &name)
{
    return name == QStringLiteral("END_SE") || name == QStringLiteral("END_MUS") ||
           name == QStringLiteral("START_MUS");
}

// "MUS_DUMMY = 00 00" — a charmap.txt entry whose value is two hex bytes.
// Only entries named by a songs.h constant are treated as song-ID mappings;
// other two-byte entries ("PKMN = 53 54", the FD placeholders) are not.
const QRegularExpression &charmapEntryRe()
{
    static const QRegularExpression re(
        QStringLiteral(R"(^(\w+)( *)= *([0-9A-Fa-f]{2}) ([0-9A-Fa-f]{2})\s*$)"));
    return re;
}

// Numeric #define names from songs.h. charmap.txt's sound section mirrors
// these constants (text control codes address songs by ID), which is what
// lets porydaw recognize that section without any marker in the file.
QSet<QString> songsHConstantNames(const QString &projectRoot)
{
    static const QRegularExpression defineRe(QStringLiteral(R"(^\s*#define\s+(\w+)\s+\d)"));
    QSet<QString> names;
    for (const QString &line :
         readLines(projectRoot + QStringLiteral("/include/constants/songs.h"))) {
        const QRegularExpressionMatch m = defineRe.match(line);
        if (m.hasMatch())
            names.insert(m.captured(1));
    }
    return names;
}

// A song ID as charmap.txt value bytes: little-endian, uppercase hex.
QString charmapIdBytes(int songId)
{
    return QStringLiteral("%1 %2")
        .arg(songId & 0xFF, 2, 16, QLatin1Char('0'))
        .arg((songId >> 8) & 0xFF, 2, 16, QLatin1Char('0'))
        .toUpper();
}

// pokeemerald-expansion's src/debug.c lists every song its debug menu can
// play as X-macro entries under two #defines — MUS_* in SOUND_LIST_BGM,
// SE_* in SOUND_LIST_SE. (Which list an entry sits in is cosmetic: both
// feed one array indexed by the constant's value, and the menu's search
// filters by name prefix.) Two entry shapes exist in the wild:
//
//   X(MUS_FOO)                \      current expansion (COMPOUND_STRING);
//                                    the macro's final line is bare
//   X(MUS_FOO   , "MUS-FOO"   ) \    older expansion and forks of it: a
//                                    display-name argument, every line
//                                    continued, a blank line ends the macro
//
// Both column-align within a list, but the two lists may align differently,
// so style is learned per list. Vanilla pokeemerald has no such file, so
// the whole leg is "applicable" only when a sound-list #define exists.
struct DebugSoundList {
    QString name; // SOUND_LIST_BGM or SOUND_LIST_SE
    int defineLine = -1;
    QVector<int> entryLines;
    QStringList entryNames;
    // Style, learned from this list's own entries. Columns are -1 when the
    // entries don't align (or nothing carries the column to learn from).
    QString indent = QStringLiteral("    ");
    bool named = false;   // entries carry the "MUS-FOO" display-name argument
    int commaColumn = -1; // ',' column in named entries
    int parenColumn = -1; // ')' column in named entries
    int slashColumn = -1; // '\' continuation column
};

struct DebugSoundScan {
    QVector<DebugSoundList> lists;
    bool applicable() const { return !lists.isEmpty(); }
};

// Either entry shape, with or without the trailing continuation.
const QRegularExpression &debugEntryRe()
{
    static const QRegularExpression re(
        QStringLiteral(R"(^(\s*)X\((\w+) *(, *"[^"]*" *)?\)\s*(\\?)\s*$)"));
    return re;
}

// A '\'-terminated line continues the macro onto the next line (trailing
// spaces after the backslash tolerated, as compilers tolerate them).
bool continuesMacro(const QString &line)
{
    int end = line.size();
    while (end > 0 && line.at(end - 1).isSpace())
        end--;
    return end > 0 && line.at(end - 1) == QLatin1Char('\\');
}

DebugSoundScan scanDebugSoundLists(const QStringList &lines)
{
    static const QRegularExpression defineRe(
        QStringLiteral(R"(^#define\s+(SOUND_LIST_BGM|SOUND_LIST_SE)\b)"));
    DebugSoundScan scan;
    for (int i = 0; i < lines.size(); i++) {
        const QRegularExpressionMatch dm = defineRe.match(lines.at(i));
        if (!dm.hasMatch())
            continue;
        DebugSoundList list;
        list.name = dm.captured(1);
        list.defineLine = i;
        // A column stays aligned while every entry that carries it agrees.
        const auto learn = [](int *column, bool *aligned, int at) {
            if (*column < 0)
                *column = at;
            else if (at != *column)
                *aligned = false;
        };
        bool commaAligned = true, parenAligned = true, slashAligned = true;
        int j = i;
        while (j < lines.size() - 1 && continuesMacro(lines.at(j))) {
            j++;
            const QRegularExpressionMatch em = debugEntryRe().match(lines.at(j));
            if (!em.hasMatch())
                continue;
            list.entryLines.append(j);
            list.entryNames.append(em.captured(2));
            list.indent = em.captured(1);
            if (em.capturedStart(3) >= 0) {
                list.named = true;
                learn(&list.commaColumn, &commaAligned, int(em.capturedStart(3)));
                learn(&list.parenColumn, &parenAligned, int(em.capturedEnd(3)));
            }
            if (!em.captured(4).isEmpty())
                learn(&list.slashColumn, &slashAligned,
                      int(lines.at(j).lastIndexOf(QLatin1Char('\\'))));
        }
        if (!commaAligned)
            list.commaColumn = -1;
        if (!parenAligned)
            list.parenColumn = -1;
        if (!slashAligned)
            list.slashColumn = -1;
        scan.lists.append(list);
        i = j;
    }
    return scan;
}

// The list a constant belongs in: SE_* under SOUND_LIST_SE, everything
// else under SOUND_LIST_BGM, whichever exists when the wanted one doesn't.
// On regioned layouts the song's region decides instead of the name — an
// SE_* song placed in the music region must sit in SOUND_LIST_BGM, whose
// array is the one indexed by its ID (forceBgm).
const DebugSoundList *debugTargetList(const DebugSoundScan &scan, const QString &constant,
                                      bool forceBgm = false)
{
    if (scan.lists.isEmpty())
        return nullptr;
    const QString want = !forceBgm && constant.startsWith(QStringLiteral("SE_"))
                             ? QStringLiteral("SOUND_LIST_SE")
                             : QStringLiteral("SOUND_LIST_BGM");
    const DebugSoundList *target = &scan.lists.first();
    for (const DebugSoundList &list : scan.lists) {
        if (list.name == want)
            target = &list;
    }
    return target;
}

// The list whose entries lend a new line its style: the target, unless it
// is empty and another list has entries to imitate.
const DebugSoundList *debugStyleList(const DebugSoundScan &scan, const DebugSoundList *target)
{
    if (!target->entryNames.isEmpty())
        return target;
    for (const DebugSoundList &list : scan.lists) {
        if (!list.entryNames.isEmpty())
            return &list;
    }
    return target;
}

// An entry line (sans continuation) in the style's shape: the named form
// derives the display name by hyphenating the constant, padding into the
// style's columns when its entries align.
QString debugEntryText(const DebugSoundList &style, const QString &constant)
{
    QString text = style.indent + QStringLiteral("X(") + constant;
    if (style.named) {
        QString display = constant;
        display.replace(QLatin1Char('_'), QLatin1Char('-'));
        if (style.commaColumn > int(text.size()))
            text += QString(style.commaColumn - int(text.size()), QLatin1Char(' '));
        text += QStringLiteral(", \"") + display + QLatin1Char('"');
        if (style.parenColumn > int(text.size()))
            text += QString(style.parenColumn - int(text.size()), QLatin1Char(' '));
    }
    return text + QLatin1Char(')');
}

// The mid-list form: pad the '\' into the style's column (one space when
// the entries don't align or the line is too long).
QString debugContinuation(const QString &text, int slashColumn)
{
    const int pad = std::max(1, slashColumn - int(text.size()));
    return text + QString(pad, QLatin1Char(' ')) + QLatin1Char('\\');
}

// The inverse: a macro's new final line sheds its continuation.
QString debugStripContinuation(QString text)
{
    const int slash = int(text.lastIndexOf(QLatin1Char('\\')));
    if (slash >= 0) {
        text.truncate(slash);
        while (!text.isEmpty() && text.endsWith(QLatin1Char(' ')))
            text.chop(1);
    }
    return text;
}

QString sidecarPath(const QString &projectRoot, const QString &label)
{
    return projectRoot + QStringLiteral("/.porydaw/") + label + QStringLiteral(".json");
}

// Raw-byte line model for the registration files, so an edit touches only
// the song's own line: every other line keeps its exact bytes (including
// any CR — these files are normally LF, but a CRLF checkout stays CRLF).
struct RawLines {
    QList<QByteArray> lines;
    bool endsWithNewline = true;
    bool crlf = false;
    bool loaded = false;
    bool dirty = false;

    QString text(int i) const
    {
        const QByteArray &line = lines.at(i);
        return QString::fromUtf8(line.endsWith('\r') ? line.chopped(1) : line);
    }
    void insert(int at, const QString &text)
    {
        QByteArray line = text.toUtf8();
        if (crlf)
            line += '\r';
        lines.insert(at, line);
        dirty = true;
    }
    void replace(int at, const QString &text)
    {
        QByteArray line = text.toUtf8();
        if (lines.at(at).endsWith('\r'))
            line += '\r';
        lines[at] = line;
        dirty = true;
    }
    void removeAt(int at)
    {
        lines.removeAt(at);
        dirty = true;
    }
    QStringList texts() const
    {
        QStringList all;
        all.reserve(lines.size());
        for (int i = 0; i < lines.size(); i++)
            all.append(text(i));
        return all;
    }
};

RawLines readRawLines(const QString &path)
{
    RawLines f;
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly))
        return f;
    const QByteArray content = in.readAll();
    f.loaded = true;
    f.endsWithNewline = content.isEmpty() || content.endsWith('\n');
    f.crlf = content.contains("\r\n");
    f.lines = content.split('\n');
    if (f.endsWithNewline && !f.lines.isEmpty())
        f.lines.removeLast(); // the empty piece after the final newline
    return f;
}

bool writeRawLines(const QString &path, const RawLines &f, QString *error)
{
    if (!f.dirty)
        return true;
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("Cannot write %1").arg(path);
        return false;
    }
    QByteArray joined = f.lines.join('\n');
    if (f.endsWithNewline)
        joined += '\n';
    out.write(joined);
    return true;
}

} // namespace

namespace SongRegistry {

QStringList voicegroupArgs(const QString &projectRoot)
{
    // The declaration regexes and file listing live with the voicegroup
    // catalog scan, which extracts all its datasets in the same read.
    return VoicegroupSource::catalogScan(projectRoot).groupArgs;
}

QString voicegroupDisplayName(const QString &arg)
{
    return arg.startsWith(QLatin1Char('_')) ? arg.mid(1) : arg;
}

QString voicegroupArgFromDisplay(const QString &text, const QStringList &knownArgs)
{
    if (text.isEmpty() || text.startsWith(QLatin1Char('_')))
        return text;
    if (!knownArgs.contains(QLatin1Char('_') + text) && knownArgs.contains(text))
        return text;
    return QLatin1Char('_') + text;
}

QVector<MusicPlayer> musicPlayers(const QString &projectRoot)
{
    // "	.equiv MUSIC_PLAYER_BGM,0"
    static const QRegularExpression equivRe(QStringLiteral(R"(^\s*\.equiv\s+(\w+)\s*,\s*(\d+))"));

    QVector<MusicPlayer> players;
    for (const QString &line : readLines(projectRoot + QStringLiteral("/sound/song_table.inc"))) {
        const QRegularExpressionMatch m = equivRe.match(line);
        if (m.hasMatch())
            players.append({m.captured(1), m.captured(2).toInt()});
    }
    if (players.isEmpty())
        players.append({QStringLiteral("MUSIC_PLAYER_BGM"), 0});

    // Track budgets from music_player_table.inc: ".equiv NUM_TRACKS_BGM, 10"
    // symbols, then the ordered music_player entries — gMPlayTable index n is
    // player number n (that is how m4aSongNumStart indexes the table).
    // "	music_player gMPlayInfo_BGM, gMPlayTrack_BGM, NUM_TRACKS_BGM, 0"
    static const QRegularExpression playerRe(
        QStringLiteral(R"(^\s*music_player\s+\w+\s*,\s*\w+\s*,\s*(\w+))"));
    QHash<QString, int> equivs;
    QVector<int> counts; // by table order
    for (const QString &line :
         readLines(projectRoot + QStringLiteral("/sound/music_player_table.inc"))) {
        const QRegularExpressionMatch eq = equivRe.match(line);
        if (eq.hasMatch()) {
            equivs.insert(eq.captured(1), eq.captured(2).toInt());
            continue;
        }
        const QRegularExpressionMatch mp = playerRe.match(line);
        if (!mp.hasMatch())
            continue;
        const QString arg = mp.captured(1);
        bool literal = false;
        int count = arg.toInt(&literal);
        if (!literal)
            count = equivs.value(arg, -1);
        // MPlayOpen clamps to MAX_MUSICPLAYER_TRACKS; a 0-track player never
        // opens at all, so 0 stays 0 (every track silent).
        counts.append(count < 0 ? -1 : std::min(count, 16));
    }
    for (MusicPlayer &p : players) {
        if (p.number >= 0 && p.number < counts.size())
            p.trackCount = counts[p.number];
    }
    return players;
}

QString constantForLabel(const QString &label)
{
    return label.toUpper();
}

SongNameConflicts checkNewSongNames(const QString &projectRoot, const QVector<SongInfo> &songs,
                                    const ReservedSongNames &reserved, const QString &label,
                                    const QString &constant, const QString &newVoicegroup)
{
    const auto tr = [](const char *text) {
        return QCoreApplication::translate("SongRegistry", text);
    };
    SongNameConflicts conflicts;
    if (!label.isEmpty()) {
        bool taken = reserved.labels.contains(label);
        for (const SongInfo &song : songs)
            taken = taken || song.label == label;
        if (!taken) {
            for (const QString &line :
                 readLines(projectRoot + QStringLiteral("/sound/song_table.inc"))) {
                const QRegularExpressionMatch m = songLineRe().match(line);
                if (m.hasMatch() && m.captured(2) == label) {
                    taken = true;
                    break;
                }
            }
        }
        if (taken)
            conflicts.label = tr("A song named %1 already exists.").arg(label);
        if (QFileInfo::exists(projectRoot + QStringLiteral("/sound/songs/midi/%1.mid").arg(label)))
            conflicts.mid = tr("%1.mid already exists.").arg(label);
    }
    if (!constant.isEmpty()) {
        // Any #define of the name, numeric or not: a second one breaks the
        // build either way.
        static const QRegularExpression defineRe(QStringLiteral(R"(^\s*#define\s+(\w+)\b)"));
        bool taken = reserved.constants.contains(constant);
        for (const SongInfo &song : songs)
            taken = taken || song.constant == constant;
        if (!taken) {
            for (const QString &line :
                 readLines(projectRoot + QStringLiteral("/include/constants/songs.h"))) {
                const QRegularExpressionMatch m = defineRe.match(line);
                if (m.hasMatch() && m.captured(1) == constant) {
                    taken = true;
                    break;
                }
            }
        }
        if (taken)
            conflicts.constant = tr("Another song already uses the constant %1.").arg(constant);
    }
    if (!newVoicegroup.isEmpty() &&
        (reserved.voicegroups.contains(newVoicegroup) ||
         QFileInfo::exists(projectRoot +
                           QStringLiteral("/sound/voicegroups/%1.inc").arg(newVoicegroup)))) {
        conflicts.voicegroup =
            tr("A voicegroup named voicegroup_%1 already exists.").arg(newVoicegroup);
    }
    return conflicts;
}

RegistrationPlan makePlan(const QString &projectRoot, const QString &label, const QString &constant,
                          const QString &player)
{
    RegistrationPlan plan;
    plan.label = label;
    plan.constant = constant;
    plan.player = player;

    // song_table.inc: match the existing entries' indentation; the third
    // argument mirrors the player's .equiv number.
    const SongTableScan scan =
        scanSongTable(readLines(projectRoot + QStringLiteral("/sound/song_table.inc")), label);
    const QString indent = scan.count > 0 ? scan.indent : QStringLiteral("\t");

    int playerNum = 0;
    for (const MusicPlayer &p : musicPlayers(projectRoot)) {
        if (p.name == player)
            playerNum = p.number;
    }
    plan.songTableLine =
        QStringLiteral("%1song %2, %3, %4").arg(indent, label, player).arg(playerNum);

    // songs.h: the file's value column for padding, the constant's own
    // current value — needed to settle which entry is THE song's when a
    // label owns several table indices — and the set of used values, which
    // tells a reusable placeholder slot from one some constant addresses.
    static const QRegularExpression defineRe(
        QStringLiteral(R"(^#define\s+([A-Z0-9_]+)(\s+)(\d+))"));
    const QStringList songsHLines =
        readLines(projectRoot + QStringLiteral("/include/constants/songs.h"));
    int valueColumn = 0;
    int ownValue = -1;
    QSet<int> usedValues;
    for (const QString &line : songsHLines) {
        const QRegularExpressionMatch m = defineRe.match(line);
        if (!m.hasMatch())
            continue;
        valueColumn = m.capturedEnd(2);
        if (ownValue < 0 && m.captured(1) == constant)
            ownValue = m.captured(3).toInt();
        if (!isRegionMarkerName(m.captured(1)))
            usedValues.insert(m.captured(3).toInt());
    }
    const QStringList debugLines = readLines(projectRoot + QStringLiteral("/src/debug.c"));
    const RegionMarkers markers = scanRegionMarkers(songsHLines, debugLines);

    // The proposed ID. An existing song keeps its identity: when the label
    // owns several table entries (forks alias real songs into filler slots
    // — pokezelda repeats ten labels), the define picking ANY of them is
    // already correct and stays; a define naming none of them drifted, and
    // heals to the label's first entry. A new song fills the lowest free
    // slot a deleted song left before growing the table. On a regioned
    // layout the region bounds all of that: an existing entry past END_MUS
    // is a mis-registration (an earlier porydaw appended it there, where
    // the marker-sized arrays cannot reach) and migrates into the region; a
    // free slot is reused only where the song's region can address it; a
    // new sound effect fills the placeholder gap between the SE region and
    // START_MUS; new music inserts at END_MUS + 1, shifting the phoneme
    // block up by one.
    const bool seRouted = constant.startsWith(QStringLiteral("SE_"));
    int settled = -1;
    if (!scan.labelIndices.isEmpty())
        settled = ownValue >= 0 && scan.labelIndices.contains(ownValue) ? ownValue
                                                                        : scan.labelIndices.first();
    if (settled >= 0 && !(markers.regioned && settled > markers.endMus.value)) {
        plan.songId = settled;
    } else {
        plan.migrateFromIndex = settled; // -1 for a new song
        // The SE region's last ID: END_SE when the layout still has the
        // marker; layouts that lost it but keep the gap (the night-music
        // line has only START_MUS/END_MUS) derive it as the highest define
        // below START_MUS — never grouping SE_* constants with MUS_*, and
        // never running END_MUS onto a sound effect, while the gap lasts.
        int seLast = markers.endSe.valid() ? markers.endSe.value : -1;
        if (seLast < 0 && markers.regioned && markers.startMus >= 0) {
            for (int v : usedValues) {
                if (v < markers.startMus)
                    seLast = std::max(seLast, v);
            }
        }
        const bool seRegioned = seRouted && seLast >= 0;
        const int musFloor = markers.startMus >= 0 ? markers.startMus
                             : seLast >= 0         ? seLast + 1
                                                   : 0;
        int freeAt = -1;
        bool freeInSeRegion = false;
        for (int idx : scan.freeIndices) {
            if (!markers.regioned) {
                freeAt = idx; // the lowest, as ever
                break;
            }
            if (seRegioned && idx >= 1 && idx <= seLast + 1 &&
                (markers.startMus < 0 || idx < markers.startMus)) {
                freeAt = idx;
                freeInSeRegion = true;
                break;
            }
            if (!seRegioned && idx >= musFloor && idx <= markers.endMus.value + 1) {
                freeAt = idx;
                break;
            }
        }
        // A placeholder row: below START_MUS, unaddressed by any constant,
        // and dummy-looking (vanilla repeats "dummy_song_header" through
        // the whole END_SE..START_MUS gap).
        const auto placeholderAt = [&](int idx) {
            if (idx <= 0 || idx >= scan.entryLabels.size())
                return false;
            if (markers.startMus >= 0 && idx >= markers.startMus)
                return false;
            if (usedValues.contains(idx))
                return false;
            const QString &rowLabel = scan.entryLabels.at(idx);
            return rowLabel.contains(QStringLiteral("dummy")) ||
                   (idx + 1 < scan.entryLabels.size() && scan.entryLabels.at(idx + 1) == rowLabel);
        };
        bool seRegion = false;
        if (freeAt >= 0) {
            plan.songId = freeAt;
            seRegion = freeInSeRegion;
        } else if (markers.regioned && seRegioned && placeholderAt(seLast + 1)) {
            plan.songId = seLast + 1;
            plan.tableReplaceIndex = plan.songId;
            seRegion = true;
        } else if (markers.regioned) {
            plan.songId = markers.endMus.value + 1;
            plan.tableInsertIndex = plan.songId;
        } else {
            plan.songId = scan.count;
        }
        if (markers.regioned) {
            if (plan.migrateFromIndex == plan.songId) {
                // The row already sits at the region's next slot — only the
                // marker went stale. Nothing structural to do.
                plan.migrateFromIndex = -1;
                plan.tableInsertIndex = -1;
                plan.tableReplaceIndex = -1;
            }
            plan.repointEndSe =
                seRegion && markers.endSe.valid() && plan.songId > markers.endSe.value;
            plan.repointEndMus = !seRegion && plan.songId > markers.endMus.value;
            plan.debugUseBgmList = seRouted && !seRegion && markers.separateDebugArrays;
            if (plan.tableInsertIndex >= 0 && plan.tableInsertIndex < scan.count) {
                plan.renumberFrom = plan.songId;
                plan.renumberBelow = plan.migrateFromIndex >= 0 ? plan.migrateFromIndex
                                                                : std::numeric_limits<int>::max();
            }
        }
    }

    QString define = QStringLiteral("#define ") + constant;
    const int pad = valueColumn - define.size();
    define += QString(std::max(1, pad), QLatin1Char(' '));
    plan.songsHLine = define + QString::number(plan.songId);

    // ld_script.ld: one line in the song_data section. A project whose ld
    // script never references per-song objects (modern-only forks) skips
    // this step entirely.
    const QStringList ldLines = readLines(projectRoot + QStringLiteral("/ld_script.ld"));
    QString ldIndent = QStringLiteral("        ");
    plan.ldApplicable = false;
    for (const QString &line : ldLines) {
        const int at = line.indexOf(QStringLiteral("sound/songs/midi/"));
        if (at < 0)
            continue;
        plan.ldApplicable = true;
        ldIndent = line.left(at);
    }
    if (plan.ldApplicable)
        plan.ldLine = QStringLiteral("%1sound/songs/midi/%2.o(.rodata);").arg(ldIndent, label);

    // charmap.txt: the sound section mirrors songs.h — each constant maps to
    // its song ID as two little-endian hex bytes, so text control codes like
    // {PLAY_BGM} can name songs. A charmap with no such entries (or no
    // charmap.txt at all) skips this file.
    const QSet<QString> songNames = songsHConstantNames(projectRoot);
    int equalsColumn = -1;
    bool columnAligned = true;
    plan.charmapApplicable = false;
    for (const QString &line : readLines(projectRoot + QStringLiteral("/charmap.txt"))) {
        const QRegularExpressionMatch m = charmapEntryRe().match(line);
        if (!m.hasMatch() || !songNames.contains(m.captured(1)))
            continue;
        plan.charmapApplicable = true;
        if (equalsColumn < 0)
            equalsColumn = m.capturedEnd(2);
        else if (m.capturedEnd(2) != equalsColumn)
            columnAligned = false;
    }
    if (plan.charmapApplicable) {
        // Vanilla emerald puts one space between name and "="; ruby and
        // firered pad "=" into a shared column. Follow the section's style.
        const int pad = columnAligned ? std::max(1, equalsColumn - int(constant.size())) : 1;
        plan.charmapLine = constant + QString(pad, QLatin1Char(' ')) + QStringLiteral("= ") +
                           charmapIdBytes(plan.songId);
    }

    // src/debug.c: the expansion debug menu's sound lists, one X-macro entry
    // per song. Projects without the file or the lists (vanilla) skip it.
    const DebugSoundScan debugScan = scanDebugSoundLists(debugLines);
    plan.debugApplicable = debugScan.applicable();
    if (plan.debugApplicable) {
        const DebugSoundList *style =
            debugStyleList(debugScan, debugTargetList(debugScan, constant, plan.debugUseBgmList));
        plan.debugLine = debugContinuation(debugEntryText(*style, constant), style->slashColumn);
    }
    return plan;
}

bool registerSong(const QString &projectRoot, const QString &label, const QString &constant,
                  const QString &player, QString *error, int *songId)
{
    const RegistrationPlan plan = makePlan(projectRoot, label, constant, player);
    if (songId)
        *songId = plan.songId;

    // song_table.inc: fill a free slot left by a deleted song, or insert
    // after the last entry — either way the new entry's index is exactly
    // plan.songId (makePlan chose the ID from the same scan). On regioned
    // layouts the plan may instead overwrite a placeholder row, insert
    // mid-table ahead of the phoneme block, or first remove a misplaced
    // row it is migrating into the region (always a later index than the
    // insertion point, so the removal never shifts it).
    {
        const QString path = projectRoot + QStringLiteral("/sound/song_table.inc");
        RawLines f = readRawLines(path);
        if (!f.loaded) {
            if (error)
                *error = QStringLiteral("Cannot read %1").arg(path);
            return false;
        }
        const SongTableScan scan = scanSongTable(f.texts(), label);
        if (scan.labelLine < 0 || plan.migrateFromIndex >= 0) {
            if (scan.lastSongLine < 0) {
                if (error)
                    *error = QStringLiteral("%1 has no song entries").arg(path);
                return false;
            }
            if (plan.migrateFromIndex >= 0 && plan.migrateFromIndex < scan.entryLines.size())
                f.removeAt(scan.entryLines.at(plan.migrateFromIndex));
            if (plan.tableReplaceIndex >= 0)
                f.replace(scan.entryLines.at(plan.tableReplaceIndex), plan.songTableLine);
            else if (plan.tableInsertIndex >= 0 && plan.tableInsertIndex < scan.entryLines.size())
                f.insert(scan.entryLines.at(plan.tableInsertIndex), plan.songTableLine);
            else if (plan.tableInsertIndex < 0 && plan.songId < scan.entryLines.size())
                f.replace(scan.entryLines.at(plan.songId), plan.songTableLine); // free slot
            else
                f.insert(scan.lastSongLine + 1, plan.songTableLine);
        }
        if (!writeRawLines(path, f, error))
            return false;
    }

    // songs.h: insert the define at its position in the file's ID order —
    // after the last define with a smaller value — or correct an existing
    // define whose value drifted from the table index. Appended songs land
    // after the last define as before (their ID exceeds every value); a song
    // reusing a freed slot lands between its ID neighbors, keeping the file
    // sorted like the charmap insertion below. Only whole decimal values
    // count as IDs: the hex sentinels after the real entries (MUS_NONE
    // 0xFFFF, PHONEME_ID_NONE 0xFF) would otherwise read as value 0 and
    // drag every insertion past them to the end of the file.
    {
        const QString path = projectRoot + QStringLiteral("/include/constants/songs.h");
        RawLines f = readRawLines(path);
        if (!f.loaded) {
            if (error)
                *error = QStringLiteral("Cannot read %1").arg(path);
            return false;
        }
        const QRegularExpression ownRe(
            QStringLiteral(R"(^(\s*#define\s+%1\s+)(\d+)(.*)$)").arg(constant));
        static const QRegularExpression anyDefineRe(
            QStringLiteral(R"(^(\s*#define\s+(\w+)\s+)(\d+)\b(.*)$)"));
        static const QRegularExpression markerLineRe(
            QStringLiteral(R"(^(\s*#define\s+(END_SE|END_MUS)\s+)([A-Za-z_]\w*|\d+)(.*)$)"));
        static const QRegularExpression endifRe(QStringLiteral(R"(^\s*#endif\b)"));
        int own = -1, insertAfter = -1, firstDefine = -1, firstEndif = -1;
        QRegularExpressionMatch ownMatch;
        for (int i = 0; i < f.lines.size(); i++) {
            const QString text = f.text(i);
            const QRegularExpressionMatch m = ownRe.match(text);
            if (m.hasMatch() && own < 0) {
                own = i;
                ownMatch = m;
            }
            // The regioned marker whose region the song extends follows it
            // — in the marker's own form: alias markers name the constant,
            // value markers take the new ID.
            const QRegularExpressionMatch mk = markerLineRe.match(text);
            if (mk.hasMatch() &&
                ((plan.repointEndSe && mk.captured(2) == QStringLiteral("END_SE")) ||
                 (plan.repointEndMus && mk.captured(2) == QStringLiteral("END_MUS")))) {
                bool numeric = false;
                mk.captured(3).toInt(&numeric);
                const QString follow = numeric ? QString::number(plan.songId) : constant;
                if (mk.captured(3) != follow)
                    f.replace(i, mk.captured(1) + follow + mk.captured(4));
                continue;
            }
            const QRegularExpressionMatch d = anyDefineRe.match(text);
            if (!d.hasMatch() || isRegionMarkerName(d.captured(2))) {
                if (firstEndif < 0 && endifRe.match(text).hasMatch())
                    firstEndif = i;
                continue;
            }
            if (firstDefine < 0)
                firstDefine = i;
            const int value = d.captured(3).toInt();
            if (value < plan.songId)
                insertAfter = i;
            // Regioned insertions displace the phoneme block: every define
            // in the displaced index range shifts up with its table row.
            if (plan.renumberFrom >= 0 && i != own && value >= plan.renumberFrom &&
                value < plan.renumberBelow)
                f.replace(i, d.captured(1) + QString::number(value + 1) + d.captured(4));
        }
        // A migrated song's define moves to its ID-order position like a
        // fresh insert; a merely drifted one is corrected in place.
        const bool moveOwn =
            own >= 0 && plan.migrateFromIndex >= 0 && ownMatch.captured(2).toInt() != plan.songId;
        if (own >= 0 && !moveOwn) {
            if (ownMatch.captured(2).toInt() != plan.songId)
                f.replace(own, ownMatch.captured(1) + QString::number(plan.songId) +
                                   ownMatch.captured(3));
        } else {
            int at;
            if (insertAfter >= 0)
                at = insertAfter + 1;
            else if (firstDefine >= 0)
                at = firstDefine;
            else if (firstEndif >= 0)
                at = firstEndif;
            else
                at = int(f.lines.size());
            if (moveOwn) {
                f.removeAt(own);
                if (own < at)
                    at--;
            }
            f.insert(at, plan.songsHLine);
        }
        if (!writeRawLines(path, f, error))
            return false;
    }

    // ld_script.ld: one object line after the last per-song line. Projects
    // whose linker script has no per-song lines skip this file.
    if (plan.ldApplicable) {
        const QString path = projectRoot + QStringLiteral("/ld_script.ld");
        RawLines f = readRawLines(path);
        if (!f.loaded) {
            if (error)
                *error = QStringLiteral("Cannot read %1").arg(path);
            return false;
        }
        const QString needle = QStringLiteral("sound/songs/midi/%1.o").arg(label);
        int lastObject = -1;
        bool present = false;
        for (int i = 0; i < f.lines.size(); i++) {
            const QString text = f.text(i);
            if (!text.contains(QStringLiteral("sound/songs/midi/")))
                continue;
            lastObject = i;
            if (text.contains(needle))
                present = true;
        }
        if (!present)
            f.insert(lastObject + 1, plan.ldLine);
        if (!writeRawLines(path, f, error))
            return false;
    }

    // charmap.txt: insert the ID mapping at its position in the sound
    // section's ID order (a backfilled song lands between its neighbors, not
    // appended), or correct an existing entry whose bytes drifted from the
    // table index. Projects whose charmap has no song entries skip this file.
    if (plan.charmapApplicable) {
        const QString path = projectRoot + QStringLiteral("/charmap.txt");
        RawLines f = readRawLines(path);
        if (!f.loaded) {
            if (error)
                *error = QStringLiteral("Cannot read %1").arg(path);
            return false;
        }
        // songs.h was just written, so the set includes this song's constant.
        const QSet<QString> songNames = songsHConstantNames(projectRoot);
        const QRegularExpression ownAnyRe(QStringLiteral(R"(^\s*%1\s*=)").arg(constant));
        int own = -1, insertAfter = -1, firstEntry = -1;
        bool ownAnyForm = false;
        QRegularExpressionMatch ownMatch;
        for (int i = 0; i < f.lines.size(); i++) {
            const QString text = f.text(i);
            // An entry in any shape (extra bytes, trailing comment) still
            // counts as present — never insert a duplicate; but only the
            // standard two-byte form is corrected.
            if (ownAnyRe.match(text).hasMatch())
                ownAnyForm = true;
            const QRegularExpressionMatch m = charmapEntryRe().match(text);
            if (!m.hasMatch() || !songNames.contains(m.captured(1)))
                continue;
            if (m.captured(1) == constant && own < 0) {
                own = i;
                ownMatch = m;
            }
            if (firstEntry < 0)
                firstEntry = i;
            const int value = m.captured(3).toInt(nullptr, 16) | m.captured(4).toInt(nullptr, 16)
                                                                     << 8;
            if (value < plan.songId)
                insertAfter = i;
            // The charmap mirrors songs.h: entries displaced by a regioned
            // insertion shift up with their defines.
            if (plan.renumberFrom >= 0 && m.captured(1) != constant && value >= plan.renumberFrom &&
                value < plan.renumberBelow)
                f.replace(i, text.left(m.capturedStart(3)) + charmapIdBytes(value + 1));
        }
        // Like the songs.h leg: a migrated song's entry moves to its ID
        // position, a merely drifted one is corrected in place.
        const bool moveOwn = own >= 0 && plan.migrateFromIndex >= 0;
        if (own >= 0 && !moveOwn) {
            const int value = ownMatch.captured(3).toInt(nullptr, 16) |
                              ownMatch.captured(4).toInt(nullptr, 16) << 8;
            if (value != plan.songId)
                f.replace(own, f.text(own).left(ownMatch.capturedStart(3)) +
                                   charmapIdBytes(plan.songId));
        } else if (moveOwn || !ownAnyForm) {
            // After the last entry with a smaller ID; a song whose ID
            // precedes every existing entry goes before the first one.
            int at = -1;
            if (insertAfter >= 0)
                at = insertAfter + 1;
            else if (firstEntry >= 0)
                at = firstEntry;
            if (moveOwn) {
                f.removeAt(own);
                if (at > own)
                    at--;
            }
            if (at >= 0)
                f.insert(at, plan.charmapLine);
        }
        if (!writeRawLines(path, f, error))
            return false;
    }

    // src/debug.c: the X-macro entry at its position in the target list's ID
    // order (by the songs.h values just written) — SE_* constants under
    // SOUND_LIST_SE, everything else under SOUND_LIST_BGM. An entry already
    // present in either list stays put; only the sound lists are touched,
    // never other X macros in the file. The macro's '\' continuations are
    // rewired as needed: a new final line arrives bare and hands the old
    // final line a continuation.
    if (plan.debugApplicable) {
        const QString path = projectRoot + QStringLiteral("/src/debug.c");
        RawLines f = readRawLines(path);
        if (!f.loaded) {
            if (error)
                *error = QStringLiteral("Cannot read %1").arg(path);
            return false;
        }
        const DebugSoundScan scan = scanDebugSoundLists(f.texts());
        bool present = false;
        for (const DebugSoundList &list : scan.lists)
            present = present || list.entryNames.contains(constant);
        const DebugSoundList *target = debugTargetList(scan, constant, plan.debugUseBgmList);
        if (!present && target) {
            static const QRegularExpression defineValueRe(
                QStringLiteral(R"(^\s*#define\s+(\w+)\s+(\d+)\b)"));
            QHash<QString, int> ids;
            for (const QString &line :
                 readLines(projectRoot + QStringLiteral("/include/constants/songs.h"))) {
                const QRegularExpressionMatch m = defineValueRe.match(line);
                if (m.hasMatch() && !ids.contains(m.captured(1)))
                    ids.insert(m.captured(1), m.captured(2).toInt());
            }
            // After the last entry with a known smaller ID; before the first
            // entry when the ID precedes them all; alone after the #define
            // when the list is empty.
            int insertAfter = -1;
            for (int e = 0; e < target->entryNames.size(); e++) {
                const auto it = ids.constFind(target->entryNames.at(e));
                if (it != ids.constEnd() && it.value() < plan.songId)
                    insertAfter = e;
            }
            int at;
            if (insertAfter >= 0)
                at = target->entryLines.at(insertAfter) + 1;
            else if (!target->entryLines.isEmpty())
                at = target->entryLines.first();
            else
                at = target->defineLine + 1;
            const DebugSoundList *style = debugStyleList(scan, target);
            const QString bare = debugEntryText(*style, constant);
            if (continuesMacro(f.text(at - 1))) {
                // Splicing between two macro lines: the new line continues.
                f.insert(at, debugContinuation(bare, style->slashColumn));
            } else {
                // The line above was the macro's end; the new line takes over
                // as the bare final line and hands it a continuation.
                f.replace(at - 1, debugContinuation(f.text(at - 1), style->slashColumn));
                f.insert(at, bare);
            }
        }
        if (!writeRawLines(path, f, error))
            return false;
    }
    return true;
}

RemovalPlan makeRemovalPlan(const QString &projectRoot, const QString &label,
                            const QString &constant)
{
    RemovalPlan plan;
    const SongTableScan scan =
        scanSongTable(readLines(projectRoot + QStringLiteral("/sound/song_table.inc")), label);
    plan.tableIndex = scan.labelIndex;
    plan.tableCount = scan.count;
    plan.lastEntry = scan.labelLine >= 0 && scan.labelLine == scan.lastSongLine;

    const QRegularExpression defineRe(QStringLiteral(R"(^\s*#define\s+%1\s+\d)").arg(constant));
    for (const QString &line :
         readLines(projectRoot + QStringLiteral("/include/constants/songs.h"))) {
        if (defineRe.match(line).hasMatch())
            plan.inSongsH = true;
    }
    const QString needle = QStringLiteral("sound/songs/midi/%1.o").arg(label);
    for (const QString &line : readLines(projectRoot + QStringLiteral("/ld_script.ld"))) {
        if (line.contains(needle))
            plan.inLdScript = true;
    }
    for (const QString &line : readLines(projectRoot + QStringLiteral("/charmap.txt"))) {
        const QRegularExpressionMatch m = charmapEntryRe().match(line);
        if (m.hasMatch() && m.captured(1) == constant)
            plan.inCharmap = true;
    }
    const DebugSoundScan debugScan =
        scanDebugSoundLists(readLines(projectRoot + QStringLiteral("/src/debug.c")));
    for (const DebugSoundList &list : debugScan.lists)
        plan.inDebugMenu = plan.inDebugMenu || list.entryNames.contains(constant);
    return plan;
}

bool unregisterSong(const QString &projectRoot, const QString &label, const QString &constant,
                    QString *error)
{
    // song_table.inc first: the index-0 refusal must precede any edit, and
    // the other files' entries are meaningless once the table entry is gone.
    {
        const QString path = projectRoot + QStringLiteral("/sound/song_table.inc");
        RawLines f = readRawLines(path);
        if (f.loaded) {
            const SongTableScan scan = scanSongTable(f.texts(), label);
            if (scan.labelIndex == 0) {
                if (error)
                    *error = QStringLiteral("%1 is the first song_table.inc entry (song ID "
                                            "0), the engine's fallback song — it cannot be "
                                            "deleted.")
                                 .arg(label);
                return false;
            }
            if (scan.labelLine >= 0 && scan.labelLine == scan.lastSongLine) {
                f.removeAt(scan.labelLine);
                // Free slots only hold their index for the songs after them;
                // any now left trailing can go too (never entry 0 itself).
                for (;;) {
                    const SongTableScan tail = scanSongTable(f.texts(), label);
                    if (tail.count <= 1 || tail.lastSongLabel != tail.firstLabel)
                        break;
                    f.removeAt(tail.lastSongLine);
                }
            } else if (scan.labelLine >= 0) {
                // Mid-table: a free slot — a duplicate of entry 0's dummy
                // line — keeps every later song's ID and the table buildable.
                f.replace(scan.labelLine, QStringLiteral("%1song %2, %3, %4")
                                              .arg(scan.labelIndent, scan.firstLabel,
                                                   scan.firstPlayer, scan.firstPlayerNum));
            }
            if (!writeRawLines(path, f, error))
                return false;
        }
    }

    // songs.h: drop the constant's define, whatever value it drifted to.
    // An END_SE / END_MUS marker naming the deleted constant (alias form)
    // or its ID (value form) follows the region's new last song — the
    // numeric define with the highest value below the deleted one —
    // instead of dangling (on regioned layouts a dangling or stale marker
    // breaks the arrays sized from it).
    {
        const QString path = projectRoot + QStringLiteral("/include/constants/songs.h");
        RawLines f = readRawLines(path);
        if (f.loaded) {
            const QRegularExpression ownRe(
                QStringLiteral(R"(^\s*#define\s+%1\s+(\d+)\b)").arg(constant));
            static const QRegularExpression anyDefineRe(
                QStringLiteral(R"(^\s*#define\s+(\w+)\s+(\d+)\b)"));
            static const QRegularExpression markerLineRe(
                QStringLiteral(R"(^(\s*#define\s+(?:END_SE|END_MUS)\s+)([A-Za-z_]\w*|\d+)(.*)$)"));
            int own = -1, ownValue = -1;
            QVector<int> markerLines;
            QVector<QPair<QString, int>> defines; // name, value — in file order
            for (int i = 0; i < f.lines.size(); i++) {
                const QString text = f.text(i);
                if (own < 0) {
                    const QRegularExpressionMatch m = ownRe.match(text);
                    if (m.hasMatch()) {
                        own = i;
                        ownValue = m.captured(1).toInt();
                        continue;
                    }
                }
                if (markerLineRe.match(text).hasMatch()) {
                    markerLines.append(i);
                    continue;
                }
                const QRegularExpressionMatch d = anyDefineRe.match(text);
                if (d.hasMatch() && d.captured(1) != constant && !isRegionMarkerName(d.captured(1)))
                    defines.append({d.captured(1), d.captured(2).toInt()});
            }
            if (own >= 0) {
                QString bestName;
                int bestValue = -1;
                for (const auto &d : defines) {
                    if (d.second < ownValue && d.second > bestValue) {
                        bestValue = d.second;
                        bestName = d.first;
                    }
                }
                for (int line : markerLines) {
                    const QRegularExpressionMatch mk = markerLineRe.match(f.text(line));
                    bool numeric = false;
                    const int markerValue = mk.captured(2).toInt(&numeric);
                    // Only a marker on the deleted song moves, and only
                    // when something is left below to point at.
                    if (numeric ? (markerValue != ownValue || bestValue < 0)
                                : (mk.captured(2) != constant || bestName.isEmpty()))
                        continue;
                    f.replace(line, mk.captured(1) +
                                        (numeric ? QString::number(bestValue) : bestName) +
                                        mk.captured(3));
                }
                f.removeAt(own);
            }
            if (!writeRawLines(path, f, error))
                return false;
        }
    }

    // ld_script.ld: the song's object line.
    {
        const QString path = projectRoot + QStringLiteral("/ld_script.ld");
        RawLines f = readRawLines(path);
        if (f.loaded) {
            const QString needle = QStringLiteral("sound/songs/midi/%1.o").arg(label);
            for (int i = 0; i < f.lines.size(); i++) {
                if (f.text(i).contains(needle)) {
                    f.removeAt(i);
                    break;
                }
            }
            if (!writeRawLines(path, f, error))
                return false;
        }
    }

    // charmap.txt: the constant's ID mapping.
    {
        const QString path = projectRoot + QStringLiteral("/charmap.txt");
        RawLines f = readRawLines(path);
        if (f.loaded) {
            for (int i = 0; i < f.lines.size(); i++) {
                const QRegularExpressionMatch m = charmapEntryRe().match(f.text(i));
                if (m.hasMatch() && m.captured(1) == constant) {
                    f.removeAt(i);
                    break;
                }
            }
            if (!writeRawLines(path, f, error))
                return false;
        }
    }

    // src/debug.c: the constant's X-macro entry, from whichever sound list
    // carries it. Removing a macro's final line promotes the line above —
    // an entry, or the #define itself when the list empties — to macro end,
    // shedding its '\' continuation.
    {
        const QString path = projectRoot + QStringLiteral("/src/debug.c");
        RawLines f = readRawLines(path);
        if (f.loaded) {
            const DebugSoundScan scan = scanDebugSoundLists(f.texts());
            for (const DebugSoundList &list : scan.lists) {
                const int e = int(list.entryNames.indexOf(constant));
                if (e < 0)
                    continue;
                const int line = list.entryLines.at(e);
                if (!continuesMacro(f.text(line)))
                    f.replace(line - 1, debugStripContinuation(f.text(line - 1)));
                f.removeAt(line);
                break;
            }
            if (!writeRawLines(path, f, error))
                return false;
        }
    }
    return true;
}

QString deletableVoicegroup(const QString &projectRoot, const QVector<SongInfo> &songs,
                            const QString &songLabel)
{
    const SongInfo *song = nullptr;
    for (const SongInfo &s : songs) {
        if (s.label == songLabel)
            song = &s;
    }
    if (!song)
        return {};
    const QString arg = song->cfg.voicegroupArg;
    if (arg.isEmpty())
        return {}; // mid2agb's default voicegroup_dummy — shared by definition
    for (const SongInfo &s : songs) {
        if (s.label != songLabel && s.cfg.voicegroupArg == arg)
            return {};
    }

    // A file of its own under sound/voicegroups/ — the only layout whose
    // deletion is one file plus one .include line.
    QString name;
    const QDir vgDir(projectRoot + QStringLiteral("/sound/voicegroups"));
    for (const QString &candidate : DecompProject::voicegroupCandidates(song->cfg)) {
        if (vgDir.exists(candidate + QStringLiteral(".inc"))) {
            name = candidate;
            break;
        }
    }
    if (name.isEmpty())
        return {};

    // Not a keysplit/drumkit sub-group of another voicegroup.
    const QString symbol = QStringLiteral("voicegroup") + arg;
    const VgCatalogScan catalog = VoicegroupSource::catalogScan(projectRoot);
    for (const auto &keysplit : catalog.keysplits) {
        if (keysplit.first == symbol)
            return {};
    }
    if (catalog.drumkits.contains(symbol))
        return {};

    // Not referenced from the project's C sources — such a reference would
    // break the link outright, not merely dangle.
    const QByteArray symbolBytes = symbol.toUtf8();
    const auto isIdentChar = [](char c) {
        return c == '_' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
               (c >= 'a' && c <= 'z');
    };
    for (const QString &top : {QStringLiteral("/src"), QStringLiteral("/include")}) {
        QDirIterator it(projectRoot + top, {QStringLiteral("*.c"), QStringLiteral("*.h")},
                        QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QFile file(it.next());
            if (!file.open(QIODevice::ReadOnly))
                continue;
            const QByteArray content = file.readAll();
            for (qsizetype at = content.indexOf(symbolBytes); at >= 0;
                 at = content.indexOf(symbolBytes, at + 1)) {
                const qsizetype end = at + symbolBytes.size();
                const bool startsClean = at == 0 || !isIdentChar(content.at(at - 1));
                if (startsClean && (end >= content.size() || !isIdentChar(content.at(end))))
                    return {};
            }
        }
    }
    return name;
}

RegistrationStatus checkRegistration(const QString &projectRoot, const QString &label,
                                     const QString &constant)
{
    SongInfo song;
    song.label = label;
    song.constant = constant;
    return checkRegistrations(projectRoot, {song}).value(label);
}

QStringList registrationGaps(const RegistrationStatus &status)
{
    QStringList gaps;
    if (!status.inSongTable)
        gaps.append(QStringLiteral("song_table.inc"));
    if (!status.inSongsH)
        gaps.append(QStringLiteral("songs.h"));
    if (status.ldApplicable && !status.inLdScript)
        gaps.append(QStringLiteral("ld_script.ld"));
    if (status.charmapApplicable && !status.inCharmap)
        gaps.append(QStringLiteral("charmap.txt"));
    if (status.debugApplicable && !status.inDebugMenu)
        gaps.append(QStringLiteral("src/debug.c"));
    return gaps;
}

QHash<QString, RegistrationStatus> checkRegistrations(const QString &projectRoot,
                                                      const QVector<SongInfo> &songs)
{
    // song_table.inc: label -> every index bearing it. Forks fill new table
    // slots with copies of real songs (vanilla uses a dummy label), so one
    // label can legitimately own several indices — a define naming any of
    // them is correctly registered. Free slots — entries past index 0
    // bearing entry 0's label — occupy their index but don't count as the
    // fallback song's entries, whose own index must stay 0.
    QHash<QString, QVector<int>> tableIndices;
    int count = 0;
    QString firstLabel;
    for (const QString &line : readLines(projectRoot + QStringLiteral("/sound/song_table.inc"))) {
        const QRegularExpressionMatch m = songLineRe().match(line);
        if (!m.hasMatch())
            continue;
        if (count == 0)
            firstLabel = m.captured(2);
        if (count == 0 || m.captured(2) != firstLabel)
            tableIndices[m.captured(2)].append(count);
        count++;
    }

    // songs.h: name -> first numeric define's value.
    static const QRegularExpression defineRe(QStringLiteral(R"(^\s*#define\s+(\w+)\s+(\d+))"));
    const QStringList songsHLines =
        readLines(projectRoot + QStringLiteral("/include/constants/songs.h"));
    QHash<QString, int> defines;
    for (const QString &line : songsHLines) {
        const QRegularExpressionMatch m = defineRe.match(line);
        if (m.hasMatch() && !defines.contains(m.captured(1)))
            defines.insert(m.captured(1), m.captured(2).toInt());
    }

    // ld_script.ld: the per-song object labels.
    static const QRegularExpression ldObjectRe(QStringLiteral(R"(sound/songs/midi/(\w+)\.o)"));
    bool ldApplicable = false;
    QSet<QString> ldLabels;
    for (const QString &line : readLines(projectRoot + QStringLiteral("/ld_script.ld"))) {
        if (line.contains(QStringLiteral("sound/songs/midi/")))
            ldApplicable = true;
        QRegularExpressionMatchIterator it = ldObjectRe.globalMatch(line);
        while (it.hasNext())
            ldLabels.insert(it.next().captured(1));
    }

    // charmap.txt: name -> first entry's decoded value; the file is a song
    // section at all only when some entry is named by a songs.h constant.
    QHash<QString, int> charmapValues;
    bool charmapApplicable = false;
    for (const QString &line : readLines(projectRoot + QStringLiteral("/charmap.txt"))) {
        const QRegularExpressionMatch m = charmapEntryRe().match(line);
        if (!m.hasMatch())
            continue;
        if (defines.contains(m.captured(1)))
            charmapApplicable = true;
        if (!charmapValues.contains(m.captured(1)))
            charmapValues.insert(m.captured(1), m.captured(3).toInt(nullptr, 16) |
                                                    m.captured(4).toInt(nullptr, 16) << 8);
    }

    // src/debug.c: the debug menu's sound lists name songs by constant alone
    // (no ID to drift), so presence is the whole check.
    const QStringList debugLines = readLines(projectRoot + QStringLiteral("/src/debug.c"));
    const DebugSoundScan debugScan = scanDebugSoundLists(debugLines);
    const RegionMarkers markers = scanRegionMarkers(songsHLines, debugLines);
    QSet<QString> debugNames;
    for (const DebugSoundList &list : debugScan.lists)
        for (const QString &name : list.entryNames)
            debugNames.insert(name);

    QHash<QString, RegistrationStatus> statuses;
    for (const SongInfo &song : songs) {
        const QString constant =
            song.constant.isEmpty() ? constantForLabel(song.label) : song.constant;
        RegistrationStatus status;
        const QVector<int> indices = tableIndices.value(song.label);
        status.inSongTable = !indices.isEmpty();
        // Once a table entry exists, the songs.h define and the charmap
        // entry must carry one of the label's real indices. On a regioned
        // layout an ID past END_MUS is a mis-registration even when the
        // table agrees — src/debug.c's marker-sized arrays cannot hold it
        // and the build breaks — so the song reads as needing (re-)
        // registration, which migrates it into the region.
        const auto define = defines.constFind(constant);
        if (define != defines.constEnd()) {
            status.inSongsH = indices.isEmpty() || indices.contains(define.value());
            if (markers.regioned && define.value() > markers.endMus.value)
                status.inSongsH = false;
        }
        status.ldApplicable = ldApplicable;
        status.inLdScript = ldLabels.contains(song.label);
        status.charmapApplicable = charmapApplicable;
        const auto charmapValue = charmapValues.constFind(constant);
        if (charmapValue != charmapValues.constEnd())
            status.inCharmap = indices.isEmpty() || indices.contains(charmapValue.value());
        status.debugApplicable = debugScan.applicable();
        status.inDebugMenu = debugNames.contains(constant);
        statuses.insert(song.label, status);
    }
    return statuses;
}

QStringList mergeCfgFlags(const SongCfg &cfg)
{
    // Updates or inserts "-<letter><value>"; a null value removes the flag.
    // Matching is case-insensitive as in mid2agb's parser.
    QStringList flags = cfg.rawFlags;
    const auto setValue = [&flags](char letter, const QString &value) {
        for (int i = 0; i < flags.size(); i++) {
            if (flags[i].size() >= 2 && flags[i][0] == QLatin1Char('-') &&
                flags[i][1].toUpper() == QLatin1Char(letter)) {
                if (value.isNull())
                    flags.removeAt(i);
                else
                    flags[i] = QLatin1Char('-') + QString(QLatin1Char(letter)) + value;
                return;
            }
        }
        if (!value.isNull())
            flags.append(QLatin1Char('-') + QString(QLatin1Char(letter)) + value);
    };
    const auto setBool = [&setValue](char letter, bool present) {
        setValue(letter, present ? QStringLiteral("") : QString());
    };

    setBool('E', cfg.exactGate);
    setValue('R', cfg.reverb >= 0 ? QString::number(cfg.reverb) : QString());
    setValue('G', cfg.voicegroupArg.isEmpty() ? QString() : cfg.voicegroupArg);
    setValue('V', QStringLiteral("%1").arg(cfg.masterVolume, 3, 10, QLatin1Char('0')));
    setValue('P', cfg.priority != 0 ? QString::number(cfg.priority) : QString());
    setBool('X', cfg.extendedClocks);
    setBool('N', cfg.noCompression);
    return flags;
}

bool writeMidiCfgLine(const QString &midiDir, const QString &label, const QStringList &flags,
                      QString *error)
{
    // Binary in/out: only the song's own line may change, byte for byte —
    // including CRLF line endings (vanilla midi.cfg uses them).
    const QString cfgPath = QDir(midiDir).filePath(QStringLiteral("midi.cfg"));
    QByteArray content;
    {
        QFile in(cfgPath);
        if (in.open(QIODevice::ReadOnly))
            content = in.readAll();
    }
    const bool endsWithNewline = content.isEmpty() || content.endsWith('\n');
    const bool crlf = content.contains("\r\n");
    QList<QByteArray> lines = content.split('\n');
    if (endsWithNewline && !lines.isEmpty())
        lines.removeLast(); // the empty piece after the final newline

    const QString fileName = label + QStringLiteral(".mid");
    const QByteArray flagBytes = flags.join(QLatin1Char(' ')).toUtf8();
    bool replaced = false;
    for (QByteArray &line : lines) {
        const bool hadCr = line.endsWith('\r');
        const QString text = QString::fromUtf8(hadCr ? line.chopped(1) : line);
        const int colon = text.indexOf(QLatin1Char(':'));
        if (colon <= 0 || text.left(colon).trimmed() != fileName)
            continue;
        // Keep the original name-column padding.
        int flagStart = colon + 1;
        while (flagStart < text.size() && text[flagStart] == QLatin1Char(' '))
            flagStart++;
        line = text.left(flagStart).toUtf8() + flagBytes;
        if (hadCr)
            line += '\r';
        replaced = true;
        break;
    }
    if (!replaced) {
        QByteArray line = fileName.toUtf8() + ": " + flagBytes;
        if (crlf)
            line += '\r';
        lines.append(line);
    }

    QFile out(cfgPath);
    if (!out.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("Cannot write %1").arg(cfgPath);
        return false;
    }
    QByteArray joined = lines.join('\n');
    if (endsWithNewline)
        joined += '\n';
    out.write(joined);
    return true;
}

bool writeSongFlags(const QString &midiDir, const QString &label, const QStringList &flags,
                    QString *error)
{
    if (!QFile::exists(QDir(midiDir).filePath(QStringLiteral("midi.cfg")))) {
        const QString mkPath =
            SongsMk::path(QDir::cleanPath(midiDir + QStringLiteral("/../../..")));
        if (QFile::exists(mkPath))
            return SongsMk::writeRule(mkPath, label, flags, error);
    }
    return writeMidiCfgLine(midiDir, label, flags, error);
}

static bool removeMidiCfgLine(const QString &midiDir, const QString &label, QString *error)
{
    // The exact inverse of writeMidiCfgLine's append: the song's own line
    // vanishes, every other byte stays.
    const QString cfgPath = QDir(midiDir).filePath(QStringLiteral("midi.cfg"));
    QByteArray content;
    {
        QFile in(cfgPath);
        if (!in.open(QIODevice::ReadOnly))
            return true; // no midi.cfg — nothing stored here
        content = in.readAll();
    }
    const bool endsWithNewline = content.isEmpty() || content.endsWith('\n');
    QList<QByteArray> lines = content.split('\n');
    if (endsWithNewline && !lines.isEmpty())
        lines.removeLast(); // the empty piece after the final newline

    const QString fileName = label + QStringLiteral(".mid");
    bool removed = false;
    for (int i = 0; i < lines.size(); i++) {
        const QByteArray &line = lines.at(i);
        const QString text = QString::fromUtf8(line.endsWith('\r') ? line.chopped(1) : line);
        const int colon = text.indexOf(QLatin1Char(':'));
        if (colon <= 0 || text.left(colon).trimmed() != fileName)
            continue;
        lines.removeAt(i);
        removed = true;
        break;
    }
    if (!removed)
        return true;

    QFile out(cfgPath);
    if (!out.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("Cannot write %1").arg(cfgPath);
        return false;
    }
    QByteArray joined = lines.join('\n');
    if (endsWithNewline)
        joined += '\n';
    out.write(joined);
    return true;
}

bool removeSongFlags(const QString &midiDir, const QString &label, QString *error)
{
    if (!removeMidiCfgLine(midiDir, label, error))
        return false;
    const QString mkPath = SongsMk::path(QDir::cleanPath(midiDir + QStringLiteral("/../../..")));
    if (QFile::exists(mkPath))
        return SongsMk::removeRule(mkPath, label, error);
    return true;
}

void removeSongSidecar(const QString &projectRoot, const QString &label)
{
    QFile::remove(sidecarPath(projectRoot, label));
}

SmfFile blankSong()
{
    SmfFile smf;
    smf.format = 1;
    smf.division = 24; // vanilla pokeemerald resolution: 1 tick per m4a clock
    const uint64_t oneBar = uint64_t(smf.division) * 4;

    SmfTrack seq; // MTrk chunk 0: the only chunk mid2agb reads seq events from
    SmfEvent tempo;
    tempo.status = 0xFF;
    tempo.metaType = 0x51;
    tempo.blob = QByteArray("\x07\xA1\x20", 3); // 500000 us/beat = 120 BPM
    seq.events.push_back(tempo);
    SmfEvent timeSig;
    timeSig.status = 0xFF;
    timeSig.metaType = 0x58;
    timeSig.blob = QByteArray("\x04\x02\x18\x08", 4); // 4/4
    seq.events.push_back(timeSig);
    seq.endTick = oneBar;
    smf.tracks.push_back(seq);

    SmfTrack track;
    SmfEvent program;
    program.status = 0xC0;
    program.data0 = 0; // voice 0
    track.events.push_back(program);
    SmfEvent volume;
    volume.status = 0xB0;
    volume.data0 = 7;
    volume.data1 = 100;
    track.events.push_back(volume);
    track.endTick = oneBar;
    smf.tracks.push_back(track);
    return smf;
}

bool saveRegistrationMeta(const QString &projectRoot, const QString &label, const QString &constant,
                          const QString &player)
{
    const QString path = sidecarPath(projectRoot, label);
    QJsonObject root;
    {
        QFile in(path);
        if (in.open(QIODevice::ReadOnly))
            root = QJsonDocument::fromJson(in.readAll()).object();
    }
    QJsonObject reg;
    reg.insert(QStringLiteral("constant"), constant);
    reg.insert(QStringLiteral("player"), player);
    root.insert(QStringLiteral("registration"), reg);

    Sidecar::ensureDir(projectRoot);
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    out.write(QJsonDocument(root).toJson());
    return true;
}

bool loadRegistrationMeta(const QString &projectRoot, const QString &label, QString *constant,
                          QString *player)
{
    QFile in(sidecarPath(projectRoot, label));
    if (!in.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject reg = QJsonDocument::fromJson(in.readAll())
                                .object()
                                .value(QStringLiteral("registration"))
                                .toObject();
    if (reg.isEmpty())
        return false;
    if (constant)
        *constant = reg.value(QStringLiteral("constant")).toString();
    if (player)
        *player = reg.value(QStringLiteral("player")).toString();
    return true;
}

void clearRegistrationMeta(const QString &projectRoot, const QString &label)
{
    const QString path = sidecarPath(projectRoot, label);
    QJsonObject root;
    {
        QFile in(path);
        if (!in.open(QIODevice::ReadOnly))
            return;
        root = QJsonDocument::fromJson(in.readAll()).object();
    }
    root.remove(QStringLiteral("registration"));
    if (root.isEmpty()) {
        QFile::remove(path);
        return;
    }
    QFile out(path);
    if (out.open(QIODevice::WriteOnly))
        out.write(QJsonDocument(root).toJson());
}

} // namespace SongRegistry

QStringList SongNameConflicts::messages() const
{
    QStringList list;
    for (const QString *message : {&label, &mid, &constant, &voicegroup}) {
        if (!message->isEmpty())
            list.append(*message);
    }
    return list;
}
