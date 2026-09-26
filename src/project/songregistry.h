#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include "project/decompproject.h"

struct SmfFile;

// The onboarding backend (SPEC.md §6.3): everything the New Song / Import
// wizards need to create a song and register it. porydaw writes the .mid,
// the midi.cfg line, and the registration files (song_table.inc, songs.h,
// ld_script.ld, charmap.txt, src/debug.c) directly — inserting or
// correcting only the song's own lines, byte-conservative for everything
// else.

struct RegistrationPlan {
    QString label;    // e.g. "mus_foo"
    QString constant; // e.g. "MUS_FOO"
    QString player;   // e.g. "MUSIC_PLAYER_BGM"
    // Proposed ID: the current count of song-table entries, a freed slot,
    // or — on regioned layouts (below) — the slot after the region marker.
    int songId = -1;

    // Region-aware placement for marker-bounded songs.h layouts (END_SE /
    // START_MUS / END_MUS in alias or value form, sizing ID-indexed arrays
    // — src/debug.c's pre-#9713 sound-tester names, overworld.c's night
    // music; scanRegionMarkers in songregistry.cpp). All inactive on
    // marker-less modern layouts: the table row appends or fills a free
    // slot, and no other define moves.
    int tableInsertIndex = -1;  // insert the row at this index, shifting later rows
    int tableReplaceIndex = -1; // overwrite the placeholder row at this index
    int migrateFromIndex = -1;  // misplaced existing row to remove first
    int renumberFrom = -1;      // songs.h defines and charmap values in
    int renumberBelow = -1;     //   [renumberFrom, renumberBelow) shift up by one
    bool repointEndMus = false; // END_MUS follows the new constant
    bool repointEndSe = false;  // END_SE follows the new constant
    // Region overrides SE_-prefix list routing — only where the split is
    // functional (pre-#9713 debug.c with separately indexed arrays).
    bool debugUseBgmList = false;

    QString songTableLine; // "\tsong mus_foo, MUSIC_PLAYER_BGM, 0"
    QString songsHLine;    // "#define MUS_FOO 610"
    QString ldLine;        // "        sound/songs/midi/mus_foo.o(.rodata);"
    QString charmapLine;   // "MUS_FOO = 62 02" — the ID, little-endian bytes
    // "    X(MUS_FOO)          \" — the debug menu's sound-list entry in its
    // mid-list form; one landing at a list's end drops the '\' continuation.
    QString debugLine;
    bool ldApplicable = true;      // false when ld_script.ld has no per-song lines
    bool charmapApplicable = true; // false when charmap.txt has no song entries
    bool debugApplicable = true;   // false when src/debug.c has no sound lists
};

// What deleting a song would do to sound/song_table.inc, plus which other
// registration files carry a line to remove — the Delete Song confirmation.
struct RemovalPlan {
    int tableIndex = -1; // the song's table index; -1 = no table entry
    int tableCount = 0;
    // The final entry's line is removed outright (no song after it shifts).
    // A mid-table entry instead becomes a free slot — a duplicate of entry
    // 0's dummy line — so every later song keeps its ID.
    bool lastEntry = false;
    bool inSongsH = false;
    bool inLdScript = false;
    bool inCharmap = false;
    bool inDebugMenu = false;
};

struct RegistrationStatus {
    bool inSongTable = false;
    bool inSongsH = false;
    bool inLdScript = false;
    bool inCharmap = false;
    bool inDebugMenu = false;
    bool ldApplicable = true;
    bool charmapApplicable = true;
    bool debugApplicable = true;

    bool complete() const
    {
        return inSongTable && inSongsH && (inLdScript || !ldApplicable) &&
               (inCharmap || !charmapApplicable) && (inDebugMenu || !debugApplicable);
    }
};

// Names held by open tabs that the project's files don't know yet — a
// draft's label, constant and new voicegroup (docs/draft-songs/PLAN.md).
// MainWindow::reservedSongNames collects them for checkNewSongNames.
struct ReservedSongNames {
    QStringList labels;
    QStringList constants;
    QStringList voicegroups; // file base names under sound/voicegroups/
};

// Why a new song's names can't be used: one message per name, empty when
// that name is free (SongRegistry::checkNewSongNames).
struct SongNameConflicts {
    QString label;      // a song already has the label
    QString mid;        // sound/songs/midi/<label>.mid exists
    QString constant;   // songs.h defines it, or another song uses it
    QString voicegroup; // its file or voicegroup_<name> exists, or a draft makes it

