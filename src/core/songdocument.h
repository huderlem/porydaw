#pragma once

#include <QObject>
#include <QString>
#include <QUndoStack>
#include <QVector>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "core/noteid.h"
#include "core/smf.h"
#include "project/decompproject.h"

class MidiTimeline;

// Pseudo-CC numbers for lanes that aren't controller-backed. DOC_CC_BEND
// matches LANE_CC_BEND in the view model.
constexpr uint8_t DOC_CC_BEND = 0xFF;  // pitch-bend events (0xE)
constexpr uint8_t DOC_CC_TEMPO = 0xFE; // tempo metas; live in SMF track 0 only,
                                       // because mid2agb reads seq events
                                       // (tempo/timesig/loop markers) from the
                                       // first MTrk chunk exclusively
constexpr uint8_t DOC_CC_VOICE = 0xFD; // program changes (0xC): the track's
                                       // voice; value is the voicegroup entry

// Loop markers as mid2agb reads them: a text-type meta (0x01-0x07) whose
// content, truncated to 32 bytes and whitespace-trimmed, is the single
// marker character. Matches MidiTimeline::build.
bool metaIsLoopMarker(const SmfEvent &ev, char marker);

// True for names mid2agb would read as a loop/label command instead of a
// name — it accepts ANY text-type meta (0x01-0x07, Track Name included)
// in the seq chunk whose whole text is one of these.
bool nameIsLoopMarker(const QString &name);

// Maps every pre-mutation SMF chunk and engine slot to its post-mutation
// owner (tracksRemapped). A value of -1 means the old owner was deleted or
// stopped being an engine track; the post-mutation counts identify inserted
// owners.
struct TrackRemap {
    std::vector<int> smfTrackMap;    // old chunk -> new chunk
    std::vector<int> engineTrackMap; // old engine slot -> new engine slot
    int newSmfTrackCount = 0;
    int newEngineTrackCount = 0;

    bool isIdentity() const;
};

// A note located in the SMF model: the note-on event plus the event that ends
// it, paired exactly as mid2agb pairs them (first same-channel same-key
// note-off or velocity-0 note-on after the note-on). Indices are valid only
// until the next document mutation; re-resolve after documentChanged().
// noteId is the mutation-stable identity: it survives moves, resizes,
// velocity edits, and undo/redo, so it can re-resolve a note after any of
// them (findNote(NoteId)).
struct DocNote {
    NoteId noteId;
    int engineTrack = -1;
    int smfTrack = -1;
    size_t onIndex = 0;
    size_t endIndex = SIZE_MAX; // SIZE_MAX = unterminated note-on
    uint64_t tick = 0;
    uint32_t duration = 0; // ticks (0 when unterminated)
    uint8_t key = 0;
    uint8_t velocity = 0;
    uint8_t channel = 0;

    bool unterminated() const { return endIndex == SIZE_MAX; }
};

// An automation point (CC value, pitch bend, tempo, or voice change) located
// in the SMF model. Same staleness rule as DocNote.
struct DocLanePoint {
    int smfTrack = -1;
    size_t index = 0;
    uint64_t tick = 0;
    int value = 0; // CC: 0-127; bend: -8192..8191; tempo: BPM; voice: 0-127
};

// A time-signature event (meta 0x58) located in the SMF model. Same
// staleness rule as DocNote.
struct DocTimeSig {
    int smfTrack = -1;
    size_t index = 0;
    uint64_t tick = 0;
    uint8_t numerator = 4;
    uint8_t denomPow2 = 2; // denominator = 1 << denomPow2
};

// The editable song document (SPEC.md §4): a full-fidelity SMF model plus the
// song's midi.cfg properties, with every mutation undoable. The .mid file is
// canonical storage; saving writes it plus (when changed) the song's midi.cfg
// line — nothing else in the project is ever touched.
class SongDocument : public QObject
{
    Q_OBJECT

  public:
    explicit SongDocument(QObject *parent = nullptr);

    bool load(const SongInfo &song, QString *error);
    bool save(QString *error);

    const QString &midPath() const { return m_midPath; }
    const QString &label() const { return m_label; }
    const SmfFile &smf() const { return m_smf; }
    const SongCfg &cfg() const { return m_cfg; }
    QUndoStack *undoStack() { return &m_undoStack; }
    bool isDirty() const { return !m_undoStack.isClean(); }
    // Bumped once per published mutation (edit, undo, redo, load), so a
    // caller can detect that state resolved earlier — DocNote indices, a
    // pending batch write — may be stale.
    uint64_t revision() const { return m_revision; }

