#include "audioengine.h"

#include <QFile>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <vector>

#include "audio/midiinput.h"
#include "audio/midiroutes.h"
#include "miniaudio.h"

#ifdef __linux__
#include <dlfcn.h>

namespace {

// Install a no-op error handler into libasound to suppress the wall of
// "cannot find card '0'" messages ALSA prints on WSL and other systems
// without hardware audio. Uses dlopen so we don't link libasound explicitly.
void alsaErrorNoop(const char *, int, const char *, int, const char *, ...) {}

void suppressAlsaErrors()
{
    void *lib = dlopen("libasound.so.2", RTLD_LAZY);
    if (!lib)
        return;
    typedef void (*ErrFn)(const char *, int, const char *, int, const char *, ...);
    typedef void (*SetFn)(ErrFn);
    SetFn setfn;
    *reinterpret_cast<void **>(&setfn) = dlsym(lib, "snd_lib_error_set_handler");
    if (setfn)
        setfn(alsaErrorNoop);
    // Leave the handle open so the handler stays installed when miniaudio
    // later opens libasound itself (same shared library instance).
}

bool runningUnderWsl()
{
    QFile osrelease(QStringLiteral("/proc/sys/kernel/osrelease"));
    if (!osrelease.open(QIODevice::ReadOnly))
        return false;
    return osrelease.readAll().toLower().contains("microsoft");
}

} // namespace
#endif

namespace {
// Silence to render after the last event before auto-stopping (no loop).
constexpr double kTailSeconds = 3.0;
} // namespace

AudioEngine::AudioEngine()
{
    // The default routes until the owner publishes the user's.
    m_liveRoutes.store(MidiRoutes().packed());
    syncLiveRoutes();
}

AudioEngine::~AudioEngine()
{
    shutdown();
}

bool AudioEngine::init(QString *error)
{
#ifdef __linux__
    suppressAlsaErrors();
    // Try PulseAudio before ALSA so WSLg's PulseAudio server is found
    // without probing (nonexistent) ALSA hardware.
    ma_backend linuxBackends[] = {ma_backend_pulseaudio, ma_backend_alsa};
    m_context = new ma_context;
    m_hasContext = (ma_context_init(linuxBackends, 2, nullptr, m_context) == MA_SUCCESS);
    if (!m_hasContext) {
        delete m_context;
        m_context = nullptr;
    }
#endif

    m_device = new ma_device;
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = 0; // use the device's native rate
    cfg.dataCallback = &AudioEngine::dataCallback;
    cfg.pUserData = this;
    // Buffering: PORYDAW_AUDIO_PERIOD_MS overrides when set. Under WSL,
    // default to 30 ms periods — WSLg's PulseAudio rides the RDP transport,
    // which underruns at miniaudio's 10 ms low-latency default and is heard
    // as constant crackling.
    bool havePeriodMs = false;
    int periodMs = qEnvironmentVariableIntValue("PORYDAW_AUDIO_PERIOD_MS", &havePeriodMs);
#ifdef __linux__
    if ((!havePeriodMs || periodMs <= 0) && runningUnderWsl()) {
        periodMs = 30;
        havePeriodMs = true;
    }
#endif
    if (havePeriodMs && periodMs > 0)
        cfg.periodSizeInMilliseconds = uint32_t(std::clamp(periodMs, 1, 500));

    if (ma_device_init(m_hasContext ? m_context : nullptr, &cfg, m_device) != MA_SUCCESS) {
        if (error)
            *error = QStringLiteral("Failed to initialize the audio output device.");
        delete m_device;
        m_device = nullptr;
        return false;
    }
    m_sampleRate = double(m_device->sampleRate);
    // When every real backend fails (headless CI, WSL without libpulse),
    // miniaudio's default context falls back to its null device: playback
    // runs but is silent. Record which backend won so the UI can warn.
    m_backendName = QString::fromUtf8(ma_get_backend_name(m_device->pContext->backend));
    m_isNullBackend = (m_device->pContext->backend == ma_backend_null);
    m_periodFrames = int(m_device->playback.internalPeriodSizeInFrames);
    m_periodCount = int(m_device->playback.internalPeriods);

    m_bufCapacity = 8192;
    m_bufL = std::make_unique<float[]>(m_bufCapacity);
    m_bufR = std::make_unique<float[]>(m_bufCapacity);
    m_pvL = std::make_unique<float[]>(m_bufCapacity);
    m_pvR = std::make_unique<float[]>(m_bufCapacity);

    m_engine = std::make_unique<M4AEngine>();
    m4a_engine_init(m_engine.get(), float(m_sampleRate));
    m_previewEngine = std::make_unique<M4AEngine>();
    m4a_engine_init(m_previewEngine.get(), float(m_sampleRate));

    if (ma_device_start(m_device) != MA_SUCCESS) {
        if (error)
            *error = QStringLiteral("Failed to start the audio output device.");
        ma_device_uninit(m_device);
        delete m_device;
        m_device = nullptr;
        return false;
    }
    m_deviceStarted = true;
    return true;
}

void AudioEngine::shutdown()
{
    if (m_device) {
        ma_device_uninit(m_device); // stops the audio thread
        delete m_device;
        m_device = nullptr;
        m_deviceStarted = false;
    }
    if (m_engine) {
        m4a_engine_destroy(m_engine.get());
        m_engine.reset();
    }
    if (m_previewEngine) {
        m4a_engine_destroy(m_previewEngine.get());
        m_previewEngine.reset();
    }
    m_audition.reset();
    m_timeline = nullptr;
    m_voicegroup = nullptr;
    if (m_context) {
        if (m_hasContext)
            ma_context_uninit(m_context);
        delete m_context;
        m_context = nullptr;
        m_hasContext = false;
    }
}

