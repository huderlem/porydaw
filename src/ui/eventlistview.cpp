#include "eventlistview.h"

#include <QAbstractTableModel>
#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSpinBox>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

#include "core/smf.h"
#include "core/songdocument.h"
#include "ui/m4asemantics.h"
#include "ui/songview.h"

namespace eventlist {

namespace {

// Editable event kinds, in status order. The UI never shows raw status
// bytes; the kind plus the channel column reconstruct them.
enum TypeKind {
    TypeNoteOff,   // 0x8
    TypeNoteOn,    // 0x9
    TypePolyTouch, // 0xA
    TypeCc,        // 0xB
    TypeProgram,   // 0xC
    TypeChanTouch, // 0xD
    TypeBend,      // 0xE
    TypeSysEx0,    // 0xF0
    TypeSysEx7,    // 0xF7
    TypeMeta,      // 0xFF
    TypeKindCount
};

int typeKindOf(const SmfEvent &ev)
{
    if (ev.isMeta())
        return TypeMeta;
    if (ev.status == 0xF0)
        return TypeSysEx0;
    if (ev.status == 0xF7)
        return TypeSysEx7;
    switch (ev.typeNibble()) {
    case 0x8: return TypeNoteOff;
    case 0x9: return TypeNoteOn;
    case 0xA: return TypePolyTouch;
    case 0xB: return TypeCc;
    case 0xC: return TypeProgram;
    case 0xD: return TypeChanTouch;
    default:  return TypeBend; // 0xE; the strict parser admits nothing else
    }
}

QString typeKindName(int kind)
{
    switch (kind) {
    case TypeNoteOff:   return EventListView::tr("Note off");
    case TypeNoteOn:    return EventListView::tr("Note on");
    case TypePolyTouch: return EventListView::tr("Poly aftertouch");
    case TypeCc:        return EventListView::tr("Control change");
    case TypeProgram:   return EventListView::tr("Program change");
    case TypeChanTouch: return EventListView::tr("Channel aftertouch");
    case TypeBend:      return EventListView::tr("Pitch bend");
    case TypeSysEx0:    return QStringLiteral("SysEx (F0)");
    case TypeSysEx7:    return QStringLiteral("SysEx (F7)");
    default:            return EventListView::tr("Meta");
    }
}

// Whether the kind uses the second channel-voice data byte.
bool hasData2(int kind)
{
    switch (kind) {
    case TypeNoteOff:
    case TypeNoteOn:
    case TypePolyTouch:
    case TypeCc:
    case TypeBend:
        return true;
    default:
        return false;
    }
}

// Blob display: text metas render as quoted text when printable, everything
// else as spaced hex pairs. parseBlob accepts both forms back.
QString blobText(const SmfEvent &ev)
{
    if (ev.isMeta() && ev.metaType >= 0x01 && ev.metaType <= 0x07 && !ev.blob.isEmpty()) {
        const bool printable = std::all_of(ev.blob.begin(), ev.blob.end(), [](char c) {
            return uint8_t(c) >= 0x20 && uint8_t(c) < 0x7F;
        });
        if (printable)
            return QStringLiteral("\"%1\"").arg(QString::fromLatin1(ev.blob));
    }
    return QString::fromLatin1(ev.blob.toHex(' ')).toUpper();
}

// Painted form of the blob: a multi-kilobyte sysex must not be re-hexed in
// full on every repaint of its cell. The editor (EditRole) still gets the
// complete text.
constexpr int kBlobDisplayBytes = 64;

QString blobDisplayText(const SmfEvent &ev)
{
    if (ev.blob.size() <= kBlobDisplayBytes)
        return blobText(ev);
    return EventListView::tr("%1 … (%2 bytes)")
        .arg(QString::fromLatin1(ev.blob.left(kBlobDisplayBytes).toHex(' ')).toUpper())
        .arg(ev.blob.size());
}

bool parseBlob(const QString &input, QByteArray *out)
{
    const QString text = input.trimmed();
    if (text.size() >= 2 && text.startsWith(QLatin1Char('"'))
        && text.endsWith(QLatin1Char('"'))) {
        *out = text.mid(1, text.size() - 2).toUtf8();
        return true;
    }
    QString hex = text;
    hex.remove(QLatin1Char(' '));
    hex.remove(QLatin1Char(','));
    static const QRegularExpression hexOnly(QStringLiteral("^([0-9A-Fa-f]{2})*$"));
    if (!hexOnly.match(hex).hasMatch())
        return false;
    *out = QByteArray::fromHex(hex.toLatin1());
    return true;
}

QString metaName(uint8_t metaType)
{
    switch (metaType) {
    case 0x00: return EventListView::tr("Sequence number");
    case 0x01: return EventListView::tr("Text");
    case 0x02: return EventListView::tr("Copyright");
    case 0x03: return EventListView::tr("Track name");
    case 0x04: return EventListView::tr("Instrument");
    case 0x05: return EventListView::tr("Lyric");
    case 0x06: return EventListView::tr("Marker");
    case 0x07: return EventListView::tr("Cue point");
    case 0x20: return EventListView::tr("Channel prefix");
    case 0x21: return EventListView::tr("MIDI port");
    case 0x51: return EventListView::tr("Tempo");
    case 0x54: return EventListView::tr("SMPTE offset");
    case 0x58: return EventListView::tr("Time signature");
    case 0x59: return EventListView::tr("Key signature");
    case 0x7F: return EventListView::tr("Sequencer-specific");
    default:
        return EventListView::tr("Meta 0x%1")
            .arg(metaType, 2, 16, QLatin1Char('0'));
    }
}

QString metaSummary(const SmfEvent &ev)
{
    const QString name = metaName(ev.metaType);
    if (ev.metaType == 0x51 && ev.blob.size() >= 3) {
        const uint32_t us = (uint8_t(ev.blob[0]) << 16) | (uint8_t(ev.blob[1]) << 8)
            | uint8_t(ev.blob[2]);
        if (us > 0)
            return EventListView::tr("Tempo %1 BPM").arg(qRound(60000000.0 / us));
    }
    if (ev.metaType == 0x58 && ev.blob.size() >= 2) {
        return EventListView::tr("Time signature %1").arg(
            midiTimeSigLabel(uint8_t(ev.blob[0]), uint8_t(ev.blob[1])));
    }
    if (ev.metaType >= 0x01 && ev.metaType <= 0x07) {
        QString text = QStringLiteral("%1 %2").arg(name, blobDisplayText(ev));
        // mid2agb's loop markers deserve a callout: they are plain text metas.
        if (metaIsLoopMarker(ev, '['))
            text += EventListView::tr(" — loop start");
        else if (metaIsLoopMarker(ev, ']'))
            text += EventListView::tr(" — loop end");
        return text;
    }
    return name;
}

QString summaryText(const SmfEvent &ev, SongView *sv)
{
    switch (typeKindOf(ev)) {
    case TypeNoteOn:
        if (ev.data1 == 0)
            return EventListView::tr("Note off %1 (velocity-0 note-on)")
                .arg(midiKeyName(ev.data0));
        return EventListView::tr("Note on %1, velocity %2")
            .arg(midiKeyName(ev.data0))
            .arg(ev.data1);
    case TypeNoteOff:
        return EventListView::tr("Note off %1").arg(midiKeyName(ev.data0));
    case TypePolyTouch:
        return EventListView::tr("Poly aftertouch %1 = %2")
            .arg(midiKeyName(ev.data0))
            .arg(ev.data1);
    case TypeCc: {
        const M4aCcInfo info = m4aClassifyCc(ev.data0);
        return EventListView::tr("CC %1 %2 = %3")
            .arg(ev.data0)
            .arg(QLatin1String(info.display), m4aFormatCcValue(ev.data0, ev.data1));
    }
    case TypeProgram: {
        QString name = sv ? sv->voiceShortName(ev.data0) : QString();
        if (name == SongView::tr("Voice")) // the no-voicegroup placeholder
            name.clear();
        return name.isEmpty()
            ? EventListView::tr("Voice %1").arg(ev.data0)
            : EventListView::tr("Voice %1 — %2").arg(ev.data0).arg(name);
    }
    case TypeChanTouch:
        return EventListView::tr("Channel aftertouch = %1").arg(ev.data0);
    case TypeBend:
        return EventListView::tr("Pitch bend %1")
            .arg(m4aFormatBend(int((ev.data1 << 7) | ev.data0) - 8192));
    case TypeSysEx0:
    case TypeSysEx7:
        return EventListView::tr("%n payload byte(s)", nullptr, int(ev.blob.size()));
    default:
        return metaSummary(ev);
    }
}

// Rebuild an event as another kind, keeping whatever carries over (tick
// always, channel and data bytes between channel-voice kinds, the payload
// between meta and sysex).
SmfEvent retyped(const SmfEvent &ev, int kind, uint8_t fallbackChannel)
{
    SmfEvent next = ev;
    const auto toChannel = [&](uint8_t nibble) {
        const uint8_t channel = ev.isChannel() ? ev.channel() : fallbackChannel;
        next.status = uint8_t((nibble << 4) | (channel & 0x0F));
        next.metaType = 0;
        next.blob.clear();
    };
    switch (kind) {
    case TypeNoteOff:   toChannel(0x8); break;
    case TypeNoteOn:    toChannel(0x9); break;
    case TypePolyTouch: toChannel(0xA); break;
    case TypeCc:        toChannel(0xB); break;
    case TypeProgram:   toChannel(0xC); next.data1 = 0; break;
    case TypeChanTouch: toChannel(0xD); next.data1 = 0; break;
    case TypeBend:      toChannel(0xE); break;
    case TypeSysEx0:
    case TypeSysEx7:
        next.status = kind == TypeSysEx0 ? 0xF0 : 0xF7;
        next.metaType = 0;
        next.data0 = next.data1 = 0;
        if (ev.isChannel())
            next.blob.clear();
        break;
    default: // TypeMeta
        next.status = 0xFF;
        next.data0 = next.data1 = 0;
        if (!ev.isMeta()) {
            next.metaType = 0x06; // Marker: the least surprising blank meta
            if (ev.isChannel())
                next.blob.clear();
        }
        break;
    }
    return next;
}

} // namespace

// One SMF chunk as a table: every event in file order (optionally filtered
// by kind) plus a trailing italic end-of-track row. Cell edits queue a raw
// document edit — never mutate inside the delegate's commit chain, because
// documentChanged resets this model — and the reset that follows re-reads
// the SMF.
class EventTableModel : public QAbstractTableModel
{
public:
    enum Col {
        ColTick,
        ColType,
        ColChannel,
        ColData1,
        ColData2,
        ColData,
        ColSummary,
        ColCount
    };
    // Filter bits, one per category; any combination may be shown.
    enum FilterBit {
        FilterNotes   = 1 << 0,
        FilterCc      = 1 << 1,
        FilterProgram = 1 << 2,
        FilterBend    = 1 << 3,
        FilterTouch   = 1 << 4,
        FilterSysEx   = 1 << 5,
        FilterMeta    = 1 << 6,
        FilterAll     = (1 << 7) - 1
    };

