#pragma once

#include <QHash>
#include <QSet>
#include <QString>

#include "project/songbundle.h"
#include "project/voicegroupsource.h"

class SongDocument;
struct SmfFile;

namespace SongBundle {

// The programs a song selects: every program-change value on every track,
// plus program 0 for any track that sounds a note before its first program
// change (the engine's default voice).
QSet<int> usedPrograms(const SmfFile &smf);

// The extension opcodes (mid2agb mnemonics, e.g. "PORTAMENTO") the MIDI
// emits — commands a stock m4a engine doesn't implement. Sorted.
QStringList usedExtensions(const SmfFile &smf);

// Writes a song bundle (§3.2 of docs/song-bundle/PLAN.md) from an open song:
// the in-memory document and voicegroup source (so unsaved edits export, like
// Export WAV), trimmed to the programs the song selects, with the closure of
// everything those voices reference copied out of the project.
//
// Refuses — naming the offenders — when a referenced symbol can't be resolved
// to a file (the loader would play it silent, but a bundle must be complete),
// when a referenced sample only exists as .aif, and when the voicegroup
// source is unavailable.
class Exporter
{
  public:
    // doc and vgSource are borrowed for the exporter's lifetime. vgSource may
    // be null (export then refuses).
    Exporter(const QString &projectRoot, const SongDocument &doc, const VoicegroupSource *vgSource);

    // Import prefill hints recorded in the manifest (may stay empty).
    void setRegistrationHints(const QString &constant, const QString &player);
    // Minted-but-unsaved Golden Sun synth definitions (they live in memory
    // until the song is saved); consulted before the project's files.
    void setPendingSynths(const QHash<QString, VgSynthDesc> &pending);

    // Writes the bundle as a directory tree under destDir (created if
    // missing; expected empty). A staged directory is itself a valid bundle.
    bool stage(const QString &destDir, QString *error);
    // stage() into a temporary directory, then zip it to zipPath.
    bool exportTo(const QString &zipPath, QString *error);

    // The manifest of the last successful stage()/exportTo().
    const BundleManifest &manifest() const { return m_manifest; }

  private:
    QString m_root;
    const SongDocument &m_doc;
    const VoicegroupSource *m_vgSource;
    QString m_constant;
    QString m_player;
    QHash<QString, VgSynthDesc> m_pendingSynths;
    BundleManifest m_manifest;
};

} // namespace SongBundle
