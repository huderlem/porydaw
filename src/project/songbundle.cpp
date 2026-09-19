#include "songbundle.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>

#include "bundlearchive.h"

namespace {

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

QJsonValue nullable(const QString &s)
{
    return s.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(s);
}

QJsonArray stringArray(const QStringList &list)
{
    QJsonArray out;
    for (const QString &s : list)
        out.append(s);
    return out;
}

// A missing key reads as empty; a present key must hold strings.
bool readStringList(const QJsonObject &obj, const QString &key, QStringList *out, QString *error)
{
    out->clear();
    if (!obj.contains(key))
        return true;
    const QJsonValue v = obj.value(key);
    if (!v.isArray()) {
        setError(error, QStringLiteral("porysong.json: \"%1\" is not an array").arg(key));
        return false;
    }
    const QJsonArray arr = v.toArray();
    for (const QJsonValue &item : arr) {
        if (!item.isString()) {
            setError(error, QStringLiteral("porysong.json: \"%1\" holds a non-string").arg(key));
            return false;
        }
        out->append(item.toString());
    }
    return true;
}

} // namespace

bool BundleManifest::operator==(const BundleManifest &o) const
{
    return format == o.format && porydaw == o.porydaw && label == o.label &&
           voicegroup == o.voicegroup && flags == o.flags && constant == o.constant &&
           player == o.player && layout == o.layout && extensions == o.extensions &&
           synth == o.synth && samples == o.samples && subVoicegroups == o.subVoicegroups &&
           keysplitTables == o.keysplitTables && waves == o.waves && synths == o.synths;
}

