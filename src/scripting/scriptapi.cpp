#include "scriptapi.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QImageReader>
#include <QInputDialog>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSpinBox>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "audio/audioengine.h"
#include "audio/wavexport.h"
#include "core/smf.h"
#include "core/songdocument.h"
#include "project/decompproject.h"
#include "project/sidecar.h"
#include "project/songregistry.h"
#include "project/voicegroupsource.h"
#include "scripthost.h"
#include "scriptmenus.h"
#include "scriptwidgets.h"
#include "songsession.h"
#include "ui/songview.h"
#include "ui/theme/themeruntime.h"
#include "ui/viewsidecar.h"

namespace scripting {

namespace {

QString storageKey(const Plugin &plugin, const QString &key)
{
    return QStringLiteral("plugins/") + plugin.manifest.id + QStringLiteral("/data/") + key;
}

// Script numbers → ticks/ids: NaN/Infinity read as 0 and anything past
// kMaxTick is clamped, so a wild value never reaches the document (an
// out-of-range double→uint64 conversion is undefined behaviour).
constexpr double kMaxTick = double(1ull << 40);

uint64_t clampTick(double value)
{
    if (!std::isfinite(value) || value <= 0.0)
        return 0;
    return uint64_t(std::min(value, kMaxTick));
}

// NoteId tokens are sequential; the same guard, with 0 as "no note".
uint64_t clampId(double value)
{
    if (!std::isfinite(value) || value <= 0.0)
        return 0;
    return uint64_t(std::min(value, 9007199254740992.0)); // 2^53, exact in a double
}

int clampInt(const QVariant &v, int lo, int hi)
{
    const double d = v.toDouble();
    if (!std::isfinite(d))
        return lo;
    return int(std::clamp(d, double(lo), double(hi)));
}

// Script tick deltas: NaN/Infinity read as 0, otherwise clamped to a span
// the document's clamps can absorb.
int64_t clampDelta(double value)
{
    if (!std::isfinite(value))
        return 0;
    return int64_t(std::clamp(value, -kMaxTick, kMaxTick));
}

// A track argument must be an actual integer: JS undefined/NaN/null would
// otherwise coerce to 0 and silently target track 0.
bool variantTrack(const QVariant &v, int *out)
{
    if (!v.isValid() || v.isNull() || v.userType() == QMetaType::QString ||
        v.userType() == QMetaType::Bool)
        return false;
    bool ok = false;
    const double d = v.toDouble(&ok);
    if (!ok || !std::isfinite(d) || d != std::floor(d) || d < -1.0 || d > 1e6)
        return false;
    *out = int(d);
    return true;
}

bool isLaneCc(int cc)
{
    return (cc >= 0 && cc <= 127) || cc == DOC_CC_BEND || cc == DOC_CC_TEMPO || cc == DOC_CC_VOICE;
}

int clampLaneValue(int cc, const QVariant &v)
{
    if (cc == DOC_CC_BEND)
        return clampInt(v, -8192, 8191);
    if (cc == DOC_CC_TEMPO)
        return clampInt(v, SongDocument::kTempoMin, SongDocument::kTempoMax);
    return clampInt(v, 0, 127);
}

bool tickRange(const QVariantMap &opts, uint64_t *from, uint64_t *to)
{
    *from = 0;
    *to = UINT64_MAX;
    if (opts.contains(QStringLiteral("from")))
        *from = clampTick(opts.value(QStringLiteral("from")).toDouble());
    if (opts.contains(QStringLiteral("to")))
        *to = clampTick(opts.value(QStringLiteral("to")).toDouble());
    return *from < *to;
}

// Pauses the watchdog for the scope: a modal dialog's nested event loop
// (or a long render) is not the script's CPU time.
struct WatchdogPause {
    explicit WatchdogPause(ScriptHost &host) : host(host) { host.pauseWatchdog(); }
    ~WatchdogPause() { host.resumeWatchdog(); }
    ScriptHost &host;
};

// Windows and (by default) macOS match filenames without regard to case,
// so two spellings of one folder must not read as two folders.
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
#endif

bool underRoot(const QString &root, const QString &path)
{
    if (root.isEmpty())
        return false;
    if (path.compare(root, kPathCase) == 0)
        return true;
    const QString prefix =
        root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/'); // never "//" at the root
    return path.startsWith(prefix, kPathCase);
}

// The same path spelled the way the filesystem itself spells it: symlinks
// resolved and, on Apple's volumes, composed Unicode (APFS and HFS+ store
// filenames decomposed, so a script's "Café" is not literally the "Café"
// entryList() hands back). Only the part that exists can be resolved — a
// file about to be written does not yet — so the deepest existing
// ancestor is resolved and the rest re-appended.
QString canonicalPath(const QString &path)
{
    QFileInfo info(QDir::cleanPath(path));
    QStringList rest;
    for (;;) {
        const QString real = info.canonicalFilePath();
        if (!real.isEmpty()) {
            QString out = real;
            for (auto it = rest.crbegin(); it != rest.crend(); ++it)
                out = QDir(out).filePath(*it);
#ifdef Q_OS_MACOS
            return out.normalized(QString::NormalizationForm_C);
#else
            return out;
#endif
        }
        // A root that does not resolve (a missing drive, an unmounted
        // volume): nothing above it left to try.
        if (info.fileName().isEmpty() || info.dir().path() == info.filePath())
            return QDir::cleanPath(path);
        rest.append(info.fileName());
        info = QFileInfo(info.dir().path());
    }
}

// porydaw.io's sandbox: the absolute path when `input` (relative paths
// against the plugin folder) is inside the plugin folder, the open
// project, or a path the user picked through a dialog; empty with *error
// otherwise. Paths are cleaned first, so ".." can't walk out.
QString sandboxPath(const Plugin &plugin, const ScriptHost &host, const QString &input,
                    QString *error)
{
    if (input.trimmed().isEmpty()) {
        *error = QStringLiteral("expected a path");
        return QString();
    }
    if (!QFileInfo(input).isAbsolute() && plugin.dir.isEmpty()) {
        // The Script Console has no folder of its own.
        *error = QStringLiteral("'%1' is relative and this plugin has no folder; use an "
                                "absolute path")
                     .arg(input);
        return QString();
    }
    const QString abs = QFileInfo(input).isAbsolute()
                            ? QDir::cleanPath(input)
                            : QDir::cleanPath(plugin.dir + QLatin1Char('/') + input);
    // Two chances, so this only ever widens what already worked: the
    // paths as written, and both sides resolved to what the filesystem
    // calls them. The second catches the folder reached through a
    // symlink (a plugin folder linked in from a checkout; macOS handing
    // back /private/tmp for /tmp), a differently-cased spelling, and
    // Apple's decomposed filenames.
    QString key; // resolved on demand: a path already spelled the way it
                 // is stored never touches the filesystem for this
    const auto under = [&abs, &key](const QString &root) {
        if (root.isEmpty())
            return false;
        if (underRoot(QDir::cleanPath(root), abs))
            return true;
        if (key.isNull())
            key = canonicalPath(abs);
        return underRoot(canonicalPath(root), key);
    };
    if (under(plugin.dir))
        return abs;
    const DecompProject *p = host.bindings().project;
    if (p && p->isOpen() && under(p->root()))
        return abs;
    for (const QString &granted : plugin.grantedPaths) {
        if (under(granted))
            return abs;
    }
    *error = QStringLiteral("'%1' is outside the plugin folder, the project and the files the "
                            "user picked")
                 .arg(input);
    return QString();
}

// ---- raw SMF events ↔ JS ----

QString rawTypeName(const SmfEvent &ev)
{
    if (ev.isMeta())
        return QStringLiteral("meta");
    if (ev.isSysEx())
        return QStringLiteral("sysex");
    switch (ev.typeNibble()) {
    case 0x8:
        return QStringLiteral("noteOff");
    case 0x9:
        return ev.data1 ? QStringLiteral("noteOn") : QStringLiteral("noteOff");
    case 0xA:
        return QStringLiteral("aftertouch");
    case 0xB:
        return QStringLiteral("cc");
    case 0xC:
        return QStringLiteral("program");
    case 0xD:
        return QStringLiteral("pressure");
    case 0xE:
        return QStringLiteral("bend");
    default:
        return QStringLiteral("unknown");
    }
}

QVariantMap rawEventToVariant(size_t index, const SmfEvent &ev)
{
    QVariantMap m{{QStringLiteral("index"), double(index)},
                  {QStringLiteral("tick"), double(ev.tick)},
                  {QStringLiteral("status"), int(ev.status)},
                  {QStringLiteral("type"), rawTypeName(ev)}};
    if (ev.isChannel()) {
        m.insert(QStringLiteral("channel"), int(ev.channel()));
        m.insert(QStringLiteral("data0"), int(ev.data0));
        m.insert(QStringLiteral("data1"), int(ev.data1));
    }
    if (ev.isMeta())
        m.insert(QStringLiteral("metaType"), int(ev.metaType));
    if (ev.isMeta() || ev.isSysEx()) {
        QVariantList blob;
        for (char b : ev.blob)
            blob.append(int(uint8_t(b)));
        m.insert(QStringLiteral("blob"), blob);
        if (ev.isMeta() && ev.metaType >= 1 && ev.metaType <= 7)
            m.insert(QStringLiteral("text"), QString::fromLatin1(ev.blob));
    }
    return m;
}

// {tick, status | type (+ channel), data0, data1, metaType, blob | text}.
bool rawEventFromVariant(const QVariantMap &spec, SmfEvent *out, QString *error)
{
    SmfEvent ev;
    ev.tick = clampTick(spec.value(QStringLiteral("tick")).toDouble());
    int status = -1;
    if (spec.contains(QStringLiteral("status"))) {
        status = clampInt(spec.value(QStringLiteral("status")), 0, 255);
    } else {
        const QString type = spec.value(QStringLiteral("type")).toString();
        const int channel = clampInt(spec.value(QStringLiteral("channel"), 0), 0, 15);
        static const std::map<QString, int> nibbles = {
            {QStringLiteral("noteOff"), 0x8},    {QStringLiteral("noteOn"), 0x9},
            {QStringLiteral("aftertouch"), 0xA}, {QStringLiteral("cc"), 0xB},
            {QStringLiteral("program"), 0xC},    {QStringLiteral("pressure"), 0xD},
            {QStringLiteral("bend"), 0xE}};
        if (type == QLatin1String("meta")) {
            status = 0xFF;
        } else if (type == QLatin1String("sysex")) {
            status = 0xF0;
        } else {
            const auto it = nibbles.find(type);
            if (it == nibbles.end()) {
                *error = QStringLiteral("an event needs a status byte or a type (noteOn, noteOff, "
                                        "cc, program, bend, aftertouch, pressure, meta, sysex)");
                return false;
            }
            status = (it->second << 4) | channel;
        }
    }
    if (!((status >= 0x80 && status < 0xF0) || status == 0xF0 || status == 0xF7 ||
          status == 0xFF)) {
        *error = QStringLiteral("status must be 0x80-0xEF, 0xF0, 0xF7 or 0xFF");
        return false;
    }
    ev.status = uint8_t(status);
    if (ev.isChannel()) {
        ev.data0 = uint8_t(clampInt(spec.value(QStringLiteral("data0")), 0, 127));
        ev.data1 = uint8_t(clampInt(spec.value(QStringLiteral("data1")), 0, 127));
    }
    if (ev.isMeta()) {
        if (!spec.contains(QStringLiteral("metaType"))) {
            *error = QStringLiteral("a meta event needs metaType");
            return false;
        }
        ev.metaType = uint8_t(clampInt(spec.value(QStringLiteral("metaType")), 0, 127));
    }
    if (ev.isMeta() || ev.isSysEx()) {
        if (spec.contains(QStringLiteral("blob"))) {
            for (const QVariant &b : spec.value(QStringLiteral("blob")).toList())
                ev.blob.append(char(clampInt(b, 0, 255)));
        } else if (spec.contains(QStringLiteral("text"))) {
            ev.blob = spec.value(QStringLiteral("text")).toString().toLatin1();
        }
    }
    *out = ev;
    return true;
}

} // namespace

// An invalid QVariant crosses as `undefined`; the API promises `null`.
QVariant jsNull()
{
    return QVariant::fromValue(nullptr);
}

QVariantMap noteToVariant(const DocNote &note)
{
    return {{QStringLiteral("id"), double(note.noteId.token())},
            {QStringLiteral("track"), note.engineTrack},
            {QStringLiteral("tick"), double(note.tick)},
            {QStringLiteral("key"), int(note.key)},
            {QStringLiteral("len"), double(note.duration)},
            {QStringLiteral("vel"), int(note.velocity)}};
}

// ---- ApiObject ----

ApiObject::ApiObject(ScriptHost &host, Plugin &plugin) : m_host(host), m_plugin(plugin) {}

SongSession *ApiObject::session() const
{
    return m_host.session();
}

SongDocument *ApiObject::doc() const
{
    SongSession *s = session();
    return s ? &s->doc : nullptr;
}

SongView *ApiObject::view() const
{
    SongSession *s = session();
    return s ? s->view : nullptr;
}

QJSEngine *ApiObject::engine() const
{
    return m_plugin.engine.get();
}

void ApiObject::throwError(const QString &message) const
{
    if (QJSEngine *e = engine())
        e->throwError(QJSValue::TypeError, message);
}

// ---- HostApi ----

QString HostApi::appVersion() const
{
    return QStringLiteral(PORYDAW_VERSION);
}

QString HostApi::apiVersion() const
{
    return QLatin1String(kApiVersion);
}

int HostApi::apiMajor() const
{
    return kApiMajor;
}

QString HostApi::pluginId() const
{
    return m_plugin.manifest.id;
}

QString HostApi::pluginName() const
{
    return m_plugin.manifest.name;
}

QString HostApi::pluginVersion() const
{
    return m_plugin.manifest.version;
}

QString HostApi::pluginDir() const
{
    return m_plugin.dir;
}

void HostApi::log(int level, const QString &text)
{
    m_host.log(m_plugin, LogLevel(std::clamp(level, 0, 2)), text);
}

void HostApi::reportError(const QJSValue &error)
{
    m_host.log(m_plugin, LogLevel::Error, m_host.formatError(error));
}

void HostApi::statusMessage(const QString &text)
{
    if (m_host.bindings().statusMessage)
        m_host.bindings().statusMessage(text);
}

void HostApi::subscribed(const QString &event, int count)
{
    m_host.setListenerCount(m_plugin, event, count);
}

// ---- AudioApi ----

double AudioApi::sampleRate() const
{
    const AudioEngine *audio = m_host.bindings().audio;
    return audio ? audio->sampleRate() : 0.0;
}

int AudioApi::windowFrames() const
{
    return int(m_host.analyzer().windowFrames());
}

QVariantList AudioApi::peak() const
{
    const AudioAnalyzer &a = m_host.analyzer();
    return {double(a.peak(0)), double(a.peak(1))};
}

QVariantList AudioApi::rms() const
{
    const AudioAnalyzer &a = m_host.analyzer();
    return {double(a.rms(0)), double(a.rms(1))};
}

QByteArray AudioApi::pcm() const
{
    const std::vector<float> &w = m_host.analyzer().window();
    return QByteArray(reinterpret_cast<const char *>(w.data()),
                      qsizetype(w.size() * sizeof(float)));
}

QByteArray AudioApi::spectrum(int bins) const
{
    const std::vector<float> &s = m_host.analyzer().spectrum(uint32_t(std::max(bins, 1)));
    return QByteArray(reinterpret_cast<const char *>(s.data()),
                      qsizetype(s.size() * sizeof(float)));
}

QVariant AudioApi::channels() const
{
    const AudioEngine *audio = m_host.bindings().audio;
    if (!audio || !audio->songLoaded())
        return jsNull();
    AudioEngine::PolySnapshot snap;
    audio->polySnapshot(&snap);
    const auto channel = [](const AudioEngine::PolyChannel &c) {
        return QVariant(QVariantMap{{QStringLiteral("on"), c.on},
                                    {QStringLiteral("releasing"), c.releasing},
                                    {QStringLiteral("track"), int(c.track)},
                                    {QStringLiteral("key"), int(c.midiKey)}});
    };
    QVariantList pcm, cgb;
    // The first half of each array is the live pool; the shadow pool
    // (sounds lost to the polyphony limit) is the Polyphony dock's business.
    for (int i = 0; i < std::min<int>(snap.maxPcmChannels, TOTAL_PCM_CHANNELS / 2); i++)
        pcm.append(channel(snap.pcm[i]));
    for (int i = 0; i < TOTAL_CGB_CHANNELS / 2; i++)
        cgb.append(channel(snap.cgb[i]));
    return QVariantMap{{QStringLiteral("pcm"), pcm},
                       {QStringLiteral("cgb"), cgb},
                       {QStringLiteral("maxPcm"), int(snap.maxPcmChannels)},
                       {QStringLiteral("activePcm"), audio->activePcmChannels()},
                       {QStringLiteral("activeCgb"), audio->activeCgbChannels()}};
}

QVariant AudioApi::render(const QString &path, const QVariantMap &opts)
{
    QString error;
    if (!m_host.dialogsAllowed(m_plugin, &error)) {
        throwError(QStringLiteral("audio.render: ") + error);
        return jsNull();
    }
    const QString target = sandboxPath(m_plugin, m_host, path, &error);
    if (target.isEmpty()) {
        throwError(QStringLiteral("audio.render: ") + error);
        return jsNull();
    }
    if (!m_host.bindings().renderWav) {
        throwError(QStringLiteral("audio.render: rendering is not available"));
        return jsNull();
    }
    WavExportOptions options;
    options.sampleRate = clampInt(opts.value(QStringLiteral("sampleRate"), 48000), 8000, 96000);
    options.loopCount = clampInt(opts.value(QStringLiteral("loopCount"), 2), 1, 99);
    options.fadeoutSeconds =
        std::clamp(opts.value(QStringLiteral("fadeout"), 5.0).toDouble(), 0.0, 60.0);
    options.tailSeconds = std::clamp(opts.value(QStringLiteral("tail"), 3.0).toDouble(), 0.0, 60.0);
    if (!std::isfinite(options.fadeoutSeconds))
        options.fadeoutSeconds = 5.0;
    if (!std::isfinite(options.tailSeconds))
        options.tailSeconds = 3.0;
    QDir().mkpath(QFileInfo(target).absolutePath());
    double seconds = 0.0;
    WatchdogPause pause(m_host);
    if (!m_host.bindings().renderWav(target, options, &seconds, &error)) {
        throwError(QStringLiteral("audio.render: ") + error);
        return jsNull();
    }
    return QVariantMap{{QStringLiteral("path"), target}, {QStringLiteral("seconds"), seconds}};
}

QVariantMap AudioApi::engine() const
{
    const auto &read = m_host.bindings().engineSettings;
    return engineSettingsMap(read ? read() : EngineSettings());
}

QVariantMap AudioApi::engineLimits() const
{
    QVariantList rates;
    for (int rate : kGbaMixRates)
        rates.append(rate);
    return {{QStringLiteral("maxPcmChannels"), int(MAX_PCM_CHANNELS)},
            {QStringLiteral("mixRates"), rates}};
}

void AudioApi::setEngine(const QVariantMap &spec)
{
    const auto &read = m_host.bindings().engineSettings;
    if (!read || !m_host.bindings().setEngineSettings) {
        throwError(QStringLiteral("audio.setEngine: engine settings are not available"));
        return;
    }
    EngineSettings next = read();
    for (auto it = spec.cbegin(); it != spec.cend(); ++it) {
        const QString &key = it.key();
        const QVariant &v = it.value();
        // A number that is a whole integer (no NaN/strings/booleans).
        const auto integer = [&](int *out) {
            bool ok = false;
            const double d = v.toDouble(&ok);
            if (!ok || v.typeId() == QMetaType::Bool || v.typeId() == QMetaType::QString ||
                !std::isfinite(d) || d != std::floor(d) || d < 0.0 || d > 1e6)
                return false;
            *out = int(d);
            return true;
        };
        if (key == QLatin1String("maxPcmChannels")) {
            int n = 0;
            if (!integer(&n) || n < 1 || n > int(MAX_PCM_CHANNELS)) {
                throwError(QStringLiteral("audio.setEngine: maxPcmChannels must be an integer "
                                          "from 1 to %1")
                               .arg(MAX_PCM_CHANNELS));
                return;
            }
            next.maxPcmChannels = n;
        } else if (key == QLatin1String("pcmMixRate")) {
            int rate = 0;
            const bool listed =
                integer(&rate) &&
                (rate == 0 || std::find(std::begin(kGbaMixRates), std::end(kGbaMixRates), rate) !=
                                  std::end(kGbaMixRates));
            if (!listed) {
                throwError(QStringLiteral("audio.setEngine: pcmMixRate must be 0 (the host "
                                          "rate) or one of audio.engineLimits().mixRates"));
                return;
            }
            next.pcmMixRate = float(rate);
        } else if (key == QLatin1String("analogFilter")) {
            if (v.typeId() != QMetaType::Bool) {
                throwError(QStringLiteral("audio.setEngine: analogFilter must be a boolean"));
                return;
            }
            next.analogFilter = v.toBool();
        } else {
            throwError(QStringLiteral("audio.setEngine: unknown key '%1'").arg(key));
            return;
        }
    }
    QString error;
    if (!m_host.bindings().setEngineSettings(next, &error))
        throwError(QStringLiteral("audio.setEngine: ") + error);
}

// ---- UiApi ----

QObject *UiApi::dock(const QVariantMap &spec, const QJSValue &build, const QJSValue &paint,
                     const QJSValue &mouse)
{
    QString error;
    DockHandle *handle = createDock(m_host, m_plugin, spec, build, paint, mouse, &error);
    if (!handle) {
        throwError(error);
        return nullptr;
    }
    return handle;
}

namespace {

// The theme roles plugins may read: the general UI palette plus the roll's
// accents, by their theme_roles.h names. Widget-chrome roles stay
// internal (a plugin painting a scrollbar is a plugin painting it wrong).
struct ThemeRoleName {
    const char *name;
    themes::Role role;
};
const ThemeRoleName kThemeRoles[] = {
    {"window_background", themes::Role::window_background},
    {"window_text", themes::Role::window_text},
    {"secondary_text", themes::Role::secondary_text},
    {"disabled_text", themes::Role::disabled_text},
    {"selection_background", themes::Role::selection_background},
    {"selection_text", themes::Role::selection_text},
    {"link_text", themes::Role::link_text},
    {"palette_outline", themes::Role::palette_outline},
    {"item_background", themes::Role::item_background},
    {"item_text", themes::Role::item_text},
    {"item_alternate_background", themes::Role::item_alternate_background},
    {"header_background", themes::Role::header_background},
    {"header_text", themes::Role::header_text},
    {"button_background", themes::Role::button_background},
    {"button_text", themes::Role::button_text},
    {"tooltip_background", themes::Role::tooltip_background},
    {"tooltip_text", themes::Role::tooltip_text},
    {"polyphony_cell_active_background", themes::Role::polyphony_cell_active_background},
    {"polyphony_cell_releasing_background", themes::Role::polyphony_cell_releasing_background},
    {"polyphony_cell_free_background", themes::Role::polyphony_cell_free_background},
    {"polyphony_cell_shadow_background", themes::Role::polyphony_cell_shadow_background},
    {"polyphony_flash_background", themes::Role::polyphony_flash_background},
    {"song_view_piano_roll_background", themes::Role::song_view_piano_roll_background},
    {"song_view_grid", themes::Role::song_view_grid},
    {"song_view_separator", themes::Role::song_view_separator},
    {"song_view_primary_text", themes::Role::song_view_primary_text},
    {"song_view_secondary_text", themes::Role::song_view_secondary_text},
    {"song_view_selection_fill", themes::Role::song_view_selection_fill},
    {"song_view_selection_edge", themes::Role::song_view_selection_edge},
    {"song_view_playhead", themes::Role::song_view_playhead},
    {"song_view_edit_cursor", themes::Role::song_view_edit_cursor},
    {"song_view_loop_marker", themes::Role::song_view_loop_marker},
    {"song_view_automation_default_curve", themes::Role::song_view_automation_default_curve},
    {"song_view_automation_tempo_curve", themes::Role::song_view_automation_tempo_curve},
    {"sample_waveform_ink", themes::Role::sample_waveform_ink},
};

} // namespace

QVariant UiApi::theme(const QString &name) const
{
    if (name.isEmpty()) {
        QVariantMap all;
        for (const ThemeRoleName &r : kThemeRoles)
            all.insert(QLatin1String(r.name), colorToCss(themes::color(r.role)));
        return all;
    }
    for (const ThemeRoleName &r : kThemeRoles) {
        if (name == QLatin1String(r.name))
            return colorToCss(themes::color(r.role));
    }
    throwError(QStringLiteral("ui.theme: unknown role '%1'").arg(name));
    return QVariant();
}

int UiApi::loadImage(const QString &relativePath)
{
    if (m_plugin.dir.isEmpty()) {
        throwError(QStringLiteral("ui.loadImage: this plugin has no folder"));
        return 0;
    }
    const QDir dir(m_plugin.dir);
    const QString base = dir.canonicalPath();
    const QString path = QFileInfo(dir.filePath(relativePath)).canonicalFilePath();
    if (base.isEmpty() || path.isEmpty() ||
        !(path == base || path.startsWith(base + QLatin1Char('/')))) {
        throwError(QStringLiteral("ui.loadImage: '%1' is not a file inside the plugin folder")
                       .arg(relativePath));
        return 0;
    }
    QImageReader reader(path);
    const QImage image = reader.read();
    if (image.isNull()) {
        throwError(QStringLiteral("ui.loadImage: could not decode '%1': %2")
                       .arg(relativePath, reader.errorString()));
        return 0;
    }
    if (m_plugin.images.size() >= 256) {
        throwError(QStringLiteral("ui.loadImage: too many images loaded (free some)"));
        return 0;
    }
    const int id = m_plugin.nextImageId++;
    m_plugin.images.emplace(id, image.convertToFormat(QImage::Format_ARGB32_Premultiplied));
    return id;
}

QVariant UiApi::imageSize(int id) const
{
    const auto it = m_plugin.images.find(id);
    if (it == m_plugin.images.end())
        return jsNull();
    return QVariantMap{{QStringLiteral("width"), it->second.width()},
                       {QStringLiteral("height"), it->second.height()}};
}

void UiApi::freeImage(int id)
{
    m_plugin.images.erase(id);
}

QObject *UiApi::menu()
{
    MenuHandle *menu = m_host.pluginMenu(m_plugin);
    if (!menu) {
        throwError(QStringLiteral("ui.menu: no menu bar is available"));
        return nullptr;
    }
    QJSEngine::setObjectOwnership(menu, QJSEngine::CppOwnership);
    return menu;
}

QObject *UiApi::contextMenu(const QString &surface)
{
    if (surface != QLatin1String("notes") && surface != QLatin1String("range")) {
        throwError(QStringLiteral("ui.contextMenu: surface must be 'notes' or 'range'"));
        return nullptr;
    }
    if (!m_plugin.uiRoot)
        return nullptr;
    auto *menu = new MenuHandle(m_host, m_plugin, surface, m_plugin.uiRoot.get());
    QJSEngine::setObjectOwnership(menu, QJSEngine::CppOwnership);
    return menu;
}

QObject *UiApi::overlay(const QVariantMap &spec, const QJSValue &paint)
{
    const QString id = spec.value(QStringLiteral("id")).toString();
    static const QRegularExpression idRe(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    if (!idRe.match(id).hasMatch()) {
        throwError(QStringLiteral("ui.overlay: id must be 1-64 letters, digits, '_' or '-'"));
        return nullptr;
    }
    if (!paint.isCallable()) {
        throwError(QStringLiteral("ui.overlay: expected a paint(g, v) function"));
        return nullptr;
    }
    for (const QPointer<OverlayHandle> &existing : m_plugin.overlays) {
        if (existing && existing->active() && existing->id() == id) {
            throwError(QStringLiteral("ui.overlay: an overlay with id '%1' exists").arg(id));
            return nullptr;
        }
    }
    if (!m_plugin.uiRoot)
        return nullptr;
    auto *overlay = new OverlayHandle(m_host, m_plugin, id, paint, m_plugin.uiRoot.get());
    QJSEngine::setObjectOwnership(overlay, QJSEngine::CppOwnership);
    m_plugin.overlays.emplace_back(overlay);
    m_host.invalidateOverlays();
    return overlay;
}

bool UiApi::dialogsAllowed(const char *api)
{
    QString error;
    if (m_host.dialogsAllowed(m_plugin, &error))
        return true;
    throwError(QStringLiteral("ui.dialog.%1: %2").arg(QLatin1String(api), error));
    return false;
}

QWidget *UiApi::dialogParent() const
{
    return QApplication::activeWindow();
}

namespace {

QString dialogTitle(const Plugin &plugin, const QVariantMap &opts)
{
    const QString title = opts.value(QStringLiteral("title")).toString();
    if (!title.isEmpty())
        return title;
    return plugin.manifest.name.isEmpty() ? QStringLiteral("porydaw") : plugin.manifest.name;
}

} // namespace

void UiApi::alert(const QString &text, const QVariantMap &opts)
{
    if (!dialogsAllowed("alert"))
        return;
    WatchdogPause pause(m_host);
    QMessageBox box(dialogParent());
    box.setObjectName(QStringLiteral("pluginDialog"));
    box.setWindowTitle(dialogTitle(m_plugin, opts));
    box.setIcon(QMessageBox::Information);
    box.setText(text);
    box.setInformativeText(opts.value(QStringLiteral("detail")).toString());
    box.addButton(opts.value(QStringLiteral("ok"), QStringLiteral("OK")).toString(),
                  QMessageBox::AcceptRole);
    box.exec();
}

bool UiApi::confirm(const QString &text, const QVariantMap &opts)
{
    if (!dialogsAllowed("confirm"))
        return false;
    WatchdogPause pause(m_host);
    QMessageBox box(dialogParent());
    box.setObjectName(QStringLiteral("pluginDialog"));
    box.setWindowTitle(dialogTitle(m_plugin, opts));
    box.setIcon(QMessageBox::Question);
    box.setText(text);
    box.setInformativeText(opts.value(QStringLiteral("detail")).toString());
    QPushButton *ok = box.addButton(
        opts.value(QStringLiteral("ok"), QStringLiteral("OK")).toString(), QMessageBox::AcceptRole);
    QPushButton *cancel =
        box.addButton(opts.value(QStringLiteral("cancel"), QStringLiteral("Cancel")).toString(),
                      QMessageBox::RejectRole);
    box.setDefaultButton(ok);
    box.setEscapeButton(cancel);
    box.exec();
    return box.clickedButton() == ok;
}

QVariant UiApi::prompt(const QString &text, const QVariantMap &opts)
{
    if (!dialogsAllowed("prompt"))
        return jsNull();
    WatchdogPause pause(m_host);
    QInputDialog dialog(dialogParent());
    dialog.setObjectName(QStringLiteral("pluginDialog"));
    dialog.setWindowTitle(dialogTitle(m_plugin, opts));
    dialog.setInputMode(QInputDialog::TextInput);
    dialog.setLabelText(text);
    dialog.setTextValue(opts.value(QStringLiteral("value")).toString());
    dialog.setOkButtonText(opts.value(QStringLiteral("ok"), QStringLiteral("OK")).toString());
    dialog.setCancelButtonText(
        opts.value(QStringLiteral("cancel"), QStringLiteral("Cancel")).toString());
    if (dialog.exec() != QDialog::Accepted)
        return jsNull();
    return dialog.textValue();
}

QVariant UiApi::form(const QVariantMap &spec)
{
    if (!dialogsAllowed("form"))
        return jsNull();
    const QVariantList fields = spec.value(QStringLiteral("fields")).toList();
    if (fields.isEmpty()) {
        throwError(QStringLiteral("ui.dialog.form: spec.fields needs at least one field"));
        return jsNull();
    }
    WatchdogPause pause(m_host);
    QDialog dialog(dialogParent());
    dialog.setObjectName(QStringLiteral("pluginDialog"));
    dialog.setWindowTitle(dialogTitle(m_plugin, spec));
    auto *layout = new QFormLayout(&dialog);
    const QString text = spec.value(QStringLiteral("text")).toString();
    if (!text.isEmpty()) {
        auto *label = new QLabel(text, &dialog);
        label->setWordWrap(true);
        layout->addRow(label);
    }
    struct Field {
        QString key;
        QString type;
        QWidget *widget;
    };
    std::vector<Field> built;
    for (const QVariant &entry : fields) {
        const QVariantMap f = entry.toMap();
        const QString key = f.value(QStringLiteral("key")).toString();
        if (key.isEmpty()) {
            throwError(QStringLiteral("ui.dialog.form: every field needs a key"));
            return jsNull();
        }
        const QString type = f.value(QStringLiteral("type"), QStringLiteral("text")).toString();
        QString label = f.value(QStringLiteral("label")).toString();
        if (label.isEmpty())
            label = key;
        QWidget *widget = nullptr;
        if (type == QLatin1String("text")) {
            auto *edit = new QLineEdit(f.value(QStringLiteral("value")).toString(), &dialog);
            edit->setPlaceholderText(f.value(QStringLiteral("placeholder")).toString());
            widget = edit;
        } else if (type == QLatin1String("number")) {
            const double step = f.value(QStringLiteral("step"), 1.0).toDouble();
            const int decimals = f.value(QStringLiteral("decimals"), 0).toInt();
            const double lo = f.value(QStringLiteral("min"), -1e9).toDouble();
            const double hi = f.value(QStringLiteral("max"), 1e9).toDouble();
            if (decimals > 0 || step != std::floor(step)) {
                auto *box = new QDoubleSpinBox(&dialog);
                box->setDecimals(std::clamp(decimals > 0 ? decimals : 2, 1, 6));
                box->setRange(lo, hi);
                box->setSingleStep(step > 0 ? step : 1.0);
                box->setValue(f.value(QStringLiteral("value"), 0.0).toDouble());
                widget = box;
            } else {
                auto *box = new QSpinBox(&dialog);
                box->setRange(int(std::clamp(lo, -2e9, 2e9)), int(std::clamp(hi, -2e9, 2e9)));
                box->setSingleStep(std::max(1, int(step)));
                box->setValue(f.value(QStringLiteral("value"), 0).toInt());
                widget = box;
            }
        } else if (type == QLatin1String("checkbox")) {
            auto *box = new QCheckBox(&dialog);
            box->setChecked(f.value(QStringLiteral("value")).toBool());
            widget = box;
        } else if (type == QLatin1String("combo")) {
            auto *box = new QComboBox(&dialog);
            for (const QVariant &item : f.value(QStringLiteral("items")).toList())
                box->addItem(item.toString());
            const QVariant value = f.value(QStringLiteral("value"));
            if (value.userType() == QMetaType::QString)
                box->setCurrentText(value.toString());
            else
                box->setCurrentIndex(clampInt(value, 0, std::max(0, box->count() - 1)));
            widget = box;
        } else {
            throwError(
                QStringLiteral("ui.dialog.form: field '%1' has unknown type '%2'").arg(key, type));
            return jsNull();
        }
        widget->setObjectName(QStringLiteral("field.") + key);
        layout->addRow(label, widget);
        built.push_back({key, type, widget});
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)
        ->setText(spec.value(QStringLiteral("ok"), QStringLiteral("OK")).toString());
    buttons->button(QDialogButtonBox::Cancel)
        ->setText(spec.value(QStringLiteral("cancel"), QStringLiteral("Cancel")).toString());
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return jsNull();
    QVariantMap result;
    for (const Field &field : built) {
        if (auto *edit = qobject_cast<QLineEdit *>(field.widget))
            result.insert(field.key, edit->text());
        else if (auto *ibox = qobject_cast<QSpinBox *>(field.widget))
            result.insert(field.key, ibox->value());
        else if (auto *dbox = qobject_cast<QDoubleSpinBox *>(field.widget))
            result.insert(field.key, dbox->value());
        else if (auto *check = qobject_cast<QCheckBox *>(field.widget))
            result.insert(field.key, check->isChecked());
        else if (auto *combo = qobject_cast<QComboBox *>(field.widget))
            result.insert(field.key, combo->currentIndex());
    }
    return result;
}

namespace {

QString startDir(const ScriptHost &host, const Plugin &plugin, const QVariantMap &opts)
{
    const QString dir = opts.value(QStringLiteral("dir")).toString();
    if (!dir.isEmpty())
        return dir;
    const DecompProject *p = host.bindings().project;
    if (p && p->isOpen())
        return p->root();
    return plugin.dir;
}

} // namespace

QVariant UiApi::openFile(const QVariantMap &opts)
{
    if (!dialogsAllowed("openFile"))
        return jsNull();
    WatchdogPause pause(m_host);
    QFileDialog dialog(dialogParent(), dialogTitle(m_plugin, opts),
                       startDir(m_host, m_plugin, opts),
                       opts.value(QStringLiteral("filter")).toString());
    dialog.setObjectName(QStringLiteral("pluginFileDialog"));
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setAcceptMode(QFileDialog::AcceptOpen);
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty())
        return jsNull();
    const QString path = QDir::cleanPath(dialog.selectedFiles().first());
    m_plugin.grantedPaths.append(path);
    return path;
}

QVariant UiApi::saveFile(const QVariantMap &opts)
{
    if (!dialogsAllowed("saveFile"))
        return jsNull();
    WatchdogPause pause(m_host);
    QFileDialog dialog(dialogParent(), dialogTitle(m_plugin, opts),
                       startDir(m_host, m_plugin, opts),
                       opts.value(QStringLiteral("filter")).toString());
    dialog.setObjectName(QStringLiteral("pluginFileDialog"));
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    const QString name = opts.value(QStringLiteral("name")).toString();
    if (!name.isEmpty())
        dialog.selectFile(name);
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty())
        return jsNull();
    const QString path = QDir::cleanPath(dialog.selectedFiles().first());
    m_plugin.grantedPaths.append(path);
    return path;
}