void AudioEngine::loadSong(const MidiTimeline *timeline, LoadedVoiceGroup *voicegroup,
                           const SongSettings &settings)
{
    // Cold swap: the audio thread must not be running while pointers change.
    if (m_deviceStarted)
        ma_device_stop(m_device);
    m_pendingSeek.store(kNoPendingSeek, std::memory_order_release);

    m_timeline = timeline;
    m_voicegroup = voicegroup;
    m_settings = settings;

    // Fresh engine state for the new song (safe here: device is stopped, and
    // init/set_pcm_mix_rate may allocate).
    m4a_engine_destroy(m_engine.get());
    m4a_engine_init(m_engine.get(), float(m_sampleRate));
    m4a_engine_set_voicegroup(m_engine.get(), m_voicegroup ? m_voicegroup->voices : nullptr);
    m4a_engine_set_song_volume(m_engine.get(), m_settings.songVolume);
    m4a_reverb_set_amount(&m_engine->reverb, m_settings.reverb);
    m_engine->maxPcmChannels = m_settings.maxPcmChannels;
    m_engine->analogFilter = m_settings.analogFilter;
    m4a_engine_set_pcm_mix_rate(m_engine.get(), m_settings.pcmMixRate);
    // Latch the song's initial controller state and prime each track's voice
    // so auditioned notes (previewNote) sound as they would in the song
    // before playback has ever dispatched the track's events. Playback
    // re-dispatches the real events, so this changes nothing it plays.
    if (m_timeline) {
        TimelinePlayer::chase(m_engine.get(), m_timeline, 0);
        TimelinePlayer::primeVoices(m_engine.get(), m_timeline, 0);
    }
    resetPreviewEngine();
    clearTimedPreviews();

    m_transport.store(static_cast<int>(Transport::Stopped));
    m_appliedTransport = static_cast<int>(Transport::Stopped);
    m_captureBegun = 0;
    m_muteMask.store(0);
    m_soloMask.store(0);
    m_appliedMute = 0;
    m_player.reset();
    m_playhead.store(0);
    m_activePcm.store(0);
    m_activeCgb.store(0);

    if (m_deviceStarted)
        ma_device_start(m_device);
}

void AudioEngine::updateTimeline(const MidiTimeline *timeline)
{
    if (!m_timeline || !timeline)
        return;
    if (m_deviceStarted)
        ma_device_stop(m_device);

    // A seek published but not yet applied by the audio thread must survive
    // the rebuild — dropping it would silently ignore an edit-cursor move
    // that raced this edit. The device is stopped, so the exchange can't
    // race applyPendingSeek.
    const uint64_t pending = m_pendingSeek.exchange(kNoPendingSeek, std::memory_order_acq_rel);
    const uint64_t pos = pending != kNoPendingSeek ? pending : m_player.position();
    if (pos != m_player.position())
        captureBoundary(m_player.position(), pos);
    m_timeline = timeline;
    m_player.seek(pos, m_timeline);
    m_playhead.store(pos);
    // Release sounding notes: their note-offs may have moved or vanished in
    // the rebuilt timeline. Envelopes fade naturally, so brief edits during
    // playback don't hard-cut the audio.
    for (int track = 0; track < MAX_TRACKS; track++)
        m4a_engine_all_notes_off(m_engine.get(), track);
    clearTimedPreviews();
    // Re-latch controller state from the rebuilt timeline: the edit may have
    // deleted or moved the events behind the engine's current bend/CC values
    // (e.g. clearing a lane), which would otherwise stay latched until stop.
    TimelinePlayer::chase(m_engine.get(), m_timeline, pos);
    TimelinePlayer::primeVoices(m_engine.get(), m_timeline, pos);

    if (m_deviceStarted)
        ma_device_start(m_device);
}

void AudioEngine::seek(uint64_t samplePos)
{
    if (!m_timeline)
        return;
    m_pendingSeek.store(samplePos, std::memory_order_release);
}

void AudioEngine::applyPendingSeek()
{
    const uint64_t samplePos = m_pendingSeek.exchange(kNoPendingSeek, std::memory_order_acq_rel);
    if (samplePos == kNoPendingSeek || !m_timeline)
        return;

    for (int track = 0; track < MAX_TRACKS; track++)
        m4a_engine_all_notes_off(m_engine.get(), track);
    const uint64_t from = m_player.position();
    clearTimedPreviews();
    m_player.seek(samplePos, m_timeline);
    captureBoundary(from, samplePos);
    TimelinePlayer::chase(m_engine.get(), m_timeline, samplePos);
    TimelinePlayer::primeVoices(m_engine.get(), m_timeline, samplePos);
    m_playhead.store(samplePos);
}

void AudioEngine::updateSettings(const SongSettings &settings)
{
    if (m_deviceStarted)
        ma_device_stop(m_device);
    const bool mixRateChanged = settings.pcmMixRate != m_settings.pcmMixRate;
    m_settings = settings;
    m4a_engine_set_song_volume(m_engine.get(), m_settings.songVolume);
    m4a_reverb_set_amount(&m_engine->reverb, m_settings.reverb);
    m_engine->maxPcmChannels = m_settings.maxPcmChannels;
    m_engine->analogFilter = m_settings.analogFilter;
    if (mixRateChanged)
        m4a_engine_set_pcm_mix_rate(m_engine.get(), m_settings.pcmMixRate);
    resetPreviewEngine();
    if (m_deviceStarted)
        ma_device_start(m_device);
}

