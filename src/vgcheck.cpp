#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <cstdio>
#include <cstring>

#include "project/decompproject.h"
#include "project/songregistry.h"
#include "project/voicegroupsource.h"

extern "C" {
#include "voicegroup_loader.h"
}

// --vgcheck <projectRoot> <song>: voicegroup edit/save/create check. Loads the
// song's voicegroup source, edits one voice of each editable family, verifies
// the pre-save preview path (temp file + loader config override), saves, then
// verifies: only the edited lines changed byte-for-byte, a plain reload
// produces the expected ToneData with every untouched slot identical to the
// baseline, and a created voicegroup is discoverable and loadable. Also
// covers synthetic mini-projects: typical-ADSR modes, Golden Sun synths,
// and single-colon (file-local) sample labels in both scan and loader.
// Run this against a scratch copy of a project — it writes into it.

namespace {

QByteArray readFileBytes(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    return f.readAll();
}

// The comparable, pointer-free projection of one loaded voice.
struct VoiceSnap {
    uint8_t type = 0, key = 0, panSweep = 0;
    uint8_t attack = 0, decay = 0, sustain = 0, release = 0;
    QByteArray name;
    uintptr_t packed = 0; // wavePointer's int payload for square/noise voices

    bool operator==(const VoiceSnap &o) const
    {
        return type == o.type && key == o.key && panSweep == o.panSweep && attack == o.attack &&
               decay == o.decay && sustain == o.sustain && release == o.release && name == o.name &&
               packed == o.packed;
    }
};

VoiceSnap snapVoice(const LoadedVoiceGroup *vg, int i)
{
    const ToneData &td = vg->voices[i];
    VoiceSnap s;
    s.type = td.type;
    s.key = td.key;
    s.panSweep = td.panSweep;
    s.attack = td.attack;
    s.decay = td.decay;
    s.sustain = td.sustain;
    s.release = td.release;
    s.name = QByteArray(vg->voiceNames[i]);
    const uint8_t cgb = td.type & VOICE_TYPE_CGB_MASK;
    if (td.type != VOICE_KEYSPLIT && td.type != VOICE_KEYSPLIT_ALL &&
        (cgb == VOICE_SQUARE_1 || cgb == VOICE_SQUARE_2 || cgb == VOICE_NOISE))
        s.packed = reinterpret_cast<uintptr_t>(td.wavePointer);
    return s;
}

// The display name the loader derives from a symbol (vg_set_voice_name):
// known prefixes stripped, truncated to the fixed-size name buffer.
QByteArray loaderVoiceName(const QString &symbol)
{
    QByteArray name = symbol.toUtf8();
    for (const char *prefix : {"DirectSoundWaveData_", "ProgrammableWaveData_", "voicegroup_"}) {
        if (name.startsWith(prefix) && name.size() > int(qstrlen(prefix))) {
            name = name.mid(int(qstrlen(prefix)));
            break;
        }
    }
    return name.left(VG_VOICE_NAME_LEN - 1);
}

bool isDirectSound(VgMacro m)
{
    return vgMacroIsDirectSound(m);
}
bool isSquare1(VgMacro m)
{
    return m == VgMacro::Square1 || m == VgMacro::Square1Alt;
}
bool isNoise(VgMacro m)
{
    return m == VgMacro::Noise || m == VgMacro::NoiseAlt;
}
bool isProgWave(VgMacro m)
{
    return m == VgMacro::ProgWave || m == VgMacro::ProgWaveAlt;
}

// What a loader parse of the edited line must produce, built by running the
// model's own ToneData packing against a blank voice of the right type.
VoiceSnap expectedSnap(const VoicegroupSource &src, int slot, const QByteArray &name)
{
    ToneData td;
    std::memset(&td, 0, sizeof(td));
    td.type = vgMacroVoiceType(src.voiceAt(slot)->macro);
    src.applyScalarsToToneData(slot, &td);
    VoiceSnap s;
    s.type = td.type;
    s.key = td.key;
    s.panSweep = td.panSweep;
    s.attack = td.attack;
    s.decay = td.decay;
    s.sustain = td.sustain;
    s.release = td.release;
    s.name = name;
    s.packed = reinterpret_cast<uintptr_t>(td.wavePointer);
    return s;
}

int checkSlot(const LoadedVoiceGroup *vg, int slot, const VoiceSnap &expected, const char *what)
{
    const VoiceSnap got = snapVoice(vg, slot);
    if (got == expected)
        return 0;
    std::fprintf(stderr,
                 "vgcheck: FAIL: %s slot %d mismatch: "
                 "type %u/%u key %u/%u panSweep %u/%u adsr %u %u %u %u / %u %u %u %u "
                 "packed %lu/%lu name '%s'/'%s' (got/expected)\n",
                 what, slot, got.type, expected.type, got.key, expected.key, got.panSweep,
                 expected.panSweep, got.attack, got.decay, got.sustain, got.release,
                 expected.attack, expected.decay, expected.sustain, expected.release,
                 ulong(got.packed), ulong(expected.packed), got.name.constData(),
                 expected.name.constData());
    return 1;
}

} // namespace

