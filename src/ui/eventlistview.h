#pragma once

#include <QWidget>

class QComboBox;
class QLabel;
class QTableView;
class SongDocument;
class SongView;
struct SmfEvent;

namespace eventlist {
class EventTableModel;
}

// Raw MIDI event list: every event of one SMF chunk as an editable table row
// (tick, type, channel, data bytes, meta/sysex payload, plus a read-only
// decoded summary), ending with the chunk's end-of-track marker. An
// alternative to the piano roll in the same screen space (SongView stacks
// the two); the ruler and automation lanes stay visible around it. Edits go
// through SongDocument's raw-event API, so they share the undo stack and
// refresh through documentChanged like every other editor.
class EventListView : public QWidget
{
    Q_OBJECT

public:
    explicit EventListView(SongView *sv, QWidget *parent = nullptr);

    // May be null (read-only sessions have no raw model to show).
    void setDocument(SongDocument *document);
    // Re-read the SMF (any mutation, undo/redo, or in-place reload). Keeps
    // the chunk/filter choice and the cursor row; idempotent.
    void refresh();
    // Follow the roll's track selection into the matching chunk.
    void syncTrackSelection();
    // Playhead marker: tints the row the play cursor last passed and, while
    // playing, keeps it scrolled into view (never while the user is holding
    // a mouse button or editing a cell). SongView pushes this every UI tick;
    // cheap when the row didn't change.
    void setPlayheadTick(double tick, bool playing);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void rebuildChunkCombo();
    void chunkPicked(int comboIndex);
    void filterPicked();
    void addEvent();
    void deleteSelected();
    void updateCountLabel();
    void updatePlayRow();
    void jumpCursorToRow(int row);
    int currentChunk() const;
    void selectEventRow(int chunk, const SmfEvent &event);

    SongView *m_sv;
    SongDocument *m_document = nullptr;
    eventlist::EventTableModel *m_model;
    QTableView *m_table;
    QComboBox *m_chunk;
    QComboBox *m_filter;
    QLabel *m_count;
    bool m_syncing = false;
    bool m_settingCurrent = false; // programmatic row changes must not
                                   // commit the edit cursor
    double m_playTick = -1.0;      // last playhead tick pushed (-1 = none)
    bool m_playing = false;
};