    explicit EventTableModel(SongView *sv, QObject *parent)
        : QAbstractTableModel(parent), m_sv(sv)
    {
    }

    void setSource(SongDocument *doc, int chunk)
    {
        beginResetModel();
        m_doc = doc;
        m_chunk = doc && chunk >= 0 && chunk < int(doc->smf().tracks.size()) ? chunk : -1;
        rebuildRows();
        endResetModel();
    }

    void setFilter(int mask)
    {
        beginResetModel();
        m_filter = mask;
        rebuildRows();
        endResetModel();
    }

    void reload()
    {
        beginResetModel();
        if (m_doc && m_chunk >= int(m_doc->smf().tracks.size()))
            m_chunk = -1;
        rebuildRows();
        endResetModel();
    }

    int chunk() const { return m_chunk; }
    size_t shownEvents() const { return m_rows.size(); }

    // The chunk-event index behind a display row; -1 for the EOT row.
    long long eventIndexForRow(int row) const
    {
        if (row < 0 || row >= int(m_rows.size()))
            return -1;
        return (long long)m_rows[row];
    }

    // -1 when the event is filtered out.
    int rowForEventIndex(size_t index) const
    {
        const auto it = std::lower_bound(m_rows.begin(), m_rows.end(), index);
        if (it == m_rows.end() || *it != index)
            return -1;
        return int(it - m_rows.begin());
    }

