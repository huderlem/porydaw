#pragma once

#include <QString>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>

#include "audio/audiotap.h"
#include "audio/auditionslots.h"
#include "core/captureevent.h"
#include "core/miditimeline.h"
#include "core/timelineplayer.h"

extern "C" {
#include "m4a_engine.h"
#include "voicegroup_loader.h"
}

struct ma_device;
struct ma_context;
struct MidiMsg;
class MidiInputRing;

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

    // MIDI keyboard live play (docs/midi-input/PLAN.md step 2). The audio
    // thread drains the input ring right after the timed previews and plays
    // each note on the live target track through the same active-note table,
    // as a "held" entry that never counts down: it sounds until its note-off
    // (deferred while the sustain pedal is down). Pitch bend, the CCs routed
    // to the MOD, VOL, PAN, BENDR and LFOS lanes (setLiveRoutes) and program
    // changes act on the whole target track, song notes included
    // (docs/midi-controls/PLAN.md §3.2); other CCs are ignored except 64
    // (sustain), 120/123 (all notes off, which also lifts the pedal) and
    // 121 (reset controllers: the pedal lifts, and the bend and every routed
    // lane go back to the engine's track-init values; the voice stays).
    //
    // Borrowed: the ring's owner must outlive the audio thread (shutdown()
    // before destroying it). Null drains nothing.
    void setMidiInputRing(MidiInputRing *ring)
    {
        m_midiRing.store(ring, std::memory_order_release);
    }
    // Hot: the track live notes play on; -1 (or no song loaded) drains the
    // input silently. A change releases the held and sustained notes on the
    // old track and re-chases the lanes (and voice) the keyboard moved there
    // at the playhead (TimelinePlayer::chaseTouched), so a parked wheel or
    // knob doesn't stick to a track you've left.
    void setLiveTarget(int track);
    int liveTarget() const { return m_liveTarget.load(); }
    // UI thread: the recording take `generation` follows its track from
    // engine index `from` to `to` (another track added, deleted or moved
    // before it), ahead of the rebuilt timeline's updateTimeline. That
    // swap moves the live target and the take's latch to the new index with
    // it, keeping the held lanes held, rather than a target change handing
    // the take's track back to the song. Until then setLiveTarget(to) keeps
    // `from`. Ignored when the take hasn't begun on the audio side.
    void followLiveTrack(uint32_t generation, int from, int to);
    // The mod wheel's full travel (0..127) maps onto 0..modMax: the target
    // track's MOD lane range as drawn, so the wheel covers what the lane
    // shows (MOD defaults to 0..16).
    void setLiveModMax(int modMax) { m_liveModMax.store(std::clamp(modMax, 1, 127)); }
    // Hot: which CC drives which lane (docs/midi-controls/PLAN.md §3.1).
    // The table is one packed word (MidiRoutes::packed), so it is published
    // whole with a single atomic store and nothing needs retiring; the audio
    // thread unpacks a changed word into its own CC → lane lookup at the next
    // callback. A routed CC's 0..127 is the lane's value as it is for VOL,
    // PAN and LFOS; MOD scales onto 0..modMax (setLiveModMax) and BENDR onto
    // 0..24 semitones (D2).
    void setLiveRoutes(uint64_t packedRoutes) { m_liveRoutes.store(packedRoutes); }
    // Hot: the active voicegroup's voice count (one past its last voice
    // line). A keyboard program change at or past it is ignored (J3) and
    // counted for the UI to report (liveIgnoredProgram).
    void setLiveVoiceCount(int count) { m_liveVoiceCount.store(std::clamp(count, 0, 128)); }
    int liveVoiceCount() const { return m_liveVoiceCount.load(); }
    // Telemetry: the last program change ignored for being past the
    // voicegroup's last voice, as sequence << 16 | voice count << 8 |
    // program (0 before the first), the count being the one it was ignored
    // against. The sequence counts them, so a poll that sees the word change
    // reports once however many arrived since.
    uint32_t liveIgnoredProgram() const
    {
        return m_liveIgnoredProgram.load(std::memory_order_relaxed);
    }
    // Hot: Settings → MIDI mirrors. Live play off drains the input silently
    // (like target -1); a fixed velocity of 1-127 overrides every note-on's,
    // 0 passes the keyboard's through.
    void setLivePlay(bool on) { m_livePlay.store(on); }
    void setLiveFixedVelocity(int velocity) { m_liveFixedVelocity.store(velocity); }
    // Hot: panic at the next callback: release every live note (held and
    // sustained) and lift the sustain pedal, whose pedal-up may never come
    // (a port that vanished with the pedal down).
    void liveAllNotesOff() { m_livePanicCmd.fetch_add(1); }
    // Hot: release every live note at the next callback, but keep the
    // pedal's state: it is still physically down (a tab switch).
    void liveReleaseNotes() { m_liveReleaseCmd.fetch_add(1); }
    // Telemetry: the keys sounding from the keyboard on the live target, as
    // a 128-bit mask (bit k of lo for keys 0-63, of hi for 64-127). Every
    // cut clears it with the notes.
    void liveHeldKeys(uint64_t *lo, uint64_t *hi) const
    {
        *lo = m_liveHeldLo.load(std::memory_order_relaxed);
        *hi = m_liveHeldHi.load(std::memory_order_relaxed);
    }

    // MIDI recording capture (docs/midi-input/PLAN.md step 3). While an arm
    // generation is set and the transport is Playing, every keyboard note
    // on/off the live path applies (after the sustain pedal and Fixed
    // velocity, whether or not live play sounds it) goes onto the capture
    // ring, stamped with the player position at the callback's start. So do
    // the take's structure: a Begin at the first capturing callback (the
    // punch-in), a Boundary at every loop wrap or seek, and a Cut whenever
    // the engine cuts the live notes or the app releases them (a panic, the
    // target changing). Arm *before* play() on a fresh start
    // so the first notes are caught; the Begin lands after the start's
    // seek. 0 disarms; each arm needs a new nonzero generation, which
    // stamps its events so a late one from an earlier arm is told apart.
    //
    // While a take captures, a lane the keyboard moves on the sounding target
    // latches (docs/midi-controls/PLAN.md §3.4, D3): the song's own events in
    // that lane on that track stop playing until the pass ends, so what you
    // hear is what the take commits (m_liveLatch has the rules).
    void armCapture(uint32_t generation) { m_captureGen.store(generation); }
    // UI thread: the single consumer. False when empty.
    bool popCapture(CaptureEvent *out);
    // Events lost to a full ring. The take tolerates the loss (an orphan
    // note-off is ignored, an orphan note on ends at the stop).
    uint32_t captureDropped() const { return m_captureDrops.load(std::memory_order_relaxed); }
    // The capturing player's position as of the last callback, and the
    // segment it was in (modulo RecordingTake::kSegmentMask), read together:
    // the stop position a take's still-held notes end at.
    void captureClock(uint64_t *samplePos, uint32_t *segment) const
    {
        const uint64_t packed = m_captureClock.load(std::memory_order_acquire);
        *samplePos = packed & kCaptureClockPosMask;
        *segment = uint32_t(packed >> kCaptureClockPosBits);
    }
    // The device's output latency, the "Auto" recording offset: you heard
    // the song about this long after the player rendered it.
    uint64_t outputLatencySamples() const
    {
        return uint64_t(m_periodFrames) * uint64_t(m_periodCount);
    }

    // Harness only (--midicheck): park the device so a check can drive the
    // callback itself, deterministically and faster than real time.
    // While held, cold operations leave the device stopped; pumpForTest
    // renders `frames` frames through the real callback into scratch.
    void holdDeviceForTest(bool hold);
    void pumpForTest(uint32_t frames);
    // Harness only: the main engine's state, read with the device held.
    const M4AEngine *engineForTest() const { return m_engine.get(); }
    // With the device held: the recording latch as the callback left it.
    LaneLatch liveLatchForTest() const { return m_liveLatch; }

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

    // The final stereo mix as the device hears it (post output gain),
    // for UI-side meters and plugin audio frames. Written every callback
    // whether or not a song plays, so auditions and ring-outs show too.
    const AudioTap &tap() const { return m_tap; }

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
    // Audio thread: the active-note table shared by timed previews and live
    // MIDI notes. startActive retriggers a sounding track+key in place,
    // else takes a free slot, else steals (a timed preview closest to its
    // note-off first, the oldest held note only when every slot is held).
    // releaseActive sends the note-off only for a key the table still
    // holds, so a key released after a cut can't stop a playback note.
    void startActive(uint8_t track, uint8_t key, uint8_t velocity, uint8_t volume, uint8_t pan,
                     int64_t remaining, bool held);
    bool releaseActive(uint8_t track, uint8_t key);
    void clearTimedPreviews();
    // Audio thread: MIDI keyboard input (see setMidiInputRing).
    void applyMidiInput();
    // Unpacks a changed m_liveRoutes into m_liveRouteOf.
    void syncLiveRoutes();
    void handleLiveMessage(const MidiMsg &msg);
    // The one place a keyboard note on (velocity > 0) or off is applied,
    // after the sustain pedal has had its say; a recording take captures it here,
    // before the sound target is consulted.
    void applyLiveNote(uint8_t key, uint8_t velocity);
    // The same for a bend (-8192..8191) and a routed CC lane (the lane's
    // document CC and the value it takes, already scaled): the whole
    // track's, captured whether or not it sounds (CaptureEvent::Bend and
    // CaptureEvent::Control). `reset`: CC121 put it back at rest (captured
    // with CaptureEvent::Reset).
    void applyLiveBend(int16_t bend, bool reset = false);
    void applyLiveControl(uint8_t lane, uint8_t value, bool reset = false);
    // A keyboard program change: the target track's voice, captured
    // whether or not it sounds (CaptureEvent::Program) unless the
    // voicegroup has no such voice (J3: ignored, and reported).
    void applyLiveProgram(uint8_t program);
    // The recording latch (docs/midi-controls/PLAN.md §3.4, m_liveLatch).
    // release: its lanes go back to the song at pos (chaseTouched) and it
    // clears; drop: it clears with no chase, for a caller whose full chase
    // (or fresh engine) already covers its lanes; reapply: after such a
    // chase, its lanes get the keyboard's values back (dropped instead once
    // the take is disarmed).
    void releaseLiveLatch(uint64_t pos);
    void dropLiveLatch() { m_liveLatch = LaneLatch(); }
    void reapplyLiveLatch();
    // Applies a note-off for every key the keyboard has on.
    void releaseLiveNotes();
    // The same on the app's say-so (a panic, a tab switch, the target
    // changing), not the keyboard's: a take's held notes end there as at a
    // cut.
    void imposeLiveRelease();
    // The pedal lifts: keys sounding only because of it release.
    void liftLivePedal();
    // Forgets the keyboard's keys without note-offs: the caller cut or
    // cleared the sound they had.
    void forgetLiveNotes();
    void publishLiveHeld();
    // Audio thread (or a cold op with the device stopped): recording capture.
    bool capturing() const;
    void syncCapture();
    // False when nothing was captured: no take, or the ring had no room.
    bool pushCapture(uint8_t kind, uint64_t samplePos, uint8_t key = 0, uint8_t velocity = 0,
                     uint64_t resumePos = 0, int16_t value = 0, uint8_t flags = 0);
    // The player jumped from `from` to `to`: the take's segment ends.
    void captureBoundary(uint64_t from, uint64_t to);
    void publishCaptureClock();
    // TimelinePlayer::settleWrap at the callback's top, with the take's
    // Boundary.
    void settleLoopWrap();
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
    AudioTap m_tap;

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
    // MIDI keyboard live play (UI writes, audio thread reads).
    std::atomic<MidiInputRing *> m_midiRing{nullptr};
    std::atomic<int> m_liveTarget{-1};
    std::atomic<int> m_liveModMax{127};
    std::atomic<int> m_liveVoiceCount{128};
    std::atomic<uint64_t> m_liveRoutes{
        0}; // MidiRoutes::packed(); the defaults from the constructor
    static_assert(std::atomic<uint64_t>::is_always_lock_free,
                  "the audio thread reads m_liveRoutes and must never lock");
    std::atomic<bool> m_livePlay{true};
    std::atomic<int> m_liveFixedVelocity{0};
    std::atomic<uint32_t> m_livePanicCmd{0};
    std::atomic<uint32_t> m_liveReleaseCmd{0};
    // Recording capture: the arm generation (UI writes), and an SPSC ring
    // from the audio thread to the UI. Cold ops push too, with the device
    // stopped, which hands the producer side over cleanly.
    std::atomic<uint32_t> m_captureGen{0};
    static constexpr uint32_t kCaptureRingSize = 4096;
    // The ring's last slots, which bend and lane moves can't take
    // (pushCapture): a quarter, held for notes, cuts and boundaries.
    static constexpr uint32_t kCaptureReserve = 1024;
    CaptureEvent m_captureRing[kCaptureRingSize];
    std::atomic<uint32_t> m_captureWrite{0}; // producer increments
    std::atomic<uint32_t> m_captureRead{0};  // consumer increments
    std::atomic<uint32_t> m_captureDrops{0};
    static constexpr int kCaptureClockPosBits = 44;
    static constexpr uint64_t kCaptureClockPosMask = (uint64_t(1) << kCaptureClockPosBits) - 1;
    std::atomic<uint64_t> m_captureClock{0}; // segment << 44 | position
    bool m_deviceHeldForTest = false;        // UI thread only

    // Telemetry (audio thread writes, UI reads)
    std::atomic<uint64_t> m_playhead{0};
    std::atomic<int> m_activePcm{0};
    std::atomic<int> m_activeCgb{0};
    std::atomic<uint64_t> m_liveHeldLo{0};
    std::atomic<uint64_t> m_liveHeldHi{0};
    std::atomic<uint32_t> m_liveIgnoredProgram{0};

    // Audio-thread-only sequencer state
    int m_appliedTransport = static_cast<int>(Transport::Stopped);
    uint32_t m_appliedMute = 0;
    uint64_t m_appliedPreview = 0;
    int m_previewTrack = -1; // sounding preview note, -1 when none
    int m_previewKey = -1;
    // Sounding timed previews, counting down to their note-offs, and live
    // MIDI notes (held: no countdown, released by their note-off).
    struct ActiveTimed {
        uint8_t track;
        uint8_t key;
        bool held;
        uint32_t order;    // start order, for stealing the oldest held note
        int64_t remaining; // samples until note-off; unused when held
    };
    ActiveTimed m_timedActive[kTimedMaxActive];
    int m_timedActiveCount = 0;
    uint32_t m_activeOrder = 0;
    // Live MIDI state (audio thread), per key of the omni keyboard.
    int m_liveTrack = -1;      // applied target; -1 sounds nothing
    bool m_liveDown[128] = {}; // physically held
    bool m_liveOn[128] = {};   // applied on: held, or released under the pedal
    bool m_liveSustain = false;
    // The message being applied waited this long for its callback
    // (pushCapture stamps it on a played note on/off, bend or lane move); 0
    // otherwise.
    uint32_t m_liveLate = 0;
    int64_t m_callbackNs = 0;      // midiClockNs at the callback's entry
    uint32_t m_callbackFrames = 0; // the callback's length
    // The lanes sent to m_liveTrack since it became the target: the ones a
    // target change hands back to the song.
    TouchedLanes m_liveTouched;
    // The recording latch (D3, J2): the lanes of m_liveTrack (its voice
    // included) whose song events render skips, because the keyboard moved
    // them, sounding, during the take. A lane latches on its first such
    // move; a CC121 reset latches only a lane the take already moved
    // (m_captureMoved), as only there is the reset recorded. It is always a
    // subset of m_liveTouched, on m_liveTrack.
    // It ends (the lanes chased back to the song) at a loop wrap, inside
    // render, to the song's loop-end state before the loop start's events;
    // at a seek; at the take's end (punch-out, a disarm, a new arm, Stop,
    // Pause); at a target change (live play off included); at a tab's
    // release; and with the song (load, unload). A panic doesn't end it. A
    // document edit's full chase keeps it: the lanes get the keyboard's
    // values back (reapplyLiveLatch), on the take's track's new index when
    // the edit moved it (followLiveTrack).
    LaneLatch m_liveLatch;
    // A take's track move waiting for updateTimeline (followLiveTrack); UI
    // thread only, so read by cold operations alone. Generation 0: none.
    struct LiveFollow {
        uint32_t generation = 0;
        int from = -1, to = -1;
    };
    LiveFollow m_liveFollow;
    // The lanes this arm's take moved (an unflagged move captured), on any
    // pass, sounding or not, as RecordingTake's own per-take mask: cleared at
    // the Begin. Bend and CC lanes only: it gates CC121's resets, which leave
    // the voice alone (J5).
    TouchedLanes m_captureMoved;
    // What the keyboard last set on m_liveTrack, per lane: reapplyLiveLatch's
    // values.
    uint8_t m_liveCcValue[128] = {};
    int16_t m_liveBendValue = 0;
    uint8_t m_liveProgramValue = 0;
    uint32_t m_liveIgnoredSeq = 0; // m_liveIgnoredProgram's sequence
    // The routes as last unpacked: the lane (document CC) each MIDI CC
    // drives, 0xFF for none, and which lanes have a CC at all (bit i for
    // MidiRoutes::kLanes[i]: the ones CC121 resets).
    uint64_t m_appliedLiveRoutes = 0; // never a valid packing, so the first sync unpacks
    uint8_t m_liveRouteOf[128] = {};
    uint8_t m_liveRoutedLanes = 0;
    uint32_t m_appliedLivePanic = 0;
    uint32_t m_captureBegun = 0;   // the generation whose Begin was pushed, 0 for none
    uint32_t m_captureSegment = 0; // segments since that Begin
    uint32_t m_appliedLiveRelease = 0;
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
