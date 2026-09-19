#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QInputDialog>
#include <QJSEngine>
#include <QJSValue>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QObject>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QUndoStack>
#include <QWheelEvent>
#include <functional>

#include "audio/audiotap.h"
#include "audio/wavexport.h"
#include "core/songdocument.h"
#include "mainwindow.h"
#include "project/songregistry.h"
#include "project/voicegroupsource.h"
#include "scripting/scripthost.h"
#include "scripting/scriptmenus.h"
#include "scripting/scriptwidgets.h"
#include "songsession.h"
#include "ui/audiosettingspage.h"
#include "ui/enginesettings.h"
#include "ui/keyboardshortcutspage.h"
#include "ui/keymap.h"
#include "ui/settingsdialog.h"
#include "ui/songview.h"
#include "ui/viewsidecar.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

// --scriptcheck: scripting host check (self-contained, no project needed).
// Phase 0 of docs/scripting/PLAN.md: proves that the linked QJSEngine does
// the four things the plugin host is built on — evaluates a script,
// bridges a QObject's Q_INVOKABLEs and properties into JS, reports thrown
// errors with the script's file name and line number, and can be
// interrupted from a watchdog thread while the UI thread is stuck inside
// evaluate(). The watchdog assertion carries a wall-clock cap so a broken
// interrupt shows up as a FAIL rather than a hang.

namespace {

// MSVC does not define M_PI without _USE_MATH_DEFINES.
constexpr float kScriptCheckPi = 3.14159265358979323846f;

// Stand-in for a `porydaw.*` facade: a plain QObject whose invokables and
// properties are what a plugin script sees.
class Bridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int revision READ revision CONSTANT)
  public:
    int revision() const { return 42; }
    Q_INVOKABLE int add(int a, int b) { return a + b; }
    Q_INVOKABLE QString echo(const QString &s)
    {
        lastEcho = s;
        return s + s;
    }
    Q_INVOKABLE void log(const QString &line) { lines << line; }
    QString lastEcho;
    QStringList lines;
};

} // namespace

namespace {

int runEngineCheck()
{
    int failures = 0;
    const auto check = [&failures](bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "scriptcheck: FAIL: %s\n", what);
            failures++;
        }
    };

    QJSEngine engine;
    engine.installExtensions(QJSEngine::ConsoleExtension);

    // Plain evaluation.
    {
        const QJSValue v = engine.evaluate(QStringLiteral("1 + 2"));
        check(!v.isError() && v.toInt() == 3, "1 + 2 did not evaluate to 3");
        const QJSValue arr = engine.evaluate(QStringLiteral("[3, 1, 2].sort().map(x => x * 2)"));
        check(!arr.isError() && arr.isArray() &&
                  arr.property(QStringLiteral("length")).toInt() == 3 &&
                  arr.property(0).toInt() == 2 && arr.property(2).toInt() == 6,
              "arrow functions / array methods missing (ES6 baseline)");
    }

    // QObject bridge: properties, invokables, values crossing both ways.
    {
        Bridge bridge;
        engine.globalObject().setProperty(QStringLiteral("porydaw"), engine.newQObject(&bridge));
        // The engine must not delete a C++-owned object when the JS wrapper
        // is collected.
        QJSEngine::setObjectOwnership(&bridge, QJSEngine::CppOwnership);
        const QJSValue sum = engine.evaluate(QStringLiteral("porydaw.add(porydaw.revision, 8)"));
        check(!sum.isError() && sum.toInt() == 50, "Q_INVOKABLE + Q_PROPERTY bridge broke");
        const QJSValue echo = engine.evaluate(QStringLiteral("porydaw.echo('ab')"));
        check(!echo.isError() && echo.toString() == QStringLiteral("abab") &&
                  bridge.lastEcho == QStringLiteral("ab"),
              "QString did not round-trip through the bridge");
        // A JS function held as a QJSValue callback, called from C++ with an
        // argument — the shape of every `on('event', fn)` subscription.
        QJSValue fn = engine.evaluate(
            QStringLiteral("(function(n) { porydaw.log('tick ' + n); return n + 1; })"));
        check(fn.isCallable(), "function literal is not callable");
        const QJSValue r = fn.call({QJSValue(6)});
        check(!r.isError() && r.toInt() == 7 &&
                  bridge.lines == QStringList{QStringLiteral("tick 6")},
              "callback from C++ did not run against the bridge");
        engine.collectGarbage();
        const QJSValue again = engine.evaluate(QStringLiteral("porydaw.add(1, 1)"));
        check(!again.isError() && again.toInt() == 2, "bridge object died across a GC");
    }

    // Errors carry file name + line number, which the Script Console needs.
    {
        const QJSValue err =
            engine.evaluate(QStringLiteral("var a = 1;\nvar b = 2;\nundefinedThing.call();\n"),
                            QStringLiteral("plugin/main.js"));
        check(err.isError(), "reference error was not reported as an error");
        check(err.property(QStringLiteral("lineNumber")).toInt() == 3,
              "error line number is not the throwing line");
        // The engine URL-ifies the name ("file:plugin/main.js"); the console
        // will pass QUrl::fromLocalFile paths, so only the tail is asserted.
        check(err.property(QStringLiteral("fileName"))
                  .toString()
                  .endsWith(QStringLiteral("plugin/main.js")),
              "error file name is not the script name passed to evaluate");
        check(err.property(QStringLiteral("stack")).toString().contains(QStringLiteral("main.js")),
              "error stack does not mention the script");
        const QJSValue thrown = engine.evaluate(QStringLiteral("throw new Error('boom')"));
        check(thrown.isError() &&
                  thrown.property(QStringLiteral("message")).toString() == QStringLiteral("boom"),
              "thrown Error lost its message");
        // The engine must be usable after an error: a plugin crashing must
        // not poison later calls.
        check(engine.evaluate(QStringLiteral("2 * 21")).toInt() == 42,
              "engine unusable after an exception");
    }

    // Watchdog: a runaway script is interrupted from another thread and
    // evaluate() returns. The watchdog thread also enforces the wall-clock
    // cap: if evaluate() has not come back 5 s after the interrupt, the
    // interrupt is broken and the UI thread is stuck for good, so the
    // thread reports the failure and exits the process instead of letting
    // the harness hang.
    {
        struct Watchdog : QThread {
            QJSEngine *engine = nullptr;
            std::atomic<bool> returned{false};
            void run() override
            {
                msleep(150);
                engine->setInterrupted(true);
                for (int i = 0; i < 50 && !returned.load(); ++i)
                    msleep(100);
                if (!returned.load()) {
                    std::fprintf(
                        stderr,
                        "scriptcheck: FAIL: infinite loop was not interrupted within 5 s\n");
                    std::fprintf(stderr, "scriptcheck: 1 failure(s)\n");
                    std::_Exit(1);
                }
            }
        } dog;
        dog.engine = &engine;
        QElapsedTimer clock;
        clock.start();
        dog.start();
        const QJSValue looped = engine.evaluate(QStringLiteral("var n = 0; while (true) n++; n"));
        const qint64 elapsedMs = clock.elapsed();
        dog.returned.store(true);
        dog.wait();
        check(elapsedMs >= 100, "interrupted before the watchdog fired (loop never ran?)");
        check(looped.isError() || looped.isUndefined(),
              "interrupted evaluate returned a normal value");
        check(engine.isInterrupted(), "isInterrupted() not set after the watchdog fired");
        // Clearing the flag must bring the engine back for the next plugin
        // call — an interrupted plugin gets disabled, the others carry on.
        engine.setInterrupted(false);
        const QJSValue after = engine.evaluate(QStringLiteral("'alive'"));
        check(!after.isError() && after.toString() == QStringLiteral("alive"),
              "engine not usable after clearing the interrupt");
    }

    return failures;
}

// ---- plugin host fixtures ----

bool writeFile(const QString &path, const QString &text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(text.toUtf8());
    return true;
}

const char *kFixtureManifest = R"({ "id": "fixture", "name": "Fixture Plugin", "version": "1.0.0",
  "api": "1.0", "description": "harness fixture" })";

// v1: a global action, a roll action, a range action, a conflicting
// default, a song.changed listener, and log lines the harness looks for.
const char *kFixtureMainV1 = R"(
function bump(key) {
    porydaw.storage.set(key, (porydaw.storage.get(key, 0) || 0) + 1);
}
export function activate(ctx) {
    porydaw.actions.register({ id: "hello", name: "Hello", context: "global",
        default: "Ctrl+Alt+Shift+F9", run: function () { bump("ran"); } });
    porydaw.actions.register({ id: "roll", name: "Roll Thing", context: "roll",
        default: "Ctrl+Alt+Shift+F10", run: function () { bump("roll"); } });
    porydaw.actions.register({ id: "range", name: "Range Thing", context: "range",
        default: "Ctrl+Alt+Shift+F11", run: function () { bump("range"); } });
    porydaw.actions.register({ id: "conflict", name: "Steals Delete", context: "roll",
        default: "Delete", run: function () {} });
    porydaw.actions.register({ id: "vel", name: "Velocity Thing", context: "velocity",
        default: "Ctrl+Alt+Shift+F8", run: function () { bump("vel"); } });
    porydaw.actions.register({ id: "save", name: "Steals Save Default", context: "global",
        default: "Ctrl+S", run: function () {} });
    porydaw.actions.register({ id: "esc", name: "Steals Escape", context: "roll",
        default: "Escape", run: function () {} });
    porydaw.song.on("changed", function (e) { porydaw.storage.set("rev", e.revision); });
    porydaw.log("activated v1 " + ctx.id + " " + porydaw.plugin.version);
}
export function deactivate() { porydaw.log("deactivated"); }
)";

// v2 (hot reload): keeps "hello", adds "hello2", drops the rest. Its
// song.changed listener opens an (empty) transaction of its own from the
// notification — allowed, since the event arrives after the change that
// caused it has fully landed — and records the outcome.
const char *kFixtureMainV2 = R"(
export function activate(ctx) {
    porydaw.actions.register({ id: "hello", name: "Hello", context: "global",
        default: "Ctrl+Alt+Shift+F9", run: function () {} });
    porydaw.actions.register({ id: "hello2", name: "Hello Two", context: "global",
        run: function () {} });
    porydaw.song.on("changed", function (e) {
        try {
            porydaw.edit.transaction("Steal", function () {});
            porydaw.storage.set("stealOk", (porydaw.storage.get("stealOk", 0) || 0) + 1);
            porydaw.storage.set("stealOrigin", e.origin);
        } catch (e) { porydaw.storage.set("stealErr", String(e.message)); }
    });
    // Phase 4: a dialog from an action — the harness disables the plugin
    // while it is up, which must wait for the call to unwind.
    porydaw.actions.register({ id: "ask", name: "Ask", context: "global",
        run: function () {
            var ok = porydaw.ui.dialog.confirm("Really?");
            porydaw.storage.set("asked", ok ? 1 : 2);
        } });
    porydaw.log("activated v2");
}
)";

// Injected into the Script Console engine as `harness`: an edit made by
// C++ while a script transaction is open (what a user-driven nested event
// loop would do), to trip the revision guard.
class HarnessBridge : public QObject
{
    Q_OBJECT
  public:
    SongDocument *doc = nullptr;
    Q_INVOKABLE void externalEdit() { doc->setStartTempo(doc->startTempo() + 1); }
};

const char *kBrokenManifest = R"({ "id": "broken", "name": "Broken", "api": 1 })";
const char *kBrokenMain =
    "export function activate() {\n  var x = 1;\n  throw new Error('boom');\n}\n";
const char *kBadManifest = R"({ "id": "badmanifest", "name": "Future", "api": 99 })";
const char *kSyntaxManifest = R"({ "id": "syntax", "name": "Syntax", "api": 1 })";
const char *kSyntaxMain = "export function activate() {\n  return (;\n}\n";
// Phase 3: a dock built from every widget primitive, plus an image.
const char *kPanelManifest = R"({ "id": "panel", "name": "Panel", "version": "1.0.0", "api": 1 })";
const char *kPanelMain = R"(
var dock = null, img = 0;
function count(key) { porydaw.storage.set(key, porydaw.storage.get(key, 0) + 1); }
export function activate() {
    img = porydaw.ui.loadImage("pic.png");
    porydaw.storage.set("imgw", porydaw.ui.imageSize(img).width);
    dock = porydaw.ui.dock({ id: "main", title: "Panel", area: "left", build: function (root) {
        var row = root.addRow();
        row.addLabel("hello");
        row.addButton("Go", function () { count("clicks"); });
        root.addCheckbox("On", false, function (on) { porydaw.storage.set("checked", on ? 1 : 0); });
        root.addSlider(0, 100, 50, function (v) { porydaw.storage.set("slider", v); }, {});
        root.addCombo(["a", "b", "c"], 1, function (i) { porydaw.storage.set("combo", i); });
        root.addCanvas({ minHeight: 40 }, function (g) {
            g.clear("#000000");
            g.image(img, 0, 0, -1, -1, 0, 0, 0, 0);
            count("panelpaints");
        }, null);
        root.addStretch();
    }});
}
export function deactivate() { dock.close(); porydaw.ui.freeImage(img); }
)";
const char *kRunawayManifest = R"({ "id": "runaway", "name": "Runaway", "api": 1 })";
const char *kRunawayMain = R"(
export function activate() {
    porydaw.actions.register({ id: "loop", name: "Loop Forever", context: "global",
        run: function () { while (true) {} } });
}
)";

struct Message {
    QString plugin;
    int level;
    QString text;
};

bool hasMessage(const QList<Message> &messages, const QString &plugin, int level,
                const QString &fragment)
{
    for (const Message &m : messages) {
        if (m.plugin == plugin && m.level == level && m.text.contains(fragment))
            return true;
    }
    return false;
}

// Pumps events until pred() holds or the deadline passes.
bool waitFor(const std::function<bool()> &pred, int ms)
{
    QDeadlineTimer deadline(ms);
    while (!deadline.hasExpired()) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        if (pred())
            return true;
        QThread::msleep(10);
    }
    return pred();
}

int storedCounter(const QString &plugin, const QString &key)
{
    const QByteArray raw =
        QSettings().value(QStringLiteral("plugins/%1/data/%2").arg(plugin, key)).toByteArray();
    return QJsonDocument::fromJson(raw).object().value(QLatin1String("v")).toInt(-1);
}

// ---- Phase 2: edit transactions ----

using Check = std::function<bool(bool, const char *)>;

