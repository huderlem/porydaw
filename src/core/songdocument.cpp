#include "songdocument.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <map>
#include <numeric>
#include <set>
#include <tuple>

#include "core/miditimeline.h"
#include "project/songregistry.h"

// Declared in songdocument.h: the event list's summary column shares it.
bool metaIsLoopMarker(const SmfEvent &ev, char marker)
{
    if (!ev.isMeta() || ev.metaType < 0x01 || ev.metaType > 0x07)
        return false;
    const int len = std::min<int>(ev.blob.size(), 32);
    const QString text = QString::fromLatin1(ev.blob.constData(), len).trimmed();
    return text.size() == 1 && text[0] == QLatin1Char(marker);
}

bool TrackRemap::isIdentity() const
{
    if (smfTrackMap.size() != size_t(newSmfTrackCount) ||
        engineTrackMap.size() != size_t(newEngineTrackCount))
        return false;
    for (int i = 0; i < int(smfTrackMap.size()); i++) {
        if (smfTrackMap[i] != i)
            return false;
    }
    for (int i = 0; i < int(engineTrackMap.size()); i++) {
        if (engineTrackMap[i] != i)
            return false;
    }
    return true;
}

namespace {

// Time-signature metas as MidiTimeline::build reads them: numerator and
// denominator exponent must both be present.
bool metaIsTimeSig(const SmfEvent &ev)
{
    return ev.isMeta() && ev.metaType == 0x58 && ev.blob.size() >= 2;
}

// Tempo metas as the tempo lane reads them (lanePoints): the three-byte
// microseconds-per-beat payload must be present.
bool metaIsTempo(const SmfEvent &ev)
{
    return ev.isMeta() && ev.metaType == 0x51 && ev.blob.size() == 3;
}

// Move the chunk at `from` to index `to`, the chunks between shifting by one
// toward the vacated slot. applyOps and revertOps share it with the endpoints
// swapped — the mirror lives here, not in two hand-maintained rotates — and
// trackRemap replays it over its chunk-origin ints.
template <typename T>
void moveChunk(std::vector<T> &tracks, int from, int to)
{
    const auto begin = tracks.begin();
    if (from < to)
        std::rotate(begin + from, begin + from + 1, begin + to + 1);
    else
        std::rotate(begin + to, begin + from, begin + from + 1);
}

// trackMoved's engine permutation for a format-1 chunk move: the contiguous
// rotation between the engine endpoints (identity when either chunk has no
// engine slot).
QVector<int> engineRotationMap(int from, int to)
{
    QVector<int> map(16);
    for (int t = 0; t < 16; t++)
        map[t] = t;
    if (from < 0 || to < 0 || from == to)
        return map;
    for (int t = 0; t < 16; t++) {
        if (t == from)
            map[t] = to;
        else if (from < to && t > from && t <= to)
            map[t] = t - 1;
        else if (to < from && t >= to && t < from)
            map[t] = t + 1;
    }
    return map;
}

// Undo publishes the mirror image of the redo's remap: every surviving
// owner maps back to where it came from, deleted owners reappear as
// insertions (absent from the map, present in the counts).
TrackRemap inverseTrackRemap(const TrackRemap &remap)
{
    TrackRemap inverse;
    inverse.smfTrackMap.assign(size_t(remap.newSmfTrackCount), -1);
    inverse.engineTrackMap.assign(size_t(remap.newEngineTrackCount), -1);
    inverse.newSmfTrackCount = int(remap.smfTrackMap.size());
    inverse.newEngineTrackCount = int(remap.engineTrackMap.size());
    for (int old = 0; old < int(remap.smfTrackMap.size()); old++) {
        if (remap.smfTrackMap[old] >= 0)
            inverse.smfTrackMap[remap.smfTrackMap[old]] = old;
    }
    for (int old = 0; old < int(remap.engineTrackMap.size()); old++) {
        if (remap.engineTrackMap[old] >= 0)
            inverse.engineTrackMap[remap.engineTrackMap[old]] = old;
    }
    return inverse;
}

// The clamped landing position of a moved/resized note — the ONE definition
// per axis, shared by the no-op guards, the op builders, and the merge
// obsolete test. These must agree exactly: a drifted copy either refuses a
// real edit as a no-op or drops a merged command while the document is not
// actually back at its origin.
uint64_t movedNoteTick(const DocNote &note, int64_t dTick)
{
    return uint64_t(std::max<int64_t>(0, int64_t(note.tick) + dTick));
}

uint8_t movedNoteKey(const DocNote &note, int dKey)
{
    return uint8_t(std::clamp(int(note.key) + dKey, 0, 127));
}

uint32_t resizedNoteDuration(const DocNote &note, int64_t dDuration)
{
    return uint32_t(std::max<int64_t>(1, int64_t(note.duration) + dDuration));
}

// An unterminated note has no note-off to pin; its note-on just moves.
uint64_t leftResizedNoteTick(const DocNote &note, int64_t dTick)
{
    const int64_t maxTick =
        note.unterminated() ? INT64_MAX : int64_t(note.tick + note.duration) - 1;
    return uint64_t(std::clamp<int64_t>(int64_t(note.tick) + dTick, 0, maxTick));
}

bool cfgSemanticEqual(const SongCfg &a, const SongCfg &b)
{
    return a.voicegroupArg == b.voicegroupArg && a.masterVolume == b.masterVolume &&
           a.reverb == b.reverb && a.priority == b.priority && a.exactGate == b.exactGate &&
           a.extendedClocks == b.extendedClocks && a.noCompression == b.noCompression;
}

} // namespace

// Applies a prebuilt op list; undo reverts it. Op index rules: removals and
// modifications carry indices valid against the document state at apply time,
// so builders order all removals first (descending index per track) and let
// insertions resolve their position when applied.
class SongEditCommand : public QUndoCommand
{
  public:
    SongEditCommand(SongDocument *doc, const QString &text, std::vector<SongDocument::EditOp> ops)
        : QUndoCommand(text)
        , m_doc(doc)
        , m_ops(std::move(ops))
    {}

    void redo() override
    {
        const auto before = m_doc->trackMapState();
        m_doc->applyOps(m_ops);
        m_doc->rebuildTrackMap();
        m_remap = m_doc->trackRemap(before, m_ops);
        m_doc->publishMutation(m_remap);
    }

    void undo() override
    {
        m_doc->revertOps(m_ops);
        m_doc->rebuildTrackMap();
        m_doc->publishMutation(inverseTrackRemap(m_remap));
    }

  private:
    SongDocument *m_doc;
    std::vector<SongDocument::EditOp> m_ops;
    TrackRemap m_remap;
};

class SongCfgCommand : public QUndoCommand
{
  public:
    SongCfgCommand(SongDocument *doc, const SongCfg &newCfg)
        : QUndoCommand(QObject::tr("song settings"))
        , m_doc(doc)
        , m_new(newCfg)
        , m_old(doc->m_cfg)
    {}

    void redo() override
    {
        m_doc->m_cfg = m_new;
        m_doc->publishMutation(m_doc->currentTrackRemap());
    }

    void undo() override
    {
        m_doc->m_cfg = m_old;
        m_doc->publishMutation(m_doc->currentTrackRemap());
    }

  private:
    SongDocument *m_doc;
    SongCfg m_new;
    SongCfg m_old;
};

// A note move that may merge with the next one (keyboard transpose/nudge —
// rapid presses form one gesture). Merging first reverts both commands,
// which restores any neighbor the intermediate position had trimmed via
// resolveNoteOverlaps, then re-lands the accumulated delta from the
// gesture's ORIGINAL notes — only the final resting position decides what
// gets trimmed. QUndoStack refuses to merge across its clean index, so a
// save between presses keeps its own command.
class MoveNotesCommand : public QUndoCommand
{
  public:
    MoveNotesCommand(SongDocument *doc, std::vector<DocNote> notes, int64_t dTick, int dKey,
                     bool mergeable)
        : QUndoCommand(SongDocument::tr("move %n note(s)", nullptr, int(notes.size())))
        , m_doc(doc)
        , m_notes(std::move(notes))
        , m_dTick(dTick)
        , m_dKey(dKey)
        , m_mergeable(mergeable)
        , m_ops(doc->buildMoveNotesOps(m_notes, dTick, dKey))
    {}

    int id() const override { return m_mergeable ? 0x4d76 : -1; } // 'Mv'

    void redo() override
    {
        const auto before = m_doc->trackMapState();
        m_doc->applyOps(m_ops);
        m_doc->rebuildTrackMap();
        m_remap = m_doc->trackRemap(before, m_ops);
        // The push-time redo publishes nothing: a merge right after it may
        // replace (or discard) this provisional state, so moveNotes
        // publishes once after the stack settles. Real redos publish here.
        if (m_initialRedo) {
            m_initialRedo = false;
            return;
        }
        m_doc->publishMutation(m_remap);
    }

    void undo() override
    {
        m_doc->revertOps(m_ops);
        m_doc->rebuildTrackMap();
        m_doc->publishMutation(inverseTrackRemap(m_remap));
    }

    bool mergeWith(const QUndoCommand *command) override
    {
        // id() matched, so the cast is safe; on success the stack deletes
        // the other command, so mutating it is fine.
        auto *other =
            const_cast<MoveNotesCommand *>(static_cast<const MoveNotesCommand *>(command));
        if (!other->m_mergeable || !movesMyOutputs(other->m_notes))
            return false;
        // Both commands are applied here (the stack redoes the new one
        // before offering the merge). Rewind to the pre-gesture state, then
        // land the accumulated move in one hop.
        m_doc->revertOps(other->m_ops);
        m_doc->revertOps(m_ops);
        m_dTick += other->m_dTick;
        m_dKey += other->m_dKey;
        m_ops = m_doc->buildMoveNotesOps(m_notes, m_dTick, m_dKey);
        // The accumulated move may return every note to the gesture's
        // start (clamping included); the document already sits there after
        // the rewind, so leave it and let the stack drop this command —
        // moving away and back leaves no undo entry. The rewind can still
        // have reordered a mixed-channel chunk's first channel event, so
        // the map must rebuild like every other apply/revert path.
        if (movesMyOutputs(m_notes)) {
            m_doc->rebuildTrackMap();
            setObsolete(true);
            return true;
        }
        m_doc->applyOps(m_ops);
        m_doc->rebuildTrackMap();
        m_remap = m_doc->currentTrackRemap();
        return true;
    }

  private:
    // The next press must edit the notes — identity included — exactly
    // where this command left them; anything else (new selection, a
    // DIFFERENT note sitting where this one landed) is a separate gesture.
    // Keyed by noteId so two indistinguishable notes crossing each other
    // never merge into one command.
    bool movesMyOutputs(const std::vector<DocNote> &next) const
    {
        if (next.size() != m_notes.size())
            return false;
        std::map<NoteId, const DocNote *> byId;
        for (const DocNote &n : next)
            byId.emplace(n.noteId, &n);
        for (const DocNote &n : m_notes) {
            const auto match = byId.find(n.noteId);
            if (match == byId.end())
                return false;
            const DocNote &cand = *match->second;
            if (cand.engineTrack != n.engineTrack || cand.tick != movedNoteTick(n, m_dTick) ||
                cand.key != movedNoteKey(n, m_dKey) || cand.duration != n.duration ||
                cand.velocity != n.velocity || cand.channel != n.channel)
                return false;
        }
        return true;
    }

    SongDocument *m_doc;
    std::vector<DocNote> m_notes; // resolved against the pre-gesture state
    int64_t m_dTick;
    int m_dKey;
    bool m_mergeable;
    bool m_initialRedo = true;
    std::vector<SongDocument::EditOp> m_ops;
    TrackRemap m_remap;
};

SongDocument::SongDocument(QObject *parent) : QObject(parent) {}

bool SongDocument::load(const SongInfo &song, QString *error)
{
    SmfFile smf;
    // readFile coerces format 0 to format 1 at the parse layer, so every
    // edit path below deals in one shape: chunks are tracks. Deterministic,
    // so an untouched file re-converts identically next open; the disk copy
    // flips to format 1 on the first real edit + save.
    if (!SmfFile::readFile(song.midPath, &smf, error))
        return false;

    m_smf = std::move(smf);
    m_cfg = song.cfg;
    m_savedCfg = song.cfg;
    m_midPath = song.midPath;
    m_label = song.label;
    m_hadCfgLine = song.hasCfg;
    m_undoStack.clear();
    mintUnassignedNoteIds();
    rebuildTrackMap();
    // A (re)load replaces the whole document: bump the revision so anything
    // resolved against the previous contents (a pending setNotesVelocities,
    // stale DocNotes) can never match again. Deliberately NOT published:
    // MainWindow reloads a session's document in place and detaches the
    // views/audio glue only afterwards, so a synchronous documentChanged
    // here would re-enter them mid-swap; the caller re-attaches and
    // refreshes everything itself after load returns.
    m_revision++;
    return true;
}