void AudioEngine::updateVoicegroup(LoadedVoiceGroup *voicegroup)
{
    if (m_deviceStarted)
        ma_device_stop(m_device);
    m4a_engine_all_sound_off(m_engine.get());
    clearTimedPreviews();
    m_voicegroup = voicegroup;
    m4a_engine_set_voicegroup(m_engine.get(), m_voicegroup ? m_voicegroup->voices : nullptr);
    // Re-latch program changes: the tracks' instrument state still points
    // into the old voices array until the chase reapplies it.
    if (m_timeline) {
        TimelinePlayer::chase(m_engine.get(), m_timeline, m_playhead.load());
        TimelinePlayer::primeVoices(m_engine.get(), m_timeline, m_playhead.load());
    }
    resetPreviewEngine();
    if (m_deviceStarted)
        ma_device_start(m_device);
}

ToneData *AudioEngine::previewVoices() const
{
    return m_voicegroup ? m_voicegroup->voices : nullptr;
}

// Rebuilds the audition instance to match the main engine's settings and
// voicegroup. Cold: callers have stopped the device.
void AudioEngine::resetPreviewEngine()
{
    if (!m_previewEngine)
        return;
    m4a_engine_destroy(m_previewEngine.get());
    m4a_engine_init(m_previewEngine.get(), float(m_sampleRate));
    m4a_engine_set_voicegroup(m_previewEngine.get(), previewVoices());
    m4a_engine_set_song_volume(m_previewEngine.get(), m_settings.songVolume);
    m4a_reverb_set_amount(&m_previewEngine->reverb, m_settings.reverb);
    m_previewEngine->maxPcmChannels = m_settings.maxPcmChannels;
    m_previewEngine->analogFilter = m_settings.analogFilter;
    m4a_engine_set_pcm_mix_rate(m_previewEngine.get(), m_settings.pcmMixRate);
    m_previewVoiceKey = -1;
    // The reinit destroyed every channel, so all audition slots retire.
    m_audition.reset();
}

// The volume byte carried by a preview command: 0xFF means "leave the track's
// VOL alone", any other value is a VOL byte clamped into range.
static uint8_t previewVolumeByte(int rawVolume)
{
    if (rawVolume < 0)
        return 0xFF;
    return uint8_t(std::min(rawVolume, MAX_SONG_VOLUME));
}

// The pan byte carried by a preview command: 0xFF means "leave the track's
// PAN alone", any other value is the engine's pan biased into 0-127.
static uint8_t previewPanByte(int pan)
{
    if (pan < -64 || pan > 63)
        return 0xFF;
    return uint8_t(pan + 64);
}

void AudioEngine::previewNote(uint8_t track, uint8_t key, uint8_t velocity, int rawVolume, int pan)
{
    m_previewGen++;
    m_previewCmd.store((uint64_t(previewPanByte(pan)) << 40) | (uint64_t(m_previewGen) << 32) |
                       (uint64_t(previewVolumeByte(rawVolume)) << 24) |
                       (uint64_t(track & 0x0F) << 16) | (uint64_t(key & 0x7F) << 8) | velocity);
}

void AudioEngine::previewNoteTimed(uint8_t track, uint8_t key, uint8_t velocity,
                                   uint32_t durationSamples, int rawVolume, int pan)
{
    if (velocity > 0 && durationSamples == 0)
        return;
    const uint32_t w = m_timedWrite.load(std::memory_order_relaxed);
    if (w - m_timedRead.load(std::memory_order_acquire) >= kTimedRingSize)
        return;
    m_timedRing[w % kTimedRingSize] = {
        uint8_t(track & 0x0F),        uint8_t(key & 0x7F), velocity,
        previewVolumeByte(rawVolume), previewPanByte(pan), durationSamples};
    m_timedWrite.store(w + 1, std::memory_order_release);
}

void AudioEngine::previewVoice(uint8_t voice, uint8_t key, uint8_t velocity)
{
    m_previewVoiceGen++;
    m_previewVoiceCmd.store((uint64_t(m_previewVoiceGen) << 32) | (uint64_t(voice & 0x7F) << 16) |
                            (uint64_t(key & 0x7F) << 8) | velocity);
}

void AudioEngine::unloadSong()
{
    if (m_deviceStarted)
        ma_device_stop(m_device);
    // Hard-cut sounding channels while the device is parked. Both transport
    // fields are assigned below, so the callback's Playing→Stopped transition
    // never fires and cutAllSound would never run — leaving mid-note channels
    // CHN_ON (with no note-offs coming after the player reset) still reading
    // WaveData owned by the voicegroup the caller frees right after this
    // returns. loadSong is exempt only because its engine reinit zeroes the
    // channels.
    m4a_engine_all_sound_off(m_engine.get());
    m_pendingSeek.store(kNoPendingSeek, std::memory_order_release);
    m_timeline = nullptr;
    m_voicegroup = nullptr;
    m4a_engine_set_voicegroup(m_engine.get(), nullptr);
    resetPreviewEngine();
    clearTimedPreviews();
    m_transport.store(static_cast<int>(Transport::Stopped));
    m_appliedTransport = static_cast<int>(Transport::Stopped);
    m_captureBegun = 0;
    m_player.reset();
    m_playhead.store(0);
    if (m_deviceStarted)
        ma_device_start(m_device);
}

void AudioEngine::play()
{
    if (songLoaded())
        m_transport.store(static_cast<int>(Transport::Playing));
}

void AudioEngine::pause()
{
    if (transport() == Transport::Playing)
        m_transport.store(static_cast<int>(Transport::Paused));
}

void AudioEngine::stop()
{
    // Cancel before publishing the transport change: once the callback can
    // see Stopped, no stale seek may survive to move the rewound playhead.
    m_pendingSeek.store(kNoPendingSeek, std::memory_order_release);
    m_transport.store(static_cast<int>(Transport::Stopped));
}