QByteArray BundleManifest::toJson() const
{
    QJsonObject song;
    song.insert(QStringLiteral("label"), label);
    song.insert(QStringLiteral("voicegroup"), voicegroup);
    song.insert(QStringLiteral("flags"), flags);
    song.insert(QStringLiteral("constant"), nullable(constant));
    song.insert(QStringLiteral("player"), nullable(player));

    QJsonObject requiresObj;
    requiresObj.insert(QStringLiteral("extensions"), stringArray(extensions));
    requiresObj.insert(QStringLiteral("synth"), synth);

    QJsonArray sampleArr;
    for (const BundleSample &s : samples) {
        QJsonObject obj;
        obj.insert(QStringLiteral("name"), s.name);
        obj.insert(QStringLiteral("file"), s.file);
        obj.insert(QStringLiteral("sha256"), s.sha256);
        sampleArr.append(obj);
    }

    QJsonObject root;
    root.insert(QStringLiteral("format"), format);
    root.insert(QStringLiteral("porydaw"), porydaw);
    root.insert(QStringLiteral("song"), song);
    root.insert(QStringLiteral("layout"), layout);
    root.insert(QStringLiteral("requires"), requiresObj);
    root.insert(QStringLiteral("samples"), sampleArr);
    root.insert(QStringLiteral("subVoicegroups"), stringArray(subVoicegroups));
    root.insert(QStringLiteral("keysplitTables"), stringArray(keysplitTables));
    root.insert(QStringLiteral("waves"), stringArray(waves));
    root.insert(QStringLiteral("synths"), stringArray(synths));
    // QJsonObject emits keys sorted, so identical manifests serialize
    // identically (the exporter's byte-identical re-export relies on it).
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool BundleManifest::fromJson(const QByteArray &json, BundleManifest *manifest, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (doc.isNull()) {
        setError(error, QStringLiteral("porysong.json: %1 at offset %2")
                            .arg(parseError.errorString())
                            .arg(parseError.offset));
        return false;
    }
    if (!doc.isObject()) {
        setError(error, QStringLiteral("porysong.json: top level is not an object"));
        return false;
    }
    const QJsonObject root = doc.object();
    const QJsonValue formatValue = root.value(QStringLiteral("format"));
    if (!formatValue.isDouble() || formatValue.toDouble() != double(formatValue.toInt()) ||
        formatValue.toInt() < 1) {
        setError(error, QStringLiteral("porysong.json: missing or invalid \"format\""));
        return false;
    }
    BundleManifest m;
    m.format = formatValue.toInt();
    if (m.format > kFormat) {
        setError(error, QStringLiteral("This song bundle uses format %1, but this porydaw "
                                       "reads up to format %2. Update porydaw to open it.")
                            .arg(m.format)
                            .arg(kFormat));
        return false;
    }
    m.porydaw = root.value(QStringLiteral("porydaw")).toString();

    const QJsonValue songValue = root.value(QStringLiteral("song"));
    if (!songValue.isObject()) {
        setError(error, QStringLiteral("porysong.json: missing \"song\" object"));
        return false;
    }
    const QJsonObject song = songValue.toObject();
    m.label = song.value(QStringLiteral("label")).toString();
    m.voicegroup = song.value(QStringLiteral("voicegroup")).toString();
    m.flags = song.value(QStringLiteral("flags")).toString();
    m.constant = song.value(QStringLiteral("constant")).toString(); // null → empty
    m.player = song.value(QStringLiteral("player")).toString();
    if (m.label.isEmpty() || m.voicegroup.isEmpty()) {
        setError(error, QStringLiteral("porysong.json: \"song\" needs a label and a voicegroup"));
        return false;
    }
    m.layout = root.value(QStringLiteral("layout")).toString();

    const QJsonObject requiresObj = root.value(QStringLiteral("requires")).toObject();
    if (!readStringList(requiresObj, QStringLiteral("extensions"), &m.extensions, error))
        return false;
    m.synth = requiresObj.value(QStringLiteral("synth")).toBool(false);

    const QJsonValue samplesValue = root.value(QStringLiteral("samples"));
    if (root.contains(QStringLiteral("samples")) && !samplesValue.isArray()) {
        setError(error, QStringLiteral("porysong.json: \"samples\" is not an array"));
        return false;
    }
    const QJsonArray sampleArr = samplesValue.toArray();
    for (const QJsonValue &item : sampleArr) {
        if (!item.isObject()) {
            setError(error, QStringLiteral("porysong.json: \"samples\" holds a non-object"));
            return false;
        }
        const QJsonObject obj = item.toObject();
        BundleSample s;
        s.name = obj.value(QStringLiteral("name")).toString();
        s.file = obj.value(QStringLiteral("file")).toString();
        s.sha256 = obj.value(QStringLiteral("sha256")).toString();
        if (s.name.isEmpty() || s.file.isEmpty()) {
            setError(error, QStringLiteral("porysong.json: sample entry without name/file"));
            return false;
        }
        m.samples.append(s);
    }
    if (!readStringList(root, QStringLiteral("subVoicegroups"), &m.subVoicegroups, error) ||
        !readStringList(root, QStringLiteral("keysplitTables"), &m.keysplitTables, error) ||
        !readStringList(root, QStringLiteral("waves"), &m.waves, error) ||
        !readStringList(root, QStringLiteral("synths"), &m.synths, error))
        return false;

    *manifest = m;
    return true;
}

bool BundleManifest::write(const QString &bundleRoot, QString *error) const
{
    const QString path = SongBundle::manifestPath(bundleRoot);
    const QByteArray bytes = toJson();
    QDir().mkpath(bundleRoot);
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit()) {
        setError(error, QStringLiteral("Cannot write %1: %2").arg(path, out.errorString()));
        return false;
    }
    return true;
}

bool BundleManifest::read(const QString &bundleRoot, BundleManifest *manifest, QString *error)
{
    const QString path = SongBundle::manifestPath(bundleRoot);
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Not a song bundle (no %1): %2")
                            .arg(SongBundle::manifestFileName(), bundleRoot));
        return false;
    }
    return fromJson(in.readAll(), manifest, error);
}