QVariant UiApi::chooseDir(const QVariantMap &opts)
{
    if (!dialogsAllowed("chooseDir"))
        return jsNull();
    WatchdogPause pause(m_host);
    QFileDialog dialog(dialogParent(), dialogTitle(m_plugin, opts),
                       startDir(m_host, m_plugin, opts));
    dialog.setObjectName(QStringLiteral("pluginFileDialog"));
    dialog.setFileMode(QFileDialog::Directory);
    dialog.setOption(QFileDialog::ShowDirsOnly, true);
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty())
        return jsNull();
    const QString path = QDir::cleanPath(dialog.selectedFiles().first());
    m_plugin.grantedPaths.append(path);
    return path;
}

// ---- IoApi ----

QString IoApi::pluginDir() const
{
    return m_plugin.dir;
}

QString IoApi::projectRoot() const
{
    const DecompProject *p = m_host.bindings().project;
    return p && p->isOpen() ? p->root() : QString();
}

QString IoApi::allowed(const QString &path, const char *api)
{
    QString error;
    const QString abs = sandboxPath(m_plugin, m_host, path, &error);
    if (abs.isEmpty())
        throwError(QStringLiteral("io.%1: %2").arg(QLatin1String(api), error));
    return abs;
}

QString IoApi::resolve(const QString &path)
{
    return allowed(path, "resolve");
}

