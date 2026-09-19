#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

// Per-song mid2agb options from the song's line in sound/songs/midi/midi.cfg
// (or, in projects predating midi.cfg, its songs.mk rule).
struct SongCfg {
    // Vanilla songs.mk passes -R$(STD_REVERB) = 50 to every song, so a song
    // whose flags lack -R effectively plays at 50 in-game; treat that as the
    // default wherever the flag is absent.
    static constexpr int kDefaultReverb = 50;

    QStringList rawFlags;        // the flags as written ($(VAR) refs pre-expanded)
    QString voicegroupArg;       // -G value, e.g. "_abandoned_ship" (mid2agb default: "_dummy")
    int masterVolume = 127;      // -V (0-127)
    int reverb = -1;             // -R, -1 = flag absent (defaults to kDefaultReverb)
    int priority = 0;            // -P
    bool exactGate = false;      // -E
    bool extendedClocks = false; // -X (48 clocks/beat)
    bool noCompression = false;  // -N
};

struct MusicPlayer {
    QString name;   // e.g. "MUSIC_PLAYER_BGM"
    int number = 0; // the .equiv value; also the song macro's third argument
    // Tracks this player allocates (music_player_table.inc), clamped to the
    // engine's 16 the way MPlayOpen clamps. MPlayStart never starts a song
    // track at or beyond this index, so it bounds what sounds in-game.
    // -1 when the table couldn't be parsed (no limit assumed).
    int trackCount = -1;
};

struct SongInfo {
    int id = -1;      // index in the project's song vector; equals the
                      // numeric song ID only when registered
    QString label;    // e.g. "mus_abandoned_ship"
    QString constant; // e.g. "MUS_ABANDONED_SHIP" (from songs.h, if matched)
    QString player;   // e.g. "MUSIC_PLAYER_BGM"
    QString midPath;  // absolute path to the .mid source, if it exists
    bool hasMid = false;
    bool hasCfg = false;
    // false: the .mid exists in sound/songs/midi/ but song_table.inc has no
    // entry yet — registerSong hasn't run (or failed) for it.
    bool registered = true;
    // Registration files still missing (or mis-stating) this song's entry,
    // e.g. "charmap.txt" for a song registered before porydaw wrote charmap
    // entries. Empty when the registration is complete. Stamped at open from
    // SongRegistry::checkRegistrations; drives the browser badge and the
    // Register Song enablement.
    QStringList registrationGaps;
    SongCfg cfg;

    bool isPlayable() const { return hasMid; }
};

// Read-only view of a Gen 3 decomp project's music data: the song list
// assembled from sound/song_table.inc, include/constants/songs.h, and
// sound/songs/midi/midi.cfg (falling back to songs.mk rules when midi.cfg
// does not exist). Voicegroups/samples are loaded separately via poryaaaa's
// voicegroup_loader.
class DecompProject
{
  public:
    bool open(const QString &rootDir, QString *error);
    void close();

    bool isOpen() const { return !m_root.isEmpty(); }
    const QString &root() const { return m_root; }
    const QVector<SongInfo> &songs() const { return m_songs; }
    // The song table's music players (SongRegistry::musicPlayers, cached at open).
    const QVector<MusicPlayer> &musicPlayers() const { return m_players; }

    // Engine tracks the song's music player allocates (its MusicPlayer::
    // trackCount, cached at open). Tracks at or beyond this never start
    // in-game (MPlayStart), so playback and the UI treat them as silent.
    // 16 when the player is unknown or the table couldn't be parsed — the
    // engine ceiling, i.e. no porydaw-invented limit.
    int trackBudgetFor(const SongInfo &song) const;

    // Loader-compatible voicegroup names to try, in order, for a song.
    // mid2agb emits "voicegroup" + <-G arg> as the symbol; poryaaaa's loader
    // wants the file-basename form with any "voicegroup_" prefix stripped.
    static QStringList voicegroupCandidates(const SongInfo &song);
    static QStringList voicegroupCandidates(const SongCfg &cfg);

    // One midi.cfg line ("mus_x.mid: -E -R50 -G_x -V080", comments allowed)
    // into its song label and parsed flags; false for a line that names no
    // song. Shared with the song-bundle reader, whose midi.cfg is one line.
    static bool parseMidiCfgLine(const QString &line, QString *label, SongCfg *cfg);
    // mid2agb option letters are case-insensitive (-v080 == -V080).
    static SongCfg cfgFromFlags(const QStringList &flags);

    // Refreshes a song's cached cfg after porydaw writes its flags back.
    void setSongCfg(int id, const SongCfg &cfg);

    // Re-reads the project's music data (after the wizard creates and
    // registers a song). Song ids are reassigned.
    bool reload(QString *error);

  private:
    bool parseSongTable(QString *error);
    void parseSongConstants();
    bool parseMidiCfg(); // false if midi.cfg does not exist (or can't open)
    void parseSongsMk();
    void discoverUnregisteredSongs();

    QString m_root;
    QVector<SongInfo> m_songs;
    QVector<MusicPlayer> m_players; // cached at open (one file read)
};
