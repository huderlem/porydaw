#pragma once

#include <QString>
#include <atomic>
#include <cstdint>
#include <memory>

#include "core/miditimeline.h"
#include "core/timelineplayer.h"

extern "C" {
#include "m4a_engine.h"
#include "voicegroup_loader.h"
}

struct ma_device;
struct ma_context;

struct SongSettings {
    uint8_t songVolume = 127;   // mid2agb -V (0-127)
    uint8_t reverb = 0;         // mid2agb -R (0-127)
    uint8_t maxPcmChannels = 5; // pokeemerald m4aSoundInit default
    float pcmMixRate = 13379.0f; // GBA-accurate DirectSound mix rate
    bool analogFilter = false;  // GBA analog output low-pass (SPEC §7)
};

enum class Transport : int {
    Stopped = 0,
    Paused = 1,
    Playing = 2,
};

// Owns the audio output device (miniaudio), the poryaaaa M4AEngine instance,
// and the sequencer that walks a MidiTimeline on the audio thread.
//
// Thread model, split hot/cold:
//  - Cold operations (loadSong/unloadSong/shutdown) stop the device first, so
//    the audio thread is not running while engine/timeline/voicegroup pointers
//    are swapped. Call from the UI thread only.
//  - Hot operations (transport, mute/solo, loop) are single-writer atomics set
//    by the UI thread; the audio thread applies transitions at callback
//    boundaries (sending note-offs for newly muted tracks, etc.).
//  - Telemetry (playhead, active channels) is written by the audio thread into
//    atomics; polyphony-overflow counters are read directly from the engine
//    struct, which its header documents as safe for lock-free monitoring.
class AudioEngine
{
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine &) = delete;
    AudioEngine &operator=(const AudioEngine &) = delete;

    bool init(QString *error);
    void shutdown();
    double sampleRate() const { return m_sampleRate; }

    // Cold: swaps song data with the device stopped. Borrows both pointers —
    // the caller (the owning song tab) keeps ownership and must detach the
    // engine (another load call, or unloadSong) before freeing them.
    void loadSong(const MidiTimeline *timeline, LoadedVoiceGroup *voicegroup,
                  const SongSettings &settings);
    void unloadSong();

    // Cold: swaps in a rebuilt timeline after a document edit, preserving
    // transport state and the playhead position. Sounding notes are released
    // (their note-offs may have moved or vanished in the new timeline).
    // Borrowed like loadSong's; the old timeline may be freed once this
    // returns.
    void updateTimeline(const MidiTimeline *timeline);
    // Cold: jumps the playhead. Releases sounding notes and chases controller
    // state so CC/program/bend/tempo are exact at the landing position. Works
    // in any transport state; playing from Stopped starts wherever the last
    // seek (or the stop-time reset to 0) left the playhead.
    void seek(uint64_t samplePos);
    // Cold: re-applies song settings (master volume, reverb) to the engine.
    void updateSettings(const SongSettings &settings);
    // Cold: swaps the voicegroup (borrowed, like loadSong's); cuts all
    // sound. The old voicegroup may be freed once this returns.
    void updateVoicegroup(LoadedVoiceGroup *voicegroup);

    // Hot: audition a single note outside the timeline (piano-key click,
    // note-draw preview). velocity 0 releases. A new preview releases the
    // previous one, so at most one preview note sounds at a time.
    void previewNote(uint8_t track, uint8_t key, uint8_t velocity);

    // Hot: audition a note for a fixed length (band-sweep chord preview).
    // Unlike previewNote, timed previews stack polyphonically; the audio
    // thread sends each note-off itself once the duration elapses. velocity
    // 0 releases that track+key's preview early instead (durationSamples
    // ignored).
    void previewNoteTimed(uint8_t track, uint8_t key, uint8_t velocity,
                          uint32_t durationSamples);

    // Hot: audition a voicegroup entry by program number (SPEC §6.1 voicegroup
    // browser). Runs on a second engine instance (SPEC §3) so the program
    // change never disturbs playback track state. velocity 0 releases.
    void previewVoice(uint8_t voice, uint8_t key, uint8_t velocity);

    // Hot: re-copy every track's cached instrument from the voicegroup, so a
    // voice edit made through voiceForEdit is heard by already-playing tracks
    // from the next note on (applied at the next callback boundary).
    void refreshVoices() { m_refreshVoicesCmd.fetch_add(1); }

    // Cold: point voice previews at a different voicegroup (the import
    // wizard's mapping page). NOT owned — the caller must call this again
    // with nullptr (restoring the song's voicegroup) before freeing it.
    void setPreviewVoicegroup(LoadedVoiceGroup *voicegroup);

    bool songLoaded() const { return m_timeline != nullptr; }
    const MidiTimeline *timeline() const { return m_timeline; }
    const LoadedVoiceGroup *voicegroup() const { return m_voicegroup; }

    // Hot-safe for scalar field pokes only (byte-sized stores the audio
    // thread re-reads per event; both engine instances share this array).
    // Pointer fields must never be swapped through this — structural voice
    // changes go through updateVoicegroup.
    ToneData *voiceForEdit(int voice)
    {
        if (!m_voicegroup || voice < 0 || voice >= VOICEGROUP_SIZE)
            return nullptr;
        return &m_voicegroup->voices[voice];
    }

    // Hot transport controls.
    void play();
    void pause();
    void stop();
    Transport transport() const { return static_cast<Transport>(m_transport.load()); }
    void setLoopEnabled(bool enabled) { m_loopEnabled.store(enabled); }
    bool loopEnabled() const { return m_loopEnabled.load(); }
    void setMuteMask(uint32_t mask) { m_muteMask.store(mask); }
    void setSoloMask(uint32_t mask) { m_soloMask.store(mask); }

    // Telemetry.
    uint64_t playheadSamples() const { return m_playhead.load(); }
    int activePcmChannels() const { return m_activePcm.load(); }
    int activeCgbChannels() const { return m_activeCgb.load(); }
    int maxPcmChannels() const { return m_settings.maxPcmChannels; }
    uint64_t polyLostTotal() const; // dropped + stolen + tail-cut, all tracks