bool IoApi::exists(const QString &path)
{
    const QString abs = allowed(path, "exists");
    return !abs.isEmpty() && QFileInfo::exists(abs);
}

bool IoApi::isDir(const QString &path)
{
    const QString abs = allowed(path, "isDir");
    return !abs.isEmpty() && QFileInfo(abs).isDir();
}

QString IoApi::readText(const QString &path)
{
    return QString::fromUtf8(readBytes(path));
}

void IoApi::writeText(const QString &path, const QString &text)
{
    writeBytes(path, text.toUtf8());
}

QByteArray IoApi::readBytes(const QString &path)
{
    const QString abs = allowed(path, "read");
    if (abs.isEmpty())
        return QByteArray();
    QFile file(abs);
    if (!file.open(QIODevice::ReadOnly)) {
        throwError(QStringLiteral("io.read: could not open %1: %2").arg(abs, file.errorString()));
        return QByteArray();
    }
    return file.readAll();
}

void IoApi::writeBytes(const QString &path, const QByteArray &bytes)
{
    const QString abs = allowed(path, "write");
    if (abs.isEmpty())
        return;
    QDir().mkpath(QFileInfo(abs).absolutePath());
    QSaveFile file(abs);
    if (!file.open(QIODevice::WriteOnly)) {
        throwError(QStringLiteral("io.write: could not open %1: %2").arg(abs, file.errorString()));
        return;
    }
    file.write(bytes);
    if (!file.commit())
        throwError(QStringLiteral("io.write: could not write %1: %2").arg(abs, file.errorString()));
}

QVariantList IoApi::list(const QString &path)
{
    QVariantList out;
    const QString abs = allowed(path, "list");
    if (abs.isEmpty())
        return out;
    const QDir dir(abs);
    if (!dir.exists()) {
        throwError(QStringLiteral("io.list: no such folder: %1").arg(abs));
        return out;
    }
    for (const QFileInfo &info :
         dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        out.append(QVariantMap{{QStringLiteral("name"), info.fileName()},
                               {QStringLiteral("dir"), info.isDir()},
                               {QStringLiteral("size"), double(info.size())}});
    }
    return out;
}

void IoApi::mkdir(const QString &path)
{
    const QString abs = allowed(path, "mkdir");
    if (!abs.isEmpty() && !QDir().mkpath(abs))
        throwError(QStringLiteral("io.mkdir: could not create %1").arg(abs));
}

void IoApi::remove(const QString &path)
{
    const QString abs = allowed(path, "remove");
    if (abs.isEmpty())
        return;
    const QFileInfo info(abs);
    if (info.isDir()) {
        throwError(QStringLiteral("io.remove: %1 is a folder").arg(abs));
        return;
    }
    if (info.exists() && !QFile::remove(abs))
        throwError(QStringLiteral("io.remove: could not remove %1").arg(abs));
}

// ---- SongApi ----

bool SongApi::loaded() const
{
    return doc() != nullptr;
}

double SongApi::revision() const
{
    const SongDocument *d = doc();
    return d ? double(d->revision()) : 0.0;
}

QString SongApi::label() const
{
    const SongDocument *d = doc();
    return d ? d->label() : QString();
}

QString SongApi::midPath() const
{
    const SongDocument *d = doc();
    return d ? d->midPath() : QString();
}

int SongApi::ticksPerBeat() const
{
    const SongDocument *d = doc();
    return d ? int(d->smf().division) : 0;
}

int SongApi::ticksPerClock() const
{
    const SongDocument *d = doc();
    return d ? int(d->ticksPerClock()) : 0;
}

int SongApi::startTempo() const
{
    const SongDocument *d = doc();
    return d ? d->startTempo() : 0;
}

int SongApi::trackCount() const
{
    const SongDocument *d = doc();
    return d ? d->engineTrackCount() : 0;
}

int SongApi::trackBudget() const
{
    const SongDocument *d = doc();
    return d ? d->trackBudget() : 0;
}

double SongApi::endTick() const
{
    const SongDocument *d = doc();
    if (!d)
        return 0.0;
    uint64_t end = 0;
    for (const SmfTrack &track : d->smf().tracks)
        end = std::max(end, track.endTick);
    return double(end);
}

namespace {
QVariantMap cfgToVariant(const SongCfg &cfg); // with the voicegroup helpers below
}

QVariantMap SongApi::settings() const
{
    const SongDocument *d = doc();
    return d ? cfgToVariant(d->cfg()) : QVariantMap();
}

