#pragma once

#include <cstdint>

// The metronome's sound (docs/metronome/PLAN.md M1): a short sine blip with
// a 1 ms attack and a fast exponential decay, higher on the downbeat, faded
// linearly to zero over its last few ms so it ends without a step.
// Synthesized on the audio thread and mixed after the GBA engine, so it
// takes no GBA channel and no song reverb. Allocation-free; at most two
// clicks ring at once (a new one replaces the oldest).
class ClickVoice
{
  public:
    // A click due `frame` frames into the block render() is given.
    struct Trigger {
        uint32_t frame;
        bool accent;
    };

    static constexpr double kAccentHz = 1568.0; // G6
    static constexpr double kBeatHz = 1046.5;   // C6
    // Peak level at 100% metronome volume and unity output gain.
    static constexpr float kAccentPeak = 0.5f;
    static constexpr float kBeatPeak = 0.38f;
    static constexpr double kAttackSeconds = 0.001;
    static constexpr double kDecaySeconds = 0.012; // e-folding time
    static constexpr double kLengthSeconds = 0.06;
    // The closing fade: the decay is about 1% of peak here, faded to 0.
    static constexpr double kFadeSeconds = 0.005;

    void setSampleRate(double sampleRate);

    // Adds the ringing clicks plus the given triggers (sorted by frame,
    // each < frames) into interleaved stereo `out`, scaled by gain.
    void render(float *out, uint32_t frames, const Trigger *triggers, uint32_t count, float gain);

    // Silences every ringing click at once (stop, seek).
    void cut();

    // A click's length in frames at the current sample rate.
    uint32_t lengthFrames() const { return m_lengthFrames; }

    bool ringing() const { return m_slots[0].remaining > 0 || m_slots[1].remaining > 0; }

  private:
    struct Slot {
        uint32_t remaining = 0; // frames left; 0 = silent
        uint32_t age = 0;       // frames since the trigger
        double phase = 0.0;     // radians
        double phaseInc = 0.0;
        float peak = 0.0f;
        float env = 0.0f; // the decay envelope after the attack
    };
    void start(bool accent);

    Slot m_slots[2];
    int m_next = 0; // the slot the next click takes
    double m_sampleRate = 48000.0;
    uint32_t m_attackFrames = 48;
    uint32_t m_lengthFrames = 2880;
    uint32_t m_fadeFrames = 240;
    float m_decayMul = 0.998f;
};
