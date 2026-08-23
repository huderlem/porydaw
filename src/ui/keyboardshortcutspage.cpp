#include "keyboardshortcutspage.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "keymap.h"
#include "layout.h"

namespace {

// The command id lives on the item so filtering/sorting never desyncs it.
constexpr int kIdRole = Qt::UserRole;

QString modifierDisplayText(Qt::KeyboardModifiers mods)
{
#ifdef Q_OS_MACOS
    // Qt swaps the pair on macOS: ControlModifier is the ⌘ Command key and
    // MetaModifier is the ⌃ Control key. Glyph order is Apple's canonical
    // ⌃⌥⇧⌘, matching what QKeySequence::NativeText renders for sequences.
    QString text;
    if (mods.testFlag(Qt::MetaModifier))
        text += QStringLiteral("⌃");
    if (mods.testFlag(Qt::AltModifier))
        text += QStringLiteral("⌥");
    if (mods.testFlag(Qt::ShiftModifier))
        text += QStringLiteral("⇧");
    if (mods.testFlag(Qt::ControlModifier))
        text += QStringLiteral("⌘");
    return text;
#else
    return keymap::Registry::modifierText(mods);
#endif
}

QKeySequence capturedKeySequence(const QKeySequence &sequence)
{
    if (sequence.isEmpty())
        return {};
    auto chord = sequence[0];
    // Registry::matches() masks the keypad flag out of every event —
    // "bindings never carry it" — so a capture that kept the flag (macOS
    // nav keys, numpad arrows elsewhere) would store a binding that can
    // never fire.
    auto modifiers = chord.keyboardModifiers();
    modifiers.setFlag(Qt::KeypadModifier, false);
    return QKeySequence(QKeyCombination(modifiers, chord.key()));
}

QString bindingText(const QList<QKeySequence> &sequences)
{
    QStringList parts;
    for (const QKeySequence &seq : sequences)
        parts.append(seq.toString(QKeySequence::NativeText));
    return parts.join(QStringLiteral(", "));
}

} // namespace

KeyboardShortcutsPage::KeyboardShortcutsPage(QWidget *parent) : QWidget(parent)
{
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Filter commands…"));
    m_filter->setClearButtonEnabled(true);
    connect(m_filter, &QLineEdit::textChanged, this, &KeyboardShortcutsPage::applyFilter);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({tr("Command"), tr("Shortcut")});
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setAllColumnsShowFocus(true);
    m_tree->header()->setStretchLastSection(true);
    connect(m_tree, &QTreeWidget::currentItemChanged, this,
            &KeyboardShortcutsPage::currentRowChanged);

    m_capture = new QKeySequenceEdit(this);
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    m_capture->setMaximumSequenceLength(1);
#endif
    connect(m_capture, &QKeySequenceEdit::keySequenceChanged, this,
            &KeyboardShortcutsPage::captureChanged);

    // QKeySequenceEdit cannot record a bare modifier chord, so modifier
    // commands pick theirs from a list instead (filled per command, since
    // not every chord is offerable everywhere — see fillModifierChoices).
    m_modCapture = new QComboBox(this);
    m_modCapture->hide();
    connect(m_modCapture, &QComboBox::activated, this, &KeyboardShortcutsPage::captureChanged);

    m_assignButton = new QPushButton(tr("&Assign"), this);
    connect(m_assignButton, &QPushButton::clicked, this, &KeyboardShortcutsPage::assign);
    m_clearButton = new QPushButton(tr("&Unbind"), this);
    connect(m_clearButton, &QPushButton::clicked, this, &KeyboardShortcutsPage::clearBinding);
    m_resetButton = new QPushButton(tr("&Reset"), this);
    connect(m_resetButton, &QPushButton::clicked, this, &KeyboardShortcutsPage::resetBinding);

    m_conflictLabel = new QLabel(this);
    m_conflictLabel->setWordWrap(true);

    auto *editRow = new QHBoxLayout;
    editRow->addWidget(m_capture, 1);
    editRow->addWidget(m_modCapture, 1);
    editRow->addWidget(m_assignButton);
    editRow->addWidget(m_clearButton);
    editRow->addWidget(m_resetButton);

    auto *resetAllButton = new QPushButton(tr("Reset All"), this);
    resetAllButton->setObjectName(QStringLiteral("settingsShortcutsResetAll"));
    connect(resetAllButton, &QPushButton::clicked, this, &KeyboardShortcutsPage::resetAll);
    auto *buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->addStretch(1);
    buttonRow->addWidget(resetAllButton);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_filter);
    layout->addWidget(m_tree, 1);
    layout->addLayout(editRow);
    layout->addWidget(m_conflictLabel);
    layout->addLayout(buttonRow);

    // External changes (another dialog instance, Reset All) refresh the view;
    // m_applying keeps our own writes from resetting the selection mid-edit.
    connect(&keymap::Registry::instance(), &keymap::Registry::bindingsChanged, this, [this] {
        if (!m_applying)
            rebuildTree();
    });

    rebuildTree();
    // Fit the command column to its widest row once, up front — the 100px
    // header default truncates most names. Not re-fit on rebuilds, so a
    // manual column drag survives assigning/resetting bindings.
    m_tree->resizeColumnToContents(0);
    // The tree is the page's body; keep enough of it visible that the
    // Settings window doesn't size down to a few rows.
    m_tree->setMinimumHeight(::layout::fontPx(22));
}

