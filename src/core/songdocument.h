#pragma once

#include <QObject>
#include <QString>
#include <QUndoStack>
#include <cstdint>
#include <memory>
#include <vector>

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

// A note located in the SMF model: the note-on event plus the event that ends
// it, paired exactly as mid2agb pairs them (first same-channel same-key
// note-off or velocity-0 note-on after the note-on). Indices are valid only
// until the next document mutation; re-resolve after documentChanged().
struct DocNote {
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

    // The song's clock base for snapping: ticks per mid2agb clock. mid2agb
    // rescales everything to 24 (or 48 with -X) clocks/beat; finer positions
    // are quantized away by the build, so the editor snaps to this grid.
    uint32_t ticksPerClock() const;

    // Engine-track mapping (mirrors MidiTimeline::build).
    int engineTrackCount() const { return int(m_engineToSmf.size()); }
    int smfTrackFor(int engineTrack) const;
    uint8_t channelFor(int engineTrack) const;

    // Lookups. Results go stale on any mutation.
    std::vector<DocNote> notesForTrack(int engineTrack) const;
    bool findNote(int engineTrack, uint64_t tick, uint8_t key, DocNote *out) const;
    std::vector<DocLanePoint> lanePoints(int engineTrack, uint8_t cc) const;
    bool findLanePoint(int engineTrack, uint8_t cc, uint64_t tick, DocLanePoint *out) const;
    // Loop markers ('[' / ']' text metas); UINT64_MAX when absent.
    uint64_t loopTick(bool endMarker) const;

    // Edits. Each call pushes one undoable command and emits documentChanged.
    void addNote(int engineTrack, uint64_t tick, uint8_t key, uint32_t duration,
                 uint8_t velocity);
    void deleteNotes(const std::vector<DocNote> &notes);
    // Move by a tick/key delta (note lengths preserved).
    void moveNotes(const std::vector<DocNote> &notes, int64_t dTick, int dKey);
    void resizeNotes(const std::vector<DocNote> &notes, int64_t dDuration);
    void setNotesVelocity(const std::vector<DocNote> &notes, uint8_t velocity);

    void addLanePoint(int engineTrack, uint8_t cc, uint64_t tick, int value);
    void moveLanePoint(int engineTrack, uint8_t cc, const DocLanePoint &point,
                       uint64_t newTick, int newValue);
    void deleteLanePoints(int engineTrack, uint8_t cc,
                          const std::vector<DocLanePoint> &points);

    // Move or create a loop marker; tick == -1 removes it.
    void setLoopTick(bool endMarker, int64_t tick);

    void setCfg(const SongCfg &cfg);

    // Playable projection for the audio engine (MidiTimeline::build).
    std::unique_ptr<MidiTimeline> buildTimeline(double sampleRate) const;

signals:
    // Emitted after every mutation, undo, and redo.
    void documentChanged();

private:
    friend class SongEditCommand;
    friend class SongCfgCommand;

    struct EditOp {
        enum Type { InsertEvent, RemoveEvent, ModifyEvent } type;
        int smfTrack = 0;
        size_t index = 0;   // Remove/Modify: target; Insert: recorded on apply
        SmfEvent event;     // Insert: new event; Modify: new content (same tick)
        SmfEvent oldEvent;  // recorded on apply (Remove/Modify)
        uint64_t oldEndTick = 0; // recorded on apply (Insert past track end)
    };

    void applyOps(std::vector<EditOp> &ops);
    void revertOps(std::vector<EditOp> &ops);
    void pushEdit(const QString &text, std::vector<EditOp> ops);
    void rebuildTrackMap();

    // Builder helpers (operate on current state; see applyOps for index rules).
    SmfEvent makeChannelEvent(uint8_t typeNibble, uint8_t channel, uint64_t tick,
                              uint8_t data0, uint8_t data1) const;
    void appendNoteInsertOps(std::vector<EditOp> &ops, int smfTrack, uint8_t channel,
                             uint64_t tick, uint8_t key, uint32_t duration,
                             uint8_t velocity) const;
    void appendRemoveOps(std::vector<EditOp> &ops, int smfTrack,
                         std::vector<size_t> indices) const;
    bool laneEventMatches(const SmfEvent &ev, uint8_t cc, uint8_t channel) const;
    int laneValue(const SmfEvent &ev, uint8_t cc) const;
    SmfEvent makeLaneEvent(uint8_t cc, uint8_t channel, uint64_t tick, int value) const;
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

    std::vector<int> m_engineToSmf;      // engine track -> SMF track
    std::vector<uint8_t> m_engineChannel; // engine track -> MIDI channel
};
