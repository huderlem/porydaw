#pragma once

#include <QString>
#include <cstdint>

// The m4a semantic layer (SPEC.md §4.2): MIDI events are presented in mid2agb
// terms. CC numbers follow tools/mid2agb/agb.cpp exactly; whether a CC gets an
// audible automation lane follows what the embedded poryaaaa engine renders
// (m4a_engine_cc) — engine no-ops surface in the "other events" strip instead.

// A drawable automation lane. Values are the lane identity used by the viewer;
// CC-backed lanes exist per (track, cc), Bend/Tempo are dedicated event types.
enum class M4aLane {
    Mod,        // CC 1  -> MOD (LFO depth)
    Volume,     // CC 7  -> VOL
    Pan,        // CC 10 -> PAN
    BendRange,  // CC 20 -> BENDR
    LfoSpeed,   // CC 21 -> LFOS
    PitchBend,  // pitch-bend events -> BEND
    Tempo,      // tempo meta -> TEMPO (song-level lane)
};

// How the viewer treats one incoming event.
enum class M4aEventClass {
    Note,        // note on/off -> piano roll
    Voice,       // program change -> instrument marker on the track
    AudibleLane, // CC the engine renders -> automation lane
    Advanced,    // valid m4a data the engine ignores -> other-events strip
    Tempo,       // tempo -> tempo lane
};

struct M4aCcInfo {
    M4aEventClass eventClass;
    M4aLane lane;        // valid only when eventClass == AudibleLane
    const char *name;    // mid2agb command mnemonic, e.g. "VOL"
    const char *display; // human lane/strip label, e.g. "Volume"
};

// Classification for a MIDI CC number per mid2agb + the poryaaaa engine.
// Never fails: unmapped CCs come back as Advanced with a generic name.
M4aCcInfo m4aClassifyCc(uint8_t cc);

// Lane display name ("Volume", "Pitch bend", ...).
QString m4aLaneName(M4aLane lane);

// Value formatting for lane readouts, mirroring how mid2agb would emit the
// value (PAN/TUNE as c_v±, bend as signed 14-bit, etc.).
QString m4aFormatCcValue(uint8_t cc, uint8_t value);
QString m4aFormatBend(int bend14);

// Human label for an advanced/no-op CC event in the other-events strip,
// e.g. "MEMACC op 17 = 3" or "XCMD xIECV = 4".
QString m4aAdvancedCcLabel(uint8_t cc, uint8_t value);