uint64_t AudioEngine::polyLostTotal() const
{
    // The engine header documents these counters as safe for lock-free
    // monitor reads from the GUI thread.
    if (!m_engine)
        return 0;
    // Tail cuts are deliberately excluded: a shortened release is barely
    // audible, and folding it in makes the transport-bar count alarmingly
    // large. The Polyphony dock still shows tail cuts in their own column.
    uint64_t total = 0;
    for (int t = 0; t < MAX_TRACKS; t++) {
        total += m_engine->polyDropCount[t];
        total += m_engine->polyStealCount[t];
    }
    return total;
}

void AudioEngine::polySnapshot(PolySnapshot *out) const
{
    *out = PolySnapshot{};
    if (!m_engine)
        return;
    const M4AEngine *engine = m_engine.get();
    out->maxPcmChannels = engine->maxPcmChannels;
    out->invert = engine->polyDebugInvert;
    // Total first, then the ring: an event published after this line shows up
    // next poll instead of tearing this one's newest row.
    out->eventTotal = engine->polyEventTotal;
    for (int i = 0; i < M4A_POLY_EVENT_CAPACITY; i++)
        out->events[i] = engine->polyEvents[i];
    for (int t = 0; t < MAX_TRACKS; t++) {
        out->drop[t] = engine->polyDropCount[t];
        out->steal[t] = engine->polyStealCount[t];
        out->tailCut[t] = engine->polyTailCutCount[t];
    }
    for (int i = 0; i < TOTAL_PCM_CHANNELS; i++) {
        const M4APCMChannel &ch = engine->pcmChannels[i];
        out->pcm[i] = {(ch.status & CHN_ON) != 0, (ch.status & (CHN_STOP | CHN_IEC)) != 0,
                       uint8_t(ch.trackIndex), uint8_t(ch.midiKey)};
    }
    for (int i = 0; i < TOTAL_CGB_CHANNELS; i++) {
        const M4ACGBChannel &ch = engine->cgbChannels[i];
        out->cgb[i] = {(ch.status & CHN_ON) != 0, (ch.status & (CHN_STOP | CHN_IEC)) != 0,
                       uint8_t(ch.trackIndex), uint8_t(ch.midiKey)};
    }
}

void AudioEngine::dataCallback(ma_device *device, void *output, const void *, uint32_t frameCount)
{
    auto *self = static_cast<AudioEngine *>(device->pUserData);
    self->process(static_cast<float *>(output), frameCount);
}

uint32_t AudioEngine::effectiveMuteMask() const
{
    const uint32_t mute = m_muteMask.load();
    const uint32_t solo = m_soloMask.load();
    // Tracks beyond the music player's budget are silent in-game
    // (MPlayStart never starts them); solo cannot bring them back.
    return ((solo ? (mute | ~solo) : mute) | trackBudgetMuteMask(m_settings.trackBudget)) & 0xFFFF;
}

void AudioEngine::applyTransportTransition()
{
    const int t = m_transport.load();
    if (t == m_appliedTransport)
        return;

    // Every transition cuts hard. Pause and Stop must fall silent — a
    // note-off alone leaves a slow-release voice ringing for seconds — and
    // entering Playing must halt auditions before the song sounds (while
    // stopped or paused a preview can ring or still be counting down).
    // Preview bookkeeping is
    // dropped with the sound (queued-but-unstarted previews too) so no
    // stale note-off can later cut a playback note on the same track and
    // key.
    cutAllSound();
    switch (static_cast<Transport>(t)) {
    case Transport::Stopped:
        m_player.reset();
        break;
    case Transport::Paused:
        break;
    case Transport::Playing:
        // No player reset here: the Stopped transition already rewound to 0,
        // and a seek() between stop and play deliberately moves the start.
        if (m_appliedTransport == static_cast<int>(Transport::Stopped))
            m4a_engine_reset_poly_stats(m_engine.get());
        break;
    }
    m_appliedTransport = t;
}

// Audio-thread: silence everything at once and drop preview bookkeeping,
// queued and active.
void AudioEngine::cutAllSound()
{
    m4a_engine_all_sound_off(m_engine.get());
    m_timedActiveCount = 0;
    m_timedRead.store(m_timedWrite.load(std::memory_order_acquire), std::memory_order_release);
    forgetLiveNotes();
    m_previewTrack = -1;
    m_previewKey = -1;
}

void AudioEngine::applyMuteTransition()
{
    const uint32_t em = effectiveMuteMask();
    if (em == m_appliedMute)
        return;
    const uint32_t newlyMuted = em & ~m_appliedMute;
    for (int track = 0; track < MAX_TRACKS; track++) {
        if ((newlyMuted >> track) & 1)
            m4a_engine_all_notes_off(m_engine.get(), track);
    }
    m_appliedMute = em;
}

void AudioEngine::applyPreviewNote()
{
    const uint64_t cmd = m_previewCmd.load();
    if (cmd == m_appliedPreview)
        return;
    m_appliedPreview = cmd;

    // A new preview releases the previous one so previews never stack.
    if (m_previewKey >= 0) {
        m4a_engine_note_off(m_engine.get(), m_previewTrack, uint8_t(m_previewKey));
        m_previewTrack = -1;
        m_previewKey = -1;
    }
    const uint8_t track = (cmd >> 16) & 0x0F;
    const uint8_t key = (cmd >> 8) & 0x7F;
    const uint8_t velocity = cmd & 0xFF;
    const uint8_t volume = (cmd >> 24) & 0xFF;
    const uint8_t pan = (cmd >> 40) & 0xFF;
    if (velocity > 0) {
        // Live note: no timeline position for any overflow event it causes,
        // and it stays audible in the solo-overflow invert mode.
        m_engine->polyEventClock = M4A_POLY_TICK_NONE;
        m_engine->auditionNote = true;
        previewNoteOn(track, key, velocity, volume, pan);
        m_previewTrack = track;
        m_previewKey = key;
    }
}

