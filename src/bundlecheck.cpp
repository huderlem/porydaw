#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <cstdio>
#include <cstring>

#include "miniz.h"
#include "project/bundlearchive.h"
#include "project/songbundle.h"

// --bundlecheck <scratchDir>: song-bundle container + manifest check
// (docs/song-bundle/PLAN.md Phase 0), self-contained. Sections: zip
// round-trip (pack a tree, list, unpack byte-exact, re-pack byte-identical,
// pack of the unpacked tree byte-identical), extraction guards (path
// traversal, absolute paths, drive letters, backslash normalization,
// symlink / encrypted / unsupported entries, entry-count and total-size
// caps, missing manifest, file-vs-directory clash, case-folded duplicates,
// trailing dot/space, over-long UTF-8 names, corrupt payload — each refusal
// leaves the destination untouched), manifest round-trip (every field, null hints,
// omitted optional sections), and format refusal (newer format, missing
// format, malformed JSON, non-object root), then the Phase 1 export sections
// (bundleexportcheck.cpp). scratchDir must not already exist.

// bundleexportcheck.cpp: the Phase 1 export sections.
void runBundleExportSections(const QString &scratchDir, int *failures);

namespace {

int failures = 0;

void expect(bool ok, const char *what)
{
    if (!ok) {
        std::fprintf(stderr, "bundlecheck: FAIL: %s\n", what);
        failures++;
    }
}

void expectMsg(bool ok, const QString &what)
{
    expect(ok, qUtf8Printable(what));
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

// Sorted relative paths of every file under root.
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

struct RawEntry {
    QByteArray name;
    QByteArray bytes;
};

// Walks the central directory, handing each entry's central-header offset
// and local-header offset to fn(index, centralOfs, localOfs).
template <typename Fn>
bool forEachCentralEntry(QByteArray *archive, Fn fn)
{
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!mz_zip_reader_init_mem(&zip, archive->constData(), size_t(archive->size()), 0))
        return false;
    qint64 ofs = qint64(zip.m_central_directory_file_ofs);
    const int n = int(mz_zip_reader_get_num_files(&zip));
    mz_zip_reader_end(&zip);
    const auto *bytes = reinterpret_cast<const unsigned char *>(archive->constData());
    for (int i = 0; i < n; i++) {
        if (ofs + 46 > archive->size())
            return false;
        const unsigned char *h = bytes + ofs;
        const qint64 localOfs = h[42] | (h[43] << 8) | (h[44] << 16) | (qint64(h[45]) << 24);
        fn(i, ofs, localOfs);
        ofs += 46 + (h[28] | (h[29] << 8)) + (h[30] | (h[31] << 8)) + (h[32] | (h[33] << 8));
    }
    return true;
}

// A zip built straight from miniz, so the harness can name entries the
// packer would never produce. Names go in as same-length placeholders
// (miniz refuses leading slashes) and are patched into both headers after
// the fact; the placeholder for entry i is unique so miniz's writer never
// sees duplicates either.
QByteArray craftZip(const QList<RawEntry> &entries)
{
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!mz_zip_writer_init_heap(&zip, 0, 0))
        return QByteArray();
    for (int i = 0; i < entries.size(); i++) {
        const RawEntry &e = entries[i];
        QByteArray placeholder(e.name.size(), 'p');
        if (!placeholder.isEmpty())
            placeholder[0] = char('A' + (i % 26));
        if (!mz_zip_writer_add_mem(&zip, placeholder.constData(), e.bytes.constData(),
                                   size_t(e.bytes.size()), MZ_DEFAULT_LEVEL)) {
            mz_zip_writer_end(&zip);
            return QByteArray();
        }
    }
    void *buf = nullptr;
    size_t size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&zip, &buf, &size)) {
        mz_zip_writer_end(&zip);
        return QByteArray();
    }
    QByteArray out(static_cast<const char *>(buf), int(size));
    mz_free(buf);
    mz_zip_writer_end(&zip);
    forEachCentralEntry(&out, [&](int i, qint64 centralOfs, qint64 localOfs) {
        const QByteArray &name = entries[i].name;
        std::memcpy(out.data() + centralOfs + 46, name.constData(), size_t(name.size()));
        std::memcpy(out.data() + localOfs + 30, name.constData(), size_t(name.size()));
    });
    return out;
}

