#pragma once

#include <QDeadlineTimer>
#include <QHash>
#include <QImage>
#include <QJSValue>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QVariantMap>
#include <QWaitCondition>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "audio/audiotap.h"
#include "pluginmanifest.h"
#include "ui/enginesettings.h"
#include "ui/keymap.h"

class AudioEngine;
class DecompProject;
class QAction;
class QDockWidget;
class QFileSystemWatcher;
class QJSEngine;
class QKeyEvent;
class QMenu;
class QPainter;
class QTimer;
class SongDocument;
class SongView;
struct SongSession;
struct RollOverlayGeometry;
struct VgAdsr;
enum class VgMacro;
struct VgVoice;
struct WavExportOptions;

namespace scripting {

class MenuHandle;
class MenuItemHandle;
class OverlayHandle;

// The project's instrument catalog as porydaw.project.voicegroups() and
// porydaw.voicegroup.symbols() report it (MainWindow::VgCatalog's lists).
struct VoicegroupCatalog {
    QStringList groupArgs; // -G args, sorted
    QStringList directSound;
    QStringList cries;           // what the compressed sample types take
    QStringList voiceMacroWords; // VgDirectSoundScan::voiceMacroWords
    QStringList progWave;
    QStringList drumkits;
    QStringList synths;
    QList<QPair<QString, QString>> keysplits; // sub-voicegroup, keysplit table
};

// porydaw.project.importBundle's overrides (SongBundle::ImportOptions) and
// what the import registered.
struct BundleImportRequest {
    QString label;
    QString constant;
    QString player;
};
struct BundleImportResult {
    QString label;
    QString constant;
    QString player;
    QString voicegroupArg; // the imported voicegroup's -G arg
    QStringList warnings;
};

// What the host borrows from the main window (docs/scripting/PLAN.md §3).
// Callbacks rather than a MainWindow pointer keep src/scripting/ free of
// the shell; every callback tolerates "no song loaded".
struct HostBindings {
    std::function<void()> play;
    std::function<void()> pause;
    std::function<void()> stop;
    std::function<void(uint64_t tick)> seekTick;
    std::function<void(const QString &text)> statusMessage;
    // Owns a plugin's global-context QAction's shortcut scope: the main
    // window adds it so the binding fires window-wide.
    std::function<void(QAction *action)> addGlobalAction;
    // Adds a plugin's dock (objectName already set) to the main window in
    // `area`, restoring its saved placement from windowState when there is
    // one, and lists it under View. Absent: the dock stays parentless.
    std::function<void(QDockWidget *dock, Qt::DockWidgetArea area)> addDock;
    // The menu-bar "Plugins" menu every plugin's own submenu hangs off
    // (porydaw.ui.menu); the window shows it while it has entries. Absent:
    // plugin menus are built but never shown.
    std::function<QMenu *()> pluginsMenu;
    // Opens a project song by label in the active tab (or a new one);
    // false when the project has no such song.
    std::function<bool(const QString &label, bool newTab)> openSong;
    // Renders the active song to a WAV file (porydaw.audio.render):
    // false with *error set on failure; *seconds receives the length.
    std::function<bool(const QString &path, const WavExportOptions &opts, double *seconds,
                       QString *error)>
        renderWav;
    // Project adapter writes (porydaw.project / porydaw.edit.setVoice).
    // Each is the window's own action minus its dialogs: false with
    // *error set on failure. Absent: the call is refused.
    // Registers a project song (SongRegistry::registerSong + the project
    // reload the window does after it); *songId receives the table index.
    std::function<bool(const QString &label, const QString &constant, const QString &player,
                       int *songId, QString *error)>
        registerSong;
    // Removes a song's registration lines (the .mid stays).
    std::function<bool(const QString &label, QString *error)> unregisterSong;
    // Re-reads the project's music data (song ids may shift).
    std::function<bool(QString *error)> reloadProject;
    // Saves the active song (and its voicegroup when edited) — File → Save.
    std::function<bool(QString *error)> saveSong;
    // Creates sound/voicegroups/<name>.inc (+ the hub .include line) as a
    // copy of the voicegroup `copyFromArg` names, or the dummy template
    // when empty, and refreshes the catalog.
    std::function<bool(const QString &name, const QString &copyFromArg, QString *error)>
        createVoicegroup;
    // Writes a project song as a .porysong song bundle (File → Export Song
    // Bundle without its dialog): an open tab exports as it is in memory,
    // any other song from disk. *samples receives the bundled sample count.
    std::function<bool(const QString &label, const QString &path, int *samples, QString *error)>
        exportBundle;
    // Imports a song bundle (.porysong file or bundle folder) into the open
    // project — the bundle tab's Import button without its dialog — and
    // opens the imported song in a new tab. Empty request fields let the
    // import choose; *result receives what was registered.
    std::function<bool(const QString &path, const BundleImportRequest &request,
                       BundleImportResult *result, QString *error)>
        importBundle;
    // The window's cached voicegroup catalog (one scan per project).
    std::function<VoicegroupCatalog()> voicegroupCatalog;
    // The project-typical envelope for a voice type and instrument symbol
    // (vgDefaultAdsr over the cached catalog).
    std::function<void(VgMacro macro, const QString &symbol, VgAdsr *out)> typicalAdsr;
    // Pushes a voice edit for the session's open voicegroup source onto
    // the session's undo stack (through SongDocument::pushCommand, so an
    // open edit group takes it), applying it to audio and the dock.
    std::function<bool(SongSession &session, int slot, const VgVoice &voice, QString *error)>
        editVoice;
    // The GBA engine settings (Settings → Audio: PCM polyphony, mix rate,
    // analog filter), read and written through porydaw.audio.engine /
    // setEngine. The write goes through the Settings window's page so the
    // page, QSettings and the audio device all follow; false with *error
    // set when the window can't take it. Absent: reads give the defaults,
    // writes are refused.
    std::function<EngineSettings()> engineSettings;
    std::function<bool(const EngineSettings &settings, QString *error)> setEngineSettings;
    const AudioEngine *audio = nullptr;
    const DecompProject *project = nullptr;
};

// {maxPcmChannels, pcmMixRate, analogFilter}: porydaw.audio.engine and the
// audio.engine event payload.
QVariantMap engineSettingsMap(const EngineSettings &settings);

enum class PluginState { Disabled, Loaded, Error };

enum class LogLevel { Info, Warning, Error };

// A command a plugin registered through porydaw.actions.register.
struct PluginAction {
    QString fullId; // keymap id: plugin.<pluginId>.<actionId>
    keymap::Context context;
    QPointer<QAction> action; // global context only; parented to the plugin's facades
};

// One loaded plugin: its own QJSEngine, so one plugin's exception or hang
// can't take another down, plus everything it registered (torn down as a
// unit on unload/reload — nothing leaks across a hot reload).
struct Plugin {
    PluginManifest manifest;
    QString dir;
    bool enabled = true;
    PluginState state = PluginState::Disabled;
    QString error;
    std::unique_ptr<QJSEngine> engine;
    // From the prelude (scriptprelude.js): the event dispatcher and the
    // action runner, both closures over the plugin's `porydaw` object.
    QJSValue dispatch;
    QJSValue runAction;
    QJSValue deactivate;
    // API facade QObjects; destroyed before the engine (they hold QJSValues).
    std::vector<std::unique_ptr<QObject>> facades;
    std::vector<PluginAction> actions;
    bool builtin = false; // the console's own engine: no manifest, no reload
    QTimer *reloadTimer = nullptr;
    QStringList watchedPaths;
    // Live listener counts per event name (the prelude reports them), so
    // the host only pumps frames/beats to plugins that want them and only
    // runs the frame timer while somebody listens.
    QHash<QString, int> listeners;
    // Docks from porydaw.ui.dock (scriptwidgets.h); deleted on teardown
    // before the engine, since their handles hold QJSValue callbacks.
    std::vector<QPointer<QDockWidget>> docks;
    // porydaw.ui.loadImage: decoded images by handle id.
    std::map<int, QImage> images;
    int nextImageId = 1;
    // Owner of the plugin's menu-bar submenu, context-menu items and roll
    // overlays (scriptmenus.h): their handles hold QJSValue callbacks, so
    // teardown deletes this before the engine goes.
    std::unique_ptr<QObject> uiRoot;
    QPointer<MenuHandle> menu; // porydaw.ui.menu(), built on first use
    // porydaw.ui.contextMenu(surface) items, appended to the roll's note
    // menu ("notes") or the time-selection menu ("range") when they open.
    struct ContextItem {
        QString surface;
        QPointer<MenuItemHandle> item;
    };
    std::vector<ContextItem> contextItems;
    std::vector<QPointer<OverlayHandle>> overlays;
    // porydaw.io: files the user picked through a dialog are readable and
    // writable even outside the plugin folder and the project.
    QStringList grantedPaths;
    // Calls into the engine currently on the C++ stack (guarded()). A
    // script call can spin a nested event loop (a dialog, a render), and a
    // teardown arriving then — hot reload, disable, a fault — must wait
    // for the call to unwind: its frames still use the engine.
    int callDepth = 0;
    bool teardownPending = false;
    // Consecutive song.changed deliveries in which this plugin's listener
    // edited the song (each edit fires song.changed again): a reactor that
    // keeps editing in reaction to its own edits is a feedback loop, and
    // the host faults it past kChangedEditStreakCap.
    int changedEditStreak = 0;
    bool pendingDeactivate = false;
    // scan() found the plugin's folder gone while a call had its engine on
    // the stack: guarded() drops it from the host when the call unwinds.
    bool removePending = false;
    bool reloadPending = false;
};

// A script edit transaction (`porydaw.edit.transaction`, API.md): one
// SongDocument edit group — one undo entry — owned by one plugin. Nested
// transactions of the owner flatten (depth); another plugin can't open
// one while it runs (its edits would land in the owner's undo entry).
// Optimistic concurrency: the transaction expects the document's revision
// to be exactly what its own last edit produced; a foreign mutation in
// between (an undo from a nested event loop, say) aborts it, and an
// aborted transaction refuses every further edit and rolls back on
// commit. inCall is a belt-and-braces re-entrancy guard: nothing spins an
// event loop inside a document call today (song.changed is delivered
// afterwards, dialogs are refused inside a transaction), but an edit call
// that somehow re-entered porydaw.edit before returning would see a
// half-updated revision — it is refused.
struct EditTransaction {
    Plugin *owner = nullptr;
    QPointer<SongDocument> doc;
    QString name;
    int depth = 0;
    uint64_t revision = 0;
    uint64_t serial = 0; // distinguishes transactions begun inside a guarded call
    bool aborted = false;
    QString abortReason;
    bool inCall = false;
    bool open() const { return depth > 0; }
};

// Interrupts a plugin engine whose single call into script overruns its
// budget (PLAN §1 stance 6): the UI thread is inside QJSEngine::evaluate
// or QJSValue::call, so only another thread can pull the plug.
// Calls nest (plugin A's action plays the transport, whose state event
// reaches plugin B): armed calls form a stack and only the innermost — the
// one actually on the CPU — is watched. When it returns, the next outer
// call's own deadline is watched again.
class Watchdog : public QThread
{
  public:
    ~Watchdog() override;
    void arm(QJSEngine *engine, int budgetMs);
    // Pops the innermost call; returns whether its interrupt fired.
    bool disarm();
    // Suspends the innermost call's deadline — a modal dialog's nested
    // event loop is the user's time, not the script's — and resume()
    // restarts it with a fresh budget. Calls armed inside the pause (an
    // event reaching another plugin) are watched as usual.
    void pause();
    void resume(int budgetMs);

