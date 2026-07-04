#pragma once

#include <QColor>
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

class QScrollArea;
class QScrollBar;
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

    // User-added automation lanes with no events yet (SPEC §6.1 "addable from
    // the m4a parameter list"). They live in view state — the model derives
    // lanes from events — and survive document rebuilds until the song is
    // swapped; once the lane gets its first point the model carries it.
    void addEmptyLane(int track, uint8_t cc);
    void removeEmptyLane(int track, uint8_t cc);

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

    // Edit cursor (Reaper-style): placed by clicking the ruler (or empty
    // roll space while read-only — with a document, left-click draws),
    // distinct from the moving playback cursor. Playback starts here, and
    // paste anchors here.
    uint64_t editCursorTick() const { return m_editCursorTick; }
    // Visual placement only (ruler drag preview); commit emits
    // editCursorMoved so playback can follow.
    void setEditCursorTick(uint64_t tick);
    void commitEditCursor(uint64_t tick);

    int selectedTrack() const { return m_selectedTrack; }
    void selectTrack(int track);
    bool trackMuted(int track) const { return m_muteMask & (1u << track); }
    bool trackSoloed(int track) const { return m_soloMask & (1u << track); }
    void setTrackMute(int track, bool on);
    void setTrackSolo(int track, bool on);

    static QColor trackColor(int track);
    QString instrumentLabel(int track) const; // "042 name (type)" from the voicegroup
    QString voiceShortName(uint8_t program) const;

    // Modal voicegroup-entry picker with press-and-hold audition. Returns
    // false on cancel; otherwise *outVoice is the chosen entry (0-127).
    bool pickVoice(const QString &title, int initialVoice, int *outVoice);
    // Track-header entry point: re-pick the voice governing the track (its
    // first program change), inserting one at tick 0 if the track has none.
    void editTrackVoice(int track);

    // Bar/beat grid over [tickBegin, tickEnd): calls fn(tick, isBarStart,
    // barNumber) for every beat, honoring the song's time signature changes.
    void forEachGridLine(uint64_t tickBegin, uint64_t tickEnd,
                         const std::function<void(uint64_t, bool, int)> &fn) const;

    // --- editing support for the child widgets ---
    // Snap grid in ticks: the visible beat subdivision, never finer than the
    // song's mid2agb clock base.
    uint64_t gridTicks() const;
    uint64_t snapTick(double tick) const;

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

    // App-internal note clipboard (copy/paste in the roll). Ticks are offsets
    // from the copied block's start so paste can re-anchor at the edit cursor.
    // Survives track switches and document rebuilds; cleared on song swap
    // (another song's ticks-per-beat may differ).
    struct ClipNote {
        uint32_t relTick;
        uint8_t key;
        uint32_t duration;
        uint8_t velocity;
    };
    std::vector<ClipNote> &noteClipboard() { return m_noteClipboard; }

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

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    int viewportWidth() const;
    void setHScroll(int px);
    void updateScrollbars();
    void rebuildAfterSongChange();
    void mergeEmptyLanes();

    const MidiTimeline *m_timeline = nullptr;
    const LoadedVoiceGroup *m_voicegroup = nullptr;
    SongDocument *m_document = nullptr;
    SongViewModel m_model;

    double m_pxPerTick = 1.0;
    int m_scrollPx = 0;
    int m_scrollY = 0;
    int m_keyHeight = 9;
    int m_keyZoomAccum = 0; // sub-notch wheel remainder for zoomKeyHeight
    int m_selectedTrack = 0;
    double m_playheadTick = 0.0;
    uint64_t m_editCursorTick = 0;
    bool m_playing = false;
    uint32_t m_muteMask = 0;
    uint32_t m_soloMask = 0;
    std::vector<NoteId> m_selection;
    std::vector<ClipNote> m_noteClipboard;
    std::vector<std::pair<int, uint8_t>> m_emptyLanes; // (track, cc), unsorted

    songview::TimeRuler *m_ruler = nullptr;
    songview::TrackHeaderPanel *m_headers = nullptr;
    songview::PianoRoll *m_roll = nullptr;
    songview::AutomationArea *m_lanes = nullptr;
    QScrollArea *m_lanesScroll = nullptr;
    songview::OtherStrip *m_strip = nullptr;
    QScrollBar *m_hbar = nullptr;
    QScrollBar *m_vbar = nullptr;
};