    // The row the playhead has reached: the last shown event at or before
    // tick (the last of a same-tick run — the most recently fired), the
    // end-of-track row once the playhead passes the chunk end, -1 while it
    // is still ahead of every shown event.
    int rowForTick(double tick) const
    {
        const SmfTrack *tr = track();
        if (!tr || tick < 0)
            return -1;
        if (tick >= double(tr->endTick))
            return int(m_rows.size()); // end-of-track row
        const auto it = std::upper_bound(
            m_rows.begin(), m_rows.end(), tick, [tr](double t, size_t i) {
                // Stale indexes (possible while hidden) sort as +infinity.
                return i >= tr->events.size() || t < double(tr->events[i].tick);
            });
        return int(it - m_rows.begin()) - 1;
    }

    int playRow() const { return m_playRow; }

    void setPlayRow(int row)
    {
        if (row == m_playRow)
            return;
        const int old = m_playRow;
        m_playRow = row;
        const auto repaint = [this](int r) {
            if (r >= 0 && r < rowCount())
                emit dataChanged(index(r, ColTick), index(r, ColCount - 1),
                                 {Qt::BackgroundRole});
        };
        repaint(old);
        repaint(row);
    }

    static bool filterMatches(int mask, const SmfEvent &ev)
    {
        switch (typeKindOf(ev)) {
        case TypeNoteOff:
        case TypeNoteOn:    return mask & FilterNotes;
        case TypeCc:        return mask & FilterCc;
        case TypeProgram:   return mask & FilterProgram;
        case TypeBend:      return mask & FilterBend;
        case TypePolyTouch:
        case TypeChanTouch: return mask & FilterTouch;
        case TypeSysEx0:
        case TypeSysEx7:    return mask & FilterSysEx;
        default:            return mask & FilterMeta;
        }
    }

    // The channel a channel-voice event gets when the source kind had none:
    // the chunk's first channel event's, else channel 0.
    uint8_t fallbackChannel() const
    {
        const SmfTrack *tr = track();
        if (tr) {
            for (const SmfEvent &ev : tr->events)
                if (ev.isChannel())
                    return ev.channel();
        }
        return 0;
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        if (parent.isValid() || !track())
            return 0;
        return int(m_rows.size()) + 1; // + end-of-track row
    }

