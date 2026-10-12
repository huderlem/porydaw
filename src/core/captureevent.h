#pragma once

#include <cstdint>

// One message on the capture ring, audio thread to UI
// (docs/midi-input/PLAN.md step 3). Every stamp is the player's sample
// position at the start of the audio callback that applied it, so all
// messages of one callback share it. A played note on or off also carries
// how long its message waited for that callback (lateSamples), which the
// take subtracts, so the stamps lose that up-to-one-period jitter.
//
// A take's sample positions are only comparable within one *segment*: a
// stretch the player rendered without jumping. A loop wrap or a seek ends
// the segment with a Boundary and starts the next, so a note held across
// either one can be ended where the jump happened instead of being compared
// against a position that went backwards.
struct CaptureEvent {
    enum Kind : uint8_t {
        Begin,    // the take's first capturing callback: samplePos is punch-in
        NoteOn,   // a keyboard note on (after the sustain pedal and Fixed velocity)
        NoteOff,  // its note off (a pedal-held note ends at pedal-up)
        Cut,      // the engine cut or released every live note at samplePos (a
                  // transport change, a document edit, a panic): open notes
                  // end there
        Boundary, // the segment ended at samplePos; segment `segment` starts
                  // at resumePos (a loop wrap: loop end to loop start)
        Bend,     // a pitch-bend move: value is the bend, -8192..8191
        Control,  // a routed CC lane's move (docs/midi-controls/PLAN.md):
                  // key is the lane's document CC (0x01 MOD, 0x07 VOL, 0x0A
                  // PAN, 0x14 BENDR, 0x15 LFOS), value what it set, as heard
                  // (scaled to the lane: MOD to its range, BENDR to 0..24)
        Program,  // a program change (docs/midi-controls/PLAN.md step 5): value
                  // is the voice slot, 0..127, one the voicegroup has (J3)
    };
    enum Flag : uint8_t {
        // Bend/Control: a Reset All Controllers (CC121) put the lane back at
        // rest, not a move the player made. The take keeps it only on a lane
        // it already moved (J5).
        Reset = 0x01,
    };
    uint64_t samplePos = 0;
    uint64_t resumePos = 0;   // Boundary only
    uint32_t generation = 0;  // the arm this belongs to (AudioEngine::armCapture)
    uint32_t segment = 0;     // Boundary: the segment it starts
    uint32_t lateSamples = 0; // NoteOn/NoteOff/Bend/Control/Program: arrival to callback, in
                              // samples
    int16_t value = 0;        // Bend/Control/Program
    uint8_t kind = NoteOn;
    uint8_t key = 0; // NoteOn/NoteOff: the key; Control: the lane's CC
    uint8_t velocity = 0;
    uint8_t flags = 0; // Bend/Control: Flag bits
};