    // The song's clock base for snapping: ticks per mid2agb clock. mid2agb
    // rescales everything to 24 (or 48 with -X) clocks/beat; finer positions
    // are quantized away by the build, so the editor snaps to this grid.
    uint32_t ticksPerClock() const;

    // Engine-track mapping (mirrors MidiTimeline::build).
    int engineTrackCount() const { return int(m_engineToSmf.size()); }
    int smfTrackFor(int engineTrack) const;
    uint8_t channelFor(int engineTrack) const;

    // Tracks this song's music player allocates in-game (DecompProject::
    // trackBudgetFor, set by the owner after load). MPlayStart never starts
    // tracks at or beyond this index, so playback mutes them and the UI
    // marks them; editing is never gated on it. Default 16 = engine ceiling.
    int trackBudget() const { return m_trackBudget; }
    void setTrackBudget(int budget) { m_trackBudget = std::clamp(budget, 0, 16); }

    // Lookups. Results go stale on any mutation.
    std::vector<DocNote> notesForTrack(int engineTrack) const;
    bool findNote(int engineTrack, uint64_t tick, uint8_t key, DocNote *out) const;
    // Re-resolve a note by its mutation-stable identity. False when the id
    // is unassigned or its note no longer exists (or its chunk lost its
    // engine slot).
    bool findNote(NoteId id, DocNote *out) const;
    std::vector<DocLanePoint> lanePoints(int engineTrack, uint8_t cc) const;
    bool findLanePoint(int engineTrack, uint8_t cc, uint64_t tick, DocLanePoint *out) const;
    // Loop markers ('[' / ']' text metas); UINT64_MAX when absent.
    uint64_t loopTick(bool endMarker) const;
    // Time signatures (meta 0x58), sorted by tick. When several share a tick
    // the last one is the one the bar grid honors.
    std::vector<DocTimeSig> timeSigs() const;

    // Edits. Each call pushes one undoable command and emits documentChanged.
    void addNote(int engineTrack, uint64_t tick, uint8_t key, uint32_t duration, uint8_t velocity);
    // Batch insert (clipboard paste): all notes land in one undoable command.
    struct NewNote {
        uint64_t tick;
        uint8_t key;
        uint32_t duration;
        uint8_t velocity;
    };
    void addNotes(int engineTrack, const std::vector<NewNote> &notes);
    void deleteNotes(const std::vector<DocNote> &notes);
    // Move by a tick/key delta (note lengths preserved). mergeable marks a
    // keyboard transpose/nudge press: consecutive mergeable moves of the
    // same notes collapse into one undo command that re-lands from the
    // gesture's start, so a neighbor trimmed by a merely-passed-through
    // overlap comes back (only the final resting position trims). A merged
    // press that returns every note to the gesture's start removes the
    // command entirely — dragging away and back leaves no undo entry — but
    // still publishes so views refresh. Mouse gestures stay one command per
    // drag. A move whose clamped result changes nothing pushes nothing.
    void moveNotes(const std::vector<DocNote> &notes, int64_t dTick, int dKey,
                   bool mergeable = false);
    void resizeNotes(const std::vector<DocNote> &notes, int64_t dDuration);
    // Left-edge resize: move the note-on by dTick with the note-off pinned
    // (tick and duration adjust together, at least 1 tick of note remains).
    void resizeNotesLeft(const std::vector<DocNote> &notes, int64_t dTick);
    void setNotesVelocity(const std::vector<DocNote> &notes, uint8_t velocity);
    // Batch velocity write by NoteId, one undo entry, values clamped to
    // 1-127 (a later entry for the same note wins). All-or-nothing: refuses
    // — mutating nothing — when expectedRevision is not the current
    // revision or any id fails to resolve, returning nullopt so the caller
    // re-resolves and retries; otherwise returns the revision the write
    // committed at (unchanged when every note already had its target).
    std::optional<uint64_t> setNotesVelocities(uint64_t expectedRevision,
                                               const std::vector<NoteVelocity> &velocities);
    // Relative velocity change, clamped to 1-127 per note; notes whose
    // clamped result is their current value are skipped (an all-skipped
    // call pushes nothing).
    void nudgeNotesVelocity(const std::vector<DocNote> &notes, int delta);

