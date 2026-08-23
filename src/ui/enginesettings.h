#pragma once

// m4aSoundInit's selectable DirectSound rates (SOUND_MODE_FREQ_*), shared
// between the Settings window's Audio page and the Sample Editor's
// target-rate presets.
extern const int kGbaMixRates[12];

// Global poryaaaa GBA-accuracy knobs (SPEC §7), persisted per user via
// QSettings. Unlike Song Settings these never touch the project; reverb
// stays per-song (midi.cfg -R). Edited on the Settings window's Audio page.
struct EngineSettings {
    int maxPcmChannels = 5;      // pokeemerald m4aSoundInit default
    float pcmMixRate = 13379.0f; // GBA DirectSound rate; 0 = follow host rate
    bool analogFilter = false;   // GBA analog output low-pass; niche, off by default

    static EngineSettings load();
    void save() const;

    bool operator==(const EngineSettings &o) const
    {
        return maxPcmChannels == o.maxPcmChannels && pcmMixRate == o.pcmMixRate &&
               analogFilter == o.analogFilter;
    }
    bool operator!=(const EngineSettings &o) const { return !(*this == o); }
};
