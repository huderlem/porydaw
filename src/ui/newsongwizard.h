#pragma once

#include <QWizard>

#include "core/midiimport.h"
#include "core/smf.h"
#include "project/decompproject.h"

class AudioEngine;
class IdentityPage;
class SoundPage;
class AnalysisPage;
class MappingPage;

// The New Song wizard (SPEC.md §6.3) and the external-MIDI import flow
// (§6.2) — the same wizard with two extra pages in import mode:
//
//   blank:  Identity -> Sound
//   import: Analysis -> Identity -> Sound -> Mapping
//
// The wizard only collects choices (and remaps programs on the imported
// file); MainWindow writes the .mid + midi.cfg line and then shows the
// registration checklist.
class NewSongWizard : public QWizard
{
    Q_OBJECT

public:
    // Blank new song.
    NewSongWizard(DecompProject *project, AudioEngine *audio, QWidget *parent = nullptr);
    // Import: `imported` is the parsed external file (kept as-is apart from
    // the mapping page's program remaps).
    NewSongWizard(DecompProject *project, AudioEngine *audio, SmfFile imported,
                  const QString &sourcePath, QWidget *parent = nullptr);
    ~NewSongWizard() override;

    QString label() const;
    QString constant() const;
    QString player() const;
    SongCfg cfg() const;
    // The song to write: the blank template, or the import with remaps applied.
    SmfFile songFile() const;

private:
    void buildPages(const QString &sourcePath);

    DecompProject *m_project;
    AudioEngine *m_audio;
    bool m_importMode = false;
    SmfFile m_imported;
    ImportAnalysis m_analysis;

    IdentityPage *m_identity = nullptr;
    SoundPage *m_sound = nullptr;
    AnalysisPage *m_analysisPage = nullptr;
    MappingPage *m_mapping = nullptr;
};
