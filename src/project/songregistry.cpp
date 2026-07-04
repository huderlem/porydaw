#include "songregistry.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTextStream>
#include <algorithm>

#include "core/smf.h"

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

QString sidecarPath(const QString &projectRoot, const QString &label)
{
    return projectRoot + QStringLiteral("/.porydaw/") + label + QStringLiteral(".json");
}

} // namespace

namespace SongRegistry {

QStringList voicegroupArgs(const QString &projectRoot)
{
    // voice_group macro (expands to a voicegroup_<name> symbol) and raw labels.
    static const QRegularExpression macroRe(
        QStringLiteral(R"(^\s*voice_group\s+(\w+))"));
    static const QRegularExpression labelRe(QStringLiteral(R"(^\s*(voicegroup\w+)::)"));

    QStringList files;
    for (const char *single : {"/sound/voice_groups.inc", "/sound/voicegroups.inc"}) {
        const QString path = projectRoot + QLatin1String(single);
        if (QFile::exists(path))
            files.append(path);
    }
    QDirIterator it(projectRoot + QStringLiteral("/sound/voicegroups"),
                    {QStringLiteral("*.inc")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        files.append(it.next());

    QStringList args;
    for (const QString &path : files) {
        for (const QString &line : readLines(path)) {
            QString symbol;
            QRegularExpressionMatch m = macroRe.match(line);
            if (m.hasMatch()) {
                symbol = QStringLiteral("voicegroup_") + m.captured(1);
            } else {
                m = labelRe.match(line);
                if (m.hasMatch())
                    symbol = m.captured(1);
            }
            // mid2agb forms the symbol as "voicegroup" + <-G arg>.
            if (!symbol.isEmpty())
                args.append(symbol.mid(10));
        }
    }
    args.removeDuplicates();
    std::sort(args.begin(), args.end());
    return args;
}

QVector<MusicPlayer> musicPlayers(const QString &projectRoot)
{
    // "	.equiv MUSIC_PLAYER_BGM,0"
    static const QRegularExpression equivRe(
        QStringLiteral(R"(^\s*\.equiv\s+(\w+)\s*,\s*(\d+))"));

    QVector<MusicPlayer> players;
    for (const QString &line :
         readLines(projectRoot + QStringLiteral("/sound/song_table.inc"))) {
        const QRegularExpressionMatch m = equivRe.match(line);
        if (m.hasMatch())
            players.append({m.captured(1), m.captured(2).toInt()});
    }
    if (players.isEmpty())
        players.append({QStringLiteral("MUSIC_PLAYER_BGM"), 0});
    return players;
}

QString constantForLabel(const QString &label)
{
    return label.toUpper();
}

RegistrationPlan makePlan(const QString &projectRoot, const QString &label,
                          const QString &constant, const QString &player)
{
    RegistrationPlan plan;
    plan.label = label;
    plan.constant = constant;
    plan.player = player;

    // song_table.inc: match the existing entries' indentation; the third
    // argument mirrors the player's .equiv number. Once the user has pasted
    // the table entry, the proposed ID is its actual index rather than the
    // append position.
    QString indent = QStringLiteral("\t");
    int songCount = 0;
    int existingIndex = -1;
    for (const QString &line :
         readLines(projectRoot + QStringLiteral("/sound/song_table.inc"))) {
        const QRegularExpressionMatch m = songLineRe().match(line);
        if (!m.hasMatch())
            continue;
        indent = m.captured(1);
        if (m.captured(2) == label)
            existingIndex = songCount;
        songCount++;
    }
    plan.songId = existingIndex >= 0 ? existingIndex : songCount;

    int playerNum = 0;
    for (const MusicPlayer &p : musicPlayers(projectRoot)) {
        if (p.name == player)
            playerNum = p.number;
    }
    plan.songTableSnippet =
        QStringLiteral("%1song %2, %3, %4").arg(indent, label, player).arg(playerNum);

    // songs.h: pad the constant to the file's existing value column.
    static const QRegularExpression defineRe(
        QStringLiteral(R"(^#define\s+([A-Z0-9_]+)(\s+)\d)"));
    int valueColumn = 0;
    for (const QString &line :
         readLines(projectRoot + QStringLiteral("/include/constants/songs.h"))) {
        const QRegularExpressionMatch m = defineRe.match(line);
        if (m.hasMatch())
            valueColumn = m.capturedEnd(2);
    }
    QString define = QStringLiteral("#define ") + constant;
    const int pad = valueColumn - define.size();
    define += QString(std::max(1, pad), QLatin1Char(' '));
    plan.songsHSnippet = define + QString::number(plan.songId);

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
        plan.ldSnippet =
            QStringLiteral("%1sound/songs/midi/%2.o(.rodata);").arg(ldIndent, label);
    return plan;
}

RegistrationStatus checkRegistration(const QString &projectRoot, const QString &label,
                                     const QString &constant)
{
    RegistrationStatus status;

    int tableIndex = -1;
    int count = 0;
    for (const QString &line :
         readLines(projectRoot + QStringLiteral("/sound/song_table.inc"))) {
        const QRegularExpressionMatch m = songLineRe().match(line);
        if (!m.hasMatch())
            continue;
        if (m.captured(2) == label)
            tableIndex = count;
        count++;
    }
    status.inSongTable = tableIndex >= 0;

    const QRegularExpression defineRe(
        QStringLiteral(R"(^\s*#define\s+%1\s+(\d+))").arg(constant));
    for (const QString &line :
         readLines(projectRoot + QStringLiteral("/include/constants/songs.h"))) {
        const QRegularExpressionMatch m = defineRe.match(line);
        if (!m.hasMatch())
            continue;
        // Once the table entry exists, the define must carry its real index.
        status.inSongsH = tableIndex < 0 || m.captured(1).toInt() == tableIndex;
        break;
    }

    const QString needle = QStringLiteral("sound/songs/midi/%1.o").arg(label);
    status.ldApplicable = false;
    for (const QString &line : readLines(projectRoot + QStringLiteral("/ld_script.ld"))) {
        if (line.contains(QStringLiteral("sound/songs/midi/")))
            status.ldApplicable = true;
        if (line.contains(needle)) {
            status.inLdScript = true;
            break;
        }
    }
    return status;
}

QStringList mergeCfgFlags(const SongCfg &cfg)
{
    // Updates or inserts "-<letter><value>"; a null value removes the flag.
    // Matching is case-insensitive as in mid2agb's parser.
    QStringList flags = cfg.rawFlags;
    const auto setValue = [&flags](char letter, const QString &value) {
        for (int i = 0; i < flags.size(); i++) {
            if (flags[i].size() >= 2 && flags[i][0] == QLatin1Char('-')
                && flags[i][1].toUpper() == QLatin1Char(letter)) {
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

bool writeMidiCfgLine(const QString &midiDir, const QString &label,
                      const QStringList &flags, QString *error)
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

bool saveRegistrationMeta(const QString &projectRoot, const QString &label,
                          const QString &constant, const QString &player)
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

    QDir().mkpath(QFileInfo(path).path());
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    out.write(QJsonDocument(root).toJson());
    return true;
}

bool loadRegistrationMeta(const QString &projectRoot, const QString &label,
                          QString *constant, QString *player)
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
