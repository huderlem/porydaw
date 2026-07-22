#pragma once

#include <QByteArray>

#include <atomic>
#include <cstdint>
#include <vector>

extern "C" {
#include "m4a_engine.h"
}

// The Sample Studio audition-slot protocol (docs/sample-studio/PLAN.md §4):
// audition an unregistered, in-memory sample through the real engine, with
// live re-render during loop-handle drags, without stopping the device.
//
// A fixed pool of N = 4 slots, each owning its ToneData/WaveData/bytes in
// porydaw memory (the SynthToneBuf precedent — zero poryaaaa changes). The
// UI thread renders into a *retired* slot and publishes a packed command;
// the audio thread adopts it at a callback boundary, note-offs the previous
// audition, keys the new slot's tone, and acknowledges. A slot retires only
// once no engine channel references its WaveData anymore — i.e. after the
// release envelope completes, not merely after the note-off.
//
// Device-independent by design: AudioEngine calls apply() from its audio
// callback, and the --samplecheck harness drives apply() against a bare
// M4AEngine to unit-test the retirement invariants.
class AuditionSlots
{
public:
    static constexpr int kSlots = 4;

    struct Adsr {
        uint8_t attack = 255;
        uint8_t decay = 0;
        uint8_t sustain = 255;
        uint8_t release = 165;
    };

    // UI thread: copy the rendered sample into a retired slot and publish a
    // note-on at midiKey. Returns false when no slot is retired
    // (pathological — the caller coalesces; the next drag tick retries).
    // toneKey is the voice's base key (ToneData.key): a keysplit sub-voice
    // can pitch its sample around a key other than middle C.
    bool publishNote(const QByteArray &s8, uint32_t freq, uint32_t loopStart,
                     bool looped, uint8_t midiKey, const Adsr &adsr,
                     uint8_t toneKey = 60);

    // UI thread: audition a CGB programmable wave (16 packed bytes = 32
    // nibbles). adsr carries CGB-range envelope values (attack/decay/release
    // 0-7, sustain 0-15). The wave bytes are copied into the slot — the CGB
    // wave channel reads them for the note's whole life, so the slot retires
    // only once no CGB channel references them anymore.
    bool publishWave(const QByteArray &wave16, uint8_t midiKey,
                     const Adsr &adsr);

    // UI thread: release the sounding audition (the slot stays busy until
    // its release envelope completes).
    void publishOff();

    // Audio thread, at the callback boundary: adopt the newest command and
    // refresh the retirement acknowledgment. track is the engine track the
    // audition plays on; its currentVoice is overwritten per note.
    void apply(M4AEngine *engine, int track);

    // Cold (device stopped / engine reinitialized): all channels are gone,
    // so every slot retires and any unadopted command is dropped.
    void reset();

private:
    struct Slot {
        ToneData tone = {};
        WaveData wave = {};
        std::vector<int8_t> bytes; // PCM (+interp pad) or 16 wave bytes
        uint64_t gen = 0; // UI thread: generation last published into it
    };
    Slot m_slots[kSlots];
    // Shared publish plumbing: pick a retired slot (-1 when none).
    int retiredSlot() const;
    // Publish command: generation << 16 | slot << 8 | key/flags (bit 7 of
    // the low byte = note-off; low 7 bits = MIDI key).
    std::atomic<uint64_t> m_publish{0};
    // Ack: adoptedGeneration << 8 | activeMask (bit s set while any engine
    // channel still references slot s's WaveData).
    std::atomic<uint64_t> m_ack{0};
    uint64_t m_gen = 0; // UI thread only

    // Audio-thread-only state.
    uint64_t m_adoptedGen = 0;
    int m_soundingKey = -1;
};
