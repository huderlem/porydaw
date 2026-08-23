#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <cstdio>

#include "ui/keyboardshortcutspage.h"
#include "ui/keymap.h"

// --keymapcheck: user-configurable keyboard shortcuts check (self-contained,
// no project needed). Verifies the registry's shipped table (unique ids,
// non-empty names, no default conflicts), event matching (exact modifiers,
// keypad tolerance, alternate defaults), override/unbind/reset with
// delta-only QSettings persistence, live re-application to attached
// QActions, cross-context conflict detection, modifier commands (mouse
// gestures bound to a bare modifier chord, with the same delta-only
// override/unbind/reset and text round-trip), and the shortcuts dialog
// (filter, keypad-stripped capture, assign with steal-on-conflict, per-row
// reset, scroll-position retention, and the modifier chord picker swapping
// in for the key capture) driven offscreen.
// QSettings is redirected into a temp dir first, so the user's real keymap
// is never read or written.

namespace {

bool keyMatches(const QString &id, int key, Qt::KeyboardModifiers mods)
{
    QKeyEvent event(QEvent::KeyPress, key, mods);
    return keymap::Registry::instance().matches(&event, id);
}

QTreeWidgetItem *findCommandItem(QTreeWidget *tree, const QString &id)
{
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *category = tree->topLevelItem(i);
        for (int j = 0; j < category->childCount(); ++j) {
            QTreeWidgetItem *item = category->child(j);
            if (item->data(0, Qt::UserRole).toString() == id)
                return item;
        }
    }
    return nullptr;
}

QPushButton *findButton(QWidget *root, const QString &text)
{
    const auto buttons = root->findChildren<QPushButton *>();
    for (QPushButton *button : buttons) {
        if (button->text() == text)
            return button;
    }
    return nullptr;
}

} // namespace

