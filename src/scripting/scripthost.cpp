#include "scripthost.h"

#include <QAction>
#include <QCoreApplication>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJSEngine>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>

#include "audio/audioengine.h"
#include "core/miditimeline.h"
#include "core/songdocument.h"
#include "scriptapi.h"
#include "scriptmenus.h"
#include "songsession.h"
#include "ui/songview.h"

namespace scripting {

namespace {

constexpr int kReloadDebounceMs = 250;
constexpr int kRescanDebounceMs = 300;
// 60 Hz is 16.6 ms; the main window's playhead timer uses the same figure.
constexpr int kFrameIntervalMs = 17;
// song.changed feedback loop: a listener that edits in reaction to more
// than this many consecutive deliveries is faulted.
constexpr int kChangedEditStreakCap = 8;
// Bits of ScriptHost::m_pendingOrigins, in precedence order for the
// delivered `origin`: a turn a user edit landed in is "user" even if
// scripts or the undo stack also published.
enum ChangeOrigin { OriginHistory = 1, OriginScript = 2, OriginUser = 4 };
const QString kConsoleId = QStringLiteral("console");
const QString kPluginsDirKey = QStringLiteral("pluginsDir");

QString enabledKey(const QString &id)
{
    return QStringLiteral("plugins/") + id + QStringLiteral("/enabled");
}

} // namespace

// ---- Watchdog ----

Watchdog::~Watchdog()
{
    {
        QMutexLocker lock(&m_mutex);
        m_quit = true;
        m_wake.wakeAll();
    }
    wait();
}

void Watchdog::arm(QJSEngine *engine, int budgetMs)
{
    QMutexLocker lock(&m_mutex);
    m_stack.push_back({engine, QDeadlineTimer(budgetMs), false, false});
    if (!isRunning())
        start();
    m_wake.wakeAll();
}

void Watchdog::pause()
{
    QMutexLocker lock(&m_mutex);
    if (!m_stack.empty())
        m_stack.back().paused = true;
}

void Watchdog::resume(int budgetMs)
{
    QMutexLocker lock(&m_mutex);
    if (m_stack.empty())
        return;
    Armed &top = m_stack.back();
    top.paused = false;
    top.deadline = QDeadlineTimer(budgetMs);
    m_wake.wakeAll();
}

bool Watchdog::disarm()
{
    QMutexLocker lock(&m_mutex);
    if (m_stack.empty())
        return false;
    const bool fired = m_stack.back().fired;
    m_stack.pop_back();
    return fired;
}

void Watchdog::run()
{
    QMutexLocker lock(&m_mutex);
    while (!m_quit) {
        // Idle (nothing armed) costs nothing: sleep until arm() wakes us.
        if (m_stack.empty()) {
            m_wake.wait(&m_mutex);
            continue;
        }
        m_wake.wait(&m_mutex, 10);
        if (m_quit || m_stack.empty())
            continue;
        // Engine pointers are only read under the lock, and disarm() pops
        // an entry before its engine can be destroyed, so the interrupt
        // never touches a dead engine.
        Armed &top = m_stack.back();
        if (!top.paused && !top.fired && top.deadline.hasExpired()) {
            top.engine->setInterrupted(true);
            top.fired = true;
        }
    }
}

// ---- ScriptHost ----

ScriptHost::ScriptHost(QObject *parent) : QObject(parent), m_pluginsDir(resolvePluginsDir())
{
    m_watcher = new QFileSystemWatcher(this);
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, &ScriptHost::onPathChanged);
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, &ScriptHost::onPathChanged);
    m_rescanTimer = new QTimer(this);
    m_rescanTimer->setSingleShot(true);
    m_rescanTimer->setInterval(kRescanDebounceMs);
    connect(m_rescanTimer, &QTimer::timeout, this, [this] {
        scan();
        for (auto &plugin : m_plugins) {
            if (plugin->state == PluginState::Disabled && plugin->enabled && !plugin->engine)
                load(*plugin);
        }
        emit pluginsChanged();
    });
    SongView::setPluginKeyHandler([this](QKeyEvent *event, keymap::Context surface, bool timeSel) {
        return handleKey(event, surface, timeSel);
    });
    SongView::setPluginMenuProvider(
        [this](QMenu &menu, const QString &surface) { appendContextMenu(menu, surface); });
    SongView::setPluginOverlayPainter(
        [this](QPainter &painter, SongView &view, const RollOverlayGeometry &geometry) {
            paintOverlays(painter, view, geometry);
        });
    m_frameTimer = new QTimer(this);
    m_frameTimer->setTimerType(Qt::PreciseTimer);
    m_frameTimer->setInterval(kFrameIntervalMs);
    connect(m_frameTimer, &QTimer::timeout, this, &ScriptHost::pumpFrame);
    m_changedTimer = new QTimer(this);
    m_changedTimer->setSingleShot(true);
    m_changedTimer->setInterval(0);
    connect(m_changedTimer, &QTimer::timeout, this, &ScriptHost::fireChanged);
}

ScriptHost::~ScriptHost()
{
    SongView::setPluginKeyHandler(nullptr);
    SongView::setPluginMenuProvider(nullptr);
    SongView::setPluginOverlayPainter(nullptr);
    unloadAll();
    if (m_console)
        teardown(*m_console);
}

void ScriptHost::setBindings(HostBindings bindings)
{
    m_bindings = std::move(bindings);
}

QString ScriptHost::defaultPluginsDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
           QStringLiteral("/plugins");
}

QString ScriptHost::environmentPluginsDir()
{
    return QString::fromLocal8Bit(qgetenv("PORYDAW_PLUGINS_DIR"));
}

QString ScriptHost::configuredPluginsDir()
{
    // A relative (hand-edited, corrupt) value would resolve against the
    // launch directory and scatter folders around; only absolute paths count.
    const QString saved = QSettings().value(kPluginsDirKey).toString();
    if (saved.isEmpty() || !QDir::isAbsolutePath(saved))
        return defaultPluginsDir();
    return QDir::cleanPath(saved);
}

QString ScriptHost::resolvePluginsDir()
{
    const QString env = environmentPluginsDir();
    return env.isEmpty() ? configuredPluginsDir() : env;
}