bool SongDocument::save(QString *error)
{
    if (!m_smf.writeFile(m_midPath, error))
        return false;

    if (!cfgSemanticEqual(m_cfg, m_savedCfg) || !m_hadCfgLine) {
        const QStringList flags = SongRegistry::mergeCfgFlags(m_cfg);
        m_cfg.rawFlags = flags;
        if (!SongRegistry::writeSongFlags(QFileInfo(m_midPath).path(), m_label, flags, error))
            return false;
        m_savedCfg = m_cfg;
        m_hadCfgLine = true;
    }

    m_undoStack.setClean();
    return true;
}

uint32_t SongDocument::ticksPerClock() const
{
    const uint32_t clocksPerBeat = 24 * (m_cfg.extendedClocks ? 2 : 1);
    return std::max<uint32_t>(1, m_smf.division / clocksPerBeat);
}

void SongDocument::rebuildTrackMap()
{
    m_engineToSmf.clear();
    m_engineChannel.clear();
    for (size_t t = 0; t < m_smf.tracks.size() && m_engineToSmf.size() < 16; t++) {
        for (const SmfEvent &ev : m_smf.tracks[t].events) {
            if (ev.isChannel()) {
                m_engineToSmf.push_back(int(t));
                m_engineChannel.push_back(ev.channel());
                break;
            }
        }
    }
}

SongDocument::TrackMapState SongDocument::trackMapState() const
{
    return {int(m_smf.tracks.size()), m_engineToSmf};
}

TrackRemap SongDocument::currentTrackRemap() const
{
    TrackRemap remap;
    remap.smfTrackMap.resize(m_smf.tracks.size());
    remap.engineTrackMap.resize(size_t(engineTrackCount()));
    std::iota(remap.smfTrackMap.begin(), remap.smfTrackMap.end(), 0);
    std::iota(remap.engineTrackMap.begin(), remap.engineTrackMap.end(), 0);
    remap.newSmfTrackCount = int(m_smf.tracks.size());
    remap.newEngineTrackCount = engineTrackCount();
    return remap;
}

TrackRemap SongDocument::trackRemap(const TrackMapState &before,
                                    const std::vector<EditOp> &ops) const
{
    // Replay just the chunk-structure ops over the old chunk list to learn
    // where each pre-mutation chunk landed (event-level ops never move a
    // chunk, but they can still flip a chunk's ENGINE status — that part is
    // diffed against the rebuilt map below).
    std::vector<int> chunkOrigins(size_t(before.smfTrackCount));
    std::iota(chunkOrigins.begin(), chunkOrigins.end(), 0);
    for (const EditOp &op : ops) {
        if (op.type == EditOp::InsertTrack) {
            chunkOrigins.insert(chunkOrigins.begin() + op.smfTrack, -1);
        } else if (op.type == EditOp::RemoveTrack) {
            chunkOrigins.erase(chunkOrigins.begin() + op.smfTrack);
        } else if (op.type == EditOp::MoveTrack) {
            moveChunk(chunkOrigins, op.smfTrack, op.smfTrackTo);
        }
    }
    TrackRemap remap;
    remap.smfTrackMap.assign(size_t(before.smfTrackCount), -1);
    remap.engineTrackMap.assign(before.engineToSmf.size(), -1);
    remap.newSmfTrackCount = int(m_smf.tracks.size());
    remap.newEngineTrackCount = engineTrackCount();
    for (int current = 0; current < int(chunkOrigins.size()); current++) {
        if (chunkOrigins[current] >= 0)
            remap.smfTrackMap[chunkOrigins[current]] = current;
    }
    for (int oldEngine = 0; oldEngine < int(before.engineToSmf.size()); oldEngine++) {
        const int newChunk = remap.smfTrackMap[before.engineToSmf[oldEngine]];
        if (newChunk >= 0)
            remap.engineTrackMap[oldEngine] = engineTrackForChunk(newChunk);
    }
    return remap;
}

void SongDocument::publishMutation(TrackRemap remap)
{
    m_revision++;
    if (!remap.isIdentity())
        emit tracksRemapped(std::move(remap));
    emit documentChanged();
}

void SongDocument::mintNoteId(SmfEvent *event)
{
    if (!event->isNoteOn() || event->noteId.isAssigned())
        return;
    event->noteId = NoteId{m_nextNoteId++};
}

void SongDocument::mintUnassignedNoteIds()
{
    for (SmfTrack &track : m_smf.tracks) {
        for (SmfEvent &ev : track.events)
            mintNoteId(&ev);
    }
}

int SongDocument::smfTrackFor(int engineTrack) const
{
    if (engineTrack < 0 || engineTrack >= int(m_engineToSmf.size()))
        return -1;
    return m_engineToSmf[engineTrack];
}

uint8_t SongDocument::channelFor(int engineTrack) const
{
    if (engineTrack < 0 || engineTrack >= int(m_engineChannel.size()))
        return 0;
    return m_engineChannel[engineTrack];
}

int SongDocument::engineTrackForChunk(int chunk) const
{
    for (int t = 0; t < int(m_engineToSmf.size()); t++) {
        if (m_engineToSmf[t] == chunk)
            return t;
    }
    return -1;
}

std::vector<DocNote> SongDocument::notesForTrack(int engineTrack) const
{
    std::vector<DocNote> notes;
    const int smfTrack = smfTrackFor(engineTrack);
    if (smfTrack < 0)
        return notes;
    const auto &evs = m_smf.tracks[smfTrack].events;

    // Pair as mid2agb does: the first same-channel same-key note end after
    // the note-on (several note-ons may legitimately share one end). One
    // backward pass keeps that exact rule in linear time: when the walk
    // reaches a note-on, endAt holds the smallest later end index for its
    // (channel, key) slot.
    // 256 key slots, not 128: the parse layer preserves out-of-range data
    // bytes (mid2agb parity), and pairing compares the raw key byte, so key
    // 0x83 must never pair with key 0x03.
    std::vector<size_t> endAt(16 * 256, SIZE_MAX);
    for (size_t i = evs.size(); i-- > 0;) {
        const SmfEvent &ev = evs[i];
        if (!ev.isChannel())
            continue;
        const size_t slot = size_t(ev.channel()) * 256 + ev.data0;
        if (ev.isNoteEnd()) {
            endAt[slot] = i;
        } else if (ev.isNoteOn()) {
            DocNote note;
            note.noteId = ev.noteId;
            note.engineTrack = engineTrack;
            note.smfTrack = smfTrack;
            note.onIndex = i;
            note.tick = ev.tick;
            note.key = ev.data0;
            note.velocity = ev.data1;
            note.channel = ev.channel();
            if (endAt[slot] != SIZE_MAX) {
                note.endIndex = endAt[slot];
                note.duration = uint32_t(evs[endAt[slot]].tick - ev.tick);
            }
            notes.push_back(note);
        }
    }
    std::reverse(notes.begin(), notes.end()); // restore note-on order
    return notes;
}

bool SongDocument::findNote(int engineTrack, uint64_t tick, uint8_t key, DocNote *out) const
{
    for (const DocNote &note : notesForTrack(engineTrack)) {
        if (note.tick == tick && note.key == key) {
            *out = note;
            return true;
        }
    }
    return false;
}

bool SongDocument::findNote(NoteId id, DocNote *out) const
{
    if (!id.isAssigned())
        return false;
    for (int t = 0; t < engineTrackCount(); t++) {
        for (const DocNote &note : notesForTrack(t)) {
            if (note.noteId == id) {
                *out = note;
                return true;
            }
        }
    }
    return false;
}

bool SongDocument::laneEventMatches(const SmfEvent &ev, uint8_t cc) const
{
    if (!ev.isChannel())
        return false;
    if (cc == DOC_CC_BEND)
        return ev.typeNibble() == 0xE;
    if (cc == DOC_CC_VOICE)
        return ev.typeNibble() == 0xC;
    return ev.typeNibble() == 0xB && ev.data0 == cc;
}

