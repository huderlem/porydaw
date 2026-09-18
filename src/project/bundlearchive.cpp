#include "bundlearchive.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <cstring>
#include <vector>

#include "miniz.h"

namespace {

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

QString zipError(mz_zip_archive *zip)
{
    return QString::fromLatin1(mz_zip_get_error_string(mz_zip_get_last_error(zip)));
}

// Fixed DOS timestamp for every entry: 1980-01-01 00:00:00. miniz writes
// zeros with MINIZ_NO_TIME (day 0 / month 0, which some tools show as a
// blank date); patching in a real date keeps the archive deterministic and
// well-formed. Local headers are found through the central directory,
// which we walk by hand (46-byte fixed header + variable name/extra/comment).
void stampFixedTimestamps(QByteArray *archive)
{
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!mz_zip_reader_init_mem(&zip, archive->constData(), size_t(archive->size()), 0))
        return;
    auto *bytes = reinterpret_cast<unsigned char *>(archive->data());
    const qint64 total = archive->size();
    const auto stamp = [&](qint64 ofs) {
        if (ofs < 0 || ofs + 4 > total)
            return;
        bytes[ofs] = 0x00; // time: 00:00:00
        bytes[ofs + 1] = 0x00;
        bytes[ofs + 2] = 0x21; // date: day 1, month 1, year 1980
        bytes[ofs + 3] = 0x00;
    };
    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (mz_zip_reader_file_stat(&zip, i, &st))
            stamp(qint64(st.m_local_header_ofs) + 10);
    }
    qint64 ofs = qint64(zip.m_central_directory_file_ofs);
    for (mz_uint i = 0; i < n; i++) {
        if (ofs + 46 > total)
            break;
        const unsigned char *h = bytes + ofs;
        const quint32 sig = h[0] | (h[1] << 8) | (h[2] << 16) | (quint32(h[3]) << 24);
        if (sig != 0x02014b50u)
            break;
        stamp(ofs + 12);
        const int nameLen = h[28] | (h[29] << 8);
        const int extraLen = h[30] | (h[31] << 8);
        const int commentLen = h[32] | (h[33] << 8);
        ofs += 46 + nameLen + extraLen + commentLen;
    }
    mz_zip_reader_end(&zip);
}

bool isSymlinkEntry(const mz_zip_archive_file_stat &st)
{
    // Unix "made by" hosts store st_mode in the high 16 bits of the external
    // attributes; S_IFLNK is 0120000.
    const unsigned mode = (st.m_external_attr >> 16) & 0170000u;
    return mode == 0120000u;
}

struct PendingEntry {
    QString name; // normalized archive path
    mz_uint index;
    quint64 size;
};

bool readAll(const QString &path, QByteArray *bytes, QString *error)
{
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Cannot read %1: %2").arg(path, in.errorString()));
        return false;
    }
    *bytes = in.readAll();
    return true;
}

bool writeAtomic(const QString &path, const QByteArray &bytes, QString *error)
{
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit()) {
        setError(error, QStringLiteral("Cannot write %1: %2").arg(path, out.errorString()));
        return false;
    }
    return true;
}

} // namespace

namespace BundleArchive {

bool validateEntryName(const QString &name, QString *normalized, QString *why)
{
    QString n = name;
    n.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const auto fail = [&](const QString &reason) {
        if (why)
            *why = QStringLiteral("%1 (\"%2\")").arg(reason, name);
        return false;
    };
    if (n.isEmpty())
        return fail(QStringLiteral("empty entry name"));
    // miniz keeps at most MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE - 1 bytes of a
    // name when reading, so the limit is on the UTF-8 form.
    if (n.toUtf8().size() >= MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE)
        return fail(QStringLiteral("entry name too long"));
    if (n.startsWith(QLatin1Char('/')))
        return fail(QStringLiteral("absolute entry path"));
    if (n.contains(QLatin1Char(':')))
        return fail(QStringLiteral("drive letter or stream in entry path"));
    for (const QChar c : n) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f)
            return fail(QStringLiteral("control character in entry path"));
    }
    const QStringList segments = n.split(QLatin1Char('/'));
    for (const QString &seg : segments) {
        if (seg.isEmpty())
            return fail(QStringLiteral("empty path segment"));
        if (seg == QLatin1String(".") || seg == QLatin1String(".."))
            return fail(QStringLiteral("relative path segment"));
        // Windows drops these, so "x.inc." would land on top of "x.inc".
        if (seg.endsWith(QLatin1Char('.')) || seg.endsWith(QLatin1Char(' ')))
            return fail(QStringLiteral("trailing dot or space in path segment"));
    }
    if (normalized)
        *normalized = n;
    return true;
}

