#include <QFile>
#include <QString>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "audio/wavexport.h"
#include "core/songdocument.h"
#include "project/decompproject.h"

extern "C" {
#include "voicegroup_loader.h"
}

// --exportcheck <projectRoot> <song>: WAV export check. Renders the song
// offline through the export path and verifies the RIFF header, the
// poryaaaa_render-parity length math, that the audio is not silent, that a
// looping export actually fades to silence, and that cancelling removes the
// partial file. Writes exportcheck.wav into the project root — use a copy.

namespace {

uint32_t leU32(const QByteArray &b, int off)
{
    return uint32_t(uint8_t(b[off])) | uint32_t(uint8_t(b[off + 1])) << 8
        | uint32_t(uint8_t(b[off + 2])) << 16 | uint32_t(uint8_t(b[off + 3])) << 24;
}

uint16_t leU16(const QByteArray &b, int off)
{
    return uint16_t(uint8_t(b[off])) | uint16_t(uint8_t(b[off + 1])) << 8;
}

int16_t sampleAt(const QByteArray &b, uint64_t index)
{
    return int16_t(leU16(b, int(44 + index * 2)));
}

} // namespace

int runExportCheck(const QString &projectRoot, const QString &songLabel)
{
    int failures = 0;
    const auto fail = [&failures](const char *msg) {
        std::fprintf(stderr, "exportcheck: FAIL: %s\n", msg);
        failures++;
    };

    DecompProject project;
    QString error;
    if (!project.open(projectRoot, &error)) {
        std::fprintf(stderr, "exportcheck: %s\n", qUtf8Printable(error));
        return 1;
    }
    const SongInfo *song = nullptr;
    for (const SongInfo &s : project.songs()) {
        if (s.label == songLabel && s.isPlayable())
            song = &s;
    }
    if (!song) {
        std::fprintf(stderr, "exportcheck: song '%s' not found\n",
                     qUtf8Printable(songLabel));
        return 1;
    }

    SongDocument doc;
    if (!doc.load(*song, &error)) {
        std::fprintf(stderr, "exportcheck: load: %s\n", qUtf8Printable(error));
        return 1;
    }

    LoadedVoiceGroup *vg = nullptr;
    const QByteArray rootUtf8 = projectRoot.toLocal8Bit();
    for (const QString &name : DecompProject::voicegroupCandidates(song->cfg)) {
        vg = voicegroup_load(rootUtf8.constData(), name.toLocal8Bit().constData(), nullptr);
        if (vg)
            break;
    }
    if (!vg) {
        std::fprintf(stderr, "exportcheck: voicegroup not found\n");
        return 1;
    }

    SongSettings settings;
    settings.songVolume = uint8_t(song->cfg.masterVolume);
    settings.reverb = uint8_t(song->cfg.reverb > 0 ? song->cfg.reverb : 0);

    WavExportOptions opts;
    opts.sampleRate = 44100; // deliberately not the device rate
    opts.loopCount = 1;
    opts.fadeoutSeconds = 1.0;
    opts.tailSeconds = 1.0;

    const auto timeline = doc.buildTimeline(double(opts.sampleRate));
    const WavExportTotals totals = wavExportTotals(*timeline, opts);
    const uint64_t loopDuration = timeline->hasLoop()
        ? timeline->loopEndSample - timeline->loopStartSample
        : 0;
    const uint64_t expectedTotal = timeline->hasLoop()
        ? timeline->loopStartSample + loopDuration + uint64_t(opts.sampleRate)
        : timeline->lengthSamples + uint64_t(opts.sampleRate);
    if (totals.totalSamples != expectedTotal)
        fail("length math does not match poryaaaa_render semantics");

    const QString wavPath = projectRoot + QStringLiteral("/exportcheck.wav");
    double lastFraction = -1.0;
    bool monotonic = true;
    if (!exportWav(wavPath, *timeline, vg, settings, opts,
                   [&](double fraction) {
                       monotonic = monotonic && fraction > lastFraction;
                       lastFraction = fraction;
                       return true;
                   },
                   &error)) {
        std::fprintf(stderr, "exportcheck: export: %s\n", qUtf8Printable(error));
        voicegroup_free(vg);
        return 1;
    }
    if (!monotonic || lastFraction != 1.0)
        fail("progress fractions not monotonic up to 1.0");

    QFile f(wavPath);
    QByteArray wav;
    if (f.open(QIODevice::ReadOnly))
        wav = f.readAll();
    const uint64_t dataSize = totals.totalSamples * 4;
    if (uint64_t(wav.size()) != 44 + dataSize)
        fail("file size does not match the rendered length");
    if (wav.size() >= 44) {
        if (std::memcmp(wav.constData(), "RIFF", 4) != 0
            || std::memcmp(wav.constData() + 8, "WAVE", 4) != 0
            || std::memcmp(wav.constData() + 12, "fmt ", 4) != 0
            || std::memcmp(wav.constData() + 36, "data", 4) != 0)
            fail("RIFF/WAVE chunk layout is wrong");
        else if (leU16(wav, 20) != 1 || leU16(wav, 22) != 2 || leU16(wav, 34) != 16
                 || leU32(wav, 24) != uint32_t(opts.sampleRate)
                 || leU32(wav, 40) != uint32_t(dataSize))
            fail("fmt/data header fields are wrong");
    }
    if (uint64_t(wav.size()) == 44 + dataSize) {
        int peak = 0;
        for (uint64_t i = 0; i < totals.totalSamples * 2; i++) {
            const int v = std::abs(int(sampleAt(wav, i)));
            peak = std::max(peak, v);
        }
        if (peak < 256)
            fail("rendered audio is (nearly) silent");
        if (timeline->hasLoop()) {
            int tailPeak = 0;
            for (uint64_t i = (totals.totalSamples - 16) * 2;
                 i < totals.totalSamples * 2; i++)
                tailPeak = std::max(tailPeak, std::abs(int(sampleAt(wav, i))));
            if (tailPeak > peak / 16)
                fail("fadeout did not reach silence");
        }
    }
    QFile::remove(wavPath);

    // Cancelled exports must clean up after themselves.
    if (exportWav(wavPath, *timeline, vg, settings, opts,
                  [](double) { return false; }, &error))
        fail("cancelled export reported success");
    else if (!error.isEmpty())
        fail("cancel produced an error instead of a clean abort");
    if (QFile::exists(wavPath))
        fail("cancelled export left a partial file behind");

    voicegroup_free(vg);
    std::printf("exportcheck: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
