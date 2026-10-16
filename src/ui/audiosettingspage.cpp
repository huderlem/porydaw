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

// A horizontal slider that double-click resets to its default.
class ResettableSlider : public QSlider
{
  public:
    ResettableSlider(int defaultValue, QWidget *parent)
        : QSlider(Qt::Horizontal, parent)
        , m_default(defaultValue)
    {}

  protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            setValue(m_default);
            event->accept();
            return;
        }
        QSlider::mouseDoubleClickEvent(event);
    }

  private:
    int m_default;
};

// Double-click resets to unity, and drags detent there.
class OutputLevelSlider : public ResettableSlider
{
  public:
    explicit OutputLevelSlider(QWidget *parent)
        : ResettableSlider(AudioSettingsPage::kOutputLevelDefault, parent)
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

    // Output device: how far ahead the device buffers, which is the delay
    // between a key on a MIDI keyboard and its sound.
    auto *deviceGroup = new QGroupBox(tr("Output device"), this);
    auto *deviceForm = new QFormLayout(deviceGroup);
    deviceForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_bufferSize = new QComboBox(this);
    m_bufferSize->setObjectName(QStringLiteral("settingsBufferSize"));
    for (int ms : kBufferSizesMs)
        m_bufferSize->addItem(ms == 0 ? tr("Default") : tr("%1 ms").arg(ms), ms);
    m_bufferSize->setToolTip(tr("How much audio the device buffers ahead. Smaller makes a MIDI "
                                "keyboard more responsive; too small crackles. Default is "
                                "about 10 ms, or %1 ms in exclusive mode. Windows may round a "
                                "shared-mode request up to its own minimum.")
                                 .arg(AudioDeviceConfig::kExclusiveDefaultPeriodMs));
    deviceForm->addRow(tr("&Buffer size:"), m_bufferSize);
#ifdef Q_OS_WIN
    m_exclusive = new QCheckBox(tr("Exclusive mode (lowest latency)"), this);
    m_exclusive->setObjectName(QStringLiteral("settingsExclusiveMode"));
    m_exclusive->setToolTip(tr("Opens the device in WASAPI exclusive mode, bypassing the Windows "
                               "mixer, for latency close to ASIO's. While porydaw runs, other "
                               "apps can't play sound on this device."));
    deviceForm->addRow(QString(), m_exclusive);
#endif
    m_deviceStatus = new QLabel(this);
    m_deviceStatus->setObjectName(QStringLiteral("settingsDeviceStatus"));
    m_deviceStatus->setForegroundRole(QPalette::PlaceholderText);
    m_deviceStatus->setWordWrap(true);
    deviceForm->addRow(QString(), m_deviceStatus);
    m_deviceWarning = new QLabel(this);
    m_deviceWarning->setObjectName(QStringLiteral("settingsDeviceWarning"));
    m_deviceWarning->setWordWrap(true);
    m_deviceWarning->hide();
    deviceForm->addRow(QString(), m_deviceWarning);

    // Metronome: the click's own level, under the output level like the song.
    auto *metronomeGroup = new QGroupBox(tr("Metronome"), this);
    const QString metronomeTip = tr("How loud the metronome clicks, relative to the song. The "
                                    "click never goes into exported WAVs. Double-click to reset "
                                    "to %1%.")
                                     .arg(kMetronomeVolumeDefault);
    m_metronomeSlider = new ResettableSlider(kMetronomeVolumeDefault, this);
    m_metronomeSlider->setObjectName(QStringLiteral("settingsMetronomeVolume"));
    m_metronomeSlider->setRange(0, 100);
    m_metronomeSlider->setSingleStep(5);
    m_metronomeSlider->setPageStep(25);
    m_metronomeSlider->setValue(kMetronomeVolumeDefault);
    m_metronomeSlider->setToolTip(metronomeTip);
    m_metronomeValue = new QLabel(this);
    m_metronomeValue->setObjectName(QStringLiteral("settingsMetronomeValue"));
    m_metronomeValue->setToolTip(metronomeTip);
    m_metronomeValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_metronomeValue->setFixedWidth(m_outputValue->minimumWidth());
    auto *metronomeRow = new QHBoxLayout;
    metronomeRow->setContentsMargins(0, 0, 0, 0);
    metronomeRow->addWidget(m_metronomeSlider, 1);
    metronomeRow->addWidget(m_metronomeValue);
    auto *metronomeForm = new QFormLayout(metronomeGroup);
    metronomeForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    metronomeForm->addRow(tr("Volume:"), metronomeRow);
    m_countIn = new QComboBox(this);
    m_countIn->setObjectName(QStringLiteral("settingsCountIn"));
    m_countIn->addItem(tr("Off"), 0);
    m_countIn->addItem(tr("1 bar"), 1);
    m_countIn->addItem(tr("2 bars"), 2);
    m_countIn->setCurrentIndex(m_countIn->findData(kCountInBarsDefault));
    m_countIn->setToolTip(tr("Before Record starts the song, click this many bars at the tempo "
                             "where it starts, even with the metronome off. Not when Record "
                             "punches in during playback."));
    metronomeForm->addRow(tr("Count-in when recording:"), m_countIn);

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
    pageLayout->addWidget(deviceGroup);
    pageLayout->addWidget(metronomeGroup);
    pageLayout->addWidget(engineGroup);
    pageLayout->addStretch(1);
    pageLayout->addLayout(buttonRow);