void runEditChecks(const Check &check, scripting::ScriptHost &host, SongSession &session,
                   QList<Message> &messages, bool haveExamples)
{
    SongDocument &doc = session.doc;
    SongView &view = *session.view;
    QUndoStack &undo = *doc.undoStack();
    const auto run = [&](const char *code) { return host.evalConsole(QLatin1String(code)); };
    // The caller left a redo entry (tempo edit + undo); park the stack at
    // its top so "no entry" means count == index.
    while (undo.canRedo())
        undo.redo();
    const QByteArray base = doc.smf().write();
    const int index0 = undo.index();
    // Snapshot of the stack (count, index) that "untouched" compares to:
    // taken at the start and after every oneEntryThenUndo, whose undo
    // legitimately leaves a redo entry behind.
    int snapCount = undo.count();
    int snapIndex = undo.index();
    const auto mark = [&] {
        snapCount = undo.count();
        snapIndex = undo.index();
    };
    // One undo entry sits on top of index0, named `name`, and undoing it
    // restores the file byte for byte (leaving the stack at index0).
    const auto oneEntryThenUndo = [&](const char *name, const char *what) {
        bool ok = undo.count() == index0 + 1 && undo.index() == index0 + 1 &&
                  undo.text(index0) == QLatin1String(name);
        check(ok, what);
        undo.undo();
        ok = doc.smf().write() == base && undo.index() == index0;
        check(ok, "undo of a script transaction did not restore the SMF byte for byte");
        mark();
        return ok;
    };
    // The stack is exactly as before: no entry, no redo, same bytes.
    const auto untouched = [&](const char *what) {
        check(undo.count() == snapCount && undo.index() == snapIndex && doc.smf().write() == base,
              what);
    };
    // A transaction that pushed edits and was then rolled back: bytes as
    // before, no entry of its own and no redo (its pushes cleared any
    // earlier redo entry, like every edit does).
    const auto rolledBack = [&](const char *what) {
        check(undo.count() == index0 && undo.index() == index0 && doc.smf().write() == base, what);
        mark();
    };

    // Outside a transaction every edit is refused.
    check(run("porydaw.edit.addNotes(0, [{tick: 0, key: 60, len: 24, vel: 100}])").isNull() &&
              hasMessage(messages, QStringLiteral("console"), 2,
                         QStringLiteral("inside porydaw.edit.transaction")),
          "edit.addNotes outside a transaction was not refused");
    untouched("a refused edit touched the document");
    check(run("porydaw.edit.active") == QStringLiteral("false"), "edit.active true at rest");

    // Lists the C++ side returns are real Arrays. Qt 6.5+ would hand
    // QVariantList/QStringList over as sequence objects (Array.isArray()
    // false, concat() nests them) if the prelude did not normalise them;
    // the id sugar below depends on it, as do plugins.
    check(run("Array.isArray(porydaw.song.tracks()) && Array.isArray(porydaw.song.notes())"
              " && Array.isArray(porydaw.selection.notes())"
              " && Array.isArray(porydaw.audio.engineLimits().mixRates)"
              " && Array.isArray(porydaw.storage.keys())"
              " && porydaw.song.tracks() instanceof Array"
              " && [].concat(porydaw.song.tracks()).length === porydaw.song.tracks().length") ==
              QStringLiteral("true"),
          "API lists did not arrive as real JS Arrays");
    // Several edits, one entry; ids come back and re-resolve; the
    // transaction returns fn's value.
    check(run("var ids = porydaw.edit.transaction('T1', function () {"
              "  if (!porydaw.edit.active) throw new Error('not active');"
              "  var ids = porydaw.edit.addNotes(0, [{tick: 0, key: 60, len: 24, vel: 100},"
              "                                      {tick: 48, key: 62, len: 24, vel: 90}]);"
              "  porydaw.edit.moveNotes(ids, 24, 1);"
              "  porydaw.edit.setVelocity(ids[1], 77);"
              "  porydaw.edit.resizeNotes(ids, 12);"
              "  return ids; });"
              "var a = porydaw.song.note(ids[0]), b = porydaw.song.note(ids[1]);"
              "ids.length === 2 && a.tick === 24 && a.key === 61 && a.len === 36 && a.vel === 100"
              " && b.tick === 72 && b.key === 63 && b.vel === 77 && b.len === 36") ==
              QStringLiteral("true"),
          "edit.addNotes/moveNotes/setVelocity/resizeNotes did not land as expected");
    oneEntryThenUndo("T1", "a transaction with four edits is not exactly one undo entry");

    // A transaction that edits nothing leaves nothing — the redo list
    // included: with an entry undone, a no-op or failed transaction must
    // not clear it (the macro opens lazily, on the first push).
    run("porydaw.edit.transaction('Nothing', function () {})");
    untouched("an empty transaction left an undo entry");
    run("porydaw.edit.transaction('Redo bait', function () {"
        "  porydaw.edit.addNotes(0, [{tick: 0, key: 61, len: 24, vel: 100}]); })");
    undo.undo();
    check(undo.canRedo(), "undo left nothing to redo");
    run("porydaw.edit.transaction('Nothing', function () {})");
    check(run("porydaw.edit.transaction('Fails', function () { throw new Error('x'); })").isNull(),
          "a throwing no-op transaction did not propagate");
    check(undo.canRedo() && undo.count() == index0 + 1 && undo.index() == index0,
          "a no-op / failed transaction discarded the user's redo entry");
    // A transaction that does push clears the redo entry like any edit.
    check(run("porydaw.edit.transaction('Fresh', function () {"
              "  porydaw.edit.addNotes(0, [{tick: 0, key: 61, len: 24, vel: 100}]); }); 'ok'") ==
              QStringLiteral("ok"),
          "transaction after a redo entry failed");
    oneEntryThenUndo("Fresh", "a transaction pushed over a redo entry is not one entry");

    // An exception mid-transaction rolls everything back — no entry, no redo.
    check(run("porydaw.edit.transaction('Boom', function () {"
              "  porydaw.edit.addNotes(0, [{tick: 0, key: 61, len: 24, vel: 100}]);"
              "  throw new Error('boom2'); })")
                  .isNull() &&
              hasMessage(messages, QStringLiteral("console"), 2, QStringLiteral("boom2")),
          "exception inside a transaction did not propagate");
    rolledBack("an exception mid-transaction did not roll the document back");
    check(run("porydaw.edit.active") == QStringLiteral("false"),
          "edit.active stayed true after a rollback");

    // Nested transactions flatten into the outer entry.
    run("porydaw.edit.transaction('Outer', function () {"
        "  porydaw.edit.addNotes(0, [{tick: 0, key: 61, len: 24, vel: 100}]);"
        "  porydaw.edit.transaction('Inner', function () {"
        "    porydaw.edit.addNotes(0, [{tick: 0, key: 62, len: 24, vel: 100}]); }); })");
    oneEntryThenUndo("Outer", "nested transactions did not flatten into one entry");

    // A refused inner edit that the script swallows still aborts the whole
    // transaction at commit.
    check(run("porydaw.edit.transaction('Swallow', function () {"
              "  porydaw.edit.addNotes(0, [{tick: 0, key: 61, len: 24, vel: 100}]);"
              "  try { porydaw.edit.transaction('In', function () { throw new Error('x'); }); }"
              "  catch (e) {} })")
                  .isNull() &&
              hasMessage(messages, QStringLiteral("console"), 2, QStringLiteral("rolled back")),
          "a swallowed inner failure did not abort the outer transaction");
    rolledBack("a swallowed inner failure left edits behind");

    // Revision guard: a foreign edit while the transaction is open.
    HarnessBridge bridge;
    bridge.doc = &doc;
    const scripting::Plugin *console = host.plugin(QStringLiteral("console"));
    if (check(console && console->engine, "console plugin not reachable for the bridge")) {
        QJSEngine::setObjectOwnership(&bridge, QJSEngine::CppOwnership);
        console->engine->globalObject().setProperty(QStringLiteral("harness"),
                                                    console->engine->newQObject(&bridge));
        check(run("porydaw.edit.transaction('Guard', function () {"
                  "  porydaw.edit.addNotes(0, [{tick: 0, key: 61, len: 24, vel: 100}]);"
                  "  harness.externalEdit();"
                  "  porydaw.edit.addNotes(0, [{tick: 0, key: 62, len: 24, vel: 100}]); })")
                      .isNull() &&
                  hasMessage(messages, QStringLiteral("console"), 2,
                             QStringLiteral("changed outside the transaction")),
              "a foreign edit during a transaction did not trip the revision guard");
        rolledBack("the revision-guard rollback did not restore the document");
        // ...and one that lands after the last script edit trips it at commit.
        check(run("porydaw.edit.transaction('Guard2', function () {"
                  "  porydaw.edit.addNotes(0, [{tick: 0, key: 61, len: 24, vel: 100}]);"
                  "  harness.externalEdit(); })")
                  .isNull(),
              "a foreign edit after the last script edit did not fail the commit");
        rolledBack("the commit-time guard rollback did not restore the document");
        console->engine->globalObject().deleteProperty(QStringLiteral("harness"));
    }

    // song.changed is delivered once the mutation's stack has unwound, so a
    // listener may edit: its transaction lands as its own entry after the
    // one that woke it. Another plugin (the fixture, v2) opening a
    // transaction from the same event is fine too.
    run("var offRe = porydaw.song.on('changed', function (e) {"
        "  if (e.origin !== 'script') return;"
        "  porydaw.edit.transaction('Listener', function () {"
        "    porydaw.edit.addNotes(0, [{tick: 0, key: 70, len: 1, vel: 1}]); }); })");
    QSettings().remove(QStringLiteral("plugins/fixture/data/stealErr"));
    QSettings().remove(QStringLiteral("plugins/fixture/data/stealOk"));
    run("porydaw.edit.transaction('Reenter', function () {"
        "  porydaw.edit.addNotes(0, [{tick: 0, key: 61, len: 24, vel: 100}]); })");
    check(undo.count() == index0 + 1 &&
              run("porydaw.song.notes({track: 0, from: 0, to: 1}).some(function (n) { "
                  "return n.key === 70; })") == QStringLiteral("false"),
          "song.changed was delivered inside the transaction that caused it");
    QApplication::processEvents();
    check(undo.count() == index0 + 2 && undo.index() == index0 + 2 &&
              undo.text(index0) == QLatin1String("Reenter") &&
              undo.text(index0 + 1) == QLatin1String("Listener") &&
              run("porydaw.song.notes({track: 0, from: 0, to: 1}).some(function (n) { "
                  "return n.key === 70 && n.len === 1 && n.vel === 1; })") ==
                  QStringLiteral("true"),
          "a listener's transaction did not land as its own entry after the edit");
    run("offRe()");
    // The listener's own edit fired song.changed again (origin "script"),
    // which the listener ignored: exactly two entries, no third.
    QApplication::processEvents();
    check(undo.count() == index0 + 2, "a reactor ignoring origin 'script' still re-edited");
    check(!QSettings().contains(QStringLiteral("plugins/fixture/data/stealErr")) &&
              storedCounter(QStringLiteral("fixture"), QStringLiteral("stealOk")) >= 2 &&
              QSettings()
                  .value(QStringLiteral("plugins/fixture/data/stealOrigin"))
                  .toByteArray()
                  .contains("script"),
          "another plugin's transaction from song.changed was refused");
    undo.undo();
    undo.undo();
    check(doc.smf().write() == base && undo.index() == index0,
          "undoing the listener's and the script's entries did not restore the SMF");
    mark();
    QApplication::processEvents();

    // Coalescing and origins. One counter/origin recorder in the console.
    run("var fires = 0, lastOrigin = null, lastRev = 0;"
        "var offCo = porydaw.song.on('changed', function (e) {"
        "  fires++; lastOrigin = e.origin; lastRev = e.revision; })");
    const auto fired = [&](int count, const char *origin) {
        QApplication::processEvents();
        return run("fires") == QString::number(count) &&
               run("lastOrigin") == QLatin1String(origin) &&
               run("lastRev") == QString::number(doc.revision());
    };
    // Forty edits in one transaction: one event, origin "script".
    run("porydaw.edit.transaction('Forty', function () {"
        "  for (var i = 0; i < 40; i++)"
        "    porydaw.edit.addNotes(0, [{tick: i * 96, key: 100, len: 12, vel: 100}]); })");
    check(fired(1, "script"), "a forty-edit transaction did not coalesce into one 'script' event");
    // Its undo (through the stack, as Edit → Undo does): one event, "history".
    undo.undo();
    check(fired(2, "history"), "undo did not report origin 'history'");
    mark();
    // A direct document edit outside any transaction: "user".
    const int tempo0 = doc.startTempo();
    doc.setStartTempo(tempo0 == 120 ? 121 : 120);
    check(fired(3, "user"), "an interactive edit did not report origin 'user'");
    // A user edit and a script edit in the same turn: "user" wins.
    run("porydaw.edit.transaction('Mixed', function () {"
        "  porydaw.edit.addNotes(0, [{tick: 0, key: 100, len: 12, vel: 100}]); })");
    doc.setStartTempo(tempo0);
    check(fired(4, "user"), "a turn mixing user and script edits did not report 'user'");
    undo.undo();
    undo.undo();
    undo.undo();
    check(fired(5, "history") && doc.smf().write() == base && undo.index() == index0,
          "three undos did not coalesce into one 'history' event that restored the SMF");
    mark();
    // A rolled-back transaction: its pushes and their revert both happen
    // while the transaction is open, so the one event reads "script".
    run("porydaw.edit.transaction('Rollback', function () {"
        "  porydaw.edit.addNotes(0, [{tick: 0, key: 100, len: 12, vel: 100}]);"
        "  throw new Error('undo me'); })");
    rolledBack("the rolled-back origin transaction left a trace");
    check(fired(6, "script"), "a rolled-back transaction did not report one 'script' event");
    // A song switch between the change and its delivery drops the event:
    // song.activated covers it.
    doc.setStartTempo(tempo0 == 120 ? 121 : 120);
    host.setSession(nullptr);
    host.setSession(&session);
    QApplication::processEvents();
    check(run("fires") == QStringLiteral("6"), "a pending song.changed survived a song switch");
    undo.undo();
    check(fired(7, "history") && doc.smf().write() == base, "the post-switch undo did not fire");
    mark();
    run("offCo()");

    // Feedback loop: a reactor that edits on every event (its own included)
    // is faulted past the streak cap; the entries it made undo clean.
    messages.clear();
    run("var offLoop = porydaw.song.on('changed', function (e) {"
        "  porydaw.edit.transaction('Loop', function () {"
        "    porydaw.edit.addNotes(0, [{tick: 0, key: 101, len: 1, vel: 1}]); }); })");
    doc.setStartTempo(tempo0 == 120 ? 121 : 120);
    check(waitFor(
              [&] {
                  return hasMessage(messages, QStringLiteral("console"), 2,
                                    QStringLiteral("feedback loop"));
              },
              4000),
          "a self-triggering song.changed reactor was not faulted");
    check(waitFor([&] { return !console || !console->engine; }, 2000),
          "the looping console engine was not torn down");
    // The tempo edit, the reactor's answer to it (a "user" delivery, which
    // does not count), then nine answers to its own "script" deliveries.
    check(undo.count() == index0 + 11 && undo.index() == index0 + 11,
          "the feedback loop did not stop at the streak cap");
    while (undo.index() > index0)
        undo.undo();
    check(doc.smf().write() == base, "undoing the feedback loop's entries did not restore the SMF");
    mark();
    QApplication::processEvents();
    check(run("porydaw.song.loaded") == QStringLiteral("true") && console &&
              console->state == scripting::PluginState::Loaded,
          "console did not come back after the feedback-loop fault");

    // Bundled Scale Snap: the canonical reactor. Toggled on, a note the
    // user paints outside C major (F#, key 102) snaps to G as its own entry.
    if (haveExamples) {
        const QString toggle = QStringLiteral("plugin.scale-snap.toggle");
        check(host.runCommand(toggle), "scale-snap's toggle command is missing");
        QApplication::processEvents();
        doc.addNote(0, 7, 102, 12, 100);
        QApplication::processEvents();
        check(undo.count() == index0 + 2 && undo.index() == index0 + 2 &&
                  undo.text(index0 + 1) == QLatin1String("Snap to scale") &&
                  run("porydaw.song.notes({track: 0, from: 7, to: 8}).map(function (n) { "
                      "return n.key; }).join()") == QStringLiteral("103"),
              "scale-snap did not snap a freshly painted F# to G");
        // Its own snap fired again (origin "script") and was ignored; an
        // undo of the snap (origin "history") is left alone too.
        undo.undo();
        QApplication::processEvents();
        check(undo.count() == index0 + 2 && undo.index() == index0 + 1 &&
                  run("porydaw.song.notes({track: 0, from: 7, to: 8})[0].key") ==
                      QStringLiteral("102"),
              "scale-snap reacted to the undo of its own snap");
        undo.undo();
        check(doc.smf().write() == base, "undoing the snapped note did not restore the SMF");
        mark();
        check(host.runCommand(toggle), "scale-snap's toggle command did not run twice");
        QApplication::processEvents();
    }

    // Selection ops through the transaction; view/time-selection state.
    check(run("var sel = porydaw.song.notes({track: 0}).slice(0, 2);"
              "porydaw.selection.setNotes(sel);"
              "porydaw.edit.transaction('Up', function () {"
              "  if (!porydaw.edit.transposeSelection(2)) throw new Error('no move'); });"
              "porydaw.selection.notes().every(function (n, i) { return n.key === sel[i].key + 2 "
              "&& n.id === sel[i].id; })") == QStringLiteral("true"),
          "edit.transposeSelection did not move the selected notes (ids kept)");
    oneEntryThenUndo("Up", "edit.transposeSelection is not one undo entry");
    check(run("porydaw.selection.setTime({start: 0, end: 96}); porydaw.selection.time().end") ==
                  QStringLiteral("96") &&
              view.timeSelection().active() && view.timeSelection().endTick == 96,
          "selection.setTime did not reach the view");
    run("porydaw.selection.clearTime()");
    check(!view.timeSelection().active(), "selection.clearTime left the selection");
    check(run("porydaw.selection.setTime({start: 10, end: 5})").isNull(),
          "selection.setTime accepted an empty span");
    check(run("porydaw.view.velocityLane = true; porydaw.view.velocityLane") ==
                  QStringLiteral("true") &&
              view.velocityLaneVisible(),
          "view.velocityLane setter did not reach the view");
    check(run("var vt = porydaw.view.visibleTicks(); vt.to > vt.from") == QStringLiteral("true"),
          "view.visibleTicks is not a span");
    check(run("var mid = Math.floor(porydaw.song.endTick / 2); porydaw.view.revealTick(mid);"
              "var vt2 = porydaw.view.visibleTicks(); vt2.from <= mid && mid <= vt2.to") ==
              QStringLiteral("true"),
          "view.revealTick did not scroll the tick into view");
    run("porydaw.view.revealTick(0)");

    // song.CC names every first-class lane (mid2agb's audible CCs + the
    // pseudo-CCs) so scripts don't hard-code controller numbers.
    check(run("var C = porydaw.song.CC; C.MOD === 1 && C.VOLUME === 7 && C.PAN === 10 && "
              "C.BEND_RANGE === 20 && C.LFO_SPEED === 21 && C.BEND === 0xFF && "
              "C.TEMPO === 0xFE && C.VOICE === 0xFD") == QStringLiteral("true"),
          "song.CC does not name every first-class lane");

    // The rest of the surface in one transaction; undo restores the bytes.
    check(run("porydaw.edit.transaction('Sink', function () {"
              "  var e = porydaw.edit, CC = porydaw.song.CC;"
              "  e.addLanePoint(0, CC.VOLUME, 96, 100);"
              "  e.writeLanePoints(0, CC.PAN, 0, 192, [{tick: 0, value: 10}, {tick: 192, value: "
              "120}]);"
              "  e.moveLanePoints(0, CC.PAN, [{tick: 192, newTick: 144, newValue: 64}]);"
              "  if (e.deleteLanePoints(0, CC.VOLUME, [96]) !== 1) throw new Error('del');"
              "  e.addLanePoint(-1, CC.TEMPO, 480, 150);"
              "  e.setStartTempo(99);"
              "  e.setLoop(96, 960);"
              "  e.setTimeSig(384, 3, 4);"
              "  var t = e.addTrack(5); if (t < 0) throw new Error('addTrack');"
              "  e.renameTrack(t, 'Scripted');"
              "  if (!e.insertTimeRange(0, 96, {tracks: [0]})) throw new Error('insert');"
              "  if (!e.removeTimeRange(0, 96, {tracks: [0]})) throw new Error('remove');"
              "  var m = e.addTrack(5); if (m < 0) throw new Error('addTrack2');"
              "  e.addNotes(m, [{tick: 3, key: 61, len: 6, vel: 90}]);"
              "  if (!e.mergeTrack(m, t, {notesOnly: true})) throw new Error('merge');"
              "  if (porydaw.song.notes({track: t}).length !== 1) throw new Error('merged notes');"
              "  e.deleteTrack(t); }); 'ok'") == QStringLiteral("ok"),
          "the kitchen-sink transaction threw");
    check(run("var p10 = porydaw.song.lanePoints(0, 10, {from: 0, to: 193}); p10.length === 2 && "
              "p10[0].tick === 0 && p10[0].value === 10 && p10[1].tick === 144 && "
              "p10[1].value === 64") == QStringLiteral("true"),
          "writeLanePoints/moveLanePoints did not land as expected");
    check(run("porydaw.song.lanePoints(0, 7, {from: 96, to: 97}).length") == QStringLiteral("0"),
          "addLanePoint + deleteLanePoints did not cancel out");
    check(run("porydaw.song.startTempo === 99 && porydaw.song.lanePoints(-1, "
              "porydaw.song.CC.TEMPO).some(function (p) { return p.tick === 480 && "
              "p.value === 150; })") == QStringLiteral("true"),
          "setStartTempo / tempo addLanePoint did not land");
    check(run("porydaw.song.loop().start === 96 && porydaw.song.loop().end === 960") ==
              QStringLiteral("true"),
          "setLoop did not land");
    check(run("porydaw.song.timeSigs().some(function (s) { return s.tick === 384 && "
              "s.numerator === 3 && s.denominator === 4; })") == QStringLiteral("true"),
          "setTimeSig did not land");
    oneEntryThenUndo("Sink", "the kitchen-sink transaction is not one undo entry");
    check(run("porydaw.edit.transaction('Bad', function () { porydaw.edit.addNotes(99, []); })")
                  .isNull() &&
              hasMessage(messages, QStringLiteral("console"), 2, QStringLiteral("no such track")),
          "edit.addNotes with a bad track was not refused");
    untouched("a refused structural edit left an entry");
    // Argument hygiene: a missing track or a non-object note must throw,
    // not coerce to track 0 / an empty note.
    check(run("porydaw.edit.transaction('Self', function () { porydaw.edit.mergeTrack(0, 0); })")
                  .isNull() &&
              hasMessage(messages, QStringLiteral("console"), 2,
                         QStringLiteral("merged into itself")),
          "edit.mergeTrack(0, 0) was not refused");
    untouched("a refused self-merge left an entry");
    check(run("porydaw.edit.transaction('Undef', function () { porydaw.edit.deleteTrack(); })")
                  .isNull() &&
              hasMessage(messages, QStringLiteral("console"), 2,
                         QStringLiteral("track must be an integer")),
          "edit.deleteTrack() with no track was not refused");
    check(
        run("porydaw.edit.transaction('Undef2', function () { porydaw.edit.addNotes(0); })")
                .isNull() &&
            hasMessage(messages, QStringLiteral("console"), 2, QStringLiteral("must be an object")),
        "edit.addNotes with a non-object note was not refused");
    check(run("porydaw.edit.transaction('Undef3', function () { "
              "porydaw.edit.removeTimeRange(0, 96, {tracks: [undefined]}); })")
              .isNull(),
          "scope.tracks with undefined was not refused");
    untouched("refused argument-hygiene edits left an entry");
    // Two batch entries on one (tick, key): the later wins, the earlier
    // reports id 0.
    check(run("var dup = porydaw.edit.transaction('Dup', function () { return "
              "porydaw.edit.addNotes(0, [{tick: 0, key: 61, len: 24, vel: 100}, "
              "{tick: 0, key: 61, len: 48, vel: 100}]); }); "
              "dup[0] === 0 && dup[1] > 0 && porydaw.song.note(dup[1]).len === 48") ==
              QStringLiteral("true"),
          "duplicate (tick, key) entries in addNotes did not resolve last-wins");
    oneEntryThenUndo("Dup", "the duplicate addNotes transaction is not one entry");
    // selection.setNotes takes one note as well as a list.
    check(run("var one = porydaw.song.notes({track: 0})[0]; porydaw.selection.setNotes(one); "
              "porydaw.selection.notes().length === 1 && porydaw.selection.notes()[0].id === "
              "one.id") == QStringLiteral("true"),
          "selection.setNotes with a single note cleared the selection");

    // Watchdog inside a transaction: the interrupt can't run the script's
    // catch, so the host rolls the open transaction back itself.
    host.setWatchdogMs(200);
    messages.clear();
    run("porydaw.edit.transaction('Runaway', function () {"
        "  porydaw.edit.addNotes(0, [{tick: 0, key: 61, len: 24, vel: 100}]);"
        "  while (true) {} })");
    check(hasMessage(messages, QStringLiteral("console"), 2, QStringLiteral("stopped")) &&
              !host.transaction().open(),
          "an interrupted transaction was not closed");
    check(!hasMessage(messages, QStringLiteral("console"), 2, QStringLiteral("Interrupted")),
          "the rollback fanned song.changed into the interrupted engine");
    rolledBack("an interrupted transaction left its edits or an undo entry");
    check(waitFor([&] { return !console || !console->engine; }, 2000),
          "faulted console engine was not torn down");
    host.setWatchdogMs(5000);
    check(run("porydaw.song.loaded") == QStringLiteral("true") && console &&
              console->state == scripting::PluginState::Loaded,
          "console did not come back after the fault");

    // Bundled Note Tools: each command is one undo entry that undoes clean.
    // A setup transaction adds a track with four notes to work on (the
    // fourth off the grid), so the checks don't depend on the song.
    if (haveExamples) {
        const auto tool = [&](const char *id) {
            return host.runCommand(QStringLiteral("plugin.note-tools.") + QLatin1String(id));
        };
        check(
            run("var st = porydaw.edit.transaction('Setup', function () {"
                "  var t = porydaw.edit.addTrack(0); if (t < 0) throw new Error('no track');"
                "  return {track: t, ids: porydaw.edit.addNotes(t, ["
                "    {tick: 0, key: 60, len: 12, vel: 100}, {tick: 48, key: 62, len: 12, vel: 100},"
                "    {tick: 96, key: 64, len: 12, vel: 100}, {tick: 145, key: 65, len: 12, vel: "
                "100}"
                "  ])}; }); st.ids.length") == QStringLiteral("4") &&
                undo.count() == index0 + 1,
            "note-tools setup transaction failed");
        const int setup = index0 + 1;
        const auto toolEntryThenUndo = [&](const char *id, const char *name, const char *what) {
            run("porydaw.selection.setNotes(st.ids)");
            check(tool(id) && undo.count() == setup + 1 && undo.index() == setup + 1 &&
                      undo.text(setup) == QLatin1String(name),
                  what);
            const bool ok = check(
                hasMessage(messages, QStringLiteral("note-tools"), 2, QStringLiteral("")) == false,
                "a note-tools command logged an error");
            undo.undo();
            check(undo.index() == setup, "undo after a note-tools command did not step back");
            return ok;
        };
        messages.clear();
        run("porydaw.selection.setNotes(st.ids)");
        check(tool("legato") && undo.count() == setup + 1 &&
                  undo.text(setup) == QLatin1String("Legato") &&
                  run("porydaw.song.note(st.ids[0]).len === 48 && "
                      "porydaw.song.note(st.ids[2]).len === 49 && "
                      "porydaw.song.note(st.ids[3]).len === 12") == QStringLiteral("true"),
              "legato did not stretch each note to the next start as one undo entry");
        undo.undo();
        toolEntryThenUndo("humanize", "Humanize velocities", "humanize is not one undo entry");
        run("porydaw.selection.setNotes(st.ids)");
        check(tool("quantize") && undo.count() == setup + 1 &&
                  undo.text(setup) == QLatin1String("Quantize to grid") &&
                  run("porydaw.song.note(st.ids[3]).tick") == QStringLiteral("144"),
              "quantize did not snap the off-grid note as one undo entry");
        undo.undo();
        // Insert a chord at the cursor, then strum it.
        run("porydaw.selection.selectTrack(st.track); porydaw.selection.clear(); "
            "porydaw.cursor.set(192)");
        check(tool("chord") && undo.count() == setup + 1 &&
                  undo.text(setup) == QLatin1String("Insert chord") &&
                  run("var ch = porydaw.selection.notes(); ch.length === 3 && "
                      "ch.every(function (n) { return n.tick === 192; }) && "
                      "ch.map(function (n) { return n.key; }).sort().join() === '60,64,67'") ==
                      QStringLiteral("true"),
              "insert chord did not add a selected triad as one undo entry");
        check(tool("strum") && undo.count() == setup + 2 &&
                  undo.text(setup + 1) == QLatin1String("Strum") &&
                  run("var c = porydaw.selection.notes(); c.length === 3 && "
                      "c.every(function (n) { return n.key === 60 ? n.tick === 192 : "
                      "n.tick > 192 && n.tick + n.len === 192 + ch[0].len; })") ==
                      QStringLiteral("true"),
              "strum did not stagger the chord (ends kept) as one undo entry");
        check(!hasMessage(messages, QStringLiteral("note-tools"), 2, QStringLiteral("")),
              "a note-tools command logged an error");
        undo.undo();
        undo.undo();
        undo.undo();
        check(doc.smf().write() == base && undo.index() == index0,
              "undoing the note-tools runs + setup did not restore the SMF");
    }
    run("porydaw.selection.clear()");
}

} // namespace

