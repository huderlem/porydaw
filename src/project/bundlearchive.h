#pragma once

#include <QString>
#include <QStringList>

// The .porysong container: a plain zip (vendored miniz) whose contents
// mirror a decomp project's sound/ tree next to a root porysong.json. The
// zip layer only packs and unpacks directories; every other bundle code
// path takes a directory root, so an unpacked bundle is just as valid.
namespace BundleArchive {

// Archive name of the manifest that marks a zip as a bundle.
inline const char *manifestEntryName()
{
    return "porysong.json";
}

// Extraction guards (§3.1 of docs/song-bundle/PLAN.md): a bundle whose
// entries claim more than this in total, or has more entries than this, is
// refused before anything is written.
constexpr qint64 kMaxTotalBytes = qint64(64) << 20;
constexpr int kMaxEntries = 4096;

// Packs every regular file under srcDir into zipPath (deflate). Entries are
// stored in sorted archive-path order with '/' separators and a fixed
// timestamp, so packing identical content yields byte-identical zips.
// Empty directories are not recorded. Refuses a srcDir without a root
// porysong.json, or whose files exceed the extraction limits (the result
// must be extractable). The zip is written atomically (QSaveFile).
bool createBundle(const QString &srcDir, const QString &zipPath, QString *error);

// Unpacks zipPath under destDir (created if missing). Every entry is
// validated before the first byte is written: no absolute paths, drive
// letters, "." / ".." segments, empty segments, control characters,
// segments ending in a dot or space, or names of 512+ UTF-8 bytes
// (backslashes are normalized to '/'); no symlink, encrypted or
// unsupported-method entries; no two names equal ignoring case, nor a file
// named like another entry's directory; entry count and claimed total size
// within the limits; and the root porysong.json must be present. Every
// entry is also inflated and CRC-checked in memory first, so on refusal
// nothing under destDir is created or changed. Only a filesystem error
// mid-write can fail later; the files written so far are then removed
// (the whole destDir when this call created it).
bool extractBundle(const QString &zipPath, const QString &destDir, QString *error);

// Same as extractBundle with explicit limits (harness use).
bool extractBundleLimited(const QString &zipPath, const QString &destDir, qint64 maxTotalBytes,
                          int maxEntries, QString *error);

// Archive-order entry names (files and directory entries alike) — what an
// exporter test compares against its expected file list.
bool listBundle(const QString &zipPath, QStringList *entries, QString *error);

// Validates one archive path against the extraction rules; the normalized
// (forward-slash) form is returned through normalized when valid. Exposed
// for the harness.
bool validateEntryName(const QString &name, QString *normalized, QString *why);

} // namespace BundleArchive
