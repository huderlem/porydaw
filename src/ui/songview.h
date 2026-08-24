#pragma once

#include <QColor>
#include <QHash>
#include <QList>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QWidget>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "core/miditimeline.h"
#include "core/velocitymodel.h"
#include "ui/songviewmodel.h"
#include "ui/timelinesurface.h"

extern "C" {
#include "voicegroup_loader.h"
}

class EventListView;
class QKeyEvent;
class QScrollArea;
class QScrollBar;
class QSplitter;
class QStackedWidget;
class QWheelEvent;
class SongDocument;

namespace songview {
class TimeRuler;
class PianoRoll;
class AutomationArea;
class VelocityLane;
class LaneToggleBar;
class OtherStrip;
class PlayheadOverlay;
class TrackHeaderPanel;

// Fixed gutter geometry shared by every timeline-aligned child: the track
// header column plus the piano-roll keyboard column. All children put
// timeline tick 0 at the same global x. Exposed for roll interaction checks.
constexpr int kHeaderW = 210;
constexpr int kKeyboardW = 52;
constexpr int kGutterW = kHeaderW + kKeyboardW;
// Velocity bar handle: with at least this much vertical zoom, or while the
// roll.velocity_drag modifier chord (Ctrl by default, Ableton-style) is
// held, each note shows a thin horizontal bar at its velocity level (bottom
// = 0, top = 127). The bar — grabbable anywhere across the note's width —
// drags the velocity at the threshold zoom; below it, the held shortcut
// starts the same drag from anywhere on the note. The right-click menu
// remains the velocity fallback, and the default key height makes the
// handle available out of the box.
constexpr int kVelHandleMinKeyH = 12;
// Note-name labels: with the View toggle on, each active-track note carries
// its pitch name unless the velocity shortcut is held. The label face is
// fixed, and it hides — never shrinks — whenever its padded height misses
// the row; this floor is only a cheap pre-gate that no padded face ever
// fits under.
constexpr int kNoteNameMinKeyH = 12;
// Auto snap grid: the zoom-adaptive grid shows the finest subdivision from
// the feel's ladder whose cells are at least this wide, so lower values
// mean a busier grid at the same zoom. Exposed so viewcheck derives its
// expectations from the same knob being tuned.
constexpr double kAutoGridMinCellPx = 16.0;
// The velocity bar's rect inside a note rect; painted by the roll at the
// threshold zoom or while the velocity shortcut is held. From
// kVelHandleMinKeyH up, it is also the grab target for velocity drags.
// Exposed for roll interaction checks. The default DPR keeps integer-DIP
// callers compatible while the roll supplies its actual display scale.
inline QRectF velBarRect(const QRectF &noteRect, int velocity, qreal dpr = 1.0)
{
    const qreal pixel = 1.0 / dpr;
    const qreal barH = qRound(noteRect.height() / pixel) >= 20 ? 2 * pixel : pixel;
    const qreal innerH = noteRect.height() - 2 * pixel;
    const qreal y = std::min(noteRect.top() + pixel + (127 - velocity) * (innerH - pixel) / 127.0,
                             noteRect.bottom() - pixel - barH);
    return QRectF(noteRect.left() + pixel, y, std::max(pixel, noteRect.width() - 2 * pixel), barH);
}
// Frame weights for note borders and the selection ring, in physical
// pixels for the given display ratio. Authored in DIPs (border 1, ring
// 1.5) so a display at 100% scale shows the same visual weight a HiDPI
// display does; painting still lands on whole physical pixels so
// fractional scale factors cannot open seams. Exposed so roll checks
// assert the same math the paint code uses.
inline int noteBorderPixels(qreal dpr)
{
    return std::max(1, qRound(dpr));
}
inline int selectionRingPixels(qreal dpr)
{
    return std::max(1, qRound(1.5 * dpr));
}
} // namespace songview

// Song view: time ruler, multi-track piano roll (selected track in full
// color, others ghosted), per-track automation lanes with m4a names, an
// "other events" strip, and track headers with instrument names from the
// loaded voicegroup. Read-only over a MidiTimeline (M1); when a SongDocument
// is attached (M2) the selected track is editable: note draw/move/resize/
// velocity/delete in the roll, point editing in the lanes, loop-marker
// dragging in the ruler. The MidiTimeline and LoadedVoiceGroup must outlive
// the view or be cleared with setSong(nullptr, nullptr) first.
class SongView : public QWidget
{
    Q_OBJECT

  public:
    explicit SongView(QWidget *parent = nullptr);

    void setSong(const MidiTimeline *timeline, const LoadedVoiceGroup *voicegroup);
    // Timeline swap after a document edit: keeps zoom, scroll, track
    // selection, mute/solo, and re-resolves the note selection.
    void updateSong(const MidiTimeline *timeline);
    void setPlayheadSample(uint64_t samplePos, bool playing);

    // Editing is enabled while a document is attached (may be null).
    void setDocument(SongDocument *document);
    SongDocument *document() const { return m_document; }