bool SongApi::save()
{
    QString error;
    if (!m_host.dialogsAllowed(m_plugin, &error)) {
        throwError(QStringLiteral("song.save: ") + error);
        return false;
    }
    if (!doc()) {
        throwError(QStringLiteral("song.save: no song is open"));
        return false;
    }
    if (!m_host.bindings().saveSong) {
        throwError(QStringLiteral("song.save: not available"));
        return false;
    }
    // A failed save reports through a message box: that wait is the user's.
    WatchdogPause pause(m_host);
    if (!m_host.bindings().saveSong(&error)) {
        throwError(QStringLiteral("song.save: ") + error);
        return false;
    }
    return true;
}

QVariant SongApi::loop() const
{
    const SongDocument *d = doc();
    if (!d)
        return jsNull();
    const uint64_t start = d->loopTick(false);
    const uint64_t end = d->loopTick(true);
    if (start == UINT64_MAX || end == UINT64_MAX)
        return jsNull();
    return QVariantMap{{QStringLiteral("start"), double(start)},
                       {QStringLiteral("end"), double(end)}};
}

QVariantList SongApi::timeSigs() const
{
    QVariantList out;
    const SongDocument *d = doc();
    if (!d)
        return out;
    for (const DocTimeSig &sig : d->timeSigs()) {
        out.append(QVariantMap{{QStringLiteral("tick"), double(sig.tick)},
                               {QStringLiteral("numerator"), int(sig.numerator)},
                               {QStringLiteral("denominator"), 1 << sig.denomPow2}});
    }
    return out;
}

QVariantList SongApi::tracks() const
{
    QVariantList out;
    const SongDocument *d = doc();
    const SongView *v = view();
    if (!d)
        return out;
    for (int t = 0; t < d->engineTrackCount(); ++t) {
        out.append(QVariantMap{{QStringLiteral("index"), t},
                               {QStringLiteral("name"), d->trackName(t)},
                               {QStringLiteral("chunk"), d->smfTrackFor(t)},
                               {QStringLiteral("channel"), int(d->channelFor(t))},
                               {QStringLiteral("muted"), v && v->trackMuted(t)},
                               {QStringLiteral("soloed"), v && v->trackSoloed(t)},
                               {QStringLiteral("voice"), v ? v->currentProgram(t) : -1}});
    }
    return out;
}

int SongApi::chunkCount() const
{
    const SongDocument *d = doc();
    return d ? int(d->smf().tracks.size()) : 0;
}

int SongApi::chunkTrack(int chunk) const
{
    const SongDocument *d = doc();
    if (!d || chunk < 0 || chunk >= int(d->smf().tracks.size()))
        return -1;
    for (int t = 0; t < d->engineTrackCount(); ++t) {
        if (d->smfTrackFor(t) == chunk)
            return t;
    }
    return -1;
}

double SongApi::chunkEndTick(int chunk) const
{
    const SongDocument *d = doc();
    if (!d || chunk < 0 || chunk >= int(d->smf().tracks.size())) {
        throwError(QStringLiteral("song.chunkEndTick: no such chunk"));
        return 0.0;
    }
    return double(d->smf().tracks[size_t(chunk)].endTick);
}

QVariantList SongApi::rawEvents(int chunk, const QVariantMap &opts) const
{
    QVariantList out;
    const SongDocument *d = doc();
    if (!d || chunk < 0 || chunk >= int(d->smf().tracks.size())) {
        throwError(QStringLiteral("song.rawEvents: no such chunk"));
        return out;
    }
    uint64_t from, to;
    if (!tickRange(opts, &from, &to))
        return out;
    const std::vector<SmfEvent> &events = d->smf().tracks[size_t(chunk)].events;
    for (size_t i = 0; i < events.size(); ++i) {
        if (events[i].tick >= from && events[i].tick < to)
            out.append(rawEventToVariant(i, events[i]));
    }
    return out;
}

QVariantList SongApi::notes(const QVariantMap &opts) const
{
    QVariantList out;
    const SongDocument *d = doc();
    if (!d)
        return out;
    uint64_t from, to;
    if (!tickRange(opts, &from, &to))
        return out;
    int first = 0, last = d->engineTrackCount() - 1;
    if (opts.contains(QStringLiteral("track"))) {
        first = last = opts.value(QStringLiteral("track")).toInt();
        if (first < 0 || first >= d->engineTrackCount())
            return out;
    }
    const bool selectedOnly = opts.value(QStringLiteral("selectedOnly")).toBool();
    const SongView *v = view();
    if (selectedOnly) {
        if (!v)
            return out;
        first = last = v->selectedTrack();
        if (first < 0 || first >= d->engineTrackCount())
            return out;
    }
    for (int t = first; t <= last; ++t) {
        for (const DocNote &note : d->notesForTrack(t)) {
            if (note.tick < from || note.tick >= to)
                continue;
            if (selectedOnly &&
                !v->isSelected(ViewNote{note.noteId, uint32_t(note.tick),
                                        uint32_t(note.tick + note.duration), note.key,
                                        note.velocity, uint8_t(t), note.unterminated()}))
                continue;
            out.append(noteToVariant(note));
        }
    }
    return out;
}

QVariant SongApi::note(double id) const
{
    const SongDocument *d = doc();
    DocNote note;
    if (!d || clampId(id) == 0 || !d->findNote(NoteId(clampId(id)), &note))
        return jsNull();
    return noteToVariant(note);
}

QVariantList SongApi::lanePoints(int track, int cc, const QVariantMap &opts) const
{
    QVariantList out;
    const SongDocument *d = doc();
    if (!d || cc < 0 || cc > 0xFF)
        return out;
    // The tempo lane is song-wide: the document addresses it as track -1.
    if (cc != DOC_CC_TEMPO && (track < 0 || track >= d->engineTrackCount()))
        return out;
    uint64_t from, to;
    if (!tickRange(opts, &from, &to))
        return out;
    for (const DocLanePoint &p : d->lanePoints(cc == DOC_CC_TEMPO ? -1 : track, uint8_t(cc))) {
        if (p.tick < from || p.tick >= to)
            continue;
        out.append(QVariantMap{{QStringLiteral("tick"), double(p.tick)},
                               {QStringLiteral("value"), p.value}});
    }
    return out;
}

// ---- SelectionApi ----

int SelectionApi::track() const
{
    const SongView *v = view();
    return v ? v->selectedTrack() : -1;
}

int SelectionApi::trackMask() const
{
    const SongView *v = view();
    return v ? int(v->trackSelectionMask()) : 0;
}

QVariantList SelectionApi::notes() const
{
    QVariantList out;
    const SongDocument *d = doc();
    const SongView *v = view();
    if (!d || !v)
        return out;
    const int track = v->selectedTrack();
    for (const SongView::NoteKey &key : v->selection()) {
        DocNote note;
        if (d->findNote(track, key.tick, key.key, &note))
            out.append(noteToVariant(note));
    }
    return out;
}

QVariant SelectionApi::time() const
{
    const SongView *v = view();
    if (!v || !v->timeSelection().active())
        return jsNull();
    const SongView::TimeSelection &sel = v->timeSelection();
    QVariantList lanes;
    for (const auto &lane : sel.lanes)
        lanes.append(QVariantMap{{QStringLiteral("track"), lane.first},
                                 {QStringLiteral("cc"), int(lane.second)}});
    return QVariantMap{{QStringLiteral("start"), double(sel.startTick)},
                       {QStringLiteral("end"), double(sel.endTick)},
                       {QStringLiteral("scope"), sel.scope == SongView::TimeSelection::Lanes
                                                     ? QStringLiteral("lanes")
                                                     : QStringLiteral("tracks")},
                       {QStringLiteral("lanes"), lanes}};
}

void SelectionApi::setNotes(const QVariantList &ids)
{
    SongDocument *d = doc();
    SongView *v = view();
    if (!d || !v)
        return;
    std::vector<SongView::NoteKey> keys;
    int track = -1;
    for (const QVariant &id : ids) {
        DocNote note;
        if (clampId(id.toDouble()) == 0 || !d->findNote(NoteId(clampId(id.toDouble())), &note))
            continue;
        if (track < 0)
            track = note.engineTrack;
        if (note.engineTrack != track) {
            throwError(QStringLiteral("selection.setNotes: notes must all be on one track"));
            return;
        }
        keys.push_back({uint32_t(note.tick), note.key});
    }
    if (track >= 0 && track != v->selectedTrack())
        v->selectTrack(track);
    v->setSelection(std::move(keys));
}

void SelectionApi::clear()
{
    if (SongView *v = view())
        v->clearSelection();
}

void SelectionApi::selectTrack(int track)
{
    SongView *v = view();
    const SongDocument *d = doc();
    if (!v || !d)
        return;
    if (track < 0 || track >= d->engineTrackCount()) {
        throwError(QStringLiteral("selection.selectTrack: no such track"));
        return;
    }
    v->selectTrack(track);
}

void SelectionApi::setTime(const QVariantMap &spec)
{
    SongView *v = view();
    const SongDocument *d = doc();
    if (!v || !d)
        return;
    SongView::TimeSelection sel;
    sel.startTick = clampTick(spec.value(QStringLiteral("start")).toDouble());
    sel.endTick = clampTick(spec.value(QStringLiteral("end")).toDouble());
    if (sel.endTick <= sel.startTick) {
        throwError(QStringLiteral("selection.setTime: end must be greater than start"));
        return;
    }
    const QString scope = spec.value(QStringLiteral("scope"), QStringLiteral("tracks")).toString();
    if (scope == QLatin1String("lanes")) {
        sel.scope = SongView::TimeSelection::Lanes;
        for (const QVariant &entry : spec.value(QStringLiteral("lanes")).toList()) {
            const QVariantMap lane = entry.toMap();
            int track = -1;
            const int cc = lane.value(QStringLiteral("cc"), -1).toInt();
            const bool tempo = cc == DOC_CC_TEMPO;
            if (!isLaneCc(cc) || !variantTrack(lane.value(QStringLiteral("track"), -1), &track) ||
                (tempo && track != -1) ||
                (!tempo && (track < 0 || track >= d->engineTrackCount()))) {
                throwError(QStringLiteral("selection.setTime: bad lane"));
                return;
            }
            sel.lanes.emplace_back(track, uint8_t(cc));
        }
        if (sel.lanes.empty()) {
            throwError(QStringLiteral("selection.setTime: lanes scope needs at least one lane"));
            return;
        }
    } else if (scope != QLatin1String("tracks")) {
        throwError(QStringLiteral("selection.setTime: scope must be 'tracks' or 'lanes'"));
        return;
    }
    v->setTimeSelection(sel);
}

void SelectionApi::clearTime()
{
    if (SongView *v = view())
        v->clearTimeSelection();
}

// ---- CursorApi ----

double CursorApi::tick() const
{
    const SongView *v = view();
    return v ? double(v->editCursorTick()) : 0.0;
}

double CursorApi::snap(double tick, const QString &mode) const
{
    const SongView *v = view();
    if (!v)
        return tick;
    tick = double(clampTick(tick));
    if (mode == QLatin1String("down"))
        return double(v->snapTickDown(tick));
    if (mode == QLatin1String("up"))
        return double(v->snapTickUp(tick));
    if (mode != QLatin1String("nearest")) {
        throwError(QStringLiteral("cursor.snap: mode must be 'nearest', 'down' or 'up'"));
        return tick;
    }
    return double(v->snapTick(tick));
}

QVariant CursorApi::grid(double tick) const
{
    const SongView *v = view();
    if (!v)
        return jsNull();
    const SongView::GridSeg seg = v->gridSegAt(clampTick(tick));
    return QVariantMap{{QStringLiteral("start"), double(seg.start)},
                       {QStringLiteral("next"), double(seg.next)},
                       {QStringLiteral("beatTicks"), double(seg.beatTicks)},
                       {QStringLiteral("feel"), v->gridFeel() == SongView::GridFeel::Triplet
                                                    ? QStringLiteral("triplet")
                                                    : QStringLiteral("straight")},
                       {QStringLiteral("minDenom"), v->gridMinDenom()}};
}

void CursorApi::set(double tick)
{
    if (SongView *v = view())
        v->commitEditCursor(clampTick(tick));
}

// ---- TransportApi ----

QString TransportApi::state() const
{
    const AudioEngine *audio = m_host.bindings().audio;
    if (!audio || !audio->songLoaded())
        return QStringLiteral("stopped");
    switch (audio->transport()) {
    case Transport::Playing:
        return QStringLiteral("playing");
    case Transport::Paused:
        return QStringLiteral("paused");
    case Transport::Stopped:
        break;
    }
    return QStringLiteral("stopped");
}

double TransportApi::playheadTick() const
{
    const AudioEngine *audio = m_host.bindings().audio;
    if (audio && audio->songLoaded() && audio->timeline())
        return audio->timeline()->tickForSample(audio->playheadSamples());
    const SongView *v = view();
    return v ? double(v->editCursorTick()) : 0.0;
}

double TransportApi::sampleRate() const
{
    const AudioEngine *audio = m_host.bindings().audio;
    return audio ? audio->sampleRate() : 0.0;
}

bool TransportApi::loopEnabled() const
{
    const AudioEngine *audio = m_host.bindings().audio;
    return audio && audio->loopEnabled();
}

void TransportApi::play()
{
    if (m_host.bindings().play)
        m_host.bindings().play();
}

void TransportApi::pause()
{
    if (m_host.bindings().pause)
        m_host.bindings().pause();
}

void TransportApi::stop()
{
    if (m_host.bindings().stop)
        m_host.bindings().stop();
}

void TransportApi::seek(double tick)
{
    if (m_host.bindings().seekTick)
        m_host.bindings().seekTick(clampTick(tick));
}

// ---- ActionsApi ----

QString ActionsApi::registerAction(const QVariantMap &spec)
{
    const QString id = spec.value(QStringLiteral("id")).toString();
    const QString name = spec.value(QStringLiteral("name")).toString();
    const QString contextText = spec.value(QStringLiteral("context")).toString();
    const QString defaultKeys = spec.value(QStringLiteral("default")).toString();
    keymap::Context context;
    if (contextText == QLatin1String("global"))
        context = keymap::Context::Global;
    else if (contextText == QLatin1String("roll"))
        context = keymap::Context::PianoRoll;
    else if (contextText == QLatin1String("velocity"))
        context = keymap::Context::Velocity;
    else if (contextText == QLatin1String("range"))
        context = keymap::Context::TimeSelection;
    else {
        throwError(QStringLiteral("actions.register: context must be 'global', 'roll', "
                                  "'velocity' or 'range'"));
        return QString();
    }
    QString error;
    const QString fullId = m_host.registerAction(m_plugin, id, name, context, defaultKeys, &error);
    if (fullId.isEmpty())
        throwError(QStringLiteral("actions.register: ") + error);
    return fullId;
}

void ActionsApi::unregister(const QString &fullId)
{
    m_host.unregisterAction(m_plugin, fullId);
}

// ---- StorageApi ----

