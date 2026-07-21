#include "sampleeditordialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShortcut>
#include <QSpinBox>
#include <QVBoxLayout>

#include <climits>
#include <cmath>

#include "audio/audioengine.h"
#include "audio/samplewav.h"
#include "enginesettingsdialog.h"
#include "m4asemantics.h"
#include "project/samplereg.h"
#include "waveformview.h"

namespace {

constexpr int kSampleParamsCommandId = 0x5350; // 'SP'
constexpr double kPi = 3.14159265358979323846;

QString sourceLine(const ImportedSample &s)
{
    QString kind;
    switch (s.sourceKind) {
    case ImportedSample::Wav:
        kind = s.sourceFloat ? QStringLiteral("float WAV")
                             : QStringLiteral("PCM WAV");
        break;
    case ImportedSample::Aif:
        kind = QStringLiteral("AIFF");
        break;
    case ImportedSample::Mp3:
        kind = QStringLiteral("MP3");
        break;
    case ImportedSample::Flac:
        kind = QStringLiteral("FLAC");
        break;
    case ImportedSample::Ogg:
        kind = QStringLiteral("Ogg Vorbis");
        break;
    default:
        kind = QStringLiteral("audio");
        break;
    }
    // Lossy sources have no container bit depth (sourceBits 0).
    if (s.sourceBits > 0)
        kind = QStringLiteral("%1-bit %2").arg(s.sourceBits).arg(kind);
    QString text = QStringLiteral("%1, %2 channel%3, %4 Hz, %5 samples")
                       .arg(kind)
                       .arg(s.sourceChannels)
                       .arg(s.sourceChannels == 1 ? QString()
                                                  : QStringLiteral("s"))
                       .arg(s.sampleRate, 0, 'f',
                            s.sampleRate == std::floor(s.sampleRate) ? 0 : 2)
                       .arg(s.frameCount());
    if (s.sampleRate > 0)
        text += QStringLiteral(" (%1 s)").arg(
            double(s.frameCount()) / s.sampleRate, 0, 'f', 2);
    return text;
}

// "Loop seam solo" (DSP.md §8): audition ±150 ms around the seam on repeat.
// The window — the material leading into the loop end followed by the
// material leaving the loop start — loops whole, with short raised-cosine
// edge fades so the artificial window wrap can't be mistaken for the seam
// under test (the seam itself sits mid-window, untouched).
QByteArray seamSoloBytes(const ProcessedSample &out)
{
    const qint64 S = out.loopStart;
    const qint64 E = qint64(out.size) - 1;
    const qint64 loopLen = E + 1 - S;
    const qint64 w = qBound<qint64>(
        1, qint64(std::llround(0.150 * out.outputRate)), loopLen);
    QByteArray bytes = out.s8.mid(int(E + 1 - w), int(w));
    bytes += out.s8.mid(int(S), int(qMin(w, loopLen)));
    const int f =
        int(qMin<qint64>(qint64(std::llround(0.005 * out.outputRate)),
                         bytes.size() / 4));
    for (int i = 0; i < f; i++) {
        const double g =
            0.5 * (1.0 - std::cos(kPi * double(i) / double(f)));
        bytes[i] = char(qint8(std::lround(double(qint8(bytes[i])) * g)));
        const int j = bytes.size() - 1 - i;
        bytes[j] = char(qint8(std::lround(double(qint8(bytes[j])) * g)));
    }
    return bytes;
}

} // namespace

// A parameter edit on the dialog-local stack. Value-based before/after;
// consecutive edits from the same control (spin arrows held down, a slider
// of re-renders) merge into one entry, and a run that lands back on its
// starting value vanishes.
class SampleParamsCommand : public QUndoCommand
{
public:
    SampleParamsCommand(SampleEditorDialog *dialog,
                        const SampleEditParams &before,
                        const SampleEditParams &after, int mergeKey)
        : QUndoCommand(QObject::tr("edit sample parameters")),
          m_dialog(dialog), m_before(before), m_after(after),
          m_mergeKey(mergeKey)
    {
    }

    int id() const override { return kSampleParamsCommandId; }

    bool mergeWith(const QUndoCommand *other) override
    {
        auto *o = static_cast<const SampleParamsCommand *>(other);
        if (m_mergeKey < 0 || o->m_mergeKey != m_mergeKey)
            return false;
        m_after = o->m_after;
        if (m_after == m_before)
            setObsolete(true);
        return true;
    }

    void redo() override
    {
        // The edit is already applied when the command is pushed.
        if (m_first) {
            m_first = false;
            return;
        }
        m_dialog->applyParamsExternal(m_after);
    }
    void undo() override { m_dialog->applyParamsExternal(m_before); }

private:
    SampleEditorDialog *m_dialog;
    SampleEditParams m_before;
    SampleEditParams m_after;
    int m_mergeKey;
    bool m_first = true;
};

