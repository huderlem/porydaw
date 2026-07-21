#pragma once

#include <QDialog>

#include <functional>

#include "audio/sampledoc.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

// The Sample Studio dialog (docs/sample-studio/PLAN.md §5), at its phase-2
// scope: any decoded source runs through the non-destructive DSP.md pipeline
// with numeric controls for crop/loop/key/rate/normalize, a live output
// readout, and a live-validated name field. Pure view: the dialog renders
// and hands out the export bytes; MainWindow does the writes on accept.
// The waveform view, engine audition, and loop suggestion land in phase 3.
class SampleEditorDialog : public QDialog
{
    Q_OBJECT

public:
    // validator: name -> ok, filling *error with the refusal shown inline
    // (SampleRegistrar::validateSampleName bound to the project).
    using NameValidator = std::function<bool(const QString &, QString *)>;

    SampleEditorDialog(ImportedSample sample, NameValidator validator,
                       QWidget *parent = nullptr);

    // The validated registration name (valid whenever the dialog accepts).
    QString sampleName() const;

    // The current render, exported per FORMATS.md §1 — what "Add to
    // Project" commits.
    QByteArray wavBytes();

    // The pipeline document behind the controls (harness introspection).
    SampleDocument *document() { return &m_doc; }

private:
    void applyParamsFromUi();
    void refreshOutputs();
    void validateName();

    SampleDocument m_doc;
    NameValidator m_validator;
    // The fine-tune spin's rendition of the source tuning (the spin rounds
    // to 2 decimals): the verbatim-agbp carry compares against this, not the
    // full-precision source fraction, so an unrelated edit can't spuriously
    // drop the override.
    double m_sourceCents = 0.0;

    QSpinBox *m_cropStart;
    QSpinBox *m_cropEnd;
    QCheckBox *m_loopOn;
    QSpinBox *m_loopStart;
    QSpinBox *m_loopEnd;
    QSpinBox *m_baseKey;
    QDoubleSpinBox *m_fineTune;
    QComboBox *m_rateCombo;
    QComboBox *m_normalizeMode;
    QLabel *m_gainReadout;
    QLabel *m_outputSummary;
    QLineEdit *m_nameEdit;
    QLabel *m_nameStatus;
    QPushButton *m_addButton;
};
