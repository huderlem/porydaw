#include "songdocument.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <algorithm>

#include "core/miditimeline.h"
#include "project/songregistry.h"

namespace {

// Loop markers as mid2agb reads them: a text-type meta (0x01-0x07) whose
// content, truncated to 32 bytes and whitespace-trimmed, is the single
// marker character. Matches MidiTimeline::build.
bool metaIsLoopMarker(const SmfEvent &ev, char marker)
{
    if (!ev.isMeta() || ev.metaType < 0x01 || ev.metaType > 0x07)
        return false;
    const int len = std::min<int>(ev.blob.size(), 32);
    const QString text = QString::fromLatin1(ev.blob.constData(), len).trimmed();
    return text.size() == 1 && text[0] == QLatin1Char(marker);
}

bool cfgSemanticEqual(const SongCfg &a, const SongCfg &b)
{
    return a.voicegroupArg == b.voicegroupArg && a.masterVolume == b.masterVolume
        && a.reverb == b.reverb && a.priority == b.priority && a.exactGate == b.exactGate
        && a.extendedClocks == b.extendedClocks && a.noCompression == b.noCompression;
}

} // namespace

// Applies a prebuilt op list; undo reverts it. Op index rules: removals and
// modifications carry indices valid against the document state at apply time,
// so builders order all removals first (descending index per track) and let
// insertions resolve their position when applied.
class SongEditCommand : public QUndoCommand
{
public:
    SongEditCommand(SongDocument *doc, const QString &text,
                    std::vector<SongDocument::EditOp> ops)
        : QUndoCommand(text), m_doc(doc), m_ops(std::move(ops))
    {
    }

    void redo() override
    {
        m_doc->applyOps(m_ops);
        emit m_doc->documentChanged();
    }

    void undo() override
    {
        m_doc->revertOps(m_ops);
        emit m_doc->documentChanged();
    }

private:
    SongDocument *m_doc;
    std::vector<SongDocument::EditOp> m_ops;
};

class SongCfgCommand : public QUndoCommand
{
public:
    SongCfgCommand(SongDocument *doc, const SongCfg &newCfg)
        : QUndoCommand(QObject::tr("song settings")), m_doc(doc), m_new(newCfg),
          m_old(doc->m_cfg)
    {
    }

    void redo() override
    {
        m_doc->m_cfg = m_new;
        emit m_doc->documentChanged();
    }

    void undo() override
    {
        m_doc->m_cfg = m_old;
        emit m_doc->documentChanged();
    }

private:
    SongDocument *m_doc;
    SongCfg m_new;
    SongCfg m_old;
};

SongDocument::SongDocument(QObject *parent)
    : QObject(parent)
{
}

bool SongDocument::load(const SongInfo &song, QString *error)
{
    SmfFile smf;
    if (!SmfFile::readFile(song.midPath, &smf, error))
        return false;

    m_smf = std::move(smf);
    m_cfg = song.cfg;
    m_savedCfg = song.cfg;
    m_midPath = song.midPath;
    m_label = song.label;
    m_hadCfgLine = song.hasCfg;
    m_undoStack.clear();
    rebuildTrackMap();
    return true;
}

