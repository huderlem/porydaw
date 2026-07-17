#pragma once

#include <QString>
#include <QStringList>
#include <cstdint>
#include <vector>

#include "core/smf.h"

// External-MIDI import analysis (SPEC.md §6.2): everything the guided mapping
// pass shows about an arbitrary .mid before it becomes a project song. The
// file's bytes are kept as-is on import — mid2agb rescales the division and
// ignores CCs outside its vocabulary — so the pass is a lens plus two
// optional transforms: program remapping against the chosen voicegroup, and
// a division rescale onto the m4a clock grid.

struct ImportTrackInfo {
    int smfTrack = -1;      // chunk index
    uint8_t channel = 0;    // the track's MIDI channel, as the engine maps it
    QString name;           // track-name meta, if present
    int noteCount = 0;
    // Programs in order of first use; empty means every note plays voice 0
    // (mid2agb's initial program) — worth flagging against the voicegroup.
    std::vector<uint8_t> programs;
    bool notesBeforeProgram = false; // notes sound before the first VOICE
};

struct ImportCcUsage {
    uint8_t cc = 0;
    int count = 0;
    QString label; // m4a meaning ("VOL — Volume") or "CC n (ignored by mid2agb)"
    bool audible = false; // rendered by the engine (vs. kept-but-inert)
};

struct ImportAnalysis {
    uint16_t format = 1;
    uint16_t division = 24;
    int smfTrackCount = 0;
    int mappedTracks = 0;  // engine tracks (first 16 channel-bearing chunks)
    int droppedTracks = 0; // channel-bearing chunks beyond the m4a limit
    int peakConcurrentNotes = 0;
    std::vector<ImportTrackInfo> tracks; // one per mapped engine track
    std::vector<ImportCcUsage> ccs;      // by CC number, ascending
    QStringList warnings;                // human-readable mapping-pass flags
};

ImportAnalysis analyzeForImport(const SmfFile &smf);

// One row of the wizard's program-mapping table: rewrite every program-change
// to `fromProgram` on this track to `toProgram`.
struct ProgramRemap {
    int smfTrack = -1;
    uint8_t fromProgram = 0;
    uint8_t toProgram = 0;
};

void applyProgramRemaps(SmfFile *smf, const std::vector<ProgramRemap> &remaps);

// Rescale every event tick (and each track's end-of-track tick) onto a new
// division, using the same floor arithmetic as mid2agb's event conversion
// (`24 * clocksPerBeat * time / division`, tools/mid2agb/midi.cpp). With
// newDivision equal to the song's clocks per beat (24, or 48 under -X), every
// onset lands on the exact tick mid2agb would have played it at, and the
// editor grid becomes exact. Note durations may still differ from an as-is
// import by one clock, because mid2agb floors onset and duration
// independently while a tick rescale floors onset and note-off.
void rescaleDivision(SmfFile *smf, uint16_t newDivision);
