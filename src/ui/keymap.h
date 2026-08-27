#pragma once

#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <array>
#include <optional>

class QAction;
class QKeyEvent;

namespace keymap {

// Where a command's shortcut is live. Global shortcuts are window-level and
// stay active while a local context has focus, so conflict detection treats
// Global as overlapping every other context.
enum class Context {
    Global,
    PianoRoll,
    // The velocity lane's own gestures. Its own context, not the roll's, so
    // the lane and the roll may bind the same modifier chord to their own
    // drags — the pointer is only ever in one of the two surfaces.
    Velocity,
    // The active time selection's own drag gestures (move / duplicate the
    // band's contents). Its own group in Settings, but unlike Velocity it
    // is an overlay, not a separate surface: inside the band its chord is
    // checked before the roll's and the lanes' own press handling, so it
    // conflicts with PianoRoll and Velocity bindings (contextsOverlap).
    // The insert-space chord lives here too: it fires on the same surfaces
    // (band or not), so the same overlap rule covers it.
    TimeSelection,
    EventList,
    // The mouse-wheel actions. Their conflicts are settled among
    // themselves (wheelConflicts), never against key sequences or the
    // hold-and-drag modifier gestures — a wheel chord and a drag chord can
    // share Ctrl without ambiguity, as today's defaults do.
    Wheel,
};

// The four wheel commands, as the song view's surfaces consume them. None
// is an unmatched or unbound chord: it scrolls nothing rather than falling
// back to a zoom.
enum class WheelAction { None, ZoomTimeline, ZoomVertical, PanHorizontal, PanVertical };
constexpr int kWheelActionCount = 4;

struct CommandInfo {
    QString id; // stable, never shown ("roll.transpose_up")
    Context context;
    QString category;             // user-visible group ("File", "Piano Roll", ...)
    QString name;                 // user-visible name
    QList<QKeySequence> defaults; // empty for modifier commands
    // Mouse-gesture modifier command: bound to a bare modifier chord
    // ("hold Ctrl and drag"), not a key sequence.
    bool modifier = false;
    // Mouse-wheel command ("scroll with X held"): also a modifier chord,
    // but NoModifier is a real binding (the plain wheel), so unbound is a
    // distinct state rather than the modifier commands' NoModifier.
    bool wheel = false;
};

// Central shortcut table: every user-configurable binding flows through here.
// Only tier-1 QActions and the piano roll's editor commands are registered;
// widget-internal navigation keys (arrows in lists, Return/Escape in inline
// editors) are platform conventions and deliberately stay hardcoded.
//
// Persistence is delta-only: QSettings holds just the bindings that differ
// from the defaults (an empty stored string means "explicitly unbound"), so
// shipped defaults can evolve without fighting stale full dumps.
class Registry : public QObject
{
    Q_OBJECT
  public:
    static Registry &instance();

    // All commands in stable table order (the settings UI's display order).
    QList<CommandInfo> commands() const;
    CommandInfo command(const QString &id) const;

    // Effective bindings: the user override if one is stored, else the
    // defaults. An overridden command has at most one sequence; defaults may
    // carry alternates (Delete/Backspace, platform StandardKey lists).
    QList<QKeySequence> bindings(const QString &id) const;
    bool isOverridden(const QString &id) const;

    // An empty sequence unbinds the command. Writes QSettings and re-applies
    // attached QActions immediately.
    void setBinding(const QString &id, const QKeySequence &sequence);
    void resetBinding(const QString &id);
    void resetAll();

    // Modifier commands: the effective bare-modifier chord (user override if
    // stored, else the default; NoModifier = unbound, gesture disabled).
    // Sequence bindings() on a modifier command report empty, and vice versa.
    Qt::KeyboardModifiers modifierBinding(const QString &id) const;
    void setModifierBinding(const QString &id, Qt::KeyboardModifiers mods);
    // Exact match against a modifier command after ignoring non-shortcut
    // modifiers such as KeypadModifier.
    bool matchesModifier(Qt::KeyboardModifiers mods, const QString &id) const;
    static bool isModifierKey(int key);

    // Portable storage/display text for a modifier chord ("Ctrl+Shift") and
    // its parse; unknown tokens make the whole parse NoModifier.
    static QString modifierText(Qt::KeyboardModifiers mods);
    static Qt::KeyboardModifiers modifierFromText(const QString &text);

    // Commands other than excludeId whose effective bindings contain
    // sequence and whose context can be active at the same time as context.
    QStringList conflicts(const QString &excludeId, Context context,
                          const QKeySequence &sequence) const;

    // Same, among modifier commands sharing the bare-modifier chord.
    QStringList modifierConflicts(const QString &excludeId, Context context,
                                  Qt::KeyboardModifiers mods) const;

    // Wheel commands: the chord held while scrolling. NoModifier means the
    // bare wheel (a real binding, stored as "None"); nullopt means the
    // action is unbound (stored as the empty marker) and never matches.
    std::optional<Qt::KeyboardModifiers> wheelBinding(const QString &id) const;
    void setWheelBinding(const QString &id, std::optional<Qt::KeyboardModifiers> mods);
    // Exact chord match after ignoring non-shortcut modifiers, like
    // matchesModifier — but the bare chord can match here. Meta is also
    // ignored: it is not a bindable wheel chord, and macOS reports the
    // physical Ctrl key as Meta.
    bool matchesWheel(Qt::KeyboardModifiers mods, const QString &id) const;
    // The wheel action bound to the chord, from a cache of the four
    // effective chords (wheel events arrive at trackpad rates, too fast to
    // consult QSettings each time). None when no action matches.
    WheelAction wheelAction(Qt::KeyboardModifiers mods) const;
    // Other wheel commands whose effective chord equals mods. Two wheel
    // actions on one chord would be ambiguous under the same cursor, so
    // conflicts stay within the wheel kind and ignore Context.
    QStringList wheelConflicts(const QString &excludeId, Qt::KeyboardModifiers mods) const;

    // Single-keystroke match against the command's effective bindings.
    // Keypad/GroupSwitch modifiers are ignored so numpad arrows keep working.
    bool matches(const QKeyEvent *event, const QString &id) const;

    // Applies the command's bindings to the action now and re-applies them on
    // every user change for the action's lifetime.
    void attach(const QString &id, QAction *action);

  signals:
    void bindingsChanged();

  private:
    Registry();
    void applyToActions();
    // Every store mutation funnels through here: drops the wheel-chord
    // cache and notifies listeners.
    void storeChanged();
    // Effective chord per wheel action, rebuilt lazily after storeChanged().
    const std::array<std::optional<Qt::KeyboardModifiers>, kWheelActionCount> &wheelChords() const;
    mutable std::optional<std::array<std::optional<Qt::KeyboardModifiers>, kWheelActionCount>>
        m_wheelChords;
    // Effective chord per modifier command, filled lazily and dropped by
    // storeChanged(): the gesture hover paths ask on every mouse move.
    mutable QHash<QString, Qt::KeyboardModifiers> m_modifierChords;

    struct Attached {
        QString id;
        QPointer<QAction> action;
    };
    QList<Attached> m_actions;
};

} // namespace keymap
