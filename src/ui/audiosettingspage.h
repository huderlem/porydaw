#pragma once

#include "audio/audiodeviceconfig.h"
#include "enginesettings.h"
#include "ui/cpubudget.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QSlider;
class QSpinBox;

// Settings window, Audio page: porydaw's own output level, the output
// device's buffering, and the global GBA-engine accuracy knobs. Every
// control applies as it changes — there is no OK/Cancel staging anywhere
// in the Settings window — so the page only reports; the main window owns
// applying and persisting.
class AudioSettingsPage : public QWidget
{
    Q_OBJECT

  public:
    // Output level in percent of unity. Up to 200 % because the engine's
    // output sits a few dB under the emulator's, so "louder than unity" is
    // the usual reason to reach for it.
    static constexpr int kOutputLevelMax = 200;
    static constexpr int kOutputLevelDefault = 100;
    // Metronome volume in percent (docs/metronome/PLAN.md M7).
    static constexpr int kMetronomeVolumeDefault = 70;
    // Record's count-in in bars, 0 = off (M5); also a punch take's
    // pre-roll (docs/punch/PLAN.md P4).
    static constexpr int kCountInBarsMax = 2;
    static constexpr int kCountInBarsDefault = 1;
    // The CPU budget in cycles per frame (ui/cpubudget.h).
    static constexpr int kCpuBudgetMin = cpubudget::kMin;
    static constexpr int kCpuBudgetMax = cpubudget::kMax;
    static constexpr int kCpuBudgetDefault = cpubudget::kDefault;
    static constexpr int kCpuBudgetStep = cpubudget::kStep;
    // `cycles` as a whole percent of the VBlank window, rounded to nearest:
    // the caption beside the spin box and scripting's derived budgetPercent
    // (porydaw.audio.cpu()).
    static int cpuBudgetPercentOf(int cycles);

    AudioSettingsPage(int outputLevel, const EngineSettings &engine, QWidget *parent = nullptr);

    // Buffer sizes offered, in ms (0 = the backend's default).
    static constexpr int kBufferSizesMs[] = {0, 2, 3, 5, 10, 20, 40};

    int outputLevel() const;
    AudioDeviceConfig deviceConfig() const;
    int metronomeVolume() const;
    int countInBars() const;
    int cpuBudget() const;
    EngineSettings engineSettings() const;

    // Programmatic loads (startup, Restore Defaults) go through these and
    // still emit when the values change, so the owner never has to
    // special-case them.
    void setOutputLevel(int percent);
    void setMetronomeVolume(int percent);
    void setCountInBars(int bars);
    void setCpuBudget(int cycles);
    void setEngineSettings(const EngineSettings &settings);
    void setDeviceConfig(const AudioDeviceConfig &config);
    // What the device actually opened with, and why it differs from the
    // request if it does (warning, or empty). Display only.
    void setDeviceStatus(const QString &status, const QString &warning);

  signals:
    void outputLevelChanged(int percent);
    void metronomeVolumeChanged(int percent);
    void countInBarsChanged(int bars);
    void cpuBudgetChanged(int cycles);
    void engineSettingsChanged(const EngineSettings &settings);
    void deviceConfigChanged(const AudioDeviceConfig &config);

  private:
    void emitEngineSettings();
    void emitDeviceConfig();
    void updateDefaultLabel();
    // The HQ mixer has no 7884 Hz: that rate greys out while it is on, and
    // a selection sitting on it moves to the GBA default.
    void updateMixRateItems();

    QSlider *m_outputSlider = nullptr;
    QLabel *m_outputValue = nullptr;
    QSlider *m_metronomeSlider = nullptr;
    QLabel *m_metronomeValue = nullptr;
    QComboBox *m_countIn = nullptr;
    QSpinBox *m_polyphony = nullptr;
    QComboBox *m_mixRate = nullptr;
    QCheckBox *m_hqMixer = nullptr;
    QSpinBox *m_cpuBudget = nullptr;
    QLabel *m_cpuBudgetPercent = nullptr; // "72% of the VBlank window"
    QComboBox *m_bufferSize = nullptr;
    QCheckBox *m_exclusive = nullptr; // null off Windows
    QLabel *m_deviceStatus = nullptr;
    QLabel *m_deviceWarning = nullptr;
    // Set while widgets are being loaded programmatically, so a struct
    // restore reports once at the end instead of per widget.
    bool m_loading = false;
    // Last reported engine settings; unchanged values never re-report.
    EngineSettings m_reported;
    AudioDeviceConfig m_reportedDevice;
};
