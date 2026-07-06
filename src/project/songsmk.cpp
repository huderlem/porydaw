#include "songsmk.h"

#include <QFile>
#include <QRegularExpression>
#include <QTextStream>

namespace SongsMk {

namespace {

// "STD_REVERB = 50" (also := ?= +=, though vanilla songs.mk only uses =).
const QRegularExpression &varRe()
{
    static const QRegularExpression re(
        QStringLiteral(R"(^([A-Za-z_][A-Za-z0-9_]*)\s*[:?+]?=\s*(.*)$)"));
    return re;
}

// "$(MID_SUBDIR)/mus_foo.s: %.s: %.mid" or the literal midi-dir path.
const QRegularExpression &ruleRe()
{
    static const QRegularExpression re(
        QStringLiteral(R"(^(?:\$\(MID_SUBDIR\)|sound/songs/midi)/(\w+)\.s\s*:)"));
    return re;
}

QString expandVars(QString text, const QHash<QString, QString> &vars)
{
    static const QRegularExpression refRe(
        QStringLiteral(R"(\$\(([A-Za-z_][A-Za-z0-9_]*)\))"));
    // Nested definitions terminate because unknown names expand to "" (as in
    // make); the depth cap breaks self-referential cycles.
    for (int depth = 0; depth < 8 && text.contains(QLatin1Char('$')); depth++) {
        QString out;
        qsizetype pos = 0;
        auto it = refRe.globalMatch(text);
        if (!it.hasNext())
            break;
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            out += text.mid(pos, m.capturedStart() - pos);
            out += vars.value(m.captured(1));
            pos = m.capturedEnd();
        }
        out += text.mid(pos);
        text = out;
    }
    return text;
}

QStringList flagsFromRecipe(const QString &recipe, const QHash<QString, QString> &vars)
{
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    QStringList flags;
    const QStringList tokens = recipe.trimmed().split(ws, Qt::SkipEmptyParts);
    for (const QString &token : tokens) {
        if (token.startsWith(QLatin1Char('-')))
            flags << expandVars(token, vars);
    }
    return flags;
}

void collectVar(const QString &line, QHash<QString, QString> *vars)
{
    QString text = line;
    const int hash = text.indexOf(QLatin1Char('#'));
    if (hash >= 0)
        text = text.left(hash);
    const QRegularExpressionMatch m = varRe().match(text);
    if (m.hasMatch())
        vars->insert(m.captured(1), m.captured(2).trimmed());
}

} // namespace

QString path(const QString &projectRoot)
{
    return projectRoot + QStringLiteral("/songs.mk");
}

QHash<QString, QStringList> parseFlags(const QString &mkPath)
{
    QHash<QString, QStringList> byLabel;
    QFile file(mkPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return byLabel;

    QHash<QString, QString> vars;
    QString pendingLabel;
    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine();
        if (line.startsWith(QLatin1Char('\t'))) { // a recipe line
            if (!pendingLabel.isEmpty() && line.contains(QStringLiteral("$(MID)"))) {
                byLabel.insert(pendingLabel, flagsFromRecipe(line, vars));
                pendingLabel.clear();
            }
            continue;
        }
        const QRegularExpressionMatch rule = ruleRe().match(line);
        if (rule.hasMatch()) {
            pendingLabel = rule.captured(1);
            continue;
        }
        pendingLabel.clear();
        collectVar(line, &vars);
    }
    return byLabel;
}