int SongDocument::laneValue(const SmfEvent &ev, uint8_t cc) const
{
    if (cc == DOC_CC_TEMPO) {
        const uint8_t *p = reinterpret_cast<const uint8_t *>(ev.blob.constData());
        const uint32_t usPerBeat = (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
        return int(60000000.0 / double(usPerBeat) + 0.5);
    }
    if (cc == DOC_CC_BEND)
        return ((int(ev.data1) << 7) | ev.data0) - 8192;
    if (cc == DOC_CC_VOICE)
        return ev.data0;
    return ev.data1;
}

std::vector<DocLanePoint> SongDocument::lanePoints(int engineTrack, uint8_t cc) const
{
    std::vector<DocLanePoint> points;
    if (cc == DOC_CC_TEMPO) {
        // Tempo lives in the first chunk: mid2agb reads seq events only there.
        if (m_smf.tracks.empty())
            return points;
        const auto &evs = m_smf.tracks[0].events;
        for (size_t i = 0; i < evs.size(); i++) {
            if (metaIsTempo(evs[i]))
                points.push_back({0, i, evs[i].tick, laneValue(evs[i], cc)});
        }
        return points;
    }

    const int smfTrack = smfTrackFor(engineTrack);
    if (smfTrack < 0)
        return points;
    const auto &evs = m_smf.tracks[smfTrack].events;
    for (size_t i = 0; i < evs.size(); i++) {
        if (laneEventMatches(evs[i], cc))
            points.push_back({smfTrack, i, evs[i].tick, laneValue(evs[i], cc)});
    }
    return points;
}

bool SongDocument::findLanePoint(int engineTrack, uint8_t cc, uint64_t tick,
                                 DocLanePoint *out) const
{
    // The LAST point at the tick, mirroring setTimeSig: playback applies a
    // tick's events in order, so among same-tick duplicates the last is the
    // audible one — edits must target it, not a shadowed earlier value.
    bool found = false;
    for (const DocLanePoint &pt : lanePoints(engineTrack, cc)) {
        if (pt.tick > tick)
            break;
        if (pt.tick == tick) {
            if (out)
                *out = pt;
            found = true;
        }
    }
    return found;
}

bool SongDocument::findLoopMarkerEvent(bool endMarker, int *smfTrack, size_t *index) const
{
    const char marker = endMarker ? ']' : '[';
    for (size_t t = 0; t < m_smf.tracks.size(); t++) {
        // Mirror MidiTimeline::build: name metas — the chunk's name (first
        // unprefixed 0x03, marker text included) and channel-scoped
        // (prefixed) non-marker 0x03s — are never checked as loop markers;
        // a prefixed 0x03 carrying marker text has no name position, so it
        // IS one (mid2agb's reading).
        bool nameSeen = false;
        SmfChannelPrefix prefix;
        const auto &evs = m_smf.tracks[t].events;
        for (size_t i = 0; i < evs.size(); i++) {
            const SmfEvent &ev = evs[i];
            prefix.observe(ev);
            if (!ev.isMeta())
                continue;
            if (ev.metaType == 0x03) {
                if (prefix.channel >= 0) {
                    if (!smfMetaIsMarker(ev))
                        continue; // channel-scoped name, not this chunk's
                } else if (!nameSeen) {
                    nameSeen = true;
                    continue;
                }
            }
            if (metaIsLoopMarker(ev, marker)) {
                *smfTrack = int(t);
                *index = i;
                return true;
            }
        }
    }
    return false;
}

uint64_t SongDocument::loopTick(bool endMarker) const
{
    int track;
    size_t index;
    if (!findLoopMarkerEvent(endMarker, &track, &index))
        return UINT64_MAX;
    return m_smf.tracks[track].events[index].tick;
}

std::vector<DocTimeSig> SongDocument::timeSigs() const
{
    std::vector<DocTimeSig> sigs;
    for (size_t t = 0; t < m_smf.tracks.size(); t++) {
        const auto &evs = m_smf.tracks[t].events;
        for (size_t i = 0; i < evs.size(); i++) {
            if (metaIsTimeSig(evs[i]))
                sigs.push_back(
                    {int(t), i, evs[i].tick, uint8_t(evs[i].blob[0]), uint8_t(evs[i].blob[1])});
        }
    }
    std::stable_sort(sigs.begin(), sigs.end(),
                     [](const DocTimeSig &a, const DocTimeSig &b) { return a.tick < b.tick; });
    return sigs;
}

SmfEvent SongDocument::makeChannelEvent(uint8_t typeNibble, uint8_t channel, uint64_t tick,
                                        uint8_t data0, uint8_t data1) const
{
    SmfEvent ev;
    ev.tick = tick;
    ev.status = uint8_t((typeNibble << 4) | (channel & 0x0F));
    ev.data0 = data0;
    ev.data1 = data1;
    return ev;
}

void SongDocument::appendNoteInsertOps(std::vector<EditOp> &ops, int smfTrack, uint8_t channel,
                                       uint64_t tick, uint8_t key, uint32_t duration,
                                       uint8_t velocity) const
{
    EditOp on;
    on.type = EditOp::InsertEvent;
    on.smfTrack = smfTrack;
    on.event = makeChannelEvent(0x9, channel, tick, key, velocity);
    ops.push_back(on);

    // Note ends are written as velocity-0 note-ons: the form mid2agb's own
    // ecosystem uses, and the one that keeps running status unbroken.
    EditOp end;
    end.type = EditOp::InsertEvent;
    end.smfTrack = smfTrack;
    end.event = makeChannelEvent(0x9, channel, tick + std::max<uint32_t>(1, duration), key, 0);
    ops.push_back(end);
}

void SongDocument::appendRemoveOps(std::vector<EditOp> &ops, int smfTrack,
                                   std::vector<size_t> indices) const
{
    std::sort(indices.begin(), indices.end(), std::greater<size_t>());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    for (size_t index : indices) {
        EditOp op;
        op.type = EditOp::RemoveEvent;
        op.smfTrack = smfTrack;
        op.index = index;
        ops.push_back(op);
    }
}

namespace {
// Two lane events of the same lane: same tempo meta, or the same channel
// status (type + channel) and, for controllers, the same controller.
bool sameLane(const SmfEvent &a, const SmfEvent &b)
{
    if (metaIsTempo(a) || metaIsTempo(b))
        return metaIsTempo(a) && metaIsTempo(b);
    if (!a.isChannel() || !b.isChannel() || a.status != b.status)
        return false;
    return a.typeNibble() != 0xB || a.data0 == b.data0;
}
} // namespace

void SongDocument::appendLaneLandingRemovals(const std::vector<DocLanePoint> &points, int64_t dTick,
                                             std::vector<std::vector<size_t>> &removals) const
{
    for (const DocLanePoint &pt : points) {
        if (pt.smfTrack < 0 || pt.smfTrack >= int(m_smf.tracks.size()))
            continue;
        const std::vector<SmfEvent> &events = m_smf.tracks[size_t(pt.smfTrack)].events;
        if (pt.index >= events.size())
            continue;
        const SmfEvent &source = events[pt.index];
        const uint64_t dest = uint64_t(std::max<int64_t>(0, int64_t(pt.tick) + dTick));
        auto it = std::lower_bound(events.begin(), events.end(), dest,
                                   [](const SmfEvent &ev, uint64_t t) { return ev.tick < t; });
        for (; it != events.end() && it->tick == dest; ++it) {
            const size_t index = size_t(it - events.begin());
            if (index != pt.index && sameLane(*it, source))
                removals[size_t(pt.smfTrack)].push_back(index);
        }
    }
}

void SongDocument::appendEventEditOps(std::vector<EditOp> &ops, int smfTrack, size_t index,
                                      const SmfEvent &event) const
{
    // A note-on replacing a note-on is the same note: carry its identity
    // (the raw editor hands in freshly built events with no id).
    const SmfEvent &old = m_smf.tracks[smfTrack].events[index];
    SmfEvent replacement = event;
    if (old.isNoteOn() && replacement.isNoteOn())
        replacement.noteId = old.noteId;
    if (replacement.tick == old.tick) {
        EditOp op;
        op.type = EditOp::ModifyEvent;
        op.smfTrack = smfTrack;
        op.index = index;
        op.event = std::move(replacement);
        ops.push_back(std::move(op));
        return;
    }
    EditOp remove;
    remove.type = EditOp::RemoveEvent;
    remove.smfTrack = smfTrack;
    remove.index = index;
    ops.push_back(remove);

    EditOp insert;
    insert.type = EditOp::InsertEvent;
    insert.smfTrack = smfTrack;
    insert.preservesNoteId = old.isNoteOn() && replacement.isNoteOn();
    insert.event = std::move(replacement);
    ops.push_back(std::move(insert));
}

void SongDocument::resolveNoteOverlaps(const std::vector<PlannedNote> &written,
                                       const std::vector<DocNote> &editNotes,
                                       std::vector<std::vector<size_t>> &removals,
                                       std::vector<EditOp> &trims) const
{
    if (written.empty())
        return;
    std::vector<PlannedNote> spans = written;
    std::sort(spans.begin(), spans.end(),
              [](const PlannedNote &a, const PlannedNote &b) { return a.tick < b.tick; });
    std::vector<int> tracks;
    for (const PlannedNote &w : spans) {
        if (std::find(tracks.begin(), tracks.end(), w.engineTrack) == tracks.end())
            tracks.push_back(w.engineTrack);
    }
    const auto isEdited = [&](const DocNote &s) {
        for (const DocNote &n : editNotes) {
            if (n.smfTrack == s.smfTrack && n.onIndex == s.onIndex)
                return true;
        }
        return false;
    };
    for (int t : tracks) {
        for (const DocNote &s : notesForTrack(t)) {
            // An unterminated note has no end to trim; it stays as-is.
            if (s.unterminated() || isEdited(s))
                continue;
            uint64_t sTick = s.tick;
            uint64_t sEnd = s.tick + s.duration;
            bool covered = false, trimEnd = false, trimLeft = false;
            for (const PlannedNote &w : spans) {
                if (w.engineTrack != t || w.key != s.key || w.endTick <= sTick || w.tick >= sEnd)
                    continue;
                if (sTick < w.tick) {
                    // Head survives (a strictly containing note keeps only
                    // its head — no splitting). Later spans start at or past
                    // the new end, so this settles the note.
                    sEnd = w.tick;
                    trimEnd = true;
                    break;
                }
                if (sEnd > w.endTick) {
                    // Tail survives; it may still hit a later span.
                    sTick = w.endTick;
                    trimLeft = true;
                    continue;
                }
                covered = true;
                break;
            }
            if (covered) {
                removals[size_t(s.smfTrack)].push_back(s.onIndex);
                removals[size_t(s.smfTrack)].push_back(s.endIndex);
                continue;
            }
            if (trimLeft) {
                removals[size_t(s.smfTrack)].push_back(s.onIndex);
                EditOp op;
                op.type = EditOp::InsertEvent;
                op.smfTrack = s.smfTrack;
                op.event = m_smf.tracks[size_t(s.smfTrack)].events[s.onIndex];
                op.event.tick = sTick;
                op.preservesNoteId = true; // a trimmed note is the same note
                trims.push_back(op);
            }
            if (trimEnd) {
                removals[size_t(s.smfTrack)].push_back(s.endIndex);
                EditOp op;
                op.type = EditOp::InsertEvent;
                op.smfTrack = s.smfTrack;
                op.event = m_smf.tracks[size_t(s.smfTrack)].events[s.endIndex];
                op.event.tick = sEnd;
                trims.push_back(op);
            }
        }
    }
}

void SongDocument::addNote(int engineTrack, uint64_t tick, uint8_t key, uint32_t duration,
                           uint8_t velocity)
{
    const int smfTrack = smfTrackFor(engineTrack);
    if (smfTrack < 0)
        return;
    std::vector<std::vector<size_t>> removals(m_smf.tracks.size());
    std::vector<EditOp> trims;
    resolveNoteOverlaps({{engineTrack, key, tick, tick + std::max<uint32_t>(1, duration)}}, {},
                        removals, trims);
    std::vector<EditOp> ops;
    for (size_t t = 0; t < m_smf.tracks.size(); t++)
        appendRemoveOps(ops, int(t), std::move(removals[t]));
    appendNoteInsertOps(ops, smfTrack, channelFor(engineTrack), tick, key, duration, velocity);
    ops.insert(ops.end(), trims.begin(), trims.end());
    pushEdit(tr("add note"), std::move(ops));
}

void SongDocument::addNotes(int engineTrack, const std::vector<NewNote> &notes)
{
    const int smfTrack = smfTrackFor(engineTrack);
    if (smfTrack < 0 || notes.empty())
        return;
    std::vector<std::vector<size_t>> removals(m_smf.tracks.size());
    std::vector<PlannedNote> written;
    for (const NewNote &note : notes)
        written.push_back(
            {engineTrack, note.key, note.tick, note.tick + std::max<uint32_t>(1, note.duration)});
    std::vector<EditOp> trims;
    resolveNoteOverlaps(written, {}, removals, trims);
    const uint8_t channel = channelFor(engineTrack);
    std::vector<EditOp> ops;
    for (size_t t = 0; t < m_smf.tracks.size(); t++)
        appendRemoveOps(ops, int(t), std::move(removals[t]));
    for (const NewNote &note : notes)
        appendNoteInsertOps(ops, smfTrack, channel, note.tick, note.key, note.duration,
                            note.velocity);
    ops.insert(ops.end(), trims.begin(), trims.end());
    pushEdit(tr("add %n note(s)", nullptr, int(notes.size())), std::move(ops));
}

void SongDocument::deleteNotes(const std::vector<DocNote> &notes)
{
    if (notes.empty())
        return;
    // Group removal indices per SMF track so each track's removals apply in
    // descending order.
    std::vector<EditOp> ops;
    for (size_t t = 0; t < m_smf.tracks.size(); t++) {
        std::vector<size_t> indices;
        for (const DocNote &note : notes) {
            if (note.smfTrack != int(t))
                continue;
            indices.push_back(note.onIndex);
            if (!note.unterminated())
                indices.push_back(note.endIndex);
        }
        appendRemoveOps(ops, int(t), std::move(indices));
    }
    pushEdit(tr("delete %n note(s)", nullptr, int(notes.size())), std::move(ops));
}

void SongDocument::moveNotes(const std::vector<DocNote> &notes, int64_t dTick, int dKey,
                             bool mergeable)
{
    if (notes.empty() || (dTick == 0 && dKey == 0))
        return;
    // A delta the clamps swallow entirely (every note pinned at tick 0 or a
    // key rail) changes nothing; don't pollute the undo stack with it.
    const bool changes = std::any_of(notes.begin(), notes.end(), [&](const DocNote &n) {
        return movedNoteTick(n, dTick) != n.tick || movedNoteKey(n, dKey) != n.key;
    });
    if (!changes)
        return;
    m_undoStack.push(new MoveNotesCommand(this, notes, dTick, dKey, mergeable));
    // The command's push-time redo deferred publication (a merge can still
    // replace that provisional state, and an inverse merge just removed the
    // command while restoring live state). The stack has settled: publish
    // the one mutation this call made.
    publishMutation(currentTrackRemap());
}

std::vector<SongDocument::EditOp> SongDocument::buildMoveNotesOps(const std::vector<DocNote> &notes,
                                                                  int64_t dTick, int dKey) const
{
    std::vector<std::vector<size_t>> removals(m_smf.tracks.size());
    std::vector<PlannedNote> written;
    for (const DocNote &note : notes) {
        if (note.smfTrack < 0 || note.smfTrack >= int(removals.size()))
            continue;
        removals[size_t(note.smfTrack)].push_back(note.onIndex);
        if (note.unterminated())
            continue;
        removals[size_t(note.smfTrack)].push_back(note.endIndex);
        const uint64_t newTick = movedNoteTick(note, dTick);
        written.push_back(
            {note.engineTrack, movedNoteKey(note, dKey), newTick, newTick + note.duration});
    }
    std::vector<EditOp> trims;
    resolveNoteOverlaps(written, notes, removals, trims);
    std::vector<EditOp> ops;
    for (size_t t = 0; t < m_smf.tracks.size(); t++)
        appendRemoveOps(ops, int(t), std::move(removals[t]));
    for (const DocNote &note : notes) {
        if (note.smfTrack < 0 || note.smfTrack >= int(m_smf.tracks.size()))
            continue;
        const uint64_t newTick = movedNoteTick(note, dTick);
        const uint8_t newKey = movedNoteKey(note, dKey);
        // Re-insert the note's OWN events with the tick/key adjusted, not
        // synthesized replacements: every other byte (status form, an end's
        // release velocity, the NoteId) survives the move.
        EditOp on;
        on.type = EditOp::InsertEvent;
        on.smfTrack = note.smfTrack;
        on.event = m_smf.tracks[size_t(note.smfTrack)].events[note.onIndex];
        on.event.tick = newTick;
        on.event.data0 = newKey;
        on.preservesNoteId = true;
        ops.push_back(std::move(on));
        if (!note.unterminated()) {
            EditOp end;
            end.type = EditOp::InsertEvent;
            end.smfTrack = note.smfTrack;
            end.event = m_smf.tracks[size_t(note.smfTrack)].events[note.endIndex];
            end.event.tick = newTick + note.duration;
            end.event.data0 = newKey;
            ops.push_back(std::move(end));
        }
    }
    ops.insert(ops.end(), trims.begin(), trims.end());
    return ops;
}

void SongDocument::resizeNotes(const std::vector<DocNote> &notes, int64_t dDuration)
{
    if (notes.empty() || dDuration == 0)
        return;
    // All durations already pinned at the 1-tick floor: nothing to undo.
    const bool changes = std::any_of(notes.begin(), notes.end(), [dDuration](const DocNote &n) {
        return n.unterminated() || resizedNoteDuration(n, dDuration) != n.duration;
    });
    if (!changes)
        return;
    std::vector<std::vector<size_t>> removals(m_smf.tracks.size());
    std::vector<PlannedNote> written;
    for (const DocNote &note : notes) {
        if (note.unterminated() || note.smfTrack < 0 || note.smfTrack >= int(removals.size()))
            continue;
        removals[size_t(note.smfTrack)].push_back(note.endIndex);
        written.push_back({note.engineTrack, note.key, note.tick,
                           note.tick + resizedNoteDuration(note, dDuration)});
    }
    std::vector<EditOp> trims;
    resolveNoteOverlaps(written, notes, removals, trims);
    std::vector<EditOp> ops;
    for (size_t t = 0; t < m_smf.tracks.size(); t++)
        appendRemoveOps(ops, int(t), std::move(removals[t]));
    for (const DocNote &note : notes) {
        if (note.smfTrack < 0 || note.smfTrack >= int(m_smf.tracks.size()))
            continue;
        const uint32_t newDuration = resizedNoteDuration(note, dDuration);
        EditOp end;
        end.type = EditOp::InsertEvent;
        end.smfTrack = note.smfTrack;
        if (note.unterminated()) {
            // Growing an unterminated note gives it a real end.
            end.event = makeChannelEvent(0x9, note.channel, note.tick + newDuration, note.key, 0);
        } else {
            // The note's own end event, moved (see buildMoveNotesOps): its
            // status form and release velocity survive the resize.
            end.event = m_smf.tracks[size_t(note.smfTrack)].events[note.endIndex];
            end.event.tick = note.tick + newDuration;
        }
        ops.push_back(std::move(end));
    }
    ops.insert(ops.end(), trims.begin(), trims.end());
    pushEdit(tr("resize %n note(s)", nullptr, int(notes.size())), std::move(ops));
}

void SongDocument::resizeNotesLeft(const std::vector<DocNote> &notes, int64_t dTick)
{
    if (notes.empty() || dTick == 0)
        return;
    // Every note-on already pinned by its clamp (tick 0, or one tick short
    // of its end): nothing to undo.
    const bool changes = std::any_of(notes.begin(), notes.end(), [dTick](const DocNote &n) {
        return leftResizedNoteTick(n, dTick) != n.tick;
    });
    if (!changes)
        return;
    std::vector<std::vector<size_t>> removals(m_smf.tracks.size());
    std::vector<PlannedNote> written;
    for (const DocNote &note : notes) {
        if (note.smfTrack < 0 || note.smfTrack >= int(removals.size()))
            continue;
        removals[size_t(note.smfTrack)].push_back(note.onIndex);
        if (note.unterminated())
            continue;
        written.push_back({note.engineTrack, note.key, leftResizedNoteTick(note, dTick),
                           note.tick + note.duration});
    }
    std::vector<EditOp> trims;
    resolveNoteOverlaps(written, notes, removals, trims);
    std::vector<EditOp> ops;
    for (size_t t = 0; t < m_smf.tracks.size(); t++)
        appendRemoveOps(ops, int(t), std::move(removals[t]));
    for (const DocNote &note : notes) {
        if (note.smfTrack < 0 || note.smfTrack >= int(m_smf.tracks.size()))
            continue;
        const uint64_t newTick = leftResizedNoteTick(note, dTick);
        // The note's own note-on, moved — not a synthesized twin — so its
        // other bytes and its NoteId survive (see buildMoveNotesOps).
        EditOp on;
        on.type = EditOp::InsertEvent;
        on.smfTrack = note.smfTrack;
        on.event = m_smf.tracks[size_t(note.smfTrack)].events[note.onIndex];
        on.event.tick = newTick;
        on.preservesNoteId = true;
        ops.push_back(std::move(on));
    }
    ops.insert(ops.end(), trims.begin(), trims.end());
    pushEdit(tr("resize %n note(s)", nullptr, int(notes.size())), std::move(ops));
}

void SongDocument::setNotesVelocity(const std::vector<DocNote> &notes, uint8_t velocity)
{
    if (notes.empty())
        return;
    const uint8_t target = std::clamp<uint8_t>(velocity, 1, 127);
    std::vector<EditOp> ops;
    for (const DocNote &note : notes) {
        // Already there: no edit for this note (an all-skipped call pushes
        // no undo command at all — pushEdit drops an empty op list).
        if (note.velocity == target)
            continue;
        EditOp op;
        op.type = EditOp::ModifyEvent;
        op.smfTrack = note.smfTrack;
        op.index = note.onIndex;
        op.event = makeChannelEvent(0x9, note.channel, note.tick, note.key, target);
        ops.push_back(op);
    }
    pushEdit(tr("set velocity"), std::move(ops));
}

std::optional<uint64_t>
SongDocument::setNotesVelocities(uint64_t expectedRevision,
                                 const std::vector<NoteVelocity> &velocities)
{
    if (expectedRevision != m_revision)
        return std::nullopt;
    // One document sweep resolves the whole batch (findNote per entry would
    // rescan every chunk per note — this is the velocity paint's commit
    // path). All-or-nothing: one unresolvable id refuses the whole write; a
    // later entry for a note overrides an earlier one (a paint gesture
    // reports its latest value last).
    std::map<NoteId, DocNote> notesById;
    for (int t = 0; t < engineTrackCount(); t++) {
        for (const DocNote &note : notesForTrack(t))
            notesById.emplace(note.noteId, note);
    }
    std::map<NoteId, int> targets;
    for (const NoteVelocity &nv : velocities) {
        if (!notesById.count(nv.noteId))
            return std::nullopt;
        targets[nv.noteId] = nv.velocity;
    }
    std::vector<EditOp> ops;
    for (const auto &entry : targets) {
        const DocNote &note = notesById[entry.first];
        const uint8_t target = uint8_t(std::clamp(entry.second, 1, 127));
        if (note.velocity == target)
            continue;
        // The note's own note-on with just the velocity byte changed, so a
        // foreign event's other bytes survive.
        SmfEvent event = m_smf.tracks[size_t(note.smfTrack)].events[note.onIndex];
        event.data1 = target;
        EditOp op;
        op.type = EditOp::ModifyEvent;
        op.smfTrack = note.smfTrack;
        op.index = note.onIndex;
        op.event = std::move(event);
        ops.push_back(std::move(op));
    }
    if (ops.empty())
        return expectedRevision; // accepted, but nothing changed
    pushEdit(tr("paint note velocities"), std::move(ops));
    return m_revision;
}

void SongDocument::nudgeNotesVelocity(const std::vector<DocNote> &notes, int delta)
{
    if (notes.empty() || delta == 0)
        return;
    std::vector<EditOp> ops;
    for (const DocNote &note : notes) {
        const uint8_t target = uint8_t(std::clamp(int(note.velocity) + delta, 1, 127));
        // Pinned at a clamp rail already: no edit for this note.
        if (note.velocity == target)
            continue;
        EditOp op;
        op.type = EditOp::ModifyEvent;
        op.smfTrack = note.smfTrack;
        op.index = note.onIndex;
        op.event = makeChannelEvent(0x9, note.channel, note.tick, note.key, target);
        ops.push_back(op);
    }
    pushEdit(tr("adjust velocity"), std::move(ops));
}

SmfEvent SongDocument::makeLaneEvent(uint8_t cc, uint8_t channel, uint64_t tick, int value) const
{
    if (cc == DOC_CC_TEMPO) {
        SmfEvent ev;
        ev.tick = tick;
        ev.status = 0xFF;
        ev.metaType = 0x51;
        // kTempoMin keeps usPerBeat inside the meta's 24-bit field (see the
        // header): 4 BPM -> 15,000,000 us, comfortably under 0xFFFFFF.
        const uint32_t usPerBeat =
            uint32_t(60000000.0 / double(std::clamp(value, kTempoMin, kTempoMax)) + 0.5);
        ev.blob.resize(3);
        ev.blob[0] = char((usPerBeat >> 16) & 0xFF);
        ev.blob[1] = char((usPerBeat >> 8) & 0xFF);
        ev.blob[2] = char(usPerBeat & 0xFF);
        return ev;
    }
    if (cc == DOC_CC_BEND) {
        const int bend14 = std::clamp(value, -8192, 8191) + 8192;
        return makeChannelEvent(0xE, channel, tick, uint8_t(bend14 & 0x7F),
                                uint8_t((bend14 >> 7) & 0x7F));
    }
    if (cc == DOC_CC_VOICE)
        return makeChannelEvent(0xC, channel, tick, uint8_t(std::clamp(value, 0, 127)), 0);
    return makeChannelEvent(0xB, channel, tick, cc, uint8_t(std::clamp(value, 0, 127)));
}

void SongDocument::addLanePoint(int engineTrack, uint8_t cc, uint64_t tick, int value)
{
    addLanePoint(cc == DOC_CC_VOICE ? tr("add voice change") : tr("add automation point"),
                 engineTrack, cc, tick, value);
}

int SongDocument::startTempo() const
{
    DocLanePoint pt;
    if (!findLanePoint(-1, DOC_CC_TEMPO, 0, &pt))
        return kTempoDefault;
    // Clamped like the writes: a foreign file's out-of-range meta must not
    // make the read disagree with what setStartTempo would store (or hand
    // the transport spinner a value outside its range).
    return std::clamp(pt.value, kTempoMin, kTempoMax);
}

void SongDocument::setStartTempo(int bpm)
{
    bpm = std::clamp(bpm, kTempoMin, kTempoMax);
    if (m_smf.tracks.empty() || startTempo() == bpm)
        return;
    addLanePoint(tr("set tempo"), -1, DOC_CC_TEMPO, 0, bpm);
}

int SongDocument::tempoChangesAfterStart() const
{
    int count = 0;
    for (const DocLanePoint &pt : lanePoints(-1, DOC_CC_TEMPO))
        if (pt.tick > 0)
            count++;
    return count;
}

int SongDocument::tempoMetasOutsideFirstChunk() const
{
    int count = 0;
    for (size_t t = 1; t < m_smf.tracks.size(); t++)
        for (const SmfEvent &ev : m_smf.tracks[t].events)
            if (ev.isMeta() && ev.metaType == 0x51)
                count++;
    return count;
}

void SongDocument::addLanePoint(const QString &text, int engineTrack, uint8_t cc, uint64_t tick,
                                int value)
{
    const int smfTrack = cc == DOC_CC_TEMPO ? 0 : smfTrackFor(engineTrack);
    if (smfTrack < 0 || m_smf.tracks.empty())
        return;
    std::vector<EditOp> ops;
    // A point already on the tick is replaced, not shadowed: only the last of
    // same-tick duplicates is audible, so keeping the old one would just
    // leave an inert ghost under the new value.
    std::vector<size_t> replaced;
    for (const DocLanePoint &pt : lanePoints(engineTrack, cc)) {
        if (pt.tick == tick)
            replaced.push_back(pt.index);
    }
    appendRemoveOps(ops, smfTrack, std::move(replaced));
    EditOp op;
    op.type = EditOp::InsertEvent;
    op.smfTrack = smfTrack;
    op.event = makeLaneEvent(cc, channelFor(engineTrack), tick, value);
    ops.push_back(op);
    pushEdit(text, std::move(ops));
}

void SongDocument::writeLanePoints(int engineTrack, uint8_t cc, uint64_t tickBegin,
                                   uint64_t tickEnd, const std::vector<LanePointValue> &points)
{
    const int smfTrack = cc == DOC_CC_TEMPO ? 0 : smfTrackFor(engineTrack);
    if (smfTrack < 0 || m_smf.tracks.empty())
        return;
    std::vector<EditOp> ops;
    // Points already inside the swept range are overwritten by the gesture
    // (with an empty stream that IS the gesture: clear the range).
    std::vector<size_t> overwritten;
    for (const DocLanePoint &pt : lanePoints(engineTrack, cc)) {
        if (pt.tick >= tickBegin && pt.tick <= tickEnd)
            overwritten.push_back(pt.index);
    }
    if (overwritten.empty() && points.empty())
        return;
    appendRemoveOps(ops, smfTrack, std::move(overwritten));
    const uint8_t channel = channelFor(engineTrack);
    for (const LanePointValue &pt : points) {
        EditOp op;
        op.type = EditOp::InsertEvent;
        op.smfTrack = smfTrack;
        op.event = makeLaneEvent(cc, channel, pt.tick, pt.value);
        ops.push_back(op);
    }
    pushEdit(tr("draw automation points"), std::move(ops));
}

void SongDocument::moveLanePoints(const std::vector<LanePointMove> &moves)
{
    if (moves.empty() || m_smf.tracks.empty())
        return;
    // One group per (chunk, lane): the shadow map and the converging-move
    // resolution below are per-lane concerns.
    struct LaneGroup {
        int smfTrack = -1;
        int engineTrack = -1;
        uint8_t cc = 0;
        std::map<uint64_t, std::vector<size_t>> pointsByTick;
    };
    struct PlannedMove {
        DocLanePoint point;
        uint64_t newTick = 0;
        SmfEvent sourceEvent;
        SmfEvent event;
        size_t groupIndex = 0;
        uint8_t cc = 0;
    };
    std::vector<LaneGroup> groups;
    std::map<std::pair<int, uint8_t>, size_t> groupByLane;
    std::set<std::pair<int, size_t>> sourceIndices;
    std::vector<PlannedMove> planned;
    planned.reserve(moves.size());
    for (const LanePointMove &move : moves) {
        // Validate each move against the live document and drop the stale
        // ones (a batch caller may hold points resolved before a sibling
        // move landed): the index must still hold this lane's event with
        // the tick and value the caller saw.
        const int smfTrack = move.cc == DOC_CC_TEMPO ? 0 : smfTrackFor(move.engineTrack);
        if (smfTrack < 0 || smfTrack >= int(m_smf.tracks.size()) ||
            move.point.smfTrack != smfTrack ||
            move.point.index >= m_smf.tracks[size_t(smfTrack)].events.size())
            continue;
        const SmfEvent &source = m_smf.tracks[size_t(smfTrack)].events[move.point.index];
        if (move.cc == DOC_CC_TEMPO ? !metaIsTempo(source) : !laneEventMatches(source, move.cc))
            continue;
        if (source.tick != move.point.tick || laneValue(source, move.cc) != move.point.value ||
            !sourceIndices.emplace(smfTrack, move.point.index).second)
            continue;
        const auto key = std::make_pair(smfTrack, move.cc);
        const auto [groupIt, inserted] = groupByLane.emplace(key, groups.size());
        if (inserted)
            groups.push_back({smfTrack, move.engineTrack, move.cc});
        const uint8_t channel = move.cc == DOC_CC_TEMPO ? uint8_t(0) : channelFor(move.engineTrack);
        planned.push_back({move.point, move.newTick, source,
                           makeLaneEvent(move.cc, channel, move.newTick, move.newValue),
                           groupIt->second, move.cc});
    }
    if (planned.empty())
        return;
    // The occupancy map is exactly the lane view: what lanePoints returns
    // for the group is what a landing point may replace.
    for (LaneGroup &group : groups) {
        for (const DocLanePoint &pt : lanePoints(group.engineTrack, group.cc))
            group.pointsByTick[pt.tick].push_back(pt.index);
    }
    // Two moved points converging on one destination tick resolve
    // last-input-wins (only the last of same-tick duplicates is audible, so
    // keeping both would leave an inert ghost — addLanePoint's rule).
    std::map<std::pair<size_t, uint64_t>, size_t> winners;
    for (size_t i = 0; i < planned.size(); i++)
        winners[{planned[i].groupIndex, planned[i].newTick}] = i;
    std::vector<std::set<size_t>> winningSources(groups.size());
    for (const auto &entry : winners)
        winningSources[planned[entry.second].groupIndex].insert(planned[entry.second].point.index);
    std::vector<std::vector<size_t>> removals(m_smf.tracks.size());
    std::vector<EditOp> modifications;
    std::vector<EditOp> insertions;
    for (size_t i = 0; i < planned.size(); i++) {
        const PlannedMove &move = planned[i];
        const LaneGroup &group = groups[move.groupIndex];
        if (winners[{move.groupIndex, move.newTick}] != i) {
            removals[size_t(group.smfTrack)].push_back(move.point.index);
            continue;
        }
        // Landing on an occupied tick replaces what sits there; for a
        // same-tick value edit that heals shadowed duplicates under the
        // edited point. Fellow winners are not shadows — they move away.
        const auto occupied = group.pointsByTick.find(move.newTick);
        if (occupied != group.pointsByTick.end()) {
            for (const size_t index : occupied->second) {
                if (!winningSources[move.groupIndex].count(index))
                    removals[size_t(group.smfTrack)].push_back(index);
            }
        }
        if (move.newTick == move.point.tick) {
            // A same-tick resubmission of the exact value needs no modify op
            // (any shadow healing above still makes the edit real; with no
            // ops at all the batch-level check below drops the whole call).
            if (move.event == move.sourceEvent)
                continue;
            // ModifyEvent so a value edit keeps the point's position within
            // its tick group; a modify shifts no indices, so the removals
            // ordered after it stay valid.
            EditOp modify;
            modify.type = EditOp::ModifyEvent;
            modify.smfTrack = group.smfTrack;
            modify.index = move.point.index;
            modify.event = move.event;
            modifications.push_back(std::move(modify));
        } else {
            removals[size_t(group.smfTrack)].push_back(move.point.index);
            EditOp insert;
            insert.type = EditOp::InsertEvent;
            insert.smfTrack = group.smfTrack;
            insert.event = move.event;
            insertions.push_back(std::move(insert));
        }
    }
    const bool hasRemovals = std::any_of(removals.begin(), removals.end(),
                                         [](const std::vector<size_t> &r) { return !r.empty(); });
    if (modifications.empty() && insertions.empty() && !hasRemovals)
        return;
    std::vector<EditOp> ops;
    ops.reserve(modifications.size() + insertions.size() + moves.size());
    for (EditOp &modify : modifications)
        ops.push_back(std::move(modify));
    for (size_t t = 0; t < m_smf.tracks.size(); t++)
        appendRemoveOps(ops, int(t), std::move(removals[t]));
    for (EditOp &insert : insertions)
        ops.push_back(std::move(insert));
    const bool singular = planned.size() == 1;
    const QString text = singular && planned.front().cc == DOC_CC_VOICE ? tr("change voice")
                         : singular ? tr("edit automation point")
                                    : tr("edit automation points");
    pushEdit(text, std::move(ops));
}

void SongDocument::moveLanePoint(int engineTrack, uint8_t cc, const DocLanePoint &point,
                                 uint64_t newTick, int newValue)
{
    moveLanePoints({LanePointMove{engineTrack, cc, point, newTick, newValue}});
}

void SongDocument::deleteLanePoints(int engineTrack, uint8_t cc,
                                    const std::vector<DocLanePoint> &points)
{
    Q_UNUSED(engineTrack);
    if (points.empty())
        return;
    std::vector<EditOp> ops;
    for (size_t t = 0; t < m_smf.tracks.size(); t++) {
        std::vector<size_t> indices;
        for (const DocLanePoint &pt : points) {
            if (pt.smfTrack == int(t))
                indices.push_back(pt.index);
        }
        appendRemoveOps(ops, int(t), std::move(indices));
    }
    pushEdit(cc == DOC_CC_VOICE ? tr("delete voice change(s)") : tr("delete automation point(s)"),
             std::move(ops));
}

void SongDocument::applyRangeEdit(const QString &text, const RangeEdit &edit)
{
    if (edit.empty() || m_smf.tracks.empty())
        return;
    std::vector<std::vector<size_t>> removals(m_smf.tracks.size());
    for (const DocNote &note : edit.removeNotes) {
        if (note.smfTrack < 0 || note.smfTrack >= int(removals.size()))
            continue;
        removals[size_t(note.smfTrack)].push_back(note.onIndex);
        if (!note.unterminated())
            removals[size_t(note.smfTrack)].push_back(note.endIndex);
    }
    for (const DocLanePoint &pt : edit.removePoints) {
        if (pt.smfTrack >= 0 && pt.smfTrack < int(removals.size()))
            removals[size_t(pt.smfTrack)].push_back(pt.index);
    }
    std::vector<PlannedNote> written;
    for (const RangeEdit::TrackNotes &tn : edit.addNotes) {
        for (const NewNote &note : tn.notes)
            written.push_back({tn.engineTrack, note.key, note.tick,
                               note.tick + std::max<uint32_t>(1, note.duration)});
    }
    std::vector<EditOp> trims;
    resolveNoteOverlaps(written, edit.removeNotes, removals, trims);

    std::vector<EditOp> ops;
    // All removals first (per SMF track, descending — appendRemoveOps sorts
    // and dedups), so every recorded index stays valid at apply time.
    for (size_t t = 0; t < m_smf.tracks.size(); t++)
        appendRemoveOps(ops, int(t), std::move(removals[t]));

    for (const RangeEdit::TrackNotes &tn : edit.addNotes) {
        const int smfTrack = smfTrackFor(tn.engineTrack);
        if (smfTrack < 0)
            continue;
        const uint8_t channel = channelFor(tn.engineTrack);
        for (const NewNote &note : tn.notes)
            appendNoteInsertOps(ops, smfTrack, channel, note.tick, note.key, note.duration,
                                note.velocity);
    }
    for (const RangeEdit::LaneWrite &lw : edit.addPoints) {
        const int smfTrack = lw.cc == DOC_CC_TEMPO ? 0 : smfTrackFor(lw.engineTrack);
        if (smfTrack < 0)
            continue;
        const uint8_t channel = channelFor(lw.engineTrack);
        for (const LanePointValue &pt : lw.points) {
            EditOp op;
            op.type = EditOp::InsertEvent;
            op.smfTrack = smfTrack;
            op.event = makeLaneEvent(lw.cc, channel, pt.tick, pt.value);
            ops.push_back(op);
        }
    }
    ops.insert(ops.end(), trims.begin(), trims.end());
    pushEdit(text, std::move(ops));
}

void SongDocument::moveRange(const std::vector<DocNote> &notes,
                             const std::vector<DocLanePoint> &points, int64_t dTick)
{
    if ((notes.empty() && points.empty()) || dTick == 0 || m_smf.tracks.empty())
        return;
    std::vector<std::vector<size_t>> moved(m_smf.tracks.size());
    const auto mark = [&](int smfTrack, size_t index) {
        if (smfTrack >= 0 && smfTrack < int(moved.size()))
            moved[size_t(smfTrack)].push_back(index);
    };
    for (const DocNote &note : notes) {
        mark(note.smfTrack, note.onIndex);
        if (!note.unterminated())
            mark(note.smfTrack, note.endIndex);
    }
    for (const DocLanePoint &pt : points)
        mark(pt.smfTrack, pt.index);
    // Ascending + deduped so the raw re-inserts below mirror the removals
    // exactly and same-tick events keep their relative order.
    for (std::vector<size_t> &indices : moved) {
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    }
    std::vector<PlannedNote> written;
    for (const DocNote &note : notes) {
        if (note.unterminated())
            continue;
        const uint64_t newTick = uint64_t(std::max<int64_t>(0, int64_t(note.tick) + dTick));
        written.push_back({note.engineTrack, note.key, newTick, newTick + note.duration});
    }
    std::vector<std::vector<size_t>> removals = moved;
    std::vector<EditOp> trims;
    resolveNoteOverlaps(written, notes, removals, trims);
    appendLaneLandingRemovals(points, dTick, removals);

    std::vector<EditOp> ops;
    // All removals first (indices are read at apply time), then the events'
    // exact bytes re-inserted at the shifted ticks.
    for (size_t t = 0; t < m_smf.tracks.size(); t++)
        appendRemoveOps(ops, int(t), std::move(removals[t]));
    for (size_t t = 0; t < m_smf.tracks.size(); t++) {
        for (size_t index : moved[t]) {
            EditOp op;
            op.type = EditOp::InsertEvent;
            op.smfTrack = int(t);
            op.event = m_smf.tracks[t].events[index];
            op.event.tick = uint64_t(std::max<int64_t>(0, int64_t(op.event.tick) + dTick));
            op.preservesNoteId = op.event.isNoteOn(); // shifted, not new
            ops.push_back(op);
        }
    }
    ops.insert(ops.end(), trims.begin(), trims.end());
    pushEdit(tr("move range"), std::move(ops));
}

void SongDocument::duplicateRange(const std::vector<DocNote> &notes,
                                  const std::vector<DocLanePoint> &points, int64_t dTick)
{
    if ((notes.empty() && points.empty()) || dTick == 0 || m_smf.tracks.empty())
        return;
    std::vector<std::vector<size_t>> copied(m_smf.tracks.size());
    const auto mark = [&](int smfTrack, size_t index) {
        if (smfTrack >= 0 && smfTrack < int(copied.size()))
            copied[size_t(smfTrack)].push_back(index);
    };
    // A copy coinciding exactly with an existing note would only replace
    // it with a fresh identity (a run of equal notes duplicated by one step
    // lands on itself): that copy is not written and the note stays.
    std::map<int, std::set<std::tuple<uint8_t, uint64_t, uint32_t>>> existing;
    const auto coincides = [&](int engineTrack, uint8_t key, uint64_t tick, uint32_t duration) {
        auto it = existing.find(engineTrack);
        if (it == existing.end()) {
            it = existing.emplace(engineTrack, std::set<std::tuple<uint8_t, uint64_t, uint32_t>>())
                     .first;
            for (const DocNote &n : notesForTrack(engineTrack)) {
                if (!n.unterminated())
                    it->second.emplace(n.key, n.tick, n.duration);
            }
        }
        return it->second.count({key, tick, duration}) > 0;
    };
    std::vector<PlannedNote> written;
    for (const DocNote &note : notes) {
        if (note.unterminated())
            continue;
        const uint64_t newTick = uint64_t(std::max<int64_t>(0, int64_t(note.tick) + dTick));
        if (coincides(note.engineTrack, note.key, newTick, note.duration))
            continue;
        mark(note.smfTrack, note.onIndex);
        mark(note.smfTrack, note.endIndex);
        written.push_back({note.engineTrack, note.key, newTick, newTick + note.duration});
    }
    for (const DocLanePoint &pt : points)
        mark(pt.smfTrack, pt.index);
    for (std::vector<size_t> &indices : copied) {
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    }
    // Nothing is edited in place: every existing note the copies land on is
    // a trim victim, the originals included (the pairing rule cannot hold a
    // same-key overlap, so a copy shorter than its delta trims its source
    // exactly as a drawn note would).
    std::vector<std::vector<size_t>> removals(m_smf.tracks.size());
    std::vector<EditOp> trims;
    resolveNoteOverlaps(written, {}, removals, trims);
    appendLaneLandingRemovals(points, dTick, removals);
    const bool hasCopies = std::any_of(copied.begin(), copied.end(),
                                       [](const std::vector<size_t> &c) { return !c.empty(); });
    if (!hasCopies)
        return;

    std::vector<EditOp> ops;
    for (size_t t = 0; t < m_smf.tracks.size(); t++)
        appendRemoveOps(ops, int(t), std::move(removals[t]));
    for (size_t t = 0; t < m_smf.tracks.size(); t++) {
        for (size_t index : copied[t]) {
            EditOp op;
            op.type = EditOp::InsertEvent;
            op.smfTrack = int(t);
            op.event = m_smf.tracks[t].events[index];
            op.event.tick = uint64_t(std::max<int64_t>(0, int64_t(op.event.tick) + dTick));
            op.event.noteId = {}; // a copy, not the original: applyOps mints its id
            ops.push_back(op);
        }
    }
    ops.insert(ops.end(), trims.begin(), trims.end());
    pushEdit(tr("duplicate range"), std::move(ops));
}

bool SongDocument::removeTimeRange(uint64_t startTick, uint64_t endTick, const RippleScope &scope)
{
    if (endTick <= startTick || m_smf.tracks.empty())
        return false;
    const uint64_t s = startTick;
    const uint64_t e = endTick;
    const uint64_t span = e - s;

    std::vector<std::vector<size_t>> removals(m_smf.tracks.size());
    std::vector<EditOp> inserts;
    // Every event a pass below decides about gets marked, so overlapping
    // scopes (or the wholeSong meta catch-all) can't remove or re-insert the
    // same event twice.
    std::vector<std::vector<bool>> taken(m_smf.tracks.size());
    for (size_t t = 0; t < m_smf.tracks.size(); t++)
        taken[t].assign(m_smf.tracks[t].events.size(), false);

    const auto consume = [&](int smfTrack, size_t index) {
        if (taken[smfTrack][index])
            return false;
        taken[smfTrack][index] = true;
        return true;
    };
    const auto removeEvent = [&](int smfTrack, size_t index) {
        if (consume(smfTrack, index))
            removals[smfTrack].push_back(index);
    };
    // Raw re-insertion at a new tick, so shifted events keep their exact
    // bytes (status form, meta payload, metronome bytes, ...).
    const auto moveEvent = [&](int smfTrack, size_t index, uint64_t newTick) {
        if (!consume(smfTrack, index))
            return;
        const SmfEvent &ev = m_smf.tracks[smfTrack].events[index];
        if (ev.tick == newTick)
            return;
        removals[smfTrack].push_back(index);
        EditOp op;
        op.type = EditOp::InsertEvent;
        op.smfTrack = smfTrack;
        op.event = ev;
        op.event.tick = newTick;
        op.preservesNoteId = op.event.isNoteOn(); // shifted, not new
        inserts.push_back(op);
    };

    // One value stream (a lane, the tempo row, the time signatures), points
    // in tick order: in-range points vanish — except the last one, which
    // moves to the seam so the state the shifted content plays under
    // survives — and points at or past the range end shift left. A point
    // exactly at the range end lands on the seam itself and overrides a
    // rescued one there, so the rescue is skipped.
    struct StreamPt {
        int smfTrack;
        size_t index;
        uint64_t tick;
    };
    const auto rippleStream = [&](const std::vector<StreamPt> &pts) {
        bool seamCovered = false;
        int winner = -1;
        for (size_t i = 0; i < pts.size(); i++) {
            if (pts[i].tick == e)
                seamCovered = true;
            if (pts[i].tick >= s && pts[i].tick < e)
                winner = int(i);
        }
        for (size_t i = 0; i < pts.size(); i++) {
            const StreamPt &pt = pts[i];
            if (pt.tick < s)
                taken[pt.smfTrack][pt.index] = true;
            else if (pt.tick >= e)
                moveEvent(pt.smfTrack, pt.index, pt.tick - span);
            else if (int(i) == winner && !seamCovered)
                moveEvent(pt.smfTrack, pt.index, s);
            else
                removeEvent(pt.smfTrack, pt.index);
        }
    };

    const auto rippleTrack = [&](int engineTrack) {
        const int smfTrack = smfTrackFor(engineTrack);
        if (smfTrack < 0)
            return;
        // Notes move as pairs so durations survive the shift.
        for (const DocNote &note : notesForTrack(engineTrack)) {
            if (note.tick >= e) {
                moveEvent(note.smfTrack, note.onIndex, note.tick - span);
                if (!note.unterminated())
                    moveEvent(note.smfTrack, note.endIndex,
                              m_smf.tracks[note.smfTrack].events[note.endIndex].tick - span);
            } else if (note.tick >= s) {
                removeEvent(note.smfTrack, note.onIndex);
                if (!note.unterminated())
                    removeEvent(note.smfTrack, note.endIndex);
            }
        }
        // Every other channel event, one value stream per kind: controllers
        // and key pressure stream per data0; program, channel pressure, and
        // bend are one stream each.
        std::map<uint32_t, std::vector<StreamPt>> streams;
        const auto &evs = m_smf.tracks[smfTrack].events;
        for (size_t i = 0; i < evs.size(); i++) {
            const SmfEvent &ev = evs[i];
            if (!ev.isChannel() || ev.typeNibble() <= 0x9)
                continue;
            const bool perData0 = ev.typeNibble() == 0xB || ev.typeNibble() == 0xA;
            const uint32_t key = (uint32_t(ev.status) << 8) | (perData0 ? ev.data0 : 0);
            streams[key].push_back({smfTrack, i, ev.tick});
        }
        for (const auto &kv : streams)
            rippleStream(kv.second);
    };

    std::vector<EditOp> trackEnds;
    if (scope.wholeSong) {
        for (int t = 0; t < engineTrackCount(); t++)
            rippleTrack(t);
        // Tempo (chunk 0 metas, like the tempo lane).
        std::vector<StreamPt> tempo;
        for (const DocLanePoint &pt : lanePoints(0, DOC_CC_TEMPO))
            tempo.push_back({pt.smfTrack, pt.index, pt.tick});
        rippleStream(tempo);
        // Time signatures: one global stream (the last at a tick wins).
        std::vector<StreamPt> sigs;
        for (const DocTimeSig &sig : timeSigs())
            sigs.push_back({sig.smfTrack, sig.index, sig.tick});
        rippleStream(sigs);
        // Every remaining meta and sysex — loop markers, text commands,
        // markers — is never deleted: inside the range it clamps to the
        // seam, past it it shifts left.
        for (size_t t = 0; t < m_smf.tracks.size(); t++) {
            const auto &evs = m_smf.tracks[t].events;
            for (size_t i = 0; i < evs.size(); i++) {
                if (taken[t][i] || evs[i].isChannel())
                    continue;
                if (evs[i].tick >= e)
                    moveEvent(int(t), i, evs[i].tick - span);
                else if (evs[i].tick > s)
                    moveEvent(int(t), i, s);
            }
        }
        // The song itself gets shorter: end-of-track ticks close the gap too.
        for (size_t t = 0; t < m_smf.tracks.size(); t++) {
            const uint64_t end = m_smf.tracks[t].endTick;
            const uint64_t newEnd = end >= e ? end - span : (end > s ? s : end);
            if (newEnd == end)
                continue;
            EditOp op;
            op.type = EditOp::SetTrackEnd;
            op.smfTrack = int(t);
            op.event.tick = newEnd;
            trackEnds.push_back(op);
        }
    } else {
        for (int t : scope.tracks)
            rippleTrack(t);
        for (const std::pair<int, uint8_t> &lane : scope.lanes) {
            if (lane.first < 0 && lane.second != DOC_CC_TEMPO)
                continue;
            std::vector<StreamPt> pts;
            for (const DocLanePoint &pt : lanePoints(lane.first < 0 ? 0 : lane.first, lane.second))
                pts.push_back({pt.smfTrack, pt.index, pt.tick});
            rippleStream(pts);
        }
    }

    std::vector<EditOp> ops;
    for (size_t t = 0; t < m_smf.tracks.size(); t++)
        appendRemoveOps(ops, int(t), std::move(removals[t]));
    ops.insert(ops.end(), inserts.begin(), inserts.end());
    ops.insert(ops.end(), trackEnds.begin(), trackEnds.end());
    if (ops.empty())
        return false;
    pushEdit(tr("remove range"), std::move(ops));
    return true;
}

void SongDocument::insertRawEvent(int smfTrack, const SmfEvent &event)
{
    if (smfTrack < 0 || smfTrack >= int(m_smf.tracks.size()))
        return;
    std::vector<EditOp> ops;
    EditOp op;
    op.type = EditOp::InsertEvent;
    op.smfTrack = smfTrack;
    op.event = event;
    ops.push_back(op);
    pushEdit(tr("insert event"), std::move(ops));
}

void SongDocument::modifyRawEvent(int smfTrack, size_t index, const SmfEvent &event)
{
    if (smfTrack < 0 || smfTrack >= int(m_smf.tracks.size()) ||
        index >= m_smf.tracks[smfTrack].events.size())
        return;
    if (m_smf.tracks[smfTrack].events[index] == event)
        return;
    std::vector<EditOp> ops;
    appendEventEditOps(ops, smfTrack, index, event);
    pushEdit(tr("edit event"), std::move(ops));
}

void SongDocument::deleteRawEvents(int smfTrack, std::vector<size_t> indices)
{
    if (smfTrack < 0 || smfTrack >= int(m_smf.tracks.size()))
        return;
    const size_t count = m_smf.tracks[smfTrack].events.size();
    indices.erase(
        std::remove_if(indices.begin(), indices.end(), [count](size_t i) { return i >= count; }),
        indices.end());
    if (indices.empty())
        return;
    std::vector<EditOp> ops;
    appendRemoveOps(ops, smfTrack, std::move(indices));
    pushEdit(tr("delete %n event(s)", nullptr, int(ops.size())), std::move(ops));
}

bool SongDocument::rawEventMoveBounds(int smfTrack, size_t index, size_t *first, size_t *last) const
{
    if (smfTrack < 0 || smfTrack >= int(m_smf.tracks.size()))
        return false;
    const auto &evs = m_smf.tracks[smfTrack].events;
    if (index >= evs.size())
        return false;
    const SmfEvent &moved = evs[index];
    // The pinning relation, exactly the pair rules InsertEvent's canonical
    // placement enforces: a setup event (CC, program, channel aftertouch,
    // bend) stays ahead of a same-tick note, a note-end ahead of a same-tick
    // note-on. Metas and sysex are pinned against nothing — their position
    // within the tick group is freely the user's.
    const auto pinnedBefore = [](const SmfEvent &a, const SmfEvent &b) {
        if (a.isChannel() && a.typeNibble() >= 0xB && b.isChannel() && b.typeNibble() <= 0x9)
            return true;
        return a.isChannel() && a.isNoteEnd() && b.isNoteOn();
    };
    size_t lo = index;
    while (lo > 0 && evs[lo - 1].tick == moved.tick && !pinnedBefore(evs[lo - 1], moved))
        lo--;
    size_t hi = index;
    while (hi + 1 < evs.size() && evs[hi + 1].tick == moved.tick &&
           !pinnedBefore(moved, evs[hi + 1]))
        hi++;
    *first = lo;
    *last = hi;
    return true;
}

void SongDocument::moveRawEvent(int smfTrack, size_t index, size_t destIndex)
{
    size_t first, last;
    if (!rawEventMoveBounds(smfTrack, index, &first, &last))
        return;
    destIndex = std::clamp(destIndex, first, last);
    if (destIndex == index)
        return;
    std::vector<EditOp> ops;
    EditOp op;
    op.type = EditOp::MoveEvent;
    op.smfTrack = smfTrack;
    op.index = index;
    op.indexTo = destIndex;
    ops.push_back(op);
    pushEdit(tr("reorder event"), std::move(ops));
}

void SongDocument::setTrackEndTick(int smfTrack, uint64_t tick)
{
    if (smfTrack < 0 || smfTrack >= int(m_smf.tracks.size()))
        return;
    const SmfTrack &track = m_smf.tracks[smfTrack];
    // Ticks are non-decreasing, so the last event is the latest.
    const uint64_t minTick = track.events.empty() ? 0 : track.events.back().tick;
    tick = std::max(tick, minTick);
    if (tick == track.endTick)
        return;
    std::vector<EditOp> ops;
    EditOp op;
    op.type = EditOp::SetTrackEnd;
    op.smfTrack = smfTrack;
    op.event.tick = tick;
    ops.push_back(op);
    pushEdit(tr("move end of track"), std::move(ops));
}

void SongDocument::setLoopTick(bool endMarker, int64_t tick)
{
    if (m_smf.tracks.empty())
        return;
    std::vector<EditOp> ops;
    int smfTrack;
    size_t index;
    const bool exists = findLoopMarkerEvent(endMarker, &smfTrack, &index);
    if (!exists && tick < 0)
        return;

    SmfEvent markerEvent;
    if (exists) {
        markerEvent = m_smf.tracks[smfTrack].events[index];
        EditOp remove;
        remove.type = EditOp::RemoveEvent;
        remove.smfTrack = smfTrack;
        remove.index = index;
        ops.push_back(remove);
    } else {
        // New markers go in the first chunk — the only place mid2agb reads
        // seq events from — as a Marker meta.
        markerEvent.status = 0xFF;
        markerEvent.metaType = 0x06;
        markerEvent.blob = QByteArray(1, endMarker ? ']' : '[');
        smfTrack = 0;
    }
    if (tick >= 0) {
        markerEvent.tick = uint64_t(tick);
        EditOp insert;
        insert.type = EditOp::InsertEvent;
        insert.smfTrack = smfTrack;
        insert.event = markerEvent;
        ops.push_back(insert);
    }
    pushEdit(endMarker ? tr("set loop end") : tr("set loop start"), std::move(ops));
}

void SongDocument::setTimeSig(uint64_t tick, int numerator, int denomPow2)
{
    if (m_smf.tracks.empty())
        return;
    const char nn = char(std::clamp(numerator, 1, 64));
    const char dd = char(std::clamp(denomPow2, 0, 6));
    // The bar grid honors the last 0x58 at a tick; modify that one in place
    // so it keeps its chunk, its position within the tick group, and its
    // metronome/32nds bytes.
    DocTimeSig target;
    bool exists = false;
    for (const DocTimeSig &sig : timeSigs()) {
        if (sig.tick == tick) {
            target = sig;
            exists = true;
        }
    }
    std::vector<EditOp> ops;
    EditOp op;
    if (exists) {
        if (char(target.numerator) == nn && char(target.denomPow2) == dd)
            return;
        op.type = EditOp::ModifyEvent;
        op.smfTrack = target.smfTrack;
        op.index = target.index;
        op.event = m_smf.tracks[target.smfTrack].events[target.index];
    } else {
        // New signatures go in the first chunk — the seq chunk, where tempo
        // and new loop markers live — with mid2agb's usual metronome bytes.
        op.type = EditOp::InsertEvent;
        op.smfTrack = 0;
        op.event.tick = tick;
        op.event.status = 0xFF;
        op.event.metaType = 0x58;
        op.event.blob = QByteArray("\x00\x00\x18\x08", 4);
    }
    op.event.blob[0] = nn;
    op.event.blob[1] = dd;
    ops.push_back(op);
    pushEdit(tr("set time signature"), std::move(ops));
}

void SongDocument::moveTimeSig(uint64_t fromTick, uint64_t toTick)
{
    if (fromTick == toTick)
        return;
    const std::vector<DocTimeSig> sigs = timeSigs();
    std::vector<EditOp> ops;
    std::vector<EditOp> inserts;
    for (size_t t = 0; t < m_smf.tracks.size(); t++) {
        std::vector<size_t> indices;
        for (const DocTimeSig &sig : sigs) {
            // A signature already at the destination is overwritten.
            if (sig.smfTrack != int(t) || (sig.tick != fromTick && sig.tick != toTick))
                continue;
            indices.push_back(sig.index);
            if (sig.tick == fromTick) {
                EditOp insert;
                insert.type = EditOp::InsertEvent;
                insert.smfTrack = int(t);
                insert.event = m_smf.tracks[t].events[sig.index];
                insert.event.tick = toTick;
                inserts.push_back(insert);
            }
        }
        appendRemoveOps(ops, int(t), std::move(indices));
    }
    if (inserts.empty())
        return;
    ops.insert(ops.end(), inserts.begin(), inserts.end());
    pushEdit(tr("move time signature"), std::move(ops));
}

void SongDocument::deleteTimeSig(uint64_t tick)
{
    std::vector<EditOp> ops;
    for (size_t t = 0; t < m_smf.tracks.size(); t++) {
        std::vector<size_t> indices;
        for (const DocTimeSig &sig : timeSigs()) {
            if (sig.smfTrack == int(t) && sig.tick == tick)
                indices.push_back(sig.index);
        }
        appendRemoveOps(ops, int(t), std::move(indices));
    }
    if (ops.empty())
        return;
    pushEdit(tr("delete time signature"), std::move(ops));
}

int SongDocument::freeChannel() const
{
    bool used[16] = {};
    for (uint8_t c : m_engineChannel)
        used[c] = true;
    for (int c = 0; c < 16; c++) {
        if (!used[c])
            return c;
    }
    return -1;
}

bool SongDocument::canAddTrack() const
{
    if (m_smf.tracks.empty())
        return false;
    if (engineTrackCount() >= 16)
        return false;
    return freeChannel() >= 0;
}

int SongDocument::addTrack(int voice)
{
    if (!canAddTrack())
        return -1;
    const int channel = freeChannel();
    std::vector<EditOp> ops;
    const int smfTrack = int(m_smf.tracks.size());
    EditOp insert;
    insert.type = EditOp::InsertTrack;
    insert.smfTrack = smfTrack;
    ops.push_back(insert);
    EditOp seed;
    seed.type = EditOp::InsertEvent;
    seed.smfTrack = smfTrack;
    seed.event = makeChannelEvent(0xC, uint8_t(channel), 0, uint8_t(std::clamp(voice, 0, 127)), 0);
    ops.push_back(seed);
    pushEdit(tr("add track"), std::move(ops));

    for (int t = 0; t < engineTrackCount(); t++) {
        if (m_engineToSmf[t] == smfTrack)
            return t;
    }
    return -1;
}

int SongDocument::duplicateTrack(int engineTrack)
{
    const int smfTrack = smfTrackFor(engineTrack);
    if (smfTrack < 0 || !canAddTrack())
        return -1;
    const int channel = freeChannel();
    const uint8_t sourceChannel = channelFor(engineTrack);
    const SmfTrack &src = m_smf.tracks[smfTrack];
    std::vector<EditOp> ops;
    const int newSmfTrack = int(m_smf.tracks.size());
    EditOp insert;
    insert.type = EditOp::InsertTrack;
    insert.smfTrack = newSmfTrack;
    insert.trackData.endTick = src.endTick;
    for (const SmfEvent &ev : src.events) {
        // Only the track's own channel: a foreign file's chunk can
        // interleave several channels, and the other channels' events
        // belong to other engine tracks — copying them would duplicate
        // those tracks' notes into this copy.
        if (!ev.isChannel() || ev.channel() != sourceChannel)
            continue;
        SmfEvent copy = ev;
        copy.status = uint8_t((ev.status & 0xF0) | channel);
        insert.trackData.events.push_back(copy);
    }
    if (insert.trackData.events.empty())
        return -1;
    ops.push_back(std::move(insert));
    pushEdit(tr("duplicate track"), std::move(ops));

    for (int t = 0; t < engineTrackCount(); t++) {
        if (m_engineToSmf[t] == newSmfTrack)
            return t;
    }
    return -1;
}

void SongDocument::deleteTrack(int engineTrack)
{
    const int smfTrack = smfTrackFor(engineTrack);
    if (smfTrack < 0)
        return;
    std::vector<EditOp> ops;
    const auto &evs = m_smf.tracks[smfTrack].events;
    if (smfTrack == 0) {
        // Chunk 0 stays (it is the seq chunk): strip the track's channel
        // events, keep everything else.
        std::vector<size_t> indices;
        for (size_t i = 0; i < evs.size(); i++) {
            if (evs[i].isChannel())
                indices.push_back(i);
        }
        appendRemoveOps(ops, smfTrack, std::move(indices));
    } else {
        // Time signatures in the doomed chunk shape the whole song's bar
        // grid; move them to chunk 0 so the grid survives.
        for (const SmfEvent &ev : evs) {
            if (metaIsTimeSig(ev)) {
                EditOp rescue;
                rescue.type = EditOp::InsertEvent;
                rescue.smfTrack = 0;
                rescue.event = ev;
                ops.push_back(rescue);
            }
        }
        // If the winning loop marker lives in the doomed chunk, move it to
        // chunk 0 (where setLoopTick puts new ones) so the loop survives.
        for (int endMarker = 0; endMarker <= 1; endMarker++) {
            int markerTrack;
            size_t markerIndex;
            if (findLoopMarkerEvent(endMarker != 0, &markerTrack, &markerIndex) &&
                markerTrack == smfTrack) {
                EditOp rescue;
                rescue.type = EditOp::InsertEvent;
                rescue.smfTrack = 0;
                rescue.event = evs[markerIndex];
                ops.push_back(rescue);
            }
        }
        EditOp remove;
        remove.type = EditOp::RemoveTrack;
        remove.smfTrack = smfTrack;
        ops.push_back(remove);
    }
    pushEdit(tr("delete track"), std::move(ops));
}

bool nameIsLoopMarker(const QString &name)
{
    return smfTextIsMarker(name);
}

namespace {

// Latin-1 (SMF text metas have no declared encoding), capped at 64 chars,
// trimmed — MidiTimeline's reading of a name meta's text.
QString trackNameText(const SmfEvent &ev)
{
    const int len = std::min<int>(int(ev.blob.size()), 64);
    return QString::fromLatin1(ev.blob.constData(), len).trimmed();
}

// Where the track's display name lives, mirroring MidiTimeline's reader:
// the chunk's first unprefixed 0x03. An 0x03 scoped to a channel by a MIDI
// Channel Prefix (SmfChannelPrefix — format 0's per-track naming mechanism;
// conversion rewrites those, but a foreign format-1 file may still carry
// them) is never a chunk name. SIZE_MAX when absent.
size_t trackNameLoc(const SmfTrack &track)
{
    SmfChannelPrefix prefix;
    for (size_t i = 0; i < track.events.size(); i++) {
        const SmfEvent &ev = track.events[i];
        prefix.observe(ev);
        if (ev.isMeta() && ev.metaType == 0x03 && prefix.channel < 0)
            return i;
    }
    return SIZE_MAX;
}

} // namespace

QString SongDocument::trackName(int engineTrack) const
{
    const int smfTrack = smfTrackFor(engineTrack);
    if (smfTrack < 0)
        return QString();
    const SmfTrack &track = m_smf.tracks[smfTrack];
    const size_t nameIndex = trackNameLoc(track);
    return nameIndex == SIZE_MAX ? QString() : trackNameText(track.events[nameIndex]);
}

void SongDocument::renameTrack(int engineTrack, const QString &name)
{
    const int smfTrack = smfTrackFor(engineTrack);
    if (smfTrack < 0)
        return;
    const QString trimmed = name.trimmed().left(64);
    if (nameIsLoopMarker(trimmed))
        return;
    const SmfTrack &track = m_smf.tracks[smfTrack];
    const size_t nameIndex = trackNameLoc(track);

    std::vector<EditOp> ops;
    if (nameIndex != SIZE_MAX) {
        if (trimmed.isEmpty()) {
            appendRemoveOps(ops, smfTrack, {nameIndex});
        } else {
            if (trackNameText(track.events[nameIndex]) == trimmed)
                return;
            EditOp op;
            op.type = EditOp::ModifyEvent;
            op.smfTrack = smfTrack;
            op.index = nameIndex;
            op.event = track.events[nameIndex];
            op.event.blob = trimmed.toLatin1();
            ops.push_back(op);
        }
    } else {
        if (trimmed.isEmpty())
            return;
        EditOp op;
        op.type = EditOp::InsertEvent;
        op.smfTrack = smfTrack;
        op.event.tick = 0;
        op.event.status = 0xFF;
        op.event.metaType = 0x03;
        op.event.blob = trimmed.toLatin1();
        ops.push_back(op);
    }
    pushEdit(tr("rename track"), std::move(ops));
}

bool SongDocument::moveTrack(int engineTrack, int targetEngine)
{
    if (engineTrack == targetEngine)
        return false;
    const int fromChunk = smfTrackFor(engineTrack);
    const int toChunk = smfTrackFor(targetEngine);
    if (fromChunk < 0 || toChunk < 0)
        return false;

    std::vector<EditOp> ops;
    // mid2agb reads tempo, time signatures, and loop markers only from the
    // first chunk (so does the tempo lane). When the move changes which
    // chunk is first, those globals stay with position 0: strip them from
    // the old seq chunk and re-insert into the new one. Everything else —
    // channel events, the track's name meta — travels with its chunk.
    std::vector<SmfEvent> rescued;
    if (fromChunk == 0 || toChunk == 0) {
        const auto &evs = m_smf.tracks[0].events;
        std::vector<size_t> indices;
        // Classify exactly as the canonical readers do: tempo as lanePoints
        // validates it (three data bytes), time signatures via metaIsTimeSig,
        // and the whole marker family mid2agb understands (smfMetaIsMarker,
        // the same set renameTrack refuses). Name metas — the chunk's name
        // (first unprefixed 0x03, marker text included) and channel-scoped
        // non-marker 0x03s, as findLoopMarkerEvent and MidiTimeline classify
        // them — are never markers: they travel with their chunk.
        bool nameSeen = false;
        SmfChannelPrefix prefix;
        for (size_t i = 0; i < evs.size(); i++) {
            const SmfEvent &ev = evs[i];
            prefix.observe(ev);
            if (!ev.isMeta())
                continue;
            if (ev.metaType == 0x03 && (prefix.channel >= 0 ? !smfMetaIsMarker(ev) : !nameSeen)) {
                if (prefix.channel < 0)
                    nameSeen = true;
                continue;
            }
            if (metaIsTempo(ev) || metaIsTimeSig(ev) || smfMetaIsMarker(ev)) {
                indices.push_back(i);
                rescued.push_back(ev);
            }
        }
        appendRemoveOps(ops, 0, std::move(indices));
    }
    EditOp move;
    move.type = EditOp::MoveTrack;
    move.smfTrack = fromChunk;
    move.smfTrackTo = toChunk;
    ops.push_back(move);
    // Re-inserted in original order: InsertEvent lands each at the end of
    // its tick group, so same-tick globals keep their relative order (the
    // last tempo/signature at a tick is the one that wins).
    for (const SmfEvent &ev : rescued) {
        EditOp insert;
        insert.type = EditOp::InsertEvent;
        insert.smfTrack = 0;
        insert.event = ev;
        ops.push_back(insert);
    }
    pushEdit(tr("move track"), std::move(ops));
    return true;
}

void SongDocument::setCfg(const SongCfg &cfg)
{
    if (cfgSemanticEqual(cfg, m_cfg))
        return;
    m_undoStack.push(new SongCfgCommand(this, cfg));
}

std::unique_ptr<MidiTimeline> SongDocument::buildTimeline(double sampleRate) const
{
    auto timeline = MidiTimeline::build(m_smf, sampleRate);
    if (timeline)
        timeline->extendedClocks = m_cfg.extendedClocks;
    return timeline;
}

void SongDocument::applyOps(std::vector<EditOp> &ops)
{
    for (EditOp &op : ops) {
        switch (op.type) {
        case EditOp::InsertEvent: {
            SmfTrack &track = m_smf.tracks[op.smfTrack];
            auto &evs = track.events;
            // A genuinely new note-on gets its identity minted on the first
            // apply only; preservesNoteId then pins it so redo replays the
            // same id (edits that move an existing note pre-set the flag
            // with the note's own id in op.event).
            if (op.event.isNoteOn() && !op.preservesNoteId) {
                op.event.noteId = NoteId{};
                mintNoteId(&op.event);
                op.preservesNoteId = true;
            }
            // End of the tick group, so unedited same-tick data keeps its
            // original relative order (mid2agb stable-sorts by time+type, so
            // same-type order within a tick is significant).
            auto it = std::upper_bound(evs.begin(), evs.end(), op.event.tick,
                                       [](uint64_t t, const SmfEvent &e) { return t < e.tick; });
            // Setup events (program change, CC, bend) must precede same-tick
            // notes or the note plays with the stale value — both here and in
            // mid2agb, which keeps file order within a tick.
            if (op.event.isChannel() && op.event.typeNibble() >= 0xB) {
                while (it != evs.begin()) {
                    const SmfEvent &prev = *std::prev(it);
                    if (prev.tick != op.event.tick || !prev.isChannel() || prev.typeNibble() > 0x9)
                        break;
                    --it;
                }
            }
            // A note end must precede same-tick note-ons: pairing (here and
            // in mid2agb) gives every note-on the first same-key end that
            // follows it, so an end landing after an on at the same tick
            // gets claimed by that later note — the earlier note swallows
            // its neighbors and the real end goes orphaned. Canonical
            // intra-tick order is setup events, note ends, note-ons.
            if (op.event.isChannel() && op.event.isNoteEnd()) {
                while (it != evs.begin()) {
                    const SmfEvent &prev = *std::prev(it);
                    if (prev.tick != op.event.tick || !prev.isNoteOn())
                        break;
                    --it;
                }
            }
            op.index = size_t(it - evs.begin());
            evs.insert(it, op.event);
            op.oldEndTick = track.endTick;
            if (op.event.tick > track.endTick)
                track.endTick = op.event.tick;
            break;
        }
        case EditOp::RemoveEvent: {
            auto &evs = m_smf.tracks[op.smfTrack].events;
            op.oldEvent = evs[op.index];
            evs.erase(evs.begin() + long(op.index));
            break;
        }
        case EditOp::ModifyEvent: {
            auto &evs = m_smf.tracks[op.smfTrack].events;
            op.oldEvent = evs[op.index];
            // A note-on staying a note-on keeps its identity; one born from
            // a non-note event (the raw editor's prerogative) mints a fresh
            // id once; anything else carries none.
            if (op.event.isNoteOn()) {
                if (op.oldEvent.isNoteOn()) {
                    op.event.noteId = op.oldEvent.noteId;
                } else if (!op.preservesNoteId) {
                    op.event.noteId = NoteId{};
                    mintNoteId(&op.event);
                    op.preservesNoteId = true;
                }
            } else {
                op.event.noteId = NoteId{};
            }
            evs[op.index] = op.event;
            break;
        }
        case EditOp::MoveEvent: {
            // Both indices sit inside one tick group (moveRawEvent clamps),
            // so tick order is untouched by construction.
            auto &evs = m_smf.tracks[op.smfTrack].events;
            const SmfEvent ev = evs[op.index];
            evs.erase(evs.begin() + long(op.index));
            evs.insert(evs.begin() + long(op.indexTo), ev);
            break;
        }
        case EditOp::InsertTrack:
            // The copies in a duplicated track are new notes: fresh ids,
            // minted once (redo re-inserts the same trackData).
            if (!op.preservesNoteId) {
                for (SmfEvent &ev : op.trackData.events) {
                    ev.noteId = NoteId{};
                    mintNoteId(&ev);
                }
                op.preservesNoteId = true;
            }
            m_smf.tracks.insert(m_smf.tracks.begin() + long(op.smfTrack), op.trackData);
            break;
        case EditOp::RemoveTrack:
            op.trackData = m_smf.tracks[op.smfTrack];
            m_smf.tracks.erase(m_smf.tracks.begin() + long(op.smfTrack));
            break;
        case EditOp::SetTrackEnd: {
            SmfTrack &track = m_smf.tracks[op.smfTrack];
            op.oldEndTick = track.endTick;
            track.endTick = op.event.tick;
            break;
        }
        case EditOp::MoveTrack:
            // The engine mapping is still pre-move here (the command
            // rebuilds the map after applyOps returns), so receivers get
            // the numbering their state is keyed by.
            emit trackMoved(op.smfTrack, op.smfTrackTo,
                            engineRotationMap(engineTrackForChunk(op.smfTrack),
                                              engineTrackForChunk(op.smfTrackTo)));
            moveChunk(m_smf.tracks, op.smfTrack, op.smfTrackTo);
            break;
        }
    }
}

void SongDocument::revertOps(std::vector<EditOp> &ops)
{
    for (auto it = ops.rbegin(); it != ops.rend(); ++it) {
        EditOp &op = *it;
        switch (op.type) {
        case EditOp::InsertEvent: {
            SmfTrack &track = m_smf.tracks[op.smfTrack];
            track.events.erase(track.events.begin() + long(op.index));
            track.endTick = op.oldEndTick;
            break;
        }
        case EditOp::RemoveEvent: {
            auto &evs = m_smf.tracks[op.smfTrack].events;
            evs.insert(evs.begin() + long(op.index), op.oldEvent);
            break;
        }
        case EditOp::ModifyEvent:
            m_smf.tracks[op.smfTrack].events[op.index] = op.oldEvent;
            break;
        case EditOp::MoveEvent: {
            auto &evs = m_smf.tracks[op.smfTrack].events;
            const SmfEvent ev = evs[op.indexTo];
            evs.erase(evs.begin() + long(op.indexTo));
            evs.insert(evs.begin() + long(op.index), ev);
            break;
        }
        case EditOp::InsertTrack:
            m_smf.tracks.erase(m_smf.tracks.begin() + long(op.smfTrack));
            break;
        case EditOp::RemoveTrack:
            m_smf.tracks.insert(m_smf.tracks.begin() + long(op.smfTrack), op.trackData);
            break;
        case EditOp::SetTrackEnd:
            m_smf.tracks[op.smfTrack].endTick = op.oldEndTick;
            break;
        case EditOp::MoveTrack:
            emit trackMoved(op.smfTrackTo, op.smfTrack,
                            engineRotationMap(engineTrackForChunk(op.smfTrackTo),
                                              engineTrackForChunk(op.smfTrack)));
            moveChunk(m_smf.tracks, op.smfTrackTo, op.smfTrack);
            break;
        }
    }
}

void SongDocument::pushEdit(const QString &text, std::vector<EditOp> ops)
{
    if (ops.empty())
        return;
    m_undoStack.push(new SongEditCommand(this, text, std::move(ops)));
}