void ScriptHost::setPluginsDirSetting(const QString &dir)
{
    // Normalised once here so "the default spelled differently" (trailing
    // slash, "." segment) clears the key rather than storing it.
    const QString clean = dir.isEmpty() ? QString() : QDir::cleanPath(dir);
    QSettings settings;
    if (clean.isEmpty() || clean == QDir::cleanPath(defaultPluginsDir()))
        settings.remove(kPluginsDirKey);
    else
        settings.setValue(kPluginsDirKey, clean);
    if (environmentPluginsDir().isEmpty())
        setPluginsDir(configuredPluginsDir());
}

void ScriptHost::setPluginsDir(const QString &dir)
{
    if (dir == m_pluginsDir)
        return;
    unloadAll();
    m_pluginsDir = dir;
}

bool ScriptHost::enabledSetting(const QString &id)
{
    return QSettings().value(enabledKey(id), true).toBool();
}

void ScriptHost::scan()
{
    QDir root(m_pluginsDir);
    if (!root.exists())
        QDir().mkpath(m_pluginsDir);
    if (root.exists() && !m_watcher->directories().contains(m_pluginsDir))
        m_watcher->addPath(m_pluginsDir);

    const QStringList dirs = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    // Plugins whose directory (or manifest — a folder without plugin.json
    // is not a plugin) vanished go away entirely.
    for (auto it = m_plugins.begin(); it != m_plugins.end();) {
        const QString dir = (*it)->dir;
        if (dirs.contains(QFileInfo(dir).fileName()) &&
            QFile::exists(dir + QStringLiteral("/plugin.json"))) {
            ++it;
        } else if ((*it)->callDepth > 0) {
            // A call has the engine on the stack (a listener's dialog, say):
            // teardown defers to guarded()'s unwind, which then erases it.
            teardown(**it);
            (*it)->removePending = true;
            ++it;
        } else {
            teardown(**it);
            unwatch(**it);
            (*it)->reloadTimer->deleteLater();
            it = m_plugins.erase(it);
        }
    }
    for (const QString &name : dirs) {
        const QString dir = root.filePath(name);
        if (!QFile::exists(dir + QStringLiteral("/plugin.json")))
            continue;
        Plugin *plugin = nullptr;
        for (auto &p : m_plugins) {
            if (p->dir == dir) {
                plugin = p.get();
                plugin->removePending = false; // the folder is back
            }
        }
        if (!plugin) {
            m_plugins.push_back(std::make_unique<Plugin>());
            plugin = m_plugins.back().get();
            plugin->dir = dir;
            plugin->reloadTimer = new QTimer(this);
            plugin->reloadTimer->setSingleShot(true);
            plugin->reloadTimer->setInterval(kReloadDebounceMs);
            connect(plugin->reloadTimer, &QTimer::timeout, this, [this, dir] {
                for (auto &p : m_plugins) {
                    if (p->dir == dir) {
                        reload(p->manifest.id);
                        return;
                    }
                }
            });
        }
        // The manifest is re-read on every scan so an edit to plugin.json
        // takes effect on reload.
        QFile file(dir + QStringLiteral("/plugin.json"));
        QString error;
        PluginManifest manifest;
        bool ok = file.open(QIODevice::ReadOnly) &&
                  PluginManifest::parse(file.readAll(), &manifest, &error);
        if (ok && manifest.id != name) {
            ok = false;
            error = tr("plugin.json id \"%1\" does not match the directory name \"%2\"")
                        .arg(manifest.id, name);
        }
        if (!ok) {
            // Listed under the directory name so the Plugins page can show
            // why it did not load.
            plugin->manifest = PluginManifest();
            plugin->manifest.id = name;
            plugin->manifest.name = name;
            plugin->error = error.isEmpty() ? tr("plugin.json could not be read") : error;
        } else {
            plugin->manifest = manifest;
            plugin->error.clear();
        }
        plugin->enabled = enabledSetting(name);
        watch(*plugin);
    }
    std::sort(m_plugins.begin(), m_plugins.end(),
              [](const auto &a, const auto &b) { return a->manifest.id < b->manifest.id; });
}

void ScriptHost::loadAll()
{
    scan();
    for (auto &plugin : m_plugins)
        load(*plugin);
    emit pluginsChanged();
}

void ScriptHost::unloadAll()
{
    for (auto &plugin : m_plugins) {
        // Reached from the Settings page only between script calls (every
        // nested loop a script can open is an app-modal dialog): a plugin
        // still on the stack would be freed under guarded().
        Q_ASSERT(plugin->callDepth == 0);
        teardown(*plugin);
        unwatch(*plugin);
        plugin->reloadTimer->deleteLater();
    }
    m_plugins.clear();
    if (m_watcher->directories().contains(m_pluginsDir))
        m_watcher->removePath(m_pluginsDir);
    emit pluginsChanged();
}

QStringList ScriptHost::pluginIds() const
{
    QStringList out;
    for (const auto &plugin : m_plugins)
        out.append(plugin->manifest.id);
    return out;
}

const Plugin *ScriptHost::plugin(const QString &id) const
{
    for (const auto &plugin : m_plugins) {
        if (plugin->manifest.id == id)
            return plugin.get();
    }
    if (m_console && id == kConsoleId)
        return m_console.get();
    return nullptr;
}

Plugin *ScriptHost::findPlugin(const QString &id)
{
    for (auto &plugin : m_plugins) {
        if (plugin->manifest.id == id)
            return plugin.get();
    }
    return nullptr;
}

Plugin *ScriptHost::pluginForPath(const QString &path)
{
    for (auto &plugin : m_plugins) {
        if (path == plugin->dir || path.startsWith(plugin->dir + QLatin1Char('/')))
            return plugin.get();
    }
    return nullptr;
}

void ScriptHost::setEnabled(const QString &id, bool enabled)
{
    Plugin *plugin = findPlugin(id);
    if (!plugin)
        return;
    QSettings().setValue(enabledKey(id), enabled);
    plugin->enabled = enabled;
    if (enabled) {
        load(*plugin);
    } else {
        teardown(*plugin);
        plugin->state = PluginState::Disabled;
    }
    emit pluginsChanged();
}

void ScriptHost::reload(const QString &id)
{
    Plugin *plugin = findPlugin(id);
    if (!plugin)
        return;
    teardown(*plugin);
    // Re-read the manifest (and re-arm the file watches an editor's
    // save-by-rename dropped) before loading again.
    scan();
    plugin = findPlugin(id);
    if (plugin)
        load(*plugin);
    emit pluginsChanged();
}

