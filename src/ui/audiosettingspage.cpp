#include "audiosettingspage.h"

#include "layout.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdlib>

extern "C" {
#include "m4a_engine.h"
}

namespace {

constexpr int kGbaDefaultRate = 13379;

// Half-width of the unity detent: drags within this many percent of 100
// stick there, so "back to normal" is findable without a reset gesture.
constexpr int kOutputLevelDetent = 4;

// Double-click resets to unity, and drags detent there.
class OutputLevelSlider : public QSlider
{
  public:
    explicit OutputLevelSlider(QWidget *parent) : QSlider(Qt::Horizontal, parent)
    {
        setRange(0, AudioSettingsPage::kOutputLevelMax);
        setValue(AudioSettingsPage::kOutputLevelDefault);
        setTickPosition(QSlider::TicksBelow);
        setTickInterval(AudioSettingsPage::kOutputLevelDefault);
        setSingleStep(5);
        setPageStep(25);
        connect(this, &QSlider::sliderMoved, this, [this](int position) {
            if (std::abs(position - AudioSettingsPage::kOutputLevelDefault) <= kOutputLevelDetent)
                setSliderPosition(AudioSettingsPage::kOutputLevelDefault);
        });
    }

  protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            setValue(AudioSettingsPage::kOutputLevelDefault);
            event->accept();
            return;
        }
        QSlider::mouseDoubleClickEvent(event);
    }
};

} // namespace

AudioSettingsPage::AudioSettingsPage(int outputLevel, const EngineSettings &engine, QWidget *parent)
    : QWidget(parent)
{
    // Output: a listening-level gain on everything the app plays (song and
    // auditions alike), never part of the song and never in exported WAVs.
    auto *outputGroup = new QGroupBox(tr("Output"), this);
    const QString outputTip = tr("How loud porydaw plays, from silent to twice unity. Only "
                                 "affects listening — it is not saved with the song and never "
                                 "changes exported WAVs. Double-click to reset to 100%.");
    m_outputSlider = new OutputLevelSlider(this);
    m_outputSlider->setObjectName(QStringLiteral("settingsOutputLevel"));
    m_outputSlider->setToolTip(outputTip);
    m_outputValue = new QLabel(this);
    m_outputValue->setObjectName(QStringLiteral("settingsOutputValue"));
    m_outputValue->setToolTip(outputTip);
    m_outputValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    // Fixed to the widest reading so the row doesn't breathe while dragging.
    m_outputValue->setFixedWidth(
        m_outputValue->fontMetrics().horizontalAdvance(tr("%1%").arg(kOutputLevelMax)) +
        ::layout::space(::layout::Space::One));
    auto *outputRow = new QHBoxLayout;
    outputRow->setContentsMargins(0, 0, 0, 0);
    outputRow->addWidget(m_outputSlider, 1);
    outputRow->addWidget(m_outputValue);
    auto *outputForm = new QFormLayout(outputGroup);
    outputForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    outputForm->addRow(tr("Output level:"), outputRow);
    auto *outputCaption = new QLabel(
        tr("porydaw's own listening level. It applies to playback and every audition, is "
           "never saved with the song, and never affects exported WAVs."),
        this);
    outputCaption->setForegroundRole(QPalette::PlaceholderText);
    outputCaption->setWordWrap(true);
    outputForm->addRow(QString(), outputCaption);

    // GBA engine: poryaaaa's global accuracy knobs (SPEC §7).
    auto *engineGroup = new QGroupBox(tr("GBA engine"), this);
    auto *engineForm = new QFormLayout(engineGroup);
    engineForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    m_polyphony = new QSpinBox(this);
    m_polyphony->setObjectName(QStringLiteral("settingsPcmPolyphony"));
    m_polyphony->setRange(1, MAX_PCM_CHANNELS);
    m_polyphony->setSuffix(tr(" channels"));
    // Typed values land once on commit (Enter/focus-out), not per keystroke,
    // so the engine isn't reconfigured for every digit.
    m_polyphony->setKeyboardTracking(false);
    m_polyphony->setToolTip(tr("Maximum simultaneous PCM (DirectSound) notes. "
                               "pokeemerald's m4aSoundInit uses 5; the engine "
                               "supports up to %1.")
                                .arg(MAX_PCM_CHANNELS));
    engineForm->addRow(tr("&PCM polyphony:"), m_polyphony);

    m_mixRate = new QComboBox(this);
    m_mixRate->setObjectName(QStringLiteral("settingsPcmMixRate"));
    for (int rate : kGbaMixRates) {
        const QString label =
            rate == kGbaDefaultRate ? tr("%1 Hz (GBA default)").arg(rate) : tr("%1 Hz").arg(rate);
        m_mixRate->addItem(label, rate);
    }
    m_mixRate->addItem(tr("Host rate (clean, no GBA resampling)"), 0);
    m_mixRate->setToolTip(tr("The GBA's DirectSound mixing rate (m4aSoundInit "
                             "frequency). 13379 Hz makes high notes alias the way "
                             "they do in-game; the host rate mixes cleanly at the "
                             "audio device's rate."));
    engineForm->addRow(tr("PCM &mix rate:"), m_mixRate);

    m_analogFilter = new QCheckBox(tr("GBA analog output filter (low-pass)"), this);
    m_analogFilter->setObjectName(QStringLiteral("settingsAnalogFilter"));
    m_analogFilter->setToolTip(tr("Emulates the rolloff of the GBA's analog output "
                                  "circuit (mGBA's low-pass filter). On sounds like "
                                  "hardware; off is the raw mixer output."));
    engineForm->addRow(QString(), m_analogFilter);

    auto *defaultsButton = new QPushButton(tr("Restore Defaults"), this);
    defaultsButton->setObjectName(QStringLiteral("settingsAudioRestoreDefaults"));
    // The only push button in the window would otherwise become its default
    // button, so Enter in the polyphony spinbox (its commit gesture) would
    // also reset every knob on the page.
    defaultsButton->setAutoDefault(false);
    auto *buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->addStretch(1);
    buttonRow->addWidget(defaultsButton);

    auto *pageLayout = new QVBoxLayout(this);
    pageLayout->addWidget(outputGroup);
    pageLayout->addWidget(engineGroup);
    pageLayout->addStretch(1);
    pageLayout->addLayout(buttonRow);

    connect(m_outputSlider, &QSlider::valueChanged, this, [this](int percent) {
        m_outputValue->setText(tr("%1%").arg(percent));
        if (!m_loading)
            emit outputLevelChanged(percent);
    });
    connect(m_polyphony, &QSpinBox::valueChanged, this, &AudioSettingsPage::emitEngineSettings);
    connect(m_mixRate, &QComboBox::currentIndexChanged, this,
            &AudioSettingsPage::emitEngineSettings);
    connect(m_analogFilter, &QCheckBox::toggled, this, &AudioSettingsPage::emitEngineSettings);
    connect(defaultsButton, &QPushButton::clicked, this, [this] {
        setOutputLevel(kOutputLevelDefault);
        setEngineSettings(EngineSettings());
    });

    // Seed without emitting: the owner already holds these values.
    m_loading = true;
    setOutputLevel(outputLevel);
    setEngineSettings(engine);
    m_loading = false;
    m_reported = engineSettings();
    // setValue is a no-op at the default, so seed the readout explicitly.
    m_outputValue->setText(tr("%1%").arg(m_outputSlider->value()));
}

