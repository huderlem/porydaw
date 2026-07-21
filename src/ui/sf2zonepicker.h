#pragma once

#include <QDialog>

#include "audio/sf2reader.h"

class QDialogButtonBox;
class QLineEdit;
class QTreeWidget;

// The pre-editor zone-selection step for SoundFont imports
// (docs/sample-studio/PLAN.md §2 Sf2ZonePicker): a searchable sample list
// grouped under instrument/preset labels. Pure view over a parsed Sf2File —
// it returns the picked zone index and writes nothing; the owner extracts
// the zone and runs the ordinary editor flow.
class Sf2ZonePicker : public QDialog
{
    Q_OBJECT

public:
    // file must outlive the dialog (the owner holds it across exec()).
    explicit Sf2ZonePicker(const Sf2File &file, QWidget *parent = nullptr);

    // Index into file.zones; -1 until a zone row is chosen.
    int selectedZone() const { return m_selected; }

private:
    void rebuild();
    void updateSelection();

    const Sf2File &m_file;
    int m_selected = -1;

    QLineEdit *m_search = nullptr;
    QTreeWidget *m_tree = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
};
