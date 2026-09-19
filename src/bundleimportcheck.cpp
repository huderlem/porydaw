#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QUndoStack>
#include <cstdio>
#include <functional>

#include "audio/wavexport.h"
#include "bundlecheckfixtures.h"
#include "mainwindow.h"
#include "project/bundlearchive.h"
#include "project/bundleimport.h"
#include "project/bundlesources.h"
#include "project/decompproject.h"
#include "project/songbundle.h"
#include "ui/bundleimportdialog.h"

// The import sections of --bundlecheck (docs/song-bundle/PLAN.md Phase 3),
// run by runBundleCheck against the bundle the export sections wrote
// (export_a_extracted / export_a.porysong) and their macro fixture project:
//  (a) into an empty project: everything is added, the song registers, and
//      the project's voicegroup loads identically to the bundle's;
//  (b) into the SOURCE project: every sample / wave / synth / table /
//      sub-voicegroup is reused, only the voicegroup and song are added
//      (with _2 suffixes), and the sound data files don't change by a byte;
//  (c) same-named sample / wave / table / sub-voicegroup with different
//      content: renamed _2, and the imported voicegroups reference the
//      renamed symbols;
//  (d) no set_synth_* macros: refusal naming the synth voice;
//  (e) legacy aif2pcm project: refusal;
//  (f) stock engine: a warning lists PORTAMENTO (and none once MPlayDef.s
//      defines it);
//  (g) a failure mid-apply (read-only song_table.inc) names the step and
//      what was written;
//  plus: .set-form and label-style targets, dialog overrides, damaged and
//  hostile bundles, and — in a MainWindow — the imported song opening in an
//  editable tab whose render is sample-exact with the bundle tab's.