  protected:
    void run() override;

  private:
    struct Armed {
        QJSEngine *engine;
        QDeadlineTimer deadline;
        bool fired;
        bool paused;
    };
    QMutex m_mutex;
    QWaitCondition m_wake;
    std::vector<Armed> m_stack;
    bool m_quit = false;
};

// The plugin host: discovery, lifecycle, hot reload, the watchdog, and the
// bridge between the app and every plugin's `porydaw` object. Lives on the
// UI thread; scripts run only on the UI thread (PLAN §1 stance 2).
class ScriptHost : public QObject
{
    Q_OBJECT
  public:
    explicit ScriptHost(QObject *parent = nullptr);
    ~ScriptHost() override;

    void setBindings(HostBindings bindings);
    const HostBindings &bindings() const { return m_bindings; }

    // Where plugins live. The folder is resolved at construction:
    // PORYDAW_PLUGINS_DIR when set (a launch-time override for developers
    // and harnesses; it beats the saved setting so what the launcher asked
    // for is what runs), else the folder saved in Settings → Plugins (the
    // "pluginsDir" QSettings key), else <AppDataLocation>/plugins.
    static QString defaultPluginsDir();
    static QString environmentPluginsDir(); // empty unless the variable is set
    static QString configuredPluginsDir();  // the saved setting, or default
    static QString resolvePluginsDir();
    // Persists the setting (empty = default) and, unless the environment
    // overrides it, unloads everything and points the host at the new
    // folder. Call loadAll() afterwards to load what it holds.
    void setPluginsDirSetting(const QString &dir);
    // Points the host at a folder without persisting anything (harnesses).
    void setPluginsDir(const QString &dir);
    QString pluginsDir() const { return m_pluginsDir; }
    // Per-call script budget (default 5000 ms). Harnesses shorten it.
    void setWatchdogMs(int ms) { m_watchdogMs = ms; }
    int watchdogMs() const { return m_watchdogMs; }