int runVgCheck(const QString &projectRoot, const QString &songLabel)
{
    DecompProject project;
    QString error;
    if (!project.open(projectRoot, &error)) {
        std::fprintf(stderr, "vgcheck: %s\n", qUtf8Printable(error));
        return 1;
    }
    const SongInfo *song = nullptr;
    for (const SongInfo &s : project.songs()) {
        if (s.label == songLabel && s.isPlayable())
            song = &s;
    }
    if (!song) {
        std::fprintf(stderr, "vgcheck: song '%s' not found\n", qUtf8Printable(songLabel));
        return 1;
    }

    VoicegroupSource src;
    if (!src.open(projectRoot, song->cfg.voicegroupArg, &error)) {
        std::fprintf(stderr, "vgcheck: locate: %s\n", qUtf8Printable(error));
        return 1;
    }
    std::printf("vgcheck: %s layout, file %s, load name %s\n",
                src.monolithic() ? "monolithic" : "per-file", qUtf8Printable(src.filePath()),
                qUtf8Printable(src.loadName()));
    const QByteArray fileBefore = readFileBytes(src.filePath());

    const QByteArray rootUtf8 = projectRoot.toLocal8Bit();
    const QByteArray loadName = src.loadName().toLocal8Bit();
    LoadedVoiceGroup *baseline =
        voicegroup_load(rootUtf8.constData(), loadName.constData(), nullptr);
    if (!baseline) {
        std::fprintf(stderr, "vgcheck: baseline voicegroup_load failed for '%s'\n",
                     loadName.constData());
        return 1;
    }
    VoiceSnap baseSnaps[VOICEGROUP_SIZE];
    for (int i = 0; i < VOICEGROUP_SIZE; i++)
        baseSnaps[i] = snapVoice(baseline, i);

    // ---- pick one slot per editable family ----
    int dsSlot = -1, sq1Slot = -1, noiseSlot = -1, pwSlot = -1;
    for (int i = 0; i < VOICEGROUP_SIZE; i++) {
        const VgVoice *v = src.voiceAt(i);
        if (!v)
            continue;
        if (dsSlot < 0 && isDirectSound(v->macro))
            dsSlot = i;
        else if (sq1Slot < 0 && isSquare1(v->macro))
            sq1Slot = i;
        else if (noiseSlot < 0 && isNoise(v->macro))
            noiseSlot = i;
        else if (pwSlot < 0 && isProgWave(v->macro))
            pwSlot = i;
    }
    if (dsSlot < 0 && sq1Slot < 0 && noiseSlot < 0 && pwSlot < 0) {
        std::fprintf(stderr, "vgcheck: no editable voices in %s\n", qUtf8Printable(src.filePath()));
        voicegroup_free(baseline);
        return 1;
    }

    // ---- edits ----
    int editedLines = 0;
    QVector<int> editedSlots;
    QVector<VoiceSnap> expected;

    if (dsSlot >= 0) {
        VgVoice v = *src.voiceAt(dsSlot);
        v.key = 61;
        v.attack = 200;
        v.decay = 100;
        v.sustain = 50;
        v.release = 25;
        // Swap to another DirectSound voice's sample so the new symbol is
        // guaranteed loadable; its baseline name is the expected new name.
        QByteArray expectedName = baseSnaps[dsSlot].name;
        for (int i = 0; i < VOICEGROUP_SIZE; i++) {
            const VgVoice *o = src.voiceAt(i);
            if (i != dsSlot && o && isDirectSound(o->macro) &&
                o->symbol != src.voiceAt(dsSlot)->symbol) {
                v.symbol = o->symbol;
                expectedName = baseSnaps[i].name;
                break;
            }
        }
        src.setVoice(dsSlot, v);
        editedLines++;
        editedSlots.append(dsSlot);
        expected.append(expectedSnap(src, dsSlot, expectedName));
        std::printf("vgcheck: edited DirectSound slot %d (key/ADSR/sample)\n", dsSlot);
    }
    if (sq1Slot >= 0) {
        VgVoice v = *src.voiceAt(sq1Slot);
        v.duty = 3;
        v.sustain = 15;
        v.sweep = 7;
        src.setVoice(sq1Slot, v);
        editedLines++;
        editedSlots.append(sq1Slot);
        expected.append(expectedSnap(src, sq1Slot, baseSnaps[sq1Slot].name));
        std::printf("vgcheck: edited Square 1 slot %d (duty/sustain/sweep)\n", sq1Slot);
    }
    if (noiseSlot >= 0) {
        VgVoice v = *src.voiceAt(noiseSlot);
        v.period = 1 - (v.period & 1);
        src.setVoice(noiseSlot, v);
        editedLines++;
        editedSlots.append(noiseSlot);
        expected.append(expectedSnap(src, noiseSlot, baseSnaps[noiseSlot].name));
        std::printf("vgcheck: edited Noise slot %d (period)\n", noiseSlot);
    }
    if (pwSlot >= 0) {
        VgVoice v = *src.voiceAt(pwSlot);
        v.release = (v.release & 7) == 5 ? 6 : 5;
        src.setVoice(pwSlot, v);
        editedLines++;
        editedSlots.append(pwSlot);
        expected.append(expectedSnap(src, pwSlot, baseSnaps[pwSlot].name));
        std::printf("vgcheck: edited Wave slot %d (release)\n", pwSlot);
    }
    // Keysplit swap: point one keysplit voice at another's sub-vg/table pair.
    int ksSlot = -1, ksDonor = -1;
    for (int i = 0; i < VOICEGROUP_SIZE; i++) {
        const VgVoice *v = src.voiceAt(i);
        if (!v || v->macro != VgMacro::Keysplit)
            continue;
        if (ksSlot < 0)
            ksSlot = i;
        else if (ksDonor < 0 && v->symbol != src.voiceAt(ksSlot)->symbol)
            ksDonor = i;
    }
    if (ksSlot >= 0 && ksDonor >= 0) {
        VgVoice v = *src.voiceAt(ksSlot);
        v.symbol = src.voiceAt(ksDonor)->symbol;
        v.keysplitTable = src.voiceAt(ksDonor)->keysplitTable;
        src.setVoice(ksSlot, v);
        editedLines++;
        editedSlots.append(ksSlot);
        expected.append(expectedSnap(src, ksSlot, baseSnaps[ksDonor].name));
        std::printf("vgcheck: edited Keysplit slot %d (sub-vg/table from slot %d)\n", ksSlot,
                    ksDonor);
    }
    // Drumkit swap: point one keysplit_all voice at a different observed
    // drumkit sub-voicegroup, and convert a spare basic voice into a drumkit.
    const QStringList drumkits = VoicegroupSource::drumkitInstruments(projectRoot);
    int dkSlot = -1;
    QString dkDonor;
    for (int i = 0; i < VOICEGROUP_SIZE && dkSlot < 0; i++) {
        const VgVoice *v = src.voiceAt(i);
        if (v && v->macro == VgMacro::KeysplitAll)
            dkSlot = i;
    }
    if (dkSlot >= 0) {
        for (const QString &symbol : drumkits) {
            if (symbol != src.voiceAt(dkSlot)->symbol) {
                dkDonor = symbol;
                break;
            }
        }
    }
    if (dkSlot >= 0 && !dkDonor.isEmpty()) {
        VgVoice v = *src.voiceAt(dkSlot);
        v.symbol = dkDonor;
        src.setVoice(dkSlot, v);
        editedLines++;
        editedSlots.append(dkSlot);
        expected.append(expectedSnap(src, dkSlot, loaderVoiceName(dkDonor)));
        std::printf("vgcheck: edited Drumkit slot %d (sub-vg %s)\n", dkSlot,
                    qUtf8Printable(dkDonor));
    }
    int dkConvSlot = -1;
    if (!drumkits.isEmpty()) {
        for (int i = 0; i < VOICEGROUP_SIZE && dkConvSlot < 0; i++) {
            const VgVoice *v = src.voiceAt(i);
            if (v && !editedSlots.contains(i) && v->macro != VgMacro::Keysplit &&
                v->macro != VgMacro::KeysplitAll)
                dkConvSlot = i;
        }
    }
    if (dkConvSlot >= 0) {
        VgVoice v; // fresh-parse form of a drumkit line: defaults + symbol
        v.macro = VgMacro::KeysplitAll;
        v.symbol = drumkits.first();
        src.setVoice(dkConvSlot, v);
        editedLines++;
        editedSlots.append(dkConvSlot);
        expected.append(expectedSnap(src, dkConvSlot, loaderVoiceName(drumkits.first())));
        std::printf("vgcheck: converted slot %d to Drumkit (%s)\n", dkConvSlot,
                    qUtf8Printable(drumkits.first()));
    }
    if (!src.dirty()) {
        std::fprintf(stderr, "vgcheck: FAIL: source not dirty after edits\n");
        voicegroup_free(baseline);
        return 1;
    }

    int failures = 0;

    // ---- pre-save preview: temp file shadows the real one via config ----
    const QString previewDir = projectRoot + QStringLiteral("/.porydaw/vgpreview");
    QDir().mkpath(previewDir);
    {
        QFile out(previewDir + QStringLiteral("/") + src.loadName() + QStringLiteral(".inc"));
        if (!out.open(QIODevice::WriteOnly)) {
            std::fprintf(stderr, "vgcheck: cannot write preview file\n");
            voicegroup_free(baseline);
            return 1;
        }
        out.write(src.renderPreview());
    }
    VoicegroupLoaderConfig cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    std::strncpy(cfg.voicegroupPaths[0], ".porydaw/vgpreview", VG_MAX_PATH_LEN - 1);
    cfg.voicegroupPathCount = 1;
    LoadedVoiceGroup *preview = voicegroup_load(rootUtf8.constData(), loadName.constData(), &cfg);
    if (!preview) {
        std::fprintf(stderr, "vgcheck: FAIL: preview voicegroup_load failed\n");
        failures++;
    } else {
        for (int e = 0; e < editedSlots.size(); e++)
            failures += checkSlot(preview, editedSlots.at(e), expected.at(e), "preview");
        voicegroup_free(preview);
    }
    if (readFileBytes(src.filePath()) != fileBefore) {
        std::fprintf(stderr, "vgcheck: FAIL: project file changed before save\n");
        failures++;
    } else {
        std::printf("vgcheck: preview load OK, project file untouched\n");
    }
    QDir(previewDir).removeRecursively();

    // ---- save: only the edited lines may differ ----
    if (!src.save(&error)) {
        std::fprintf(stderr, "vgcheck: save: %s\n", qUtf8Printable(error));
        voicegroup_free(baseline);
        return 1;
    }
    if (src.dirty()) {
        std::fprintf(stderr, "vgcheck: FAIL: still dirty after save\n");
        failures++;
    }
    const QByteArray fileAfter = readFileBytes(src.filePath());
    {
        const QList<QByteArray> before = fileBefore.split('\n');
        const QList<QByteArray> after = fileAfter.split('\n');
        if (before.size() != after.size()) {
            std::fprintf(stderr, "vgcheck: FAIL: line count changed on save (%d -> %d)\n",
                         int(before.size()), int(after.size()));
            failures++;
        } else {
            int changed = 0;
            for (int i = 0; i < before.size(); i++) {
                if (before.at(i) != after.at(i))
                    changed++;
            }
            if (changed != editedLines) {
                std::fprintf(stderr, "vgcheck: FAIL: %d lines changed on save, expected %d\n",
                             changed, editedLines);
                failures++;
            } else {
                std::printf("vgcheck: save OK, exactly %d line(s) changed\n", changed);
            }
        }
    }

    // ---- plain reload: edited slots as expected, every other slot identical ----
    LoadedVoiceGroup *reloaded =
        voicegroup_load(rootUtf8.constData(), loadName.constData(), nullptr);
    if (!reloaded) {
        std::fprintf(stderr, "vgcheck: FAIL: reload voicegroup_load failed\n");
        failures++;
    } else {
        for (int e = 0; e < editedSlots.size(); e++)
            failures += checkSlot(reloaded, editedSlots.at(e), expected.at(e), "reload");
        if (ksSlot >= 0 && ksDonor >= 0 && !reloaded->voices[ksSlot].subGroup) {
            std::fprintf(stderr, "vgcheck: FAIL: swapped keysplit slot %d has no sub-voicegroup\n",
                         ksSlot);
            failures++;
        }
        for (int slot : {dkSlot >= 0 && !dkDonor.isEmpty() ? dkSlot : -1, dkConvSlot}) {
            if (slot >= 0 && !reloaded->voices[slot].subGroup) {
                std::fprintf(stderr, "vgcheck: FAIL: drumkit slot %d has no sub-voicegroup\n",
                             slot);
                failures++;
            }
        }
        int preserved = 0;
        for (int i = 0; i < VOICEGROUP_SIZE; i++) {
            if (editedSlots.contains(i))
                continue;
            if (snapVoice(reloaded, i) == baseSnaps[i]) {
                preserved++;
            } else {
                std::fprintf(stderr, "vgcheck: FAIL: untouched slot %d changed\n", i);
                failures++;
            }
        }
        std::printf("vgcheck: reload OK, %d untouched slots preserved\n", preserved);
        voicegroup_free(reloaded);
    }

    // ---- text round trip through a fresh parse ----
    VoicegroupSource src2;
    if (!src2.open(projectRoot, song->cfg.voicegroupArg, &error)) {
        std::fprintf(stderr, "vgcheck: FAIL: reopen: %s\n", qUtf8Printable(error));
        failures++;
    } else {
        if (src2.dirty()) {
            std::fprintf(stderr, "vgcheck: FAIL: fresh parse reports dirty\n");
            failures++;
        }
        for (int slot : editedSlots) {
            const VgVoice *a = src.voiceAt(slot);
            const VgVoice *b = src2.voiceAt(slot);
            if (!a || !b || a->macro != b->macro || a->key != b->key || a->pan != b->pan ||
                a->symbol != b->symbol || a->sweep != b->sweep || a->duty != b->duty ||
                a->period != b->period || a->attack != b->attack || a->decay != b->decay ||
                a->sustain != b->sustain || a->release != b->release) {
                std::fprintf(stderr, "vgcheck: FAIL: slot %d fields did not round-trip\n", slot);
                failures++;
            }
        }
    }

    // ---- create a new voicegroup as a copy of the edited one ----
    if (QDir(projectRoot + QStringLiteral("/sound/voicegroups")).exists()) {
        const QString hubPath = projectRoot + QStringLiteral("/sound/voice_groups.inc");
        const QByteArray hubBefore = readFileBytes(hubPath);
        if (!VoicegroupSource::createVoicegroup(projectRoot, QStringLiteral("vgcheck_new"),
                                                src.filePath(), src.sectionLabel(), &error) ||
            !VoicegroupSource::appendIncludeLine(projectRoot, QStringLiteral("vgcheck_new"),
                                                 &error)) {
            std::fprintf(stderr, "vgcheck: FAIL: create: %s\n", qUtf8Printable(error));
            failures++;
        } else {
            LoadedVoiceGroup *created =
                voicegroup_load(rootUtf8.constData(), "vgcheck_new", nullptr);
            if (!created) {
                std::fprintf(stderr, "vgcheck: FAIL: created voicegroup did not load\n");
                failures++;
            } else {
                for (int e = 0; e < editedSlots.size(); e++)
                    failures += checkSlot(created, editedSlots.at(e), expected.at(e), "created");
                voicegroup_free(created);
            }
            if (!SongRegistry::voicegroupArgs(projectRoot)
                     .contains(QStringLiteral("_vgcheck_new"))) {
                std::fprintf(stderr, "vgcheck: FAIL: _vgcheck_new missing from voicegroupArgs\n");
                failures++;
            }
            if (QFile::exists(hubPath)) {
                const QList<QByteArray> before = hubBefore.split('\n');
                const QList<QByteArray> after = readFileBytes(hubPath).split('\n');
                if (after.size() != before.size() + 1) {
                    std::fprintf(stderr,
                                 "vgcheck: FAIL: voice_groups.inc grew by %d lines, "
                                 "expected 1\n",
                                 int(after.size() - before.size()));
                    failures++;
                }
            }
            if (failures == 0)
                std::printf("vgcheck: create OK (sound/voicegroups/vgcheck_new.inc)\n");
        }
    } else {
        std::printf("vgcheck: skipping create check (no sound/voicegroups/ dir)\n");
    }

    // ---- typical-ADSR scan: deterministic modes from a synthetic mini-project ----
    {
        const int before = failures;
        const QString fakeRoot = projectRoot + QStringLiteral("/.porydaw/adsrcheck");
        QDir().mkpath(fakeRoot + QStringLiteral("/sound/voicegroups"));
        QFile out(fakeRoot + QStringLiteral("/sound/voicegroups/adsrcheck.inc"));
        if (!out.open(QIODevice::WriteOnly)) {
            std::fprintf(stderr, "vgcheck: cannot write adsrcheck voicegroup\n");
            failures++;
        } else {
            out.write("voicegroup_adsrcheck::\n"
                      "\tvoice_square_1 60, 0, 0, 2, 0, 0, 15, 0 @ filler: release 0\n"
                      "\tvoice_square_1 60, 0, 0, 2, 0, 0, 15, 0\n"
                      "\tvoice_square_1 60, 0, 0, 2, 1, 2, 10, 3\n"
                      "\tvoice_square_1 60, 0, 0, 2, 1, 2, 10, 3\n"
                      "\tvoice_square_1 60, 0, 0, 2, 2, 0, 12, 1\n"
                      "\tvoice_square_2 60, 0, 2, 9, 2, 20, 11 @ masks to 1 2 4 3\n"
                      "\tvoice_directsound 60, 0, AdsrCheckA, 255, 0, 255, 165\n"
                      "\tvoice_directsound_no_resample 60, 0, AdsrCheckA, 255, 0, 255, 165\n"
                      "\tvoice_directsound 60, 0, AdsrCheckA, 200, 100, 128, 216\n"
                      "\tvoice_directsound 60, 0, AdsrCheckB, 0, 0, 255, 165 @ silent: attack 0\n"
                      "\tvoice_noise 60, 0, 0, 1, 0, 13, 2\n");
            out.close();
            const VgAdsrDefaults defaults = VoicegroupSource::typicalAdsr(fakeRoot);
            const auto expectAdsr = [&failures](const char *what, bool present, const VgAdsr &got,
                                                int a, int d, int s, int r) {
                if (!present || got.attack != a || got.decay != d || got.sustain != s ||
                    got.release != r) {
                    std::fprintf(stderr,
                                 "vgcheck: FAIL: typicalAdsr %s = %d %d %d %d "
                                 "(present %d), expected %d %d %d %d\n",
                                 what, got.attack, got.decay, got.sustain, got.release,
                                 int(present), a, d, s, r);
                    failures++;
                }
            };
            const int sq1 = vgAdsrFamily(VgMacro::Square1);
            const int sq2 = vgAdsrFamily(VgMacro::Square2Alt); // variants collapse
            const int ds = vgAdsrFamily(VgMacro::DirectSoundNoResample);
            const int noise = vgAdsrFamily(VgMacro::Noise);
            expectAdsr("Square1 family", defaults.byFamily.contains(sq1),
                       defaults.byFamily.value(sq1), 1, 2, 10, 3);
            expectAdsr("Square2 family", defaults.byFamily.contains(sq2),
                       defaults.byFamily.value(sq2), 1, 2, 4, 3);
            expectAdsr("DirectSound family", defaults.byFamily.contains(ds),
                       defaults.byFamily.value(ds), 255, 0, 255, 165);
            expectAdsr("Noise family", defaults.byFamily.contains(noise),
                       defaults.byFamily.value(noise), 1, 0, 13, 2);
            expectAdsr("symbol AdsrCheckA",
                       defaults.bySymbol.contains(QStringLiteral("AdsrCheckA")),
                       defaults.bySymbol.value(QStringLiteral("AdsrCheckA")), 255, 0, 255, 165);
            if (defaults.bySymbol.contains(QStringLiteral("AdsrCheckB"))) {
                std::fprintf(stderr, "vgcheck: FAIL: silent AdsrCheckB envelope not excluded\n");
                failures++;
            }
            // Adoption order: symbol beats family beats the built-in fallback.
            expectAdsr("adopt via symbol", true,
                       vgDefaultAdsr(defaults, VgMacro::DirectSound, QStringLiteral("AdsrCheckA")),
                       255, 0, 255, 165);
            expectAdsr("adopt via family", true,
                       vgDefaultAdsr(defaults, VgMacro::Square1Alt, QString()), 1, 2, 10, 3);
            expectAdsr("adopt CGB fallback", true,
                       vgDefaultAdsr(defaults, VgMacro::ProgWave, QStringLiteral("NoSuchWave")), 0,
                       0, 15, 3);
            expectAdsr("adopt DirectSound fallback", true,
                       vgDefaultAdsr(VgAdsrDefaults(), VgMacro::DirectSound, QString()), 255, 0,
                       255, 165);
            if (failures == before)
                std::printf("vgcheck: typical-ADSR synthetic scan OK\n");
        }
        QDir(fakeRoot).removeRecursively();
    }

    // ---- typical-ADSR scan over the real project: every suggestion audible ----
    {
        const int before = failures;
        const VgAdsrDefaults defaults = VoicegroupSource::typicalAdsr(projectRoot);
        for (auto it = defaults.byFamily.constBegin(); it != defaults.byFamily.constEnd(); ++it) {
            const bool cgb = it.key() != vgAdsrFamily(VgMacro::DirectSound);
            const VgAdsr &a = it.value();
            const int maxAdr = cgb ? 7 : 255;
            if (a.release <= 0 || a.release > maxAdr || a.attack < 0 || a.attack > maxAdr ||
                a.decay < 0 || a.decay > maxAdr || a.sustain < 0 || a.sustain > (cgb ? 15 : 255) ||
                (!cgb && a.attack == 0)) {
                std::fprintf(stderr,
                             "vgcheck: FAIL: project typical ADSR for %s unusable: "
                             "%d %d %d %d\n",
                             qUtf8Printable(vgMacroDisplayName(VgMacro(it.key()))), a.attack,
                             a.decay, a.sustain, a.release);
                failures++;
            } else {
                std::printf("vgcheck: typical %s ADSR: %d %d %d %d\n",
                            qUtf8Printable(vgMacroDisplayName(VgMacro(it.key()))), a.attack,
                            a.decay, a.sustain, a.release);
            }
        }
        for (auto it = defaults.bySymbol.constBegin(); it != defaults.bySymbol.constEnd(); ++it) {
            const VgAdsr &a = it.value();
            if (a.release <= 0 || a.release > 255 || a.attack < 0 || a.attack > 255 ||
                a.decay < 0 || a.decay > 255 || a.sustain < 0 || a.sustain > 255) {
                std::fprintf(stderr,
                             "vgcheck: FAIL: project typical ADSR for symbol %s "
                             "unusable: %d %d %d %d\n",
                             qUtf8Printable(it.key()), a.attack, a.decay, a.sustain, a.release);
                failures++;
            }
        }
        if (failures == before)
            std::printf("vgcheck: project typical-ADSR scan OK (%d symbols)\n",
                        int(defaults.bySymbol.size()));
    }

    // ---- Golden Sun synths: scan, dedupe, and definition writing ----
    {
        const int before = failures;
        const QString fakeRoot = projectRoot + QStringLiteral("/.porydaw/synthcheck");
        QDir().mkpath(fakeRoot + QStringLiteral("/sound"));
        QDir().mkpath(fakeRoot + QStringLiteral("/asm/macros"));
        const auto writeFile = [](const QString &path, const QByteArray &bytes) {
            QFile out(path);
            return out.open(QIODevice::WriteOnly) && out.write(bytes) == bytes.size();
        };
        const auto expectSynth = [&failures](const char *what, bool ok) {
            if (!ok) {
                std::fprintf(stderr, "vgcheck: FAIL: synth %s\n", what);
                failures++;
            }
        };
        bool wrote = writeFile(fakeRoot + QStringLiteral("/asm/macros/music_voice.inc"),
                               "\t.macro set_synth_pulse base_duty=0x80, duty_step=0x00, "
                               "mod_depth=0x00, duty_phase=0x00\n\t.endm\n"
                               "\t.macro set_synth_saw\n\t.endm\n"
                               "\t.macro set_synth_triangle\n\t.endm\n"
                               "\t.macro set_synth_custom parameter1:req, parameter2:req, "
                               "parameter3:req, parameter4:req\n\t.endm\n");
        // An inline definition in direct_sound_data.inc (Golden Sun layout)
        // next to a normal sample the symbol list must keep.
        wrote = wrote && writeFile(fakeRoot + QStringLiteral("/sound/direct_sound_data.inc"),
                                   "\t.align 2\n"
                                   "DirectSoundWaveData_synthcheck_sample::\n"
                                   "\t.incbin \"sound/direct_sound_samples/sample.bin\"\n"
                                   "\n"
                                   "\t.align 2\n"
                                   "SynthCheckInline:: @ Golden Sun pulse\n"
                                   "\tset_synth_custom 0x10, 0xF0, 0xE0, 0x80\n");
        // A CRLF synth data file: appended entries must match its endings.
        wrote = wrote && writeFile(fakeRoot + QStringLiteral("/sound/direct_sound_synth_data.inc"),
                                   "\t.align 2\r\n"
                                   "SynthCheckSaw::\r\n"
                                   "    set_synth_25\r\n");
        // The build hub: the writer must wire the synth data file into the
        // assembly (an .include next to direct_sound_data.inc's) or the ROM
        // build fails on the saved symbols while porydaw plays them fine.
        QDir().mkpath(fakeRoot + QStringLiteral("/data"));
        const QString soundDataPath = fakeRoot + QStringLiteral("/data/sound_data.s");
        wrote = wrote && writeFile(soundDataPath, "\t.section .rodata\n"
                                                  "\t.include \"sound/direct_sound_data.inc\"\n"
                                                  "\t.include \"sound/music_player_table.inc\"\n");
        if (!wrote) {
            std::fprintf(stderr, "vgcheck: cannot write synthcheck mini-project\n");
            failures++;
        } else {
            const VgSynthCatalog catalog = VoicegroupSource::synthInstruments(fakeRoot);
            expectSynth("scan finds both definitions", catalog.defs.size() == 2);
            const VgSynthDesc *inl = catalog.find(QStringLiteral("SynthCheckInline"));
            expectSynth("inline pulse parsed",
                        inl && *inl == VgSynthDesc{0, 0x10, 0xF0, 0xE0, 0x80});
            const VgSynthDesc *saw = catalog.find(QStringLiteral("SynthCheckSaw"));
            expectSynth("set_synth_25 alias parsed as saw", saw && saw->waveform == 1);
            expectSynth("macro words discovered",
                        catalog.macroWords.size() == 4 && catalog.creatable());

            const QStringList samples = VoicegroupSource::directSoundSymbols(fakeRoot);
            expectSynth("sample list keeps the PCM sample",
                        samples.contains(QStringLiteral("DirectSoundWaveData_synthcheck_sample")));
            expectSynth("sample list excludes synth definitions",
                        !samples.contains(QStringLiteral("SynthCheckInline")) &&
                            !samples.contains(QStringLiteral("SynthCheckSaw")));

            // Canonical param-derived symbol names.
            const VgSynthDesc fresh{0, 0x40, 2, 3, 4};
            expectSynth("pulse symbol name",
                        vgSynthSymbolName(fresh) ==
                            QStringLiteral("DirectSoundSynth_GoldenSun_40020304"));
            expectSynth("saw symbol name", vgSynthSymbolName(VgSynthDesc{1, 0, 0, 0, 0}) ==
                                               QStringLiteral("DirectSoundSynth_GoldenSun_Saw"));
            expectSynth("triangle symbol name",
                        vgSynthSymbolName(VgSynthDesc{2, 0, 0, 0, 0}) ==
                            QStringLiteral("DirectSoundSynth_GoldenSun_Triangle"));

            // Value-equal descriptors resolve to existing symbols, whichever
            // file defines them (this is the commit-time dedupe).
            expectSynth("dedupe reuses the inline pulse",
                        catalog.symbolFor(VgSynthDesc{0, 0x10, 0xF0, 0xE0, 0x80}) ==
                            QStringLiteral("SynthCheckInline"));
            expectSynth("dedupe reuses the saw", catalog.symbolFor(VgSynthDesc{1, 0, 0, 0, 0}) ==
                                                     QStringLiteral("SynthCheckSaw"));

            // The save-time writer: appends new definitions, skips ones
            // already on disk with equal bytes, refuses conflicting bytes.
            const QString synthPath =
                fakeRoot + QStringLiteral("/sound/direct_sound_synth_data.inc");
            const QList<QPair<QString, VgSynthDesc>> newDefs = {
                {vgSynthSymbolName(fresh), fresh},
                {vgSynthSymbolName(VgSynthDesc{2, 0, 0, 0, 0}), VgSynthDesc{2, 0, 0, 0, 0}},
            };
            expectSynth("write appends new definitions",
                        VoicegroupSource::writeSynthDefinitions(fakeRoot, newDefs, &error));
            // The wiring: sound_data.s gains the synth include, right after
            // the direct_sound_data.inc line it anchors on.
            const QByteArray soundData = readFileBytes(soundDataPath);
            expectSynth("write wires the synth data file into the build",
                        soundData.contains("\t.include \"sound/direct_sound_data.inc\"\n"
                                           "\t.include "
                                           "\"sound/direct_sound_synth_data.inc\"\n"));
            const VgSynthCatalog written = VoicegroupSource::synthInstruments(fakeRoot);
            const VgSynthDesc *minted =
                written.find(QStringLiteral("DirectSoundSynth_GoldenSun_40020304"));
            expectSynth("written pulse parses back", minted && *minted == fresh);
            expectSynth("written triangle parses back",
                        written.find(QStringLiteral("DirectSoundSynth_GoldenSun_Triangle")) !=
                            nullptr);
            const QByteArray grown = readFileBytes(synthPath);
            const QByteArray wired = readFileBytes(soundDataPath);
            expectSynth("re-writing the same definitions is a no-op",
                        VoicegroupSource::writeSynthDefinitions(fakeRoot, newDefs, &error) &&
                            readFileBytes(synthPath) == grown &&
                            readFileBytes(soundDataPath) == wired);
            expectSynth("the include is wired exactly once",
                        wired.count("direct_sound_synth_data.inc") == 1);
            error.clear();
            expectSynth("conflicting bytes under an existing name refused",
                        !VoicegroupSource::writeSynthDefinitions(
                            fakeRoot,
                            {{QStringLiteral("SynthCheckSaw"), VgSynthDesc{0, 9, 9, 9, 9}}},
                            &error) &&
                            !error.isEmpty() && readFileBytes(synthPath) == grown);
            // The CRLF file must stay CRLF: no bare '\n' anywhere.
            bool crlfOk = true;
            for (int i = 0; i < grown.size(); i++) {
                if (grown[i] == '\n' && (i == 0 || grown[i - 1] != '\r'))
                    crlfOk = false;
            }
            expectSynth("appends keep CRLF endings", crlfOk);

            // Without the set_synth_* macros, existing definitions still
            // resolve but new ones are refused (they wouldn't assemble).
            const QString bareRoot = projectRoot + QStringLiteral("/.porydaw/synthcheck_bare");
            QDir().mkpath(bareRoot + QStringLiteral("/sound"));
            if (!writeFile(bareRoot + QStringLiteral("/sound/direct_sound_synth_data.inc"),
                           "SynthCheckSaw::\n    set_synth_saw\n")) {
                std::fprintf(stderr, "vgcheck: cannot write synthcheck_bare\n");
                failures++;
            } else {
                const VgSynthCatalog bare = VoicegroupSource::synthInstruments(bareRoot);
                expectSynth("gate: not creatable without macros", !bare.creatable());
                expectSynth("gate: existing definition still resolves",
                            bare.symbolFor(VgSynthDesc{1, 0, 0, 0, 0}) ==
                                QStringLiteral("SynthCheckSaw"));
                error.clear();
                expectSynth("gate: new definition refused",
                            !VoicegroupSource::writeSynthDefinitions(
                                bareRoot,
                                {{QStringLiteral("DirectSoundSynth_GoldenSun_09090909"),
                                  VgSynthDesc{0, 9, 9, 9, 9}}},
                                &error) &&
                                !error.isEmpty());
            }
            QDir(bareRoot).removeRecursively();

            // With macros but no assembly source including
            // direct_sound_data.inc, the writer has nowhere to wire the synth
            // data file into the build — refused with instructions instead of
            // silently breaking the ROM build.
            const QString unwiredRoot =
                projectRoot + QStringLiteral("/.porydaw/synthcheck_unwired");
            QDir().mkpath(unwiredRoot + QStringLiteral("/sound"));
            QDir().mkpath(unwiredRoot + QStringLiteral("/asm/macros"));
            if (!writeFile(unwiredRoot + QStringLiteral("/asm/macros/music_voice.inc"),
                           "\t.macro set_synth_pulse a=0, b=0, c=0, d=0\n"
                           "\t.endm\n")) {
                std::fprintf(stderr, "vgcheck: cannot write synthcheck_unwired\n");
                failures++;
            } else {
                error.clear();
                expectSynth("unwired build refused with instructions",
                            !VoicegroupSource::writeSynthDefinitions(
                                unwiredRoot,
                                {{QStringLiteral("DirectSoundSynth_GoldenSun_09090909"),
                                  VgSynthDesc{0, 9, 9, 9, 9}}},
                                &error) &&
                                error.contains(QStringLiteral("direct_sound_synth_data.inc")));
            }
            QDir(unwiredRoot).removeRecursively();
            if (failures == before)
                std::printf("vgcheck: synth scan/dedupe/write OK\n");
        }
        QDir(fakeRoot).removeRecursively();
    }

    // ---- single-colon labels: file-local sample symbols still count ----
    // GAS accepts "Name:" (file-local) as well as "Name::" (global), and a
    // single-colon sample assembles and links because sample data shares its
    // assembly unit with the voicegroups referencing it — so both the catalog
    // scan and the loader must recognize both forms.
    {
        const int before = failures;
        const QString fakeRoot = projectRoot + QStringLiteral("/.porydaw/coloncheck");
        QDir().mkpath(fakeRoot + QStringLiteral("/sound/direct_sound_samples"));
        QDir().mkpath(fakeRoot + QStringLiteral("/sound/programmable_wave_samples"));
        QDir().mkpath(fakeRoot + QStringLiteral("/sound/voicegroups"));
        const auto writeFile = [](const QString &path, const QByteArray &bytes) {
            QFile out(path);
            return out.open(QIODevice::WriteOnly) && out.write(bytes) == bytes.size();
        };
        const auto expectColon = [&failures](const char *what, bool ok) {
            if (!ok) {
                std::fprintf(stderr, "vgcheck: FAIL: single-colon %s\n", what);
                failures++;
            }
        };
        // AGB sample .bin: 16-byte header (type, status, freq, loopStart,
        // size), then signed 8-bit PCM.
        const auto sampleBin = [](char fill) {
            QByteArray bin(16, '\0');
            bin[12] = 4; // size
            bin += QByteArray(4, fill);
            return bin;
        };
        const bool wrote =
            writeFile(fakeRoot + QStringLiteral("/sound/direct_sound_samples/colon_single.bin"),
                      sampleBin('\x11')) &&
            writeFile(fakeRoot + QStringLiteral("/sound/direct_sound_samples/colon_double.bin"),
                      sampleBin('\x22')) &&
            writeFile(fakeRoot + QStringLiteral("/sound/programmable_wave_samples/colon_wave.pcm"),
                      QByteArray(16, '\x33')) &&
            writeFile(fakeRoot + QStringLiteral("/sound/direct_sound_data.inc"),
                      "\t.align 2\n"
                      "DirectSoundWaveData_colon_double::\n"
                      "\t.incbin \"sound/direct_sound_samples/colon_double.bin\"\n"
                      "\n"
                      "\t.align 2\n"
                      "DirectSoundWaveData_colon_single: @ file-local label\n"
                      "\t.incbin \"sound/direct_sound_samples/colon_single.bin\"\n"
                      "\n"
                      "\t.align 2\n"
                      "ColonCheckSynth:\n"
                      "\tset_synth_25\n") &&
            writeFile(fakeRoot + QStringLiteral("/sound/programmable_wave_data.inc"),
                      "\t.align 2\n"
                      "ProgrammableWaveData_colon_wave:\n"
                      "\t.incbin \"sound/programmable_wave_samples/colon_wave.pcm\"\n") &&
            writeFile(fakeRoot + QStringLiteral("/sound/voicegroups/coloncheck.inc"),
                      "voicegroup_coloncheck::\n"
                      "\tvoice_directsound 60, 0, DirectSoundWaveData_colon_single, "
                      "255, 0, 255, 165\n"
                      "\tvoice_directsound 60, 0, DirectSoundWaveData_colon_double, "
                      "255, 0, 255, 165\n"
                      "\tvoice_programmable_wave 60, 0, ProgrammableWaveData_colon_wave, "
                      "0, 0, 15, 3\n");
        if (!wrote) {
            std::fprintf(stderr, "vgcheck: cannot write coloncheck mini-project\n");
            failures++;
        } else {
            const VgDirectSoundScan scan = VoicegroupSource::directSoundCatalog(fakeRoot);
            expectColon(
                "catalog lists the single-colon sample",
                scan.directSound.contains(QStringLiteral("DirectSoundWaveData_colon_single")));
            expectColon(
                "catalog lists the double-colon sample",
                scan.directSound.contains(QStringLiteral("DirectSoundWaveData_colon_double")));
            expectColon("synth def stays out of the sample list",
                        !scan.directSound.contains(QStringLiteral("ColonCheckSynth")));
            expectColon("single-colon synth def is scanned",
                        scan.synths.find(QStringLiteral("ColonCheckSynth")) != nullptr);
            expectColon("prog-wave scan sees the single-colon label",
                        VoicegroupSource::progWaveSymbols(fakeRoot).contains(
                            QStringLiteral("ProgrammableWaveData_colon_wave")));

            LoadedVoiceGroup *vg =
                voicegroup_load(fakeRoot.toUtf8().constData(), "coloncheck", nullptr);
            if (!vg) {
                std::fprintf(stderr, "vgcheck: FAIL: coloncheck voicegroup_load failed\n");
                failures++;
            } else {
                const ToneData &s = vg->voices[0];
                const ToneData &d = vg->voices[1];
                const ToneData &w = vg->voices[2];
                expectColon("loader resolves the single-colon sample",
                            s.wav && s.wav->size == 4 && s.wav->data[0] == 0x11);
                expectColon("loader resolves the double-colon sample",
                            d.wav && d.wav->size == 4 && d.wav->data[0] == 0x22);
                expectColon("loader resolves the single-colon prog wave", w.wavePointer != nullptr);
                voicegroup_free(vg);
            }
        }
        QDir(fakeRoot).removeRecursively();
        if (failures == before)
            std::printf("vgcheck: single-colon label scan OK\n");
    }

    // ---- compressed samples: the pokeemerald-expansion voice and cry macros ----
    // Every macro the loader counts as a voice must be one here too, or the
    // slots after it drift apart. A compressed voice loads its built DPCM
    // .bin, and falls back to the sample's source in an unbuilt project.
    {
        const int before = failures;
        const QString fakeRoot = projectRoot + QStringLiteral("/.porydaw/compcheck");
        QDir().mkpath(fakeRoot + QStringLiteral("/asm/macros"));
        QDir().mkpath(fakeRoot + QStringLiteral("/sound/direct_sound_samples/cries"));
        QDir().mkpath(fakeRoot + QStringLiteral("/sound/voicegroups"));
        const auto writeFile = [](const QString &path, const QByteArray &bytes) {
            QFile out(path);
            return out.open(QIODevice::WriteOnly) && out.write(bytes) == bytes.size();
        };
        const auto expectComp = [&failures](const char *what, bool ok) {
            if (!ok) {
                std::fprintf(stderr, "vgcheck: FAIL: compressed %s\n", what);
                failures++;
            }
        };
        // One DPCM-compressed sample: type 1, 64 samples, and the two 33-byte
        // blocks the encoders emit for that size.
        QByteArray dpcmBin(16, '\0');
        dpcmBin[0] = 1;
        dpcmBin[12] = 64;
        dpcmBin += QByteArray(66, '\0');
        // A minimal 8-bit mono WAV of eight samples.
        QByteArray wav("RIFF\x2c\0\0\0WAVEfmt \x10\0\0\0\x01\0\x01\0", 24);
        wav += QByteArray("\x11\x2b\0\0\x11\x2b\0\0\x01\0\x08\0", 12);
        wav += QByteArray("data\x08\0\0\0", 8) + QByteArray(8, '\x90');
        const bool wrote =
            writeFile(fakeRoot + QStringLiteral("/asm/macros/music_voice.inc"),
                      "\t.macro voice_directsound_compressed base_midi_key:req\n\t.endm\n"
                      "\t.macro _voice_directsound base_midi_key:req\n\t.endm\n") &&
            writeFile(fakeRoot + QStringLiteral("/sound/direct_sound_samples/cries/built.bin"),
                      dpcmBin) &&
            writeFile(fakeRoot + QStringLiteral("/sound/direct_sound_samples/cries/unbuilt.wav"),
                      wav) &&
            writeFile(fakeRoot + QStringLiteral("/sound/direct_sound_data.inc"),
                      "\t.align 2\n"
                      "Cry_Built::\n"
                      "\t.incbin \"sound/direct_sound_samples/cries/built.bin\"\n"
                      "\n"
                      "\t.align 2\n"
                      "Cry_Unbuilt::\n"
                      "\t.incbin \"sound/direct_sound_samples/cries/unbuilt.bin\"\n"
                      "\n"
                      "\t.align 2\n"
                      "Cry_Both::\n"
                      "\t.incbin \"sound/direct_sound_samples/cries/both.bin\"\n") &&
            writeFile(fakeRoot + QStringLiteral("/sound/direct_sound_samples/cries/both.bin"),
                      dpcmBin) &&
            writeFile(fakeRoot + QStringLiteral("/sound/direct_sound_samples/cries/both.wav"),
                      wav) &&
            writeFile(fakeRoot + QStringLiteral("/sound/voicegroups/compcheck.inc"),
                      "voicegroup_compcheck::\n"
                      "\tvoice_directsound_compressed 60, 0, Cry_Built, 255, 0, 255, 0\n"
                      "\tvoice_directsound_compressed_reverse 60, 0, Cry_Unbuilt, 255, 0, 255, 0\n"
                      "\tcry_custom 72, 0, Cry_Built, 255, 0, 255, 9\n"
                      "\tcry_reverse_uncomp Cry_Unbuilt\n"
                      "\tvoice_directsound_reverse 60, 0, Cry_Unbuilt, 255, 0, 255, 0\n"
                      // A tab after the word matches on neither side: no slot.
                      "\tvoice_directsound_compressed\t60, 0, Cry_Built, 255, 0, 255, 0\n"
                      "\tvoice_noise 60, 0, 0, 0, 1, 0, 1\n"
                      "\tcry Cry_Both\n"
                      // Symbols longer than the loader's buffers (ASAN's to catch).
                      "\tvoice_directsound 60, 0, " +
                          QByteArray(600, 'A') + ", 255, 0, 255, 0\n\tcry " + QByteArray(600, 'B') +
                          "\n\tvoice_keysplit " + QByteArray(600, 'C') + ", " +
                          QByteArray(600, 'D') + "\n\tvoice_noise 60, 0, 0, 0, 2, 0, 2\n");
        if (!wrote) {
            std::fprintf(stderr, "vgcheck: cannot write compcheck mini-project\n");
            failures++;
        } else {
            const VgDirectSoundScan scan = VoicegroupSource::directSoundCatalog(fakeRoot);
            expectComp("catalog lists the cries apart from the samples",
                       scan.cries ==
                               QStringList({QStringLiteral("Cry_Both"), QStringLiteral("Cry_Built"),
                                            QStringLiteral("Cry_Unbuilt")}) &&
                           scan.directSound.isEmpty());
            expectComp("catalog reports only the defined voice macros",
                       scan.voiceMacroWords ==
                           QStringList({QStringLiteral("voice_directsound_compressed")}));

            // The .bin against the source beside it: the newer one plays.
            const auto loadsBothAs = [&](int hoursAfterSource) -> int {
                QFile bin(fakeRoot + QStringLiteral("/sound/direct_sound_samples/cries/both.bin"));
                const QDateTime source =
                    QFileInfo(fakeRoot +
                              QStringLiteral("/sound/direct_sound_samples/cries/both.wav"))
                        .lastModified();
                if (!bin.open(QIODevice::ReadWrite) ||
                    !bin.setFileTime(source.addSecs(hoursAfterSource * 3600),
                                     QFileDevice::FileModificationTime))
                    return -1;
                bin.close();
                LoadedVoiceGroup *both =
                    voicegroup_load(fakeRoot.toUtf8().constData(), "compcheck", nullptr);
                const int type = both && both->voices[6].wav ? both->voices[6].wav->type : -1;
                if (both)
                    voicegroup_free(both);
                return type;
            };
            expectComp("a .bin built after its source plays", loadsBothAs(1) == 1);
            expectComp("a .bin as old as its source plays", loadsBothAs(0) == 1);
            expectComp("a .bin older than its source gives way to it", loadsBothAs(-1) == 0);

            LoadedVoiceGroup *vg =
                voicegroup_load(fakeRoot.toUtf8().constData(), "compcheck", nullptr);
            if (!vg) {
                std::fprintf(stderr, "vgcheck: FAIL: compcheck voicegroup_load failed\n");
                failures++;
            } else {
                const ToneData *t = vg->voices;
                expectComp("voice loads its DPCM .bin", t[0].type == VOICE_CRY && t[0].wav &&
                                                            t[0].wav->type == 1 &&
                                                            t[0].wav->size == 64);
                expectComp("reverse voice falls back to the unbuilt cry's source",
                           t[1].type == VOICE_CRY_REVERSE && t[1].wav && t[1].wav->type == 0 &&
                               t[1].wav->size == 8);
                expectComp("cry_custom keeps its arguments", t[2].type == VOICE_CRY &&
                                                                 t[2].key == 72 &&
                                                                 t[2].release == 9 && t[2].wav);
                expectComp("cry_reverse_uncomp loads reversed",
                           t[3].type == VOICE_DIRECTSOUND_ALT && t[3].wav);
                expectComp("voice_directsound_reverse loads reversed",
                           t[4].type == VOICE_DIRECTSOUND_ALT && t[4].wav);
                expectComp("the slot after them is where the file says", t[5].type == VOICE_NOISE);
                expectComp("overlong symbols resolve to nothing and keep their slots",
                           !t[7].wav && !t[8].wav && !t[9].subGroup && t[10].type == VOICE_NOISE &&
                               t[10].decay == 2);
                voicegroup_free(vg);
            }

            VoicegroupSource comp;
            QString compError;
            if (!comp.open(fakeRoot, QStringLiteral("_compcheck"), &compError)) {
                std::fprintf(stderr, "vgcheck: FAIL: compcheck source open: %s\n",
                             qUtf8Printable(compError));
                failures++;
            } else {
                const VgVoice *v0 = comp.voiceAt(0);
                const VgVoice *v1 = comp.voiceAt(1);
                const VgVoice *v4 = comp.voiceAt(4);
                const VgVoice *v5 = comp.voiceAt(5);
                expectComp("source parses the compressed macros as editable",
                           v0 && v0->macro == VgMacro::DirectSoundCompressed && v1 &&
                               v1->macro == VgMacro::DirectSoundCompressedReverse &&
                               v1->symbol == QLatin1String("Cry_Unbuilt") && v4 &&
                               v4->macro == VgMacro::DirectSoundReverse);
                expectComp("source keeps the cry macros read-only",
                           comp.kindAt(2) == VgLineKind::ReadOnlyVoice &&
                               comp.kindAt(3) == VgLineKind::ReadOnlyVoice);
                expectComp("source slots match the loader's", v5 && v5->macro == VgMacro::Noise);
                const VgParsedSource parsed = VoicegroupSource::parseSource(comp.renderPreview());
                QStringList crySymbols;
                for (const VgSourceLine &line : parsed.lines) {
                    if (line.kind == VgLineKind::ReadOnlyVoice)
                        crySymbols.append(line.crySymbol);
                }
                expectComp("parseSource finds the cry macros' symbols",
                           crySymbols.mid(0, 3) == QStringList({QStringLiteral("Cry_Built"),
                                                                QStringLiteral("Cry_Unbuilt"),
                                                                QStringLiteral("Cry_Both")}));
                if (v0) {
                    VgVoice edited = *v0;
                    edited.release = 200;
                    comp.setVoice(0, edited);
                    expectComp("an edit rewrites only its arguments",
                               comp.renderPreview().contains(
                                   "\tvoice_directsound_compressed 60, 0, Cry_Built, "
                                   "255, 0, 255, 200\n"));
                }
            }
        }
        QDir(fakeRoot).removeRecursively();
        if (failures == before)
            std::printf("vgcheck: compressed sample voices OK\n");
    }

    // ---- Golden Sun synths: loader roundtrip through the real voicegroup ----
    if (dsSlot >= 0) {
        const int before = failures;
        // The loader needs only the data file, not the .macro definitions, so
        // this works on projects without synth support. Append (never
        // truncate) in case the project already has synth definitions.
        QFile synthData(projectRoot + QStringLiteral("/sound/direct_sound_synth_data.inc"));
        if (!synthData.open(QIODevice::WriteOnly | QIODevice::Append)) {
            std::fprintf(stderr, "vgcheck: cannot append synth data file\n");
            failures++;
        } else {
            synthData.write("\n\t.align 2\nVgcheckSynthPulse::\n"
                            "\tset_synth_pulse 0x21, 0x43, 0x65, 0x87\n");
            synthData.close();
            VgVoice v = *src.voiceAt(dsSlot);
            v.symbol = QStringLiteral("VgcheckSynthPulse");
            src.setVoice(dsSlot, v);
            if (!src.save(&error)) {
                std::fprintf(stderr, "vgcheck: synth save: %s\n", qUtf8Printable(error));
                failures++;
            } else {
                LoadedVoiceGroup *vg =
                    voicegroup_load(rootUtf8.constData(), loadName.constData(), nullptr);
                if (!vg) {
                    std::fprintf(stderr, "vgcheck: synth voicegroup_load failed\n");
                    failures++;
                } else {
                    const ToneData &td = vg->voices[dsSlot];
                    const auto *d =
                        td.wav ? reinterpret_cast<const uint8_t *>(td.wav->data) : nullptr;
                    if ((td.type & ~0x18) != 0 || !td.wav || td.wav->size != 0 || !d || d[1] != 0 ||
                        d[2] != 0x21 || d[3] != 0x43 || d[4] != 0x65 || d[5] != 0x87) {
                        std::fprintf(stderr,
                                     "vgcheck: FAIL: synth voice slot %d didn't load "
                                     "as a pulse descriptor\n",
                                     dsSlot);
                        failures++;
                    }
                    voicegroup_free(vg);
                }
            }
            if (failures == before)
                std::printf("vgcheck: synth loader roundtrip OK (slot %d)\n", dsSlot);
        }
    }

    voicegroup_free(baseline);
    std::printf("vgcheck: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
