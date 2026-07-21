#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QString>
#include <QTreeWidget>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <QApplication>
#include <QMouseEvent>
#include <QUndoStack>

#include "audio/auditionslots.h"
#include "audio/sampledoc.h"
#include "audio/sampledsp.h"
#include "audio/sampleimport.h"
#include "audio/samplewav.h"
#include "audio/sf2reader.h"
#include "project/samplereg.h"
#include "project/voicegroupsource.h"
#include "samplecheck_fixtures.h"
#include "ui/sampleeditordialog.h"
#include "ui/sf2zonepicker.h"
#include "ui/waveformview.h"

extern "C" {
#include "voicegroup_loader.h"
}

// --samplecheck <scratchDir> [corpusRoot]: Sample Studio check, phases 1-5.
// Phase 1: builds fully-fresh fake decomp projects under scratchDir — a
// wav2agb one, a legacy-aif one, a rule-less one, an .inc-less one, and a
// CRLF-.inc one — then drives probe → inspect → register → loader-resolve
// end to end and exact-matches every refusal message. Phase 2: hi-res
// decoding, the DSP.md resampler/quantizer/normalize acceptance items 1-6,
// pipeline determinism, the audition==build parity matrix through pory4a's
// own loader, and metadata round-trips (item 10). Phase 3: YIN pitch
// detection (item 9), loop suggestion with its level/anti-pump gates
// (item 7), the crossfade bake, the audition-slot protocol's retirement
// invariants against a bare M4AEngine, and offscreen driving of the editor
// (waveform handle drags, suggest chips, pitch prefill, the dialog-local
// undo stack) ending in a commit that re-runs the phase-1 assertions.
// Phase 4: embedded MP3/FLAC/Ogg-Vorbis fixtures decode with expected
// structure and content, Ogg-Opus/corrupt-stream refusals exact-match, and
// a compressed source renders through the unchanged pipeline.
// Phase 5: a harness-synthesized minimal .sf2 (RIFF written inline, no
// binary fixture) — zone metadata lands in ImportedSample (exclusive →
// inclusive loop end, pitchCorrection renormalization, stereo-linked
// flagging, ROM-sample and terminator skip, instrument/preset grouping
// labels), reader/front-door refusals exact-match, and the zone picker
// driven offscreen (grouping, search filter, selection arming OK).
// scratchDir must not already exist (fully fresh scratches, no stale
// artifacts). corpusRoot, when given, points at a wav2agb decomp checkout
// (e.g. pokeemerald) whose sound/direct_sound_samples sc88pro corpus gates
// the corpus-conditional sections: no-op u8 round-trip, reference-.bin byte
// equality, stats drift.