    int columnCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : ColCount;
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
            switch (section) {
            case ColTick:    return EventListView::tr("Tick");
            case ColType:    return EventListView::tr("Type");
            case ColChannel: return EventListView::tr("Ch");
            case ColData1:   return EventListView::tr("Data 1");
            case ColData2:   return EventListView::tr("Data 2");
            case ColData:    return EventListView::tr("Data");
            case ColSummary: return EventListView::tr("Summary");
            }
        }
        return QAbstractTableModel::headerData(section, orientation, role);
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        const SmfTrack *tr = track();
        if (!tr || !index.isValid())
            return {};
        if (role == Qt::TextAlignmentRole) {
            if (index.column() == ColTick || index.column() == ColChannel
                || index.column() == ColData1 || index.column() == ColData2)
                return int(Qt::AlignRight | Qt::AlignVCenter);
            return {};
        }
        if (role == Qt::BackgroundRole) {
            // Playhead tint (translucent over the alternating base): the
            // row the play cursor last passed. Same red as the timeline's
            // playhead line.
            if (index.row() == m_playRow)
                return QColor(226, 66, 66, 44);
            return {};
        }
        if (index.row() == int(m_rows.size())) { // end-of-track row
            if (role == Qt::FontRole) {
                QFont font;
                font.setItalic(true);
                return font;
            }
            if (role == Qt::DisplayRole || role == Qt::EditRole) {
                if (index.column() == ColTick)
                    return qulonglong(tr->endTick);
                if (index.column() == ColType && role == Qt::DisplayRole)
                    return EventListView::tr("End of track");
            }
            return {};
        }
        if (role != Qt::DisplayRole && role != Qt::EditRole)
            return {};
        if (m_rows[index.row()] >= tr->events.size()) // stale while hidden
            return {};
        const SmfEvent &ev = tr->events[m_rows[index.row()]];
        const int kind = typeKindOf(ev);
        switch (index.column()) {
        case ColTick:
            return qulonglong(ev.tick);
        case ColType:
            return role == Qt::EditRole ? QVariant(kind) : QVariant(typeKindName(kind));
        case ColChannel:
            return ev.isChannel() ? QVariant(ev.channel() + 1) : QVariant();
        case ColData1:
            if (ev.isMeta())
                return ev.metaType;
            return ev.isChannel() ? QVariant(ev.data0) : QVariant();
        case ColData2:
            return hasData2(kind) ? QVariant(ev.data1) : QVariant();
        case ColData:
            if (!ev.isMeta() && !ev.isSysEx())
                return {};
            return role == Qt::EditRole ? QVariant(blobText(ev))
                                        : QVariant(blobDisplayText(ev));
        case ColSummary:
            return role == Qt::DisplayRole ? QVariant(summaryText(ev, m_sv)) : QVariant();
        }
        return {};
    }

    Qt::ItemFlags flags(const QModelIndex &index) const override
    {
        Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        const SmfTrack *tr = track();
        if (!tr || !index.isValid())
            return f;
        if (index.row() == int(m_rows.size())) {
            if (index.column() == ColTick)
                f |= Qt::ItemIsEditable;
            return f;
        }
        if (m_rows[index.row()] >= tr->events.size()) // stale while hidden
            return f;
        const SmfEvent &ev = tr->events[m_rows[index.row()]];
        switch (index.column()) {
        case ColTick:
        case ColType:
            f |= Qt::ItemIsEditable;
            break;
        case ColChannel:
            if (ev.isChannel())
                f |= Qt::ItemIsEditable;
            break;
        case ColData1:
            if (ev.isChannel() || ev.isMeta())
                f |= Qt::ItemIsEditable;
            break;
        case ColData2:
            if (hasData2(typeKindOf(ev)))
                f |= Qt::ItemIsEditable;
            break;
        case ColData:
            if (ev.isMeta() || ev.isSysEx())
                f |= Qt::ItemIsEditable;
            break;
        }
        return f;
    }

    bool setData(const QModelIndex &index, const QVariant &value, int role) override
    {
        if (role != Qt::EditRole)
            return false;
        const SmfTrack *tr = track();
        if (!tr || !index.isValid())
            return false;
        SongDocument *doc = m_doc;
        const int chunk = m_chunk;
        if (index.row() == int(m_rows.size())) {
            if (index.column() != ColTick)
                return false;
            // toULongLong covers the delegate's line-edit text and direct
            // qulonglong values; ticks are 64-bit, never squeeze through int.
            const uint64_t tick = value.toULongLong();
            QMetaObject::invokeMethod(
                this, [doc, chunk, tick] { doc->setTrackEndTick(chunk, tick); },
                Qt::QueuedConnection);
            return true;
        }
        const size_t evIndex = m_rows[index.row()];
        if (evIndex >= tr->events.size()) // stale while hidden
            return false;
        SmfEvent next = tr->events[evIndex];
        switch (index.column()) {
        case ColTick:
            next.tick = value.toULongLong();
            break;
        case ColType: {
            const int kind = value.toInt();
            if (kind < 0 || kind >= TypeKindCount)
                return false;
            next = retyped(next, kind, fallbackChannel());
            break;
        }
        case ColChannel:
            if (!next.isChannel())
                return false;
            next.status = uint8_t((next.status & 0xF0)
                                  | uint8_t(std::clamp(value.toInt() - 1, 0, 15)));
            break;
        case ColData1:
            if (next.isMeta())
                next.metaType = uint8_t(std::clamp(value.toInt(), 0, 127));
            else if (next.isChannel())
                next.data0 = uint8_t(std::clamp(value.toInt(), 0, 127));
            else
                return false;
            break;
        case ColData2:
            if (!hasData2(typeKindOf(next)))
                return false;
            next.data1 = uint8_t(std::clamp(value.toInt(), 0, 127));
            break;
        case ColData: {
            QByteArray blob;
            if (!parseBlob(value.toString(), &blob)) {
                if (m_sv)
                    m_sv->announce(EventListView::tr(
                        "Data must be hex bytes (\"4F 12 ...\") or \"quoted text\""));
                return false;
            }
            next.blob = blob;
            break;
        }
        default:
            return false;
        }
        // Queued: the document edit resets this model (documentChanged →
        // refresh), which must not happen inside the delegate's commit chain.
        QMetaObject::invokeMethod(
            this, [doc, chunk, evIndex, next] { doc->modifyRawEvent(chunk, evIndex, next); },
            Qt::QueuedConnection);
        return true;
    }

private:
    const SmfTrack *track() const
    {
        if (!m_doc || m_chunk < 0 || m_chunk >= int(m_doc->smf().tracks.size()))
            return nullptr;
        return &m_doc->smf().tracks[m_chunk];
    }

    void rebuildRows()
    {
        m_rows.clear();
        m_playRow = -1; // the mapping changed; the view recomputes it
        const SmfTrack *tr = track();
        if (!tr)
            return;
        for (size_t i = 0; i < tr->events.size(); i++)
            if (filterMatches(m_filter, tr->events[i]))
                m_rows.push_back(i);
    }

    SongView *m_sv;
    SongDocument *m_doc = nullptr;
    int m_chunk = -1;
    int m_filter = FilterAll; // FilterBit mask of the shown categories
    int m_playRow = -1; // display row under the playhead; -1 = none
    std::vector<size_t> m_rows; // display row -> chunk event index (ascending)
};

namespace {

// A menu whose checkable items toggle without closing it, so several event
// categories can be checked or unchecked in one visit.
class CheckMenu : public QMenu
{
public:
    using QMenu::QMenu;

protected:
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        QAction *action = actionAt(event->pos());
        if (action && action->isCheckable()) {
            action->trigger();
            return;
        }
        QMenu::mouseReleaseEvent(event);
    }
};

class EventItemDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option,
                          const QModelIndex &index) const override
    {
        const auto spin = [parent](int min, int max) {
            auto *box = new QSpinBox(parent);
            box->setRange(min, max);
            box->setFrame(false);
            return box;
        };
        switch (index.column()) {
        case EventTableModel::ColTick: {
            // Ticks are 64-bit; QSpinBox is int and would silently truncate
            // a large tick just by opening and committing the editor.
            auto *edit = new QLineEdit(parent);
            edit->setFrame(false);
            static const QRegularExpression digits(QStringLiteral("[0-9]{1,19}"));
            edit->setValidator(new QRegularExpressionValidator(digits, edit));
            return edit;
        }
        case EventTableModel::ColChannel:
            return spin(1, 16);
        case EventTableModel::ColData1:
        case EventTableModel::ColData2:
            return spin(0, 127);
        case EventTableModel::ColType: {
            auto *combo = new QComboBox(parent);
            for (int kind = 0; kind < TypeKindCount; kind++)
                combo->addItem(typeKindName(kind), kind);
            // Commit on pick; waiting for a focus change reads as a dead combo.
            connect(combo, QOverload<int>::of(&QComboBox::activated), this,
                    [this, combo] {
                        auto *self = const_cast<EventItemDelegate *>(this);
                        emit self->commitData(combo);
                        emit self->closeEditor(combo);
                    });
            return combo;
        }
        default:
            return QStyledItemDelegate::createEditor(parent, option, index);
        }
    }

    void setEditorData(QWidget *editor, const QModelIndex &index) const override
    {
        if (index.column() == EventTableModel::ColType) {
            static_cast<QComboBox *>(editor)->setCurrentIndex(
                index.data(Qt::EditRole).toInt());
            return;
        }
        QStyledItemDelegate::setEditorData(editor, index);
    }

    void setModelData(QWidget *editor, QAbstractItemModel *model,
                      const QModelIndex &index) const override
    {
        if (index.column() == EventTableModel::ColType) {
            model->setData(index, static_cast<QComboBox *>(editor)->currentData(),
                           Qt::EditRole);
            return;
        }
        QStyledItemDelegate::setModelData(editor, model, index);
    }
};

} // namespace

} // namespace eventlist

using namespace eventlist;

// ------------------------------------------------------------ EventListView

EventListView::EventListView(SongView *sv, QWidget *parent)
    : QWidget(parent), m_sv(sv)
{
    m_model = new EventTableModel(sv, this);

    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(4, 2, 4, 2);
    bar->setSpacing(4);

    m_chunk = new QComboBox(this);
    m_chunk->setObjectName(QStringLiteral("eventListChunk"));
    m_chunk->setToolTip(tr("The MIDI file chunk shown (follows the selected track)"));
    bar->addWidget(m_chunk);

    // Event-type filter: checkboxes, not an exclusive pick — any combination
    // of categories can be shown (e.g. program changes + CCs without notes).
    m_filter = new QToolButton(this);
    m_filter->setObjectName(QStringLiteral("eventListFilter"));
    m_filter->setAutoRaise(true);
    m_filter->setPopupMode(QToolButton::InstantPopup);
    m_filter->setToolTip(tr("Which event types are shown"));
    m_filterMenu = new CheckMenu(this);
    m_filterMenu->setObjectName(QStringLiteral("eventListFilterMenu"));
    const std::pair<QString, int> categories[] = {
        {tr("Notes"),            EventTableModel::FilterNotes},
        {tr("Control changes"),  EventTableModel::FilterCc},
        {tr("Program changes"),  EventTableModel::FilterProgram},
        {tr("Pitch bends"),      EventTableModel::FilterBend},
        {tr("Aftertouch"),       EventTableModel::FilterTouch},
        {tr("SysEx"),            EventTableModel::FilterSysEx},
        {tr("Meta"),             EventTableModel::FilterMeta},
    };
    for (const auto &category : categories) {
        QAction *action = m_filterMenu->addAction(category.first);
        action->setCheckable(true);
        action->setChecked(true);
        action->setData(category.second);
        connect(action, &QAction::toggled, this, &EventListView::filterChanged);
    }
    m_filter->setMenu(m_filterMenu);
    updateFilterText();
    bar->addWidget(m_filter);

    auto *add = new QToolButton(this);
    add->setObjectName(QStringLiteral("eventListAdd"));
    add->setText(tr("+ Add"));
    add->setAutoRaise(true);
    add->setToolTip(tr("Insert an event at the edit cursor "
                       "(a copy of the current row, if any)"));
    connect(add, &QToolButton::clicked, this, &EventListView::addEvent);
    bar->addWidget(add);

    auto *remove = new QToolButton(this);
    remove->setObjectName(QStringLiteral("eventListRemove"));
    remove->setText(tr("Delete"));
    remove->setAutoRaise(true);
    remove->setToolTip(tr("Delete the selected events (Del)"));
    connect(remove, &QToolButton::clicked, this, &EventListView::deleteSelected);
    bar->addWidget(remove);

    bar->addStretch();
    m_count = new QLabel(this);
    bar->addWidget(m_count);

    m_table = new QTableView(this);
    m_table->setObjectName(QStringLiteral("eventListTable"));
    m_table->setModel(m_model);
    m_table->setItemDelegate(new EventItemDelegate(m_table));
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::DoubleClicked
                             | QAbstractItemView::EditKeyPressed
                             | QAbstractItemView::SelectedClicked);
    m_table->setAlternatingRowColors(true);
    m_table->setFrameShape(QFrame::NoFrame);
    m_table->verticalHeader()->setDefaultSectionSize(m_table->fontMetrics().height() + 6);
    m_table->horizontalHeader()->setHighlightSections(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setColumnWidth(EventTableModel::ColTick, 70);
    m_table->setColumnWidth(EventTableModel::ColType, 120);
    m_table->setColumnWidth(EventTableModel::ColChannel, 36);
    m_table->setColumnWidth(EventTableModel::ColData1, 56);
    m_table->setColumnWidth(EventTableModel::ColData2, 56);
    m_table->setColumnWidth(EventTableModel::ColData, 140);
    m_table->installEventFilter(this);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QTableView::customContextMenuRequested, this,
            &EventListView::showContextMenu);
    setFocusProxy(m_table);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
    layout->addWidget(m_table, 1);

    // activated (not currentIndexChanged): only user picks re-target the
    // roll's track selection; programmatic sync must not echo back.
    connect(m_chunk, QOverload<int>::of(&QComboBox::activated), this,
            &EventListView::chunkPicked);
    connect(m_sv, &SongView::selectedTrackChanged, this,
            [this](int) { syncTrackSelection(); });

    // Row focus drives the song position: clicking or arrowing onto a row
    // commits the edit cursor at the event's tick (which also seeks playback
    // when not stopped). Programmatic row changes — the refresh restore,
    // add-event reselect — are guarded so document edits never move the
    // cursor by themselves.
    connect(m_table->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex &current, const QModelIndex &) {
                if (!m_settingCurrent && current.isValid())
                    jumpCursorToRow(current.row());
            });
    // currentRowChanged misses a re-click on the already-current row (the
    // cursor may have moved elsewhere since); clicked() covers it.
    connect(m_table, &QTableView::clicked, this,
            [this](const QModelIndex &index) { jumpCursorToRow(index.row()); });
}

