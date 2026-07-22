#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <cstdio>

#include "project/sidecar.h"

// --ignorecheck <scratchDir>: sidecar-dir/.gitignore check, self-contained
// (builds its own bare scratch projects). Sidecar::ensureDir must create
// .porydaw/ (and a requested subdir) and, in a git checkout, append
// ".porydaw/" to the root .gitignore — creating the file when missing,
// byte-preserving the rest (CRLF style carries over, a missing trailing
// newline is repaired first), recognizing every anchored/dir spelling as
// already present (file untouched, including on a second call), treating a
// plain-file .git (worktree/submodule checkout) as a repo, and leaving
// non-repos alone. scratchDir must not already exist.

namespace {

int failures = 0;

void expect(bool ok, const char *what)
{
    if (!ok) {
        std::fprintf(stderr, "ignorecheck: FAIL: %s\n", what);
        failures++;
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
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    return f.readAll();
}

} // namespace

int runIgnoreCheck(const QString &scratchDir)
{
    if (QDir(scratchDir).exists()) {
        std::fprintf(stderr,
                     "ignorecheck: scratch dir %s already exists; give a "
                     "fresh path\n",
                     qUtf8Printable(scratchDir));
        return 1;
    }

    int caseNo = 0;
    const auto freshRoot = [&](bool gitDir) {
        const QString root =
            scratchDir + QStringLiteral("/proj%1").arg(++caseNo);
        QDir().mkpath(root);
        if (gitDir)
            QDir().mkpath(root + QStringLiteral("/.git"));
        return root;
    };
    const QString gi = QStringLiteral("/.gitignore");

    { // append to an existing LF file
        const QString root = freshRoot(true);
        writeFile(root + gi, "*.o\nbuild/\n");
        expect(Sidecar::ensureDir(root), "existing-file ensureDir succeeds");
        expect(QDir(root + QStringLiteral("/.porydaw")).exists(),
               "sidecar dir created");
        expect(readFileBytes(root + gi) == "*.o\nbuild/\n.porydaw/\n",
               "entry appended, prior bytes preserved");
    }

    { // every already-present spelling leaves the file byte-untouched
        const char *spellings[] = {".porydaw", ".porydaw/", "/.porydaw",
                                   "/.porydaw/"};
        for (const char *s : spellings) {
            const QString root = freshRoot(true);
            const QByteArray seed = QByteArray("*.o\n") + s + "\nbuild/\n";
            writeFile(root + gi, seed);
            expect(Sidecar::ensureDir(root),
                   "present-spelling ensureDir succeeds");
            expect(readFileBytes(root + gi) == seed,
                   "already-present spelling leaves .gitignore untouched");
        }
    }

    { // no .gitignore -> created holding just the entry
        const QString root = freshRoot(true);
        expect(Sidecar::ensureDir(root), "no-file ensureDir succeeds");
        expect(readFileBytes(root + gi) == ".porydaw/\n",
               "missing .gitignore created with the entry");
    }

    { // not a git repo -> sidecar dir still appears, .gitignore does not
        const QString root = freshRoot(false);
        expect(Sidecar::ensureDir(root), "non-repo ensureDir succeeds");
        expect(QDir(root + QStringLiteral("/.porydaw")).exists(),
               "non-repo still gets the sidecar dir");
        expect(!QFileInfo::exists(root + gi),
               "non-repo .gitignore not created");
    }

    { // CRLF file keeps CRLF, including on the appended entry
        const QString root = freshRoot(true);
        writeFile(root + gi, "*.o\r\nbuild/\r\n");
        expect(Sidecar::ensureDir(root), "CRLF ensureDir succeeds");
        expect(readFileBytes(root + gi) == "*.o\r\nbuild/\r\n.porydaw/\r\n",
               "CRLF file appended with CRLF");
    }

    { // missing trailing newline repaired before the entry
        const QString root = freshRoot(true);
        writeFile(root + gi, "*.o\nbuild/");
        expect(Sidecar::ensureDir(root), "no-newline ensureDir succeeds");
        expect(readFileBytes(root + gi) == "*.o\nbuild/\n.porydaw/\n",
               "missing trailing newline repaired before appending");
    }

    { // .git as a plain file (worktree/submodule checkout) counts as a repo
        const QString root = freshRoot(false);
        writeFile(root + QStringLiteral("/.git"),
                  "gitdir: ../.git/worktrees/proj\n");
        expect(Sidecar::ensureDir(root), ".git-file ensureDir succeeds");
        expect(readFileBytes(root + gi) == ".porydaw/\n",
               ".git-file checkout still gets the entry");
    }

    { // subdir variant, and a second call is a byte no-op
        const QString root = freshRoot(true);
        expect(Sidecar::ensureDir(root, QStringLiteral("samples")),
               "subdir ensureDir succeeds");
        expect(QDir(root + QStringLiteral("/.porydaw/samples")).exists(),
               "subdir created under the sidecar dir");
        const QByteArray once = readFileBytes(root + gi);
        expect(once == ".porydaw/\n", "subdir call still writes the entry");
        expect(Sidecar::ensureDir(root), "second ensureDir succeeds");
        expect(readFileBytes(root + gi) == once,
               "second call leaves .gitignore untouched");
    }

    { // a commented-out entry does not count as present
        const QString root = freshRoot(true);
        writeFile(root + gi, "# .porydaw/\n");
        expect(Sidecar::ensureDir(root), "commented ensureDir succeeds");
        expect(readFileBytes(root + gi) == "# .porydaw/\n.porydaw/\n",
               "commented entry still gets a real one");
    }

    std::printf("ignorecheck: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
