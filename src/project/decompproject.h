#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

// Per-song mid2agb options from the song's line in sound/songs/midi/midi.cfg.
struct SongCfg {
    QStringList rawFlags;    // the flags exactly as written in midi.cfg
    QString voicegroupArg;   // -G value, e.g. "_abandoned_ship" (mid2agb default: "_dummy")
    int masterVolume = 127;  // -V (0-127)
    int reverb = -1;         // -R, -1 = flag absent (no reverb override)
    int priority = 0;        // -P
    bool exactGate = false;  // -E
    bool extendedClocks = false; // -X (48 clocks/beat)
    bool noCompression = false;  // -N
};

struct SongInfo {
    int id = -1;             // index in the project's song vector; equals the
                             // numeric song ID only when registered
    QString label;           // e.g. "mus_abandoned_ship"
    QString constant;        // e.g. "MUS_ABANDONED_SHIP" (from songs.h, if matched)
    QString player;          // e.g. "MUSIC_PLAYER_BGM"
    QString midPath;         // absolute path to the .mid source, if it exists
    bool hasMid = false;
    bool hasCfg = false;
    // false: the .mid exists in sound/songs/midi/ but song_table.inc has no
    // entry yet — registerSong hasn't run (or failed) for it.
    bool registered = true;
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
    static QStringList voicegroupCandidates(const SongCfg &cfg);

    // Refreshes a song's cached cfg after porydaw writes its midi.cfg line.
    void setSongCfg(int id, const SongCfg &cfg);

    // Re-reads the project's music data (after the wizard creates and
    // registers a song). Song ids are reassigned.
    bool reload(QString *error);

private:
    bool parseSongTable(QString *error);
    void parseSongConstants();
    void parseMidiCfg();
    void discoverUnregisteredSongs();

    QString m_root;
    QVector<SongInfo> m_songs;
};
