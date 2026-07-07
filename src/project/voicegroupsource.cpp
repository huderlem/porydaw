#include "voicegroupsource.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <algorithm>
#include <cctype>

// Line classification and slot accounting mirror parse_voicegroup_file in
// external/poryaaaa/plugin/voicegroup_loader.c (:1671-2058) exactly:
//  - every recognized voice-macro line advances the slot, even when its
//    arguments fail to parse (the increment sits outside the sscanf check);
//  - "voice_group NAME, n" sets the slot only with the comma form and
//    0 < n < 128 (:1727); the bare header leaves it at 0;
//  - prefix order matters (_no_resample before _alt before base, keysplit_all
//    before keysplit, cry_reverse before cry), and the two programmable_wave
//    prefixes match without requiring a trailing space (:1877/:1903);
//  - comments are stripped at the first '@' or "//" (strip_comment, :179);
//  - a monolithic section starts at "<label>::" and, once a voice has been
//    parsed, ends at the first line with "::" past position 0 or starting
//    with ".align" (:1706-1724).

namespace {

struct MacroDef {
    VgMacro macro;
    const char *word;
    bool requireSpace; // loader's strncmp length includes a trailing space
};

// In the loader's dispatch order.
const MacroDef kEditableMacros[] = {
    {VgMacro::DirectSoundNoResample, "voice_directsound_no_resample", true},
    {VgMacro::DirectSoundAlt, "voice_directsound_alt", true},
    {VgMacro::DirectSound, "voice_directsound", true},
    {VgMacro::Square1Alt, "voice_square_1_alt", true},
    {VgMacro::Square1, "voice_square_1", true},
    {VgMacro::Square2Alt, "voice_square_2_alt", true},
    {VgMacro::Square2, "voice_square_2", true},
    {VgMacro::ProgWaveAlt, "voice_programmable_wave_alt", false},
    {VgMacro::ProgWave, "voice_programmable_wave", false},
    {VgMacro::NoiseAlt, "voice_noise_alt", true},
    {VgMacro::Noise, "voice_noise", true},
    // keysplit_all before keysplit, matching the loader's dispatch order.
    {VgMacro::KeysplitAll, "voice_keysplit_all", true},
    {VgMacro::Keysplit, "voice_keysplit", true},
};

const char *const kReadOnlyPrefixes[] = {
    "cry_reverse ",
    "cry ",
};

int macroArgCount(VgMacro macro)
{
    if (macro == VgMacro::KeysplitAll)
        return 1;
    if (macro == VgMacro::Keysplit)
        return 2;
    return macro == VgMacro::Square1 || macro == VgMacro::Square1Alt ? 8 : 7;
}

// Whether argument index a of the macro is a symbol (vs. an integer).
bool macroArgIsSymbol(VgMacro macro, int a)
{
    if (macro == VgMacro::Keysplit || macro == VgMacro::KeysplitAll)
        return true; // sub-voicegroup (and keysplit table)
    return vgMacroHasSymbol(macro) && a == 2;
}

bool isIntArg(const QByteArray &trimmed)
{
    if (trimmed.isEmpty())
        return false;
    int i = (trimmed[0] == '-' || trimmed[0] == '+') ? 1 : 0;
    if (i == trimmed.size())
        return false;
    for (; i < trimmed.size(); i++) {
        if (trimmed[i] < '0' || trimmed[i] > '9')
            return false;
    }
    return true;
}

// Mirrors the loader's strip_comment + rtrim + ltrim: fills the content
// region [contentStart, contentEnd) of a raw line (which keeps its '\r').
void contentBounds(const QByteArray &raw, int *contentStart, int *contentEnd)
{
    int end = raw.size();
    const int at = raw.indexOf('@');
    if (at >= 0)
        end = at;
    const int slashes = raw.indexOf("//");
    if (slashes >= 0 && slashes < end)
        end = slashes;
    while (end > 0 && isspace(uchar(raw[end - 1])))
        end--;
    int start = 0;
    while (start < end && isspace(uchar(raw[start])))
        start++;
    *contentStart = start;
    *contentEnd = end;
}

QByteArray leadingWs(const QByteArray &piece)
{
    int i = 0;
    while (i < piece.size() && isspace(uchar(piece[i])))
        i++;
    return piece.left(i);
}

QByteArray trailingWs(const QByteArray &piece)
{
    int i = piece.size();
    while (i > 0 && isspace(uchar(piece[i - 1])))
        i--;
    return piece.mid(i);
}

// Collects "Label::" symbols; a label whose .incbin points into a cries
// directory is a pokemon cry, not an instrument sample, and is skipped.
QStringList symbolsFromFiles(const QStringList &paths)
{
    static const QRegularExpression labelRe(QStringLiteral(R"(^(\w+)::)"));
    static const QRegularExpression incbinRe(
        QStringLiteral(R"(^\s*\.incbin\s+"([^"]+)\")"));
    QStringList symbols;
    for (const QString &path : paths) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        QString pending;
        while (!file.atEnd()) {
            const QString line = QString::fromUtf8(file.readLine());
            const QRegularExpressionMatch label = labelRe.match(line);
            if (label.hasMatch()) {
                if (!pending.isEmpty())
                    symbols.append(pending); // no .incbin followed (synth def)
                pending = label.captured(1);
                continue;
            }
            const QRegularExpressionMatch incbin = incbinRe.match(line);
            if (incbin.hasMatch() && !pending.isEmpty()) {
                if (!incbin.captured(1).contains(QStringLiteral("cries/")))
                    symbols.append(pending);
                pending.clear();
            }
        }
        if (!pending.isEmpty())
            symbols.append(pending);
    }
    symbols.removeDuplicates();
    std::sort(symbols.begin(), symbols.end());
    return symbols;
}

QByteArray readAllBytes(const QString &path, bool *ok = nullptr)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (ok)
            *ok = false;
        return QByteArray();
    }
    if (ok)
        *ok = true;
    return file.readAll();
}