    bool isEmpty() const
    {
        return label.isEmpty() && mid.isEmpty() && constant.isEmpty() && voicegroup.isEmpty();
    }
    // The non-empty messages, in field order.
    QStringList messages() const;
};

namespace SongRegistry {

// -G arguments for every voicegroup label findable in the project: the
// "voice_group <name>" macro (modern pokeemerald) and raw "voicegroup*::"
// labels (pokefirered et al.), scanned from sound/voice_groups.inc,
// sound/voicegroups.inc, and sound/voicegroups/ recursively. Sorted.
QStringList voicegroupArgs(const QString &projectRoot);

// Display form of a -G arg: the leading underscore folds into the fixed
// "voicegroup_" prefix the UI shows, so "_abandoned_ship" reads as
// "abandoned_ship". Underscore-less args (vanilla "128"-style symbols)
// pass through unchanged.
QString voicegroupDisplayName(const QString &arg);

// The inverse: a name typed under the "voicegroup_" prefix back to a -G
// arg. A leading underscore means a raw arg was pasted; a verbatim match
// against knownArgs keeps legacy underscore-less args addressable;
// everything else assumes the underscore.
QString voicegroupArgFromDisplay(const QString &text, const QStringList &knownArgs);

// Music players from song_table.inc's ".equiv MUSIC_PLAYER_*,n" lines.
QVector<MusicPlayer> musicPlayers(const QString &projectRoot);

// Default constant for a label: "mus_foo" -> "MUS_FOO".
QString constantForLabel(const QString &label);

// Whether a new song may take these names: label, constant, and (when not
// empty) the voicegroup it creates. Reads the disk as it is now — the .mid,
// song_table.inc's labels, songs.h's defines, the voicegroup file and any
// file declaring its voicegroup_<name> symbol — on top
// of songs (the project's list, which may be stale after a git pull) and
// the names reserved by open drafts. An empty argument skips its checks.
// The New Song wizard and the draft commit (MainWindow::commitDraft) both
// decide through this, so their rules can't drift.
//
// ownMidPath (optional) is a .mid the caller wrote itself — a draft whose
// earlier commit attempt wrote its .mid and then failed (PLAN stance 5). That
// file is not a conflict: neither its existence nor the unregistered song a
// project reload lists for it (DecompProject::discoverUnregisteredSongs gives
// it the draft's label and a label-derived constant) counts as taken. Only
// that exact path is waived; song_table.inc and songs.h still decide.
SongNameConflicts checkNewSongNames(const QString &projectRoot, const QVector<SongInfo> &songs,
                                    const ReservedSongNames &reserved, const QString &label,
                                    const QString &constant, const QString &newVoicegroup,
                                    const QString &ownMidPath = QString());

// Computes the registration lines against the files as they are on disk
// right now, matching each file's existing indentation/alignment.
RegistrationPlan makePlan(const QString &projectRoot, const QString &label, const QString &constant,
                          const QString &player);

// Writes the song into all registration files: the song_table.inc entry
// (filling the lowest free slot a deleted song left, else appending), the
// songs.h #define and charmap.txt ID mapping each at their ID-order
// position, the ld_script.ld object line, and the src/debug.c sound-list
// X-macro entry (the last three when applicable). Idempotent —
// entries that already exist are left byte-identical, except a songs.h
// define or charmap.txt entry whose ID matches none of the label's table
// entries (a label can own several — forks alias real songs into filler
// slots), which is corrected in place. Only the song's own lines change —
// except on regioned layouts (RegistrationPlan's region fields), where an
// insertion ahead of the phoneme block also shifts the displaced defines,
// charmap values, and the END_MUS/END_SE marker, and a registration
// stranded past the markers migrates into the region.
// On success *songId carries the song's table index.
bool registerSong(const QString &projectRoot, const QString &label, const QString &constant,
                  const QString &player, QString *error, int *songId = nullptr);

// What unregisterSong would edit, for the Delete Song confirmation dialog.
RemovalPlan makeRemovalPlan(const QString &projectRoot, const QString &label,
                            const QString &constant);

// The inverse of registerSong: removes the song's songs.h define, ld_script
// object line, charmap entry, and src/debug.c sound-list entry, and
// removes its song_table.inc entry when
// it is the last one (also dropping free slots left trailing) or replaces
// it with a free slot when other songs follow. A free slot is a plain
// duplicate of entry 0's line — the engine's fallback song (mus_dummy), so
// any later entry bearing its label is a placeholder by construction, no
// marker needed — which keeps every later song's index and stays reusable:
// makePlan/registerSong fill the lowest free slot before growing the table.
// Entry 0 itself is never a free slot, and this refuses to delete it.
// An END_SE/END_MUS marker aliasing the deleted constant re-points to the
// region's new last song instead of dangling.
// Byte-conservative and idempotent — a song with no entries anywhere is a
// no-op success.
bool unregisterSong(const QString &projectRoot, const QString &label, const QString &constant,
                    QString *error);

// The song's voicegroup when deleting the song may delete it too: its -G arg
// resolves to a file under sound/voicegroups/, no other song's -G references
// it, no other voicegroup uses it as a keysplit/drumkit sub-group, and its
// symbol appears nowhere in the project's src/ or include/ sources (a C
// reference would become a link error, not a harmless dangling line).
// Returns the voicegroup's file base name, or empty when any condition
// fails.
QString deletableVoicegroup(const QString &projectRoot, const QVector<SongInfo> &songs,
                            const QString &songLabel);

// Re-parses the registration files from disk. The songs.h and charmap.txt
// items additionally require their value to match one of the label's
// song-table indices once a table entry exists (a value naming none of
// them is a mis-registration; several indices per label are legitimate —
// forks fill new slots with copies of real songs).
RegistrationStatus checkRegistration(const QString &projectRoot, const QString &label,
                                     const QString &constant);

// The registration files a status says still miss (or mis-state) the
// song, in registration order and skipping inapplicable files:
// SongInfo::registrationGaps and porydaw.project.registration().gaps.
QStringList registrationGaps(const RegistrationStatus &status);

// checkRegistration for every song at once, keyed by label: one read of
// each registration file for the whole project (the song browser audits
// hundreds of songs at open). A song with no parsed constant is checked
// under its label-derived default, like the Register Song action.
QHash<QString, RegistrationStatus> checkRegistrations(const QString &projectRoot,
                                                      const QVector<SongInfo> &songs);

// Rebuilds a song's midi.cfg flag list from its properties, keeping unknown
// flags (e.g. -L) and the original flag order intact.
QStringList mergeCfgFlags(const SongCfg &cfg);

// Updates or appends the song's line in <midiDir>/midi.cfg, byte-conservative
// for every other line (vanilla midi.cfg is CRLF; per-line \r is preserved).
bool writeMidiCfgLine(const QString &midiDir, const QString &label, const QStringList &flags,
                      QString *error);

// Persists a song's flags wherever the project stores them: its midi.cfg
// line when <midiDir>/midi.cfg exists, its songs.mk rule for projects
// predating midi.cfg, and a fresh midi.cfg when the project has neither.
bool writeSongFlags(const QString &midiDir, const QString &label, const QStringList &flags,
                    QString *error);

// Removes the song's flag storage everywhere it may live: its midi.cfg line
// and its songs.mk rule (a project can carry both when midi.cfg arrived
// later). Missing files or lines are a no-op success.
bool removeSongFlags(const QString &midiDir, const QString &label, QString *error);

// Deletes the song's .porydaw/<label>.json sidecar outright (view state and
// pending-registration metadata alike) — the file describes a song that no
// longer exists. Best-effort, like all sidecar writes.
void removeSongSidecar(const QString &projectRoot, const QString &label);

// A minimal editable song: format 1, division 24 (vanilla), a seq track with
// tempo 120 + 4/4 time signature, and one instrument track (voice 0, VOL 100)
// spanning one bar.
SmfFile blankSong();

// Pending-registration metadata in the sidecar (.porydaw/<label>.json), so
// an unregistered song's chosen constant/player survive a project reopen
// when registerSong could not complete (SPEC §6.3).
bool saveRegistrationMeta(const QString &projectRoot, const QString &label, const QString &constant,
                          const QString &player);
bool loadRegistrationMeta(const QString &projectRoot, const QString &label, QString *constant,
                          QString *player);
void clearRegistrationMeta(const QString &projectRoot, const QString &label);

} // namespace SongRegistry