SampleEditorDialog::SampleEditorDialog(ImportedSample sample,
                                       NameValidator validator,
                                       AudioEngine *engine,
                                       const AuditionSlots::Adsr *destAdsr,
                                       QWidget *parent)
    : QDialog(parent), m_doc(std::move(sample)),
      m_validator(std::move(validator)), m_engine(engine)
{
    setWindowTitle(tr("Sample Studio"));
    if (destAdsr) {
        m_hasDestAdsr = true;
        m_destAdsr = *destAdsr;
    }

    // Pitch-detect prefill (DSP.md §4): only when the container carried no
    // pitch metadata — a real smpl/INST unity note always wins, and the
    // detection is otherwise display-only until the user applies it.
    if (!m_doc.source().hasPitchMetadata) {
        ensurePitchDetected();
        if (m_pitch.pitched) {
            SampleEditParams p = m_doc.params();
            const double exact =
                69.0 + 12.0 * std::log2(m_pitch.f0 / 440.0);
            p.baseKey = qBound(0, int(std::floor(exact)), 127);
            p.fineTuneCents = qBound(
                0.0, std::round((exact - std::floor(exact)) * 10000.0) / 100.0,
                99.99);
            m_doc.setParams(p);
        }
    }

    const ImportedSample &src = m_doc.source();
    const SampleEditParams defaults = m_doc.params();
    const int frames = int(qMin<qint64>(src.frameCount(), INT_MAX));

    auto *layout = new QVBoxLayout(this);

    // ---- the waveform, dominant on top ----
    m_waveform = new WaveformView(this);
    m_waveform->setSample(&m_doc.source());
    layout->addWidget(m_waveform, 1);
    connect(m_waveform, &WaveformView::gestureStarted, this, [this] {
        m_gestureBase = m_doc.params();
    });
    connect(m_waveform, &WaveformView::markersDragged, this,
            [this](qint64 cropStart, qint64 cropEnd, qint64 loopStart,
                   qint64 loopEnd) {
                // Live while dragging: apply directly (undo lands once, on
                // gesture end) and keep a sounding looped audition current.
                SampleEditParams p = m_doc.params();
                p.cropStart = cropStart;
                p.cropEnd = cropEnd;
                p.loopStart = loopStart;
                p.loopEnd = loopEnd;
                if (p == m_doc.params())
                    return;
                m_doc.setParams(p);
                m_syncing = true;
                syncUiFromParams();
                m_syncing = false;
                refreshOutputs();
                republishAudition();
            });
    connect(m_waveform, &WaveformView::gestureFinished, this, [this] {
        if (m_doc.params() != m_gestureBase)
            m_undo.push(new SampleParamsCommand(this, m_gestureBase,
                                                m_doc.params(), -1));
    });

    // ---- loop tools ----
    auto *tools = new QHBoxLayout;
    m_snapZero = new QCheckBox(tr("Snap to zero"), this);
    m_snapZero->setObjectName(QStringLiteral("sampleSnapZero"));
    m_snapZero->setToolTip(
        tr("Snap dragged markers to the nearest zero crossing."));
    connect(m_snapZero, &QCheckBox::toggled, m_waveform,
            &WaveformView::setSnapToZero);
    tools->addWidget(m_snapZero);
    m_suggestButton = new QPushButton(tr("Suggest loop"), this);
    m_suggestButton->setObjectName(QStringLiteral("sampleSuggestLoop"));
    connect(m_suggestButton, &QPushButton::clicked, this,
            &SampleEditorDialog::suggestLoops);
    tools->addWidget(m_suggestButton);
    m_refineButton = new QPushButton(tr("Refine"), this);
    m_refineButton->setObjectName(QStringLiteral("sampleRefineLoop"));
    m_refineButton->setToolTip(
        tr("Re-seat the current loop markers with a local seam search."));
    connect(m_refineButton, &QPushButton::clicked, this,
            &SampleEditorDialog::refineCurrentLoop);
    tools->addWidget(m_refineButton);
    m_chipRow = new QHBoxLayout;
    m_chipRow->setSpacing(2);
    tools->addLayout(m_chipRow);
    m_suggestStatus = new QLabel(this);
    m_suggestStatus->setObjectName(QStringLiteral("sampleSuggestStatus"));
    tools->addWidget(m_suggestStatus);
    tools->addStretch();
    m_seamBadge = new QLabel(this);
    m_seamBadge->setObjectName(QStringLiteral("sampleSeamBadge"));
    m_seamBadge->setMargin(3);
    tools->addWidget(m_seamBadge);
    layout->addLayout(tools);

    // ---- the pipeline form ----
    auto *form = new QFormLayout;
    auto *sourceLabel = new QLabel(QFileInfo(src.sourcePath).fileName(), this);
    sourceLabel->setToolTip(src.sourcePath);
    form->addRow(tr("Source:"), sourceLabel);
    form->addRow(tr("Format:"), new QLabel(sourceLine(src), this));

    const auto makeSpin = [this](const char *name, int min, int max,
                                 int value, int mergeKey) {
        auto *spin = new QSpinBox(this);
        spin->setObjectName(QLatin1String(name));
        spin->setRange(min, max);
        spin->setValue(value);
        spin->setKeyboardTracking(false);
        connect(spin, &QSpinBox::valueChanged, this,
                [this, mergeKey] { applyParamsFromUi(mergeKey); });
        return spin;
    };

    m_cropStart = makeSpin("sampleCropStart", 0, frames - 1,
                           int(defaults.cropStart), 1);
    m_cropEnd = makeSpin("sampleCropEnd", 1, frames, int(defaults.cropEnd), 2);
    auto *cropRow = new QHBoxLayout;
    cropRow->addWidget(m_cropStart);
    cropRow->addWidget(new QLabel(tr("to"), this));
    cropRow->addWidget(m_cropEnd);
    cropRow->addStretch();
    form->addRow(tr("Crop (samples):"), cropRow);

    m_loopOn = new QCheckBox(tr("Loop"), this);
    m_loopOn->setObjectName(QStringLiteral("sampleLoopOn"));
    m_loopOn->setChecked(defaults.loopOn);
    connect(m_loopOn, &QCheckBox::toggled, this,
            [this] { applyParamsFromUi(-1); });
    m_loopStart = makeSpin("sampleLoopStart", 0, frames - 1,
                           int(defaults.loopStart), 3);
    m_loopEnd = makeSpin("sampleLoopEnd", 0, frames - 1,
                         int(defaults.loopEnd), 4);
    auto *loopRow = new QHBoxLayout;
    loopRow->addWidget(m_loopOn);
    loopRow->addWidget(m_loopStart);
    loopRow->addWidget(new QLabel(tr("to"), this));
    loopRow->addWidget(m_loopEnd);
    loopRow->addStretch();
    form->addRow(tr("Loop (samples):"), loopRow);

    m_baseKey = makeSpin("sampleBaseKey", 0, 127, defaults.baseKey, 5);
    m_fineTune = new QDoubleSpinBox(this);
    m_fineTune->setObjectName(QStringLiteral("sampleFineTune"));
    m_fineTune->setRange(0.0, 99.99);
    m_fineTune->setDecimals(2);
    m_fineTune->setSuffix(tr(" cents"));
    m_fineTune->setValue(defaults.fineTuneCents);
    m_sourceCents = m_fineTune->value(); // spin-rounded source tuning
    m_fineTune->setKeyboardTracking(false);
    connect(m_fineTune, &QDoubleSpinBox::valueChanged, this,
            [this] { applyParamsFromUi(6); });
    m_pitchLabel = new QLabel(this);
    m_pitchLabel->setObjectName(QStringLiteral("samplePitchLabel"));
    m_pitchApply = new QPushButton(tr("Apply"), this);
    m_pitchApply->setObjectName(QStringLiteral("samplePitchApply"));
    m_pitchApply->setToolTip(
        tr("Set the base key and cents from the detected pitch."));
    connect(m_pitchApply, &QPushButton::clicked, this,
            &SampleEditorDialog::applyDetectedPitch);
    auto *keyRow = new QHBoxLayout;
    keyRow->addWidget(m_baseKey);
    keyRow->addWidget(new QLabel(QStringLiteral("+"), this));
    keyRow->addWidget(m_fineTune);
    keyRow->addSpacing(12);
    keyRow->addWidget(m_pitchLabel);
    keyRow->addWidget(m_pitchApply);
    keyRow->addStretch();
    form->addRow(tr("Base key:"), keyRow);
    if (m_pitchTried) {
        // Prefill already ran (no container metadata); show the result.
        ensurePitchDetected();
    } else {
        m_pitchLabel->setText(tr("Detect pitch:"));
        m_pitchApply->setText(tr("Detect"));
        m_pitchApply->setToolTip(
            tr("Detect the sample's pitch and show it here."));
    }

    m_rateCombo = new QComboBox(this);
    m_rateCombo->setObjectName(QStringLiteral("sampleRateCombo"));
    m_rateCombo->setEditable(true);
    m_rateCombo->addItem(tr("Keep source (%1 Hz)")
                             .arg(src.sampleRate, 0, 'f',
                                  src.sampleRate == std::floor(src.sampleRate)
                                      ? 0
                                      : 2));
    for (const int rate : kGbaMixRates)
        m_rateCombo->addItem(QString::number(rate));
    m_rateCombo->setCurrentIndex(0);
    // Apply on commit (preset pick, Enter, focus-out) — not per keystroke:
    // every apply is a full synchronous pipeline render (~120 ms/5 s of
    // source), too heavy to run while a custom rate is being typed.
    connect(m_rateCombo, &QComboBox::currentIndexChanged, this,
            [this] { applyParamsFromUi(-1); });
    connect(m_rateCombo->lineEdit(), &QLineEdit::editingFinished, this,
            [this] { applyParamsFromUi(-1); });
    form->addRow(tr("Target rate (Hz):"), m_rateCombo);

    m_normalizeMode = new QComboBox(this);
    m_normalizeMode->setObjectName(QStringLiteral("sampleNormalizeMode"));
    m_normalizeMode->addItems({tr("Auto"), tr("Looped (−9 dBFS loop RMS)"),
                               tr("One-shot (peak)"), tr("Off")});
    m_normalizeMode->setCurrentIndex(int(defaults.normalizeMode));
    connect(m_normalizeMode, &QComboBox::currentIndexChanged, this,
            [this] { applyParamsFromUi(-1); });
    m_gainReadout = new QLabel(this);
    m_gainReadout->setObjectName(QStringLiteral("sampleGainReadout"));
    auto *normRow = new QHBoxLayout;
    normRow->addWidget(m_normalizeMode);
    normRow->addWidget(m_gainReadout);
    normRow->addStretch();
    form->addRow(tr("Normalize:"), normRow);

    m_crossfade = new QCheckBox(
        tr("Crossfade the seam (bakes the loop end into the pre-loop "
           "material)"),
        this);
    m_crossfade->setObjectName(QStringLiteral("sampleCrossfade"));
    m_crossfade->setChecked(defaults.crossfadeOn);
    connect(m_crossfade, &QCheckBox::toggled, this,
            [this] { applyParamsFromUi(-1); });
    form->addRow(tr("Loop seam:"), m_crossfade);

    layout->addLayout(form);

    // ---- audition strip (engine slots, PLAN.md §4) ----
    auto *audition = new QHBoxLayout;
    m_playOnce = new QPushButton(tr("Play once"), this);
    m_playOnce->setObjectName(QStringLiteral("sampleAuditionOnce"));
    connect(m_playOnce, &QPushButton::clicked, this,
            [this] { startAudition(false); });
    m_playLoop = new QPushButton(tr("Play looped"), this);
    m_playLoop->setObjectName(QStringLiteral("sampleAuditionLoop"));
    connect(m_playLoop, &QPushButton::clicked, this,
            [this] { startAudition(true); });
    m_stopButton = new QPushButton(tr("Stop"), this);
    m_stopButton->setObjectName(QStringLiteral("sampleAuditionStop"));
    connect(m_stopButton, &QPushButton::clicked, this,
            &SampleEditorDialog::stopAudition);
    m_auditionKey = new QSpinBox(this);
    m_auditionKey->setObjectName(QStringLiteral("sampleAuditionKey"));
    m_auditionKey->setRange(0, 127);
    m_auditionKey->setValue(60);
    m_auditionKey->setToolTip(tr("Audition MIDI key."));
    connect(m_auditionKey, &QSpinBox::valueChanged, this, [this] {
        if (m_auditionMode != AuditionMode::None)
            startAudition(m_auditionMode == AuditionMode::Loop);
    });
    m_seamSolo = new QCheckBox(tr("Loop seam solo"), this);
    m_seamSolo->setObjectName(QStringLiteral("sampleSeamSolo"));
    m_seamSolo->setToolTip(
        tr("Audition ±150 ms around the loop seam on repeat."));
    connect(m_seamSolo, &QCheckBox::toggled, this, [this] {
        if (m_auditionMode == AuditionMode::Loop)
            startAudition(true);
    });
    audition->addWidget(m_playOnce);
    audition->addWidget(m_playLoop);
    audition->addWidget(m_stopButton);
    audition->addWidget(new QLabel(tr("Key:"), this));
    audition->addWidget(m_auditionKey);
    audition->addWidget(m_seamSolo);
    m_useDestAdsr = new QCheckBox(tr("Use destination voice ADSR"), this);
    m_useDestAdsr->setObjectName(QStringLiteral("sampleDestAdsr"));
    m_useDestAdsr->setChecked(m_hasDestAdsr);
    m_useDestAdsr->setVisible(m_hasDestAdsr);
    connect(m_useDestAdsr, &QCheckBox::toggled, this, [this] {
        if (m_auditionMode != AuditionMode::None)
            startAudition(m_auditionMode == AuditionMode::Loop);
    });
    audition->addWidget(m_useDestAdsr);
    audition->addStretch();
    layout->addLayout(audition);
    if (!m_engine) {
        for (QWidget *w : std::initializer_list<QWidget *>{
                 m_playOnce, m_playLoop, m_stopButton, m_auditionKey,
                 m_seamSolo}) {
            w->setEnabled(false);
            w->setToolTip(tr("Audio is unavailable."));
        }
    }
    m_auditionTimer.setInterval(33);
    connect(&m_auditionTimer, &QTimer::timeout, this,
            &SampleEditorDialog::auditionTick);

    m_outputSummary = new QLabel(this);
    m_outputSummary->setObjectName(QStringLiteral("sampleOutputSummary"));
    m_outputSummary->setWordWrap(true);
    layout->addWidget(m_outputSummary);

    auto *nameForm = new QFormLayout;
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setObjectName(QStringLiteral("sampleNameEdit"));
    m_nameEdit->setText(src.suggestedName);
    nameForm->addRow(tr("Name:"), m_nameEdit);
    layout->addLayout(nameForm);

    m_nameStatus = new QLabel(this);
    m_nameStatus->setObjectName(QStringLiteral("sampleNameStatus"));
    m_nameStatus->setWordWrap(true);
    layout->addWidget(m_nameStatus);

    auto *note = new QLabel(
        tr("wav2agb pipeline detected — the exported .wav lands in "
           "sound/direct_sound_samples/ and the build's %.bin rule compiles "
           "it."),
        this);
    note->setWordWrap(true);
    layout->addWidget(note);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_addButton = buttons->addButton(tr("Add to Project"),
                                     QDialogButtonBox::AcceptRole);
    m_addButton->setObjectName(QStringLiteral("sampleAddButton"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    // Dialog-local undo (PLAN.md §3): param edits only; nothing
    // project-visible exists until commit.
    auto *undoSc = new QShortcut(QKeySequence::Undo, this);
    connect(undoSc, &QShortcut::activated, &m_undo, &QUndoStack::undo);
    auto *redoSc = new QShortcut(QKeySequence::Redo, this);
    connect(redoSc, &QShortcut::activated, &m_undo, &QUndoStack::redo);

    connect(m_nameEdit, &QLineEdit::textChanged, this,
            &SampleEditorDialog::validateName);
    validateName();
    refreshOutputs();
}

QString SampleEditorDialog::sampleName() const
{
    return m_nameEdit->text();
}

QByteArray SampleEditorDialog::wavBytes()
{
    return writeSampleWav(m_doc.processed());
}

void SampleEditorDialog::done(int result)
{
    stopAudition();
    QDialog::done(result);
}

void SampleEditorDialog::applyParamsFromUi(int mergeKey)
{
    if (m_syncing)
        return;
    const ImportedSample &src = m_doc.source();
    SampleEditParams p = m_doc.params();
    p.cropStart = m_cropStart->value();
    p.cropEnd = m_cropEnd->value();
    p.loopOn = m_loopOn->isChecked();
    p.loopStart = m_loopStart->value();
    p.loopEnd = m_loopEnd->value();
    p.baseKey = m_baseKey->value();
    p.fineTuneCents = m_fineTune->value();
    p.crossfadeOn = m_crossfade->isChecked();
    bool rateOk = false;
    const double rate = m_rateCombo->currentText().toDouble(&rateOk);
    p.targetRate = rateOk && rate > 0.0 ? rate : src.sampleRate;
    p.normalizeMode =
        SampleEditParams::NormalizeMode(m_normalizeMode->currentIndex());
    // The source agbp word stays authoritative only while the pipeline
    // leaves pitch untouched (identity rate, source key/tuning). Tuning
    // compares against the spin's own rounded rendition of the source
    // fraction — both sides are spin values, so equality is exact.
    const bool pitchUntouched = src.exactPitch != 0
        && p.targetRate == src.sampleRate && p.baseKey == src.baseKey
        && p.fineTuneCents == m_sourceCents;
    p.exactPitchOverride = pitchUntouched ? src.exactPitch : 0;
    commitParams(p, mergeKey);
}

void SampleEditorDialog::commitParams(const SampleEditParams &params,
                                      int mergeKey)
{
    const SampleEditParams before = m_doc.params();
    if (params == before)
        return;
    m_doc.setParams(params);
    m_undo.push(new SampleParamsCommand(this, before, params, mergeKey));
    m_syncing = true;
    syncUiFromParams();
    m_syncing = false;
    refreshOutputs();
    republishAudition();
}

void SampleEditorDialog::applyParamsExternal(const SampleEditParams &params)
{
    m_doc.setParams(params);
    m_syncing = true;
    syncUiFromParams();
    m_syncing = false;
    refreshOutputs();
    republishAudition();
}

void SampleEditorDialog::syncUiFromParams()
{
    const SampleEditParams &p = m_doc.params();
    const ImportedSample &src = m_doc.source();
    m_cropStart->setValue(int(p.cropStart));
    m_cropEnd->setValue(int(p.cropEnd));
    m_loopOn->setChecked(p.loopOn);
    m_loopStart->setValue(int(p.loopStart));
    m_loopEnd->setValue(int(p.loopEnd));
    m_baseKey->setValue(p.baseKey);
    m_fineTune->setValue(p.fineTuneCents);
    m_crossfade->setChecked(p.crossfadeOn);
    m_normalizeMode->setCurrentIndex(int(p.normalizeMode));
    if (p.targetRate == src.sampleRate) {
        if (m_rateCombo->currentIndex() != 0)
            m_rateCombo->setCurrentIndex(0);
    } else {
        const QString text = QString::number(p.targetRate);
        if (m_rateCombo->currentText() != text)
            m_rateCombo->setEditText(text);
    }
}

void SampleEditorDialog::refreshOutputs()
{
    const ProcessedSample &out = m_doc.processed();
    const SampleEditParams &p = m_doc.params();
    m_loopStart->setEnabled(m_loopOn->isChecked());
    m_loopEnd->setEnabled(m_loopOn->isChecked());
    m_refineButton->setEnabled(p.loopOn);
    m_crossfade->setEnabled(p.loopOn);
    if (m_engine) {
        m_playLoop->setEnabled(out.looped);
        m_seamSolo->setEnabled(out.looped);
    }
    m_waveform->setMarkers(p.cropStart, p.cropEnd, p.loopStart, p.loopEnd,
                           p.loopOn);

    const double gainDb =
        out.normalizeGain > 0.0 ? 20.0 * std::log10(out.normalizeGain) : 0.0;
    m_gainReadout->setText(
        p.normalizeMode == SampleEditParams::NormalizeOff
            ? tr("gain 0.0 dB")
            : tr("gain %1 dB").arg(gainDb, 0, 'f', 1));

    // The seam badge (DSP.md §6): green amp ≤ 2 LSB and deriv ≤ 3
    // LSB/sample, amber up to double those, red otherwise.
    if (out.looped && out.seam.valid) {
        const bool green = out.seam.ampLsb <= 2 && out.seam.derivLsb <= 3;
        const bool amber = out.seam.ampLsb <= 4 && out.seam.derivLsb <= 6;
        m_seamBadge->setVisible(true);
        m_seamBadge->setText(green ? tr("seam: clean")
                                   : amber ? tr("seam: fair")
                                           : tr("seam: click"));
        m_seamBadge->setStyleSheet(
            QStringLiteral("background: %1; color: black; border-radius: 3px;")
                .arg(green ? QStringLiteral("#7CCB7C")
                           : amber ? QStringLiteral("#E0C060")
                                   : QStringLiteral("#E08080")));
    } else {
        m_seamBadge->setVisible(false);
    }

    QStringList lines;
    QString first =
        tr("Output: %1 samples @ %2 Hz").arg(out.size).arg(out.declaredRate);
    first += out.looped
        ? tr(" — loop %1..%2").arg(out.loopStart).arg(out.size)
        : tr(" — one-shot");
    if (out.outputRate > 0)
        first += QStringLiteral(" (%1 s)").arg(
            double(out.size) / out.outputRate, 0, 'f', 2);
    lines += first;
    lines += tr("Pitch: %1 Hz at C4 (60) — agbp %2, unity %3 (%4)")
                 .arg(double(out.freq) / 1024.0, 0, 'f', 2)
                 .arg(out.freq)
                 .arg(out.unityNote)
                 .arg(midiKeyName(out.unityNote));
    const quint32 romBytes = 16 + ((out.size + 3) & ~quint32(3));
    QString cost = tr("ROM cost: %1 bytes").arg(romBytes);
    if (out.seam.valid) {
        cost += tr(" — seam amp %1 LSB, slope %2")
                    .arg(out.seam.ampLsb)
                    .arg(out.seam.derivLsb);
        if (out.seam.nccValid)
            cost += tr(", match %1%").arg(int(out.seam.ncc * 100.0));
    }
    lines += cost;
    for (const QString &w : m_doc.source().warnings + out.warnings)
        lines += tr("Warning: %1").arg(w);
    m_outputSummary->setText(lines.join(QLatin1Char('\n')));
}

void SampleEditorDialog::validateName()
{
    QString error;
    const bool ok = m_validator && m_validator(m_nameEdit->text(), &error);
    m_addButton->setEnabled(ok);
    m_nameStatus->setText(
        ok ? tr("Registers as DirectSoundWaveData_%1").arg(m_nameEdit->text())
           : error);
}

void SampleEditorDialog::ensurePitchDetected()
{
    if (!m_pitchTried) {
        m_pitchTried = true;
        // Run on pre-resample audio over the sustained region: the loop
        // region if set, else the middle 50% of the crop (DSP.md §4).
        const ImportedSample &src = m_doc.source();
        const SampleEditParams &p = m_doc.params();
        const float *x = src.buffer.data();
        qint64 from, len;
        if (p.loopOn && p.loopEnd > p.loopStart) {
            from = qBound<qint64>(0, p.loopStart, src.frameCount());
            len = qMin(p.loopEnd + 1, src.frameCount()) - from;
        } else {
            const qint64 cropLen = p.cropEnd - p.cropStart;
            from = p.cropStart + cropLen / 4;
            len = cropLen / 2;
        }
        if (len < 8192) { // too short for 3 frames — widen to everything
            from = 0;
            len = src.frameCount();
        }
        m_pitch = SampleDsp::detectPitchYin(x + from, len, src.sampleRate);
    }
    if (m_pitchLabel) {
        if (m_pitch.pitched) {
            const double exact =
                69.0 + 12.0 * std::log2(m_pitch.f0 / 440.0);
            const int nearest = qBound(0, int(std::lround(exact)), 127);
            const double cents = (exact - double(nearest)) * 100.0;
            m_pitchLabel->setText(
                tr("Detected: %1 %2%3¢ (%4 Hz)")
                    .arg(midiKeyName(nearest))
                    .arg(cents >= 0 ? QStringLiteral("+") : QString())
                    .arg(cents, 0, 'f', 0)
                    .arg(m_pitch.f0, 0, 'f', 1));
            m_pitchApply->setText(tr("Apply"));
            m_pitchApply->setEnabled(true);
        } else {
            m_pitchLabel->setText(tr("Detected: unpitched"));
            m_pitchApply->setText(tr("Apply"));
            m_pitchApply->setEnabled(false);
        }
    }
}

void SampleEditorDialog::applyDetectedPitch()
{
    // First click on a metadata-carrying source is "Detect": run the
    // detection and display it; applying takes a second, deliberate click
    // (DSP.md §4 — never silently applied).
    const bool firstDetection = !m_pitchTried;
    ensurePitchDetected();
    if (firstDetection || !m_pitch.pitched)
        return;
    const double exact = 69.0 + 12.0 * std::log2(m_pitch.f0 / 440.0);
    SampleEditParams p = m_doc.params();
    p.baseKey = qBound(0, int(std::floor(exact)), 127);
    p.fineTuneCents = qBound(
        0.0, std::round((exact - std::floor(exact)) * 10000.0) / 100.0,
        99.99);
    p.exactPitchOverride = 0;
    commitParams(p, -1);
}

// The loop analysis render: the whole crop as a fade-free one-shot on the
// current target grid, so candidate ends keep their untrimmed tail (the
// seam comparison needs real samples past E — DSP.md §6).
SampleEditParams SampleEditorDialog::analysisParams() const
{
    SampleEditParams p = m_doc.params();
    p.loopOn = false;
    p.fadeIn = false;
    p.fadeOut = false;
    p.crossfadeOn = false;
    p.ditherOn = false;
    if (p.normalizeMode != SampleEditParams::NormalizeOff)
        p.normalizeMode = SampleEditParams::NormalizeOneShot;
    return p;
}

void SampleEditorDialog::suggestLoops()
{
    ensurePitchDetected();
    for (QPushButton *chip : m_chipButtons)
        chip->deleteLater();
    m_chipButtons.clear();
    m_chips.clear();

    SampleDocument analysis(m_doc.source());
    analysis.setParams(analysisParams());
    const ProcessedSample &ana = analysis.processed();
    const qint64 n = qint64(ana.preview.size());
    const double srcRate = m_doc.source().sampleRate;
    const double ratio = srcRate > 0.0 ? ana.outputRate / srcRate : 1.0;
    if (n < 256) {
        m_suggestStatus->setText(
            tr("sample too short for a loop search — set markers manually."));
        return;
    }
    const double period =
        m_pitch.pitched ? ana.outputRate / m_pitch.f0 : 0.0;
    const std::vector<SampleDsp::LoopCandidate> cands =
        SampleDsp::suggestLoop(ana.preview.data(), n, ana.outputRate, period,
                               qint64(std::llround(0.40 * double(n))), n - 1);
    if (cands.empty()) {
        m_suggestStatus->setText(tr("no loop candidates found."));
        return;
    }

    const qint8 *s8 = reinterpret_cast<const qint8 *>(ana.s8.constData());
    const qint64 cropStart = m_doc.params().cropStart;
    bool anyClean = false;
    for (const SampleDsp::LoopCandidate &cand : cands) {
        Chip chip;
        chip.cand = cand;
        chip.metrics =
            SampleDsp::seamMetricsAt(s8, n, cand.loopStart, cand.loopEnd);
        chip.srcStart =
            cropStart + qint64(std::llround(double(cand.loopStart) / ratio));
        chip.srcEnd =
            cropStart + qint64(std::llround(double(cand.loopEnd) / ratio));
        anyClean = anyClean || cand.passedGates;
        m_chips.push_back(chip);
    }
    for (size_t i = 0; i < m_chips.size(); i++) {
        const Chip &chip = m_chips[i];
        auto *button = new QPushButton(
            tr("%1% · %2 s")
                .arg(int(chip.cand.ncc * 100.0))
                .arg(double(chip.cand.loopEnd + 1 - chip.cand.loopStart)
                         / ana.outputRate,
                     0, 'f', 2),
            this);
        button->setObjectName(QStringLiteral("sampleLoopChip%1").arg(i));
        button->setToolTip(
            tr("Loop %1..%2 — seam amp %3 LSB, slope %4%5")
                .arg(chip.srcStart)
                .arg(chip.srcEnd)
                .arg(chip.metrics.ampLsb)
                .arg(chip.metrics.derivLsb)
                .arg(chip.cand.passedGates ? QString()
                                           : tr(" (fails level gates)")));
        const int index = int(i);
        connect(button, &QPushButton::clicked, this,
                [this, index] { applyChip(index); });
        m_chipRow->addWidget(button);
        m_chipButtons.push_back(button);
    }
    m_suggestStatus->setText(
        anyClean ? QString()
                 : tr("no clean loop found — consider a crossfade bake."));
}

void SampleEditorDialog::applyChip(int index)
{
    if (index < 0 || size_t(index) >= m_chips.size())
        return;
    const Chip &chip = m_chips[size_t(index)];
    SampleEditParams p = m_doc.params();
    p.loopOn = true;
    p.loopStart = chip.srcStart;
    p.loopEnd = chip.srcEnd;
    commitParams(p, -1);
}

void SampleEditorDialog::refineCurrentLoop()
{
    const SampleEditParams &cur = m_doc.params();
    if (!cur.loopOn)
        return;
    ensurePitchDetected();
    SampleDocument analysis(m_doc.source());
    analysis.setParams(analysisParams());
    const ProcessedSample &ana = analysis.processed();
    const qint64 n = qint64(ana.preview.size());
    const double srcRate = m_doc.source().sampleRate;
    const double ratio = srcRate > 0.0 ? ana.outputRate / srcRate : 1.0;
    if (n < 32)
        return;
    qint64 S = qBound<qint64>(
        0, qint64(std::llround(double(cur.loopStart - cur.cropStart) * ratio)),
        n - 2);
    qint64 E = qBound<qint64>(
        S + 1, qint64(std::llround(double(cur.loopEnd - cur.cropStart) * ratio)),
        n - 1);
    const double period =
        m_pitch.pitched ? ana.outputRate / m_pitch.f0 : 0.0;
    SampleDsp::refineLoop(ana.preview.data(), n, period, &S, &E);
    SampleEditParams p = cur;
    p.loopStart = cur.cropStart + qint64(std::llround(double(S) / ratio));
    p.loopEnd = cur.cropStart + qint64(std::llround(double(E) / ratio));
    commitParams(p, -1);
}

void SampleEditorDialog::startAudition(bool looped)
{
    if (!m_engine)
        return;
    const ProcessedSample &out = m_doc.processed();
    if (out.s8.isEmpty())
        return;
    if (looped && !out.looped)
        looped = false;

    QByteArray bytes;
    quint32 loopStart = 0;
    bool loopFlag = looped;
    m_auditionSeamSolo = false;
    if (looped && m_seamSolo->isChecked()) {
        bytes = seamSoloBytes(out);
        loopFlag = true;
        m_auditionSeamSolo = true;
    } else {
        bytes = out.s8;
        loopStart = out.loopStart;
        // "Play once" on a looped render: drop the loop flag so the note
        // plays through the data once and the channel ends itself.
    }

    AuditionSlots::Adsr adsr;
    if (m_hasDestAdsr && m_useDestAdsr->isChecked())
        adsr = m_destAdsr;
    const uint8_t key = uint8_t(m_auditionKey->value());
    m_republishPending = !m_engine->auditionSample(bytes, out.freq, loopStart,
                                                   loopFlag, key, adsr);
    m_auditionMode = looped ? AuditionMode::Loop : AuditionMode::Once;
    m_auditionLooped = loopFlag;
    m_auditionSize = quint32(bytes.size());
    m_auditionLoopStart = loopStart;
    m_auditionRate = double(out.freq) / 1024.0
        * std::pow(2.0, (double(key) - 60.0) / 12.0);
    const double srcRate = m_doc.source().sampleRate;
    m_auditionRatio = out.outputRate > 0.0 && srcRate > 0.0
        ? out.outputRate / srcRate
        : 1.0;
    m_auditionCrop = m_doc.params().cropStart;
    m_auditionPos = 0.0;
    m_auditionClock.restart();
    m_auditionTimer.start();
}

void SampleEditorDialog::stopAudition()
{
    if (m_engine)
        m_engine->auditionSampleOff();
    m_auditionMode = AuditionMode::None;
    m_republishPending = false;
    m_auditionTimer.stop();
    if (m_waveform)
        m_waveform->setPlayhead(-1);
}

// Live re-render: keep a sounding looped audition current with the params
// (loop-handle drags, crossfade toggles, retune). Publish failures coalesce
// — the tick retries.
void SampleEditorDialog::republishAudition()
{
    if (m_auditionMode == AuditionMode::Loop)
        startAudition(true);
}

void SampleEditorDialog::auditionTick()
{
    if (m_auditionMode == AuditionMode::None) {
        m_auditionTimer.stop();
        return;
    }
    if (m_republishPending) {
        startAudition(m_auditionMode == AuditionMode::Loop);
        return;
    }
    // Display-only playhead approximation: advance at the channel's playback
    // rate and wrap in the loop, mapped back to source coordinates.
    const double dt = double(m_auditionClock.restart()) / 1000.0;
    m_auditionPos += dt * m_auditionRate;
    if (m_auditionLooped) {
        const double S = double(m_auditionLoopStart);
        const double len = double(m_auditionSize) - S;
        if (len > 0.0 && m_auditionPos >= double(m_auditionSize))
            m_auditionPos = S + std::fmod(m_auditionPos - S, len);
    } else if (m_auditionPos >= double(m_auditionSize)) {
        stopAudition(); // the channel ended itself at the data end
        return;
    }
    if (m_auditionSeamSolo) {
        m_waveform->setPlayhead(-1);
        return;
    }
    m_waveform->setPlayhead(
        m_auditionCrop
        + qint64(std::llround(m_auditionPos / m_auditionRatio)));
}
