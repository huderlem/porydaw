#pragma once

#include "enginesettings.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QSlider;
class QSpinBox;

// Settings window, Audio page: porydaw's own output level and the global
// GBA-engine accuracy knobs. Every control applies as it changes — there
// is no OK/Cancel staging anywhere in the Settings window — so the page
// only reports; the main window owns applying and persisting.
class AudioSettingsPage : public QWidget
{
    Q_OBJECT

  public:
    // Output level in percent of unity. Up to 200 % because the engine's
    // output sits a few dB under the emulator's, so "louder than unity" is
    // the usual reason to reach for it.
    static constexpr int kOutputLevelMax = 200;
    static constexpr int kOutputLevelDefault = 100;

    AudioSettingsPage(int outputLevel, const EngineSettings &engine, QWidget *parent = nullptr);

    int outputLevel() const;
    EngineSettings engineSettings() const;

    // Programmatic loads (startup, Restore Defaults) go through these and
    // still emit when the values change, so the owner never has to
    // special-case them.
    void setOutputLevel(int percent);
    void setEngineSettings(const EngineSettings &settings);

  signals:
    void outputLevelChanged(int percent);
    void engineSettingsChanged(const EngineSettings &settings);

  private:
    void emitEngineSettings();

    QSlider *m_outputSlider = nullptr;
    QLabel *m_outputValue = nullptr;
    QSpinBox *m_polyphony = nullptr;
    QComboBox *m_mixRate = nullptr;
    QCheckBox *m_analogFilter = nullptr;
    // Set while widgets are being loaded programmatically, so a struct
    // restore reports once at the end instead of per widget.
    bool m_loading = false;
    // Last reported engine settings; unchanged values never re-report.
    EngineSettings m_reported;
};
