#pragma once

#include <QDialog>

#include <functional>

#include "project/samplereg.h"

class QLabel;
class QLineEdit;
class QPushButton;

// The Sample Studio dialog (docs/sample-studio/PLAN.md §5), at its phase-1
// scope: a prepared (already GBA-ready) .wav is shown read-only — header
// format, smpl/agbp pitch metadata, loop, ROM cost — with a live-validated
// name field and an "Add to Project" commit. Pure view: the dialog collects
// the name; MainWindow does the writes on accept. Later phases grow the
// editing pipeline around this shell.
class SampleEditorDialog : public QDialog
{
    Q_OBJECT

public:
    // validator: name -> ok, filling *error with the refusal shown inline
    // (SampleRegistrar::validateSampleName bound to the project).
    using NameValidator = std::function<bool(const QString &, QString *)>;

    SampleEditorDialog(const QString &sourcePath, const SampleWavInfo &info,
                       NameValidator validator, QWidget *parent = nullptr);

    // The validated registration name (valid whenever the dialog accepts).
    QString sampleName() const;

private:
    void validateName();

    SampleWavInfo m_info;
    NameValidator m_validator;
    QLineEdit *m_nameEdit;
    QLabel *m_nameStatus;
    QPushButton *m_addButton;
};