void ScriptHost::load(Plugin &plugin)
{
    teardown(plugin);
    if (plugin.teardownPending) {
        // Mid-call (a dialog is up): the reload happens when it unwinds.
        plugin.reloadPending = true;
        return;
    }
    if (!plugin.enabled) {
        plugin.state = PluginState::Disabled;
        return;
    }
    if (!plugin.error.isEmpty()) { // manifest problem from scan()
        plugin.state = PluginState::Error;
        log(plugin, LogLevel::Error, plugin.error);
        return;
    }
    const QString mainPath = plugin.dir + QLatin1Char('/') + plugin.manifest.main;
    if (!QFile::exists(mainPath)) {
        plugin.error = tr("%1 not found").arg(plugin.manifest.main);
        plugin.state = PluginState::Error;
        log(plugin, LogLevel::Error, plugin.error);
        return;
    }
    buildEngine(plugin);
    if (plugin.state == PluginState::Error)
        return;

    // ES module: `export function activate(ctx) {}` (+ optional deactivate).
    const QJSValue module = guarded(plugin, [&] { return plugin.engine->importModule(mainPath); });
    if (plugin.state == PluginState::Error)
        return;
    if (module.isError()) {
        plugin.error = formatError(module);
        plugin.state = PluginState::Error;
        log(plugin, LogLevel::Error, plugin.error);
        teardown(plugin, /*callDeactivate=*/false);
        plugin.state = PluginState::Error;
        return;
    }
    const QJSValue activate = module.property(QStringLiteral("activate"));
    if (!activate.isCallable()) {
        plugin.error = tr("%1 does not export activate()").arg(plugin.manifest.main);
        plugin.state = PluginState::Error;
        log(plugin, LogLevel::Error, plugin.error);
        teardown(plugin, false);
        plugin.state = PluginState::Error;
        return;
    }
    plugin.deactivate = module.property(QStringLiteral("deactivate"));

    QJSValue ctx = plugin.engine->newObject();
    ctx.setProperty(QStringLiteral("id"), plugin.manifest.id);
    ctx.setProperty(QStringLiteral("name"), plugin.manifest.name);
    ctx.setProperty(QStringLiteral("version"), plugin.manifest.version);
    ctx.setProperty(QStringLiteral("dir"), plugin.dir);
    plugin.state = PluginState::Loaded;
    const QJSValue result = guarded(plugin, [&] { return activate.call({ctx}); });
    if (plugin.state == PluginState::Error)
        return;
    if (result.isError()) {
        plugin.error = formatError(result);
        plugin.state = PluginState::Error;
        teardown(plugin, false);
        plugin.state = PluginState::Error;
        return;
    }
    plugin.error.clear();
    log(plugin, LogLevel::Info,
        tr("loaded %1 %2").arg(plugin.manifest.name, plugin.manifest.version));
}

void ScriptHost::buildEngine(Plugin &plugin)
{
    plugin.engine = std::make_unique<QJSEngine>();
    plugin.uiRoot = std::make_unique<QObject>();
    QString error;
    if (!installApi(*this, plugin, &error)) {
        plugin.error = error;
        plugin.state = PluginState::Error;
        log(plugin, LogLevel::Error, error);
        teardown(plugin, false);
        plugin.state = PluginState::Error;
    }
}

void ScriptHost::teardown(Plugin &plugin)
{
    teardown(plugin, plugin.state == PluginState::Loaded);
}

void ScriptHost::teardown(Plugin &plugin, bool callDeactivate)
{
    if (!plugin.engine)
        return;
    if (plugin.callDepth > 0) {
        // The engine has frames on the stack (a script call is inside a
        // nested event loop): guarded() finishes this when they unwind.
        plugin.teardownPending = true;
        plugin.pendingDeactivate = plugin.pendingDeactivate || callDeactivate;
        return;
    }
    if (m_transaction.open() && m_transaction.owner == &plugin)
        forceRollback(tr("the plugin was unloaded"));
    if (callDeactivate && plugin.deactivate.isCallable()) {
        plugin.state = PluginState::Loaded;
        guarded(plugin, [&] { return plugin.deactivate.call(); }); // errors logged inside
    }
    // Everything the plugin registered goes with it (PLAN §3 Disposables).
    // Docks first: their widget handles hold QJSValue callbacks into the
    // engine, and a canvas mid-paint must never outlive it.
    for (const QPointer<QDockWidget> &dock : plugin.docks) {
        if (dock)
            delete dock.data();
    }
    plugin.docks.clear();
    // Menus, context items and overlays next, for the same reason.
    plugin.contextItems.clear();
    plugin.overlays.clear();
    plugin.menu = nullptr;
    plugin.uiRoot.reset();
    invalidateOverlays();
    plugin.images.clear();
    plugin.listeners.clear();
    plugin.changedEditStreak = 0;
    auto &keys = keymap::Registry::instance();
    for (PluginAction &action : plugin.actions) {
        delete action.action.data();
        keys.unregisterDynamic(action.fullId);
    }
    plugin.actions.clear();
    // QJSValues must die before their engine.
    plugin.dispatch = QJSValue();
    plugin.runAction = QJSValue();
    plugin.deactivate = QJSValue();
    plugin.facades.clear();
    plugin.engine.reset();
    plugin.state = PluginState::Disabled;
    updateFrameTimer();
}

QJSValue ScriptHost::guarded(Plugin &plugin, const std::function<QJSValue()> &fn)
{
    if (!plugin.engine)
        return QJSValue();
    const bool txOpenBefore = m_transaction.open();
    const uint64_t txSerialBefore = m_transaction.serial;
    plugin.callDepth++;
    m_watchdog.arm(plugin.engine.get(), m_watchdogMs);
    QJSValue result = fn();
    const bool fired = m_watchdog.disarm();
    plugin.callDepth--;
    if (plugin.callDepth == 0 && plugin.teardownPending) {
        // A teardown (reload, disable, fault) that arrived while this call
        // had the engine on the stack. The result belongs to the engine
        // that is about to go, so nothing of it is returned.
        result = QJSValue();
        plugin.teardownPending = false;
        const bool deactivate = plugin.pendingDeactivate;
        plugin.pendingDeactivate = false;
        const PluginState stateBefore = plugin.state;
        teardown(plugin, deactivate && stateBefore == PluginState::Loaded);
        if (stateBefore == PluginState::Error)
            plugin.state = PluginState::Error;
        if (plugin.removePending) {
            // Deferred: the caller may still be walking m_plugins.
            Plugin *p = &plugin;
            QTimer::singleShot(0, this, [this, p] { erasePlugin(p); });
            return QJSValue();
        }
        if (plugin.reloadPending) {
            plugin.reloadPending = false;
            load(plugin);
        }
        emit pluginsChanged();
        return QJSValue();
    }
    // A transaction begun inside this call and still open on the way out
    // was never committed or rolled back: the interrupt cut the script
    // off before the prelude's try/catch could. Close it here.
    if (m_transaction.open() && m_transaction.owner == &plugin &&
        (!txOpenBefore || m_transaction.serial != txSerialBefore)) {
        forceRollback(fired ? tr("the script was interrupted")
                            : tr("the script returned without finishing it"));
    }
    if (fired) {
        plugin.engine->setInterrupted(false);
        if (plugin.engine->hasError())
            plugin.engine->catchError();
        fault(plugin, tr("script ran for more than %1 ms and was stopped; the plugin is "
                         "disabled until it is reloaded")
                          .arg(m_watchdogMs));
        return plugin.engine ? plugin.engine->newErrorObject(QJSValue::GenericError,
                                                             QStringLiteral("interrupted"))
                             : QJSValue();
    }
    if (result.isError() && plugin.state == PluginState::Loaded)
        log(plugin, LogLevel::Error, formatError(result));
    return result;
}