    // Scans the plugins dir and loads every enabled plugin. Also starts the
    // file watchers, so later edits hot-reload. Safe to call again (rescan).
    void loadAll();
    void unloadAll();

    QStringList pluginIds() const;
    // Also answers "console" with the Script Console's own plugin (once
    // the REPL has run).
    const Plugin *plugin(const QString &id) const;
    // Persisted under plugins/<id>/enabled; loads or unloads immediately.
    void setEnabled(const QString &id, bool enabled);
    void reload(const QString &id);

    // The song whose document/view the read API reflects (nullptr = none).
    void setSession(SongSession *session);
    SongSession *session() const { return m_session; }
    // Called at the UI cadence: detects transport-state changes to report.
    void tick();
    // One realtime frame (PLAN §4): polls the audio tap and emits
    // audio.frame, transport.tick and transport.beat to the plugins that
    // listen. The host's own ~60 Hz timer calls this while any plugin
    // listens; harnesses call it directly.
    void pumpFrame();
    bool frameTimerActive() const;
    // The analysis the last pumpFrame produced (porydaw.audio reads it).
    AudioAnalyzer &analyzer() { return m_analyzer; }

    // Script Console REPL: evaluates in the console's own engine (which has
    // the full porydaw API). Returns the result's text; errors go to log().
    QString evalConsole(const QString &code);