// Splits file bytes into lines without '\n' (keeping '\r'), recording whether
// the file ended with a newline — the writeMidiCfgLine recipe.
QList<QByteArray> splitLines(const QByteArray &content, bool *endsWithNewline)
{
    *endsWithNewline = content.isEmpty() || content.endsWith('\n');
    QList<QByteArray> lines = content.split('\n');
    if (*endsWithNewline && !lines.isEmpty())
        lines.removeLast();
    return lines;
}

// The voicegroup symbols a file declares, in file order, with the line forms.
// Macro "voice_group X" declares voicegroup_X; a label declares itself
// (same regexes as SongRegistry::voicegroupArgs).
struct DeclaredSymbol {
    QString symbol;
    bool labelForm;
};

QVector<DeclaredSymbol> declaredVoicegroups(const QString &path)
{
    static const QRegularExpression macroRe(QStringLiteral(R"(^\s*voice_group\s+(\w+))"));
    static const QRegularExpression labelRe(QStringLiteral(R"(^\s*(voicegroup\w+)::)"));
    QVector<DeclaredSymbol> symbols;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return symbols;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine());
        QRegularExpressionMatch m = macroRe.match(line);
        if (m.hasMatch()) {
            symbols.append({QStringLiteral("voicegroup_") + m.captured(1), false});
            continue;
        }
        m = labelRe.match(line);
        if (m.hasMatch())
            symbols.append({m.captured(1), true});
    }
    return symbols;
}