    connect(m_outputSlider, &QSlider::valueChanged, this, [this](int percent) {
        m_outputValue->setText(tr("%1%").arg(percent));
        if (!m_loading)
            emit outputLevelChanged(percent);
    });
    connect(m_metronomeSlider, &QSlider::valueChanged, this, [this](int percent) {
        m_metronomeValue->setText(tr("%1%").arg(percent));
        if (!m_loading)
            emit metronomeVolumeChanged(percent);
    });
    connect(m_countIn, &QComboBox::currentIndexChanged, this, [this] {
        if (!m_loading)
            emit countInBarsChanged(countInBars());
    });
    connect(m_polyphony, &QSpinBox::valueChanged, this, &AudioSettingsPage::emitEngineSettings);
    connect(m_mixRate, &QComboBox::currentIndexChanged, this,
            &AudioSettingsPage::emitEngineSettings);
    connect(m_analogFilter, &QCheckBox::toggled, this, &AudioSettingsPage::emitEngineSettings);
    connect(m_bufferSize, &QComboBox::currentIndexChanged, this,
            &AudioSettingsPage::emitDeviceConfig);
    if (m_exclusive) {
        connect(m_exclusive, &QCheckBox::toggled, this, &AudioSettingsPage::emitDeviceConfig);
        connect(m_exclusive, &QCheckBox::toggled, this, &AudioSettingsPage::updateDefaultLabel);
    }
    updateDefaultLabel();
    connect(defaultsButton, &QPushButton::clicked, this, [this] {
        setOutputLevel(kOutputLevelDefault);
        setMetronomeVolume(kMetronomeVolumeDefault);
        setCountInBars(kCountInBarsDefault);
        setEngineSettings(EngineSettings());
        setDeviceConfig(AudioDeviceConfig());
    });

    // Seed without emitting: the owner already holds these values.
    m_loading = true;
    setOutputLevel(outputLevel);
    setEngineSettings(engine);
    m_loading = false;
    m_reported = engineSettings();
    m_reportedDevice = deviceConfig();
    // setValue is a no-op at the default, so seed the readout explicitly.
    m_outputValue->setText(tr("%1%").arg(m_outputSlider->value()));
    m_metronomeValue->setText(tr("%1%").arg(m_metronomeSlider->value()));
}

int AudioSettingsPage::outputLevel() const
{
    return m_outputSlider->value();
}

void AudioSettingsPage::setOutputLevel(int percent)
{
    m_outputSlider->setValue(std::clamp(percent, 0, kOutputLevelMax));
}

int AudioSettingsPage::metronomeVolume() const
{
    return m_metronomeSlider->value();
}

void AudioSettingsPage::setMetronomeVolume(int percent)
{
    m_metronomeSlider->setValue(std::clamp(percent, 0, 100));
}

int AudioSettingsPage::countInBars() const
{
    return m_countIn->currentData().toInt();
}

void AudioSettingsPage::setCountInBars(int bars)
{
    m_countIn->setCurrentIndex(m_countIn->findData(std::clamp(bars, 0, kCountInBarsMax)));
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

AudioDeviceConfig AudioSettingsPage::deviceConfig() const
{
    AudioDeviceConfig config;
    config.periodMs = m_bufferSize->currentData().toInt();
    config.exclusive = m_exclusive && m_exclusive->isChecked();
    return config;
}

void AudioSettingsPage::setDeviceConfig(const AudioDeviceConfig &config)
{
    const bool wasLoading = m_loading;
    m_loading = true;
    int idx = m_bufferSize->findData(config.periodMs);
    if (idx < 0) { // hand-edited QSettings value: keep it selectable
        m_bufferSize->addItem(tr("%1 ms (custom)").arg(config.periodMs), config.periodMs);
        idx = m_bufferSize->count() - 1;
    }
    m_bufferSize->setCurrentIndex(idx);
    if (m_exclusive)
        m_exclusive->setChecked(config.exclusive);
    m_loading = wasLoading;
    emitDeviceConfig();
}

void AudioSettingsPage::setDeviceStatus(const QString &status, const QString &warning)
{
    m_deviceStatus->setText(status);
    m_deviceWarning->setText(warning);
    m_deviceWarning->setVisible(!warning.isEmpty());
}

// Like the engine knobs: each report reopens the device, so only a real
// change reports.
void AudioSettingsPage::emitDeviceConfig()
{
    if (m_loading)
        return;
    const AudioDeviceConfig config = deviceConfig();
    if (config == m_reportedDevice)
        return;
    m_reportedDevice = config;
    emit deviceConfigChanged(config);
}

// Default means a different size per mode; the item says which.
void AudioSettingsPage::updateDefaultLabel()
{
    const int idx = m_bufferSize->findData(0);
    if (idx < 0)
        return;
    m_bufferSize->setItemText(idx, m_exclusive && m_exclusive->isChecked()
                                       ? tr("Default (%1 ms)")
                                             .arg(AudioDeviceConfig::kExclusiveDefaultPeriodMs)
                                       : tr("Default (about 10 ms)"));
}