bool createBundle(const QString &srcDir, const QString &zipPath, QString *error)
{
    const QDir src(srcDir);
    if (!src.exists()) {
        setError(error, QStringLiteral("Bundle source directory does not exist: %1").arg(srcDir));
        return false;
    }
    if (!QFileInfo(src.filePath(QString::fromLatin1(manifestEntryName()))).isFile()) {
        setError(error, QStringLiteral("Bundle source has no root %1")
                            .arg(QString::fromLatin1(manifestEntryName())));
        return false;
    }

    QStringList names;
    qint64 totalBytes = 0;
    QDirIterator it(srcDir, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        QString rel = src.relativeFilePath(path);
        rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
        QString why;
        if (!validateEntryName(rel, &rel, &why)) {
            setError(error, QStringLiteral("Cannot bundle: %1").arg(why));
            return false;
        }
        names.append(rel);
        totalBytes += QFileInfo(path).size();
    }
    if (names.size() > kMaxEntries) {
        setError(error, QStringLiteral("Cannot bundle: %1 files exceeds the %2-entry limit")
                            .arg(names.size())
                            .arg(kMaxEntries));
        return false;
    }
    if (totalBytes > kMaxTotalBytes) {
        setError(error, QStringLiteral("Cannot bundle: %1 bytes exceeds the %2 MiB limit")
                            .arg(totalBytes)
                            .arg(kMaxTotalBytes >> 20));
        return false;
    }
    // Byte order of the UTF-8 form: identical on every platform and locale.
    std::sort(names.begin(), names.end(),
              [](const QString &a, const QString &b) { return a.toUtf8() < b.toUtf8(); });

    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) {
        setError(error, QStringLiteral("Cannot create zip: %1").arg(zipError(&zip)));
        return false;
    }
    for (const QString &name : names) {
        QByteArray bytes;
        if (!readAll(src.filePath(name), &bytes, error)) {
            mz_zip_writer_end(&zip);
            return false;
        }
        const QByteArray utf8 = name.toUtf8();
        if (!mz_zip_writer_add_mem(&zip, utf8.constData(), bytes.constData(), size_t(bytes.size()),
                                   MZ_BEST_COMPRESSION)) {
            setError(error, QStringLiteral("Cannot add %1 to zip: %2").arg(name, zipError(&zip)));
            mz_zip_writer_end(&zip);
            return false;
        }
    }
    void *buf = nullptr;
    size_t size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&zip, &buf, &size)) {
        setError(error, QStringLiteral("Cannot finalize zip: %1").arg(zipError(&zip)));
        mz_zip_writer_end(&zip);
        return false;
    }
    QByteArray archive(static_cast<const char *>(buf), int(size));
    mz_free(buf);
    mz_zip_writer_end(&zip);

    stampFixedTimestamps(&archive);
    return writeAtomic(zipPath, archive, error);
}

