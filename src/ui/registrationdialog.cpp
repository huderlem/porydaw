#include "registrationdialog.h"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "project/songregistry.h"

namespace {

const char *kFileNames[3] = {"sound/song_table.inc", "include/constants/songs.h",
                             "ld_script.ld"};

} // namespace

RegistrationDialog::RegistrationDialog(const QString &projectRoot, const QString &label,
                                       const QString &constant, const QString &player,
                                       QWidget *parent)
    : QDialog(parent), m_projectRoot(projectRoot), m_label(label), m_constant(constant),
      m_player(player)
{
    setWindowTitle(tr("Register %1").arg(label));
    setMinimumWidth(560);

    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(
        tr("porydaw wrote <b>sound/songs/midi/%1.mid</b> and its midi.cfg line. "
           "To make the song playable in-game, paste each snippet below into the "
           "file shown, then press <b>Recheck</b>. porydaw never edits these three "
           "files itself.")
            .arg(label),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    const QString hints[3] = {
        tr("Add at the end of the song list (before the trailing .align)."),
        tr("Add after the last song constant."),
        tr("Add at the end of the song_data section."),
    };

    auto *grid = new QGridLayout;
    grid->setColumnStretch(1, 1);
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    for (int i = 0; i < 3; i++) {
        auto *status = new QLabel(this);
        auto *file = new QLabel(QStringLiteral("<b>%1</b>").arg(QLatin1String(kFileNames[i])), this);
        file->setToolTip(hints[i]);
        auto *snippet = new QLineEdit(this);
        snippet->setReadOnly(true);
        snippet->setFont(mono);
        snippet->setToolTip(hints[i]);
        auto *copy = new QPushButton(tr("Copy"), this);
        connect(copy, &QPushButton::clicked, this, [snippet] {
            QApplication::clipboard()->setText(snippet->text());
        });

        grid->addWidget(status, i * 2, 0);
        grid->addWidget(file, i * 2, 1, 1, 2);
        grid->addWidget(snippet, i * 2 + 1, 1);
        grid->addWidget(copy, i * 2 + 1, 2);
        m_items[i] = {status, snippet};
    }
    layout->addLayout(grid);

    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto *recheckButton = buttons->addButton(tr("Recheck"), QDialogButtonBox::ActionRole);
    connect(recheckButton, &QPushButton::clicked, this, &RegistrationDialog::recheck);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    recheck();
}

void RegistrationDialog::recheck()
{
    const RegistrationPlan plan =
        SongRegistry::makePlan(m_projectRoot, m_label, m_constant, m_player);
    const RegistrationStatus status =
        SongRegistry::checkRegistration(m_projectRoot, m_label, m_constant);

    const bool done[3] = {status.inSongTable, status.inSongsH,
                          status.inLdScript || !status.ldApplicable};
    const QString snippets[3] = {plan.songTableSnippet, plan.songsHSnippet,
                                 plan.ldSnippet};
    for (int i = 0; i < 3; i++) {
        m_items[i].status->setText(done[i] ? QStringLiteral("✅") : QStringLiteral("⬜"));
        m_items[i].snippet->setText(snippets[i]);
        m_items[i].snippet->setEnabled(!done[i]);
    }
    if (!status.ldApplicable)
        m_items[2].snippet->setPlaceholderText(
            tr("This project's linker script needs no per-song line."));

    m_complete = status.complete();
    if (m_complete) {
        SongRegistry::clearRegistrationMeta(m_projectRoot, m_label);
        m_summary->setText(tr("<b>%1 is fully registered!</b> Build your project and "
                              "play it in-game with %2.")
                               .arg(m_label, m_constant));
    } else {
        m_summary->setText(tr("Song ID will be <b>%1</b>. You can close this window "
                              "and finish later — the song shows a badge in the song "
                              "browser until registration is complete.")
                               .arg(plan.songId));
    }
}
