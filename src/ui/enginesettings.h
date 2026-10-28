#pragma once

// m4aSoundInit's selectable DirectSound rates (SOUND_MODE_FREQ_*, the
// engine's PCM frequency modes 1..12 in order), shared between the Settings
// window's Audio page and the Sample Editor's target-rate presets.
extern const int kGbaMixRates[12];

// The mix rate the HQ mixer cannot run at (its frame is not a multiple of
// 8); the engine snaps it to the default while HQ mixing is on.
constexpr int kGbaMixRateHqUnsupported = 7884;

// True for one of kGbaMixRates.
bool isGbaMixRate(int rate);

// Global poryaaaa GBA-accuracy knobs (SPEC §7), persisted per user via
// QSettings. Unlike Song Settings these never touch the project; reverb
// stays per-song (midi.cfg -R). Edited on the Settings window's Audio page.
struct EngineSettings {
    int maxPcmChannels = 5;      // pokeemerald m4aSoundInit default
    float pcmMixRate = 13379.0f; // GBA DirectSound rate, one of kGbaMixRates
    bool hqMixer = false;        // ipatix's HQ mixer in place of the stock SoundMainRAM

    static EngineSettings load();
    void save() const;

    bool operator==(const EngineSettings &o) const
    {
        return maxPcmChannels == o.maxPcmChannels && pcmMixRate == o.pcmMixRate &&
               hqMixer == o.hqMixer;
    }
    bool operator!=(const EngineSettings &o) const { return !(*this == o); }
};