QJSValue StorageApi::get(const QString &key, const QJSValue &fallback) const
{
    const QSettings settings;
    const QString k = storageKey(m_plugin, key);
    if (!settings.contains(k))
        return fallback;
    const QJsonDocument json = QJsonDocument::fromJson(settings.value(k).toByteArray());
    if (!json.isObject())
        return fallback;
    QJSEngine *e = engine();
    return e ? e->toScriptValue(json.object().value(QLatin1String("v")).toVariant()) : fallback;
}

void StorageApi::set(const QString &key, const QJSValue &value)
{
    if (value.isUndefined()) {
        remove(key);
        return;
    }
    QSettings settings;
    settings.setValue(
        storageKey(m_plugin, key),
        QJsonDocument::fromVariant(QVariantMap{{QStringLiteral("v"), value.toVariant()}})
            .toJson(QJsonDocument::Compact));
}

void StorageApi::remove(const QString &key)
{
    QSettings settings;
    settings.remove(storageKey(m_plugin, key));
}

QStringList StorageApi::keys() const
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("plugins/") + m_plugin.manifest.id +
                        QStringLiteral("/data"));
    return settings.childKeys();
}

bool StorageApi::songStore(QJsonObject *store, QString *path, const char *api) const
{
    const DecompProject *p = m_host.bindings().project;
    const SongDocument *d = doc();
    if (!p || !p->isOpen() || !d || d->label().isEmpty()) {
        throwError(QStringLiteral("storage.song.%1: no song is open in a project")
                       .arg(QLatin1String(api)));
        return false;
    }
    if (d->isLocked()) {
        // A song-bundle tab does not live under the open project: its label
        // must not reach (or clobber) a project song's sidecar.
        throwError(QStringLiteral("storage.song.%1: the song is a read-only song bundle")
                       .arg(QLatin1String(api)));
        return false;
    }
    *path = ViewSidecar::pathFor(p->root(), d->label());
    QJsonObject root;
    QFile file(*path);
    if (file.open(QIODevice::ReadOnly))
        root = QJsonDocument::fromJson(file.readAll()).object();
    *store = root.value(QLatin1String("plugins")).toObject().value(m_plugin.manifest.id).toObject();
    return true;
}

void StorageApi::writeSongStore(const QString &path, const QJsonObject &store)
{
    // Merge: the sidecar also carries the view state and registration
    // metadata, and every writer re-reads before writing.
    QJsonObject root;
    {
        QFile in(path);
        if (in.open(QIODevice::ReadOnly))
            root = QJsonDocument::fromJson(in.readAll()).object();
    }
    QJsonObject plugins = root.value(QLatin1String("plugins")).toObject();
    if (store.isEmpty())
        plugins.remove(m_plugin.manifest.id);
    else
        plugins.insert(m_plugin.manifest.id, store);
    if (plugins.isEmpty())
        root.remove(QLatin1String("plugins"));
    else
        root.insert(QLatin1String("plugins"), plugins);
    const DecompProject *p = m_host.bindings().project;
    if (!p || !Sidecar::ensureDir(p->root())) {
        throwError(QStringLiteral("storage.song: could not create the .porydaw folder"));
        return;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        throwError(QStringLiteral("storage.song: could not write %1").arg(path));
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit())
        throwError(QStringLiteral("storage.song: could not write %1").arg(path));
}

QJSValue StorageApi::songGet(const QString &key, const QJSValue &fallback) const
{
    QJsonObject store;
    QString path;
    if (!songStore(&store, &path, "get") || !store.contains(key))
        return fallback;
    QJSEngine *e = engine();
    return e ? e->toScriptValue(store.value(key).toVariant()) : fallback;
}

void StorageApi::songSet(const QString &key, const QJSValue &value)
{
    if (value.isUndefined()) {
        songRemove(key);
        return;
    }
    QJsonObject store;
    QString path;
    if (!songStore(&store, &path, "set"))
        return;
    store.insert(key, QJsonValue::fromVariant(value.toVariant()));
    writeSongStore(path, store);
}

void StorageApi::songRemove(const QString &key)
{
    QJsonObject store;
    QString path;
    if (!songStore(&store, &path, "remove") || !store.contains(key))
        return;
    store.remove(key);
    writeSongStore(path, store);
}

QStringList StorageApi::songKeys() const
{
    QJsonObject store;
    QString path;
    if (!songStore(&store, &path, "keys"))
        return {};
    return store.keys();
}

// ---- settings / voicegroup value helpers ----

namespace {

const VgMacro kVgMacros[] = {
    VgMacro::DirectSound,    VgMacro::DirectSoundNoResample,
    VgMacro::DirectSoundAlt, VgMacro::Square1,
    VgMacro::Square1Alt,     VgMacro::Square2,
    VgMacro::Square2Alt,     VgMacro::ProgWave,
    VgMacro::ProgWaveAlt,    VgMacro::Noise,
    VgMacro::NoiseAlt,       VgMacro::Keysplit,
    VgMacro::KeysplitAll,
};

// "voice_directsound" or "directsound" → the macro.
bool parseVgMacro(const QString &text, VgMacro *out)
{
    QString word = text.trimmed().toLower();
    if (!word.startsWith(QLatin1String("voice_")))
        word.prepend(QLatin1String("voice_"));
    for (VgMacro macro : kVgMacros) {
        if (vgMacroName(macro) == word) {
            *out = macro;
            return true;
        }
    }
    return false;
}

QString vgLineKindName(VgLineKind kind)
{
    switch (kind) {
    case VgLineKind::Editable:
        return QStringLiteral("voice");
    case VgLineKind::ReadOnlyVoice:
        return QStringLiteral("cry");
    case VgLineKind::Broken:
        return QStringLiteral("broken");
    case VgLineKind::None:
        return QStringLiteral("empty");
    case VgLineKind::Other:
    case VgLineKind::Header:
        break;
    }
    return QStringLiteral("other");
}

QVariantMap voiceToVariant(int slot, const VgVoice &v)
{
    return QVariantMap{{QStringLiteral("slot"), slot},
                       {QStringLiteral("kind"), QStringLiteral("voice")},
                       {QStringLiteral("type"), vgMacroName(v.macro)},
                       {QStringLiteral("key"), v.key},
                       {QStringLiteral("pan"), v.pan},
                       {QStringLiteral("symbol"), v.symbol},
                       {QStringLiteral("keysplitTable"), v.keysplitTable},
                       {QStringLiteral("sweep"), v.sweep},
                       {QStringLiteral("duty"), v.duty},
                       {QStringLiteral("period"), v.period},
                       {QStringLiteral("attack"), v.attack},
                       {QStringLiteral("decay"), v.decay},
                       {QStringLiteral("sustain"), v.sustain},
                       {QStringLiteral("release"), v.release}};
}

QVariantMap slotToVariant(const VoicegroupSource &source, int slot)
{
    if (const VgVoice *v = source.voiceAt(slot))
        return voiceToVariant(slot, *v);
    return QVariantMap{{QStringLiteral("slot"), slot},
                       {QStringLiteral("kind"), vgLineKindName(source.kindAt(slot))}};
}

QVariantMap adsrToVariant(const VgAdsr &a)
{
    return QVariantMap{{QStringLiteral("attack"), a.attack},
                       {QStringLiteral("decay"), a.decay},
                       {QStringLiteral("sustain"), a.sustain},
                       {QStringLiteral("release"), a.release}};
}

// Applies a partial voice spec over `voice`; false with *error on a bad
// key or value. Numbers are clamped to the macro's own ranges.
bool applyVoiceSpec(const QVariantMap &spec, VgVoice *voice, QString *error)
{
    static const QStringList kKeys{
        QStringLiteral("type"),   QStringLiteral("key"),           QStringLiteral("pan"),
        QStringLiteral("symbol"), QStringLiteral("keysplitTable"), QStringLiteral("sweep"),
        QStringLiteral("duty"),   QStringLiteral("period"),        QStringLiteral("attack"),
        QStringLiteral("decay"),  QStringLiteral("sustain"),       QStringLiteral("release"),
        QStringLiteral("slot"),   QStringLiteral("kind")};
    for (auto it = spec.cbegin(); it != spec.cend(); ++it) {
        if (!kKeys.contains(it.key())) {
            *error = QStringLiteral("unknown voice field '%1'").arg(it.key());
            return false;
        }
    }
    const auto number = [&](const char *key, int lo, int hi, int *out) {
        const auto it = spec.constFind(QLatin1String(key));
        if (it == spec.cend())
            return true;
        bool ok = false;
        const double v = it->toDouble(&ok);
        if (!ok || !std::isfinite(v)) {
            *error = QStringLiteral("'%1' must be a number").arg(QLatin1String(key));
            return false;
        }
        *out = std::clamp(int(std::lround(v)), lo, hi);
        return true;
    };
    const auto text = [&](const char *key, QString *out) {
        const auto it = spec.constFind(QLatin1String(key));
        if (it == spec.cend())
            return true;
        if (it->type() != QVariant::String) {
            *error = QStringLiteral("'%1' must be a string").arg(QLatin1String(key));
            return false;
        }
        *out = it->toString().trimmed();
        return true;
    };
    QString typeName;
    if (!text("type", &typeName))
        return false;
    if (spec.contains(QStringLiteral("type")) && !parseVgMacro(typeName, &voice->macro)) {
        *error = QStringLiteral("unknown voice type '%1'").arg(typeName);
        return false;
    }
    const bool cgb = vgMacroIsCgb(voice->macro);
    if (!number("key", 0, 127, &voice->key) || !number("pan", -128, 127, &voice->pan) ||
        !text("symbol", &voice->symbol) || !text("keysplitTable", &voice->keysplitTable) ||
        !number("sweep", 0, 255, &voice->sweep) || !number("duty", 0, 3, &voice->duty) ||
        !number("period", 0, 1, &voice->period) ||
        !number("attack", 0, cgb ? 7 : 255, &voice->attack) ||
        !number("decay", 0, cgb ? 7 : 255, &voice->decay) ||
        !number("sustain", 0, cgb ? 15 : 255, &voice->sustain) ||
        !number("release", 0, cgb ? 7 : 255, &voice->release)) {
        return false;
    }
    // Fields the resulting macro doesn't write would vanish silently
    // (renderLine only emits the macro's own arguments): refuse them.
    const VgMacro m = voice->macro;
    const bool split = m == VgMacro::Keysplit || m == VgMacro::KeysplitAll;
    const bool square1 = m == VgMacro::Square1 || m == VgMacro::Square1Alt;
    const bool square = square1 || m == VgMacro::Square2 || m == VgMacro::Square2Alt;
    const bool noise = m == VgMacro::Noise || m == VgMacro::NoiseAlt;
    const auto applies = [&](const QString &key) {
        if (key == QLatin1String("keysplitTable"))
            return m == VgMacro::Keysplit;
        if (key == QLatin1String("symbol"))
            return split || vgMacroHasSymbol(m);
        if (key == QLatin1String("sweep"))
            return square1;
        if (key == QLatin1String("duty"))
            return square;
        if (key == QLatin1String("period"))
            return noise;
        if (key == QLatin1String("key") || key == QLatin1String("pan") ||
            key == QLatin1String("attack") || key == QLatin1String("decay") ||
            key == QLatin1String("sustain") || key == QLatin1String("release"))
            return !split;
        return true; // type / slot / kind
    };
    for (auto it = spec.cbegin(); it != spec.cend(); ++it) {
        if (!applies(it.key())) {
            *error = QStringLiteral("'%1' does not apply to %2").arg(it.key(), vgMacroName(m));
            return false;
        }
    }
    const bool wantsSymbol = vgMacroHasSymbol(voice->macro) || split;
    if (wantsSymbol && voice->symbol.isEmpty()) {
        *error = QStringLiteral("%1 needs a symbol").arg(vgMacroName(voice->macro));
        return false;
    }
    if (voice->macro == VgMacro::Keysplit && voice->keysplitTable.isEmpty()) {
        *error = QStringLiteral("voice_keysplit needs a keysplitTable");
        return false;
    }
    return true;
}

QVariantMap cfgToVariant(const SongCfg &cfg)
{
    return QVariantMap{
        {QStringLiteral("voicegroup"), cfg.voicegroupArg},
        {QStringLiteral("voicegroupName"), SongRegistry::voicegroupDisplayName(cfg.voicegroupArg)},
        {QStringLiteral("masterVolume"), cfg.masterVolume},
        {QStringLiteral("reverb"), cfg.reverb < 0 ? jsNull() : QVariant(cfg.reverb)},
        {QStringLiteral("priority"), cfg.priority},
        {QStringLiteral("exactGate"), cfg.exactGate},
        {QStringLiteral("extendedClocks"), cfg.extendedClocks},
        {QStringLiteral("noCompression"), cfg.noCompression},
        {QStringLiteral("flags"), cfg.rawFlags}};
}

// Applies a partial settings spec over `cfg`; knownArgs (the project's
// voicegroups) resolves a display name and refuses unknown voicegroups.
bool applySettingsSpec(const QVariantMap &spec, const QStringList &knownArgs, SongCfg *cfg,
                       QString *error)
{
    for (auto it = spec.cbegin(); it != spec.cend(); ++it) {
        const QString key = it.key();
        const QVariant &v = it.value();
        const auto intValue = [&](int lo, int hi, int *out) {
            bool ok = false;
            const double d = v.toDouble(&ok);
            if (!ok || !std::isfinite(d)) {
                *error = QStringLiteral("'%1' must be a number").arg(key);
                return false;
            }
            *out = std::clamp(int(std::lround(d)), lo, hi);
            return true;
        };
        const auto boolValue = [&](bool *out) {
            if (v.type() != QVariant::Bool) {
                *error = QStringLiteral("'%1' must be a boolean").arg(key);
                return false;
            }
            *out = v.toBool();
            return true;
        };
        if (key == QLatin1String("voicegroup")) {
            if (v.type() != QVariant::String) {
                *error = QStringLiteral("'voicegroup' must be a string");
                return false;
            }
            const QString arg =
                SongRegistry::voicegroupArgFromDisplay(v.toString().trimmed(), knownArgs);
            if (arg.isEmpty() || (!knownArgs.isEmpty() && !knownArgs.contains(arg))) {
                *error = QStringLiteral("unknown voicegroup '%1'").arg(v.toString());
                return false;
            }
            cfg->voicegroupArg = arg;
        } else if (key == QLatin1String("masterVolume")) {
            if (!intValue(0, 127, &cfg->masterVolume))
                return false;
        } else if (key == QLatin1String("reverb")) {
            // JS null drops the flag; undefined (an invalid QVariant) is a
            // mistake like any other non-number.
            if (v.isValid() && v.isNull()) {
                cfg->reverb = -1;
            } else if (!intValue(0, 127, &cfg->reverb)) {
                return false;
            }
        } else if (key == QLatin1String("priority")) {
            if (!intValue(0, 255, &cfg->priority))
                return false;
        } else if (key == QLatin1String("exactGate")) {
            if (!boolValue(&cfg->exactGate))
                return false;
        } else if (key == QLatin1String("extendedClocks")) {
            if (!boolValue(&cfg->extendedClocks))
                return false;
        } else if (key == QLatin1String("noCompression")) {
            if (!boolValue(&cfg->noCompression))
                return false;
        } else {
            *error = QStringLiteral("unknown setting '%1'").arg(key);
            return false;
        }
    }
    return true;
}

QVariantMap songInfoToVariant(const SongInfo &song)
{
    return QVariantMap{{QStringLiteral("id"), song.id},
                       {QStringLiteral("label"), song.label},
                       {QStringLiteral("constant"), song.constant},
                       {QStringLiteral("player"), song.player},
                       {QStringLiteral("midPath"), song.midPath},
                       {QStringLiteral("hasMid"), song.hasMid},
                       {QStringLiteral("registered"), song.registered},
                       {QStringLiteral("registrationGaps"), song.registrationGaps},
                       {QStringLiteral("settings"), cfgToVariant(song.cfg)}};
}

const SongInfo *findProjectSong(const DecompProject *p, const QString &label)
{
    if (!p || !p->isOpen())
        return nullptr;
    for (const SongInfo &song : p->songs()) {
        if (song.label == label)
            return &song;
    }
    return nullptr;
}

} // namespace

