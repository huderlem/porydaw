#pragma once

#include <QDialog>

#include "project/bundleimport.h"

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;

// Import Song Bundle (docs/song-bundle/PLAN.md §3.4): shows what importing
// the bundle would add to, reuse from and rename in the open project, lets
// the user override the song's label / constant / music player, and carries
// the warnings (engine extensions…) or the refusal. Nothing is written here:
// the caller applies plan() after the dialog is accepted.
class BundleImportDialog : public QDialog
{
    Q_OBJECT

  public:
    BundleImportDialog(const QString &bundleRoot, const QString &projectRoot,
                       const QStringList &players, QWidget *parent = nullptr);

    // The plan for the fields as they stand; ok() whenever the dialog was
    // accepted.
    const SongBundle::ImportPlan &plan() const { return m_plan; }

    // The summary block for a plan (also the harness's view of the dialog).
    static QString summaryText(const SongBundle::ImportPlan &plan);

    void accept() override;

  private:
    void replan();

    QString m_bundleRoot;
    QString m_projectRoot;
    SongBundle::ImportPlan m_plan;
    bool m_constantEdited = false;
    QLabel *m_summary;
    QLabel *m_notes;
    QLineEdit *m_label;
    QLineEdit *m_constant;
    QComboBox *m_player;
    QDialogButtonBox *m_buttons;
};
