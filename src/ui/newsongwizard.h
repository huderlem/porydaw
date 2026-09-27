#pragma once

#include <QDialog>
#include <QWizard>

#include <memory>

#include "core/midiimport.h"
#include "core/smf.h"
#include "project/decompproject.h"
#include "project/songregistry.h"

class IdentityPage;
class SoundPage;
class AnalysisPage;
class SongNameFields;
class QDialogButtonBox;

// The New Song wizard (SPEC.md §6.3) and the external-MIDI import flow
// (§6.2) — the same wizard with an extra page in import mode:
//
//   blank:  Identity -> Sound
//   import: Analysis -> Identity -> Sound
//
// The wizard only collects choices; MainWindow writes the .mid + midi.cfg
// line and registers the song in the three registration files.
//
// `voicegroupArgs` is the project's -G choices (SongRegistry::voicegroupArgs);
// the caller passes it in because scanning every voicegroup file is too slow
// to redo per dialog — MainWindow hands over its cached catalog. `reserved`
// is the names open drafts hold (MainWindow::reservedSongNames); the wizard
// rejects them like names the project already uses.
class NewSongWizard : public QWizard
{
    Q_OBJECT

  public:
    // Blank new song.
    NewSongWizard(DecompProject *project, const QStringList &voicegroupArgs,
                  const ReservedSongNames &reserved = {}, QWidget *parent = nullptr);
    // Import: `imported` is the parsed external file (kept as-is apart from
    // the analysis page's optional division rescale).
    NewSongWizard(DecompProject *project, SmfFile imported, const QString &sourcePath,
                  const QStringList &voicegroupArgs, const ReservedSongNames &reserved = {},
                  QWidget *parent = nullptr);

    QString label() const;
    QString constant() const;
    QString player() const;
    SongCfg cfg() const;
    // The song to write: the blank template, or the import with the
    // optional division rescale applied.
    SmfFile songFile() const;
    // Non-empty when the user chose "(create a new voicegroup for this song)"
    // on the Sound page: the voicegroup to create (named after the song; cfg()
    // already carries its -G arg). Empty for an existing voicegroup.
    QString newVoicegroupName() const;

  private:
    void buildPages(const QString &sourcePath, const QStringList &voicegroupArgs);

    DecompProject *m_project;
    ReservedSongNames m_reserved;
    bool m_importMode = false;
    SmfFile m_imported;
    ImportAnalysis m_analysis;

    IdentityPage *m_identity = nullptr;
    SoundPage *m_sound = nullptr;
    AnalysisPage *m_analysisPage = nullptr;
};

// The Rename dialog a draft's commit opens when a name it holds was taken
// since the wizard (docs/draft-songs/PLAN.md D2): the Identity page's name
// rows, decided by the same SongRegistry::checkNewSongNames, under an
// explanation listing conflicts. Its OK button stays disabled while any
// name is taken. renamesVoicegroup: the draft creates a voicegroup named
// after the song, which must be free under the new label too. ownMidPath: a
// .mid the draft's earlier commit attempt wrote (SongDraft::wroteMidPath),
// waived by the check so keeping the label stays possible when only, say,
// the constant conflicts. ownVoicegroupPath: likewise, a voicegroup file
// such an attempt created and failed to finish (SongDraft::
// voicegroupFileCreated). alreadyWritten: the project files (display paths)
// such an earlier attempt already wrote, which the explanation lists in
// place of "The song hasn't been written yet."
class SongRenameDialog : public QDialog
{
    Q_OBJECT

  public:
    SongRenameDialog(const DecompProject *project, const ReservedSongNames &reserved,
                     const QString &label, const QString &constant, bool renamesVoicegroup,
                     const QStringList &conflicts, const QString &ownMidPath = QString(),
                     const QString &ownVoicegroupPath = QString(),
                     const QStringList &alreadyWritten = QStringList(), QWidget *parent = nullptr);
    ~SongRenameDialog() override;

    QString label() const;
    QString constant() const;

  private:
    void refresh();

    std::unique_ptr<SongNameFields> m_names;
    QDialogButtonBox *m_buttons;
    bool m_renamesVoicegroup;
};
