#pragma once

#include <QColor>
#include <QWidget>
#include <cstdint>
#include <functional>

#include "core/miditimeline.h"
#include "ui/songviewmodel.h"

extern "C" {
#include "voicegroup_loader.h"
}

class QScrollArea;
class QScrollBar;

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

// Read-only song viewer (M1): time ruler, multi-track piano roll (selected
// track in full color, others ghosted), per-track automation lanes with m4a
// names, an "other events" strip, and track headers with instrument names
// from the loaded voicegroup. The MidiTimeline and LoadedVoiceGroup must
// outlive the view or be cleared with setSong(nullptr, nullptr) first.
class SongView : public QWidget
{
    Q_OBJECT

public:
    explicit SongView(QWidget *parent = nullptr);

    void setSong(const MidiTimeline *timeline, const LoadedVoiceGroup *voicegroup);
    void setPlayheadSample(uint64_t samplePos, bool playing);

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

    int selectedTrack() const { return m_selectedTrack; }
    void selectTrack(int track);
    bool trackMuted(int track) const { return m_muteMask & (1u << track); }
    bool trackSoloed(int track) const { return m_soloMask & (1u << track); }
    void setTrackMute(int track, bool on);
    void setTrackSolo(int track, bool on);

    static QColor trackColor(int track);
    QString instrumentLabel(int track) const; // "042 name (type)" from the voicegroup
    QString voiceShortName(uint8_t program) const;

    // Bar/beat grid over [tickBegin, tickEnd): calls fn(tick, isBarStart,
    // barNumber) for every beat, honoring the song's time signature changes.
    void forEachGridLine(uint64_t tickBegin, uint64_t tickEnd,
                         const std::function<void(uint64_t, bool, int)> &fn) const;

    // Interaction from children.
    void zoomAroundContentX(double factor, int anchorContentX);
    void scrollByPx(int dx);
    void scrollRollBy(int dy);
    void refreshTimelineViews();

signals:
    void muteMaskChanged(uint32_t mask);
    void soloMaskChanged(uint32_t mask);
    void selectedTrackChanged(int track);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    int viewportWidth() const;
    void setHScroll(int px);
    void updateScrollbars();
    void rebuildAfterSongChange();

    const MidiTimeline *m_timeline = nullptr;
    const LoadedVoiceGroup *m_voicegroup = nullptr;
    SongViewModel m_model;

    double m_pxPerTick = 1.0;
    int m_scrollPx = 0;
    int m_scrollY = 0;
    int m_keyHeight = 9;
    int m_selectedTrack = 0;
    double m_playheadTick = 0.0;
    bool m_playing = false;
    uint32_t m_muteMask = 0;
    uint32_t m_soloMask = 0;

    songview::TimeRuler *m_ruler = nullptr;
    songview::TrackHeaderPanel *m_headers = nullptr;
    songview::PianoRoll *m_roll = nullptr;
    songview::AutomationArea *m_lanes = nullptr;
    QScrollArea *m_lanesScroll = nullptr;
    songview::OtherStrip *m_strip = nullptr;
    QScrollBar *m_hbar = nullptr;
    QScrollBar *m_vbar = nullptr;
};
