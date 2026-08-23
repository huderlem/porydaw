#pragma once

#include <QString>
#include <QStringList>
#include <cstdint>
#include <vector>

#include "core/smf.h"

// External-MIDI import analysis (SPEC.md §6.2): everything the import wizard
// shows about an arbitrary .mid before it becomes a project song. The
// analysis pass is a pure lens; the import itself applies two transforms —
// a silent same-tick setter dedup (removeRedundantSetterEvents) and an
// optional division rescale onto the m4a clock grid.

struct ImportTrackInfo {
    int smfTrack = -1; // chunk index
    QString name;      // track-name meta, if present
    int noteCount = 0;
    // Programs in order of first use; empty means every note plays voice 0
    // (mid2agb's initial program) — worth flagging against the voicegroup.
    std::vector<uint8_t> programs;
    bool notesBeforeProgram = false; // notes sound before the first VOICE
};

struct ImportCcUsage {
    uint8_t cc = 0;
    int count = 0;
    QString label;        // m4a meaning ("VOL — Volume") or "CC n (ignored by mid2agb)"
    bool audible = false; // rendered by the engine (vs. kept-but-inert)
};

struct ImportAnalysis {
    uint16_t division = 24;
    int smfTrackCount = 0;
    int mappedTracks = 0;  // engine tracks (first 16 channel-bearing chunks)
    int droppedTracks = 0; // channel-bearing chunks beyond the m4a limit
    int silentTracks = 0;  // mapped tracks beyond the player's track budget
    int peakConcurrentNotes = 0;
    int sampleNoteLimit = 0;
    std::vector<ImportTrackInfo> tracks; // one per mapped engine track
    std::vector<ImportCcUsage> ccs;      // by CC number, ascending
    QStringList warnings;                // human-readable mapping-pass flags
};

// trackBudget/playerName describe the music player the song will run on
// (MusicPlayer::trackCount): mapped tracks at or beyond the budget never
// start in-game, which is worth a warning of its own below the hard 16
// ceiling. Budget 16 (or a negative unknown) disables that warning.
ImportAnalysis analyzeForImport(const SmfFile &smf, int trackBudget = 16,
                                const QString &playerName = QString());

// User-facing wording shared by the analysis warnings and the import wizard's
// notices, so two renderings of the same fact cannot drift apart.

// "Background music"/"Sound effect n" for the music-player symbols every
// decomp ships; any other symbol comes back unchanged. includeSymbol appends
// the engine symbol in parentheses.
QString playerRoleName(const QString &symbol, bool includeSymbol);

// "1 track" / "<n> tracks" — count phrases stay grammatical at one.
QString trackCountPhrase(int count);

// The sample-note concurrency notice and the notes-before-any-instrument
// notice, verbatim as the wizard shows them.
QString concurrencyNoticeText(int peakNotes, int sampleNoteLimit);
QString instrumentFallbackNoticeText();

// Rescale every event tick (and each track's end-of-track tick) onto a new
// division, using the same floor arithmetic as mid2agb's event conversion
// (`24 * clocksPerBeat * time / division`, tools/mid2agb/midi.cpp). With
// newDivision equal to the song's clocks per beat (24, or 48 under -X), every
// onset lands on the exact tick mid2agb would have played it at, and the
// editor grid becomes exact. Note durations may still differ from an as-is
// import by one clock, because mid2agb floors onset and duration
// independently while a tick rescale floors onset and note-off.
void rescaleDivision(SmfFile *smf, uint16_t newDivision);

// Drop same-tick duplicate state-setters, keeping the last of each run in
// place. Exporters commonly emit a channel-init block several times over
// (duplicate tick-0 program/volume/pan/bend), and since both mid2agb's output
// and the engine apply a tick's events in order, only the last of a same-slot
// run is ever audible — the rest just shadow it from every editing surface.
// Events where every occurrence acts (notes, text/marker metas, and the
// coupled-protocol CCs: MEMACC plumbing, XCMD, the loop Label) are never
// touched. Returns the number of events removed.
int removeRedundantSetterEvents(SmfFile *smf);

// Move tempo metas (0x51) from later chunks into the first chunk. mid2agb
// reads tempo from the first chunk only, so a foreign format-1 file that
// keeps its tempo map elsewhere would silently play at 120 BPM in-game (and
// in the app, whose timeline mirrors mid2agb); moving the metas at import
// makes them real. Tick order is preserved, and at a shared tick the moved
// metas land after the first chunk's own (and in chunk order among
// themselves), so the file-order winner of same-tick duplicates keeps
// winning — run before removeRedundantSetterEvents, which then collapses
// the losers. Returns the number of metas moved.
int moveTempoMetasToFirstChunk(SmfFile *smf);
