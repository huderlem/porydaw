// porydaw_render_cli — offline renderer used to verify that porydaw's
// playback path (MidiTimeline + TimelinePlayer + poryaaaa engine) produces
// output matching poryaaaa_render for the same song. Not shipped to users.
//
// Usage:
//   porydaw_render_cli <projectRoot> <voicegroup> <file.mid> <out.wav>
//       [--seconds N] [--sample-rate HZ] [--song-volume V] [--reverb R] [--no-loop]

#include <QString>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/miditimeline.h"
#include "core/timelineplayer.h"

extern "C" {
#include "m4a_engine.h"
#include "m4a_reverb.h"
#include "voicegroup_loader.h"
}

namespace {

void writeU16(FILE *f, uint16_t v)
{
    uint8_t b[2] = {uint8_t(v & 0xFF), uint8_t(v >> 8)};
    fwrite(b, 1, 2, f);
}

void writeU32(FILE *f, uint32_t v)
{
    uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
    fwrite(b, 1, 4, f);
}

int writeWav(const char *path, const std::vector<float> &left, const std::vector<float> &right,
             int sampleRate)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "Cannot open %s for writing\n", path);
        return -1;
    }
    const uint64_t numSamples = left.size();
    const uint32_t dataSize = uint32_t(numSamples * 2 * 2);

    fwrite("RIFF", 1, 4, f);
    writeU32(f, 36 + dataSize);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    writeU32(f, 16);
    writeU16(f, 1);
    writeU16(f, 2);
    writeU32(f, uint32_t(sampleRate));
    writeU32(f, uint32_t(sampleRate) * 4);
    writeU16(f, 4);
    writeU16(f, 16);
    fwrite("data", 1, 4, f);
    writeU32(f, dataSize);

    for (uint64_t i = 0; i < numSamples; i++) {
        int32_t l = int32_t(left[i] * 32767.0f);
        int32_t r = int32_t(right[i] * 32767.0f);
        l = l > 32767 ? 32767 : (l < -32768 ? -32768 : l);
        r = r > 32767 ? 32767 : (r < -32768 ? -32768 : r);
        writeU16(f, uint16_t(int16_t(l)));
        writeU16(f, uint16_t(int16_t(r)));
    }
    fclose(f);
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <projectRoot> <voicegroup> <file.mid> <out.wav> "
                        "[--seconds N] [--sample-rate HZ] [--song-volume V] [--reverb R] "
                        "[--no-loop]\n",
                argv[0]);
        return 1;
    }
    const char *projectRoot = argv[1];
    const char *vgName = argv[2];
    const char *midiPath = argv[3];
    const char *outPath = argv[4];
    double seconds = 20.0;
    int sampleRate = 48000;
    int songVolume = 127;
    int reverb = 0;
    bool loop = true;

    for (int i = 5; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--sample-rate") && i + 1 < argc)
            sampleRate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--song-volume") && i + 1 < argc)
            songVolume = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--reverb") && i + 1 < argc)
            reverb = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-loop"))
            loop = false;
        else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    QString error;
    auto timeline =
        MidiTimeline::load(QString::fromLocal8Bit(midiPath), double(sampleRate), &error);
    if (!timeline) {
        fprintf(stderr, "%s\n", error.toLocal8Bit().constData());
        return 1;
    }
    printf("Timeline: %zu events, length %.2f s, loop %s\n", timeline->events.size(),
           double(timeline->lengthSamples) / sampleRate, timeline->hasLoop() ? "yes" : "no");

    LoadedVoiceGroup *vg = voicegroup_load(projectRoot, vgName, nullptr);
    if (!vg) {
        fprintf(stderr, "Failed to load voicegroup '%s'\n", vgName);
        return 1;
    }

    M4AEngine engine;
    m4a_engine_init(&engine, float(sampleRate));
    m4a_engine_set_voicegroup(&engine, vg->voices);
    m4a_engine_set_song_volume(&engine, uint8_t(songVolume));
    m4a_reverb_set_amount(&engine.reverb, uint8_t(reverb));
    engine.maxPcmChannels = 5;
    m4a_engine_set_pcm_mix_rate(&engine, 13379.0f);

    const uint64_t totalSamples = uint64_t(seconds * sampleRate + 0.5);
    std::vector<float> outL(totalSamples), outR(totalSamples);

    TimelinePlayer player;
    player.reset();
    constexpr uint32_t kChunk = 4096;
    uint64_t pos = 0;
    while (pos < totalSamples) {
        const uint32_t n = uint32_t(std::min<uint64_t>(kChunk, totalSamples - pos));
        player.render(&engine, timeline.get(), outL.data() + pos, outR.data() + pos, n,
                      loop, 0);
        pos += n;
    }

    const int rc = writeWav(outPath, outL, outR, sampleRate);
    m4a_engine_destroy(&engine);
    voicegroup_free(vg);
    if (rc == 0)
        printf("Wrote %s (%.2f s @ %d Hz)\n", outPath, seconds, sampleRate);
    return rc;
}
