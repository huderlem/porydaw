#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QString>
#include <cstdio>

#include "project/samplereg.h"
#include "project/voicegroupsource.h"
#include "ui/sampleeditordialog.h"

extern "C" {
#include "voicegroup_loader.h"
}

// --samplecheck <scratchDir>: prepared-sample import check (Sample Studio
// phase 1). Builds fully-fresh fake decomp projects under scratchDir — a
// wav2agb one, a legacy-aif one, a rule-less one, an .inc-less one, and a
// CRLF-.inc one — then drives probe → inspect → register → loader-resolve
// end to end and exact-matches every refusal message. scratchDir must not
// already exist (fully fresh scratches, no stale artifacts).

namespace {

int failures = 0;

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

struct FixtureSpec {
    quint32 rate = 13379;
    QByteArray samples; // unsigned 8-bit
    quint16 channels = 1;
    quint32 unityKey = 60;
    quint32 pitchFraction = 0;
    int numLoops = 0; // smpl records written (and declared)
    quint32 loopType = 0;
    quint32 loopStart = 0;
    quint32 loopEndIncl = 0;
    quint32 agbp = 0; // 0 = omit chunk
    quint32 agbl = 0; // 0 = omit chunk
};

// A GBA-ready .wav per FORMATS.md §1: fmt/data/smpl/agbp/agbl.
QByteArray fixtureWav(const FixtureSpec &spec)
{
    QByteArray wav("RIFF\0\0\0\0WAVE", 12);
    wav += "fmt ";
    putU32(&wav, 16);
    putU16(&wav, 1); // PCM
    putU16(&wav, spec.channels);
    putU32(&wav, spec.rate);
    putU32(&wav, spec.rate * spec.channels);
    putU16(&wav, spec.channels); // blockAlign
    putU16(&wav, 8);
    wav += "data";
    putU32(&wav, quint32(spec.samples.size()));
    wav += spec.samples;
    if (spec.samples.size() & 1)
        wav += '\0'; // RIFF pad byte
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

} // namespace

int runSampleCheck(const QString &scratchDir)
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

    // ---- the dialog, offscreen: live validation gates the commit ----
    {
        const int before = failures;
        SampleWavInfo info;
        QString error;
        SampleRegistrar::inspectSampleWav(fixture, &info, &error);
        const QStringList symbols = VoicegroupSource::directSoundSymbols(root);
        SampleEditorDialog dialog(
            root + QStringLiteral("/sound/direct_sound_samples/"
                                  "samplecheck_tone.wav"),
            info, [&](const QString &name, QString *validationError) {
                return SampleRegistrar::validateSampleName(root, name, symbols,
                                                           validationError);
            });
        auto *nameEdit =
            dialog.findChild<QLineEdit *>(QStringLiteral("sampleNameEdit"));
        auto *addButton =
            dialog.findChild<QPushButton *>(QStringLiteral("sampleAddButton"));
        auto *status =
            dialog.findChild<QLabel *>(QStringLiteral("sampleNameStatus"));
        expect(nameEdit && addButton && status, "dialog widgets found");
        if (nameEdit && addButton && status) {
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
        }
        if (failures == before)
            std::printf("samplecheck: dialog validation OK\n");
    }

    std::printf("samplecheck: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