QStringList voicegroupFiles(const QString &projectRoot)
{
    QStringList files;
    for (const char *single : {"/sound/voice_groups.inc", "/sound/voicegroups.inc"}) {
        const QString path = projectRoot + QLatin1String(single);
        if (QFile::exists(path))
            files.append(path);
    }
    QDirIterator it(projectRoot + QStringLiteral("/sound/voicegroups"),
                    {QStringLiteral("*.inc")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        files.append(it.next());
    return files;
}

} // namespace

QString vgMacroName(VgMacro macro)
{
    for (const MacroDef &def : kEditableMacros) {
        if (def.macro == macro)
            return QLatin1String(def.word);
    }
    return QString();
}

QString vgMacroDisplayName(VgMacro macro)
{
    switch (macro) {
    case VgMacro::DirectSound: return QStringLiteral("Sample");
    case VgMacro::DirectSoundNoResample: return QStringLiteral("Sample (no resample)");
    case VgMacro::DirectSoundAlt: return QStringLiteral("Sample (fixed pitch)");
    case VgMacro::Square1: return QStringLiteral("Square 1");
    case VgMacro::Square1Alt: return QStringLiteral("Square 1 (alt)");
    case VgMacro::Square2: return QStringLiteral("Square 2");
    case VgMacro::Square2Alt: return QStringLiteral("Square 2 (alt)");
    case VgMacro::ProgWave: return QStringLiteral("Wave");
    case VgMacro::ProgWaveAlt: return QStringLiteral("Wave (alt)");
    case VgMacro::Noise: return QStringLiteral("Noise");
    case VgMacro::NoiseAlt: return QStringLiteral("Noise (alt)");
    case VgMacro::Keysplit: return QStringLiteral("Keysplit");
    case VgMacro::KeysplitAll: return QStringLiteral("Drumkit");
    }
    return QString();
}

uint8_t vgMacroVoiceType(VgMacro macro)
{
    switch (macro) {
    case VgMacro::DirectSound: return VOICE_DIRECTSOUND;
    case VgMacro::DirectSoundNoResample: return VOICE_DIRECTSOUND_NO_RESAMPLE;
    case VgMacro::DirectSoundAlt: return VOICE_DIRECTSOUND_ALT;
    case VgMacro::Square1: return VOICE_SQUARE_1;
    case VgMacro::Square1Alt: return VOICE_SQUARE_1_ALT;
    case VgMacro::Square2: return VOICE_SQUARE_2;
    case VgMacro::Square2Alt: return VOICE_SQUARE_2_ALT;
    case VgMacro::ProgWave: return VOICE_PROGRAMMABLE_WAVE;
    case VgMacro::ProgWaveAlt: return VOICE_PROGRAMMABLE_WAVE_ALT;
    case VgMacro::Noise: return VOICE_NOISE;
    case VgMacro::NoiseAlt: return VOICE_NOISE_ALT;
    case VgMacro::Keysplit: return VOICE_KEYSPLIT;
    case VgMacro::KeysplitAll: return VOICE_KEYSPLIT_ALL;
    }
    return VOICE_DIRECTSOUND;
}

bool vgMacroHasSymbol(VgMacro macro)
{
    switch (macro) {
    case VgMacro::DirectSound:
    case VgMacro::DirectSoundNoResample:
    case VgMacro::DirectSoundAlt:
    case VgMacro::ProgWave:
    case VgMacro::ProgWaveAlt:
    case VgMacro::Keysplit:
    case VgMacro::KeysplitAll:
        return true;
    default:
        return false;
    }
}

bool vgMacroIsCgb(VgMacro macro)
{
    switch (macro) {
    case VgMacro::DirectSound:
    case VgMacro::DirectSoundNoResample:
    case VgMacro::DirectSoundAlt:
    case VgMacro::Keysplit:
    case VgMacro::KeysplitAll:
        return false;
    default:
        return true;
    }
}

int vgAdsrFamily(VgMacro macro)
{
    switch (macro) {
    case VgMacro::DirectSound:
    case VgMacro::DirectSoundNoResample:
    case VgMacro::DirectSoundAlt:
        return int(VgMacro::DirectSound);
    case VgMacro::Square1:
    case VgMacro::Square1Alt:
        return int(VgMacro::Square1);
    case VgMacro::Square2:
    case VgMacro::Square2Alt:
        return int(VgMacro::Square2);
    case VgMacro::ProgWave:
    case VgMacro::ProgWaveAlt:
        return int(VgMacro::ProgWave);
    case VgMacro::Noise:
    case VgMacro::NoiseAlt:
        return int(VgMacro::Noise);
    case VgMacro::Keysplit:
    case VgMacro::KeysplitAll:
        return -1;
    }
    return -1;
}

VgAdsr vgDefaultAdsr(const VgAdsrDefaults &defaults, VgMacro macro,
                     const QString &symbol)
{
    if (!symbol.isEmpty() && defaults.bySymbol.contains(symbol))
        return defaults.bySymbol.value(symbol);
    const int family = vgAdsrFamily(macro);
    if (defaults.byFamily.contains(family))
        return defaults.byFamily.value(family);
    return vgMacroIsCgb(macro) ? VgAdsr{0, 0, 15, 3} : VgAdsr{255, 0, 255, 165};
}

bool vgVoiceStructuralChange(const VgVoice &before, const VgVoice &after)
{
    return before.macro != after.macro || before.symbol != after.symbol
        || before.keysplitTable != after.keysplitTable;
}

bool VoicegroupSource::open(const QString &projectRoot, const QString &voicegroupArg,
                            QString *error)
{
    m_projectRoot = projectRoot;
    // mid2agb's default -G argument is "_dummy".
    m_arg = voicegroupArg.isEmpty() ? QStringLiteral("_dummy") : voicegroupArg;
    const QString symbol = QStringLiteral("voicegroup") + m_arg;

    m_filePath.clear();
    m_sectionLabel.clear();
    m_loadName.clear();

    for (const QString &path : voicegroupFiles(projectRoot)) {
        const QVector<DeclaredSymbol> declared = declaredVoicegroups(path);
        for (const DeclaredSymbol &decl : declared) {
            if (decl.symbol != symbol)
                continue;
            if (declared.size() > 1) {
                // Monolithic file: the loader locates the section by its
                // "<label>::" line, so only the label form is editable.
                if (!decl.labelForm) {
                    if (error)
                        *error = QStringLiteral(
                            "%1 declares %2 with the voice_group macro inside a "
                            "multi-voicegroup file — not an editable layout.")
                                     .arg(path, symbol);
                    return false;
                }
                m_sectionLabel = decl.symbol;
                m_loadName = decl.symbol;
            } else {
                m_loadName = QFileInfo(path).completeBaseName();
            }
            m_filePath = path;
            break;
        }
        if (!m_filePath.isEmpty())
            break;
    }
    if (m_filePath.isEmpty()) {
        if (error)
            *error = QStringLiteral("No voicegroup file declares %1.").arg(symbol);
        return false;
    }
    return reload(error);
}

bool VoicegroupSource::reload(QString *error)
{
    bool ok = false;
    const QByteArray content = readAllBytes(m_filePath, &ok);
    if (!ok) {
        if (error)
            *error = QStringLiteral("Cannot read %1").arg(m_filePath);
        return false;
    }
    return parse(content, error);
}

bool VoicegroupSource::parse(const QByteArray &content, QString *error)
{
    m_lines.clear();
    std::fill(std::begin(m_slotToLine), std::end(m_slotToLine), -1);

    const QList<QByteArray> rawLines = splitLines(content, &m_endsWithNewline);
    m_lines.reserve(rawLines.size());
    m_sectionBegin = m_sectionLabel.isEmpty() ? 0 : -1;
    m_sectionEnd = rawLines.size();

    const QByteArray sectionStart = m_sectionLabel.toUtf8() + "::";
    bool inSection = m_sectionLabel.isEmpty();
    bool sectionDone = false;
    int voicesInSection = 0;
    int nextSlot = 0;

    for (int i = 0; i < rawLines.size(); i++) {
        Line line;
        line.raw = rawLines.at(i);

        int contentStart = 0, contentEnd = 0;
        contentBounds(line.raw, &contentStart, &contentEnd);
        const QByteArray text = line.raw.mid(contentStart, contentEnd - contentStart);

        if (sectionDone || text.isEmpty()) {
            m_lines.append(line);
            continue;
        }
        if (!inSection) {
            if (text.startsWith(sectionStart)) {
                inSection = true;
                m_sectionBegin = i;
            }
            m_lines.append(line);
            continue;
        }
        if (!m_sectionLabel.isEmpty() && voicesInSection > 0) {
            const int labelIdx = text.indexOf("::");
            if (labelIdx > 0 || text.startsWith(".align")) {
                sectionDone = true;
                m_sectionEnd = i;
                m_lines.append(line);
                continue;
            }
        }
        if (nextSlot >= VOICEGROUP_SIZE) {
            // The loader stops reading once all 128 slots are filled.
            m_lines.append(line);
            continue;
        }

        if (text.startsWith("voice_group ")) {
            line.kind = VgLineKind::Header;
            const QByteArray rest = text.mid(12);
            const int comma = rest.indexOf(',');
            if (comma >= 0) {
                const QByteArray numText = rest.mid(comma + 1).trimmed();
                int digits = (numText.startsWith('-') || numText.startsWith('+')) ? 1 : 0;
                while (digits < numText.size() && numText[digits] >= '0'
                       && numText[digits] <= '9')
                    digits++;
                bool numOk = false;
                const int startingNote = numText.left(digits).toInt(&numOk);
                if (numOk && startingNote > 0 && startingNote < VOICEGROUP_SIZE)
                    nextSlot = startingNote;
            }
            m_lines.append(line);
            continue;
        }

        const MacroDef *matched = nullptr;
        for (const MacroDef &def : kEditableMacros) {
            const int wordLen = int(qstrlen(def.word));
            if (def.requireSpace
                    ? (text.startsWith(def.word) && text.size() > wordLen
                       && text[wordLen] == ' ')
                    : text.startsWith(def.word)) {
                matched = &def;
                break;
            }
        }
        bool isReadOnlyVoice = false;
        if (!matched) {
            for (const char *prefix : kReadOnlyPrefixes) {
                if (text.startsWith(prefix)) {
                    isReadOnlyVoice = true;
                    break;
                }
            }
        }
        if (!matched && !isReadOnlyVoice) {
            m_lines.append(line);
            continue;
        }

        line.slot = nextSlot++;
        voicesInSection++;
        if (isReadOnlyVoice) {
            line.kind = VgLineKind::ReadOnlyVoice;
        } else {
            // Tokenize the arguments, keeping every byte for re-rendering.
            const int prefixLen = int(qstrlen(matched->word)) + (matched->requireSpace ? 1 : 0);
            line.indent = line.raw.left(contentStart);
            line.macroText = text.left(prefixLen);
            line.tail = line.raw.mid(contentEnd);
            line.argPieces = text.mid(prefixLen).split(',');

            line.kind = VgLineKind::Broken;
            const int expected = macroArgCount(matched->macro);
            if (line.argPieces.size() == expected) {
                bool valid = true;
                QVector<QByteArray> values(expected);
                for (int a = 0; a < expected; a++) {
                    values[a] = line.argPieces.at(a).trimmed();
                    valid = valid
                        && (macroArgIsSymbol(matched->macro, a) ? !values[a].isEmpty()
                                                                : isIntArg(values[a]));
                }
                if (valid) {
                    line.kind = VgLineKind::Editable;
                    VgVoice &v = line.voice;
                    v.macro = matched->macro;
                    if (v.macro == VgMacro::KeysplitAll) {
                        v.symbol = QString::fromUtf8(values[0]);
                    } else if (v.macro == VgMacro::Keysplit) {
                        v.symbol = QString::fromUtf8(values[0]);
                        v.keysplitTable = QString::fromUtf8(values[1]);
                    } else {
                        v.key = values[0].toInt();
                        v.pan = values[1].toInt();
                        int a = 2;
                        if (vgMacroHasSymbol(v.macro))
                            v.symbol = QString::fromUtf8(values[a++]);
                        else if (v.macro == VgMacro::Square1
                                 || v.macro == VgMacro::Square1Alt) {
                            v.sweep = values[a++].toInt();
                            v.duty = values[a++].toInt();
                        } else if (v.macro == VgMacro::Square2
                                   || v.macro == VgMacro::Square2Alt)
                            v.duty = values[a++].toInt();
                        else
                            v.period = values[a++].toInt();
                        v.attack = values[a++].toInt();
                        v.decay = values[a++].toInt();
                        v.sustain = values[a++].toInt();
                        v.release = values[a++].toInt();
                    }
                }
            }
        }
        if (line.slot >= 0 && line.slot < VOICEGROUP_SIZE)
            m_slotToLine[line.slot] = i;
        m_lines.append(line);
    }

    if (!m_sectionLabel.isEmpty() && m_sectionBegin < 0) {
        if (error)
            *error = QStringLiteral("Label %1:: not found in %2")
                         .arg(m_sectionLabel, m_filePath);
        return false;
    }
    for (Line &line : m_lines)
        line.pristine = line.raw;
    Q_UNUSED(error);
    return true;
}

VgLineKind VoicegroupSource::kindAt(int slot) const
{
    if (slot < 0 || slot >= VOICEGROUP_SIZE || m_slotToLine[slot] < 0)
        return VgLineKind::None;
    return m_lines.at(m_slotToLine[slot]).kind;
}

const VgVoice *VoicegroupSource::voiceAt(int slot) const
{
    if (kindAt(slot) != VgLineKind::Editable)
        return nullptr;
    return &m_lines.at(m_slotToLine[slot]).voice;
}

bool VoicegroupSource::setVoice(int slot, const VgVoice &voice)
{
    if (kindAt(slot) != VgLineKind::Editable)
        return false;
    Line &line = m_lines[m_slotToLine[slot]];
    line.voice = voice;
    renderLine(line);
    return true;
}

void VoicegroupSource::renderLine(Line &line) const
{
    const VgVoice &v = line.voice;
    QVector<QByteArray> values;
    if (v.macro == VgMacro::KeysplitAll) {
        values << v.symbol.toUtf8();
    } else if (v.macro == VgMacro::Keysplit) {
        values << v.symbol.toUtf8() << v.keysplitTable.toUtf8();
    } else {
        values << QByteArray::number(v.key) << QByteArray::number(v.pan);
        if (vgMacroHasSymbol(v.macro))
            values << v.symbol.toUtf8();
        else if (v.macro == VgMacro::Square1 || v.macro == VgMacro::Square1Alt)
            values << QByteArray::number(v.sweep) << QByteArray::number(v.duty);
        else if (v.macro == VgMacro::Square2 || v.macro == VgMacro::Square2Alt)
            values << QByteArray::number(v.duty);
        else
            values << QByteArray::number(v.period);
        values << QByteArray::number(v.attack) << QByteArray::number(v.decay)
               << QByteArray::number(v.sustain) << QByteArray::number(v.release);
    }

    const QByteArray macroWord = vgMacroName(v.macro).toUtf8();
    const bool sameMacro = line.macroText.trimmed() == macroWord
        && line.argPieces.size() == values.size();
    if (sameMacro) {
        // Substitute values inside the original pieces, keeping each one's
        // surrounding whitespace.
        for (int i = 0; i < values.size(); i++) {
            const QByteArray &piece = line.argPieces.at(i);
            line.argPieces[i] = leadingWs(piece) + values.at(i) + trailingWs(piece);
        }
    } else {
        // Macro changed: canonical spacing, preserving indent and comment.
        line.macroText = macroWord + ' ';
        line.argPieces.clear();
        for (int i = 0; i < values.size(); i++)
            line.argPieces.append(i == 0 ? values.at(i) : ' ' + values.at(i));
    }

    QByteArray args;
    for (int i = 0; i < line.argPieces.size(); i++) {
        if (i > 0)
            args += ',';
        args += line.argPieces.at(i);
    }
    line.raw = line.indent + line.macroText + args + line.tail;
}

bool VoicegroupSource::dirty() const
{
    for (const Line &line : m_lines) {
        if (line.raw != line.pristine)
            return true;
    }
    return false;
}

bool VoicegroupSource::save(QString *error)
{
    QFile out(m_filePath);
    if (!out.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("Cannot write %1").arg(m_filePath);
        return false;
    }
    QByteArray joined;
    for (int i = 0; i < m_lines.size(); i++) {
        if (i > 0)
            joined += '\n';
        joined += m_lines.at(i).raw;
    }
    if (m_endsWithNewline && !m_lines.isEmpty())
        joined += '\n';
    if (out.write(joined) != joined.size()) {
        if (error)
            *error = QStringLiteral("Short write to %1").arg(m_filePath);
        return false;
    }
    out.close();
    for (Line &line : m_lines)
        line.pristine = line.raw;
    return true;
}

QByteArray VoicegroupSource::renderPreview() const
{
    const int begin = m_sectionLabel.isEmpty() ? 0 : m_sectionBegin;
    const int end = m_sectionLabel.isEmpty() ? m_lines.size() : m_sectionEnd;
    QByteArray joined;
    for (int i = begin; i < end; i++) {
        joined += m_lines.at(i).raw;
        joined += '\n';
    }
    return joined;
}

bool VoicegroupSource::applyScalarsToToneData(int slot, ToneData *td) const
{
    const VgVoice *v = voiceAt(slot);
    if (!v || !td || td->type != vgMacroVoiceType(v->macro))
        return false;
    if (v->macro == VgMacro::Keysplit || v->macro == VgMacro::KeysplitAll)
        return true; // no scalar fields; any change is structural
    // Field packing per parse_voicegroup_file (voicegroup_loader.c:1738+).
    td->key = uint8_t(v->key);
    if (!vgMacroIsCgb(v->macro)) {
        td->panSweep = v->pan ? uint8_t(0x80 | v->pan) : 0;
        td->attack = uint8_t(v->attack);
        td->decay = uint8_t(v->decay);
        td->sustain = uint8_t(v->sustain);
        td->release = uint8_t(v->release);
        return true;
    }
    td->attack = uint8_t(v->attack & 0x07);
    td->decay = uint8_t(v->decay & 0x07);
    td->sustain = uint8_t(v->sustain & 0x0F);
    td->release = uint8_t(v->release & 0x07);
    switch (v->macro) {
    case VgMacro::Square1:
    case VgMacro::Square1Alt:
        td->panSweep = uint8_t(v->sweep);
        td->wavePointer = reinterpret_cast<uint32_t *>(uintptr_t(v->duty & 0x03));
        break;
    case VgMacro::Square2:
    case VgMacro::Square2Alt:
        td->panSweep = 0;
        td->wavePointer = reinterpret_cast<uint32_t *>(uintptr_t(v->duty & 0x03));
        break;
    case VgMacro::Noise:
    case VgMacro::NoiseAlt:
        td->wavePointer = reinterpret_cast<uint32_t *>(uintptr_t(v->period & 0x01));
        break;
    default:
        break; // ProgWave: wavePointer owns loaded wave data — never touched.
    }
    return true;
}

QStringList VoicegroupSource::directSoundSymbols(const QString &projectRoot)
{
    const QStringList all = symbolsFromFiles(
        {projectRoot + QStringLiteral("/sound/direct_sound_data.inc"),
         projectRoot + QStringLiteral("/sound/direct_sound_synth_data.inc")});
    // Phonemes (voice snippets, not instruments) go to the end of the list.
    QStringList instruments, phonemes;
    for (const QString &symbol : all) {
        (symbol.contains(QStringLiteral("Phoneme")) ? phonemes : instruments)
            .append(symbol);
    }
    return instruments + phonemes;
}

QList<QPair<QString, QString>> VoicegroupSource::keysplitInstruments(
    const QString &projectRoot)
{
    static const QRegularExpression keysplitRe(
        QStringLiteral(R"(^\s*voice_keysplit\s+(\w+)\s*,\s*(\w+))"));
    QMap<QString, QString> pairs; // sub-voicegroup -> table, sorted by key
    for (const QString &path : voicegroupFiles(projectRoot)) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        while (!file.atEnd()) {
            const QRegularExpressionMatch m =
                keysplitRe.match(QString::fromUtf8(file.readLine()));
            if (m.hasMatch() && !pairs.contains(m.captured(1)))
                pairs.insert(m.captured(1), m.captured(2));
        }
    }
    QList<QPair<QString, QString>> result;
    for (auto it = pairs.constBegin(); it != pairs.constEnd(); ++it)
        result.append({it.key(), it.value()});
    return result;
}