// ---- ProjectApi ----

bool ProjectApi::isOpen() const
{
    const DecompProject *p = m_host.bindings().project;
    return p && p->isOpen();
}

QString ProjectApi::root() const
{
    const DecompProject *p = m_host.bindings().project;
    return p && p->isOpen() ? p->root() : QString();
}

QVariantList ProjectApi::songs() const
{
    QVariantList out;
    const DecompProject *p = m_host.bindings().project;
    if (!p || !p->isOpen())
        return out;
    for (const SongInfo &song : p->songs())
        out.append(songInfoToVariant(song));
    return out;
}

QVariant ProjectApi::song(const QString &label) const
{
    const SongInfo *info = findProjectSong(m_host.bindings().project, label);
    return info ? QVariant(songInfoToVariant(*info)) : jsNull();
}

QVariantMap ProjectApi::registration(const QString &label) const
{
    const DecompProject *p = m_host.bindings().project;
    const SongInfo *info = findProjectSong(p, label);
    if (!info) {
        throwError(QStringLiteral("project.registration: no song named '%1'").arg(label));
        return {};
    }
    const QString constant =
        info->constant.isEmpty() ? SongRegistry::constantForLabel(label) : info->constant;
    const RegistrationStatus st = SongRegistry::checkRegistration(p->root(), label, constant);
    return QVariantMap{{QStringLiteral("complete"), st.complete()},
                       {QStringLiteral("inSongTable"), st.inSongTable},
                       {QStringLiteral("inSongsH"), st.inSongsH},
                       {QStringLiteral("inLdScript"), st.inLdScript},
                       {QStringLiteral("inCharmap"), st.inCharmap},
                       {QStringLiteral("inDebugMenu"), st.inDebugMenu},
                       {QStringLiteral("gaps"), SongRegistry::registrationGaps(st)}};
}

bool ProjectApi::writeAllowed(const char *api)
{
    QString error;
    if (!m_host.dialogsAllowed(m_plugin, &error)) {
        throwError(QStringLiteral("project.%1: %2").arg(QLatin1String(api), error));
        return false;
    }
    const DecompProject *p = m_host.bindings().project;
    if (!p || !p->isOpen()) {
        throwError(QStringLiteral("project.%1: no project is open").arg(QLatin1String(api)));
        return false;
    }
    return true;
}

int ProjectApi::registerSong(const QString &label, const QString &constant, const QString &player)
{
    if (!writeAllowed("registerSong"))
        return -1;
    if (!m_host.bindings().registerSong) {
        throwError(QStringLiteral("project.registerSong: not available"));
        return -1;
    }
    int songId = -1;
    QString error;
    WatchdogPause pause(m_host);
    if (!m_host.bindings().registerSong(label, constant, player, &songId, &error)) {
        throwError(QStringLiteral("project.registerSong: ") + error);
        return -1;
    }
    return songId;
}

void ProjectApi::unregisterSong(const QString &label)
{
    if (!writeAllowed("unregisterSong"))
        return;
    if (!m_host.bindings().unregisterSong) {
        throwError(QStringLiteral("project.unregisterSong: not available"));
        return;
    }
    QString error;
    WatchdogPause pause(m_host);
    if (!m_host.bindings().unregisterSong(label, &error))
        throwError(QStringLiteral("project.unregisterSong: ") + error);
}

void ProjectApi::reload()
{
    if (!writeAllowed("reload"))
        return;
    if (!m_host.bindings().reloadProject) {
        throwError(QStringLiteral("project.reload: not available"));
        return;
    }
    QString error;
    WatchdogPause pause(m_host);
    if (!m_host.bindings().reloadProject(&error))
        throwError(QStringLiteral("project.reload: ") + error);
}

QVariantList ProjectApi::musicPlayers() const
{
    QVariantList out;
    const DecompProject *p = m_host.bindings().project;
    if (!p || !p->isOpen())
        return out;
    for (const MusicPlayer &player : SongRegistry::musicPlayers(p->root())) {
        out.append(QVariantMap{{QStringLiteral("name"), player.name},
                               {QStringLiteral("number"), player.number},
                               {QStringLiteral("trackCount"), player.trackCount}});
    }
    return out;
}

QVariantList ProjectApi::voicegroups() const
{
    QVariantList out;
    if (!m_host.bindings().voicegroupCatalog)
        return out;
    for (const QString &arg : m_host.bindings().voicegroupCatalog().groupArgs) {
        out.append(QVariantMap{{QStringLiteral("arg"), arg},
                               {QStringLiteral("name"), SongRegistry::voicegroupDisplayName(arg)}});
    }
    return out;
}

QString ProjectApi::createVoicegroup(const QString &name, const QString &copyFromArg)
{
    if (!writeAllowed("createVoicegroup"))
        return {};
    if (!m_host.bindings().createVoicegroup) {
        throwError(QStringLiteral("project.createVoicegroup: not available"));
        return {};
    }
    QString error;
    WatchdogPause pause(m_host);
    if (!m_host.bindings().createVoicegroup(name, copyFromArg, &error)) {
        throwError(QStringLiteral("project.createVoicegroup: ") + error);
        return {};
    }
    return QStringLiteral("_") + name;
}

QVariant ProjectApi::exportBundle(const QString &label, const QString &path)
{
    if (!writeAllowed("exportBundle"))
        return jsNull();
    // Like the menu action, a bare name gets the suffix; any other suffix is
    // refused, so the result always reads back through importBundle and a
    // stray path cannot replace a project file with a zip.
    QString bundlePath = path;
    const QString suffix = QFileInfo(bundlePath).suffix();
    if (suffix.isEmpty() && !bundlePath.isEmpty()) {
        bundlePath += QStringLiteral(".porysong");
    } else if (suffix.compare(QLatin1String("porysong"), Qt::CaseInsensitive) != 0) {
        throwError(QStringLiteral("project.exportBundle: %1 is not a .porysong path").arg(path));
        return jsNull();
    }
    QString error;
    const QString target = sandboxPath(m_plugin, m_host, bundlePath, &error);
    if (target.isEmpty()) {
        throwError(QStringLiteral("project.exportBundle: ") + error);
        return jsNull();
    }
    if (!m_host.bindings().exportBundle) {
        throwError(QStringLiteral("project.exportBundle: not available"));
        return jsNull();
    }
    QDir().mkpath(QFileInfo(target).absolutePath());
    int samples = 0;
    WatchdogPause pause(m_host);
    if (!m_host.bindings().exportBundle(label, target, &samples, &error)) {
        throwError(QStringLiteral("project.exportBundle: ") + error);
        return jsNull();
    }
    return QVariantMap{{QStringLiteral("path"), target}, {QStringLiteral("samples"), samples}};
}

QVariant ProjectApi::importBundle(const QString &path, const QString &label,
                                  const QString &constant, const QString &player)
{
    if (!writeAllowed("importBundle"))
        return jsNull();
    QString error;
    const QString source = sandboxPath(m_plugin, m_host, path, &error);
    if (source.isEmpty()) {
        throwError(QStringLiteral("project.importBundle: ") + error);
        return jsNull();
    }
    if (!m_host.bindings().importBundle) {
        throwError(QStringLiteral("project.importBundle: not available"));
        return jsNull();
    }
    BundleImportResult result;
    // Opening the imported song may ask about nothing (it is a new tab),
    // but planning hashes samples and apply rewrites project files.
    WatchdogPause pause(m_host);
    if (!m_host.bindings().importBundle(source, {label, constant, player}, &result, &error)) {
        throwError(QStringLiteral("project.importBundle: ") + error);
        return jsNull();
    }
    return QVariantMap{{QStringLiteral("label"), result.label},
                       {QStringLiteral("constant"), result.constant},
                       {QStringLiteral("player"), result.player},
                       {QStringLiteral("voicegroup"), result.voicegroupArg},
                       {QStringLiteral("warnings"), result.warnings}};
}

// ---- VoicegroupApi ----

namespace {
const VoicegroupSource *sessionVoicegroup(const SongSession *s)
{
    return s ? s->vgSource.get() : nullptr;
}
} // namespace

bool VoicegroupApi::isOpen() const
{
    return sessionVoicegroup(session()) != nullptr;
}

QString VoicegroupApi::arg() const
{
    const SongDocument *d = doc();
    return d ? d->cfg().voicegroupArg : QString();
}

QString VoicegroupApi::name() const
{
    const SongDocument *d = doc();
    return d ? SongRegistry::voicegroupDisplayName(d->cfg().voicegroupArg) : QString();
}

QString VoicegroupApi::file() const
{
    const VoicegroupSource *vg = sessionVoicegroup(session());
    return vg ? vg->filePath() : QString();
}

QString VoicegroupApi::loadName() const
{
    const VoicegroupSource *vg = sessionVoicegroup(session());
    return vg ? vg->loadName() : QString();
}

bool VoicegroupApi::dirty() const
{
    const VoicegroupSource *vg = sessionVoicegroup(session());
    return vg && vg->dirty();
}

bool VoicegroupApi::monolithic() const
{
    const VoicegroupSource *vg = sessionVoicegroup(session());
    return vg && vg->monolithic();
}

QVariantList VoicegroupApi::voices() const
{
    QVariantList out;
    const VoicegroupSource *vg = sessionVoicegroup(session());
    if (!vg)
        return out;
    for (int slot = 0; slot < VOICEGROUP_SIZE; ++slot)
        out.append(slotToVariant(*vg, slot));
    return out;
}

QVariant VoicegroupApi::voice(int slot) const
{
    const VoicegroupSource *vg = sessionVoicegroup(session());
    if (!vg || slot < 0 || slot >= VOICEGROUP_SIZE)
        return jsNull();
    return slotToVariant(*vg, slot);
}

QVariantMap VoicegroupApi::symbols() const
{
    QVariantMap out;
    if (!m_host.bindings().voicegroupCatalog)
        return out;
    const VoicegroupCatalog c = m_host.bindings().voicegroupCatalog();
    QVariantList keysplits;
    for (const auto &pair : c.keysplits) {
        keysplits.append(QVariantMap{{QStringLiteral("voicegroup"), pair.first},
                                     {QStringLiteral("table"), pair.second}});
    }
    out.insert(QStringLiteral("directSound"), c.directSound);
    out.insert(QStringLiteral("progWave"), c.progWave);
    out.insert(QStringLiteral("drumkits"), c.drumkits);
    out.insert(QStringLiteral("synths"), c.synths);
    out.insert(QStringLiteral("keysplits"), keysplits);
    return out;
}

QVariantMap VoicegroupApi::typicalAdsr(const QString &type, const QString &symbol) const
{
    VgMacro macro = VgMacro::DirectSound;
    if (!parseVgMacro(type, &macro)) {
        throwError(QStringLiteral("voicegroup.typicalAdsr: unknown voice type '%1'").arg(type));
        return {};
    }
    VgAdsr adsr;
    if (m_host.bindings().typicalAdsr)
        m_host.bindings().typicalAdsr(macro, symbol, &adsr);
    else
        adsr = vgDefaultAdsr(VgAdsrDefaults(), macro, symbol);
    return adsrToVariant(adsr);
}

bool ProjectApi::open(const QString &label, bool newTab)
{
    QString error;
    if (!m_host.dialogsAllowed(m_plugin, &error)) {
        // Swapping the session under an open transaction (or mid-undo)
        // would pull its document away.
        throwError(QStringLiteral("project.open: ") + error);
        return false;
    }
    if (!m_host.bindings().openSong)
        return false;
    // Already the active song: nothing to do (loadSong would reload it
    // from disk, asking about unsaved edits). A read-only bundle tab can
    // share the label without being the project's song.
    if (!newTab && doc() && !doc()->isLocked() && doc()->label() == label)
        return true;
    // Replacing a dirty tab asks the user first: that wait is theirs.
    WatchdogPause pause(m_host);
    return m_host.bindings().openSong(label, newTab);
}

// ---- EditApi ----

bool EditApi::active() const
{
    const EditTransaction &tx = m_host.transaction();
    return tx.open() && tx.owner == &m_plugin;
}

void EditApi::begin(const QString &name)
{
    QString error;
    if (!m_host.beginTransaction(m_plugin, name, &error))
        throwError(QStringLiteral("edit.transaction: ") + error);
}

void EditApi::commit()
{
    QString error;
    if (!m_host.commitTransaction(m_plugin, &error))
        throwError(QStringLiteral("edit.transaction: ") + error);
}

void EditApi::rollback()
{
    m_host.rollbackTransaction(m_plugin);
}

SongDocument *EditApi::begin()
{
    QString error;
    SongDocument *d = m_host.transactionDocument(m_plugin, &error);
    if (!d)
        throwError(QStringLiteral("porydaw.edit: ") + error);
    return d;
}

void EditApi::done()
{
    m_host.transactionEdited();
}

// One sweep of the document, not one findNote per id (which rebuilds
// every track's note list each time).
std::vector<DocNote> EditApi::resolveNotes(const SongDocument *d, const QVariantList &ids) const
{
    std::vector<DocNote> notes;
    if (ids.isEmpty())
        return notes;
    std::map<uint64_t, DocNote> byId;
    for (int t = 0; t < d->engineTrackCount(); ++t) {
        for (const DocNote &note : d->notesForTrack(t))
            byId.emplace(note.noteId.token(), note);
    }
    for (const QVariant &id : ids) {
        const auto it = byId.find(clampId(id.toDouble()));
        if (it != byId.end())
            notes.push_back(it->second);
    }
    return notes;
}