void ScriptHost::fault(Plugin &plugin, const QString &why)
{
    plugin.error = why;
    plugin.state = PluginState::Error;
    log(plugin, LogLevel::Error, why);
    // Deferred: the fault may surface inside a nested call into this very
    // engine, whose frames are still on the C++ stack.
    const QString id = plugin.manifest.id;
    const bool builtin = plugin.builtin;
    QTimer::singleShot(0, this, [this, id, builtin] {
        Plugin *p = builtin ? m_console.get() : findPlugin(id);
        if (!p || p->state != PluginState::Error)
            return;
        teardown(*p, false);
        p->state = PluginState::Error;
        emit pluginsChanged();
    });
}

bool ScriptHost::beginTransaction(Plugin &plugin, const QString &name, QString *error)
{
    EditTransaction &tx = m_transaction;
    if (tx.open()) {
        if (tx.owner != &plugin) {
            *error = tr("another plugin's transaction ('%1') is in progress").arg(tx.name);
            return false;
        }
        if (tx.inCall) {
            *error = tr("porydaw.edit re-entered while an edit call was in progress");
            return false;
        }
        if (tx.aborted) {
            *error = tr("transaction '%1' was aborted: %2").arg(tx.name, tx.abortReason);
            return false;
        }
        tx.depth++;
        return true;
    }
    SongDocument *doc = m_session ? &m_session->doc : nullptr;
    if (!doc) {
        *error = tr("no song is loaded");
        return false;
    }
    if (doc->isLocked()) {
        // A song-bundle tab: the document drops every command, so refuse
        // up front rather than let edit.* calls silently do nothing.
        *error = tr("the song is read-only (a song bundle); import it into a project to edit");
        return false;
    }
    tx = EditTransaction();
    tx.owner = &plugin;
    tx.doc = doc;
    tx.name = name;
    tx.depth = 1;
    tx.revision = doc->revision();
    tx.serial = ++m_transactionSerial;
    doc->beginEditGroup(name);
    return true;
}

bool ScriptHost::commitTransaction(Plugin &plugin, QString *error)
{
    EditTransaction &tx = m_transaction;
    if (!tx.open() || tx.owner != &plugin) {
        *error = tr("no transaction to commit");
        return false;
    }
    if (tx.depth > 1) {
        tx.depth--;
        if (tx.aborted) {
            *error = tr("transaction '%1' was aborted: %2").arg(tx.name, tx.abortReason);
            return false;
        }
        return true;
    }
    if (!tx.aborted && tx.doc && tx.doc->revision() != tx.revision)
        abortTransaction(tr("the document changed outside the transaction"));
    if (!tx.aborted && !tx.doc)
        abortTransaction(tr("the song was closed"));
    const QString name = tx.name;
    const QString reason = tx.abortReason;
    const bool aborted = tx.aborted;
    if (tx.doc)
        tx.doc->endEditGroup(/*discard=*/aborted);
    tx = EditTransaction();
    if (aborted) {
        *error = tr("transaction '%1' was rolled back: %2").arg(name, reason);
        return false;
    }
    return true;
}

void ScriptHost::rollbackTransaction(Plugin &plugin)
{
    EditTransaction &tx = m_transaction;
    if (!tx.open() || tx.owner != &plugin)
        return;
    if (!tx.aborted)
        abortTransaction(tr("rolled back by the script"));
    if (--tx.depth > 0)
        return;
    if (tx.doc)
        tx.doc->endEditGroup(/*discard=*/true);
    tx = EditTransaction();
}

SongDocument *ScriptHost::transactionDocument(Plugin &plugin, QString *error)
{
    EditTransaction &tx = m_transaction;
    if (!tx.open() || tx.owner != &plugin) {
        *error = tr("must be called inside porydaw.edit.transaction()");
        return nullptr;
    }
    if (tx.inCall) {
        *error = tr("porydaw.edit re-entered while an edit call was in progress");
        return nullptr;
    }
    if (!tx.aborted && !tx.doc)
        abortTransaction(tr("the song was closed"));
    if (!tx.aborted && (!m_session || &m_session->doc != tx.doc.data()))
        abortTransaction(tr("the active song changed"));
    if (!tx.aborted && tx.doc->revision() != tx.revision)
        abortTransaction(tr("the document changed outside the transaction"));
    if (tx.aborted) {
        *error = tr("transaction '%1' was aborted: %2").arg(tx.name, tx.abortReason);
        return nullptr;
    }
    tx.inCall = true;
    return tx.doc.data();
}

void ScriptHost::transactionEdited()
{
    EditTransaction &tx = m_transaction;
    tx.inCall = false;
    if (tx.doc)
        tx.revision = tx.doc->revision();
}

void ScriptHost::abortTransaction(const QString &reason)
{
    if (m_transaction.aborted)
        return;
    m_transaction.aborted = true;
    m_transaction.abortReason = reason;
}

void ScriptHost::forceRollback(const QString &why)
{
    EditTransaction &tx = m_transaction;
    if (!tx.open())
        return;
    if (tx.owner) {
        log(*tx.owner, LogLevel::Error,
            tr("transaction '%1' was left open and has been rolled back: %2").arg(tx.name, why));
    }
    if (tx.doc)
        tx.doc->endEditGroup(/*discard=*/true);
    tx = EditTransaction();
}

