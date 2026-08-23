#include "keymap.h"

#include <QAction>
#include <QKeyEvent>
#include <QSettings>

namespace keymap {
namespace {

struct Def {
    const char *id;
    Context context;
    const char *category;
    const char *name;
    // Platform-adaptive default; UnknownKey means use `keys` instead.
    QKeySequence::StandardKey standard;
    // Portable-text alternates separated by ';' ("Delete;Backspace"). For a
    // modifier command this holds the default modifier chord ("Ctrl").
    const char *keys;
    // Mouse-gesture modifier command ("hold X and drag"): bound to a bare
    // modifier chord, never a key sequence.
    bool modifier;
};

// Stable order: the settings UI lists commands exactly as they appear here.
const Def kDefs[] = {
    // File
    {"file.open_project", Context::Global, QT_TR_NOOP("File"), QT_TR_NOOP("Open Project"),
     QKeySequence::Open, ""},
    {"file.new_song", Context::Global, QT_TR_NOOP("File"), QT_TR_NOOP("New Song"),
     QKeySequence::New, ""},
    {"file.import_midi", Context::Global, QT_TR_NOOP("File"), QT_TR_NOOP("Import MIDI"),
     QKeySequence::UnknownKey, ""},
    {"file.save_song", Context::Global, QT_TR_NOOP("File"), QT_TR_NOOP("Save Song"),
     QKeySequence::Save, ""},
    {"file.register_song", Context::Global, QT_TR_NOOP("File"), QT_TR_NOOP("Register Song"),
     QKeySequence::UnknownKey, ""},
    {"file.close_tab", Context::Global, QT_TR_NOOP("File"), QT_TR_NOOP("Close Tab"),
     QKeySequence::Close, ""},
    {"file.export_wav", Context::Global, QT_TR_NOOP("File"), QT_TR_NOOP("Export WAV"),
     QKeySequence::UnknownKey, ""},
    {"file.quit", Context::Global, QT_TR_NOOP("File"), QT_TR_NOOP("Quit"), QKeySequence::Quit, ""},
    // Edit
    {"edit.undo", Context::Global, QT_TR_NOOP("Edit"), QT_TR_NOOP("Undo"), QKeySequence::Undo, ""},
    {"edit.redo", Context::Global, QT_TR_NOOP("Edit"), QT_TR_NOOP("Redo"), QKeySequence::Redo, ""},
    {"edit.song_settings", Context::Global, QT_TR_NOOP("Edit"), QT_TR_NOOP("Song Settings"),
     QKeySequence::UnknownKey, ""},
    // The platform Preferences key where there is one (⌘, on macOS);
    // Ctrl+, everywhere the platform leaves it unbound.
    {"edit.settings", Context::Global, QT_TR_NOOP("Edit"), QT_TR_NOOP("Settings"),
     QKeySequence::Preferences, "Ctrl+,"},
    // View
    {"view.event_list", Context::Global, QT_TR_NOOP("View"), QT_TR_NOOP("MIDI Event List"),
     QKeySequence::UnknownKey, "Ctrl+Shift+E"},
    {"view.velocity_colors", Context::Global, QT_TR_NOOP("View"),
     QT_TR_NOOP("Color Notes by Velocity"), QKeySequence::UnknownKey, ""},
    {"view.note_names", Context::Global, QT_TR_NOOP("View"), QT_TR_NOOP("Show Note Names"),
     QKeySequence::UnknownKey, ""},
    // Tools
    {"tools.import_sample", Context::Global, QT_TR_NOOP("Tools"), QT_TR_NOOP("Import Sample"),
     QKeySequence::UnknownKey, ""},
    // Transport
    {"transport.go_to_start", Context::Global, QT_TR_NOOP("Transport"), QT_TR_NOOP("Go to Start"),
     QKeySequence::UnknownKey, "Home"},
    {"transport.play", Context::Global, QT_TR_NOOP("Transport"), QT_TR_NOOP("Play"),
     QKeySequence::UnknownKey, ""},
    {"transport.play_pause", Context::Global, QT_TR_NOOP("Transport"), QT_TR_NOOP("Play/Pause"),
     QKeySequence::UnknownKey, "Space"},
    {"transport.pause", Context::Global, QT_TR_NOOP("Transport"), QT_TR_NOOP("Pause"),
     QKeySequence::UnknownKey, ""},
    {"transport.stop", Context::Global, QT_TR_NOOP("Transport"), QT_TR_NOOP("Stop"),
     QKeySequence::UnknownKey, ""},
    {"transport.loop", Context::Global, QT_TR_NOOP("Transport"), QT_TR_NOOP("Toggle Loop"),
     QKeySequence::UnknownKey, ""},
    {"transport.follow_playhead", Context::Global, QT_TR_NOOP("Transport"),
     QT_TR_NOOP("Follow Playhead"), QKeySequence::UnknownKey, ""},
    // Songs dock
    {"songs.find", Context::Global, QT_TR_NOOP("Songs"), QT_TR_NOOP("Find Song"),
     QKeySequence::Find, ""},
    // Help
    {"help.about", Context::Global, QT_TR_NOOP("Help"), QT_TR_NOOP("About porydaw"),
     QKeySequence::UnknownKey, ""},
    // Piano roll. The old modifier-changes-the-step families (Ctrl+Up, with
    // Shift meaning an octave) are split into explicit commands so each step
    // is independently rebindable.
    {"roll.copy", Context::PianoRoll, QT_TR_NOOP("Piano Roll"), QT_TR_NOOP("Copy Selection"),
     QKeySequence::Copy, ""},
    {"roll.cut", Context::PianoRoll, QT_TR_NOOP("Piano Roll"), QT_TR_NOOP("Cut Selection"),
     QKeySequence::Cut, ""},
    {"roll.paste", Context::PianoRoll, QT_TR_NOOP("Piano Roll"), QT_TR_NOOP("Paste at Edit Cursor"),
     QKeySequence::Paste, ""},
    {"roll.select_all", Context::PianoRoll, QT_TR_NOOP("Piano Roll"),
     QT_TR_NOOP("Select All Notes"), QKeySequence::SelectAll, ""},
    {"roll.delete", Context::PianoRoll, QT_TR_NOOP("Piano Roll"), QT_TR_NOOP("Delete Selection"),
     QKeySequence::UnknownKey, "Delete;Backspace"},
    {"roll.transpose_up", Context::PianoRoll, QT_TR_NOOP("Piano Roll"),
     QT_TR_NOOP("Transpose Up (Semitone)"), QKeySequence::UnknownKey, "Ctrl+Up"},
    {"roll.transpose_down", Context::PianoRoll, QT_TR_NOOP("Piano Roll"),
     QT_TR_NOOP("Transpose Down (Semitone)"), QKeySequence::UnknownKey, "Ctrl+Down"},
    {"roll.transpose_up_octave", Context::PianoRoll, QT_TR_NOOP("Piano Roll"),
     QT_TR_NOOP("Transpose Up (Octave)"), QKeySequence::UnknownKey, "Ctrl+Shift+Up"},
    {"roll.transpose_down_octave", Context::PianoRoll, QT_TR_NOOP("Piano Roll"),
     QT_TR_NOOP("Transpose Down (Octave)"), QKeySequence::UnknownKey, "Ctrl+Shift+Down"},
    {"roll.nudge_left", Context::PianoRoll, QT_TR_NOOP("Piano Roll"), QT_TR_NOOP("Nudge Left"),
     QKeySequence::UnknownKey, "Ctrl+Left"},
    {"roll.nudge_right", Context::PianoRoll, QT_TR_NOOP("Piano Roll"), QT_TR_NOOP("Nudge Right"),
     QKeySequence::UnknownKey, "Ctrl+Right"},
    // Toggle the header buttons from the keyboard, over the whole
    // multi-track scope (the selected track plus Ctrl/Shift-scoped rows).
    {"roll.mute_tracks", Context::PianoRoll, QT_TR_NOOP("Piano Roll"),
     QT_TR_NOOP("Mute Selected Tracks"), QKeySequence::UnknownKey, "M"},
    {"roll.solo_tracks", Context::PianoRoll, QT_TR_NOOP("Piano Roll"),
     QT_TR_NOOP("Solo Selected Tracks"), QKeySequence::UnknownKey, "S"},
    // The velocity lane's pane toggle. A bare letter, so it dispatches
    // through handleEditKey from the focused roll/lanes surface like M/S/B
    // — never as a window shortcut, which would eat the key in text fields.
    {"view.velocity_lane", Context::PianoRoll, QT_TR_NOOP("Piano Roll"),
     QT_TR_NOOP("Toggle Velocity Lane"), QKeySequence::UnknownKey, "V"},
    // The automation lanes' pane toggle, V's sibling. Bare A is free —
    // "select all" is Ctrl+A — and it dispatches the same way, never as a
    // window shortcut.
    {"view.automation_lanes", Context::PianoRoll, QT_TR_NOOP("Piano Roll"),
     QT_TR_NOOP("Toggle Automation Lanes"), QKeySequence::UnknownKey, "A"},
    // Ableton-style pencil for the automation lanes ("B is for Bencil" —
    // reachable without taking the right hand off the mouse). Dispatched
    // through handleEditKey like M/S, so text inputs keep the letter.
    {"automation.pencil_mode", Context::PianoRoll, QT_TR_NOOP("Piano Roll"),
     QT_TR_NOOP("Toggle Automation Pencil Mode"), QKeySequence::UnknownKey, "B"},
    // Ableton-style: hold the modifier and drag vertically anywhere on a
    // note to adjust its velocity. Qt maps Ctrl to Cmd on macOS.
    {"roll.velocity_drag", Context::PianoRoll, QT_TR_NOOP("Piano Roll"),
     QT_TR_NOOP("Adjust Velocity (Hold + Drag Note)"), QKeySequence::UnknownKey, "Ctrl", true},
    // Velocity lane: hold the modifier while starting a gesture to write
    // exact velocities on a PSG track instead of the voice's own detents.
    // Captured at the press, so the values a drag lands on cannot change
    // under it; a Shift ramp adds its Shift to whatever this is bound to.
    {"velocity.detent_unlock", Context::Velocity, QT_TR_NOOP("Velocity Lane"),
     QT_TR_NOOP("Unlock Detents (Hold)"), QKeySequence::UnknownKey, "Ctrl", true},
    // MIDI event list: same-tick reorder nudges (the keyboard face of the
    // row drag).
    {"eventlist.move_up", Context::EventList, QT_TR_NOOP("MIDI Event List"),
     QT_TR_NOOP("Move Event Up (Same Tick)"), QKeySequence::UnknownKey, "Alt+Up"},
    {"eventlist.move_down", Context::EventList, QT_TR_NOOP("MIDI Event List"),
     QT_TR_NOOP("Move Event Down (Same Tick)"), QKeySequence::UnknownKey, "Alt+Down"},
};

const Def *findDef(const QString &id)
{
    for (const Def &def : kDefs) {
        if (id == QLatin1String(def.id))
            return &def;
    }
    return nullptr;
}

QList<QKeySequence> defaultBindings(const Def &def)
{
    if (def.modifier) // modifier chords are not key sequences
        return {};
    if (def.standard != QKeySequence::UnknownKey) {
        // A standard key with no binding on this platform (Preferences on
        // Windows/X11) falls through to the literal fallback.
        const QList<QKeySequence> platform = QKeySequence::keyBindings(def.standard);
        if (!platform.isEmpty())
            return platform;
    }
    QList<QKeySequence> out;
    const QString keys = QLatin1String(def.keys);
    // ';' separates alternates; QKeySequence's own multi-stroke separator is
    // ", " so the two never collide.
    for (const QString &part : keys.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const QKeySequence seq = QKeySequence::fromString(part, QKeySequence::PortableText);
        if (!seq.isEmpty())
            out.append(seq);
    }
    return out;
}

QString settingsKey(const QString &id)
{
    return QStringLiteral("keymap/") + id;
}

// Global shortcuts stay live while any local context has focus.
bool contextsOverlap(Context a, Context b)
{
    return a == b || a == Context::Global || b == Context::Global;
}

Qt::KeyboardModifiers shortcutModifiers(Qt::KeyboardModifiers modifiers)
{
    return modifiers &
           (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);
}

} // namespace

Registry::Registry() = default;

Registry &Registry::instance()
{
    static Registry registry;
    return registry;
}

QList<CommandInfo> Registry::commands() const
{
    QList<CommandInfo> out;
    out.reserve(int(std::size(kDefs)));
    for (const Def &def : kDefs) {
        out.append({QLatin1String(def.id), def.context, tr(def.category), tr(def.name),
                    defaultBindings(def), def.modifier});
    }
    return out;
}

CommandInfo Registry::command(const QString &id) const
{
    const Def *def = findDef(id);
    Q_ASSERT(def);
    if (!def)
        return {};
    return {QLatin1String(def->id), def->context,          tr(def->category),
            tr(def->name),          defaultBindings(*def), def->modifier};
}

QList<QKeySequence> Registry::bindings(const QString &id) const
{
    const Def *def = findDef(id);
    Q_ASSERT(def);
    if (!def || def->modifier)
        return {};
    const QSettings settings;
    const QString key = settingsKey(id);
    if (!settings.contains(key))
        return defaultBindings(*def);
    const QString stored = settings.value(key).toString();
    if (stored.isEmpty()) // explicitly unbound
        return {};
    const QKeySequence seq = QKeySequence::fromString(stored, QKeySequence::PortableText);
    if (seq.isEmpty()) // unparseable hand-edited value: fall back
        return defaultBindings(*def);
    return {seq};
}

bool Registry::isOverridden(const QString &id) const
{
    const QSettings settings;
    return settings.contains(settingsKey(id));
}

void Registry::setBinding(const QString &id, const QKeySequence &sequence)
{
    const Def *def = findDef(id);
    Q_ASSERT(def && !def->modifier);
    if (!def || def->modifier)
        return;
    QSettings settings;
    // Setting a command back to its (sole) default is a reset, keeping the
    // store delta-only.
    const QList<QKeySequence> defaults = defaultBindings(*def);
    if (!sequence.isEmpty() && defaults.size() == 1 && defaults[0] == sequence)
        settings.remove(settingsKey(id));
    else
        settings.setValue(settingsKey(id), sequence.toString(QKeySequence::PortableText));
    applyToActions();
    emit bindingsChanged();
}

Qt::KeyboardModifiers Registry::modifierBinding(const QString &id) const
{
    const Def *def = findDef(id);
    Q_ASSERT(def && def->modifier);
    if (!def || !def->modifier)
        return Qt::NoModifier;
    const QSettings settings;
    const QString key = settingsKey(id);
    if (!settings.contains(key))
        return modifierFromText(QLatin1String(def->keys));
    const QString stored = settings.value(key).toString();
    if (stored.isEmpty()) // explicitly unbound: the gesture is off
        return Qt::NoModifier;
    const Qt::KeyboardModifiers mods = modifierFromText(stored);
    if (mods == Qt::NoModifier) // unparseable hand-edited value: fall back
        return modifierFromText(QLatin1String(def->keys));
    return mods;
}

bool Registry::matchesModifier(Qt::KeyboardModifiers mods, const QString &id) const
{
    const auto binding = modifierBinding(id);
    return binding != Qt::NoModifier && shortcutModifiers(mods) == binding;
}

bool Registry::isModifierKey(int key)
{
    return key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt ||
           key == Qt::Key_Meta;
}

void Registry::setModifierBinding(const QString &id, Qt::KeyboardModifiers mods)
{
    const Def *def = findDef(id);
    Q_ASSERT(def && def->modifier);
    if (!def || !def->modifier)
        return;
    QSettings settings;
    // Same delta-only store as setBinding: the default is a reset, and
    // NoModifier persists as the empty "explicitly unbound" marker.
    if (mods != Qt::NoModifier && mods == modifierFromText(QLatin1String(def->keys)))
        settings.remove(settingsKey(id));
    else
        settings.setValue(settingsKey(id), mods == Qt::NoModifier ? QString() : modifierText(mods));
    emit bindingsChanged();
}

QString Registry::modifierText(Qt::KeyboardModifiers mods)
{
    // Portable text on purpose (it is also the storage form); Qt's own
    // portable sequence text spells modifiers the same way.
    QStringList parts;
    if (mods & Qt::ControlModifier)
        parts.append(QStringLiteral("Ctrl"));
    if (mods & Qt::ShiftModifier)
        parts.append(QStringLiteral("Shift"));
    if (mods & Qt::AltModifier)
        parts.append(QStringLiteral("Alt"));
    if (mods & Qt::MetaModifier)
        parts.append(QStringLiteral("Meta"));
    return parts.join(QLatin1Char('+'));
}

Qt::KeyboardModifiers Registry::modifierFromText(const QString &text)
{
    Qt::KeyboardModifiers mods = Qt::NoModifier;
    const QStringList parts = text.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const QString token = part.trimmed();
        if (token.compare(QLatin1String("Ctrl"), Qt::CaseInsensitive) == 0)
            mods |= Qt::ControlModifier;
        else if (token.compare(QLatin1String("Shift"), Qt::CaseInsensitive) == 0)
            mods |= Qt::ShiftModifier;
        else if (token.compare(QLatin1String("Alt"), Qt::CaseInsensitive) == 0)
            mods |= Qt::AltModifier;
        else if (token.compare(QLatin1String("Meta"), Qt::CaseInsensitive) == 0)
            mods |= Qt::MetaModifier;
        else
            return Qt::NoModifier;
    }
    return mods;
}