void EventListView::setDocument(SongDocument *document)
{
    if (m_document != document) {
        if (m_document)
            disconnect(m_document, nullptr, this, nullptr);
        m_document = document;
        m_pendingChunk = -1;
        if (m_document) {
            connect(m_document, &SongDocument::documentChanged, this,
                    &EventListView::refresh);
            connect(m_document, &SongDocument::trackMoved, this,
                    &EventListView::onTrackMoved);
        }
    }
    rebuildChunkCombo();
    syncTrackSelection();
}

void EventListView::refresh()
{
    if (!m_document) {
        m_model->setSource(nullptr, -1);
        m_chunk->clear();
        updateCountLabel();
        return;
    }
    // Hidden (the roll's page is current): skip the O(events) rebuild; the
    // toggle refreshes on show. The model tolerates the stale rows (data/
    // flags/setData bounds-check against the live event vector).
    if (isHidden())
        return;
    if (m_chunk->count() != int(m_document->smf().tracks.size())) {
        // A track add/delete (or file reload) shifted the chunk numbering,
        // so the old combo index may now name a different chunk; re-anchor
        // on the roll's selected track rather than trust the raw index.
        m_pendingChunk = -1;
        rebuildChunkCombo();
        syncTrackSelection();
    } else if (m_pendingChunk >= 0) {
        // A track move permuted the chunk numbering at constant count (the
        // count check can't see it): follow the anchored chunk to its new
        // index and refresh the combo's stale Track labels.
        const int target = m_pendingChunk;
        m_pendingChunk = -1;
        rebuildChunkCombo();
        const int idx = m_chunk->findData(target);
        if (idx >= 0) {
            m_syncing = true;
            m_chunk->setCurrentIndex(idx);
            m_syncing = false;
            m_model->setSource(m_document, target);
        }
    } else {
        const QModelIndex current = m_table->currentIndex();
        QList<int> selectedRows;
        if (m_table->selectionModel()) {
            const QModelIndexList rows = m_table->selectionModel()->selectedRows();
            for (const QModelIndex &row : rows)
                selectedRows.append(row.row());
        }
        m_settingCurrent = true;
        m_model->reload();
        const int rowCount = m_model->rowCount();
        if (current.isValid() && rowCount > 0) {
            const int row = std::min(current.row(), rowCount - 1);
            m_table->setCurrentIndex(m_model->index(row, current.column()));
        }
        // Re-select by row position: exact for value edits (rows are
        // stable), approximate after inserts/deletes — good enough to keep
        // a multi-row selection alive across the reset.
        if (selectedRows.size() > 1 && m_table->selectionModel()) {
            QItemSelection selection;
            for (int row : selectedRows) {
                if (row < rowCount)
                    selection.select(m_model->index(row, 0),
                                     m_model->index(row, m_model->columnCount() - 1));
            }
            m_table->selectionModel()->select(selection,
                                              QItemSelectionModel::ClearAndSelect);
        }
        m_settingCurrent = false;
    }
    updateCountLabel();
    updatePlayRow();
}

void EventListView::syncTrackSelection()
{
    if (m_syncing || !m_document || isHidden())
        return;
    const int chunk = m_document->smfTrackFor(m_sv->selectedTrack());
    if (chunk < 0 || chunk == currentChunk())
        return;
    const int comboIndex = m_chunk->findData(chunk);
    if (comboIndex < 0)
        return;
    m_syncing = true;
    m_chunk->setCurrentIndex(comboIndex);
    m_syncing = false;
    m_model->setSource(m_document, chunk);
    updateCountLabel();
    updatePlayRow();
}

void EventListView::onTrackMoved(int fromChunk, int toChunk)
{
    // Remap the anchored chunk through the rotation, into m_pendingChunk
    // for the documentChanged refresh that follows (the document is
    // mid-mutation here — no reads, no rebuild yet). Starts from a prior
    // pending value so moves chain while the page is hidden and refreshes
    // are skipped.
    const int cur = m_pendingChunk >= 0 ? m_pendingChunk : currentChunk();
    if (cur < 0)
        return;
    int next = cur;
    if (cur == fromChunk)
        next = toChunk;
    else if (fromChunk < toChunk && cur > fromChunk && cur <= toChunk)
        next = cur - 1;
    else if (toChunk < fromChunk && cur >= toChunk && cur < fromChunk)
        next = cur + 1;
    // Set even when the index is unchanged: chunks elsewhere in the rotation
    // swapped occupants, so the combo's Track labels need the rebuild.
    m_pendingChunk = next;
}

