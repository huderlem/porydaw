#include "sidecar.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace Sidecar {

QString dirPath(const QString &projectRoot)
{
    return projectRoot + QStringLiteral("/.porydaw");
}

namespace {

// Textual scan only, not full git-ignore semantics: a broader pre-existing
// pattern that already covers .porydaw just costs one redundant line. The
// rest of the file is byte-preserved — its line-ending style carries over
// to the appended entry, and a missing trailing newline is repaired rather
// than gluing onto the last pattern. .git may be a plain file (worktree or
// submodule checkout).
void ensureGitignore(const QString &projectRoot)
{
    if (!QFileInfo::exists(projectRoot + QStringLiteral("/.git")))
        return;
    const QString path = projectRoot + QStringLiteral("/.gitignore");
    QByteArray bytes;
    {
        QFile in(path);
        if (in.open(QIODevice::ReadOnly))
            bytes = in.readAll();
    }
    for (QByteArray line : bytes.split('\n')) {
        line = line.trimmed();
        if (line == ".porydaw" || line == ".porydaw/" || line == "/.porydaw"
            || line == "/.porydaw/")
            return;
    }
    const QByteArray eol = bytes.contains("\r\n") ? QByteArrayLiteral("\r\n")
                                                  : QByteArrayLiteral("\n");
    if (!bytes.isEmpty() && !bytes.endsWith('\n'))
        bytes += eol;
    bytes += ".porydaw/" + eol;
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly))
        return;
    out.write(bytes);
    out.commit();
}

} // namespace

bool ensureDir(const QString &projectRoot, const QString &subdir)
{
    QString dir = dirPath(projectRoot);
    if (!subdir.isEmpty())
        dir += QLatin1Char('/') + subdir;
    if (!QDir().mkpath(dir))
        return false;
    ensureGitignore(projectRoot);
    return true;
}

} // namespace Sidecar
