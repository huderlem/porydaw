#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

// Per-song mid2agb options from the song's line in sound/songs/midi/midi.cfg.
struct SongCfg {
    QString voicegroupArg;   // -G value, e.g. "_abandoned_ship" (mid2agb default: "_dummy")
    int masterVolume = 127;  // -V (0-127)
    int reverb = -1;         // -R, -1 = flag absent (no reverb override)
    int priority = 0;        // -P
    bool exactGate = false;  // -E
    bool extendedClocks = false; // -X (48 clocks/beat)
    bool noCompression = false;  // -N
};

struct SongInfo {
    int id = -1;             // index in song_table.inc == numeric song ID
    QString label;           // e.g. "mus_abandoned_ship"
    QString constant;        // e.g. "MUS_ABANDONED_SHIP" (from songs.h, if matched)
    QString player;          // e.g. "MUSIC_PLAYER_BGM"
    QString midPath;         // absolute path to the .mid source, if it exists
    bool hasMid = false;
    bool hasCfg = false;
    SongCfg cfg;

    bool isPlayable() const { return hasMid; }
};

// Read-only view of a Gen 3 decomp project's music data: the song list
// assembled from sound/song_table.inc, include/constants/songs.h, and
// sound/songs/midi/midi.cfg. Voicegroups/samples are loaded separately via
// poryaaaa's voicegroup_loader.
class DecompProject
{
public:
    bool open(const QString &rootDir, QString *error);
    void close();

    bool isOpen() const { return !m_root.isEmpty(); }
    const QString &root() const { return m_root; }
    const QVector<SongInfo> &songs() const { return m_songs; }

    // Loader-compatible voicegroup names to try, in order, for a song.
    // mid2agb emits "voicegroup" + <-G arg> as the symbol; poryaaaa's loader
    // wants the file-basename form with any "voicegroup_" prefix stripped.
    static QStringList voicegroupCandidates(const SongInfo &song);

private:
    bool parseSongTable(QString *error);
    void parseSongConstants();
    void parseMidiCfg();

    QString m_root;
    QVector<SongInfo> m_songs;
};