void EventListView::setPlayheadTick(double tick, bool playing)
{
    if (m_playTick == tick && m_playing == playing)
        return;
    m_playTick = tick;
    m_playing = playing;
    updatePlayRow();
}

void EventListView::updatePlayRow()
{
    // Hidden (the roll's page is current): the model's rows may be stale and
    // there is nothing to tint; the show-time refresh recomputes.
    if (isHidden())
        return;
    const int row = m_model->rowForTick(m_playTick);
    if (row == m_model->playRow())
        return;
    m_model->setPlayRow(row);
    if (!m_playing || row < 0)
        return;
    // Follow the playhead like the roll does — but never yank the table
    // while the user is holding a mouse button (row-drag selection, the
    // scrollbar) or editing a cell.
    if (QApplication::mouseButtons() != Qt::NoButton)
        return;
    QWidget *focus = QApplication::focusWidget();
    if (focus && focus != m_table && m_table->isAncestorOf(focus))
        return; // an open cell editor
    m_table->scrollTo(m_model->index(row, EventTableModel::ColTick));
}

// Commit the edit cursor at a row's tick — the event's, or the chunk end for
// the end-of-track row — and scroll the timeline around the list to show it.
// No-op when the cursor is already there, so a click that also changed the
// current row commits only once.
void EventListView::jumpCursorToRow(int row)
{
    const int chunk = m_model->chunk();
    if (!m_document || chunk < 0 || chunk >= int(m_document->smf().tracks.size()))
        return;
    const SmfTrack &track = m_document->smf().tracks[chunk];
    uint64_t tick = 0;
    const long long i = m_model->eventIndexForRow(row);
    if (i >= 0 && size_t(i) < track.events.size())
        tick = track.events[i].tick;
    else if (row == int(m_model->shownEvents()))
        tick = track.endTick;
    else
        return;
    if (tick == m_sv->editCursorTick())
        return;
    m_sv->commitEditCursor(tick);
    m_sv->ensureTickVisible(tick);
}

