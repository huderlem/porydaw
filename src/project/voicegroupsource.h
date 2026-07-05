#pragma once

#include <QByteArray>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

extern "C" {
#include "voicegroup_loader.h"
}

// The editable voice macros: the five basic families and their variants,
// plus voice_keysplit (whose sub-voicegroup/table pair is swappable).
// Drumkit (keysplit_all) and cry voices stay read-only and round-trip
// verbatim.
enum class VgMacro {
    DirectSound,
    DirectSoundNoResample,
    DirectSoundAlt,
    Square1,
    Square1Alt,
    Square2,
    Square2Alt,
    ProgWave,
    ProgWaveAlt,
    Noise,
    NoiseAlt,
    Keysplit,
};

QString vgMacroName(VgMacro macro);        // the .inc macro word
QString vgMacroDisplayName(VgMacro macro); // UI label
uint8_t vgMacroVoiceType(VgMacro macro);   // matching VOICE_* constant
bool vgMacroHasSymbol(VgMacro macro);      // DirectSound/ProgWave sample arg
bool vgMacroIsCgb(VgMacro macro);          // CGB ADSR ranges (A/D/R 0-7, S 0-15)

// One editable voice's parsed macro arguments, exactly as written in the file
// (unpacked: pan/duty/period/sweep are the raw macro args, not the ToneData
// encodings).
struct VgVoice {
    VgMacro macro = VgMacro::DirectSound;
    int key = 60;
    int pan = 0;
    QString symbol; // DirectSound sample / programmable-wave / keysplit sub-voicegroup
    QString keysplitTable; // keysplit only
    int sweep = 0;  // square_1 only
    int duty = 2;   // square_1/2 only
    int period = 0; // noise only
    int attack = 0;
    int decay = 0;
    int sustain = 0;
    int release = 0;

    bool operator==(const VgVoice &o) const
    {
        return macro == o.macro && key == o.key && pan == o.pan && symbol == o.symbol
            && keysplitTable == o.keysplitTable && sweep == o.sweep && duty == o.duty
            && period == o.period && attack == o.attack && decay == o.decay
            && sustain == o.sustain && release == o.release;
    }
    bool operator!=(const VgVoice &o) const { return !(*this == o); }
};

// A voice edit is "structural" when audio can't be updated by poking scalar
// ToneData fields: the macro (voice type) or a sample/wave symbol changed, so
// the caller must reload the voicegroup from rendered source instead.
bool vgVoiceStructuralChange(const VgVoice &before, const VgVoice &after);

enum class VgLineKind {
    None,          // slot has no source line (past the file's last voice)
    Other,         // comment / label / directive — verbatim
    Header,        // voice_group NAME[, startingNote]
    Editable,      // one of the VgMacro macros, args parsed OK
    ReadOnlyVoice, // keysplit / keysplit_all / cry / cry_reverse
    Broken,        // recognized macro prefix but unparseable args — verbatim
};

// Source-of-truth model for one voicegroup: the .inc file's lines with the
// loader's slot accounting, byte-conservative editing of the editable voice
// lines, and re-rendering for save or for pre-save audition.
//
// The C loader (external/poryaaaa/plugin/voicegroup_loader.c) is read-only
// and lossy, so saving works from this text model, never from ToneData.
class VoicegroupSource
{
public:
    // Locates the file (or monolithic section) declaring "voicegroup<arg>"
    // and parses it. arg is the song's mid2agb -G value ("" means "_dummy").
    bool open(const QString &projectRoot, const QString &voicegroupArg, QString *error);
    // Re-reads the located file from disk, dropping all unsaved edits.
    bool reload(QString *error);

    QString filePath() const { return m_filePath; }
    // The name voicegroup_load() resolves this voicegroup with — also the
    // basename the pre-save preview file must use to shadow the real one.
    QString loadName() const { return m_loadName; }
    bool monolithic() const { return !m_sectionLabel.isEmpty(); }
    QString sectionLabel() const { return m_sectionLabel; }

    VgLineKind kindAt(int slot) const;
    bool isEditable(int slot) const { return kindAt(slot) == VgLineKind::Editable; }
    const VgVoice *voiceAt(int slot) const;
    // Rewrites the slot's line (preserving indentation, per-argument spacing,
    // and any trailing comment; a macro change falls back to canonical form).
    bool setVoice(int slot, const VgVoice &voice);
    bool dirty() const;

    // Writes the whole file back; only edited voice lines differ from the
    // bytes read at open/reload time.
    bool save(QString *error);

    // The edited source as a standalone parseable file: the whole buffer for
    // a per-file voicegroup, the section slice for a monolithic one.
    QByteArray renderPreview() const;

    // Pushes the slot's scalar fields into a loaded ToneData using the C
    // loader's packing, for live audition without a reload. Returns false
    // when the voice needs a structural reload instead (type mismatch).
    bool applyScalarsToToneData(int slot, ToneData *td) const;

    // Sample/wave symbols declared in the project's sound data files.
    // DirectSound symbols exclude pokemon cries and sort phonemes last.
    static QStringList directSoundSymbols(const QString &projectRoot);
    static QStringList progWaveSymbols(const QString &projectRoot);
    // Keysplit instruments observed across the project's voicegroups:
    // sub-voicegroup symbol -> its paired keysplit table symbol.
    static QList<QPair<QString, QString>> keysplitInstruments(const QString &projectRoot);

    // Writes sound/voicegroups/<name>.inc matching the siblings' header style
    // and line endings. copyFromFile/copySectionLabel name an existing
    // voicegroup to copy the voice lines from; empty means the 128-slot dummy
    // template. Requires the per-file layout (sound/voicegroups/ exists).
    static bool createVoicegroup(const QString &projectRoot, const QString &name,
                                 const QString &copyFromFile,
                                 const QString &copySectionLabel, QString *error);
    // Appends .include "sound/voicegroups/<name>.inc" after the last .include
    // in sound/voice_groups.inc (byte-conservative; no-op if the hub file
    // doesn't exist — the loader and browser discover the file regardless).
    static bool appendIncludeLine(const QString &projectRoot, const QString &name,
                                  QString *error);

private:
    struct Line {
        QByteArray raw; // original bytes, no '\n', trailing '\r' kept
        VgLineKind kind = VgLineKind::Other;
        int slot = -1;
        VgVoice voice;            // valid when kind == Editable
        bool dirty = false;
        // Editable-line formatting, captured for faithful re-rendering:
        QByteArray indent;        // leading whitespace
        QByteArray macroText;     // macro word incl. any trailing space
        QVector<QByteArray> argPieces; // between-comma pieces, whitespace kept
        QByteArray tail;          // trailing whitespace + comment
    };

    bool parse(const QByteArray &content, QString *error);
    void renderLine(Line &line) const;

    QString m_projectRoot;
    QString m_arg;
    QString m_filePath;
    QString m_sectionLabel; // empty = per-file layout
    QString m_loadName;
    QVector<Line> m_lines;
    int m_slotToLine[VOICEGROUP_SIZE];
    int m_sectionBegin = 0; // line index of the label line (0 for per-file)
    int m_sectionEnd = 0;   // exclusive
    bool m_endsWithNewline = true;
};
