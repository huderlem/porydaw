#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QString>
#include <QTemporaryDir>
#include <cstdio>

#include "core/smf.h"
#include "project/decompproject.h"

// --roundtrip <projectRoot> [mid2agbPath]: M2 acceptance check. For every song
// with a MIDI source: load through the full-fidelity SMF model, save, then run
// the project's real mid2agb on both the original and the saved file and diff
// the compiled .s output. A clean diff proves a porydaw load -> save cannot
// change what the ROM plays. Byte-identical .mid round-trips are reported as
// an informational extra (encoding choices like running status may differ).

namespace {

// Runs mid2agb on midPath with the song's midi.cfg flags, producing a .s file
// next to it. Returns the .s contents, or a null QByteArray on failure.
QByteArray compileMid(const QString &mid2agb, const QStringList &flags, const QString &midPath,
                      QString *error)
{
    const QString outPath = midPath.left(midPath.size() - 4) + QStringLiteral(".s");
    QProcess proc;
    proc.start(mid2agb, QStringList() << flags << midPath << outPath);
    if (!proc.waitForFinished(15000)) {
        *error = QStringLiteral("mid2agb timed out");
        return QByteArray();
    }
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        *error = QStringLiteral("mid2agb failed: %1")
                     .arg(QString::fromLocal8Bit(proc.readAllStandardError()).trimmed());
        return QByteArray();
    }
    QFile out(outPath);
    if (!out.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("mid2agb produced no output");
        return QByteArray();
    }
    return out.readAll();
}

// First differing line of two .s files, for failure diagnostics.
QString firstDiffLine(const QByteArray &a, const QByteArray &b)
{
    const QList<QByteArray> la = a.split('\n');
    const QList<QByteArray> lb = b.split('\n');
    const int n = qMin(la.size(), lb.size());
    for (int i = 0; i < n; i++) {
        if (la[i] != lb[i])
            return QStringLiteral("line %1: '%2' vs '%3'")
                .arg(i + 1)
                .arg(QString::fromLatin1(la[i]).trimmed(),
                     QString::fromLatin1(lb[i]).trimmed());
    }
    return QStringLiteral("line count %1 vs %2").arg(la.size()).arg(lb.size());
}

} // namespace

int runRoundTrip(const QString &projectRoot, const QString &mid2agbArg)
{
    DecompProject project;
    QString error;
    if (!project.open(projectRoot, &error)) {
        std::fprintf(stderr, "roundtrip: %s\n", qUtf8Printable(error));
        return 1;
    }

    QString mid2agb = mid2agbArg;
    if (mid2agb.isEmpty())
        mid2agb = projectRoot + QStringLiteral("/tools/mid2agb/mid2agb");
    if (!QFileInfo::exists(mid2agb)) {
        std::fprintf(stderr,
                     "roundtrip: mid2agb not found at %s (build the project's tools first, "
                     "or pass an explicit path)\n",
                     qUtf8Printable(mid2agb));
        return 1;
    }

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::fprintf(stderr, "roundtrip: cannot create temp dir\n");
        return 1;
    }
    QDir(tmp.path()).mkpath(QStringLiteral("orig"));
    QDir(tmp.path()).mkpath(QStringLiteral("saved"));

    QElapsedTimer timer;
    timer.start();

    int checked = 0, failures = 0, byteIdentical = 0;
    for (const SongInfo &song : project.songs()) {
        if (!song.isPlayable())
            continue;

        QFile origFile(song.midPath);
        if (!origFile.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "roundtrip: FAIL %s: cannot read %s\n",
                         qUtf8Printable(song.label), qUtf8Printable(song.midPath));
            failures++;
            continue;
        }
        const QByteArray origBytes = origFile.readAll();

        SmfFile smf;
        if (!SmfFile::read(origBytes, &smf, &error)) {
            std::fprintf(stderr, "roundtrip: FAIL %s: parse: %s\n", qUtf8Printable(song.label),
                         qUtf8Printable(error));
            failures++;
            continue;
        }
        const QByteArray savedBytes = smf.write();
        if (savedBytes == origBytes)
            byteIdentical++;

        // Same basename in both dirs so mid2agb's derived asm label matches.
        const QString name = QFileInfo(song.midPath).fileName();
        const QString origMid = tmp.path() + QStringLiteral("/orig/") + name;
        const QString savedMid = tmp.path() + QStringLiteral("/saved/") + name;
        {
            QFile f(origMid);
            f.open(QIODevice::WriteOnly);
            f.write(origBytes);
        }
        {
            QFile f(savedMid);
            f.open(QIODevice::WriteOnly);
            f.write(savedBytes);
        }

        const QByteArray origS = compileMid(mid2agb, song.cfg.rawFlags, origMid, &error);
        if (origS.isNull()) {
            std::fprintf(stderr, "roundtrip: FAIL %s: original: %s\n",
                         qUtf8Printable(song.label), qUtf8Printable(error));
            failures++;
            continue;
        }
        const QByteArray savedS = compileMid(mid2agb, song.cfg.rawFlags, savedMid, &error);
        if (savedS.isNull()) {
            std::fprintf(stderr, "roundtrip: FAIL %s: saved: %s\n", qUtf8Printable(song.label),
                         qUtf8Printable(error));
            failures++;
            continue;
        }

        if (origS != savedS) {
            std::fprintf(stderr, "roundtrip: FAIL %s: .s output differs — %s\n",
                         qUtf8Printable(song.label),
                         qUtf8Printable(firstDiffLine(origS, savedS)));
            failures++;
        }
        checked++;
    }

    std::printf("roundtrip: %d songs in %lld ms — %d byte-identical .mid, %d .s diffs/errors\n",
                checked, (long long)timer.elapsed(), byteIdentical, failures);
    std::printf("roundtrip: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