    // Voicegroup swap after a -G settings change (labels only; may be null
    // while the audio engine frees the old one).
    void setVoicegroup(const LoadedVoiceGroup *voicegroup);

    // Per-song sidecar view state (SPEC §4.4): the cosmetic state worth
    // restoring when the song is reopened. Everything is clamped/validated
    // on apply, so a stale or hand-edited sidecar can't wedge the view.
    struct ViewState {
        bool valid = false;
        double pxPerBeat = 32.0;                        // horizontal zoom (ticks-per-beat neutral)
        double keyHeight = songview::kVelHandleMinKeyH; // vertical roll zoom
        double scrollPx = 0.0;
        double scrollY = 0.0;
        int selectedTrack = 0;
        uint64_t editCursorTick = 0;
        int laneHeight = 48;                             // shared automation row height
        QHash<QString, int> laneHeights;                 // per-row overrides (AutomationArea keys)
        QHash<QString, int> laneRanges;                  // per-lane display max (AutomationArea
                                                         // keys); 0 = auto-fit to the data
        QSet<QString> hiddenLanes;                       // hidden CC lanes (AutomationArea keys)
        bool tempoLane = false;                          // the global tempo row is shown
        QList<int> splitterSizes;                        // roll pane, lanes pane
        std::vector<std::pair<int, uint8_t>> emptyLanes; // (track, cc)
        int gridMinDenom = 0;                            // drawn-grid floor as a note denominator
                                                         // (4/8/16/32); 0 = down to the clock grid
        bool gridTriplet = false;                        // triplet vs straight beat subdivisions
        bool eventList = false;                          // raw MIDI event list instead of the roll
    };
    ViewState viewState() const;
    // Call after setSong (and setDocument); a default-constructed (invalid)
    // state is a no-op.
    void applyViewState(const ViewState &state);

    // User-added automation lanes with no events yet (SPEC §6.1 "addable from
    // the m4a parameter list"). They live in view state — the model derives
    // lanes from events — and survive document rebuilds until the song is
    // swapped; once the lane gets its first point the model carries it.
    void addEmptyLane(int track, uint8_t cc);
    void removeEmptyLane(int track, uint8_t cc);

    // Display max for a CC lane's value axis (0 = auto-fit): the lane
    // menu's "Value range" choice, exposed for the harnesses. View state
    // only — lane values themselves are untouched.
    void setLaneDisplayRange(int track, uint8_t cc, int maxValue);

    // Velocity lane: a hidden-by-default pane between the roll and the
    // automation lanes showing the selected track's note velocities. App-wide
    // preference (View menu / view.velocity_lane, default V), like Follow
    // Playhead; the pane's height is per-song view state (the splitter's).
    bool velocityLaneVisible() const;
    void setVelocityLaneVisible(bool visible);

    // The automation lanes' pane. App-wide preference like the velocity
    // lane's (View menu / view.automation_lanes, default A), except it
    // starts shown — the lanes have always been part of the editor. Hiding
    // is view-only: the rows, their heights, and their points are untouched,
    // and per-lane "Hide lane" stays a separate, per-song thing.
    bool automationLanesVisible() const;
    void setAutomationLanesVisible(bool visible);

    // The global Tempo row inside the automation lanes. Hidden by default:
    // a song's tempo is usually one number, which the transport bar's Tempo
    // spinner already shows and edits, so the row only earns its space when
    // the tempo moves mid-song. Per-song view state (sidecar), like the
    // per-lane "Hide lane"; showing it also opens the automation pane so
    // the row actually appears.
    bool tempoLaneVisible() const;
    void setTempoLaneVisible(bool visible);

    // Raw MIDI event list: an alternative to the piano roll in the same
    // screen space (the ruler, headers, and automation lanes stay). Per-song
    // view state; toggled from the View menu.
    bool eventListVisible() const;
    void setEventListVisible(bool visible);

    // --- shared state for the child widgets ---
    const MidiTimeline *timeline() const { return m_timeline; }
    const SongViewModel &model() const { return m_model; }
    const LoadedVoiceGroup *voicegroup() const { return m_voicegroup; }
    songview::TimelineSurfaces timelineSurfaces() noexcept;

    qreal contentX(double tick) const { return qreal(tick * m_pxPerTick - m_scrollX); }
    double tickAtContentX(qreal x) const { return (double(x) + m_scrollX) / m_pxPerTick; }
    // Camera dead space before tick 0: the horizontal scroll floor is
    // -leadPadPx(), so the song start can rest inside the viewport instead
    // of pinned to its left edge (zooming near the start clamps here, which
    // keeps tick 0 on screen).
    double leadPadPx() const;
    qreal displayX(double tick, qreal origin, qreal dpr) const;
    double pxPerTick() const { return m_pxPerTick; }
    double pxPerBeat() const;
    double scrollY() const { return m_scrollY; }
    double keyHeight() const { return m_keyHeight; }
    double playheadTick() const { return m_playheadTick; }

