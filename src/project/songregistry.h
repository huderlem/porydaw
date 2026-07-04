#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include "project/decompproject.h"

struct SmfFile;

// The onboarding backend (SPEC.md §6.3): everything the New Song / Import
// wizards need to create a song and walk the user through registering it.
// porydaw itself writes only the .mid and the midi.cfg line; the three
// registration files get copy-paste snippets plus a live "recheck" that
// re-parses them from disk.

struct MusicPlayer {
    QString name;  // e.g. "MUSIC_PLAYER_BGM"
    int number = 0; // the .equiv value; also the song macro's third argument
};

struct RegistrationPlan {
    QString label;    // e.g. "mus_foo"
    QString constant; // e.g. "MUS_FOO"
    QString player;   // e.g. "MUSIC_PLAYER_BGM"
    int songId = -1;  // proposed ID = current count of song-table entries

    QString songTableSnippet; // "\tsong mus_foo, MUSIC_PLAYER_BGM, 0"
    QString songsHSnippet;    // "#define MUS_FOO 610 // MUS_FOO"
    QString ldSnippet;        // "        sound/songs/midi/mus_foo.o(.rodata);"
    bool ldApplicable = true; // false when ld_script.ld has no per-song lines
};

struct RegistrationStatus {
    bool inSongTable = false;
    bool inSongsH = false;
    bool inLdScript = false;
    bool ldApplicable = true;

    bool complete() const
    {
        return inSongTable && inSongsH && (inLdScript || !ldApplicable);
    }
};

namespace SongRegistry {

// -G arguments for every voicegroup label findable in the project: the
// "voice_group <name>" macro (modern pokeemerald) and raw "voicegroup*::"
// labels (pokefirered et al.), scanned from sound/voice_groups.inc,
// sound/voicegroups.inc, and sound/voicegroups/ recursively. Sorted.
QStringList voicegroupArgs(const QString &projectRoot);

// Music players from song_table.inc's ".equiv MUSIC_PLAYER_*,n" lines.
QVector<MusicPlayer> musicPlayers(const QString &projectRoot);

// Default constant for a label: "mus_foo" -> "MUS_FOO".
QString constantForLabel(const QString &label);

// Computes the three snippets against the files as they are on disk right
// now, matching each file's existing indentation/alignment.
RegistrationPlan makePlan(const QString &projectRoot, const QString &label,
                          const QString &constant, const QString &player);

// Re-parses the three files from disk. The songs.h item additionally
// requires the define's value to match the label's actual song-table index
// once the table entry exists (a mismatched ID is a mis-registration).
RegistrationStatus checkRegistration(const QString &projectRoot, const QString &label,
                                     const QString &constant);

// Rebuilds a song's midi.cfg flag list from its properties, keeping unknown
// flags (e.g. -L) and the original flag order intact.
QStringList mergeCfgFlags(const SongCfg &cfg);

// Updates or appends the song's line in <midiDir>/midi.cfg, byte-conservative
// for every other line (vanilla midi.cfg is CRLF; per-line \r is preserved).
bool writeMidiCfgLine(const QString &midiDir, const QString &label,
                      const QStringList &flags, QString *error);

// A minimal editable song: format 1, division 24 (vanilla), a seq track with
// tempo 120 + 4/4 time signature, and one instrument track (voice 0, VOL 100)
// spanning one bar.
SmfFile blankSong();

// Pending-registration metadata in the sidecar (.porydaw/<label>.json), so a
// half-registered song's checklist survives a project reopen (SPEC §6.3).
bool saveRegistrationMeta(const QString &projectRoot, const QString &label,
                          const QString &constant, const QString &player);
bool loadRegistrationMeta(const QString &projectRoot, const QString &label,
                          QString *constant, QString *player);
void clearRegistrationMeta(const QString &projectRoot, const QString &label);

} // namespace SongRegistry
