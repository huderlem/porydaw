#include "enginesettings.h"

#include <QSettings>
#include <QString>
#include <QtGlobal>

extern "C" {
#include "m4a_engine.h"
}

const int kGbaMixRates[12] = {5734,  7884,  10512, 13379, 15768, 18157,
                              21024, 26758, 31536, 36314, 40137, 42048};

namespace {

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
        qBound(1, qs.value(kKeyPolyphony, defaults.maxPcmChannels).toInt(), int(MAX_PCM_CHANNELS));
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
