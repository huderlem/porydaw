#include "bundleimportdialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

using SongBundle::ImportAction;
using SongBundle::ImportItem;
using SongBundle::ImportPlan;

BundleImportDialog::BundleImportDialog(const QString &bundleRoot, const QString &projectRoot,
                                       const QStringList &players, QWidget *parent)
    : QDialog(parent)
    , m_bundleRoot(bundleRoot)
    , m_projectRoot(projectRoot)
{
    setWindowTitle(tr("Import Song Bundle"));

    auto *layout = new QVBoxLayout(this);
    // The notes block comes and goes (and word-wraps) as the plan changes;
    // the dialog follows its content instead of clipping it.
    layout->setSizeConstraint(QLayout::SetFixedSize);
    auto *intro =
        new QLabel(tr("Import into %1. Anything the project already has is reused, not copied.")
                       .arg(QDir(projectRoot).dirName()),
                   this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("bundleImportSummary"));
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_summary);

    auto *form = new QFormLayout;
    m_label = new QLineEdit(this);
    m_label->setObjectName(QStringLiteral("bundleImportLabel"));
    m_constant = new QLineEdit(this);
    m_constant->setObjectName(QStringLiteral("bundleImportConstant"));
    m_player = new QComboBox(this);
    m_player->setObjectName(QStringLiteral("bundleImportPlayer"));
    m_player->addItems(players);
    form->addRow(tr("Song label:"), m_label);
    form->addRow(tr("Constant:"), m_constant);
    form->addRow(tr("Music player:"), m_player);
    layout->addLayout(form);

    m_notes = new QLabel(this);
    m_notes->setObjectName(QStringLiteral("bundleImportNotes"));
    m_notes->setWordWrap(true);
    m_notes->setTextFormat(Qt::PlainText);
    m_notes->setMinimumWidth(460);
    layout->addWidget(m_notes);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Import"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &BundleImportDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(m_buttons);

    // Planning reads the whole project's voicegroups, so it runs when a field
    // is committed (and again on Import), not on every keystroke.
    connect(m_label, &QLineEdit::editingFinished, this, &BundleImportDialog::replan);
    connect(m_constant, &QLineEdit::textEdited, this, [this] { m_constantEdited = true; });
    connect(m_constant, &QLineEdit::editingFinished, this, &BundleImportDialog::replan);
    connect(m_player, &QComboBox::currentTextChanged, this, &BundleImportDialog::replan);

    // First plan: the bundle's own choices (suffixed past any clash).
    m_plan = SongBundle::makeImportPlan(m_bundleRoot, m_projectRoot);
    {
        const QSignalBlocker blockPlayer(m_player);
        m_label->setText(m_plan.label);
        m_constant->setText(m_plan.constant);
        if (m_player->findText(m_plan.player) < 0 && !m_plan.player.isEmpty())
            m_player->addItem(m_plan.player);
        m_player->setCurrentText(m_plan.player);
    }
    replan();
}

QString BundleImportDialog::summaryText(const ImportPlan &plan)
{
    const auto line = [](const QString &what, const QList<ImportItem> &items) {
        if (items.isEmpty())
            return QString();
        QString text = tr("%1: %2 new, %3 reused")
                           .arg(what)
                           .arg(ImportPlan::count(items, ImportAction::Add) +
                                ImportPlan::count(items, ImportAction::Rename))
                           .arg(ImportPlan::count(items, ImportAction::Reuse));
        QStringList renamed;
        for (const ImportItem &item : items) {
            if (item.action == ImportAction::Rename)
                renamed.append(
                    QStringLiteral("%1 → %2").arg(item.bundleSymbol, item.projectSymbol));
        }
        if (!renamed.isEmpty())
            text += tr(" (renamed: %1)").arg(renamed.join(QStringLiteral(", ")));
        return text + QLatin1Char('\n');
    };
    QString text = line(tr("Samples"), plan.samples) + line(tr("Programmable waves"), plan.waves) +
                   line(tr("Synth instruments"), plan.synths) +
                   line(tr("Keysplit tables"), plan.tables) +
                   line(tr("Keysplit / drumkit voicegroups"), plan.subVoicegroups);
    if (!plan.voicegroup.projectSymbol.isEmpty())
        text += tr("Voicegroup: %1 (new)").arg(plan.voicegroup.projectSymbol);
    return text.trimmed();
}

void BundleImportDialog::replan()
{
    SongBundle::ImportOptions options;
    options.label = m_label->text().trimmed();
    options.player = m_player->currentText();
    if (m_constantEdited)
        options.constant = m_constant->text().trimmed();
    m_plan = SongBundle::makeImportPlan(m_bundleRoot, m_projectRoot, options);
    if (!m_constantEdited && !m_plan.constant.isEmpty())
        m_constant->setText(m_plan.constant);

    m_summary->setText(summaryText(m_plan));
    QStringList notes;
    for (const QString &refusal : m_plan.refusals)
        notes.append(tr("Can't import: %1").arg(refusal));
    for (const QString &warning : m_plan.warnings)
        notes.append(tr("Note: %1").arg(warning));
    m_notes->setText(notes.join(QStringLiteral("\n\n")));
    m_notes->setVisible(!notes.isEmpty());
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(m_plan.ok());
}

void BundleImportDialog::accept()
{
    // The fields may hold an uncommitted edit (Enter pressed inside one).
    replan();
    if (m_plan.ok())
        QDialog::accept();
}