    void addLanePoint(int engineTrack, uint8_t cc, uint64_t tick, int value);
    // The one tempo range, shared by every tempo read, write, and the
    // transport spinner. The floor is 4, not 1: SMF's tempo meta is a
    // 24-bit microseconds-per-beat field, and 0xFFFFFF us rounds to 4 BPM —
    // anything slower would overflow the field and store a wrong tempo.
    static constexpr int kTempoMin = 4;
    static constexpr int kTempoMax = 999;
    static constexpr int kTempoDefault = 120; // SMF's tempo when a song sets none
    // The song's starting tempo, the transport bar's Tempo spinner: the
    // tempo in effect at tick 0 (the last tick-0 tempo meta in the seq
    // chunk; kTempoDefault when the song sets none). Reads clamp a foreign
    // file's out-of-range meta into kTempoMin-kTempoMax, matching the write
    // clamp. setStartTempo writes a tick-0 tempo meta, replacing any already
    // there (addLanePoint's rule), clamped the same way; a value the song
    // already starts at pushes nothing. Later tempo changes are untouched.
    int startTempo() const;
    void setStartTempo(int bpm);
    // How many tempo metas sit after tick 0 — nonzero means the song's
    // tempo is not a single number, and the start tempo alone does not
    // describe it.
    int tempoChangesAfterStart() const;
    // How many tempo metas sit outside the first chunk. mid2agb reads tempo
    // from the first chunk only, so these never play — in-game or in the app
    // (MidiTimeline mirrors mid2agb). Import moves them into the first chunk
    // (moveTempoMetasToFirstChunk); a nonzero count here means a hand-placed
    // foreign file, surfaced on the transport's tempo warning.
    int tempoMetasOutsideFirstChunk() const;
    // Gesture write (freehand sweep / line ramp): replaces every point of the
    // lane inside [tickBegin, tickEnd] with the given stream, as one undoable
    // command. An empty stream just clears the range; a call with nothing to
    // remove and nothing to write pushes nothing. Not for DOC_CC_VOICE (the
    // voice row has no drag editing).
    struct LanePointValue {
        uint64_t tick;
        int value;
    };
    void writeLanePoints(int engineTrack, uint8_t cc, uint64_t tickBegin, uint64_t tickEnd,
                         const std::vector<LanePointValue> &points);
    // Batch lane-point move, one undo entry. Landing on an occupied tick
    // replaces what sits there (addLanePoint's rule), including shadowed
    // same-tick duplicates under a value edit; two moved points converging
    // on one tick resolve last-input-wins. Stale moves (the point no longer
    // matches the document) are dropped, and a batch that changes nothing
    // pushes nothing. engineTrack -1 with DOC_CC_TEMPO targets the tempo
    // lane.
    struct LanePointMove {
        int engineTrack = -1;
        uint8_t cc = 0;
        DocLanePoint point;
        uint64_t newTick = 0;
        int newValue = 0;
    };
    void moveLanePoints(const std::vector<LanePointMove> &moves);
    // Single-point convenience over moveLanePoints for the existing editor.
    void moveLanePoint(int engineTrack, uint8_t cc, const DocLanePoint &point, uint64_t newTick,
                       int newValue);
    void deleteLanePoints(int engineTrack, uint8_t cc, const std::vector<DocLanePoint> &points);

    // Multi-track range edit (time-selection delete/paste): removals and
    // insertions across any mix of tracks and lanes, applied as one undoable
    // command with a single documentChanged emission. Notes/points to remove
    // must be freshly resolved (their indices are read at push time); an
    // engineTrack of -1 targets the tempo lane (DOC_CC_TEMPO only).
    struct RangeEdit {
        std::vector<DocNote> removeNotes;
        std::vector<DocLanePoint> removePoints;
        struct TrackNotes {
            int engineTrack;
            std::vector<NewNote> notes;
        };
        std::vector<TrackNotes> addNotes;
        struct LaneWrite {
            int engineTrack; // -1 = tempo (seq chunk)
            uint8_t cc;
            std::vector<LanePointValue> points; // absolute ticks
        };
        std::vector<LaneWrite> addPoints;

        bool empty() const
        {
            return removeNotes.empty() && removePoints.empty() && addNotes.empty() &&
                   addPoints.empty();
        }
    };
    void applyRangeEdit(const QString &text, const RangeEdit &edit);

    // Time-selection nudge: shift notes and lane points (any mix of tracks
    // and lanes, tempo included) by a tick delta as one undoable command.
    // Events are re-inserted with their exact bytes, so tempo blobs keep
    // their precise microseconds and unterminated notes stay unterminated.
    void moveRange(const std::vector<DocNote> &notes, const std::vector<DocLanePoint> &points,
                   int64_t dTick);