int AudioSettingsPage::outputLevel() const
{
    return m_outputSlider->value();
}

void AudioSettingsPage::setOutputLevel(int percent)
{
    m_outputSlider->setValue(std::clamp(percent, 0, kOutputLevelMax));
}

EngineSettings AudioSettingsPage::engineSettings() const
{
    EngineSettings s;
    s.maxPcmChannels = m_polyphony->value();
    s.pcmMixRate = float(m_mixRate->currentData().toInt());
    s.analogFilter = m_analogFilter->isChecked();
    return s;
}

// One engineSettingsChanged for the whole struct, not one per widget with
// the others still half-restored.
void AudioSettingsPage::setEngineSettings(const EngineSettings &settings)
{
    const bool wasLoading = m_loading;
    m_loading = true;
    m_polyphony->setValue(settings.maxPcmChannels);
    const int rate = int(settings.pcmMixRate + 0.5f);
    int idx = m_mixRate->findData(rate);
    if (idx < 0) { // hand-edited QSettings value: keep it selectable
        m_mixRate->addItem(tr("%1 Hz (custom)").arg(rate), rate);
        idx = m_mixRate->count() - 1;
    }
    m_mixRate->setCurrentIndex(idx);
    m_analogFilter->setChecked(settings.analogFilter);
    m_loading = wasLoading;
    emitEngineSettings();
}

// Only a real change reports: the owner restarts the audio device on every
// report, so Restore Defaults on factory knobs must stay silent.
void AudioSettingsPage::emitEngineSettings()
{
    if (m_loading)
        return;
    const EngineSettings settings = engineSettings();
    if (settings == m_reported)
        return;
    m_reported = settings;
    emit engineSettingsChanged(settings);
}