void AudioEngine::previewNoteOn(uint8_t track, uint8_t key, uint8_t velocity, uint8_t volume,
                                uint8_t pan)
{
    TimelinePlayer::auditionNoteOn(m_engine.get(), track, key, velocity,
                                   volume == 0xFF ? -1 : int(volume),
                                   pan == 0xFF ? M4A_AUDITION_PAN_NONE : int(pan) - 64);
}

void AudioEngine::applyTimedPreviews(uint32_t frameCount)
{
    const uint32_t w = m_timedWrite.load(std::memory_order_acquire);
    uint32_t r = m_timedRead.load(std::memory_order_relaxed);
    for (; r != w; r++) {
        const TimedPreview cmd = m_timedRing[r % kTimedRingSize];
        if (cmd.velocity == 0) {
            // Early release: the band no longer covers the note.
            releaseActive(cmd.track, cmd.key);
            continue;
        }
        startActive(cmd.track, cmd.key, cmd.velocity, cmd.volume, cmd.pan,
                    int64_t(cmd.durationSamples), /*held=*/false);
    }
    m_timedRead.store(r, std::memory_order_release);

    // Count down and release. Expiry lands at callback granularity, which is
    // plenty for an audition. Held (live) notes wait for their note-off.
    for (int i = 0; i < m_timedActiveCount;) {
        if (m_timedActive[i].held) {
            i++;
            continue;
        }
        m_timedActive[i].remaining -= frameCount;
        if (m_timedActive[i].remaining <= 0) {
            m4a_engine_note_off(m_engine.get(), m_timedActive[i].track, m_timedActive[i].key);
            m_timedActive[i] = m_timedActive[--m_timedActiveCount];
        } else {
            i++;
        }
    }
}

void AudioEngine::startActive(uint8_t track, uint8_t key, uint8_t velocity, uint8_t volume,
                              uint8_t pan, int64_t remaining, bool held)
{
    // Retrigger a still-sounding key (note-off first — the engine stops
    // channels by track+key, so duplicates must never stack) and reuse its
    // slot; otherwise take a free slot, or steal: the timed preview closest
    // to its own note-off, and a held note (the oldest) only when every slot
    // is held — a band sweep must not cut what the keyboard plays.
    int slot = -1;
    for (int i = 0; i < m_timedActiveCount; i++) {
        if (m_timedActive[i].track == track && m_timedActive[i].key == key) {
            slot = i;
            break;
        }
    }
    if (slot < 0 && m_timedActiveCount < kTimedMaxActive) {
        slot = m_timedActiveCount++;
    } else {
        if (slot < 0) {
            slot = 0;
            for (int i = 1; i < m_timedActiveCount; i++) {
                const ActiveTimed &a = m_timedActive[i];
                const ActiveTimed &best = m_timedActive[slot];
                bool better;
                if (a.held != best.held)
                    better = !a.held;
                else if (!a.held)
                    better = a.remaining < best.remaining;
                else // older, wrap-safe
                    better = int32_t(a.order - best.order) < 0;
                if (better)
                    slot = i;
            }
        }
        m4a_engine_note_off(m_engine.get(), m_timedActive[slot].track, m_timedActive[slot].key);
    }
    // Auditions are live notes: no timeline position for overflow events,
    // and they stay audible in the solo-overflow invert mode.
    m_engine->polyEventClock = M4A_POLY_TICK_NONE;
    m_engine->auditionNote = true;
    previewNoteOn(track, key, velocity, volume, pan);
    m_timedActive[slot] = {track, key, held, m_activeOrder++, remaining};
}

bool AudioEngine::releaseActive(uint8_t track, uint8_t key)
{
    for (int i = 0; i < m_timedActiveCount; i++) {
        if (m_timedActive[i].track == track && m_timedActive[i].key == key) {
            m4a_engine_note_off(m_engine.get(), track, key);
            m_timedActive[i] = m_timedActive[--m_timedActiveCount];
            return true;
        }
    }
    return false;
}

// Cold: drop queued and sounding timed previews. Callers have already cut or
// released the engine's sound (or destroyed the engine), so no note-offs are
// owed; stale entries would otherwise cut a later playback note that lands on
// the same track and key.
void AudioEngine::clearTimedPreviews()
{
    m_timedRead.store(m_timedWrite.load());
    m_timedActiveCount = 0;
    forgetLiveNotes();
}

void AudioEngine::forgetLiveNotes()
{
    // The keys stay physically down, but nothing of theirs sounds any more:
    // their note-offs (and pedal-up) must find nothing to release. A take
    // records what was heard, so its held notes end here too.
    pushCapture(CaptureEvent::Cut, m_player.position());
    std::fill(std::begin(m_liveOn), std::end(m_liveOn), false);
    std::fill(std::begin(m_liveDown), std::end(m_liveDown), false);
    m_liveHeldLo.store(0, std::memory_order_relaxed);
    m_liveHeldHi.store(0, std::memory_order_relaxed);
}