void ScriptHost::watch(Plugin &plugin)
{
    unwatch(plugin);
    QStringList paths{plugin.dir};
    const QDir dir(plugin.dir);
    for (const QString &file : dir.entryList(
             {QStringLiteral("*.js"), QStringLiteral("*.mjs"), QStringLiteral("plugin.json")},
             QDir::Files))
        paths.append(dir.filePath(file));
    for (const QString &path : paths) {
        if (m_watcher->addPath(path))
            plugin.watchedPaths.append(path);
    }
}

void ScriptHost::unwatch(Plugin &plugin)
{
    if (!plugin.watchedPaths.isEmpty())
        m_watcher->removePaths(plugin.watchedPaths);
    plugin.watchedPaths.clear();
}

void ScriptHost::onPathChanged(const QString &path)
{
    if (path == m_pluginsDir) {
        m_rescanTimer->start();
        return;
    }
    if (Plugin *plugin = pluginForPath(path))
        plugin->reloadTimer->start();
}

void ScriptHost::setSession(SongSession *session)
{
    // The same session can be handed a different song in place (opening a
    // song into the active tab): that is a new song to the API too.
    if (session == m_session && (!session || session->doc.label() == m_sessionLabel))
        return;
    disconnect(m_docConnection);
    // A song.changed still pending for the old song is moot: song.activated
    // tells the plugins everything changed.
    m_pendingOrigins = 0;
    m_changedTimer->stop();
    m_session = session;
    m_sessionLabel = session ? session->doc.label() : QString();
    m_sessionGeneration++;
    m_lastBeat = -1; // the new song's first beat should fire
    if (session) {
        m_docConnection = connect(&session->doc, &SongDocument::documentChanged, this,
                                  &ScriptHost::onDocumentChanged);
    }
    emitEventAll(QStringLiteral("song.activated"),
                 session ? QVariant(QVariantMap{{QStringLiteral("label"), session->doc.label()}})
                         : QVariant());
}

void ScriptHost::onDocumentChanged()
{
    if (!m_session)
        return;
    // Every script mutation runs inside a transaction; every fresh user
    // edit inside a push; what is left is the undo stack replaying history
    // (Edit → Undo/Redo, or a transaction's rollback — which reads as
    // "script", since its transaction is still open).
    if (m_transaction.open())
        m_pendingOrigins |= OriginScript;
    else if (m_session->doc.pushing())
        m_pendingOrigins |= OriginUser;
    else
        m_pendingOrigins |= OriginHistory;
    // Inside the fan-out itself (a listener's dialog spun a nested loop in
    // which the song changed) the pending bits are delivered by a fresh
    // timer once the fan-out unwinds.
    if (!m_inChangedFire && !m_changedTimer->isActive())
        m_changedTimer->start();
}

void ScriptHost::fireChanged()
{
    if (m_inChangedFire)
        return;
    if (!m_pendingOrigins || !m_session || m_session->doc.label() != m_sessionLabel) {
        m_pendingOrigins = 0;
        return;
    }
    const int origins = m_pendingOrigins;
    m_pendingOrigins = 0;
    const char *origin = (origins & OriginUser)     ? "user"
                         : (origins & OriginScript) ? "script"
                                                    : "history";
    // A listener's dialog can let the user open another song, into this
    // tab or another: the rest of the fan-out is then about a song that
    // is gone (song.activated told everyone), so it stops.
    const uint64_t generation = m_sessionGeneration;
    const auto sameSong = [&] { return m_session && m_sessionGeneration == generation; };
    m_inChangedFire = true;
    forEachPlugin([&](Plugin &plugin) {
        if (!sameSong())
            return;
        SongDocument &doc = m_session->doc;
        // The revision at delivery: an earlier listener in this fan-out
        // may already have edited.
        const QVariantMap payload{{QStringLiteral("revision"), double(doc.revision())},
                                  {QStringLiteral("origin"), QLatin1String(origin)}};
        const uint64_t serialBefore = m_transactionSerial;
        const uint64_t revisionBefore = doc.revision();
        emitEvent(plugin, QStringLiteral("song.changed"), plugin.engine->toScriptValue(payload));
        if (!sameSong())
            return;
        // Feedback-loop detector: a listener that edits (a transaction of
        // its own that moved the document) fires song.changed again. A loop
        // can only feed itself through "script" deliveries, so only those
        // count; a reactor answering a run of genuine user edits is fine.
        // (Two plugins alternating, or a plugin editing from a frame
        // listener instead, stay under this radar — a deliberate choice of
        // a cheap detector over a complete one.)
        const bool edited = m_transactionSerial != serialBefore && doc.revision() != revisionBefore;
        if (!edited)
            plugin.changedEditStreak = 0;
        else if (!(origins & OriginUser))
            plugin.changedEditStreak++;
        if (plugin.changedEditStreak > kChangedEditStreakCap) {
            plugin.changedEditStreak = 0;
            fault(plugin, tr("song.changed feedback loop: the listener edited the song in "
                             "reaction to %1 consecutive changes (its own edits included); "
                             "reactors should ignore events whose origin is not \"user\". The "
                             "plugin is disabled until it is reloaded")
                              .arg(kChangedEditStreakCap + 1));
        }
    });
    m_inChangedFire = false;
    if (m_pendingOrigins && m_session)
        m_changedTimer->start();
}

void ScriptHost::tick()
{
    int state = 0;
    if (m_bindings.audio && m_bindings.audio->songLoaded())
        state = int(m_bindings.audio->transport());
    if (m_lastTransport < 0) {
        m_lastTransport = state;
        return;
    }
    if (state == m_lastTransport)
        return;
    m_lastTransport = state;
    static const char *const names[] = {"stopped", "paused", "playing"};
    emitEventAll(
        QStringLiteral("transport.state"),
        QVariantMap{{QStringLiteral("state"), QLatin1String(names[std::clamp(state, 0, 2)])}});
}

void ScriptHost::setListenerCount(Plugin &plugin, const QString &event, int count)
{
    if (count <= 0)
        plugin.listeners.remove(event);
    else
        plugin.listeners.insert(event, count);
    updateFrameTimer();
}

bool ScriptHost::anyListener(const QString &event) const
{
    for (const auto &plugin : m_plugins) {
        if (plugin->state == PluginState::Loaded && plugin->listeners.value(event) > 0)
            return true;
    }
    return m_console && m_console->state == PluginState::Loaded &&
           m_console->listeners.value(event) > 0;
}

