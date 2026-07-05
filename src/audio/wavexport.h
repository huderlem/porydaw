#pragma once

#include <QString>
#include <cstdint>
#include <functional>

#include "audio/audioengine.h"
#include "core/miditimeline.h"

extern "C" {
#include "voicegroup_loader.h"
}

// Offline WAV export (SPEC §7): renders a timeline through a private
// M4AEngine faster than realtime and writes 16-bit stereo PCM. Length
// semantics match poryaaaa_render: a looping song plays its intro plus
// loopCount loop bodies and then fades out linearly over fadeoutSeconds;
// a song without loop markers plays once with tailSeconds of ring-out
// after its last event.
struct WavExportOptions {
    int sampleRate = 48000;
    int loopCount = 2;           // loop-body playthroughs before the fadeout
    double fadeoutSeconds = 5.0; // looping songs only
    double tailSeconds = 3.0;    // non-looping songs only
};

struct WavExportTotals {
    uint64_t totalSamples = 0;
    uint64_t fadeStartSample = UINT64_MAX; // UINT64_MAX = no fadeout
};

// The render length implied by a timeline and options (also drives the
// export dialog's duration preview and the harness's expected file size).
// The timeline must have been built at opts.sampleRate.
WavExportTotals wavExportTotals(const MidiTimeline &timeline, const WavExportOptions &opts);

// Renders and writes the file, streaming in chunks so memory use is flat.
// progress (optional) receives a 0..1 fraction; returning false cancels the
// export, which removes the partial file and fails with an empty *error.
// Real failures set a non-empty *error.
bool exportWav(const QString &path, const MidiTimeline &timeline,
               const LoadedVoiceGroup *voicegroup, const SongSettings &settings,
               const WavExportOptions &opts,
               const std::function<bool(double)> &progress, QString *error);
