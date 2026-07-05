#include "audio/wavexport.h"

#include <QCoreApplication>
#include <QFile>

#include <algorithm>
#include <vector>

#include "core/timelineplayer.h"

extern "C" {
#include "m4a_engine.h"
#include "m4a_reverb.h"
}

namespace {

void putU16(QByteArray &out, uint16_t v)
{
    out.append(char(v & 0xFF));
    out.append(char(v >> 8));
}

void putU32(QByteArray &out, uint32_t v)
{
    out.append(char(v & 0xFF));
    out.append(char((v >> 8) & 0xFF));
    out.append(char((v >> 16) & 0xFF));
    out.append(char(v >> 24));
}

int16_t toPcm16(float sample)
{
    const int32_t v = int32_t(sample * 32767.0f);
    return int16_t(std::clamp(v, -32768, 32767));
}

} // namespace

WavExportTotals wavExportTotals(const MidiTimeline &timeline, const WavExportOptions &opts)
{
    WavExportTotals totals;
    const double rate = double(opts.sampleRate);
    if (timeline.hasLoop()) {
        const uint64_t loopDuration = timeline.loopEndSample - timeline.loopStartSample;
        totals.fadeStartSample =
            timeline.loopStartSample + uint64_t(opts.loopCount) * loopDuration;
        totals.totalSamples =
            totals.fadeStartSample + uint64_t(opts.fadeoutSeconds * rate + 0.5);
    } else {
        totals.totalSamples =
            timeline.lengthSamples + uint64_t(opts.tailSeconds * rate + 0.5);
    }
    return totals;
}

bool exportWav(const QString &path, const MidiTimeline &timeline,
               const LoadedVoiceGroup *voicegroup, const SongSettings &settings,
               const WavExportOptions &opts,
               const std::function<bool(double)> &progress, QString *error)
{
    const WavExportTotals totals = wavExportTotals(timeline, opts);
    const uint64_t dataSize = totals.totalSamples * 4; // 16-bit stereo
    if (totals.totalSamples == 0) {
        *error = QCoreApplication::translate("WavExport", "Nothing to render.");
        return false;
    }
    if (dataSize + 36 > UINT32_MAX) {
        *error = QCoreApplication::translate(
            "WavExport", "The rendered file would exceed the 4 GB WAV limit — "
                         "reduce the loop count.");
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = QCoreApplication::translate("WavExport", "Cannot write %1: %2")
                     .arg(path, file.errorString());
        return false;
    }

    QByteArray header;
    header.append("RIFF");
    putU32(header, uint32_t(36 + dataSize));
    header.append("WAVE");
    header.append("fmt ");
    putU32(header, 16);
    putU16(header, 1); // PCM
    putU16(header, 2); // stereo
    putU32(header, uint32_t(opts.sampleRate));
    putU32(header, uint32_t(opts.sampleRate) * 4); // byte rate
    putU16(header, 4);                             // block align
    putU16(header, 16);                            // bits per sample
    header.append("data");
    putU32(header, uint32_t(dataSize));

    // The engine only reads the voicegroup, so sharing it with a running
    // AudioEngine is safe; set_voicegroup just takes a non-const pointer.
    M4AEngine engine;
    m4a_engine_init(&engine, float(opts.sampleRate));
    m4a_engine_set_voicegroup(&engine, const_cast<ToneData *>(voicegroup->voices));
    m4a_engine_set_song_volume(&engine, settings.songVolume);
    m4a_reverb_set_amount(&engine.reverb, settings.reverb);
    engine.maxPcmChannels = settings.maxPcmChannels;
    m4a_engine_set_pcm_mix_rate(&engine, settings.pcmMixRate);
    engine.analogFilter = settings.analogFilter;

    constexpr uint32_t kChunk = 4096;
    std::vector<float> bufL(kChunk), bufR(kChunk);
    QByteArray pcm;
    pcm.reserve(kChunk * 4);

    bool ok = file.write(header) == header.size();
    bool cancelled = false;
    TimelinePlayer player;
    player.reset();
    uint64_t pos = 0;
    while (ok && pos < totals.totalSamples) {
        const uint32_t n = uint32_t(std::min<uint64_t>(kChunk, totals.totalSamples - pos));
        player.render(&engine, &timeline, bufL.data(), bufR.data(), n,
                      timeline.hasLoop(), 0);

        pcm.resize(int(n) * 4);
        char *out = pcm.data();
        const uint64_t fadeLength = totals.totalSamples - totals.fadeStartSample;
        for (uint32_t i = 0; i < n; i++) {
            float gain = 1.0f;
            if (pos + i >= totals.fadeStartSample)
                gain = 1.0f - float(pos + i - totals.fadeStartSample) / float(fadeLength);
            const int16_t l = toPcm16(bufL[i] * gain);
            const int16_t r = toPcm16(bufR[i] * gain);
            out[i * 4 + 0] = char(uint16_t(l) & 0xFF);
            out[i * 4 + 1] = char(uint16_t(l) >> 8);
            out[i * 4 + 2] = char(uint16_t(r) & 0xFF);
            out[i * 4 + 3] = char(uint16_t(r) >> 8);
        }
        ok = file.write(pcm) == pcm.size();
        pos += n;

        if (ok && progress && !progress(double(pos) / double(totals.totalSamples))) {
            cancelled = true;
            break;
        }
    }
    m4a_engine_destroy(&engine);

    if (!ok) {
        *error = QCoreApplication::translate("WavExport", "Cannot write %1: %2")
                     .arg(path, file.errorString());
        file.close();
        file.remove();
        return false;
    }
    if (cancelled) {
        error->clear();
        file.close();
        file.remove();
        return false;
    }
    if (!file.flush()) {
        *error = QCoreApplication::translate("WavExport", "Cannot write %1: %2")
                     .arg(path, file.errorString());
        file.close();
        file.remove();
        return false;
    }
    file.close();
    return true;
}
