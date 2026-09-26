#pragma once

#include <QByteArray>
#include <QHash>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

extern "C" {
#include "voicegroup_loader.h"
}

// The editable voice macros: the five basic families and their variants,
// plus voice_keysplit (whose sub-voicegroup/table pair is swappable) and
// voice_keysplit_all (drumkit; its sub-voicegroup is swappable). Cry voices
// stay read-only and round-trip verbatim.
enum class VgMacro {
    DirectSound,
    DirectSoundNoResample,
    DirectSoundAlt,
    // pokeemerald-expansion spellings: _reverse assembles the same type byte
    // as _alt; the _compressed pair are the cry types (0x20 / 0x30) with a
    // full argument list, for DPCM-compressed samples.
    DirectSoundReverse,
    DirectSoundCompressed,
    DirectSoundCompressedReverse,
    Square1,
    Square1Alt,
    Square2,
    Square2Alt,
    ProgWave,
    ProgWaveAlt,
    Noise,
    NoiseAlt,
    Keysplit,
    KeysplitAll,
};

QString vgMacroName(VgMacro macro);        // the .inc macro word
QString vgMacroDisplayName(VgMacro macro); // UI label
uint8_t vgMacroVoiceType(VgMacro macro);   // matching VOICE_* constant
bool vgMacroHasSymbol(VgMacro macro);      // DirectSound/ProgWave sample arg
bool vgMacroIsDirectSound(VgMacro macro);  // any voice_directsound* sample voice
bool vgMacroIsCompressed(VgMacro macro);   // plays a DPCM-compressed (cry) sample
bool vgMacroIsCgb(VgMacro macro);          // CGB ADSR ranges (A/D/R 0-7, S 0-15)

// One editable voice's parsed macro arguments, exactly as written in the file
// (unpacked: pan/duty/period/sweep are the raw macro args, not the ToneData
// encodings).
struct VgVoice {
    VgMacro macro = VgMacro::DirectSound;
    int key = 60;
    int pan = 0;
    QString symbol; // DirectSound sample / programmable-wave / keysplit or drumkit sub-voicegroup
    QString keysplitTable; // keysplit only
    int sweep = 0;         // square_1 only
    int duty = 2;          // square_1/2 only
    int period = 0;        // noise only
    int attack = 0;
    int decay = 0;
    int sustain = 0;
    int release = 0;

    bool operator==(const VgVoice &o) const
    {
        return macro == o.macro && key == o.key && pan == o.pan && symbol == o.symbol &&
               keysplitTable == o.keysplitTable && sweep == o.sweep && duty == o.duty &&
               period == o.period && attack == o.attack && decay == o.decay &&
               sustain == o.sustain && release == o.release;
    }
    bool operator!=(const VgVoice &o) const { return !(*this == o); }
};

// One ADSR envelope, in the raw macro-argument scale of its voice family
// (CGB: A/D/R 0-7, S 0-15; DirectSound: 0-255 each).
struct VgAdsr {
    int attack = 0;
    int decay = 0;
    int sustain = 0;
    int release = 0;

    bool operator==(const VgAdsr &o) const
    {
        return attack == o.attack && decay == o.decay && sustain == o.sustain &&
               release == o.release;
    }
    bool operator!=(const VgAdsr &o) const { return !(*this == o); }
};

// The envelope family a macro's ADSR values belong to: the _alt/no_resample
// variants collapse onto their base macro (identical envelope semantics).
// -1 for keysplit/drumkit voices, which carry no envelope of their own.
int vgAdsrFamily(VgMacro macro);

// One Golden Sun synth instrument (ipatix improved-mixer feature): a
// DirectSound voice whose "sample" has size 0 and whose data bytes select a
// synthesized waveform instead of PCM. Pulse carries a duty-cycle LFO;
// saw/triangle take no parameters (see parse_synth_macro_line and the
// M4A_SYNTH_* notes in external/poryaaaa/plugin).
struct VgSynthDesc {
    int waveform = 0;    // 0 = pulse, 1 = pseudo-saw, 2 = triangle
    int baseDuty = 0x80; // pulse only: duty threshold (0x80 = 50% square)
    int dutyStep = 0;    // pulse only: duty LFO advance per frame
    int modDepth = 0;    // pulse only: LFO swing around the base duty
    int phase = 0;       // pulse only: duty LFO phase offset

    bool operator==(const VgSynthDesc &o) const
    {
        if (waveform != o.waveform)
            return false;
        return waveform != 0 || (baseDuty == o.baseDuty && dutyStep == o.dutyStep &&
                                 modDepth == o.modDepth && phase == o.phase);
    }
    bool operator!=(const VgSynthDesc &o) const { return !(*this == o); }
};