    // Ripple delete (time-selection "Remove contents"): erases [startTick,
    // endTick) on the scoped streams and closes the gap — everything at or
    // after endTick moves left by the span. Value streams (CC, bend, voice,
    // tempo, time signatures) keep the state the shifted content was
    // authored under: the last in-range point moves to startTick instead of
    // vanishing (unless a point shifts onto that seam from endTick anyway).
    // tracks ripple notes plus every non-note channel event of the track;
    // wholeSong — the all-tracks cut — ignores tracks/lanes and ripples
    // every engine track plus the global rows: tempo, time signatures, loop
    // markers and other metas (moved to the seam, never deleted), and each
    // chunk's end-of-track tick, so the song itself gets shorter. One
    // undoable command; returns false when nothing would change.
    struct RippleScope {
        std::vector<int> tracks;                    // engine tracks (ignored when wholeSong)
        std::vector<std::pair<int, uint8_t>> lanes; // (engineTrack, cc); -1 = tempo
        bool wholeSong = false;
    };
    bool removeTimeRange(uint64_t startTick, uint64_t endTick, const RippleScope &scope);

    // Raw SMF edits (the MIDI event list view): direct event-level operations
    // on one chunk, indices being current positions in its event vector.
    // Insert places the event at its tick's canonical position (setup events
    // ahead of same-tick notes and note ends ahead of same-tick note-ons,
    // like every other edit); a modify that changes
    // the tick re-inserts so event order stays non-decreasing — re-resolve
    // indices afterwards. No semantic validation happens here: an orphan
    // note-on or a bogus meta is the raw editor's prerogative (the SMF still
    // writes, and the playable projection is built defensively).
    void insertRawEvent(int smfTrack, const SmfEvent &event);
    void modifyRawEvent(int smfTrack, size_t index, const SmfEvent &event);
    void deleteRawEvents(int smfTrack, std::vector<size_t> indices);
    // Reorder within a tick: the event at index ends up at destIndex (its
    // position in the post-move vector). Same-tick order is significant —
    // the file keeps it and mid2agb stable-sorts — so this is the one raw
    // edit that picks position directly. The destination is clamped to
    // rawEventMoveBounds, so a move can never cross a tick boundary or
    // break the canonical intra-tick invariants insert maintains; a move
    // whose clamped destination is the current position is a no-op.
    void moveRawEvent(int smfTrack, size_t index, size_t destIndex);
    // The inclusive [first, last] range moveRawEvent would accept for the
    // event at index: its own tick group, minus positions that would put a
    // setup event after a same-tick note or a note-end after a same-tick
    // note-on (in either direction — the moved event may not cross an
    // event the canonical order pins it against). False when index is out
    // of range.
    bool rawEventMoveBounds(int smfTrack, size_t index, size_t *first, size_t *last) const;
    // Move the chunk's end-of-track marker; clamped so it never precedes the
    // chunk's last event.
    void setTrackEndTick(int smfTrack, uint64_t tick);

    // Move or create a loop marker; tick == -1 removes it.
    void setLoopTick(bool endMarker, int64_t tick);

    // Set the time signature at a tick: modifies the winning 0x58 meta
    // already there (keeping its chunk and metronome bytes), or inserts a
    // new one in the seq chunk, like tempo and loop markers. moveTimeSig
    // relocates every 0x58 at fromTick, overwriting any at toTick;
    // deleteTimeSig removes every 0x58 at the tick.
    void setTimeSig(uint64_t tick, int numerator, int denomPow2);
    void moveTimeSig(uint64_t fromTick, uint64_t toTick);
    void deleteTimeSig(uint64_t tick);

    // Track create/delete. A track needs a channel event to occupy an engine
    // slot (rebuildTrackMap), so a new track is seeded with a program change
    // at tick 0 carrying the given voicegroup entry, in a new MTrk chunk
    // appended on an unused MIDI channel. Returns the new engine track, -1
    // if none is free.
    bool canAddTrack() const;
    int addTrack(int voice);
    // Copy a track onto a fresh engine slot: the channel events on the
    // track's OWN channel land in a new appended chunk on a free MIDI
    // channel (a foreign file's chunk can interleave several channels; the
    // other channels' events belong to other engine tracks and are not
    // copied). Metas are not copied — a duplicated tempo, time signature,
    // or loop marker would double up as a global event. Returns the copy's
    // engine track, -1 when no slot is free.
    int duplicateTrack(int engineTrack);
    // Removes the track's chunk — except chunk 0 (the seq chunk mid2agb
    // reads tempo/timesig/loop from), which only has its channel events
    // stripped; a winning loop marker inside a removed chunk is moved to
    // chunk 0 so the loop survives.
    void deleteTrack(int engineTrack);
    // Reorder: the track lands at the target track's engine slot. The
    // track's chunk moves, events untouched — AGB track order is chunk
    // order — and when the move displaces chunk 0, the seq globals (tempo,
    // time signatures, loop markers) migrate to the new first chunk, where
    // mid2agb and the tempo lane read them. Returns true when a command was
    // pushed (false: no-op or unmapped slot).
    bool moveTrack(int engineTrack, int targetEngine);

