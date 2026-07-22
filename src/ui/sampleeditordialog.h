#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QTimer>
#include <QUndoStack>

#include <functional>
#include <vector>

#include "audio/auditionslots.h"
#include "audio/sampledoc.h"
#include "audio/sampledsp.h"

class AudioEngine;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QToolButton;
class WaveformView;

// The Sample Studio dialog (docs/sample-studio/PLAN.md §5): the dominant
// waveform view with crop/loop drag handles, a checkable "Loop this
// sample" group that hides all loop chrome for one-shots and seeds the
// best analyzer candidate (plus a crossfade bake iff its seam isn't
// clean) on first enable, a green/amber/red seam badge with a one-click
// Fix, pitch-detect prefill, and an engine audition strip (play / key)
// driven through the audition-slot protocol (PLAN.md §4). Expert rows
// live in a collapsed Advanced disclosure; the whole control column
// below the waveform rides a squeeze-then-scroll area. Pure view: the
// dialog renders and hands out the export bytes; MainWindow does the
// writes on accept. Parameter edits ride a dialog-local QUndoStack —
// nothing project-visible exists until commit.
class SampleEditorDialog : public QDialog
{
    Q_OBJECT

public:
    // validator: name -> ok, filling *error with the refusal shown inline
    // (SampleRegistrar::validateSampleName bound to the project).
    using NameValidator = std::function<bool(const QString &, QString *)>;

    // engine may be null (audition strip disabled). destAdsr, when given,
    // is the destination voice's envelope (browser-initiated flow) and
    // enables the "use destination voice ADSR" audition option.
    SampleEditorDialog(ImportedSample sample, NameValidator validator,
                       AudioEngine *engine = nullptr,
                       const AuditionSlots::Adsr *destAdsr = nullptr,
                       QWidget *parent = nullptr);

    // The validated registration name (valid whenever the dialog accepts).
    QString sampleName() const;

    // The current render, exported per FORMATS.md §1 — what "Add to
    // Project" commits.
    QByteArray wavBytes();

    // The pipeline document behind the controls (harness introspection).
    SampleDocument *document() { return &m_doc; }
    QUndoStack *undoStack() { return &m_undo; }
    WaveformView *waveform() { return m_waveform; }

    // Undo/redo plumbing: apply a parameter set and re-sync every control.
    void applyParamsExternal(const SampleEditParams &params);

    // "Edit sample…" reopen (PLAN.md §6 phase 6): the sample is already
    // registered as <name>, so the name is fixed (renames would need .inc
    // surgery — out of scope) and the commit button reads "Save Sample".
    void setEditTarget(const QString &name);

protected:
    void done(int result) override; // silence the audition on any close

private:
    void applyParamsFromUi(int mergeKey);
    void commitParams(const SampleEditParams &params, int mergeKey);
    void syncUiFromParams();
    void refreshOutputs();
    void validateName();
    void ensurePitchDetected();
    void updatePitchHint();
    void applyDetectedPitch();
    void computeChips();
    bool ensureChips();
    void autoPopulateLoop();
    void tryAnotherLoop();
    void refineCurrentLoop();
    void applyChip(int index);
    SampleEditParams analysisParams() const;
    void startAudition(bool looped);
    void stopAudition();
    void republishAudition();
    void auditionTick();

    SampleDocument m_doc;
    NameValidator m_validator;
    AudioEngine *m_engine = nullptr;
    bool m_hasDestAdsr = false;
    AuditionSlots::Adsr m_destAdsr;
    QUndoStack m_undo;
    bool m_syncing = false;
    // The fine-tune spin's rendition of the source tuning (the spin rounds
    // to 2 decimals): the verbatim-agbp carry compares against this, not the
    // full-precision source fraction, so an unrelated edit can't spuriously
    // drop the override.
    double m_sourceCents = 0.0;
    // Waveform drag gestures collapse into one undo entry.
    SampleEditParams m_gestureBase;

    // Pitch detection (DSP.md §4) — computed once at open. Prefills the
    // key/cents only when the container carried no pitch metadata;
    // otherwise it powers the quiet mismatch hint beside the base key.
    bool m_pitchTried = false;
    SampleDsp::PitchResult m_pitch;

    // Suggested loop candidates, mapped back to source coordinates.
    // Computed lazily (one analysis render + seam search) and cached
    // against the inputs that shape the analysis grid.
    struct Chip {
        qint64 srcStart = 0;
        qint64 srcEnd = 0;
        SampleDsp::LoopCandidate cand;
        SeamMetrics metrics;
    };
    std::vector<Chip> m_chips;
    bool m_chipsValid = false;
    int m_chipIndex = -1; // last-applied candidate ("Try another" cycles)
    qint64 m_chipsCropStart = -1;
    qint64 m_chipsCropEnd = -1;
    double m_chipsRate = -1.0;

    // Audition state (engine slots; display playhead is a UI approximation).
    enum class AuditionMode { None, Once, Loop };
    AuditionMode m_auditionMode = AuditionMode::None;
    bool m_republishPending = false;
    double m_auditionPos = 0.0;   // output-domain samples
    double m_auditionRate = 0.0;  // output samples/sec at the audition key
    double m_auditionRatio = 1.0; // output rate / source rate
    qint64 m_auditionCrop = 0;    // source crop start for playhead mapping
    quint32 m_auditionSize = 0;
    quint32 m_auditionLoopStart = 0;
    bool m_auditionLooped = false;
    QTimer m_auditionTimer;
    QElapsedTimer m_auditionClock;

    WaveformView *m_waveform = nullptr;
    // The loop group: checkable, and its body hides entirely while the
    // sample is a one-shot.
    QGroupBox *m_loopGroup = nullptr;
    QWidget *m_loopBody = nullptr;
    QPushButton *m_tryLoop = nullptr;
    QPushButton *m_refineButton = nullptr;
    QPushButton *m_seamFix = nullptr;
    QLabel *m_suggestStatus = nullptr;
    QLabel *m_seamBadge = nullptr;
    QSpinBox *m_cropStart = nullptr;
    QSpinBox *m_cropEnd = nullptr;
    QSpinBox *m_loopStart = nullptr;
    QSpinBox *m_loopEnd = nullptr;
    QSpinBox *m_baseKey = nullptr;
    QDoubleSpinBox *m_fineTune = nullptr;
    QLabel *m_pitchLabel = nullptr;
    QPushButton *m_pitchApply = nullptr;
    QComboBox *m_rateCombo = nullptr;
    QComboBox *m_normalizeMode = nullptr;
    QLabel *m_gainReadout = nullptr;
    QCheckBox *m_crossfade = nullptr;
    QPushButton *m_playButton = nullptr;
    QSpinBox *m_auditionKey = nullptr;
    QCheckBox *m_useDestAdsr = nullptr;
    QLabel *m_outputSummary = nullptr;
    // The Advanced disclosure: expert rows (format, crop, rate, normalize,
    // fine-tune) plus the technical output readout, collapsed by default.
    QToolButton *m_advancedToggle = nullptr;
    QWidget *m_advancedBody = nullptr;
    QLabel *m_techDetail = nullptr;
    QLineEdit *m_nameEdit = nullptr;
    QLabel *m_nameStatus = nullptr;
    QPushButton *m_addButton = nullptr;
};