void ScriptHost::updateFrameTimer()
{
    const bool wanted = anyListener(QStringLiteral("audio.frame")) ||
                        anyListener(QStringLiteral("transport.tick")) ||
                        anyListener(QStringLiteral("transport.beat"));
    if (wanted && !m_frameTimer->isActive())
        m_frameTimer->start();
    else if (!wanted && m_frameTimer->isActive())
        m_frameTimer->stop();
}

bool ScriptHost::frameTimerActive() const
{
    return m_frameTimer->isActive();
}

void ScriptHost::emitEventListening(const QString &event, const QVariant &payload)
{
    forEachPlugin([&](Plugin &plugin) {
        if (plugin.listeners.value(event) <= 0)
            return;
        emitEvent(plugin, event, plugin.engine->toScriptValue(payload));
    });
}

void ScriptHost::pumpFrame()
{
    if (m_inFrame)
        return;
    m_inFrame = true;
    const AudioEngine *audio = m_bindings.audio;
    const bool loaded = audio && audio->songLoaded();
    const Transport transport = loaded ? audio->transport() : Transport::Stopped;
    const bool playing = transport == Transport::Playing;
    double tick = 0.0;
    if (loaded && audio->timeline())
        tick = audio->timeline()->tickForSample(audio->playheadSamples());

    // audio.frame: one analysis per frame, shared by every subscriber.
    if (anyListener(QStringLiteral("audio.frame"))) {
        uint32_t fresh = 0;
        if (audio)
            fresh = m_analyzer.poll(audio->tap());
        emitEventListening(
            QStringLiteral("audio.frame"),
            QVariantMap{{QStringLiteral("peak"),
                         QVariantList{double(m_analyzer.peak(0)), double(m_analyzer.peak(1))}},
                        {QStringLiteral("rms"),
                         QVariantList{double(m_analyzer.rms(0)), double(m_analyzer.rms(1))}},
                        {QStringLiteral("frames"), double(fresh)},
                        {QStringLiteral("sampleRate"), audio ? audio->sampleRate() : 0.0},
                        {QStringLiteral("playing"), playing}});
    }

    static const char *const names[] = {"stopped", "paused", "playing"};
    const QString stateName = QLatin1String(names[std::clamp(int(transport), 0, 2)]);
    if (anyListener(QStringLiteral("transport.tick"))) {
        emitEventListening(QStringLiteral("transport.tick"),
                           QVariantMap{{QStringLiteral("state"), stateName},
                                       {QStringLiteral("tick"), tick},
                                       {QStringLiteral("playing"), playing}});
    }

    // transport.beat: fires when the playhead enters a new beat of the
    // meter in force there (a bar of 3/4 has three), including the beat
    // playback starts on, and again after a loop wrap or a seek.
    if (!playing || !audio->timeline()) {
        m_lastBeat = -1;
    } else if (anyListener(QStringLiteral("transport.beat"))) {
        const MidiTimeline *tl = audio->timeline();
        const MidiTimeline::BarPosition pos = tl->barPositionForTick(tick);
        const int beatsPerBar = std::max(pos.beatsPerBar, 1);
        const double barFloor = std::floor(pos.bar);
        int beat = int((pos.bar - barFloor) * beatsPerBar);
        beat = std::clamp(beat, 0, beatsPerBar - 1);
        const int64_t index = int64_t(barFloor) * beatsPerBar + beat;
        if (index != m_lastBeat) {
            m_lastBeat = index;
            double bpm = SongDocument::kTempoDefault;
            for (const TempoPoint &p : tl->tempoMap) {
                if (double(p.tick) > tick)
                    break;
                bpm = p.bpm;
            }
            emitEventListening(QStringLiteral("transport.beat"),
                               QVariantMap{{QStringLiteral("bar"), double(barFloor)},
                                           {QStringLiteral("beat"), beat},
                                           {QStringLiteral("beatsPerBar"), beatsPerBar},
                                           {QStringLiteral("beatTicks"), double(pos.beatTicks)},
                                           {QStringLiteral("tick"), tick},
                                           {QStringLiteral("bpm"), bpm}});
        }
    }
    m_inFrame = false;
}

QJSValue ScriptHost::invoke(Plugin &plugin, const QJSValue &fn, const QJSValueList &args)
{
    if (plugin.state != PluginState::Loaded || !plugin.engine || !fn.isCallable())
        return QJSValue();
    if (plugin.engine->isInterrupted())
        return QJSValue();
    QJSValue callable = fn;
    return guarded(plugin, [&] { return callable.call(args); });
}

void ScriptHost::registerDock(Plugin &plugin, QDockWidget *dock, Qt::DockWidgetArea area)
{
    plugin.docks.emplace_back(dock);
    // Prune docks a script closed (deleteLater) so the list stays small.
    plugin.docks.erase(std::remove_if(plugin.docks.begin(), plugin.docks.end(),
                                      [](const QPointer<QDockWidget> &d) { return d.isNull(); }),
                       plugin.docks.end());
    if (m_bindings.addDock)
        m_bindings.addDock(dock, area);
}

MenuHandle *ScriptHost::pluginMenu(Plugin &plugin)
{
    if (plugin.menu)
        return plugin.menu;
    if (!m_bindings.pluginsMenu || !plugin.uiRoot)
        return nullptr;
    QMenu *parent = m_bindings.pluginsMenu();
    if (!parent)
        return nullptr;
    const QString name = plugin.manifest.name.isEmpty() ? plugin.manifest.id : plugin.manifest.name;
    // A widget child of the Plugins menu, so it lives in the window's
    // object tree (findChild, styling); the handle still owns its life.
    auto *menu = new QMenu(name, parent);
    menu->setObjectName(QStringLiteral("plugin.") + plugin.manifest.id + QStringLiteral(".menu"));
    // Not the application menu on macOS, whatever the plugin is called
    // (see MenuItemHandle's constructor).
    menu->menuAction()->setMenuRole(QAction::NoRole);
    // The window shows Plugins while some plugin has a submenu in it;
    // deleting the submenu (teardown) removes its entry by itself.
    parent->addMenu(menu);
    parent->menuAction()->setVisible(true);
    connect(menu, &QObject::destroyed, parent, [parent] {
        // destroyed fires before the submenu's own action leaves the
        // parent, so the count waits a turn.
        QTimer::singleShot(0, parent, [parent] {
            for (QAction *action : parent->actions()) {
                if (action->menu())
                    return;
            }
            parent->menuAction()->setVisible(false);
        });
    });
    plugin.menu = new MenuHandle(*this, plugin, menu, plugin.uiRoot.get());
    return plugin.menu;
}