    // Edit cursor (Reaper-style): placed by clicking the ruler or empty
    // roll space (with a document, dragging or double-clicking there draws
    // a note instead), distinct from the moving playback cursor. Playback
    // starts here, and paste anchors here.
    uint64_t editCursorTick() const { return m_editCursorTick; }
    // The tick the view is "at" for readouts that follow the song rather
    // than a note: the playhead while playing, the edit cursor otherwise.
    uint64_t displayTick() const;
    // Visual placement only (ruler drag preview); commit emits
    // editCursorMoved so playback can follow.
    void setEditCursorTick(uint64_t tick);
    void commitEditCursor(uint64_t tick);
    // Transport "go to start": edit cursor to tick 0 and scroll home.
    void goToStart();

    int selectedTrack() const { return m_selectedTrack; }
    void selectTrack(int track);
    // Reveal a polyphony-overflow event's note: select its track, select the
    // last note on (track, key) starting at or before tick — the lost note (a
    // dropped note starts exactly there, a stolen one spans it, a cut tail
    // ended just before) — and scroll the key into view. Returns whether a
    // note was found and selected (the track selection sticks either way).
    bool revealNote(int track, uint8_t key, uint64_t tick);
    // Multi-track scope for time-range operations: the selected track plus
    // any Ctrl/Shift-clicked header rows (always contains the selected
    // track, intersected with used tracks).
    uint32_t trackSelectionMask() const;
    // Header-row click with modifiers: plain = select (collapses the multi-
    // selection), Ctrl = toggle the track in the scope, Shift = contiguous
    // range from the selected track.
    void trackHeaderClicked(int track, Qt::KeyboardModifiers modifiers);
    bool trackMuted(int track) const { return m_muteMask & (1u << track); }
    bool trackSoloed(int track) const { return m_soloMask & (1u << track); }
    // Full masks, for re-applying to the audio engine on a tab switch.
    uint32_t muteMask() const { return m_muteMask; }
    uint32_t soloMask() const { return m_soloMask; }
    void setTrackMute(int track, bool on);
    void setTrackSolo(int track, bool on);
    // Keyboard face of the header buttons, over the multi-track scope:
    // mixed state resolves toward on (mute/solo everything in the scope),
    // a second press turns it back off.
    void toggleMuteOnSelectedTracks();
    void toggleSoloOnSelectedTracks();

    static QColor trackColor(int track);
    static QColor noteColor(int track, int velocity);
    // Velocity-hue display mode (View menu, app-wide): the active track's
    // note fills take their hue from velocity — purple (1) sweeping the long
    // way around the wheel to red (127) — instead of the track identity.
    // Ghost notes and every other identity-colored surface are unchanged.
    static QColor velocityNoteColor(int velocity);
    bool velocityColorMode() const { return m_velocityColorMode; }
    void setVelocityColorMode(bool on);
    // Note-name display mode (View menu, app-wide): from kNoteNameMinKeyH of
    // vertical zoom up, each visible active-track note independently carries
    // its pitch name when its face fits the complete name plus two trailing
    // spaces. Ghost notes are never labeled.
    bool noteNameMode() const { return m_noteNameMode; }
    void setNoteNameMode(bool on);
    // App-wide Follow Playhead toggle (transport bar / View menu): off, the
    // playback follow-scroll — the roll's and the event list's — is
    // suppressed and the camera stays where the user put it.
    void setFollowPlayhead(bool on);
    // The active-track note fill under the current display mode.
    QColor noteFillColor(int track, int velocity) const;
    // The velocity a velocity-lane gesture is currently holding a note at,
    // if one is: the roll draws its notes at the value the lane's release
    // will write, so a lane edit recolors (and relabels) the roll live.
    std::optional<uint8_t> velocityLanePreview(const ViewNote &note) const;
    // A lane preview moved (or ended): the roll reads those previews too.
    void velocityPreviewChanged();
    // The mirror of velocityLanePreview: the velocity a roll velocity drag
    // is currently holding a note at, so the lane's node rides the drag
    // instead of sitting at the stored value until the release commits.
    std::optional<uint8_t> rollVelocityPreview(const ViewNote &note) const;
    // A roll velocity drag moved (or ended): the lane reads those previews.
    void rollVelocityPreviewChanged();
    // The track's program at a tick: the last voice change at or before
    // it. Before the first change it stays firstProgram (which is what
    // primes the engine), -1 if the track has none. The one statement of
    // "voice in effect", shared by the header label and the lanes' voice
    // hover so they can never disagree on screen.
    int programAtTick(int track, uint64_t tick) const;
    // programAtTick at the display position — the playhead while playing,
    // the edit cursor otherwise — so the header label follows the song's
    // voice changes.
    int currentProgram(int track) const;
    // The selected track's voice at a tick, resolved through the loaded
    // voicegroup, with the track volume and pan in force there and the tick
    // where any of them next changes (UINT64_MAX past the last change). The
    // velocity lane needs all of it: on a CGB channel the voice, the volume
    // and the pan together decide what loudness levels exist, and endTick says
    // how far across the plot that one set of level lines reaches.
    struct VoiceContext {
        const ToneData *voice = nullptr;
        int program = -1;
        uint64_t endTick = UINT64_MAX;
        // The compiled VOL byte: the track's own volume with the song's
        // master volume already folded in (m4aEffectiveTrackVolume).
        uint8_t trackVolume = uint8_t(kM4aMaxVolume);
        // The PAN in engine units (-64..63): the CC10 byte less 64.
        int8_t trackPan = 0;
    };
    VoiceContext voiceContext(uint64_t tick) const;
    // The compiled VOL byte in force on a track at a tick: its CC7 automation
    // (127 before the first point, as mid2agb primes it) with the song's
    // master volume folded in. When nextChangeTick is given it is lowered to
    // the next VOL point past the tick, if that comes first.
    uint8_t trackVolumeAt(int track, uint64_t tick, uint64_t *nextChangeTick = nullptr) const;
    // The same lookup without the master volume folded in: the raw VOL byte
    // the track's CC7 automation puts in force at the tick. Auditions want
    // this one — the engine applies the master volume itself.
    int trackRawVolumeAt(int track, uint64_t tick, uint64_t *nextChangeTick = nullptr) const;
    // The PAN in force on a track at a tick, in engine units (-64..63): its
    // CC10 automation less 64, centered before the first point as mid2agb
    // primes it. When nextChangeTick is given it is lowered to the next PAN
    // point past the tick, if that comes first.
    int8_t trackPanAt(int track, uint64_t tick, uint64_t *nextChangeTick = nullptr) const;
    // The VOL byte an audition aimed at atTick should sound at, or -1 for
    // "whatever the engine's track holds" (kAuditionAtCursor, or no song).
    int auditionVolume(int track, uint64_t atTick) const
    {
        if (atTick == kAuditionAtCursor || track < 0 || track >= 16)
            return -1;
        return trackRawVolumeAt(track, atTick);
    }
    // The PAN an audition aimed at atTick should sound at, or the engine's
    // no-override sentinel for "whatever the engine's track holds".
    int auditionPan(int track, uint64_t atTick) const
    {
        if (atTick == kAuditionAtCursor || track < 0 || track >= 16)
            return M4A_AUDITION_PAN_NONE;
        return trackPanAt(track, atTick);
    }
    QString instrumentLabel(int track) const; // "042 name (type)" from the voicegroup
    QString voiceShortName(uint8_t program) const;
    QString voiceLabel(uint8_t program) const; // "042 name", the marker/header format

