#pragma once

#include <QString>

#include "sampledata.h"

// Hi-res sample decoding front door (docs/sample-studio/PLAN.md §2
// SampleImport): any supported source file → an immutable ImportedSample,
// dispatching on sniffed magic, never extension. Containers: .wav (vendored
// dr_wav) and .aif (small in-house reader cribbed from pory4a's
// load_aif_from_path, kept hi-res); compressed sources via vendored dr_mp3 /
// dr_flac / stb_vorbis — decode-only, no pitch or loop metadata, so the
// editor prefills from pitch detection.
//
// Stereo sources downmix by arithmetic channel mean; when the full-file L/R
// correlation is negative the sample carries a warning and the caller may
// re-import with leftChannelOnly (DSP.md §2).
bool importAudioFile(const QString &path, ImportedSample *out, QString *error,
                     bool leftChannelOnly = false);

// Same, over in-memory bytes (the harness feeds generated fixtures without
// touching disk more than once). sourcePath only labels the result.
bool importAudioBytes(const QByteArray &bytes, const QString &sourcePath,
                      ImportedSample *out, QString *error,
                      bool leftChannelOnly = false);
