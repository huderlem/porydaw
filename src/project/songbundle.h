#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include "decompproject.h"

// The porysong.json manifest at the root of a song bundle (§3.3 of
// docs/song-bundle/PLAN.md). A bundle is a directory (or a zip of one, see
// BundleArchive) that mirrors a decomp project's sound/ tree; the manifest
// names the song and the symbols the bundle carries so a reader can present
// and import it without re-walking the voicegroup.
struct BundleSample {
    QString name;   // project symbol minus the DirectSoundWaveData_ prefix
    QString file;   // archive path, e.g. sound/direct_sound_samples/<name>.wav
    QString sha256; // hex digest of the file bytes as stored
    bool operator==(const BundleSample &o) const
    {
        return name == o.name && file == o.file && sha256 == o.sha256;
    }
};

struct BundleManifest {
    // Bumped on breaking layout changes; readers refuse a newer format.
    static constexpr int kFormat = 1;

    int format = kFormat;
    QString porydaw; // writer version string

    // "song"
    QString label;      // e.g. mus_route101
    QString voicegroup; // the bundle's (trimmed) top-level voicegroup symbol
    QString flags;      // the midi.cfg flag string with -G pointing at voicegroup
    QString constant;   // import prefill hint (may be empty → JSON null)
    QString player;     // import prefill hint (may be empty → JSON null)

    QString layout; // source project layout, e.g. pokeemerald / pokefirered

    // "requires"
    QStringList extensions; // extension opcodes the MIDI actually emits
    bool synth = false;     // any bundled voice resolves to a set_synth_* def

    QList<BundleSample> samples;
    QStringList subVoicegroups;
    QStringList keysplitTables;
    QStringList waves;
    QStringList synths;

    bool operator==(const BundleManifest &o) const;
    bool operator!=(const BundleManifest &o) const { return !(*this == o); }

    // Serialization. fromJson refuses malformed JSON, a non-object root, a
    // missing/invalid "format", and a format newer than kFormat; optional
    // sections default to empty.
    QByteArray toJson() const;
    static bool fromJson(const QByteArray &json, BundleManifest *manifest, QString *error);

    // File forms over a bundle root (directory).
    bool write(const QString &bundleRoot, QString *error) const;
    static bool read(const QString &bundleRoot, BundleManifest *manifest, QString *error);
};

namespace SongBundle {

// File name of the manifest inside a bundle root.
QString manifestFileName();
QString manifestPath(const QString &bundleRoot);

// True when root holds a porysong.json (the only thing that makes a
// directory a bundle; the manifest's validity is a separate read).
bool isBundleDir(const QString &root);

// Reads a bundle root (an extracted .porysong or a bundle folder) into what
// a tab needs to open it: the manifest, plus a SongInfo synthesized from the
// bundle's own midi.cfg line (the manifest's flag string when that line is
// missing). The song belongs to no project: id -1, unregistered.
//
// A bundle is untrusted input that the voicegroup loader will path-probe,
// so this refuses anything that could reach outside the root: a label or -G
// value that isn't a plain identifier, and any .incbin/.include in the
// bundle's sources whose path is absolute or has a ".." segment.
bool readSong(const QString &bundleRoot, BundleManifest *manifest, SongInfo *song, QString *error);

} // namespace SongBundle
