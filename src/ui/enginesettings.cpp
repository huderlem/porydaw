#include "enginesettings.h"

#include <QSettings>
#include <QString>
#include <QtGlobal>

#include "core/m4a.h"

const int kGbaMixRates[12] = {5734,  7884,  10512, 13379, 15768, 18157,
                              21024, 26758, 31536, 36314, 40137, 42048};

namespace {

const QString kKeyPolyphony = QStringLiteral("engine/maxPcmChannels");
const QString kKeyMixRate = QStringLiteral("engine/pcmMixRate");
const QString kKeyHqMixer = QStringLiteral("engine/hqMixer");

} // namespace

bool isGbaMixRate(int rate)
{
    for (int r : kGbaMixRates)
        if (r == rate)
            return true;
    return false;
}

EngineSettings EngineSettings::load()
{
    const EngineSettings defaults;
    QSettings qs;
    EngineSettings s;
    s.maxPcmChannels =
        qBound(1, qs.value(kKeyPolyphony, defaults.maxPcmChannels).toInt(), int(MAX_PCM_CHANNELS));
    // Only m4aSoundInit's rates exist now: a hand-edited value, or the old
    // "0 = host rate" choice that predates the GBA hardware model, falls back
    // to the game's rate.
    const int rate = qs.value(kKeyMixRate, double(defaults.pcmMixRate)).toInt();
    s.hqMixer = qs.value(kKeyHqMixer, defaults.hqMixer).toBool();
    // ... and the HQ mixer has no 7884 Hz: hold the rate the engine will
    // actually play, so the Settings page and porydaw.audio.engine agree.
    s.pcmMixRate = isGbaMixRate(rate) && !(s.hqMixer && rate == kGbaMixRateHqUnsupported)
                       ? float(rate)
                       : defaults.pcmMixRate;
    return s;
}

void EngineSettings::save() const
{
    QSettings qs;
    qs.setValue(kKeyPolyphony, maxPcmChannels);
    qs.setValue(kKeyMixRate, double(pcmMixRate));
    qs.setValue(kKeyHqMixer, hqMixer);
}
