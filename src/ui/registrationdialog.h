#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;

// The registration checklist (SPEC.md §6.3). porydaw never edits
// song_table.inc / songs.h / ld_script.ld; this dialog hands the user exact
// copy-paste snippets for them and recheck-verifies each paste by re-parsing
// the file from disk. Snippets are recomputed on every recheck so the
// proposed song ID stays correct even if other songs were added meanwhile.
class RegistrationDialog : public QDialog
{
    Q_OBJECT

public:
    RegistrationDialog(const QString &projectRoot, const QString &label,
                       const QString &constant, const QString &player,
                       QWidget *parent = nullptr);

    // True once all applicable checklist items verified; the caller should
    // reload the project so the song shows up as registered.
    bool registrationComplete() const { return m_complete; }

private:
    void recheck();

    struct Item {
        QLabel *status = nullptr;
        QLineEdit *snippet = nullptr;
    };

    QString m_projectRoot;
    QString m_label;
    QString m_constant;
    QString m_player;
    bool m_complete = false;

    Item m_items[3]; // song_table.inc, songs.h, ld_script.ld
    QLabel *m_summary = nullptr;
};