QString KeyboardShortcutsPage::currentCommandId() const
{
    QTreeWidgetItem *item = m_tree->currentItem();
    return item ? item->data(0, kIdRole).toString() : QString();
}

void KeyboardShortcutsPage::rebuildTree()
{
    const QString selected = currentCommandId();
    const int scrollPosition = m_tree->verticalScrollBar()->value();
    m_tree->clear();
    auto &registry = keymap::Registry::instance();
    QTreeWidgetItem *categoryItem = nullptr;
    QTreeWidgetItem *toReselect = nullptr;
    for (const keymap::CommandInfo &info : registry.commands()) {
        if (!categoryItem || categoryItem->text(0) != info.category) {
            categoryItem = new QTreeWidgetItem(m_tree, {info.category});
            categoryItem->setFlags(Qt::ItemIsEnabled);
            categoryItem->setFirstColumnSpanned(true);
            QFont font = categoryItem->font(0);
            font.setBold(true);
            categoryItem->setFont(0, font);
        }
        const QString binding = info.modifier
                                    ? modifierDisplayText(registry.modifierBinding(info.id))
                                    : bindingText(registry.bindings(info.id));
        auto *item = new QTreeWidgetItem(categoryItem, {info.name, binding});
        item->setData(0, kIdRole, info.id);
        if (registry.isOverridden(info.id)) {
            // Non-default bindings read bold, Qt Creator-style.
            QFont font = item->font(1);
            font.setBold(true);
            item->setFont(1, font);
        }
        if (info.id == selected)
            toReselect = item;
    }
    m_tree->expandAll();
    applyFilter();
    if (toReselect)
        m_tree->setCurrentItem(toReselect);
    else
        currentRowChanged();
    // Last on purpose: setCurrentItem auto-scrolls to the selection, and
    // keeping the user's list position is the point. The scrollbar range is
    // still the pre-clear one (recompute is deferred), which clamps
    // correctly only because a rebuild recreates the identical item set.
    m_tree->verticalScrollBar()->setValue(scrollPosition);
}

void KeyboardShortcutsPage::applyFilter()
{
    const QString needle = m_filter->text().trimmed();
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *category = m_tree->topLevelItem(i);
        int visible = 0;
        for (int j = 0; j < category->childCount(); ++j) {
            QTreeWidgetItem *item = category->child(j);
            const bool match = needle.isEmpty() ||
                               item->text(0).contains(needle, Qt::CaseInsensitive) ||
                               item->text(1).contains(needle, Qt::CaseInsensitive);
            item->setHidden(!match);
            if (match)
                ++visible;
        }
        category->setHidden(visible == 0);
    }
}

// The chords a modifier command may be bound to. The velocity lane's detent
// unlock is the one command that cannot take Shift: Shift is its surface's
// ramp gesture, and the unlock is read from the same press.
void KeyboardShortcutsPage::fillModifierChoices(const QString &id)
{
    const QSignalBlocker blocker(m_modCapture);
    m_modCapture->clear();
    for (const auto mods :
         {Qt::KeyboardModifiers(Qt::ControlModifier), Qt::KeyboardModifiers(Qt::ShiftModifier),
          Qt::KeyboardModifiers(Qt::AltModifier), Qt::ControlModifier | Qt::ShiftModifier,
          Qt::ControlModifier | Qt::AltModifier, Qt::ShiftModifier | Qt::AltModifier}) {
        if (id == QStringLiteral("velocity.detent_unlock") && mods.testFlag(Qt::ShiftModifier))
            continue;
        m_modCapture->addItem(modifierDisplayText(mods), int(mods.toInt()));
    }
}

