#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QString>
#include <cstdio>

#include "core/songdocument.h"
#include "project/decompproject.h"

// --savecheck <projectRoot> <song> [mid2agbPath]: M2 save-path check. Loads a
// song, makes a real edit (note + loop marker + song settings), saves, then
// verifies: the edits survive a fresh project re-open, the saved .mid still
// compiles through mid2agb, and every other line of midi.cfg is untouched.
// Run this against a scratch copy of a project — it writes into it.

namespace {

QByteArray readFileBytes(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    return f.readAll();
}

} // namespace

int runSaveCheck(const QString &projectRoot, const QString &songLabel,
                 const QString &mid2agbPath)
{
    DecompProject project;
    QString error;
    if (!project.open(projectRoot, &error)) {
        std::fprintf(stderr, "savecheck: %s\n", qUtf8Printable(error));
        return 1;
    }
    const SongInfo *song = nullptr;
    for (const SongInfo &s : project.songs()) {
        if (s.label == songLabel && s.isPlayable())
            song = &s;
    }
    if (!song) {
        std::fprintf(stderr, "savecheck: song '%s' not found\n", qUtf8Printable(songLabel));
        return 1;
    }

    const QString cfgPath =
        QFileInfo(song->midPath).dir().filePath(QStringLiteral("midi.cfg"));
    const QStringList cfgBefore =
        QString::fromUtf8(readFileBytes(cfgPath)).split(QLatin1Char('\n'));

    SongDocument doc;
    if (!doc.load(*song, &error)) {
        std::fprintf(stderr, "savecheck: load: %s\n", qUtf8Printable(error));
        return 1;
    }

    // Real edits: a note in empty space, a moved loop start, a volume change.
    int track = -1;
    for (int t = 0; t < doc.engineTrackCount(); t++) {
        if (!doc.notesForTrack(t).empty()) {
            track = t;
            break;
        }
    }
    if (track < 0) {
        std::fprintf(stderr, "savecheck: song has no notes\n");
        return 1;
    }
    uint64_t base = 0;
    for (const SmfTrack &tr : doc.smf().tracks)
        base = std::max(base, tr.endTick);
    base += 96;
    doc.addNote(track, base, 72, 24, 93);
    const uint64_t loopStart = doc.loopTick(false);
    doc.setLoopTick(false, loopStart == UINT64_MAX ? 0 : int64_t(loopStart + 24));
    SongCfg cfg = doc.cfg();
    cfg.masterVolume = 111;
    doc.setCfg(cfg);

    if (!doc.save(&error)) {
        std::fprintf(stderr, "savecheck: save: %s\n", qUtf8Printable(error));
        return 1;
    }
    if (doc.isDirty()) {
        std::fprintf(stderr, "savecheck: still dirty after save\n");
        return 1;
    }

    int failures = 0;

    // The edits must survive a fresh open (new project parse, new document).
    DecompProject project2;
    if (!project2.open(projectRoot, &error)) {
        std::fprintf(stderr, "savecheck: reopen: %s\n", qUtf8Printable(error));
        return 1;
    }
    const SongInfo *song2 = nullptr;
    for (const SongInfo &s : project2.songs()) {
        if (s.label == songLabel)
            song2 = &s;
    }
    SongDocument doc2;
    if (!song2 || !doc2.load(*song2, &error)) {
        std::fprintf(stderr, "savecheck: FAIL reload: %s\n", qUtf8Printable(error));
        return 1;
    }
    DocNote note;
    if (!doc2.findNote(track, base, 72, &note) || note.velocity != 93
        || note.duration != 24) {
        std::fprintf(stderr, "savecheck: FAIL: edited note missing after reload\n");
        failures++;
    }
    if (doc2.cfg().masterVolume != 111) {
        std::fprintf(stderr, "savecheck: FAIL: midi.cfg volume not persisted (got %d)\n",
                     doc2.cfg().masterVolume);
        failures++;
    }
    const uint64_t movedLoop = doc2.loopTick(false);
    const uint64_t expectedLoop = loopStart == UINT64_MAX ? 0 : loopStart + 24;
    if (movedLoop != expectedLoop) {
        std::fprintf(stderr, "savecheck: FAIL: loop start %llu, expected %llu\n",
                     (unsigned long long)movedLoop, (unsigned long long)expectedLoop);
        failures++;
    }

    // Every other midi.cfg line must be byte-identical.
    const QStringList cfgAfter =
        QString::fromUtf8(readFileBytes(cfgPath)).split(QLatin1Char('\n'));
    if (cfgBefore.size() != cfgAfter.size()) {
        std::fprintf(stderr, "savecheck: FAIL: midi.cfg line count changed (%lld -> %lld)\n",
                     (long long)cfgBefore.size(), (long long)cfgAfter.size());
        failures++;
    } else {
        const QString fileName = songLabel + QStringLiteral(".mid");
        for (int i = 0; i < cfgBefore.size(); i++) {
            const bool isSongLine =
                cfgBefore[i].section(QLatin1Char(':'), 0, 0).trimmed() == fileName;
            if (!isSongLine && cfgBefore[i] != cfgAfter[i]) {
                std::fprintf(stderr, "savecheck: FAIL: midi.cfg line %d changed: '%s'\n",
                             i + 1, qUtf8Printable(cfgAfter[i]));
                failures++;
            }
            if (isSongLine)
                std::printf("savecheck: cfg line now: %s\n", qUtf8Printable(cfgAfter[i]));
        }
    }

    // The saved .mid must still compile through mid2agb with the new flags.
    QString mid2agb = mid2agbPath;
    if (mid2agb.isEmpty())
        mid2agb = projectRoot + QStringLiteral("/tools/mid2agb/mid2agb");
    if (QFileInfo::exists(mid2agb)) {
        QProcess proc;
        const QString outS = song2->midPath.left(song2->midPath.size() - 4) + ".s";
        proc.start(mid2agb, QStringList()
                                << doc2.cfg().rawFlags << song2->midPath << outS);
        proc.waitForFinished(15000);
        if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
            std::fprintf(stderr, "savecheck: FAIL: mid2agb rejected the saved file: %s\n",
                         qUtf8Printable(QString::fromLocal8Bit(proc.readAllStandardError())));
            failures++;
        }
    } else {
        std::printf("savecheck: note: mid2agb not found at %s, compile check skipped\n",
                    qUtf8Printable(mid2agb));
    }

    std::printf("savecheck: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
