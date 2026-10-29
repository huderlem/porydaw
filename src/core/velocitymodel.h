#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

extern "C" {
#include "m4a_engine.h"
}

// How a track's velocities reach the hardware. A note's stored MIDI velocity
// is continuous (1-127) on DirectSound voices, but the CGB channels have only
// a handful of real loudness steps, so several stored values sound identical.
// VelocityMap answers, for one resolved voice at one track volume and pan,
// which stored values collapse together and which value best represents each
// step.
//
// The track volume is part of the question, not a trim applied after it. A
// CGB channel's loudness is a 4-bit envelope goal the engine derives from
// velocity AND volume together (ChnVolSetAsm then CgbModVol), so halving the
// volume halves the number of steps velocity can still reach: at full volume
// a square channel has 16, at volume 64 it has 8, and below about volume 16
// it has none at all — the channel is silent whatever the velocity says.
enum class VelocityVoice : uint8_t {
    Unresolved, // no voice at all (no program, no voicegroup)
    Invalid,    // a voice that has no velocity behavior to model (cry, nested keysplit)
    Keyless,    // a keysplit asked about without a key to resolve it with
    DirectSound,
    Square1,
    Square2,
    Wave,
    Noise,
};

// The inclusive stored-velocity span that shares one hardware level.
struct VelocityLevelRange {
    uint8_t first = 1;
    uint8_t last = 127;
};

// mxv in MPlayDef.s: the divisor both mid2agb and the engine scale volumes by.
constexpr int kM4aMaxVolume = 127;

// The VOL byte a compiled song actually carries. mid2agb emits every volume as
// `value*<song>_mvl/mxv` (agb.cpp PrintOp), so the assembler's integer
// division bakes the -V master volume into the track's own volume; the engine
// repeats the same arithmetic at runtime (m4a_engine_set_song_volume) when the
// master volume is auditioned live. Both arguments are 0-127.
uint8_t m4aEffectiveTrackVolume(int volumeEvent, int masterVolume);

class VelocityMap
{
  public:
    VelocityMap() = default;
    // key resolves keysplit voices; without one a keysplit stays unresolved
    // rather than guessing a child voice. trackVolume is the effective VOL
    // byte in force where the question is being asked (m4aEffectiveTrackVolume
    // of the track's VOL and the song's master volume) — it decides how many
    // of the channel's levels velocity can still reach. trackPan is the PAN in
    // force there in engine units (-64..63, i.e. the CC10 byte less 64): it
    // moves the boundaries between those levels, so a panned track detents at
    // different velocities than a centered one.
    static VelocityMap resolve(const ToneData *tone, std::optional<uint8_t> key,
                               uint8_t trackVolume, int8_t trackPan = 0);
    // A keysplit asked about without a key: the section plays on whichever
    // channel each key resolves to, so it has no one level table of its own.
    bool isKeyless() const;
    // True only for the four CGB channels, the voices with real detents.
    bool isPsg() const;
    // A CGB channel with more than one loudness step left to snap between.
    // A channel the volume has squeezed down to a single (silent) step has
    // nothing to detent, so the lane keeps the plain velocity ruler there.
    bool hasDetents() const;
    uint8_t trackVolume() const { return m_trackVolume; }
    int8_t trackPan() const { return m_trackPan; }
    bool operator==(const VelocityMap &other) const;
    bool operator!=(const VelocityMap &other) const;
    // Whether two notes can share one intrinsic (detent) editing context:
    // both PSG, the same channel, and under the same track volume and pan,
    // since all of those change the level table.
    bool compatibleWith(const VelocityMap &other) const;
    const char *voiceName() const;
    // 0 on non-PSG voices — a continuous voice has no levels to count. On a
    // CGB channel it is how many of that channel's loudness steps the track's
    // volume still leaves reachable, so it falls with the volume.
    std::size_t levelCount() const;
    VelocityLevelRange levelRange(int requestedLevel) const;
    std::optional<std::size_t> levelOf(int storedVelocity) const;
    uint8_t representative(int requestedLevel) const;
    // The representative of the level a value falls in; the value unchanged
    // on continuous voices.
    uint8_t canonicalize(int proposedVelocity) const;
    // Steps delta levels from a value that need not be a representative, so a
    // gesture that returns to its origin level restores the exact origin.
    uint8_t moveLevels(uint8_t exactOrigin, int delta) const;

  private:
    VelocityMap(VelocityVoice voice, uint8_t trackVolume, int8_t trackPan)
        : m_voice(voice)
        , m_trackVolume(trackVolume)
        , m_trackPan(trackPan)
    {}
    // The level a stored velocity reaches, without the isPsg guard.
    std::size_t levelAt(int storedVelocity) const;

    VelocityVoice m_voice = VelocityVoice::Unresolved;
    uint8_t m_trackVolume = uint8_t(kM4aMaxVolume);
    int8_t m_trackPan = 0;
};
