#pragma once

#include <QByteArray>

#include "sampledata.h"

// The Sample Studio .wav writer (docs/sample-studio/FORMATS.md §1): 8-bit
// unsigned PCM mono RIFF/WAVE with chunks fmt/data/smpl/agbp/agbl, the same
// shape as every shipped pokeemerald sample. wav2agb's u8→s8 conversion is a
// lossless x − 128, so these bytes are bit-identical to the ROM's .bin data.
// Deliberately separate from src/audio/wavexport.cpp (the song exporter).
QByteArray writeSampleWav(const ProcessedSample &sample);