QStringList VoicegroupSource::drumkitInstruments(const QString &projectRoot)
{
    static const QRegularExpression drumkitRe(
        QStringLiteral(R"(^\s*voice_keysplit_all\s+(\w+))"));
    QStringList symbols;
    for (const QString &path : voicegroupFiles(projectRoot)) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        while (!file.atEnd()) {
            const QRegularExpressionMatch m =
                drumkitRe.match(QString::fromUtf8(file.readLine()));
            if (m.hasMatch())
                symbols.append(m.captured(1));
        }
    }
    symbols.removeDuplicates();
    std::sort(symbols.begin(), symbols.end());
    return symbols;
}

QStringList VoicegroupSource::progWaveSymbols(const QString &projectRoot)
{
    return symbolsFromFiles({projectRoot + QStringLiteral("/sound/programmable_wave_data.inc")});
}

namespace {

// Envelopes are counted encoded (attack<<24 | decay<<16 | sustain<<8 |
// release); the mode tie-breaks on the smaller code so the result doesn't
// depend on directory iteration order.
VgAdsr decodeAdsr(quint32 code)
{
    return VgAdsr{int(code >> 24), int((code >> 16) & 0xFF),
                  int((code >> 8) & 0xFF), int(code & 0xFF)};
}

quint32 adsrModeCode(const QHash<quint32, int> &counts)
{
    quint32 best = 0;
    int bestCount = 0;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
        if (it.value() > bestCount || (it.value() == bestCount && it.key() < best)) {
            best = it.key();
            bestCount = it.value();
        }
    }
    return best;
}

} // namespace