void AudioEngine::syncLiveRoutes()
{
    const uint64_t packed = m_liveRoutes.load();
    if (packed == m_appliedLiveRoutes)
        return;
    m_appliedLiveRoutes = packed;
    std::fill(std::begin(m_liveRouteOf), std::end(m_liveRouteOf), uint8_t(0xFF));
    // The publisher packs a valid MidiRoutes; anything else is skipped, so
    // a reserved CC never reaches the routed path.
    for (int i = 0; i < MidiRoutes::kLaneCount; i++) {
        const int cc = int((packed >> (8 * i)) & 0xFF);
        if (MidiRoutes::assignable(cc) && m_liveRouteOf[cc] == 0xFF)
            m_liveRouteOf[cc] = MidiRoutes::kLanes[i];
    }
}

void AudioEngine::applyMidiInput()
{
    // After the transport transition, the seek and a settled wrap: a fresh
    // start's Begin lands on the edit cursor it seeked to (the loop start,
    // when that is at or past the loop end).
    syncCapture();
    // Sound target: none without a song or with live play off; the input is
    // still drained so it never backs up.
    int target = m_liveTarget.load();
    if (!m_livePlay.load() || !m_timeline || target < 0 || target >= MAX_TRACKS)
        target = -1;

    const uint32_t panic = m_livePanicCmd.load();
    if (panic != m_appliedLivePanic) {
        m_appliedLivePanic = panic;
        imposeLiveRelease();
        m_liveSustain = false;
    }
    const uint32_t release = m_liveReleaseCmd.load();
    if (release != m_appliedLiveRelease) {
        m_appliedLiveRelease = release;
        imposeLiveRelease();
    }
    if (target != m_liveTrack) {
        imposeLiveRelease(); // on the old track
        // Hand the old track back to the song: its bend and mod as chased
        // at the playhead, every other track and parameter untouched.
        if (m_liveBent && m_liveTrack >= 0 && m_timeline)
            TimelinePlayer::chaseBendMod(m_engine.get(), m_timeline, m_player.position(),
                                         m_liveTrack);
        m_liveBent = false;
        m_liveTrack = target;
    }

    syncLiveRoutes();
    if (MidiInputRing *ring = m_midiRing.load(std::memory_order_acquire)) {
        MidiMsg msg;
        // Bounded by a few periods (or callbacks, when the backend hands
        // over more than a period at once), so a stalled callback can't
        // throw a note far back.
        const long long cap =
            std::max(4LL * std::max(m_periodFrames, 1), 2LL * (long long)m_callbackFrames);
        while (ring->pop(&msg)) {
            // How long the message waited for this callback (measured to
            // its entry, the moment its stamp stands for): the take places
            // it back where it arrived.
            m_liveLate = 0;
            if (msg.arrivalNs > 0) {
                const double waited =
                    double(std::max<int64_t>(0, m_callbackNs - msg.arrivalNs)) * 1e-9;
                m_liveLate = uint32_t(std::min(std::llround(waited * m_sampleRate), cap));
            }
            handleLiveMessage(msg);
        }
        m_liveLate = 0;
    }
    publishLiveHeld();
}

void AudioEngine::handleLiveMessage(const MidiMsg &msg)
{
    // Omni (D2): the channel nibble is ignored. MidiInput::parse already
    // turned a velocity-0 note on into a note off.
    const uint8_t key = msg.d1 & 0x7F;
    switch (msg.status & 0xF0) {
    case 0x90: {
        const int fixed = m_liveFixedVelocity.load();
        const uint8_t velocity = fixed > 0 ? uint8_t(std::min(fixed, 127)) : msg.d2;
        // A re-press (a sustained key, or a doubled note on) retriggers.
        if (m_liveOn[key])
            applyLiveNote(key, 0);
        m_liveDown[key] = true;
        m_liveOn[key] = true;
        applyLiveNote(key, velocity);
        break;
    }
    case 0x80:
        m_liveDown[key] = false;
        // Under the pedal the note-off waits for pedal-up.
        if (m_liveOn[key] && !m_liveSustain) {
            m_liveOn[key] = false;
            applyLiveNote(key, 0);
        }
        break;
    case 0xB0:
        switch (msg.d1) {
        case 64: // sustain pedal
            if (msg.d2 >= 64)
                m_liveSustain = true;
            else
                liftLivePedal();
            break;
        case 121: // reset all controllers: the pedal is up, bend and mod at rest
            liftLivePedal();
            applyLiveBend(0);
            applyLiveMod(0);
            break;
        case 120: // all sound off
        case 123: // all notes off
            // A panic: a pedal-up lost with them must not defer every later
            // note-off.
            releaseLiveNotes();
            m_liveSustain = false;
            break;
        default:
            // A routed control (reserved CCs never route). Only MOD sounds
            // so far; the other lanes join in step 2.
            switch (m_liveRouteOf[msg.d1 & 0x7F]) {
            case 0x01: // MOD: the whole track's LFO depth, at the lane's scale
                applyLiveMod(uint8_t((msg.d2 * m_liveModMax.load() + 63) / 127));
                break;
            default:
                break;
            }
            break;
        }
        break;
    case 0xE0:
        applyLiveBend(int16_t((((msg.d2 & 0x7F) << 7) | (msg.d1 & 0x7F)) - 8192));
        break;
    }
}

void AudioEngine::applyLiveBend(int16_t bend)
{
    // The whole track's bend, song notes included; the engine scales the
    // signed 14-bit value to its -64..63 and BENDR sets the range.
    pushCapture(CaptureEvent::Bend, m_player.position(), 0, 0, 0, bend);
    if (m_liveTrack >= 0) {
        m4a_engine_pitch_bend(m_engine.get(), m_liveTrack, bend);
        m_liveBent = true;
    }
}

void AudioEngine::applyLiveMod(uint8_t mod)
{
    // Captured as heard, whether or not it sounds (as notes are).
    pushCapture(CaptureEvent::Mod, m_player.position(), 0, 0, 0, mod);
    if (m_liveTrack >= 0) {
        m4a_engine_cc(m_engine.get(), m_liveTrack, 0x01, mod);
        m_liveBent = true;
    }
}

