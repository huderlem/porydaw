#include "samplewav.h"

namespace {

void putU16(QByteArray *out, quint16 v)
{
    out->append(char(v)).append(char(v >> 8));
}

void putU32(QByteArray *out, quint32 v)
{
    out->append(char(v)).append(char(v >> 8)).append(char(v >> 16)).append(
        char(v >> 24));
}

} // namespace

QByteArray writeSampleWav(const ProcessedSample &sample)
{
    const quint32 n = sample.size;
    QByteArray wav("RIFF\0\0\0\0WAVE", 12);

    wav += "fmt ";
    putU32(&wav, 16);
    putU16(&wav, 1); // PCM
    putU16(&wav, 1); // mono
    putU32(&wav, sample.declaredRate); // the exact rate lives in agbp
    putU32(&wav, sample.declaredRate); // byte rate (1 byte per frame)
    putU16(&wav, 1);                   // block align
    putU16(&wav, 8);

    wav += "data";
    putU32(&wav, n);
    for (quint32 i = 0; i < n; i++)
        wav += char(quint8(int(qint8(sample.s8[int(i)])) + 128));
    if (n & 1)
        wav += '\0'; // RIFF pad byte

    // smpl: unity note + standard-semantics pitch fraction + exactly one
    // forward loop record when looped (inclusive end n − 1), else zero loops.
    const int numLoops = sample.looped ? 1 : 0;
    wav += "smpl";
    putU32(&wav, quint32(36 + 24 * numLoops));
    putU32(&wav, 0); // manufacturer
    putU32(&wav, 0); // product
    putU32(&wav, 0); // sample period
    putU32(&wav, quint32(sample.unityNote));
    putU32(&wav, sample.pitchFraction);
    putU32(&wav, 0); // SMPTE format
    putU32(&wav, 0); // SMPTE offset
    putU32(&wav, quint32(numLoops));
    putU32(&wav, 0); // sampler data
    if (sample.looped) {
        putU32(&wav, 0); // cue point id
        putU32(&wav, 0); // type: forward
        putU32(&wav, sample.loopStart);
        putU32(&wav, n - 1); // inclusive (wav2agb adds +1 on read)
        putU32(&wav, 0);     // fraction
        putU32(&wav, 0);     // play count
    }

    // agbp: the exact GBA pitch word — authoritative, sidesteps the
    // dwMIDIPitchFraction quirk (FORMATS.md §3.1).
    wav += "agbp";
    putU32(&wav, 4);
    putU32(&wav, sample.freq);

    // agbl = n for fresh exports: every exported sample plays. (The shipped
    // corpus's n − 1 is a ROM-round-trip artifact — never imitate it.)
    wav += "agbl";
    putU32(&wav, 4);
    putU32(&wav, n);

    const quint32 riffSize = quint32(wav.size()) - 8;
    wav[4] = char(riffSize);
    wav[5] = char(riffSize >> 8);
    wav[6] = char(riffSize >> 16);
    wav[7] = char(riffSize >> 24);
    return wav;
}
