#include "sampleeditordialog.h"

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "m4asemantics.h"

namespace {

QString formatLine(const SampleWavInfo &info)
{
    const QString kind = info.formatTag == 3 ? QStringLiteral("float")
                                             : QStringLiteral("PCM");
    QString text = QStringLiteral("%1-bit %2 mono, %3 Hz, %4 samples")
                       .arg(info.bitsPerSample)
                       .arg(kind)
                       .arg(info.sampleRate)
                       .arg(info.numSamples);
    if (info.sampleRate > 0)
        text += QStringLiteral(" (%1 s)").arg(
            double(info.numSamples) / double(info.sampleRate), 0, 'f', 2);
    return text;
}

QString pitchLine(const SampleWavInfo &info)
{
    // waveFreq is the Q10 sample-playback rate at middle C — the one number
    // the build and the engine agree on, however it was derived.
    QString text = QStringLiteral("%1 Hz at C4 (60)")
                       .arg(double(info.waveFreq) / 1024.0, 0, 'f', 2);
    if (info.agbPitch != 0)
        text += QStringLiteral(" — agbp %1").arg(info.agbPitch);
    else if (info.hasSmpl)
        text += QStringLiteral(" — from smpl unity note %1 (%2)")
                    .arg(info.midiKey)
                    .arg(midiKeyName(int(info.midiKey)));
    else
        text += QStringLiteral(" — no pitch metadata, declared rate assumed");
    return text;
}

QString loopLine(const SampleWavInfo &info)
{
    if (!info.waveLooped)
        return QStringLiteral("one-shot (%1 samples play)").arg(info.waveSize);
    QString text = QStringLiteral("loop %1..%2")
                       .arg(info.waveLoopStart)
                       .arg(info.waveSize);
    if (info.agbLoopEnd != 0 && info.agbLoopEnd != info.loopEndIncl + 1)
        text += QStringLiteral(" (agbl override)");
    return text;
}

} // namespace

SampleEditorDialog::SampleEditorDialog(const QString &sourcePath,
                                       const SampleWavInfo &info,
                                       NameValidator validator, QWidget *parent)
    : QDialog(parent), m_info(info), m_validator(std::move(validator))
{
    setWindowTitle(tr("Sample Studio"));

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    auto *sourceLabel = new QLabel(QFileInfo(sourcePath).fileName(), this);
    sourceLabel->setToolTip(sourcePath);
    form->addRow(tr("Source:"), sourceLabel);
    form->addRow(tr("Format:"), new QLabel(formatLine(info), this));
    form->addRow(tr("Pitch:"), new QLabel(pitchLine(info), this));
    form->addRow(tr("Loop:"), new QLabel(loopLine(info), this));
    const quint32 romBytes = 16 + ((info.numSamples + 3) & ~quint32(3));
    form->addRow(tr("ROM cost:"),
                 new QLabel(tr("%1 bytes").arg(romBytes), this));

    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setObjectName(QStringLiteral("sampleNameEdit"));
    m_nameEdit->setText(SampleRegistrar::sanitizeSampleName(
        QFileInfo(sourcePath).completeBaseName()));
    form->addRow(tr("Name:"), m_nameEdit);
    layout->addLayout(form);

    m_nameStatus = new QLabel(this);
    m_nameStatus->setObjectName(QStringLiteral("sampleNameStatus"));
    m_nameStatus->setWordWrap(true);
    layout->addWidget(m_nameStatus);

    auto *note = new QLabel(
        tr("wav2agb pipeline detected — the file is copied verbatim into "
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
}

QString SampleEditorDialog::sampleName() const
{
    return m_nameEdit->text();
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
