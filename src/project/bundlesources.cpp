#include "bundlesources.h"

#include <QRegularExpression>

namespace SongBundle::Sources {

// The loader's strip_comment + rtrim + ltrim over one line.
QByteArray contentOf(QByteArray line)
{
    const int at = line.indexOf('@');
    if (at >= 0)
        line.truncate(at);
    const int slashes = line.indexOf("//");
    if (slashes >= 0)
        line.truncate(slashes);
    return line.trimmed();
}

// Label -> .incbin path, first definition wins (symbol_map_find). Labels
// consumed by a set_synth_* macro are not samples and are skipped: a new
// label simply replaces a pending one, as in parse_direct_sound_data_file.
void collectIncbins(const QByteArray &content, QHash<QString, QString> *map)
{
    static const QRegularExpression labelRe(QStringLiteral(R"(^(\w+):)"));
    QString pending;
    for (const QByteArray &raw : content.split('\n')) {
        const QByteArray text = contentOf(raw);
        const QRegularExpressionMatch label = labelRe.match(QString::fromUtf8(text));
        if (label.hasMatch()) {
            pending = label.captured(1);
            continue;
        }
        if (pending.isEmpty())
            continue;
        if (text.contains(".incbin")) {
            const int q1 = text.indexOf('"');
            const int q2 = q1 < 0 ? -1 : text.indexOf('"', q1 + 1);
            if (q2 > q1 && !map->contains(pending))
                map->insert(pending, QString::fromUtf8(text.mid(q1 + 1, q2 - q1 - 1)));
            pending.clear();
        } else if (text.startsWith("set_synth_")) {
            pending.clear();
        }
    }
}

// The source text of each keysplit table in a keysplit_tables.inc, keyed by
// the symbol voice_keysplit lines use. A table runs from its declaring line
// to the next declaration, minus trailing blank / comment-only / .align
// lines (which belong to whatever follows). First definition wins.
QHash<QString, QList<QByteArray>> collectKeysplitTables(const QByteArray &content)
{
    static const QRegularExpression macroRe(QStringLiteral(R"(^keysplit\s+([^,\s]+))"));
    static const QRegularExpression setRe(QStringLiteral(R"(^\.set\s+([^,\s]+)\s*,\s*\.\s*-)"));
    QHash<QString, QList<QByteArray>> tables;
    QString current;
    QList<QByteArray> block;
    const auto flush = [&] {
        while (!block.isEmpty()) {
            const QByteArray text = contentOf(block.last());
            if (!text.isEmpty() && !text.startsWith(".align"))
                break;
            block.removeLast();
        }
        if (!current.isEmpty() && !tables.contains(current))
            tables.insert(current, block);
        block.clear();
    };
    for (QByteArray raw : content.split('\n')) {
        if (raw.endsWith('\r'))
            raw.chop(1);
        const QString text = QString::fromUtf8(contentOf(raw));
        QRegularExpressionMatch m = macroRe.match(text);
        QString declared;
        if (m.hasMatch())
            declared = QStringLiteral("keysplit_") + m.captured(1);
        else if ((m = setRe.match(text)).hasMatch())
            declared = m.captured(1);
        if (!declared.isEmpty()) {
            flush();
            current = declared;
        }
        if (!current.isEmpty())
            block.append(raw);
    }
    flush();
    return tables;
}

bool parseKeysplitTable(const QList<QByteArray> &block, KeysplitTable *table)
{
    static const QRegularExpression macroRe(
        QStringLiteral(R"(^keysplit\s+[^,\s]+\s*(?:,\s*(\d+))?$)"));
    static const QRegularExpression setRe(
        QStringLiteral(R"(^\.set\s+[^,\s]+\s*,\s*\.\s*-\s*(\d+)$)"));
    static const QRegularExpression splitRe(QStringLiteral(R"(^split\s+(\d+)\s*,\s*(\d+)$)"));
    static const QRegularExpression byteRe(QStringLiteral(R"(^\.byte\s+(.+)$)"));

    KeysplitTable out;
    bool declared = false;
    int lastNote = 0;
    for (const QByteArray &raw : block) {
        const QString text = QString::fromUtf8(contentOf(raw));
        if (text.isEmpty())
            continue;
        QRegularExpressionMatch m;
        if (!declared) {
            if ((m = macroRe.match(text)).hasMatch())
                out.macroForm = true;
            else if (!(m = setRe.match(text)).hasMatch())
                return false;
            out.offset = m.captured(1).toInt(); // "" -> 0: the label sits on the data
            lastNote = out.offset;
            declared = true;
            continue;
        }
        if (out.macroForm) {
            if (!(m = splitRe.match(text)).hasMatch())
                return false;
            const int index = m.captured(1).toInt();
            const int end = m.captured(2).toInt();
            if (index > 255 || end < lastNote || end > 256)
                return false;
            out.bytes.append(end - lastNote, char(index));
            lastNote = end;
        } else {
            if (!(m = byteRe.match(text)).hasMatch())
                return false;
            for (const QString &piece : m.captured(1).split(QLatin1Char(','))) {
                bool ok = false;
                const int value = piece.trimmed().toInt(&ok, 0);
                if (!ok || value < 0 || value > 255)
                    return false;
                out.bytes.append(char(value));
            }
        }
    }
    if (!declared || out.bytes.size() > 256)
        return false;
    *table = out;
    return true;
}

QList<QByteArray> renderKeysplitTable(const QString &symbol, const KeysplitTable &table,
                                      bool macroForm)
{
    QList<QByteArray> lines;
    if (macroForm) {
        QByteArray decl = "keysplit " + symbol.mid(int(qstrlen("keysplit_"))).toUtf8();
        if (table.offset != 0)
            decl += ", " + QByteArray::number(table.offset);
        lines.append(decl);
        int note = table.offset;
        for (int i = 0; i < table.bytes.size();) {
            int run = 1;
            while (i + run < table.bytes.size() && table.bytes.at(i + run) == table.bytes.at(i))
                run++;
            note += run;
            lines.append("\tsplit " + QByteArray::number(int(uchar(table.bytes.at(i)))) + ", " +
                         QByteArray::number(note));
            i += run;
        }
        return lines;
    }
    lines.append(".set " + symbol.toUtf8() + ", . - " + QByteArray::number(table.offset));
    for (int i = 0; i < table.bytes.size(); i++)
        lines.append("\t.byte " + QByteArray::number(int(uchar(table.bytes.at(i)))) + " @ " +
                     QByteArray::number(table.offset + i));
    return lines;
}

} // namespace SongBundle::Sources