void AudioEngine::applyLiveNote(uint8_t key, uint8_t velocity)
{
    // Captured whether or not it sounds: live play off (or no sound target)
    // still records.
    pushCapture(velocity > 0 ? CaptureEvent::NoteOn : CaptureEvent::NoteOff, m_player.position(),
                key, velocity);
    if (m_liveTrack < 0)
        return;
    if (velocity > 0)
        startActive(uint8_t(m_liveTrack), key, velocity, 0xFF, 0xFF, 0, /*held=*/true);
    else
        releaseActive(uint8_t(m_liveTrack), key);
}

void AudioEngine::imposeLiveRelease()
{
    // Nobody played these note-offs: a take ends its held notes here
    // unshifted, as at a cut, and the offs that follow match nothing.
    pushCapture(CaptureEvent::Cut, m_player.position());
    releaseLiveNotes();
}

void AudioEngine::releaseLiveNotes()
{
    for (int k = 0; k < 128; k++) {
        if (m_liveOn[k]) {
            m_liveOn[k] = false;
            applyLiveNote(uint8_t(k), 0);
        }
        m_liveDown[k] = false;
    }
}

void AudioEngine::liftLivePedal()
{
    if (m_liveSustain) {
        for (int k = 0; k < 128; k++) {
            if (m_liveOn[k] && !m_liveDown[k]) {
                m_liveOn[k] = false;
                applyLiveNote(uint8_t(k), 0);
            }
        }
    }
    m_liveSustain = false;
}

void AudioEngine::publishLiveHeld()
{
    uint64_t mask[2] = {0, 0};
    for (int i = 0; i < m_timedActiveCount; i++) {
        const ActiveTimed &a = m_timedActive[i];
        if (a.held && int(a.track) == m_liveTrack)
            mask[a.key >> 6] |= uint64_t(1) << (a.key & 63);
    }
    m_liveHeldLo.store(mask[0], std::memory_order_relaxed);
    m_liveHeldHi.store(mask[1], std::memory_order_relaxed);
}

bool AudioEngine::capturing() const
{
    return m_captureBegun != 0 && m_captureBegun == m_captureGen.load() && m_timeline &&
           m_appliedTransport == static_cast<int>(Transport::Playing);
}

void AudioEngine::syncCapture()
{
    const uint32_t gen = m_captureGen.load();
    if (gen == m_captureBegun)
        return;
    // A new arm (or a disarm). It begins at the first callback that plays.
    m_captureBegun = 0;
    if (gen == 0 || !m_timeline || m_appliedTransport != static_cast<int>(Transport::Playing))
        return;
    m_captureBegun = gen;
    m_captureSegment = 0;
    // The clock first: a commit that reads it once the Begin is visible
    // must not see the previous take's position.
    publishCaptureClock();
    pushCapture(CaptureEvent::Begin, m_player.position());
}