VgAdsrDefaults VoicegroupSource::typicalAdsr(const QString &projectRoot)
{
    QHash<int, QHash<quint32, int>> familyCounts;
    QHash<QString, QHash<quint32, int>> symbolCounts;
    for (const QString &path : voicegroupFiles(projectRoot)) {
        bool ok = false;
        const QByteArray content = readAllBytes(path, &ok);
        if (!ok)
            continue;
        bool endsWithNewline = false;
        for (const QByteArray &raw : splitLines(content, &endsWithNewline)) {
            int contentStart = 0, contentEnd = 0;
            contentBounds(raw, &contentStart, &contentEnd);
            const QByteArray text = raw.mid(contentStart, contentEnd - contentStart);
            const MacroDef *matched = nullptr;
            for (const MacroDef &def : kEditableMacros) {
                const int wordLen = int(qstrlen(def.word));
                if (def.requireSpace
                        ? (text.startsWith(def.word) && text.size() > wordLen
                           && text[wordLen] == ' ')
                        : text.startsWith(def.word)) {
                    matched = &def;
                    break;
                }
            }
            if (!matched || vgAdsrFamily(matched->macro) < 0)
                continue;
            const QList<QByteArray> args =
                text.mid(int(qstrlen(matched->word))).split(',');
            if (args.size() != macroArgCount(matched->macro))
                continue;
            int values[4];
            bool valid = true;
            for (int i = 0; i < 4 && valid; i++) {
                const QByteArray piece = args.at(args.size() - 4 + i).trimmed();
                valid = isIntArg(piece);
                values[i] = piece.toInt();
            }
            if (!valid)
                continue;
            // Count the values the engine would see (the loader's packing:
            // CGB fields masked, DirectSound fields truncated to a byte).
            const bool cgb = vgMacroIsCgb(matched->macro);
            const int attack = cgb ? values[0] & 0x07 : uint8_t(values[0]);
            const int decay = cgb ? values[1] & 0x07 : uint8_t(values[1]);
            const int sustain = cgb ? values[2] & 0x0F : uint8_t(values[2]);
            const int release = cgb ? values[3] & 0x07 : uint8_t(values[3]);
            // Only envelopes worth suggesting count: release 0 cuts off with
            // a click and a DirectSound attack of 0 never sounds at all.
            // This also drops the release-0 filler squares padding unused
            // slots, which would otherwise be the runaway Square 1 mode.
            if (release == 0 || (!cgb && attack == 0))
                continue;
            const quint32 code = quint32(attack) << 24 | quint32(decay) << 16
                | quint32(sustain) << 8 | quint32(release);
            familyCounts[vgAdsrFamily(matched->macro)][code]++;
            if (vgMacroHasSymbol(matched->macro)) {
                const QByteArray symbol = args.at(2).trimmed();
                if (!symbol.isEmpty())
                    symbolCounts[QString::fromUtf8(symbol)][code]++;
            }
        }
    }
    VgAdsrDefaults defaults;
    for (auto it = familyCounts.constBegin(); it != familyCounts.constEnd(); ++it)
        defaults.byFamily.insert(it.key(), decodeAdsr(adsrModeCode(it.value())));
    for (auto it = symbolCounts.constBegin(); it != symbolCounts.constEnd(); ++it)
        defaults.bySymbol.insert(it.key(), decodeAdsr(adsrModeCode(it.value())));
    return defaults;
}

