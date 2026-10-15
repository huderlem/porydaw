#include "metronome.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr double kTwoPi = 6.283185307179586;
}

void ClickVoice::setSampleRate(double sampleRate)
{
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    m_attackFrames = std::max<uint32_t>(1, uint32_t(kAttackSeconds * m_sampleRate + 0.5));
    m_lengthFrames =
        std::max<uint32_t>(m_attackFrames + 1, uint32_t(kLengthSeconds * m_sampleRate));
    m_fadeFrames = std::clamp<uint32_t>(uint32_t(kFadeSeconds * m_sampleRate + 0.5), 1,
                                        m_lengthFrames - m_attackFrames);
    m_decayMul = float(std::exp(-1.0 / (kDecaySeconds * m_sampleRate)));
    cut();
}

void ClickVoice::cut()
{
    for (Slot &s : m_slots)
        s.remaining = 0;
}

void ClickVoice::start(bool accent)
{
    Slot &s = m_slots[m_next];
    m_next ^= 1;
    s.remaining = m_lengthFrames;
    s.age = 0;
    s.phase = 0.0;
    s.phaseInc = kTwoPi * (accent ? kAccentHz : kBeatHz) / m_sampleRate;
    s.peak = accent ? kAccentPeak : kBeatPeak;
    s.env = 1.0f;
}

void ClickVoice::render(float *out, uint32_t frames, const Trigger *triggers, uint32_t count,
                        float gain)
{
    uint32_t t = 0;
    for (uint32_t f = 0; f < frames;) {
        while (t < count && triggers[t].frame <= f)
            start(triggers[t++].accent);
        if (!ringing()) {
            // Skip silence to the next trigger, or out.
            if (t >= count || triggers[t].frame >= frames)
                return;
            f = triggers[t].frame;
            continue;
        }
        const uint32_t end = t < count ? std::min(frames, triggers[t].frame) : frames;
        for (; f < end; f++) {
            float v = 0.0f;
            for (Slot &s : m_slots) {
                if (s.remaining == 0)
                    continue;
                float amp;
                if (s.age < m_attackFrames) {
                    amp = float(s.age) / float(m_attackFrames);
                } else {
                    amp = s.env;
                    s.env *= m_decayMul;
                }
                if (s.remaining <= m_fadeFrames)
                    amp *= float(s.remaining) / float(m_fadeFrames + 1);
                v += float(std::sin(s.phase)) * amp * s.peak;
                s.phase += s.phaseInc;
                if (s.phase >= kTwoPi)
                    s.phase -= kTwoPi;
                s.age++;
                s.remaining--;
            }
            v *= gain;
            out[f * 2] += v;
            out[f * 2 + 1] += v;
        }
    }
}
