#include <QDir>
#include <QFile>
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
// baseline, and a created voicegroup is discoverable and loadable.
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
        return type == o.type && key == o.key && panSweep == o.panSweep
            && attack == o.attack && decay == o.decay && sustain == o.sustain
            && release == o.release && name == o.name && packed == o.packed;
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
    if (td.type != VOICE_KEYSPLIT && td.type != VOICE_KEYSPLIT_ALL
        && (cgb == VOICE_SQUARE_1 || cgb == VOICE_SQUARE_2 || cgb == VOICE_NOISE))
        s.packed = reinterpret_cast<uintptr_t>(td.wavePointer);
    return s;
}

bool isDirectSound(VgMacro m)
{
    return m == VgMacro::DirectSound || m == VgMacro::DirectSoundNoResample
        || m == VgMacro::DirectSoundAlt;
}
bool isSquare1(VgMacro m) { return m == VgMacro::Square1 || m == VgMacro::Square1Alt; }
bool isNoise(VgMacro m) { return m == VgMacro::Noise || m == VgMacro::NoiseAlt; }
bool isProgWave(VgMacro m) { return m == VgMacro::ProgWave || m == VgMacro::ProgWaveAlt; }

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

int checkSlot(const LoadedVoiceGroup *vg, int slot, const VoiceSnap &expected,
              const char *what)
{
    const VoiceSnap got = snapVoice(vg, slot);
    if (got == expected)
        return 0;
    std::fprintf(stderr,
                 "vgcheck: FAIL: %s slot %d mismatch: "
                 "type %u/%u key %u/%u panSweep %u/%u adsr %u %u %u %u / %u %u %u %u "
                 "packed %lu/%lu name '%s'/'%s' (got/expected)\n",
                 what, slot, got.type, expected.type, got.key, expected.key,
                 got.panSweep, expected.panSweep, got.attack, got.decay, got.sustain,
                 got.release, expected.attack, expected.decay, expected.sustain,
                 expected.release, ulong(got.packed), ulong(expected.packed),
                 got.name.constData(), expected.name.constData());
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
                src.monolithic() ? "monolithic" : "per-file",
                qUtf8Printable(src.filePath()), qUtf8Printable(src.loadName()));
    const QByteArray fileBefore = readFileBytes(src.filePath());

    const QByteArray rootUtf8 = projectRoot.toLocal8Bit();
    const QByteArray loadName = src.loadName().toLocal8Bit();
    LoadedVoiceGroup *baseline = voicegroup_load(rootUtf8.constData(),
                                                 loadName.constData(), nullptr);
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
        std::fprintf(stderr, "vgcheck: no editable voices in %s\n",
                     qUtf8Printable(src.filePath()));
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
            if (i != dsSlot && o && isDirectSound(o->macro)
                && o->symbol != src.voiceAt(dsSlot)->symbol) {
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
        std::printf("vgcheck: edited Keysplit slot %d (sub-vg/table from slot %d)\n",
                    ksSlot, ksDonor);
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
        QFile out(previewDir + QStringLiteral("/") + src.loadName()
                  + QStringLiteral(".inc"));
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
    LoadedVoiceGroup *preview =
        voicegroup_load(rootUtf8.constData(), loadName.constData(), &cfg);
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
                std::fprintf(stderr,
                             "vgcheck: FAIL: %d lines changed on save, expected %d\n",
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
            std::fprintf(stderr,
                         "vgcheck: FAIL: swapped keysplit slot %d has no sub-voicegroup\n",
                         ksSlot);
            failures++;
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
            if (!a || !b || a->macro != b->macro || a->key != b->key || a->pan != b->pan
                || a->symbol != b->symbol || a->sweep != b->sweep || a->duty != b->duty
                || a->period != b->period || a->attack != b->attack
                || a->decay != b->decay || a->sustain != b->sustain
                || a->release != b->release) {
                std::fprintf(stderr, "vgcheck: FAIL: slot %d fields did not round-trip\n",
                             slot);
                failures++;
            }
        }
    }

    // ---- create a new voicegroup as a copy of the edited one ----
    if (QDir(projectRoot + QStringLiteral("/sound/voicegroups")).exists()) {
        const QString hubPath = projectRoot + QStringLiteral("/sound/voice_groups.inc");
        const QByteArray hubBefore = readFileBytes(hubPath);
        if (!VoicegroupSource::createVoicegroup(projectRoot,
                                                QStringLiteral("vgcheck_new"),
                                                src.filePath(), src.sectionLabel(),
                                                &error)
            || !VoicegroupSource::appendIncludeLine(projectRoot,
                                                    QStringLiteral("vgcheck_new"),
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
                    failures +=
                        checkSlot(created, editedSlots.at(e), expected.at(e), "created");
                voicegroup_free(created);
            }
            if (!SongRegistry::voicegroupArgs(projectRoot)
                     .contains(QStringLiteral("_vgcheck_new"))) {
                std::fprintf(stderr,
                             "vgcheck: FAIL: _vgcheck_new missing from voicegroupArgs\n");
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

    voicegroup_free(baseline);
    std::printf("vgcheck: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