bool extractBundleLimited(const QString &zipPath, const QString &destDir, qint64 maxTotalBytes,
                          int maxEntries, QString *error)
{
    const QFileInfo zipInfo(zipPath);
    if (!zipInfo.isFile()) {
        setError(error, QStringLiteral("Not a file: %1").arg(zipPath));
        return false;
    }
    // Stored (level 0) entries plus per-entry headers bound how large a
    // within-limit archive can be; anything beyond that cannot be valid.
    if (zipInfo.size() > maxTotalBytes + qint64(4) * 1024 * 1024) {
        setError(error, QStringLiteral("Refusing %1: larger than the %2 MiB bundle limit")
                            .arg(zipPath)
                            .arg(maxTotalBytes >> 20));
        return false;
    }
    QByteArray archive;
    if (!readAll(zipPath, &archive, error))
        return false;

    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!mz_zip_reader_init_mem(&zip, archive.constData(), size_t(archive.size()), 0)) {
        setError(error, QStringLiteral("Not a song bundle (not a zip archive): %1").arg(zipPath));
        return false;
    }
    const auto fail = [&](const QString &message) {
        setError(error, QStringLiteral("Refusing %1: %2").arg(zipPath, message));
        mz_zip_reader_end(&zip);
        return false;
    };

    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    if (qint64(count) > maxEntries)
        return fail(
            QStringLiteral("%1 entries exceeds the %2-entry limit").arg(count).arg(maxEntries));

    // Names are compared case-folded: on a case-insensitive filesystem
    // "Foo.wav" and "foo.wav" are the same file.
    std::vector<PendingEntry> files;
    QSet<QString> fileNames;
    QSet<QString> dirNames;
    quint64 claimed = 0;
    bool haveManifest = false;
    for (mz_uint i = 0; i < count; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st))
            return fail(QStringLiteral("unreadable entry %1: %2").arg(i).arg(zipError(&zip)));
        // file_stat silently truncates an over-long name; ask for the real
        // length (returned with its terminator) and refuse instead.
        const mz_uint rawLen = mz_zip_reader_get_filename(&zip, i, nullptr, 0);
        if (rawLen == 0 || rawLen > MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE)
            return fail(QStringLiteral("entry name too long (entry %1)").arg(i));
        const QString raw = QString::fromUtf8(st.m_filename, int(rawLen) - 1);
        QString name;
        QString why;
        const bool isDir =
            st.m_is_directory || raw.endsWith(QLatin1Char('/')) || raw.endsWith(QLatin1Char('\\'));
        if (!validateEntryName(isDir ? raw.left(raw.size() - 1) : raw, &name, &why))
            return fail(why);
        if (st.m_is_encrypted)
            return fail(QStringLiteral("encrypted entry \"%1\"").arg(name));
        if (!st.m_is_supported)
            return fail(QStringLiteral("unsupported compression on \"%1\"").arg(name));
        if (isSymlinkEntry(st))
            return fail(QStringLiteral("symbolic link entry \"%1\"").arg(name));
        const QString folded = name.toCaseFolded();
        if (isDir) {
            dirNames.insert(folded);
            continue;
        }
        if (fileNames.contains(folded))
            return fail(QStringLiteral("duplicate entry \"%1\"").arg(name));
        fileNames.insert(folded);
        // Every ancestor of a file is a directory the extractor will create.
        for (int slash = folded.indexOf(QLatin1Char('/')); slash >= 0;
             slash = folded.indexOf(QLatin1Char('/'), slash + 1))
            dirNames.insert(folded.left(slash));
        if (st.m_uncomp_size > quint64(maxTotalBytes) ||
            claimed + st.m_uncomp_size > quint64(maxTotalBytes)) {
            return fail(
                QStringLiteral("contents exceed the %1 MiB limit").arg(maxTotalBytes >> 20));
        }
        claimed += st.m_uncomp_size;
        if (name == QLatin1String(manifestEntryName()))
            haveManifest = true;
        files.push_back({name, i, st.m_uncomp_size});
    }
    for (const QString &name : fileNames) {
        if (dirNames.contains(name))
            return fail(QStringLiteral("\"%1\" is both a file and a directory").arg(name));
    }
    if (!haveManifest)
        return fail(QStringLiteral("no root %1").arg(QString::fromLatin1(manifestEntryName())));

    // Inflate every entry before touching destDir, so a corrupt payload
    // (inflate error, CRC or size mismatch) is refused like any other guard.
    const QDir dest(destDir);
    QString destBase = QDir::cleanPath(dest.absolutePath());
    if (!destBase.endsWith(QLatin1Char('/')))
        destBase += QLatin1Char('/');
    std::vector<QString> outPaths;
    std::vector<QByteArray> contents;
    outPaths.reserve(files.size());
    contents.reserve(files.size());
    for (const PendingEntry &entry : files) {
        const QString outPath = QDir::cleanPath(dest.absoluteFilePath(entry.name));
        // Belt and braces: the validated name cannot escape, but the
        // resolved path must still sit under destDir.
        if (!outPath.startsWith(destBase))
            return fail(
                QStringLiteral("entry \"%1\" resolves outside %2").arg(entry.name, destDir));
        size_t size = 0;
        void *data = mz_zip_reader_extract_to_heap(&zip, entry.index, &size, 0);
        if (!data)
            return fail(
                QStringLiteral("cannot extract \"%1\": %2").arg(entry.name, zipError(&zip)));
        contents.emplace_back(static_cast<const char *>(data), int(size));
        mz_free(data);
        if (quint64(size) != entry.size)
            return fail(QStringLiteral("size mismatch on \"%1\"").arg(entry.name));
        outPaths.push_back(outPath);
    }

    // Everything checked out: only now touch destDir. What is left to go
    // wrong is the filesystem itself; undo what was written if it does.
    const bool destExisted = dest.exists();
    size_t written = 0;
    const auto failWrite = [&](const QString &message) {
        if (!destExisted) {
            QDir(destDir).removeRecursively();
        } else {
            for (size_t i = 0; i < written; i++)
                QFile::remove(outPaths[i]);
        }
        return fail(message);
    };
    if (!QDir().mkpath(destDir))
        return failWrite(QStringLiteral("cannot create %1").arg(destDir));
    for (; written < outPaths.size(); written++) {
        const QString &outPath = outPaths[written];
        if (!QDir().mkpath(QFileInfo(outPath).path()))
            return failWrite(
                QStringLiteral("cannot create directory for \"%1\"").arg(files[written].name));
        QString writeError;
        if (!writeAtomic(outPath, contents[written], &writeError))
            return failWrite(writeError);
    }
    mz_zip_reader_end(&zip);
    return true;
}

bool extractBundle(const QString &zipPath, const QString &destDir, QString *error)
{
    return extractBundleLimited(zipPath, destDir, kMaxTotalBytes, kMaxEntries, error);
}

bool listBundle(const QString &zipPath, QStringList *entries, QString *error)
{
    QByteArray archive;
    if (!readAll(zipPath, &archive, error))
        return false;
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!mz_zip_reader_init_mem(&zip, archive.constData(), size_t(archive.size()), 0)) {
        setError(error, QStringLiteral("Not a zip archive: %1").arg(zipPath));
        return false;
    }
    entries->clear();
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; i++) {
        char name[MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE];
        mz_zip_reader_get_filename(&zip, i, name, sizeof name);
        entries->append(QString::fromUtf8(name));
    }
    mz_zip_reader_end(&zip);
    return true;
}

} // namespace BundleArchive