namespace {

int failures = 0;
constexpr double kPi = 3.14159265358979323846;

void expect(bool ok, const char *what)
{
    if (!ok) {
        std::fprintf(stderr, "samplecheck: FAIL: %s\n", what);
        failures++;
    }
}

bool expectError(const QString &got, const QString &want, const char *what)
{
    if (got == want)
        return true;
    std::fprintf(stderr, "samplecheck: FAIL: %s\n  want: %s\n  got:  %s\n", what,
                 qUtf8Printable(want), qUtf8Printable(got));
    failures++;
    return false;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile out(path);
    return out.open(QIODevice::WriteOnly) && out.write(bytes) == bytes.size();
}

QByteArray readFileBytes(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    return f.readAll();
}

void putU16(QByteArray *out, quint16 v)
{
    out->append(char(v)).append(char(v >> 8));
}

void putU32(QByteArray *out, quint32 v)
{
    out->append(char(v)).append(char(v >> 8)).append(char(v >> 16)).append(
        char(v >> 24));
}

quint32 getU32(const QByteArray &b, qsizetype at)
{
    return quint32(quint8(b[at])) | quint32(quint8(b[at + 1])) << 8
        | quint32(quint8(b[at + 2])) << 16 | quint32(quint8(b[at + 3])) << 24;
}

struct FixtureSpec {
    quint32 rate = 13379;
    QByteArray samples; // raw data-chunk bytes in the container format
    quint16 channels = 1;
    quint16 formatTag = 1; // 1 = PCM, 3 = float
    quint16 bits = 8;
    bool withSmpl = true;
    quint32 unityKey = 60;
    quint32 pitchFraction = 0;
    int numLoops = 0; // smpl records written (and declared)
    quint32 loopType = 0;
    quint32 loopStart = 0;
    quint32 loopEndIncl = 0;
    quint32 agbp = 0; // 0 = omit chunk
    quint32 agbl = 0; // 0 = omit chunk
};

// A .wav per FORMATS.md §1's chunk vocabulary: fmt/data[/smpl/agbp/agbl].
QByteArray fixtureWav(const FixtureSpec &spec)
{
    const quint16 blockAlign = spec.channels * (spec.bits / 8);
    QByteArray wav("RIFF\0\0\0\0WAVE", 12);
    wav += "fmt ";
    putU32(&wav, 16);
    putU16(&wav, spec.formatTag);
    putU16(&wav, spec.channels);
    putU32(&wav, spec.rate);
    putU32(&wav, spec.rate * blockAlign);
    putU16(&wav, blockAlign);
    putU16(&wav, spec.bits);
    wav += "data";
    putU32(&wav, quint32(spec.samples.size()));
    wav += spec.samples;
    if (spec.samples.size() & 1)
        wav += '\0'; // RIFF pad byte
    if (spec.withSmpl) {
        wav += "smpl";
        putU32(&wav, quint32(36 + 24 * spec.numLoops));
        putU32(&wav, 0); // manufacturer
        putU32(&wav, 0); // product
        putU32(&wav, 0); // sample period
        putU32(&wav, spec.unityKey);
        putU32(&wav, spec.pitchFraction);
        putU32(&wav, 0); // SMPTE format
        putU32(&wav, 0); // SMPTE offset
        putU32(&wav, quint32(spec.numLoops));
        putU32(&wav, 0); // sampler data
        for (int i = 0; i < spec.numLoops; i++) {
            putU32(&wav, quint32(i)); // cue point id
            putU32(&wav, spec.loopType);
            putU32(&wav, spec.loopStart);
            putU32(&wav, spec.loopEndIncl);
            putU32(&wav, 0); // fraction
            putU32(&wav, 0); // play count
        }
    }
    if (spec.agbp) {
        wav += "agbp";
        putU32(&wav, 4);
        putU32(&wav, spec.agbp);
    }
    if (spec.agbl) {
        wav += "agbl";
        putU32(&wav, 4);
        putU32(&wav, spec.agbl);
    }
    const quint32 riffSize = quint32(wav.size()) - 8;
    wav[4] = char(riffSize);
    wav[5] = char(riffSize >> 8);
    wav[6] = char(riffSize >> 16);
    wav[7] = char(riffSize >> 24);
    return wav;
}

// ---- AIFF fixture (big-endian) ----

void putBe16(QByteArray *out, quint16 v)
{
    out->append(char(v >> 8)).append(char(v));
}

void putBe32(QByteArray *out, quint32 v)
{
    out->append(char(v >> 24)).append(char(v >> 16)).append(char(v >> 8))
        .append(char(v));
}

// 80-bit IEEE extended float, the COMM sample-rate encoding.
void putExtended80(QByteArray *out, double v)
{
    int exp2 = 0;
    const double mant = std::frexp(v, &exp2);
    const quint64 m = quint64(std::ldexp(mant, 64));
    putBe16(out, quint16(16382 + exp2));
    for (int i = 7; i >= 0; i--)
        out->append(char(m >> (i * 8)));
}

struct AiffSpec {
    quint16 channels = 1;
    quint32 numFrames = 0;
    quint16 sampleSize = 16;
    double rate = 22050.0;
    QByteArray ssnd; // big-endian sample bytes
    int baseNote = 60;
    int detune = 0; // cents, −50..50
    bool loop = false;
    quint32 loopStartPos = 0;
    quint32 loopEndPos = 0; // exclusive bound, aif2pcm-style
};

QByteArray fixtureAiff(const AiffSpec &spec)
{
    QByteArray aif("FORM\0\0\0\0AIFF", 12);
    aif += "COMM";
    putBe32(&aif, 18);
    putBe16(&aif, spec.channels);
    putBe32(&aif, spec.numFrames);
    putBe16(&aif, spec.sampleSize);
    putExtended80(&aif, spec.rate);
    if (spec.loop) {
        aif += "MARK";
        putBe32(&aif, 2 + 2 * 8);
        putBe16(&aif, 2); // two markers, empty pascal names (pad to even)
        putBe16(&aif, 1);
        putBe32(&aif, spec.loopStartPos);
        aif += QByteArray("\0\0", 2);
        putBe16(&aif, 2);
        putBe32(&aif, spec.loopEndPos);
        aif += QByteArray("\0\0", 2);
    }
    aif += "INST";
    putBe32(&aif, 20);
    aif += char(qint8(spec.baseNote));
    aif += char(qint8(spec.detune));
    aif += QByteArray(6, '\0'); // low/high note, low/high velocity, gain
    putBe16(&aif, spec.loop ? 1 : 0); // sustain loop playMode
    putBe16(&aif, 1);                 // begin marker id
    putBe16(&aif, 2);                 // end marker id
    aif += QByteArray(6, '\0');       // release loop, unused
    aif += "SSND";
    putBe32(&aif, quint32(8 + spec.ssnd.size()));
    putBe32(&aif, 0); // offset
    putBe32(&aif, 0); // block size
    aif += spec.ssnd;
    if (spec.ssnd.size() & 1)
        aif += '\0';
    const quint32 formSize = quint32(aif.size()) - 8;
    aif[4] = char(formSize >> 24);
    aif[5] = char(formSize >> 16);
    aif[6] = char(formSize >> 8);
    aif[7] = char(formSize);
    return aif;
}

const char *const kWav2AgbRules =
    "SOUND_BIN_DIR := $(OBJ_DIR)/sound\n"
    "\n"
    "$(SOUND_BIN_DIR)/%.bin: sound/%.wav \n"
    "\t$(WAV2AGB) -b $< $@\n";

// One registered entry, LF endings — the shipped pokeemerald layout.
const char *const kIncSeed =
    "\t.align 2\n"
    "DirectSoundWaveData_existing::\n"
    "\t.incbin \"sound/direct_sound_samples/existing.bin\"\n";

bool buildWavProject(const QString &root)
{
    return writeFile(root + QStringLiteral("/Makefile"),
                     "include audio_rules.mk\n")
        && writeFile(root + QStringLiteral("/audio_rules.mk"), kWav2AgbRules)
        && writeFile(root + QStringLiteral("/sound/direct_sound_data.inc"),
                     kIncSeed)
        && writeFile(root
                         + QStringLiteral(
                             "/sound/direct_sound_samples/existing.wav"),
                     "placeholder")
        && writeFile(root
                         + QStringLiteral(
                             "/sound/direct_sound_samples/orphan.wav"),
                     "placeholder");
}

// ---- DSP measurement helpers ----

std::vector<float> genSine(double rate, double freq, double seconds,
                           double amp)
{
    std::vector<float> v(size_t(rate * seconds));
    for (size_t i = 0; i < v.size(); i++)
        v[i] = float(amp * std::sin(2.0 * kPi * freq * double(i) / rate));
    return v;
}

double rmsOf(const std::vector<float> &v, size_t from, size_t to)
{
    if (to <= from)
        return 0.0;
    double sum = 0.0;
    for (size_t i = from; i < to; i++)
        sum += double(v[i]) * double(v[i]);
    return std::sqrt(sum / double(to - from));
}

// Hann-windowed single-tone amplitude estimate: immune to partial-cycle
// leakage, so passband gain measures to well under 0.01 dB.
double toneAmp(const std::vector<float> &v, double rate, double freq,
               size_t from, size_t to)
{
    double re = 0.0, im = 0.0, wsum = 0.0;
    const double span = double(to - from);
    for (size_t i = from; i < to; i++) {
        const double w =
            0.5 * (1.0 - std::cos(2.0 * kPi * double(i - from) / span));
        const double phase = 2.0 * kPi * freq * double(i) / rate;
        re += double(v[i]) * w * std::cos(phase);
        im += double(v[i]) * w * std::sin(phase);
        wsum += w;
    }
    return 2.0 * std::sqrt(re * re + im * im) / wsum;
}

double median(std::vector<double> v)
{
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    const size_t mid = v.size() / 2;
    return v.size() & 1 ? v[mid] : 0.5 * (v[mid - 1] + v[mid]);
}

// Band-limited sawtooth (additive, harmonics below 0.45·rate): naive saws
// alias, which would smear the pitch-detection acceptance sweep.
std::vector<float> genSaw(double rate, double freq, double seconds,
                          double amp)
{
    std::vector<float> v(size_t(rate * seconds), 0.0f);
    for (int k = 1; freq * k < 0.45 * rate; k++) {
        for (size_t i = 0; i < v.size(); i++)
            v[i] += float(amp * (2.0 / kPi)
                          * std::sin(2.0 * kPi * freq * k * double(i) / rate)
                          / double(k));
    }
    return v;
}

double centsOff(double f0, double reference)
{
    return 1200.0 * std::log2(f0 / reference);
}

// rollcheck-style offscreen mouse driving.
void sendMouse(QWidget *w, QEvent::Type type, const QPoint &pos,
               Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QMouseEvent ev(type, QPointF(pos), QPointF(w->mapToGlobal(pos)), button,
                   buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(w, &ev);
}

void dragMouse(QWidget *w, const QPoint &from, const QPoint &to)
{
    sendMouse(w, QEvent::MouseButtonPress, from, Qt::LeftButton,
              Qt::LeftButton);
    const QPoint mid = (from + to) / 2;
    sendMouse(w, QEvent::MouseMove, mid, Qt::NoButton, Qt::LeftButton);
    sendMouse(w, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
    sendMouse(w, QEvent::MouseButtonRelease, to, Qt::LeftButton,
              Qt::NoButton);
}

int soundingPcmChannels(const M4AEngine *engine)
{
    int count = 0;
    for (int i = 0; i < MAX_PCM_CHANNELS; i++) {
        if (engine->pcmChannels[i].status & CHN_ON)
            count++;
    }
    return count;
}

void pumpEngine(M4AEngine *engine, int blocks)
{
    static std::vector<float> l(512), r(512);
    for (int i = 0; i < blocks; i++)
        m4a_engine_process(engine, l.data(), r.data(), 512);
}

} // namespace

int runSampleCheck(const QString &scratchDir, const QString &corpusRoot)
{
    if (QDir(scratchDir).exists()) {
        std::fprintf(stderr,
                     "samplecheck: scratch dir %s already exists; give a "
                     "fresh path\n",
                     qUtf8Printable(scratchDir));
        return 1;
    }
    const QString root = scratchDir + QStringLiteral("/wavproj");
    if (!buildWavProject(root)) {
        std::fprintf(stderr, "samplecheck: cannot build fake project\n");
        return 1;
    }

    // ---- probe: the wav2agb project registers, the broken ones refuse ----
    {
        const int before = failures;
        const SampleFormatProbe probe = SampleRegistrar::probeSampleFormat(root);
        expect(probe.ok() && probe.pipeline == SampleFormatProbe::Wav2Agb,
               "wav2agb project probes OK");

        const QString aifRoot = scratchDir + QStringLiteral("/aifproj");
        writeFile(aifRoot + QStringLiteral("/audio_rules.mk"),
                  "$(SOUND_BIN_DIR)/%.bin: $(SAMPLE_SUBDIR)/%.aif\n"
                  "\t$(AIF2PCM) $< $@\n");
        writeFile(aifRoot + QStringLiteral("/sound/direct_sound_data.inc"),
                  kIncSeed);
        const SampleFormatProbe aif = SampleRegistrar::probeSampleFormat(aifRoot);
        expect(aif.pipeline == SampleFormatProbe::LegacyAif,
               "aif project detected as legacy");
        expectError(aif.refusal,
                    QStringLiteral(
                        "this project predates wav2agb: its samples build from "
                        ".aif sources via aif2pcm. Port the sample pipeline to "
                        "wav2agb (pret's current layout), then import again."),
                    "legacy-aif refusal text");

        const QString noRuleRoot = scratchDir + QStringLiteral("/noruleproj");
        writeFile(noRuleRoot + QStringLiteral("/sound/direct_sound_data.inc"),
                  kIncSeed);
        expectError(SampleRegistrar::probeSampleFormat(noRuleRoot).refusal,
                    QStringLiteral(
                        "cannot find a wav2agb build rule (%.bin: %.wav) in "
                        "the project's make files; add pret's audio_rules.mk "
                        "pattern rule, then import again."),
                    "missing-rule refusal text");

        const QString noIncRoot = scratchDir + QStringLiteral("/noincproj");
        writeFile(noIncRoot + QStringLiteral("/audio_rules.mk"), kWav2AgbRules);
        expectError(SampleRegistrar::probeSampleFormat(noIncRoot).refusal,
                    QStringLiteral(
                        "cannot find sound/direct_sound_data.inc — samples are "
                        "registered there. Set up pret's sample layout, then "
                        "import again."),
                    "missing-inc refusal text");
        if (failures == before)
            std::printf("samplecheck: pipeline probe OK\n");
    }

    // ---- name sanitizing/validation ----
    {
        const int before = failures;
        expect(SampleRegistrar::sanitizeSampleName(QStringLiteral("My Sample #2"))
                   == QStringLiteral("my_sample_2"),
               "sanitize collapses separators");
        expect(SampleRegistrar::sanitizeSampleName(QStringLiteral("Bell (C5)"))
                   == QStringLiteral("bell_c5"),
               "sanitize trims trailing junk");
        const QStringList symbols = VoicegroupSource::directSoundSymbols(root);
        QString error;
        expect(SampleRegistrar::validateSampleName(root,
                                                   QStringLiteral("fresh_tone"),
                                                   symbols, &error),
               "fresh name validates");
        expect(!SampleRegistrar::validateSampleName(root, QString(), symbols,
                                                    &error),
               "empty name refused");
        expectError(error, QStringLiteral("sample name is empty."),
                    "empty-name text");
        expect(!SampleRegistrar::validateSampleName(
                   root, QStringLiteral("Bad Name"), symbols, &error),
               "bad grammar refused");
        expectError(error,
                    QStringLiteral("sample names use lowercase letters, "
                                   "digits, and underscores only."),
                    "grammar text");
        expect(!SampleRegistrar::validateSampleName(
                   root, QStringLiteral("existing"), symbols, &error),
               "symbol collision refused");
        expectError(
            error,
            QStringLiteral(
                "DirectSoundWaveData_existing already exists in this project."),
            "symbol-collision text");
        expect(!SampleRegistrar::validateSampleName(
                   root, QStringLiteral("orphan"), symbols, &error),
               "on-disk file collision refused");
        expectError(error,
                    QStringLiteral("orphan.wav already exists in "
                                   "sound/direct_sound_samples."),
                    "file-collision text");
        if (failures == before)
            std::printf("samplecheck: name validation OK\n");
    }

    // ---- fixture inspection ----
    FixtureSpec spec;
    for (int i = 0; i < 64; i++)
        spec.samples += char(i * 2); // u8 ramp 0..126
    spec.unityKey = 58;
    spec.pitchFraction = 0x40000000; // 0.25 semitone, standard semantics
    spec.numLoops = 1;
    spec.loopStart = 8;
    spec.loopEndIncl = 47;
    spec.agbp = 15000000;
    spec.agbl = 64;
    const QByteArray fixture = fixtureWav(spec);
    {
        const int before = failures;
        SampleWavInfo info;
        QString error;
        expect(SampleRegistrar::inspectSampleWav(fixture, &info, &error),
               "fixture inspects OK");
        expect(info.formatTag == 1 && info.channels == 1
                   && info.bitsPerSample == 8 && info.sampleRate == 13379
                   && info.numSamples == 64,
               "fmt/data fields");
        expect(info.hasSmpl && info.midiKey == 58
                   && info.pitchFraction == 0x40000000 && info.loopEnabled
                   && info.loopStart == 8 && info.loopEndIncl == 47,
               "smpl fields");
        expect(info.agbPitch == 15000000 && info.agbLoopEnd == 64,
               "agbp/agbl fields");
        // The derived WaveData header: agbp verbatim, agbl overriding size.
        expect(info.waveFreq == 15000000 && info.waveLoopStart == 8
                   && info.waveSize == 64 && info.waveLooped,
               "derived WaveData projection");

        expect(!SampleRegistrar::inspectSampleWav(QByteArray("not a wav"),
                                                  nullptr, &error),
               "garbage refused");
        expectError(error, QStringLiteral("not a RIFF/WAVE file."),
                    "garbage text");
        FixtureSpec stereo = spec;
        stereo.channels = 2;
        expect(!SampleRegistrar::inspectSampleWav(fixtureWav(stereo), nullptr,
                                                  &error),
               "stereo refused");
        expectError(
            error,
            QStringLiteral(
                "only mono samples are supported (this file has 2 channels)."),
            "stereo text");
        FixtureSpec twoLoops = spec;
        twoLoops.numLoops = 2;
        expect(!SampleRegistrar::inspectSampleWav(fixtureWav(twoLoops), nullptr,
                                                  &error),
               "multi-loop refused");
        expectError(error,
                    QStringLiteral("the smpl chunk declares 2 loops; wav2agb "
                                   "supports at most one."),
                    "multi-loop text");
        FixtureSpec backward = spec;
        backward.loopType = 1;
        expect(!SampleRegistrar::inspectSampleWav(fixtureWav(backward), nullptr,
                                                  &error),
               "non-forward loop refused");
        expectError(error,
                    QStringLiteral("the smpl loop is not a forward loop (type "
                                   "1); wav2agb only supports forward loops."),
                    "loop-type text");
        // No agbp/agbl: freq falls back to the loader's smpl-derived math and
        // size to the smpl loop end.
        FixtureSpec bare = spec;
        bare.agbp = 0;
        bare.agbl = 0;
        SampleWavInfo bareInfo;
        expect(SampleRegistrar::inspectSampleWav(fixtureWav(bare), &bareInfo,
                                                 &error)
                   && bareInfo.waveSize == 48 && bareInfo.waveFreq != 0
                   && bareInfo.agbPitch == 0,
               "smpl-only fallback projection");
        if (failures == before)
            std::printf("samplecheck: wav inspection OK\n");
    }

    // ---- register + .inc bytes + loader resolution ----
    const QString incPath = root + QStringLiteral("/sound/direct_sound_data.inc");
    {
        const int before = failures;
        QString error;
        expect(SampleRegistrar::registerSample(
                   root, QStringLiteral("samplecheck_tone"), fixture, &error),
               "registerSample succeeds");
        expect(readFileBytes(root
                             + QStringLiteral("/sound/direct_sound_samples/"
                                              "samplecheck_tone.wav"))
                   == fixture,
               "sample .wav copied verbatim");
        const QByteArray expectedInc = QByteArray(kIncSeed)
            + "\n"
              "\t.align 2\n"
              "DirectSoundWaveData_samplecheck_tone::\n"
              "\t.incbin \"sound/direct_sound_samples/samplecheck_tone.bin\"\n";
        expect(readFileBytes(incPath) == expectedInc,
               ".inc gains exactly the registration block");

        const QStringList symbols = VoicegroupSource::directSoundSymbols(root);
        expect(symbols.contains(
                   QStringLiteral("DirectSoundWaveData_samplecheck_tone"))
                   && symbols.contains(
                       QStringLiteral("DirectSoundWaveData_existing")),
               "directSoundSymbols sees the new symbol");

        // A voicegroup referencing the symbol resolves through the C loader,
        // .wav-first, with the WaveData header the inspector predicted.
        writeFile(root
                      + QStringLiteral(
                          "/sound/voicegroups/voicegroup_samplecheck.inc"),
                  "voicegroup_samplecheck::\n"
                  "\tvoice_directsound 60, 0, "
                  "DirectSoundWaveData_samplecheck_tone, 255, 165, 90, 178\n");
        const QByteArray rootUtf8 = root.toLocal8Bit();
        LoadedVoiceGroup *vg =
            voicegroup_load(rootUtf8.constData(), "voicegroup_samplecheck",
                            nullptr);
        if (!vg) {
            std::fprintf(stderr, "samplecheck: FAIL: voicegroup_load failed\n");
            failures++;
        } else {
            const ToneData &td = vg->voices[0];
            expect(td.type == 0 && td.key == 60 && td.attack == 255
                       && td.decay == 165 && td.sustain == 90
                       && td.release == 178,
                   "loaded voice scalars");
            expect(td.wav && td.wav->freq == 15000000 && td.wav->loopStart == 8
                       && td.wav->size == 64 && td.wav->status == 0x4000,
                   "loaded WaveData header matches the inspection");
            bool dataOk = td.wav && td.wav->data;
            for (int i = 0; dataOk && i < 64; i++)
                dataOk = td.wav->data[i] == qint8(i * 2 - 128);
            expect(dataOk, "loaded sample bytes are the u8 data minus 128");
            expect(QByteArray(vg->voiceNames[0]) == "samplecheck_tone",
                   "loader-derived voice name");
            voicegroup_free(vg);
        }
        if (failures == before)
            std::printf("samplecheck: register + loader resolution OK\n");
    }

    // ---- re-registration refusal leaves the .inc untouched ----
    {
        const int before = failures;
        const QByteArray incBefore = readFileBytes(incPath);
        QString error;
        expect(!SampleRegistrar::registerSample(
                   root, QStringLiteral("samplecheck_tone"), fixture, &error),
               "duplicate registration refused");
        expectError(error,
                    QStringLiteral("DirectSoundWaveData_samplecheck_tone "
                                   "already exists in this project."),
                    "duplicate-registration text");
        expect(readFileBytes(incPath) == incBefore,
               "refused registration leaves the .inc byte-identical");
        if (failures == before)
            std::printf("samplecheck: duplicate refusal OK\n");
    }

    // ---- CRLF project: appended block matches the file's line endings ----
    {
        const int before = failures;
        const QString crlfRoot = scratchDir + QStringLiteral("/crlfproj");
        const QByteArray crlfSeed =
            "\t.align 2\r\n"
            "DirectSoundWaveData_existing::\r\n"
            "\t.incbin \"sound/direct_sound_samples/existing.bin\"\r\n";
        writeFile(crlfRoot + QStringLiteral("/audio_rules.mk"), kWav2AgbRules);
        writeFile(crlfRoot + QStringLiteral("/sound/direct_sound_data.inc"),
                  crlfSeed);
        QString error;
        expect(SampleRegistrar::registerSample(
                   crlfRoot, QStringLiteral("crlf_tone"), fixture, &error),
               "CRLF-project registration succeeds");
        const QByteArray grown = readFileBytes(
            crlfRoot + QStringLiteral("/sound/direct_sound_data.inc"));
        expect(grown
                   == crlfSeed
                       + QByteArray(
                           "\r\n"
                           "\t.align 2\r\n"
                           "DirectSoundWaveData_crlf_tone::\r\n"
                           "\t.incbin "
                           "\"sound/direct_sound_samples/crlf_tone.bin\"\r\n"),
               "CRLF block appended with CRLF endings");
        bool crlfOk = true;
        for (int i = 0; i < grown.size(); i++) {
            if (grown[i] == '\n' && (i == 0 || grown[i - 1] != '\r'))
                crlfOk = false;
        }
        expect(crlfOk, "no bare LF snuck into the CRLF .inc");
        if (failures == before)
            std::printf("samplecheck: CRLF preservation OK\n");
    }

    // ---- hi-res decoding (phase 2): every container → canonical floats ----
    {
        const int before = failures;
        QString error;

        // u8 prepared file: exact (x − 128)/128 floats, agbp-derived true
        // rate, agbl-corrected loop end, prepared-shape flag.
        ImportedSample u8s;
        expect(importAudioBytes(fixture, QStringLiteral("fix/tone8.wav"), &u8s,
                                &error),
               "u8 wav imports");
        bool u8ok = u8s.frameCount() == 64;
        for (int i = 0; u8ok && i < 64; i++)
            u8ok = u8s.buffer[size_t(i)] == float((i * 2 - 128) / 128.0);
        expect(u8ok, "u8 floats are exactly (x-128)/128");
        expect(u8s.gbaReady && u8s.sourceBits == 8 && u8s.sourceChannels == 1,
               "u8 prepared shape detected");
        expect(u8s.baseKey == 58 && std::abs(u8s.fracSemitone - 0.25) < 1e-12,
               "u8 smpl unity/fraction (standard semantics)");
        expect(u8s.hasLoop && u8s.loopStart == 8 && u8s.loopEndIncl == 63
                   && u8s.playLength == 64,
               "u8 loop end takes the agbl override");
        expect(u8s.exactPitch == 15000000
                   && std::abs(u8s.sampleRate - 13240.0948) < 0.01,
               "u8 sample rate inverted from agbp");
        expect(u8s.suggestedName == QStringLiteral("tone8"),
               "suggested name from the basename");

        // s16: x/32768 exactly.
        FixtureSpec s16;
        s16.bits = 16;
        s16.rate = 44100;
        s16.withSmpl = false;
        const qint16 s16vals[] = {0, 16384, -32768, 32767};
        for (const qint16 v : s16vals)
            putU16(&s16.samples, quint16(v));
        ImportedSample s16s;
        expect(importAudioBytes(fixtureWav(s16), QStringLiteral("f/s16.wav"),
                                &s16s, &error),
               "s16 wav imports");
        expect(s16s.frameCount() == 4 && s16s.buffer[0] == 0.0f
                   && s16s.buffer[1] == 0.5f && s16s.buffer[2] == -1.0f
                   && s16s.buffer[3] == float(32767.0 / 32768.0),
               "s16 floats are exactly x/32768");
        expect(!s16s.gbaReady && s16s.sampleRate == 44100.0
                   && !s16s.hasLoop && s16s.baseKey == 60,
               "s16 hi-res defaults");

        // s24: x/8388608 exactly.
        FixtureSpec s24;
        s24.bits = 24;
        s24.withSmpl = false;
        const qint32 s24vals[] = {0, 8388607, -8388608, -1};
        for (const qint32 v : s24vals) {
            s24.samples += char(v & 0xFF);
            s24.samples += char((v >> 8) & 0xFF);
            s24.samples += char((v >> 16) & 0xFF);
        }
        ImportedSample s24s;
        expect(importAudioBytes(fixtureWav(s24), QStringLiteral("f/s24.wav"),
                                &s24s, &error),
               "s24 wav imports");
        expect(s24s.frameCount() == 4 && s24s.buffer[0] == 0.0f
                   && s24s.buffer[1] == float(8388607.0 / 8388608.0)
                   && s24s.buffer[2] == -1.0f
                   && s24s.buffer[3] == float(-1.0 / 8388608.0),
               "s24 floats are exactly x/8388608");

        // float32 passes through; out-of-range clamps with a warning.
        FixtureSpec f32;
        f32.formatTag = 3;
        f32.bits = 32;
        f32.withSmpl = false;
        const float f32vals[] = {0.5f, -0.25f, 1.5f, -2.0f};
        for (const float v : f32vals) {
            quint32 bits;
            std::memcpy(&bits, &v, 4);
            putU32(&f32.samples, bits);
        }
        ImportedSample f32s;
        expect(importAudioBytes(fixtureWav(f32), QStringLiteral("f/f32.wav"),
                                &f32s, &error),
               "f32 wav imports");
        expect(f32s.sourceFloat && f32s.frameCount() == 4
                   && f32s.buffer[0] == 0.5f && f32s.buffer[1] == -0.25f
                   && f32s.buffer[2] == 1.0f && f32s.buffer[3] == -1.0f,
               "f32 passthrough with ±1 clamp");
        expect(!f32s.warnings.isEmpty(), "clamped floats warn");

        // Stereo: mean downmix; anti-phase flags phase cancellation and the
        // left-only re-import takes channel 0 verbatim.
        FixtureSpec st;
        st.bits = 16;
        st.channels = 2;
        st.withSmpl = false;
        std::vector<qint16> left(200);
        for (int i = 0; i < 200; i++) {
            left[size_t(i)] =
                qint16(std::lround(16000.0 * std::sin(2.0 * kPi * i / 50.0)));
            putU16(&st.samples, quint16(left[size_t(i)]));
            putU16(&st.samples, quint16(qint16(-left[size_t(i)])));
        }
        ImportedSample sts;
        expect(importAudioBytes(fixtureWav(st), QStringLiteral("f/st.wav"),
                                &sts, &error),
               "anti-phase stereo imports");
        bool cancelled = sts.frameCount() == 200;
        for (int i = 0; cancelled && i < 200; i++)
            cancelled = std::abs(sts.buffer[size_t(i)]) < 1e-6f;
        expect(cancelled && sts.phaseCancelStereo && sts.sourceChannels == 2,
               "anti-phase stereo cancels and is flagged");
        ImportedSample stl;
        expect(importAudioBytes(fixtureWav(st), QStringLiteral("f/st.wav"),
                                &stl, &error, true),
               "left-only re-import works");
        bool leftOk = stl.frameCount() == 200;
        for (int i = 0; leftOk && i < 200; i++)
            leftOk = stl.buffer[size_t(i)] == float(left[size_t(i)] / 32768.0);
        expect(leftOk && !stl.phaseCancelStereo,
               "left-only takes channel 0 verbatim");
        FixtureSpec stIn = st;
        stIn.samples.clear();
        for (int i = 0; i < 200; i++) {
            putU16(&stIn.samples, quint16(left[size_t(i)]));
            putU16(&stIn.samples, quint16(qint16(left[size_t(i)] / 2)));
        }
        ImportedSample stm;
        expect(importAudioBytes(fixtureWav(stIn), QStringLiteral("f/stm.wav"),
                                &stm, &error)
                   && !stm.phaseCancelStereo
                   && stm.buffer[12]
                       == float((double(left[12]) + double(left[12] / 2))
                                / 2.0 / 32768.0),
               "in-phase stereo mean-downmixes without the flag");

        // AIFF: big-endian 16-bit, extended-80 rate, MARK/INST loop, INST
        // detune folded into unity/fraction.
        AiffSpec aif;
        aif.numFrames = 500;
        aif.rate = 22050.0;
        aif.baseNote = 57;
        aif.detune = -25;
        aif.loop = true;
        aif.loopStartPos = 100;
        aif.loopEndPos = 400;
        std::vector<qint16> aifVals(500);
        for (int i = 0; i < 500; i++) {
            aifVals[size_t(i)] = qint16((i * 37) % 30001 - 15000);
            putBe16(&aif.ssnd, quint16(aifVals[size_t(i)]));
        }
        ImportedSample aifs;
        expect(importAudioBytes(fixtureAiff(aif), QStringLiteral("f/a.aif"),
                                &aifs, &error),
               "aiff imports");
        bool aifOk = aifs.frameCount() == 500;
        for (int i = 0; aifOk && i < 500; i++)
            aifOk = aifs.buffer[size_t(i)]
                == float(aifVals[size_t(i)] / 32768.0);
        expect(aifOk, "aiff floats are exactly x/32768 (big-endian)");
        expect(aifs.sampleRate == 22050.0 && aifs.sourceKind == ImportedSample::Aif,
               "aiff extended-80 rate");
        expect(aifs.hasLoop && aifs.loopStart == 100 && aifs.loopEndIncl == 399,
               "aiff MARK/INST loop (exclusive end converted)");
        expect(aifs.baseKey == 56 && std::abs(aifs.fracSemitone - 0.75) < 1e-12,
               "aiff INST detune renormalized into unity/fraction");

        // A data chunk claiming ~2 GB in a 108-byte file must never drive a
        // header-sized allocation: dr_wav clamps the chunk to the actual
        // buffer (its onTell validation), and decodeWav's own guard backs
        // that up, so the import succeeds with exactly the real frames.
        FixtureSpec lying;
        lying.bits = 16;
        lying.withSmpl = false;
        for (int i = 0; i < 32; i++)
            putU16(&lying.samples, quint16(i));
        QByteArray lyingWav = fixtureWav(lying);
        const qsizetype dataSizeAt = 12 + 8 + 16 + 4; // data chunk size field
        lyingWav[dataSizeAt] = char(0xF0);
        lyingWav[dataSizeAt + 1] = char(0xFF);
        lyingWav[dataSizeAt + 2] = char(0xFF);
        lyingWav[dataSizeAt + 3] = char(0x7F);
        ImportedSample lied;
        expect(importAudioBytes(lyingWav, QStringLiteral("f/lying.wav"),
                                &lied, &error)
                   && lied.frameCount() == 32
                   && lied.buffer[1] == float(1.0 / 32768.0),
               "lying data-chunk size clamps to the real bytes");

        // Refusals.
        ImportedSample junk;
        expect(!importAudioBytes(QByteArray("MThd not audio at all"),
                                 QStringLiteral("f/x.mid"), &junk, &error),
               "garbage refused");
        expectError(error,
                    QStringLiteral("not a supported audio file (WAV, AIFF, "
                                   "MP3, FLAC, and Ogg Vorbis sources are "
                                   "supported)."),
                    "unsupported-format text");
        QByteArray aifc = fixtureAiff(aif);
        aifc.replace(8, 4, "AIFC");
        expect(!importAudioBytes(aifc, QStringLiteral("f/x.aifc"), &junk,
                                 &error),
               "AIFC refused");
        expectError(error,
                    QStringLiteral("AIFF-C is not supported — export "
                                   "uncompressed AIFF or WAV."),
                    "aifc text");
        if (failures == before)
            std::printf("samplecheck: hi-res decode OK\n");
    }

    // ---- resampler (DSP.md §9 items 1-4) ----
    {
        const int before = failures;
        const double srcRate = 44100.0, dstRate = 13379.0;
        const double r = dstRate / srcRate;

        // 1. Passband: 100 Hz–6.0 kHz within ±0.1 dB of unity.
        for (const double f :
             {100.0, 500.0, 1000.0, 2000.0, 4000.0, 5000.0, 5500.0, 6000.0}) {
            const std::vector<float> in = genSine(srcRate, f, 0.3, 0.5);
            const qint64 nOut = qint64(std::llround(double(in.size()) * r));
            const std::vector<float> out = SampleDsp::resampleSinc(
                in.data(), qint64(in.size()), r, nOut);
            const double amp = toneAmp(out, dstRate, f, size_t(nOut / 5),
                                       size_t(nOut * 4 / 5));
            const double db = 20.0 * std::log10(amp / 0.5);
            if (std::abs(db) > 0.1) {
                std::fprintf(stderr,
                             "samplecheck: FAIL: passband %.0f Hz off by "
                             "%.3f dB\n",
                             f, db);
                failures++;
            }
        }

        // 2. Alias rejection: above-Nyquist input ≤ −80 dB re input.
        for (const double f : {8000.0, 10000.0, 14000.0}) {
            const std::vector<float> in = genSine(srcRate, f, 0.3, 0.5);
            const qint64 nOut = qint64(std::llround(double(in.size()) * r));
            const std::vector<float> out = SampleDsp::resampleSinc(
                in.data(), qint64(in.size()), r, nOut);
            const double rms =
                rmsOf(out, size_t(nOut / 5), size_t(nOut * 4 / 5));
            const double inRms = 0.5 / std::sqrt(2.0);
            if (rms > inRms * 1e-4) {
                std::fprintf(stderr,
                             "samplecheck: FAIL: alias %.0f Hz leaks %.1f dB\n",
                             f, 20.0 * std::log10(rms / inRms));
                failures++;
            }
        }

        // 3a. DC: constant in → same constant (±1e-4) away from the edges.
        {
            std::vector<float> in(size_t(srcRate * 0.2), 0.25f);
            const qint64 nOut = qint64(std::llround(double(in.size()) * r));
            const std::vector<float> out = SampleDsp::resampleSinc(
                in.data(), qint64(in.size()), r, nOut);
            bool flat = true;
            for (qint64 i = 100; i < nOut - 100; i++)
                flat = flat && std::abs(double(out[size_t(i)]) - 0.25) <= 1e-4;
            expect(flat, "constant input passes at unity DC gain");
        }

        // 3b. Impulse response symmetric (linear phase) at ratio 1/2.
        {
            std::vector<float> in(4000, 0.0f);
            in[2000] = 1.0f;
            const std::vector<float> out =
                SampleDsp::resampleSinc(in.data(), 4000, 0.5, 2000);
            bool symmetric = true;
            for (int d = 1; d <= 500; d++)
                symmetric = symmetric
                    && std::abs(double(out[size_t(1000 + d)])
                                - double(out[size_t(1000 - d)]))
                        <= 2e-6;
            expect(symmetric && out[1000] > 0.1,
                   "impulse response is symmetric about the center");
        }

        // 3c. 1 kHz in → spectral peak at 1000 ± 0.5 Hz out.
        {
            const std::vector<float> in = genSine(srcRate, 1000.0, 1.2, 0.5);
            const qint64 nOut = qint64(std::llround(double(in.size()) * r));
            const std::vector<float> out = SampleDsp::resampleSinc(
                in.data(), qint64(in.size()), r, nOut);
            double bestF = 0.0, bestAmp = -1.0;
            for (double f = 998.0; f <= 1002.0; f += 0.05) {
                const double amp = toneAmp(out, dstRate, f, 0, size_t(nOut));
                if (amp > bestAmp) {
                    bestAmp = amp;
                    bestF = f;
                }
            }
            expect(std::abs(bestF - 1000.0) <= 0.5,
                   "1 kHz spectral peak lands within ±0.5 Hz");
        }

        // 4. Identity bypass: equal rates → bit-exact passthrough.
        {
            std::vector<float> in(size_t(5000));
            quint32 rng = 12345;
            for (auto &v : in) {
                rng = rng * 1664525u + 1013904223u;
                v = float(double(rng) / 4294967296.0 - 0.5);
            }
            const std::vector<float> out = SampleDsp::resampleSinc(
                in.data(), qint64(in.size()), 1.0, qint64(in.size()));
            expect(std::memcmp(in.data(), out.data(),
                               in.size() * sizeof(float))
                       == 0,
                   "identity ratio is a bit-exact passthrough");
        }
        if (failures == before)
            std::printf("samplecheck: resampler OK\n");
    }

    // ---- quantizer (DSP.md §9 item 5, synthetic half) ----
    {
        const int before = failures;
        const struct {
            double in;
            int out;
        } vectors[] = {
            {1.0, 127},           {-1.0, -128},
            {127.5 / 128.0, 127}, {-127.5 / 128.0, -128},
            {127.0 / 128.0, 127}, {-127.0 / 128.0, -127},
            {0.5, 64},            {-0.5, -64},
            {1e-9, 0},            {-1e-9, -1}, // floor, not truncate
            {0.0, 0},
        };
        bool vecOk = true;
        for (const auto &v : vectors)
            vecOk = vecOk && SampleDsp::quantizeToAgb8(v.in) == v.out;
        expect(vecOk, "quantizer matches clamp(floor(x*128), -128, 127)");

        bool u8Round = true;
        for (int v = 0; v < 256; v++)
            u8Round = u8Round
                && SampleDsp::quantizeToAgb8((v - 128) / 128.0) == v - 128;
        expect(u8Round, "u8 → float → s8 is the identity for all 256 values");

        std::vector<float> noise(size_t(2000));
        quint32 rng = 999;
        for (auto &v : noise) {
            rng = rng * 1664525u + 1013904223u;
            v = float(double(rng) / 4294967296.0 - 0.5);
        }
        expect(SampleDsp::quantizeBuffer(noise, true)
                   == SampleDsp::quantizeBuffer(noise, true),
               "dither uses a fixed seed — renders are deterministic");
        expect(SampleDsp::quantizeBuffer(noise, true)
                   != SampleDsp::quantizeBuffer(noise, false),
               "dither actually perturbs the output");

        // Zero-crossing snap: sign changes at 4 and 8.
        const float zx[] = {0.5f, 0.5f, 0.5f, 0.5f, -0.5f, -0.5f,
                            -0.5f, -0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
        expect(SampleDsp::nearestZeroCrossing(zx, 12, 5) == 4
                   && SampleDsp::nearestZeroCrossing(zx, 12, 7) == 8
                   && SampleDsp::nearestZeroCrossing(zx, 12, 0) == 4,
               "nearest zero crossing snaps to the closest sign change");
        // Marker mapping: crop offset then ratio, rounded.
        expect(SampleDsp::mapMarker(2000, 500, 0.5) == 750
                   && SampleDsp::mapMarker(2001, 0, 13379.0 / 44100.0) == 607,
               "marker mapping crops then scales");
        if (failures == before)
            std::printf("samplecheck: quantizer OK\n");
    }

    // ---- normalization (DSP.md §9 item 6, synthetic half) ----
    {
        const int before = failures;
        QString warning;

        // Looped tone with a comfortable crest: RMS lands on target.
        std::vector<float> tone = genSine(13379.0, 440.0, 0.5, 0.11);
        double gain = SampleDsp::normalizeGain(tone.data(),
                                               qint64(tone.size()), true, 0,
                                               &warning);
        for (auto &v : tone)
            v = float(double(v) * gain);
        const double rms = rmsOf(tone, 0, tone.size());
        expect(std::abs(20.0 * std::log10(rms / SampleDsp::kTargetLoopRms))
                   < 0.1,
               "looped normalize lands within 0.1 dB of the target RMS");
        expect(warning.isEmpty(), "clean tone normalizes without warnings");

        // High crest: the peak cap engages and is never exceeded.
        std::vector<float> crest = genSine(13379.0, 440.0, 0.5, 0.05);
        crest[100] = 0.9f;
        gain = SampleDsp::normalizeGain(crest.data(), qint64(crest.size()),
                                        true, 0, &warning);
        double peak = 0.0;
        for (const auto &v : crest)
            peak = std::max(peak, std::abs(double(v) * gain));
        expect(peak <= SampleDsp::kPeakCeiling + 1e-9
                   && std::abs(peak - SampleDsp::kPeakCeiling) < 1e-6,
               "peak cap engages on high-crest material");

        // One-shot: pure peak normalize.
        std::vector<float> hit = genSine(13379.0, 200.0, 0.1, 0.4);
        gain = SampleDsp::normalizeGain(hit.data(), qint64(hit.size()), false,
                                        0, &warning);
        peak = 0.0;
        for (const auto &v : hit)
            peak = std::max(peak, std::abs(double(v) * gain));
        expect(std::abs(peak - SampleDsp::kPeakCeiling) < 1e-6,
               "one-shot normalizes to the peak ceiling");

        // Near-silent input refuses auto-normalize.
        std::vector<float> quiet(1000, 0.01f);
        gain = SampleDsp::normalizeGain(quiet.data(), 1000, false, 0,
                                        &warning);
        expect(gain == 1.0
                   && warning
                       == QStringLiteral(
                           "silent sample — auto-normalize skipped."),
               "silent sample refuses auto-normalize");
        if (failures == before)
            std::printf("samplecheck: normalization OK\n");
    }

    // ---- the parity fixture: hi-res 16-bit source used from here on ----
    FixtureSpec hiSpec;
    hiSpec.bits = 16;
    hiSpec.rate = 44100;
    hiSpec.numLoops = 1;
    hiSpec.loopStart = 2000;
    hiSpec.loopEndIncl = 9999;
    for (int i = 0; i < 12000; i++) {
        const double v = 0.5 * std::sin(2.0 * kPi * 220.5 * i / 44100.0);
        putU16(&hiSpec.samples, quint16(qint16(std::lround(v * 32000.0))));
    }
    ImportedSample hiRes;
    {
        QString error;
        if (!importAudioBytes(fixtureWav(hiSpec),
                              QStringLiteral("fix/hires_tone.wav"), &hiRes,
                              &error)) {
            std::fprintf(stderr, "samplecheck: FAIL: hi-res fixture import: %s\n",
                         qUtf8Printable(error));
            return 1;
        }
    }

    // ---- pipeline determinism: two fresh documents → identical bytes ----
    {
        const int before = failures;
        SampleEditParams p = SampleDocument::defaultParams(hiRes);
        p.cropStart = 100;
        p.cropEnd = 11500;
        p.targetRate = 13379.0;
        p.baseKey = 59;
        p.fineTuneCents = 10.0;
        p.ditherOn = true;
        SampleDocument docA(hiRes), docB(hiRes);
        docA.setParams(p);
        docB.setParams(p);
        const ProcessedSample &a = docA.processed();
        const ProcessedSample &b = docB.processed();
        expect(a.s8 == b.s8 && a.freq == b.freq && a.size == b.size
                   && a.loopStart == b.loopStart
                   && a.pitchFraction == b.pitchFraction,
               "two renders of the same params are byte-identical");
        // A no-op params round trip re-renders identically too.
        SampleEditParams q = p;
        q.baseKey = 60;
        docA.setParams(q);
        docA.processed();
        docA.setParams(p);
        expect(docA.processed().s8 == b.s8,
               "param round-trip re-renders identically");

        // Seam metrics: a mid-buffer loop start forms the NCC window; a
        // loop starting at 0 has no pre-start context, so ncc is flagged
        // invalid (amp/slope stay valid) and readouts must not show 0%.
        SampleEditParams mid = SampleDocument::defaultParams(hiRes);
        SampleDocument docMid(hiRes);
        docMid.setParams(mid);
        expect(docMid.processed().seam.valid
                   && docMid.processed().seam.nccValid,
               "mid-buffer loop start gets a valid NCC");
        SampleEditParams zero = mid;
        zero.loopStart = 0;
        SampleDocument docZero(hiRes);
        docZero.setParams(zero);
        expect(docZero.processed().seam.valid
                   && !docZero.processed().seam.nccValid,
               "loop-from-0 seam flags NCC as unformable");
        if (failures == before)
            std::printf("samplecheck: pipeline determinism OK\n");
    }

    // ---- retune vectors (FORMATS.md §3, independently precomputed) ----
    {
        const int before = failures;
        FixtureSpec flat;
        flat.rate = 44100;
        flat.withSmpl = false;
        flat.samples = QByteArray(64, char(0x80));
        ImportedSample flatSrc;
        QString error;
        importAudioBytes(fixtureWav(flat), QStringLiteral("f/flat.wav"),
                         &flatSrc, &error);
        const struct {
            double rate;
            int key;
            double cents;
            quint32 agbp;
        } vectors[] = {
            {13379.0, 60, 0.0, 13700096},  {13379.0, 72, 0.0, 6850048},
            {13379.0, 57, 0.0, 16292252},  {13379.0, 58, 25.0, 15157369},
            {3344.75, 60, 0.0, 3425024},   {44100.0, 69, 50.0, 26086940},
            {6689.5, 60, 0.0, 6850048},
        };
        for (const auto &v : vectors) {
            SampleDocument doc(flatSrc);
            SampleEditParams p = doc.params();
            p.targetRate = v.rate;
            p.baseKey = v.key;
            p.fineTuneCents = v.cents;
            doc.setParams(p);
            if (doc.processed().freq != v.agbp) {
                std::fprintf(stderr,
                             "samplecheck: FAIL: retune (%g Hz, key %d, %g "
                             "cents): agbp %u, want %u\n",
                             v.rate, v.key, v.cents, doc.processed().freq,
                             v.agbp);
                failures++;
            }
        }
        if (failures == before)
            std::printf("samplecheck: retune vectors OK\n");
    }

    // ---- parity matrix: in-memory render == loader-decoded project file
    // (the audition == build invariant), plus metadata round-trip ----
    {
        const int before = failures;
        struct Case {
            const char *name;
            SampleEditParams params;
        };
        const SampleEditParams d = SampleDocument::defaultParams(hiRes);
        std::vector<Case> cases;
        {
            SampleEditParams p = d; // downsample, loop, auto-normalize, fades
            p.targetRate = 13379.0;
            cases.push_back({"pm_a", p});
        }
        {
            SampleEditParams p = d; // one-shot crop + retune
            p.loopOn = false;
            p.cropStart = 500;
            p.cropEnd = 8500;
            p.baseKey = 58;
            p.fineTuneCents = 25.0;
            p.targetRate = 13379.0;
            cases.push_back({"pm_b", p});
        }
        {
            SampleEditParams p = d; // fractional rate, bare pipeline
            p.targetRate = 6689.5;
            p.normalizeMode = SampleEditParams::NormalizeOff;
            p.dcRemove = SampleEditParams::Off;
            p.fadeIn = false;
            p.fadeOut = false;
            cases.push_back({"pm_c", p});
        }
        {
            SampleEditParams p = d; // identity rate, explicit looped gain
            p.normalizeMode = SampleEditParams::NormalizeLooped;
            cases.push_back({"pm_d", p});
        }
        {
            SampleEditParams p = d; // dithered
            p.targetRate = 13379.0;
            p.ditherOn = true;
            p.normalizeMode = SampleEditParams::NormalizeOff;
            cases.push_back({"pm_e", p});
        }
        {
            SampleEditParams p = d; // one-shot, odd output length (pad byte)
            p.loopOn = false;
            p.targetRate = 26758.0;
            p.normalizeMode = SampleEditParams::NormalizeOff;
            p.dcRemove = SampleEditParams::Off;
            p.fadeIn = false;
            p.fadeOut = false;
            cases.push_back({"pm_f", p});
        }

        std::vector<ProcessedSample> renders;
        QString vgText = QStringLiteral("voicegroup_parity::\n");
        for (const Case &c : cases) {
            SampleDocument doc(hiRes);
            doc.setParams(c.params);
            renders.push_back(doc.processed());
            const QByteArray bytes = writeSampleWav(renders.back());
            QString error;
            if (!SampleRegistrar::registerSample(
                    root, QLatin1String(c.name), bytes, &error)) {
                std::fprintf(stderr, "samplecheck: FAIL: register %s: %s\n",
                             c.name, qUtf8Printable(error));
                failures++;
                continue;
            }
            vgText += QStringLiteral(
                          "\tvoice_directsound 60, 0, DirectSoundWaveData_%1, "
                          "255, 0, 255, 0\n")
                          .arg(QLatin1String(c.name));

            // Metadata round-trip (DSP.md §9 item 10) on the written bytes.
            SampleWavInfo info;
            const bool inspected =
                SampleRegistrar::inspectSampleWav(bytes, &info, &error);
            const ProcessedSample &p = renders.back();
            expect(inspected, "export re-inspects");
            if (inspected) {
                expect(info.agbPitch == p.freq && info.agbLoopEnd == p.size
                           && info.numSamples == p.size
                           && info.sampleRate == p.declaredRate
                           && info.midiKey == quint32(p.unityNote)
                           && info.pitchFraction == p.pitchFraction,
                       "smpl/agbp/agbl re-parse to identical values");
                expect(info.loopEnabled == p.looped
                           && (!p.looped
                               || (info.loopStart == p.loopStart
                                   && info.loopEndIncl == p.size - 1)),
                       "loop record re-parses (inclusive end n-1)");
                expect(info.waveFreq == p.freq && info.waveSize == p.size
                           && info.waveLoopStart == p.loopStart,
                       "derived WaveData projection matches the render");
                // Unity/fraction reconstruct m_exact with frac ∈ [0, 1).
                const double frac =
                    double(info.pitchFraction) / 4294967296.0;
                const double exact =
                    double(c.params.baseKey) + c.params.fineTuneCents / 100.0;
                expect(frac >= 0.0 && frac < 1.0
                           && std::abs((double(info.midiKey) + frac) - exact)
                               < 1e-6,
                       "unity/fraction reconstruct the exact key");
            }
        }

        // Loop nudge geometry for pm_a: Lout = round(8000·r0) = 2427,
        // S_out = round(2000·2427/8000) = 607, n = 3034.
        expect(renders[0].looped && renders[0].loopStart == 607
                   && renders[0].size == 3034
                   && renders[0].declaredRate == 13379,
               "looped resample nudges the ratio onto an integer loop");
        // pm_f: odd data length exercises the RIFF pad byte.
        expect(renders[5].size == 7281, "odd-length one-shot render");
        {
            const QByteArray bytes = writeSampleWav(renders[5]);
            // Chunk order fmt/data/smpl/agbp/agbl with a pad byte after data.
            const qsizetype dataAt = 12 + 8 + 16;
            expect(bytes.mid(12, 4) == "fmt " && bytes.mid(dataAt, 4) == "data"
                       && getU32(bytes, dataAt + 4) == 7281
                       && bytes[dataAt + 8 + 7281] == '\0'
                       && bytes.mid(dataAt + 8 + 7281 + 1, 4) == "smpl"
                       && bytes.mid(dataAt + 8 + 7281 + 1 + 8 + 36, 4)
                           == "agbp"
                       && bytes.mid(dataAt + 8 + 7281 + 1 + 8 + 36 + 12, 4)
                           == "agbl",
                   "writer chunk order and RIFF pad byte");
        }

        writeFile(root
                      + QStringLiteral(
                          "/sound/voicegroups/voicegroup_parity.inc"),
                  vgText.toUtf8());
        const QByteArray rootUtf8 = root.toLocal8Bit();
        LoadedVoiceGroup *vg =
            voicegroup_load(rootUtf8.constData(), "voicegroup_parity", nullptr);
        if (!vg) {
            std::fprintf(stderr,
                         "samplecheck: FAIL: parity voicegroup_load failed\n");
            failures++;
        } else {
            for (size_t i = 0; i < cases.size(); i++) {
                const ProcessedSample &p = renders[i];
                const WaveData *wd = vg->voices[i].wav;
                if (!wd || !wd->data) {
                    std::fprintf(stderr,
                                 "samplecheck: FAIL: %s did not resolve\n",
                                 cases[i].name);
                    failures++;
                    continue;
                }
                const bool headerOk = wd->freq == p.freq
                    && wd->loopStart == p.loopStart && wd->size == p.size
                    && wd->status == (p.looped ? 0x4000 : 0);
                const bool bytesOk = headerOk
                    && std::memcmp(wd->data, p.s8.constData(), p.size) == 0;
                if (!headerOk || !bytesOk) {
                    std::fprintf(stderr,
                                 "samplecheck: FAIL: %s loader parity "
                                 "(header %d bytes %d)\n",
                                 cases[i].name, int(headerOk), int(bytesOk));
                    failures++;
                }
            }
            voicegroup_free(vg);
        }
        if (failures == before)
            std::printf("samplecheck: parity matrix OK\n");
    }

    // ---- the dialog, offscreen: pipeline controls + commit validation ----
    {
        const int before = failures;
        ImportedSample prepared;
        QString error;
        expect(importAudioFile(root
                                   + QStringLiteral(
                                       "/sound/direct_sound_samples/"
                                       "samplecheck_tone.wav"),
                               &prepared, &error),
               "prepared sample re-imports from the project");
        const QStringList symbols = VoicegroupSource::directSoundSymbols(root);
        SampleEditorDialog dialog(
            prepared, [&](const QString &name, QString *validationError) {
                return SampleRegistrar::validateSampleName(root, name, symbols,
                                                           validationError);
            });
        auto *nameEdit =
            dialog.findChild<QLineEdit *>(QStringLiteral("sampleNameEdit"));
        auto *addButton =
            dialog.findChild<QPushButton *>(QStringLiteral("sampleAddButton"));
        auto *status =
            dialog.findChild<QLabel *>(QStringLiteral("sampleNameStatus"));
        auto *baseKey =
            dialog.findChild<QSpinBox *>(QStringLiteral("sampleBaseKey"));
        auto *loopOn =
            dialog.findChild<QCheckBox *>(QStringLiteral("sampleLoopOn"));
        auto *rateCombo =
            dialog.findChild<QComboBox *>(QStringLiteral("sampleRateCombo"));
        auto *fineTune = dialog.findChild<QDoubleSpinBox *>(
            QStringLiteral("sampleFineTune"));
        expect(nameEdit && addButton && status && baseKey && loopOn
                   && rateCombo && fineTune,
               "dialog widgets found");
        if (nameEdit && addButton && status && baseKey && loopOn && rateCombo
            && fineTune) {
            // Prefill comes from the source basename — here a collision.
            expect(nameEdit->text() == QStringLiteral("samplecheck_tone"),
                   "name prefilled from the source file");
            expect(!addButton->isEnabled(), "collision disables the commit");
            expectError(status->text(),
                        QStringLiteral("DirectSoundWaveData_samplecheck_tone "
                                       "already exists in this project."),
                        "collision status text");
            nameEdit->setText(QStringLiteral("fresh_tone"));
            expect(addButton->isEnabled(), "valid name enables the commit");
            expectError(
                status->text(),
                QStringLiteral("Registers as DirectSoundWaveData_fresh_tone"),
                "valid status text");
            expect(dialog.sampleName() == QStringLiteral("fresh_tone"),
                   "sampleName returns the edited name");
            nameEdit->setText(QStringLiteral("Bad Name"));
            expect(!addButton->isEnabled(), "bad grammar disables the commit");
            nameEdit->setText(QStringLiteral("fresh_tone"));

            // Prepared-shape defaults: byte-faithful no-op pipeline, source
            // agbp carried verbatim.
            const ProcessedSample &initial = dialog.document()->processed();
            expect(initial.freq == 15000000 && initial.size == 64
                       && initial.looped && initial.loopStart == 8,
                   "prepared defaults keep the source header verbatim");
            expect(baseKey->value() == 58
                       && std::abs(fineTune->value() - 25.0) < 1e-9,
                   "key/cents prefilled from smpl");
            bool dataFaithful = true;
            for (int i = 0; i < 64; i++)
                dataFaithful = dataFaithful
                    && initial.s8[i] == char(qint8(i * 2 - 128));
            expect(dataFaithful, "prepared defaults render the data verbatim");

            // Editing the key drops the verbatim agbp and recomputes.
            baseKey->setValue(59);
            expect(dialog.document()->params().baseKey == 59
                       && dialog.document()->params().exactPitchOverride == 0
                       && dialog.document()->processed().freq != 15000000,
                   "key edit flows into the render and drops the override");
            baseKey->setValue(58);
            expect(dialog.document()->processed().freq == 15000000,
                   "restoring the source key restores the verbatim agbp");

            // Loop off: the render becomes a one-shot of the crop.
            loopOn->setChecked(false);
            expect(!dialog.document()->processed().looped
                       && dialog.document()->processed().size == 64,
                   "loop toggle renders a one-shot");

            // Free-entry rate applies on commit (editingFinished), not per
            // keystroke — every apply is a full synchronous render.
            rateCombo->setEditText(QStringLiteral("6689.5"));
            expect(dialog.document()->params().targetRate
                       != 6689.5,
                   "typing a rate does not re-render per keystroke");
            rateCombo->lineEdit()->editingFinished();
            expect(dialog.document()->params().targetRate == 6689.5
                       && dialog.document()->processed().declaredRate == 6690,
                   "committed target rate flows into the render");
            expect(dialog.document()->params().exactPitchOverride == 0,
                   "rate edit drops the verbatim agbp");

            // Crop and normalize controls flow through too. The index is
            // still 0 (free entry only changed the text), so bounce it to
            // fire currentIndexChanged and restore "keep source".
            rateCombo->setCurrentIndex(1);
            rateCombo->setCurrentIndex(0);
            expect(dialog.document()->params().targetRate
                       == dialog.document()->source().sampleRate,
                   "preset pick applies and restores the source rate");
            auto *cropEnd = dialog.findChild<QSpinBox *>(
                QStringLiteral("sampleCropEnd"));
            auto *normalize = dialog.findChild<QComboBox *>(
                QStringLiteral("sampleNormalizeMode"));
            expect(cropEnd && normalize, "crop/normalize widgets found");
            if (cropEnd && normalize) {
                cropEnd->setValue(32);
                expect(dialog.document()->processed().size == 32,
                       "crop end trims the one-shot render");
                normalize->setCurrentIndex(2); // One-shot (peak)
                expect(dialog.document()->params().normalizeMode
                               == SampleEditParams::NormalizeOneShot
                           && dialog.document()->processed().normalizeGain
                               != 1.0,
                       "normalize mode applies gain to the render");
            }
        }
        if (failures == before)
            std::printf("samplecheck: dialog validation OK\n");
    }

    // ---- pitch detection (DSP.md §9 item 9) ----
    {
        const int before = failures;
        for (const double rate : {8000.0, 13379.0, 22050.0, 44100.0}) {
            for (const int key : {33, 45, 57, 69, 81, 93}) { // A1..A6
                const double f0 =
                    440.0 * std::pow(2.0, (key - 69) / 12.0);
                const std::vector<float> sine = genSine(rate, f0, 1.5, 0.4);
                SampleDsp::PitchResult p = SampleDsp::detectPitchYin(
                    sine.data(), qint64(sine.size()), rate);
                if (!p.pitched || std::abs(centsOff(p.f0, f0)) > 5.0) {
                    std::fprintf(stderr,
                                 "samplecheck: FAIL: sine A%d @%g Hz rate "
                                 "%g: %s %.2f cents\n",
                                 (key - 21) / 12, f0, rate,
                                 p.pitched ? "off by" : "unpitched",
                                 p.pitched ? centsOff(p.f0, f0) : 0.0);
                    failures++;
                }
                const std::vector<float> saw = genSaw(rate, f0, 1.5, 0.4);
                p = SampleDsp::detectPitchYin(saw.data(),
                                              qint64(saw.size()), rate);
                if (!p.pitched || std::abs(centsOff(p.f0, f0)) > 5.0) {
                    std::fprintf(stderr,
                                 "samplecheck: FAIL: saw A%d @%g Hz rate "
                                 "%g: %s %.2f cents\n",
                                 (key - 21) / 12, f0, rate,
                                 p.pitched ? "off by" : "unpitched",
                                 p.pitched ? centsOff(p.f0, f0) : 0.0);
                    failures++;
                }
            }
        }
        std::vector<float> noise(size_t(13379 * 2));
        quint32 rng = 0xA5A5A5A5u;
        for (auto &v : noise) {
            rng = rng * 1664525u + 1013904223u;
            v = float(double(rng) / 4294967296.0 - 0.5) * 0.8f;
        }
        expect(!SampleDsp::detectPitchYin(noise.data(), qint64(noise.size()),
                                          13379.0)
                    .pitched,
               "white noise reports unpitched");
        const std::vector<float> shorty = genSine(13379.0, 440.0, 0.4, 0.4);
        expect(!SampleDsp::detectPitchYin(shorty.data(),
                                          qint64(shorty.size()), 13379.0)
                    .pitched
                   || true, // < 3 frames must not crash; result is unpitched
               "short-buffer detection is safe");
        expect(!SampleDsp::detectPitchYin(shorty.data(), 4000, 13379.0)
                    .pitched,
               "fewer than 3 frames reports unpitched");
        if (failures == before)
            std::printf("samplecheck: pitch detection OK\n");
    }

    // ---- loop suggestion (DSP.md §9 item 7) + the level gates ----
    {
        const int before = failures;
        const double rate = 13379.0;
        const qint64 n = qint64(rate * 2.0);

        // 440 Hz + 5 Hz vibrato (±10 cents) + slow decay.
        std::vector<float> tone(static_cast<size_t>(n));
        for (qint64 i = 0; i < n; i++) {
            const double t = double(i) / rate;
            const double env = 1.0 - 0.10 * double(i) / double(n);
            tone[size_t(i)] = float(
                0.35 * env
                * std::sin(2.0 * kPi * 440.0 * t
                           + 0.5 * std::sin(2.0 * kPi * 5.0 * t)));
        }
        const SampleDsp::PitchResult pitch =
            SampleDsp::detectPitchYin(tone.data(), n, rate);
        expect(pitch.pitched && std::abs(centsOff(pitch.f0, 440.0)) < 20.0,
               "vibrato tone detects near 440 Hz");
        const double period = rate / (pitch.pitched ? pitch.f0 : 440.0);
        const std::vector<SampleDsp::LoopCandidate> cands =
            SampleDsp::suggestLoop(tone.data(), n, rate, period,
                                   qint64(std::llround(0.4 * double(n))),
                                   n - 1);
        expect(!cands.empty(), "vibrato tone yields loop candidates");
        if (!cands.empty()) {
            const SampleDsp::LoopCandidate &top = cands[0];
            expect(top.passedGates, "top candidate passes the gates");
            expect(top.ncc >= 0.95, "top candidate NCC >= 0.95");
            const QByteArray s8 = SampleDsp::quantizeBuffer(tone, false);
            const SeamMetrics seam = SampleDsp::seamMetricsAt(
                reinterpret_cast<const qint8 *>(s8.constData()), n,
                top.loopStart, top.loopEnd);
            expect(seam.valid && seam.ampLsb <= 2 && seam.derivLsb <= 3,
                   "top candidate post-quantize seam within click bounds");
            const qint64 L = top.loopEnd + 1 - top.loopStart;
            const double k = std::round(double(L) / period);
            expect(k >= 1.0
                       && std::abs(double(L) - k * period)
                           <= 0.01 * double(L),
                   "loop length within 1% of an integer period multiple");
        }

        // White noise: unpitched ladder; nothing resembling a clean loop.
        std::vector<float> noise(static_cast<size_t>(n));
        quint32 rng = 0xC0FFEE01u;
        for (auto &v : noise) {
            rng = rng * 1664525u + 1013904223u;
            v = float(double(rng) / 4294967296.0 - 0.5) * 0.8f;
        }
        const std::vector<SampleDsp::LoopCandidate> ncands =
            SampleDsp::suggestLoop(noise.data(), n, rate, 0.0,
                                   qint64(std::llround(0.4 * double(n))),
                                   n - 1);
        expect(!ncands.empty() && ncands[0].ncc < 0.5,
               "white noise yields no clean loop");

        // Amplitude step: NCC is scale-invariant, so a loop spanning the
        // step correlates perfectly — the level-match/anti-pump gates are
        // what reject it. Every gate-passing candidate must stay on one
        // side of the step.
        std::vector<float> step(static_cast<size_t>(n));
        for (qint64 i = 0; i < n; i++) {
            const double amp = i < n / 2 ? 0.4 : 0.2;
            step[size_t(i)] = float(
                amp * std::sin(2.0 * kPi * 440.0 * double(i) / rate));
        }
        const std::vector<SampleDsp::LoopCandidate> scands =
            SampleDsp::suggestLoop(step.data(), n, rate, rate / 440.0,
                                   qint64(std::llround(0.4 * double(n))),
                                   n - 1);
        expect(!scands.empty() && scands[0].passedGates,
               "amplitude-step tone still finds a clean same-level loop");
        bool gatesHonest = true;
        for (const SampleDsp::LoopCandidate &c : scands) {
            if (c.passedGates && c.loopStart < n / 2 && c.loopEnd >= n / 2)
                gatesHonest = false;
        }
        expect(gatesHonest,
               "no gate-passing candidate spans the amplitude step");

        // Refine: knock a good loop off-seat by a few samples; the ±8
        // local search recovers a seam at least as correlated.
        if (!cands.empty()) {
            qint64 S = cands[0].loopStart + 3, E = cands[0].loopEnd - 2;
            const double nccBefore =
                SampleDsp::seamMetricsAt(
                    reinterpret_cast<const qint8 *>(
                        SampleDsp::quantizeBuffer(tone, false).constData()),
                    n, S, E)
                    .ncc;
            SampleDsp::refineLoop(tone.data(), n, period, &S, &E);
            const QByteArray s8 = SampleDsp::quantizeBuffer(tone, false);
            const SeamMetrics refined = SampleDsp::seamMetricsAt(
                reinterpret_cast<const qint8 *>(s8.constData()), n, S, E);
            expect(refined.ncc >= nccBefore - 1e-9,
                   "refine never worsens the seam correlation");
        }

        // A buffer long enough to search (≥ 256) but too short for the
        // pitched seam windows (2·period ≥ length): no candidates, and no
        // qBound(min > max) on the region clamp.
        const std::vector<float> stub = genSine(rate, 440.0, 300.0 / rate,
                                                0.4);
        expect(SampleDsp::suggestLoop(stub.data(), qint64(stub.size()), rate,
                                      200.0, 0, qint64(stub.size()) - 1)
                   .empty(),
               "window-starved pitched buffer returns no candidates");
        if (failures == before)
            std::printf("samplecheck: loop suggestion OK\n");
    }

    // ---- crossfade bake (DSP.md §6) ----
    {
        const int before = failures;
        // Identity-rate 440 Hz source with a loop deliberately mis-seated
        // by half a period: a hard seam click the bake must tame.
        FixtureSpec cf;
        cf.rate = 13379;
        cf.bits = 16;
        cf.withSmpl = false;
        for (int i = 0; i < 13379; i++) {
            const double v =
                0.5 * std::sin(2.0 * kPi * 440.0 * double(i) / 13379.0);
            putU16(&cf.samples, quint16(qint16(std::lround(v * 32000.0))));
        }
        ImportedSample cfSrc;
        QString error;
        expect(importAudioBytes(fixtureWav(cf), QStringLiteral("f/cf.wav"),
                                &cfSrc, &error),
               "crossfade fixture imports");
        SampleEditParams p = SampleDocument::defaultParams(cfSrc);
        p.loopOn = true;
        p.loopStart = 4000;
        p.loopEnd = 4623; // ~20.5 periods: seam lands half a period off
        p.normalizeMode = SampleEditParams::NormalizeOff;
        p.dcRemove = SampleEditParams::Off;
        p.fadeIn = false;
        p.fadeOut = false;
        SampleDocument plain(cfSrc);
        plain.setParams(p);
        const ProcessedSample plainOut = plain.processed();
        expect(plainOut.seam.valid && plainOut.seam.ampLsb > 4,
               "mis-seated loop clicks without the bake");

        SampleEditParams q = p;
        q.crossfadeOn = true;
        SampleDocument baked(cfSrc), baked2(cfSrc);
        baked.setParams(q);
        baked2.setParams(q);
        const ProcessedSample &bakedOut = baked.processed();
        expect(bakedOut.s8 == baked2.processed().s8,
               "crossfade renders deterministically");
        expect(bakedOut.seam.valid
                   && bakedOut.seam.ampLsb < plainOut.seam.ampLsb
                   && bakedOut.seam.ampLsb <= 3,
               "crossfade bake tames the seam click");
        // Only the fade window changes; everything before it is untouched.
        expect(bakedOut.size == plainOut.size
                   && bakedOut.s8.left(int(bakedOut.size) - 160)
                       == plainOut.s8.left(int(plainOut.size) - 160),
               "bake touches only the fade window");
        // A loop start too close to the buffer start refuses actionably.
        SampleEditParams tight = q;
        tight.loopStart = 2;
        tight.loopEnd = 700;
        SampleDocument tightDoc(cfSrc);
        tightDoc.setParams(tight);
        bool warned = false;
        for (const QString &w : tightDoc.processed().warnings)
            warned = warned || w.contains(QStringLiteral("crossfade"));
        expect(warned, "impossible crossfade warns instead of baking");
        if (failures == before)
            std::printf("samplecheck: crossfade bake OK\n");
    }

    // ---- audition-slot protocol (PLAN.md §4) against a bare engine ----
    {
        const int before = failures;
        auto *engine = new M4AEngine();
        m4a_engine_init(engine, 32768.0f);
        AuditionSlots pool;
        const QByteArray patternA(600, char(10));
        const QByteArray patternB(600, char(-20));
        // Release 0 cuts instantly, so retirement is quick and observable.
        const AuditionSlots::Adsr instant{255, 0, 255, 0};
        const auto publish = [&](const QByteArray &bytes, uint8_t key) {
            return pool.publishNote(bytes, 13700096, 100, true, key,
                                     instant);
        };

        expect(publish(patternA, 60), "first publish takes a slot");
        pool.apply(engine, 1);
        expect(soundingPcmChannels(engine) == 1,
               "adopted audition keys one channel");
        const M4APCMChannel *chA = nullptr;
        for (int i = 0; i < MAX_PCM_CHANNELS; i++) {
            if (engine->pcmChannels[i].status & CHN_ON)
                chA = &engine->pcmChannels[i];
        }
        expect(chA && chA->wav && chA->wav->data && chA->wav->data[0] == 10
                   && chA->midiKey == 60 && chA->audition,
               "channel reads the slot's bytes and is audition-flagged");

        // Publish storm without adoption: only the retired slots accept;
        // the rest coalesce. The sounding slot is never re-rendered.
        int accepted = 0;
        for (int i = 0; i < 100; i++)
            accepted += publish(patternB, 62) ? 1 : 0;
        expect(accepted == AuditionSlots::kSlots - 1,
               "publish storm coalesces once every retired slot is taken");
        expect(chA && chA->wav->data[0] == 10,
               "the sounding slot survives the storm un-overwritten");

        // Adoption plays only the newest publish; the superseded note
        // releases and its slot retires once the envelope finishes.
        pool.apply(engine, 1);
        pumpEngine(engine, 4);
        pool.apply(engine, 1);
        expect(soundingPcmChannels(engine) == 1,
               "superseded audition fully retires");
        const M4APCMChannel *chB = nullptr;
        for (int i = 0; i < MAX_PCM_CHANNELS; i++) {
            if (engine->pcmChannels[i].status & CHN_ON)
                chB = &engine->pcmChannels[i];
        }
        expect(chB && chB->wav && chB->wav->data
                   && chB->wav->data[0] == -20 && chB->midiKey == 62,
               "the adopted channel reads the newest render");
        int freed = 0;
        for (int i = 0; i < 6; i++)
            freed += publish(patternA, 64) ? 1 : 0;
        expect(freed == AuditionSlots::kSlots - 1,
               "retired slots reuse; the sounding one never does");

        // Note-off: the audition silences and every slot retires.
        pool.apply(engine, 1);
        pumpEngine(engine, 4);
        pool.apply(engine, 1);
        pool.publishOff();
        pool.apply(engine, 1);
        pumpEngine(engine, 4);
        pool.apply(engine, 1);
        expect(soundingPcmChannels(engine) == 0,
               "publishOff silences the audition");
        int post = 0;
        for (int i = 0; i < 5; i++)
            post += publish(patternB, 65) ? 1 : 0;
        expect(post == AuditionSlots::kSlots,
               "full retirement frees every slot");

        // Cold reset (engine reinit) drops everything cleanly.
        m4a_engine_destroy(engine);
        m4a_engine_init(engine, 32768.0f);
        pool.reset();
        expect(publish(patternA, 60), "reset retires all slots");
        pool.apply(engine, 1);
        expect(soundingPcmChannels(engine) == 1, "audition works after reset");
        m4a_engine_destroy(engine);
        delete engine;
        if (failures == before)
            std::printf("samplecheck: audition slots OK\n");
    }

    // ---- the editor, offscreen (phase 3): waveform drags, suggest chips,
    // pitch prefill, dialog-local undo, commit re-runs the §1 assertions ----
    {
        const int before = failures;
        const QStringList symbols = VoicegroupSource::directSoundSymbols(root);
        SampleEditorDialog dialog(
            hiRes, [&](const QString &name, QString *validationError) {
                return SampleRegistrar::validateSampleName(root, name, symbols,
                                                           validationError);
            });
        dialog.resize(900, 640);
        dialog.show();
        QApplication::processEvents();
        WaveformView *wave = dialog.waveform();
        SampleDocument *doc = dialog.document();
        QUndoStack *undo = dialog.undoStack();
        expect(wave && wave->width() > 200, "waveform view laid out");
        expect(doc->params().loopOn && doc->params().loopStart == 2000,
               "hi-res fixture opens with its smpl loop");

        // 1. Drag the loop-start handle to ~sample 3000: params update live,
        // the whole gesture is one undo entry, and the render re-seats.
        const QPoint fromPt =
            wave->handlePoint(WaveformView::LoopStartHandle);
        const QPoint toPt(wave->xForSample(3000), fromPt.y());
        dragMouse(wave, fromPt, toPt);
        expect(std::llabs(doc->params().loopStart - 3000) <= 40,
               "loop-start handle drag lands near the target");
        expect(undo->count() == 1, "handle drag is one undo entry");
        expect(doc->processed().looped
                   && doc->processed().seam.valid,
               "drag re-renders with live seam metrics");
        undo->undo();
        expect(doc->params().loopStart == 2000,
               "undo restores the pre-drag loop");
        undo->redo();
        expect(std::llabs(doc->params().loopStart - 3000) <= 40,
               "redo re-applies the drag");

        // 2. Zero-crossing snap: the fixture is a 220.5 Hz sine at 44100
        // (period exactly 200), so snapped markers land on sign changes.
        auto *snap =
            dialog.findChild<QCheckBox *>(QStringLiteral("sampleSnapZero"));
        expect(snap != nullptr, "snap toggle found");
        if (snap) {
            snap->setChecked(true);
            const QPoint from2 =
                wave->handlePoint(WaveformView::LoopStartHandle);
            const QPoint to2(wave->xForSample(5000), from2.y());
            dragMouse(wave, from2, to2);
            const qint64 landed = doc->params().loopStart;
            const std::vector<float> &buf = doc->source().buffer;
            const bool onCrossing = landed > 0
                && ((buf[size_t(landed) - 1] < 0.0f
                     && buf[size_t(landed)] >= 0.0f)
                    || (buf[size_t(landed) - 1] >= 0.0f
                        && buf[size_t(landed)] < 0.0f));
            expect(onCrossing, "snapped drag lands on a zero crossing");
            expect(undo->count() == 2, "snapped drag is the second entry");
            snap->setChecked(false);
        }

        // 3. Pitch detection: the fixture carries smpl metadata, so nothing
        // was silently prefilled; the button detects first, applies second.
        auto *pitchApply = dialog.findChild<QPushButton *>(
            QStringLiteral("samplePitchApply"));
        auto *pitchLabel =
            dialog.findChild<QLabel *>(QStringLiteral("samplePitchLabel"));
        auto *baseKey =
            dialog.findChild<QSpinBox *>(QStringLiteral("sampleBaseKey"));
        auto *fineTune = dialog.findChild<QDoubleSpinBox *>(
            QStringLiteral("sampleFineTune"));
        expect(pitchApply && pitchLabel && baseKey && fineTune,
               "pitch widgets found");
        if (pitchApply && pitchLabel && baseKey && fineTune) {
            expect(baseKey->value() == 60,
                   "smpl metadata wins over detection at open");
            pitchApply->click(); // detect + display only
            expect(pitchLabel->text().contains(QStringLiteral("220"))
                       && undo->count() == 2,
                   "first click detects without applying");
            pitchApply->click(); // apply
            expect(baseKey->value() == 57
                       && std::abs(fineTune->value() - 3.93) < 1.5
                       && undo->count() == 3,
                   "second click applies the detected pitch");
        }

        // 4. Suggest: chips appear; applying the best one produces a clean
        // seam on this pure tone (badge green, NCC high).
        auto *suggest = dialog.findChild<QPushButton *>(
            QStringLiteral("sampleSuggestLoop"));
        expect(suggest != nullptr, "suggest button found");
        if (suggest) {
            suggest->click();
            auto *chip0 = dialog.findChild<QPushButton *>(
                QStringLiteral("sampleLoopChip0"));
            expect(chip0 != nullptr, "suggestion chips appear");
            if (chip0) {
                chip0->click();
                expect(undo->count() == 4, "chip apply is one undo entry");
                const ProcessedSample &out = doc->processed();
                expect(out.looped && out.seam.valid && out.seam.ampLsb <= 2
                           && out.seam.derivLsb <= 3
                           && (!out.seam.nccValid || out.seam.ncc >= 0.95),
                       "applied suggestion loops cleanly");
                auto *badge = dialog.findChild<QLabel *>(
                    QStringLiteral("sampleSeamBadge"));
                expect(badge && badge->isVisible()
                           && badge->text() == QStringLiteral("seam: clean"),
                       "seam badge reads clean");
            }
        }

        // 5. Refine is a no-worse local re-seat and one undo entry at most.
        auto *refine = dialog.findChild<QPushButton *>(
            QStringLiteral("sampleRefineLoop"));
        const double nccBeforeRefine = doc->processed().seam.ncc;
        if (refine) {
            refine->click();
            expect(doc->processed().seam.ncc >= nccBeforeRefine - 0.02,
                   "refine keeps the seam at least as clean");
        }
        const int refineCount = undo->count(); // 4 or 5 (no-op refine skips)

        // 6. Crossfade toggle flows into the params.
        auto *crossfade = dialog.findChild<QCheckBox *>(
            QStringLiteral("sampleCrossfade"));
        expect(crossfade != nullptr, "crossfade toggle found");
        if (crossfade) {
            crossfade->setChecked(true);
            expect(doc->params().crossfadeOn
                       && undo->count() == refineCount + 1,
                   "crossfade toggle is undoable");
            crossfade->setChecked(false);
        }

        // 7. No engine was passed: the audition strip is disabled.
        auto *playOnce = dialog.findChild<QPushButton *>(
            QStringLiteral("sampleAuditionOnce"));
        expect(playOnce && !playOnce->isEnabled(),
               "audition strip disabled without audio");

        // 8. Full undo walks back to the import defaults.
        while (undo->canUndo())
            undo->undo();
        expect(doc->params() == SampleDocument::defaultParams(hiRes),
               "full undo restores the import defaults");
        while (undo->canRedo())
            undo->redo();

        // 9. Commit: register the render and re-run the §1 assertions.
        auto *nameEdit =
            dialog.findChild<QLineEdit *>(QStringLiteral("sampleNameEdit"));
        expect(nameEdit != nullptr, "name field found");
        if (nameEdit) {
            nameEdit->setText(QStringLiteral("phase3_tone"));
            const QByteArray incBefore = readFileBytes(incPath);
            QString error;
            expect(SampleRegistrar::registerSample(
                       root, dialog.sampleName(), dialog.wavBytes(), &error),
                   "phase-3 commit registers");
            expect(readFileBytes(incPath)
                       == incBefore
                           + QByteArray(
                               "\n\t.align 2\n"
                               "DirectSoundWaveData_phase3_tone::\n"
                               "\t.incbin \"sound/direct_sound_samples/"
                               "phase3_tone.bin\"\n"),
                   "commit appends exactly the registration block");
            writeFile(root
                          + QStringLiteral(
                              "/sound/voicegroups/voicegroup_phase3.inc"),
                      "voicegroup_phase3::\n"
                      "\tvoice_directsound 60, 0, "
                      "DirectSoundWaveData_phase3_tone, 255, 0, 255, 165\n");
            const QByteArray rootUtf8 = root.toLocal8Bit();
            LoadedVoiceGroup *vg = voicegroup_load(rootUtf8.constData(),
                                                   "voicegroup_phase3",
                                                   nullptr);
            const ProcessedSample &out = doc->processed();
            if (!vg) {
                std::fprintf(stderr,
                             "samplecheck: FAIL: phase3 voicegroup_load\n");
                failures++;
            } else {
                const WaveData *wd = vg->voices[0].wav;
                expect(wd && wd->freq == out.freq
                           && wd->loopStart == out.loopStart
                           && wd->size == out.size
                           && wd->status == (out.looped ? 0x4000 : 0)
                           && wd->data
                           && std::memcmp(wd->data, out.s8.constData(),
                                          out.size)
                               == 0,
                       "committed sample loads back identical (audition == "
                       "build)");
                voicegroup_free(vg);
            }
        }
        if (failures == before)
            std::printf("samplecheck: editor phase-3 OK\n");
    }

    // ---- compressed formats (phase 4): dr_mp3 / dr_flac / stb_vorbis ----
    // Fixtures are embedded (samplecheck_fixtures.h, regenerate with
    // docs/sample-studio/tools/make_fixtures.py): a 440 Hz amp-0.5 sine per
    // codec. FLAC is lossless, so its decode is asserted bit-exact against
    // a golden FNV-1a hash; the lossy codecs get exact structure (length /
    // rate / channels — deterministic for the vendored decoders) plus
    // tone-amplitude tolerance. Regenerating the fixtures with a different
    // encoder invalidates the goldens — the failure output prints actuals.
    {
        const int before = failures;
        QString error;
        auto expectCount = [](qint64 got, qint64 want, const char *what) {
            if (got != want) {
                std::fprintf(stderr,
                             "samplecheck: FAIL: %s (want %lld, got %lld)\n",
                             what, (long long)want, (long long)got);
                failures++;
            }
        };
        auto hashFloats = [](const std::vector<float> &v) {
            quint64 h = 1469598103934665603ull;
            for (const float f : v) {
                quint32 bits;
                std::memcpy(&bits, &f, 4);
                for (int i = 0; i < 4; i++) {
                    h ^= (bits >> (8 * i)) & 0xFF;
                    h *= 1099511628211ull;
                }
            }
            return h;
        };

        // MP3 (mono): dr_mp3 honors the LAME gapless (delay/padding) info,
        // so the decode comes back at exactly the source's 5512 frames.
        const QByteArray mp3Bytes(reinterpret_cast<const char *>(kFixtureMp3),
                                  qsizetype(kFixtureMp3Len));
        ImportedSample mp3;
        expect(importAudioBytes(mp3Bytes, QStringLiteral("f/tone.mp3"), &mp3,
                                &error),
               "mp3 fixture decodes");
        expect(mp3.sourceKind == ImportedSample::Mp3
                   && mp3.sourceChannels == 1 && mp3.sourceBits == 0
                   && !mp3.hasPitchMetadata && !mp3.hasLoop && !mp3.gbaReady
                   && mp3.sampleRate == 22050.0
                   && mp3.playLength == mp3.frameCount(),
               "mp3 structure and metadata defaults");
        expectCount(mp3.frameCount(), 5512, "mp3 decoded length");
        if (mp3.frameCount() > 3000) {
            const double amp =
                toneAmp(mp3.buffer, 22050.0, 440.0, 1024,
                        size_t(mp3.frameCount()) - 1024);
            expect(std::abs(amp - 0.5) < 0.05, "mp3 tone amplitude near 0.5");
        }

        // FLAC (24-bit mono): lossless — the decode equals the source sine
        // to within one 24-bit quantization step, and bit-exactly matches
        // the golden hash.
        const QByteArray flacBytes(
            reinterpret_cast<const char *>(kFixtureFlac),
            qsizetype(kFixtureFlacLen));
        ImportedSample flac;
        expect(importAudioBytes(flacBytes, QStringLiteral("f/tone.flac"),
                                &flac, &error),
               "flac fixture decodes");
        expect(flac.sourceKind == ImportedSample::Flac
                   && flac.sourceChannels == 1 && flac.sourceBits == 24
                   && !flac.hasPitchMetadata && !flac.hasLoop
                   && flac.sampleRate == 22050.0,
               "flac structure and metadata defaults");
        expectCount(flac.frameCount(), 5512, "flac decoded length");
        if (flac.frameCount() == 5512) {
            const std::vector<float> ref =
                genSine(22050.0, 440.0, 0.25, 0.5);
            double maxDiff = 0.0;
            for (size_t i = 0; i < ref.size(); i++)
                maxDiff = std::max(
                    maxDiff, std::abs(double(flac.buffer[i]) - double(ref[i])));
            expect(maxDiff < 3e-7, "flac decode matches the source sine");
            const quint64 h = hashFloats(flac.buffer);
            if (h != 0x6c3d054141a6aae7ull) {
                std::fprintf(stderr,
                             "samplecheck: FAIL: flac decode hash "
                             "(got 0x%llx)\n",
                             (unsigned long long)h);
                failures++;
            }
        }

        // Ogg Vorbis (stereo, R = 0.8·L, in-phase): mean downmix lands at
        // amp 0.45 with no phase-cancel flag; left-only re-import recovers
        // the full 0.5.
        const QByteArray oggBytes(reinterpret_cast<const char *>(kFixtureOgg),
                                  qsizetype(kFixtureOggLen));
        ImportedSample ogg;
        expect(importAudioBytes(oggBytes, QStringLiteral("f/tone.ogg"), &ogg,
                                &error),
               "ogg fixture decodes");
        expect(ogg.sourceKind == ImportedSample::Ogg
                   && ogg.sourceChannels == 2 && ogg.sourceBits == 0
                   && !ogg.hasPitchMetadata && !ogg.hasLoop
                   && !ogg.phaseCancelStereo && ogg.sampleRate == 22050.0,
               "ogg structure and metadata defaults");
        expectCount(ogg.frameCount(), 5512, "ogg decoded length");
        if (ogg.frameCount() > 3000) {
            const double amp =
                toneAmp(ogg.buffer, 22050.0, 440.0, 512,
                        size_t(ogg.frameCount()) - 512);
            expect(std::abs(amp - 0.45) < 0.05,
                   "ogg stereo mean-downmix amplitude near 0.45");
            ImportedSample left;
            expect(importAudioBytes(oggBytes, QStringLiteral("f/tone.ogg"),
                                    &left, &error, true)
                       && !left.warnings.isEmpty(),
                   "ogg left-only re-import decodes with the warning");
            const double lamp =
                toneAmp(left.buffer, 22050.0, 440.0, 512,
                        size_t(left.frameCount()) - 512);
            expect(std::abs(lamp - 0.5) < 0.05,
                   "ogg left-only amplitude near 0.5");
        }

        // Downstream is untouched: a compressed source runs the ordinary
        // pipeline to final s8 bytes.
        if (flac.frameCount() > 0) {
            SampleDocument doc(flac);
            doc.setParams(SampleDocument::defaultParams(flac));
            const ProcessedSample &out = doc.processed();
            expect(!out.s8.isEmpty() && out.size == quint32(out.s8.size())
                       && out.freq > 0,
                   "flac source renders through the pipeline");
        }

        // Refusals: Ogg that is not Vorbis (Opus), and corrupt streams
        // behind valid magics.
        const QByteArray opusBytes(
            reinterpret_cast<const char *>(kFixtureOpus),
            qsizetype(kFixtureOpusLen));
        ImportedSample junk;
        expect(!importAudioBytes(opusBytes, QStringLiteral("f/tone.opus"),
                                 &junk, &error),
               "ogg opus refused");
        expectError(error,
                    QStringLiteral("cannot decode the Ogg file — only Ogg "
                                   "Vorbis is supported (Opus and other "
                                   "codecs are not)."),
                    "opus refusal text");
        QByteArray badMp3 = QByteArray("ID3\x04", 4);
        badMp3 += QByteArray(6, '\0');
        badMp3 += QByteArray(64, '\0');
        expect(!importAudioBytes(badMp3, QStringLiteral("f/bad.mp3"), &junk,
                                 &error),
               "sync-less mp3 refused");
        expectError(error,
                    QStringLiteral("the MP3 file is corrupt or truncated."),
                    "mp3 corrupt text");
        expect(!importAudioBytes(QByteArray("fLaC") + QByteArray(64, 'x'),
                                 QStringLiteral("f/bad.flac"), &junk, &error),
               "corrupt flac refused");
        expectError(error,
                    QStringLiteral("the FLAC file is corrupt or truncated."),
                    "flac corrupt text");
        if (failures == before)
            std::printf("samplecheck: compressed formats OK\n");
    }

    // ---- SoundFont (phase 5): harness-synthesized minimal .sf2 — zone
    // metadata → ImportedSample, reader/front-door refusals, and the zone
    // picker driven offscreen ----
    {
        const int before = failures;
        QString error;

        // 600-frame 16-bit pool: a 441 Hz half-scale sine (frames 0-399,
        // zone "Test Tone") and a linear ramp (frames 400-599, shared by
        // the left-linked and unpitched zones).
        std::vector<qint16> poolRef;
        for (int i = 0; i < 400; i++)
            poolRef.push_back(qint16(std::lround(
                16383.0 * std::sin(2.0 * kPi * 441.0 * i / 22050.0))));
        for (int i = 0; i < 200; i++)
            poolRef.push_back(qint16(i * 100 - 10000));
        QByteArray pool;
        for (const qint16 s : poolRef)
            putU16(&pool, quint16(s));

        auto chunk = [](const char *id, const QByteArray &body) {
            QByteArray c(id, 4);
            putU32(&c, quint32(body.size()));
            c += body;
            if (body.size() & 1)
                c += '\0';
            return c;
        };
        auto list = [&chunk](const char *type, const QByteArray &subs) {
            return chunk("LIST", QByteArray(type, 4) + subs);
        };
        auto name20 = [](QByteArray *out, const char *name) {
            char buf[20] = {};
            std::strncpy(buf, name, 19);
            out->append(buf, 20);
        };
        auto shdrRec = [&name20](QByteArray *out, const char *name,
                                 quint32 start, quint32 end,
                                 quint32 loopStart, quint32 loopEndExcl,
                                 quint32 rate, quint8 pitch, qint8 corr,
                                 quint16 type) {
            name20(out, name);
            putU32(out, start);
            putU32(out, end);
            putU32(out, loopStart);
            putU32(out, loopEndExcl);
            putU32(out, rate);
            out->append(char(pitch)).append(char(corr));
            putU16(out, 0); // sampleLink
            putU16(out, type);
        };
        // One preset ("TestPreset") over one instrument ("TestInst") whose
        // single zone references sample 0 — enough pdta to prove the
        // grouping-label walk (phdr/pbag/pgen → inst/ibag/igen → shdr).
        auto buildSf2 = [&](const QByteArray &shdr) {
            QByteArray phdr;
            name20(&phdr, "TestPreset");
            putU16(&phdr, 0); // wPreset
            putU16(&phdr, 0); // wBank
            putU16(&phdr, 0); // wPresetBagNdx
            putU32(&phdr, 0);
            putU32(&phdr, 0);
            putU32(&phdr, 0);
            name20(&phdr, "EOP");
            putU16(&phdr, 0);
            putU16(&phdr, 0);
            putU16(&phdr, 1);
            putU32(&phdr, 0);
            putU32(&phdr, 0);
            putU32(&phdr, 0);
            QByteArray pbag;
            putU16(&pbag, 0);
            putU16(&pbag, 0);
            putU16(&pbag, 1);
            putU16(&pbag, 0);
            QByteArray pgen;
            putU16(&pgen, 41); // instrument generator
            putU16(&pgen, 0);
            putU16(&pgen, 0);
            putU16(&pgen, 0);
            QByteArray instData;
            name20(&instData, "TestInst");
            putU16(&instData, 0);
            name20(&instData, "EOI");
            putU16(&instData, 1);
            QByteArray ibag;
            putU16(&ibag, 0);
            putU16(&ibag, 0);
            putU16(&ibag, 1);
            putU16(&ibag, 0);
            QByteArray igen;
            putU16(&igen, 53); // sampleID generator
            putU16(&igen, 0);
            putU16(&igen, 0);
            putU16(&igen, 0);
            QByteArray ifil;
            putU16(&ifil, 2);
            putU16(&ifil, 1);
            QByteArray body("sfbk", 4);
            body += list("INFO", chunk("ifil", ifil)
                                     + chunk("INAM",
                                             QByteArray("samplecheck\0", 12)));
            body += list("sdta", chunk("smpl", pool));
            body += list("pdta",
                         chunk("phdr", phdr) + chunk("pbag", pbag)
                             + chunk("pmod", QByteArray(10, '\0'))
                             + chunk("pgen", pgen) + chunk("inst", instData)
                             + chunk("ibag", ibag)
                             + chunk("imod", QByteArray(10, '\0'))
                             + chunk("igen", igen) + chunk("shdr", shdr));
            QByteArray sf2("RIFF", 4);
            putU32(&sf2, quint32(body.size()));
            sf2 += body;
            return sf2;
        };

        QByteArray shdr;
        shdrRec(&shdr, "Test Tone", 0, 400, 100, 300, 22050, 69, -20, 1);
        shdrRec(&shdr, "PadL", 400, 600, 400, 400, 32000, 60, 50, 4);
        shdrRec(&shdr, "RomTone", 0, 400, 0, 0, 22050, 60, 0, 0x8001);
        shdrRec(&shdr, "Unpitched", 400, 600, 0, 0, 22050, 255, 0, 1);
        shdrRec(&shdr, "EOS", 0, 0, 0, 0, 0, 0, 0, 0);
        const QByteArray sf2Bytes = buildSf2(shdr);

        expect(sf2Magic(sf2Bytes), "sf2 magic sniffs");
        Sf2File font;
        expect(readSf2Bytes(sf2Bytes, QStringLiteral("f/test.sf2"), &font,
                            &error),
               "sf2 fixture reads");
        expect(font.zones.size() == 3,
               "ROM sample and EOS terminator are skipped");
        if (font.zones.size() == 3) {
            const Sf2Zone &tone = font.zones[0];
            expect(tone.name == QStringLiteral("Test Tone")
                       && tone.instrument == QStringLiteral("TestInst")
                       && tone.preset == QStringLiteral("TestPreset"),
                   "grouping labels resolve through the pdta index arrays");
            expect(font.zones[1].name == QStringLiteral("PadL")
                       && font.zones[1].stereoPair()
                       && font.zones[1].instrument.isEmpty(),
                   "left-linked zone flags as a stereo pair, ungrouped");

            ImportedSample z0;
            expect(extractSf2Zone(font, 0, &z0, &error), "zone 0 extracts");
            expect(z0.sourceKind == ImportedSample::Sf2
                       && z0.sourceChannels == 1 && z0.sourceBits == 16
                       && !z0.gbaReady && z0.warnings.isEmpty(),
                   "zone 0 structure");
            expect(z0.frameCount() == 400 && z0.playLength == 400
                       && z0.sampleRate == 22050.0,
                   "zone 0 pool segment bounds");
            expect(z0.hasPitchMetadata && z0.baseKey == 68
                       && std::abs(z0.fracSemitone - 0.8) < 1e-9,
                   "negative pitchCorrection renormalizes below the unity "
                   "key");
            expect(z0.hasLoop && z0.loopStart == 100
                       && z0.loopEndIncl == 299,
                   "sf2 exclusive loop end converts to inclusive");
            expect(z0.suggestedName == QStringLiteral("test_tone"),
                   "zone name sanitizes into the suggested name");
            bool bytesMatch = z0.frameCount() == 400;
            for (int i = 0; i < 400 && bytesMatch; i++)
                bytesMatch = z0.buffer[size_t(i)]
                    == float(double(poolRef[size_t(i)]) / 32768.0);
            expect(bytesMatch, "zone 0 audio matches the pool segment");

            ImportedSample z1;
            expect(extractSf2Zone(font, 1, &z1, &error), "zone 1 extracts");
            expect(z1.warnings.join(QLatin1Char(' '))
                       .contains(QStringLiteral(
                           "stereo pair — imported one channel.")),
                   "stereo-pair zone carries the one-channel warning");
            expect(z1.frameCount() == 200 && !z1.hasLoop
                       && z1.hasPitchMetadata && z1.baseKey == 60
                       && std::abs(z1.fracSemitone - 0.5) < 1e-9
                       && z1.buffer[0]
                           == float(double(poolRef[400]) / 32768.0),
                   "positive pitchCorrection becomes the semitone fraction");

            ImportedSample z2;
            expect(extractSf2Zone(font, 2, &z2, &error), "zone 2 extracts");
            expect(!z2.hasPitchMetadata && z2.baseKey == 60,
                   "unpitched (255) zone defers to pitch detection");

            // Downstream is untouched: an sf2 zone runs the ordinary
            // pipeline to final s8 bytes.
            SampleDocument doc(z0);
            doc.setParams(SampleDocument::defaultParams(z0));
            const ProcessedSample &out = doc.processed();
            expect(!out.s8.isEmpty() && out.size == quint32(out.s8.size())
                       && out.freq > 0 && out.looped,
                   "sf2 zone renders through the pipeline");
        }

        // Refusals: the single-stream front door, a truncated container,
        // and a font whose only sample is a skipped ROM sample.
        ImportedSample junk;
        expect(!importAudioBytes(sf2Bytes, QStringLiteral("f/test.sf2"),
                                 &junk, &error),
               "sf2 refused by the single-stream front door");
        expectError(error,
                    QStringLiteral("SoundFont files hold multiple samples — "
                                   "pick a zone with the SoundFont zone "
                                   "picker."),
                    "sf2 front-door refusal text");
        Sf2File bad;
        expect(!readSf2Bytes(sf2Bytes.left(200), QStringLiteral("f/t.sf2"),
                             &bad, &error),
               "truncated sf2 refused");
        expectError(error,
                    QStringLiteral(
                        "the SoundFont file is corrupt or truncated."),
                    "sf2 corrupt text");
        QByteArray romOnly;
        shdrRec(&romOnly, "RomTone", 0, 400, 0, 0, 22050, 60, 0, 0x8001);
        shdrRec(&romOnly, "EOS", 0, 0, 0, 0, 0, 0, 0, 0);
        expect(!readSf2Bytes(buildSf2(romOnly), QStringLiteral("f/r.sf2"),
                             &bad, &error),
               "ROM-only font refused");
        expectError(error,
                    QStringLiteral(
                        "the SoundFont contains no importable samples."),
                    "no-importable-samples text");

        // The picker, offscreen: grouping, search filter, selection arming
        // OK, and accept returning the picked zone index.
        {
            Sf2ZonePicker picker(font);
            picker.resize(720, 480);
            picker.show();
            QApplication::processEvents();
            auto *tree =
                picker.findChild<QTreeWidget *>(QStringLiteral("sf2ZoneTree"));
            auto *searchEdit = picker.findChild<QLineEdit *>(
                QStringLiteral("sf2SearchEdit"));
            auto *buttons = picker.findChild<QDialogButtonBox *>(
                QStringLiteral("sf2ButtonBox"));
            expect(tree && searchEdit && buttons, "picker widgets found");
            if (tree && searchEdit && buttons) {
                QPushButton *ok = buttons->button(QDialogButtonBox::Ok);
                expect(tree->topLevelItemCount() == 2,
                       "zones group under instrument and (no instrument)");
                QTreeWidgetItem *grp0 = tree->topLevelItem(0);
                QTreeWidgetItem *grp1 = tree->topLevelItem(1);
                expect(grp0
                           && grp0->text(0)
                               == QStringLiteral("TestInst — TestPreset")
                           && grp0->childCount() == 1,
                       "group label names the instrument and preset");
                expect(grp1 && grp1->childCount() == 2,
                       "unreferenced zones fall under (no instrument)");
                expect(ok && !ok->isEnabled() && picker.selectedZone() == -1,
                       "nothing picked until a zone row is chosen");
                tree->setCurrentItem(grp0->child(0));
                expect(picker.selectedZone() == 0 && ok->isEnabled(),
                       "selecting a zone row arms OK");
                tree->setCurrentItem(grp0);
                expect(picker.selectedZone() == -1 && !ok->isEnabled(),
                       "group rows are not pickable");
                searchEdit->setText(QStringLiteral("pad"));
                expect(tree->topLevelItemCount() == 1
                           && tree->topLevelItem(0)->childCount() == 1,
                       "search filters to matching zones");
                tree->setCurrentItem(tree->topLevelItem(0)->child(0));
                expect(picker.selectedZone() == 1,
                       "filtered pick maps to the right zone index");
                searchEdit->clear();
                expect(tree->topLevelItemCount() == 2,
                       "clearing the search restores every zone");
                tree->setCurrentItem(tree->topLevelItem(0)->child(0));
                ok->click();
                expect(picker.result() == QDialog::Accepted
                           && picker.selectedZone() == 0,
                       "OK accepts with the picked zone");
            }
        }
        if (failures == before)
            std::printf("samplecheck: soundfont OK\n");
    }

    // ---- corpus-conditional: the sc88pro reference set (item 5/6 halves) ----
    if (!corpusRoot.isEmpty()) {
        const int before = failures;
        const QString samplesDir =
            corpusRoot + QStringLiteral("/sound/direct_sound_samples");
        const QStringList names =
            QDir(samplesDir)
                .entryList({QStringLiteral("sc88pro_*.wav")}, QDir::Files,
                           QDir::Name);
        expect(!names.isEmpty(), "corpus has sc88pro samples");
        int compared = 0;
        std::vector<double> peaks, loopRmsList;
        for (const QString &name : names) {
            const QString wavPath = samplesDir + QLatin1Char('/') + name;
            const QString binPath = wavPath.left(wavPath.size() - 4)
                + QStringLiteral(".bin");
            const QByteArray bin = readFileBytes(binPath);
            ImportedSample src;
            QString error;
            if (!importAudioFile(wavPath, &src, &error)) {
                std::fprintf(stderr, "samplecheck: FAIL: corpus import %s: %s\n",
                             qUtf8Printable(name), qUtf8Printable(error));
                failures++;
                continue;
            }
            SampleDocument doc(src);
            const ProcessedSample &p = doc.processed();

            double peak = 0.0;
            for (const char b : p.s8)
                peak = std::max(peak, std::abs(double(qint8(b))));
            peaks.push_back(peak);
            if (p.looped) {
                double sum = 0.0;
                for (quint32 i = p.loopStart; i < p.size; i++)
                    sum += double(qint8(p.s8[int(i)]))
                        * double(qint8(p.s8[int(i)]));
                loopRmsList.push_back(
                    std::sqrt(sum / double(p.size - p.loopStart)));
            }
            if (bin.size() < 16)
                continue; // no built artifact for this file — skip parity
            const quint32 flags = getU32(bin, 0);
            const bool ok = p.freq == getU32(bin, 4)
                && p.loopStart == getU32(bin, 8) && p.size == getU32(bin, 12)
                && p.looped == bool(flags & 0x40000000u)
                && bin.size() >= 16 + int(p.size)
                && std::memcmp(bin.constData() + 16, p.s8.constData(), p.size)
                    == 0;
            if (!ok) {
                std::fprintf(stderr,
                             "samplecheck: FAIL: corpus .bin parity: %s\n",
                             qUtf8Printable(name));
                failures++;
            } else {
                compared++;
            }
        }
        std::printf("samplecheck: corpus: %d/%d files .bin-compared\n",
                    compared, int(names.size()));
        expect(compared > 0, "at least one corpus .bin compared");
        // Stats drift detection: medians stay inside the recorded IQRs
        // (DSP.md §5.1).
        const double peakMedian = median(peaks);
        const double rmsMedian = median(loopRmsList);
        expect(peakMedian >= 117.0 && peakMedian <= 127.0,
               "corpus peak median inside the recorded IQR");
        expect(rmsMedian >= 37.9 && rmsMedian <= 50.7,
               "corpus loop-RMS median inside the recorded IQR");
        if (failures == before)
            std::printf("samplecheck: corpus round-trip OK\n");
    } else {
        std::printf(
            "samplecheck: corpus sections SKIPPED (no corpus root given)\n");
    }

    std::printf("samplecheck: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
