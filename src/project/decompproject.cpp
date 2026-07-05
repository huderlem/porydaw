#include "decompproject.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>

#include "project/songregistry.h"

bool DecompProject::open(const QString &rootDir, QString *error)
{
    close();

    const QDir dir(rootDir);
    if (!dir.exists()) {
        if (error)
            *error = QStringLiteral("Directory does not exist: %1").arg(rootDir);
        return false;
    }
    m_root = dir.absolutePath();

    if (!parseSongTable(error)) {
        m_root.clear();
        return false;
    }
    parseSongConstants();
    discoverUnregisteredSongs();
    parseMidiCfg();
    return true;
}

bool DecompProject::reload(QString *error)
{
    const QString root = m_root;
    return open(root, error);
}

void DecompProject::close()
{
    m_root.clear();
    m_songs.clear();
}

bool DecompProject::parseSongTable(QString *error)
{
    const QString path = m_root + QStringLiteral("/sound/song_table.inc");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral(
                         "Cannot open %1.\n\nIs this a pokeemerald/pokefirered/pokeruby "
                         "project directory?")
                         .arg(path);
        return false;
    }

    // e.g. "	song mus_dummy, MUSIC_PLAYER_BGM, 0"
    static const QRegularExpression songRe(
        QStringLiteral(R"(^\s*song\s+(\w+)\s*,\s*(\w+)\s*,\s*(\w+))"));

    const QString midiDir = m_root + QStringLiteral("/sound/songs/midi/");
    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine();
        const QRegularExpressionMatch m = songRe.match(line);
        if (!m.hasMatch())
            continue;

        SongInfo song;
        song.id = m_songs.size();
        song.label = m.captured(1);
        song.player = m.captured(2);

        const QString midPath = midiDir + song.label + QStringLiteral(".mid");
        if (QFile::exists(midPath)) {
            song.midPath = midPath;
            song.hasMid = true;
        }
        m_songs.append(song);
    }

    if (m_songs.isEmpty()) {
        if (error)
            *error = QStringLiteral("No songs found in %1").arg(path);
        return false;
    }
    return true;
}

void DecompProject::parseSongConstants()
{
    QFile file(m_root + QStringLiteral("/include/constants/songs.h"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    // e.g. "#define MUS_ABANDONED_SHIP 381"
    static const QRegularExpression defineRe(
        QStringLiteral(R"(^\s*#define\s+([A-Z0-9_]+)\s+(\d+)\s*$)"));

    QHash<int, QString> byId;
    QTextStream in(&file);
    while (!in.atEnd()) {
        const QRegularExpressionMatch m = defineRe.match(in.readLine());
        if (!m.hasMatch())
            continue;
        const int id = m.captured(2).toInt();
        // First definition wins (later defines like PH_* aliases share IDs).
        if (!byId.contains(id))
            byId.insert(id, m.captured(1));
    }

    for (SongInfo &song : m_songs) {
        const auto it = byId.constFind(song.id);
        if (it != byId.constEnd())
            song.constant = it.value();
    }
}

void DecompProject::discoverUnregisteredSongs()
{
    // .mid files with no song_table.inc entry: songs whose registration
    // never ran (dropped-in files) or failed. Listing them keeps the badge
    // visible across project reopens so Register Song can finish the job.
    // Identity chosen in the wizard comes back from the sidecar; the
    // constant falls back to the label-derived default.
    QSet<QString> known;
    for (const SongInfo &song : m_songs)
        known.insert(song.label);

    const QDir midiDir(m_root + QStringLiteral("/sound/songs/midi"));
    const QStringList mids =
        midiDir.entryList({QStringLiteral("*.mid")}, QDir::Files, QDir::Name);
    for (const QString &fileName : mids) {
        const QString label = fileName.chopped(4);
        if (known.contains(label))
            continue;
        SongInfo song;
        song.id = m_songs.size();
        song.label = label;
        song.registered = false;
        song.midPath = midiDir.filePath(fileName);
        song.hasMid = true;
        song.constant = SongRegistry::constantForLabel(label);
        song.player = QStringLiteral("MUSIC_PLAYER_BGM");
        QString constant, player;
        if (SongRegistry::loadRegistrationMeta(m_root, label, &constant, &player)) {
            if (!constant.isEmpty())
                song.constant = constant;
            if (!player.isEmpty())
                song.player = player;
        }
        m_songs.append(song);
    }
}

void DecompProject::parseMidiCfg()
{
    QFile file(m_root + QStringLiteral("/sound/songs/midi/midi.cfg"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    // e.g. "mus_abandoned_ship.mid: -E -R50 -G_abandoned_ship -V080"
    QHash<QString, SongCfg> byLabel;
    QTextStream in(&file);
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        const int hash = line.indexOf(QLatin1Char('#'));
        if (hash >= 0)
            line = line.left(hash).trimmed();
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0)
            continue;

        QString name = line.left(colon).trimmed();
        if (name.endsWith(QStringLiteral(".mid"), Qt::CaseInsensitive))
            name.chop(4);

        SongCfg cfg;
        const QStringList flags =
            line.mid(colon + 1).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        cfg.rawFlags = flags;
        for (const QString &flag : flags) {
            if (flag.size() < 2 || flag[0] != QLatin1Char('-'))
                continue;
            const QChar opt = flag[1];
            const QString arg = flag.mid(2);
            switch (opt.toLatin1()) {
            case 'G':
                cfg.voicegroupArg = arg;
                break;
            case 'V':
                cfg.masterVolume = qBound(0, arg.toInt(), 127);
                break;
            case 'R':
                cfg.reverb = qBound(0, arg.toInt(), 127);
                break;
            case 'P':
                cfg.priority = arg.toInt();
                break;
            case 'E':
                cfg.exactGate = true;
                break;
            case 'X':
                cfg.extendedClocks = true;
                break;
            case 'N':
                cfg.noCompression = true;
                break;
            default:
                break; // -L and unknown flags: irrelevant for playback
            }
        }
        byLabel.insert(name, cfg);
    }

    for (SongInfo &song : m_songs) {
        const auto it = byLabel.constFind(song.label);
        if (it != byLabel.constEnd()) {
            song.hasCfg = true;
            song.cfg = it.value();
        }
    }
}

QStringList DecompProject::voicegroupCandidates(const SongInfo &song)
{
    return voicegroupCandidates(song.cfg);
}

void DecompProject::setSongCfg(int id, const SongCfg &cfg)
{
    if (id >= 0 && id < m_songs.size()) {
        m_songs[id].cfg = cfg;
        m_songs[id].hasCfg = true;
    }
}

QStringList DecompProject::voicegroupCandidates(const SongCfg &cfg)
{
    // mid2agb's default -G argument is "_dummy" (symbol voicegroup_dummy).
    const QString arg = cfg.voicegroupArg.isEmpty() ? QStringLiteral("_dummy")
                                                    : cfg.voicegroupArg;
    const QString symbol = QStringLiteral("voicegroup") + arg;

    QStringList candidates;
    if (symbol.startsWith(QStringLiteral("voicegroup_")))
        candidates << symbol.mid(11); // e.g. "abandoned_ship"
    candidates << symbol;             // e.g. "voicegroup000" (pokefirered labels)
    if (!arg.isEmpty() && !candidates.contains(arg))
        candidates << arg;
    return candidates;
}
