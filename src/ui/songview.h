#pragma once

#include <QColor>
#include <QHash>
#include <QList>
#include <QWidget>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "core/miditimeline.h"
#include "ui/songviewmodel.h"

extern "C" {
#include "voicegroup_loader.h"
}

class EventListView;
class QKeyEvent;
class QScrollArea;
class QScrollBar;
class QSplitter;
class QStackedWidget;
class SongDocument;

namespace songview {
class TimeRuler;
class PianoRoll;
class AutomationArea;
class OtherStrip;
class TrackHeaderPanel;

// Fixed gutter geometry shared by every timeline-aligned child: the track
// header column plus the piano-roll keyboard column. All children put
// timeline tick 0 at the same global x.
constexpr int kHeaderW = 210;
constexpr int kKeyboardW = 52;
constexpr int kGutterW = kHeaderW + kKeyboardW;

// Full-width velocity handle (Reaper-style): with at least this much
// vertical zoom, the top strip of a note drags velocity across its whole
// width, and a thin horizontal bar inside the note shows the level
// (bottom = 0, top = 127). Below the threshold notes are all-Move; the
// right-click menu remains the velocity fallback. Also the default key
// height, so the handle is available out of the box.
constexpr int kVelHandleMinKeyH = 12;
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
        double pxPerBeat = 32.0;  // horizontal zoom (ticks-per-beat neutral)
        int keyHeight = songview::kVelHandleMinKeyH; // vertical roll zoom
        int scrollPx = 0;
        int scrollY = 0;
        int selectedTrack = 0;
        uint64_t editCursorTick = 0;
        int laneHeight = 48;      // shared automation row height
        QHash<QString, int> laneHeights; // per-row overrides (AutomationArea keys)
        QList<int> splitterSizes; // roll pane, lanes pane
        std::vector<std::pair<int, uint8_t>> emptyLanes; // (track, cc)
        int gridMinDenom = 0;     // snap-grid floor as a note denominator
                                  // (4/8/16/32); 0 = down to the clock grid
        bool gridTriplet = false; // triplet vs straight beat subdivisions
        bool eventList = false;   // raw MIDI event list instead of the roll
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

    // Raw MIDI event list: an alternative to the piano roll in the same
    // screen space (the ruler, headers, and automation lanes stay). Per-song
    // view state; toggled from the View menu.
    bool eventListVisible() const;
    void setEventListVisible(bool visible);

    // --- shared state for the child widgets ---
    const MidiTimeline *timeline() const { return m_timeline; }
    const SongViewModel &model() const { return m_model; }
    const LoadedVoiceGroup *voicegroup() const { return m_voicegroup; }

    int contentX(double tick) const { return int(tick * m_pxPerTick) - m_scrollPx; }
    double tickAtContentX(int x) const { return double(x + m_scrollPx) / m_pxPerTick; }
    double pxPerTick() const { return m_pxPerTick; }
    double pxPerBeat() const;
    int scrollY() const { return m_scrollY; }
    int keyHeight() const { return m_keyHeight; }
    double playheadTick() const { return m_playheadTick; }

    // Edit cursor (Reaper-style): placed by clicking the ruler or empty
    // roll space (with a document, dragging or double-clicking there draws
    // a note instead), distinct from the moving playback cursor. Playback
    // starts here, and paste anchors here.
    uint64_t editCursorTick() const { return m_editCursorTick; }
    // Visual placement only (ruler drag preview); commit emits
    // editCursorMoved so playback can follow.
    void setEditCursorTick(uint64_t tick);
    void commitEditCursor(uint64_t tick);
    // Transport "go to start": edit cursor to tick 0 and scroll home.
    void goToStart();

    int selectedTrack() const { return m_selectedTrack; }
    void selectTrack(int track);
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

    static QColor trackColor(int track);
    // The track's program at the display position — the playhead while
    // playing, the edit cursor otherwise — so the header label follows the
    // song's voice changes. Before the first change it stays firstProgram
    // (which is what primes the engine), -1 if the track has none.
    int currentProgram(int track) const;
    QString instrumentLabel(int track) const; // "042 name (type)" from the voicegroup
    QString voiceShortName(uint8_t program) const;

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
    // barNumber) for every beat, honoring the song's time signature changes.
    void forEachGridLine(uint64_t tickBegin, uint64_t tickEnd,
                         const std::function<void(uint64_t, bool, int)> &fn) const;

    // --- editing support for the child widgets ---
    // Snap-grid feel and floor (the ruler's grid controls): the zoom-adaptive
    // grid subdivides beats by powers of two (straight) or by threes
    // (triplet), and the minimum subdivision — a note denominator, quarter =
    // one beat — stops it from refining past the note value the user cares
    // about. 0 keeps the default clock-grid floor. Per-song view state.
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

    // Snap grid in ticks at a position: the visible subdivision of the
    // governing segment's beat at the current feel, floored at the minimum
    // subdivision (1/4 = one beat of that signature) and never finer than
    // the song's mid2agb clock base.
    uint64_t gridTicksAt(uint64_t tick) const;
    // Fine placement (Alt-drag in the lanes): the mid2agb clock grid — the
    // document's real resolution — regardless of the zoom-dependent grid.
    uint64_t fineGridTicks() const;
    // Nearest / previous grid position, anchored at the governing
    // time-signature segment (fine snap stays on the absolute clock grid).
    uint64_t snapTick(double tick, bool fine = false) const;
    uint64_t snapTickDown(double tick) const;
    uint64_t snapTickUp(double tick) const;