bool EventListView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_table) {
        // Leave plain Space unaccepted so the window-level play/pause
        // shortcut fires (same idiom as the song list).
        if (event->type() == QEvent::ShortcutOverride) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            if (keyEvent->key() == Qt::Key_Space
                && keyEvent->modifiers() == Qt::NoModifier) {
                keyEvent->ignore();
                return true;
            }
        }
        if (event->type() == QEvent::KeyPress) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            if (keyEvent->key() == Qt::Key_Delete
                || keyEvent->key() == Qt::Key_Backspace) {
                deleteSelected();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void EventListView::rebuildChunkCombo()
{
    m_syncing = true;
    const int previous = currentChunk();
    m_chunk->clear();
    if (m_document) {
        const SmfFile &smf = m_document->smf();
        for (int t = 0; t < int(smf.tracks.size()); t++) {
            int engineTrack = -1;
            for (int e = 0; e < m_document->engineTrackCount(); e++) {
                if (m_document->smfTrackFor(e) == t) {
                    engineTrack = e;
                    break;
                }
            }
            QString label;
            if (engineTrack >= 0)
                label = tr("Chunk %1 — Track %2").arg(t).arg(engineTrack + 1);
            else
                label = tr("Chunk %1 (tempo/meta)").arg(t);
            m_chunk->addItem(label, t);
        }
        const int restore = m_chunk->findData(previous);
        m_chunk->setCurrentIndex(restore >= 0 ? restore : 0);
    }
    m_syncing = false;
    m_model->setSource(m_document, currentChunk());
    updateCountLabel();
    updatePlayRow();
}

void EventListView::chunkPicked(int)
{
    if (m_syncing)
        return;
    m_model->setSource(m_document, currentChunk());
    updateCountLabel();
    updatePlayRow();
    if (!m_document)
        return;
    // Picking a chunk with an engine track selects that track in the roll,
    // mirroring how the roll's selection steers this combo.
    for (int e = 0; e < m_document->engineTrackCount(); e++) {
        if (m_document->smfTrackFor(e) == currentChunk()) {
            if (e != m_sv->selectedTrack()) {
                m_syncing = true;
                m_sv->selectTrack(e);
                m_syncing = false;
            }
            break;
        }
    }
}

void EventListView::filterChanged()
{
    m_model->setFilter(filterMask());
    updateFilterText();
    updateCountLabel();
    updatePlayRow();
}

int EventListView::filterMask() const
{
    int mask = 0;
    const QList<QAction *> actions = m_filterMenu->actions();
    for (QAction *action : actions) {
        if (action->isChecked())
            mask |= action->data().toInt();
    }
    return mask;
}

// The button reads as the current selection: "All events", one category's
// name, "Notes +2", or "No events".
void EventListView::updateFilterText()
{
    const QList<QAction *> actions = m_filterMenu->actions();
    QStringList checked;
    for (QAction *action : actions) {
        if (action->isChecked())
            checked.append(action->text());
    }
    QString text;
    if (checked.size() == actions.size())
        text = tr("All events");
    else if (checked.isEmpty())
        text = tr("No events");
    else if (checked.size() == 1)
        text = checked.first();
    else
        text = tr("%1 +%2").arg(checked.first()).arg(checked.size() - 1);
    m_filter->setText(text);
}

void EventListView::addEvent()
{
    const int chunk = currentChunk();
    if (!m_document || chunk < 0)
        return;
    const auto &events = m_document->smf().tracks[chunk].events;
    SmfEvent ev;
    const long long src = m_model->eventIndexForRow(m_table->currentIndex().row());
    if (src >= 0 && size_t(src) < events.size()) {
        ev = events[src]; // duplicate the cursor row at the edit cursor
    } else {
        ev.status = uint8_t(0xB0 | m_model->fallbackChannel()); // CC 7 = 100
        ev.data0 = 7;
        ev.data1 = 100;
    }
    ev.tick = m_sv->editCursorTick();
    m_document->insertRawEvent(chunk, ev); // refresh arrives via documentChanged
    selectEventRow(chunk, ev);
}

void EventListView::insertCopyOfRow(int row)
{
    const int chunk = currentChunk();
    if (!m_document || chunk < 0)
        return;
    const auto &events = m_document->smf().tracks[chunk].events;
    const long long src = m_model->eventIndexForRow(row);
    if (src < 0 || size_t(src) >= events.size())
        return;
    const SmfEvent ev = events[src];
    m_document->insertRawEvent(chunk, ev); // refresh arrives via documentChanged
    selectEventRow(chunk, ev);
}

void EventListView::showContextMenu(const QPoint &pos)
{
    const int chunk = currentChunk();
    if (!m_document || chunk < 0)
        return;
    const QModelIndex idx = m_table->indexAt(pos);
    // Right-clicking outside the selection focuses that row first, exactly
    // like a left click (so the edit cursor follows); a click inside the
    // selection keeps it, so multi-row delete stays possible.
    if (idx.isValid() && m_table->selectionModel()
        && !m_table->selectionModel()->isRowSelected(idx.row(), QModelIndex()))
        m_table->setCurrentIndex(m_model->index(idx.row(), EventTableModel::ColTick));

    const long long src = idx.isValid() ? m_model->eventIndexForRow(idx.row()) : -1;
    int deletable = 0;
    if (m_table->selectionModel()) {
        const QModelIndexList rows = m_table->selectionModel()->selectedRows();
        for (const QModelIndex &row : rows) {
            if (m_model->eventIndexForRow(row.row()) >= 0)
                deletable++;
        }
    }

    // One insert, no before/after placement: rows order by tick, and within
    // a tick the document enforces the canonical setup → note-end → note-on
    // order that mid2agb's pairing depends on, so raw position is not the
    // user's to pick. On a row, the insert is a copy of it at its own tick
    // (lands adjacent, ready to edit); on the end-of-track row or empty
    // space it falls back to the toolbar's add-at-edit-cursor. (No separate
    // "at edit cursor" item: the retarget above just moved the cursor to
    // the clicked row, so the two would coincide.)
    QMenu menu(this);
    QAction *insert = menu.addAction(tr("Insert event"));
    // Jump-from-context on a program-change row: surface its voice in the
    // voicegroup dock.
    QAction *showVoice = nullptr;
    int showProgram = -1;
    const auto &events = m_document->smf().tracks[chunk].events;
    if (src >= 0 && size_t(src) < events.size()
        && typeKindOf(events[src]) == TypeProgram) {
        showProgram = events[src].data0;
        showVoice = menu.addAction(tr("Show voice in voicegroup"));
    }
    menu.addSeparator();
    QAction *del = menu.addAction(deletable > 0
                                      ? tr("Delete %n event(s)", nullptr, deletable)
                                      : tr("Delete"));
    del->setEnabled(deletable > 0);

    QAction *chosen = menu.exec(m_table->viewport()->mapToGlobal(pos));
    if (!chosen)
        return;
    if (chosen == insert) {
        if (src >= 0)
            insertCopyOfRow(idx.row());
        else
            addEvent();
    } else if (showVoice && chosen == showVoice) {
        m_sv->revealVoice(showProgram);
    } else if (chosen == del) {
        deleteSelected();
    }
}

void EventListView::deleteSelected()
{
    const int chunk = currentChunk();
    if (!m_document || chunk < 0 || !m_table->selectionModel())
        return;
    std::vector<size_t> indices;
    const QModelIndexList rows = m_table->selectionModel()->selectedRows();
    for (const QModelIndex &row : rows) {
        const long long i = m_model->eventIndexForRow(row.row());
        if (i >= 0) // the EOT row is not deletable; skip it silently
            indices.push_back(size_t(i));
    }
    if (!indices.empty())
        m_document->deleteRawEvents(chunk, std::move(indices));
}

void EventListView::updateCountLabel()
{
    const int chunk = currentChunk();
    if (!m_document || chunk < 0) {
        m_count->clear();
        return;
    }
    const size_t total = m_document->smf().tracks[chunk].events.size();
    const size_t shown = m_model->shownEvents();
    m_count->setText(shown == total
                         ? tr("%n event(s)", nullptr, int(total))
                         : tr("%1 of %2 events").arg(shown).arg(total));
}

int EventListView::currentChunk() const
{
    return m_chunk->count() > 0 ? m_chunk->currentData().toInt() : -1;
}

void EventListView::selectEventRow(int chunk, const SmfEvent &target)
{
    if (!m_document || m_model->chunk() != chunk)
        return;
    const auto &events = m_document->smf().tracks[chunk].events;
    for (size_t i = 0; i < events.size(); i++) {
        if (events[i] == target) {
            const int row = m_model->rowForEventIndex(i);
            if (row >= 0) {
                const QModelIndex idx = m_model->index(row, EventTableModel::ColTick);
                m_settingCurrent = true;
                m_table->setCurrentIndex(idx);
                m_settingCurrent = false;
                m_table->scrollTo(idx);
            }
            return; // an identical earlier twin is just as good a cursor home
        }
    }
}