namespace {

// ---- Phase 3: docks, canvas, images ----

int countMessages(const QList<Message> &messages, const QString &plugin, const QString &fragment)
{
    int n = 0;
    for (const Message &m : messages) {
        if (m.plugin == plugin && m.text.contains(fragment))
            n++;
    }
    return n;
}

void clickCanvas(QWidget *canvas, const QPoint &pos)
{
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(pos), QPointF(canvas->mapToGlobal(pos)),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(canvas, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(pos), QPointF(canvas->mapToGlobal(pos)),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(canvas, &release);
}

void runPanelChecks(const Check &check, scripting::ScriptHost &host, MainWindow &window,
                    QList<Message> &messages, QMenu *panelsMenu)
{
    const scripting::Plugin *panel = host.plugin(QStringLiteral("panel"));
    if (!check(panel && panel->state == scripting::PluginState::Loaded,
               "panel fixture plugin did not load"))
        return;
    const QString dockName = QStringLiteral("plugin.panel.main");
    QPointer<QDockWidget> dock = window.findChild<QDockWidget *>(dockName);
    if (!check(dock && panel->docks.size() == 1 && panel->docks.front() == dock,
               "ui.dock did not create the panel's dock on the window"))
        return;
    check(window.dockWidgetArea(dock) == Qt::LeftDockWidgetArea && !dock->isHidden(),
          "panel dock did not land in the requested area, shown");
    check(dock->windowTitle() == QStringLiteral("Panel") && dock->titleBarWidget() &&
              dock->titleBarWidget()->inherits("QLabel"),
          "panel dock did not get its title strip");
    check(panelsMenu && panelsMenu->menuAction()->isVisible() &&
              panelsMenu->actions().contains(dock->toggleViewAction()),
          "View → Plugin Panels did not list the dock");
    check(storedCounter(QStringLiteral("panel"), QStringLiteral("imgw")) == 16,
          "ui.loadImage/imageSize did not decode the plugin's image");

    // Every widget primitive is a real QWidget under the dock, and its
    // callback reaches the script.
    // (Searched under the body: the title strip is a QLabel too.)
    QWidget *body = dock->widget();
    auto *label = body->findChild<QLabel *>();
    auto *button = body->findChild<QPushButton *>();
    auto *box = body->findChild<QCheckBox *>();
    auto *slider = body->findChild<QSlider *>();
    auto *combo = body->findChild<QComboBox *>();
    auto *canvas = body->findChild<scripting::CanvasWidget *>();
    if (!check(label && button && box && slider && combo && canvas,
               "build(root) did not create every widget primitive"))
        return;
    check(label->text() == QStringLiteral("hello") && button->text() == QStringLiteral("Go") &&
              combo->count() == 3 && combo->currentIndex() == 1 && slider->value() == 50,
          "widget primitives did not take their initial arguments");
    check(button->focusPolicy() == Qt::NoFocus && canvas->focusPolicy() == Qt::NoFocus,
          "plugin widgets may take keyboard focus away from the roll");
    button->click();
    check(storedCounter(QStringLiteral("panel"), QStringLiteral("clicks")) == 1,
          "button onClick did not reach the script");
    box->setChecked(true);
    check(storedCounter(QStringLiteral("panel"), QStringLiteral("checked")) == 1,
          "checkbox onChange did not reach the script");
    slider->setValue(70);
    check(storedCounter(QStringLiteral("panel"), QStringLiteral("slider")) == 70,
          "slider onChange did not reach the script");
    combo->setCurrentIndex(2);
    check(storedCounter(QStringLiteral("panel"), QStringLiteral("combo")) == 2,
          "combo onChange did not reach the script");
    // The canvas painted the plugin's image at its natural size.
    canvas->resize(64, 48);
    const QImage shot = canvas->grab().toImage();
    check(storedCounter(QStringLiteral("panel"), QStringLiteral("panelpaints")) >= 1 &&
              canvas->paintCount() >= 1 && canvas->errorCount() == 0,
          "canvas paint(g) did not run");
    check(shot.pixelColor(2, 2) == QColor(255, 0, 0) && shot.pixelColor(40, 20) == QColor(0, 0, 0),
          "g.image/g.clear did not paint what the script asked");

    // Dock placement survives a reload through the window state: move
    // it, save, unload (dock gone), restore, reload — it comes back on
    // the right.
    window.addDockWidget(Qt::RightDockWidgetArea, dock);
    const QByteArray state = window.saveState();
    const int menuEntries = panelsMenu->actions().size();
    host.setEnabled(QStringLiteral("panel"), false);
    QApplication::processEvents();
    check(dock.isNull() && !window.findChild<QDockWidget *>(dockName),
          "unloading the plugin did not delete its dock");
    check(panelsMenu->actions().size() == menuEntries - 1,
          "View → Plugin Panels kept the unloaded plugin's entry");
    window.restoreState(state);
    host.setEnabled(QStringLiteral("panel"), true);
    dock = window.findChild<QDockWidget *>(dockName);
    check(dock && window.dockWidgetArea(dock) == Qt::RightDockWidgetArea && !dock->isHidden(),
          "a reloaded plugin's dock did not restore its saved placement, shown");
    // A dock the user closed stays closed across a reload (and, through
    // the same setting, across restarts); reopening it is remembered too.
    // (trigger() is the user's click; setChecked() would be programmatic.)
    dock->toggleViewAction()->trigger();
    check(dock->isHidden(), "the toggle action did not close the dock");
    host.reload(QStringLiteral("panel"));
    dock = window.findChild<QDockWidget *>(dockName);
    check(dock && dock->isHidden(), "a closed plugin dock reopened on reload");
    dock->toggleViewAction()->trigger();
    host.reload(QStringLiteral("panel"));
    dock = window.findChild<QDockWidget *>(dockName);
    check(dock && !dock->isHidden(), "a reopened plugin dock did not stay open on reload");
    // The dock's own close button counts too; a script's hide() does not.
    dock->close();
    host.reload(QStringLiteral("panel"));
    dock = window.findChild<QDockWidget *>(dockName);
    check(dock && dock->isHidden(), "the dock's close button was not remembered");
    dock->toggleViewAction()->trigger();
    check(!dock->isHidden(), "reopening from the menu did not show the dock");
    dock->hide();
    host.reload(QStringLiteral("panel"));
    dock = window.findChild<QDockWidget *>(dockName);
    check(dock && !dock->isHidden(), "a programmatic hide was mistaken for a user close");
    check(!hasMessage(messages, QStringLiteral("panel"), 2, QStringLiteral("")),
          "the panel fixture logged an error");
}

void runDockChecks(const Check &check, scripting::ScriptHost &host, MainWindow &window,
                   QList<Message> &messages)
{
    // Console-built dock: pixel probe, `g` outside paint, mouse, closing.
    host.evalConsole(QStringLiteral("var G = null;"));
    check(!host.evalConsole(QStringLiteral(
                                "var meter = porydaw.ui.dock({id: 'meter', title: 'Meter', "
                                "area: 'left', minWidth: 40, minHeight: 40, paint: function (g) "
                                "{ porydaw.storage.set('paints', porydaw.storage.get('paints', 0) "
                                "+ 1); g.clear('#000000'); g.fillRect(0, 0, 10, 10, 'red'); "
                                "g.text(2, 30, 'hi', 'white', {}); G = g; }, mouse: function (ev) "
                                "{ porydaw.storage.set('mouse', ev.type + ':' + ev.x + ':' + "
                                "ev.button + (ev.type === 'wheel' ? ':' + ev.deltaY : '')); }}); "
                                "meter.id"))
               .isNull(),
          "ui.dock with a paint callback threw");
    QPointer<QDockWidget> dock =
        window.findChild<QDockWidget *>(QStringLiteral("plugin.console.meter"));
    if (!check(dock && window.dockWidgetArea(dock) == Qt::LeftDockWidgetArea,
               "console dock was not created on the left"))
        return;
    auto *canvas = dock->findChild<scripting::CanvasWidget *>();
    if (!check(canvas, "single-paint dock has no canvas"))
        return;
    canvas->resize(48, 48);
    const QImage shot = canvas->grab().toImage();
    check(storedCounter(QStringLiteral("console"), QStringLiteral("paints")) >= 1,
          "console dock paint did not run");
    check(shot.pixelColor(5, 5) == QColor(255, 0, 0) && shot.pixelColor(30, 5) == QColor(0, 0, 0),
          "fillRect/clear pixels are wrong");
    check(host.evalConsole(QStringLiteral("G.fillRect(0, 0, 1, 1, 'red')")).isNull() &&
              hasMessage(messages, QStringLiteral("console"), 2,
                         QStringLiteral("only usable inside paint")),
          "painter used outside paint() was not refused");
    check(host.evalConsole(QStringLiteral("G.width")).toInt() == canvas->width() &&
              canvas->width() >= 16,
          "painter width did not match the canvas");
    clickCanvas(canvas, QPoint(3, 4));
    check(QSettings()
              .value(QStringLiteral("plugins/console/data/mouse"))
              .toByteArray()
              .contains("release:3:left"),
          "mouse events did not reach the script");
    {
        QWheelEvent wheel(QPointF(3, 4), QPointF(canvas->mapToGlobal(QPoint(3, 4))), QPoint(),
                          QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(canvas, &wheel);
        check(QSettings()
                  .value(QStringLiteral("plugins/console/data/mouse"))
                  .toByteArray()
                  .contains("wheel:3:none:1"),
              "wheel events did not reach the script");
    }
    {
        // A trackpad (and a free-spin wheel) delivers a stream of deltas
        // far under one notch each. They must add up to whole steps
        // rather than reaching scripts as unusable hundredths.
        QSettings().remove(QStringLiteral("plugins/console/data/mouse"));
        const auto sendPixels = [canvas](int dy) {
            QWheelEvent ev(QPointF(3, 4), QPointF(canvas->mapToGlobal(QPoint(3, 4))), QPoint(0, dy),
                           QPoint(0, dy * 2), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase,
                           false);
            QCoreApplication::sendEvent(canvas, &ev);
        };
        for (int i = 0; i < 11; ++i) // 11 * (2 px * 5) = 110: still short of a notch
            sendPixels(-2);
        check(QSettings().value(QStringLiteral("plugins/console/data/mouse")).isNull(),
              "a sub-notch trackpad delta reached the script as a fraction of a step");
        sendPixels(-2);
        check(QSettings()
                  .value(QStringLiteral("plugins/console/data/mouse"))
                  .toByteArray()
                  .contains("wheel:3:none:1"),
              "accumulated trackpad deltas did not add up to one wheel step");
    }
    check(host.evalConsole(QStringLiteral("meter.visible")) == QStringLiteral("true"),
          "dock.visible did not read true");
    host.evalConsole(QStringLiteral("meter.hide()"));
    check(dock->isHidden() &&
              host.evalConsole(QStringLiteral("meter.visible")) == QStringLiteral("false"),
          "dock.hide() did not hide the dock");
    host.evalConsole(QStringLiteral("meter.title = 'Renamed'"));
    check(dock->windowTitle() == QStringLiteral("Renamed"), "dock.title setter did not apply");
    // Theme colors.
    check(host.evalConsole(QStringLiteral("porydaw.ui.theme('window_text')"))
                  .startsWith(QLatin1Char('#')) &&
              host.evalConsole(QStringLiteral("Object.keys(porydaw.ui.theme()).length > 10")) ==
                  QStringLiteral("true"),
          "ui.theme did not return colors");
    check(host.evalConsole(QStringLiteral("porydaw.ui.theme('scrollbar_handle')")).isNull(),
          "ui.theme accepted an unexposed role");
    // Refusals.
    check(host.evalConsole(QStringLiteral("porydaw.ui.dock({id: 'meter', paint: function () {}})"))
              .isNull(),
          "a duplicate dock id was accepted");
    check(host.evalConsole(QStringLiteral("porydaw.ui.dock({id: 'bad id', paint: function () {}})"))
              .isNull(),
          "a bad dock id was accepted");
    check(host.evalConsole(QStringLiteral("porydaw.ui.dock({id: 'nothing'})")).isNull(),
          "a dock without paint or build was accepted");
    check(host.evalConsole(QStringLiteral(
                               "porydaw.ui.dock({id: 'x', area: 'middle', paint: function () {}})"))
              .isNull(),
          "a bad dock area was accepted");
    check(host.evalConsole(QStringLiteral("porydaw.ui.loadImage('nope.png')")).isNull(),
          "loadImage in the folder-less console did not throw");
    host.evalConsole(QStringLiteral("meter.close()"));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    check(dock.isNull() &&
              host.evalConsole(QStringLiteral("meter.open")) == QStringLiteral("false"),
          "dock.close() did not dispose the dock");

    // A paint that throws is logged once, then held back for a moment.
    host.evalConsole(QStringLiteral(
        "var thrower = porydaw.ui.dock({id: 'thrower', paint: function (g) { throw new "
        "Error('paint boom'); }})"));
    QDockWidget *throwerDock =
        window.findChild<QDockWidget *>(QStringLiteral("plugin.console.thrower"));
    auto *throwerCanvas =
        throwerDock ? throwerDock->findChild<scripting::CanvasWidget *>() : nullptr;
    if (check(throwerCanvas, "thrower dock missing")) {
        throwerCanvas->grab();
        throwerCanvas->grab();
        check(countMessages(messages, QStringLiteral("console"), QStringLiteral("paint boom")) ==
                      1 &&
                  throwerCanvas->errorCount() == 1,
              "a throwing paint was not logged exactly once per hold");
        host.evalConsole(QStringLiteral("thrower.close()"));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    // The watchdog reaches into paint too: a hung paint faults the plugin
    // and its docks go with it.
    host.setWatchdogMs(200);
    host.evalConsole(QStringLiteral(
        "var hang = porydaw.ui.dock({id: 'hang', paint: function (g) { while (true) {} }})"));
    QDockWidget *hangDock = window.findChild<QDockWidget *>(QStringLiteral("plugin.console.hang"));
    auto *hangCanvas = hangDock ? hangDock->findChild<scripting::CanvasWidget *>() : nullptr;
    if (check(hangCanvas, "hang dock missing")) {
        QElapsedTimer clock;
        clock.start();
        hangCanvas->grab();
        const scripting::Plugin *console = host.plugin(QStringLiteral("console"));
        check(clock.elapsed() < 5000 && console && console->state == scripting::PluginState::Error,
              "a hung paint was not interrupted");
        check(waitFor(
                  [&] {
                      return !window.findChild<QDockWidget *>(
                          QStringLiteral("plugin.console.hang"));
                  },
                  2000),
              "a faulted plugin's dock was not torn down");
    }
    host.setWatchdogMs(5000);
    check(host.evalConsole(QStringLiteral("1 + 1")) == QStringLiteral("2"),
          "the console did not revive after a paint fault");
}

// ---- Phase 3: audio frames + transport beats ----

void runRealtimeChecks(const Check &check, scripting::ScriptHost &host, MainWindow &window,
                       QList<Message> &messages, bool audioOk)
{
    runDockChecks(check, host, window, messages);
    if (!audioOk)
        return;
    // Let the bundled examples run against real playback first, and keep
    // a picture of each panel when asked (PORYDAW_SCRIPTCHECK_SHOTS=<dir>)
    // — the only way to eyeball a plugin's painting headlessly.
    const QString shotsDir = qEnvironmentVariable("PORYDAW_SCRIPTCHECK_SHOTS");
    if (host.plugin(QStringLiteral("vu-meter"))) {
        host.evalConsole(QStringLiteral("porydaw.transport.play()"));
        waitFor([] { return false; }, 1500);
        for (const char *id : {"vu-meter", "spectrum", "dancer"}) {
            const scripting::Plugin *p = host.plugin(QLatin1String(id));
            if (!p || p->docks.empty() || !p->docks.front())
                continue;
            auto *canvas = p->docks.front()->findChild<scripting::CanvasWidget *>();
            if (!check(canvas && canvas->paintCount() > 0 && canvas->errorCount() == 0,
                       "an example panel did not paint cleanly during playback"))
                continue;
            if (!shotsDir.isEmpty()) {
                canvas->resize(240, 160);
                canvas->grab().save(shotsDir + QLatin1Char('/') + QLatin1String(id) +
                                    QStringLiteral(".png"));
            }
        }
        host.evalConsole(QStringLiteral("porydaw.transport.stop()"));
    }
    // The bundled examples listen for frames; park them so the timer
    // gating below sees only the console's listeners (and unloading them
    // must drop their listener counts).
    QStringList parked;
    for (const char *id : {"vu-meter", "spectrum", "dancer"}) {
        const scripting::Plugin *p = host.plugin(QLatin1String(id));
        if (p && p->state == scripting::PluginState::Loaded) {
            parked.append(QLatin1String(id));
            host.setEnabled(QLatin1String(id), false);
        }
    }
    check(!host.frameTimerActive(), "the frame timer runs with nobody listening");
    // With the examples parked and the console's docks gone, the menu
    // lists only the panel fixture; parking that too empties and hides it.
    if (auto *panels = window.findChild<QMenu *>(QStringLiteral("viewPluginPanelsMenu"))) {
        check(panels->actions().size() == 1 && panels->menuAction()->isVisible(),
              "View → Plugin Panels did not list exactly the one open plugin dock");
        host.setEnabled(QStringLiteral("panel"), false);
        check(panels->actions().isEmpty() && !panels->menuAction()->isVisible(),
              "View → Plugin Panels stayed visible after the last plugin dock closed");
        host.setEnabled(QStringLiteral("panel"), true);
    }
    check(!host
               .evalConsole(QStringLiteral(
                   "var offFrame = porydaw.audio.on('frame', function (f) { "
                   "var v = Math.round(Math.max(f.rms[0], f.rms[1]) * 100000); "
                   "if (v > porydaw.storage.get('rms', 0)) porydaw.storage.set('rms', v); "
                   "porydaw.storage.set('frames', porydaw.storage.get('frames', 0) + 1); "
                   "if (f.frames > 0) porydaw.storage.set('fresh', 1); });"))
               .isNull(),
          "audio.on('frame') threw");
    check(host.frameTimerActive(), "an audio.frame listener did not start the frame timer");
    host.evalConsole(
        QStringLiteral("var offTick = porydaw.transport.on('tick', function (t) { "
                       "porydaw.storage.set('ticks', porydaw.storage.get('ticks', 0) + 1); "
                       "if (t.playing) porydaw.storage.set('tickplaying', 1); });"
                       "var offBeat = porydaw.transport.on('beat', function (b) { "
                       "porydaw.storage.set('beats', porydaw.storage.get('beats', 0) + 1); "
                       "porydaw.storage.set('bpb', b.beatsPerBar); "
                       "porydaw.storage.set('beatidx', b.bar * b.beatsPerBar + b.beat); "
                       "if (b.bpm > 0 && b.beatTicks > 0) porydaw.storage.set('beatok', 1); });"));
    host.evalConsole(QStringLiteral("porydaw.transport.play()"));
    const auto counter = [](const char *key) {
        return storedCounter(QStringLiteral("console"), QLatin1String(key));
    };
    // The null device advances in real time; a couple of seconds of the
    // fixture song yield audible RMS and at least two beats.
    check(waitFor([&] { return counter("rms") > 0 && counter("beats") >= 2; }, 6000),
          "no non-zero RMS / second beat arrived while the song played");
    check(counter("frames") > 0 && counter("fresh") == 1, "audio frames carried no new samples");
    check(counter("ticks") > 0 && counter("tickplaying") == 1,
          "transport.tick did not fire while playing");
    check(counter("bpb") > 0 && counter("beatok") == 1 && counter("beatidx") >= 1,
          "transport.beat payload is malformed");
    check(host.evalConsole(QStringLiteral("porydaw.audio.pcm() instanceof Float32Array && "
                                          "porydaw.audio.pcm().length === "
                                          "porydaw.audio.windowFrames * 2")) ==
              QStringLiteral("true"),
          "audio.pcm() is not a Float32Array of the window");
    check(host.evalConsole(QStringLiteral("porydaw.audio.spectrum(32).length")) ==
              QStringLiteral("32"),
          "audio.spectrum(32) did not return 32 bands");
    check(host.evalConsole(QStringLiteral("porydaw.audio.spectrum(32).some(function (v) { "
                                          "return v > 0; })")) == QStringLiteral("true"),
          "audio.spectrum is silent while the song plays");
    check(host.evalConsole(QStringLiteral("porydaw.audio.channels().pcm.length > 0")) ==
              QStringLiteral("true"),
          "audio.channels() has no pcm pool");
    check(host.evalConsole(QStringLiteral("porydaw.audio.peak.length === 2 && "
                                          "porydaw.audio.rms.length === 2")) ==
              QStringLiteral("true"),
          "audio.peak/rms are not stereo pairs");
    host.evalConsole(QStringLiteral("porydaw.transport.stop()"));
    // A stop then play restarts the beat sequence (the first beat fires
    // again).
    const int beatsBefore = counter("beats");
    host.evalConsole(QStringLiteral("porydaw.transport.play()"));
    check(waitFor([&] { return counter("beats") > beatsBefore; }, 2000),
          "restarting playback did not re-fire the starting beat");
    host.evalConsole(QStringLiteral("porydaw.transport.stop()"));
    host.evalConsole(QStringLiteral("offFrame(); offTick(); offBeat();"));
    check(!host.frameTimerActive(), "removing every listener did not stop the frame timer");
    for (const QString &id : parked)
        host.setEnabled(id, true);
    if (!parked.isEmpty())
        check(host.frameTimerActive(), "re-enabled examples did not restart the frame timer");
    // The examples' listeners ran during the song half; none may have thrown.
    for (const QString &id : parked)
        check(!hasMessage(messages, id, 2, QStringLiteral("")),
              "an example plugin logged an error");
}

// ---- Phase 3: the tap ring and analyzer, no engine ----

// ---- Phase 4: menus, context menus, dialogs, io, song storage, raw
// events, range moves, overlays, render, project.open ----

// Runs `drive` on the next modal dialog (a script's ui.dialog.* call
// blocks in exec(), so the driver has to come from the event loop).
void driveNextModal(const std::function<void(QWidget *)> &drive, QDeadlineTimer deadline)
{
    QTimer::singleShot(20, [drive, deadline] {
        if (QWidget *w = QApplication::activeModalWidget()) {
            drive(w);
            return;
        }
        if (!deadline.hasExpired())
            driveNextModal(drive, deadline);
    });
}

void driveNextModal(const std::function<void(QWidget *)> &drive)
{
    driveNextModal(drive, QDeadlineTimer(3000));
}

// The open popup menu, if any (a top-level scan: activePopupWidget is
// not reliable offscreen).
QMenu *openMenu()
{
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (auto *menu = qobject_cast<QMenu *>(w)) {
            if (menu->isVisible())
                return menu;
        }
    }
    return nullptr;
}

// Same for a popup menu (QMenu::exec / popup).
void driveNextPopup(const std::function<void(QMenu *)> &drive, QDeadlineTimer deadline)
{
    // A 0 ms poll: offscreen, an exec()'d menu can dismiss itself within
    // a few ms, so the first event-loop turn inside exec() is the moment.
    QTimer::singleShot(0, [drive, deadline] {
        if (QMenu *menu = openMenu()) {
            drive(menu);
            return;
        }
        if (!deadline.hasExpired())
            driveNextPopup(drive, deadline);
    });
}

QAction *actionNamed(const QList<QAction *> &actions, const QString &prefix)
{
    for (QAction *a : actions) {
        if (a->text() == prefix || a->text().startsWith(prefix + QLatin1Char('\t')))
            return a;
    }
    return nullptr;
}

QJsonObject readJson(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QJsonObject();
    return QJsonDocument::fromJson(file.readAll()).object();
}

bool isRed(const QColor &c)
{
    return c.red() > 200 && c.green() < 60 && c.blue() < 60;
}

void runReachChecks(const Check &check, scripting::ScriptHost &host, MainWindow &window,
                    SongSession &session, QList<Message> &messages, const QString &pluginsDir,
                    const QString &projectRoot, const QString &songLabel)
{
    SongDocument &doc = session.doc;
    SongView &view = *session.view;
    QUndoStack &undo = *doc.undoStack();
    auto &keys = keymap::Registry::instance();
    const auto run = [&](const QString &code) { return host.evalConsole(code); };
    const auto runc = [&](const char *code) { return host.evalConsole(QLatin1String(code)); };
    const auto errorLogged = [&](const char *fragment) {
        return hasMessage(messages, QStringLiteral("console"), 2, QLatin1String(fragment));
    };
    while (undo.canRedo())
        undo.redo();
    const QByteArray base = doc.smf().write();
    const int index0 = undo.index();
    // One entry named `name` on top of index0; undoing it restores the
    // file byte for byte.
    const auto oneEntryThenUndo = [&](const char *name, const char *what) {
        check(undo.count() == index0 + 1 && undo.index() == index0 + 1 &&
                  undo.text(index0) == QLatin1String(name),
              what);
        undo.undo();
        check(doc.smf().write() == base && undo.index() == index0,
              "undo of a Phase 4 transaction did not restore the SMF byte for byte");
    };
    const auto untouched = [&](const char *what) {
        check(undo.index() == index0 && doc.smf().write() == base, what);
    };
    const int watchdogBefore = host.watchdogMs();

    // --- Plugins menu ---
    auto *pluginsMenu = window.findChild<QMenu *>(QStringLiteral("pluginsMenu"));
    if (!check(pluginsMenu, "the window has no Plugins menu"))
        return;
    check(run(QStringLiteral(
              "var M = porydaw.ui.menu(); var MI = M.addItem({label: 'Hi there', tooltip: "
              "'tip', run: function () { porydaw.storage.set('menuran', "
              "porydaw.storage.get('menuran', 0) + 1 + (porydaw.song.loaded ? 0 : 100)); }}); "
              "MI.label")) == QStringLiteral("Hi there"),
          "ui.menu().addItem did not return a handle with the label");
    QPointer<QMenu> consoleMenu = window.findChild<QMenu *>(QStringLiteral("plugin.console.menu"));
    if (!check(consoleMenu && consoleMenu->title() == QStringLiteral("Script Console") &&
                   pluginsMenu->actions().contains(consoleMenu->menuAction()) &&
                   pluginsMenu->menuAction()->isVisible(),
               "the plugin's submenu did not appear under a visible Plugins menu"))
        return;
    QAction *hi = actionNamed(consoleMenu->actions(), QStringLiteral("Hi there"));
    check(hi && hi->toolTip() == QStringLiteral("tip"), "menu item action missing or no tooltip");
    if (hi)
        hi->trigger();
    check(storedCounter(QStringLiteral("console"), QStringLiteral("menuran")) == 1,
          "triggering a menu item did not run its callback");
    // Linked to a registered command: shows the binding, follows rebinds,
    // runs the command.
    check(run(QStringLiteral("var AID = porydaw.actions.register({id: 'mi', name: 'Menu Item "
                             "Cmd', context: 'roll', default: 'Ctrl+Alt+Shift+F6', run: function "
                             "() { porydaw.storage.set('micmd', porydaw.storage.get('micmd', 0) + "
                             "1); }}); var LI = M.addItem({label: 'Linked', action: AID}); "
                             "LI.label")) == QStringLiteral("Linked"),
          "menu item linked to an action was refused");
    QAction *linked = actionNamed(consoleMenu->actions(), QStringLiteral("Linked"));
    check(linked && linked->text() == QStringLiteral("Linked\tCtrl+Alt+Shift+F6") &&
              linked->shortcut().isEmpty(),
          "linked item does not show the binding as a display-only hint");
    keys.setBinding(QStringLiteral("plugin.console.mi"),
                    QKeySequence(QStringLiteral("Ctrl+Alt+Shift+F5")));
    check(linked && linked->text() == QStringLiteral("Linked\tCtrl+Alt+Shift+F5"),
          "linked item did not follow a rebind");
    keys.resetBinding(QStringLiteral("plugin.console.mi"));
    if (linked)
        linked->trigger();
    check(storedCounter(QStringLiteral("console"), QStringLiteral("micmd")) == 1,
          "triggering a linked item did not run the command");
    // Checkable items, submenus, separators, remove, enabled, clear.
    check(run(QStringLiteral("var CI = M.addItem({label: 'Check', checkable: true, checked: "
                             "true, run: function (on) { porydaw.storage.set('checkon', on === "
                             "true ? 1 : on === false ? 2 : 0); }}); var SUB = M.addMenu('Sub'); "
                             "SUB.addItem({label: 'Deep', "
                             "run: function () { porydaw.storage.set('deep', 1); }}); "
                             "M.addSeparator(); CI.checked")) == QStringLiteral("true"),
          "checkable item / submenu / separator setup failed");
    QAction *checkA = actionNamed(consoleMenu->actions(), QStringLiteral("Check"));
    check(checkA && checkA->isCheckable() && checkA->isChecked(), "checkable item not checkable");
    if (checkA)
        checkA->trigger();
    // 2 = the callback received an explicit false (0 would mean it got
    // nothing at all).
    check(storedCounter(QStringLiteral("console"), QStringLiteral("checkon")) == 2 &&
              run(QStringLiteral("CI.checked")) == QStringLiteral("false"),
          "toggling a checkable item did not pass the new state");
    run(QStringLiteral("CI.checked = true"));
    check(checkA && checkA->isChecked() &&
              storedCounter(QStringLiteral("console"), QStringLiteral("checkon")) == 2,
          "setting checked from the script fired the callback or did not apply");
    QMenu *sub = nullptr;
    for (QAction *a : consoleMenu->actions()) {
        if (a->menu() && a->menu()->title() == QStringLiteral("Sub"))
            sub = a->menu();
    }
    check(sub, "addMenu did not add a submenu");
    if (QAction *deep = sub ? actionNamed(sub->actions(), QStringLiteral("Deep")) : nullptr)
        deep->trigger();
    check(storedCounter(QStringLiteral("console"), QStringLiteral("deep")) == 1,
          "submenu item did not run");
    check(run(QStringLiteral("MI.remove(); MI.label")) == QStringLiteral("Hi there") &&
              !actionNamed(consoleMenu->actions(), QStringLiteral("Hi there")),
          "item.remove() did not take the entry out (or killed the handle)");
    // An item that removes itself (and one that clears its menu) from its
    // own run(): the action must outlive the triggered handler.
    check(run(QStringLiteral("var RM = M.addItem({label: 'RmMe', run: function () { RM.remove(); "
                             "porydaw.storage.set('rmme', 1); }}); var SM = M.addMenu('Gone'); "
                             "SM.addItem({label: 'ClearMe', run: function () { M.clear(); "
                             "porydaw.storage.set('clearme', 1); }}); 'ok'")) ==
              QStringLiteral("ok"),
          "self-removing items could not be added");
    if (QAction *rm = actionNamed(consoleMenu->actions(), QStringLiteral("RmMe")))
        rm->trigger();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    check(storedCounter(QStringLiteral("console"), QStringLiteral("rmme")) == 1 &&
              !actionNamed(consoleMenu->actions(), QStringLiteral("RmMe")),
          "an item removing itself from run() did not run or stayed listed");
    {
        QMenu *gone = nullptr;
        for (QAction *a : consoleMenu->actions()) {
            if (a->menu() && a->menu()->title() == QStringLiteral("Gone"))
                gone = a->menu();
        }
        if (QAction *cm = gone ? actionNamed(gone->actions(), QStringLiteral("ClearMe")) : nullptr)
            cm->trigger();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(storedCounter(QStringLiteral("console"), QStringLiteral("clearme")) == 1 &&
                  consoleMenu->actions().isEmpty(),
              "clear() from a submenu item's run() did not run or left entries");
    }
    // The menu is rebuilt for the checks below.
    run(QStringLiteral("var CI = M.addItem({label: 'Check', checkable: true, checked: true, run: "
                       "function (on) { porydaw.storage.set('checkon', on ? 1 : 0); }}); "
                       "var SUB = M.addMenu('Sub'); SUB.addItem({label: 'Deep', run: function () { "
                       "porydaw.storage.set('deep', 1); }}); M.addSeparator();"));
    QAction *checkA2 = actionNamed(consoleMenu->actions(), QStringLiteral("Check"));
    checkA = checkA2;
    check(run(QStringLiteral("CI.enabled = false; CI.enabled")) == QStringLiteral("false") &&
              checkA && !checkA->isEnabled(),
          "item.enabled did not reach the action");
    check(run(QStringLiteral("M.addItem({run: function () {}})")).isNull() &&
              errorLogged("needs a label"),
          "an item without a label was accepted");
    check(run(QStringLiteral("M.addItem({label: 'x', action: 'plugin.nope.x'})")).isNull() &&
              errorLogged("no command"),
          "an item linked to an unknown command was accepted");
    check(run(QStringLiteral("M.addItem({label: 'x'})")).isNull() &&
              errorLogged("needs run() or an action"),
          "an item with neither run nor action was accepted");
    check(run(QStringLiteral("porydaw.ui.contextMenu('bogus')")).isNull() &&
              errorLogged("surface must be"),
          "a bogus context-menu surface was accepted");
    run(QStringLiteral("M.clear()"));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    check(consoleMenu->actions().isEmpty(), "menu.clear() left entries behind");
    check(run(QStringLiteral("SUB.addItem({label: 'late', run: function () {}})")).isNull() &&
              errorLogged("menu was removed"),
          "adding to a cleared submenu was not refused");

    // --- context menus ---
    check(run(QStringLiteral(
              "var RC = porydaw.ui.contextMenu('range'); RC.addItem({label: 'Range plug', run: "
              "function () { porydaw.storage.set('ctxrange', porydaw.selection.time().end); "
              "}}); var NC = porydaw.ui.contextMenu('notes'); NC.addItem({label: 'Note plug', "
              "run: function () { porydaw.storage.set('ctxnote', "
              "porydaw.selection.notes()[0].key); }}); 'ok'")) == QStringLiteral("ok"),
          "contextMenu items could not be added");
    view.selectTrack(0);
    run(QStringLiteral("porydaw.selection.setTime({start: 0, end: 96})"));
    bool sawSeparator = false;
    driveNextPopup(
        [&](QMenu *menu) {
            QAction *plug = actionNamed(menu->actions(), QStringLiteral("Range plug"));
            QAction *clear = actionNamed(menu->actions(), QStringLiteral("Clear selection"));
            // Plugin entries (every plugin's, the examples' included) sit
            // in one block after the built-ins, behind a separator.
            const int at = plug ? menu->actions().indexOf(plug) : -1;
            const int clearAt = clear ? menu->actions().indexOf(clear) : -1;
            for (int i = clearAt + 1; clearAt >= 0 && i < at; ++i)
                sawSeparator = sawSeparator || menu->actions().at(i)->isSeparator();
            if (plug)
                plug->trigger();
            menu->close();
        },
        QDeadlineTimer(3000));
    view.showTimeSelectionMenu(view.mapToGlobal(QPoint(200, 100)));
    check(storedCounter(QStringLiteral("console"), QStringLiteral("ctxrange")) == 96 &&
              sawSeparator,
          "the range menu did not show the plugin entry (after a separator) or run it");
    run(QStringLiteral("porydaw.selection.clearTime()"));
    // The note menu: draw a note, select it, right-click it on the roll.
    check(run(QStringLiteral("var NID = porydaw.edit.transaction('Note for menu', function () { "
                             "return porydaw.edit.addNotes(0, [{tick: 48, key: 60, len: 48, vel: "
                             "100}])[0]; }); porydaw.selection.setNotes(NID); "
                             "porydaw.view.revealRange(0, 192); "
                             "porydaw.view.revealNote(NID)")) == QStringLiteral("true"),
          "could not place the note for the context-menu test");
    QWidget &roll = view.timelineSurfaces().roll.widget;
    const auto rightClickNote = [&](double tick, int key) {
        const QPointF pos(songview::kKeyboardW + view.contentX(tick),
                          (127 - key) * view.keyHeight() - view.scrollY() + view.keyHeight() / 2);
        QMouseEvent press(QEvent::MouseButtonPress, pos, roll.mapToGlobal(pos.toPoint()),
                          Qt::RightButton, Qt::RightButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&roll, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, pos, roll.mapToGlobal(pos.toPoint()),
                            Qt::RightButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&roll, &release);
        return openMenu();
    };
    QMenu *noteMenu = rightClickNote(72, 60);
    QAction *notePlug =
        noteMenu ? actionNamed(noteMenu->actions(), QStringLiteral("Note plug")) : nullptr;
    check(notePlug, "right-clicking a note did not open a menu with the plugin entry");
    if (notePlug)
        notePlug->trigger();
    if (noteMenu)
        noteMenu->close();
    check(storedCounter(QStringLiteral("console"), QStringLiteral("ctxnote")) == 60,
          "the note menu's plugin entry did not run with the selection");
    noteMenu = rightClickNote(72, 60);
    int plugCount = 0;
    if (noteMenu) {
        for (QAction *a : noteMenu->actions())
            plugCount += a->text() == QStringLiteral("Note plug");
        noteMenu->close();
    }
    check(plugCount == 1, "reopening the note menu duplicated the plugin entries");
    oneEntryThenUndo("Note for menu", "the note transaction is not one entry");
    if (host.plugin(QStringLiteral("note-tools"))) {
        // note-tools' Legato entry is gated on shouldShow() (two or more
        // selected notes): out with one note selected, in with two.
        const auto noteMenuHas = [&](const QString &label) {
            QMenu *menu = rightClickNote(72, 60);
            const bool found = menu && actionNamed(menu->actions(), label);
            if (menu)
                menu->close();
            return found;
        };
        check(run(QStringLiteral(
                  "var NIDS = porydaw.edit.transaction('Notes for shouldShow', function () { "
                  "return porydaw.edit.addNotes(0, [{tick: 48, key: 60, len: 48, vel: 100}, "
                  "{tick: 144, key: 64, len: 48, vel: 100}]); }); "
                  "porydaw.selection.setNotes(NIDS[0]); porydaw.view.revealRange(0, 192); "
                  "porydaw.view.revealNote(NIDS[0])")) == QStringLiteral("true"),
              "could not place the notes for the example shouldShow test");
        check(!noteMenuHas(QStringLiteral("Legato")) &&
                  noteMenuHas(QStringLiteral("Humanize velocities")),
              "note-tools' Legato showed for a single note (or Humanize was missing)");
        check(run(QStringLiteral("porydaw.selection.setNotes(NIDS); "
                                 "porydaw.selection.notes().length")) == QStringLiteral("2") &&
                  noteMenuHas(QStringLiteral("Legato")),
              "note-tools' Legato stayed hidden for two selected notes");
        oneEntryThenUndo("Notes for shouldShow", "the example notes transaction is not one entry");
    }
    // shouldShow(): asked as the menu opens; a false verdict leaves the
    // entry out of that opening only. A throwing predicate keeps the
    // entry (logged), and `visible = false` wins over a true verdict.
    check(run(QStringLiteral(
              "var SS = porydaw.ui.contextMenu('range').addItem({label: 'Maybe', run: function "
              "() {}, shouldShow: function () { porydaw.storage.set('asked', "
              "(porydaw.storage.get('asked') || 0) + 1); return porydaw.storage.get('want') == "
              "1; }}); var TS = porydaw.ui.contextMenu('range').addItem({label: 'Throws', run: "
              "function "
              "() {}, shouldShow: function () { throw new Error('boom'); }}); 'ok'")) ==
              QStringLiteral("ok"),
          "shouldShow items could not be added");
    run(QStringLiteral("porydaw.selection.setTime({start: 0, end: 96})"));
    const auto rangeMenuHas = [&](const QString &label) {
        bool found = false;
        driveNextPopup(
            [&](QMenu *menu) {
                found = actionNamed(menu->actions(), label) != nullptr;
                menu->close();
            },
            QDeadlineTimer(3000));
        view.showTimeSelectionMenu(view.mapToGlobal(QPoint(200, 100)));
        return found;
    };
    check(!rangeMenuHas(QStringLiteral("Maybe")) &&
              storedCounter(QStringLiteral("console"), QStringLiteral("asked")) == 1,
          "a false shouldShow() did not hide the entry (or was not asked once)");
    run(QStringLiteral("porydaw.storage.set('want', 1)"));
    check(rangeMenuHas(QStringLiteral("Maybe")) &&
              storedCounter(QStringLiteral("console"), QStringLiteral("asked")) == 2,
          "a true shouldShow() did not show the entry on the next opening");
    check(rangeMenuHas(QStringLiteral("Throws")) && errorLogged("boom"),
          "a throwing shouldShow() hid the entry or went unlogged");
    run(QStringLiteral("TS.remove()"));
    check(run(QStringLiteral("SS.visible = false; 'ok'")) == QStringLiteral("ok") &&
              !rangeMenuHas(QStringLiteral("Maybe")),
          "visible = false did not win over a true shouldShow()");
    run(QStringLiteral("SS.remove(); porydaw.selection.clearTime()"));
    // The same for a menu-bar item, asked on the menu's aboutToShow.
    check(run(QStringLiteral(
              "var MS = M.addItem({label: 'MaybeBar', run: function () {}, shouldShow: function "
              "() { return porydaw.storage.get('wantbar') == 1; }}); 'ok'")) ==
              QStringLiteral("ok"),
          "a menu-bar shouldShow item could not be added");
    QAction *maybeBar = actionNamed(consoleMenu->actions(), QStringLiteral("MaybeBar"));
    emit consoleMenu->aboutToShow();
    check(maybeBar && !maybeBar->isVisible(),
          "a false shouldShow() left the menu-bar entry visible");
    run(QStringLiteral("porydaw.storage.set('wantbar', 1)"));
    emit consoleMenu->aboutToShow();
    check(maybeBar && maybeBar->isVisible(),
          "a true shouldShow() did not bring the menu-bar entry back");
    run(QStringLiteral("MS.remove()"));

    // --- dialogs ---
    check(run(QStringLiteral("porydaw.edit.transaction('D', function () { "
                             "porydaw.ui.dialog.alert('no'); })"))
                  .isNull() &&
              errorLogged("inside a transaction"),
          "a dialog inside a transaction was not refused");
    untouched("the refused dialog's transaction touched the document");
    // A dialog held longer than the budget must not trip the watchdog.
    host.setWatchdogMs(200);
    QString alertText;
    driveNextModal([&](QWidget *w) {
        QTimer::singleShot(500, w, [w, &alertText] {
            auto *box = qobject_cast<QMessageBox *>(w);
            if (!box)
                return;
            alertText = box->text();
            box->buttons().first()->click();
        });
    });
    check(run(QStringLiteral("porydaw.ui.dialog.alert('Hello', {title: 'T'}); 'alive'")) ==
                  QStringLiteral("alive") &&
              alertText == QStringLiteral("Hello") && !errorLogged("interrupted"),
          "alert did not show, or the watchdog fired during the dialog");
    host.setWatchdogMs(watchdogBefore);
    const auto clickButton = [](const QString &text) {
        return [text](QWidget *w) {
            auto *box = qobject_cast<QMessageBox *>(w);
            if (!box)
                return;
            for (QAbstractButton *b : box->buttons()) {
                if (b->text() == text)
                    b->click();
            }
        };
    };
    driveNextModal(clickButton(QStringLiteral("Sure")));
    check(run(QStringLiteral("porydaw.ui.dialog.confirm('Q?', {ok: 'Sure', cancel: 'Nah'})")) ==
              QStringLiteral("true"),
          "confirm did not report the ok button");
    driveNextModal(clickButton(QStringLiteral("Nah")));
    check(run(QStringLiteral("porydaw.ui.dialog.confirm('Q?', {ok: 'Sure', cancel: 'Nah'})")) ==
              QStringLiteral("false"),
          "confirm did not report the cancel button");
    driveNextModal([](QWidget *w) {
        if (auto *dlg = qobject_cast<QInputDialog *>(w)) {
            dlg->setTextValue(QStringLiteral("typed"));
            dlg->accept();
        }
    });
    check(run(QStringLiteral("porydaw.ui.dialog.prompt('Name?', {value: 'def'})")) ==
              QStringLiteral("typed"),
          "prompt did not return the typed text");
    driveNextModal([](QWidget *w) {
        if (auto *dlg = qobject_cast<QDialog *>(w))
            dlg->reject();
    });
    check(run(QStringLiteral("porydaw.ui.dialog.prompt('Name?')")) == QStringLiteral("null"),
          "a cancelled prompt did not return null");
    driveNextModal([](QWidget *w) {
        auto *dlg = qobject_cast<QDialog *>(w);
        if (!dlg)
            return;
        dlg->findChild<QLineEdit *>(QStringLiteral("field.name"))->setText(QStringLiteral("Bob"));
        dlg->findChild<QSpinBox *>(QStringLiteral("field.n"))->setValue(7);
        dlg->findChild<QDoubleSpinBox *>(QStringLiteral("field.f"))->setValue(1.5);
        dlg->findChild<QCheckBox *>(QStringLiteral("field.c"))->setChecked(true);
        dlg->findChild<QComboBox *>(QStringLiteral("field.k"))->setCurrentIndex(2);
        dlg->accept();
    });
    check(run(QStringLiteral(
              "var F = porydaw.ui.dialog.form({title: 'F', text: 'fill', fields: [{key: "
              "'name', label: 'Name', type: 'text', value: 'x'}, {key: 'n', type: 'number', "
              "value: 1, min: 0, max: 10}, {key: 'f', type: 'number', value: 0.5, decimals: "
              "1}, {key: 'c', type: 'checkbox'}, {key: 'k', type: 'combo', items: ['a', 'b', "
              "'c'], value: 'b'}]}); F.name === 'Bob' && F.n === 7 && F.f === 1.5 && F.c === "
              "true && F.k === 2")) == QStringLiteral("true"),
          "form did not return the edited field values");
    driveNextModal([](QWidget *w) {
        if (auto *dlg = qobject_cast<QDialog *>(w))
            dlg->reject();
    });
    check(run(QStringLiteral("porydaw.ui.dialog.form({fields: [{key: 'a'}]})")) ==
              QStringLiteral("null"),
          "a cancelled form did not return null");
    check(run(QStringLiteral("porydaw.ui.dialog.form({fields: []})")).isNull() &&
              errorLogged("at least one field"),
          "a form without fields was accepted");
    check(run(QStringLiteral("porydaw.ui.dialog.form({fields: [{key: 'a', type: 'color'}]})"))
                  .isNull() &&
              errorLogged("unknown type"),
          "a form field of unknown type was accepted");
    // Disabling a plugin while its dialog is up (a hot reload or a fault
    // would do the same) must wait for the call to unwind.
    {
        const scripting::Plugin *fixture = host.plugin(QStringLiteral("fixture"));
        bool wasLoadedDuringDialog = false;
        driveNextModal([&](QWidget *w) {
            host.setEnabled(QStringLiteral("fixture"), false);
            wasLoadedDuringDialog = fixture && fixture->engine != nullptr;
            if (auto *box = qobject_cast<QMessageBox *>(w))
                box->buttons().first()->click();
        });
        check(host.runCommand(QStringLiteral("plugin.fixture.ask")) && wasLoadedDuringDialog &&
                  fixture && fixture->state == scripting::PluginState::Disabled &&
                  !fixture->engine &&
                  storedCounter(QStringLiteral("fixture"), QStringLiteral("asked")) == 1,
              "disabling a plugin inside its own dialog did not defer the teardown");
        host.setEnabled(QStringLiteral("fixture"), true);
        check(fixture && fixture->state == scripting::PluginState::Loaded,
              "the fixture did not come back after the deferred teardown");
    }
    // File pickers grant the chosen path to porydaw.io.
    const QString outside = pluginsDir + QStringLiteral("/picked.txt");
    check(writeFile(outside, QStringLiteral("picked")), "could not write the pick fixture");
    check(run(QStringLiteral("porydaw.io.readText('%1')").arg(outside)).isNull() &&
              errorLogged("outside the plugin folder"),
          "io.readText outside the sandbox was allowed before a dialog granted it");
    const auto pickFile = [](const QString &path) {
        return [path](QWidget *w) {
            auto *dlg = qobject_cast<QFileDialog *>(w);
            if (!dlg)
                return;
            // Through accept(): it resolves the typed name the way a
            // user's Enter would (done() would skip that and report the
            // directory instead). A folder pick enters the folder, whose
            // empty selection accept() reports as the folder itself.
            if (dlg->fileMode() == QFileDialog::Directory) {
                dlg->setDirectory(path);
            } else if (auto *edit = dlg->findChild<QLineEdit *>(QStringLiteral("fileNameEdit"))) {
                // selectFile() leaves a focused name field alone; type it.
                edit->setText(path);
            }
            static_cast<QDialog *>(dlg)->accept();
            // Never wedge the harness in a dialog accept() refused.
            QTimer::singleShot(1500, dlg, [dlg] {
                if (dlg->isVisible()) {
                    std::fprintf(stderr,
                                 "scriptcheck: a file dialog refused accept(); rejecting\n");
                    dlg->reject();
                }
            });
        };
    };
    driveNextModal(pickFile(outside));
    check(run(QStringLiteral("var P = porydaw.ui.dialog.openFile({title: 'Pick'}); P")) == outside,
          "openFile did not return the picked path");
    check(run(QStringLiteral("porydaw.io.readText(P)")) == QStringLiteral("picked"),
          "a picked file was not readable through porydaw.io");
    const QString saveTarget = pluginsDir + QStringLiteral("/saved.txt");
    driveNextModal(pickFile(saveTarget));
    check(run(QStringLiteral("var S = porydaw.ui.dialog.saveFile({name: 'saved.txt'}); "
                             "porydaw.io.writeText(S, 'saved!'); S")) == saveTarget &&
              QFile::exists(saveTarget),
          "saveFile did not return a writable path");
    driveNextModal([](QWidget *w) {
        if (auto *dlg = qobject_cast<QDialog *>(w))
            dlg->reject();
    });
    check(run(QStringLiteral("porydaw.ui.dialog.openFile()")) == QStringLiteral("null"),
          "a cancelled file dialog did not return null");
    driveNextModal(pickFile(pluginsDir));
    check(run(QStringLiteral("var DD = porydaw.ui.dialog.chooseDir(); porydaw.io.writeText(DD + "
                             "'/indir.txt', 'x'); porydaw.io.exists(DD + '/indir.txt') && DD")) ==
              pluginsDir,
          "chooseDir did not return and grant the picked folder");

    // --- io ---
    const QString ioDir = projectRoot + QStringLiteral("/.porydaw/reach");
    QDir(ioDir).removeRecursively(); // a rerun against the same scratch copy
    check(run(QStringLiteral("porydaw.io.projectRoot")) == projectRoot, "io.projectRoot is wrong");
    check(run(QStringLiteral("porydaw.io.writeText('%1/a.txt', 'héllo'); "
                             "porydaw.io.readText('%1/a.txt')")
                  .arg(ioDir)) == QStringLiteral("héllo"),
          "io.writeText/readText round trip failed (folders created on demand)");
    check(run(QStringLiteral("var L = porydaw.io.list('%1'); L.length === 1 && L[0].name === "
                             "'a.txt' && !L[0].dir && L[0].size === 6")
                  .arg(ioDir)) == QStringLiteral("true"),
          "io.list did not describe the folder");
    check(
        run(QStringLiteral("porydaw.io.mkdir('%1/sub'); porydaw.io.isDir('%1/sub')").arg(ioDir)) ==
            QStringLiteral("true"),
        "io.mkdir/isDir failed");
    check(run(QStringLiteral("porydaw.io.writeBytes('%1/b.bin', new Uint8Array([1, 2, 255])); "
                             "var B = porydaw.io.readBytes('%1/b.bin'); B.length === 3 && B[2] === "
                             "255")
                  .arg(ioDir)) == QStringLiteral("true"),
          "io.writeBytes/readBytes round trip failed");
    check(run(QStringLiteral("porydaw.io.remove('%1/a.txt'); porydaw.io.exists('%1/a.txt')")
                  .arg(ioDir)) == QStringLiteral("false"),
          "io.remove did not delete the file");
    check(run(QStringLiteral("porydaw.io.remove('%1/sub')").arg(ioDir)).isNull() &&
              errorLogged("is a folder"),
          "io.remove accepted a folder");
    check(run(QStringLiteral("porydaw.io.readText('/etc/hostname')")).isNull() &&
              errorLogged("outside the plugin folder"),
          "io.readText of a system file was allowed");
    check(run(QStringLiteral("porydaw.io.writeText('%1/../escape.txt', 'x')").arg(projectRoot))
                  .isNull() &&
              errorLogged("outside the plugin folder") &&
              !QFile::exists(QFileInfo(projectRoot).path() + QStringLiteral("/escape.txt")),
          "a '..' path escaped the project");
    check(run(QStringLiteral("porydaw.io.readText('relative.txt')")).isNull() &&
              errorLogged("has no folder"),
          "a relative path from the console was not refused");
    check(run(QStringLiteral("porydaw.io.readText('%1/missing.txt')").arg(ioDir)).isNull() &&
              errorLogged("could not open"),
          "reading a missing file did not throw");
    // A path that reaches the project through a symlink names the same
    // files: macOS resolves what its dialogs return (/private/tmp for
    // /tmp), so comparing the spellings alone would say "outside".
    {
        const QString link = QDir::tempPath() + QStringLiteral("/porydaw-scriptcheck-root");
        QFile::remove(link);
        if (QFile::link(projectRoot, link) && QFileInfo(link).isSymLink()) {
            check(run(QStringLiteral("porydaw.io.writeText('%1/.porydaw/reach/vialink.txt', 'x'); "
                                     "porydaw.io.readText('%2/.porydaw/reach/vialink.txt')")
                          .arg(link, projectRoot)) == QStringLiteral("x"),
                  "a symlinked path into the project was refused");
            QFile::remove(link);
        }
    }

    // --- per-song storage ---
    const QString sidecar = ViewSidecar::pathFor(projectRoot, songLabel);
    check(run(QStringLiteral("porydaw.storage.song.set('k', {a: 1, b: [1, 2]}); "
                             "porydaw.storage.song.set('n', 5); "
                             "JSON.stringify(porydaw.storage.song.get('k')) + "
                             "porydaw.storage.song.keys().length")) ==
              QStringLiteral("{\"a\":1,\"b\":[1,2]}2"),
          "storage.song set/get/keys failed");
    check(readJson(sidecar)
                  .value(QLatin1String("plugins"))
                  .toObject()
                  .value(QLatin1String("console"))
                  .toObject()
                  .value(QLatin1String("k"))
                  .toObject()
                  .value(QLatin1String("a"))
                  .toInt() == 1,
          "storage.song did not land under plugins/<id> in the song sidecar");
    check(ViewSidecar::save(projectRoot, songLabel, view.viewState()) &&
              readJson(sidecar)
                  .value(QLatin1String("plugins"))
                  .toObject()
                  .contains(QLatin1String("console")) &&
              readJson(sidecar).contains(QLatin1String("view")),
          "a view-state save dropped the plugin data (or vice versa)");
    check(run(QStringLiteral("porydaw.storage.song.get('zzz', 3)")) == QStringLiteral("3"),
          "storage.song.get fallback failed");
    check(run(QStringLiteral("porydaw.storage.song.remove('k'); porydaw.storage.song.remove('n'); "
                             "porydaw.storage.song.get('k', 'gone')")) == QStringLiteral("gone") &&
              !readJson(sidecar).contains(QLatin1String("plugins")),
          "storage.song.remove did not clean the sidecar");

    // --- raw events ---
    check(run(QStringLiteral("porydaw.song.chunkCount > 0 && porydaw.song.tracks()[0].chunk >= 0 "
                             "&& porydaw.song.chunkTrack(porydaw.song.tracks()[0].chunk) === 0")) ==
              QStringLiteral("true"),
          "chunk addressing is inconsistent");
    check(run(QStringLiteral("porydaw.song.rawEvents(99)")).isNull() &&
              errorLogged("no such chunk"),
          "rawEvents of a bad chunk did not throw");
    check(run(QStringLiteral(
              "var C = porydaw.song.tracks()[0].chunk, CH = porydaw.song.tracks()[0].channel; "
              "var before = porydaw.song.rawEvents(C).length; "
              "porydaw.edit.transaction('Raw', function () { "
              "  porydaw.edit.insertRawEvent(C, {tick: 30, type: 'cc', channel: CH, data0: 7, "
              "data1: 99}); "
              "  porydaw.edit.insertRawEvent(C, {tick: 30, status: 0xFF, metaType: 1, text: "
              "'hey'}); "
              "  porydaw.edit.insertRawEvent(C, {tick: 31, type: 'sysex', blob: [1, 2, 3]}); "
              "}); "
              "var evs = porydaw.song.rawEvents(C, {from: 30, to: 32}); "
              "var cc = evs.filter(function (e) { return e.type === 'cc'; })[0]; "
              "var meta = evs.filter(function (e) { return e.type === 'meta'; })[0]; "
              "var sx = evs.filter(function (e) { return e.type === 'sysex'; })[0]; "
              "porydaw.song.rawEvents(C).length === before + 3 && cc.data0 === 7 && cc.data1 "
              "=== 99 && cc.channel === CH && cc.status === (0xB0 | CH) && meta.text === 'hey' "
              "&& meta.metaType === 1 && meta.blob.length === 3 && sx.blob[2] === 3 && "
              "typeof cc.index === 'number'")) == QStringLiteral("true"),
          "insertRawEvent / rawEvents did not round-trip cc, meta text and sysex blob");
    oneEntryThenUndo("Raw", "raw inserts are not one undo entry");
    check(run(QStringLiteral(
              "porydaw.edit.transaction('Raw2', function () { "
              "  porydaw.edit.insertRawEvent(C, {tick: 30, type: 'cc', channel: CH, data0: 7, "
              "data1: 99}); "
              "  var e = porydaw.song.rawEvents(C, {from: 30, to: 31}).filter(function (x) { "
              "return x.type === 'cc' && x.data0 === 7; })[0]; "
              "  porydaw.edit.modifyRawEvent(C, e.index, {tick: 40, type: 'cc', channel: CH, "
              "data0: 7, data1: 50}); "
              "}); "
              "var at40 = porydaw.song.rawEvents(C, {from: 40, to: 41}).filter(function (x) { "
              "return x.type === 'cc' && x.data0 === 7; }); "
              "at40.length === 1 && at40[0].data1 === 50 && porydaw.song.rawEvents(C, {from: "
              "30, to: 31}).filter(function (x) { return x.type === 'cc' && x.data0 === 7 && "
              "x.data1 === 99; }).length === 0")) == QStringLiteral("true"),
          "modifyRawEvent did not move/change the event");
    oneEntryThenUndo("Raw2", "raw modify is not one undo entry");
    check(run(QStringLiteral(
              "var moved = false, same = true; "
              "porydaw.edit.transaction('Raw3', function () { "
              "  porydaw.edit.insertRawEvent(C, {tick: 30, type: 'cc', channel: CH, data0: 7, "
              "data1: 1}); "
              "  porydaw.edit.insertRawEvent(C, {tick: 30, type: 'cc', channel: CH, data0: 10, "
              "data1: 2}); "
              "  var pair = porydaw.song.rawEvents(C, {from: 30, to: 31}).filter(function (x) "
              "{ return x.type === 'cc' && (x.data0 === 7 || x.data0 === 10); }); "
              "  moved = porydaw.edit.moveRawEvent(C, pair[1].index, pair[0].index); "
              "  same = porydaw.edit.moveRawEvent(C, pair[0].index, pair[0].index); "
              "  var after = porydaw.song.rawEvents(C, {from: 30, to: 31}).filter(function (x) "
              "{ return x.type === 'cc' && (x.data0 === 7 || x.data0 === 10); }); "
              "  porydaw.storage.set('rawmove', after[0].data0 === 10 && after[1].data0 === 7 "
              "? 1 : 0); "
              "  var n = porydaw.edit.deleteRawEvents(C, [after[0].index, after[1].index, "
              "after[1].index, 1e9]); "
              "  porydaw.storage.set('rawdel', n); "
              "}); "
              "moved && !same && porydaw.song.rawEvents(C).length === before")) ==
                  QStringLiteral("true") &&
              storedCounter(QStringLiteral("console"), QStringLiteral("rawmove")) == 1 &&
              storedCounter(QStringLiteral("console"), QStringLiteral("rawdel")) == 2,
          "moveRawEvent / deleteRawEvents misbehaved");
    oneEntryThenUndo("Raw3", "raw move+delete are not one undo entry");
    check(run(QStringLiteral("var E0 = porydaw.song.chunkEndTick(C); porydaw.edit.transaction("
                             "'End', function () { porydaw.edit.setChunkEndTick(C, E0 + 96); }); "
                             "porydaw.song.chunkEndTick(C) === E0 + 96")) == QStringLiteral("true"),
          "setChunkEndTick did not move the end of track");
    oneEntryThenUndo("End", "setChunkEndTick is not one undo entry");
    const auto rawRefused = [&](const char *spec, const char *fragment, const char *what) {
        messages.clear();
        check(run(QStringLiteral("porydaw.edit.transaction('Bad', function () { "
                                 "porydaw.edit.insertRawEvent(C, %1); })")
                      .arg(QLatin1String(spec)))
                      .isNull() &&
                  errorLogged(fragment),
              what);
        untouched("a refused raw insert left the document changed");
    };
    rawRefused("{tick: 0}", "status byte or a type", "an event with no status/type was accepted");
    rawRefused("{tick: 0, status: 0x50}", "status must be", "a data byte as status was accepted");
    rawRefused("{tick: 0, type: 'meta'}", "needs metaType", "a meta without metaType was accepted");
    check(run(QStringLiteral("porydaw.edit.transaction('Bad', function () { "
                             "porydaw.edit.modifyRawEvent(C, 1e9, {tick: 0, type: 'cc'}); })"))
                  .isNull() &&
              errorLogged("no such event"),
          "modifyRawEvent of a bad index was accepted");
    untouched("a refused raw modify left the document changed");
    check(run(QStringLiteral("porydaw.edit.transaction('Bad', function () { "
                             "porydaw.edit.insertRawEvent(99, {tick: 0, type: 'cc'}); })"))
                  .isNull() &&
              errorLogged("no such chunk"),
          "insertRawEvent on a bad chunk was accepted");

    // --- moveRange / duplicateRange ---
    check(run(QStringLiteral(
              "var T = -1; "
              "porydaw.edit.transaction('Range', function () { "
              "  T = porydaw.edit.addTrack(0); "
              "  porydaw.edit.addNotes(T, [{tick: 480, key: 60, len: 24, vel: 100}, {tick: "
              "528, key: 62, len: 24, vel: 100}]); "
              "  porydaw.edit.addLanePoint(T, 7, 500, 80); "
              "  porydaw.storage.set('rangemoved', porydaw.edit.moveRange(480, 576, {tracks: "
              "[T]}, 192)); "
              "}); "
              "var moved = porydaw.song.notes({track: T, from: 672, to: 768}); "
              "moved.length === 2 && moved[0].key === 60 && moved[1].tick === 720 && "
              "porydaw.song.lanePoints(T, 7, {from: 692, to: 693}).length === 1 && "
              "porydaw.song.notes({track: T, from: 480, to: 576}).length === 0")) ==
                  QStringLiteral("true") &&
              storedCounter(QStringLiteral("console"), QStringLiteral("rangemoved")) >= 3,
          "moveRange did not move the notes and lane point");
    oneEntryThenUndo("Range", "moveRange's transaction is not one entry");
    check(run(QStringLiteral(
              "porydaw.edit.transaction('Dup', function () { "
              "  T = porydaw.edit.addTrack(0); "
              "  porydaw.edit.addNotes(T, [{tick: 480, key: 60, len: 24, vel: 100}]); "
              "  porydaw.edit.addLanePoint(T, 7, 500, 80); "
              "  porydaw.storage.set('dupn', porydaw.edit.duplicateRange(480, 576, {lanes: "
              "[{track: T, cc: 7}]}, 96)); "
              "  porydaw.edit.duplicateRange(480, 576, {tracks: [T]}, 192); "
              "}); "
              "porydaw.song.notes({track: T, from: 480, to: 481}).length === 1 && "
              "porydaw.song.notes({track: T, from: 576, to: 577}).length === 0 && "
              "porydaw.song.notes({track: T, from: 672, to: 673}).length === 1 && "
              "porydaw.song.lanePoints(T, 7, {from: 596, to: 597}).length === 1")) ==
                  QStringLiteral("true") &&
              storedCounter(QStringLiteral("console"), QStringLiteral("dupn")) == 1,
          "duplicateRange (lanes scope, then tracks scope) did not copy as expected");
    oneEntryThenUndo("Dup", "duplicateRange's transaction is not one entry");
    check(run(QStringLiteral("porydaw.edit.transaction('Zero', function () { return "
                             "porydaw.edit.moveRange(0, 96, {tracks: [0]}, 0); })")) ==
              QStringLiteral("0"),
          "moveRange with dTick 0 did not report 0");
    // A delta past the range start is floored there, so spacing survives.
    check(run(QStringLiteral(
              "porydaw.edit.transaction('Floor', function () { "
              "  T = porydaw.edit.addTrack(0); "
              "  porydaw.edit.addNotes(T, [{tick: 480, key: 60, len: 24, vel: 100}, {tick: "
              "528, key: 62, len: 24, vel: 100}]); "
              "  porydaw.edit.moveRange(480, 576, {tracks: [T]}, -5000); "
              "}); "
              "var fl = porydaw.song.notes({track: T}); fl.length === 2 && fl[0].tick === 0 "
              "&& fl[1].tick === 48")) == QStringLiteral("true"),
          "moveRange past tick 0 smeared the range");
    oneEntryThenUndo("Floor", "the floored moveRange is not one entry");
    untouched("a zero moveRange left an entry");
    check(run(QStringLiteral("porydaw.edit.transaction('BadScope', function () { "
                             "porydaw.edit.moveRange(0, 96, {}, 10); })"))
                  .isNull() &&
              errorLogged("scope needs"),
          "moveRange with an empty scope was accepted");
    check(run(QStringLiteral("porydaw.edit.transaction('BadRange', function () { "
                             "porydaw.edit.duplicateRange(96, 96, {tracks: [0]}, 10); })"))
                  .isNull() &&
              errorLogged("end must be after start"),
          "duplicateRange with an empty range was accepted");
    untouched("refused range calls left the document changed");

    // --- roll overlays ---
    check(run(QStringLiteral(
              "var OV = porydaw.ui.overlay({id: 'ov', paint: function (g, v) { "
              "porydaw.storage.set('ovpaints', porydaw.storage.get('ovpaints', 0) + 1); "
              "g.fillRect(0, 0, v.width, v.height, '#ff0000'); "
              "porydaw.storage.set('ovinfo', JSON.stringify({w: v.width, k: "
              "v.key(v.keyTop(60) + 1), t: v.tick(v.x(96)), tr: v.track, from: v.from})); }}); "
              "OV.id")) == QStringLiteral("ov"),
          "ui.overlay was refused");
    QImage rollShot = roll.grab().toImage();
    const QPoint inGrid(songview::kKeyboardW + 20, 10);
    check(isRed(rollShot.pixelColor(inGrid)) && !isRed(rollShot.pixelColor(QPoint(5, 10))),
          "overlay did not paint over the note area (and only there)");
    check(storedCounter(QStringLiteral("console"), QStringLiteral("ovpaints")) >= 1,
          "overlay paint did not run");
    {
        const QJsonObject info =
            QJsonDocument::fromJson(
                QJsonDocument::fromJson(
                    QSettings().value(QStringLiteral("plugins/console/data/ovinfo")).toByteArray())
                    .object()
                    .value(QLatin1String("v"))
                    .toString()
                    .toUtf8())
                .object();
        check(info.value(QLatin1String("w")).toDouble() == roll.width() - songview::kKeyboardW &&
                  info.value(QLatin1String("k")).toInt() == 60 &&
                  std::abs(info.value(QLatin1String("t")).toDouble() - 96.0) < 1.0 &&
                  info.value(QLatin1String("tr")).toInt() == view.selectedTrack(),
              "overlay view geometry (width, key/keyTop, tick/x, track) is inconsistent");
    }
    run(QStringLiteral("OV.visible = false"));
    rollShot = roll.grab().toImage();
    check(!isRed(rollShot.pixelColor(inGrid)), "a hidden overlay still painted");
    run(QStringLiteral("OV.visible = true"));
    rollShot = roll.grab().toImage();
    check(isRed(rollShot.pixelColor(inGrid)), "re-showing the overlay did not repaint");
    check(run(QStringLiteral("porydaw.ui.overlay({id: 'ov', paint: function () {}})")).isNull() &&
              errorLogged("exists"),
          "a duplicate overlay id was accepted");
    check(run(QStringLiteral("porydaw.ui.overlay({id: 'x', paint: 3})")).isNull() &&
              errorLogged("paint(g, v)"),
          "an overlay without a paint function was accepted");
    // A paint that removes its own overlay, adds another, measures text
    // consistently, and tries to open a dialog.
    messages.clear();
    check(run(QStringLiteral(
              "var SELF = porydaw.ui.overlay({id: 'self', paint: function (g, v) { "
              "  SELF.remove(); "
              "  porydaw.ui.overlay({id: 'born', paint: function (g) { "
              "    var w1 = g.measureText('abc', {size: 2}).width; "
              "    g.text(0, 10, 'abc', 'red', {size: 2}); g.text(0, 20, 'abc', 'red', {bold: "
              "true}); "
              "    var w2 = g.measureText('abc', {size: 2}).width; "
              "    porydaw.storage.set('fontstable', w1 === w2 && w1 > g.measureText('abc', "
              "{}).width ? 1 : 0); "
              "    try { porydaw.ui.dialog.alert('no'); } catch (e) { "
              "porydaw.storage.set('paintdlg', String(e.message)); } "
              "  }}); "
              "}}); SELF.id")) == QStringLiteral("self"),
          "self-removing overlay could not be created");
    roll.grab();
    QCoreApplication::processEvents(); // the deferred invalidation for 'born'
    roll.grab();
    check(run(QStringLiteral("SELF.active")) == QStringLiteral("false") &&
              storedCounter(QStringLiteral("console"), QStringLiteral("fontstable")) == 1 &&
              QSettings()
                  .value(QStringLiteral("plugins/console/data/paintdlg"))
                  .toByteArray()
                  .contains("paint callback"),
          "overlay self-removal / text sizing / dialog-from-paint refusal misbehaved");
    run(QStringLiteral("porydaw.storage.remove('fontstable')"));
    messages.clear();
    run(QStringLiteral("var BAD = porydaw.ui.overlay({id: 'bad', paint: function () { throw new "
                       "Error('ovboom'); }})"));
    roll.grab();
    roll.grab();
    check(countMessages(messages, QStringLiteral("console"), QStringLiteral("ovboom")) == 1,
          "a throwing overlay was not logged exactly once (held back afterwards)");
    check(run(QStringLiteral("OV.remove(); BAD.remove(); OV.active")) == QStringLiteral("false"),
          "overlay.remove() did not deactivate the handle");
    rollShot = roll.grab().toImage();
    check(!isRed(rollShot.pixelColor(inGrid)), "a removed overlay still painted");
    check(run(QStringLiteral("porydaw.ui.overlay({id: 'ov', paint: function () {}}).id")) ==
              QStringLiteral("ov"),
          "an overlay id could not be reused after remove()");

    // --- render ---
    const QString wav = projectRoot + QStringLiteral("/.porydaw/reach.wav");
    const QString rendered =
        run(QStringLiteral("var R = porydaw.audio.render('%1', {sampleRate: 32000, loopCount: 1, "
                           "fadeout: 0.5, tail: 0.5}); R.path + '|' + R.seconds")
                .arg(wav));
    {
        WavExportOptions opts;
        opts.sampleRate = 32000;
        opts.loopCount = 1;
        opts.fadeoutSeconds = 0.5;
        opts.tailSeconds = 0.5;
        auto timeline = doc.buildTimeline(32000.0);
        const uint64_t total = wavExportTotals(*timeline, opts).totalSamples;
        const QStringList parts = rendered.split(QLatin1Char('|'));
        check(parts.size() == 2 && parts[0] == wav &&
                  std::abs(parts[1].toDouble() - double(total) / 32000.0) < 0.01 &&
                  QFileInfo(wav).size() == qint64(44 + total * 4),
              "audio.render did not write the WAV the Export path would");
    }
    check(run(QStringLiteral("porydaw.audio.render('/etc/reach.wav', {})")).isNull() &&
              errorLogged("outside the plugin folder"),
          "audio.render outside the sandbox was allowed");

    // --- project.open (last: it replaces the active session) ---
    const QString other =
        run(QStringLiteral("var other = porydaw.project.songs().filter(function (s) { return "
                           "s.hasMid && s.label !== '%1'; })[0]; other ? other.label : ''")
                .arg(songLabel));
    check(!other.isEmpty(), "project has no second playable song for project.open");
    check(run(QStringLiteral("porydaw.project.open('nope_zzz_not_a_song')")) ==
              QStringLiteral("false"),
          "project.open of an unknown label did not return false");
    check(run(QStringLiteral("porydaw.edit.transaction('O', function () { "
                             "porydaw.project.open('%1'); })")
                  .arg(other))
                  .isNull() &&
              errorLogged("inside a transaction"),
          "project.open inside a transaction was allowed");
    undo.setClean();
    check(run(QStringLiteral("porydaw.project.open('%1')").arg(songLabel)) ==
              QStringLiteral("true"),
          "project.open of the current song did not report true");
    run(QStringLiteral("porydaw.storage.set('acts', 0); porydaw.song.on('activated', function (e) "
                       "{ porydaw.storage.set('acts', porydaw.storage.get('acts', 0) + 1); "
                       "porydaw.storage.set('actlabel', e ? e.label : ''); })"));
    check(run(QStringLiteral("porydaw.project.open('%1') && porydaw.song.label").arg(other)) ==
                  other &&
              host.session() && host.session()->doc.label() == other,
          "project.open did not switch the active song");
    check(storedCounter(QStringLiteral("console"), QStringLiteral("acts")) == 1 &&
              QSettings()
                  .value(QStringLiteral("plugins/console/data/actlabel"))
                  .toByteArray()
                  .contains(other.toUtf8()),
          "an in-place song swap did not fire song.activated with the new label");
    check(run(QStringLiteral("porydaw.project.open('%1') && porydaw.song.label").arg(songLabel)) ==
              songLabel,
          "project.open could not switch back");
    // The console is not unloaded by unloadAll: release its test command.
    run(QStringLiteral("porydaw.actions.unregister(AID)"));
}

// Phase 4, second slice: project adapter writes and the voicegroup API —
// song settings (read + undoable edit, voicegroup switch by display name),
// voice edits through the session's undo stack (transaction entry,
// rollback, structural type change with envelope adoption, validation),
// song.save() of an edited voicegroup, voicegroup creation, and song
// registration/unregistration on a copied .mid. Writes into the project:
// scratch copy only. Leaves the project as it found it (bar the .mid
// write-back of song.save()).
// ---- GBA engine settings through porydaw.audio.engine / setEngine ----

void runEngineSettingsChecks(const Check &check, scripting::ScriptHost &host, MainWindow &window,
                             QList<Message> &messages)
{
    const auto runc = [&](const char *code) { return host.evalConsole(QLatin1String(code)); };
    // Only errors logged since the last look: an earlier refusal's message
    // must not satisfy a later check.
    qsizetype seen = messages.size();
    const auto errorLogged = [&](const char *fragment) {
        const QList<Message> tail = messages.mid(seen);
        seen = messages.size();
        return hasMessage(tail, QStringLiteral("console"), 2, QLatin1String(fragment));
    };
    // The window applies (and announces) settings from a coalescing timer.
    const auto settle = [] {
        QDeadlineTimer deadline(600);
        while (!deadline.hasExpired())
            QApplication::processEvents(QEventLoop::AllEvents, 50);
    };
    AudioSettingsPage &page = *window.settingsDialog()->audioPage();
    const EngineSettings before = page.engineSettings();

    check(runc("porydaw.audio.engine.maxPcmChannels").toInt() == before.maxPcmChannels &&
              runc("porydaw.audio.engine.pcmMixRate").toFloat() == before.pcmMixRate &&
              runc("porydaw.audio.engine.analogFilter") ==
                  (before.analogFilter ? QStringLiteral("true") : QStringLiteral("false")),
          "audio.engine does not mirror the Settings window's engine settings");
    check(runc("porydaw.audio.engineLimits().mixRates.indexOf(13379) >= 0 && "
               "porydaw.audio.engineLimits().mixRates.indexOf(0) < 0 && "
               "porydaw.audio.engineLimits().maxPcmChannels >= 5") == QStringLiteral("true"),
          "audio.engineLimits() lacks the GBA rates or the polyphony ceiling");

    // A partial write: the page, QSettings and the event all follow.
    runc("var engLog = []; var engFn = function (e) { engLog.push(e.maxPcmChannels + '/' + "
         "e.pcmMixRate + '/' + e.analogFilter); }; porydaw.audio.on('engine', engFn)");
    const int poly = before.maxPcmChannels == 3 ? 4 : 3;
    check(host.evalConsole(QStringLiteral("porydaw.audio.setEngine({maxPcmChannels: %1, "
                                          "analogFilter: %2})")
                               .arg(poly)
                               .arg(before.analogFilter ? "false" : "true")) ==
                  QStringLiteral("undefined") &&
              page.engineSettings().maxPcmChannels == poly &&
              page.engineSettings().analogFilter != before.analogFilter &&
              page.engineSettings().pcmMixRate == before.pcmMixRate &&
              runc("porydaw.audio.engine.maxPcmChannels").toInt() == poly,
          "audio.setEngine did not reach the Settings page (or touched an omitted knob)");
    check(QSettings().value(QStringLiteral("engine/maxPcmChannels")).toInt() == poly,
          "audio.setEngine did not persist");
    // A second write before the timer fires: one event, the settled value.
    runc("porydaw.audio.setEngine({pcmMixRate: 0})");
    check(page.engineSettings().pcmMixRate == 0.0f, "audio.setEngine({pcmMixRate: 0}) refused");
    settle();
    check(runc("engLog.join(' ')") ==
              QStringLiteral("%1/0/%2").arg(poly).arg(before.analogFilter ? "false" : "true"),
          "audio.engine did not fire exactly once with the settled settings");

    // Refusals leave everything as it was.
    const EngineSettings mid = page.engineSettings();
    check(runc("porydaw.audio.setEngine({maxPcmChannels: 0})").isNull() &&
              errorLogged("maxPcmChannels must be"),
          "audio.setEngine accepted maxPcmChannels 0");
    check(runc("porydaw.audio.setEngine({maxPcmChannels: 2.5})").isNull() &&
              runc("porydaw.audio.setEngine({maxPcmChannels: '3'})").isNull(),
          "audio.setEngine accepted a fractional or string polyphony");
    check(runc("porydaw.audio.setEngine({pcmMixRate: 12345})").isNull() &&
              errorLogged("pcmMixRate must be"),
          "audio.setEngine accepted an unlisted mix rate");
    check(runc("porydaw.audio.setEngine({analogFilter: 1})").isNull() &&
              errorLogged("analogFilter must be"),
          "audio.setEngine accepted a non-boolean analogFilter");
    check(runc("porydaw.audio.setEngine({polyphony: 3})").isNull() &&
              errorLogged("unknown key 'polyphony'"),
          "audio.setEngine accepted an unknown key");
    check(runc("porydaw.audio.setEngine(null)").isNull() && errorLogged("expected an object"),
          "audio.setEngine accepted null");
    check(runc("porydaw.audio.setEngine({maxPcmChannels: 1e300})").isNull() &&
              errorLogged("maxPcmChannels must be"),
          "audio.setEngine accepted a huge polyphony");
    check(runc("porydaw.audio.setEngine([3])").isNull() && errorLogged("expected an object"),
          "audio.setEngine accepted an array");
    check(page.engineSettings() == mid, "a refused audio.setEngine changed the settings");
    settle();
    check(runc("engLog.length") == QStringLiteral("1"), "refused writes fired audio.engine");

    // Back to where the run started through the page (the run's starting
    // rate need not be one the API lists); the window tells scripts.
    page.setEngineSettings(before);
    check(page.engineSettings() == before, "the page could not restore the settings");
    settle();
    check(runc("engLog.length") == QStringLiteral("2") &&
              runc("engLog[1]") == QStringLiteral("%1/%2/%3")
                                       .arg(before.maxPcmChannels)
                                       .arg(double(before.pcmMixRate))
                                       .arg(before.analogFilter ? "true" : "false"),
          "a Settings-page change did not fire audio.engine");
    runc("porydaw.audio.off('engine', engFn)");
}

void runAdapterChecks(const Check &check, scripting::ScriptHost &host, SongSession &session,
                      QList<Message> &messages, const QString &projectRoot,
                      const QString &songLabel, bool audioOk)
{
    // The window swaps a session's voicegroup source on a -G change only
    // with audio up (onDocumentChanged bails without it); the cfg edit
    // itself is checked either way.
    const bool swaps = audioOk;
    SongDocument &doc = session.doc;
    QUndoStack &undo = *doc.undoStack();
    const auto run = [&](const QString &code) { return host.evalConsole(code); };
    const auto runc = [&](const char *code) { return host.evalConsole(QLatin1String(code)); };
    const auto errorLogged = [&](const char *fragment) {
        return hasMessage(messages, QStringLiteral("console"), 2, QLatin1String(fragment));
    };
    const auto fileBytes = [](const QString &path) {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    };
    while (undo.canRedo())
        undo.redo();
    const int index0 = undo.index();
    const SongCfg cfg0 = doc.cfg();

    // --- settings (read) + project.song / registration / musicPlayers ---
    check(runc("porydaw.song.settings().voicegroup") == cfg0.voicegroupArg &&
              runc("porydaw.song.settings().masterVolume") == QString::number(cfg0.masterVolume) &&
              runc("porydaw.song.settings().flags.length") == QString::number(cfg0.rawFlags.size()),
          "song.settings() does not mirror the document's midi.cfg settings");
    check(run(QStringLiteral("porydaw.project.song('%1').settings.voicegroup").arg(songLabel)) ==
                  cfg0.voicegroupArg &&
              runc("porydaw.project.song('nope_zzz_not_a_song')") == QStringLiteral("null"),
          "project.song(label) does not return the entry with its settings (or null)");
    check(runc("porydaw.project.musicPlayers().some(function (p) { return p.name === "
               "'MUSIC_PLAYER_BGM' && p.trackCount > 0; })") == QStringLiteral("true"),
          "project.musicPlayers() lacks MUSIC_PLAYER_BGM with a track count");
    check(run(QStringLiteral("porydaw.project.registration('%1').complete").arg(songLabel)) ==
              QStringLiteral("true"),
          "project.registration() of a registered song is not complete");
    check(runc("porydaw.project.registration('nope_zzz_not_a_song')").isNull() &&
              errorLogged("no song named"),
          "project.registration() of an unknown label did not throw");

    // --- edit.setSettings ---
    const int vol1 = cfg0.masterVolume == 99 ? 98 : 99;
    check(run(QStringLiteral("porydaw.edit.transaction('Vol', function () { "
                             "porydaw.edit.setSettings({masterVolume: %1}); })")
                  .arg(vol1)) == QStringLiteral("undefined") &&
              doc.cfg().masterVolume == vol1 && undo.index() == index0 + 1 &&
              undo.text(index0) == QStringLiteral("Vol") &&
              runc("porydaw.song.settings().masterVolume") == QString::number(vol1),
          "edit.setSettings({masterVolume}) did not apply as one named undo entry");
    undo.undo();
    check(doc.cfg().masterVolume == cfg0.masterVolume && undo.index() == index0,
          "undoing the settings transaction did not restore the master volume");
    run(QStringLiteral("porydaw.edit.transaction('Rev', function () { "
                       "porydaw.edit.setSettings({reverb: 33}); })"));
    check(doc.cfg().reverb == 33 && runc("porydaw.song.settings().reverb") == QStringLiteral("33"),
          "edit.setSettings({reverb}) did not set the -R flag");
    run(QStringLiteral("porydaw.edit.transaction('NoRev', function () { "
                       "porydaw.edit.setSettings({reverb: null}); })"));
    check(doc.cfg().reverb == -1 &&
              runc("porydaw.song.settings().reverb") == QStringLiteral("null"),
          "edit.setSettings({reverb: null}) did not drop the -R flag");
    check(runc("porydaw.edit.transaction('U', function () { porydaw.edit.setSettings({reverb: "
               "undefined}); })")
                  .isNull() &&
              errorLogged("'reverb' must be a number") && doc.cfg().reverb == -1,
          "edit.setSettings({reverb: undefined}) was taken for null");
    while (undo.index() > index0)
        undo.undo();
    check(doc.cfg().reverb == cfg0.reverb, "undo did not restore the reverb flag");
    check(
        runc("porydaw.edit.transaction('Bad', function () { porydaw.edit.setSettings({bogus: 1}); "
             "})")
                .isNull() &&
            errorLogged("unknown setting") && undo.index() == index0,
        "edit.setSettings with an unknown key was not refused");
    check(runc("porydaw.edit.transaction('BadVg', function () { porydaw.edit.setSettings({"
               "voicegroup: 'no_such_voicegroup_zzz'}); })")
                  .isNull() &&
              errorLogged("unknown voicegroup") && undo.index() == index0 &&
              doc.cfg().voicegroupArg == cfg0.voicegroupArg,
          "edit.setSettings with an unknown voicegroup was not refused");
    check(runc("porydaw.edit.setSettings({masterVolume: 5})").isNull() &&
              errorLogged("inside porydaw.edit.transaction") &&
              doc.cfg().masterVolume == cfg0.masterVolume,
          "edit.setSettings outside a transaction was allowed");

    // Switching the voicegroup by display name: the window swaps the
    // session's source on the cfg change, and porydaw.voicegroup follows.
    const QString otherArg = runc(
        "var VGS = porydaw.project.voicegroups(); var o = VGS.filter(function (g) { return g.arg "
        "!== porydaw.song.settings().voicegroup; })[0]; o ? o.arg : ''");
    check(!otherArg.isEmpty() &&
              run(QStringLiteral("VGS.some(function (g) { return g.arg === '%1' && g.name === "
                                 "'%2'; })")
                      .arg(cfg0.voicegroupArg, SongRegistry::voicegroupDisplayName(
                                                   cfg0.voicegroupArg))) == QStringLiteral("true"),
          "project.voicegroups() lacks the song's own voicegroup (with its display name) or a "
          "second one");
    if (!check(session.vgSource != nullptr, "the session has no voicegroup source to edit"))
        return;
    run(QStringLiteral("porydaw.edit.transaction('VG', function () { "
                       "porydaw.edit.setSettings({voicegroup: '%1'}); })")
            .arg(SongRegistry::voicegroupDisplayName(otherArg)));
    check(doc.cfg().voicegroupArg == otherArg && runc("porydaw.voicegroup.arg") == otherArg &&
              runc("porydaw.voicegroup.name") == SongRegistry::voicegroupDisplayName(otherArg) &&
              (!swaps || (session.vgSource &&
                          runc("porydaw.voicegroup.file") == session.vgSource->filePath() &&
                          !session.vgSource->filePath().contains(
                              SongRegistry::voicegroupDisplayName(cfg0.voicegroupArg)))),
          "setSettings({voicegroup}) by display name did not switch the voicegroup (or "
          "porydaw.voicegroup did not follow)");
    undo.undo();
    check(doc.cfg().voicegroupArg == cfg0.voicegroupArg &&
              runc("porydaw.voicegroup.arg") == cfg0.voicegroupArg && session.vgSource,
          "undoing the voicegroup switch did not reopen the original source");

    // --- porydaw.voicegroup (read) ---
    check(runc("porydaw.voicegroup.isOpen") == QStringLiteral("true") &&
              runc("porydaw.voicegroup.voices().length") == QString::number(VOICEGROUP_SIZE) &&
              runc("porydaw.voicegroup.loadName") == session.vgSource->loadName(),
          "porydaw.voicegroup does not report the open source with 128 slots");
    // A voice with an envelope of its own (keysplit/drumkit voices carry
    // none — an `attack` on those is refused below).
    int slot = -1;
    int splitSlot = -1;
    for (int s = 0; s < VOICEGROUP_SIZE; ++s) {
        const VgVoice *v = session.vgSource->voiceAt(s);
        if (!v)
            continue;
        if (vgAdsrFamily(v->macro) >= 0 && slot < 0)
            slot = s;
        if (vgAdsrFamily(v->macro) < 0 && splitSlot < 0)
            splitSlot = s;
    }
    if (!check(slot >= 0, "the voicegroup has no editable voice with an envelope to test with"))
        return;
    const QString vgPath = session.vgSource->filePath();
    if (splitSlot >= 0) {
        check(run(QStringLiteral("porydaw.edit.transaction('B0', function () { "
                                 "porydaw.edit.setVoice(%1, {attack: 1}); })")
                      .arg(splitSlot))
                      .isNull() &&
                  errorLogged("does not apply to") && !session.vgSource->dirty() &&
                  undo.index() == index0,
              "setVoice accepted an envelope field on a keysplit/drumkit voice");
    }
    const VgVoice v0 = *session.vgSource->voiceAt(slot);
    check(run(QStringLiteral("var V = porydaw.voicegroup.voice(%1); V.kind + '|' + V.type + '|' + "
                             "V.key + '|' + V.symbol + '|' + V.attack + '|' + V.release")
                  .arg(slot)) == QStringLiteral("voice|%1|%2|%3|%4|%5")
                                     .arg(vgMacroName(v0.macro))
                                     .arg(v0.key)
                                     .arg(v0.symbol)
                                     .arg(v0.attack)
                                     .arg(v0.release),
          "voicegroup.voice(slot) does not report the parsed macro arguments");
    check(runc("porydaw.voicegroup.voice(999)") == QStringLiteral("null") &&
              runc("porydaw.voicegroup.voices()[0].slot") == QStringLiteral("0"),
          "voicegroup.voice(out of range) is not null / voices() slots not numbered");
    check(runc("porydaw.voicegroup.symbols().directSound.length > 0 && "
               "Array.isArray(porydaw.voicegroup.symbols().keysplits)") == QStringLiteral("true"),
          "voicegroup.symbols() lacks the project's DirectSound symbols");
    check(runc("var A = porydaw.voicegroup.typicalAdsr('voice_directsound'); typeof A.attack === "
               "'number' && A.release > 0") == QStringLiteral("true") &&
              runc("porydaw.voicegroup.typicalAdsr('voice_bogus')").isNull() &&
              errorLogged("unknown voice type"),
          "voicegroup.typicalAdsr() did not return an envelope (or accepted a bad type)");

    // --- edit.setVoice ---
    const bool cgb0 = vgMacroIsCgb(v0.macro);
    const int attack1 = cgb0 ? (v0.attack == 3 ? 4 : 3) : (v0.attack == 200 ? 201 : 200);
    check(run(QStringLiteral("porydaw.edit.transaction('Voice', function () { "
                             "porydaw.edit.setVoice(%1, {attack: %2}); })")
                  .arg(slot)
                  .arg(attack1)) == QStringLiteral("undefined") &&
              session.vgSource->voiceAt(slot)->attack == attack1 && session.vgSource->dirty() &&
              undo.index() == index0 + 1 && undo.text(index0) == QStringLiteral("Voice") &&
              runc("porydaw.voicegroup.dirty") == QStringLiteral("true") &&
              run(QStringLiteral("porydaw.voicegroup.voice(%1).attack").arg(slot)) ==
                  QString::number(attack1),
          "edit.setVoice did not apply as one transaction entry on the song's undo stack");
    undo.undo();
    check(session.vgSource->voiceAt(slot)->attack == v0.attack && !session.vgSource->dirty() &&
              undo.index() == index0,
          "undoing the voice transaction did not restore the voice (or left the source dirty)");
    check(run(QStringLiteral("porydaw.edit.transaction('Roll', function () { "
                             "porydaw.edit.setVoice(%1, {attack: %2}); throw new Error('boom'); })")
                  .arg(slot)
                  .arg(attack1))
                  .isNull() &&
              errorLogged("boom") && session.vgSource->voiceAt(slot)->attack == v0.attack &&
              !session.vgSource->dirty() && undo.index() == index0,
          "a voice edit was not rolled back with its transaction");
    // Structural: a type change into another family adopts a fitting
    // envelope and undoes back to the original macro.
    const QString type1 = run(
        QStringLiteral("var SPEC = porydaw.voicegroup.voice(%1).type.indexOf('directsound') >= 0 ? "
                       "{type: 'voice_square_1'} : {type: 'voice_directsound', symbol: "
                       "porydaw.voicegroup.symbols().directSound[0]}; "
                       "porydaw.edit.transaction('Type', function () { porydaw.edit.setVoice(%1, "
                       "SPEC); }); porydaw.voicegroup.voice(%1).type")
            .arg(slot));
    {
        const VgVoice *v1 = session.vgSource->voiceAt(slot);
        const bool cgb1 = v1 && vgMacroIsCgb(v1->macro);
        check(v1 && v1->macro != v0.macro && type1 == vgMacroName(v1->macro) &&
                  v1->attack <= (cgb1 ? 7 : 255) && v1->sustain <= (cgb1 ? 15 : 255) &&
                  v1->release <= (cgb1 ? 7 : 255) && undo.index() == index0 + 1,
              "a structural setVoice (type change) did not apply with an envelope in range");
    }
    undo.undo();
    check(session.vgSource->voiceAt(slot)->macro == v0.macro &&
              *session.vgSource->voiceAt(slot) == v0 && !session.vgSource->dirty(),
          "undoing a structural voice edit did not restore the original macro line");
    // A crossing with a partial envelope: the given key lands, the others
    // come from the typical envelope, never from the old family's digits.
    {
        const int a = cgb0 ? 200 : 2; // fits the target family only
        run(QStringLiteral("var SPEC2 = Object.assign({attack: %2}, SPEC); "
                           "porydaw.edit.transaction('Type2', function () { "
                           "porydaw.edit.setVoice(%1, SPEC2); })")
                .arg(slot)
                .arg(a));
        const VgVoice *v2 = session.vgSource->voiceAt(slot);
        check(v2 && v2->macro != v0.macro && v2->attack == a &&
                  v2->release <= (vgMacroIsCgb(v2->macro) ? 7 : 255) &&
                  v2->sustain <= (vgMacroIsCgb(v2->macro) ? 15 : 255),
              "a partial envelope on a family change did not land the given key on a fitting "
              "envelope");
        undo.undo();
        check(*session.vgSource->voiceAt(slot) == v0 && !session.vgSource->dirty(),
              "undo after the partial-envelope crossing did not restore the voice");
    }
    check(run(QStringLiteral("porydaw.edit.transaction('B1', function () { porydaw.edit.setVoice("
                             "999, {attack: 1}); })"))
                  .isNull() &&
              errorLogged("not an editable voice"),
          "setVoice on a slot outside the voicegroup was accepted");
    check(run(QStringLiteral("porydaw.edit.transaction('B2', function () { porydaw.edit.setVoice("
                             "%1, {type: 'voice_bogus'}); })")
                  .arg(slot))
                  .isNull() &&
              errorLogged("unknown voice type"),
          "setVoice with an unknown type was accepted");
    check(run(QStringLiteral("porydaw.edit.transaction('B3', function () { porydaw.edit.setVoice("
                             "%1, {bogus: 1}); })")
                  .arg(slot))
                  .isNull() &&
              errorLogged("unknown voice field"),
          "setVoice with an unknown field was accepted");
    check(run(QStringLiteral("porydaw.edit.setVoice(%1, {attack: 1})").arg(slot)).isNull() &&
              errorLogged("inside porydaw.edit.transaction"),
          "setVoice outside a transaction was allowed");
    check(*session.vgSource->voiceAt(slot) == v0 && !session.vgSource->dirty() &&
              undo.index() == index0,
          "a refused setVoice touched the voice or the undo stack");

    // A transaction's voice edit survives a voicegroup switch and back:
    // the reopened source replays the macro's children.
    if (swaps) {
        run(QStringLiteral("porydaw.edit.transaction('Voice4', function () { "
                           "porydaw.edit.setVoice(%1, {attack: %2}); }); "
                           "porydaw.edit.transaction('VG3', function () { "
                           "porydaw.edit.setSettings({voicegroup: '%3'}); })")
                .arg(slot)
                .arg(attack1)
                .arg(otherArg));
        check(session.vgSource && session.vgSource->filePath() != vgPath &&
                  undo.index() == index0 + 2,
              "the voicegroup switch after a voice edit did not open the other source");
        undo.undo();
        check(session.vgSource && session.vgSource->filePath() == vgPath &&
                  session.vgSource->voiceAt(slot)->attack == attack1 && session.vgSource->dirty(),
              "a transaction's voice edit was not replayed after undoing a voicegroup switch");
        undo.undo();
        check(session.vgSource->voiceAt(slot)->attack == v0.attack && !session.vgSource->dirty() &&
                  undo.index() == index0,
              "undoing the replayed voice edit did not restore the voice");
        // The same inside ONE transaction: the open macro isn't counted by
        // index() yet, but its voice edit must survive the switch and back.
        run(QStringLiteral("porydaw.edit.transaction('Voice5', function () { "
                           "porydaw.edit.setVoice(%1, {attack: %2}); "
                           "porydaw.edit.setSettings({voicegroup: '%3'}); "
                           "porydaw.edit.setSettings({voicegroup: '%4'}); })")
                .arg(slot)
                .arg(attack1)
                .arg(otherArg, cfg0.voicegroupArg));
        check(session.vgSource && session.vgSource->filePath() == vgPath &&
                  session.vgSource->voiceAt(slot)->attack == attack1 && session.vgSource->dirty() &&
                  undo.index() == index0 + 1,
              "a voice edit earlier in the same transaction was lost by a voicegroup switch "
              "and back");
        undo.undo();
        check(session.vgSource->voiceAt(slot)->attack == v0.attack && !session.vgSource->dirty() &&
                  undo.index() == index0,
              "undoing the switch-and-back transaction did not restore the voice");
    }

    // --- song.save() writes the edited voicegroup ---
    const QByteArray vgBytes0 = fileBytes(vgPath);
    check(runc("porydaw.edit.transaction('S', function () { porydaw.song.save(); })").isNull() &&
              errorLogged("inside a transaction"),
          "song.save() inside a transaction was allowed");
    run(QStringLiteral("porydaw.edit.transaction('Voice2', function () { "
                       "porydaw.edit.setVoice(%1, {attack: %2}); })")
            .arg(slot)
            .arg(attack1));
    check(runc("porydaw.song.save()") == QStringLiteral("true") && !session.vgSource->dirty() &&
              undo.isClean() && fileBytes(vgPath) != vgBytes0 &&
              runc("porydaw.voicegroup.dirty") == QStringLiteral("false"),
          "song.save() did not write the edited voicegroup");
    run(QStringLiteral("porydaw.edit.transaction('Voice3', function () { "
                       "porydaw.edit.setVoice(%1, {attack: %2}); }); porydaw.song.save()")
            .arg(slot)
            .arg(v0.attack));
    check(fileBytes(vgPath) == vgBytes0 && !session.vgSource->dirty(),
          "editing the voice back and saving did not restore the voicegroup file byte for byte");
    const int index1 = undo.index();

    // --- project.createVoicegroup ---
    const QString newVgFile = projectRoot + QStringLiteral("/sound/voicegroups/plugin_test.inc");
    {
        QString cleanupError;
        VoicegroupSource::removeIncludeLine(projectRoot, QStringLiteral("plugin_test"),
                                            &cleanupError);
        VoicegroupSource::deleteVoicegroup(projectRoot, QStringLiteral("plugin_test"),
                                           &cleanupError);
        runc("porydaw.project.reload()");
    }
    check(runc("porydaw.project.createVoicegroup('plugin_test', {copyFrom: "
               "porydaw.voicegroup.arg})") == QStringLiteral("_plugin_test") &&
              QFile::exists(newVgFile) &&
              runc("porydaw.project.voicegroups().some(function (g) { return g.arg === "
                   "'_plugin_test' && g.name === 'plugin_test'; })") == QStringLiteral("true"),
          "project.createVoicegroup did not create the file and list it");
    check(runc("porydaw.project.createVoicegroup('plugin_test')").isNull() &&
              errorLogged("already exists") &&
              runc("porydaw.project.createVoicegroup('9bad')").isNull() &&
              errorLogged("not a valid voicegroup name"),
          "createVoicegroup accepted a duplicate or an invalid name");
    run(QStringLiteral("porydaw.edit.transaction('VG2', function () { "
                       "porydaw.edit.setSettings({voicegroup: '_plugin_test'}); })"));
    check(doc.cfg().voicegroupArg == QStringLiteral("_plugin_test") &&
              (!swaps ||
               (session.vgSource && session.vgSource->filePath() == newVgFile &&
                runc("porydaw.voicegroup.file") == newVgFile &&
                run(QStringLiteral("porydaw.voicegroup.voice(%1).symbol").arg(slot)) == v0.symbol)),
          "switching to the created copy did not open it with the copied voices");
    undo.undo();
    check(undo.index() == index1 && doc.cfg().voicegroupArg == cfg0.voicegroupArg,
          "undo did not leave the created voicegroup behind");
    {
        QString cleanupError;
        check(VoicegroupSource::removeIncludeLine(projectRoot, QStringLiteral("plugin_test"),
                                                  &cleanupError) &&
                  VoicegroupSource::deleteVoicegroup(projectRoot, QStringLiteral("plugin_test"),
                                                     &cleanupError),
              "could not delete the created voicegroup again");
    }
    check(runc("porydaw.project.reload(); porydaw.project.voicegroups().some(function (g) { "
               "return g.arg === '_plugin_test'; })") == QStringLiteral("false"),
          "project.reload() did not drop the deleted voicegroup from the catalog");

    // --- registration on a copied .mid ---
    const QString regLabel = QStringLiteral("mus_plugin_reg");
    const QString midiDir = projectRoot + QStringLiteral("/sound/songs/midi");
    const QString regMid = midiDir + QStringLiteral("/") + regLabel + QStringLiteral(".mid");
    {
        // A previous run on this scratch may have left the song behind.
        QString cleanupError;
        if (run(QStringLiteral("porydaw.project.song('%1') !== null").arg(regLabel)) ==
            QStringLiteral("true")) {
            run(QStringLiteral("porydaw.project.unregisterSong('%1')").arg(regLabel));
        }
        QFile::remove(regMid);
        SongRegistry::removeSongFlags(midiDir, regLabel, &cleanupError);
        runc("porydaw.project.reload()");
    }
    check(QFile::copy(doc.midPath(), regMid), "could not copy the song's .mid for registration");
    check(run(QStringLiteral("porydaw.project.reload(); var R = porydaw.project.song('%1'); R && "
                             "R.hasMid && !R.registered")
                  .arg(regLabel)) == QStringLiteral("true") &&
              run(QStringLiteral("porydaw.project.registration('%1').inSongTable").arg(regLabel)) ==
                  QStringLiteral("false"),
          "a new .mid did not appear as an unregistered song after project.reload()");
    check(run(QStringLiteral("porydaw.project.registerSong('%1', {player: 'MUSIC_PLAYER_ZZZ'})")
                  .arg(regLabel))
                  .isNull() &&
              errorLogged("not a music player"),
          "registerSong accepted an unknown music player");
    check(runc("porydaw.project.registerSong('nope_zzz_not_a_song')").isNull() &&
              errorLogged("no song named"),
          "registerSong of an unknown label did not throw");
    check(run(QStringLiteral("porydaw.project.registerSong('%1', {constant: 'bad constant'})")
                  .arg(regLabel))
                  .isNull() &&
              errorLogged("not a valid song constant"),
          "registerSong accepted a constant that is not an identifier");
    check(run(QStringLiteral("porydaw.edit.transaction('R', function () { "
                             "porydaw.project.registerSong('%1'); })")
                  .arg(regLabel))
                  .isNull() &&
              errorLogged("inside a transaction"),
          "registerSong inside a transaction was allowed");
    // null options mean the defaults, like undefined.
    const QString newId =
        run(QStringLiteral("porydaw.project.registerSong('%1', {constant: null, player: null})")
                .arg(regLabel));
    check(newId.toInt() > 0 &&
              run(QStringLiteral("porydaw.project.song('%1').registered && "
                                 "porydaw.project.song('%1').constant === 'MUS_PLUGIN_REG' && "
                                 "porydaw.project.registration('%1').complete")
                      .arg(regLabel)) == QStringLiteral("true"),
          "project.registerSong did not register the song (id, constant, complete status)");
    {
        const DecompProject *project = host.bindings().project;
        check(project && session.songId >= 0 && session.songId < project->songs().size() &&
                  project->songs().at(session.songId).label == songLabel &&
                  runc("porydaw.song.label") == songLabel,
              "the active session lost its song id across the registration reload");
    }
    run(QStringLiteral("porydaw.project.unregisterSong('%1')").arg(regLabel));
    check(run(QStringLiteral("!porydaw.project.song('%1').registered && "
                             "!porydaw.project.registration('%1').inSongTable")
                  .arg(regLabel)) == QStringLiteral("true"),
          "project.unregisterSong did not drop the registration");
    check(run(QStringLiteral("porydaw.project.unregisterSong('%1'); 'ok'").arg(regLabel)) ==
              QStringLiteral("ok"),
          "unregistering an unregistered song is not a no-op success");
    {
        QString cleanupError;
        QFile::remove(regMid);
        SongRegistry::removeSongFlags(midiDir, regLabel, &cleanupError);
    }
    check(
        run(QStringLiteral("porydaw.project.reload(); porydaw.project.song('%1')").arg(regLabel)) ==
            QStringLiteral("null"),
        "project.reload() did not drop the removed .mid");
    undo.setClean();
}

// Takes an imported song back out of the scratch project (its .mid, flags,
// registration, view sidecar and its own voicegroup — never keepArg's), so
// a run leaves the project as it found it and a re-run on the same scratch
// starts clean.
void removeImportedSong(scripting::ScriptHost &host, const QString &projectRoot,
                        const QString &label, const QString &keepArg)
{
    const QString arg = host.evalConsole(
        QStringLiteral("(porydaw.project.song('%1') || {settings: {voicegroup: ''}})"
                       ".settings.voicegroup")
            .arg(label));
    // Closing the import's tab saved its view state; an earlier run (before
    // this cleanup knew about it) may have left one without the song.
    SongRegistry::removeSongSidecar(projectRoot, label);
    if (host.evalConsole(QStringLiteral("porydaw.project.song('%1') !== null").arg(label)) !=
        QStringLiteral("true"))
        return;
    host.evalConsole(QStringLiteral("porydaw.project.unregisterSong('%1')").arg(label));
    QString cleanupError;
    const QString midiDir = projectRoot + QStringLiteral("/sound/songs/midi");
    QFile::remove(midiDir + QLatin1Char('/') + label + QStringLiteral(".mid"));
    SongRegistry::removeSongFlags(midiDir, label, &cleanupError);
    if (arg.startsWith(QLatin1Char('_')) && arg != keepArg) {
        VoicegroupSource::removeIncludeLine(projectRoot, arg.mid(1), &cleanupError);
        VoicegroupSource::deleteVoicegroup(projectRoot, arg.mid(1), &cleanupError);
    }
    host.evalConsole(QStringLiteral("porydaw.project.reload()"));
}

// porydaw.project.exportBundle / importBundle against the scratch project,
// and the read-only gates a bundle tab puts on the editing API.
void runBundleChecks(const Check &check, scripting::ScriptHost &host, MainWindow &window,
                     QList<Message> &messages, const QString &projectRoot, const QString &songLabel,
                     bool audioOk)
{
    const auto run = [&](const QString &code) { return host.evalConsole(code); };
    const auto errorLogged = [&](const char *fragment) {
        return hasMessage(messages, QStringLiteral("console"), 2, QLatin1String(fragment));
    };
    // The console has no plugin folder: its sandbox is the project.
    const QString bundlePath = projectRoot + QStringLiteral("/plugin_bundle_test.porysong");
    QFile::remove(bundlePath);
    const QString importLabel = songLabel + QStringLiteral("_plugin");
    // A previous run on this scratch may have left the import behind.
    removeImportedSong(host, projectRoot, importLabel,
                       run(QStringLiteral("porydaw.song.settings().voicegroup")));

    // A refusal: the call throws (null result) and logs this fragment. The
    // log is cumulative, so it is cleared first — an earlier check's message
    // must not satisfy this one.
    const auto refused = [&](const QString &code, const char *fragment) {
        messages.clear();
        return run(code).isNull() && errorLogged(fragment);
    };

    // --- exportBundle ---
    check(refused(QStringLiteral("porydaw.project.exportBundle('nope_zzz_not_a_song', '%1')")
                      .arg(bundlePath),
                  "no song named") &&
              !QFile::exists(bundlePath),
          "exportBundle of an unknown label did not throw");
    QTemporaryDir outside;
    const QString outsideBundle = outside.filePath(QStringLiteral("x.porysong"));
    check(refused(QStringLiteral("porydaw.project.exportBundle('%1', '%2')")
                      .arg(songLabel, outsideBundle),
                  "is outside the plugin folder") &&
              !QFile::exists(outsideBundle),
          "exportBundle wrote outside the sandbox");
    check(refused(QStringLiteral("porydaw.edit.transaction('B', function () { "
                                 "porydaw.project.exportBundle('%1', '%2'); })")
                      .arg(songLabel, bundlePath),
                  "inside a transaction") &&
              !QFile::exists(bundlePath),
          "exportBundle inside a transaction was allowed");
    // Any suffix but .porysong is refused (a project file stays what it is);
    // a bare name gets the suffix, like the menu action.
    const QString notBundle = projectRoot + QStringLiteral("/plugin_bundle_test.mid");
    check(refused(
              QStringLiteral("porydaw.project.exportBundle('%1', '%2')").arg(songLabel, notBundle),
              "is not a .porysong path") &&
              !QFile::exists(notBundle),
          "exportBundle wrote a bundle under another suffix");
    check(run(QStringLiteral("porydaw.project.exportBundle('%1', '%2').path")
                  .arg(songLabel, bundlePath.chopped(9))) == bundlePath &&
              MainWindow::isBundlePath(bundlePath),
          "exportBundle did not add the .porysong suffix to a bare name");
    QFile::remove(bundlePath);
    check(run(QStringLiteral("var X = porydaw.project.exportBundle('%1', '%2'); "
                             "X.path === '%2' && X.samples >= 0")
                  .arg(songLabel, bundlePath)) == QStringLiteral("true") &&
              MainWindow::isBundlePath(bundlePath),
          "project.exportBundle did not write the bundle of the open song");
    // A real bundle outside the sandbox: only the sandbox can refuse it.
    check(QFile::copy(bundlePath, outsideBundle) &&
              refused(QStringLiteral("porydaw.project.importBundle('%1')").arg(outsideBundle),
                      "is outside the plugin folder") &&
              run(QStringLiteral("porydaw.project.song('%1')").arg(importLabel)) ==
                  QStringLiteral("null"),
          "importBundle read outside the sandbox");
    // A song that is not open in any tab exports from disk.
    const QString otherPath = projectRoot + QStringLiteral("/plugin_bundle_other.porysong");
    QFile::remove(otherPath);
    const QString otherLabel = run(
        QStringLiteral(
            "(porydaw.project.songs().filter(function (s) { return s.hasMid && s.label !== '%1' && "
            "s.settings.voicegroup === porydaw.song.settings().voicegroup; })[0] || {label: ''})"
            ".label")
            .arg(songLabel));
    if (!otherLabel.isEmpty()) {
        check(run(QStringLiteral("porydaw.project.exportBundle('%1', '%2').path")
                      .arg(otherLabel, otherPath)) == otherPath &&
                  MainWindow::isBundlePath(otherPath),
              "project.exportBundle did not export a song that is not open");
        QFile::remove(otherPath);
    }

    // --- importBundle ---
    check(refused(QStringLiteral("porydaw.project.importBundle('%1/sound/song_table.inc')")
                      .arg(projectRoot),
                  "is not a song bundle"),
          "importBundle accepted a file that is not a bundle");
    check(
        refused(
            QStringLiteral("porydaw.project.importBundle('%1/nope_zzz.porysong')").arg(projectRoot),
            "does not exist"),
        "importBundle of a missing file did not say it is missing");
    check(refused(QStringLiteral("porydaw.project.importBundle('%1', {label: '%2'})")
                      .arg(bundlePath, songLabel),
                  "already has a song"),
          "importBundle with a clashing label did not refuse");
    check(refused(QStringLiteral("porydaw.project.importBundle('%1', {player: 'MUSIC_PLAYER_ZZZ'})")
                      .arg(bundlePath),
                  "no music player named"),
          "importBundle accepted an unknown music player");
    check(run(QStringLiteral("porydaw.project.song('%1_plugin')").arg(songLabel)) ==
              QStringLiteral("null"),
          "a refused import left a song behind");
    // Into its own source project: everything is reused, the song and its
    // trimmed voicegroup land under new names, and the song opens in a tab.
    check(run(QStringLiteral(
                  "var I = porydaw.project.importBundle('%1', {label: '%2', constant: null}); "
                  "var S = porydaw.project.song('%2'); "
                  "I.label === '%2' && Array.isArray(I.warnings) && S !== null && S.registered && "
                  "S.constant === I.constant && S.player === I.player && "
                  "S.settings.voicegroup === I.voicegroup && "
                  "porydaw.project.registration('%2').complete && "
                  "porydaw.project.voicegroups().some(function (g) { return g.arg === "
                  "I.voicegroup; })")
                  .arg(bundlePath, importLabel)) == QStringLiteral("true"),
          "project.importBundle did not register the song with its voicegroup");
    if (audioOk) {
        check(run(QStringLiteral("porydaw.song.label")) == importLabel &&
                  run(QStringLiteral("porydaw.song.settings().voicegroup === I.voicegroup")) ==
                      QStringLiteral("true"),
              "the imported song did not open in its own tab");
    }

    // --- a bundle tab is read-only to plugins ---
    QString openError;
    if (audioOk && check(window.openBundle(bundlePath, &openError), "the bundle did not open")) {
        check(run(QStringLiteral("porydaw.song.label")) == songLabel,
              "the bundle tab is not what the API sees");
        check(run(QStringLiteral("porydaw.edit.transaction('L', function () { "
                                 "porydaw.edit.setSettings({reverb: 1}); })"))
                      .isNull() &&
                  errorLogged("the song is read-only (a song bundle)"),
              "edit.transaction on a bundle tab was allowed");
        check(run(QStringLiteral("porydaw.storage.song.set('k', 1)")).isNull() &&
                  errorLogged("storage.song.set: the song is a read-only song bundle") &&
                  run(QStringLiteral("porydaw.storage.song.get('k', 7)")).isNull(),
              "storage.song on a bundle tab was allowed");
        check(run(QStringLiteral("porydaw.song.save()")).isNull() &&
                  errorLogged("song.save: the song could not be saved"),
              "song.save() on a bundle tab did not fail");
        // The project song of the same label is still reachable by label.
        check(run(QStringLiteral("porydaw.project.exportBundle('%1', '%2').path")
                      .arg(songLabel, bundlePath)) == bundlePath,
              "exportBundle by label failed while the bundle tab was active");
    } else if (!openError.isEmpty()) {
        std::fprintf(stderr, "scriptcheck: openBundle: %s\n", qUtf8Printable(openError));
    }
    QFile::remove(bundlePath);
}

int runTapCheck()
{
    int failures = 0;
    const auto check = [&failures](bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "scriptcheck: FAIL: %s\n", what);
            failures++;
        }
    };
    AudioTap tap(1000); // rounds up to 1024
    check(tap.capacity() == 1024, "tap capacity did not round up to a power of two");
    std::vector<float> out(4096);
    check(tap.readLatest(out.data(), 8) == 0 && out[0] == 0.0f && out[15] == 0.0f,
          "reading an empty tap did not yield silence");
    std::vector<float> in;
    for (int i = 0; i < 10; i++) {
        in.push_back(float(i));
        in.push_back(float(-i));
    }
    tap.write(in.data(), 10);
    check(tap.framesWritten() == 10, "framesWritten did not count the write");
    check(tap.readLatest(out.data(), 4) == 4 && out[0] == 6.0f && out[1] == -6.0f && out[6] == 9.0f,
          "readLatest did not return the newest frames oldest-first");
    check(tap.readLatest(out.data(), 16) == 10 && out[0] == 0.0f && out[11] == 0.0f &&
              out[12] == 0.0f && out[13] == 0.0f && out[30] == 9.0f,
          "readLatest did not pad pre-history with silence");
    in.clear();
    for (int i = 0; i < 2000; i++) {
        in.push_back(float(i));
        in.push_back(float(-i));
    }
    tap.write(in.data(), 2000); // wraps and overruns the capacity
    check(tap.framesWritten() == 2010 && tap.readLatest(out.data(), 1024) == 1024 &&
              out[0] == 976.0f && out[2046] == 1999.0f && out[2047] == -1999.0f,
          "an oversized write did not keep exactly the newest capacity");

    // FFT: DC only lands in bin 0.
    std::vector<float> reim(16, 0.0f);
    for (int i = 0; i < 8; i++)
        reim[i * 2] = 1.0f;
    AudioAnalyzer::fft(reim.data(), 8);
    check(std::fabs(reim[0] - 8.0f) < 1e-4f && std::fabs(reim[2]) < 1e-4f &&
              std::fabs(reim[14]) < 1e-4f,
          "fft of DC is wrong");

    // Analyzer: a half-scale sine at bin 8 of a 256 window.
    AudioTap tap2(4096);
    AudioAnalyzer an(256);
    check(an.windowFrames() == 256, "analyzer window did not round");
    in.clear();
    for (int i = 0; i < 1024; i++) {
        const float v = 0.5f * std::sin(2.0f * kScriptCheckPi * 8.0f * float(i) / 256.0f);
        in.push_back(v);
        in.push_back(v);
    }
    tap2.write(in.data(), 1024);
    check(an.poll(tap2) == 1024 && an.newFrames() == 1024, "poll did not count the new frames");
    check(std::fabs(an.peak(0) - 0.5f) < 0.01f && std::fabs(an.rms(1) - 0.3536f) < 0.02f,
          "peak/rms of a half-scale sine are wrong");
    const std::vector<float> &spec = an.spectrum(128);
    const auto argmax = [](const std::vector<float> &v) {
        return int(std::max_element(v.begin(), v.end()) - v.begin());
    };
    check(spec.size() == 128 && argmax(spec) == 8 && spec[8] > 0.4f && spec[8] < 0.6f &&
              spec[40] < 0.01f,
          "spectrum did not place a bin-8 sine at bin 8 with ~0.5 magnitude");
    const std::vector<float> &bands = an.spectrum(16);
    check(bands.size() == 16 && argmax(bands) == 1, "spectrum banding did not fold bins");
    check(an.spectrum(0).size() == 1 && an.spectrum(100000).size() == 128,
          "spectrum bins were not clamped");
    check(an.poll(tap2) == 0 && an.peak(0) == 0.0f && an.rms(0) == 0.0f,
          "a poll with nothing new did not read silence");
    return failures;
}

} // namespace

