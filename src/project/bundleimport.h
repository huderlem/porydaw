#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include "project/songbundle.h"
#include "project/voicegroupsource.h"

// Importing a song bundle into a decomp project (§3.4 of
// docs/song-bundle/PLAN.md), as plan → apply. The plan is a pure function of
// (bundle, project, options): nothing is written until applyImportPlan, and
// the plan is what the import dialog shows and the harness asserts on.
namespace SongBundle {

enum class ImportAction {
    Add,    // written under the bundle's own symbol
    Reuse,  // the project already has the same content; its symbol is used
    Rename, // written under a new symbol (name clash, or the project's naming rules)
};

// One thing the bundle carries, resolved against the project.
struct ImportItem {
    QString bundleSymbol;
    QString projectSymbol; // what the imported voicegroups reference
    ImportAction action = ImportAction::Add;

    // What apply writes (unused for Reuse):
    QString name;            // file base name in the project (sample / wave / voicegroup)
    QString wavSource;       // sample: absolute path of the bundle's .wav ("" when none)
    QString binSource;       // sample: the raw .bin; wave: the .pcm
    QList<QByteArray> lines; // voicegroup body / keysplit table source, no line endings
    int startingNote = 0;    // voicegroup: the macro header's second argument
    VgSynthDesc synth;       // synth definition
};

// The user's overrides from the import dialog; empty fields take the plan's
// own choice (manifest hints, with _2… suffixes on a clash). A label or
// constant given here that clashes refuses instead of being suffixed.
struct ImportOptions {
    QString label;
    QString constant;
    QString player;
};

struct ImportPlan {
    QString bundleRoot;
    QString projectRoot;
    BundleManifest manifest;

    QList<ImportItem> samples;
    QList<ImportItem> waves;
    QList<ImportItem> synths;
    QList<ImportItem> tables;
    QList<ImportItem> subVoicegroups;
    ImportItem voicegroup; // never reused: the trimmed voicegroup is the song's own

    // The song as it will be registered.
    QString label;
    QString constant;
    QString player;
    QStringList flags; // midi.cfg flags, -G naming the imported voicegroup
    QString midSource; // absolute path of the bundle's .mid

    // Things the user should know but that don't stop the import: engine
    // extensions the project lacks, raw samples, a created table file.
    QStringList warnings;
    // Why the import can't run at all; empty when apply may be called.
    QStringList refusals;

    bool ok() const { return refusals.isEmpty(); }
    static int count(const QList<ImportItem> &items, ImportAction action);
};

// Resolves the bundle at bundleRoot (an extracted .porysong or a bundle
// folder; validated through SongBundle::readSong first) against the project.
// Never writes. Failures of any kind land in ImportPlan::refusals.
ImportPlan makeImportPlan(const QString &bundleRoot, const QString &projectRoot,
                          const ImportOptions &options = ImportOptions());

// Writes the plan into its project: samples, waves, synths, keysplit tables,
// sub-voicegroups, the voicegroup, then the song (.mid, flags, registration)
// — so a failure midway leaves a project that still builds. On failure
// *error names the step and lists what was already written; written (when
// given) receives that list either way. Writes are byte-conservative and
// not undoable, like sample registration.
bool applyImportPlan(const ImportPlan &plan, QString *error, QStringList *written = nullptr);

// Drops the in-memory sample hash cache (keyed by path + mtime + size, so
// this is only for tests that want a cold start).
void clearImportHashCache();

} // namespace SongBundle
