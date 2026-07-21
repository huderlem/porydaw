#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include "audio/sampledata.h"

// DirectSound sample registration for wav2agb-pipeline decomp projects
// (docs/sample-studio/FORMATS.md §4): verbatim-copy a prepared .wav into
// sound/direct_sound_samples/ and append its registration block to
// sound/direct_sound_data.inc. Follows the VoicegroupSource house rules:
// byte-conservative CRLF-preserving QSaveFile writes, actionable refusals.

// The project's sample build pipeline, probed from its make rules and sound
// data files. A non-empty refusal means samples can't be registered here; the
// message says what's missing and what to do about it.
struct SampleFormatProbe {
    enum Pipeline { Wav2Agb, LegacyAif, Unknown };
    Pipeline pipeline = Unknown;
    QString incPath;    // sound/direct_sound_data.inc (absolute)
    QString samplesDir; // sound/direct_sound_samples (absolute)
    QString refusal;    // empty when registration can proceed
    bool ok() const { return refusal.isEmpty(); }
};

// Light header inspection of a prepared (already GBA-ready) .wav: enough to
// refuse files wav2agb or porydaw's loader would reject, and to display the
// pitch/loop metadata that will take effect. Full hi-res decoding is the
// phase-2 import pipeline's job, not this.
struct SampleWavInfo {
    int formatTag = 0; // 1 = integer PCM, 3 = float
    int channels = 0;
    int bitsPerSample = 0;
    quint32 sampleRate = 0; // fmt declared rate
    quint32 numSamples = 0; // data length / bytes-per-sample
    // smpl chunk:
    bool hasSmpl = false;
    quint32 midiKey = 60;       // dwMIDIUnityNote, clamped to 127
    quint32 pitchFraction = 0;  // dwMIDIPitchFraction, raw
    bool loopEnabled = false;
    quint32 loopStart = 0;   // inclusive sample index
    quint32 loopEndIncl = 0; // inclusive sample index
    // custom chunks (0 = absent — both are nonzero in every real file):
    quint32 agbPitch = 0;
    quint32 agbLoopEnd = 0;
    // The WaveData header the build and porydaw's loader derive from the
    // above, mirroring load_wav_from_path's math exactly.
    quint32 waveFreq = 0;
    quint32 waveLoopStart = 0;
    quint32 waveSize = 0;
    bool waveLooped = false;
};

// Provenance sidecar for a committed sample (.porydaw/samples/<name>.json,
// docs/sample-studio/PLAN.md §3): which source file it came from (content
// hash guards against edits behind porydaw's back, plus the decode choices
// that aren't in the params) and the full edit parameters, so "Edit sample…"
// reopens the hi-res source exactly where the user left it. The project is
// canonical without it — a missing sidecar means re-importing the committed
// 8-bit .wav.
struct SampleSidecar {
    int version = 1;
    QString sourcePath;   // absolute path of the imported source file
    QString sourceSha256; // hex SHA-256 of the source file's bytes
    bool leftOnly = false; // stereo source imported as left channel only
    int sf2Zone = -1;      // .sf2 zone index; -1 for ordinary audio files
    SampleEditParams params;
};

class SampleRegistrar
{
public:
    // Detects the project's sample pipeline. Refuses (with instructions)
    // when sound/direct_sound_data.inc is missing, when the project builds
    // samples from .aif via aif2pcm (pre-wav2agb fork), or when no wav2agb
    // %.bin: %.wav pattern rule exists in its make files.
    static SampleFormatProbe probeSampleFormat(const QString &projectRoot);

    // A registration-grammar name derived from arbitrary text (a source file
    // basename): lowercased, runs of other characters collapsed to '_'.
    static QString sanitizeSampleName(const QString &raw);

    // Grammar ([a-z0-9_]+) plus collision checks against the project's
    // declared symbols (pass VoicegroupSource::directSoundSymbols) and
    // on-disk sample files.
    static bool validateSampleName(const QString &projectRoot, const QString &name,
                                   const QStringList &existingSymbols,
                                   QString *error);

    // Parses the RIFF header chunks (fmt/data/smpl/agbp/agbl) and refuses
    // files the pipeline can't take: non-mono, unsupported sample formats,
    // multiple smpl loops, non-forward loops.
    static bool inspectSampleWav(const QByteArray &bytes, SampleWavInfo *info,
                                 QString *error);

    // Writes sound/direct_sound_samples/<name>.wav verbatim, then appends the
    // registration block to sound/direct_sound_data.inc matching its line
    // endings and entry style. Re-validates pipeline and name; both writes
    // are QSaveFile-atomic and the .inc gains exactly one block.
    static bool registerSample(const QString &projectRoot, const QString &name,
                               const QByteArray &wavBytes, QString *error);

    // Overwrites an already-registered sample's .wav in place ("Edit
    // sample…" commit). The registration block is untouched — the symbol
    // must already exist, and only .wav-sourced samples can be updated.
    static bool updateSample(const QString &projectRoot, const QString &name,
                             const QByteArray &wavBytes, QString *error);

    // Provenance sidecar plumbing. Reads validate the version and required
    // fields; writes are QSaveFile-atomic. The sidecar is auxiliary — a
    // failed write must not fail the commit that triggered it.
    static QString sampleSidecarPath(const QString &projectRoot,
                                     const QString &name);
    static QString sourceHashHex(const QByteArray &sourceBytes);
    static bool writeSampleSidecar(const QString &projectRoot,
                                   const QString &name,
                                   const SampleSidecar &sidecar,
                                   QString *error);
    static bool readSampleSidecar(const QString &projectRoot,
                                  const QString &name, SampleSidecar *sidecar);
    static void removeSampleSidecar(const QString &projectRoot,
                                    const QString &name);
};
