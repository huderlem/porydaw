#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <cstdio>
#include <functional>

#include "bundlecheckfixtures.h"
#include "core/smf.h"
#include "core/songdocument.h"
#include "project/bundlearchive.h"
#include "project/bundleexport.h"
#include "project/decompproject.h"
#include "project/samplereg.h"
#include "project/songbundle.h"
#include "project/voicegroupsource.h"

extern "C" {
#include "voicegroup_loader.h"
}

// The export sections of --bundlecheck (docs/song-bundle/PLAN.md Phase 1),
// run by runBundleCheck against scratch projects built here:
//  - a macro-style (pokeemerald) project whose song selects programs 0
//    (implicitly: notes before any program change), 1, 5 (keysplit), 9
//    (drumkit), 20 (programmable wave), 30 (Golden Sun synth) and 40 (cry),
//    with a portamento CC: exact file list, trimmed voicegroup, manifest,
//    byte-identical re-export, and a voicegroup_load of the extracted bundle
//    that matches the project's load on every used slot;
//  - unsaved voice and pending-synth edits export without touching disk;
//  - refusals: no voicegroup source, .aif sample, undefined symbols, missing
//    cry build artifact;
//  - label-style projects, per-file and monolithic, whose short drumset
//    overflows into the voicegroup assembled after it (ROM contiguity);
//  - when PORYDAW_SAMPLE_CORPUS names a real decomp tree (the sweep sets
//    it), every 8th playable song of it exports and its bundle loads
//    identically to the project on every program the song selects.

namespace {

int *g_failures = nullptr;

void expect(bool ok, const QString &what)
{
    if (!ok) {
        std::fprintf(stderr, "bundlecheck: FAIL: export: %s\n", qUtf8Printable(what));
        (*g_failures)++;
    }
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
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QStringList treeFiles(const QString &root)
{
    QStringList out;
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext())
        out.append(QDir(root).relativeFilePath(it.next()));
    out.sort();
    return out;
}

void putLe32(QByteArray *out, quint32 v)
{
    for (int i = 0; i < 4; i++)
        out->append(char((v >> (8 * i)) & 0xff));
}

void putLe16(QByteArray *out, quint16 v)
{
    out->append(char(v & 0xff));
    out->append(char(v >> 8));
}

// A minimal 8-bit mono PCM .wav; seed makes each sample's bytes (and so its
// hash and loaded size) distinct.
QByteArray fixtureWav(int seed)
{
    QByteArray data;
    const int frames = 64 + seed * 8;
    for (int i = 0; i < frames; i++)
        data.append(char(128 + ((i * (seed + 3)) % 100) - 50));
    QByteArray wav("RIFF");
    putLe32(&wav, quint32(36 + data.size()));
    wav += "WAVEfmt ";
    putLe32(&wav, 16);
    putLe16(&wav, 1); // PCM
    putLe16(&wav, 1); // mono
    putLe32(&wav, 13379);
    putLe32(&wav, 13379);
    putLe16(&wav, 1);
    putLe16(&wav, 8);
    wav += "data";
    putLe32(&wav, quint32(data.size()));
    wav += data;
    return wav;
}

// A raw GBA sample (.bin build artifact): 16-byte WaveData header + PCM.
QByteArray fixtureBin(int frames)
{
    QByteArray bin;
    putLe16(&bin, 0);
    putLe16(&bin, 0);
    putLe32(&bin, 13379 * 1024);
    putLe32(&bin, 0);
    putLe32(&bin, quint32(frames));
    for (int i = 0; i <= frames; i++)
        bin.append(char((i * 5) % 90 - 45));
    return bin;
}

QByteArray sampleEntry(const QByteArray &symbol, const QByteArray &path)
{
    return "\t.align 2\n" + symbol + "::\n\t.incbin \"" + path + "\"\n\n";
}

const char kDummy[] = "\tvoice_square_1 60, 0, 0, 2, 0, 0, 15, 0";

// 128 voice lines: the dummy square except where voices names another line.
QByteArray voiceLines(const QHash<int, QByteArray> &voices, int count = 128)
{
    QByteArray out;
    for (int i = 0; i < count; i++)
        out += voices.value(i, kDummy) + '\n';
    return out;
}

SmfEvent channelEvent(uint64_t tick, uint8_t status, uint8_t d0, uint8_t d1)
{
    SmfEvent ev;
    ev.tick = tick;
    ev.status = status;
    ev.data0 = d0;
    ev.data1 = d1;
    return ev;
}

// One track per program; a negative program means "no program change" (the
// track plays the default voice 0). The first real track carries the
// portamento CC when requested.
QByteArray fixtureMidi(const QList<int> &programs, bool portamento)
{
    SmfFile smf;
    smf.division = 24;
    SmfTrack conductor;
    SmfEvent tempo;
    tempo.status = 0xFF;
    tempo.metaType = 0x51;
    tempo.blob = QByteArray("\x07\xA1\x20", 3);
    conductor.events.push_back(tempo);
    conductor.endTick = 96;
    smf.tracks.push_back(conductor);
    for (int i = 0; i < programs.size(); i++) {
        const uint8_t ch = uint8_t(i);
        SmfTrack track;
        if (programs.at(i) >= 0)
            track.events.push_back(channelEvent(0, 0xC0 | ch, uint8_t(programs.at(i)), 0));
        if (portamento && i == 0)
            track.events.push_back(channelEvent(0, 0xB0 | ch, 0x05, 12));
        track.events.push_back(channelEvent(0, 0x90 | ch, 60, 100));
        track.events.push_back(channelEvent(48, 0x80 | ch, 60, 0));
        track.endTick = 96;
        smf.tracks.push_back(track);
    }
    return smf.write();
}

// An open song over a scratch project: what MainWindow hands the exporter.
struct OpenSong {
    SongDocument doc;
    VoicegroupSource vg;
    bool ok = false;