// Waveform label ("Pulse", "Sawtooth", "Triangle").
QString vgSynthWaveformName(int waveform);

// The canonical param-named symbol for a descriptor
// ("DirectSoundSynth_GoldenSun_<params>" / "_Saw" / "_Triangle"), before any
// collision suffixing.
QString vgSynthSymbolName(const VgSynthDesc &desc);

// The project's Golden Sun synth instruments: every set_synth_* definition in
// the sound data files (file order), plus which set_synth_* assembler macros
// the project defines. An empty macro list means new definitions can't be
// written — they wouldn't assemble (the mixer support ships with the macros).
struct VgSynthCatalog {
    QList<QPair<QString, VgSynthDesc>> defs;
    QStringList macroWords;

    bool available() const { return !defs.isEmpty() || !macroWords.isEmpty(); }
    bool creatable() const { return !macroWords.isEmpty(); }
    const VgSynthDesc *find(const QString &symbol) const;
    QString symbolFor(const VgSynthDesc &desc) const; // "" when none matches
};

// The most common envelopes observed across a project's voicegroups, keyed
// by instrument symbol and by envelope family. Silent or clicking envelopes
// never qualify (see typicalAdsr), so a hit is always audible.
struct VgAdsrDefaults {
    QHash<QString, VgAdsr> bySymbol; // DirectSound sample / prog-wave symbol
    QHash<int, VgAdsr> byFamily;     // vgAdsrFamily() key
};

// One pass over the voicegroup files: all four project-wide datasets
// extracted from a single read of each file. Each member is value-identical
// to its single-dataset accessor (named in the comments).
struct VgCatalogScan {
    QStringList groupArgs;                    // SongRegistry::voicegroupArgs
    QList<QPair<QString, QString>> keysplits; // keysplitInstruments
    QStringList drumkits;                     // drumkitInstruments
    VgAdsrDefaults typicalAdsr;               // typicalAdsr
};

// One read of the sound data files: the sample symbol list and the synth
// catalog together (both parse the same files).
struct VgDirectSoundScan {
    QStringList directSound; // directSoundSymbols
    QStringList cries;       // the cries/ labels directSoundSymbols leaves out
    // The voice_directsound* and cry* macro words the project's asm/macros
    // define. A variant missing here wouldn't assemble, so it is never
    // offered (nor imported from a song bundle).
    QStringList voiceMacroWords;
    VgSynthCatalog synths; // synthInstruments
};

// The envelope a voice should adopt when it switches into a new envelope
// family, or picks a new instrument symbol while its envelope is untouched:
// the project-typical envelope for its instrument symbol, then for its
// family, then a full-sustain fallback with a short release tail (an
// instant release-0 cutoff clicks audibly).
VgAdsr vgDefaultAdsr(const VgAdsrDefaults &defaults, VgMacro macro, const QString &symbol);

// A voice edit is "structural" when audio can't be updated by poking scalar
// ToneData fields: the macro (voice type) or a sample/wave symbol changed, so
// the caller must reload the voicegroup from rendered source instead.
bool vgVoiceStructuralChange(const VgVoice &before, const VgVoice &after);

enum class VgLineKind {
    None,          // slot has no source line (past the file's last voice)
    Other,         // comment / label / directive — verbatim
    Header,        // voice_group NAME[, startingNote]
    Editable,      // one of the VgMacro macros, args parsed OK
    ReadOnlyVoice, // a cry-table macro (cry, cry_reverse, cry_uncomp, their
                   // _custom forms, ...) with well-formed arguments
    Broken,        // recognized macro prefix, but not plain integer / symbol
                   // args of the right count — verbatim
};

// One line of voicegroup source as parseSource() classifies it.
struct VgSourceLine {
    QByteArray raw; // no '\n', trailing '\r' kept
    VgLineKind kind = VgLineKind::Other;
    int slot = -1;     // voice lines only (Editable / ReadOnlyVoice / Broken)
    VgVoice voice;     // valid when kind == Editable
    QString crySymbol; // ReadOnlyVoice: the cry sample symbol
};

struct VgParsedSource {
    QVector<VgSourceLine> lines;
    int sectionBegin = 0; // line index of the label line (0 for per-file)
    int sectionEnd = 0;   // exclusive; lines.size() for per-file
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

    // Classifies standalone voicegroup bytes (a whole per-file voicegroup, a
    // renderPreview() slice, or — with sectionLabel — one section of a
    // monolithic file) with the same slot accounting open() uses. Lines
    // outside the section come back as Other. Read-only: for consumers that
    // walk the source text, like the song-bundle exporter.
    static VgParsedSource parseSource(const QByteArray &content,
                                      const QString &sectionLabel = QString());

