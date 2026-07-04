#include "audioengine.h"

#include <algorithm>

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

} // namespace
#endif

namespace {
// Silence to render after the last event before auto-stopping (no loop).
constexpr double kTailSeconds = 3.0;
} // namespace

AudioEngine::AudioEngine() = default;

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

    if (ma_device_init(m_hasContext ? m_context : nullptr, &cfg, m_device) != MA_SUCCESS) {
        if (error)
            *error = QStringLiteral("Failed to initialize the audio output device.");
        delete m_device;
        m_device = nullptr;
        return false;
    }
    m_sampleRate = double(m_device->sampleRate);

    m_bufCapacity = 8192;
    m_bufL = std::make_unique<float[]>(m_bufCapacity);
    m_bufR = std::make_unique<float[]>(m_bufCapacity);

    m_engine = std::make_unique<M4AEngine>();
    m4a_engine_init(m_engine.get(), float(m_sampleRate));

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
    m_timeline.reset();
    if (m_voicegroup) {
        voicegroup_free(m_voicegroup);
        m_voicegroup = nullptr;
    }
    if (m_context) {
        if (m_hasContext)
            ma_context_uninit(m_context);
        delete m_context;
        m_context = nullptr;
        m_hasContext = false;
    }
}

void AudioEngine::loadSong(std::unique_ptr<MidiTimeline> timeline, LoadedVoiceGroup *voicegroup,
                           const SongSettings &settings)
{
    // Cold swap: the audio thread must not be running while pointers change.
    if (m_deviceStarted)
        ma_device_stop(m_device);

    m_timeline = std::move(timeline);
    if (m_voicegroup)
        voicegroup_free(m_voicegroup);
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
    m4a_engine_set_pcm_mix_rate(m_engine.get(), m_settings.pcmMixRate);

    m_transport.store(static_cast<int>(Transport::Stopped));
    m_appliedTransport = static_cast<int>(Transport::Stopped);
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

void AudioEngine::unloadSong()
{
    if (m_deviceStarted)
        ma_device_stop(m_device);
    m_timeline.reset();
    if (m_voicegroup) {
        voicegroup_free(m_voicegroup);
        m_voicegroup = nullptr;
    }
    m4a_engine_set_voicegroup(m_engine.get(), nullptr);
    m_transport.store(static_cast<int>(Transport::Stopped));
    m_appliedTransport = static_cast<int>(Transport::Stopped);
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
    m_transport.store(static_cast<int>(Transport::Stopped));
}

uint64_t AudioEngine::polyLostTotal() const
{
    // The engine header documents these counters as safe for lock-free
    // monitor reads from the GUI thread.
    if (!m_engine)
        return 0;
    uint64_t total = 0;
    for (int t = 0; t < MAX_TRACKS; t++) {
        total += m_engine->polyDropCount[t];
        total += m_engine->polyStealCount[t];
        total += m_engine->polyTailCutCount[t];
    }
    return total;
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
    return (solo ? (mute | ~solo) : mute) & 0xFFFF;
}

void AudioEngine::applyTransportTransition()
{
    const int t = m_transport.load();
    if (t == m_appliedTransport)
        return;

    switch (static_cast<Transport>(t)) {
    case Transport::Stopped:
        m4a_engine_all_sound_off(m_engine.get());
        m_player.reset();
        break;
    case Transport::Paused:
        for (int track = 0; track < MAX_TRACKS; track++)
            m4a_engine_all_notes_off(m_engine.get(), track);
        break;
    case Transport::Playing:
        if (m_appliedTransport == static_cast<int>(Transport::Stopped)) {
            m_player.reset();
            m4a_engine_reset_poly_stats(m_engine.get());
        }
        break;
    }
    m_appliedTransport = t;
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

void AudioEngine::process(float *interleavedOut, uint32_t frameCount)
{
    applyTransportTransition();
    applyMuteTransition();

    M4AEngine *engine = m_engine.get();
    uint32_t done = 0;

    while (done < frameCount) {
        const uint32_t n = std::min(frameCount - done, m_bufCapacity);
        const MidiTimeline *tl = m_timeline.get();
        const bool playing =
            m_appliedTransport == static_cast<int>(Transport::Playing) && tl != nullptr;

        if (playing) {
            const bool looping = m_loopEnabled.load();
            m_player.render(engine, tl, m_bufL.get(), m_bufR.get(), n, looping,
                            m_appliedMute);

            // Auto-stop a non-looping song after the tail rings out.
            if (!(looping && tl->hasLoop())
                && m_player.position()
                       > tl->lengthSamples + uint64_t(kTailSeconds * m_sampleRate)) {
                m_transport.store(static_cast<int>(Transport::Stopped));
                applyTransportTransition();
            }
        } else {
            // Idle: keep processing so releases and reverb ring out.
            m4a_engine_process(engine, m_bufL.get(), m_bufR.get(), int(n));
        }

        for (uint32_t i = 0; i < n; i++) {
            interleavedOut[(done + i) * 2] = m_bufL[i];
            interleavedOut[(done + i) * 2 + 1] = m_bufR[i];
        }
        done += n;
    }

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