    OpenSong(const QString &root, const QString &label, const QString &voicegroupArg)
    {
        SongInfo song;
        song.label = label;
        song.midPath = root + QStringLiteral("/sound/songs/midi/%1.mid").arg(label);
        song.hasMid = true;
        song.hasCfg = true;
        song.cfg.voicegroupArg = voicegroupArg;
        song.cfg.masterVolume = 90;
        song.cfg.reverb = 50;
        song.cfg.exactGate = true;
        song.cfg.rawFlags = {QStringLiteral("-E"), QStringLiteral("-R50"),
                             QStringLiteral("-G") + voicegroupArg, QStringLiteral("-V090")};
        QString error;
        ok = doc.load(song, &error) && vg.open(root, voicegroupArg, &error);
        if (!ok)
            std::fprintf(stderr, "bundlecheck: fixture open failed: %s\n", qUtf8Printable(error));
    }
};

QString describeWave(const WaveData *wd)
{
    if (!wd)
        return QStringLiteral("null");
    QByteArray head(reinterpret_cast<const char *>(wd->data),
                    wd->size ? int(qMin(wd->size, 24u)) : 6);
    return QStringLiteral("%1/%2/%3/%4/%5")
        .arg(wd->type)
        .arg(wd->freq)
        .arg(wd->loopStart)
        .arg(wd->size)
        .arg(QString::fromLatin1(head.toHex()));
}

// Everything audible about a voice, sub-voicegroups included (one level,
// like the engine).
QString describeVoice(const ToneData &td, bool nested = false, int skipSubSlot = -1)
{
    QString out = QStringLiteral("t%1 k%2 p%3 a%4/%5/%6/%7 ")
                      .arg(td.type)
                      .arg(td.key)
                      .arg(td.panSweep)
                      .arg(td.attack)
                      .arg(td.decay)
                      .arg(td.sustain)
                      .arg(td.release);
    if (td.type == VOICE_KEYSPLIT || td.type == VOICE_KEYSPLIT_ALL) {
        if (!td.subGroup || nested)
            return out + QStringLiteral("sub:null");
        if (td.type == VOICE_KEYSPLIT) {
            out += QStringLiteral("table:");
            out +=
                td.keySplitTable
                    ? QString::fromLatin1(
                          QByteArray(reinterpret_cast<const char *>(td.keySplitTable), 128).toHex())
                    : QStringLiteral("null");
        }
        const ToneData *sub = static_cast<const ToneData *>(td.subGroup);
        for (int i = 0; i < VOICEGROUP_SIZE; i++) {
            if (i != skipSubSlot)
                out += QStringLiteral(" [%1 %2]").arg(i).arg(describeVoice(sub[i], true));
        }
        return out;
    }
    const int cgb = td.type & 0x07;
    if (cgb == VOICE_PROGRAMMABLE_WAVE) {
        return out +
               (td.wavePointer
                    ? QString::fromLatin1(
                          QByteArray(reinterpret_cast<const char *>(td.wavePointer), 16).toHex())
                    : QStringLiteral("wave:null"));
    }
    if (cgb != 0)
        return out + QStringLiteral("cgb:%1").arg(quintptr(td.wavePointer));
    return out + describeWave(td.wav);
}

struct Loaded {
    LoadedVoiceGroup *vg = nullptr;
    Loaded(const QString &root, const QString &name)
    {
        vg = voicegroup_load(root.toLocal8Bit().constData(), name.toLocal8Bit().constData(),
                             nullptr);
    }
    ~Loaded()
    {
        if (vg)
            voicegroup_free(vg);
    }
};

// The bundle at bundleRoot loads, and sounds exactly like the project on
// every used program. skipSubSlot exempts one sub-voicegroup key (a nested
// keysplit in an overflow region, which the bundle replaces by design).
void expectSameVoices(const QString &tag, const QString &projectRoot, const QString &bundleRoot,
                      const QString &loadName, const QList<int> &usedSlots, int skipSubSlot = -1)
{
    Loaded project(projectRoot, loadName);
    Loaded bundle(bundleRoot, loadName);
    expect(project.vg != nullptr, tag + QStringLiteral(": project voicegroup loads"));
    expect(bundle.vg != nullptr, tag + QStringLiteral(": bundle voicegroup loads"));
    if (!project.vg || !bundle.vg)
        return;
    for (int slot : usedSlots) {
        const QString a = describeVoice(project.vg->voices[slot], false, skipSubSlot);
        const QString b = describeVoice(bundle.vg->voices[slot], false, skipSubSlot);
        expect(a == b, QStringLiteral("%1: slot %2 differs between project and bundle\n  "
                                      "project: %3\n  bundle:  %4")
                           .arg(tag)
                           .arg(slot)
                           .arg(a.left(400), b.left(400)));
    }
}

const char kSamples[] = "sound/direct_sound_samples/";

bool buildMacroProject(const QString &root)
{
    const QString snd = root + QStringLiteral("/sound/");
    QByteArray dsData;
    int seed = 0;
    for (const char *name :
         {"flute", "organ", "unused", "strings_lo", "strings_hi", "kick", "snare", "orphan"}) {
        dsData += sampleEntry(QByteArray("DirectSoundWaveData_") + name,
                              QByteArray(kSamples) + name + ".bin");
        if (!writeFile(snd + QStringLiteral("direct_sound_samples/%1.wav").arg(QLatin1String(name)),
                       fixtureWav(++seed)))
            return false;
    }
    // A cry: lives in a subdirectory, has no DirectSoundWaveData_ prefix, and
    // is read by `cry` voices from the raw .bin only.
    dsData += sampleEntry("Cry_Testmon", QByteArray(kSamples) + "cries/testmon.bin");

    QHash<int, QByteArray> top;
    top[0] = "\tvoice_directsound 60, 0, DirectSoundWaveData_flute, 255, 0, 255, 165";
    top[1] = "\tvoice_directsound_no_resample 60, 10, DirectSoundWaveData_organ, 255, 249, 0, 165 "
             "@ organ";
    top[2] = "\tvoice_directsound 60, 0, DirectSoundWaveData_unused, 255, 0, 255, 165";
    top[3] = "\tvoice_keysplit voicegroup_unused_keysplit, keysplit_unused";
    top[5] = "\tvoice_keysplit voicegroup_strings_keysplit, keysplit_strings";
    top[9] = "\tvoice_keysplit_all voicegroup_test_drumset";
    top[20] = "\tvoice_programmable_wave 60, 0, ProgrammableWaveData_7, 0, 7, 15, 1";
    top[21] = "\tvoice_programmable_wave 60, 0, ProgrammableWaveData_8, 0, 7, 15, 1";
    top[30] = "\tvoice_directsound 60, 0, DirectSoundSynth_GoldenSun_80101020, 255, 0, 255, 165";
    top[31] = "\tvoice_directsound 60, 0, DirectSoundSynth_GoldenSun_Saw, 255, 0, 255, 165";
    top[40] = "\tcry Cry_Testmon";
    top[50] = "\tvoice_noise 60, 0, 1, 0, 1, 9, 2";

    QHash<int, QByteArray> strings;
    strings[0] = "\tvoice_directsound 60, 0, DirectSoundWaveData_strings_lo, 255, 0, 255, 165";
    strings[1] = "\tvoice_directsound 72, 0, DirectSoundWaveData_strings_hi, 255, 0, 255, 165";
    QHash<int, QByteArray> drums;
    drums[0] = "\tvoice_directsound_no_resample 60, 64, DirectSoundWaveData_kick, 255, 0, 255, 242";
    drums[2] =
        "\tvoice_directsound_no_resample 60, 64, DirectSoundWaveData_snare, 255, 0, 255, 242";
    drums[3] = "\tvoice_noise 60, 0, 0, 0, 2, 0, 1";

    return writeFile(snd + QStringLiteral("direct_sound_data.inc"), dsData) &&
           writeFile(snd + QStringLiteral("direct_sound_samples/cries/testmon.bin"),
                     fixtureBin(200)) &&
           writeFile(snd + QStringLiteral("direct_sound_synth_data.inc"),
                     "\t.align 2\nDirectSoundSynth_GoldenSun_80101020::\n"
                     "\tset_synth_custom 0x80, 0x10, 0x10, 0x20\n\n"
                     "\t.align 2\nDirectSoundSynth_GoldenSun_Saw::\n\tset_synth_25\n") &&
           writeFile(snd + QStringLiteral("programmable_wave_data.inc"),
                     "\t.align 2\nProgrammableWaveData_7::\n"
                     "\t.incbin \"sound/programmable_wave_samples/07.pcm\"\n\n"
                     "\t.align 2\nProgrammableWaveData_8::\n"
                     "\t.incbin \"sound/programmable_wave_samples/08.pcm\"\n") &&
           writeFile(snd + QStringLiteral("programmable_wave_samples/07.pcm"),
                     QByteArray::fromHex("0123456789abcdeffedcba9876543210")) &&
           writeFile(snd + QStringLiteral("programmable_wave_samples/08.pcm"),
                     QByteArray::fromHex("ffffffffffffffff0000000000000000")) &&
           writeFile(snd + QStringLiteral("keysplit_tables.inc"),
                     "@ tables\n\nkeysplit unused, 36\n\tsplit 0, 60\n\tsplit 1, 108\n\n"
                     "@ the strings\nkeysplit strings, 36\n\tsplit 0, 69 @ low\n"
                     "\tsplit 1, 108\n\n@ trailing commentary\n") &&
           writeFile(snd + QStringLiteral("voice_groups.inc"),
                     ".include \"sound/voicegroups/drumsets/test.inc\"\n"
                     ".include \"sound/voicegroups/keysplits/strings.inc\"\n"
                     ".include \"sound/voicegroups/keysplits/unused.inc\"\n"
                     ".include \"sound/voicegroups/bundle_song.inc\"\n") &&
           writeFile(snd + QStringLiteral("voicegroups/bundle_song.inc"),
                     "voice_group bundle_song\n" + voiceLines(top)) &&
           writeFile(snd + QStringLiteral("voicegroups/keysplits/strings.inc"),
                     "voice_group strings_keysplit\n" + voiceLines(strings, 2)) &&
           writeFile(snd + QStringLiteral("voicegroups/keysplits/unused.inc"),
                     "voice_group unused_keysplit\n" + voiceLines(strings, 2)) &&
           writeFile(snd + QStringLiteral("voicegroups/drumsets/test.inc"),
                     "voice_group test_drumset, 36\n" + voiceLines(drums, 4)) &&
           writeFile(snd + QStringLiteral("songs/midi/mus_bundle.mid"),
                     fixtureMidi({-1, 1, 5, 9, 20, 30, 40}, true));
}

// A label-style project whose 3-voice drumset overflows into the group
// assembled after it. monolithic: everything in sound/voice_groups.inc
// (pokefirered); otherwise one label-form file per group, ordered by the hub.
bool buildLabelProject(const QString &root, bool monolithic)
{
    const QString snd = root + QStringLiteral("/sound/");
    QByteArray dsData;
    int seed = 10;
    for (const char *name : {"lead", "kick", "overflow", "beyond"}) {
        dsData += sampleEntry(QByteArray("DirectSoundWaveData_") + name,
                              QByteArray(kSamples) + name + ".bin");
        if (!writeFile(snd + QStringLiteral("direct_sound_samples/%1.wav").arg(QLatin1String(name)),
                       fixtureWav(++seed)))
            return false;
    }
    QHash<int, QByteArray> top;
    top[0] = "\tvoice_directsound 60, 0, DirectSoundWaveData_lead, 255, 0, 255, 165";
    top[1] = "\tvoice_keysplit_all voicegroup101";
    QHash<int, QByteArray> drums;
    drums[0] = "\tvoice_directsound 60, 0, DirectSoundWaveData_kick, 255, 0, 255, 165";
    QHash<int, QByteArray> next;
    next[0] = "\tvoice_directsound 60, 0, DirectSoundWaveData_overflow, 255, 0, 255, 165";
    next[1] = "\tvoice_keysplit_all voicegroup100"; // never recursed from an overflow region
    const QByteArray g100 = "\t.align 2\nvoicegroup100::\n" + voiceLines(top);
    const QByteArray g101 = "\t.align 2\nvoicegroup101::\n" + voiceLines(drums, 3);
    const QByteArray g102 = "\t.align 2\nvoicegroup102::\n" + voiceLines(next, 126);
    // Past slot 128 of the drumset's view: must not be pulled in.
    const QByteArray g103 =
        "\t.align 2\nvoicegroup103::\n"
        "\tvoice_directsound 60, 0, DirectSoundWaveData_beyond, 255, 0, 255, 165\n";

    bool ok =
        writeFile(snd + QStringLiteral("direct_sound_data.inc"), dsData) &&
        writeFile(snd + QStringLiteral("songs/midi/mus_label.mid"), fixtureMidi({0, 1}, false));
    if (monolithic)
        return ok && writeFile(snd + QStringLiteral("voice_groups.inc"), g100 + g101 + g102 + g103);
    return ok &&
           writeFile(snd + QStringLiteral("voice_groups.inc"),
                     ".include \"sound/voicegroups/voicegroup100.inc\"\n"
                     ".include \"sound/voicegroups/voicegroup101.inc\"\n"
                     ".include \"sound/voicegroups/voicegroup102.inc\"\n"
                     ".include \"sound/voicegroups/voicegroup103.inc\"\n") &&
           writeFile(snd + QStringLiteral("voicegroups/voicegroup100.inc"), g100) &&
           writeFile(snd + QStringLiteral("voicegroups/voicegroup101.inc"), g101) &&
           writeFile(snd + QStringLiteral("voicegroups/voicegroup102.inc"), g102) &&
           writeFile(snd + QStringLiteral("voicegroups/voicegroup103.inc"), g103);
}

} // namespace