    // Jump-from-context: surface the program in the voicegroup dock (the
    // main window raises it and selects the slot via revealVoiceRequested).
    // revealTrackVoice resolves the track's program at the display position
    // (what currentProgram shows in the header) first. Entry points: the
    // header row's voice line and context menu, and the event list's
    // program-change rows.
    void revealVoice(int program);
    void revealTrackVoice(int track);
    // Every program the song references: each track's first program plus
    // all voice changes. Feeds the dock's used-row highlighting.
    QSet<int> usedVoices() const;

    // Modal voicegroup-entry picker with press-and-hold audition. Returns
    // false on cancel; otherwise *outVoice is the chosen entry (0-127).
    bool pickVoice(const QString &title, int initialVoice, int *outVoice);
    // Track-header entry point: re-pick the voice governing the track (its
    // first program change), inserting one at tick 0 if the track has none.
    void editTrackVoice(int track);

    // Track create/duplicate/delete (header-panel entry points; all undoable
    // through the document). addTrack picks the new track's voice first, then
    // selects the created track; duplicateTrack selects the copy (a fresh
    // slot, so no per-track view state moves); deleteTrack shifts the view's
    // per-track state (mute/solo, empty lanes, selection) over the removed
    // engine slot; moveTrack (header-row drag; the track's chunk moves —
    // AGB track order is chunk order) rotates that state along with the
    // reordered engine slots — in onTrackMoved, off the document's
    // trackMoved signal, so undo/redo rotate it back too.
    void addTrack();
    void duplicateTrack(int track);
    void deleteTrack(int track);
    void moveTrack(int from, int to);
    // Inline rename: opens a line editor on the track's header row
    // (double-click and the context menu land here). commitTrackRename
    // applies the typed name — queued, since the edit rebuilds the header
    // panel out from under the editor's own signal — and refuses names
    // mid2agb would read as loop/label markers, with a status message.
    void renameTrack(int track);
    void commitTrackRename(int track, const QString &name);
    // Focus the current editing surface (roll or event list), e.g. after an
    // inline editor closes.
    void focusContent();

    // Bar/beat grid over [tickBegin, tickEnd): calls fn(tick, isBarStart,
    // barNumber, beatNumber) for every beat, honoring the song's time
    // signature changes.
    void forEachGridLine(uint64_t tickBegin, uint64_t tickEnd,
                         const std::function<void(uint64_t, bool, int, int)> &fn) const;