bool SongDocument::save(QString *error)
{
    if (!m_smf.writeFile(m_midPath, error))
        return false;

    if (!cfgSemanticEqual(m_cfg, m_savedCfg) || !m_hadCfgLine) {
        const QStringList flags = SongRegistry::mergeCfgFlags(m_cfg);
        m_cfg.rawFlags = flags;
        if (!SongRegistry::writeMidiCfgLine(QFileInfo(m_midPath).path(), m_label, flags,
                                            error))
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
    if (m_smf.format == 0) {
        // Engine track == MIDI channel, all events in the single chunk.
        for (int c = 0; c < 16; c++) {
            m_engineToSmf.push_back(0);
            m_engineChannel.push_back(uint8_t(c));
        }
        return;
    }
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

std::vector<DocNote> SongDocument::notesForTrack(int engineTrack) const
{
    std::vector<DocNote> notes;
    const int smfTrack = smfTrackFor(engineTrack);
    if (smfTrack < 0)
        return notes;
    const auto &evs = m_smf.tracks[smfTrack].events;
    const uint8_t wantChannel =
        m_smf.format == 0 ? channelFor(engineTrack) : 0xFF; // 0xFF = any

    for (size_t i = 0; i < evs.size(); i++) {
        const SmfEvent &on = evs[i];
        if (!on.isNoteOn())
            continue;
        if (wantChannel != 0xFF && on.channel() != wantChannel)
            continue;

        DocNote note;
        note.engineTrack = engineTrack;
        note.smfTrack = smfTrack;
        note.onIndex = i;
        note.tick = on.tick;
        note.key = on.data0;
        note.velocity = on.data1;
        note.channel = on.channel();

        // Pair as mid2agb does: the first same-channel same-key note end at
        // or after the note-on.
        for (size_t j = i + 1; j < evs.size(); j++) {
            const SmfEvent &end = evs[j];
            if (end.isChannel() && end.isNoteEnd() && end.channel() == on.channel()
                && end.data0 == on.data0) {
                note.endIndex = j;
                note.duration = uint32_t(end.tick - on.tick);
                break;
            }
        }
        notes.push_back(note);
    }
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

bool SongDocument::laneEventMatches(const SmfEvent &ev, uint8_t cc, uint8_t channel) const
{
    if (!ev.isChannel())
        return false;
    if (m_smf.format == 0 && ev.channel() != channel)
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
            if (evs[i].isMeta() && evs[i].metaType == 0x51 && evs[i].blob.size() == 3)
                points.push_back({0, i, evs[i].tick, laneValue(evs[i], cc)});
        }
        return points;
    }

    const int smfTrack = smfTrackFor(engineTrack);
    if (smfTrack < 0)
        return points;
    const uint8_t channel = channelFor(engineTrack);
    const auto &evs = m_smf.tracks[smfTrack].events;
    for (size_t i = 0; i < evs.size(); i++) {
        if (laneEventMatches(evs[i], cc, channel))
            points.push_back({smfTrack, i, evs[i].tick, laneValue(evs[i], cc)});
    }
    return points;
}

bool SongDocument::findLanePoint(int engineTrack, uint8_t cc, uint64_t tick,
                                 DocLanePoint *out) const
{
    for (const DocLanePoint &pt : lanePoints(engineTrack, cc)) {
        if (pt.tick == tick) {
            *out = pt;
            return true;
        }
    }
    return false;
}

bool SongDocument::findLoopMarkerEvent(bool endMarker, int *smfTrack, size_t *index) const
{
    const char marker = endMarker ? ']' : '[';
    for (size_t t = 0; t < m_smf.tracks.size(); t++) {
        // Mirror MidiTimeline::build: the first 0x03 meta is consumed as the
        // track name and never checked as a loop marker.
        bool nameSeen = false;
        const auto &evs = m_smf.tracks[t].events;
        for (size_t i = 0; i < evs.size(); i++) {
            const SmfEvent &ev = evs[i];
            if (!ev.isMeta())
                continue;
            if (ev.metaType == 0x03 && !nameSeen) {
                nameSeen = true;
                continue;
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

void SongDocument::appendNoteInsertOps(std::vector<EditOp> &ops, int smfTrack,
                                       uint8_t channel, uint64_t tick, uint8_t key,
                                       uint32_t duration, uint8_t velocity) const
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

void SongDocument::addNote(int engineTrack, uint64_t tick, uint8_t key, uint32_t duration,
                           uint8_t velocity)
{
    const int smfTrack = smfTrackFor(engineTrack);
    if (smfTrack < 0)
        return;
    std::vector<EditOp> ops;
    appendNoteInsertOps(ops, smfTrack, channelFor(engineTrack), tick, key, duration,
                        velocity);
    pushEdit(tr("add note"), std::move(ops));
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

void SongDocument::moveNotes(const std::vector<DocNote> &notes, int64_t dTick, int dKey)
{
    if (notes.empty() || (dTick == 0 && dKey == 0))
        return;
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
    for (const DocNote &note : notes) {
        const uint64_t newTick = uint64_t(std::max<int64_t>(0, int64_t(note.tick) + dTick));
        const int newKey = std::clamp(int(note.key) + dKey, 0, 127);
        if (note.unterminated()) {
            EditOp on;
            on.type = EditOp::InsertEvent;
            on.smfTrack = note.smfTrack;
            on.event = makeChannelEvent(0x9, note.channel, newTick, uint8_t(newKey),
                                        note.velocity);
            ops.push_back(on);
        } else {
            appendNoteInsertOps(ops, note.smfTrack, note.channel, newTick, uint8_t(newKey),
                                note.duration, note.velocity);
        }
    }
    pushEdit(tr("move %n note(s)", nullptr, int(notes.size())), std::move(ops));
}

void SongDocument::resizeNotes(const std::vector<DocNote> &notes, int64_t dDuration)
{
    if (notes.empty() || dDuration == 0)
        return;
    std::vector<EditOp> ops;
    for (size_t t = 0; t < m_smf.tracks.size(); t++) {
        std::vector<size_t> indices;
        for (const DocNote &note : notes) {
            if (note.smfTrack == int(t) && !note.unterminated())
                indices.push_back(note.endIndex);
        }
        appendRemoveOps(ops, int(t), std::move(indices));
    }
    for (const DocNote &note : notes) {
        const uint32_t newDuration =
            uint32_t(std::max<int64_t>(1, int64_t(note.duration) + dDuration));
        EditOp end;
        end.type = EditOp::InsertEvent;
        end.smfTrack = note.smfTrack;
        end.event =
            makeChannelEvent(0x9, note.channel, note.tick + newDuration, note.key, 0);
        ops.push_back(end);
    }
    pushEdit(tr("resize %n note(s)", nullptr, int(notes.size())), std::move(ops));
}

void SongDocument::setNotesVelocity(const std::vector<DocNote> &notes, uint8_t velocity)
{
    if (notes.empty())
        return;
    std::vector<EditOp> ops;
    for (const DocNote &note : notes) {
        EditOp op;
        op.type = EditOp::ModifyEvent;
        op.smfTrack = note.smfTrack;
        op.index = note.onIndex;
        op.event = makeChannelEvent(0x9, note.channel, note.tick, note.key,
                                    std::clamp<uint8_t>(velocity, 1, 127));
        ops.push_back(op);
    }
    pushEdit(tr("set velocity"), std::move(ops));
}

SmfEvent SongDocument::makeLaneEvent(uint8_t cc, uint8_t channel, uint64_t tick,
                                     int value) const
{
    if (cc == DOC_CC_TEMPO) {
        SmfEvent ev;
        ev.tick = tick;
        ev.status = 0xFF;
        ev.metaType = 0x51;
        const uint32_t usPerBeat =
            uint32_t(60000000.0 / double(std::clamp(value, 1, 999)) + 0.5);
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
    const int smfTrack = cc == DOC_CC_TEMPO ? 0 : smfTrackFor(engineTrack);
    if (smfTrack < 0 || m_smf.tracks.empty())
        return;
    EditOp op;
    op.type = EditOp::InsertEvent;
    op.smfTrack = smfTrack;
    op.event = makeLaneEvent(cc, channelFor(engineTrack), tick, value);
    std::vector<EditOp> ops{op};
    pushEdit(cc == DOC_CC_VOICE ? tr("add voice change") : tr("add automation point"),
             std::move(ops));
}

void SongDocument::moveLanePoint(int engineTrack, uint8_t cc, const DocLanePoint &point,
                                 uint64_t newTick, int newValue)
{
    const QString text =
        cc == DOC_CC_VOICE ? tr("change voice") : tr("edit automation point");
    std::vector<EditOp> ops;
    if (newTick == point.tick) {
        // Value-only edit: modify in place so the event keeps its position
        // within its tick group (mid2agb stable-sorts, so a program change
        // hopping past a same-tick note-on would change which voice plays).
        EditOp op;
        op.type = EditOp::ModifyEvent;
        op.smfTrack = point.smfTrack;
        op.index = point.index;
        op.event = makeLaneEvent(cc, channelFor(engineTrack), newTick, newValue);
        ops.push_back(op);
        pushEdit(text, std::move(ops));
        return;
    }
    EditOp remove;
    remove.type = EditOp::RemoveEvent;
    remove.smfTrack = point.smfTrack;
    remove.index = point.index;
    ops.push_back(remove);

    EditOp insert;
    insert.type = EditOp::InsertEvent;
    insert.smfTrack = point.smfTrack;
    insert.event = makeLaneEvent(cc, channelFor(engineTrack), newTick, newValue);
    ops.push_back(insert);
    pushEdit(text, std::move(ops));
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
    pushEdit(cc == DOC_CC_VOICE ? tr("delete voice change(s)")
                                : tr("delete automation point(s)"),
             std::move(ops));
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

void SongDocument::setCfg(const SongCfg &cfg)
{
    if (cfgSemanticEqual(cfg, m_cfg))
        return;
    m_undoStack.push(new SongCfgCommand(this, cfg));
}

std::unique_ptr<MidiTimeline> SongDocument::buildTimeline(double sampleRate) const
{
    return MidiTimeline::build(m_smf, sampleRate);
}

void SongDocument::applyOps(std::vector<EditOp> &ops)
{
    for (EditOp &op : ops) {
        SmfTrack &track = m_smf.tracks[op.smfTrack];
        auto &evs = track.events;
        switch (op.type) {
        case EditOp::InsertEvent: {
            // End of the tick group, so unedited same-tick data keeps its
            // original relative order (mid2agb stable-sorts by time+type, so
            // same-type order within a tick is significant).
            auto it = std::upper_bound(evs.begin(), evs.end(), op.event.tick,
                                       [](uint64_t t, const SmfEvent &e) {
                                           return t < e.tick;
                                       });
            op.index = size_t(it - evs.begin());
            evs.insert(it, op.event);
            op.oldEndTick = track.endTick;
            if (op.event.tick > track.endTick)
                track.endTick = op.event.tick;
            break;
        }
        case EditOp::RemoveEvent:
            op.oldEvent = evs[op.index];
            evs.erase(evs.begin() + long(op.index));
            break;
        case EditOp::ModifyEvent:
            op.oldEvent = evs[op.index];
            evs[op.index] = op.event;
            break;
        }
    }
    rebuildTrackMap();
}

void SongDocument::revertOps(std::vector<EditOp> &ops)
{
    for (auto it = ops.rbegin(); it != ops.rend(); ++it) {
        EditOp &op = *it;
        SmfTrack &track = m_smf.tracks[op.smfTrack];
        auto &evs = track.events;
        switch (op.type) {
        case EditOp::InsertEvent:
            evs.erase(evs.begin() + long(op.index));
            track.endTick = op.oldEndTick;
            break;
        case EditOp::RemoveEvent:
            evs.insert(evs.begin() + long(op.index), op.oldEvent);
            break;
        case EditOp::ModifyEvent:
            evs[op.index] = op.oldEvent;
            break;
        }
    }
    rebuildTrackMap();
}

void SongDocument::pushEdit(const QString &text, std::vector<EditOp> ops)
{
    if (ops.empty())
        return;
    m_undoStack.push(new SongEditCommand(this, text, std::move(ops)));
}
