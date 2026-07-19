#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <vector>

// Value types for the Sample Studio pipeline (docs/sample-studio/PLAN.md §2).
// The editing model is non-destructive: an immutable decoded ImportedSample
// plus an equality-comparable SampleEditParams, rendered deterministically to
// a ProcessedSample by SampleDocument (pipeline order DSP.md §1).

// An immutable decoded source. The buffer is mono float32 at the source's
// native rate on the canonical scale value = s8/128 (full scale 1.0 ⇔ 128,
// DSP.md §2 conventions).
struct ImportedSample {
    enum SourceKind { Wav, Aif, Mp3, Flac, Ogg, Sf2 };

    std::vector<float> buffer;
    // True capture rate. When the source carried an agbp chunk this is the
    // agbp-derived exact rate (agbp/1024 · 2^((key+frac−60)/12), the inverse
    // of FORMATS.md §3) — fractional rates like 3344.75 Hz are normal.
    double sampleRate = 0.0;
    int baseKey = 60;          // smpl unity note / AIFF INST base note
    double fracSemitone = 0.0; // standard semantics, ∈ [0, 1)
    bool hasLoop = false;
    qint64 loopStart = 0;      // inclusive source frame
    qint64 loopEndIncl = 0;    // inclusive; agbl override already applied
    // Playable length for one-shots: agbl override when present, else the
    // full frame count (the engine never plays past a nonzero agbl).
    qint64 playLength = 0;
    quint32 exactPitch = 0;    // source agbp word verbatim (0 = absent)
    QString suggestedName;     // sanitized from the source basename
    QString sourcePath;
    SourceKind sourceKind = Wav;

    // Decode diagnostics (display + default-parameter policy).
    int sourceChannels = 1;
    int sourceBits = 0;        // container bits per sample
    bool sourceFloat = false;
    // 8-bit unsigned mono PCM .wav — the shipped-GBA shape. Prepared files
    // get no-op default params so a phase-1-style import stays byte-faithful.
    bool gbaReady = false;
    // Stereo source whose channels phase-cancel (negative L/R correlation);
    // the importer downmixed anyway, the caller may offer left-only.
    bool phaseCancelStereo = false;
    QStringList warnings;      // clipped floats, phase-cancelling stereo, ...

    qint64 frameCount() const { return qint64(buffer.size()); }
};

// The user-editable parameter set. All positions are source-domain frame
// indices (crop end exclusive, loop end inclusive); the render maps them to
// the final grid stage by stage (DSP.md §1 marker bookkeeping).
struct SampleEditParams {
    enum NormalizeMode { NormalizeAuto, NormalizeLooped, NormalizeOneShot,
                         NormalizeOff };
    enum Toggle { Auto, On, Off };

    qint64 cropStart = 0; // inclusive
    qint64 cropEnd = 0;   // exclusive
    bool loopOn = false;
    qint64 loopStart = 0;   // inclusive, uncropped source coords
    qint64 loopEnd = 0;     // inclusive, uncropped source coords
    int baseKey = 60;       // smpl unity note to export
    double fineTuneCents = 0.0; // [0, 100) — fraction of a semitone above key
    double targetRate = 0.0;    // output rate (doubles allowed, FORMATS.md §1)
    NormalizeMode normalizeMode = NormalizeAuto;
    Toggle dcRemove = Auto; // Auto = on when looped (DSP.md §5 guards)
    bool fadeIn = true;     // 1.5 ms raised-cosine at the final start
    bool fadeOut = true;    // one-shots: 5 ms raised-cosine to exactly 0
    bool crossfadeOn = false; // loop-seam crossfade bake (lands in phase 3)
    bool ditherOn = false;    // TPDF ±1 LSB, fixed seed (DSP.md §2)
    // Carry a source agbp word verbatim while the pipeline leaves pitch
    // untouched (identity rate, source key); 0 = compute from FORMATS.md §3.
    quint32 exactPitchOverride = 0;

    bool operator==(const SampleEditParams &o) const
    {
        return cropStart == o.cropStart && cropEnd == o.cropEnd
            && loopOn == o.loopOn && loopStart == o.loopStart
            && loopEnd == o.loopEnd && baseKey == o.baseKey
            && fineTuneCents == o.fineTuneCents && targetRate == o.targetRate
            && normalizeMode == o.normalizeMode && dcRemove == o.dcRemove
            && fadeIn == o.fadeIn && fadeOut == o.fadeOut
            && crossfadeOn == o.crossfadeOn && ditherOn == o.ditherOn
            && exactPitchOverride == o.exactPitchOverride;
    }
    bool operator!=(const SampleEditParams &o) const { return !(*this == o); }
};

// Post-quantize loop-seam click metrics (DSP.md §6), in signed-8 LSB.
struct SeamMetrics {
    bool valid = false;
    int ampLsb = 0;   // first-order continuation error at the wrap
    int derivLsb = 0; // slope mismatch across the wrap
    double ncc = 0.0; // normalized cross-correlation of the seam windows
};

// The deterministic render: final signed-8 bytes plus the GBA header fields
// and export metadata (FORMATS.md §1-3). freq/loopStart/size/looped are
// exactly the WaveData header the build and pory4a's loader will derive.
struct ProcessedSample {
    QByteArray s8;          // final quantized samples (size == this->size)
    quint32 freq = 0;       // agbp pitch word = round(effectiveRate·1024)
    quint32 loopStart = 0;  // output-domain
    quint32 size = 0;       // output-domain sample count (loop end == end)
    bool looped = false;
    // .wav export metadata:
    quint32 declaredRate = 0;  // fmt sampleRate = round(outputRate)
    int unityNote = 60;        // smpl dwMIDIUnityNote
    quint32 pitchFraction = 0; // smpl dwMIDIPitchFraction, standard semantics
    // Diagnostics / readouts:
    double outputRate = 0.0;    // exact final grid rate (after any loop nudge)
    double effectiveRate = 0.0; // playback rate at middle C
    double normalizeGain = 1.0; // linear gain the normalize stage applied
    SeamMetrics seam;
    QStringList warnings;
    std::vector<float> preview; // final-stage floats, pre-quantize (A/B)
};