bool VoicegroupSource::createVoicegroup(const QString &projectRoot, const QString &name,
                                        const QString &copyFromFile,
                                        const QString &copySectionLabel, QString *error)
{
    const QDir dir(projectRoot + QStringLiteral("/sound/voicegroups"));
    if (!dir.exists()) {
        if (error)
            *error = QStringLiteral("sound/voicegroups/ does not exist in this project.");
        return false;
    }
    const QString targetPath = dir.filePath(name + QStringLiteral(".inc"));
    if (QFile::exists(targetPath)) {
        if (error)
            *error = QStringLiteral("%1 already exists.").arg(targetPath);
        return false;
    }

    // Match the siblings' header style and line endings.
    bool labelStyle = false;
    bool alignBeforeLabel = false;
    bool crlf = false;
    const QStringList siblings =
        dir.entryList({QStringLiteral("*.inc")}, QDir::Files, QDir::Name);
    if (!siblings.isEmpty()) {
        const QByteArray bytes = readAllBytes(dir.filePath(siblings.first()));
        crlf = bytes.contains("\r\n");
        bool unusedNewline = false;
        for (const QByteArray &raw : splitLines(bytes, &unusedNewline)) {
            const QByteArray text = raw.trimmed();
            if (text.startsWith(".align"))
                alignBeforeLabel = true;
            static const QRegularExpression labelRe(QStringLiteral(R"(^(voicegroup\w+)::)"));
            if (labelRe.match(QString::fromUtf8(text)).hasMatch()) {
                labelStyle = true;
                break;
            }
            if (text.startsWith("voice_group "))
                break;
        }
    }

    QList<QByteArray> body;
    if (!copyFromFile.isEmpty()) {
        bool ok = false;
        const QByteArray bytes = readAllBytes(copyFromFile, &ok);
        if (!ok) {
            if (error)
                *error = QStringLiteral("Cannot read %1").arg(copyFromFile);
            return false;
        }
        bool unusedNewline = false;
        const QList<QByteArray> lines = splitLines(bytes, &unusedNewline);
        static const QRegularExpression declRe(
            QStringLiteral(R"(^\s*(voice_group\s+\w+|voicegroup\w+::))"));
        bool copying = false;
        for (const QByteArray &raw : lines) {
            QByteArray line = raw;
            if (line.endsWith('\r'))
                line.chop(1);
            const QByteArray text = line.trimmed();
            if (!copying) {
                if (copySectionLabel.isEmpty()
                        ? declRe.match(QString::fromUtf8(text)).hasMatch()
                        : text.startsWith(copySectionLabel.toUtf8() + "::"))
                    copying = true;
                continue;
            }
            if (!copySectionLabel.isEmpty()
                && (text.indexOf("::") > 0 || text.startsWith(".align")))
                break;
            body.append(line);
        }
        // Drop trailing blank lines so the new file ends cleanly.
        while (!body.isEmpty() && body.last().trimmed().isEmpty())
            body.removeLast();
        if (body.isEmpty()) {
            if (error)
                *error = QStringLiteral("No voice lines found to copy in %1").arg(copyFromFile);
            return false;
        }
    } else {
        // The dummy.inc convention: 128 silent-envelope square waves.
        for (int i = 0; i < VOICEGROUP_SIZE; i++)
            body.append(QByteArrayLiteral("\tvoice_square_1 60, 0, 0, 2, 0, 0, 15, 0"));
    }

    QList<QByteArray> lines;
    if (labelStyle) {
        if (alignBeforeLabel)
            lines.append(QByteArrayLiteral("\t.align 2"));
        lines.append(QStringLiteral("voicegroup_%1::").arg(name).toUtf8());
    } else {
        lines.append(QStringLiteral("voice_group %1").arg(name).toUtf8());
    }
    lines.append(body);

    QFile out(targetPath);
    if (!out.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("Cannot write %1").arg(targetPath);
        return false;
    }
    const QByteArray eol = crlf ? QByteArrayLiteral("\r\n") : QByteArrayLiteral("\n");
    for (const QByteArray &line : lines) {
        out.write(line);
        out.write(eol);
    }
    return true;
}