    // --- editing support for the child widgets ---
    // Grid feel and floor (the ruler's grid controls): the zoom-adaptive
    // grid subdivides beats by powers of two (straight) or by threes
    // (triplet), and the minimum subdivision — a note denominator, quarter =
    // one beat — stops the DRAWN grid from refining past the note value the
    // user cares about (display only; snapping still steps one rung finer).
    // 0 keeps the default clock-grid floor. Per-song view state.
    enum class GridFeel { Straight, Triplet };
    GridFeel gridFeel() const { return m_gridFeel; }
    void setGridFeel(GridFeel feel);
    int gridMinDenom() const { return m_gridMinDenom; }
    void setGridMinDenom(int denom); // 4/8/16/32; anything else means 0

    // Time-signature segment governing a tick. The grid — beats, snap
    // positions, sub-beat lines — restarts at every signature change and
    // scales the beat by the signature's denominator, exactly like
    // forEachGridLine; a signature placed mid-measure must still leave the
    // drawn lines snappable.
    struct GridSeg {
        uint64_t start = 0;         // governing signature's tick (0 = song start)
        uint64_t next = UINT64_MAX; // next signature's tick; the grid restarts there
        uint64_t beatTicks = 24;    // denominator-scaled beat length in ticks
    };
    GridSeg gridSegAt(uint64_t tick) const;

    // Visible grid in ticks at a position: the drawn subdivision of the
    // governing segment's beat at the current feel, floored at the minimum
    // subdivision (1/4 = one beat of that signature) and never finer than
    // the song's mid2agb clock base.
    uint64_t gridTicksAt(uint64_t tick) const;
    // Snap grid in ticks at a position: one feel-ladder step finer than the
    // visible grid, so edits can land halfway between drawn lines (thirds
    // stepping from beats in triplet feel). The minimum subdivision is a
    // display floor only — snapping steps past it — but the clock base
    // still bounds it, and it always divides the visible grid.
    uint64_t snapTicksAt(uint64_t tick) const;
    // Fine placement (Alt-drag in the lanes): the mid2agb clock grid — the
    // document's real resolution — regardless of the zoom-dependent grid.
    uint64_t fineGridTicks() const;
    // Nearest / previous snap-grid position, anchored at the governing
    // time-signature segment (fine snap stays on the absolute clock grid).
    uint64_t snapTick(double tick, bool fine = false) const;
    uint64_t snapTickDown(double tick) const;
    uint64_t snapTickUp(double tick) const;

    // Note selection on the selected track, identified by (startTick, key) so
    // it survives document rebuilds. Distinct from the document's ::NoteId
    // (core/noteid.h) — named apart so an unqualified mention can never
    // silently bind to the wrong identity type.
    struct NoteKey {
        uint32_t tick;
        uint8_t key;
        bool operator==(const NoteKey &other) const
        {
            return tick == other.tick && key == other.key;
        }
    };
    const std::vector<NoteKey> &selection() const { return m_selection; }
    bool isSelected(const ViewNote &note) const;
    void setSelection(std::vector<NoteKey> ids);
    void clearSelection();

    // Time-range selection: a half-open [startTick, endTick) span with a
    // scope — the header-selected tracks (ruler sweep and Shift+right-drag
    // in the roll behave identically; the scope resolves LIVE from
    // trackSelectionMask(), so Ctrl/Shift-clicking headers re-scopes an
    // active selection) or individual automation lanes (right-drag in the
    // lanes area). Mutually exclusive with the note selection; survives
    // document rebuilds (it is tick-addressed), cleared on song swap and
    // plain track switches.
    struct TimeSelection {
        enum Scope { Tracks, Lanes };
        uint64_t startTick = 0;
        uint64_t endTick = 0; // <= startTick means no selection
        Scope scope = Tracks;
        std::vector<std::pair<int, uint8_t>> lanes; // Scope::Lanes: (track, cc);
                                                    // track -1 = the tempo row
        bool active() const { return endTick > startTick; }
    };
    const TimeSelection &timeSelection() const { return m_timeSel; }
    void setTimeSelection(const TimeSelection &sel);
    void clearTimeSelection();
    bool timeSelectionCoversTrack(int track) const;
    // Whether a lanes-area row (identified as the lane scope encodes it) is
    // inside the selection; track scopes cover a track's CC/voice rows but
    // never the global tempo row.
    bool timeSelectionCoversRow(int track, uint8_t cc) const;
    // "Time selection: 8 beats · 3 tracks" status-bar line; children call it
    // when a selection gesture commits.
    void announceTimeSelection();