    // Note selection on the selected track, identified by (startTick, key) so
    // it survives document rebuilds.
    struct NoteId {
        uint32_t tick;
        uint8_t key;
        bool operator==(const NoteId &other) const
        {
            return tick == other.tick && key == other.key;
        }
    };
    const std::vector<NoteId> &selection() const { return m_selection; }
    bool isSelected(const ViewNote &note) const;
    void setSelection(std::vector<NoteId> ids);
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
    // clips, and Ctrl+arrow transpose/nudge of the selection. Returns true
    // when consumed.
    bool handleEditKey(QKeyEvent *event);
    // Copy/Cut/Delete/Paste/Clear context menu on the active selection.
    void showTimeSelectionMenu(const QPoint &globalPos);

    // App-internal clipboard. A plain note copy (roll selection) has span 0
    // and pastes additively; a range copy carries span > 0 plus lane
    // segments and pastes with replace semantics. Ticks are offsets from
    // the copied block's start so paste can re-anchor at the edit cursor.
    // Survives track switches and document rebuilds; cleared on song swap
    // (another song's ticks-per-beat may differ).
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
        uint64_t span = 0; // ticks covered; 0 = plain note clip
        bool wholeLane = false; // gutter "Copy lane" (paste-lane anchor is 0)
        std::vector<ClipTrack> tracks;
        std::vector<ClipLane> lanes;
        bool empty() const { return tracks.empty() && lanes.empty(); }
    };
    Clip &clipboard() { return m_clip; }

    // "velocity 93 → plays 96 · length 25 → 24 clocks" for the status bar.
    void announceNote(const ViewNote &note);

    // Child-widget entry point for the auditionNote signal.
    void audition(int track, int key, int velocity)
    {
        emit auditionNote(track, key, velocity);
    }

    // Child-widget entry point for the statusMessage signal.
    void announce(const QString &text) { emit statusMessage(text); }

    // Interaction from children.
    void zoomAroundContentX(double factor, int anchorContentX);
    // Vertical roll zoom (key height) from Ctrl+wheel, pinning the key under
    // anchorY (roll-local y). wheelDelta is the raw angleDelta value.
    void zoomKeyHeight(int wheelDelta, int anchorY);
    void scrollByPx(int dx);
    void scrollRollBy(int dy);
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
    void auditionNote(int track, int key, int velocity);
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

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    uint64_t gridTicksIn(const GridSeg &seg) const;
    // Document trackMoved handler: rotates the per-track view state with the
    // renumbered engine slots on apply, undo, and redo alike.
    void onTrackMoved(int fromChunk, int toChunk, const QVector<int> &map);
    // A mouse gesture is live in the ruler, roll, or lanes (pan, drag,
    // sweep); playhead follow-scroll pauses while one runs.
    bool userGestureActive() const;
    int viewportWidth() const;
    void setHScroll(int px);
    void updateScrollbars();
    void rebuildAfterSongChange();
    void mergeEmptyLanes();
    // Engine tracks a track-scoped time selection resolves to (used and
    // document-mapped), and the copyable lane identities of one track (its
    // model lanes plus the voice changes).
    std::vector<int> timeSelectionTracks() const;
    std::vector<uint8_t> trackCcs(int track) const;

    const MidiTimeline *m_timeline = nullptr;
    const LoadedVoiceGroup *m_voicegroup = nullptr;
    SongDocument *m_document = nullptr;
    SongViewModel m_model;

    double m_pxPerTick = 1.0;
    int m_scrollPx = 0;
    int m_scrollY = 0;
    int m_keyHeight = songview::kVelHandleMinKeyH;
    int m_keyZoomAccum = 0; // sub-notch wheel remainder for zoomKeyHeight
    int m_selectedTrack = 0;
    double m_playheadTick = 0.0;
    uint64_t m_editCursorTick = 0;
    bool m_playing = false;
    uint32_t m_muteMask = 0;
    uint32_t m_soloMask = 0;
    std::vector<NoteId> m_selection;
    TimeSelection m_timeSel;
    Clip m_clip;
    uint32_t m_trackSelMask = 0; // header multi-selection (see trackSelectionMask)
    GridFeel m_gridFeel = GridFeel::Straight;
    int m_gridMinDenom = 0; // note denominator; 0 = clock-grid floor
    std::vector<std::pair<int, uint8_t>> m_emptyLanes; // (track, cc), unsorted

    songview::TimeRuler *m_ruler = nullptr;
    songview::TrackHeaderPanel *m_headers = nullptr;
    songview::PianoRoll *m_roll = nullptr;
    QStackedWidget *m_rollStack = nullptr; // page 0: roll (+vbar), page 1: event list
    EventListView *m_events = nullptr;
    songview::AutomationArea *m_lanes = nullptr;
    QScrollArea *m_lanesScroll = nullptr;
    QSplitter *m_splitter = nullptr; // roll above, lanes area below
    bool m_splitInit = false;        // initial sizes applied on first layout
    songview::OtherStrip *m_strip = nullptr;
    QScrollBar *m_hbar = nullptr;
    QScrollBar *m_vbar = nullptr;
};