int runKeymapCheck()
{
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "keymapcheck: no temp dir for settings\n");
        return 1;
    }
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());

    int failures = 0;
    const auto check = [&failures](bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "keymapcheck: FAIL: %s\n", what);
            failures++;
        }
        return ok;
    };

    auto &registry = keymap::Registry::instance();

    // 1. Shipped table sanity: unique ids, visible names/categories, and no
    // command's default colliding with another live in an overlapping
    // context — a collision here would make two commands fire on one key.
    {
        const QList<keymap::CommandInfo> commands = registry.commands();
        check(!commands.isEmpty(), "empty command table");
        QSet<QString> ids;
        for (const keymap::CommandInfo &info : commands) {
            check(!ids.contains(info.id), "duplicate command id");
            ids.insert(info.id);
            check(!info.name.isEmpty() && !info.category.isEmpty(),
                  "command missing name or category");
            for (const QKeySequence &seq : info.defaults) {
                check(registry.conflicts(info.id, info.context, seq).isEmpty(),
                      "default binding conflicts across commands");
            }
        }
    }

    // 2. Default matching: exact modifiers decide between the semitone and
    // octave transpose commands; keypad arrows still count; StandardKey
    // multi-bindings and the Delete/Backspace alternates all match.
    {
        check(keyMatches(QStringLiteral("roll.transpose_up"), Qt::Key_Up, Qt::ControlModifier),
              "Ctrl+Up should transpose up a semitone");
        check(!keyMatches(QStringLiteral("roll.transpose_up"), Qt::Key_Up,
                          Qt::ControlModifier | Qt::ShiftModifier),
              "Ctrl+Shift+Up must not match the semitone command");
        check(keyMatches(QStringLiteral("roll.transpose_up_octave"), Qt::Key_Up,
                         Qt::ControlModifier | Qt::ShiftModifier),
              "Ctrl+Shift+Up should transpose up an octave");
        check(keyMatches(QStringLiteral("roll.transpose_up"), Qt::Key_Up,
                         Qt::ControlModifier | Qt::KeypadModifier),
              "keypad Ctrl+Up should still transpose");
        check(keyMatches(QStringLiteral("roll.delete"), Qt::Key_Delete, Qt::NoModifier) &&
                  keyMatches(QStringLiteral("roll.delete"), Qt::Key_Backspace, Qt::NoModifier),
              "Delete and Backspace alternates should both delete");
        check(keyMatches(QStringLiteral("roll.copy"), Qt::Key_C, Qt::ControlModifier),
              "Ctrl+C should match roll.copy");
        check(keyMatches(QStringLiteral("roll.mute_tracks"), Qt::Key_M, Qt::NoModifier),
              "M should match mute tracks");
        check(!keyMatches(QStringLiteral("roll.mute_tracks"), Qt::Key_M, Qt::ControlModifier),
              "Ctrl+M must not match mute tracks");
        check(keyMatches(QStringLiteral("roll.solo_tracks"), Qt::Key_S, Qt::NoModifier),
              "S should match solo tracks");
        check(keyMatches(QStringLiteral("transport.play_pause"), Qt::Key_Space, Qt::NoModifier),
              "Space should match play/pause");
        check(
            !keyMatches(QStringLiteral("transport.play_pause"), Qt::Key_Space, Qt::ControlModifier),
            "Ctrl+Space must not match play/pause");
    }

    // 3. Override: the new key matches, the default stops matching, and the
    // store holds exactly the one delta.
    {
        registry.setBinding(QStringLiteral("roll.transpose_up"),
                            QKeySequence(QStringLiteral("Alt+T")));
        check(keyMatches(QStringLiteral("roll.transpose_up"), Qt::Key_T, Qt::AltModifier),
              "override Alt+T should match");
        check(!keyMatches(QStringLiteral("roll.transpose_up"), Qt::Key_Up, Qt::ControlModifier),
              "old Ctrl+Up must stop matching after override");
        check(registry.isOverridden(QStringLiteral("roll.transpose_up")),
              "override not marked as overridden");
        QSettings settings;
        check(settings.value(QStringLiteral("keymap/roll.transpose_up")).toString() ==
                  QStringLiteral("Alt+T"),
              "override not persisted as portable text");
        check(!settings.contains(QStringLiteral("keymap/roll.transpose_down")),
              "untouched command leaked into the settings store");
    }

    // 4. Unbind: explicitly bound to nothing, persisted as an empty delta.
    {
        registry.setBinding(QStringLiteral("roll.nudge_left"), QKeySequence());
        check(!keyMatches(QStringLiteral("roll.nudge_left"), Qt::Key_Left, Qt::ControlModifier),
              "unbound command still matches its default");
        check(registry.bindings(QStringLiteral("roll.nudge_left")).isEmpty(),
              "unbound command reports bindings");
        QSettings settings;
        check(settings.contains(QStringLiteral("keymap/roll.nudge_left")), "unbind not persisted");
    }

    // 5. Reset: default returns and the delta is removed. Re-assigning the
    // sole default is also a reset — the store stays delta-only.
    {
        registry.resetBinding(QStringLiteral("roll.transpose_up"));
        check(keyMatches(QStringLiteral("roll.transpose_up"), Qt::Key_Up, Qt::ControlModifier),
              "reset did not restore the default");
        QSettings settings;
        check(!settings.contains(QStringLiteral("keymap/roll.transpose_up")),
              "reset left a delta behind");
        registry.setBinding(QStringLiteral("view.event_list"),
                            QKeySequence(QStringLiteral("Ctrl+Shift+E")));
        check(!QSettings().contains(QStringLiteral("keymap/view.event_list")),
              "assigning the default value should store no delta");
        registry.resetAll();
        check(!QSettings().contains(QStringLiteral("keymap/roll.nudge_left")),
              "resetAll left deltas behind");
        check(keyMatches(QStringLiteral("roll.nudge_left"), Qt::Key_Left, Qt::ControlModifier),
              "resetAll did not restore nudge left");
    }

    // 6. Attached QActions re-apply live on changes and detach safely on
    // destruction.
    {
        auto *action = new QAction(QStringLiteral("Go to Start"));
        registry.attach(QStringLiteral("transport.go_to_start"), action);
        check(action->shortcut() == QKeySequence(Qt::Key_Home),
              "attach did not apply the default shortcut");
        registry.setBinding(QStringLiteral("transport.go_to_start"),
                            QKeySequence(QStringLiteral("Ctrl+Home")));
        check(action->shortcut() == QKeySequence(QStringLiteral("Ctrl+Home")),
              "override was not re-applied to the attached action");
        delete action;
        // A change after deletion must not touch the dead pointer.
        registry.resetBinding(QStringLiteral("transport.go_to_start"));
    }

    // 7. Conflicts: Global overlaps every context, local contexts overlap
    // themselves.
    {
        const QStringList onSave =
            registry.conflicts(QStringLiteral("roll.copy"), keymap::Context::PianoRoll,
                               QKeySequence(QStringLiteral("Ctrl+S")));
        check(onSave.contains(QStringLiteral("file.save_song")),
              "roll binding on Ctrl+S should conflict with Save Song");
        const QStringList onCopy =
            registry.conflicts(QStringLiteral("roll.cut"), keymap::Context::PianoRoll,
                               QKeySequence(QStringLiteral("Ctrl+C")));
        check(onCopy.contains(QStringLiteral("roll.copy")),
              "roll binding on Ctrl+C should conflict with roll copy");
        check(registry
                  .conflicts(QStringLiteral("roll.copy"), keymap::Context::PianoRoll,
                             QKeySequence(QStringLiteral("Alt+9")))
                  .isEmpty(),
              "unused sequence reported a conflict");
    }

    // 8. Modifier commands: the velocity-drag gesture ships on Ctrl, never
    // matches key events, and overrides/unbinds/resets through the same
    // delta-only store; the chord text round-trips in any spelling.
    {
        const QString id = QStringLiteral("roll.velocity_drag");
        check(registry.command(id).modifier, "velocity drag is not a modifier command");
        check(registry.modifierBinding(id) == Qt::ControlModifier,
              "velocity drag does not default to Ctrl");
        check(registry.bindings(id).isEmpty(), "modifier command reports key-sequence bindings");
        check(!keyMatches(id, Qt::Key_C, Qt::ControlModifier),
              "a key event matched a modifier command");

        registry.setModifierBinding(id, Qt::AltModifier);
        check(registry.modifierBinding(id) == Qt::AltModifier, "modifier override did not apply");
        check(registry.isOverridden(id), "modifier override not marked as overridden");
        check(QSettings().value(QStringLiteral("keymap/roll.velocity_drag")).toString() ==
                  QStringLiteral("Alt"),
              "modifier override not persisted as portable text");

        registry.setModifierBinding(id, Qt::ControlModifier);
        check(!registry.isOverridden(id),
              "re-assigning the default modifier should store no delta");

        registry.setModifierBinding(id, Qt::NoModifier);
        check(registry.modifierBinding(id) == Qt::NoModifier && registry.isOverridden(id),
              "modifier unbind did not persist as an empty delta");
        registry.resetBinding(id);
        check(registry.modifierBinding(id) == Qt::ControlModifier,
              "modifier reset did not restore Ctrl");

        check(keymap::Registry::modifierFromText(QStringLiteral("ctrl+shift")) ==
                  (Qt::ControlModifier | Qt::ShiftModifier),
              "modifier text parse is not case-insensitive");
        check(keymap::Registry::modifierText(Qt::ControlModifier | Qt::ShiftModifier) ==
                  QStringLiteral("Ctrl+Shift"),
              "modifier chord text did not serialize canonically");
        check(keymap::Registry::modifierFromText(QStringLiteral("Ctrl+F5")) == Qt::NoModifier,
              "a non-modifier token parsed as a chord");
    }

    // 9. Settings page: filter narrows rows, assigning through the capture
    // widget steals from the conflicting command, and per-row Reset restores
    // it. Modifier commands swap the key capture for the chord picker.
    {
        KeyboardShortcutsPage page;
        page.show(); // lay the tree out so column geometry is real
        QApplication::processEvents();
        auto *tree = page.findChild<QTreeWidget *>();
        auto *filter = page.findChild<QLineEdit *>();
        auto *capture = page.findChild<QKeySequenceEdit *>();
        QPushButton *assignButton = findButton(&page, QStringLiteral("&Assign"));
        QPushButton *resetButton = findButton(&page, QStringLiteral("&Reset"));
        if (!check(tree && filter && capture && assignButton && resetButton,
                   "page widgets missing")) {
            return failures ? 1 : 0;
        }

        // The command column must fit its widest row out of the box (user
        // report: names were cut off until the header was dragged).
        // sizeHintForColumn is re-protected by QTreeView; the base keeps it
        // public.
        const int columnHint = static_cast<QAbstractItemView *>(tree)->sizeHintForColumn(0);
        check(tree->columnWidth(0) >= columnHint, "command column narrower than its contents");

        // Captures shed the keypad flag on every platform (macOS nav keys
        // and numpad arrows arrive with it set): bindings never carry it,
        // so keeping it would store a binding that can never match.
        const QKeySequence keypadShiftUp(
            QKeyCombination(Qt::ShiftModifier | Qt::KeypadModifier, Qt::Key_Up));
        const QKeySequence shiftUp(QKeyCombination(Qt::ShiftModifier, Qt::Key_Up));
        tree->setCurrentItem(findCommandItem(tree, QStringLiteral("roll.copy")));
        capture->setKeySequence(keypadShiftUp);
        check(capture->keySequence() == shiftUp, "shortcut capture kept the keypad modifier");
        assignButton->click();
        check(registry.bindings(QStringLiteral("roll.copy")) == QList<QKeySequence>{shiftUp},
              "shortcut assignment kept the keypad modifier");
        registry.resetBinding(QStringLiteral("roll.copy"));

        filter->setText(QStringLiteral("Transpose"));
        QTreeWidgetItem *findSong = findCommandItem(tree, QStringLiteral("songs.find"));
        QTreeWidgetItem *transposeUp = findCommandItem(tree, QStringLiteral("roll.transpose_up"));
        check(findSong && findSong->isHidden(), "filter left a non-matching row visible");
        check(transposeUp && !transposeUp->isHidden(), "filter hid a matching row");
        filter->clear();

        // Steal: give Save Song's Ctrl+S to the roll copy command.
        tree->setCurrentItem(findCommandItem(tree, QStringLiteral("roll.copy")));
        capture->setKeySequence(QKeySequence(QStringLiteral("Ctrl+S")));
        const int scrollBeforeAssign = tree->verticalScrollBar()->maximum() / 2;
        tree->verticalScrollBar()->setValue(scrollBeforeAssign);
        assignButton->click();
        check(registry.bindings(QStringLiteral("roll.copy")) ==
                  QList<QKeySequence>{QKeySequence(QStringLiteral("Ctrl+S"))},
              "page assign did not apply the binding");
        check(registry.isOverridden(QStringLiteral("file.save_song")) &&
                  registry.bindings(QStringLiteral("file.save_song")).isEmpty(),
              "conflicting command was not unbound by the steal");
        check(tree->verticalScrollBar()->value() == scrollBeforeAssign,
              "page assign did not preserve the list scroll position");

        QTreeWidgetItem *copyItem = findCommandItem(tree, QStringLiteral("roll.copy"));
        check(copyItem &&
                  copyItem->text(1).contains(
                      QKeySequence(QStringLiteral("Ctrl+S")).toString(QKeySequence::NativeText)),
              "tree does not show the new binding");
        check(copyItem && copyItem->font(1).bold(), "overridden row is not bold");

        // Per-row reset for both sides of the steal.
        tree->setCurrentItem(copyItem);
        resetButton->click();
        check(!registry.isOverridden(QStringLiteral("roll.copy")),
              "page reset did not clear the override");
        tree->setCurrentItem(findCommandItem(tree, QStringLiteral("file.save_song")));
        resetButton->click();
        check(keyMatches(QStringLiteral("file.save_song"), Qt::Key_S, Qt::ControlModifier),
              "Save Song did not get Ctrl+S back after reset");

        // Modifier row: the chord picker replaces the key capture, assigns
        // through the registry, and per-row Reset restores the default.
        auto *modCapture = page.findChild<QComboBox *>();
        if (check(modCapture != nullptr, "modifier chord picker missing")) {
            tree->setCurrentItem(findCommandItem(tree, QStringLiteral("roll.velocity_drag")));
            check(modCapture->isVisible() && !capture->isVisible(),
                  "modifier row did not swap in the chord picker");
            const int altIndex = modCapture->findData(int(Qt::AltModifier));
            check(altIndex >= 0, "chord picker offers no Alt");
#ifdef Q_OS_MACOS
            const int ctrlShiftIndex =
                modCapture->findData(int((Qt::ControlModifier | Qt::ShiftModifier).toInt()));
            check(modCapture->itemText(modCapture->findData(int(Qt::ControlModifier))) ==
                          QStringLiteral("⌘") &&
                      modCapture->itemText(altIndex) == QStringLiteral("⌥") &&
                      modCapture->itemText(ctrlShiftIndex) == QStringLiteral("⇧⌘"),
                  "chord picker does not use native macOS modifier labels");
#endif
            modCapture->setCurrentIndex(altIndex);
            assignButton->click();
            check(registry.modifierBinding(QStringLiteral("roll.velocity_drag")) == Qt::AltModifier,
                  "chord picker assign did not apply the modifier");
            QTreeWidgetItem *velItem = findCommandItem(tree, QStringLiteral("roll.velocity_drag"));
#ifdef Q_OS_MACOS
            check(velItem && velItem->text(1) == QStringLiteral("⌥"),
                  "tree does not show the native macOS modifier chord");
#else
            check(velItem && velItem->text(1) == QStringLiteral("Alt"),
                  "tree does not show the new modifier chord");
#endif
            tree->setCurrentItem(velItem);
            resetButton->click();
            check(registry.modifierBinding(QStringLiteral("roll.velocity_drag")) ==
                      Qt::ControlModifier,
                  "modifier reset did not restore Ctrl");
            tree->setCurrentItem(findCommandItem(tree, QStringLiteral("roll.copy")));
            check(!modCapture->isVisible() && capture->isVisible(),
                  "leaving the modifier row did not restore the key capture");
        }
    }

    registry.resetAll();
    if (failures) {
        std::fprintf(stderr, "keymapcheck: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("keymapcheck: PASS\n");
    return 0;
}
