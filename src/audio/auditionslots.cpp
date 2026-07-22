#include "auditionslots.h"

int AuditionSlots::retiredSlot() const
{
    // A slot is retired when it was never used, or its last publish has been
    // adopted (generation acknowledged-superseded) AND no engine channel
    // still references its data. With N = 4 and one sounding audition a
    // free slot always exists; if none is retired, drop the re-render — the
    // caller coalesces and the next drag tick retries.
    const uint64_t ack = m_ack.load(std::memory_order_acquire);
    const uint64_t ackGen = ack >> 8;
    const uint32_t active = uint32_t(ack) & 0xFF;
    for (int s = 0; s < kSlots; s++) {
        if (m_slots[s].gen != 0
            && (m_slots[s].gen > ackGen || ((active >> s) & 1)))
            continue;
        return s;
    }
    return -1;
}

bool AuditionSlots::publishNote(const QByteArray &s8, uint32_t freq,
                                uint32_t loopStart, bool looped,
                                uint8_t midiKey, const Adsr &adsr,
                                uint8_t toneKey)
{
    if (s8.isEmpty())
        return false;
    const int slot = retiredSlot();
    if (slot < 0)
        return false;

    Slot &sl = m_slots[slot];
    sl.bytes.assign(reinterpret_cast<const int8_t *>(s8.constData()),
                    reinterpret_cast<const int8_t *>(s8.constData())
                        + s8.size());
    // The engine's interpolating mixer reads one sample past the current
    // position (m4a_pcm_channel_render's s1 lookahead); the voicegroup
    // loader pads every WaveData with a repeat of the final sample for
    // exactly this. Mirror it — size stays the real sample count.
    sl.bytes.push_back(sl.bytes.back());
    sl.wave.type = 0;
    sl.wave.status = looped ? 0x4000 : 0;
    sl.wave.freq = freq;
    sl.wave.loopStart = looped ? loopStart : 0;
    sl.wave.size = uint32_t(sl.bytes.size() - 1);
    sl.wave.data = sl.bytes.data();
    sl.tone = ToneData{};
    sl.tone.type = VOICE_DIRECTSOUND;
    sl.tone.key = toneKey; // usually 60, the project-conventional base key
    sl.tone.wav = &sl.wave;
    sl.tone.attack = adsr.attack;
    sl.tone.decay = adsr.decay;
    sl.tone.sustain = adsr.sustain;
    sl.tone.release = adsr.release;

    m_gen++;
    sl.gen = m_gen;
    m_publish.store((m_gen << 16) | (uint64_t(slot) << 8)
                        | uint64_t(midiKey & 0x7F),
                    std::memory_order_release);
    return true;
}

bool AuditionSlots::publishWave(const QByteArray &wave16, uint8_t midiKey,
                                const Adsr &adsr)
{
    if (wave16.size() != 16)
        return false;
    const int slot = retiredSlot();
    if (slot < 0)
        return false;

    Slot &sl = m_slots[slot];
    sl.bytes.assign(reinterpret_cast<const int8_t *>(wave16.constData()),
                    reinterpret_cast<const int8_t *>(wave16.constData()) + 16);
    // The vector's allocation is operator-new aligned, so the byte buffer
    // can carry the engine's word-typed wave pointer.
    sl.wave = WaveData{};
    sl.tone = ToneData{};
    sl.tone.type = VOICE_PROGRAMMABLE_WAVE;
    sl.tone.key = 60;
    sl.tone.wavePointer = reinterpret_cast<uint32_t *>(sl.bytes.data());
    sl.tone.attack = adsr.attack & 0x07;
    sl.tone.decay = adsr.decay & 0x07;
    sl.tone.sustain = adsr.sustain & 0x0F;
    sl.tone.release = adsr.release & 0x07;

    m_gen++;
    sl.gen = m_gen;
    m_publish.store((m_gen << 16) | (uint64_t(slot) << 8)
                        | uint64_t(midiKey & 0x7F),
                    std::memory_order_release);
    return true;
}

void AuditionSlots::publishOff()
{
    if (m_gen == 0)
        return; // nothing was ever published
    m_gen++;
    m_publish.store((m_gen << 16) | 0x80, std::memory_order_release);
}

void AuditionSlots::apply(M4AEngine *engine, int track)
{
    if (!engine || track < 0 || track >= MAX_TRACKS)
        return;
    const uint64_t cmd = m_publish.load(std::memory_order_acquire);
    const uint64_t gen = cmd >> 16;
    if (gen != m_adoptedGen) {
        // Note-off the currently-sounding audition before keying the next
        // one; its channel keeps releasing against its own slot's data.
        if (m_soundingKey >= 0) {
            m4a_engine_note_off(engine, track, uint8_t(m_soundingKey));
            m_soundingKey = -1;
        }
        const uint8_t low = uint8_t(cmd & 0xFF);
        if (!(low & 0x80)) {
            const int slot = int((cmd >> 8) & 0xFF) % kSlots;
            engine->tracks[track].currentVoice = m_slots[slot].tone;
            // Live audition semantics, like the other previews: no timeline
            // position for overflow events, audible in solo-overflow mode.
            engine->polyEventClock = M4A_POLY_TICK_NONE;
            engine->auditionNote = true;
            m4a_engine_note_on(engine, track, low & 0x7F, 127);
            m_soundingKey = low & 0x7F;
        }
        m_adoptedGen = gen;
    }

    // Retirement: a slot stays busy while any channel (sounding or
    // releasing) still reads its data — PCM channels reference the slot's
    // WaveData, the CGB wave channel references the slot's wave bytes.
    uint32_t mask = 0;
    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++) {
        const M4APCMChannel &ch = engine->pcmChannels[i];
        if (!(ch.status & CHN_ON) || !ch.wav)
            continue;
        for (int s = 0; s < kSlots; s++) {
            if (ch.wav == &m_slots[s].wave)
                mask |= 1u << s;
        }
    }
    for (int i = 0; i < TOTAL_CGB_CHANNELS; i++) {
        const M4ACGBChannel &ch = engine->cgbChannels[i];
        if (!(ch.status & CHN_ON) || !ch.wavePointer)
            continue;
        for (int s = 0; s < kSlots; s++) {
            const Slot &sl = m_slots[s];
            if (sl.tone.type == VOICE_PROGRAMMABLE_WAVE
                && ch.wavePointer == sl.tone.wavePointer)
                mask |= 1u << s;
        }
    }
    m_ack.store((m_adoptedGen << 8) | mask, std::memory_order_release);
}

void AuditionSlots::reset()
{
    // Cold path: the engine was reinitialized (or destroyed), so no channel
    // references any slot and an unadopted command has nothing to play on.
    const uint64_t cmd = m_publish.load(std::memory_order_acquire);
    m_adoptedGen = cmd >> 16;
    m_soundingKey = -1;
    m_ack.store(m_adoptedGen << 8, std::memory_order_release);
}