    // Range operations on the time selection. Copy captures notes plus every
    // editable lane (including voice changes) of the scoped tracks — or just
    // the scoped lanes — with ticks relative to the range start. Paste
    // anchors at the edit cursor and REPLACES the covered span: pasted
    // "silence" clears, and a single-source-track clip retargets to the
    // selected track. All one undoable command each.
    void copyTimeSelection();
    void deleteTimeSelection();
    // "Remove contents": ripple delete — the selected span vanishes and
    // everything after it shifts left to close the gap. Selecting every
    // track cuts the whole song (tempo, time signatures, loop markers and
    // track ends ripple too); a partial scope shifts only its own tracks or
    // lanes so the rest of the song keeps its alignment.
    void removeTimeSelectionContents();
    void pasteRangeAtEditCursor();
    // Ctrl+Up/Down on the selection: transpose every covered note (all
    // scoped tracks at once). Same all-or-nothing rule as the roll's note
    // selection — if any note would clamp at the key range, nothing moves.
    void transposeTimeSelection(int dKey);
    // Ctrl+Left/Right: the selection start moves to the previous/next
    // ruler grid line and the covered contents (notes and automation
    // points) move with it; the band follows.
    void nudgeTimeSelection(bool right);
    // Shared shortcut handling for the roll and the lanes area: range
    // copy/cut/delete while a time selection is active, paste of range
    // clips, and transpose/nudge of the selection (keymap commands).
    // Returns true when consumed.
    bool handleEditKey(QKeyEvent *event);
    // Automation-lane pencil mode (automation.pencil_mode, default B): a
    // left drag in the lanes always draws, and Shift locks the stroke to a
    // horizontal line. The key is Ableton-style momentary: releases route
    // through handleEditKeyRelease, which reverts a hold (or a hold that
    // drew — the lanes report that via markPencilKeyGesture) and keeps a
    // quick tap as a sticky toggle.
    bool automationPencilMode() const;
    void setAutomationPencilMode(bool on);
    bool handleEditKeyRelease(QKeyEvent *event);
    void markPencilKeyGesture();
    // Semitone step for the transpose command the event matches (0 if none);
    // shared by the note- and time-selection key paths.
    int transposeStepFor(const QKeyEvent *event) const;
    // Copy/Cut/Delete/Paste/Clear context menu on the active selection.
    void showTimeSelectionMenu(const QPoint &globalPos);

    // App-shared clipboard: one clipboard for every tab/SongView, so copies
    // travel between songs (paste into another tab, or into a song opened
    // over this one). A plain note copy (roll selection) has span 0 and
    // pastes additively; a range copy carries span > 0 plus lane segments
    // and pastes with replace semantics. Ticks are offsets from the copied
    // block's start so paste can re-anchor at the edit cursor, and are in
    // the SOURCE song's resolution: copies stamp ticksPerBeat (see
    // setClipboard) and pastes rescale through clipForPaste, so the music
    // keeps its length across songs with different MIDI divisions. Entirely
    // self-contained value data — no pointers into a document, timeline, or
    // voicegroup — so it safely outlives the song it came from. Voice-change
    // values are indices into the source song's voicegroup, which another
    // song's may lay out differently; copies stamp the referenced voices'
    // names (voiceNames) so a paste can say when they'd sound as different
    // instruments (see foreignVoiceCount).
    struct ClipNote {
        uint32_t relTick;
        uint8_t key;
        uint32_t duration;
        uint8_t velocity;
    };
    struct ClipTrack {
        int track; // source engine track
        std::vector<ClipNote> notes;
    };
    struct ClipLane {
        int track; // source engine track; -1 = tempo
        uint8_t cc;
        std::vector<std::pair<uint32_t, int>> points; // (relTick, value)
    };
    struct Clip {
        uint64_t span = 0;      // ticks covered; 0 = plain note clip
        bool wholeLane = false; // gutter "Copy lane" (paste-lane anchor is 0)
        // Source song's resolution at copy time.
        uint32_t ticksPerBeat = MidiTimeline::kDefaultTicksPerBeat;
        // Source voicegroup's name for each voice index the lanes' voice
        // changes reference (empty for square/noise voices, which have no
        // symbol). Only indices actually referenced are present.
        std::map<int, QString> voiceNames;
        std::vector<ClipTrack> tracks;
        std::vector<ClipLane> lanes;
        bool empty() const { return tracks.empty() && lanes.empty(); }
    };
    // Read-only: the clipboard only changes through setClipboard, which
    // stamps the clip, so pastes can trust the stamps.
    static const Clip &clipboard();
    // The one way onto the clipboard for copies: stamps this song's
    // ticksPerBeat and the referenced voices' names into the clip so a paste
    // into another song knows how to rescale and what the voices meant.
    void setClipboard(Clip clip);
    // Stores a clip as given, stamps included — for the harnesses' foreign-
    // resolution fixtures. Copies go through setClipboard.
    static void storeClipboard(Clip clip);
    // The clipboard's contents rescaled to this song's resolution (identity
    // when they match — the common case). Downscaling rounds and can land
    // two events on one tick; the later one wins, matching the editor's
    // same-tick convention, so paste sites never see colliding events.
    Clip clipForPaste() const;
    // How many of the clip's voice changes would sound as a different
    // instrument here: their stamped source voice name differs from this
    // song's voicegroup's name at the same index. 0 when the clip carries no
    // voice changes. Exposed for the harnesses.
    int foreignVoiceCount(const Clip &clip) const;

    // "velocity 93 → plays 96 · length 25 → 24 clocks" for the status bar.
    void announceNote(const ViewNote &note);

