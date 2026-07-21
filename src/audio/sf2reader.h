#pragma once

#include <QByteArray>
#include <QString>

#include <vector>

struct ImportedSample;

// Minimal in-house SoundFont zone extractor (docs/sample-studio/PLAN.md §2
// Sf2Reader, subset spec FORMATS.md §5). Reads only the sdta/smpl 16-bit PCM
// pool and the pdta shdr records, plus phdr/inst/ibag/igen (and the
// pbag/pgen index arrays that link presets to instruments) purely for the
// picker's grouping labels. Not a synth: generator/modulator semantics,
// velocity layers, and per-zone tuning overrides are deliberately ignored.

// One presentable shdr record. Positions are sample-pool frame indices; the
// loop end keeps the sf2 exclusive convention until zone extraction converts
// it (FORMATS.md §5).
struct Sf2Zone {
    QString name;            // shdr achSampleName, NUL-trimmed
    quint32 start = 0;       // inclusive pool frame
    quint32 end = 0;         // exclusive pool frame
    quint32 loopStart = 0;   // pool frame
    quint32 loopEndExcl = 0; // pool frame, sf2 exclusive convention
    quint32 sampleRate = 0;
    int originalPitch = 60;  // byOriginalPitch; > 127 = unpitched convention
    int pitchCorrection = 0; // chPitchCorrection, signed cents
    quint16 sampleType = 1;
    QString instrument;      // grouping label; empty = unreferenced
    QString preset;          // grouping label; empty = none

    qint64 frames() const { return qint64(end) - qint64(start); }
    bool stereoPair() const { return sampleType == 2 || sampleType == 4; }
    bool hasLoop() const
    {
        return loopStart >= start && loopEndExcl <= end
            && loopEndExcl >= loopStart + 2;
    }
};

// A parsed SoundFont: the raw 16-bit LE pool plus the presentable zones (ROM
// samples and the EOS terminator already skipped).
struct Sf2File {
    QByteArray pool; // sdta smpl bytes, 16-bit LE PCM
    std::vector<Sf2Zone> zones;
    QString sourcePath;
};

// RIFF form "sfbk" magic sniff (dispatch is on magic, never extension).
bool sf2Magic(const QByteArray &bytes);

// Parse a SoundFont byte stream. Refuses actionably on corrupt/truncated
// containers, on missing smpl/shdr chunks, and when no importable zone
// remains. sourcePath only labels the result.
bool readSf2Bytes(const QByteArray &bytes, const QString &sourcePath,
                  Sf2File *out, QString *error);
bool readSf2File(const QString &path, Sf2File *out, QString *error);

// Selected zone → hi-res ImportedSample (FORMATS.md §5 mapping): the zone's
// pool segment on the canonical float scale, loop converted exclusive →
// inclusive, unity = originalPitch with pitchCorrection folded in and the
// fraction renormalized to [0, 1) by borrowing from the key. Left/right
// stereo-linked zones import this channel and carry the pair warning.
bool extractSf2Zone(const Sf2File &file, int zoneIndex, ImportedSample *out,
                    QString *error);