    // Runs a registered command by keymap id; false when none matches.
    bool runCommand(const QString &fullId);
    // SongView's plugin key handler (installed by the host).
    bool handleKey(QKeyEvent *event, keymap::Context surface, bool timeSelectionActive);

    // --- for the API facades ---
    void log(const Plugin &plugin, LogLevel level, const QString &text);
    // Registers a command for `plugin`; returns the full id, or empty with
    // *error set. Global-context commands get a QAction on the window.
    QString registerAction(Plugin &plugin, const QString &actionId, const QString &name,
                           keymap::Context context, const QString &defaultKeys, QString *error);
    void unregisterAction(Plugin &plugin, const QString &fullId);
    // Emits an event to one plugin / every loaded plugin (prelude dispatch).
    void emitEvent(Plugin &plugin, const QString &event, const QJSValue &payload);
    void emitEventAll(const QString &event, const QVariant &payload);
    // "Name: message (file:line)" plus the stack, for the console.
    QString formatError(const QJSValue &error) const;
    // Calls a plugin's JS function (a widget callback) under the watchdog;
    // errors are logged. Returns an undefined value when the plugin can't
    // run (faulted, interrupted, unloading).
    QJSValue invoke(Plugin &plugin, const QJSValue &fn, const QJSValueList &args);
    // The prelude reports listener counts here (see Plugin::listeners).
    void setListenerCount(Plugin &plugin, const QString &event, int count);
    // Hands a freshly built dock to the main window (HostBindings::addDock)
    // and tracks it for teardown.
    void registerDock(Plugin &plugin, QDockWidget *dock, Qt::DockWidgetArea area);
    // The plugin's submenu of the window's Plugins menu (created on first
    // use, parented to plugin.uiRoot); nullptr when the window offers none.
    MenuHandle *pluginMenu(Plugin &plugin);
    // Appends every plugin's items for `surface` to a context menu that is
    // opening (SongView's PluginMenuProvider).
    void appendContextMenu(QMenu &menu, const QString &surface);
    // Paints every plugin's roll overlays (SongView's PluginOverlayPainter);
    // only the active session's view gets them.
    void paintOverlays(QPainter &painter, SongView &view, const RollOverlayGeometry &geometry);
    // A plugin overlay changed: repaint the active roll.
    void invalidateOverlays();
    // Pauses the watchdog around a modal dialog (pauseWatchdog) or any
    // other nested event loop the script is not responsible for. Pairs.
    void pauseWatchdog();
    void resumeWatchdog();
    // A dialog can't open inside a transaction (its nested event loop
    // could edit the document under the transaction's revision guard),
    // nor from a paint callback (a nested loop inside paintEvent).
    bool dialogsAllowed(const Plugin &plugin, QString *error) const;
    // Brackets a script paint callback (canvas or overlay).
    void beginPaint() { m_paintDepth++; }
    void endPaint() { m_paintDepth--; }