bool VoicegroupSource::appendIncludeLine(const QString &projectRoot, const QString &name,
                                         QString *error)
{
    const QString hubPath = projectRoot + QStringLiteral("/sound/voice_groups.inc");
    if (!QFile::exists(hubPath))
        return true; // discovery works without the hub; only the game build needs it

    bool ok = false;
    const QByteArray content = readAllBytes(hubPath, &ok);
    if (!ok) {
        if (error)
            *error = QStringLiteral("Cannot read %1").arg(hubPath);
        return false;
    }
    bool endsWithNewline = false;
    QList<QByteArray> lines = splitLines(content, &endsWithNewline);
    const bool crlf = content.contains("\r\n");

    int lastInclude = -1;
    QByteArray indent;
    for (int i = 0; i < lines.size(); i++) {
        QByteArray line = lines.at(i);
        if (line.endsWith('\r'))
            line.chop(1);
        const QByteArray text = line.trimmed();
        if (text.startsWith(".include")) {
            lastInclude = i;
            indent = leadingWs(line);
        }
    }
    QByteArray newLine =
        indent + ".include \"sound/voicegroups/" + name.toUtf8() + ".inc\"";
    if (crlf)
        newLine += '\r';
    lines.insert(lastInclude < 0 ? lines.size() : lastInclude + 1, newLine);

    QFile out(hubPath);
    if (!out.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("Cannot write %1").arg(hubPath);
        return false;
    }
    QByteArray joined;
    for (int i = 0; i < lines.size(); i++) {
        if (i > 0)
            joined += '\n';
        joined += lines.at(i);
    }
    if (endsWithNewline && !lines.isEmpty())
        joined += '\n';
    out.write(joined);
    return true;
}