namespace SongBundle {

QString manifestFileName()
{
    return QString::fromLatin1(BundleArchive::manifestEntryName());
}

QString manifestPath(const QString &bundleRoot)
{
    return bundleRoot + QLatin1Char('/') + manifestFileName();
}

bool isBundleDir(const QString &root)
{
    return QFileInfo(manifestPath(root)).isFile();
}

namespace {

bool plainIdentifier(const QString &s)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_]+$"));
    return re.match(s).hasMatch();
}

// Every .incbin/.include target in the bundle's assembler sources must stay
// under the root: the loader opens them verbatim relative to it. The loader
// finds a directive with strstr and takes the line's first quoted string, so
// this is no stricter: every quoted string on a line naming either directive.
bool sourcesStayInside(const QString &bundleRoot, QString *error)
{
    static const QRegularExpression directive(QStringLiteral("\\.(?:incbin|include)"));
    static const QRegularExpression quoted(QStringLiteral("\"([^\"]*)\""));
    QDirIterator it(bundleRoot, {QStringLiteral("*.inc"), QStringLiteral("*.s")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        QFile in(path);
        if (!in.open(QIODevice::ReadOnly))
            continue;
        const QString text = QString::fromUtf8(in.readAll());
        static const QRegularExpression lineBreak(QStringLiteral("[\r\n]"));
        for (const QString &line : text.split(lineBreak, Qt::SkipEmptyParts)) {
            if (!directive.match(line).hasMatch())
                continue;
            auto matches = quoted.globalMatch(line);
            while (matches.hasNext()) {
                const QString target = matches.next().captured(1);
                QString normalized, reason;
                if (!BundleArchive::validateEntryName(target, &normalized, &reason)) {
                    setError(error,
                             QStringLiteral("Song bundle refused: %1 references \"%2\" (%3)")
                                 .arg(QDir(bundleRoot).relativeFilePath(path), target, reason));
                    return false;
                }
            }
        }
    }
    return true;
}

} // namespace

bool readSong(const QString &bundleRoot, BundleManifest *manifest, SongInfo *song, QString *error)
{
    if (!BundleManifest::read(bundleRoot, manifest, error))
        return false;
    if (!plainIdentifier(manifest->label)) {
        setError(error, QStringLiteral("Song bundle refused: \"%1\" is not a valid song label")
                            .arg(manifest->label));
        return false;
    }
    if (!sourcesStayInside(bundleRoot, error))
        return false;

    const QString midiDir = bundleRoot + QStringLiteral("/sound/songs/midi/");
    SongInfo info;
    info.label = manifest->label;
    info.constant = manifest->constant;
    info.player = manifest->player;
    info.midPath = midiDir + manifest->label + QStringLiteral(".mid");
    info.hasMid = QFileInfo(info.midPath).isFile();
    info.registered = false;
    if (!info.hasMid) {
        setError(error, QStringLiteral("Song bundle is incomplete: no sound/songs/midi/%1.mid")
                            .arg(manifest->label));
        return false;
    }

    QFile cfgFile(midiDir + QStringLiteral("midi.cfg"));
    if (cfgFile.open(QIODevice::ReadOnly)) {
        const QStringList lines = QString::fromUtf8(cfgFile.readAll()).split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            QString label;
            SongCfg cfg;
            if (DecompProject::parseMidiCfgLine(line, &label, &cfg) && label == info.label) {
                info.cfg = cfg;
                info.hasCfg = true;
            }
        }
    }
    if (!info.hasCfg) {
        info.cfg = DecompProject::cfgFromFlags(
            manifest->flags.split(QLatin1Char(' '), Qt::SkipEmptyParts));
    }
    if (!info.cfg.voicegroupArg.isEmpty() && !plainIdentifier(info.cfg.voicegroupArg)) {
        setError(error, QStringLiteral("Song bundle refused: \"-G%1\" is not a valid voicegroup")
                            .arg(info.cfg.voicegroupArg));
        return false;
    }
    *song = info;
    return true;
}

} // namespace SongBundle