bool EditApi::checkTrack(const SongDocument *d, int track, const char *api)
{
    if (track >= 0 && track < d->engineTrackCount())
        return true;
    throwError(QStringLiteral("edit.%1: no such track").arg(QLatin1String(api)));
    return false;
}

int EditApi::laneTrack(const SongDocument *d, int track, int cc, const char *api)
{
    if (!isLaneCc(cc)) {
        throwError(QStringLiteral("edit.%1: cc must be 0-127 or a porydaw.song.CC value")
                       .arg(QLatin1String(api)));
        return -2;
    }
    if (cc == DOC_CC_TEMPO)
        return -1;
    return checkTrack(d, track, api) ? track : -2;
}

QVariantList EditApi::addNotes(int track, const QVariantList &notes)
{
    QVariantList ids;
    SongDocument *d = begin();
    if (!d)
        return ids;
    if (!checkTrack(d, track, "addNotes")) {
        done();
        return ids;
    }
    std::vector<SongDocument::NewNote> batch;
    for (const QVariant &entry : notes) {
        if (entry.userType() != QMetaType::QVariantMap) {
            throwError(QStringLiteral("edit.addNotes: each note must be an object "
                                      "{tick, key, len, vel}"));
            done();
            return QVariantList();
        }
        const QVariantMap n = entry.toMap();
        SongDocument::NewNote note;
        note.tick = clampTick(n.value(QStringLiteral("tick")).toDouble());
        note.key = uint8_t(clampInt(n.value(QStringLiteral("key")), 0, 127));
        note.duration = uint32_t(clampInt(n.value(QStringLiteral("len"), 1), 1, INT32_MAX));
        note.velocity = uint8_t(clampInt(n.value(QStringLiteral("vel"), 127), 1, 127));
        batch.push_back(note);
    }
    // Two entries on one (tick, key) can't both exist (the pairing rule);
    // the later one wins and the earlier reports id 0.
    std::vector<bool> shadowed(batch.size(), false);
    std::vector<SongDocument::NewNote> written;
    for (size_t i = 0; i < batch.size(); ++i) {
        for (size_t j = i + 1; j < batch.size() && !shadowed[i]; ++j)
            shadowed[i] = batch[j].tick == batch[i].tick && batch[j].key == batch[i].key;
        if (!shadowed[i])
            written.push_back(batch[i]);
    }
    if (!written.empty())
        d->addNotes(track, written);
    std::map<std::pair<uint64_t, uint8_t>, double> minted;
    for (const DocNote &note : d->notesForTrack(track))
        minted.emplace(std::make_pair(note.tick, note.key), double(note.noteId.token()));
    for (size_t i = 0; i < batch.size(); ++i) {
        const auto it = minted.find({batch[i].tick, batch[i].key});
        ids.append(!shadowed[i] && it != minted.end() ? it->second : 0.0);
    }
    done();
    return ids;
}

int EditApi::deleteNotes(const QVariantList &ids)
{
    SongDocument *d = begin();
    if (!d)
        return 0;
    const std::vector<DocNote> notes = resolveNotes(d, ids);
    if (!notes.empty())
        d->deleteNotes(notes);
    done();
    return int(notes.size());
}

int EditApi::moveNotes(const QVariantList &ids, double dTick, int dKey)
{
    SongDocument *d = begin();
    if (!d)
        return 0;
    const std::vector<DocNote> notes = resolveNotes(d, ids);
    if (!notes.empty())
        d->moveNotes(notes, clampDelta(dTick), std::clamp(dKey, -127, 127));
    done();
    return int(notes.size());
}

int EditApi::resizeNotes(const QVariantList &ids, double dLen, bool fromLeft)
{
    SongDocument *d = begin();
    if (!d)
        return 0;
    const std::vector<DocNote> notes = resolveNotes(d, ids);
    if (!notes.empty()) {
        if (fromLeft)
            d->resizeNotesLeft(notes, -clampDelta(dLen));
        else
            d->resizeNotes(notes, clampDelta(dLen));
    }
    done();
    return int(notes.size());
}

int EditApi::setVelocities(const QVariantList &pairs)
{
    SongDocument *d = begin();
    if (!d)
        return 0;
    QVariantList ids;
    for (const QVariant &entry : pairs)
        ids.append(entry.toMap().value(QStringLiteral("id")));
    const std::vector<DocNote> known = resolveNotes(d, ids);
    std::vector<NoteVelocity> velocities;
    for (const QVariant &entry : pairs) {
        const QVariantMap pair = entry.toMap();
        const uint64_t token = clampId(pair.value(QStringLiteral("id")).toDouble());
        const bool exists = std::any_of(known.begin(), known.end(), [token](const DocNote &n) {
            return n.noteId.token() == token;
        });
        if (!exists)
            continue;
        velocities.push_back(
            {NoteId(token), uint8_t(clampInt(pair.value(QStringLiteral("vel")), 1, 127))});
    }
    if (!velocities.empty())
        d->setNotesVelocities(d->revision(), velocities);
    done();
    return int(velocities.size());
}

int EditApi::nudgeVelocity(const QVariantList &ids, int delta)
{
    SongDocument *d = begin();
    if (!d)
        return 0;
    const std::vector<DocNote> notes = resolveNotes(d, ids);
    if (!notes.empty())
        d->nudgeNotesVelocity(notes, std::clamp(delta, -127, 127));
    done();
    return int(notes.size());
}

void EditApi::addLanePoint(int track, int cc, double tick, int value)
{
    SongDocument *d = begin();
    if (!d)
        return;
    const int engineTrack = laneTrack(d, track, cc, "addLanePoint");
    if (engineTrack != -2)
        d->addLanePoint(engineTrack, uint8_t(cc), clampTick(tick), clampLaneValue(cc, value));
    done();
}

void EditApi::writeLanePoints(int track, int cc, double from, double to, const QVariantList &points)
{
    SongDocument *d = begin();
    if (!d)
        return;
    const int engineTrack = laneTrack(d, track, cc, "writeLanePoints");
    if (engineTrack == -2) {
        done();
        return;
    }
    if (cc == DOC_CC_VOICE) {
        throwError(QStringLiteral("edit.writeLanePoints: use addLanePoint for the voice lane"));
        done();
        return;
    }
    const uint64_t begin = clampTick(from);
    const uint64_t end = clampTick(to);
    std::vector<SongDocument::LanePointValue> values;
    for (const QVariant &entry : points) {
        const QVariantMap p = entry.toMap();
        const uint64_t tick = clampTick(p.value(QStringLiteral("tick")).toDouble());
        if (tick < begin || tick > end)
            continue;
        values.push_back({tick, clampLaneValue(cc, p.value(QStringLiteral("value")))});
    }
    if (begin <= end)
        d->writeLanePoints(engineTrack, uint8_t(cc), begin, end, values);
    done();
}

int EditApi::moveLanePoints(int track, int cc, const QVariantList &moves)
{
    SongDocument *d = begin();
    if (!d)
        return 0;
    const int engineTrack = laneTrack(d, track, cc, "moveLanePoints");
    std::vector<SongDocument::LanePointMove> batch;
    if (engineTrack != -2) {
        for (const QVariant &entry : moves) {
            const QVariantMap m = entry.toMap();
            SongDocument::LanePointMove move;
            move.engineTrack = engineTrack;
            move.cc = uint8_t(cc);
            const uint64_t tick = clampTick(m.value(QStringLiteral("tick")).toDouble());
            if (!d->findLanePoint(engineTrack, uint8_t(cc), tick, &move.point))
                continue;
            move.newTick = m.contains(QStringLiteral("newTick"))
                               ? clampTick(m.value(QStringLiteral("newTick")).toDouble())
                               : tick;
            move.newValue = m.contains(QStringLiteral("newValue"))
                                ? clampLaneValue(cc, m.value(QStringLiteral("newValue")))
                                : move.point.value;
            batch.push_back(move);
        }
        if (!batch.empty())
            d->moveLanePoints(batch);
    }
    done();
    return int(batch.size());
}

int EditApi::deleteLanePoints(int track, int cc, const QVariantList &ticks)
{
    SongDocument *d = begin();
    if (!d)
        return 0;
    const int engineTrack = laneTrack(d, track, cc, "deleteLanePoints");
    std::vector<DocLanePoint> points;
    if (engineTrack != -2) {
        for (const QVariant &tick : ticks) {
            DocLanePoint p;
            if (d->findLanePoint(engineTrack, uint8_t(cc), clampTick(tick.toDouble()), &p))
                points.push_back(p);
        }
        if (!points.empty())
            d->deleteLanePoints(engineTrack, uint8_t(cc), points);
    }
    done();
    return int(points.size());
}

void EditApi::setSettings(const QVariantMap &spec)
{
    SongDocument *d = begin();
    if (!d)
        return;
    SongCfg cfg = d->cfg();
    QStringList knownArgs;
    if (m_host.bindings().voicegroupCatalog)
        knownArgs = m_host.bindings().voicegroupCatalog().groupArgs;
    QString error;
    if (!applySettingsSpec(spec, knownArgs, &cfg, &error))
        throwError(QStringLiteral("edit.setSettings: ") + error);
    else
        d->setCfg(cfg); // no-op when nothing changed semantically
    done();
}

void EditApi::setVoice(int slot, const QVariantMap &spec)
{
    SongDocument *d = begin();
    if (!d)
        return;
    SongSession *s = session();
    const VoicegroupSource *vg = s ? s->vgSource.get() : nullptr;
    QString error;
    if (!vg) {
        throwError(QStringLiteral("edit.setVoice: the song's voicegroup is not open for editing"));
    } else if (slot < 0 || slot >= VOICEGROUP_SIZE || !vg->isEditable(slot)) {
        throwError(QStringLiteral("edit.setVoice: slot %1 is not an editable voice").arg(slot));
    } else if (!m_host.bindings().editVoice) {
        throwError(QStringLiteral("edit.setVoice: not available"));
    } else {
        const VgVoice before = *vg->voiceAt(slot);
        VgVoice voice = before;
        if (!applyVoiceSpec(spec, &voice, &error)) {
            throwError(QStringLiteral("edit.setVoice: ") + error);
        } else {
            // A type change into another envelope family starts from the
            // project-typical envelope, as the dock does (the old family's
            // digits would be a nonsense envelope on the new scale); the
            // spec's own envelope keys then overlay it. Whatever the
            // source, the values must fit the new family's scale.
            const int family = vgAdsrFamily(voice.macro);
            if (family >= 0 && family != vgAdsrFamily(before.macro) &&
                m_host.bindings().typicalAdsr) {
                VgAdsr adsr;
                m_host.bindings().typicalAdsr(voice.macro, voice.symbol, &adsr);
                voice = before;
                voice.attack = adsr.attack;
                voice.decay = adsr.decay;
                voice.sustain = adsr.sustain;
                voice.release = adsr.release;
                applyVoiceSpec(spec, &voice, &error); // validated above
            }
            const bool cgb = vgMacroIsCgb(voice.macro);
            voice.attack = std::clamp(voice.attack, 0, cgb ? 7 : 255);
            voice.decay = std::clamp(voice.decay, 0, cgb ? 7 : 255);
            voice.sustain = std::clamp(voice.sustain, 0, cgb ? 15 : 255);
            voice.release = std::clamp(voice.release, 0, cgb ? 7 : 255);
            if (!m_host.bindings().editVoice(*s, slot, voice, &error))
                throwError(QStringLiteral("edit.setVoice: ") + error);
        }
    }
    done();
}

void EditApi::setStartTempo(int bpm)
{
    SongDocument *d = begin();
    if (!d)
        return;
    d->setStartTempo(std::clamp(bpm, SongDocument::kTempoMin, SongDocument::kTempoMax));
    done();
}

void EditApi::setLoop(const QJSValue &start, const QJSValue &end)
{
    SongDocument *d = begin();
    if (!d)
        return;
    const auto marker = [](const QJSValue &v) -> int64_t {
        if (v.isNull() || v.isUndefined())
            return -1;
        return int64_t(clampTick(v.toNumber()));
    };
    d->setLoopTick(false, marker(start));
    d->setLoopTick(true, marker(end));
    done();
}

void EditApi::setTimeSig(double tick, int numerator, int denominator)
{
    SongDocument *d = begin();
    if (!d)
        return;
    int pow2 = -1;
    for (int p = 0; p <= 7; ++p) {
        if (denominator == (1 << p))
            pow2 = p;
    }
    if (pow2 < 0 || numerator < 1 || numerator > 255) {
        throwError(QStringLiteral("edit.setTimeSig: numerator must be 1-255 and the denominator "
                                  "a power of two up to 128"));
    } else {
        d->setTimeSig(clampTick(tick), numerator, pow2);
    }
    done();
}

void EditApi::deleteTimeSig(double tick)
{
    SongDocument *d = begin();
    if (!d)
        return;
    d->deleteTimeSig(clampTick(tick));
    done();
}

namespace {

bool parseScope(const SongDocument *d, const QVariantMap &spec, RippleScope *scope, QString *error)
{
    scope->wholeSong = spec.value(QStringLiteral("wholeSong")).toBool();
    for (const QVariant &t : spec.value(QStringLiteral("tracks")).toList()) {
        int track = -1;
        if (!variantTrack(t, &track) || track < 0 || track >= d->engineTrackCount()) {
            *error = QStringLiteral("scope.tracks names a track that does not exist");
            return false;
        }
        scope->tracks.push_back(track);
    }
    for (const QVariant &entry : spec.value(QStringLiteral("lanes")).toList()) {
        const QVariantMap lane = entry.toMap();
        int track = -1;
        const int cc = lane.value(QStringLiteral("cc"), -1).toInt();
        const bool tempo = cc == DOC_CC_TEMPO;
        if (!isLaneCc(cc) ||
            (!tempo && (!variantTrack(lane.value(QStringLiteral("track")), &track) || track < 0 ||
                        track >= d->engineTrackCount()))) {
            *error = QStringLiteral("scope.lanes has a bad lane");
            return false;
        }
        scope->lanes.emplace_back(tempo ? -1 : track, uint8_t(cc));
    }
    if (!scope->wholeSong && scope->tracks.empty() && scope->lanes.empty()) {
        *error = QStringLiteral("scope needs tracks, lanes, or wholeSong: true");
        return false;
    }
    return true;
}

} // namespace

bool EditApi::removeTimeRange(double start, double end, const QVariantMap &scopeSpec)
{
    SongDocument *d = begin();
    if (!d)
        return false;
    RippleScope scope;
    QString error;
    bool changed = false;
    if (!parseScope(d, scopeSpec, &scope, &error))
        throwError(QStringLiteral("edit.removeTimeRange: ") + error);
    else if (clampTick(end) > clampTick(start))
        changed = d->removeTimeRange(clampTick(start), clampTick(end), scope);
    done();
    return changed;
}

