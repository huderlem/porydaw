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
#include <QSpinBox>
#include <QVBoxLayout>

#include <climits>
#include <cmath>

#include "audio/samplewav.h"
#include "enginesettingsdialog.h"
#include "m4asemantics.h"
#include "project/samplereg.h"

namespace {

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
    default:
        kind = QStringLiteral("audio");
        break;
    }
    QString text = QStringLiteral("%1-bit %2, %3 channel%4, %5 Hz, %6 samples")
                       .arg(s.sourceBits)
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

} // namespace

SampleEditorDialog::SampleEditorDialog(ImportedSample sample,
                                       NameValidator validator,
                                       QWidget *parent)
    : QDialog(parent), m_doc(std::move(sample)),
      m_validator(std::move(validator))
{
    setWindowTitle(tr("Sample Studio"));
    const ImportedSample &src = m_doc.source();
    const SampleEditParams defaults = m_doc.params();
    const int frames = int(qMin<qint64>(src.frameCount(), INT_MAX));

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    auto *sourceLabel = new QLabel(QFileInfo(src.sourcePath).fileName(), this);
    sourceLabel->setToolTip(src.sourcePath);
    form->addRow(tr("Source:"), sourceLabel);
    form->addRow(tr("Format:"), new QLabel(sourceLine(src), this));

    const auto makeSpin = [this](const char *name, int min, int max,
                                 int value) {
        auto *spin = new QSpinBox(this);
        spin->setObjectName(QLatin1String(name));
        spin->setRange(min, max);
        spin->setValue(value);
        spin->setKeyboardTracking(false);
        connect(spin, &QSpinBox::valueChanged, this,
                &SampleEditorDialog::applyParamsFromUi);
        return spin;
    };

    m_cropStart = makeSpin("sampleCropStart", 0, frames - 1,
                           int(defaults.cropStart));
    m_cropEnd = makeSpin("sampleCropEnd", 1, frames, int(defaults.cropEnd));
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
            &SampleEditorDialog::applyParamsFromUi);
    m_loopStart = makeSpin("sampleLoopStart", 0, frames - 1,
                           int(defaults.loopStart));
    m_loopEnd = makeSpin("sampleLoopEnd", 0, frames - 1,
                         int(defaults.loopEnd));
    auto *loopRow = new QHBoxLayout;
    loopRow->addWidget(m_loopOn);
    loopRow->addWidget(m_loopStart);
    loopRow->addWidget(new QLabel(tr("to"), this));
    loopRow->addWidget(m_loopEnd);
    loopRow->addStretch();
    form->addRow(tr("Loop (samples):"), loopRow);

    m_baseKey = makeSpin("sampleBaseKey", 0, 127, defaults.baseKey);
    m_fineTune = new QDoubleSpinBox(this);
    m_fineTune->setObjectName(QStringLiteral("sampleFineTune"));
    m_fineTune->setRange(0.0, 99.99);
    m_fineTune->setDecimals(2);
    m_fineTune->setSuffix(tr(" cents"));
    m_fineTune->setValue(defaults.fineTuneCents);
    m_fineTune->setKeyboardTracking(false);
    connect(m_fineTune, &QDoubleSpinBox::valueChanged, this,
            &SampleEditorDialog::applyParamsFromUi);
    auto *keyRow = new QHBoxLayout;
    keyRow->addWidget(m_baseKey);
    keyRow->addWidget(new QLabel(QStringLiteral("+"), this));
    keyRow->addWidget(m_fineTune);
    keyRow->addStretch();
    form->addRow(tr("Base key:"), keyRow);

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
    connect(m_rateCombo, &QComboBox::editTextChanged, this,
            &SampleEditorDialog::applyParamsFromUi);
    form->addRow(tr("Target rate (Hz):"), m_rateCombo);

    m_normalizeMode = new QComboBox(this);
    m_normalizeMode->setObjectName(QStringLiteral("sampleNormalizeMode"));
    m_normalizeMode->addItems({tr("Auto"), tr("Looped (−9 dBFS loop RMS)"),
                               tr("One-shot (peak)"), tr("Off")});
    m_normalizeMode->setCurrentIndex(int(defaults.normalizeMode));
    connect(m_normalizeMode, &QComboBox::currentIndexChanged, this,
            &SampleEditorDialog::applyParamsFromUi);
    m_gainReadout = new QLabel(this);
    m_gainReadout->setObjectName(QStringLiteral("sampleGainReadout"));
    auto *normRow = new QHBoxLayout;
    normRow->addWidget(m_normalizeMode);
    normRow->addWidget(m_gainReadout);
    normRow->addStretch();
    form->addRow(tr("Normalize:"), normRow);

    layout->addLayout(form);

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

void SampleEditorDialog::applyParamsFromUi()
{
    if (m_updatingUi)
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
    bool rateOk = false;
    const double rate = m_rateCombo->currentText().toDouble(&rateOk);
    p.targetRate = rateOk && rate > 0.0 ? rate : src.sampleRate;
    p.normalizeMode =
        SampleEditParams::NormalizeMode(m_normalizeMode->currentIndex());
    // The source agbp word stays authoritative only while the pipeline
    // leaves pitch untouched (identity rate, source key/tuning).
    const bool pitchUntouched = src.exactPitch != 0
        && p.targetRate == src.sampleRate && p.baseKey == src.baseKey
        && std::abs(p.fineTuneCents - src.fracSemitone * 100.0) < 1e-9;
    p.exactPitchOverride = pitchUntouched ? src.exactPitch : 0;
    m_doc.setParams(p);
    refreshOutputs();
}

void SampleEditorDialog::refreshOutputs()
{
    const ProcessedSample &out = m_doc.processed();
    m_loopStart->setEnabled(m_loopOn->isChecked());
    m_loopEnd->setEnabled(m_loopOn->isChecked());

    const double gainDb =
        out.normalizeGain > 0.0 ? 20.0 * std::log10(out.normalizeGain) : 0.0;
    m_gainReadout->setText(
        m_doc.params().normalizeMode == SampleEditParams::NormalizeOff
            ? tr("gain 0.0 dB")
            : tr("gain %1 dB").arg(gainDb, 0, 'f', 1));

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
    if (out.seam.valid)
        cost += tr(" — seam amp %1 LSB, slope %2, match %3%")
                    .arg(out.seam.ampLsb)
                    .arg(out.seam.derivLsb)
                    .arg(int(out.seam.ncc * 100.0));
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
