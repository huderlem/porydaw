#pragma once

#include <QWidget>

class QComboBox;
class QKeySequenceEdit;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

// Settings window page over keymap::Registry. Changes apply immediately
// (the registry persists and re-applies them); there is no OK/Cancel
// staging — per-command Reset and Reset All are the undo story.
class KeyboardShortcutsPage : public QWidget
{
    Q_OBJECT

  public:
    explicit KeyboardShortcutsPage(QWidget *parent = nullptr);

  private:
    void rebuildTree();
    void applyFilter();
    void fillModifierChoices(const QString &id);
    void currentRowChanged();
    void captureChanged();
    void assign();
    void clearBinding();
    void resetBinding();
    void resetAll();
    QString currentCommandId() const;

    QLineEdit *m_filter = nullptr;
    QTreeWidget *m_tree = nullptr;
    QKeySequenceEdit *m_capture = nullptr;
    // Modifier commands (hold-and-drag gestures) pick from a chord list
    // instead of the key-sequence capture; the two swap per selected row.
    QComboBox *m_modCapture = nullptr;
    QPushButton *m_assignButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    QPushButton *m_resetButton = nullptr;
    QLabel *m_conflictLabel = nullptr;
    // Guards the tree-refresh feedback loop while an assignment is applied.
    bool m_applying = false;
};