// Overwrites a little-endian field of central-directory entry `index`
// (fieldOfs from the 46-byte header start) — how the harness forges
// symlink / encrypted / unsupported-method entries.
bool patchCentralDir(QByteArray *archive, int index, int fieldOfs, quint32 value, int width)
{
    bool patched = false;
    forEachCentralEntry(archive, [&](int i, qint64 centralOfs, qint64) {
        if (i != index)
            return;
        auto *bytes = reinterpret_cast<unsigned char *>(archive->data());
        for (int b = 0; b < width; b++)
            bytes[centralOfs + fieldOfs + b] = (value >> (8 * b)) & 0xff;
        patched = true;
    });
    return patched;
}

const QByteArray kManifestJson = "{\"format\":1,\"song\":{\"label\":\"mus_x\",\"voicegroup\":"
                                 "\"voicegroup_x\",\"flags\":\"-G voicegroup_x\"}}";

} // namespace

int runBundleCheck(const QString &scratchDir)
{
    if (QDir(scratchDir).exists()) {
        std::fprintf(stderr,
                     "bundlecheck: scratch dir %s already exists; give a "
                     "fresh path\n",
                     qUtf8Printable(scratchDir));
        return 1;
    }
    QDir().mkpath(scratchDir);
    int caseNo = 0;
    const auto fresh = [&](const char *tag) {
        return scratchDir + QStringLiteral("/%1_%2").arg(++caseNo).arg(QLatin1String(tag));
    };

    { // ---- zip round-trip -------------------------------------------------
        const QString src = fresh("src");
        QByteArray binary;
        for (int i = 0; i < 70000; i++)
            binary.append(char((i * 7 + (i >> 8)) & 0xff)); // crosses the 64 KiB block size
        QByteArray zeros(150000, '\0');
        writeFile(src + "/porysong.json", kManifestJson);
        writeFile(src + "/sound/songs/midi/mus_x.mid", binary);
        writeFile(src + "/sound/songs/midi/midi.cfg", "mus_x: -G voicegroup_x\r\n");
        writeFile(src + "/sound/direct_sound_samples/zeros.wav", zeros);
        writeFile(src + "/sound/voicegroups/voicegroup_x.inc", "\tvoice_square_1 60, 0, 0, 2\n");
        writeFile(src + "/sound/empty.inc", QByteArray()); // zero-length entry
        writeFile(src + "/.hidden", "h");                  // hidden files pack too

        const QString zip = fresh("bundle") + ".porysong";
        QString error;
        expectMsg(BundleArchive::createBundle(src, zip, &error),
                  QStringLiteral("createBundle succeeds: %1").arg(error));
        QStringList entries;
        expect(BundleArchive::listBundle(zip, &entries, &error), "listBundle succeeds");
        const QStringList expected = {".hidden",
                                      "porysong.json",
                                      "sound/direct_sound_samples/zeros.wav",
                                      "sound/empty.inc",
                                      "sound/songs/midi/midi.cfg",
                                      "sound/songs/midi/mus_x.mid",
                                      "sound/voicegroups/voicegroup_x.inc"};
        expectMsg(entries == expected, QStringLiteral("entries are sorted archive paths: %1")
                                           .arg(entries.join(QLatin1Char(' '))));
        const QByteArray zipBytes = readFileBytes(zip);
        expect(zipBytes.size() < 20000, "deflate actually compressed the zeros/binary");
        // Every entry carries the fixed 1980-01-01 timestamp in both headers.
        {
            mz_zip_archive r;
            mz_zip_zero_struct(&r);
            expect(mz_zip_reader_init_mem(&r, zipBytes.constData(), size_t(zipBytes.size()), 0),
                   "miniz reads the packed zip back");
            const auto *b = reinterpret_cast<const unsigned char *>(zipBytes.constData());
            bool stamped = true;
            for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&r); i++) {
                mz_zip_archive_file_stat st;
                if (!mz_zip_reader_file_stat(&r, i, &st))
                    continue;
                const unsigned char *l = b + st.m_local_header_ofs + 10;
                stamped = stamped && l[0] == 0 && l[1] == 0 && l[2] == 0x21 && l[3] == 0;
            }
            qint64 ofs = qint64(r.m_central_directory_file_ofs);
            for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&r); i++) {
                const unsigned char *h = b + ofs;
                stamped = stamped && h[12] == 0 && h[13] == 0 && h[14] == 0x21 && h[15] == 0;
                ofs +=
                    46 + (h[28] | (h[29] << 8)) + (h[30] | (h[31] << 8)) + (h[32] | (h[33] << 8));
            }
            expect(stamped, "every local + central header carries the fixed 1980-01-01 stamp");
            expect(mz_zip_validate_archive(&r, 0), "miniz validates the packed archive");
            mz_zip_reader_end(&r);
        }

        const QString dest = fresh("extract") + "/nested/deeper"; // created on demand
        expectMsg(BundleArchive::extractBundle(zip, dest, &error),
                  QStringLiteral("extractBundle succeeds: %1").arg(error));
        expect(treeFiles(dest) == treeFiles(src), "extracted tree has exactly the source files");
        bool same = true;
        for (const QString &rel : treeFiles(src))
            same = same && readFileBytes(src + "/" + rel) == readFileBytes(dest + "/" + rel);
        expect(same, "every extracted file is byte-exact");

        const QString zip2 = fresh("again") + ".porysong";
        expect(BundleArchive::createBundle(src, zip2, &error), "second createBundle succeeds");
        expect(readFileBytes(zip2) == zipBytes, "re-pack of the same tree is byte-identical");
        const QString zip3 = fresh("fromextract") + ".porysong";
        expect(BundleArchive::createBundle(dest, zip3, &error),
               "createBundle from the extracted tree succeeds");
        expect(readFileBytes(zip3) == zipBytes, "pack of the extracted tree is byte-identical");

        // Packing refuses a tree without a root manifest.
        const QString bare = fresh("bare");
        writeFile(bare + "/sound/x.inc", "x");
        expect(!BundleArchive::createBundle(bare, fresh("bare") + ".porysong", &error) &&
                   error.contains("porysong.json"),
               "createBundle refuses a tree without porysong.json");
        // Overwriting an existing zip works (QSaveFile replaces it).
        writeFile(zip2, "stale");
        expect(BundleArchive::createBundle(src, zip2, &error) && readFileBytes(zip2) == zipBytes,
               "createBundle replaces an existing file");
    }

    { // ---- extraction guards ----------------------------------------------
        const RawEntry manifest{"porysong.json", kManifestJson};
        const auto refused = [&](const char *tag, const QByteArray &zipBytes, const char *needle,
                                 qint64 maxBytes = BundleArchive::kMaxTotalBytes,
                                 int maxEntries = BundleArchive::kMaxEntries) {
            const QString zip = fresh(tag) + ".zip";
            writeFile(zip, zipBytes);
            const QString dest = fresh(tag) + "_out";
            QString error;
            const bool ok =
                BundleArchive::extractBundleLimited(zip, dest, maxBytes, maxEntries, &error);
            expectMsg(!ok, QStringLiteral("%1: extraction refused").arg(QLatin1String(tag)));
            expectMsg(error.contains(QLatin1String(needle)),
                      QStringLiteral("%1: message names the reason (\"%2\"): %3")
                          .arg(QLatin1String(tag), QLatin1String(needle), error));
            expectMsg(!QDir(dest).exists(),
                      QStringLiteral("%1: nothing written on refusal").arg(QLatin1String(tag)));
        };

        refused("traversal", craftZip({manifest, {"../evil.txt", "x"}}), "relative path segment");
        refused("traversal-mid", craftZip({manifest, {"sound/../../evil.txt", "x"}}),
                "relative path segment");
        refused("dot-segment", craftZip({manifest, {"sound/./x.inc", "x"}}),
                "relative path segment");
        refused("absolute", craftZip({manifest, {"/etc/evil", "x"}}), "absolute");
        refused("drive", craftZip({manifest, {"C:\\evil.txt", "x"}}), "drive letter");
        refused("backslash-traversal", craftZip({manifest, {"..\\evil.txt", "x"}}),
                "relative path segment");
        refused("empty-segment", craftZip({manifest, {"sound//x.inc", "x"}}), "empty path segment");
        refused("control-char", craftZip({manifest, {QByteArray("sound/x\n.inc"), "x"}}),
                "control character");
        refused("no-manifest", craftZip({{"sound/x.inc", "x"}}), "porysong.json");
        refused("manifest-not-root", craftZip({{"sub/porysong.json", kManifestJson}}),
                "porysong.json");
        refused("not-a-zip", QByteArray("this is not a zip archive at all"), "not a zip");
        refused("file-dir-clash", craftZip({manifest, {"sound", "x"}, {"sound/x.inc", "x"}}),
                "both a file and a directory");
        refused("duplicate", craftZip({manifest, {"sound/x.inc", "x"}, {"sound\\x.inc", "y"}}),
                "duplicate");
        refused("too-many-entries", craftZip({manifest, {"a", "1"}, {"b", "2"}, {"c", "3"}}),
                "entry limit", BundleArchive::kMaxTotalBytes, 3);
        refused("too-large", craftZip({manifest, {"big", QByteArray(5000, 'z')}}), "limit", 4096,
                BundleArchive::kMaxEntries);
        refused("too-large-summed",
                craftZip({manifest, {"a", QByteArray(3000, 'z')}, {"b", QByteArray(3000, 'y')}}),
                "limit", 4096, BundleArchive::kMaxEntries);
        {
            QByteArray z = craftZip({manifest, {"sound/link", "target"}});
            expect(patchCentralDir(&z, 1, 38, quint32(0120777u) << 16, 4),
                   "forged a symlink entry");
            refused("symlink", z, "symbolic link");
        }
        {
            QByteArray z = craftZip({manifest, {"sound/enc", "secret"}});
            expect(patchCentralDir(&z, 1, 8, 1, 2), "forged an encrypted entry");
            refused("encrypted", z, "encrypted");
        }
        {
            QByteArray z = craftZip({manifest, {"sound/lzma", "data"}});
            expect(patchCentralDir(&z, 1, 10, 14, 2), "forged an unsupported-method entry");
            refused("unsupported-method", z, "unsupported compression");
        }
        // Case-insensitive filesystems would merge these.
        refused("duplicate-case",
                craftZip({manifest, {"sound/Foo.wav", "x"}, {"sound/foo.wav", "y"}}), "duplicate");
        refused("file-dir-clash-case", craftZip({manifest, {"Sound", "x"}, {"sound/x.inc", "x"}}),
                "both a file and a directory");
        refused("trailing-dot", craftZip({manifest, {"sound/x.inc.", "x"}}), "trailing dot");
        refused("trailing-space", craftZip({manifest, {"sound /x.inc", "x"}}), "trailing dot");
        // Names are limited in UTF-8 bytes, not characters: 300 x U+00E9 is
        // 600 bytes, which miniz would otherwise truncate mid-name.
        {
            const QString longName = QStringLiteral("sound/") + QString(300, QChar(0x00e9));
            refused("long-name", craftZip({manifest, {longName.toUtf8(), "x"}}), "too long");
            QString why;
            expect(!BundleArchive::validateEntryName(longName, nullptr, &why) &&
                       why.contains("too long"),
                   "validateEntryName counts UTF-8 bytes");
            expect(BundleArchive::validateEntryName(QString(300, QLatin1Char('a')), nullptr, &why),
                   "a 300-byte ASCII name is fine");
        }
        // A payload that only fails while inflating (metadata intact) is
        // refused before the manifest ahead of it reaches the disk.
        {
            QByteArray payload;
            for (int i = 0; i < 4000; i++)
                payload.append(char((i * 31 + (i >> 3)) & 0xff));
            QByteArray z = craftZip({manifest, {"sound/corrupt.bin", payload}});
            qint64 dataOfs = -1;
            forEachCentralEntry(&z, [&](int i, qint64, qint64 localOfs) {
                if (i != 1)
                    return;
                const auto *l = reinterpret_cast<const unsigned char *>(z.constData()) + localOfs;
                dataOfs = localOfs + 30 + (l[26] | (l[27] << 8)) + (l[28] | (l[29] << 8));
            });
            expect(dataOfs > 0 && dataOfs + 40 < z.size(), "located the payload to corrupt");
            if (dataOfs > 0 && dataOfs + 40 < z.size()) {
                for (int b = 20; b < 40; b++)
                    z[int(dataOfs) + b] = char(z[int(dataOfs) + b] ^ 0x5a);
                refused("corrupt-payload", z, "cannot extract");
            }
        }
        // Into an existing directory, a refusal leaves its contents alone.
        {
            const QString zip = fresh("existing") + ".zip";
            writeFile(zip, craftZip({manifest, {"../evil.txt", "x"}}));
            const QString dest = fresh("existing") + "_out";
            writeFile(dest + "/keep.txt", "keep");
            QString error;
            expect(!BundleArchive::extractBundle(zip, dest, &error) &&
                       treeFiles(dest) == QStringList{"keep.txt"},
                   "refusal leaves an existing destination untouched");
        }
        // A file larger than the cap plus header slack is refused before
        // it is even read.
        {
            const QString zip = fresh("oversize-file") + ".zip";
            writeFile(zip, QByteArray(5 * 1024 * 1024 + 4096, 'q'));
            QString error;
            expect(!BundleArchive::extractBundleLimited(zip, fresh("oversize-file") + "_out",
                                                        1024 * 1024, 10, &error) &&
                       error.contains("larger than"),
                   "over-cap archive file refused up front");
        }

        // Accepted shapes: backslashes normalize, directory entries are
        // skipped, dotfiles and Unicode names extract.
        {
            const QString zip = fresh("normalize") + ".zip";
            writeFile(zip, craftZip({manifest,
                                     {"sound\\voicegroups\\vg.inc", "vg"},
                                     {"sound/", QByteArray()},
                                     {"sound/direct_sound_samples/", QByteArray()},
                                     {".dot", "d"},
                                     {QString::fromUtf8("sound/caf\u00e9.wav").toUtf8(), "c"}}));
            const QString dest = fresh("normalize") + "_out";
            QString error;
            expectMsg(BundleArchive::extractBundle(zip, dest, &error),
                      QStringLiteral("normalizing extraction succeeds: %1").arg(error));
            const QStringList files = treeFiles(dest);
            const QStringList want = {".dot", "porysong.json",
                                      QString::fromUtf8("sound/caf\u00e9.wav"),
                                      "sound/voicegroups/vg.inc"};
            expectMsg(files == want,
                      QStringLiteral("normalized tree: %1").arg(files.join(QLatin1Char(' '))));
            expect(readFileBytes(dest + "/sound/voicegroups/vg.inc") == "vg",
                   "backslash entry landed under the forward-slash path");
            expect(QDir(dest + "/sound/direct_sound_samples").exists() == false,
                   "bare directory entries are not materialized");
        }
        // The defaults are the documented caps.
        expect(BundleArchive::kMaxTotalBytes == qint64(64) << 20, "default size cap is 64 MiB");
        expect(BundleArchive::kMaxEntries == 4096, "default entry cap is 4096");
        expect(std::strcmp(BundleArchive::manifestEntryName(), "porysong.json") == 0,
               "manifest entry name");
    }

    { // ---- manifest round-trip --------------------------------------------
        BundleManifest m;
        m.porydaw = "1.2.0";
        m.label = "mus_route101";
        m.voicegroup = "voicegroup_route101";
        m.flags = "-E -R50 -G voicegroup_route101 -V080";
        m.constant = "MUS_ROUTE101";
        m.player = "MPlayInfo_BGM";
        m.layout = "pokeemerald";
        m.extensions = {"PORTAMENTO", "PWMC"};
        m.synth = true;
        m.samples = {{"route101_flute", "sound/direct_sound_samples/route101_flute.wav",
                      QString(64, QLatin1Char('a'))},
                     {"cry_x", "sound/direct_sound_samples/cry_x.wav", QString(64, 'b')}};
        m.subVoicegroups = {"voicegroup_route101_drums"};
        m.keysplitTables = {"KeySplitTable_route101"};
        m.waves = {"ProgrammableWaveData_1"};
        m.synths = {"DirectSoundSynth_GoldenSun_80_00_00_00"};

        const QByteArray json = m.toJson();
        BundleManifest back;
        QString error;
        expectMsg(BundleManifest::fromJson(json, &back, &error),
                  QStringLiteral("fromJson(toJson) succeeds: %1").arg(error));
        expect(back == m, "full manifest survives a JSON round-trip");
        expect(back.format == BundleManifest::kFormat && back.format == 1, "format is 1");
        expect(m.toJson() == json, "toJson is deterministic");
        expect(json.contains("\"constant\": \"MUS_ROUTE101\""), "hint written as a string");

        // Empty hints serialize as null and read back empty; optional
        // sections may be omitted entirely.
        BundleManifest minimal;
        minimal.label = "mus_min";
        minimal.voicegroup = "voicegroup_min";
        const QByteArray minJson = minimal.toJson();
        expect(minJson.contains("\"constant\": null") && minJson.contains("\"player\": null"),
               "empty hints serialize as null");
        BundleManifest minBack;
        expect(BundleManifest::fromJson(minJson, &minBack, &error) && minBack == minimal,
               "minimal manifest round-trips");
        BundleManifest sparse;
        expect(BundleManifest::fromJson(kManifestJson, &sparse, &error) &&
                   sparse.label == "mus_x" && sparse.samples.isEmpty() &&
                   sparse.extensions.isEmpty() && !sparse.synth && sparse.constant.isEmpty(),
               "omitted optional sections read as empty");

        // File forms over a bundle root.
        const QString root = fresh("manifest");
        expect(!SongBundle::isBundleDir(root), "fresh dir is not a bundle");
        expectMsg(m.write(root, &error), QStringLiteral("write: %1").arg(error));
        expect(SongBundle::isBundleDir(root), "written manifest makes it a bundle dir");
        expect(SongBundle::manifestPath(root) == root + "/porysong.json", "manifest path");
        BundleManifest fromFile;
        expect(BundleManifest::read(root, &fromFile, &error) && fromFile == m,
               "read(write) round-trips through the file");
        expect(readFileBytes(root + "/porysong.json") == json, "file holds exactly toJson()");
        expect(!BundleManifest::read(fresh("nodir"), &fromFile, &error) &&
                   error.contains("porysong.json"),
               "read of a non-bundle dir fails naming the manifest");
    }

    { // ---- format refusal -------------------------------------------------
        BundleManifest out;
        QString error;
        const auto refuses = [&](const char *tag, const QByteArray &json, const char *needle) {
            error.clear();
            const bool ok = BundleManifest::fromJson(json, &out, &error);
            expectMsg(!ok && error.contains(QLatin1String(needle)),
                      QStringLiteral("%1 refused with \"%2\": %3")
                          .arg(QLatin1String(tag), QLatin1String(needle), error));
        };
        refuses("newer format", "{\"format\":2,\"song\":{\"label\":\"a\",\"voicegroup\":\"b\"}}",
                "format 2");
        refuses("missing format", "{\"song\":{\"label\":\"a\",\"voicegroup\":\"b\"}}", "format");
        refuses("string format",
                "{\"format\":\"1\",\"song\":{\"label\":\"a\",\"voicegroup\":\"b\"}}", "format");
        refuses("fractional format",
                "{\"format\":1.5,\"song\":{\"label\":\"a\",\"voicegroup\":\"b\"}}", "format");
        refuses("zero format", "{\"format\":0,\"song\":{\"label\":\"a\",\"voicegroup\":\"b\"}}",
                "format");
        refuses("malformed", "{\"format\":1,", "porysong.json");
        refuses("array root", "[1,2]", "not an object");
        refuses("missing song", "{\"format\":1}", "song");
        refuses("unlabeled song", "{\"format\":1,\"song\":{\"voicegroup\":\"b\"}}", "label");
        refuses("bad samples",
                "{\"format\":1,\"song\":{\"label\":\"a\",\"voicegroup\":\"b\"},"
                "\"samples\":[\"x\"]}",
                "samples");
        refuses("bad list",
                "{\"format\":1,\"song\":{\"label\":\"a\",\"voicegroup\":\"b\"},"
                "\"waves\":[1]}",
                "waves");
        // The newer-format refusal also fires through the file form.
        const QString root = fresh("newer");
        writeFile(root + "/porysong.json",
                  "{\"format\":99,\"song\":{\"label\":\"a\",\"voicegroup\":\"b\"}}");
        expect(SongBundle::isBundleDir(root),
               "unreadable-but-present manifest is still a bundle dir");
        expect(!BundleManifest::read(root, &out, &error) && error.contains("format 99") &&
                   error.contains("Update porydaw"),
               "read refuses format 99 and tells the user to update");
    }

    runBundleExportSections(scratchDir, &failures);

    std::printf("bundlecheck: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