namespace bundlefixtures {

bool buildMacroProject(const QString &root)
{
    return ::buildMacroProject(root);
}

QByteArray fixtureWav(int seed)
{
    return ::fixtureWav(seed);
}

QStringList describeSlots(const QString &root, const QString &loadName, const QList<int> &slotList)
{
    QStringList out;
    const Loaded loaded(root, loadName);
    if (!loaded.vg)
        return out;
    for (int slot : slotList)
        out.append(describeVoice(loaded.vg->voices[slot]));
    return out;
}

} // namespace bundlefixtures

void runBundleExportSections(const QString &scratchDir, int *failures)
{
    g_failures = failures;
    const auto path = [&](const char *name) {
        return scratchDir + QStringLiteral("/export_") + QLatin1String(name);
    };

    { // ---- used-program and extension scans ----------------------------------
        SmfFile smf;
        QString error;
        expect(SmfFile::read(fixtureMidi({-1, 1, 5}, true), &smf, &error),
               QStringLiteral("fixture MIDI parses"));
        expect(SongBundle::usedPrograms(smf) == QSet<int>({0, 1, 5}),
               QStringLiteral("used programs = program changes + 0 for a track that plays "
                              "before any"));
        expect(SongBundle::usedExtensions(smf) == QStringList{QStringLiteral("PORTAMENTO")},
               QStringLiteral("portamento CC is the one extension"));
        expect(SmfFile::read(fixtureMidi({7}, false), &smf, &error) &&
                   SongBundle::usedPrograms(smf) == QSet<int>({7}) &&
                   SongBundle::usedExtensions(smf).isEmpty(),
               QStringLiteral("a program change before the first note keeps 0 out"));
    }

    const QString root = path("macro");
    expect(buildMacroProject(root), QStringLiteral("macro fixture project builds"));
    const QList<int> usedSlots = {0, 1, 5, 9, 20, 30, 40};
    const QStringList expectedFiles = {
        QStringLiteral("porysong.json"),
        QStringLiteral("sound/direct_sound_data.inc"),
        QStringLiteral("sound/direct_sound_samples/Cry_Testmon.bin"),
        QStringLiteral("sound/direct_sound_samples/flute.wav"),
        QStringLiteral("sound/direct_sound_samples/kick.wav"),
        QStringLiteral("sound/direct_sound_samples/organ.wav"),
        QStringLiteral("sound/direct_sound_samples/snare.wav"),
        QStringLiteral("sound/direct_sound_samples/strings_hi.wav"),
        QStringLiteral("sound/direct_sound_samples/strings_lo.wav"),
        QStringLiteral("sound/direct_sound_synth_data.inc"),
        QStringLiteral("sound/keysplit_tables.inc"),
        QStringLiteral("sound/programmable_wave_data.inc"),
        QStringLiteral("sound/programmable_wave_samples/7.pcm"),
        QStringLiteral("sound/songs/midi/midi.cfg"),
        QStringLiteral("sound/songs/midi/mus_bundle.mid"),
        QStringLiteral("sound/voicegroups/bundle_song.inc"),
        QStringLiteral("sound/voicegroups/strings_keysplit.inc"),
        QStringLiteral("sound/voicegroups/test_drumset.inc"),
    };

    { // ---- the full export -----------------------------------------------------
        OpenSong song(root, QStringLiteral("mus_bundle"), QStringLiteral("_bundle_song"));
        expect(song.ok, QStringLiteral("macro fixture song opens"));
        const QStringList projectBefore = treeFiles(root);

        SongBundle::Exporter exporter(root, song.doc, &song.vg);
        exporter.setRegistrationHints(QStringLiteral("MUS_BUNDLE"), QStringLiteral("MUS_PLAYER"));
        const QString zip = path("a.porysong");
        QString error;
        expect(exporter.exportTo(zip, &error), QStringLiteral("export succeeds: ") + error);
        expect(treeFiles(root) == projectBefore, QStringLiteral("export leaves the project alone"));

        QStringList entries;
        expect(BundleArchive::listBundle(zip, &entries, &error) && entries == expectedFiles,
               QStringLiteral("exact file list in the zip, got: ") +
                   entries.join(QLatin1Char(' ')));

        const QString out = path("a_extracted");
        expect(BundleArchive::extractBundle(zip, out, &error),
               QStringLiteral("exported zip extracts: ") + error);
        expect(SongBundle::isBundleDir(out), QStringLiteral("extracted export is a bundle dir"));

        // Manifest.
        BundleManifest m;
        expect(BundleManifest::read(out, &m, &error), QStringLiteral("manifest reads: ") + error);
        expect(m == exporter.manifest(), QStringLiteral("manifest() matches what was written"));
        expect(m.format == 1 && m.porydaw == QStringLiteral(PORYDAW_VERSION),
               QStringLiteral("manifest format + writer version"));
        expect(m.label == QStringLiteral("mus_bundle") &&
                   m.voicegroup == QStringLiteral("voicegroup_bundle_song") &&
                   m.flags == QStringLiteral("-E -R50 -G_bundle_song -V090") &&
                   m.constant == QStringLiteral("MUS_BUNDLE") &&
                   m.player == QStringLiteral("MUS_PLAYER") &&
                   m.layout == QStringLiteral("pokeemerald"),
               QStringLiteral("manifest song block, got flags '%1' layout '%2'")
                   .arg(m.flags, m.layout));
        expect(m.extensions == QStringList{QStringLiteral("PORTAMENTO")} && m.synth,
               QStringLiteral("manifest requires: PORTAMENTO + synth"));
        expect(m.subVoicegroups == QStringList({QStringLiteral("voicegroup_strings_keysplit"),
                                                QStringLiteral("voicegroup_test_drumset")}) &&
                   m.keysplitTables == QStringList{QStringLiteral("keysplit_strings")} &&
                   m.waves == QStringList{QStringLiteral("ProgrammableWaveData_7")} &&
                   m.synths == QStringList{QStringLiteral("DirectSoundSynth_GoldenSun_80101020")},
               QStringLiteral("manifest symbol lists"));
        expect(m.samples.size() == 7, QStringLiteral("manifest lists 7 sample files"));
        for (const BundleSample &s : m.samples) {
            const QByteArray bytes = readFileBytes(out + QLatin1Char('/') + s.file);
            expect(!bytes.isEmpty() && SampleRegistrar::sourceHashHex(bytes) == s.sha256,
                   QStringLiteral("sample %1 hash matches its stored bytes").arg(s.file));
        }
        expect(
            readFileBytes(out + QStringLiteral("/sound/direct_sound_samples/flute.wav")) ==
                    readFileBytes(root + QStringLiteral("/sound/direct_sound_samples/flute.wav")) &&
                readFileBytes(out +
                              QStringLiteral("/sound/direct_sound_samples/Cry_Testmon.bin")) ==
                    readFileBytes(root + QStringLiteral("/sound/direct_sound_samples/cries/"
                                                        "testmon.bin")),
            QStringLiteral("samples are byte copies (the cry flattened out of cries/)"));

        // Trimmed top-level voicegroup.
        const QList<QByteArray> vgLines =
            readFileBytes(out + QStringLiteral("/sound/voicegroups/bundle_song.inc")).split('\n');
        expect(vgLines.size() == 130 && vgLines.at(0) == "voice_group bundle_song",
               QStringLiteral("trimmed voicegroup keeps its header and 128 lines"));
        if (vgLines.size() == 130) {
            for (int slot : {2, 3, 21, 31, 50})
                expect(vgLines.at(slot + 1) == kDummy,
                       QStringLiteral("unused slot %1 is the dummy square").arg(slot));
            expect(vgLines.at(2).endsWith("@ organ") && vgLines.at(41) == "\tcry Cry_Testmon",
                   QStringLiteral("used lines are verbatim"));
        }
        const QByteArray dsInc =
            readFileBytes(out + QStringLiteral("/sound/direct_sound_data.inc"));
        expect(!dsInc.contains("unused") && !dsInc.contains("orphan") &&
                   dsInc.contains("Cry_Testmon::\n\t.incbin "
                                  "\"sound/direct_sound_samples/Cry_Testmon.bin\""),
               QStringLiteral("direct_sound_data.inc holds only referenced symbols, flat paths"));
        const QByteArray tables = readFileBytes(out + QStringLiteral("/sound/keysplit_tables.inc"));
        expect(tables == "keysplit strings, 36\n\tsplit 0, 69 @ low\n\tsplit 1, 108\n",
               QStringLiteral("only the referenced keysplit table, verbatim: ") +
                   QString::fromUtf8(tables));
        expect(readFileBytes(out + QStringLiteral("/sound/songs/midi/midi.cfg")) ==
                   "mus_bundle.mid: -E -R50 -G_bundle_song -V090\n",
               QStringLiteral("midi.cfg is the song's one line"));
        expect(readFileBytes(out + QStringLiteral("/sound/songs/midi/mus_bundle.mid")) ==
                   song.doc.smf().write(),
               QStringLiteral(".mid is the serialized document"));

        // The loader resolves everything the song uses, from the bundle alone.
        {
            Loaded bundle(out, QStringLiteral("bundle_song"));
            expect(bundle.vg != nullptr, QStringLiteral("bundle voicegroup loads"));
            if (bundle.vg) {
                const ToneData *v = bundle.vg->voices;
                expect(v[0].wav && v[1].wav && v[30].wav && v[40].wav,
                       QStringLiteral("sample, synth and cry voices resolve"));
                expect(v[30].wav && v[30].wav->size == 0,
                       QStringLiteral("synth voice is a synth descriptor"));
                expect(v[5].subGroup && v[5].keySplitTable,
                       QStringLiteral("keysplit resolves its sub-voicegroup and table"));
                expect(v[9].subGroup != nullptr, QStringLiteral("drumkit resolves"));
                expect(v[20].wavePointer != nullptr, QStringLiteral("programmable wave resolves"));
                if (v[9].subGroup) {
                    const ToneData *drums = static_cast<const ToneData *>(v[9].subGroup);
                    expect(drums[36].wav && drums[38].wav,
                           QStringLiteral("drumkit samples resolve at their keys"));
                }
            }
        }
        expectSameVoices(QStringLiteral("macro"), root, out, QStringLiteral("bundle_song"),
                         usedSlots);

        // Determinism: a second export, and a pack of the extracted tree.
        const QString zip2 = path("a2.porysong");
        SongBundle::Exporter again(root, song.doc, &song.vg);
        again.setRegistrationHints(QStringLiteral("MUS_BUNDLE"), QStringLiteral("MUS_PLAYER"));
        expect(again.exportTo(zip2, &error) && readFileBytes(zip) == readFileBytes(zip2) &&
                   !readFileBytes(zip).isEmpty(),
               QStringLiteral("re-export is byte-identical"));

        // A staged directory is the same bundle.
        const QString staged = path("a_staged");
        expect(again.stage(staged, &error) && treeFiles(staged) == expectedFiles,
               QStringLiteral("stage() writes the same tree as the zip"));
    }

    { // ---- unsaved edits export, disk untouched ------------------------------
        OpenSong song(root, QStringLiteral("mus_bundle"), QStringLiteral("_bundle_song"));
        const QByteArray diskBefore =
            readFileBytes(root + QStringLiteral("/sound/voicegroups/bundle_song.inc"));
        VgVoice voice = *song.vg.voiceAt(0);
        voice.release = 77;
        song.vg.setVoice(0, voice);
        // Slot 1 switches to a minted-but-unsaved synth definition.
        VgVoice synthVoice = *song.vg.voiceAt(1);
        synthVoice.macro = VgMacro::DirectSound;
        synthVoice.symbol = QStringLiteral("DirectSoundSynth_GoldenSun_Triangle");
        song.vg.setVoice(1, synthVoice);

        QString error;
        SongBundle::Exporter blocked(root, song.doc, &song.vg);
        expect(!blocked.stage(path("pending_refused"), &error) &&
                   error.contains(QStringLiteral("DirectSoundSynth_GoldenSun_Triangle")),
               QStringLiteral("an undefined synth symbol refuses by name: ") + error);

        SongBundle::Exporter exporter(root, song.doc, &song.vg);
        VgSynthDesc triangle;
        triangle.waveform = 2;
        exporter.setPendingSynths(
            {{QStringLiteral("DirectSoundSynth_GoldenSun_Triangle"), triangle}});
        const QString out = path("pending");
        expect(exporter.stage(out, &error), QStringLiteral("pending-synth export: ") + error);
        const QByteArray vg =
            readFileBytes(out + QStringLiteral("/sound/voicegroups/bundle_song.inc"));
        expect(vg.contains("DirectSoundWaveData_flute, 255, 0, 255, 77"),
               QStringLiteral("unsaved voice edit is exported"));
        expect(readFileBytes(out + QStringLiteral("/sound/direct_sound_synth_data.inc"))
                   .contains("DirectSoundSynth_GoldenSun_Triangle::\n\tset_synth_triangle\n"),
               QStringLiteral("pending synth definition is written into the bundle"));
        expect(!QFile::exists(out + QStringLiteral("/sound/direct_sound_samples/organ.wav")),
               QStringLiteral("the replaced voice's sample is no longer bundled"));
        expect(readFileBytes(root + QStringLiteral("/sound/voicegroups/bundle_song.inc")) ==
                   diskBefore,
               QStringLiteral("project voicegroup file untouched"));
        Loaded bundle(out, QStringLiteral("bundle_song"));
        expect(bundle.vg && bundle.vg->voices[1].wav && bundle.vg->voices[1].wav->size == 0 &&
                   bundle.vg->voices[0].release == 77,
               QStringLiteral("edited bundle loads with the synth and the new release"));
    }

    { // ---- refusals ------------------------------------------------------------
        OpenSong song(root, QStringLiteral("mus_bundle"), QStringLiteral("_bundle_song"));
        QString error;
        const auto refuses = [&](const char *tag, const QStringList &needles) {
            const QString dest = path(tag);
            SongBundle::Exporter exporter(root, song.doc, &song.vg);
            error.clear();
            bool ok = !exporter.exportTo(dest + QStringLiteral(".porysong"), &error);
            for (const QString &needle : needles)
                ok = ok && error.contains(needle);
            expect(ok, QStringLiteral("refusal '%1' names the problem, got: %2")
                           .arg(QLatin1String(tag), error));
            expect(!QFile::exists(dest + QStringLiteral(".porysong")),
                   QStringLiteral("refusal '%1' writes no zip").arg(QLatin1String(tag)));
        };

        SongBundle::Exporter noSource(root, song.doc, nullptr);
        expect(!noSource.exportTo(path("nosource.porysong"), &error) &&
                   error.contains(QStringLiteral("voicegroup")),
               QStringLiteral("no voicegroup source refuses"));

        const QString samples = root + QStringLiteral("/sound/direct_sound_samples/");
        // .aif-only samples (the legacy pipeline).
        QFile::rename(samples + QStringLiteral("kick.wav"), samples + QStringLiteral("kick.aif"));
        refuses("aif", {QStringLiteral("DirectSoundWaveData_kick"), QStringLiteral(".aif"),
                        QStringLiteral("Sample Studio")});
        QFile::rename(samples + QStringLiteral("kick.aif"), samples + QStringLiteral("kick.wav"));

        // A sample file that is simply gone, and a cry whose .bin isn't built.
        QFile::rename(samples + QStringLiteral("snare.wav"), samples + QStringLiteral("snare.bak"));
        QFile::rename(samples + QStringLiteral("cries/testmon.bin"),
                      samples + QStringLiteral("cries/testmon.bak"));
        refuses("missing", {QStringLiteral("DirectSoundWaveData_snare"),
                            QStringLiteral("Cry_Testmon"), QStringLiteral("incomplete")});
        QFile::rename(samples + QStringLiteral("snare.bak"), samples + QStringLiteral("snare.wav"));
        QFile::rename(samples + QStringLiteral("cries/testmon.bak"),
                      samples + QStringLiteral("cries/testmon.bin"));

        // Undefined table, wave and sub-voicegroup symbols, all named at once.
        const QString snd = root + QStringLiteral("/sound/");
        const QByteArray tablesBefore = readFileBytes(snd + QStringLiteral("keysplit_tables.inc"));
        const QByteArray wavesBefore =
            readFileBytes(snd + QStringLiteral("programmable_wave_data.inc"));
        writeFile(snd + QStringLiteral("keysplit_tables.inc"), "@ nothing\n");
        writeFile(snd + QStringLiteral("programmable_wave_data.inc"), "@ nothing\n");
        QFile::rename(snd + QStringLiteral("voicegroups/drumsets/test.inc"),
                      snd + QStringLiteral("voicegroups/drumsets/test.bak"));
        refuses("undefined",
                {QStringLiteral("keysplit_strings"), QStringLiteral("ProgrammableWaveData_7"),
                 QStringLiteral("voicegroup_test_drumset")});
        writeFile(snd + QStringLiteral("keysplit_tables.inc"), tablesBefore);
        writeFile(snd + QStringLiteral("programmable_wave_data.inc"), wavesBefore);
        QFile::rename(snd + QStringLiteral("voicegroups/drumsets/test.bak"),
                      snd + QStringLiteral("voicegroups/drumsets/test.inc"));

        // A voice line gas assembles (macro arguments may be space-separated)
        // but the loader can't read: import would refuse it, so export does.
        const QString stringsInc = snd + QStringLiteral("voicegroups/keysplits/strings.inc");
        const QByteArray stringsBefore = readFileBytes(stringsInc);
        QByteArray noComma = stringsBefore;
        noComma.replace("DirectSoundWaveData_strings_hi,", "DirectSoundWaveData_strings_hi");
        writeFile(stringsInc, noComma);
        refuses("unreadable", {QStringLiteral("voicegroup_strings_keysplit, voice 1"),
                               QStringLiteral("DirectSoundWaveData_strings_hi 255")});
        writeFile(stringsInc, stringsBefore);

        SongBundle::Exporter restored(root, song.doc, &song.vg);
        restored.setRegistrationHints(QStringLiteral("MUS_BUNDLE"), QStringLiteral("MUS_PLAYER"));
        error.clear();
        expect(restored.exportTo(path("restored.porysong"), &error) &&
                   readFileBytes(path("restored.porysong")) == readFileBytes(path("a2.porysong")),
               QStringLiteral("restored project exports the same bytes again: ") + error);
    }

    { // ---- nonstandard layouts the loader still resolves ----------------------
        const QString oddRoot = path("odd");
        expect(buildMacroProject(oddRoot), QStringLiteral("odd-layout fixture builds"));
        const QString snd = oddRoot + QStringLiteral("/sound/");
        const QString samples = snd + QStringLiteral("direct_sound_samples/");
        // organ: a raw sample whose .incbin isn't called .bin. flute: no
        // .incbin at all, found as <symbol>.wav in a sample directory.
        // Keysplit tables: kept next to the voicegroups.
        QByteArray dsData = readFileBytes(snd + QStringLiteral("direct_sound_data.inc"));
        dsData.replace(QByteArray(kSamples) + "organ.bin", QByteArray(kSamples) + "organ.raw");
        dsData.replace(sampleEntry("DirectSoundWaveData_flute", QByteArray(kSamples) + "flute.bin"),
                       "");
        expect(writeFile(snd + QStringLiteral("direct_sound_data.inc"), dsData) &&
                   writeFile(samples + QStringLiteral("organ.raw"), fixtureBin(300)) &&
                   QFile::remove(samples + QStringLiteral("organ.wav")) &&
                   QFile::rename(samples + QStringLiteral("flute.wav"),
                                 samples + QStringLiteral("DirectSoundWaveData_flute.wav")) &&
                   QFile::rename(snd + QStringLiteral("keysplit_tables.inc"),
                                 snd + QStringLiteral("voicegroups/keysplit_tables.inc")),
               QStringLiteral("odd-layout fixture rearranges"));

        OpenSong song(oddRoot, QStringLiteral("mus_bundle"), QStringLiteral("_bundle_song"));
        SongBundle::Exporter exporter(oddRoot, song.doc, &song.vg);
        const QString out = path("odd_staged");
        QString error;
        expect(exporter.stage(out, &error), QStringLiteral("odd-layout export: ") + error);
        const QStringList files = treeFiles(out);
        expect(files.contains(QStringLiteral("sound/direct_sound_samples/organ.bin")) &&
                   !files.contains(QStringLiteral("sound/direct_sound_samples/organ.raw")),
               QStringLiteral("a raw sample is bundled under the .bin name its .incbin uses"));
        expect(readFileBytes(out + QStringLiteral("/sound/direct_sound_samples/flute.wav")) ==
                   readFileBytes(samples + QStringLiteral("DirectSoundWaveData_flute.wav")),
               QStringLiteral("a sample-directory fallback .wav is bundled"));
        expect(readFileBytes(out + QStringLiteral("/sound/keysplit_tables.inc"))
                   .startsWith("keysplit strings, 36\n"),
               QStringLiteral("a keysplit table beside the voicegroups is bundled"));
        Loaded bundle(out, QStringLiteral("bundle_song"));
        expect(bundle.vg && bundle.vg->voices[0].wav && bundle.vg->voices[1].wav &&
                   bundle.vg->voices[5].keySplitTable,
               QStringLiteral("odd-layout bundle resolves all three"));
        expectSameVoices(QStringLiteral("odd"), oddRoot, out, QStringLiteral("bundle_song"),
                         usedSlots);
    }

    // ---- label-style projects: ROM-contiguity overflow of a short drumset ------
    for (bool monolithic : {false, true}) {
        const QString tag = monolithic ? QStringLiteral("monolithic") : QStringLiteral("labels");
        const QString labelRoot = path(monolithic ? "mono" : "labels");
        expect(buildLabelProject(labelRoot, monolithic), tag + QStringLiteral(": fixture builds"));
        OpenSong song(labelRoot, QStringLiteral("mus_label"), QStringLiteral("100"));
        expect(song.ok && song.vg.monolithic() == monolithic, tag + QStringLiteral(": song opens"));
        SongBundle::Exporter exporter(labelRoot, song.doc, &song.vg);
        const QString out = path(monolithic ? "mono_staged" : "labels_staged");
        QString error;
        expect(exporter.stage(out, &error), tag + QStringLiteral(": export: ") + error);
        expect(exporter.manifest().layout ==
                   (monolithic ? QStringLiteral("pokefirered") : QStringLiteral("pokeemerald")),
               tag + QStringLiteral(": manifest layout"));
        const QStringList files = treeFiles(out);
        expect(files.contains(QStringLiteral("sound/voicegroups/voicegroup100.inc")) &&
                   files.contains(QStringLiteral("sound/voicegroups/voicegroup101.inc")) &&
                   !files.contains(QStringLiteral("sound/voicegroups/voicegroup102.inc")) &&
                   !files.contains(QStringLiteral("sound/voice_groups.inc")),
               tag + QStringLiteral(": per-file voicegroups, no hub: ") +
                   files.join(QLatin1Char(' ')));
        expect(files.contains(QStringLiteral("sound/direct_sound_samples/overflow.wav")) &&
                   !files.contains(QStringLiteral("sound/direct_sound_samples/beyond.wav")),
               tag + QStringLiteral(": overflow sample bundled, nothing past slot 128"));
        const QByteArray drums =
            readFileBytes(out + QStringLiteral("/sound/voicegroups/voicegroup101.inc"));
        expect(drums.count("voice_") == 128 && drums.count("DirectSoundWaveData_overflow") == 1,
               tag + QStringLiteral(": drumset file is filled to 128 voices"));
        expect(!drums.contains("voice_keysplit"),
               tag + QStringLiteral(": an overflow-region drumkit line is not carried (the loader "
                                    "would follow it from the group's own file, here in a cycle)"));
        // Key 4 is that line: keysplit-with-no-target in the project (silent:
        // the engine never resolves a nested keysplit), a voice whose
        // envelope ends at note start in the bundle.
        expect(drums.count("\tvoice_square_1 60, 0, 0, 2, 0, 0, 0, 0") == 1,
               tag + QStringLiteral(": the overflow-region drumkit line became the silent voice"));
        expectSameVoices(tag, labelRoot, out, QStringLiteral("voicegroup100"), {0, 1}, 4);
        Loaded bundle(out, QStringLiteral("voicegroup100"));
        if (bundle.vg && bundle.vg->voices[1].subGroup) {
            const ToneData *sub = static_cast<const ToneData *>(bundle.vg->voices[1].subGroup);
            expect(sub[0].wav && sub[3].wav && sub[3].wav != sub[0].wav,
                   tag + QStringLiteral(": overflow voice sounds from the bundle"));
            expect(!(sub[4].type & (VOICE_KEYSPLIT | VOICE_KEYSPLIT_ALL)) && sub[4].attack == 0 &&
                       sub[4].decay == 0 && sub[4].sustain == 0,
                   tag + QStringLiteral(": key 4 is silent in the bundle, as in the project"));
        } else {
            expect(false, tag + QStringLiteral(": bundle drumkit resolves"));
        }

        // An unreadable overflow line is named by the slot the drumset sees
        // it at, not only by its slot in the group it was read out of.
        const QString overflowInc =
            labelRoot + (monolithic ? QStringLiteral("/sound/voice_groups.inc")
                                    : QStringLiteral("/sound/voicegroups/voicegroup102.inc"));
        QByteArray noComma = readFileBytes(overflowInc);
        noComma.replace("DirectSoundWaveData_overflow,", "DirectSoundWaveData_overflow");
        writeFile(overflowInc, noComma);
        SongBundle::Exporter broken(labelRoot, song.doc, &song.vg);
        error.clear();
        expect(!broken.stage(path(monolithic ? "mono_broken" : "labels_broken"), &error) &&
                   error.contains(QStringLiteral("voicegroup101, voice 3 (")) &&
                   error.contains(QStringLiteral("voice 0 of a voicegroup after it")) &&
                   error.contains(QStringLiteral("\n    voice_directsound 60, 0, "
                                                 "DirectSoundWaveData_overflow 255")),
               tag + QStringLiteral(": broken overflow line names its slot, got: ") + error);
    }

    // ---- real project corpus (optional) -------------------------------------------
    const QString corpus = qEnvironmentVariable("PORYDAW_SAMPLE_CORPUS");
    if (!corpus.isEmpty()) {
        DecompProject project;
        QString error;
        expect(project.open(corpus, &error), QStringLiteral("corpus opens: ") + error);
        int playable = 0, exported = 0, skipped = 0;
        for (const SongInfo &info : project.songs()) {
            if (!info.isPlayable() || playable++ % 8 != 0 || exported >= 60)
                continue;
            const QString tag = QStringLiteral("corpus ") + info.label;
            SongDocument doc;
            VoicegroupSource vg;
            if (!doc.load(info, &error)) {
                expect(false, tag + QStringLiteral(": opens: ") + error);
                continue;
            }
            // By-design refusals are skips, not failures: a song whose
            // voicegroup source can't be located (the app's "editing
            // unavailable" state) and a legacy .aif sample tree.
            if (!vg.open(corpus, info.cfg.voicegroupArg, &error)) {
                skipped++;
                continue;
            }
            SongBundle::Exporter exporter(corpus, doc, &vg);
            const QString out = scratchDir + QStringLiteral("/corpus_") + info.label;
            if (!exporter.stage(out, &error)) {
                if (error.contains(QStringLiteral(".aif")))
                    skipped++;
                else
                    expect(false, tag + QStringLiteral(": exports: ") + error);
                continue;
            }
            exported++;
            QList<int> usedSlots = SongBundle::usedPrograms(doc.smf()).values();
            std::sort(usedSlots.begin(), usedSlots.end());
            expectSameVoices(tag, corpus, out, DecompProject::voicegroupCandidates(info).first(),
                             usedSlots);
        }
        expect(exported + skipped > 0, QStringLiteral("corpus had songs to export"));
        std::printf("bundlecheck: corpus exported %d song(s), skipped %d\n", exported, skipped);
    }
}