void ScriptHost::appendContextMenu(QMenu &menu, const QString &surface)
{
    bool separated = false;
    const auto append = [&](Plugin &plugin) {
        if (plugin.state != PluginState::Loaded)
            return;
        // A copy: a shouldShow() predicate may add or remove items.
        const std::vector<Plugin::ContextItem> items = plugin.contextItems;
        for (const Plugin::ContextItem &entry : items) {
            if (entry.surface != surface || !entry.item || !entry.item->shouldShow())
                continue;
            QAction *action = entry.item->action();
            if (!action || plugin.state != PluginState::Loaded)
                continue;
            if (!separated) {
                menu.addSeparator();
                separated = true;
            }
            menu.addAction(action);
        }
    };
    for (auto &plugin : m_plugins)
        append(*plugin);
    if (m_console)
        append(*m_console);
}

void ScriptHost::paintOverlays(QPainter &painter, SongView &view,
                               const RollOverlayGeometry &geometry)
{
    // porydaw.song reflects the active session: overlays paint only on its
    // view, so a script never reads one song while drawing over another.
    if (!m_session || m_session->view != &view)
        return;
    const auto paint = [&](Plugin &plugin) {
        if (plugin.state != PluginState::Loaded)
            return;
        // A copy: a paint callback may remove its overlay or add another,
        // both of which edit plugin.overlays.
        const std::vector<QPointer<OverlayHandle>> overlays = plugin.overlays;
        for (const QPointer<OverlayHandle> &overlay : overlays) {
            if (overlay && overlay->visible())
                overlay->paint(painter, view, geometry);
        }
    };
    for (auto &plugin : m_plugins)
        paint(*plugin);
    if (m_console)
        paint(*m_console);
}

void ScriptHost::invalidateOverlays()
{
    // From inside a paint (an overlay removing itself, adding another) the
    // roll's cache is about to be marked clean: ask again next turn.
    if (m_paintDepth > 0) {
        QTimer::singleShot(0, this, [this] { invalidateOverlays(); });
        return;
    }
    if (m_session && m_session->view)
        m_session->view->invalidateRoll();
}

void ScriptHost::pauseWatchdog()
{
    m_watchdog.pause();
}

void ScriptHost::resumeWatchdog()
{
    m_watchdog.resume(m_watchdogMs);
}

bool ScriptHost::dialogsAllowed(const Plugin &plugin, QString *error) const
{
    if (m_paintDepth > 0) {
        *error = tr("not from a paint callback");
        return false;
    }
    if (m_transaction.open() && m_transaction.owner == &plugin) {
        *error = tr("a dialog can't open inside a transaction (finish the edit first)");
        return false;
    }
    return true;
}

void ScriptHost::emitEvent(Plugin &plugin, const QString &event, const QJSValue &payload)
{
    if (plugin.state != PluginState::Loaded || !plugin.dispatch.isCallable())
        return;
    // An engine the watchdog has just interrupted can't run listeners —
    // every call would fail with "Interrupted" — and the rollback of its
    // open transaction fans song.changed right back at it.
    if (plugin.engine->isInterrupted())
        return;
    guarded(plugin, [&] { return plugin.dispatch.call({QJSValue(event), payload}); });
}

void ScriptHost::emitEventAll(const QString &event, const QVariant &payload)
{
    forEachPlugin(
        [&](Plugin &plugin) { emitEvent(plugin, event, plugin.engine->toScriptValue(payload)); });
}

void ScriptHost::forEachPlugin(const std::function<void(Plugin &)> &fn)
{
    QStringList ids;
    for (const auto &plugin : m_plugins)
        ids.append(plugin->manifest.id);
    for (const QString &id : ids) {
        Plugin *plugin = findPlugin(id);
        if (plugin && plugin->state == PluginState::Loaded && plugin->engine)
            fn(*plugin);
    }
    if (m_console && m_console->state == PluginState::Loaded && m_console->engine)
        fn(*m_console);
}

void ScriptHost::erasePlugin(Plugin *plugin)
{
    for (auto it = m_plugins.begin(); it != m_plugins.end(); ++it) {
        if (it->get() != plugin)
            continue;
        if (!plugin->removePending || plugin->callDepth > 0)
            return;
        teardown(*plugin, false);
        unwatch(*plugin);
        plugin->reloadTimer->deleteLater();
        m_plugins.erase(it);
        emit pluginsChanged();
        return;
    }
}

QVariantMap engineSettingsMap(const EngineSettings &settings)
{
    return {{QStringLiteral("maxPcmChannels"), settings.maxPcmChannels},
            {QStringLiteral("pcmMixRate"), double(settings.pcmMixRate)},
            {QStringLiteral("analogFilter"), settings.analogFilter}};
}

QString ScriptHost::evalConsole(const QString &code)
{
    if (!m_console) {
        m_console = std::make_unique<Plugin>();
        m_console->builtin = true;
        m_console->manifest.id = kConsoleId;
        m_console->manifest.name = tr("Script Console");
        m_console->manifest.version = QStringLiteral(PORYDAW_VERSION);
        m_console->manifest.apiMajor = kApiMajor;
    }
    Plugin &console = *m_console;
    if (!console.engine) {
        // A faulted console (watchdog) comes back with a fresh engine.
        console.error.clear();
        console.state = PluginState::Disabled;
        buildEngine(console);
        if (console.state == PluginState::Error)
            return QString();
        console.state = PluginState::Loaded;
    }
    const QJSValue result =
        guarded(console, [&] { return console.engine->evaluate(code, QStringLiteral("console")); });
    if (console.state != PluginState::Loaded || result.isError())
        return QString();
    if (result.isUndefined())
        return QStringLiteral("undefined");
    if (result.isObject() && !result.isCallable()) {
        const QJSValue stringify = console.engine->globalObject()
                                       .property(QStringLiteral("JSON"))
                                       .property(QStringLiteral("stringify"));
        const QJSValue text = stringify.call({result, QJSValue(), QJSValue(2)});
        if (text.isString())
            return text.toString();
    }
    return result.toString();
}