    // Child-widget entry point for the auditionNote signal. atTick is the
    // tick whose track VOL and PAN the note should sound at — the start of
    // the note being auditioned, not wherever the edit cursor happens to sit,
    // so a note under a volume ramp previews at its own loudness and one in a
    // panned passage previews where it really sits. kAuditionAtCursor (the
    // default) leaves the engine on the state chased to the playhead, which
    // is what a bare keyboard-column click wants.
    static constexpr uint64_t kAuditionAtCursor = UINT64_MAX;
    void audition(int track, int key, int velocity, uint64_t atTick = kAuditionAtCursor)
    {
        emit auditionNote(track, key, velocity, auditionVolume(track, atTick),
                          auditionPan(track, atTick));
    }

    // Fixed-length audition for the band-sweep chord preview: the note's tick
    // span converts to samples through the display timeline, so the preview
    // lasts at most as long as the note does in the song (tempo changes
    // included).
    void auditionTimed(int track, int key, int velocity, uint64_t startTick, uint64_t endTick);

    // Early release for a timed audition (the band no longer covers the
    // note); the velocity-0 form of the same signal.
    void auditionTimedOff(int track, int key)
    {
        emit auditionNoteTimed(track, key, 0, 0, -1, M4A_AUDITION_PAN_NONE);
    }

    // Child-widget entry point for the statusMessage signal.
    void announce(const QString &text) { emit statusMessage(text); }

    // Interaction from children.
    void zoomAroundContentX(double factor, qreal anchorContentX);
    // Vertical roll zoom (key height) from Ctrl+wheel, pinning the key under
    // the cursor. The wheel event supplies continuous deltas.
    void zoomKeyHeight(const QWheelEvent *event);
    void scrollByPx(double dx);
    void scrollRollBy(double dy);
    // The configurable wheel actions (Settings → Keyboard Shortcuts → Mouse
    // Wheel), applied for one child surface. headerW is the surface's
    // header column (no timeline under the cursor there); pan names the
    // vertical camera the surface owns; zoomVertical is its own vertical
    // zoom, taking the wheel delta, or empty to fall back to the timeline
    // zoom. Returns false when the event is not the surface's to consume.
    enum class WheelPan { Roll, Lanes, None };
    bool applyWheel(QWheelEvent *event, int headerW, WheelPan pan,
                    const std::function<void(int delta)> &zoomVertical = {});
    // Scrolls horizontally so the tick sits a third of the way into the
    // viewport if it is currently off-screen; on-screen ticks are left
    // alone. Pastes anchor at the edit cursor, which can be scrolled out
    // of view — without this the paste looks like a no-op.
    void ensureTickVisible(uint64_t tick);
    // Minimal-scroll companion for the keyboard transpose/nudge moves:
    // shifts the view just enough to bring the tick span back inside,
    // instead of ensureTickVisible's jump-to-a-third anchoring. A span
    // wider than the viewport keeps the edge the move headed toward
    // (the end when preferEnd, else the start).
    void ensureRangeVisible(uint64_t startTick, uint64_t endTick, bool preferEnd);
    // Vertical counterpart: scrolls the roll just enough for the key's
    // row to be fully visible.
    void ensureKeyVisible(int key);
    void refreshTimelineViews();

  signals:
    void muteMaskChanged(uint32_t mask);
    void soloMaskChanged(uint32_t mask);
    void selectedTrackChanged(int track);
    // Audition request (velocity 0 releases); forwarded to the audio engine.
    // rawVolume is the track VOL byte to sound it at, or -1 for the track's
    // current one (AudioEngine::kPreviewVolNone); pan is the track PAN to
    // sound it at, or M4A_AUDITION_PAN_NONE for the track's current one.
    void auditionNote(int track, int key, int velocity, int rawVolume, int pan);
    // Self-releasing audition (band-sweep chord preview); forwarded to
    // AudioEngine::previewNoteTimed, which sends the note-off itself.
    // velocity 0 releases the track+key's preview early. rawVolume and pan as
    // above.
    void auditionNoteTimed(int track, int key, int velocity, quint32 durationSamples, int rawVolume,
                           int pan);
    // Voicegroup-entry audition from the voice picker; routed to
    // AudioEngine::previewVoice like the voicegroup browser's signal.
    void auditionVoice(int voice, int key, int velocity);
    void statusMessage(const QString &text);
    // Edit cursor committed to a new position (click released); the main
    // window seeks playback here when not stopped.
    void editCursorMoved(uint64_t tick);
    // Roll/event-list swap (user toggle or applyViewState); the main window
    // mirrors it into the View-menu checkbox.
    void eventListVisibilityChanged(bool visible);
    // Velocity-lane toggle from the keyboard; the main window mirrors it into
    // the View-menu checkbox, which owns the persisted preference.
    void velocityLaneVisibilityChanged(bool visible);
    // Automation-lanes toggle from the keyboard or the lane toggle bar; the
    // main window mirrors it into the View-menu checkbox, which owns the
    // persisted preference.
    void automationLanesVisibilityChanged(bool visible);
    // Jump-from-context voice navigation: the main window raises the
    // voicegroup dock and selects this slot.
    void revealVoiceRequested(int program);