    // Track display name, exactly as MidiTimeline reads it: the chunk's
    // first unprefixed Track Name meta (0x03; one scoped to a channel by a
    // MIDI Channel Prefix meta 0x20 never counts). Rename modifies the 0x03
    // in place (keeping its tick and position) or inserts it at tick 0; an
    // empty name removes it. Names mid2agb would read as loop/label markers
    // (nameIsLoopMarker) are refused.
    QString trackName(int engineTrack) const;
    void renameTrack(int engineTrack, const QString &name);

    void setCfg(const SongCfg &cfg);

    // Playable projection for the audio engine (MidiTimeline::build).
    std::unique_ptr<MidiTimeline> buildTimeline(double sampleRate) const;

  signals:
    // Emitted after every mutation, undo, and redo.
    void documentChanged();
    // Emitted after the track map is rebuilt — before the documentChanged
    // that follows — whenever a mutation, undo, or redo changes chunk or
    // engine-track ownership (add/delete/move/duplicate, or a raw edit that
    // gives a chunk its first channel event or removes its last). Receivers
    // holding per-track or per-chunk state remap it here.
    void tracksRemapped(TrackRemap remap);
    // Emitted while a track-reorder MoveTrack op applies or reverts, before
    // the documentChanged that follows. fromChunk/toChunk are the chunk
    // endpoints. engineMap has 16 entries: engineMap[t] is where the track
    // at engine slot t (pre-move numbering) lands — a contiguous rotation
    // between the endpoints, identity elsewhere. Undo emits the inverse, so
    // receivers holding per-track or per-chunk state remap it here and stay
    // right across undo/redo. The document is mid-mutation when this fires:
    // remap state only, don't read back. Transitional: tracksRemapped above
    // covers every ownership change (moves included); existing receivers
    // migrate there and this signal then goes away.
    void trackMoved(int fromChunk, int toChunk, QVector<int> engineMap);

  private:
    friend class SongEditCommand;
    friend class SongCfgCommand;
    friend class MoveNotesCommand;

    struct EditOp {
        enum Type {
            InsertEvent,
            RemoveEvent,
            ModifyEvent,
            MoveEvent,   // reorder within a tick group: index -> indexTo
            InsertTrack, // insert trackData as chunk smfTrack
            RemoveTrack, // remove chunk smfTrack (contents recorded on apply)
            SetTrackEnd, // set chunk endTick to event.tick (old recorded on apply)
            MoveTrack    // move chunk smfTrack so it lands at index smfTrackTo
        } type;
        int smfTrack = 0;
        int smfTrackTo = 0;      // MoveTrack: the chunk's index after the move
        size_t index = 0;        // Remove/Modify/Move: target; Insert: recorded on apply
        size_t indexTo = 0;      // MoveEvent: the event's index after the move
        SmfEvent event;          // Insert: new event; Modify: new content (same tick)
        SmfEvent oldEvent;       // recorded on apply (Remove/Modify)
        uint64_t oldEndTick = 0; // recorded on apply (Insert past track end)
        // Insert/Modify of a note-on: event.noteId is the moved note's
        // identity, keep it. Otherwise applyOps mints a fresh id on first
        // apply and sets this, so redo replays the same identity.
        bool preservesNoteId = false;
        SmfTrack trackData; // InsertTrack: content; RemoveTrack: recorded on apply
    };
    // What trackRemap diffs against: the chunk count and engine mapping
    // before a command's ops applied.
    struct TrackMapState {
        int smfTrackCount = 0;
        std::vector<int> engineToSmf;
    };