bool MainWindow::runScriptHostCheck(const QString &pluginsDir, const QString &projectRoot,
                                    const QString &songLabel)
{
    int failures = 0;
    const auto check = [&failures](bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "scriptcheck: FAIL: %s\n", what);
            failures++;
        }
        return ok;
    };
    scripting::ScriptHost &host = *m_scriptHost;
    auto &keys = keymap::Registry::instance();
    QList<Message> messages;
    connect(&host, &scripting::ScriptHost::message, this,
            [&messages](const QString &plugin, int level, const QString &text) {
                messages.append({plugin, level, text});
            });

    // --- fixtures ---
    const QString fixtureDir = pluginsDir + QStringLiteral("/fixture");
    check(
        writeFile(fixtureDir + QStringLiteral("/plugin.json"), QLatin1String(kFixtureManifest)) &&
            writeFile(fixtureDir + QStringLiteral("/main.js"), QLatin1String(kFixtureMainV1)) &&
            writeFile(pluginsDir + QStringLiteral("/broken/plugin.json"),
                      QLatin1String(kBrokenManifest)) &&
            writeFile(pluginsDir + QStringLiteral("/broken/main.js"), QLatin1String(kBrokenMain)) &&
            writeFile(pluginsDir + QStringLiteral("/badmanifest/plugin.json"),
                      QLatin1String(kBadManifest)) &&
            writeFile(pluginsDir + QStringLiteral("/runaway/plugin.json"),
                      QLatin1String(kRunawayManifest)) &&
            writeFile(pluginsDir + QStringLiteral("/runaway/main.js"),
                      QLatin1String(kRunawayMain)) &&
            writeFile(pluginsDir + QStringLiteral("/syntax/plugin.json"),
                      QLatin1String(kSyntaxManifest)) &&
            writeFile(pluginsDir + QStringLiteral("/syntax/main.js"), QLatin1String(kSyntaxMain)) &&
            writeFile(pluginsDir + QStringLiteral("/notaplugin/readme.txt"),
                      QStringLiteral("no manifest")) &&
            writeFile(pluginsDir + QStringLiteral("/panel/plugin.json"),
                      QLatin1String(kPanelManifest)) &&
            writeFile(pluginsDir + QStringLiteral("/panel/main.js"), QLatin1String(kPanelMain)),
        "could not write fixture plugins");
    {
        QImage pic(16, 9, QImage::Format_ARGB32);
        pic.fill(QColor(255, 0, 0));
        check(pic.save(pluginsDir + QStringLiteral("/panel/pic.png")),
              "could not write the panel fixture image");
    }

    // The bundled examples double as API smoke tests (PLAN §8): copied in
    // from the source tree when the binary runs out of its build dir.
    bool haveExamples = false;
    {
        // Walk up from the binary rather than assuming one fixed depth:
        // on macOS the executable sits three levels down inside
        // porydaw.app/Contents/MacOS, and "../plugins/examples" there
        // silently found nothing, skipping every example check below.
        QString found;
        QDir up(QCoreApplication::applicationDirPath());
        for (int level = 0; found.isEmpty() && level < 6; ++level) {
            const QString candidate = up.filePath(QStringLiteral("plugins/examples"));
            if (QFileInfo(candidate).isDir())
                found = candidate;
            else if (!up.cdUp())
                break;
        }
        check(!found.isEmpty(), "could not find plugins/examples above the binary; every "
                                "example plugin check would have been skipped");
        const QDir examples(found);
        for (const QString &name :
             examples.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            const QDir src(examples.filePath(name));
            const QString dst = pluginsDir + QLatin1Char('/') + name;
            QDir().mkpath(dst);
            for (const QString &file : src.entryList(QDir::Files))
                QFile::copy(src.filePath(file), dst + QLatin1Char('/') + file);
            haveExamples = true;
        }
    }

    const int shippedCommands = keys.commands().size();
    // A user who moved Save off Ctrl+S: the plugin's Ctrl+S default must
    // still be refused, since Reset would bring the collision back.
    keys.setBinding(QStringLiteral("file.save_song"),
                    QKeySequence(QStringLiteral("Ctrl+Alt+Shift+F7")));
    host.setPluginsDir(pluginsDir);
    host.loadAll();
    keys.resetBinding(QStringLiteral("file.save_song"));

    // --- discovery + states ---
    QStringList expectedIds{QStringLiteral("badmanifest"), QStringLiteral("broken"),
                            QStringLiteral("fixture"),     QStringLiteral("panel"),
                            QStringLiteral("runaway"),     QStringLiteral("syntax")};
    int exampleCommands = 0;
    if (haveExamples) {
        expectedIds.append(QStringLiteral("select-same-pitch"));
        expectedIds.append(QStringLiteral("note-tools"));
        expectedIds.append(QStringLiteral("vu-meter"));
        expectedIds.append(QStringLiteral("spectrum"));
        expectedIds.append(QStringLiteral("dancer"));
        expectedIds.append(QStringLiteral("song-report"));
        expectedIds.append(QStringLiteral("range-tools"));
        expectedIds.append(QStringLiteral("scale-guide"));
        expectedIds.append(QStringLiteral("project-tools"));
        expectedIds.append(QStringLiteral("scale-snap"));
        expectedIds.append(QStringLiteral("arpeggiate"));
        expectedIds.sort();
        // The Phase 3 examples each open a dock in activate().
        for (const char *id : {"vu-meter", "spectrum", "dancer"}) {
            const scripting::Plugin *p = host.plugin(QLatin1String(id));
            check(p && p->state == scripting::PluginState::Loaded && p->docks.size() == 1 &&
                      p->docks.front() && findChild<QDockWidget *>(p->docks.front()->objectName()),
                  "a Phase 3 example plugin did not load with its dock");
        }
        const scripting::Plugin *example = host.plugin(QStringLiteral("select-same-pitch"));
        check(example && example->state == scripting::PluginState::Loaded,
              "bundled example plugin select-same-pitch did not load");
        check(keys.command(QStringLiteral("plugin.select-same-pitch.select")).context ==
                  keymap::Context::PianoRoll,
              "bundled example did not register its roll command");
        exampleCommands = example ? int(example->actions.size()) : 0;
        const scripting::Plugin *tools = host.plugin(QStringLiteral("note-tools"));
        check(tools && tools->state == scripting::PluginState::Loaded && tools->actions.size() == 5,
              "bundled example plugin note-tools did not load its 5 commands");
        check(!hasMessage(messages, QStringLiteral("note-tools"), 1, QStringLiteral("already")),
              "a note-tools default shortcut collides with a shipped binding");
        exampleCommands += tools ? int(tools->actions.size()) : 0;
        // The Phase 4 examples: menus, context items, an overlay.
        {
            const scripting::Plugin *report = host.plugin(QStringLiteral("song-report"));
            QMenu *reportMenu = findChild<QMenu *>(QStringLiteral("plugin.song-report.menu"));
            check(report && report->state == scripting::PluginState::Loaded && reportMenu &&
                      reportMenu->actions().size() == 2,
                  "song-report did not load with two Plugins-menu entries");
            const scripting::Plugin *range = host.plugin(QStringLiteral("range-tools"));
            check(range && range->state == scripting::PluginState::Loaded &&
                      range->contextItems.size() == 3 && range->actions.size() == 1,
                  "range-tools did not load with three range-menu items and one command");
            exampleCommands += range ? int(range->actions.size()) : 0;
            const scripting::Plugin *snap = host.plugin(QStringLiteral("scale-snap"));
            check(snap && snap->state == scripting::PluginState::Loaded &&
                      snap->actions.size() == 1 &&
                      findChild<QMenu *>(QStringLiteral("plugin.scale-snap.menu")),
                  "scale-snap did not load with its toggle command and menu");
            exampleCommands += snap ? int(snap->actions.size()) : 0;
            const scripting::Plugin *scale = host.plugin(QStringLiteral("scale-guide"));
            check(scale && scale->state == scripting::PluginState::Loaded &&
                      scale->overlays.size() == 1 && scale->contextItems.size() == 1 &&
                      findChild<QMenu *>(QStringLiteral("plugin.scale-guide.menu")),
                  "scale-guide did not load with its overlay, note-menu item and menu");
            // The manual's tutorial plugin: one roll command + one note-menu item.
            const scripting::Plugin *arp = host.plugin(QStringLiteral("arpeggiate"));
            check(arp && arp->state == scripting::PluginState::Loaded && arp->actions.size() == 1 &&
                      arp->contextItems.size() == 1 &&
                      keys.command(QStringLiteral("plugin.arpeggiate.arpeggiate")).context ==
                          keymap::Context::PianoRoll,
                  "arpeggiate did not load with its roll command and note-menu item");
            check(!hasMessage(messages, QStringLiteral("arpeggiate"), 1, QStringLiteral("already")),
                  "the arpeggiate default shortcut collides with a shipped binding");
            exampleCommands += arp ? int(arp->actions.size()) : 0;
            QMenu *pluginsMenu = findChild<QMenu *>(QStringLiteral("pluginsMenu"));
            check(pluginsMenu && pluginsMenu->menuAction()->isVisible(),
                  "the Plugins menu is hidden although plugins filled it");
        }
    }
    check(host.pluginIds() == expectedIds,
          "plugin discovery did not list exactly the manifest-bearing folders, sorted");
    const scripting::Plugin *fixture = host.plugin(QStringLiteral("fixture"));
    const scripting::Plugin *broken = host.plugin(QStringLiteral("broken"));
    const scripting::Plugin *bad = host.plugin(QStringLiteral("badmanifest"));
    if (!check(fixture && broken && bad, "fixture plugins missing from the host"))
        return false;
    check(fixture->state == scripting::PluginState::Loaded, "fixture plugin did not load");
    check(fixture->manifest.name == QStringLiteral("Fixture Plugin") &&
              fixture->manifest.apiMajor == 1,
          "manifest fields (name, api \"1.0\" → major 1) not parsed");
    check(broken->state == scripting::PluginState::Error &&
              broken->error.contains(QStringLiteral("boom")) &&
              broken->error.contains(QStringLiteral("main.js:3")),
          "activate() throwing did not fault the plugin with file:line");
    check(!broken->engine, "a faulted plugin kept its engine");
    check(bad->state == scripting::PluginState::Error &&
              bad->error.contains(QStringLiteral("API 99")),
          "api major mismatch was not refused with the version in the message");
    check(hasMessage(messages, QStringLiteral("fixture"), 0,
                     QStringLiteral("activated v1 fixture 1.0.0")),
          "porydaw.log from activate() did not reach the console signal");
    check(hasMessage(messages, QStringLiteral("broken"), 2, QStringLiteral("boom")),
          "activate() error was not logged at error level");
    const scripting::Plugin *syntax = host.plugin(QStringLiteral("syntax"));
    check(syntax && syntax->state == scripting::PluginState::Error && !syntax->engine &&
              hasMessage(messages, QStringLiteral("syntax"), 2, QStringLiteral("main.js")),
          "a module syntax error was not faulted and logged to the console");

    // --- actions in the keymap ---
    const QString hello = QStringLiteral("plugin.fixture.hello");
    check(keys.command(hello).id == hello &&
              keys.command(hello).category == QStringLiteral("Fixture Plugin") &&
              keys.command(hello).context == keymap::Context::Global,
          "global action not registered under the plugin's name");
    check(keys.bindings(hello) ==
              QList<QKeySequence>{QKeySequence(QStringLiteral("Ctrl+Alt+Shift+F9"))},
          "action default binding not applied");
    check(keys.commands().size() == shippedCommands + 8 + exampleCommands,
          "dynamic commands not appended to the registry (expected 7 fixture + 1 runaway)");
    check(keys.bindings(QStringLiteral("plugin.fixture.conflict")).isEmpty() &&
              hasMessage(messages, QStringLiteral("fixture"), 1, QStringLiteral("already used by")),
          "a default that collides with a shipped roll binding was not dropped with a warning");
    check(keys.bindings(QStringLiteral("plugin.fixture.save")).isEmpty() &&
              hasMessage(messages, QStringLiteral("fixture"), 1, QStringLiteral("file.save_song")),
          "a default that collides with a shipped DEFAULT (currently rebound) was not dropped");
    check(keys.bindings(QStringLiteral("plugin.fixture.esc")).isEmpty() &&
              hasMessage(messages, QStringLiteral("fixture"), 1, QStringLiteral("Escape")),
          "an Escape default was not refused");
    check(keys.command(QStringLiteral("plugin.fixture.nope")).id.isEmpty(),
          "unknown command id did not report empty");
    check(host.runCommand(hello) &&
              storedCounter(QStringLiteral("fixture"), QStringLiteral("ran")) == 1,
          "runCommand did not run the action (storage counter)");
    check(!host.runCommand(QStringLiteral("plugin.fixture.nope")),
          "runCommand accepted an unknown id");
    QAction *helloAction = nullptr;
    for (QAction *a : actions()) {
        if (a->text() == QStringLiteral("Hello"))
            helloAction = a;
    }
    check(helloAction &&
              helloAction->shortcut() == QKeySequence(QStringLiteral("Ctrl+Alt+Shift+F9")),
          "global action has no window-level QAction with the binding");
    if (helloAction)
        helloAction->trigger();
    check(storedCounter(QStringLiteral("fixture"), QStringLiteral("ran")) == 2,
          "triggering the window QAction did not run the plugin action");

    // Editor-context actions dispatch through the key handler.
    QKeyEvent rollKey(QEvent::KeyPress, Qt::Key_F10,
                      Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier);
    QKeyEvent rangeKey(QEvent::KeyPress, Qt::Key_F11,
                       Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier);
    QKeyEvent otherKey(QEvent::KeyPress, Qt::Key_F10, Qt::ControlModifier);
    using keymap::Context;
    QKeyEvent velKey(QEvent::KeyPress, Qt::Key_F8,
                     Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier);
    check(host.handleKey(&rollKey, Context::PianoRoll, false) &&
              storedCounter(QStringLiteral("fixture"), QStringLiteral("roll")) == 1,
          "roll-context action did not fire from the key handler");
    check(!host.handleKey(&otherKey, Context::PianoRoll, false),
          "key handler matched an unbound chord");
    check(!host.handleKey(&rollKey, Context::Velocity, false),
          "roll-context action fired from the velocity lane");
    check(!host.handleKey(&velKey, Context::PianoRoll, false),
          "velocity-context action fired from the piano roll");
    check(host.handleKey(&velKey, Context::Velocity, false) &&
              storedCounter(QStringLiteral("fixture"), QStringLiteral("vel")) == 1,
          "velocity-context action did not fire from the velocity lane");
    check(!host.handleKey(&rangeKey, Context::PianoRoll, false),
          "range-context action fired without a time selection");
    check(host.handleKey(&rangeKey, Context::Velocity, true) &&
              storedCounter(QStringLiteral("fixture"), QStringLiteral("range")) == 1,
          "range-context action did not fire inside a time selection");

    // The Shortcuts page lists the plugin's group.
    {
        auto *tree = settingsDialog()->shortcutsPage()->findChild<QTreeWidget *>();
        bool found = false;
        for (int i = 0; tree && i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *cat = tree->topLevelItem(i);
            for (int j = 0; j < cat->childCount(); ++j) {
                if (cat->child(j)->data(0, Qt::UserRole).toString() == hello)
                    found = cat->text(0) == QStringLiteral("Fixture Plugin");
            }
        }
        check(found, "Keyboard Shortcuts page does not list the plugin action under its group");
    }

    // --- console ---
    check(host.evalConsole(QStringLiteral("1 + 2")) == QStringLiteral("3"),
          "console did not evaluate 1 + 2");
    check(host.evalConsole(QStringLiteral("porydaw.plugin.id")) == QStringLiteral("console"),
          "console engine has no porydaw API");
    check(host.evalConsole(QStringLiteral("({a: 1})")).contains(QStringLiteral("\"a\": 1")),
          "console did not pretty-print an object");
    check(host.evalConsole(QStringLiteral("nope()")).isNull() &&
              hasMessage(messages, QStringLiteral("console"), 2, QStringLiteral("nope")),
          "console error was not logged");
    check(host.evalConsole(QStringLiteral("porydaw.song.loaded")) == QStringLiteral("false"),
          "song.loaded not false with no song");
    check(host.evalConsole(QStringLiteral("porydaw.actions.register({id: 'x', name: 'X', context: "
                                          "'bogus', run: function(){}})"))
                  .isNull() &&
              hasMessage(messages, QStringLiteral("console"), 2, QStringLiteral("context must be")),
          "bad action context did not throw a TypeError into the script");

    // --- disable / enable ---
    host.setEnabled(QStringLiteral("fixture"), false);
    check(fixture->state == scripting::PluginState::Disabled && !fixture->engine,
          "disabling did not unload the plugin");
    check(hasMessage(messages, QStringLiteral("fixture"), 0, QStringLiteral("deactivated")),
          "deactivate() did not run on disable");
    check(keys.command(hello).id.isEmpty() && !actions().contains(helloAction),
          "disabling left the plugin's command/QAction behind");
    check(QSettings().value(QStringLiteral("plugins/fixture/enabled")).toBool() == false,
          "disabled state not persisted");
    host.setEnabled(QStringLiteral("fixture"), true);
    check(fixture->state == scripting::PluginState::Loaded && keys.command(hello).id == hello,
          "re-enabling did not reload the plugin and its command");

    // --- hot reload keeps user rebinds ---
    keys.setBinding(hello, QKeySequence(QStringLiteral("Ctrl+Alt+Shift+F12")));
    messages.clear();
    check(writeFile(fixtureDir + QStringLiteral("/main.js"), QLatin1String(kFixtureMainV2)),
          "could not rewrite main.js");
    check(waitFor(
              [&] {
                  return hasMessage(messages, QStringLiteral("fixture"), 0,
                                    QStringLiteral("activated v2"));
              },
              4000),
          "editing main.js did not hot-reload the plugin");
    check(hasMessage(messages, QStringLiteral("fixture"), 0, QStringLiteral("deactivated")),
          "hot reload did not deactivate the old instance first");
    check(keys.command(QStringLiteral("plugin.fixture.hello2")).id ==
                  QStringLiteral("plugin.fixture.hello2") &&
              keys.command(QStringLiteral("plugin.fixture.roll")).id.isEmpty(),
          "hot reload did not swap the registered command set");
    check(keys.bindings(hello) ==
              QList<QKeySequence>{QKeySequence(QStringLiteral("Ctrl+Alt+Shift+F12"))},
          "user rebind of a plugin action did not survive the reload");
    keys.resetBinding(hello);

    // --- watchdog ---
    host.setWatchdogMs(200);
    QElapsedTimer clock;
    clock.start();
    check(host.runCommand(QStringLiteral("plugin.runaway.loop")) && clock.elapsed() < 5000,
          "runaway action was not interrupted");
    const scripting::Plugin *runaway = host.plugin(QStringLiteral("runaway"));
    check(runaway && runaway->state == scripting::PluginState::Error &&
              hasMessage(messages, QStringLiteral("runaway"), 2, QStringLiteral("stopped")),
          "watchdog interrupt did not fault the plugin");
    check(waitFor([&] { return !runaway->engine; }, 2000) &&
              keys.command(QStringLiteral("plugin.runaway.loop")).id.isEmpty(),
          "faulted plugin was not torn down (engine + commands)");
    check(fixture->state == scripting::PluginState::Loaded &&
              host.evalConsole(QStringLiteral("2 * 2")) == QStringLiteral("4"),
          "other engines were disturbed by the watchdog");
    host.setWatchdogMs(5000);

    runPanelChecks(check, host, *this, messages, m_pluginPanelsMenu);

    // --- song half ---
    if (!projectRoot.isEmpty()) {
        if (!check(openProjectDir(projectRoot, /*interactive=*/false),
                   "could not open the project"))
            return false;
        loadSongByLabel(songLabel);
        if (!check(m_active != nullptr, "song did not load"))
            return false;
        SongDocument &doc = m_active->doc;
        check(host.evalConsole(QStringLiteral("porydaw.song.loaded")) == QStringLiteral("true"),
              "song.loaded not true after loading");
        check(host.evalConsole(QStringLiteral("porydaw.song.label")) == songLabel,
              "song.label wrong");
        check(host.evalConsole(QStringLiteral("porydaw.song.tracks().length")).toInt() ==
                  doc.engineTrackCount(),
              "song.tracks() count differs from the document");
        check(host.evalConsole(QStringLiteral("porydaw.song.ticksPerBeat")).toInt() ==
                  int(doc.smf().division),
              "song.ticksPerBeat differs from the SMF division");
        int total = 0;
        for (int t = 0; t < doc.engineTrackCount(); ++t)
            total += int(doc.notesForTrack(t).size());
        check(host.evalConsole(QStringLiteral("porydaw.song.notes().length")).toInt() == total &&
                  total > 0,
              "song.notes() count differs from the document");
        check(host.evalConsole(QStringLiteral(
                  "porydaw.song.notes({track: 0, from: 0, to: 1}).every(function(n){ return n.tick "
                  "=== 0 && n.track === 0; })")) == QStringLiteral("true"),
              "song.notes() range/track filter wrong");
        check(host.evalConsole(QStringLiteral(
                  "var n0 = porydaw.song.notes()[0]; porydaw.song.note(n0.id).tick === n0.tick && "
                  "porydaw.song.note(-5) === null")) == QStringLiteral("true"),
              "song.note(id) round trip failed");
        check(host.evalConsole(QStringLiteral("porydaw.project.songs().some(function(s){ return "
                                              "s.label === porydaw.song.label; })")) ==
                  QStringLiteral("true"),
              "project.songs() does not list the loaded song");
        // Selection round trip, then a document edit observed by a listener.
        check(host.evalConsole(QStringLiteral(
                  "porydaw.selection.setNotes(porydaw.song.notes({track: 0}).slice(0, 2)); "
                  "porydaw.selection.notes().length")) == QStringLiteral("2"),
              "selection.setNotes/notes round trip failed");
        check(m_active->view->selection().size() == 2 && m_active->view->selectedTrack() == 0,
              "selection.setNotes did not reach the view");
        host.evalConsole(QStringLiteral("porydaw.cursor.set(1e30)"));
        check(m_active->view->editCursorTick() <= (1ull << 40),
              "cursor.set did not clamp a wild tick");
        host.evalConsole(QStringLiteral("porydaw.cursor.set(NaN)"));
        check(m_active->view->editCursorTick() == 0 &&
                  host.evalConsole(QStringLiteral("porydaw.song.note(NaN)")) ==
                      QStringLiteral("null"),
              "NaN tick/id did not read as 0 / null");
        host.evalConsole(QStringLiteral("porydaw.cursor.set(96)"));
        check(m_active->view->editCursorTick() == 96 &&
                  host.evalConsole(QStringLiteral("porydaw.cursor.tick")) == QStringLiteral("96"),
              "cursor.set did not move the edit cursor");
        check(host.evalConsole(QStringLiteral("porydaw.cursor.grid(96).beatTicks")).toInt() > 0,
              "cursor.grid reports no beat ticks");
        host.evalConsole(QStringLiteral("porydaw.song.on('changed', function (e) { "
                                        "porydaw.storage.set('rev', e.revision); "
                                        "porydaw.storage.set('origin', e.origin); })"));
        const uint64_t before = doc.revision();
        doc.setStartTempo(doc.startTempo() == 120 ? 121 : 120);
        QApplication::processEvents(); // song.changed is delivered after the turn
        check(doc.revision() > before &&
                  storedCounter(QStringLiteral("console"), QStringLiteral("rev")) ==
                      int(doc.revision()) &&
                  QSettings()
                      .value(QStringLiteral("plugins/console/data/origin"))
                      .toByteArray()
                      .contains("user"),
              "song.changed listener did not see the new revision as a 'user' change");
        doc.undoStack()->undo();
        QApplication::processEvents();
        check(storedCounter(QStringLiteral("console"), QStringLiteral("rev")) ==
                      int(doc.revision()) &&
                  QSettings()
                      .value(QStringLiteral("plugins/console/data/origin"))
                      .toByteArray()
                      .contains("history"),
              "song.changed listener did not fire on undo with origin 'history'");
        runEditChecks(check, host, *m_active, messages, haveExamples);
        // Transport through the bindings.
        if (m_audioOk) {
            host.evalConsole(QStringLiteral("porydaw.transport.on('state', function (e) { "
                                            "porydaw.storage.set('tstate', e.state); })"));
            host.evalConsole(QStringLiteral("porydaw.transport.play()"));
            check(host.evalConsole(QStringLiteral("porydaw.transport.state")) ==
                      QStringLiteral("playing"),
                  "transport.play() did not start playback");
            host.tick();
            check(QSettings()
                      .value(QStringLiteral("plugins/console/data/tstate"))
                      .toByteArray()
                      .contains("playing"),
                  "transport.state event did not fire on play");
            host.evalConsole(QStringLiteral("porydaw.transport.stop()"));
            check(host.evalConsole(QStringLiteral("porydaw.transport.state")) ==
                      QStringLiteral("stopped"),
                  "transport.stop() did not stop playback");
        }
        runRealtimeChecks(check, host, *this, messages, m_audioOk);
        runReachChecks(check, host, *this, *m_active, messages, pluginsDir, projectRoot, songLabel);
        runAdapterChecks(check, host, *m_active, messages, projectRoot, songLabel, m_audioOk);
        {
            // The import and the bundle tab each open a tab of their own;
            // the rest of the run stays on the song it started with.
            SongSession *original = m_active;
            QList<QWidget *> before;
            for (int i = 0; i < m_tabs->count(); ++i)
                before.append(m_tabs->widget(i));
            runBundleChecks(check, host, *this, messages, projectRoot, songLabel, m_audioOk);
            for (int i = m_tabs->count() - 1; i >= 0; --i) {
                if (!before.contains(m_tabs->widget(i)))
                    closeTab(i);
            }
            m_tabs->setCurrentWidget(original->view);
            check(m_active == original, "closing the import's tabs lost the original song");
            removeImportedSong(host, projectRoot, songLabel + QStringLiteral("_plugin"),
                               original->doc.cfg().voicegroupArg);
            check(!QFile::exists(projectRoot + QStringLiteral("/.porydaw/") + songLabel +
                                 QStringLiteral("_plugin.json")),
                  "the import's view sidecar outlived the cleanup");
        }
        runEngineSettingsChecks(check, host, *this, messages);
        // Switching to no song drops the API's view.
        activateSession(nullptr);
        check(host.evalConsole(QStringLiteral("porydaw.song.loaded")) == QStringLiteral("false"),
              "song.loaded stayed true after the session went away");
    }

    host.unloadAll();
    check(keys.commands().size() == shippedCommands, "unloadAll left dynamic commands behind");
    disconnect(&host, nullptr, this, nullptr);
    return failures == 0;
}

int runScriptCheck(const QString &projectRoot, const QString &songLabel)
{
    int failures = runEngineCheck() + runTapCheck();

    QTemporaryDir settingsDir;
    QTemporaryDir pluginsDir;
    if (!settingsDir.isValid() || !pluginsDir.isValid()) {
        std::fprintf(stderr, "scriptcheck: no temp dir\n");
        return 1;
    }
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    {
        MainWindow window;
        window.show();
        if (!window.runScriptHostCheck(pluginsDir.path(), projectRoot, songLabel))
            failures++;
    }

    if (failures) {
        std::fprintf(stderr, "scriptcheck: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("scriptcheck: PASS (Qt %s%s)\n", qVersion(),
                projectRoot.isEmpty() ? ", no project" : "");
    return 0;
}

#include "scriptcheck.moc"