bool writeRule(const QString &mkPath, const QString &label, const QStringList &flags,
               QString *error)
{
    // Binary in/out: only the song's own recipe line may change, byte for
    // byte — the writeMidiCfgLine recipe.
    QByteArray content;
    {
        QFile in(mkPath);
        if (!in.open(QIODevice::ReadOnly)) {
            // Unlike midi.cfg, songs.mk is never created from scratch: a
            // project without one stores its flags in midi.cfg instead.
            if (error)
                *error = QStringLiteral("Cannot read %1").arg(mkPath);
            return false;
        }
        content = in.readAll();
    }
    const bool endsWithNewline = content.isEmpty() || content.endsWith('\n');
    const bool crlf = content.contains("\r\n");
    QList<QByteArray> lines = content.split('\n');
    if (endsWithNewline && !lines.isEmpty())
        lines.removeLast(); // the empty piece after the final newline

    QHash<QString, QString> vars;
    for (const QByteArray &raw : lines) {
        const QString text =
            QString::fromUtf8(raw.endsWith('\r') ? raw.chopped(1) : raw);
        if (!text.startsWith(QLatin1Char('\t')))
            collectVar(text, &vars);
    }

    int ruleIdx = -1;
    for (int i = 0; i < lines.size(); i++) {
        const QString text = QString::fromUtf8(
            lines[i].endsWith('\r') ? lines[i].chopped(1) : lines[i]);
        const QRegularExpressionMatch m = ruleRe().match(text);
        if (m.hasMatch() && m.captured(1) == label) {
            ruleIdx = i;
            break;
        }
    }

    bool replaced = false;
    if (ruleIdx >= 0) {
        for (int i = ruleIdx + 1; i < lines.size(); i++) {
            const bool hadCr = lines[i].endsWith('\r');
            const QString text =
                QString::fromUtf8(hadCr ? lines[i].chopped(1) : lines[i]);
            if (!text.startsWith(QLatin1Char('\t')))
                break; // past the rule's recipe
            if (!text.contains(QStringLiteral("$(MID)")))
                continue;

            // Keep the original spelling (e.g. -R$(STD_REVERB), -v080) of any
            // flag whose expanded value is unchanged.
            static const QRegularExpression ws(QStringLiteral("\\s+"));
            QHash<QChar, QPair<QString, QString>> existing; // letter -> (text, expanded)
            const QStringList tokens = text.trimmed().split(ws, Qt::SkipEmptyParts);
            for (const QString &token : tokens) {
                if (token.size() >= 2 && token[0] == QLatin1Char('-'))
                    existing.insert(token[1].toUpper(),
                                    {token, expandVars(token, vars)});
            }
            QStringList spelled;
            for (const QString &flag : flags) {
                const auto it = flag.size() >= 2 ? existing.constFind(flag[1].toUpper())
                                                 : existing.constEnd();
                spelled << (it != existing.constEnd()
                                    && it->second.compare(flag, Qt::CaseInsensitive) == 0
                                ? it->first
                                : flag);
            }

            const int at = text.indexOf(QStringLiteral("$@"));
            QString newText = at >= 0 ? text.left(at + 2)
                                      : QStringLiteral("\t$(MID) $< $@");
            if (!spelled.isEmpty())
                newText += QLatin1Char(' ') + spelled.join(QLatin1Char(' '));
            lines[i] = newText.toUtf8();
            if (hadCr)
                lines[i] += '\r';
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        const QByteArray eol = crlf ? "\r" : "";
        QString recipe = QStringLiteral("\t$(MID) $< $@");
        if (!flags.isEmpty())
            recipe += QLatin1Char(' ') + flags.join(QLatin1Char(' '));
        if (ruleIdx >= 0) {
            // The rule exists but has no $(MID) recipe: give it one.
            lines.insert(ruleIdx + 1, recipe.toUtf8() + eol);
        } else {
            if (!lines.isEmpty() && lines.last() != eol)
                lines.append(eol); // blank separator, matching the file's style
            lines.append(QStringLiteral("$(MID_SUBDIR)/%1.s: %.s: %.mid")
                             .arg(label)
                             .toUtf8()
                         + eol);
            lines.append(recipe.toUtf8() + eol);
        }
    }

    QFile out(mkPath);
    if (!out.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("Cannot write %1").arg(mkPath);
        return false;
    }
    QByteArray joined = lines.join('\n');
    if (endsWithNewline)
        joined += '\n';
    out.write(joined);
    return true;
}

} // namespace SongsMk