void Registry::resetBinding(const QString &id)
{
    QSettings settings;
    settings.remove(settingsKey(id));
    applyToActions();
    emit bindingsChanged();
}

void Registry::resetAll()
{
    QSettings settings;
    settings.remove(QStringLiteral("keymap"));
    applyToActions();
    emit bindingsChanged();
}

QStringList Registry::conflicts(const QString &excludeId, Context context,
                                const QKeySequence &sequence) const
{
    QStringList out;
    if (sequence.isEmpty())
        return out;
    for (const Def &def : kDefs) {
        const QString id = QLatin1String(def.id);
        if (id == excludeId || !contextsOverlap(context, def.context))
            continue;
        if (bindings(id).contains(sequence))
            out.append(id);
    }
    return out;
}

QStringList Registry::modifierConflicts(const QString &excludeId, Context context,
                                        Qt::KeyboardModifiers mods) const
{
    QStringList out;
    if (mods == Qt::NoModifier)
        return out;
    for (const Def &def : kDefs) {
        const QString id = QLatin1String(def.id);
        if (!def.modifier || id == excludeId || !contextsOverlap(context, def.context))
            continue;
        if (modifierBinding(id) == mods)
            out.append(id);
    }
    return out;
}

bool Registry::matches(const QKeyEvent *event, const QString &id) const
{
    const int key = event->key();
    if (key == 0 || key == Qt::Key_unknown || isModifierKey(key))
        return false;
    // Keypad arrows arrive with KeypadModifier set; bindings never carry it.
    const auto mods = shortcutModifiers(event->modifiers());
    const int combined = key | int(mods.toInt());
    for (const QKeySequence &seq : bindings(id)) {
        if (seq.count() == 1 && seq[0].toCombined() == combined)
            return true;
    }
    return false;
}

void Registry::attach(const QString &id, QAction *action)
{
    Q_ASSERT(findDef(id) && !findDef(id)->modifier);
    m_actions.append({id, QPointer<QAction>(action)});
    action->setShortcuts(bindings(id));
}

void Registry::applyToActions()
{
    m_actions.removeIf([](const Attached &a) { return a.action.isNull(); });
    for (const Attached &a : m_actions)
        a.action->setShortcuts(bindings(a.id));
}

} // namespace keymap
