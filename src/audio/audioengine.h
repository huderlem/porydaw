#pragma once

#include <QString>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>

#include "audio/auditionslots.h"
#include "core/miditimeline.h"
#include "core/timelineplayer.h"

extern "C" {
#include "m4a_engine.h"
#include "voicegroup_loader.h"
}

struct ma_device;
struct ma_context;

struct SongSettings {
    uint8_t songVolume = 127;    // mid2agb -V (0-127)
    uint8_t reverb = 0;          // mid2agb -R (0-127)
    uint8_t maxPcmChannels = 5;  // pokeemerald m4aSoundInit default
    uint8_t trackBudget = 16;    // song's music player track count (SongDocument)
    float pcmMixRate = 13379.0f; // GBA-accurate DirectSound mix rate
    bool analogFilter = false;   // GBA analog output low-pass (SPEC §7)
};

// Mute bits for the tracks a song's music player never starts: MPlayStart
// stops at min(song tracks, player tracks), so tracks at or beyond the
// budget are silent in-game and porydaw plays them the same way.
inline uint32_t trackBudgetMuteMask(int trackBudget)
{
    if (trackBudget >= MAX_TRACKS)
        return 0;
    return (0xFFFFu << std::max(0, trackBudget)) & 0xFFFFu;
}

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
    // Which miniaudio backend the device landed on ("PulseAudio", "ALSA",
    // "Null", ...). The null device keeps the sequencer running with no
    // sound — deliberate for headless harnesses, a trap for real users, so
    // the UI must surface it.
    QString backendName() const { return m_backendName; }
    bool usingNullBackend() const { return m_isNullBackend; }
    // The device's resolved buffering (per-period frames × period count),
    // for diagnostics: underruns on slow transports (WSLg's RDP audio)
    // show up as crackling, and the first support question is "how big are
    // the periods really?".
    int periodSizeFrames() const { return m_periodFrames; }
    int periodCount() const { return m_periodCount; }

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
    // Hot: requests a jump at the next audio callback. Releases sounding
    // notes and chases controller state at the landing position. Works in
    // any transport state; playing from Stopped starts wherever the last
    // seek (or the stop-time reset to 0) left the playhead. Takes effect
    // within one audio period — read the target back, not playheadSamples(),
    // when acting immediately after.
    void seek(uint64_t samplePos);
    // Cold: re-applies song settings (master volume, reverb) to the engine.
    void updateSettings(const SongSettings &settings);
    // Cold: swaps the voicegroup (borrowed, like loadSong's); cuts all
    // sound. The old voicegroup may be freed once this returns.
    void updateVoicegroup(LoadedVoiceGroup *voicegroup);

    // Hot: audition a single note outside the timeline (piano-key click,
    // note-draw preview). velocity 0 releases. A new preview releases the
    // previous one, so at most one preview note sounds at a time.
    // rawVolume is the track's VOL byte (0-127, before the song's master
    // volume) the note should sound at; -1 uses whatever VOL the engine's
    // track currently holds — i.e. the one chased to the playhead. pan is the
    // track's PAN (-64..63) the note should sound at, with the same fallback
    // for kPreviewPanNone: it places the sound and, on a CGB channel, moves
    // the envelope goal the note is heard at.
    void previewNote(uint8_t track, uint8_t key, uint8_t velocity, int rawVolume = kPreviewVolNone,
                     int pan = kPreviewPanNone);

    // Hot: audition a note for a fixed length (band-sweep chord preview).
    // Unlike previewNote, timed previews stack polyphonically; the audio
    // thread sends each note-off itself once the duration elapses. velocity
    // 0 releases that track+key's preview early instead (durationSamples
    // ignored). rawVolume and pan as in previewNote.
    void previewNoteTimed(uint8_t track, uint8_t key, uint8_t velocity, uint32_t durationSamples,
                          int rawVolume = kPreviewVolNone, int pan = kPreviewPanNone);

    // "Sound this preview at the track's current VOL" — the caller has no
    // particular volume in mind.
    static constexpr int kPreviewVolNone = -1;
    // The same for PAN. Every value in -64..63 is a real pan, so the
    // no-override marker is the engine's own out-of-range sentinel.
    static constexpr int kPreviewPanNone = M4A_AUDITION_PAN_NONE;

    // Hot: audition a voicegroup entry by program number (SPEC §6.1 voicegroup
    // browser). Runs on a second engine instance (SPEC §3) so the program
    // change never disturbs playback track state. velocity 0 releases.
    void previewVoice(uint8_t voice, uint8_t key, uint8_t velocity);

    // Hot: Sample Editor in-memory sample audition (PLAN.md §4). Plays the
    // rendered s8 bytes through the audition engine instance on a dedicated
    // track, so an unregistered sample is heard with the engine's real
    // fetch/loop/envelope math. A new publish releases the previous
    // audition. Returns false when every slot is still busy (the caller
    // coalesces — retry on the next re-render).
    bool auditionSample(const QByteArray &s8, uint32_t freq, uint32_t loopStart, bool looped,
                        uint8_t key, const AuditionSlots::Adsr &adsr, uint8_t toneKey = 60)
    {
        return m_audition.publishNote(s8, freq, loopStart, looped, key, adsr, toneKey);
    }
    // CGB programmable-wave audition (16 packed bytes; CGB-range adsr).
    bool auditionWave(const QByteArray &wave16, uint8_t key, const AuditionSlots::Adsr &adsr)
    {
        return m_audition.publishWave(wave16, key, adsr);
    }
    void auditionSampleOff() { m_audition.publishOff(); }

    // Hot: re-copy every track's cached instrument from the voicegroup, so a
    // voice edit made through voiceForEdit is heard by already-playing tracks
    // from the next note on (applied at the next callback boundary).
    void refreshVoices() { m_refreshVoicesCmd.fetch_add(1); }

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
    // Hot: porydaw's own listening level — a linear gain on the final mix
    // (song playback and every audition alike), applied per callback. It is
    // not an m4a concept: never part of the song, never in exported WAVs.
    // 1.0 is unity; above it the device's float stream is left to clip.
    void setOutputGain(float gain) { m_outputGain.store(gain); }
    float outputGain() const { return m_outputGain.load(); }

    // Hot: polyphony-overflow debug mode — mutes normal playback and plays
    // only the sounds lost to the polyphony limit (SPEC §6.1 Polyphony dock).
    // Applied at the callback boundary against the live engine field, so it
    // re-asserts itself after loadSong reinitializes the engine: the mode is
    // session-sticky (survives play/stop and song switches) but never
    // persisted.
    void setPolyDebugInvert(bool on) { m_polyInvert.store(on); }
    bool polyDebugInvert() const { return m_polyInvert.load(); }
    // Hot: zero the overflow counters and event ring at the next callback
    // boundary (a GUI-thread reset would race the audio thread's writes).
    void resetPolyStats() { m_polyResetCmd.fetch_add(1); }

    // Telemetry.
    uint64_t playheadSamples() const { return m_playhead.load(); }
    int activePcmChannels() const { return m_activePcm.load(); }
    int activeCgbChannels() const { return m_activeCgb.load(); }
    int maxPcmChannels() const { return m_settings.maxPcmChannels; }
    int trackBudget() const { return m_settings.trackBudget; }
    uint64_t polyLostTotal() const; // dropped + stolen, all tracks (no tail cuts)

    // GUI-thread copy of the engine's polyphony-overflow state (the engine
    // header documents these fields as safe for lock-free monitor reads).
    // If more than M4A_POLY_EVENT_CAPACITY events land between two polls the
    // oldest ring rows may be torn or stale — benign for a debug display;
    // eventTotal is always exact.
    struct PolyChannel {
        bool on = false;
        bool releasing = false; // CHN_STOP | CHN_IEC: fading out
        uint8_t track = 0;
        uint8_t midiKey = 0;
    };
    struct PolySnapshot {
        uint8_t maxPcmChannels = 0;
        bool invert = false;
        // Second half of each array is the shadow pool (lost sounds).
        PolyChannel pcm[TOTAL_PCM_CHANNELS];
        PolyChannel cgb[TOTAL_CGB_CHANNELS];
        uint32_t drop[MAX_TRACKS] = {};
        uint32_t steal[MAX_TRACKS] = {};
        uint32_t tailCut[MAX_TRACKS] = {};
        uint32_t eventTotal = 0;
        M4APolyEvent events[M4A_POLY_EVENT_CAPACITY] = {};
    };
    void polySnapshot(PolySnapshot *out) const;

  private:
    static void dataCallback(ma_device *device, void *output, const void *input,
                             uint32_t frameCount);
    void process(float *interleavedOut, uint32_t frameCount);
    void applyPendingSeek();
    void applyTransportTransition();
    void cutAllSound();
    void applyMuteTransition();
    void applyPreviewNote();
    void applyTimedPreviews(uint32_t frameCount);
    // Audio thread: key an audition note at a specific track VOL (0xFF = the
    // track's current one), through TimelinePlayer::auditionNoteOn.
    void previewNoteOn(uint8_t track, uint8_t key, uint8_t velocity, uint8_t volume, uint8_t pan);
    void clearTimedPreviews();
    void applyPreviewVoice();
    void applyPolyDebug();
    void resetPreviewEngine();
    ToneData *previewVoices() const;
    uint32_t effectiveMuteMask() const;

    // Device / engine (audio thread reads; cold ops swap while stopped)
    ma_context *m_context = nullptr;
    bool m_hasContext = false;
    ma_device *m_device = nullptr;
    bool m_deviceStarted = false;
    double m_sampleRate = 0.0;
    QString m_backendName;
    bool m_isNullBackend = false;
    int m_periodFrames = 0;
    int m_periodCount = 0;
    std::unique_ptr<M4AEngine> m_engine;
    const MidiTimeline *m_timeline = nullptr; // not owned (the active song tab's)
    LoadedVoiceGroup *m_voicegroup = nullptr; // not owned (the active song tab's)
    SongSettings m_settings;
    // Audition instance: voice previews and sample auditions, mixed on top
    // of the main engine.
    std::unique_ptr<M4AEngine> m_previewEngine;
    // Sample Editor audition slots (PLAN.md §4), played on the audition
    // instance's track 1 (previewVoice owns track 0).
    static constexpr int kAuditionTrack = 1;
    AuditionSlots m_audition;

    // Hot control state (UI writes, audio thread reads)
    std::atomic<int> m_transport{static_cast<int>(Transport::Stopped)};
    std::atomic<bool> m_loopEnabled{true};
    std::atomic<uint32_t> m_muteMask{0};
    std::atomic<uint32_t> m_soloMask{0};
    std::atomic<float> m_outputGain{1.0f};
    // Avoid stop/start stalls: publish the latest seek for the audio callback.
    static constexpr uint64_t kNoPendingSeek = UINT64_MAX;
    std::atomic<uint64_t> m_pendingSeek{kNoPendingSeek};
    // Preview-note command:
    // pan<<40 | generation<<32 | volume<<24 | track<<16 | key<<8 | velocity,
    // where the volume byte is 0xFF for kPreviewVolNone and the pan byte is
    // the PAN plus 64 (0-127), 0xFF for kPreviewPanNone. The generation
    // counter makes every request distinct so repeated notes are seen by the
    // audio thread.
    std::atomic<uint64_t> m_previewCmd{0};
    uint8_t m_previewGen = 0; // UI thread only
    // Timed-preview commands (band-sweep chord audition): a fixed SPSC ring.
    // The UI thread produces at m_timedWrite; the audio thread consumes at
    // m_timedRead, starting each note and releasing it when its duration
    // elapses. A full ring drops the preview, which is harmless.
    struct TimedPreview {
        uint8_t track;
        uint8_t key;
        uint8_t velocity;
        uint8_t volume; // track VOL byte, 0xFF for kPreviewVolNone
        uint8_t pan;    // PAN + 64, 0xFF for kPreviewPanNone
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
    // Polyphony-overflow debug: desired invert state + reset command.
    std::atomic<bool> m_polyInvert{false};
    std::atomic<uint32_t> m_polyResetCmd{0};

    // Telemetry (audio thread writes, UI reads)
    std::atomic<uint64_t> m_playhead{0};
    std::atomic<int> m_activePcm{0};
    std::atomic<int> m_activeCgb{0};

    // Audio-thread-only sequencer state
    int m_appliedTransport = static_cast<int>(Transport::Stopped);
    uint32_t m_appliedMute = 0;
    uint64_t m_appliedPreview = 0;
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
    uint32_t m_appliedPolyReset = 0;
    TimelinePlayer m_player;

    // Scratch deinterleave buffers (allocated in init)
    std::unique_ptr<float[]> m_bufL;
    std::unique_ptr<float[]> m_bufR;
    std::unique_ptr<float[]> m_pvL; // voice-preview engine mix
    std::unique_ptr<float[]> m_pvR;
    uint32_t m_bufCapacity = 0;
};
