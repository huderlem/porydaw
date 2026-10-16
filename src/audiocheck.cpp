#include <cstdio>

#include "audio/audioengine.h"

// --audiocheck: report which audio backend the output device landed on
// (self-contained, no project needed). Field diagnostic for silent-playback
// reports: WSL without the PulseAudio client library (or any headless box)
// falls through to miniaudio's null device, which plays time but no sound —
// this prints the backend name, sample rate, and whether that silent
// fallback is in effect. Fails only if the engine's backend report is
// internally inconsistent.
int runAudioCheck()
{
    AudioEngine engine;
    QString error;
    if (!engine.init(&error)) {
        std::printf("audiocheck: SKIP (no audio device: %s)\n", qUtf8Printable(error));
        return 0;
    }

    const QString backend = engine.backendName();
    const double periodMs =
        engine.sampleRate() > 0 ? 1000.0 * engine.periodSizeFrames() / engine.sampleRate() : 0.0;
    std::printf("audiocheck: backend=%s rate=%d period=%dx%d frames "
                "(~%.0f ms) silent-null-fallback=%s\n",
                qUtf8Printable(backend), int(engine.sampleRate()), engine.periodCount(),
                engine.periodSizeFrames(), periodMs, engine.usingNullBackend() ? "yes" : "no");

    int failures = 0;
    if (backend.isEmpty()) {
        std::fprintf(stderr, "audiocheck: FAIL: empty backend name\n");
        failures++;
    }
    if (engine.usingNullBackend() != (backend == QStringLiteral("Null"))) {
        std::fprintf(stderr, "audiocheck: FAIL: null-backend flag disagrees with "
                             "the backend name\n");
        failures++;
    }

    // Reopening with other buffering (Settings → Audio → Output device)
    // must stay on the same backend: exclusive mode WASAPI refuses (another
    // app holds the device) falls back to shared WASAPI with a warning,
    // never to DirectSound or the silent Null device.
    if (!engine.usingNullBackend()) {
        AudioDeviceConfig configs[4];
        configs[0].periodMs = 3;
        configs[1].periodMs = 3;
        configs[1].exclusive = true;
        configs[2].exclusive = true; // Default buffer size in exclusive mode
        // configs[3]: back to the defaults
        for (const AudioDeviceConfig &config : configs) {
            QString reopenError;
            const bool ok = engine.reopenDevice(config, &reopenError);
            std::printf("audiocheck: reopen periodMs=%d exclusive=%s -> %s, %s %s, period=%dx%d "
                        "frames (~%.1f ms)%s%s%s%s\n",
                        config.periodMs, config.exclusive ? "yes" : "no", ok ? "ok" : "FAILED",
                        qUtf8Printable(engine.backendName()),
                        engine.exclusiveMode() ? "exclusive" : "shared", engine.periodCount(),
                        engine.periodSizeFrames(),
                        1000.0 * engine.periodSizeFrames() / engine.sampleRate(),
                        engine.deviceWarning().isEmpty() ? "" : " warning: ",
                        qUtf8Printable(engine.deviceWarning()),
                        reopenError.isEmpty() ? "" : " error: ", qUtf8Printable(reopenError));
            if (!ok) {
                std::fprintf(stderr, "audiocheck: FAIL: the device didn't reopen\n");
                failures++;
            }
            if (engine.backendName() != backend) {
                std::fprintf(stderr, "audiocheck: FAIL: reopening moved to the %s backend\n",
                             qUtf8Printable(engine.backendName()));
                failures++;
            }
            // Exclusive's Default is small periods, not shared mode's 10 ms.
            const double periodMs = 1000.0 * engine.periodSizeFrames() / engine.sampleRate();
            if (engine.exclusiveMode() && config.periodMs == 0 &&
                !qEnvironmentVariableIsSet("PORYDAW_AUDIO_PERIOD_MS") &&
                periodMs > AudioDeviceConfig::kExclusiveDefaultPeriodMs + 1) {
                std::fprintf(stderr, "audiocheck: FAIL: exclusive Default opened %.1f ms periods\n",
                             periodMs);
                failures++;
            }
            if (engine.exclusiveMode() && !config.exclusive) {
                std::fprintf(stderr, "audiocheck: FAIL: exclusive mode without asking\n");
                failures++;
            }
        }
    }

    if (failures == 0)
        std::printf("audiocheck: PASS\n");
    return failures ? 1 : 0;
}