  protected:
    void resizeEvent(QResizeEvent *event) override;
    // Application-wide filter, watching only for mouse presses inside this
    // view: see refocusAfterDeadClick.
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    uint64_t gridTicksIn(const GridSeg &seg, bool snap = false) const;
    // The bare-letter shortcuts (A, V, B, M, S, …) dispatch from whichever
    // editing surface holds the keyboard focus, so a press on a part of the
    // view that takes no focus of its own — a track header, the ruler, the
    // toggle strip, a scrollbar — used to leave them stranded wherever the
    // focus was (the song list, a dock's search field). Such a press now
    // hands the focus to the editing surface, unless one of the view's
    // surfaces already holds it: clicking a header's M/S button with the
    // lanes focused keeps the lanes' shortcuts on the lanes. Presses that
    // land on a click-focusable widget (the surfaces, an inline editor, the
    // event table) are Qt's to focus and are left alone.
    void refocusAfterDeadClick(QWidget *clicked);
    // Document trackMoved handler: rotates the per-track view state with the
    // renumbered engine slots on apply, undo, and redo alike.
    void onTrackMoved(int fromChunk, int toChunk, const QVector<int> &map);
    // A mouse gesture is live in the ruler, roll, or lanes (pan, drag,
    // sweep); playhead follow-scroll pauses while one runs.
    bool userGestureActive() const;
    void syncPlayheadOverlay();
    int viewportWidth() const;
    void setHScroll(double px);
    double minHScroll() const;
    double maxHScroll() const;
    void setVScroll(double y);
    double maxRollScroll() const;
    void updateScrollbars();
    void rebuildAfterSongChange();
    void mergeEmptyLanes();
    // Engine tracks a track-scoped time selection resolves to (used and
    // document-mapped), and the copyable lane identities of one track (its
    // model lanes plus the voice changes).
    std::vector<int> timeSelectionTracks() const;
    std::vector<uint8_t> trackCcs(int track) const;
    // This song's MIDI resolution, clamped to at least 1 (the default when
    // no song is loaded). Copy and paste both derive theirs from here.
    uint32_t songTicksPerBeat() const;

    const MidiTimeline *m_timeline = nullptr;
    const LoadedVoiceGroup *m_voicegroup = nullptr;
    SongDocument *m_document = nullptr;
    SongViewModel m_model;

    double m_pxPerTick = 1.0;
    double m_scrollX = 0.0;
    double m_scrollY = 0.0;
    double m_keyHeight = songview::kVelHandleMinKeyH;
    int m_selectedTrack = 0;
    double m_playheadTick = 0.0;
    uint64_t m_editCursorTick = 0;
    bool m_playing = false;
    uint32_t m_muteMask = 0;
    uint32_t m_soloMask = 0;
    std::vector<NoteKey> m_selection;
    TimeSelection m_timeSel;
    uint32_t m_trackSelMask = 0; // header multi-selection (see trackSelectionMask)
    GridFeel m_gridFeel = GridFeel::Straight;
    int m_gridMinDenom = 0;                            // note denominator; 0 = clock-grid floor
    bool m_velocityColorMode = false;                  // velocityNoteColor fills (View menu)
    bool m_noteNameMode = false;                       // pitch labels on notes (View menu)
    bool m_followPlayhead = true;                      // playback follow-scroll (transport bar)
    std::vector<std::pair<int, uint8_t>> m_emptyLanes; // (track, cc), unsorted

    songview::TimeRuler *m_ruler = nullptr;
    songview::TrackHeaderPanel *m_headers = nullptr;
    songview::PianoRoll *m_roll = nullptr;
    QStackedWidget *m_rollStack = nullptr; // page 0: roll (+vbar), page 1: event list
    EventListView *m_events = nullptr;
    songview::AutomationArea *m_lanes = nullptr;
    QScrollArea *m_lanesScroll = nullptr;
    songview::VelocityLane *m_velocityLane = nullptr;
    songview::LaneToggleBar *m_laneToggles = nullptr; // owned by m_strip's gutter
    // Momentary pencil-key hold: press state kept until the matching
    // release decides sticky tap vs momentary hold (see handleEditKey /
    // handleEditKeyRelease).
    bool m_pencilKeyHeld = false;
    bool m_pencilKeyGesture = false; // a lane gesture started during the hold
    bool m_pencilKeyPrior = false;   // mode before the press; momentary restores it
    int m_pencilKey = 0;             // the pressed key, robust across rebinds
    std::chrono::steady_clock::time_point m_pencilKeyPressedAt;
    QSplitter *m_splitter = nullptr; // roll above, lanes area below
    bool m_splitInit = false;        // initial sizes applied on first layout
    songview::OtherStrip *m_strip = nullptr;
    songview::PlayheadOverlay *m_playheadOverlay = nullptr;
    QScrollBar *m_hbar = nullptr;
    QScrollBar *m_vbar = nullptr;
};
