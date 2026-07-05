#include "enginesettingsdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

extern "C" {
#include "m4a_engine.h"
}

namespace {

// m4aSoundInit's selectable DirectSound rates (SOUND_MODE_FREQ_*).
constexpr int kGbaMixRates[] = {5734,  7884,  10512, 13379, 15768, 18157,
                                21024, 26758, 31536, 36314, 40137, 42048};
constexpr int kGbaDefaultRate = 13379;

const QString kKeyPolyphony = QStringLiteral("engine/maxPcmChannels");
const QString kKeyMixRate = QStringLiteral("engine/pcmMixRate");
const QString kKeyAnalogFilter = QStringLiteral("engine/analogFilter");

} // namespace

EngineSettings EngineSettings::load()
{
    const EngineSettings defaults;
    QSettings qs;
    EngineSettings s;
    s.maxPcmChannels =
        qBound(1, qs.value(kKeyPolyphony, defaults.maxPcmChannels).toInt(),
               int(MAX_PCM_CHANNELS));
    s.pcmMixRate = qs.value(kKeyMixRate, double(defaults.pcmMixRate)).toFloat();
    if (s.pcmMixRate < 0.0f)
        s.pcmMixRate = defaults.pcmMixRate;
    s.analogFilter = qs.value(kKeyAnalogFilter, defaults.analogFilter).toBool();
    return s;
}

void EngineSettings::save() const
{
    QSettings qs;
    qs.setValue(kKeyPolyphony, maxPcmChannels);
    qs.setValue(kKeyMixRate, double(pcmMixRate));
    qs.setValue(kKeyAnalogFilter, analogFilter);
}

EngineSettingsDialog::EngineSettingsDialog(const EngineSettings &settings, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Engine Settings"));

    auto *form = new QFormLayout;

    m_polyphony = new QSpinBox(this);
    m_polyphony->setRange(1, MAX_PCM_CHANNELS);
    m_polyphony->setSuffix(tr(" channels"));
    m_polyphony->setToolTip(tr("Maximum simultaneous PCM (DirectSound) notes. "
                               "pokeemerald's m4aSoundInit uses 5; the engine "
                               "supports up to %1.")
                                .arg(MAX_PCM_CHANNELS));
    form->addRow(tr("&PCM polyphony:"), m_polyphony);

    m_mixRate = new QComboBox(this);
    for (int rate : kGbaMixRates) {
        const QString label = rate == kGbaDefaultRate
                                  ? tr("%1 Hz (GBA default)").arg(rate)
                                  : tr("%1 Hz").arg(rate);
        m_mixRate->addItem(label, rate);
    }
    m_mixRate->addItem(tr("Host rate (clean, no GBA resampling)"), 0);
    m_mixRate->setToolTip(tr("The GBA's DirectSound mixing rate (m4aSoundInit "
                             "frequency). 13379 Hz makes high notes alias the way "
                             "they do in-game; the host rate mixes cleanly at the "
                             "audio device's rate."));
    form->addRow(tr("PCM &mix rate:"), m_mixRate);

    m_analogFilter = new QCheckBox(tr("GBA analog output filter (low-pass)"), this);
    m_analogFilter->setToolTip(tr("Emulates the rolloff of the GBA's analog output "
                                  "circuit (mGBA's low-pass filter). On sounds like "
                                  "hardware; off is the raw mixer output."));

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_analogFilter);

    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    QPushButton *defaultsButton =
        buttons->addButton(tr("Restore Defaults"), QDialogButtonBox::ResetRole);
    connect(defaultsButton, &QPushButton::clicked, this,
            [this] { applyToWidgets(EngineSettings()); });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    applyToWidgets(settings);
}

void EngineSettingsDialog::applyToWidgets(const EngineSettings &settings)
{
    m_polyphony->setValue(settings.maxPcmChannels);
    const int rate = int(settings.pcmMixRate + 0.5f);
    int idx = m_mixRate->findData(rate);
    if (idx < 0) { // hand-edited QSettings value: keep it selectable
        m_mixRate->addItem(tr("%1 Hz (custom)").arg(rate), rate);
        idx = m_mixRate->count() - 1;
    }
    m_mixRate->setCurrentIndex(idx);
    m_analogFilter->setChecked(settings.analogFilter);
}

EngineSettings EngineSettingsDialog::settings() const
{
    EngineSettings s;
    s.maxPcmChannels = m_polyphony->value();
    s.pcmMixRate = float(m_mixRate->currentData().toInt());
    s.analogFilter = m_analogFilter->isChecked();
    return s;
}
