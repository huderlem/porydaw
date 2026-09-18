#include "songbundle.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
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

} // namespace SongBundle