    void applyOps(std::vector<EditOp> &ops);
    void revertOps(std::vector<EditOp> &ops);
    void pushEdit(const QString &text, std::vector<EditOp> ops);
    void rebuildTrackMap();
    TrackMapState trackMapState() const;
    // Identity maps sized to the current document (what an edit that never
    // touches track structure publishes).
    TrackRemap currentTrackRemap() const;
    // The remap a just-applied op list produced: chunk ownership replayed
    // from the ops, engine ownership diffed against the rebuilt map.
    TrackRemap trackRemap(const TrackMapState &before, const std::vector<EditOp> &ops) const;
    // The one place every mutation, undo, and redo reports through: bumps
    // the revision, emits tracksRemapped for a non-identity remap, then
    // documentChanged.
    void publishMutation(TrackRemap remap);
    void mintNoteId(SmfEvent *event);
    void mintUnassignedNoteIds();
    int engineTrackForChunk(int chunk) const; // -1 = no engine slot
    // Lowest MIDI channel no existing engine track uses; -1 when all 16 are
    // taken.
    int freeChannel() const;

    // Builder helpers (operate on current state; see applyOps for index rules).
    SmfEvent makeChannelEvent(uint8_t typeNibble, uint8_t channel, uint64_t tick, uint8_t data0,
                              uint8_t data1) const;
    void appendNoteInsertOps(std::vector<EditOp> &ops, int smfTrack, uint8_t channel, uint64_t tick,
                             uint8_t key, uint32_t duration, uint8_t velocity) const;
    void appendRemoveOps(std::vector<EditOp> &ops, int smfTrack, std::vector<size_t> indices) const;
    // Same-key overlap resolution for edits that write notes. The pairing
    // rule (every note-on takes the first same-key end after it) cannot
    // represent two overlapping notes on one key — a written note landing
    // over a stationary one would silently re-pair the neighbor's end. So
    // the edited note wins: a stationary same-track same-key note
    // overlapping a written span keeps its head (end trimmed to the span
    // start), keeps its tail (start moved to the span end), or is removed
    // when fully covered — never split. written spans are the notes the
    // edit is inserting; editNotes are the notes it already rewrites
    // (excluded from trimming). Victim indices are appended to removals
    // (per SMF track, for the caller's appendRemoveOps pass) and the
    // trimmed events re-inserted with their exact bytes via trims (the
    // caller appends them after all its removals).
    struct PlannedNote {
        int engineTrack;
        uint8_t key;
        uint64_t tick;
        uint64_t endTick; // exclusive
    };
    void resolveNoteOverlaps(const std::vector<PlannedNote> &written,
                             const std::vector<DocNote> &editNotes,
                             std::vector<std::vector<size_t>> &removals,
                             std::vector<EditOp> &trims) const;
    // moveNotes' op builder, split out so MoveNotesCommand can rebuild the
    // move with an accumulated delta when merging keyboard presses.
    std::vector<EditOp> buildMoveNotesOps(const std::vector<DocNote> &notes, int64_t dTick,
                                          int dKey) const;
    // Replace one event: modify in place when the tick is unchanged (the
    // event keeps its position within its tick group — mid2agb stable-sorts,
    // so same-tick order is significant), else remove + re-insert so ticks
    // stay sorted.
    void appendEventEditOps(std::vector<EditOp> &ops, int smfTrack, size_t index,
                            const SmfEvent &event) const;
    bool laneEventMatches(const SmfEvent &ev, uint8_t cc) const;
    int laneValue(const SmfEvent &ev, uint8_t cc) const;
    SmfEvent makeLaneEvent(uint8_t cc, uint8_t channel, uint64_t tick, int value) const;
    void addLanePoint(const QString &text, int engineTrack, uint8_t cc, uint64_t tick, int value);
    // Locates the loop marker event, mirroring MidiTimeline::build's rule
    // (first matching text meta in track/event order). Returns false if absent.
    bool findLoopMarkerEvent(bool endMarker, int *smfTrack, size_t *index) const;

    SmfFile m_smf;
    SongCfg m_cfg;
    SongCfg m_savedCfg; // as on disk, to detect midi.cfg write-back needs
    QString m_midPath;
    QString m_label;
    QString m_projectRoot;
    bool m_hadCfgLine = false;
    QUndoStack m_undoStack;
    uint64_t m_revision = 0;
    // Monotonic across loads (never reset), so a stale NoteId from before a
    // reload can never alias a freshly minted one.
    uint64_t m_nextNoteId = 1;

    std::vector<int> m_engineToSmf;       // engine track -> SMF track
    std::vector<uint8_t> m_engineChannel; // engine track -> MIDI channel
    int m_trackBudget = 16;
};