void AudioEngine::pushCapture(uint8_t kind, uint64_t samplePos, uint8_t key, uint8_t velocity,
                              uint64_t resumePos, int16_t value)
{
    if (!capturing())
        return;
    const uint32_t w = m_captureWrite.load(std::memory_order_relaxed);
    if (w - m_captureRead.load(std::memory_order_acquire) >= kCaptureRingSize) {
        m_captureDrops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    CaptureEvent &ev = m_captureRing[w % kCaptureRingSize];
    ev.samplePos = samplePos;
    ev.resumePos = resumePos;
    ev.generation = m_captureBegun;
    ev.segment = m_captureSegment;
    ev.kind = kind;
    ev.key = key;
    ev.velocity = velocity;
    ev.value = value;
    ev.lateSamples = kind == CaptureEvent::NoteOn || kind == CaptureEvent::NoteOff ||
                             kind == CaptureEvent::Bend || kind == CaptureEvent::Mod
                         ? m_liveLate
                         : 0;
    m_captureWrite.store(w + 1, std::memory_order_release);
}

void AudioEngine::captureBoundary(uint64_t from, uint64_t to)
{
    if (!capturing())
        return;
    m_captureSegment++;
    pushCapture(CaptureEvent::Boundary, from, 0, 0, to);
}

void AudioEngine::settleLoopWrap()
{
    // Before the input is stamped: a player at or past the loop end renders
    // nothing of where it is, so what this callback captures belongs after
    // the wrap.
    if (m_appliedTransport != static_cast<int>(Transport::Playing) || !m_timeline)
        return;
    const uint64_t from = m_player.position();
    const uint32_t wraps = m_player.wrapCount();
    m_player.settleWrap(m_engine.get(), m_timeline, m_loopEnabled.load(), m_appliedMute);
    if (m_player.wrapCount() != wraps)
        captureBoundary(from, m_timeline->loopStartSample);
}

void AudioEngine::publishCaptureClock()
{
    m_captureClock.store((uint64_t(m_captureSegment) << kCaptureClockPosBits) |
                             (m_player.position() & kCaptureClockPosMask),
                         std::memory_order_release);
}

bool AudioEngine::popCapture(CaptureEvent *out)
{
    const uint32_t r = m_captureRead.load(std::memory_order_relaxed);
    if (r == m_captureWrite.load(std::memory_order_acquire))
        return false;
    *out = m_captureRing[r % kCaptureRingSize];
    m_captureRead.store(r + 1, std::memory_order_release);
    return true;
}

void AudioEngine::holdDeviceForTest(bool hold)
{
    if (!m_device || hold == m_deviceHeldForTest)
        return;
    m_deviceHeldForTest = hold;
    if (hold) {
        ma_device_stop(m_device);
        m_deviceStarted = false;
    } else {
        m_deviceStarted = ma_device_start(m_device) == MA_SUCCESS;
    }
}

void AudioEngine::pumpForTest(uint32_t frames)
{
    if (!m_deviceHeldForTest || !m_engine)
        return;
    constexpr uint32_t kChunk = 512;
    std::vector<float> out(kChunk * 2);
    while (frames > 0) {
        const uint32_t n = std::min(frames, kChunk);
        process(out.data(), n);
        frames -= n;
    }
}

void AudioEngine::applyPreviewVoice()
{
    const uint64_t cmd = m_previewVoiceCmd.load();
    if (cmd == m_appliedPreviewVoice)
        return;
    m_appliedPreviewVoice = cmd;

    M4AEngine *engine = m_previewEngine.get();
    if (m_previewVoiceKey >= 0) {
        m4a_engine_note_off(engine, 0, uint8_t(m_previewVoiceKey));
        m_previewVoiceKey = -1;
    }
    const uint8_t voice = (cmd >> 16) & 0x7F;
    const uint8_t key = (cmd >> 8) & 0x7F;
    const uint8_t velocity = cmd & 0xFF;
    if (velocity > 0 && previewVoices() != nullptr) {
        m4a_engine_program_change(engine, 0, voice);
        m4a_engine_note_on(engine, 0, key, velocity);
        m_previewVoiceKey = key;
    }
}

void AudioEngine::applyPolyDebug()
{
    // Compared against the live engine field, not an applied-state shadow:
    // loadSong's engine reinit clears polyDebugInvert, and this re-asserts
    // the desired mode on the next callback (session-sticky semantics).
    const bool invert = m_polyInvert.load();
    if (invert != m_engine->polyDebugInvert)
        m4a_engine_set_poly_debug_invert(m_engine.get(), invert);

    const uint32_t resetGen = m_polyResetCmd.load();
    if (resetGen != m_appliedPolyReset) {
        m_appliedPolyReset = resetGen;
        m4a_engine_reset_poly_stats(m_engine.get());
    }
}

void AudioEngine::process(float *interleavedOut, uint32_t frameCount)
{
    // Transition before pending seek: a Stop rewinds the player, so a seek
    // requested after stop() (play-from-cursor) must land on top of the
    // rewind, not under it.
    // The callback's entry: what applyMidiInput measures a message's wait
    // against.
    m_callbackNs = midiClockNs();
    m_callbackFrames = frameCount;
    applyTransportTransition();
    applyPendingSeek();
    applyMuteTransition();
    applyPreviewNote();
    applyTimedPreviews(frameCount);
    settleLoopWrap();
    applyMidiInput();
    applyPreviewVoice();
    m_audition.apply(m_previewEngine.get(), kAuditionTrack);
    applyPolyDebug();

    // Voice edits: tracks hold a ToneData copy taken at program change, so a
    // scalar edit isn't heard until the copies are refreshed.
    const uint32_t refreshGen = m_refreshVoicesCmd.load();
    if (refreshGen != m_appliedRefreshVoices) {
        m_appliedRefreshVoices = refreshGen;
        m4a_engine_refresh_voices(m_engine.get());
    }

    M4AEngine *engine = m_engine.get();
    const float gain = m_outputGain.load();
    uint32_t done = 0;
    const uint32_t wrapsBefore = m_player.wrapCount();

    while (done < frameCount) {
        const uint32_t n = std::min(frameCount - done, m_bufCapacity);
        const MidiTimeline *tl = m_timeline;
        const bool playing =
            m_appliedTransport == static_cast<int>(Transport::Playing) && tl != nullptr;

        if (playing) {
            const bool looping = m_loopEnabled.load();
            m_player.render(engine, tl, m_bufL.get(), m_bufR.get(), n, looping, m_appliedMute);

            // Auto-stop a non-looping song after the tail rings out.
            if (!(looping && tl->hasLoop()) &&
                m_player.position() > tl->lengthSamples + uint64_t(kTailSeconds * m_sampleRate)) {
                m_transport.store(static_cast<int>(Transport::Stopped));
                applyTransportTransition();
            }
        } else {
            // Idle: keep processing so releases and reverb ring out.
            m4a_engine_process(engine, m_bufL.get(), m_bufR.get(), int(n));
        }

        // The audition instance is mixed on top so voice previews are heard
        // as they would sound in the song, without touching playback state.
        m4a_engine_process(m_previewEngine.get(), m_pvL.get(), m_pvR.get(), int(n));

        for (uint32_t i = 0; i < n; i++) {
            interleavedOut[(done + i) * 2] = (m_bufL[i] + m_pvL[i]) * gain;
            interleavedOut[(done + i) * 2 + 1] = (m_bufR[i] + m_pvR[i]) * gain;
        }
        done += n;
    }
    m_tap.write(interleavedOut, frameCount);

    // A take's segment ends at every loop wrap this callback rendered.
    for (uint32_t w = wrapsBefore; w != m_player.wrapCount(); w++)
        captureBoundary(m_timeline->loopEndSample, m_timeline->loopStartSample);
    if (capturing())
        publishCaptureClock();
    m_playhead.store(m_player.position());

    int pcm = 0;
    for (int i = 0; i < engine->maxPcmChannels; i++) {
        if (engine->pcmChannels[i].status & CHN_ON)
            pcm++;
    }
    int cgb = 0;
    for (int i = 0; i < MAX_CGB_CHANNELS; i++) {
        if (engine->cgbChannels[i].status & CHN_ON)
            cgb++;
    }
    m_activePcm.store(pcm);
    m_activeCgb.store(cgb);
}
