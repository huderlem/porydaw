#pragma once

#include <QObject>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "audio/audioengine.h"
#include "core/miditimeline.h"

extern "C" {
#include "voicegroup_loader.h"
}

// The offline CPU profile (docs/cpu-meter/PLAN.md D3, step 7): the song
// rendered once through a private engine set up exactly as playback's
// (SongSettings: mix rate, polyphony, HQ mixer, reverb, volume, the track
// budget), with the cost hooks on, keeping every SoundMain's cost. One frame
// per GBA frame, in order.
struct CpuProfileFrame {
    uint64_t samplePos; // where the frame's SoundMain ran, on the timeline
    uint32_t tick;      // the same position in song ticks (floor)
    int32_t total;      // sequencer + cgb + mixer
    int32_t sequencer;
    int32_t cgb;
    int32_t mixer;
};

struct CpuProfile {
    std::vector<CpuProfileFrame> frames;
    int32_t maxTotal = 0;
    size_t maxIndex = 0;
    // The span profiled: a looping song once through to its loop end; any
    // other to its last event plus AudioEngine::kTailSeconds of release
    // tail, as playback renders it before auto-stopping.
    uint64_t endSample = 0;
    uint64_t endTick = 0;
    double sampleRate = 0.0;
    double renderSeconds = 0.0; // wall time the render took
    // A song longer than kMaxSeconds is not profiled (skipped stays true,
    // frames empty).
    bool skipped = false;

    // Past this many seconds of song the profile is skipped. The render runs
    // at ~500x real time with the stock mixer and ~370x with the HQ mixer
    // at 42048 Hz (--cpuprofilecheck, a WSL2 desktop), so the longest
    // profile takes under 2 s there.
    static constexpr double kMaxSeconds = 600.0;

    // The frame whose SoundMain covers `tick` (the last at or before it), or
    // null outside the profile.
    const CpuProfileFrame *frameAtTick(double tick) const;
};

// Renders the profile synchronously. `cancel` (optional) is polled between
// render chunks; a cancelled render returns false and leaves *out partial.
// The timeline and voicegroup are only read (the voicegroup must stay alive
// for the call: the caller cancels and waits before freeing it).
bool renderCpuProfile(const MidiTimeline &timeline, const LoadedVoiceGroup *voicegroup,
                      const SongSettings &settings, const std::atomic<bool> *cancel,
                      CpuProfile *out);

// Runs renderCpuProfile on a worker thread, one job at a time. start()
// cancels (and joins) the job in flight before starting the next; a
// finished job's profile arrives on the owner's thread through finished(),
// tagged with the generation it was started with, unless a later start() or
// cancel() superseded it. UI thread only.
class CpuProfiler : public QObject
{
    Q_OBJECT

  public:
    explicit CpuProfiler(QObject *parent = nullptr);
    ~CpuProfiler() override;

    // Takes the timeline (an owned copy); borrows the voicegroup until the
    // job ends or cancel() returns.
    void start(std::unique_ptr<MidiTimeline> timeline, const LoadedVoiceGroup *voicegroup,
               const SongSettings &settings, uint64_t generation);
    // Cancels the job in flight and waits for its thread (a GBA frame's
    // render at most): after it returns nothing reads the voicegroup.
    void cancel();
    // Jobs started, ever (harness).
    uint64_t jobsStarted() const { return m_jobsStarted; }

  signals:
    void finished(std::shared_ptr<const CpuProfile> profile, uint64_t generation);

  private:
    void join();

    std::thread m_thread;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    uint64_t m_token = 0; // the job whose result is still wanted
    uint64_t m_jobsStarted = 0;
};