    // Pushes the slot's scalar fields into a loaded ToneData using the C
    // loader's packing, for live audition without a reload. Returns false
    // when the voice needs a structural reload instead (type mismatch).
    bool applyScalarsToToneData(int slot, ToneData *td) const;

    // Sample/wave symbols declared in the project's sound data files.
    // DirectSound symbols exclude pokemon cries and synth definitions (which
    // get their own UI) and sort phonemes last.
    static QStringList directSoundSymbols(const QString &projectRoot);
    static QStringList progWaveSymbols(const QString &projectRoot);
    // Golden Sun synth instruments across the project's sound data files.
    static VgSynthCatalog synthInstruments(const QString &projectRoot);
    // Appends the given definitions to sound/direct_sound_synth_data.inc
    // (created if missing), using set_synth_* macros the project defines —
    // fails when it defines none. A symbol already on disk with an equal
    // descriptor is skipped; one with a different descriptor is an error.
    // Called at save time only: unsaved (pending) definitions live in memory.
    static bool writeSynthDefinitions(const QString &projectRoot,
                                      const QList<QPair<QString, VgSynthDesc>> &defs,
                                      QString *error);
    // Keysplit instruments observed across the project's voicegroups:
    // sub-voicegroup symbol -> its paired keysplit table symbol.
    static QList<QPair<QString, QString>> keysplitInstruments(const QString &projectRoot);
    // Drumkit sub-voicegroups observed across the project's voicegroups
    // (voice_keysplit_all targets), sorted.
    static QStringList drumkitInstruments(const QString &projectRoot);
    // Scans every voicegroup for the most common ADSR per symbol and per
    // family, skipping envelopes that click (release 0) or never sound
    // (DirectSound attack 0) — which also excludes the release-0 filler
    // squares that pad unused slots and would otherwise dominate the counts.
    static VgAdsrDefaults typicalAdsr(const QString &projectRoot);
    // Everything the voicegroup files feed the browser catalog, in one read
    // of each file (the single-dataset functions above re-run this scan).
    static VgCatalogScan catalogScan(const QString &projectRoot);
    // directSoundSymbols + synthInstruments from one read of the sound data
    // files instead of one each.
    static VgDirectSoundScan directSoundCatalog(const QString &projectRoot);

    // Writes sound/voicegroups/<name>.inc matching the siblings' header style
    // and line endings. copyFromFile/copySectionLabel name an existing
    // voicegroup to copy the voice lines from; empty means the 128-slot dummy
    // template. Requires the per-file layout (sound/voicegroups/ exists).
    static bool createVoicegroup(const QString &projectRoot, const QString &name,
                                 const QString &copyFromFile, const QString &copySectionLabel,
                                 QString *error);
    // The same writer over voice lines the caller already holds (no line
    // endings; the song-bundle importer's renamed copies). startingNote > 0
    // is the macro header's second argument — the first line is that key's
    // voice; a label-style project gets dummy voices for the keys below it.
    static bool createVoicegroupFromLines(const QString &projectRoot, const QString &name,
                                          const QList<QByteArray> &body, int startingNote,
                                          QString *error);
    // Appends .include "sound/voicegroups/<name>.inc" after the last .include
    // in sound/voice_groups.inc (byte-conservative; no-op if the hub file
    // doesn't exist — the loader and browser discover the file regardless).
    static bool appendIncludeLine(const QString &projectRoot, const QString &name, QString *error);
    // The inverse pair, for deleting a song's now-unused voicegroup: drops
    // the hub's .include line (no-op when absent), then the .inc file itself.
    // Idempotent — an already-deleted voicegroup is a success.
    static bool removeIncludeLine(const QString &projectRoot, const QString &name, QString *error);
    static bool deleteVoicegroup(const QString &projectRoot, const QString &name, QString *error);

  private:
    struct Line {
        QByteArray raw; // original bytes, no '\n', trailing '\r' kept
        VgLineKind kind = VgLineKind::Other;
        int slot = -1;
        VgVoice voice; // valid when kind == Editable
        // raw as of open/reload or the last save; a line is dirty while raw
        // differs, so an edit undone back to the on-disk value counts clean.
        QByteArray pristine;
        // Editable-line formatting, captured for faithful re-rendering:
        QByteArray indent;             // leading whitespace
        QByteArray macroText;          // macro word incl. any trailing space
        QVector<QByteArray> argPieces; // between-comma pieces, whitespace kept
        QByteArray tail;               // trailing whitespace + comment
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