    // Edit transactions (EditTransaction above). begin/commit return false
    // with *error set; the prelude turns that into a thrown Error.
    bool beginTransaction(Plugin &plugin, const QString &name, QString *error);
    bool commitTransaction(Plugin &plugin, QString *error);
    void rollbackTransaction(Plugin &plugin);
    // Every edit call starts here: the document to edit, or nullptr with
    // *error (no transaction, another plugin's, re-entered from a
    // listener, aborted, or the revision guard tripped — the last two
    // abort the transaction). A non-null result must be followed by
    // transactionEdited() once the edit is done.
    SongDocument *transactionDocument(Plugin &plugin, QString *error);
    void transactionEdited();
    const EditTransaction &transaction() const { return m_transaction; }

  signals:
    // Plugin list or a plugin's state changed.
    void pluginsChanged();
    // A line for the Script Console (pluginId "console" for the REPL).
    void message(const QString &pluginId, int level, const QString &text);

  private:
    Plugin *findPlugin(const QString &id);
    Plugin *pluginForPath(const QString &path);
    void scan();
    void load(Plugin &plugin);
    void teardown(Plugin &plugin);
    void teardown(Plugin &plugin, bool callDeactivate);
    void buildEngine(Plugin &plugin);
    // Runs `fn` (a call into the engine) under the watchdog; on interrupt
    // the plugin is faulted (disabled until reload) and an error returned.
    QJSValue guarded(Plugin &plugin, const std::function<QJSValue()> &fn);
    void abortTransaction(const QString &reason);
    // Closes a transaction its owner can no longer finish (interrupted by
    // the watchdog, torn down): reverts and logs.
    void forceRollback(const QString &why);
    void fault(Plugin &plugin, const QString &why);
    void watch(Plugin &plugin);
    void unwatch(Plugin &plugin);
    void onPathChanged(const QString &path);
    void runPluginAction(Plugin &plugin, const QString &fullId);
    static bool enabledSetting(const QString &id);
    bool anyListener(const QString &event) const;
    void updateFrameTimer();
    // song.changed: documentChanged arms a zero-length timer (coalescing a
    // whole transaction, or a macro undo, into one event) and fireChanged
    // delivers it once the mutation's stack has fully unwound.
    void onDocumentChanged();
    void fireChanged();
    // Emits to every plugin with a listener for `event` (payload converted
    // per engine, so the conversion is skipped for the rest).
    void emitEventListening(const QString &event, const QVariant &payload);
    // Calls fn for every plugin (the console last). A call into a plugin
    // can spin a nested event loop in which scan() adds or removes
    // plugins, so this walks a snapshot of ids and re-resolves each.
    void forEachPlugin(const std::function<void(Plugin &)> &fn);
    void erasePlugin(Plugin *plugin);

    HostBindings m_bindings;
    QString m_pluginsDir;
    int m_watchdogMs = 5000;
    std::vector<std::unique_ptr<Plugin>> m_plugins;
    std::unique_ptr<Plugin> m_console;
    Watchdog m_watchdog;
    QFileSystemWatcher *m_watcher = nullptr;
    QTimer *m_rescanTimer = nullptr;
    SongSession *m_session = nullptr;
    QString m_sessionLabel; // the song the session held when set (in-place swaps re-activate)
    uint64_t m_sessionGeneration = 0; // bumped by every setSession that took effect
    QMetaObject::Connection m_docConnection;
    int m_paintDepth = 0;
    int m_lastTransport = -1;
    QTimer *m_frameTimer = nullptr;
    AudioAnalyzer m_analyzer;
    int64_t m_lastBeat = -1; // global beat index of the last transport.beat
    bool m_inFrame = false;  // pumpFrame re-entrancy (a listener that pumps)
    EditTransaction m_transaction;
    uint64_t m_transactionSerial = 0;
    // Pending song.changed: the origins (ChangeOrigin bits) of every
    // documentChanged since the last delivery, and the timer that delivers
    // them. m_inChangedFire guards the delivery against its own timer
    // firing from a listener's nested event loop.
    QTimer *m_changedTimer = nullptr;
    int m_pendingOrigins = 0;
    bool m_inChangedFire = false;
};

} // namespace scripting