bool ScriptHost::runCommand(const QString &fullId)
{
    for (auto &plugin : m_plugins) {
        for (const PluginAction &action : plugin->actions) {
            if (action.fullId == fullId) {
                runPluginAction(*plugin, fullId);
                return true;
            }
        }
    }
    if (m_console) {
        for (const PluginAction &action : m_console->actions) {
            if (action.fullId == fullId) {
                runPluginAction(*m_console, fullId);
                return true;
            }
        }
    }
    return false;
}

bool ScriptHost::handleKey(QKeyEvent *event, keymap::Context surface, bool timeSelectionActive)
{
    // Escape cancels drags and clears selections on every surface; a
    // rebind can't take it (registerAction refuses it as a default).
    if (event->key() == Qt::Key_Escape)
        return false;
    const auto &keys = keymap::Registry::instance();
    const auto tryPlugin = [&](Plugin &plugin) {
        if (plugin.state != PluginState::Loaded)
            return false;
        for (const PluginAction &action : plugin.actions) {
            if (action.context == keymap::Context::Global)
                continue; // window-level QAction fires those
            // Only the focused surface's own context, plus the range
            // overlay while a time selection is active — mirroring how
            // the registry judged conflicts for this action.
            if (action.context == keymap::Context::TimeSelection) {
                if (!timeSelectionActive)
                    continue;
            } else if (action.context != surface) {
                continue;
            }
            if (keys.matches(event, action.fullId)) {
                runPluginAction(plugin, action.fullId);
                return true;
            }
        }
        return false;
    };
    for (auto &plugin : m_plugins) {
        if (tryPlugin(*plugin))
            return true;
    }
    return m_console && tryPlugin(*m_console);
}

void ScriptHost::runPluginAction(Plugin &plugin, const QString &fullId)
{
    if (plugin.state != PluginState::Loaded || !plugin.runAction.isCallable())
        return;
    guarded(plugin, [&] { return plugin.runAction.call({QJSValue(fullId)}); });
}

void ScriptHost::log(const Plugin &plugin, LogLevel level, const QString &text)
{
    if (level == LogLevel::Error)
        qWarning("[plugin %s] %s", qPrintable(plugin.manifest.id), qPrintable(text));
    emit message(plugin.manifest.id, int(level), text);
}

QString ScriptHost::registerAction(Plugin &plugin, const QString &actionId, const QString &name,
                                   keymap::Context context, const QString &defaultKeys,
                                   QString *error)
{
    if (!PluginManifest::validId(actionId)) {
        *error = tr("action id \"%1\" must be lowercase letters, digits, '-' or '_'").arg(actionId);
        return QString();
    }
    if (name.trimmed().isEmpty()) {
        *error = tr("action \"%1\" needs a name").arg(actionId);
        return QString();
    }
    const QString fullId =
        QStringLiteral("plugin.") + plugin.manifest.id + QLatin1Char('.') + actionId;
    auto &keys = keymap::Registry::instance();
    if (keys.command(fullId).id == fullId) {
        *error = tr("action \"%1\" is already registered").arg(actionId);
        return QString();
    }
    // A default that collides with a shipped (or another plugin's) binding
    // in an overlapping context ships unbound instead: the user can still
    // bind it by hand in Settings → Keyboard Shortcuts.
    QString keysText = defaultKeys;
    for (const QString &part : defaultKeys.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const QKeySequence seq = QKeySequence::fromString(part, QKeySequence::PortableText);
        if (seq.isEmpty()) {
            log(plugin, LogLevel::Warning,
                tr("action \"%1\": default shortcut \"%2\" is not a valid key sequence; "
                   "registered without one")
                    .arg(actionId, part));
            keysText.clear();
            break;
        }
        // Escape is every surface's cancel key and deliberately not a
        // keymap command, so the conflict scan below can't protect it.
        if (seq == QKeySequence(Qt::Key_Escape)) {
            log(plugin, LogLevel::Warning,
                tr("action \"%1\": Escape can't be a plugin shortcut; registered without one")
                    .arg(actionId));
            keysText.clear();
            break;
        }
        // Against effective bindings AND shipped defaults: a user who
        // moved Save off Ctrl+S can Reset it later, and two window-level
        // actions on one chord make Qt fire neither.
        QStringList taken = keys.conflicts(fullId, context, seq);
        taken += keys.defaultConflicts(fullId, context, seq);
        taken.removeDuplicates();
        if (!taken.isEmpty()) {
            log(plugin, LogLevel::Warning,
                tr("action \"%1\": default shortcut %2 is already used by %3; registered "
                   "without one")
                    .arg(actionId, seq.toString(QKeySequence::NativeText),
                         taken.join(QStringLiteral(", "))));
            keysText.clear();
            break;
        }
    }
    if (!keys.registerDynamic({fullId, context, plugin.manifest.name, name, keysText})) {
        *error = tr("could not register \"%1\"").arg(actionId);
        return QString();
    }
    PluginAction action;
    action.fullId = fullId;
    action.context = context;
    if (context == keymap::Context::Global) {
        // Parented to a facade so it dies with the plugin's engine.
        auto *qaction = new QAction(name, plugin.facades.front().get());
        keys.attach(fullId, qaction);
        connect(qaction, &QAction::triggered, this,
                [this, &plugin, fullId] { runPluginAction(plugin, fullId); });
        if (m_bindings.addGlobalAction)
            m_bindings.addGlobalAction(qaction);
        action.action = qaction;
    }
    plugin.actions.push_back(action);
    return fullId;
}

void ScriptHost::unregisterAction(Plugin &plugin, const QString &fullId)
{
    auto &keys = keymap::Registry::instance();
    for (auto it = plugin.actions.begin(); it != plugin.actions.end(); ++it) {
        if (it->fullId != fullId)
            continue;
        delete it->action.data();
        keys.unregisterDynamic(fullId);
        plugin.actions.erase(it);
        return;
    }
}

QString ScriptHost::formatError(const QJSValue &error) const
{
    if (!error.isError())
        return error.toString();
    const QJSValue stack = error.property(QStringLiteral("stack"));
    QString text = error.toString(); // "Name: message"
    const QString file = error.property(QStringLiteral("fileName")).toString();
    const int line = error.property(QStringLiteral("lineNumber")).toInt();
    if (!file.isEmpty())
        text += QStringLiteral(" (%1:%2)").arg(QFileInfo(file).fileName()).arg(line);
    if (stack.isString() && !stack.toString().isEmpty())
        text += QLatin1Char('\n') + stack.toString();
    return text;
}

} // namespace scripting
