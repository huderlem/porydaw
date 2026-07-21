#include "sf2zonepicker.h"

#include <QDialogButtonBox>
#include <QHash>
#include <QHeaderView>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "m4asemantics.h"

Sf2ZonePicker::Sf2ZonePicker(const Sf2File &file, QWidget *parent)
    : QDialog(parent)
    , m_file(file)
{
    setWindowTitle(tr("Import Sample — pick a SoundFont zone"));
    auto *layout = new QVBoxLayout(this);

    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("sf2SearchEdit"));
    m_search->setPlaceholderText(
        tr("Search samples, instruments, presets…"));
    m_search->setClearButtonEnabled(true);
    layout->addWidget(m_search);

    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("sf2ZoneTree"));
    m_tree->setColumnCount(6);
    m_tree->setHeaderLabels({tr("Sample"), tr("Key"), tr("Rate"),
                             tr("Frames"), tr("Loop"), tr("Notes")});
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setAllColumnsShowFocus(true);
    layout->addWidget(m_tree, 1);

    m_buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->setObjectName(QStringLiteral("sf2ButtonBox"));
    layout->addWidget(m_buttons);

    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_search, &QLineEdit::textChanged, this,
            [this] { rebuild(); });
    connect(m_tree, &QTreeWidget::currentItemChanged, this,
            [this] { updateSelection(); });
    // Double-click (or Return) on a zone row picks it directly.
    connect(m_tree, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem *item, int) {
                if (item && item->data(0, Qt::UserRole).isValid()) {
                    m_selected = item->data(0, Qt::UserRole).toInt();
                    accept();
                }
            });

    rebuild();
    resize(720, 480);
}

void Sf2ZonePicker::rebuild()
{
    const QString needle = m_search->text().trimmed();
    m_tree->clear();
    QHash<QString, QTreeWidgetItem *> groups;
    for (int i = 0; i < int(m_file.zones.size()); i++) {
        const Sf2Zone &zone = m_file.zones[size_t(i)];
        if (!needle.isEmpty()
            && !zone.name.contains(needle, Qt::CaseInsensitive)
            && !zone.instrument.contains(needle, Qt::CaseInsensitive)
            && !zone.preset.contains(needle, Qt::CaseInsensitive))
            continue;
        QString label = zone.instrument.isEmpty() ? tr("(no instrument)")
                                                  : zone.instrument;
        if (!zone.preset.isEmpty())
            label += QStringLiteral(" — ") + zone.preset;
        QTreeWidgetItem *group = groups.value(label);
        if (!group) {
            group = new QTreeWidgetItem(m_tree, {label});
            group->setFlags(Qt::ItemIsEnabled);
            groups.insert(label, group);
        }
        auto *item = new QTreeWidgetItem(group);
        item->setText(0, zone.name);
        if (zone.originalPitch > 127) {
            item->setText(1, tr("—")); // unpitched convention
        } else {
            QString key = midiKeyName(zone.originalPitch);
            if (zone.pitchCorrection != 0)
                key += QStringLiteral(" %1%2¢")
                           .arg(zone.pitchCorrection > 0
                                    ? QStringLiteral("+")
                                    : QString())
                           .arg(zone.pitchCorrection);
            item->setText(1, key);
        }
        item->setText(2, QStringLiteral("%1 Hz").arg(zone.sampleRate));
        item->setText(3, QString::number(zone.frames()));
        item->setText(4, zone.hasLoop()
                             ? QStringLiteral("%1–%2")
                                   .arg(zone.loopStart - zone.start)
                                   .arg(zone.loopEndExcl - zone.start - 1)
                             : QStringLiteral("—"));
        item->setText(5,
                      zone.stereoPair() ? tr("stereo pair") : QString());
        item->setData(0, Qt::UserRole, i);
    }
    m_tree->expandAll();
    for (int c = 0; c < m_tree->columnCount(); c++)
        m_tree->resizeColumnToContents(c);
    updateSelection();
}

void Sf2ZonePicker::updateSelection()
{
    const QTreeWidgetItem *current = m_tree->currentItem();
    const QVariant zone =
        current ? current->data(0, Qt::UserRole) : QVariant();
    m_selected = zone.isValid() ? zone.toInt() : -1;
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(m_selected >= 0);
}