namespace {

using SongBundle::ImportAction;
using SongBundle::ImportItem;
using SongBundle::ImportOptions;
using SongBundle::ImportPlan;

int *g_failures = nullptr;

void expect(bool ok, const QString &what)
{
    if (!ok) {
        std::fprintf(stderr, "bundlecheck: FAIL: import: %s\n", qUtf8Printable(what));
        (*g_failures)++;
    }
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile out(path);
    return out.open(QIODevice::WriteOnly | QIODevice::Truncate) && out.write(bytes) == bytes.size();
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QStringList treeFiles(const QString &root)
{
    QStringList files;
    QDirIterator it(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext())
        files << QDir(root).relativeFilePath(it.next());
    files.sort();
    return files;
}

bool copyTree(const QString &from, const QString &to)
{
    QDirIterator it(from, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString src = it.next();
        const QString dst = to + QLatin1Char('/') + QDir(from).relativeFilePath(src);
        QDir().mkpath(QFileInfo(dst).path());
        if (!QFile::copy(src, dst))
            return false;
    }
    return true;
}

const char kWav2AgbRules[] = "$(SOUND_BIN_DIR)/%.bin: sound/%.wav \n\t$(WAV2AGB) -b $< $@\n";
const char kAifRules[] = "%.bin: %.aif\n\t$(AIF2PCM) $< $@\n";
const char kSynthMacros[] = "\t.macro set_synth_custom a, b, c, d\n\t.endm\n"
                            "\t.macro set_synth_25\n\t.endm\n\t.macro set_synth_50\n\t.endm\n";
const char kKeysplitMacro[] = "\t.macro keysplit label:req, starting_note\n\t.endm\n";
const char kDummy[] = "\tvoice_square_1 60, 0, 0, 2, 0, 0, 15, 0\n";

// The registration and build scaffolding every target needs (what a decomp
// project has besides its sound data).
bool addScaffolding(const QString &root, bool synthMacros = true, bool wav2agb = true)
{
    return writeFile(root + QStringLiteral("/Makefile"), "include audio_rules.mk\n") &&
           writeFile(root + QStringLiteral("/audio_rules.mk"),
                     wav2agb ? kWav2AgbRules : kAifRules) &&
           writeFile(root + QStringLiteral("/asm/macros/m4a.inc"),
                     QByteArray(kKeysplitMacro) + (synthMacros ? kSynthMacros : "")) &&
           writeFile(root + QStringLiteral("/data/sound_data.s"),
                     "\t.include \"sound/direct_sound_data.inc\"\n") &&
           writeFile(root + QStringLiteral("/sound/song_table.inc"),
                     "\t.equiv MUSIC_PLAYER_BGM, 0\n\t.equiv MUSIC_PLAYER_SE1, 1\n\n"
                     "\t.align 2\ngSongTable::\n\tsong mus_dummy, MUSIC_PLAYER_BGM, 0\n") &&
           writeFile(root + QStringLiteral("/include/constants/songs.h"),
                     "#ifndef GUARD_CONSTANTS_SONGS_H\n#define GUARD_CONSTANTS_SONGS_H\n\n"
                     "#define MUS_DUMMY 0\n\n#endif\n");
}

// An empty wav2agb project in the macro style.
bool buildEmptyProject(const QString &root)
{
    const QString snd = root + QStringLiteral("/sound/");
    QByteArray dummy = "voice_group dummy\n";
    for (int i = 0; i < 128; i++)
        dummy += kDummy;
    return addScaffolding(root) && writeFile(snd + QStringLiteral("direct_sound_data.inc"), "") &&
           writeFile(snd + QStringLiteral("programmable_wave_data.inc"), "") &&
           writeFile(snd + QStringLiteral("keysplit_tables.inc"),
                     "keysplit other, 36\n\tsplit 0, 60\n\tsplit 1, 108\n") &&
           writeFile(snd + QStringLiteral("voice_groups.inc"),
                     ".include \"sound/voicegroups/dummy.inc\"\n") &&
           writeFile(snd + QStringLiteral("voicegroups/dummy.inc"), dummy) &&
           writeFile(snd + QStringLiteral("songs/midi/midi.cfg"), "");
}

const ImportItem *find(const QList<ImportItem> &items, const QString &bundleSymbol)
{
    for (const ImportItem &item : items) {
        if (item.bundleSymbol == bundleSymbol)
            return &item;
    }
    return nullptr;
}

bool isItem(const QList<ImportItem> &items, const QString &bundleSymbol, ImportAction action,
            const QString &projectSymbol)
{
    const ImportItem *item = find(items, bundleSymbol);
    return item && item->action == action && item->projectSymbol == projectSymbol;
}

QString describePlan(const ImportPlan &plan)
{
    QString out = BundleImportDialog::summaryText(plan);
    if (!plan.refusals.isEmpty())
        out += QStringLiteral("\nREFUSED: ") + plan.refusals.join(QStringLiteral(" | "));
    return out;
}

const QList<int> kUsedSlots = {0, 1, 5, 9, 20, 30, 40};

// The imported voicegroup resolves fully and sounds like the bundle's.
void expectLoadsLikeBundle(const QString &tag, const QString &bundleRoot, const ImportPlan &plan)
{
    const QStringList bundle =
        bundlefixtures::describeSlots(bundleRoot, QStringLiteral("bundle_song"), kUsedSlots);
    const QStringList project =
        bundlefixtures::describeSlots(plan.projectRoot, plan.voicegroup.name, kUsedSlots);
    expect(bundle.size() == kUsedSlots.size() && project.size() == kUsedSlots.size(),
           tag + QStringLiteral(": both voicegroups load"));
    for (int i = 0; i < bundle.size() && i < project.size(); i++) {
        // Keysplit / drumkit slots list all 128 keys, most of them empty by
        // design; there it is the sub-voicegroup and table that must resolve.
        const bool split = kUsedSlots.at(i) == 5 || kUsedSlots.at(i) == 9;
        expect(split ? !project.at(i).contains(QStringLiteral("sub:null")) &&
                           !project.at(i).contains(QStringLiteral("table:null"))
                     : !project.at(i).contains(QStringLiteral("null")),
               QStringLiteral("%1: slot %2 resolves fully: %3")
                   .arg(tag)
                   .arg(kUsedSlots.at(i))
                   .arg(project.at(i).left(200)));
        expect(bundle.at(i) == project.at(i),
               QStringLiteral("%1: slot %2 differs\n  bundle:  %3\n  project: %4")
                   .arg(tag)
                   .arg(kUsedSlots.at(i))
                   .arg(bundle.at(i).left(300), project.at(i).left(300)));
    }
}

const SongInfo *songByLabel(const DecompProject &project, const QString &label)
{
    for (const SongInfo &song : project.songs()) {
        if (song.label == label)
            return &song;
    }
    return nullptr;
}

} // namespace

void runBundleImportSections(const QString &scratchDir, int *failures)
{
    g_failures = failures;
    const auto path = [&](const char *name) {
        return scratchDir + QStringLiteral("/import_") + QLatin1String(name);
    };
    const QString bundle = scratchDir + QStringLiteral("/export_a_extracted");
    const QString sourceProject = scratchDir + QStringLiteral("/export_macro");
    expect(SongBundle::isBundleDir(bundle), QStringLiteral("the Phase 1 bundle is there"));
    const QStringList bundleBefore = treeFiles(bundle);

    { // ---- keysplit table forms ----------------------------------------------------
        using namespace SongBundle::Sources;
        KeysplitTable macro, set;
        expect(parseKeysplitTable(
                   {"keysplit strings, 36", "\tsplit 0, 38 @ low", "", "\tsplit 1, 40"}, &macro) &&
                   macro.macroForm && macro.offset == 36 &&
                   macro.bytes == QByteArray("\0\0\1\1", 4),
               QStringLiteral("macro-form keysplit table parses"));
        expect(parseKeysplitTable(
                   {".set KeySplitTable1, . - 36", "\t.byte 0 @ 36", "\t.byte 0", "\t.byte 1, 1"},
                   &set) &&
                   !set.macroForm && set.sameContent(macro),
               QStringLiteral(".set-form table parses to the same content"));
        KeysplitTable again;
        expect(parseKeysplitTable(renderKeysplitTable(QStringLiteral("keysplit_x"), set, true),
                                  &again) &&
                   again.sameContent(set) &&
                   parseKeysplitTable(renderKeysplitTable(QStringLiteral("Table9"), macro, false),
                                      &again) &&
                   again.sameContent(macro),
               QStringLiteral("rendering either form round-trips"));
        KeysplitTable bad;
        expect(!parseKeysplitTable({"keysplit x, 36", "\t.word 5"}, &bad) &&
                   !parseKeysplitTable({"\tsplit 0, 5"}, &bad),
               QStringLiteral("unknown table forms don't parse"));
    }

    { // ---- (a) + (f): an empty, stock-engine project: everything is added ----------------
        const QString root = path("empty");
        expect(buildEmptyProject(root), QStringLiteral("(a) empty project builds"));
        const QStringList before = treeFiles(root);
        const ImportPlan plan = SongBundle::makeImportPlan(bundle, root);
        expect(treeFiles(root) == before && treeFiles(bundle) == bundleBefore,
               QStringLiteral("(a) planning writes nothing"));
        expect(plan.ok(), QStringLiteral("(a) plan is applicable: ") + describePlan(plan));
        expect(plan.samples.size() == 7 &&
                   ImportPlan::count(plan.samples, ImportAction::Add) == 6 &&
                   isItem(plan.samples, QStringLiteral("DirectSoundWaveData_flute"),
                          ImportAction::Add, QStringLiteral("DirectSoundWaveData_flute")) &&
                   // The registrar's grammar: prefix + lowercase name.
                   isItem(plan.samples, QStringLiteral("Cry_Testmon"), ImportAction::Rename,
                          QStringLiteral("DirectSoundWaveData_cry_testmon")),
               QStringLiteral("(a) samples: 6 added, the cry renamed into the sample grammar: ") +
                   describePlan(plan));
        expect(isItem(plan.waves, QStringLiteral("ProgrammableWaveData_7"), ImportAction::Add,
                      QStringLiteral("ProgrammableWaveData_7")) &&
                   isItem(plan.synths, QStringLiteral("DirectSoundSynth_GoldenSun_80101020"),
                          ImportAction::Add,
                          QStringLiteral("DirectSoundSynth_GoldenSun_80101020")) &&
                   isItem(plan.tables, QStringLiteral("keysplit_strings"), ImportAction::Add,
                          QStringLiteral("keysplit_strings")) &&
                   isItem(plan.subVoicegroups, QStringLiteral("voicegroup_strings_keysplit"),
                          ImportAction::Add, QStringLiteral("voicegroup_strings_keysplit")) &&
                   isItem(plan.subVoicegroups, QStringLiteral("voicegroup_test_drumset"),
                          ImportAction::Add, QStringLiteral("voicegroup_test_drumset")) &&
                   plan.voicegroup.action == ImportAction::Add &&
                   plan.voicegroup.projectSymbol == QStringLiteral("voicegroup_bundle_song"),
               QStringLiteral("(a) wave, synth, table, sub-voicegroups and voicegroup are added"));
        expect(plan.label == QStringLiteral("mus_bundle") &&
                   plan.constant == QStringLiteral("MUS_BUNDLE") &&
                   // The manifest's MUS_PLAYER hint doesn't exist here.
                   plan.player == QStringLiteral("MUSIC_PLAYER_BGM") &&
                   plan.flags.join(QLatin1Char(' ')) ==
                       QStringLiteral("-E -R50 -G_bundle_song -V090"),
               QStringLiteral("(a) song fields: %1 %2 %3 [%4]")
                   .arg(plan.label, plan.constant, plan.player, plan.flags.join(QLatin1Char(' '))));
        // (f) no sound/MPlayDef.s defining PORTAMENTO: a warning, not a gate.
        expect(!plan.warnings.isEmpty() && plan.warnings.first().contains("PORTAMENTO") &&
                   plan.warnings.first().contains("MIDI track 1"),
               QStringLiteral("(f) stock engine: the warning lists PORTAMENTO and its track: ") +
                   plan.warnings.join(QStringLiteral(" | ")));
        expect(plan.warnings.join(QLatin1Char(' ')).contains("cry_testmon.bin"),
               QStringLiteral("(a) the raw cry sample is called out"));

        QString error;
        QStringList written;
        expect(SongBundle::applyImportPlan(plan, &error, &written),
               QStringLiteral("(a) apply succeeds: ") + error);
        expect(treeFiles(bundle) == bundleBefore, QStringLiteral("(a) the bundle is untouched"));
        expectLoadsLikeBundle(QStringLiteral("(a)"), bundle, plan);

        const QString snd = root + QStringLiteral("/sound/");
        expect(readFile(snd + QStringLiteral("direct_sound_samples/flute.wav")) ==
                       readFile(bundle + QStringLiteral("/sound/direct_sound_samples/flute.wav")) &&
                   readFile(snd + QStringLiteral("direct_sound_samples/cry_testmon.bin")) ==
                       readFile(bundle +
                                QStringLiteral("/sound/direct_sound_samples/Cry_Testmon.bin")),
               QStringLiteral("(a) sample files are byte copies"));
        const QByteArray vg = readFile(snd + QStringLiteral("voicegroups/bundle_song.inc"));
        expect(vg.startsWith("voice_group bundle_song\n") &&
                   vg.contains("\tcry DirectSoundWaveData_cry_testmon\n") &&
                   vg.contains("@ organ") && vg.count('\n') == 129,
               QStringLiteral("(a) voicegroup file: header, renamed cry, comments kept"));
        expect(readFile(snd + QStringLiteral("voicegroups/test_drumset.inc"))
                   .startsWith("voice_group test_drumset, 36\n"),
               QStringLiteral("(a) the drumset keeps its starting note"));
        expect(readFile(snd + QStringLiteral("voice_groups.inc"))
                   .endsWith(".include \"sound/voicegroups/strings_keysplit.inc\"\n"
                             ".include \"sound/voicegroups/test_drumset.inc\"\n"
                             ".include \"sound/voicegroups/bundle_song.inc\"\n"),
               QStringLiteral("(a) the hub includes the new voicegroups, subs first"));
        expect(readFile(snd + QStringLiteral("keysplit_tables.inc")) ==
                   "keysplit other, 36\n\tsplit 0, 60\n\tsplit 1, 108\n\n"
                   "keysplit strings, 36\n\tsplit 0, 69\n\tsplit 1, 108\n",
               QStringLiteral("(a) the table is appended in the project's macro form"));
        expect(readFile(snd + QStringLiteral("programmable_wave_data.inc")) ==
                   "\t.align 2\nProgrammableWaveData_7::\n"
                   "\t.incbin \"sound/programmable_wave_samples/7.pcm\"\n",
               QStringLiteral("(a) the wave is registered"));
        expect(
            readFile(snd + QStringLiteral("direct_sound_synth_data.inc"))
                    .contains("DirectSoundSynth_GoldenSun_80101020::\n\tset_synth_custom 0x80") &&
                readFile(root + QStringLiteral("/data/sound_data.s"))
                    .contains("direct_sound_synth_data.inc"),
            QStringLiteral("(a) the synth is defined with the project's macro and included"));
        expect(readFile(snd + QStringLiteral("songs/midi/mus_bundle.mid")) ==
                       readFile(bundle + QStringLiteral("/sound/songs/midi/mus_bundle.mid")) &&
                   readFile(snd + QStringLiteral("songs/midi/midi.cfg"))
                       .contains("mus_bundle.mid: -E -R50 -G_bundle_song -V090"),
               QStringLiteral("(a) .mid and midi.cfg line are written"));
        DecompProject project;
        expect(project.open(root, &error), QStringLiteral("(a) project reopens: ") + error);
        const SongInfo *song = songByLabel(project, QStringLiteral("mus_bundle"));
        expect(song && song->registered && song->isPlayable() &&
                   song->constant == QStringLiteral("MUS_BUNDLE") &&
                   song->player == QStringLiteral("MUSIC_PLAYER_BGM") &&
                   song->cfg.voicegroupArg == QStringLiteral("_bundle_song"),
               QStringLiteral("(a) the song is registered and points at its voicegroup"));
        expect(written.contains(QStringLiteral("voicegroup_bundle_song")) &&
                   written.contains(QStringLiteral("DirectSoundWaveData_flute")),
               QStringLiteral("(a) apply reports what it wrote"));

        // A second import of the same bundle reuses all of it.
        const ImportPlan second = SongBundle::makeImportPlan(bundle, root);
        expect(second.ok() && ImportPlan::count(second.samples, ImportAction::Reuse) == 7 &&
                   ImportPlan::count(second.waves, ImportAction::Reuse) == 1 &&
                   ImportPlan::count(second.synths, ImportAction::Reuse) == 1 &&
                   ImportPlan::count(second.tables, ImportAction::Reuse) == 1 &&
                   ImportPlan::count(second.subVoicegroups, ImportAction::Reuse) == 2 &&
                   isItem(second.samples, QStringLiteral("Cry_Testmon"), ImportAction::Reuse,
                          QStringLiteral("DirectSoundWaveData_cry_testmon")) &&
                   second.voicegroup.projectSymbol == QStringLiteral("voicegroup_bundle_song_2") &&
                   second.label == QStringLiteral("mus_bundle_2") &&
                   second.constant == QStringLiteral("MUS_BUNDLE_2"),
               QStringLiteral("(a) re-import reuses everything: ") + describePlan(second));

        // (f) once the project's MPlayDef.s defines the opcode, no warning.
        writeFile(snd + QStringLiteral("MPlayDef.s"), "\t.equ\tPORTAMENTO, 0xc9\n");
        const ImportPlan extended = SongBundle::makeImportPlan(bundle, root);
        expect(!extended.warnings.join(QLatin1Char(' ')).contains("PORTAMENTO"),
               QStringLiteral("(f) an extended engine gets no extension warning"));
    }

    { // ---- (b) the source project: everything but the voicegroup + song is reused ----------
        const QString root = path("source");
        expect(copyTree(sourceProject, root) && addScaffolding(root),
               QStringLiteral("(b) source project copies"));
        writeFile(root + QStringLiteral("/sound/songs/midi/midi.cfg"),
                  "mus_bundle.mid: -E -R50 -G_bundle_song -V090\n");
        const QString snd = root + QStringLiteral("/sound/");
        const QStringList dataFiles = {
            QStringLiteral("direct_sound_data.inc"), QStringLiteral("direct_sound_synth_data.inc"),
            QStringLiteral("programmable_wave_data.inc"), QStringLiteral("keysplit_tables.inc")};
        QList<QByteArray> dataBefore;
        for (const QString &file : dataFiles)
            dataBefore.append(readFile(snd + file));
        const QStringList filesBefore = treeFiles(root);

        const ImportPlan plan = SongBundle::makeImportPlan(bundle, root);
        expect(plan.ok(), QStringLiteral("(b) plan is applicable: ") + describePlan(plan));
        expect(ImportPlan::count(plan.samples, ImportAction::Reuse) == 7 &&
                   isItem(plan.samples, QStringLiteral("Cry_Testmon"), ImportAction::Reuse,
                          QStringLiteral("Cry_Testmon")) &&
                   ImportPlan::count(plan.waves, ImportAction::Reuse) == 1 &&
                   ImportPlan::count(plan.synths, ImportAction::Reuse) == 1 &&
                   ImportPlan::count(plan.tables, ImportAction::Reuse) == 1 &&
                   ImportPlan::count(plan.subVoicegroups, ImportAction::Reuse) == 2,
               QStringLiteral("(b) everything the project has is reused: ") + describePlan(plan));
        expect(plan.voicegroup.action == ImportAction::Rename &&
                   plan.voicegroup.projectSymbol == QStringLiteral("voicegroup_bundle_song_2") &&
                   plan.label == QStringLiteral("mus_bundle_2") &&
                   plan.constant == QStringLiteral("MUS_BUNDLE_2") &&
                   plan.flags.contains(QStringLiteral("-G_bundle_song_2")),
               QStringLiteral("(b) voicegroup and song take _2 suffixes"));
        QString error;
        expect(SongBundle::applyImportPlan(plan, &error),
               QStringLiteral("(b) apply succeeds: ") + error);
        for (int i = 0; i < dataFiles.size(); i++)
            expect(readFile(snd + dataFiles.at(i)) == dataBefore.at(i),
                   QStringLiteral("(b) %1 doesn't change by a byte").arg(dataFiles.at(i)));
        QStringList added = treeFiles(root);
        for (const QString &file : filesBefore)
            added.removeAll(file);
        expect(added == QStringList({QStringLiteral("sound/songs/midi/mus_bundle_2.mid"),
                                     QStringLiteral("sound/voicegroups/bundle_song_2.inc")}),
               QStringLiteral("(b) only the .mid and the voicegroup are new files, got: ") +
                   added.join(QLatin1Char(' ')));
        expectLoadsLikeBundle(QStringLiteral("(b)"), bundle, plan);
        DecompProject project;
        const bool opened = project.open(root, &error);
        const SongInfo *song =
            opened ? songByLabel(project, QStringLiteral("mus_bundle_2")) : nullptr;
        expect(song && song->registered && song->constant == QStringLiteral("MUS_BUNDLE_2"),
               QStringLiteral("(b) mus_bundle_2 is registered"));

        // Dialog overrides: a free label is taken as given, a clashing one
        // refuses instead of being suffixed.
        ImportOptions options;
        options.label = QStringLiteral("mus_my_cover");
        options.player = QStringLiteral("MUSIC_PLAYER_SE1");
        const ImportPlan custom = SongBundle::makeImportPlan(bundle, root, options);
        expect(custom.ok() && custom.label == QStringLiteral("mus_my_cover") &&
                   custom.constant == QStringLiteral("MUS_MY_COVER") &&
                   custom.player == QStringLiteral("MUSIC_PLAYER_SE1"),
               QStringLiteral("(b) overrides are honoured: ") + describePlan(custom));
        options.label = QStringLiteral("mus_bundle");
        expect(!SongBundle::makeImportPlan(bundle, root, options).ok(),
               QStringLiteral("(b) an overridden label that clashes refuses"));
        options = ImportOptions();
        options.constant = QStringLiteral("MUS_DUMMY");
        expect(!SongBundle::makeImportPlan(bundle, root, options).ok(),
               QStringLiteral("(b) an overridden constant that clashes refuses"));
        options.constant = QStringLiteral("bad name");
        options.player = QStringLiteral("MUSIC_PLAYER_NOPE");
        expect(SongBundle::makeImportPlan(bundle, root, options).refusals.size() == 2,
               QStringLiteral("(b) invalid constant and unknown player both refuse"));
    }

    { // ---- (c) same names, different content: renamed, and referenced renamed --------------
        const QString root = path("clash");
        expect(buildEmptyProject(root), QStringLiteral("(c) project builds"));
        const QString snd = root + QStringLiteral("/sound/");
        writeFile(snd + QStringLiteral("direct_sound_samples/flute.wav"),
                  bundlefixtures::fixtureWav(40));
        writeFile(snd + QStringLiteral("direct_sound_samples/renamed_kick.wav"),
                  readFile(bundle + QStringLiteral("/sound/direct_sound_samples/kick.wav")));
        writeFile(snd + QStringLiteral("direct_sound_data.inc"),
                  "\t.align 2\nDirectSoundWaveData_flute::\n"
                  "\t.incbin \"sound/direct_sound_samples/flute.bin\"\n\n"
                  "\t.align 2\nDirectSoundWaveData_renamed_kick::\n"
                  "\t.incbin \"sound/direct_sound_samples/renamed_kick.bin\"\n");
        writeFile(snd + QStringLiteral("programmable_wave_samples/7.pcm"),
                  QByteArray::fromHex("00112233445566778899aabbccddeeff"));
        writeFile(snd + QStringLiteral("programmable_wave_data.inc"),
                  "    .align 2\nProgrammableWaveData_7::\n"
                  "    .incbin \"sound/programmable_wave_samples/7.pcm\"\n");
        writeFile(snd + QStringLiteral("keysplit_tables.inc"),
                  "keysplit strings, 36\n\tsplit 0, 50\n\tsplit 1, 108\n");
        writeFile(snd + QStringLiteral("voicegroups/strings_keysplit.inc"),
                  QByteArray("voice_group strings_keysplit\n") + kDummy + kDummy);
        writeFile(snd + QStringLiteral("direct_sound_synth_data.inc"),
                  "\t.align 2\nDirectSoundSynth_GoldenSun_80101020::\n"
                  "\tset_synth_custom 0x40, 0x00, 0x00, 0x00\n");

        const ImportPlan plan = SongBundle::makeImportPlan(bundle, root);
        expect(plan.ok(), QStringLiteral("(c) plan is applicable: ") + describePlan(plan));
        expect(isItem(plan.samples, QStringLiteral("DirectSoundWaveData_flute"),
                      ImportAction::Rename, QStringLiteral("DirectSoundWaveData_flute_2")) &&
                   // Same bytes under another name: reused by content.
                   isItem(plan.samples, QStringLiteral("DirectSoundWaveData_kick"),
                          ImportAction::Reuse,
                          QStringLiteral("DirectSoundWaveData_renamed_kick")) &&
                   isItem(plan.waves, QStringLiteral("ProgrammableWaveData_7"),
                          ImportAction::Rename, QStringLiteral("ProgrammableWaveData_7_2")) &&
                   isItem(plan.tables, QStringLiteral("keysplit_strings"), ImportAction::Rename,
                          QStringLiteral("keysplit_strings_2")) &&
                   isItem(plan.synths, QStringLiteral("DirectSoundSynth_GoldenSun_80101020"),
                          ImportAction::Rename,
                          QStringLiteral("DirectSoundSynth_GoldenSun_80101020_2")) &&
                   isItem(plan.subVoicegroups, QStringLiteral("voicegroup_strings_keysplit"),
                          ImportAction::Rename, QStringLiteral("voicegroup_strings_keysplit_2")),
               QStringLiteral("(c) clashes rename, equal content reuses: ") + describePlan(plan));
        QString error;
        expect(SongBundle::applyImportPlan(plan, &error),
               QStringLiteral("(c) apply succeeds: ") + error);
        const QByteArray vg = readFile(snd + QStringLiteral("voicegroups/bundle_song.inc"));
        expect(
            vg.contains("DirectSoundWaveData_flute_2,") &&
                vg.contains("voice_keysplit voicegroup_strings_keysplit_2, keysplit_strings_2") &&
                vg.contains("ProgrammableWaveData_7_2,") &&
                vg.contains("DirectSoundSynth_GoldenSun_80101020_2,"),
            QStringLiteral("(c) the voicegroup references the renamed symbols"));
        expect(readFile(snd + QStringLiteral("voicegroups/test_drumset.inc"))
                   .contains("DirectSoundWaveData_renamed_kick,"),
               QStringLiteral("(c) the drumset references the reused sample's symbol"));
        expect(readFile(snd + QStringLiteral("direct_sound_samples/flute.wav")) ==
                       bundlefixtures::fixtureWav(40) &&
                   readFile(snd + QStringLiteral("programmable_wave_data.inc"))
                       .endsWith("\n    .align 2\nProgrammableWaveData_7_2::\n"
                                 "    .incbin \"sound/programmable_wave_samples/7_2.pcm\"\n"),
               QStringLiteral("(c) the project's own files stay; appended entries match their "
                              "indentation"));
        expectLoadsLikeBundle(QStringLiteral("(c)"), bundle, plan);
    }

    { // ---- (d) no set_synth_* macros: refuse, naming the synth ------------------------------
        const QString root = path("nosynth");
        expect(buildEmptyProject(root) && addScaffolding(root, /*synthMacros=*/false),
               QStringLiteral("(d) project builds"));
        const QStringList before = treeFiles(root);
        const ImportPlan plan = SongBundle::makeImportPlan(bundle, root);
        expect(!plan.ok() &&
                   plan.refusals.join(QLatin1Char(' '))
                       .contains("DirectSoundSynth_GoldenSun_80101020") &&
                   plan.refusals.join(QLatin1Char(' ')).contains("set_synth_"),
               QStringLiteral("(d) refusal names the synth voice: ") + describePlan(plan));
        QString error;
        expect(!SongBundle::applyImportPlan(plan, &error) && treeFiles(root) == before &&
                   error.contains("Nothing was written"),
               QStringLiteral("(d) a refused plan applies nothing"));
    }

    { // ---- (e) legacy aif2pcm project: net-new samples refuse -------------------------------
        const QString root = path("legacy");
        expect(buildEmptyProject(root) &&
                   addScaffolding(root, /*synthMacros=*/true, /*wav2agb=*/false),
               QStringLiteral("(e) project builds"));
        const ImportPlan plan = SongBundle::makeImportPlan(bundle, root);
        expect(!plan.ok() && plan.refusals.join(QLatin1Char(' ')).contains("aif2pcm"),
               QStringLiteral("(e) the registrar's legacy refusal is carried: ") +
                   describePlan(plan));
    }

    { // ---- (g) a failure mid-apply names the step and what was written ---------------------
        const QString root = path("readonly");
        expect(buildEmptyProject(root), QStringLiteral("(g) project builds"));
        const QString table = root + QStringLiteral("/sound/song_table.inc");
        QFile::setPermissions(table, QFileDevice::ReadOwner);
        if (QFileInfo(table).isWritable()) {
            std::printf("bundlecheck: (g) skipped: permissions don't bind this user\n");
        } else {
            const ImportPlan plan = SongBundle::makeImportPlan(bundle, root);
            expect(plan.ok(), QStringLiteral("(g) plan is applicable: ") + describePlan(plan));
            QString error;
            QStringList written;
            expect(!SongBundle::applyImportPlan(plan, &error, &written),
                   QStringLiteral("(g) apply fails on the read-only song table"));
            expect(error.contains("registering mus_bundle") &&
                       error.contains("voicegroup_bundle_song") &&
                       error.contains("sound/songs/midi/mus_bundle.mid") &&
                       written.contains(QStringLiteral("DirectSoundWaveData_flute")),
                   QStringLiteral("(g) the message names the step and what was written: ") + error);
            expect(QFile::exists(root + QStringLiteral("/sound/songs/midi/mus_bundle.mid")) &&
                       !readFile(table).contains("mus_bundle"),
                   QStringLiteral("(g) the .mid is there, the table untouched"));
            expectLoadsLikeBundle(QStringLiteral("(g)"), bundle, plan);
            QFile::setPermissions(table, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        }
    }

    { // ---- a .set/.byte project gets its tables in that form --------------------------------
        const QString root = path("setform");
        expect(buildEmptyProject(root), QStringLiteral("set-form project builds"));
        const QString tables = root + QStringLiteral("/sound/keysplit_tables.inc");
        writeFile(tables, "\r\n.set KeySplitTable1, . - 36\r\n\t.byte 0 @ 36\r\n\t.byte 1\r\n");
        const ImportPlan plan = SongBundle::makeImportPlan(bundle, root);
        QString error;
        expect(plan.ok() &&
                   isItem(plan.tables, QStringLiteral("keysplit_strings"), ImportAction::Add,
                          QStringLiteral("keysplit_strings")) &&
                   SongBundle::applyImportPlan(plan, &error),
               QStringLiteral("set-form import applies: ") + describePlan(plan) + error);
        const QByteArray after = readFile(tables);
        expect(after.contains("\r\n\r\n.set keysplit_strings, . - 36\r\n\t.byte 0 @ 36\r\n") &&
                   after.endsWith("\t.byte 1 @ 107\r\n") && !after.contains("split "),
               QStringLiteral("the table is rendered as .set/.byte with the file's CRLF"));
        expectLoadsLikeBundle(QStringLiteral("set-form"), bundle, plan);
    }

    { // ---- a label-style project: plain labels, the starting note becomes padding -----------
        const QString root = path("labelstyle");
        expect(buildEmptyProject(root), QStringLiteral("label-style project builds"));
        QByteArray dummy = "\t.align 2\nvoicegroup_dummy::\n";
        for (int i = 0; i < 128; i++)
            dummy += kDummy;
        writeFile(root + QStringLiteral("/sound/voicegroups/dummy.inc"), dummy);
        const ImportPlan plan = SongBundle::makeImportPlan(bundle, root);
        QString error;
        expect(plan.ok() && SongBundle::applyImportPlan(plan, &error),
               QStringLiteral("label-style import applies: ") + describePlan(plan) + error);
        const QList<QByteArray> drums =
            readFile(root + QStringLiteral("/sound/voicegroups/test_drumset.inc")).split('\n');
        expect(drums.size() == 2 + 36 + 4 + 1 && drums.at(0) == "\t.align 2" &&
                   drums.at(1) == "voicegroup_test_drumset::" &&
                   drums.at(2 + 35) + '\n' == kDummy && drums.at(2 + 36).contains("_kick"),
               QStringLiteral("the drumset is a plain label with 36 padding voices"));
        // Not slots 5 and 9: a label-style sub-voicegroup shorter than 128
        // voices runs on into whatever the hub includes next (ROM contiguity),
        // so its unmapped keys differ from the bundle's by layout.
        const QList<int> plain = {0, 1, 20, 30, 40};
        const QStringList loaded =
            bundlefixtures::describeSlots(root, QStringLiteral("bundle_song"), plain);
        const QStringList wanted =
            bundlefixtures::describeSlots(bundle, QStringLiteral("bundle_song"), plain);
        expect(loaded.size() == plain.size(), QStringLiteral("label-style voicegroup loads"));
        for (int i = 0; i < loaded.size() && i < wanted.size(); i++)
            expect(loaded.at(i) == wanted.at(i),
                   QStringLiteral("label-style: slot %1 differs\n  bundle:  %2\n  project: %3")
                       .arg(plain.at(i))
                       .arg(wanted.at(i).left(200), loaded.at(i).left(200)));
        // (A resolved sub-voicegroup lists its keys; nested keysplits reached
        // through the overflow legitimately print sub:null.)
        const QStringList splits =
            bundlefixtures::describeSlots(root, QStringLiteral("bundle_song"), {5, 9});
        expect(splits.size() == 2 && splits.at(0).contains(QStringLiteral(" [0 ")) &&
                   splits.at(0).contains(QStringLiteral("table:0")) &&
                   splits.at(1).contains(QStringLiteral(" [36 ")),
               QStringLiteral("label-style: keysplit and drumkit resolve"));
    }

    { // ---- damaged, hostile and unusable inputs ----------------------------------------------
        const QString root = path("refusals");
        expect(buildEmptyProject(root), QStringLiteral("refusal project builds"));
        const QStringList before = treeFiles(root);

        const QString damaged = path("damaged_bundle");
        copyTree(bundle, damaged);
        writeFile(damaged + QStringLiteral("/sound/direct_sound_samples/flute.wav"),
                  bundlefixtures::fixtureWav(77));
        ImportPlan plan = SongBundle::makeImportPlan(damaged, root);
        expect(!plan.ok() && plan.refusals.join(QLatin1Char(' ')).contains("damaged"),
               QStringLiteral("a sample that doesn't match its manifest hash refuses: ") +
                   describePlan(plan));

        const QString hostile = path("hostile_bundle");
        copyTree(bundle, hostile);
        QByteArray inc = readFile(hostile + QStringLiteral("/sound/direct_sound_data.inc"));
        inc.replace("\"sound/direct_sound_samples/flute.bin\"", "\"../../secret.bin\"");
        writeFile(hostile + QStringLiteral("/sound/direct_sound_data.inc"), inc);
        plan = SongBundle::makeImportPlan(hostile, root);
        expect(!plan.ok() && plan.refusals.join(QLatin1Char(' ')).contains("secret.bin"),
               QStringLiteral("planning goes through readSong's path checks"));

        const QString flagged = path("flags_bundle");
        copyTree(bundle, flagged);
        writeFile(flagged + QStringLiteral("/sound/songs/midi/midi.cfg"),
                  "mus_bundle.mid: -E -G_bundle_song -V090;touch${IFS}pwned\n");
        plan = SongBundle::makeImportPlan(flagged, root);
        expect(!plan.ok() && plan.refusals.join(QLatin1Char(' ')).contains("not a plain option"),
               QStringLiteral("flags that aren't plain options refuse: ") + describePlan(plan));

        const QString smuggled = path("smuggled_bundle");
        copyTree(bundle, smuggled);
        QByteArray drums =
            readFile(smuggled + QStringLiteral("/sound/voicegroups/test_drumset.inc"));
        drums.replace("\tvoice_noise 60, 0, 0, 0, 2, 0, 1",
                      "\tvoice_noise 60, 0, 0, 0, 2, 0, 1 ; .incbin \"src/secret.c\"");
        writeFile(smuggled + QStringLiteral("/sound/voicegroups/test_drumset.inc"), drums);
        plan = SongBundle::makeImportPlan(smuggled, root);
        expect(!plan.ok() &&
                   plan.refusals.join(QLatin1Char(' ')).contains("voicegroup_test_drumset"),
               QStringLiteral("a voice line carrying a second directive refuses: ") +
                   describePlan(plan));

        const QString broken = path("broken_bundle");
        copyTree(bundle, broken);
        drums = readFile(broken + QStringLiteral("/sound/voicegroups/test_drumset.inc"));
        drums.replace("\tvoice_noise 60, 0, 0, 0, 2, 0, 1", "\tvoice_noise 60, 0, 0");
        writeFile(broken + QStringLiteral("/sound/voicegroups/test_drumset.inc"), drums);
        plan = SongBundle::makeImportPlan(broken, root);
        expect(!plan.ok() &&
                   plan.refusals.join(QLatin1Char(' ')).contains("voicegroup_test_drumset"),
               QStringLiteral("a voice line whose arguments don't parse refuses: ") +
                   describePlan(plan));

        const QString directive = path("directive_bundle");
        copyTree(bundle, directive);
        QByteArray topVg =
            readFile(directive + QStringLiteral("/sound/voicegroups/bundle_song.inc"));
        topVg += "\t.word 0x12345678\n";
        writeFile(directive + QStringLiteral("/sound/voicegroups/bundle_song.inc"), topVg);
        plan = SongBundle::makeImportPlan(directive, root);
        expect(plan.ok() && !plan.voicegroup.lines.join('\n').contains(".word"),
               QStringLiteral("non-voice directives are never carried into the project"));

        const QString partial = path("partial_bundle");
        copyTree(bundle, partial);
        QFile::remove(partial + QStringLiteral("/sound/voicegroups/test_drumset.inc"));
        QFile::remove(partial + QStringLiteral("/sound/programmable_wave_samples/7.pcm"));
        plan = SongBundle::makeImportPlan(partial, root);
        const QString why = plan.refusals.join(QLatin1Char(' '));
        expect(!plan.ok() && why.contains("incomplete") &&
                   why.contains("voicegroup_test_drumset") &&
                   why.contains("ProgrammableWaveData_7"),
               QStringLiteral("an incomplete bundle names what is missing: ") + why);

        const QString mono = path("monolithic");
        expect(buildEmptyProject(mono) &&
                   QDir(mono + QStringLiteral("/sound/voicegroups")).removeRecursively(),
               QStringLiteral("monolithic project builds"));
        plan = SongBundle::makeImportPlan(bundle, mono);
        expect(!plan.ok() && plan.refusals.join(QLatin1Char(' ')).contains("sound/voicegroups/"),
               QStringLiteral("a project without sound/voicegroups/ refuses"));
        plan = SongBundle::makeImportPlan(bundle, path("no_such_project"));
        expect(!plan.ok(), QStringLiteral("a missing project refuses"));
        expect(treeFiles(root) == before, QStringLiteral("refused plans write nothing"));
    }

    // ---- real project corpus (optional): re-importing a project's own songs ----------
    // The bundles the export sections staged from PORYDAW_SAMPLE_CORPUS, planned
    // (never applied) back into that tree: all of it must be found again.
    const QString corpus = qEnvironmentVariable("PORYDAW_SAMPLE_CORPUS");
    if (!corpus.isEmpty()) {
        const QStringList staged =
            QDir(scratchDir).entryList({QStringLiteral("corpus_*")}, QDir::Dirs, QDir::Name);
        int planned = 0;
        for (int i = 0; i < staged.size(); i += 4) {
            const QString root = scratchDir + QLatin1Char('/') + staged.at(i);
            const ImportPlan plan = SongBundle::makeImportPlan(root, corpus);
            const QString tag = QStringLiteral("corpus %1").arg(staged.at(i));
            expect(plan.ok(), tag + QStringLiteral(": plan is applicable: ") + describePlan(plan));
            expect(ImportPlan::count(plan.samples, ImportAction::Reuse) == plan.samples.size() &&
                       ImportPlan::count(plan.waves, ImportAction::Reuse) == plan.waves.size() &&
                       ImportPlan::count(plan.tables, ImportAction::Reuse) == plan.tables.size() &&
                       ImportPlan::count(plan.subVoicegroups, ImportAction::Reuse) ==
                           plan.subVoicegroups.size(),
                   tag + QStringLiteral(": everything is reused: ") + describePlan(plan));
            expect(plan.label == plan.manifest.label + QStringLiteral("_2") &&
                       plan.voicegroup.action == ImportAction::Rename,
                   tag + QStringLiteral(": the song and voicegroup take new names"));
            planned++;
        }
        std::printf("bundlecheck: corpus planned %d re-import(s)\n", planned);
    }
}

// ---- in a MainWindow: Import button → plan → apply → editable tab ------------------------

namespace {

bool renderSession(const SongDocument &doc, const LoadedVoiceGroup *voicegroup,
                   const SongSettings &settings, const QString &wav, QString *error)
{
    WavExportOptions opts;
    opts.loopCount = 1;
    opts.fadeoutSeconds = 0.5;
    opts.tailSeconds = 0.5;
    const auto timeline = doc.buildTimeline(double(opts.sampleRate));
    return ::exportWav(wav, *timeline, voicegroup, settings, opts, nullptr, error);
}

} // namespace

int MainWindow::runBundleImportTabCheck(const QString &bundleZip, const QString &projectRoot,
                                        const QString &scratchDir)
{
    m_persistSession = false;
    int failures = 0;
    const auto check = [&failures](bool ok, const QString &what) {
        if (!ok) {
            std::fprintf(stderr, "bundlecheck: FAIL: import tab: %s\n", qUtf8Printable(what));
            failures++;
        }
    };
    if (!m_audioOk) {
        check(false, QStringLiteral("no audio device available"));
        return failures;
    }
    QString error;
    check(openBundle(bundleZip, &error), QStringLiteral("the bundle opens: ") + error);
    SongSession *tab = m_active;
    if (!tab || !tab->bundle)
        return failures + 1;

    // Not while no project is open.
    const ImportPlan orphan = SongBundle::makeImportPlan(tab->root, projectRoot);
    check(!applyBundleImport(orphan, &error) && m_tabs->count() == 1,
          QStringLiteral("applyBundleImport refuses without an open project"));

    const QString bundleWav = scratchDir + QStringLiteral("/import_render_bundle.wav");
    check(renderSession(tab->doc, tab->voicegroup, songSettingsFor(*tab), bundleWav, &error),
          QStringLiteral("the bundle tab renders: ") + error);

    check(openProjectDir(projectRoot, /*interactive=*/false) && m_active == tab &&
              tab->bundleImportButton->isEnabled(),
          QStringLiteral("the target project opens next to the bundle tab"));

    // What the dialog would show and hand back.
    QStringList players;
    for (const MusicPlayer &player : m_project.musicPlayers())
        players.append(player.name);
    BundleImportDialog dialog(tab->root, m_project.root(), players, this);
    check(dialog.plan().ok() && dialog.plan().label == QStringLiteral("mus_bundle") &&
              BundleImportDialog::summaryText(dialog.plan()).contains("Samples: 7 new, 0 reused"),
          QStringLiteral("the dialog plans the import: ") +
              BundleImportDialog::summaryText(dialog.plan()));

    // PORYDAW_BUNDLE_IMPORT_SHOT=<png> saves the dialog for visual review.
    const QString shot = qEnvironmentVariable("PORYDAW_BUNDLE_IMPORT_SHOT");
    if (!shot.isEmpty()) {
        dialog.show();
        QApplication::processEvents();
        dialog.grab().save(shot);
        dialog.hide();
    }

    const QStringList bundleFiles = treeFiles(tab->root);
    check(applyBundleImport(dialog.plan(), &error), QStringLiteral("the import applies: ") + error);
    check(treeFiles(tab->root) == bundleFiles, QStringLiteral("the bundle's files are untouched"));
    SongSession *imported = m_active;
    check(m_tabs->count() == 2 && imported && imported != tab && !imported->bundle &&
              !imported->doc.isLocked() && imported->root == m_project.root() &&
              imported->doc.label() == QStringLiteral("mus_bundle") && imported->songId >= 0 &&
              imported->vgSource && m_saveAction->isEnabled(),
          QStringLiteral("the imported song opens in its own editable tab; the bundle tab stays"));
    if (imported && imported != tab) {
        const QString projectWav = scratchDir + QStringLiteral("/import_render_project.wav");
        check(renderSession(imported->doc, imported->voicegroup, songSettingsFor(*imported),
                            projectWav, &error),
              QStringLiteral("the imported song renders: ") + error);
        const QByteArray a = readFile(bundleWav);
        const QByteArray b = readFile(projectWav);
        check(a.size() > 10000 && a == b,
              QStringLiteral("the imported song renders sample-exact with the bundle (%1 vs %2 "
                             "bytes)")
                  .arg(a.size())
                  .arg(b.size()));
        imported->doc.addNote(0, 0, 60, 24, 100);
        check(imported->doc.isDirty(), QStringLiteral("the imported song takes edits"));
        imported->doc.undoStack()->undo();
    }

    // Importing again from the same tab: all reuse, _2 names, a third tab.
    BundleImportDialog again(tab->root, m_project.root(), players, this);
    check(again.plan().ok() && again.plan().label == QStringLiteral("mus_bundle_2") &&
              BundleImportDialog::summaryText(again.plan()).contains("Samples: 0 new, 7 reused"),
          QStringLiteral("a second import reuses what the first one added"));
    check(applyBundleImport(again.plan(), &error) && m_tabs->count() == 3 && m_active &&
              m_active->doc.label() == QStringLiteral("mus_bundle_2"),
          QStringLiteral("the second import opens mus_bundle_2: ") + error);

    while (m_tabs->count() > 0)
        closeTab(m_tabs->count() - 1);
    return failures;
}

int runBundleImportTabSections(const QString &bundleZip, const QString &scratchDir)
{
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "bundlecheck: FAIL: import tab: no temp dir for settings\n");
        return 1;
    }
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());

    const QString projectRoot = scratchDir + QStringLiteral("/import_tab_project");
    if (!buildEmptyProject(projectRoot)) {
        std::fprintf(stderr, "bundlecheck: FAIL: import tab: fixture project\n");
        return 1;
    }
    MainWindow window;
    return window.runBundleImportTabCheck(bundleZip, projectRoot, scratchDir);
}