private:
    static void dataCallback(ma_device *device, void *output, const void *input,
                             uint32_t frameCount);
    void process(float *interleavedOut, uint32_t frameCount);
    void applyTransportTransition();
    void cutAllSound();
    void applyMuteTransition();
    void applyPreviewNote();
    void applyTimedPreviews(uint32_t frameCount);
    void clearTimedPreviews();
    void applyPreviewVoice();
    void resetPreviewEngine();
    ToneData *previewVoices() const;
    uint32_t effectiveMuteMask() const;

    // Device / engine (audio thread reads; cold ops swap while stopped)
    ma_context *m_context = nullptr;
    bool m_hasContext = false;
    ma_device *m_device = nullptr;
    bool m_deviceStarted = false;
    double m_sampleRate = 0.0;
    std::unique_ptr<M4AEngine> m_engine;
    const MidiTimeline *m_timeline = nullptr; // not owned (the active song tab's)
    LoadedVoiceGroup *m_voicegroup = nullptr; // not owned (the active song tab's)
    SongSettings m_settings;
    // Audition instance: voice previews only, mixed on top of the main engine.
    std::unique_ptr<M4AEngine> m_previewEngine;
    LoadedVoiceGroup *m_previewOverrideVg = nullptr; // not owned

    // Hot control state (UI writes, audio thread reads)
    std::atomic<int> m_transport{static_cast<int>(Transport::Stopped)};
    std::atomic<bool> m_loopEnabled{true};
    std::atomic<uint32_t> m_muteMask{0};
    std::atomic<uint32_t> m_soloMask{0};
    // Preview-note command: generation<<24 | track<<16 | key<<8 | velocity.
    // The generation counter makes every request distinct so repeated notes
    // are seen by the audio thread.
    std::atomic<uint32_t> m_previewCmd{0};
    uint8_t m_previewGen = 0; // UI thread only
    // Timed-preview commands (band-sweep chord audition): a fixed SPSC ring.
    // The UI thread produces at m_timedWrite; the audio thread consumes at
    // m_timedRead, starting each note and releasing it when its duration
    // elapses. A full ring drops the preview, which is harmless.
    struct TimedPreview {
        uint8_t track;
        uint8_t key;
        uint8_t velocity;
        uint32_t durationSamples;
    };
    static constexpr uint32_t kTimedRingSize = 64;
    static constexpr int kTimedMaxActive = 24;
    TimedPreview m_timedRing[kTimedRingSize];
    std::atomic<uint32_t> m_timedWrite{0}; // UI thread increments
    std::atomic<uint32_t> m_timedRead{0};  // audio thread increments
    // Voice-preview command: generation<<32 | voice<<16 | key<<8 | velocity.
    std::atomic<uint64_t> m_previewVoiceCmd{0};
    uint8_t m_previewVoiceGen = 0; // UI thread only
    // Refresh-voices command: bumped by the UI, applied at callback boundary.
    std::atomic<uint32_t> m_refreshVoicesCmd{0};

    // Telemetry (audio thread writes, UI reads)
    std::atomic<uint64_t> m_playhead{0};
    std::atomic<int> m_activePcm{0};
    std::atomic<int> m_activeCgb{0};

    // Audio-thread-only sequencer state
    int m_appliedTransport = static_cast<int>(Transport::Stopped);
    uint32_t m_appliedMute = 0;
    uint32_t m_appliedPreview = 0;
    int m_previewTrack = -1; // sounding preview note, -1 when none
    int m_previewKey = -1;
    // Sounding timed previews, counting down to their note-offs.
    struct ActiveTimed {
        uint8_t track;
        uint8_t key;
        int64_t remaining; // samples until note-off
    };
    ActiveTimed m_timedActive[kTimedMaxActive];
    int m_timedActiveCount = 0;
    uint64_t m_appliedPreviewVoice = 0;
    int m_previewVoiceKey = -1; // sounding voice-preview note, -1 when none
    uint32_t m_appliedRefreshVoices = 0;
    TimelinePlayer m_player;

    // Scratch deinterleave buffers (allocated in init)
    std::unique_ptr<float[]> m_bufL;
    std::unique_ptr<float[]> m_bufR;
    std::unique_ptr<float[]> m_pvL; // voice-preview engine mix
    std::unique_ptr<float[]> m_pvR;
    uint32_t m_bufCapacity = 0;
};