void KeyboardShortcutsPage::currentRowChanged()
{
    const QString id = currentCommandId();
    const bool hasCommand = !id.isEmpty();
    m_capture->setEnabled(hasCommand);
    m_assignButton->setEnabled(hasCommand);
    m_clearButton->setEnabled(hasCommand);
    m_resetButton->setEnabled(hasCommand);
    m_conflictLabel->clear();
    if (!hasCommand) {
        m_capture->clear();
        return;
    }
    auto &registry = keymap::Registry::instance();
    const bool modifier = registry.command(id).modifier;
    m_capture->setVisible(!modifier);
    m_modCapture->setVisible(modifier);
    if (modifier) {
        fillModifierChoices(id);
        const int index = m_modCapture->findData(int(registry.modifierBinding(id).toInt()));
        const QSignalBlocker blocker(m_modCapture);
        m_modCapture->setCurrentIndex(std::max(0, index));
    } else {
        const QList<QKeySequence> bindings = registry.bindings(id);
        const QSignalBlocker blocker(m_capture); // loading isn't a user edit
        m_capture->setKeySequence(bindings.isEmpty() ? QKeySequence() : bindings.first());
    }
    m_resetButton->setEnabled(registry.isOverridden(id));
}

void KeyboardShortcutsPage::captureChanged()
{
    m_conflictLabel->clear();
    const QString id = currentCommandId();
    if (id.isEmpty())
        return;
    auto &registry = keymap::Registry::instance();
    QStringList conflicts;
    if (registry.command(id).modifier) {
        conflicts = registry.modifierConflicts(
            id, registry.command(id).context,
            Qt::KeyboardModifiers(QFlag(m_modCapture->currentData().toInt())));
    } else {
        const QKeySequence seq = capturedKeySequence(m_capture->keySequence());
        if (seq.isEmpty())
            return;
        if (seq != m_capture->keySequence()) {
            const QSignalBlocker blocker(m_capture);
            m_capture->setKeySequence(seq);
        }
        conflicts = registry.conflicts(id, registry.command(id).context, seq);
    }
    if (conflicts.isEmpty())
        return;
    QStringList names;
    for (const QString &other : conflicts)
        names.append(registry.command(other).name);
    m_conflictLabel->setText(
        tr("Also bound to %1 — assigning will unbind it there.").arg(names.join(tr(", "))));
}

void KeyboardShortcutsPage::assign()
{
    const QString id = currentCommandId();
    if (id.isEmpty())
        return;
    auto &registry = keymap::Registry::instance();
    if (registry.command(id).modifier) {
        const Qt::KeyboardModifiers mods(QFlag(m_modCapture->currentData().toInt()));
        if (mods == Qt::NoModifier)
            return;
        m_applying = true;
        // Same steal as below, among the modifier gestures.
        const QStringList conflicts =
            registry.modifierConflicts(id, registry.command(id).context, mods);
        for (const QString &other : conflicts)
            registry.setModifierBinding(other, Qt::NoModifier);
        registry.setModifierBinding(id, mods);
        m_applying = false;
        rebuildTree();
        return;
    }
    const QKeySequence seq = capturedKeySequence(m_capture->keySequence());
    if (seq.isEmpty())
        return;
    m_applying = true;
    // Assigning steals the key: the same sequence firing two commands in one
    // context would be ambiguous, so the loser goes unbound (still visible in
    // its row for the user to rebind).
    const QStringList conflicts = registry.conflicts(id, registry.command(id).context, seq);
    for (const QString &other : conflicts)
        registry.setBinding(other, QKeySequence());
    registry.setBinding(id, seq);
    m_applying = false;
    rebuildTree();
}

void KeyboardShortcutsPage::clearBinding()
{
    const QString id = currentCommandId();
    if (id.isEmpty())
        return;
    auto &registry = keymap::Registry::instance();
    m_applying = true;
    if (registry.command(id).modifier)
        registry.setModifierBinding(id, Qt::NoModifier);
    else
        registry.setBinding(id, QKeySequence());
    m_applying = false;
    rebuildTree();
}

void KeyboardShortcutsPage::resetBinding()
{
    const QString id = currentCommandId();
    if (id.isEmpty())
        return;
    m_applying = true;
    keymap::Registry::instance().resetBinding(id);
    m_applying = false;
    rebuildTree();
}

void KeyboardShortcutsPage::resetAll()
{
    const auto answer = QMessageBox::question(this, tr("Reset All Shortcuts"),
                                              tr("Reset every shortcut to its default?"));
    if (answer != QMessageBox::Yes)
        return;
    m_applying = true;
    keymap::Registry::instance().resetAll();
    m_applying = false;
    rebuildTree();
}
