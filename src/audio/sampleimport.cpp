#include "sampleimport.h"

#include <QFile>
#include <QFileInfo>

#include <cmath>
#include <cstring>

#include "project/samplereg.h"

#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#include "dr_wav.h"

namespace {

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

// Interleaved hi-res frames → mono canonical floats (DSP.md §2): arithmetic
// channel mean, or channel 0 when leftOnly. Flags phase-cancelling stereo
// (negative full-file L/R correlation) so the caller can offer left-only.
void downmix(const std::vector<double> &interleaved, int channels,
             bool leftOnly, ImportedSample *out)
{
    const size_t frames = channels > 0 ? interleaved.size() / channels : 0;
    out->buffer.resize(frames);
    if (channels == 1 || leftOnly) {
        for (size_t i = 0; i < frames; i++)
            out->buffer[i] = float(interleaved[i * channels]);
        if (leftOnly && channels > 1)
            out->warnings += QStringLiteral("imported the left channel only.");
        return;
    }
    for (size_t i = 0; i < frames; i++) {
        double sum = 0.0;
        for (int c = 0; c < channels; c++)
            sum += interleaved[i * channels + c];
        out->buffer[i] = float(sum / channels);
    }
    if (channels == 2) {
        double lr = 0.0, ll = 0.0, rr = 0.0;
        for (size_t i = 0; i < frames; i++) {
            const double l = interleaved[i * 2], r = interleaved[i * 2 + 1];
            lr += l * r;
            ll += l * l;
            rr += r * r;
        }
        if (ll > 0.0 && rr > 0.0 && lr / std::sqrt(ll * rr) < 0.0) {
            out->phaseCancelStereo = true;
            out->warnings += QStringLiteral(
                "left and right channels are phase-cancelling — the mono mix "
                "may sound hollow; consider re-importing with the left "
                "channel only.");
        }
    }
}

void finishDiagnostics(ImportedSample *out)
{
    // DSP.md §5 guard: > 0.1 % of samples at full scale → already clipped.
    qint64 atFullScale = 0;
    for (const float v : out->buffer) {
        if (std::abs(v) >= 0.9999f)
            atFullScale++;
    }
    if (out->frameCount() > 0
        && double(atFullScale) / double(out->frameCount()) > 0.001)
        out->warnings += QStringLiteral(
            "source is already clipped (%1 samples at full scale).")
                             .arg(atFullScale);
}

// The agbp inverse of FORMATS.md §3: the exact rate the pitch word implies
// for the recorded unity note, so "keep source rate" round-trips fractional
// rates like 3344.75 Hz exactly.
double rateFromAgbPitch(quint32 agbp, double exactKey)
{
    return (double(agbp) / 1024.0)
        * std::pow(2.0, (exactKey - 60.0) / 12.0);
}

bool decodeWav(const QByteArray &bytes, bool leftOnly, ImportedSample *out,
               QString *error)
{
    drwav wav;
    if (!drwav_init_memory_with_metadata(&wav, bytes.constData(),
                                         size_t(bytes.size()), 0, nullptr))
        return fail(error,
                    QStringLiteral("the WAV file is corrupt or truncated."));

    const quint16 tag = wav.translatedFormatTag;
    const int bits = wav.bitsPerSample;
    const int channels = wav.channels;
    const quint64 frames = wav.totalPCMFrameCount;
    bool supported = false;
    if (tag == DR_WAVE_FORMAT_PCM)
        supported = bits == 8 || bits == 16 || bits == 24 || bits == 32;
    else if (tag == DR_WAVE_FORMAT_IEEE_FLOAT)
        supported = bits == 32 || bits == 64;
    if (!supported) {
        drwav_uninit(&wav);
        if (tag != DR_WAVE_FORMAT_PCM && tag != DR_WAVE_FORMAT_IEEE_FLOAT)
            return fail(error, QStringLiteral("unsupported WAV encoding "
                                              "(format tag %1) — save as PCM "
                                              "or float.")
                                   .arg(tag));
        return fail(error, QStringLiteral("unsupported WAV bit depth "
                                          "(%1-bit, format tag %2).")
                               .arg(bits)
                               .arg(tag));
    }
    if (channels < 1 || frames == 0) {
        drwav_uninit(&wav);
        return fail(error, QStringLiteral("no audio data."));
    }
    const double declaredRate = double(wav.sampleRate);

    // Decode the container's native frames and convert on the canonical
    // scale ourselves — dr_wav's own u8 float conversion is (x/255)·2 − 1,
    // not the (x − 128)/128 that DSP.md §2's bit-exact round-trip needs.
    // dr_wav clamps a lying data-chunk size to the actual buffer (its
    // onTell validation); this bound is the backstop so the allocation can
    // never be header-sized even if that vendored behavior changes.
    const size_t bytesPerFrame = size_t(bits / 8) * size_t(channels);
    if (frames > quint64(bytes.size()) / bytesPerFrame) {
        drwav_uninit(&wav);
        return fail(error,
                    QStringLiteral("the WAV file is corrupt or truncated."));
    }
    std::vector<quint8> raw(size_t(frames) * bytesPerFrame);
    const quint64 read = drwav_read_pcm_frames(&wav, frames, raw.data());

    // Metadata: smpl + the top-level unknown chunks agbp/agbl.
    quint32 unity = 60, pitchFraction = 0, agbp = 0, agbl = 0;
    bool hasSmpl = false, hasLoop = false;
    quint32 loopStart = 0, loopEndIncl = 0, loopType = 0;
    for (quint32 i = 0; i < wav.metadataCount; i++) {
        const drwav_metadata &m = wav.pMetadata[i];
        if (m.type == drwav_metadata_type_smpl) {
            hasSmpl = true;
            unity = qMin<quint32>(m.data.smpl.midiUnityNote, 127);
            pitchFraction = m.data.smpl.midiPitchFraction;
            if (m.data.smpl.sampleLoopCount >= 1 && m.data.smpl.pLoops) {
                loopType = m.data.smpl.pLoops[0].type;
                loopStart = m.data.smpl.pLoops[0].firstSampleOffset;
                loopEndIncl = m.data.smpl.pLoops[0].lastSampleOffset;
                hasLoop = true;
            }
        } else if (m.type == drwav_metadata_type_unknown
                   && m.data.unknown.chunkLocation
                       == drwav_metadata_location_top_level
                   && m.data.unknown.dataSizeInBytes >= 4) {
            const quint8 *d = m.data.unknown.pData;
            const quint32 value = quint32(d[0]) | quint32(d[1]) << 8
                | quint32(d[2]) << 16 | quint32(d[3]) << 24;
            if (std::memcmp(m.data.unknown.id, "agbp", 4) == 0)
                agbp = value;
            else if (std::memcmp(m.data.unknown.id, "agbl", 4) == 0)
                agbl = value;
        }
    }
    drwav_uninit(&wav);
    if (read < frames)
        return fail(error,
                    QStringLiteral("the WAV file is corrupt or truncated."));

    std::vector<double> interleaved(size_t(frames) * size_t(channels));
    const quint8 *p = raw.data();
    qint64 clamped = 0;
    for (size_t i = 0; i < interleaved.size(); i++) {
        double v = 0.0;
        if (tag == DR_WAVE_FORMAT_PCM && bits == 8) {
            v = (double(p[0]) - 128.0) / 128.0;
        } else if (tag == DR_WAVE_FORMAT_PCM && bits == 16) {
            v = double(qint16(quint16(p[0]) | quint16(p[1]) << 8)) / 32768.0;
        } else if (tag == DR_WAVE_FORMAT_PCM && bits == 24) {
            qint32 s = qint32(quint32(p[0]) | quint32(p[1]) << 8
                              | quint32(p[2]) << 16);
            if (s & 0x800000)
                s -= 0x1000000;
            v = double(s) / 8388608.0;
        } else if (tag == DR_WAVE_FORMAT_PCM && bits == 32) {
            qint32 s;
            std::memcpy(&s, p, 4);
            v = double(s) / 2147483648.0;
        } else if (bits == 32) {
            float f;
            std::memcpy(&f, p, 4);
            v = double(f);
        } else {
            std::memcpy(&v, p, 8);
        }
        if (v > 1.0) {
            v = 1.0;
            clamped++;
        } else if (v < -1.0) {
            v = -1.0;
            clamped++;
        }
        interleaved[i] = v;
        p += size_t(bits / 8);
    }
    if (clamped > 0)
        out->warnings += QStringLiteral(
                             "%1 float samples beyond ±1.0 were clamped.")
                             .arg(clamped);
    if (hasLoop && loopType != 0) {
        out->warnings += QStringLiteral(
            "the smpl loop is not a forward loop — ignored.");
        hasLoop = false;
    }

    downmix(interleaved, channels, leftOnly, out);
    out->sourceKind = ImportedSample::Wav;
    out->sourceChannels = channels;
    out->sourceBits = bits;
    out->sourceFloat = tag == DR_WAVE_FORMAT_IEEE_FLOAT;
    out->gbaReady = tag == DR_WAVE_FORMAT_PCM && bits == 8 && channels == 1;
    out->baseKey = hasSmpl ? int(unity) : 60;
    out->fracSemitone = double(pitchFraction) / 4294967296.0;
    out->exactPitch = agbp;
    out->hasPitchMetadata = hasSmpl || agbp != 0;

    // The agbl override pins the playable length; a loop's exclusive end is
    // the sample end (FORMATS.md §1.2, CONTEXT.md §3.1).
    const qint64 n = out->frameCount();
    qint64 loopEndExcl = hasLoop ? qint64(loopEndIncl) + 1 : 0;
    if (agbl != 0 && qint64(agbl) <= n) {
        out->playLength = qint64(agbl);
        if (hasLoop)
            loopEndExcl = qint64(agbl);
    } else {
        out->playLength = n;
    }
    if (hasLoop) {
        loopEndExcl = qMin(loopEndExcl, n);
        if (qint64(loopStart) < loopEndExcl - 1) {
            out->hasLoop = true;
            out->loopStart = qint64(loopStart);
            out->loopEndIncl = loopEndExcl - 1;
        } else {
            out->warnings += QStringLiteral(
                "the smpl loop is empty or out of range — ignored.");
        }
    }
    if (agbp != 0)
        out->sampleRate = rateFromAgbPitch(
            agbp, double(out->baseKey) + out->fracSemitone);
    else
        out->sampleRate = declaredRate;
    return true;
}

// ---- AIFF (hi-res, cribbed from pory4a's load_aif_from_path semantics) ----

quint32 beU32(const quint8 *p)
{
    return quint32(p[0]) << 24 | quint32(p[1]) << 16 | quint32(p[2]) << 8
        | quint32(p[3]);
}

quint16 beU16(const quint8 *p)
{
    return quint16(quint16(p[0]) << 8 | p[1]);
}

// 80-bit IEEE extended float (COMM sample rate): sign(1) exponent(15)
// mantissa(64), value = mantissa · 2^(exponent − 16383 − 63).
double readExtended80(const quint8 *p)
{
    const int sign = (p[0] & 0x80) ? -1 : 1;
    const int exponent = int(quint16(quint16(p[0] & 0x7F) << 8 | p[1]));
    quint64 mantissa = 0;
    for (int i = 0; i < 8; i++)
        mantissa = mantissa << 8 | p[2 + i];
    if (exponent == 0 && mantissa == 0)
        return 0.0;
    return sign * std::ldexp(double(mantissa), exponent - 16383 - 63);
}

bool decodeAif(const QByteArray &bytes, bool leftOnly, ImportedSample *out,
               QString *error)
{
    const quint8 *data = reinterpret_cast<const quint8 *>(bytes.constData());
    const qint64 total = bytes.size();

    bool commFound = false, ssndFound = false;
    int channels = 0, sampleSize = 0;
    quint32 numFrames = 0;
    double sampleRate = 0.0;
    qint64 ssndDataStart = 0, ssndDataBytes = 0;
    int baseNote = 60, detuneCents = 0;
    bool instFound = false;
    bool haveSustainLoop = false;
    quint16 loopStartId = 0, loopEndId = 0;
    std::vector<std::pair<quint16, quint32>> markers;

    qint64 pos = 12;
    while (pos + 8 <= total) {
        const quint8 *hdr = data + pos;
        const qint64 chunkLen = beU32(hdr + 4);
        const qint64 at = pos + 8;
        if (at + chunkLen > total)
            break;
        if (std::memcmp(hdr, "COMM", 4) == 0 && chunkLen >= 18) {
            channels = beU16(data + at);
            numFrames = beU32(data + at + 2);
            sampleSize = beU16(data + at + 6);
            sampleRate = readExtended80(data + at + 8);
            commFound = true;
        } else if (std::memcmp(hdr, "MARK", 4) == 0 && chunkLen >= 2
                   && markers.empty()) {
            const quint16 numMarkers = beU16(data + at);
            qint64 mp = at + 2;
            for (quint16 i = 0; i < numMarkers && mp + 7 <= at + chunkLen;
                 i++) {
                const quint16 id = beU16(data + mp);
                const quint32 position = beU32(data + mp + 2);
                markers.push_back({id, position});
                // Pascal-style name, padded so the record length is even.
                const int nameSize = data[mp + 6];
                mp += 7 + nameSize + !(nameSize & 1);
            }
        } else if (std::memcmp(hdr, "INST", 4) == 0 && chunkLen >= 20) {
            baseNote = qBound(0, int(qint8(data[at])), 127);
            detuneCents = qBound(-50, int(qint8(data[at + 1])), 50);
            instFound = true;
            const int loopType = beU16(data + at + 8);
            if (loopType) {
                loopStartId = beU16(data + at + 10);
                loopEndId = beU16(data + at + 12);
                haveSustainLoop = true;
            }
        } else if (std::memcmp(hdr, "SSND", 4) == 0 && chunkLen >= 8) {
            const quint32 offset = beU32(data + at);
            ssndDataStart = at + 8 + offset;
            ssndDataBytes = chunkLen - 8 - offset;
            ssndFound = true;
        }
        pos = at + chunkLen + (chunkLen & 1);
    }
    if (!commFound || !ssndFound)
        return fail(error,
                    QStringLiteral("missing COMM or SSND chunk in the AIFF "
                                   "file."));
    if (channels < 1 || numFrames == 0)
        return fail(error, QStringLiteral("no audio data."));
    if (sampleSize != 8 && sampleSize != 16 && sampleSize != 24
        && sampleSize != 32)
        return fail(error, QStringLiteral("unsupported AIFF sample size "
                                          "(%1-bit).")
                               .arg(sampleSize));
    if (sampleRate <= 0.0)
        return fail(error,
                    QStringLiteral("the AIFF COMM sample rate is invalid."));

    const int bytesPerSample = sampleSize / 8;
    const qint64 availableFrames =
        ssndDataBytes / (qint64(bytesPerSample) * channels);
    const qint64 frames = qMin<qint64>(numFrames, availableFrames);
    if (frames <= 0)
        return fail(error, QStringLiteral("no audio data."));

    // AIFF samples are signed big-endian.
    std::vector<double> interleaved(size_t(frames) * size_t(channels));
    const quint8 *p = data + ssndDataStart;
    for (size_t i = 0; i < interleaved.size(); i++) {
        qint32 s = qint8(p[0]);
        for (int b = 1; b < bytesPerSample; b++)
            s = s << 8 | p[b];
        interleaved[i] = std::ldexp(double(s), -(sampleSize - 1));
        p += bytesPerSample;
    }

    downmix(interleaved, channels, leftOnly, out);
    out->sourceKind = ImportedSample::Aif;
    out->sourceChannels = channels;
    out->sourceBits = sampleSize;
    out->sampleRate = sampleRate;
    out->playLength = out->frameCount();

    // INST detune folds into the exact key; renormalize so frac ∈ [0, 1).
    const double exactKey = double(baseNote) + double(detuneCents) / 100.0;
    out->baseKey = int(std::floor(exactKey));
    out->fracSemitone = exactKey - std::floor(exactKey);
    out->hasPitchMetadata = instFound;

    // Resolve the sustain loop exactly as pory4a's loader does: the end
    // marker's position bounds the sample (exclusive), the smaller position
    // is the loop start (guards marker-order mistakes in hand-made files).
    if (haveSustainLoop) {
        bool haveStart = false;
        qint64 loopStart = 0, loopEndExcl = out->frameCount();
        for (const auto &m : markers) {
            if (m.first == loopStartId) {
                loopStart = m.second;
                haveStart = true;
                break;
            }
        }
        for (const auto &m : markers) {
            if (m.first == loopEndId) {
                if (qint64(m.second) < loopStart || !haveStart) {
                    loopStart = m.second;
                    haveStart = true;
                }
                loopEndExcl = qMin<qint64>(m.second, out->frameCount());
                break;
            }
        }
        if (haveStart && loopStart < loopEndExcl - 1) {
            out->hasLoop = true;
            out->loopStart = loopStart;
            out->loopEndIncl = loopEndExcl - 1;
        } else {
            out->warnings += QStringLiteral(
                "the AIFF sustain loop is empty or out of range — ignored.");
        }
    }
    return true;
}

} // namespace