bool EditApi::insertTimeRange(double at, double span, const QVariantMap &scopeSpec)
{
    SongDocument *d = begin();
    if (!d)
        return false;
    RippleScope scope;
    QString error;
    bool changed = false;
    if (!parseScope(d, scopeSpec, &scope, &error))
        throwError(QStringLiteral("edit.insertTimeRange: ") + error);
    else if (clampTick(span) > 0)
        changed = d->insertTimeRange(clampTick(at), clampTick(span), scope);
    done();
    return changed;
}

int EditApi::addTrack(int voice)
{
    SongDocument *d = begin();
    if (!d)
        return -1;
    const int index = d->canAddTrack() ? d->addTrack(std::clamp(voice, 0, 127)) : -1;
    done();
    return index;
}

int EditApi::duplicateTrack(int track)
{
    SongDocument *d = begin();
    if (!d)
        return -1;
    int index = -1;
    if (checkTrack(d, track, "duplicateTrack"))
        index = d->duplicateTrack(track);
    done();
    return index;
}

void EditApi::deleteTrack(int track)
{
    SongDocument *d = begin();
    if (!d)
        return;
    if (checkTrack(d, track, "deleteTrack"))
        d->deleteTrack(track);
    done();
}

bool EditApi::mergeTrack(int track, int target, const QVariantMap &options)
{
    SongDocument *d = begin();
    if (!d)
        return false;
    bool merged = false;
    if (checkTrack(d, track, "mergeTrack") && checkTrack(d, target, "mergeTrack")) {
        if (track == target)
            throwError(QStringLiteral("edit.mergeTrack: a track cannot be merged into itself"));
        else
            merged =
                d->mergeTrack(track, target, options.value(QStringLiteral("notesOnly")).toBool());
    }
    done();
    return merged;
}

bool EditApi::moveTrack(int track, int target)
{
    SongDocument *d = begin();
    if (!d)
        return false;
    bool moved = false;
    if (checkTrack(d, track, "moveTrack") && checkTrack(d, target, "moveTrack"))
        moved = d->moveTrack(track, target);
    done();
    return moved;
}

void EditApi::renameTrack(int track, const QString &name)
{
    SongDocument *d = begin();
    if (!d)
        return;
    if (checkTrack(d, track, "renameTrack")) {
        if (nameIsLoopMarker(name))
            throwError(QStringLiteral("edit.renameTrack: mid2agb would read that name as a "
                                      "loop marker"));
        else
            d->renameTrack(track, name);
    }
    done();
}

bool EditApi::transposeSelection(int dKey)
{
    SongDocument *d = begin();
    if (!d)
        return false;
    SongView *v = view();
    const bool moved = v && v->transposeSelection(std::clamp(dKey, -127, 127), false);
    done();
    return moved;
}

bool EditApi::checkChunk(const SongDocument *d, int chunk, const char *api)
{
    if (chunk >= 0 && chunk < int(d->smf().tracks.size()))
        return true;
    throwError(QStringLiteral("edit.%1: no such chunk").arg(QLatin1String(api)));
    return false;
}

bool EditApi::gatherRange(const SongDocument *d, double start, double end,
                          const QVariantMap &scopeSpec, const char *api,
                          std::vector<DocNote> *notes, std::vector<DocLanePoint> *points)
{
    RippleScope scope;
    QString error;
    if (!parseScope(d, scopeSpec, &scope, &error)) {
        throwError(QStringLiteral("edit.%1: %2").arg(QLatin1String(api), error));
        return false;
    }
    const uint64_t s = clampTick(start);
    const uint64_t e = clampTick(end);
    if (e <= s) {
        throwError(QStringLiteral("edit.%1: end must be after start").arg(QLatin1String(api)));
        return false;
    }
    std::vector<int> tracks = scope.tracks;
    if (scope.wholeSong) {
        tracks.clear();
        for (int t = 0; t < d->engineTrackCount(); ++t)
            tracks.push_back(t);
    }
    const auto gatherLane = [&](int track, uint8_t cc) {
        for (const DocLanePoint &pt : d->lanePoints(track < 0 ? 0 : track, cc)) {
            if (pt.tick >= s && pt.tick < e)
                points->push_back(pt);
        }
    };
    // Every lane of a scoped track, hidden ones and the voice changes
    // included — what the time selection's own move takes. One pass over
    // the chunk finds which lanes exist; only those are gathered.
    for (int t : tracks) {
        for (const DocNote &note : d->notesForTrack(t)) {
            if (note.tick >= s && note.tick < e)
                notes->push_back(note);
        }
        const int chunk = d->smfTrackFor(t);
        if (chunk < 0 || chunk >= int(d->smf().tracks.size()))
            continue;
        std::set<uint8_t> ccs;
        for (const SmfEvent &ev : d->smf().tracks[size_t(chunk)].events) {
            if (!ev.isChannel())
                continue;
            if (ev.typeNibble() == 0xB)
                ccs.insert(ev.data0);
            else if (ev.typeNibble() == 0xE)
                ccs.insert(DOC_CC_BEND);
            else if (ev.typeNibble() == 0xC)
                ccs.insert(DOC_CC_VOICE);
        }
        for (uint8_t cc : ccs)
            gatherLane(t, cc);
    }
    if (scope.wholeSong)
        gatherLane(-1, DOC_CC_TEMPO);
    for (const std::pair<int, uint8_t> &lane : scope.lanes)
        gatherLane(lane.first, lane.second);
    // A lane named twice (tracks + lanes) must not move twice.
    std::sort(points->begin(), points->end(), [](const DocLanePoint &a, const DocLanePoint &b) {
        return std::tie(a.smfTrack, a.index) < std::tie(b.smfTrack, b.index);
    });
    points->erase(std::unique(points->begin(), points->end(),
                              [](const DocLanePoint &a, const DocLanePoint &b) {
                                  return a.smfTrack == b.smfTrack && a.index == b.index;
                              }),
                  points->end());
    return true;
}

int EditApi::moveRange(double start, double end, const QVariantMap &scope, double dTick)
{
    SongDocument *d = begin();
    if (!d)
        return 0;
    std::vector<DocNote> notes;
    std::vector<DocLanePoint> points;
    int count = 0;
    if (gatherRange(d, start, end, scope, "moveRange", &notes, &points)) {
        // Floored at the range start, like the time selection's own move:
        // the document clamps events one by one, which would smear the
        // range's internal spacing against tick 0.
        const int64_t delta = std::max(clampDelta(dTick), -int64_t(clampTick(start)));
        if (delta != 0 && (!notes.empty() || !points.empty())) {
            d->moveRange(notes, points, delta);
            count = int(notes.size() + points.size());
        }
    }
    done();
    return count;
}

int EditApi::duplicateRange(double start, double end, const QVariantMap &scope, double dTick)
{
    SongDocument *d = begin();
    if (!d)
        return 0;
    std::vector<DocNote> notes;
    std::vector<DocLanePoint> points;
    int count = 0;
    if (gatherRange(d, start, end, scope, "duplicateRange", &notes, &points)) {
        const int64_t delta = std::max(clampDelta(dTick), -int64_t(clampTick(start)));
        if (delta != 0 && (!notes.empty() || !points.empty())) {
            d->duplicateRange(notes, points, delta);
            count = int(notes.size() + points.size());
        }
    }
    done();
    return count;
}

void EditApi::insertRawEvent(int chunk, const QVariantMap &spec)
{
    SongDocument *d = begin();
    if (!d)
        return;
    SmfEvent ev;
    QString error;
    if (!checkChunk(d, chunk, "insertRawEvent")) {
    } else if (!rawEventFromVariant(spec, &ev, &error)) {
        throwError(QStringLiteral("edit.insertRawEvent: ") + error);
    } else {
        d->insertRawEvent(chunk, ev);
    }
    done();
}

void EditApi::modifyRawEvent(int chunk, int index, const QVariantMap &spec)
{
    SongDocument *d = begin();
    if (!d)
        return;
    SmfEvent ev;
    QString error;
    if (!checkChunk(d, chunk, "modifyRawEvent")) {
    } else if (index < 0 || index >= int(d->smf().tracks[size_t(chunk)].events.size())) {
        throwError(QStringLiteral("edit.modifyRawEvent: no such event"));
    } else if (!rawEventFromVariant(spec, &ev, &error)) {
        throwError(QStringLiteral("edit.modifyRawEvent: ") + error);
    } else {
        d->modifyRawEvent(chunk, size_t(index), ev);
    }
    done();
}

int EditApi::deleteRawEvents(int chunk, const QVariantList &indices)
{
    SongDocument *d = begin();
    if (!d)
        return 0;
    int count = 0;
    if (checkChunk(d, chunk, "deleteRawEvents")) {
        const size_t size = d->smf().tracks[size_t(chunk)].events.size();
        std::set<size_t> unique;
        for (const QVariant &v : indices) {
            const double i = v.toDouble();
            if (std::isfinite(i) && i >= 0.0 && i < double(size))
                unique.insert(size_t(i));
        }
        if (!unique.empty()) {
            d->deleteRawEvents(chunk, std::vector<size_t>(unique.begin(), unique.end()));
            count = int(unique.size());
        }
    }
    done();
    return count;
}

bool EditApi::moveRawEvent(int chunk, int index, int destIndex)
{
    SongDocument *d = begin();
    if (!d)
        return false;
    bool moved = false;
    size_t first = 0, last = 0;
    if (!checkChunk(d, chunk, "moveRawEvent")) {
    } else if (index < 0 || !d->rawEventMoveBounds(chunk, size_t(index), &first, &last)) {
        throwError(QStringLiteral("edit.moveRawEvent: no such event"));
    } else {
        const size_t dest = size_t(std::clamp<int64_t>(destIndex, int64_t(first), int64_t(last)));
        if (dest != size_t(index)) {
            d->moveRawEvent(chunk, size_t(index), dest);
            moved = true;
        }
    }
    done();
    return moved;
}

void EditApi::setChunkEndTick(int chunk, double tick)
{
    SongDocument *d = begin();
    if (!d)
        return;
    if (checkChunk(d, chunk, "setChunkEndTick"))
        d->setTrackEndTick(chunk, clampTick(tick));
    done();
}

bool EditApi::nudgeSelection(bool right)
{
    SongDocument *d = begin();
    if (!d)
        return false;
    SongView *v = view();
    const bool moved = v && v->nudgeSelection(right, false);
    done();
    return moved;
}

// ---- ViewApi ----

bool ViewApi::velocityLane() const
{
    const SongView *v = view();
    return v && v->velocityLaneVisible();
}

void ViewApi::setVelocityLane(bool on)
{
    if (SongView *v = view())
        v->setVelocityLaneVisible(on);
}

bool ViewApi::automationLanes() const
{
    const SongView *v = view();
    return v && v->automationLanesVisible();
}

void ViewApi::setAutomationLanes(bool on)
{
    if (SongView *v = view())
        v->setAutomationLanesVisible(on);
}

bool ViewApi::tempoLane() const
{
    const SongView *v = view();
    return v && v->tempoLaneVisible();
}

void ViewApi::setTempoLane(bool on)
{
    if (SongView *v = view())
        v->setTempoLaneVisible(on);
}

bool ViewApi::eventList() const
{
    const SongView *v = view();
    return v && v->eventListVisible();
}

void ViewApi::setEventList(bool on)
{
    if (SongView *v = view())
        v->setEventListVisible(on);
}

double ViewApi::pxPerBeat() const
{
    const SongView *v = view();
    return v ? v->pxPerBeat() : 0.0;
}

double ViewApi::keyHeight() const
{
    const SongView *v = view();
    return v ? v->keyHeight() : 0.0;
}

QVariant ViewApi::visibleTicks() const
{
    const SongView *v = view();
    if (!v)
        return jsNull();
    uint64_t from, to;
    v->visibleTickRange(&from, &to);
    return QVariantMap{{QStringLiteral("from"), double(from)}, {QStringLiteral("to"), double(to)}};
}

void ViewApi::revealTick(double tick)
{
    if (SongView *v = view())
        v->ensureTickVisible(clampTick(tick));
}

void ViewApi::revealRange(double from, double to)
{
    SongView *v = view();
    if (!v)
        return;
    const uint64_t a = clampTick(from);
    const uint64_t b = clampTick(to);
    v->ensureRangeVisible(std::min(a, b), std::max(a, b), false);
}

bool ViewApi::revealNote(double id)
{
    SongView *v = view();
    const SongDocument *d = doc();
    DocNote note;
    if (!v || !d || clampId(id) == 0 || !d->findNote(NoteId(clampId(id)), &note))
        return false;
    return v->revealNote(note.engineTrack, note.key, note.tick);
}

void ViewApi::revealKey(int key)
{
    if (SongView *v = view())
        v->ensureKeyVisible(std::clamp(key, 0, 127));
}

// ---- installApi ----

bool installApi(ScriptHost &host, Plugin &plugin, QString *error)
{
    QJSEngine *engine = plugin.engine.get();
    QFile preludeFile(QStringLiteral(":/scripting/prelude.js"));
    if (!preludeFile.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("scripting prelude resource missing");
        return false;
    }
    const QJSValue factory = engine->evaluate(QString::fromUtf8(preludeFile.readAll()),
                                              QStringLiteral("porydaw:prelude.js"));
    if (factory.isError() || !factory.isCallable()) {
        *error = QStringLiteral("scripting prelude failed: ") + factory.toString();
        return false;
    }

    QJSValueList facades;
    const auto add = [&](ApiObject *object) {
        plugin.facades.emplace_back(object);
        QJSEngine::setObjectOwnership(object, QJSEngine::CppOwnership);
        facades.append(engine->newQObject(object));
    };
    add(new HostApi(host, plugin));
    add(new SongApi(host, plugin));
    add(new SelectionApi(host, plugin));
    add(new CursorApi(host, plugin));
    add(new TransportApi(host, plugin));
    add(new ActionsApi(host, plugin));
    add(new StorageApi(host, plugin));
    add(new ProjectApi(host, plugin));
    add(new EditApi(host, plugin));
    add(new ViewApi(host, plugin));
    add(new AudioApi(host, plugin));
    add(new UiApi(host, plugin));
    add(new IoApi(host, plugin));
    add(new VoicegroupApi(host, plugin));

    const QJSValue result = factory.call(facades);
    if (result.isError() || !result.isObject()) {
        *error = QStringLiteral("scripting prelude failed: ") + result.toString();
        return false;
    }
    plugin.dispatch = result.property(QStringLiteral("dispatch"));
    plugin.runAction = result.property(QStringLiteral("runAction"));
    engine->globalObject().setProperty(QStringLiteral("porydaw"),
                                       result.property(QStringLiteral("porydaw")));
    engine->globalObject().setProperty(QStringLiteral("console"),
                                       result.property(QStringLiteral("console")));
    return true;
}

} // namespace scripting
