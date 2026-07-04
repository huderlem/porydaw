#pragma once

#include <QString>
#include <QStringList>
#include <cstdint>
#include <vector>

#include "core/smf.h"

// External-MIDI import analysis (SPEC.md §6.2): everything the guided mapping
// pass shows about an arbitrary .mid before it becomes a project song. The
// file's bytes are kept as-is on import — mid2agb rescales the division and
// ignores CCs outside its vocabulary — so the pass is a lens plus one
// optional transform (program remapping against the chosen voicegroup).

struct ImportTrackInfo {
    int smfTrack = -1;      // chunk index (format 1) or 0 (format 0)
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
// to `fromProgram` on this track/channel to `toProgram`.
struct ProgramRemap {
    int smfTrack = -1;
    uint8_t channel = 0;
    uint8_t fromProgram = 0;
    uint8_t toProgram = 0;
};

void applyProgramRemaps(SmfFile *smf, const std::vector<ProgramRemap> &remaps);