bool importAudioBytes(const QByteArray &bytes, const QString &sourcePath,
                      ImportedSample *out, QString *error,
                      bool leftChannelOnly)
{
    *out = ImportedSample();
    out->sourcePath = sourcePath;
    out->suggestedName = SampleRegistrar::sanitizeSampleName(
        QFileInfo(sourcePath).completeBaseName());

    bool ok;
    if (bytes.size() >= 12 && bytes.startsWith("RIFF")
        && bytes.mid(8, 4) == "WAVE") {
        ok = decodeWav(bytes, leftChannelOnly, out, error);
    } else if (bytes.size() >= 12 && bytes.startsWith("FORM")
               && bytes.mid(8, 4) == "AIFF") {
        ok = decodeAif(bytes, leftChannelOnly, out, error);
    } else if (bytes.size() >= 12 && bytes.startsWith("FORM")
               && bytes.mid(8, 4) == "AIFC") {
        return fail(error, QStringLiteral("AIFF-C is not supported — export "
                                          "uncompressed AIFF or WAV."));
    } else {
        return fail(error,
                    QStringLiteral("not a supported audio file (WAV and AIFF "
                                   "sources are supported in this build)."));
    }
    if (ok)
        finishDiagnostics(out);
    return ok;
}

bool importAudioFile(const QString &path, ImportedSample *out, QString *error,
                     bool leftChannelOnly)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("cannot read %1.").arg(path));
    return importAudioBytes(file.readAll(), path, out, error, leftChannelOnly);
}
