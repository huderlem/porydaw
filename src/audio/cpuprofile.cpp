#include "audio/cpuprofile.h"

#include <QElapsedTimer>
#include <QMetaObject>

#include <algorithm>
#include <cmath>

#include "core/timelineplayer.h"

extern "C" {
#include "m4a_engine.h"
#include "m4a_gba.h"
}

namespace {

struct HookContext {
    CpuProfile *out;
    double samplesPerCycle;
    int64_t startCycle; // the GBA clock at timeline sample 0
};

// Every SoundMain's cost, at the timeline position its frame ran at: the
// engine's GBA clock since the end of its boot (where the timeline's sample
// 0 plays) in host samples.
void collectFrame(M4AEngine *engine, void *user)
{
    auto *ctx = static_cast<HookContext *>(user);
    const M4AFrameCost &cost = engine->frameCost;
    if (!cost.valid)
        return;
    CpuProfileFrame frame;
    const int64_t cycles = std::max<int64_t>(0, engine->gba.now - ctx->startCycle);
    frame.samplePos = uint64_t(double(cycles) * ctx->samplesPerCycle);
    frame.tick = 0; // filled in after the render, off the hot path
    frame.total = cost.total;
    frame.sequencer = cost.sequencer;
    frame.cgb = cost.cgb;
    frame.mixer = cost.mixer;
    ctx->out->frames.push_back(frame);
}

} // namespace

const CpuProfileFrame *CpuProfile::frameAtTick(double tick) const
{
    if (frames.empty() || tick < 0.0 || tick >= double(endTick))
        return nullptr;
    // The last frame starting at or before the tick; ticks before the first
    // SoundMain belong to it.
    auto it = std::upper_bound(frames.begin(), frames.end(), tick,
                               [](double t, const CpuProfileFrame &f) { return t < f.tick; });
    if (it == frames.begin())
        return &frames.front();
    return &*(it - 1);
}

bool renderCpuProfile(const MidiTimeline &timeline, const LoadedVoiceGroup *voicegroup,
                      const SongSettings &settings, const std::atomic<bool> *cancel,
                      CpuProfile *out)
{
    QElapsedTimer clock;
    clock.start();
    *out = CpuProfile();
    out->sampleRate = timeline.sampleRate;
    // A looping song once through to its loop end (past it the strip would
    // only repeat); any other with the release tail playback renders before
    // it auto-stops.
    const bool looping = timeline.hasLoop();
    if (looping) {
        out->endSample = timeline.loopEndSample;
        out->endTick = timeline.loopEndTick;
    } else if (timeline.lengthSamples > 0) {
        out->endSample = timeline.lengthSamples +
                         uint64_t(AudioEngine::kTailSeconds * timeline.sampleRate + 0.5);
        out->endTick = std::max<uint64_t>(
            timeline.lengthTicks, uint64_t(std::ceil(timeline.tickForSample(out->endSample))));
    }
    if (timeline.sampleRate <= 0.0 || out->endSample == 0)
        return true;
    if (double(out->endSample) / timeline.sampleRate > CpuProfile::kMaxSeconds) {
        out->skipped = true;
        return true;
    }

    // As AudioEngine::loadSong and a play from stop set the song engine up.
    const auto engine = std::make_unique<M4AEngine>();
    m4a_engine_init(engine.get(), float(timeline.sampleRate));
    m4a_engine_set_voicegroup(engine.get(),
                              voicegroup ? const_cast<ToneData *>(voicegroup->voices) : nullptr);
    applySongSettings(engine.get(), settings);
    const auto player = std::make_unique<TimelinePlayer>();
    player->reset();
    player->setCostHooks(true);
    TimelinePlayer::chase(engine.get(), &timeline, 0);
    TimelinePlayer::primeVoices(engine.get(), &timeline, 0);
    player->costSongStart(engine.get(), &timeline, false);
    // The first m4a_engine_process would boot the engine quietly (the
    // hardware's power-on up to the first playing SoundMain) and discard
    // that audio, so the timeline's sample 0 is the GBA clock where the boot
    // ends. Boot it here, before the hook goes in: the boot's own SoundMains
    // are not the song's, and its end is where positions count from.
    m4a_engine_boot(engine.get(), false);
    HookContext ctx{out, timeline.sampleRate / double(GBA_CPU_HZ), engine->gba.now};
    engine->frameEndHook = collectFrame;
    engine->frameEndHookUser = &ctx;
    out->frames.reserve(size_t(double(out->endSample) / timeline.sampleRate * 60.0) + 16);

    // In-game the tracks past the song's player budget never start.
    const uint32_t muteMask = trackBudgetMuteMask(settings.trackBudget);
    constexpr uint32_t kChunk = 4096;
    std::vector<float> left(kChunk), right(kChunk);
    bool cancelled = false;
    uint64_t pos = 0;
    while (pos < out->endSample) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            cancelled = true;
            break;
        }
        const uint32_t n = uint32_t(std::min<uint64_t>(kChunk, out->endSample - pos));
        player->render(engine.get(), &timeline, left.data(), right.data(), n, looping, muteMask);
        pos += n;
    }
    engine->frameEndHook = nullptr;
    engine->frameEndHookUser = nullptr;
    m4a_engine_destroy(engine.get());
    if (cancelled)
        return false;

    // The engine runs a little ahead of the samples it has handed out; a
    // frame past the span belongs to the next pass.
    while (!out->frames.empty() && out->frames.back().samplePos >= out->endSample)
        out->frames.pop_back();
    for (size_t i = 0; i < out->frames.size(); i++) {
        CpuProfileFrame &frame = out->frames[i];
        frame.tick =
            uint32_t(std::min<double>(timeline.tickForSample(frame.samplePos), double(UINT32_MAX)));
        if (frame.total > out->maxTotal) {
            out->maxTotal = frame.total;
            out->maxIndex = i;
        }
    }
    out->renderSeconds = double(clock.nsecsElapsed()) / 1e9;
    return true;
}

CpuProfiler::CpuProfiler(QObject *parent) : QObject(parent) {}

CpuProfiler::~CpuProfiler()
{
    cancel();
}

void CpuProfiler::join()
{
    if (m_thread.joinable())
        m_thread.join();
}

void CpuProfiler::cancel()
{
    if (m_cancel)
        m_cancel->store(true);
    join();
    m_cancel.reset();
    m_token++;
}

void CpuProfiler::start(std::unique_ptr<MidiTimeline> timeline, const LoadedVoiceGroup *voicegroup,
                        const SongSettings &settings, uint64_t generation)
{
    cancel();
    if (!timeline)
        return;
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    m_jobsStarted++;
    const uint64_t token = m_token;
    std::shared_ptr<std::atomic<bool>> flag = m_cancel;
    std::shared_ptr<const MidiTimeline> owned(std::move(timeline));
    m_thread = std::thread([this, owned, voicegroup, settings, generation, token, flag] {
        auto profile = std::make_shared<CpuProfile>();
        if (!renderCpuProfile(*owned, voicegroup, settings, flag.get(), profile.get()))
            return;
        // Back on the owner's thread. A cancel() (or the destructor) joins
        // this thread first, so `this` is alive here, and the event is
        // dropped with the object if it never gets delivered.
        QMetaObject::invokeMethod(
            this,
            [this, profile, generation, token] {
                if (token != m_token)
                    return; // superseded meanwhile
                join();
                m_cancel.reset();
                emit finished(profile, generation);
            },
            Qt::QueuedConnection);
    });
}
