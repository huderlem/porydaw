#include "songview.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <climits>
#include <cmath>

#include "core/mid2agbtables.h"
#include "core/songdocument.h"

namespace songview {

namespace {

constexpr int kRulerH = 26;
constexpr int kLaneH = 48; // default row height; Ctrl+wheel rescales
constexpr int kMinLaneH = 28;
constexpr int kMaxLaneH = 128;
constexpr int kAddLaneH = 20;
constexpr int kLanesAreaH = 150;
constexpr int kStripH = 24;
constexpr int kTrackRowH = 46;
constexpr double kMinPxPerBeat = 4.0;
constexpr double kMaxPxPerBeat = 640.0;
constexpr int kMinKeyHeight = 4;
constexpr int kMaxKeyHeight = 32;
constexpr int kVoiceAuditionKey = 60; // middle C, matching the voicegroup browser
constexpr int kVoiceAuditionVel = 112;
// Resize hit-zone half-width at a note's left/right edges.
constexpr int kEdgeW = 3;

bool isBlackKey(int key)
{
    switch (key % 12) {
    case 1: case 3: case 6: case 8: case 10:
        return true;
    default:
        return false;
    }
}

QString keyName(int key)
{
    static const char *const names[] = {"C", "C#", "D", "D#", "E", "F",
                                        "F#", "G", "G#", "A", "A#", "B"};
    return QStringLiteral("%1%2").arg(QLatin1String(names[key % 12])).arg(key / 12 - 1);
}

QColor loopFill() { return QColor(255, 200, 60, 16); }
QColor loopEdge() { return QColor(224, 168, 0); }
QColor playheadColor() { return QColor(226, 66, 66); }

// Draw the loop-region band and the playhead line across rect. x positions
// are computed with origin = local x of timeline tick 0's content position.
// timeSelCovered says whether this widget (or row) is inside the active time
// selection's scope, so the selection band tints exactly the covered content.
void drawOverlays(QPainter &p, const SongView *sv, const QRect &rect, int origin,
                  bool timeSelCovered)
{
    const MidiTimeline *tl = sv->timeline();
    if (!tl)
        return;

    const SongView::TimeSelection &tsel = sv->timeSelection();
    if (timeSelCovered && tsel.active()) {
        const int x0 = origin + sv->contentX(double(tsel.startTick));
        const int x1 = origin + sv->contentX(double(tsel.endTick));
        if (x1 > rect.left() && x0 < rect.right()) {
            QColor fill = sv->palette().color(QPalette::Highlight);
            fill.setAlpha(30);
            p.fillRect(QRect(QPoint(std::max(x0, rect.left()), rect.top()),
                             QPoint(std::min(x1, rect.right()), rect.bottom())),
                       fill);
            p.setPen(QPen(sv->palette().color(QPalette::Highlight), 1));
            p.drawLine(x0, rect.top(), x0, rect.bottom());
            p.drawLine(x1, rect.top(), x1, rect.bottom());
        }
    }
    if (tl->loopStartTick != UINT64_MAX || tl->loopEndTick != UINT64_MAX) {
        const int x0 = tl->loopStartTick != UINT64_MAX
                           ? origin + sv->contentX(double(tl->loopStartTick))
                           : rect.left();
        const int x1 = tl->loopEndTick != UINT64_MAX
                           ? origin + sv->contentX(double(tl->loopEndTick))
                           : rect.right();
        if (x1 > rect.left() && x0 < rect.right()) {
            p.fillRect(QRect(QPoint(std::max(x0, rect.left()), rect.top()),
                             QPoint(std::min(x1, rect.right()), rect.bottom())),
                       loopFill());
            p.setPen(QPen(loopEdge(), 1));
            if (tl->loopStartTick != UINT64_MAX)
                p.drawLine(x0, rect.top(), x0, rect.bottom());
            if (tl->loopEndTick != UINT64_MAX)
                p.drawLine(x1, rect.top(), x1, rect.bottom());
        }
    }

    // Edit cursor (dashed, theme foreground) under the playback cursor.
    const int ex = origin + sv->contentX(double(sv->editCursorTick()));
    if (ex >= rect.left() && ex <= rect.right()) {
        p.setPen(QPen(sv->palette().color(QPalette::WindowText), 1, Qt::DashLine));
        p.drawLine(ex, rect.top(), ex, rect.bottom());
    }

    const int px = origin + sv->contentX(sv->playheadTick());
    if (px >= rect.left() && px <= rect.right()) {
        p.setPen(QPen(playheadColor(), 1));
        p.drawLine(px, rect.top(), px, rect.bottom());
    }
}

// Subdivision level of a sub-beat grid tick: 1 = half beat, 2 = quarter
// beat, 3 = finer. Cosmetic only (drives the line fade).
int subGridLevel(uint64_t tick, uint64_t tpb)
{
    if (tick % std::max<uint64_t>(1, tpb / 2) == 0)
        return 1;
    if (tick % std::max<uint64_t>(1, tpb / 4) == 0)
        return 2;
    return 3;
}

// Calls fn(tick, level) for every sub-beat snap-grid position in [t0, t1)
// that is not a beat line, at the current zoom's snap resolution
// (SongView::gridTicks, which bottoms out at the mid2agb clock grid). No
// callbacks when the grid is at (or coarser than) whole beats.
void forEachSubGridLine(const SongView *sv, double t0, double t1,
                        const std::function<void(uint64_t, int)> &fn)
{
    const uint64_t tpb = sv->timeline()->ticksPerBeat;
    const uint64_t g = sv->gridTicks();
    if (g == 0 || g >= tpb || sv->pxPerBeat() < 10.0)
        return;
    for (uint64_t tick = uint64_t(std::max(0.0, t0) / double(g)) * g;
         tick < uint64_t(t1); tick += g) {
        if (tick % tpb == 0)
            continue; // beat/bar lines are drawn separately
        fn(tick, subGridLevel(tick, tpb));
    }
}

// Vertical bar/beat grid lines inside rect, with zoom-adaptive sub-beat
// lines at the snap grid's positions fading lighter per subdivision level.
void drawGrid(QPainter &p, const SongView *sv, const QRect &rect, int origin)
{
    if (!sv->timeline())
        return;
    const double t0 = std::max(0.0, sv->tickAtContentX(rect.left() - origin));
    const double t1 = sv->tickAtContentX(rect.right() - origin) + 1;
    const bool drawBeats = sv->pxPerBeat() >= 10.0;
    const QColor barColor = sv->palette().color(QPalette::Mid);
    QColor beatColor = barColor;
    beatColor.setAlpha(70);

    QColor subColor = barColor;
    forEachSubGridLine(sv, t0, t1, [&](uint64_t tick, int level) {
        const int x = origin + sv->contentX(double(tick));
        subColor.setAlpha(level == 1 ? 48 : level == 2 ? 34 : 22);
        p.setPen(subColor);
        p.drawLine(x, rect.top(), x, rect.bottom());
    });

    sv->forEachGridLine(uint64_t(t0), uint64_t(t1),
                        [&](uint64_t tick, bool isBar, int) {
                            if (!isBar && !drawBeats)
                                return;
                            const int x = origin + sv->contentX(double(tick));
                            p.setPen(isBar ? barColor : beatColor);
                            p.drawLine(x, rect.top(), x, rect.bottom());
                        });
}

} // namespace

// ---------------------------------------------------------------- TimeRuler

class TimeRuler : public QWidget
{
public:
    explicit TimeRuler(SongView *sv)
        : QWidget(sv), m_sv(sv)
    {
        setFixedHeight(kRulerH);
        setMouseTracking(true);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), palette().color(QPalette::Window).darker(104));
        p.setPen(palette().color(QPalette::Mid));
        p.drawLine(0, height() - 1, width(), height() - 1);

        if (!m_sv->timeline()) {
            p.setPen(palette().color(QPalette::PlaceholderText));
            p.drawText(rect().adjusted(kGutterW + 8, 0, 0, 0), Qt::AlignVCenter,
                       SongView::tr("No song loaded — double-click a song in the browser."));
            return;
        }

        const QRect area(kGutterW, 0, width() - kGutterW, height());
        p.setClipRect(area);

        // Loop band across the whole ruler height.
        drawOverlays(p, m_sv, area, kGutterW, true);

        const double t0 = std::max(0.0, m_sv->tickAtContentX(0));
        const double t1 = m_sv->tickAtContentX(area.width()) + 1;
        const QColor fg = palette().color(QPalette::WindowText);

        // Short sub-beat ticks at the snap grid, mirroring the roll's grid.
        p.setPen(palette().color(QPalette::Mid));
        forEachSubGridLine(m_sv, t0, t1, [&](uint64_t tick, int level) {
            const int x = kGutterW + m_sv->contentX(double(tick));
            p.drawLine(x, height() - (level == 1 ? 5 : 3), x, height() - 1);
        });

        int lastLabelX = -1000; // not INT_MIN: x - lastLabelX must not overflow
        m_sv->forEachGridLine(uint64_t(t0), uint64_t(t1),
                              [&](uint64_t tick, bool isBar, int barNumber) {
                                  const int x = kGutterW + m_sv->contentX(double(tick));
                                  p.setPen(fg);
                                  p.drawLine(x, isBar ? height() - 12 : height() - 6, x,
                                             height() - 1);
                                  if (isBar && x - lastLabelX >= 30) {
                                      p.drawText(x + 3, height() - 12,
                                                 QString::number(barNumber));
                                      lastLabelX = x;
                                  }
                              });

        // Loop bracket glyphs above the band edges.
        const MidiTimeline *tl = m_sv->timeline();
        p.setPen(loopEdge());
        QFont f = p.font();
        f.setBold(true);
        p.setFont(f);
        if (tl->loopStartTick != UINT64_MAX)
            p.drawText(kGutterW + m_sv->contentX(double(tl->loopStartTick)) + 2, 11,
                       QStringLiteral("["));
        if (tl->loopEndTick != UINT64_MAX)
            p.drawText(kGutterW + m_sv->contentX(double(tl->loopEndTick)) + 2, 11,
                       QStringLiteral("]"));

        // Marker drag preview.
        if (m_dragMarker >= 0) {
            const int x = kGutterW + m_sv->contentX(double(m_dragTick));
            p.setPen(QPen(loopEdge(), 2));
            p.drawLine(x, 0, x, height());
        }

        // Time-selection edge handles (the 1px band edges come from
        // drawOverlays); these are what a left-drag grabs.
        const SongView::TimeSelection &tsel = m_sv->timeSelection();
        if (tsel.active()) {
            p.setPen(QPen(palette().color(QPalette::Highlight), 2));
            const int sx0 = kGutterW + m_sv->contentX(double(tsel.startTick));
            const int sx1 = kGutterW + m_sv->contentX(double(tsel.endTick));
            p.drawLine(sx0, 0, sx0, height());
            p.drawLine(sx1, 0, sx1, height());
        }

        // Playhead handle.
        const int px = kGutterW + m_sv->contentX(m_sv->playheadTick());
        if (px >= area.left() && px <= area.right()) {
            QPainterPath tri;
            tri.moveTo(px - 4, height() - 12);
            tri.lineTo(px + 4, height() - 12);
            tri.lineTo(px, height() - 4);
            tri.closeSubpath();
            p.fillPath(tri, playheadColor());
        }
    }

    void wheelEvent(QWheelEvent *event) override
    {
        // Same bindings as the roll's notes area: plain wheel zooms the
        // timeline; Shift (or a trackpad's horizontal delta) scrolls it.
        const QPoint delta = event->angleDelta();
        if (event->modifiers() & Qt::ShiftModifier)
            m_sv->scrollByPx(-(delta.y() ? delta.y() : delta.x()));
        else if (delta.x() && !delta.y())
            m_sv->scrollByPx(-delta.x());
        else
            m_sv->zoomAroundContentX(std::pow(1.0015, delta.y()),
                                     int(event->position().x()) - kGutterW);
        event->accept();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        SongDocument *doc = m_sv->document();
        const MidiTimeline *tl = m_sv->timeline();
        if (!tl || event->pos().x() < kGutterW)
            return;
        const uint64_t clickTick =
            m_sv->snapTick(m_sv->tickAtContentX(event->pos().x() - kGutterW));

        if (event->button() == Qt::RightButton) {
            // Deferred: a drag from here sweeps out a time selection;
            // releasing in place opens the loop/selection menu. Resolved in
            // mouseReleaseEvent.
            if (!doc)
                return;
            m_rightPress = true;
            m_rightPressPos = event->pos();
            m_selAnchor = clickTick;
            return;
        }
        if (event->button() != Qt::LeftButton)
            return;
        m_dragMarker = doc ? hitMarker(event->pos().x()) : -1;
        if (m_dragMarker >= 0) {
            m_dragTick = clickTick;
            update();
            return;
        }
        m_dragSelEdge = doc ? hitSelEdge(event->pos().x()) : -1;
        if (m_dragSelEdge >= 0)
            return;
        // Elsewhere on the ruler: place the edit cursor (drag scrubs it;
        // playback follows on release).
        m_placingCursor = true;
        m_sv->setEditCursorTick(clickTick);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const auto dragTick = [this, event] {
            return m_sv->snapTick(
                m_sv->tickAtContentX(std::max(kGutterW, event->pos().x()) - kGutterW));
        };
        if (m_rightPress) {
            if (!m_selSweep
                && (event->pos() - m_rightPressPos).manhattanLength()
                       >= QApplication::startDragDistance())
                m_selSweep = true;
            if (m_selSweep) {
                const uint64_t tick = dragTick();
                SongView::TimeSelection sel;
                sel.startTick = std::min(m_selAnchor, tick);
                sel.endTick = std::max(m_selAnchor, tick);
                // Scope: the header multi-selection when one exists,
                // otherwise the whole song.
                const uint32_t mask = m_sv->trackSelectionMask();
                if (mask & (mask - 1)) {
                    sel.scope = SongView::TimeSelection::Tracks;
                    sel.trackMask = mask;
                } else {
                    sel.scope = SongView::TimeSelection::AllTracks;
                }
                m_sv->setTimeSelection(sel);
            }
            return;
        }
        if (m_dragMarker >= 0) {
            m_dragTick = dragTick();
            update();
            return;
        }
        if (m_dragSelEdge >= 0) {
            // Selection edges move live (view state, unlike the loop
            // markers' commit-on-release document edit).
            SongView::TimeSelection sel = m_sv->timeSelection();
            const uint64_t tick = dragTick();
            if (m_dragSelEdge == 0)
                sel.startTick = tick;
            else
                sel.endTick = tick;
            if (sel.startTick > sel.endTick) {
                std::swap(sel.startTick, sel.endTick);
                m_dragSelEdge ^= 1;
            }
            m_sv->setTimeSelection(sel);
            return;
        }
        if (m_placingCursor) {
            m_sv->setEditCursorTick(dragTick());
            return;
        }
        setCursor(m_sv->document()
                          && (hitMarker(event->pos().x()) >= 0
                              || hitSelEdge(event->pos().x()) >= 0)
                      ? Qt::SplitHCursor
                      : Qt::ArrowCursor);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::RightButton && m_rightPress) {
            m_rightPress = false;
            if (m_selSweep) {
                m_selSweep = false;
                if (m_sv->timeSelection().active())
                    m_sv->announceTimeSelection();
                else
                    m_sv->clearTimeSelection();
            } else {
                showRulerMenu(m_selAnchor, event->globalPosition().toPoint());
            }
            return;
        }
        if (event->button() != Qt::LeftButton)
            return;
        if (m_placingCursor) {
            m_placingCursor = false;
            m_sv->commitEditCursor(m_sv->editCursorTick());
            return;
        }
        if (m_dragSelEdge >= 0) {
            m_dragSelEdge = -1;
            if (m_sv->timeSelection().active())
                m_sv->announceTimeSelection();
            else
                m_sv->clearTimeSelection(); // edges dragged together
            return;
        }
        if (m_dragMarker < 0)
            return;
        const bool endMarker = m_dragMarker == 1;
        m_dragMarker = -1;
        if (SongDocument *doc = m_sv->document())
            doc->setLoopTick(endMarker, int64_t(m_dragTick));
        update();
    }

private:
    // 0 = start marker, 1 = end marker, -1 = neither near x.
    int hitMarker(int x) const
    {
        const MidiTimeline *tl = m_sv->timeline();
        if (!tl)
            return -1;
        if (tl->loopStartTick != UINT64_MAX
            && std::abs(kGutterW + m_sv->contentX(double(tl->loopStartTick)) - x) <= 6)
            return 0;
        if (tl->loopEndTick != UINT64_MAX
            && std::abs(kGutterW + m_sv->contentX(double(tl->loopEndTick)) - x) <= 6)
            return 1;
        return -1;
    }

    // 0 = selection start edge, 1 = end edge, -1 = neither near x.
    int hitSelEdge(int x) const
    {
        const SongView::TimeSelection &sel = m_sv->timeSelection();
        if (!sel.active())
            return -1;
        if (std::abs(kGutterW + m_sv->contentX(double(sel.startTick)) - x) <= 5)
            return 0;
        if (std::abs(kGutterW + m_sv->contentX(double(sel.endTick)) - x) <= 5)
            return 1;
        return -1;
    }

    void showRulerMenu(uint64_t clickTick, const QPoint &globalPos)
    {
        SongDocument *doc = m_sv->document();
        const MidiTimeline *tl = m_sv->timeline();
        if (!doc || !tl)
            return;
        QMenu menu(this);
        QAction *setStart = menu.addAction(SongView::tr("Set loop start here"));
        QAction *setEnd = menu.addAction(SongView::tr("Set loop end here"));
        QAction *remove = menu.addAction(SongView::tr("Remove loop markers"));
        remove->setEnabled(tl->loopStartTick != UINT64_MAX
                           || tl->loopEndTick != UINT64_MAX);
        QAction *loopFromSel = nullptr;
        QAction *clearSel = nullptr;
        const SongView::TimeSelection sel = m_sv->timeSelection();
        if (sel.active()) {
            menu.addSeparator();
            loopFromSel = menu.addAction(SongView::tr("Set loop to selection"));
            clearSel = menu.addAction(SongView::tr("Clear time selection"));
        }
        QAction *chosen = menu.exec(globalPos);
        if (chosen == setStart) {
            doc->setLoopTick(false, int64_t(clickTick));
        } else if (chosen == setEnd) {
            doc->setLoopTick(true, int64_t(clickTick));
        } else if (chosen == remove) {
            // Two commands; undo restores them one at a time.
            if (tl->loopStartTick != UINT64_MAX)
                doc->setLoopTick(false, -1);
            if (m_sv->timeline()->loopEndTick != UINT64_MAX)
                doc->setLoopTick(true, -1);
        } else if (chosen && chosen == loopFromSel) {
            // Same two-command shape as "Remove loop markers".
            doc->setLoopTick(false, int64_t(sel.startTick));
            doc->setLoopTick(true, int64_t(sel.endTick));
        } else if (chosen && chosen == clearSel) {
            m_sv->clearTimeSelection();
        }
    }

    SongView *m_sv;
    int m_dragMarker = -1;
    uint64_t m_dragTick = 0;
    bool m_placingCursor = false;
    bool m_rightPress = false;  // right button held; sweep vs. menu undecided
    bool m_selSweep = false;    // right-drag time-selection sweep is live
    QPoint m_rightPressPos;
    uint64_t m_selAnchor = 0;   // snapped tick of the right press
    int m_dragSelEdge = -1;     // selection edge being left-dragged (0/1)
};

// ---------------------------------------------------------------- PianoRoll

class PianoRoll : public QWidget
{
public:
    explicit PianoRoll(SongView *sv)
        : QWidget(sv), m_sv(sv)
    {
        setMinimumHeight(120);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMouseTracking(true);
        setFocusPolicy(Qt::ClickFocus);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), palette().color(QPalette::Base));
        if (!m_sv->timeline()) {
            drawKeyboard(p);
            return;
        }

        const int keyH = m_sv->keyHeight();
        const QRect grid(kKeyboardW, 0, width() - kKeyboardW, height());
        p.setClipRect(grid);

        // Pitch row shading + octave boundaries.
        QColor blackRow = palette().color(QPalette::AlternateBase);
        if (blackRow == palette().color(QPalette::Base))
            blackRow = palette().color(QPalette::Window);
        const QColor octaveLine = palette().color(QPalette::Mid);
        for (int key = 0; key < 128; key++) {
            const int y = keyToY(key);
            if (y + keyH < 0 || y > height())
                continue;
            if (isBlackKey(key))
                p.fillRect(QRect(grid.left(), y, grid.width(), keyH), blackRow);
            if (key % 12 == 0) { // octave line under every C
                p.setPen(octaveLine);
                p.drawLine(grid.left(), y + keyH, grid.right(), y + keyH);
            }
        }

        drawGrid(p, m_sv, grid, kKeyboardW);

        // Notes: ghost pass (unselected tracks), then the selected track.
        const SongViewModel &model = m_sv->model();
        const int selected = m_sv->selectedTrack();
        drawNotes(p, model, selected, true);
        drawNotes(p, model, selected, false);
        drawDragPreview(p, model, selected);

        if (m_drag == Drag::Band) {
            const QRect band = QRect(m_pressPos, m_curPos).normalized();
            QColor c = palette().color(QPalette::Highlight);
            p.setPen(QPen(c, 1, Qt::DashLine));
            c.setAlpha(30);
            p.fillRect(band, c);
            p.drawRect(band);
        }

        drawOverlays(p, m_sv, grid, kKeyboardW,
                     m_sv->timeSelectionCoversTrack(m_sv->selectedTrack()));

        p.setClipping(false);
        drawKeyboard(p);
    }

    void wheelEvent(QWheelEvent *event) override
    {
        // Reaper-style bindings: plain wheel over the notes area zooms the
        // timeline, over the keyboard column it scrolls the note range.
        // Ctrl+wheel zooms the key height (the track-height analog); Shift
        // (or a trackpad's horizontal delta) scrolls horizontally.
        const QPoint delta = event->angleDelta();
        const int d = delta.y() ? delta.y() : delta.x();
        if (event->modifiers() & Qt::ControlModifier)
            m_sv->zoomKeyHeight(d, int(event->position().y()));
        else if (event->modifiers() & Qt::ShiftModifier)
            m_sv->scrollByPx(-d);
        else if (delta.x() && !delta.y())
            m_sv->scrollByPx(-delta.x());
        else if (event->position().x() < kKeyboardW)
            m_sv->scrollRollBy(-delta.y() / 2);
        else
            m_sv->zoomAroundContentX(std::pow(1.0015, delta.y()),
                                     int(event->position().x()) - kKeyboardW);
        event->accept();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        setFocus();
        if (!m_sv->timeline())
            return;

        if (event->button() == Qt::MiddleButton) {
            // Reaper-style pan: drag scrolls the roll on both axes.
            m_panning = true;
            m_panPos = event->globalPosition().toPoint();
            setCursor(Qt::ClosedHandCursor);
            return;
        }

        // Keyboard column: audition the clicked key on the selected track.
        if (event->pos().x() < kKeyboardW) {
            if (event->button() == Qt::LeftButton) {
                m_kbdKey = yToKey(event->pos().y());
                auditionKey(m_kbdKey, 100);
            }
            return;
        }

        SongDocument *doc = m_sv->document();
        const ViewNote *hit = doc ? hitNote(event->pos()) : nullptr;

        if (event->button() == Qt::RightButton) {
            // Deferred: a drag from here rubber-band-selects (with Shift, it
            // sweeps a full-height time selection instead); releasing in
            // place context-acts on the pressed note (or on the time
            // selection under the click, or clears the selections over empty
            // space). Resolved in mouseReleaseEvent.
            if (!doc)
                return;
            m_pressPos = m_curPos = event->pos();
            m_rightPress = true;
            m_rightShift = event->modifiers() & Qt::ShiftModifier;
            m_rightAnchorTick = m_sv->snapTick(
                m_sv->tickAtContentX(event->pos().x() - kKeyboardW));
            m_rightHit = hit != nullptr;
            if (hit)
                m_rightHitId = {hit->startTick, hit->key};
            return;
        }
        if (event->button() != Qt::LeftButton)
            return;

        m_pressPos = m_curPos = event->pos();
        m_pressTick = m_sv->tickAtContentX(event->pos().x() - kKeyboardW);
        m_pressKey = yToKey(event->pos().y());
        m_dTick = 0;
        m_dKey = 0;
        m_dDur = 0;
        m_dVel = 0;

        if (hit) {
            std::vector<SongView::NoteId> ids = m_sv->selection();
            const SongView::NoteId id{hit->startTick, hit->key};
            if (event->modifiers() & Qt::ControlModifier) {
                const auto it = std::find(ids.begin(), ids.end(), id);
                if (it != ids.end())
                    ids.erase(it);
                else
                    ids.push_back(id);
                m_sv->setSelection(std::move(ids));
            } else if (!m_sv->isSelected(*hit)) {
                m_sv->setSelection({id});
            }
            m_sv->announceNote(*hit);
            if (nearRightEdge(*hit, event->pos())) {
                m_drag = Drag::Resize;
            } else if (nearLeftEdge(*hit, event->pos())) {
                m_drag = Drag::ResizeLeft;
            } else if (nearVelocityHandle(*hit, event->pos())) {
                m_drag = Drag::Velocity;
                m_velAnchor = *hit;
                m_velAudEff = mid2agbEffectiveVelocity(hit->velocity);
            } else {
                m_drag = Drag::Move;
            }
            // Sound the grabbed note so a press gives the same pitch feedback
            // a drag already does.
            auditionKey(hit->key, hit->velocity);
            m_auditioned = true;
        } else if (doc) {
            // Empty space: deferred, Reaper-style. A horizontal drag from
            // here draws a note (resolved in mouseMoveEvent); releasing in
            // place parks the edit cursor at the click instead. A
            // double-click draws immediately (mouseDoubleClickEvent).
            m_leftPress = true;
            m_sv->clearSelection();
        } else {
            // Read-only (no document): park the edit cursor at the click,
            // like the ruler; playback follows when running.
            m_sv->commitEditCursor(m_sv->snapTick(m_pressTick));
        }
        update();
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        // Double-click on empty space drops a grid-sized note (committed on
        // release; dragging before release still sizes it). Anywhere else a
        // fast click-click behaves as two presses — Qt replaces the second
        // press with this event.
        if (event->button() == Qt::LeftButton && m_sv->document()
            && event->pos().x() >= kKeyboardW && !hitNote(event->pos())) {
            setFocus();
            m_pressPos = m_curPos = event->pos();
            m_pressTick = m_sv->tickAtContentX(event->pos().x() - kKeyboardW);
            m_pressKey = yToKey(event->pos().y());
            beginDraw();
            return;
        }
        mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_panning) {
            const QPoint pos = event->globalPosition().toPoint();
            const QPoint d = pos - m_panPos;
            m_panPos = pos;
            m_sv->scrollByPx(-d.x());
            m_sv->scrollRollBy(-d.y());
            return;
        }
        if (m_kbdKey >= 0) {
            // Keyboard column: dragging glisses — the sounding key follows
            // the cursor (the engine's mono preview releases the old key).
            const int key = yToKey(event->pos().y());
            if (key != m_kbdKey) {
                m_kbdKey = key;
                auditionKey(m_kbdKey, 100);
            }
            return;
        }
        m_curPos = event->pos();
        if (m_rightPress && m_drag == Drag::None
            && (event->pos() - m_pressPos).manhattanLength()
                   >= QApplication::startDragDistance()) {
            m_drag = m_rightShift ? Drag::TimeSel : Drag::Band;
        }
        if (m_leftPress && m_drag == Drag::None
            && std::abs(event->pos().x() - m_pressPos.x())
                   >= QApplication::startDragDistance()) {
            // The deferred empty-space press turns out to be a draw gesture.
            beginDraw();
        }
        if (m_drag == Drag::None) {
            // Hover cursor: resize handle at note left/right edges, velocity
            // handle across the top strip (when zoomed in enough).
            const ViewNote *hit =
                m_sv->document() && event->pos().x() >= kKeyboardW ? hitNote(event->pos())
                                                                   : nullptr;
            setCursor(hit
                              && (nearRightEdge(*hit, event->pos())
                                  || nearLeftEdge(*hit, event->pos()))
                          ? Qt::SizeHorCursor
                          : hit && nearVelocityHandle(*hit, event->pos())
                                ? Qt::SizeVerCursor
                                : Qt::ArrowCursor);
            return;
        }

        const double tick = m_sv->tickAtContentX(event->pos().x() - kKeyboardW);
        const int64_t grid = int64_t(m_sv->gridTicks());
        const int64_t rawD = int64_t(std::llround(tick - m_pressTick));
        const int64_t snappedD = (rawD >= 0 ? rawD + grid / 2 : rawD - grid / 2) / grid * grid;

        if (m_drag == Drag::Move) {
            const int dKey = yToKey(event->pos().y()) - m_pressKey;
            if (snappedD != m_dTick || dKey != m_dKey) {
                m_dTick = snappedD;
                if (dKey != m_dKey) {
                    m_dKey = dKey;
                    // Audition the new pitch while dragging vertically.
                    const std::vector<DocNote> notes = resolveSelection();
                    if (!notes.empty()) {
                        const int key =
                            std::clamp(int(notes.front().key) + m_dKey, 0, 127);
                        auditionKey(key, notes.front().velocity);
                        m_auditioned = true;
                    }
                }
                update();
            }
        } else if (m_drag == Drag::Resize) {
            if (snappedD != m_dDur) {
                m_dDur = snappedD;
                update();
            }
        } else if (m_drag == Drag::ResizeLeft) {
            if (snappedD != m_dTick) {
                m_dTick = snappedD;
                update();
            }
        } else if (m_drag == Drag::Velocity) {
            const int dv = m_pressPos.y() - event->pos().y(); // up = louder
            if (dv != m_dVel) {
                m_dVel = dv;
                const int vel = std::clamp(int(m_velAnchor.velocity) + m_dVel, 1, 127);
                ViewNote preview = m_velAnchor;
                preview.velocity = uint8_t(vel);
                m_sv->announceNote(preview);
                // Re-audition whenever the effective (played) velocity moves
                // to the next mid2agb step.
                const int eff = mid2agbEffectiveVelocity(vel);
                if (eff != m_velAudEff) {
                    m_velAudEff = eff;
                    auditionKey(m_velAnchor.key, vel);
                    m_auditioned = true;
                }
                update();
            }
        } else if (m_drag == Drag::Draw) {
            // The edge under the cursor follows it: right of the anchor cell
            // the end grows (rounded up to the next grid line, never shorter
            // than one cell); left of it the start moves back (snapped down)
            // with the end pinned to the anchor cell. The key follows the
            // cursor vertically — a slight misclick on mouse-down is fixable
            // mid-gesture, with the new pitch auditioned.
            const int64_t cur = int64_t(std::llround(tick));
            const int64_t anchor = int64_t(m_drawAnchor);
            uint64_t start = m_drawAnchor;
            int64_t dur;
            if (cur >= anchor) {
                dur = std::max(grid, (cur - anchor + grid - 1) / grid * grid);
            } else {
                start = uint64_t(
                    std::floor(std::max(0.0, tick) / double(grid)) * double(grid));
                dur = anchor + grid - int64_t(start);
            }
            const int key = yToKey(event->pos().y());
            if (start != m_drawTick || dur != m_drawDur || key != m_drawKey) {
                m_drawTick = start;
                m_drawDur = dur;
                if (key != m_drawKey) {
                    m_drawKey = key;
                    auditionKey(m_drawKey, m_lastVelocity);
                    m_auditioned = true;
                }
                update();
            }
        } else if (m_drag == Drag::TimeSel) {
            // Full-height sweep: a time selection on the current track
            // (notes and automation together).
            const uint64_t t = m_sv->snapTick(tick);
            SongView::TimeSelection sel;
            sel.startTick = std::min(m_rightAnchorTick, t);
            sel.endTick = std::max(m_rightAnchorTick, t);
            sel.scope = SongView::TimeSelection::Tracks;
            sel.trackMask = 1u << m_sv->selectedTrack();
            m_sv->setTimeSelection(sel);
        } else if (m_drag == Drag::Band) {
            update();
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::MiddleButton && m_panning) {
            m_panning = false;
            setCursor(Qt::ArrowCursor);
            return;
        }
        if (m_kbdKey >= 0) {
            auditionKey(m_kbdKey, 0);
            m_kbdKey = -1;
        }
        if (m_auditioned) {
            auditionKey(0, 0);
            m_auditioned = false;
        }
        SongDocument *doc = m_sv->document();
        if (event->button() == Qt::RightButton && m_rightPress) {
            const Drag drag = m_drag;
            m_rightPress = false;
            m_drag = Drag::None;
            if (drag == Drag::TimeSel) {
                if (m_sv->timeSelection().active())
                    m_sv->announceTimeSelection();
                else
                    m_sv->clearTimeSelection();
            } else if (drag == Drag::Band) {
                selectBand(QRect(m_pressPos, m_curPos).normalized(),
                           event->modifiers() & Qt::ControlModifier);
            } else if (doc && m_rightHit) {
                const std::vector<SongView::NoteId> &sel = m_sv->selection();
                if (std::find(sel.begin(), sel.end(), m_rightHitId) == sel.end())
                    m_sv->setSelection({m_rightHitId});
                showNoteMenu(event->pos());
            } else if (insideTimeSelection(event->pos())) {
                m_sv->showTimeSelectionMenu(event->globalPosition().toPoint());
            } else {
                m_sv->clearSelection();
                m_sv->clearTimeSelection();
            }
            update();
            return;
        }
        if (event->button() == Qt::LeftButton && m_leftPress) {
            m_leftPress = false;
            if (m_drag == Drag::None) {
                // Click without a drag: park the edit cursor at the click,
                // like the ruler; playback follows when running.
                m_sv->commitEditCursor(m_sv->snapTick(m_pressTick));
                update();
                return;
            }
        }
        if (event->button() != Qt::LeftButton || m_drag == Drag::None)
            return;

        const Drag drag = m_drag;
        m_drag = Drag::None;

        if (doc && drag == Drag::Draw) {
            doc->addNote(m_sv->selectedTrack(), m_drawTick, uint8_t(m_drawKey),
                         uint32_t(m_drawDur), m_lastVelocity);
            m_sv->setSelection({{uint32_t(m_drawTick), uint8_t(m_drawKey)}});
        } else if (doc && drag == Drag::Move && (m_dTick != 0 || m_dKey != 0)) {
            const std::vector<DocNote> notes = resolveSelection();
            doc->moveNotes(notes, m_dTick, m_dKey);
            // Follow the notes with the selection.
            std::vector<SongView::NoteId> ids;
            for (const DocNote &note : notes)
                ids.push_back(
                    {uint32_t(std::max<int64_t>(0, int64_t(note.tick) + m_dTick)),
                     uint8_t(std::clamp(int(note.key) + m_dKey, 0, 127))});
            m_sv->setSelection(std::move(ids));
        } else if (doc && drag == Drag::Resize && m_dDur != 0) {
            doc->resizeNotes(resolveSelection(), m_dDur);
        } else if (doc && drag == Drag::ResizeLeft && m_dTick != 0) {
            const std::vector<DocNote> notes = resolveSelection();
            doc->resizeNotesLeft(notes, m_dTick);
            // Selection ids key on the start tick, which just moved; follow
            // it (same clamp as the document: the note-off pins the drag).
            std::vector<SongView::NoteId> ids;
            for (const DocNote &note : notes) {
                const int64_t maxTick = note.unterminated()
                                            ? INT64_MAX
                                            : int64_t(note.tick + note.duration) - 1;
                ids.push_back({uint32_t(std::clamp<int64_t>(
                                   int64_t(note.tick) + m_dTick, 0, maxTick)),
                               note.key});
            }
            m_sv->setSelection(std::move(ids));
        } else if (doc && drag == Drag::Velocity && m_dVel != 0) {
            doc->nudgeNotesVelocity(resolveSelection(), m_dVel);
        }
        m_dTick = 0;
        m_dKey = 0;
        m_dDur = 0;
        m_dVel = 0;
        update();
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        // Time-selection range ops (and range-clip paste) win over the
        // note-selection shortcuts; the two selections are mutually
        // exclusive, so there is never a real conflict.
        if (m_sv->handleEditKey(event))
            return;
        SongDocument *doc = m_sv->document();
        if (doc
            && (event->matches(QKeySequence::Copy) || event->matches(QKeySequence::Cut))) {
            const std::vector<DocNote> notes = resolveSelection();
            if (!notes.empty()) {
                copyNotes(notes);
                if (event->matches(QKeySequence::Cut)) {
                    doc->deleteNotes(notes);
                    m_sv->clearSelection();
                }
            }
            event->accept();
            return;
        }
        if (doc && event->matches(QKeySequence::Paste)) {
            pasteAtEditCursor();
            event->accept();
            return;
        }
        if (doc && event->matches(QKeySequence::SelectAll)) {
            selectAllNotes();
            event->accept();
            return;
        }
        if (doc && (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)) {
            const std::vector<DocNote> notes = resolveSelection();
            if (!notes.empty()) {
                doc->deleteNotes(notes);
                m_sv->clearSelection();
            }
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Escape) {
            m_drag = Drag::None;
            m_leftPress = false;
            m_rightPress = false;
            m_sv->clearSelection();
            m_sv->clearTimeSelection();
            update();
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }

private:
    enum class Drag { None, Band, TimeSel, Move, Resize, ResizeLeft, Velocity, Draw };

    // Whether pos falls inside the active time selection's band as this
    // widget draws it (the selection must cover the shown track).
    bool insideTimeSelection(QPoint pos) const
    {
        const SongView::TimeSelection &sel = m_sv->timeSelection();
        if (!sel.active() || !m_sv->timeSelectionCoversTrack(m_sv->selectedTrack()))
            return false;
        const double tick = m_sv->tickAtContentX(pos.x() - kKeyboardW);
        return tick >= double(sel.startTick) && tick < double(sel.endTick);
    }

    int keyToY(int key) const
    {
        return (127 - key) * m_sv->keyHeight() - m_sv->scrollY();
    }

    int yToKey(int y) const
    {
        return std::clamp(127 - (y + m_sv->scrollY()) / m_sv->keyHeight(), 0, 127);
    }

    // All roll auditions go through here so the keyboard column can mark the
    // sounding key (velocity 0 releases and clears the mark).
    void auditionKey(int key, int velocity)
    {
        m_sv->audition(m_sv->selectedTrack(), key, velocity);
        const int sounding = velocity > 0 ? key : -1;
        if (sounding != m_soundingKey) {
            m_soundingKey = sounding;
            update(0, 0, kKeyboardW, height());
        }
    }

    // Begin the pencil gesture: a pending grid-cell note at the press
    // position that sounds while the button is held; the document note is
    // committed on release (one undo entry).
    void beginDraw()
    {
        const double grid = double(m_sv->gridTicks());
        m_drawAnchor =
            uint64_t(std::floor(std::max(0.0, m_pressTick) / grid) * grid);
        m_drawTick = m_drawAnchor;
        m_drawDur = int64_t(m_sv->gridTicks());
        m_drawKey = m_pressKey;
        m_drag = Drag::Draw;
        m_sv->clearSelection();
        ViewNote pending{};
        pending.startTick = uint32_t(m_drawTick);
        pending.endTick = uint32_t(m_drawTick + uint64_t(m_drawDur));
        pending.key = uint8_t(m_drawKey);
        pending.velocity = m_lastVelocity;
        pending.track = uint8_t(m_sv->selectedTrack());
        m_sv->announceNote(pending);
        auditionKey(m_drawKey, m_lastVelocity);
        m_auditioned = true;
        update();
    }

    QRect noteRect(const ViewNote &note) const
    {
        const int x0 = kKeyboardW + m_sv->contentX(double(note.startTick));
        const int x1 = kKeyboardW + m_sv->contentX(double(note.endTick));
        return QRect(x0, keyToY(note.key) + 1, std::max(2, x1 - x0),
                     std::max(2, m_sv->keyHeight() - 1));
    }

    // Topmost (last-drawn) note of the selected track under pos. The rect is
    // widened a little on both sides so the edge resize handles can be
    // grabbed from just outside the note.
    const ViewNote *hitNote(QPoint pos) const
    {
        const int selected = m_sv->selectedTrack();
        const ViewNote *hit = nullptr;
        for (const ViewNote &note : m_sv->model().notes) {
            if (note.track != selected)
                continue;
            if (noteRect(note).adjusted(-2, 0, 2, 0).contains(pos))
                hit = &note;
        }
        return hit;
    }

    bool nearRightEdge(const ViewNote &note, QPoint pos) const
    {
        const QRect r = noteRect(note);
        return pos.x() >= r.right() - kEdgeW && pos.x() <= r.right() + kEdgeW;
    }

    bool nearLeftEdge(const ViewNote &note, QPoint pos) const
    {
        const QRect r = noteRect(note);
        return pos.x() >= r.left() - kEdgeW && pos.x() <= r.left() + kEdgeW;
    }

    bool nearVelocityHandle(const ViewNote &note, QPoint pos) const
    {
        if (m_sv->keyHeight() < kVelHandleMinKeyH)
            return false;
        const QRect r = noteRect(note);
        return pos.x() > r.left() + kEdgeW && pos.x() < r.right() - kEdgeW
            && pos.y() < r.top() + velHandleZoneH(r);
    }

    // Height of the top strip that grabs as the velocity handle: a third of
    // the note, kept clear of dwarfing the Move zone at low zoom.
    static int velHandleZoneH(const QRect &noteRect)
    {
        return std::clamp(noteRect.height() / 3, 2, 6);
    }

    // Resolves the current selection to document notes (skips stale ids).
    std::vector<DocNote> resolveSelection() const
    {
        std::vector<DocNote> notes;
        SongDocument *doc = m_sv->document();
        if (!doc)
            return notes;
        for (const SongView::NoteId &id : m_sv->selection()) {
            DocNote note;
            if (doc->findNote(m_sv->selectedTrack(), id.tick, id.key, &note))
                notes.push_back(note);
        }
        return notes;
    }

    // Fills the clipboard with the notes as a plain note clip (span 0,
    // additive paste), ticks relative to the block start.
    void copyNotes(const std::vector<DocNote> &notes)
    {
        uint64_t base = UINT64_MAX;
        for (const DocNote &note : notes)
            base = std::min(base, note.tick);
        SongView::Clip clip;
        SongView::ClipTrack ct{m_sv->selectedTrack(), {}};
        for (const DocNote &note : notes)
            ct.notes.push_back({uint32_t(note.tick - base), note.key,
                                note.duration ? note.duration
                                              : uint32_t(m_sv->gridTicks()),
                                note.velocity});
        clip.tracks.push_back(std::move(ct));
        m_sv->clipboard() = std::move(clip);
        m_sv->announce(SongView::tr("Copied %n note(s)", nullptr, int(notes.size())));
    }

    // Pastes a plain note clip onto the selected track, anchored at the edit
    // cursor (snapped to the grid), and selects the pasted notes. Range
    // clips (span > 0) are handled by SongView::pasteRangeAtEditCursor.
    void pasteAtEditCursor()
    {
        SongDocument *doc = m_sv->document();
        const SongView::Clip &clip = m_sv->clipboard();
        if (!doc || clip.span != 0 || clip.tracks.empty()
            || clip.tracks.front().notes.empty())
            return;
        const uint64_t base = m_sv->snapTick(double(m_sv->editCursorTick()));
        std::vector<SongDocument::NewNote> notes;
        std::vector<SongView::NoteId> ids;
        for (const SongView::ClipNote &cn : clip.tracks.front().notes) {
            const uint64_t tick = base + cn.relTick;
            notes.push_back({tick, cn.key, cn.duration, cn.velocity});
            ids.push_back({uint32_t(tick), cn.key});
        }
        doc->addNotes(m_sv->selectedTrack(), notes);
        m_sv->setSelection(std::move(ids));
        m_sv->announce(SongView::tr("Pasted %n note(s)", nullptr, int(notes.size())));
    }

    void selectAllNotes()
    {
        std::vector<SongView::NoteId> ids;
        for (const ViewNote &note : m_sv->model().notes) {
            if (note.track == m_sv->selectedTrack())
                ids.push_back({note.startTick, note.key});
        }
        m_sv->setSelection(std::move(ids));
    }

    void drawNotes(QPainter &p, const SongViewModel &model, int selected, bool ghostPass)
    {
        const int keyH = m_sv->keyHeight();
        const bool velZoomed = keyH >= kVelHandleMinKeyH;
        if (!ghostPass && m_drag == Drag::Velocity) {
            QFont f = p.font();
            f.setPixelSize(std::clamp(keyH - 3, 7, 11));
            p.setFont(f);
        }
        for (const ViewNote &note : model.notes) {
            const bool ghost = note.track != selected;
            if (ghost != ghostPass)
                continue;
            const QRect r = noteRect(note);
            if (r.right() < kKeyboardW || r.left() > width())
                continue;
            if (r.bottom() < 0 || r.top() > height())
                continue;
            QColor c = SongView::trackColor(note.track);
            if (ghost) {
                c.setAlpha(60);
                p.fillRect(r, c);
            } else {
                int vel = note.velocity;
                if (m_drag == Drag::Velocity && m_sv->isSelected(note))
                    vel = std::clamp(int(note.velocity) + m_dVel, 1, 127);
                c.setAlpha(120 + vel); // velocity shows as opacity
                p.fillRect(r, c);
                // Velocity bar (bottom = 0, top = 127) once zoomed in enough
                // for the full-width handle; it tracks the drag preview.
                if (velZoomed) {
                    const int barH = r.height() >= 20 ? 2 : 1;
                    const int innerH = r.height() - 2;
                    const int y = std::min(
                        r.top() + 1 + (127 - vel) * (innerH - 1) / 127,
                        r.bottom() - barH);
                    p.fillRect(QRect(r.left() + 1, y, std::max(1, r.width() - 2), barH),
                               SongView::trackColor(note.track).darker(170));
                }
                // While a velocity drag is live, every current-track note
                // shows its (previewed) value.
                if (m_drag == Drag::Velocity) {
                    const QString text = QString::number(vel);
                    if (r.width() >= p.fontMetrics().horizontalAdvance(text) + 4) {
                        p.setPen(c.lightness() > 127 ? Qt::black : Qt::white);
                        p.drawText(r, Qt::AlignCenter, text);
                    }
                }
                if (m_sv->isSelected(note)) {
                    p.setPen(QPen(palette().color(QPalette::HighlightedText), 1));
                    p.drawRect(r.adjusted(0, 0, -1, -1));
                    p.setPen(QPen(palette().color(QPalette::Highlight), 1));
                    p.drawRect(r.adjusted(-1, -1, 0, 0));
                } else {
                    p.setPen(note.unterminated
                                 ? QPen(playheadColor(), 1, Qt::DashLine)
                                 : QPen(SongView::trackColor(note.track).darker(150), 1));
                    p.drawRect(r.adjusted(0, 0, -1, -1));
                }
            }
        }
    }

    // Dashed outlines of the selected notes at their dragged position/length,
    // or the pending note of a draw gesture (solid, like the real note).
    void drawDragPreview(QPainter &p, const SongViewModel &model, int selected)
    {
        if (m_drag == Drag::Draw) {
            const int x0 = kKeyboardW + m_sv->contentX(double(m_drawTick));
            const int x1 =
                kKeyboardW + m_sv->contentX(double(m_drawTick + uint64_t(m_drawDur)));
            const QRect r(x0, keyToY(m_drawKey) + 1, std::max(2, x1 - x0),
                          std::max(2, m_sv->keyHeight() - 1));
            QColor c = SongView::trackColor(selected);
            c.setAlpha(120 + m_lastVelocity);
            p.fillRect(r, c);
            p.setPen(QPen(SongView::trackColor(selected).darker(150), 1));
            p.drawRect(r.adjusted(0, 0, -1, -1));
            return;
        }
        if (m_drag != Drag::Move && m_drag != Drag::Resize && m_drag != Drag::ResizeLeft)
            return;
        if (m_dTick == 0 && m_dKey == 0 && m_dDur == 0)
            return;
        p.setPen(QPen(palette().color(QPalette::WindowText), 1, Qt::DashLine));
        for (const ViewNote &note : model.notes) {
            if (note.track != selected || !m_sv->isSelected(note))
                continue;
            int64_t tick, endTick;
            if (m_drag == Drag::ResizeLeft) {
                // The note-off pins the gesture; only the start moves.
                endTick = int64_t(note.endTick);
                tick = std::clamp<int64_t>(int64_t(note.startTick) + m_dTick, 0,
                                           endTick - 1);
            } else {
                tick = std::max<int64_t>(0, int64_t(note.startTick) + m_dTick);
                endTick = std::max<int64_t>(tick + 1,
                                            int64_t(note.endTick) + m_dTick + m_dDur);
            }
            const int key = std::clamp(int(note.key) + m_dKey, 0, 127);
            const int x0 = kKeyboardW + m_sv->contentX(double(tick));
            const int x1 = kKeyboardW + m_sv->contentX(double(endTick));
            p.drawRect(QRect(x0, keyToY(key) + 1, std::max(2, x1 - x0),
                             std::max(2, m_sv->keyHeight() - 1)));
        }
    }

    void showNoteMenu(QPoint pos)
    {
        SongDocument *doc = m_sv->document();
        if (!doc)
            return;
        const std::vector<DocNote> notes = resolveSelection();
        if (notes.empty())
            return;
        QMenu menu(this);
        QAction *velocity = menu.addAction(
            SongView::tr("Set velocity… (%1)").arg(notes.front().velocity));
        QAction *copy = menu.addAction(SongView::tr("Copy"));
        copy->setShortcut(QKeySequence::Copy);
        QAction *cut = menu.addAction(SongView::tr("Cut"));
        cut->setShortcut(QKeySequence::Cut);
        QAction *del = menu.addAction(SongView::tr("Delete"));
        QAction *chosen = menu.exec(mapToGlobal(pos));
        if (chosen == copy) {
            copyNotes(notes);
        } else if (chosen == cut) {
            copyNotes(notes);
            doc->deleteNotes(notes);
            m_sv->clearSelection();
        } else if (chosen == velocity) {
            bool ok = false;
            const int v = QInputDialog::getInt(
                this, SongView::tr("Note velocity"),
                SongView::tr("Velocity (1-127, plays as %1-127 in steps of 4):")
                    .arg(mid2agbEffectiveVelocity(1)),
                notes.front().velocity, 1, 127, 1, &ok);
            if (ok)
                doc->setNotesVelocity(notes, uint8_t(v));
        } else if (chosen == del) {
            doc->deleteNotes(notes);
            m_sv->clearSelection();
        }
    }

    void drawKeyboard(QPainter &p)
    {
        const int keyH = m_sv->keyHeight();
        p.fillRect(QRect(0, 0, kKeyboardW, height()), QColor(0xf4, 0xf4, 0xf4));
        QFont f = p.font();
        f.setPixelSize(std::min(10, keyH));
        p.setFont(f);
        for (int key = 0; key < 128; key++) {
            const int y = keyToY(key);
            if (y + keyH < 0 || y > height())
                continue;
            const bool sounding = key == m_soundingKey;
            if (isBlackKey(key)) {
                p.fillRect(QRect(0, y, kKeyboardW * 3 / 5, keyH),
                           sounding ? m_sv->palette().color(QPalette::Highlight)
                                    : QColor(0x2e, 0x2e, 0x2e));
            } else {
                if (sounding)
                    p.fillRect(QRect(0, y, kKeyboardW, keyH),
                               m_sv->palette().color(QPalette::Highlight));
                if (key % 12 == 0) {
                    p.setPen(QColor(0x9a, 0x9a, 0x9a));
                    p.drawLine(0, y + keyH, kKeyboardW, y + keyH);
                    p.setPen(QColor(0x50, 0x50, 0x50));
                    if (keyH >= 7)
                        p.drawText(QRect(0, y, kKeyboardW - 3, keyH),
                                   Qt::AlignRight | Qt::AlignVCenter, keyName(key));
                }
            }
        }
        p.setPen(m_sv->palette().color(QPalette::Mid));
        p.drawLine(kKeyboardW - 1, 0, kKeyboardW - 1, height());
    }

    // Selects the selected track's notes intersecting the band rect.
    void selectBand(const QRect &band, bool additive)
    {
        std::vector<SongView::NoteId> ids = additive
                                                ? m_sv->selection()
                                                : std::vector<SongView::NoteId>();
        for (const ViewNote &note : m_sv->model().notes) {
            if (note.track != m_sv->selectedTrack())
                continue;
            if (!noteRect(note).intersects(band))
                continue;
            const SongView::NoteId id{note.startTick, note.key};
            if (std::find(ids.begin(), ids.end(), id) == ids.end())
                ids.push_back(id);
        }
        m_sv->setSelection(std::move(ids));
    }

    SongView *m_sv;
    Drag m_drag = Drag::None;
    QPoint m_pressPos;
    QPoint m_curPos;
    double m_pressTick = 0.0;
    int m_pressKey = 0;
    int64_t m_dTick = 0;
    int m_dKey = 0;
    int64_t m_dDur = 0;
    int m_dVel = 0;
    uint64_t m_drawTick = 0;   // pending note of a draw gesture
    int64_t m_drawDur = 0;
    int m_drawKey = 0;         // follows the cursor vertically mid-draw
    uint64_t m_drawAnchor = 0; // grid cell pressed; drags pivot around it
    bool m_leftPress = false;  // left button held on empty space; cursor
                               // move vs. draw undecided
    bool m_rightPress = false; // right button held; band vs. menu undecided
    bool m_rightShift = false; // …with Shift: drag sweeps a time selection
    uint64_t m_rightAnchorTick = 0; // snapped tick of the right press
    bool m_rightHit = false;   // that press landed on a note…
    SongView::NoteId m_rightHitId{}; // …this one
    ViewNote m_velAnchor{};    // pressed note of a velocity drag (a copy)
    int m_velAudEff = -1;      // last effective velocity auditioned mid-drag
    int m_kbdKey = -1;         // key sounding from a keyboard-column press
    int m_soundingKey = -1;    // auditioned key highlighted on the keyboard
    bool m_auditioned = false; // a drag/draw preview note is sounding
    uint8_t m_lastVelocity = 100;
    bool m_panning = false;    // middle-drag pan
    QPoint m_panPos;           // last pan sample, global coords
};

// ----------------------------------------------------------- AutomationArea

class AutomationArea : public QWidget
{
public:
    AutomationArea(SongView *sv, QScrollArea *scroll)
        : QWidget(nullptr), m_sv(sv), m_scroll(scroll) // parented by the scroll area
    {
        setMinimumHeight(kLaneH);
        setMouseTracking(true); // divider hover cursor
        // Range shortcuts (copy/cut/delete/paste on the time selection) work
        // from the lanes area too; a click focuses it, like the roll.
        setFocusPolicy(Qt::ClickFocus);
    }

    // View-state plumbing for the .porydaw sidecar: the shared row height
    // plus the individually-resized rows (keyed by rowKey). laneH <= 0
    // resets to the default.
    int laneHeight() const { return m_laneH; }
    const QHash<QString, int> &rowHeightOverrides() const { return m_rowHeights; }
    void setViewHeights(int laneH, const QHash<QString, int> &overrides)
    {
        m_laneH = laneH > 0 ? std::clamp(laneH, kMinLaneH, kMaxLaneH) : kLaneH;
        m_rowHeights.clear();
        for (auto it = overrides.begin(); it != overrides.end(); ++it)
            m_rowHeights.insert(it.key(), std::clamp(it.value(), kMinLaneH, kMaxLaneH));
        applyHeight();
        update();
    }

    void rebuildRows()
    {
        m_rows.clear();
        m_dragRow = -1;
        m_resizeRow = -1;
        m_gesture = Gesture::None;
        m_sweep.clear();
        m_rightPress = false;
        m_selSweep = false;
        if (m_sv->timeline()) {
            m_rows.push_back({Row::Tempo, nullptr});
            const SongViewModel &model = m_sv->model();
            const int selected = m_sv->selectedTrack();
            // The voice row shows whenever the track has changes; with a
            // document attached it is always present as the place to add one.
            bool voiceRow = m_sv->document() != nullptr;
            for (const VoiceChange &vc : model.voices) {
                if (vc.track == selected) {
                    voiceRow = true;
                    break;
                }
            }
            if (voiceRow)
                m_rows.push_back({Row::Voice, nullptr});
            for (const AutoLane &lane : model.lanes)
                if (lane.track == selected)
                    m_rows.push_back({Row::Lane, &lane});
        }
        applyHeight();
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), palette().color(QPalette::Window));
        if (!m_sv->timeline())
            return;

        int rowY = 0;
        for (size_t i = 0; i < m_rows.size(); i++) {
            const int h = rowHeight(m_rows[i]);
            paintRow(p, m_rows[i], QRect(0, rowY, width(), h));
            rowY += h;
        }

        if (m_sv->document()) {
            const QRect strip = addLaneRect();
            p.setPen(palette().color(QPalette::Highlight));
            p.drawText(strip.adjusted(8, 0, -8, 0), Qt::AlignLeft | Qt::AlignVCenter,
                       SongView::tr("+ Add lane"));
        }

        // Drag preview: the pending stream (sweep) or ramp (line), plus a
        // marker with the value the gesture will commit at the cursor.
        if (m_dragRow >= 0 && m_dragRow < int(m_rows.size())) {
            int minV, maxV;
            rowRange(m_rows[m_dragRow], &minV, &maxV);
            const int top = rowTop(m_dragRow) + 5;
            const int bottom = rowBottom(m_dragRow) - 1 - 4;
            auto valueY = [&](int v) {
                return bottom - (v - minV) * (bottom - top) / std::max(1, maxV - minV);
            };
            auto tickX = [&](uint64_t t) {
                return kGutterW + m_sv->contentX(double(t));
            };
            p.setClipRect(QRect(kGutterW, rowTop(m_dragRow), width() - kGutterW,
                                rowHeight(m_rows[m_dragRow])));
            p.setPen(QPen(palette().color(QPalette::WindowText), 1));
            p.setBrush(Qt::NoBrush);
            if (m_gesture == Gesture::Sweep && m_sweep.size() > 1) {
                // Hold-value steps, like paintCurve draws committed points.
                for (size_t i = 0; i + 1 < m_sweep.size(); i++) {
                    const int y = valueY(m_sweep[i].second);
                    p.drawLine(tickX(m_sweep[i].first), y,
                               tickX(m_sweep[i + 1].first), y);
                    p.drawLine(tickX(m_sweep[i + 1].first), y,
                               tickX(m_sweep[i + 1].first),
                               valueY(m_sweep[i + 1].second));
                }
            } else if (m_gesture == Gesture::Line) {
                p.drawLine(tickX(m_lineStartTick), valueY(m_lineStartValue),
                           tickX(m_dragTick), valueY(m_dragValue));
            }
            const int x = tickX(m_dragTick);
            const int y = valueY(m_dragValue);
            p.drawEllipse(QPoint(x, y), 3, 3);
            p.drawText(QPoint(x + 6, y - 4),
                       formatRowValue(m_rows[m_dragRow], m_dragValue));
            p.setClipping(false);
        }
    }

    void wheelEvent(QWheelEvent *event) override
    {
        // Same bindings as the roll's notes area: plain wheel over the plot
        // zooms the timeline; Ctrl+wheel resizes the lane rows (the roll's
        // key-height analog); Shift (or a trackpad's horizontal delta)
        // scrolls horizontally. Over the gutter the wheel pages the lane
        // list vertically via the scroll area.
        const QPoint delta = event->angleDelta();
        const int d = delta.y() ? delta.y() : delta.x();
        if (event->modifiers() & Qt::ControlModifier) {
            zoomLaneHeight(d, int(event->position().y()));
        } else if (event->modifiers() & Qt::ShiftModifier) {
            m_sv->scrollByPx(-d);
        } else if (delta.x() && !delta.y()) {
            m_sv->scrollByPx(-delta.x());
        } else if (event->position().x() < kGutterW) {
            event->ignore();
            return;
        } else {
            m_sv->zoomAroundContentX(std::pow(1.0015, delta.y()),
                                     int(event->position().x()) - kGutterW);
        }
        event->accept();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::MiddleButton) {
            // Reaper-style pan: drag scrolls the timeline and the lane list.
            // Tracked in global coords — the vertical scroll moves this
            // widget under the cursor, so local deltas would double-count.
            m_panning = true;
            m_panPos = event->globalPosition().toPoint();
            setCursor(Qt::ClosedHandCursor);
            return;
        }
        const int boundary = event->button() == Qt::LeftButton
                                 ? rowBoundaryAt(event->pos().y())
                                 : -1;
        if (boundary >= 0) {
            // Dragging the divider under a row gives it an individual
            // height, overriding the shared Ctrl+wheel height.
            m_resizeRow = boundary;
            m_resizeOrigH = rowHeight(m_rows[boundary]);
            m_resizePressY = event->pos().y();
            return;
        }
        SongDocument *doc = m_sv->document();
        if (!doc)
            return;
        if (event->button() == Qt::LeftButton
            && addLaneRect().contains(event->pos())) {
            showAddLaneMenu(event->globalPosition().toPoint());
            return;
        }
        const int ri = rowIndexAt(event->pos().y());
        if (ri < 0)
            return;
        setFocus();
        const Row &row = m_rows[ri];
        if (event->pos().x() < kGutterW) {
            if (row.kind == Row::Lane
                && (event->button() == Qt::LeftButton
                    || event->button() == Qt::RightButton))
                showLaneMenu(*row.lane, event->globalPosition().toPoint());
            return;
        }
        if (event->button() == Qt::RightButton) {
            // Deferred: a drag from here sweeps a time selection across the
            // crossed rows; releasing in place context-acts (menu inside the
            // selection, point/voice-marker delete elsewhere). Resolved in
            // mouseReleaseEvent.
            m_rightPress = true;
            m_rightPressPos = event->pos();
            m_rightRow = ri;
            m_selAnchorTick = m_sv->snapTick(rawTickAt(event->pos().x()),
                                             event->modifiers() & Qt::AltModifier);
            return;
        }
        if (row.kind == Row::Voice) {
            voiceRowPress(event);
            return;
        }
        uint8_t cc;
        int track;
        if (!rowTarget(row, &cc, &track))
            return;

        if (event->button() != Qt::LeftButton)
            return;
        m_dragRow = ri;
        const bool fine = event->modifiers() & Qt::AltModifier;
        updateDrag(event->pos(), fine, event->modifiers() & Qt::ControlModifier);
        const LanePoint *grab = grabPoint(row, ri, event->pos());
        if (event->modifiers() & Qt::ShiftModifier) {
            // Line ramp: the press anchors one end, release commits the
            // interpolated segment (checked before the point grab so a
            // ramp can start exactly on an existing point).
            m_gesture = Gesture::Line;
            m_lineStartTick = m_dragTick;
            m_lineStartValue = m_dragValue;
        } else if (grab) {
            // Grabbing requires hitting the point's dot (x and y), so a
            // freehand redraw over a dense curve isn't captured by every
            // cell's point — sweeping overwrites them instead.
            m_gesture = Gesture::Point;
            m_dragOrigTick = int64_t(grab->tick);
            // Start from the point's exact position, not the pixel-derived
            // one: a no-motion click (or the first half of a double-click)
            // must not quantize the value to the pixel grid.
            m_dragTick = grab->tick;
            m_dragValue = grab->value;
        } else {
            // Freehand sweep; a no-motion click degenerates to a single
            // point (overwriting any point already on that tick).
            m_gesture = Gesture::Sweep;
            m_dragOrigTick = -1;
            m_sweep.assign(1, {m_dragTick, m_dragValue});
            m_prevTick = rawTickAt(event->pos().x());
            m_prevValue = m_dragValue;
        }
        update();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_panning) {
            const QPoint pos = event->globalPosition().toPoint();
            const QPoint d = pos - m_panPos;
            m_panPos = pos;
            m_sv->scrollByPx(-d.x());
            if (m_scroll) {
                QScrollBar *vbar = m_scroll->verticalScrollBar();
                vbar->setValue(vbar->value() - d.y());
            }
            return;
        }
        if (m_resizeRow >= 0 && m_resizeRow < int(m_rows.size())) {
            const int newH =
                std::clamp(m_resizeOrigH + event->pos().y() - m_resizePressY,
                           kMinLaneH, kMaxLaneH);
            if (newH != rowHeight(m_rows[m_resizeRow])) {
                m_rowHeights.insert(rowKey(m_rows[m_resizeRow]), newH);
                applyHeight();
                update();
            }
            return;
        }
        if (m_rightPress) {
            if (!m_selSweep
                && (event->pos() - m_rightPressPos).manhattanLength()
                       >= QApplication::startDragDistance())
                m_selSweep = true;
            if (m_selSweep)
                updateSelSweep(event);
            return;
        }
        if (m_dragRow < 0) {
            setCursor(rowBoundaryAt(event->pos().y()) >= 0 ? Qt::SplitVCursor
                                                           : Qt::ArrowCursor);
            return;
        }
        const bool fine = event->modifiers() & Qt::AltModifier;
        updateDrag(event->pos(), fine, event->modifiers() & Qt::ControlModifier);
        if (m_gesture == Gesture::Sweep)
            extendSweep(event->pos(), fine);
        update();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::MiddleButton && m_panning) {
            m_panning = false;
            setCursor(Qt::ArrowCursor);
            return;
        }
        if (event->button() == Qt::RightButton && m_rightPress) {
            m_rightPress = false;
            if (m_selSweep) {
                m_selSweep = false;
                if (m_sv->timeSelection().active())
                    m_sv->announceTimeSelection();
                else
                    m_sv->clearTimeSelection();
            } else {
                rightClickInPlace(event);
            }
            return;
        }
        if (event->button() == Qt::LeftButton && m_resizeRow >= 0) {
            m_resizeRow = -1;
            return;
        }
        if (event->button() != Qt::LeftButton || m_dragRow < 0
            || m_dragRow >= int(m_rows.size()))
            return;
        const Row &row = m_rows[m_dragRow];
        const Gesture gesture = m_gesture;
        m_gesture = Gesture::None;
        m_dragRow = -1;
        update();

        SongDocument *doc = m_sv->document();
        uint8_t cc;
        int track;
        if (!doc || !rowTarget(row, &cc, &track))
            return;
        if (gesture == Gesture::Point) {
            if (m_dragOrigTick < 0)
                return;
            DocLanePoint pt;
            // Skip the no-op commit: a plain click on a point (including the
            // first half of a double-click) must not touch the document.
            if (doc->findLanePoint(track, cc, uint64_t(m_dragOrigTick), &pt)
                && (pt.tick != m_dragTick || pt.value != m_dragValue))
                doc->moveLanePoint(track, cc, pt, m_dragTick, m_dragValue);
        } else if (gesture == Gesture::Sweep) {
            // writeLanePoints even for a plain click: it overwrites any
            // point already sitting on the tick instead of duplicating it.
            if (!m_sweep.empty())
                doc->writeLanePoints(track, cc, m_sweep.front().first,
                                     m_sweep.back().first, sweepPoints());
            m_sweep.clear();
        } else if (gesture == Gesture::Line) {
            const bool fine = event->modifiers() & Qt::AltModifier;
            uint64_t a = m_lineStartTick, b = m_dragTick;
            int va = m_lineStartValue, vb = m_dragValue;
            if (a > b) {
                std::swap(a, b);
                std::swap(va, vb);
            }
            if (a == b) {
                doc->writeLanePoints(track, cc, a, a, {{a, vb}});
                return;
            }
            const uint64_t g =
                std::max<uint64_t>(1, fine ? m_sv->fineGridTicks() : m_sv->gridTicks());
            std::vector<SongDocument::LanePointValue> pts;
            for (uint64_t t = a; t < b; t += g)
                pts.push_back({t, va
                                      + int(std::llround(double(vb - va) * double(t - a)
                                                         / double(b - a)))});
            pts.push_back({b, vb});
            doc->writeLanePoints(track, cc, a, b, pts);
        }
    }

    // Double-click: type-in for the exact value the pixel grid can't hit
    // (e.g. pan dead-center). The first click of the pair already ran as a
    // normal click, so on empty space a point exists at the snapped tick by
    // now — either way this edits the point on that tick via an overwriting
    // single-point write.
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        SongDocument *doc = m_sv->document();
        if (!doc || event->button() != Qt::LeftButton
            || event->pos().x() < kGutterW)
            return;
        const int ri = rowIndexAt(event->pos().y());
        if (ri < 0)
            return;
        const Row &row = m_rows[ri];
        uint8_t cc;
        int track;
        if (!rowTarget(row, &cc, &track))
            return;
        // The double-click replaced this pair's second press; drop any
        // half-open gesture so its release is a no-op.
        m_gesture = Gesture::None;
        m_dragRow = -1;
        m_sweep.clear();
        update();

        uint64_t tick;
        int value;
        if (const LanePoint *nearPt = nearestPoint(row, event->pos().x())) {
            tick = nearPt->tick;
            value = nearPt->value;
        } else {
            // The click's point can sit farther than nearestPoint's radius
            // when the snap grid is coarse; re-derive its tick the same way.
            tick = m_sv->snapTick(rawTickAt(event->pos().x()),
                                  event->modifiers() & Qt::AltModifier);
            DocLanePoint pt;
            value = doc->findLanePoint(track, cc, tick, &pt)
                        ? pt.value
                        : valueAtY(row, ri, event->pos().y());
        }
        if (!editValue(row, &value))
            return;
        doc->writeLanePoints(track, cc, tick, tick, {{tick, value}});
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (m_sv->handleEditKey(event))
            return;
        if (event->key() == Qt::Key_Escape) {
            m_rightPress = false;
            m_selSweep = false;
            m_sv->clearTimeSelection();
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }

private:
    struct Row {
        enum Kind { Tempo, Voice, Lane } kind;
        const AutoLane *lane;
    };

    // The lane identity a row contributes to a lane-scoped time selection:
    // (engine track, cc), with the global tempo row as track -1 so it
    // survives track switches.
    std::pair<int, uint8_t> rowIdentity(const Row &row) const
    {
        switch (row.kind) {
        case Row::Tempo:
            return {-1, DOC_CC_TEMPO};
        case Row::Voice:
            return {m_sv->selectedTrack(), DOC_CC_VOICE};
        case Row::Lane:
            return {row.lane->track, row.lane->cc};
        }
        return {-1, 0};
    }

    // Live update of a right-drag selection sweep: the tick span between the
    // press anchor and the cursor, across every row the drag crosses.
    void updateSelSweep(QMouseEvent *event)
    {
        if (m_rightRow < 0 || m_rightRow >= int(m_rows.size()))
            return;
        const bool fine = event->modifiers() & Qt::AltModifier;
        const uint64_t tick = m_sv->snapTick(rawTickAt(event->pos().x()), fine);
        SongView::TimeSelection sel;
        sel.startTick = std::min(m_selAnchorTick, tick);
        sel.endTick = std::max(m_selAnchorTick, tick);
        sel.scope = SongView::TimeSelection::Lanes;
        int r0 = m_rightRow;
        int r1 = rowIndexAt(
            std::clamp(event->pos().y(), 0, rowTop(int(m_rows.size())) - 1));
        if (r1 < 0)
            r1 = r0;
        if (r0 > r1)
            std::swap(r0, r1);
        for (int ri = r0; ri <= r1 && ri < int(m_rows.size()); ri++)
            sel.lanes.push_back(rowIdentity(m_rows[ri]));
        m_sv->setTimeSelection(sel);
    }

    // Right release without a drag: menu inside the time selection, voice-
    // marker or point delete elsewhere, and clearing the selection over
    // empty space (mirroring the roll).
    void rightClickInPlace(QMouseEvent *event)
    {
        SongDocument *doc = m_sv->document();
        if (!doc || m_rightRow < 0 || m_rightRow >= int(m_rows.size()))
            return;
        const Row &row = m_rows[m_rightRow];
        const std::pair<int, uint8_t> id = rowIdentity(row);
        const SongView::TimeSelection &sel = m_sv->timeSelection();
        const double tick = rawTickAt(event->pos().x());
        if (sel.active() && m_sv->timeSelectionCoversRow(id.first, id.second)
            && tick >= double(sel.startTick) && tick < double(sel.endTick)) {
            m_sv->showTimeSelectionMenu(event->globalPosition().toPoint());
            return;
        }
        if (row.kind == Row::Voice) {
            DocLanePoint hit;
            if (voiceChangeNear(event->pos().x(), &hit))
                doc->deleteLanePoints(m_sv->selectedTrack(), DOC_CC_VOICE, {hit});
            return;
        }
        uint8_t cc;
        int track;
        if (!rowTarget(row, &cc, &track))
            return;
        if (const LanePoint *nearPt = nearestPoint(row, event->pos().x())) {
            DocLanePoint pt;
            if (doc->findLanePoint(track, cc, nearPt->tick, &pt))
                doc->deleteLanePoints(track, cc, {pt});
            return;
        }
        m_sv->clearTimeSelection();
    }

    // Voice change within the marker hit radius of x, if any.
    bool voiceChangeNear(int x, DocLanePoint *out) const
    {
        SongDocument *doc = m_sv->document();
        if (!doc)
            return false;
        bool found = false;
        int bestDist = 9; // same radius as nearestPoint
        for (const DocLanePoint &pt :
             doc->lanePoints(m_sv->selectedTrack(), DOC_CC_VOICE)) {
            const int dist =
                std::abs(kGutterW + m_sv->contentX(double(pt.tick)) - x);
            if (dist < bestDist) {
                bestDist = dist;
                *out = pt;
                found = true;
            }
        }
        return found;
    }

    // Per-row geometry: individually-resized rows (divider drag) override
    // the shared m_laneH. Keys survive track switches, so each lane keeps
    // its height when the user comes back to the track.
    QString rowKey(const Row &row) const
    {
        switch (row.kind) {
        case Row::Tempo:
            return QStringLiteral("tempo");
        case Row::Voice:
            return QStringLiteral("voice:%1").arg(m_sv->selectedTrack());
        case Row::Lane:
            return QStringLiteral("cc:%1:%2").arg(row.lane->track).arg(row.lane->cc);
        }
        return QString();
    }

    int rowHeight(const Row &row) const
    {
        const auto it = m_rowHeights.constFind(rowKey(row));
        return it != m_rowHeights.constEnd() ? it.value() : m_laneH;
    }

    // Top of row `index`; index == m_rows.size() gives the total height.
    int rowTop(int index) const
    {
        int y = 0;
        for (int i = 0; i < index && i < int(m_rows.size()); i++)
            y += rowHeight(m_rows[i]);
        return y;
    }

    int rowBottom(int index) const { return rowTop(index) + rowHeight(m_rows[index]); }

    int rowIndexAt(int y) const
    {
        if (y < 0)
            return -1;
        int bottom = 0;
        for (size_t i = 0; i < m_rows.size(); i++) {
            bottom += rowHeight(m_rows[i]);
            if (y < bottom)
                return int(i);
        }
        return -1;
    }

    // Divider hit test: the bottom edge of row i (±3 px) starts an
    // individual-height drag for that row.
    int rowBoundaryAt(int y) const
    {
        int bottom = 0;
        for (size_t i = 0; i < m_rows.size(); i++) {
            bottom += rowHeight(m_rows[i]);
            if (std::abs(y - bottom) <= 3)
                return int(i);
        }
        return -1;
    }

    QRect addLaneRect() const
    {
        return QRect(0, rowTop(int(m_rows.size())), width(), kAddLaneH);
    }

    void applyHeight()
    {
        // Minimum, not fixed: the scroll area stretches the widget to fill
        // its viewport when the user drags the lanes area taller.
        const int addH = m_sv->timeline() && m_sv->document() ? kAddLaneH : 0;
        setMinimumHeight(std::max(m_laneH, rowTop(int(m_rows.size())) + addH));
    }

    // Ctrl+wheel: rescale the lane rows (the roll's key-height analog),
    // keeping the row under the cursor pinned. anchorY is widget-local, so
    // it already includes the scroll offset. Individually-resized rows
    // scale by the same factor, keeping their proportions.
    void zoomLaneHeight(int wheelDelta, int anchorY)
    {
        m_laneZoomAccum += wheelDelta;
        const int steps = m_laneZoomAccum / 120;
        if (steps == 0)
            return;
        m_laneZoomAccum -= steps * 120;
        const int newH = std::clamp(m_laneH + steps * 4, kMinLaneH, kMaxLaneH);
        if (newH == m_laneH)
            return;
        const double factor = double(newH) / double(m_laneH);
        for (auto it = m_rowHeights.begin(); it != m_rowHeights.end(); ++it)
            it.value() = std::clamp(int(std::lround(it.value() * factor)),
                                    kMinLaneH, kMaxLaneH);
        m_laneH = newH;
        applyHeight();
        if (m_scroll) {
            QScrollBar *vbar = m_scroll->verticalScrollBar();
            const int viewportY = anchorY - vbar->value();
            vbar->setValue(int(std::lround(anchorY * factor)) - viewportY);
        }
        update();
    }

    // Menu of §4.2 audible parameters without a lane on the selected track.
    void showAddLaneMenu(const QPoint &globalPos)
    {
        const int track = m_sv->selectedTrack();
        QMenu menu;
        static constexpr uint8_t kAudibleCcs[] = {0x01, 0x07, 0x0A, 0x14, 0x15,
                                                  LANE_CC_BEND};
        for (uint8_t cc : kAudibleCcs) {
            if (m_sv->model().findLane(track, cc))
                continue;
            QString label;
            if (cc == LANE_CC_BEND) {
                label = SongView::tr("Pitch bend (BEND)");
            } else {
                const M4aCcInfo info = m4aClassifyCc(cc);
                label = QStringLiteral("%1 (%2)").arg(QLatin1String(info.display),
                                                      QLatin1String(info.name));
            }
            menu.addAction(label)->setData(int(cc));
        }
        if (menu.isEmpty())
            menu.addAction(SongView::tr("All parameters already have lanes"))
                ->setEnabled(false);
        QAction *chosen = menu.exec(globalPos);
        if (chosen && chosen->data().isValid())
            m_sv->addEmptyLane(track, uint8_t(chosen->data().toInt()));
    }

    // Gutter menu on a CC/bend lane: clear its events (the lane stays,
    // empty), or delete the lane outright — confirmed first while it still
    // has events, since that throws the whole curve away in one step.
    void showLaneMenu(const AutoLane &lane, const QPoint &globalPos)
    {
        // Copies: the menu's actions mutate the model, and lane points into it.
        const int track = lane.track;
        const uint8_t cc = lane.cc;
        const QString name = lane.name;
        const bool empty = lane.points.empty();

        QMenu menu;
        QAction *copyLane = menu.addAction(SongView::tr("Copy lane"));
        copyLane->setEnabled(!empty);
        QAction *pasteLane = menu.addAction(SongView::tr("Paste lane (replace)"));
        // Only whole-lane clips (their ticks are absolute) paste here; range
        // clips are anchored at the edit cursor instead.
        {
            const SongView::Clip &clip = m_sv->clipboard();
            pasteLane->setEnabled(clip.wholeLane && clip.lanes.size() == 1
                                  && !clip.lanes.front().points.empty());
        }
        menu.addSeparator();
        QAction *clear = menu.addAction(SongView::tr("Clear events"));
        clear->setEnabled(!empty);
        QAction *del = menu.addAction(empty ? SongView::tr("Remove empty lane")
                                            : SongView::tr("Delete lane"));
        QAction *chosen = menu.exec(globalPos);
        if (!chosen)
            return;

        SongDocument *doc = m_sv->document();
        const std::vector<DocLanePoint> points = doc->lanePoints(track, cc);
        if (chosen == copyLane) {
            if (points.empty())
                return;
            SongView::Clip clip;
            clip.wholeLane = true;
            clip.span = points.back().tick + 1;
            SongView::ClipLane cl{track, cc, {}};
            for (const DocLanePoint &pt : points)
                cl.points.push_back({uint32_t(pt.tick), pt.value});
            clip.lanes.push_back(std::move(cl));
            m_sv->clipboard() = std::move(clip);
            m_sv->announce(SongView::tr("Copied the %1 lane (%n point(s))", nullptr,
                                        int(points.size()))
                               .arg(name));
            return;
        }
        if (chosen == pasteLane) {
            // Replace this lane's whole contents with the clipboard lane at
            // its original ticks (values clamp to this lane's range), as one
            // undoable command.
            SongDocument::RangeEdit edit;
            edit.removePoints = points;
            SongDocument::RangeEdit::LaneWrite lw{track, cc, {}};
            for (const std::pair<uint32_t, int> &pv :
                 m_sv->clipboard().lanes.front().points)
                lw.points.push_back({uint64_t(pv.first), pv.second});
            edit.addPoints.push_back(std::move(lw));
            doc->applyRangeEdit(SongView::tr("paste lane"), edit);
            m_sv->announce(SongView::tr("Replaced the %1 lane").arg(name));
            return;
        }
        if (chosen == clear) {
            if (points.empty())
                return;
            // Keep the row alive as an empty lane once its events go.
            m_sv->addEmptyLane(track, cc);
            doc->deleteLanePoints(track, cc, points);
        } else if (chosen == del) {
            if (!points.empty()
                && QMessageBox::question(
                       this, SongView::tr("Delete lane"),
                       SongView::tr("Delete the %1 lane and its %2 events?")
                           .arg(name)
                           .arg(points.size()))
                       != QMessageBox::Yes)
                return;
            m_sv->removeEmptyLane(track, cc); // forget the view state first
            if (!points.empty())
                doc->deleteLanePoints(track, cc, points);
        }
    }

    // Voice row, left button only (right-button gestures are handled by the
    // shared deferral): click a marker to re-pick its voice, click empty
    // space to insert a change at the snapped tick; release-in-place
    // right-clicks delete a marker via rightClickInPlace. The value axis is
    // meaningless here (a voice is an identity, not a level), so the picker
    // dialog replaces the lanes' drag editing.
    void voiceRowPress(QMouseEvent *event)
    {
        SongDocument *doc = m_sv->document();
        const int track = m_sv->selectedTrack();
        if (event->button() != Qt::LeftButton)
            return;
        DocLanePoint hitPt;
        if (voiceChangeNear(event->pos().x(), &hitPt)) {
            const DocLanePoint *hit = &hitPt;
            int voice = hit->value;
            if (m_sv->pickVoice(SongView::tr("Change voice"), hit->value, &voice)
                && voice != hit->value)
                doc->moveLanePoint(track, DOC_CC_VOICE, *hit, hit->tick, voice);
        } else {
            const std::vector<DocLanePoint> changes =
                doc->lanePoints(track, DOC_CC_VOICE);
            const uint64_t tick = m_sv->snapTick(
                m_sv->tickAtContentX(std::max(kGutterW, event->pos().x()) - kGutterW));
            // Preselect the voice already sounding at that tick.
            int voice = 0;
            for (const DocLanePoint &pt : changes) {
                if (pt.tick > tick)
                    break;
                voice = pt.value;
            }
            if (m_sv->pickVoice(SongView::tr("Insert voice change"), voice, &voice))
                doc->addLanePoint(track, DOC_CC_VOICE, tick, voice);
        }
    }

    // Document target of an editable row; false for the voice row.
    bool rowTarget(const Row &row, uint8_t *cc, int *track) const
    {
        if (row.kind == Row::Tempo) {
            *cc = DOC_CC_TEMPO;
            *track = m_sv->selectedTrack();
            return true;
        }
        if (row.kind == Row::Lane) {
            *cc = row.lane->cc; // LANE_CC_BEND == DOC_CC_BEND
            *track = row.lane->track;
            return true;
        }
        return false;
    }

    const std::vector<LanePoint> *rowPoints(const Row &row) const
    {
        if (row.kind == Row::Tempo)
            return &m_sv->model().tempoLane;
        if (row.kind == Row::Lane)
            return &row.lane->points;
        return nullptr;
    }

    void rowRange(const Row &row, int *minV, int *maxV) const
    {
        *minV = 0;
        *maxV = 127;
        if (row.kind == Row::Tempo) {
            *maxV = 200;
            for (const LanePoint &pt : m_sv->model().tempoLane)
                *maxV = std::max(*maxV, pt.value + 20);
        } else if (row.kind == Row::Lane && row.lane->cc == LANE_CC_BEND) {
            *minV = -8192;
            *maxV = 8191;
        }
    }

    QString rowTitle(const Row &row) const
    {
        switch (row.kind) {
        case Row::Tempo:
            return SongView::tr("Tempo (BPM)");
        case Row::Voice:
            return SongView::tr("Voice");
        case Row::Lane: {
            if (row.lane->cc == LANE_CC_BEND)
                return SongView::tr("Pitch bend (BEND)");
            const M4aCcInfo info = m4aClassifyCc(row.lane->cc);
            return QStringLiteral("%1 (%2)").arg(row.lane->name,
                                                 QLatin1String(info.name));
        }
        }
        return QString();
    }

    // The m4a display convention used elsewhere in the app (PAN/TUNE as
    // c_v±, bend signed): a raw "64" for pan hides that it IS center.
    QString formatRowValue(const Row &row, int v) const
    {
        if (row.kind == Row::Lane) {
            if (row.lane->cc == LANE_CC_BEND)
                return m4aFormatBend(v);
            return m4aFormatCcValue(row.lane->cc, uint8_t(v));
        }
        return QString::number(v);
    }

    // Neutral value a Ctrl-drag magnetizes to; only lanes where "centered"
    // is meaningful and hard to hit by eye.
    bool rowDetent(const Row &row, int *value) const
    {
        if (row.kind != Row::Lane)
            return false;
        if (row.lane->cc == 0x0A || row.lane->cc == 0x18) { // PAN/TUNE: c_v 0
            *value = 64;
            return true;
        }
        if (row.lane->cc == LANE_CC_BEND) {
            *value = 0;
            return true;
        }
        return false;
    }

    // Type-in editor: the only way to hit an arbitrary exact value, since a
    // pixel spans several value units at normal lane heights. Input uses the
    // display convention; PAN/TUNE entry is c_v (stored value minus 64).
    bool editValue(const Row &row, int *value)
    {
        int minShown = 0, maxShown = 127, offset = 0; // stored = shown + offset
        QString label = SongView::tr("Value:");
        if (row.kind == Row::Tempo) {
            minShown = 1;
            maxShown = 999;
            label = SongView::tr("BPM:");
        } else if (row.lane->cc == LANE_CC_BEND) {
            minShown = -8192;
            maxShown = 8191;
            label = SongView::tr("Bend (0 = none):");
        } else if (row.lane->cc == 0x0A || row.lane->cc == 0x18) {
            minShown = -64;
            maxShown = 63;
            offset = 64;
            label = SongView::tr("c_v value (0 = center):");
        }
        bool ok = false;
        const int shown = QInputDialog::getInt(this, rowTitle(row), label,
                                               *value - offset, minShown,
                                               maxShown, 1, &ok);
        if (ok)
            *value = shown + offset;
        return ok;
    }

    const LanePoint *nearestPoint(const Row &row, int x) const
    {
        const std::vector<LanePoint> *points = rowPoints(row);
        if (!points)
            return nullptr;
        const LanePoint *best = nullptr;
        int bestDist = 9;
        for (const LanePoint &pt : *points) {
            const int dist = std::abs(kGutterW + m_sv->contentX(double(pt.tick)) - x);
            if (dist < bestDist) {
                bestDist = dist;
                best = &pt;
            }
        }
        return best;
    }

    // Left-press grab test: near the point's dot in BOTH x and y. A dense
    // freehand curve has a point on every grid cell, so an x-only radius
    // (nearestPoint, kept for right-click delete) would capture every press
    // and make redrawing impossible; grab the dot itself to move a point.
    const LanePoint *grabPoint(const Row &row, int ri, QPoint pos) const
    {
        const std::vector<LanePoint> *points = rowPoints(row);
        if (!points)
            return nullptr;
        int minV, maxV;
        rowRange(row, &minV, &maxV);
        // paintCurve's valueY mapping for this row.
        const int top = rowTop(ri) + 5;
        const int bottom = rowBottom(ri) - 1 - 4;
        const LanePoint *best = nullptr;
        int bestDist = INT_MAX;
        for (const LanePoint &pt : *points) {
            const int dx = kGutterW + m_sv->contentX(double(pt.tick)) - pos.x();
            const int dy = bottom
                           - (pt.value - minV) * (bottom - top) / std::max(1, maxV - minV)
                           - pos.y();
            if (std::abs(dx) > 7 || std::abs(dy) > 7)
                continue;
            const int dist = dx * dx + dy * dy;
            if (dist < bestDist) {
                bestDist = dist;
                best = &pt;
            }
        }
        return best;
    }

    double rawTickAt(int x) const
    {
        return std::max(0.0, m_sv->tickAtContentX(std::max(kGutterW, x) - kGutterW));
    }

    // Invert paintCurve's valueY mapping; ri indexes the row for geometry.
    int valueAtY(const Row &row, int ri, int yPos) const
    {
        int minV, maxV;
        rowRange(row, &minV, &maxV);
        const int top = rowTop(ri) + 5;
        const int bottom = rowBottom(ri) - 1 - 4;
        const int y = std::clamp(yPos, top, bottom);
        return minV + (bottom - y) * (maxV - minV) / std::max(1, bottom - top);
    }

    void updateDrag(QPoint pos, bool fine, bool detent)
    {
        if (m_dragRow < 0 || m_dragRow >= int(m_rows.size()))
            return;
        const Row &row = m_rows[m_dragRow];
        m_dragValue = valueAtY(row, m_dragRow, pos.y());
        if (row.kind == Row::Tempo)
            m_dragValue = std::max(1, m_dragValue);
        // Ctrl detent: magnetize to the lane's neutral value within ~8 px,
        // so dead-center doesn't require pixel-perfect aim.
        int neutral;
        if (detent && rowDetent(row, &neutral)) {
            int minV, maxV;
            rowRange(row, &minV, &maxV);
            const int plotH = std::max(1, rowHeight(row) - 10); // bottom - top
            if (std::abs(m_dragValue - neutral) <= (maxV - minV) * 8 / plotH)
                m_dragValue = neutral;
        }
        m_dragTick = m_sv->snapTick(rawTickAt(pos.x()), fine);
    }

    // Freehand sweep bookkeeping: fills every grid cell crossed since the
    // last mouse sample (linear interpolation, so a fast drag leaves no
    // gaps), overwriting cells swept more than once.
    void extendSweep(QPoint pos, bool fine)
    {
        const double rawTick = rawTickAt(pos.x());
        const uint64_t g =
            std::max<uint64_t>(1, fine ? m_sv->fineGridTicks() : m_sv->gridTicks());
        const double from = m_prevTick;
        const double to = rawTick;
        const uint64_t t0 = m_sv->snapTick(std::min(from, to), fine);
        const uint64_t t1 = m_sv->snapTick(std::max(from, to), fine);
        for (uint64_t t = t0; t <= t1; t += g) {
            int v = m_dragValue;
            if (to != from) {
                const double f =
                    std::clamp((double(t) - from) / (to - from), 0.0, 1.0);
                v = m_prevValue + int(std::llround(f * (m_dragValue - m_prevValue)));
            }
            sweepUpsert(t, v);
        }
        m_prevTick = rawTick;
        m_prevValue = m_dragValue;
    }

    void sweepUpsert(uint64_t tick, int value)
    {
        auto it = std::lower_bound(
            m_sweep.begin(), m_sweep.end(), tick,
            [](const std::pair<uint64_t, int> &a, uint64_t t) { return a.first < t; });
        if (it != m_sweep.end() && it->first == tick)
            it->second = value;
        else
            m_sweep.insert(it, {tick, value});
    }

    std::vector<SongDocument::LanePointValue> sweepPoints() const
    {
        std::vector<SongDocument::LanePointValue> pts;
        pts.reserve(m_sweep.size());
        for (const std::pair<uint64_t, int> &s : m_sweep)
            pts.push_back({s.first, s.second});
        return pts;
    }

    void paintRow(QPainter &p, const Row &row, const QRect &r)
    {
        const QRect plot(kGutterW, r.top(), width() - kGutterW, r.height());
        p.setClipRect(r);
        p.setPen(palette().color(QPalette::Mid));
        p.drawLine(r.left(), r.bottom(), r.right(), r.bottom());

        // Gutter label.
        const QString name = rowTitle(row);
        int minV = 0, maxV = 127;
        const std::vector<LanePoint> *points = nullptr;
        QColor curve = palette().color(QPalette::Highlight);
        rowRange(row, &minV, &maxV);
        switch (row.kind) {
        case Row::Tempo:
            points = &m_sv->model().tempoLane;
            curve = QColor(0xb0, 0x60, 0xd0);
            break;
        case Row::Voice:
            break;
        case Row::Lane:
            points = &row.lane->points;
            curve = SongView::trackColor(row.lane->track);
            break;
        }

        p.setPen(palette().color(QPalette::WindowText));
        QFont f = p.font();
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRect(8, r.top() + 4, kGutterW - 16, 14), Qt::AlignLeft, name);
        f.setBold(false);
        p.setFont(f);
        if (points && !points->empty()) {
            p.setPen(palette().color(QPalette::PlaceholderText));
            p.drawText(QRect(8, r.top() + 20, kGutterW - 16, 14), Qt::AlignLeft,
                       SongView::tr("%1 points · %2..%3")
                           .arg(points->size())
                           .arg(minV)
                           .arg(maxV));
        } else if (points && row.kind == Row::Lane) {
            p.setPen(palette().color(QPalette::PlaceholderText));
            p.drawText(QRect(8, r.top() + 20, kGutterW - 16, 14), Qt::AlignLeft,
                       SongView::tr("empty · click to add points"));
        } else if (row.kind == Row::Voice && m_sv->document()) {
            int count = 0;
            for (const VoiceChange &vc : m_sv->model().voices)
                if (vc.track == m_sv->selectedTrack())
                    count++;
            p.setPen(palette().color(QPalette::PlaceholderText));
            p.drawText(QRect(8, r.top() + 20, kGutterW - 16, 14), Qt::AlignLeft,
                       count ? SongView::tr("%n change(s) · click to edit", nullptr, count)
                             : SongView::tr("no voice set · click to add"));
        }

        p.setClipRect(plot);
        drawGrid(p, m_sv, plot, kGutterW);

        if (row.kind == Row::Voice)
            paintVoiceRow(p, plot);
        else if (points)
            paintCurve(p, plot, *points, minV, maxV, curve,
                       row.kind == Row::Lane && row.lane->cc == LANE_CC_BEND);

        const std::pair<int, uint8_t> id = rowIdentity(row);
        drawOverlays(p, m_sv, plot, kGutterW,
                     m_sv->timeSelectionCoversRow(id.first, id.second));
        p.setClipping(false);
    }

    void paintCurve(QPainter &p, const QRect &plot, const std::vector<LanePoint> &points,
                    int minV, int maxV, const QColor &color, bool centerLine)
    {
        const int top = plot.top() + 5;
        const int bottom = plot.bottom() - 4;
        auto valueY = [&](int v) {
            return bottom - (v - minV) * (bottom - top) / std::max(1, maxV - minV);
        };
        if (centerLine) {
            p.setPen(QPen(palette().color(QPalette::Mid), 1, Qt::DashLine));
            p.drawLine(plot.left(), valueY(0), plot.right(), valueY(0));
        }
        p.setPen(QPen(color, 2));
        for (size_t i = 0; i < points.size(); i++) {
            const int x0 = kGutterW + m_sv->contentX(double(points[i].tick));
            const int x1 = i + 1 < points.size()
                               ? kGutterW + m_sv->contentX(double(points[i + 1].tick))
                               : plot.right();
            if (x1 < plot.left() || x0 > plot.right())
                continue;
            const int y = valueY(points[i].value);
            p.drawLine(x0, y, x1, y); // hold value until the next point
            if (i + 1 < points.size())
                p.drawLine(x1, y, x1, valueY(points[i + 1].value));
            if (m_sv->pxPerBeat() >= 24.0)
                p.fillRect(x0 - 1, y - 1, 3, 3, color);
        }
    }

    void paintVoiceRow(QPainter &p, const QRect &plot)
    {
        const SongViewModel &model = m_sv->model();
        const int selected = m_sv->selectedTrack();
        std::vector<const VoiceChange *> changes;
        for (const VoiceChange &vc : model.voices)
            if (vc.track == selected)
                changes.push_back(&vc);

        const QColor color = SongView::trackColor(selected);
        for (size_t i = 0; i < changes.size(); i++) {
            const int x = kGutterW + m_sv->contentX(double(changes[i]->tick));
            const int xEnd = i + 1 < changes.size()
                                 ? kGutterW + m_sv->contentX(double(changes[i + 1]->tick))
                                 : plot.right();
            if (xEnd < plot.left() || x > plot.right())
                continue;
            p.setPen(QPen(color, 2));
            p.drawLine(x, plot.top() + 4, x, plot.bottom() - 4);
            p.setPen(palette().color(QPalette::WindowText));
            const QString text = QStringLiteral("%1 %2")
                                     .arg(int(changes[i]->program), 3, 10, QLatin1Char('0'))
                                     .arg(m_sv->voiceShortName(changes[i]->program));
            // Keep the label readable while its voice region is scrolled
            // partially off the left edge.
            const int textX = std::max(x + 4, plot.left() + 4);
            const int textW = std::max(10, xEnd - textX - 4);
            p.drawText(QRect(textX, plot.top() + 4, textW, plot.height() - 8),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       fontMetrics().elidedText(text, Qt::ElideRight, textW));
        }
    }

    // Left-drag gestures: Point moves an existing point (press landed near
    // one), Sweep freehand-draws a stream of points, Line (Shift) commits an
    // interpolated ramp between press and release. Alt snaps to the clock
    // grid instead of the visible grid throughout; Ctrl magnetizes the value
    // to the lane's neutral (pan/tune center, zero bend); double-click opens
    // a type-in for the exact value.
    enum class Gesture { None, Point, Sweep, Line };

    SongView *m_sv;
    QScrollArea *m_scroll;      // hosting scroll area, for lane-zoom pinning
    std::vector<Row> m_rows;
    int m_laneH = kLaneH;       // shared row height; Ctrl+wheel rescales
    int m_laneZoomAccum = 0;    // sub-notch wheel remainder, like zoomKeyHeight
    QHash<QString, int> m_rowHeights; // individual row heights (rowKey → px)
    int m_resizeRow = -1;       // row whose bottom divider is being dragged
    int m_resizeOrigH = 0;
    int m_resizePressY = 0;
    bool m_panning = false;     // middle-drag pan
    QPoint m_panPos;            // last pan sample, global coords
    bool m_rightPress = false;  // right button held; sweep vs. click undecided
    bool m_selSweep = false;    // right-drag time-selection sweep is live
    QPoint m_rightPressPos;
    int m_rightRow = -1;        // row of the right press
    uint64_t m_selAnchorTick = 0;
    Gesture m_gesture = Gesture::None;
    int m_dragRow = -1;
    int64_t m_dragOrigTick = -1; // existing point being moved, -1 = new point
    uint64_t m_dragTick = 0;
    int m_dragValue = 0;
    std::vector<std::pair<uint64_t, int>> m_sweep; // tick-sorted freehand samples
    uint64_t m_lineStartTick = 0; // Shift-drag anchor
    int m_lineStartValue = 0;
    double m_prevTick = 0.0; // last raw (unsnapped) sweep sample
    int m_prevValue = 0;
};

// ---------------------------------------------------------------- OtherStrip

class OtherStrip : public QWidget
{
public:
    explicit OtherStrip(SongView *sv)
        : QWidget(sv), m_sv(sv)
    {
        setFixedHeight(kStripH);
        setMouseTracking(true);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), palette().color(QPalette::Window).darker(104));
        p.setPen(palette().color(QPalette::Mid));
        p.drawLine(0, 0, width(), 0);

        const SongViewModel &model = m_sv->model();
        p.setPen(palette().color(QPalette::WindowText));
        p.drawText(QRect(8, 0, kGutterW - 16, height()), Qt::AlignVCenter,
                   SongView::tr("Other events (%1)").arg(model.strip.size()));
        if (!m_sv->timeline())
            return;

        const QRect area(kGutterW, 0, width() - kGutterW, height());
        p.setClipRect(area);
        drawOverlays(p, m_sv, area, kGutterW, false);

        const int cy = height() / 2;
        for (const StripItem &item : model.strip) {
            const int x = kGutterW + m_sv->contentX(double(item.tick));
            if (x < area.left() - 4 || x > area.right() + 4)
                continue;
            QColor c = item.track >= 0 ? SongView::trackColor(item.track)
                                       : palette().color(QPalette::Mid);
            QPainterPath diamond;
            diamond.moveTo(x, cy - 5);
            diamond.lineTo(x + 4, cy);
            diamond.lineTo(x, cy + 5);
            diamond.lineTo(x - 4, cy);
            diamond.closeSubpath();
            p.fillPath(diamond, c);
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const MidiTimeline *tl = m_sv->timeline();
        if (!tl || event->pos().x() < kGutterW) {
            QToolTip::hideText();
            return;
        }
        QStringList lines;
        for (const StripItem &item : m_sv->model().strip) {
            const int x = kGutterW + m_sv->contentX(double(item.tick));
            if (std::abs(x - event->pos().x()) > 4)
                continue;
            const double seconds = double(tl->sampleForTick(item.tick)) / tl->sampleRate;
            QString where = item.track >= 0
                                ? SongView::tr("Track %1").arg(item.track + 1)
                                : SongView::tr("File");
            lines << QStringLiteral("%1:%2 · %3 · %4")
                         .arg(int(seconds) / 60)
                         .arg(int(seconds) % 60, 2, 10, QLatin1Char('0'))
                         .arg(where, item.label);
            if (lines.size() >= 12) {
                lines << SongView::tr("…");
                break;
            }
        }
        if (lines.isEmpty())
            QToolTip::hideText();
        else
            QToolTip::showText(event->globalPosition().toPoint(),
                               lines.join(QStringLiteral("\n")), this);
    }

private:
    SongView *m_sv;
};

// ---------------------------------------------------------- VoicePickerDialog

// Modal instrument picker (SPEC §4.2): the voicegroup's 128 entries, the same
// list the import wizard's mapping combo renders. Press-and-hold auditions
// through the preview engine; double-click chooses.
class VoicePickerDialog : public QDialog
{
public:
    VoicePickerDialog(SongView *sv, const QString &title, int initialVoice,
                      std::function<void(int, int)> audition)
        : QDialog(sv), m_audition(std::move(audition))
    {
        setWindowTitle(title);
        resize(360, 440);
        auto *layout = new QVBoxLayout(this);
        m_list = new QListWidget(this);
        m_list->setUniformItemSizes(true);
        m_list->setToolTip(SongView::tr("Click and hold to audition (middle C)."));
        for (int v = 0; v < VOICEGROUP_SIZE; v++)
            m_list->addItem(QStringLiteral("%1  %2")
                                .arg(v, 3, 10, QLatin1Char('0'))
                                .arg(sv->voiceShortName(uint8_t(v))));
        m_list->setCurrentRow(std::clamp(initialVoice, 0, VOICEGROUP_SIZE - 1));
        m_list->scrollToItem(m_list->currentItem(), QAbstractItemView::PositionAtCenter);
        layout->addWidget(m_list, 1);

        auto *buttons =
            new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);

        connect(m_list, &QListWidget::itemPressed, this, [this](QListWidgetItem *item) {
            releaseVoice();
            if (item) {
                m_sounding = m_list->row(item);
                m_audition(m_sounding, kVoiceAuditionVel);
            }
        });
        connect(m_list, &QListWidget::itemDoubleClicked, this, [this] { accept(); });
        m_list->viewport()->installEventFilter(this);
    }

    ~VoicePickerDialog() override { releaseVoice(); }

    int selectedVoice() const { return std::max(0, m_list->currentRow()); }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_list->viewport() && event->type() == QEvent::MouseButtonRelease)
            releaseVoice();
        return QDialog::eventFilter(watched, event);
    }

private:
    void releaseVoice()
    {
        if (m_sounding < 0)
            return;
        m_audition(m_sounding, 0);
        m_sounding = -1;
    }

    QListWidget *m_list;
    std::function<void(int, int)> m_audition;
    int m_sounding = -1;
};

// ---------------------------------------------------------- TrackHeaderPanel

class TrackHeaderRow : public QWidget
{
public:
    TrackHeaderRow(SongView *sv, int track, QWidget *parent)
        : QWidget(parent), m_sv(sv), m_track(track)
    {
        setFixedHeight(kTrackRowH);
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 2, 4, 2);
        layout->addStretch();

        auto *buttons = new QVBoxLayout;
        buttons->setSpacing(2);
        m_mute = new QToolButton(this);
        m_mute->setText(QStringLiteral("M"));
        m_mute->setCheckable(true);
        m_mute->setFixedSize(20, 18);
        m_mute->setToolTip(SongView::tr("Mute"));
        m_mute->setStyleSheet(
            QStringLiteral("QToolButton:checked { background: #d9534f; color: white; }"));
        // Headers are rebuilt on every document edit; keep the persistent
        // mute/solo state (checked before connect, so nothing re-emits).
        m_mute->setChecked(sv->trackMuted(track));
        connect(m_mute, &QToolButton::toggled, this,
                [this](bool on) { m_sv->setTrackMute(m_track, on); });
        m_solo = new QToolButton(this);
        m_solo->setText(QStringLiteral("S"));
        m_solo->setCheckable(true);
        m_solo->setFixedSize(20, 18);
        m_solo->setToolTip(SongView::tr("Solo"));
        m_solo->setStyleSheet(
            QStringLiteral("QToolButton:checked { background: #5cb85c; color: white; }"));
        m_solo->setChecked(sv->trackSoloed(track));
        connect(m_solo, &QToolButton::toggled, this,
                [this](bool on) { m_sv->setTrackSolo(m_track, on); });
        buttons->addWidget(m_mute);
        buttons->addWidget(m_solo);
        layout->addLayout(buttons);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const bool selected = m_sv->selectedTrack() == m_track;
        if (selected) {
            QColor hl = palette().color(QPalette::Highlight);
            hl.setAlpha(50);
            p.fillRect(rect(), hl);
        } else if (m_sv->trackSelectionMask() & (1u << m_track)) {
            // Part of the multi-track scope (Ctrl/Shift+click), lighter than
            // the primary selection.
            QColor hl = palette().color(QPalette::Highlight);
            hl.setAlpha(22);
            p.fillRect(rect(), hl);
        }
        p.fillRect(QRect(0, 0, 4, height()), SongView::trackColor(m_track));
        p.setPen(palette().color(QPalette::Mid));
        p.drawLine(0, height() - 1, width(), height() - 1);

        const MidiTimeline *tl = m_sv->timeline();
        QString name = tl ? tl->tracks[m_track].name : QString();
        if (name.isEmpty())
            name = SongView::tr("Track %1").arg(m_track + 1);
        const int textW = width() - 36;
        QFont f = p.font();
        f.setBold(selected);
        p.setFont(f);
        p.setPen(palette().color(QPalette::WindowText));
        p.drawText(QRect(10, 4, textW, 16), Qt::AlignLeft | Qt::AlignVCenter,
                   fontMetrics().elidedText(
                       QStringLiteral("%1 · %2").arg(m_track + 1).arg(name),
                       Qt::ElideRight, textW));
        f.setBold(false);
        f.setPixelSize(std::max(9, f.pixelSize() > 0 ? f.pixelSize() - 2 : 10));
        p.setFont(f);
        p.setPen(palette().color(QPalette::PlaceholderText));
        p.drawText(QRect(10, 22, textW, 16), Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(f).elidedText(m_sv->instrumentLabel(m_track),
                                              Qt::ElideRight, textW));
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        m_sv->trackHeaderClicked(m_track, event->modifiers());
    }

    void mouseDoubleClickEvent(QMouseEvent *) override
    {
        m_sv->selectTrack(m_track);
        m_sv->editTrackVoice(m_track);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        if (!m_sv->document())
            return;
        m_sv->selectTrack(m_track);
        QMenu menu(this);
        QAction *voiceAction = menu.addAction(SongView::tr("Change voice..."));
        QAction *deleteAction = menu.addAction(SongView::tr("Delete track"));
        QAction *chosen = menu.exec(event->globalPos());
        // Queued: both edits rebuild the header panel, which deletes this
        // row out from under its own event handler.
        if (chosen == voiceAction) {
            QMetaObject::invokeMethod(
                m_sv, [sv = m_sv, t = m_track] { sv->editTrackVoice(t); },
                Qt::QueuedConnection);
        } else if (chosen == deleteAction) {
            QMetaObject::invokeMethod(
                m_sv, [sv = m_sv, t = m_track] { sv->deleteTrack(t); },
                Qt::QueuedConnection);
        }
    }

private:
    SongView *m_sv;
    int m_track;
    QToolButton *m_mute;
    QToolButton *m_solo;
};

class TrackHeaderPanel : public QWidget
{
public:
    explicit TrackHeaderPanel(SongView *sv)
        : QWidget(nullptr), m_sv(sv)
    {
        m_layout = new QVBoxLayout(this);
        m_layout->setContentsMargins(0, 0, 0, 0);
        m_layout->setSpacing(0);
        m_layout->addStretch();
    }

    void rebuild()
    {
        qDeleteAll(m_rows);
        m_rows.clear();
        const MidiTimeline *tl = m_sv->timeline();
        if (tl) {
            for (int t = 0; t < 16; t++) {
                if (!tl->tracks[t].used)
                    continue;
                auto *row = new TrackHeaderRow(m_sv, t, this);
                QString tip = SongView::tr("%1 notes · %2")
                                  .arg(tl->tracks[t].noteCount)
                                  .arg(m_sv->instrumentLabel(t));
                if (m_sv->document())
                    tip += SongView::tr(
                        "\nDouble-click to change the voice · right-click to delete");
                row->setToolTip(tip);
                m_layout->insertWidget(m_layout->count() - 1, row);
                m_rows.push_back(row);
            }
            SongDocument *doc = m_sv->document();
            if (doc && doc->canAddTrack()) {
                auto *add = new QToolButton(this);
                add->setText(SongView::tr("+ Add track"));
                add->setAutoRaise(true);
                add->setToolTip(SongView::tr("Add a track (picks its voice first)"));
                // Queued: the edit rebuilds this panel, deleting the button
                // out from under its own clicked handler.
                connect(add, &QToolButton::clicked, m_sv,
                        [sv = m_sv] { sv->addTrack(); }, Qt::QueuedConnection);
                m_layout->insertWidget(m_layout->count() - 1, add);
                m_rows.push_back(add);
            }
        }
    }

    void syncSelection()
    {
        for (QWidget *row : m_rows)
            row->update();
    }

private:
    SongView *m_sv;
    QVBoxLayout *m_layout;
    std::vector<QWidget *> m_rows;
};

} // namespace songview

// ------------------------------------------------------------------ SongView

using namespace songview;

SongView::SongView(QWidget *parent)
    : QWidget(parent)
{
    auto *vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    m_ruler = new TimeRuler(this);
    vbox->addWidget(m_ruler);

    // Roll (with headers) above, automation lanes below, split by a
    // draggable boundary; kLanesAreaH is only the initial lanes height.
    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->setChildrenCollapsible(false);
    auto *rollPane = new QWidget(m_splitter);
    auto *mid = new QHBoxLayout(rollPane);
    mid->setContentsMargins(0, 0, 0, 0);
    mid->setSpacing(0);
    auto *headerScroll = new QScrollArea(this);
    headerScroll->setFixedWidth(kHeaderW);
    headerScroll->setFrameShape(QFrame::NoFrame);
    headerScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    headerScroll->setWidgetResizable(true);
    // The roll owns keyboard editing (delete, copy/paste); the scroll areas
    // must not steal its focus on click (QAbstractScrollArea defaults to
    // StrongFocus, which broke shortcuts right after a track switch).
    headerScroll->setFocusPolicy(Qt::NoFocus);
    m_headers = new TrackHeaderPanel(this);
    headerScroll->setWidget(m_headers);
    mid->addWidget(headerScroll);
    m_roll = new PianoRoll(this);
    mid->addWidget(m_roll, 1);
    m_vbar = new QScrollBar(Qt::Vertical, this);
    mid->addWidget(m_vbar);
    m_splitter->addWidget(rollPane);

    m_lanesScroll = new QScrollArea(this);
    m_lanesScroll->setMinimumHeight(kLaneH + kAddLaneH);
    m_lanesScroll->setFrameShape(QFrame::NoFrame);
    m_lanesScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_lanesScroll->setWidgetResizable(true);
    m_lanesScroll->setFocusPolicy(Qt::NoFocus);
    m_lanes = new AutomationArea(this, m_lanesScroll);
    m_lanesScroll->setWidget(m_lanes);
    m_splitter->addWidget(m_lanesScroll);
    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 0);
    vbox->addWidget(m_splitter, 1);

    m_strip = new OtherStrip(this);
    vbox->addWidget(m_strip);

    m_hbar = new QScrollBar(Qt::Horizontal, this);
    auto *hbarRow = new QHBoxLayout;
    hbarRow->addSpacing(kGutterW);
    hbarRow->addWidget(m_hbar);
    vbox->addLayout(hbarRow);

    connect(m_hbar, &QScrollBar::valueChanged, this, [this](int v) {
        if (m_scrollPx != v) {
            m_scrollPx = v;
            refreshTimelineViews();
        }
    });
    connect(m_vbar, &QScrollBar::valueChanged, this, [this](int v) {
        if (m_scrollY != v) {
            m_scrollY = v;
            m_roll->update();
        }
    });
}

void SongView::setSong(const MidiTimeline *timeline, const LoadedVoiceGroup *voicegroup)
{
    m_timeline = timeline;
    m_voicegroup = voicegroup;
    m_model = timeline ? buildSongViewModel(*timeline) : SongViewModel();
    m_emptyLanes.clear();
    m_selection.clear();
    m_timeSel = TimeSelection();
    m_clip = Clip();
    m_muteMask = 0;
    m_soloMask = 0;
    emit muteMaskChanged(0);
    emit soloMaskChanged(0);
    m_playheadTick = 0.0;
    m_editCursorTick = 0;
    m_playing = false;
    m_scrollPx = 0;
    // Lane heights are per-song view state; back to defaults until a
    // sidecar (applyViewState) says otherwise.
    m_lanes->setViewHeights(0, {});

    m_selectedTrack = 0;
    if (timeline) {
        for (int t = 0; t < 16; t++) {
            if (timeline->tracks[t].used) {
                m_selectedTrack = t;
                break;
            }
        }
    }
    m_trackSelMask = 1u << m_selectedTrack;

    rebuildAfterSongChange();
}

void SongView::rebuildAfterSongChange()
{
    if (m_timeline) {
        // Default zoom: 32 px per beat, scrolled so the notes' pitch range
        // is centered in the roll.
        m_pxPerTick = 32.0 / double(m_timeline->ticksPerBeat);
        const int midKey = m_model.minNoteKey <= m_model.maxNoteKey
                               ? (m_model.minNoteKey + m_model.maxNoteKey) / 2
                               : 60;
        m_scrollY = std::max(0, (127 - midKey) * m_keyHeight
                                    - std::max(200, m_roll->height()) / 2);
    } else {
        m_pxPerTick = 1.0;
        m_scrollY = 0;
    }
    m_headers->rebuild();
    m_lanes->rebuildRows();
    updateScrollbars();
    refreshTimelineViews();
}

void SongView::updateSong(const MidiTimeline *timeline)
{
    m_timeline = timeline;
    m_model = timeline ? buildSongViewModel(*timeline) : SongViewModel();
    mergeEmptyLanes();

    if (timeline && !timeline->tracks[m_selectedTrack].used) {
        // The edited track disappeared (e.g. undo of its only events).
        m_selectedTrack = 0;
        for (int t = 0; t < 16; t++) {
            if (timeline->tracks[t].used) {
                m_selectedTrack = t;
                break;
            }
        }
    }

    // Keep only selection ids that still resolve to a note.
    std::vector<NoteId> keep;
    for (const NoteId &id : m_selection) {
        for (const ViewNote &note : m_model.notes) {
            if (note.track == m_selectedTrack && note.startTick == id.tick
                && note.key == id.key) {
                keep.push_back(id);
                break;
            }
        }
    }
    m_selection = std::move(keep);

    m_headers->rebuild();
    m_lanes->rebuildRows();
    updateScrollbars();
    refreshTimelineViews();
}

void SongView::setDocument(SongDocument *document)
{
    m_document = document;
    m_selection.clear();
    m_headers->rebuild();   // the "+ Add track" button follows editability
    m_lanes->rebuildRows(); // the "+ Add lane" strip follows editability
}

void SongView::addEmptyLane(int track, uint8_t cc)
{
    if (track < 0 || track > 15 || !m_timeline)
        return;
    const std::pair<int, uint8_t> key(track, cc);
    if (std::find(m_emptyLanes.begin(), m_emptyLanes.end(), key) == m_emptyLanes.end())
        m_emptyLanes.push_back(key);
    mergeEmptyLanes();
    m_lanes->rebuildRows();
}

void SongView::removeEmptyLane(int track, uint8_t cc)
{
    m_emptyLanes.erase(std::remove(m_emptyLanes.begin(), m_emptyLanes.end(),
                                   std::pair<int, uint8_t>(track, cc)),
                       m_emptyLanes.end());
    for (auto it = m_model.lanes.begin(); it != m_model.lanes.end(); ++it) {
        if (it->track == track && it->cc == cc && it->points.empty()) {
            m_model.lanes.erase(it);
            break;
        }
    }
    m_lanes->rebuildRows();
}

void SongView::mergeEmptyLanes()
{
    bool added = false;
    for (const std::pair<int, uint8_t> &key : m_emptyLanes) {
        if (m_model.findLane(key.first, key.second))
            continue;
        AutoLane lane;
        lane.track = uint8_t(key.first);
        lane.cc = key.second;
        if (key.second == LANE_CC_BEND) {
            lane.lane = M4aLane::PitchBend;
            lane.name = m4aLaneName(M4aLane::PitchBend);
        } else {
            const M4aCcInfo info = m4aClassifyCc(key.second);
            lane.lane = info.lane;
            lane.name = QString::fromLatin1(info.display);
        }
        m_model.lanes.push_back(std::move(lane));
        added = true;
    }
    if (added) {
        // Same order buildSongViewModel establishes.
        std::stable_sort(m_model.lanes.begin(), m_model.lanes.end(),
                         [](const AutoLane &a, const AutoLane &b) {
                             if (a.track != b.track)
                                 return a.track < b.track;
                             return a.cc < b.cc;
                         });
    }
}

SongView::ViewState SongView::viewState() const
{
    ViewState state;
    if (!m_timeline)
        return state;
    state.valid = true;
    state.pxPerBeat = m_pxPerTick * double(m_timeline->ticksPerBeat);
    state.keyHeight = m_keyHeight;
    state.scrollPx = m_scrollPx;
    state.scrollY = m_scrollY;
    state.selectedTrack = m_selectedTrack;
    state.editCursorTick = m_editCursorTick;
    state.laneHeight = m_lanes->laneHeight();
    state.laneHeights = m_lanes->rowHeightOverrides();
    state.splitterSizes = m_splitter->sizes();
    state.emptyLanes = m_emptyLanes;
    return state;
}

void SongView::applyViewState(const ViewState &state)
{
    if (!state.valid || !m_timeline)
        return;
    const double tpb = double(m_timeline->ticksPerBeat);
    m_pxPerTick = std::clamp(state.pxPerBeat, kMinPxPerBeat, kMaxPxPerBeat) / tpb;
    m_keyHeight = std::clamp(state.keyHeight, kMinKeyHeight, kMaxKeyHeight);
    m_editCursorTick = std::min<uint64_t>(state.editCursorTick, m_timeline->lengthTicks);
    for (const std::pair<int, uint8_t> &lane : state.emptyLanes)
        if (lane.first >= 0 && lane.first < 16
            && std::find(m_emptyLanes.begin(), m_emptyLanes.end(), lane)
                   == m_emptyLanes.end())
            m_emptyLanes.push_back(lane);
    mergeEmptyLanes();
    m_lanes->setViewHeights(state.laneHeight, state.laneHeights);
    if (state.selectedTrack >= 0 && state.selectedTrack < 16
        && m_timeline->tracks[state.selectedTrack].used)
        selectTrack(state.selectedTrack);
    if (state.splitterSizes.size() == 2 && state.splitterSizes[0] > 0
        && state.splitterSizes[1] > 0) {
        // Real sizes exist; skip resizeEvent's default split.
        m_splitInit = true;
        m_splitter->setSizes(state.splitterSizes);
    }
    m_lanes->rebuildRows();
    updateScrollbars();
    setHScroll(std::max(0, state.scrollPx));
    m_vbar->setValue(std::max(0, state.scrollY));
    refreshTimelineViews();
}

void SongView::setVoicegroup(const LoadedVoiceGroup *voicegroup)
{
    m_voicegroup = voicegroup;
    m_headers->rebuild();
    refreshTimelineViews();
}

uint64_t SongView::gridTicks() const
{
    if (!m_timeline)
        return 24;
    const uint64_t tpb = std::max<uint32_t>(1, m_timeline->ticksPerBeat);
    const uint64_t clock = m_document ? m_document->ticksPerClock() : 1;
    // Finest visible beat subdivision at least ~8 px wide, floored at the
    // mid2agb clock grid (divisions 24/48 reach it exactly at deep zoom).
    for (uint64_t div : {uint64_t(48), uint64_t(32), uint64_t(24), uint64_t(16),
                         uint64_t(8), uint64_t(4), uint64_t(2), uint64_t(1)}) {
        if (pxPerBeat() / double(div) >= 8.0)
            return std::max(std::max<uint64_t>(1, tpb / div), clock);
    }
    return std::max(tpb, clock);
}

uint64_t SongView::fineGridTicks() const
{
    return m_document ? std::max<uint32_t>(1, m_document->ticksPerClock())
                      : gridTicks();
}

uint64_t SongView::snapTick(double tick, bool fine) const
{
    const double g = double(fine ? fineGridTicks() : gridTicks());
    return uint64_t(std::round(std::max(0.0, tick) / g) * g);
}

bool SongView::isSelected(const ViewNote &note) const
{
    if (note.track != m_selectedTrack)
        return false;
    const NoteId id{note.startTick, note.key};
    return std::find(m_selection.begin(), m_selection.end(), id) != m_selection.end();
}

void SongView::setSelection(std::vector<NoteId> ids)
{
    m_selection = std::move(ids);
    // The two selection kinds are mutually exclusive, so Ctrl+C is never
    // ambiguous.
    if (!m_selection.empty() && m_timeSel.active())
        clearTimeSelection();
    m_roll->update();
}

void SongView::clearSelection()
{
    if (!m_selection.empty()) {
        m_selection.clear();
        m_roll->update();
    }
}

void SongView::setTimeSelection(const TimeSelection &sel)
{
    m_timeSel = sel;
    if (m_timeSel.active() && !m_selection.empty())
        m_selection.clear();
    refreshTimelineViews();
}

void SongView::clearTimeSelection()
{
    m_timeSel = TimeSelection();
    refreshTimelineViews();
}

bool SongView::timeSelectionCoversTrack(int track) const
{
    if (!m_timeSel.active() || track < 0 || track > 15)
        return false;
    switch (m_timeSel.scope) {
    case TimeSelection::AllTracks:
        return true;
    case TimeSelection::Tracks:
        return m_timeSel.trackMask & (1u << track);
    case TimeSelection::Lanes:
        return false;
    }
    return false;
}

bool SongView::timeSelectionCoversRow(int track, uint8_t cc) const
{
    if (!m_timeSel.active())
        return false;
    if (m_timeSel.scope == TimeSelection::Lanes)
        return std::find(m_timeSel.lanes.begin(), m_timeSel.lanes.end(),
                         std::pair<int, uint8_t>(track, cc))
            != m_timeSel.lanes.end();
    // Track scopes cover the track's CC/voice rows, never the global tempo.
    if (cc == DOC_CC_TEMPO)
        return false;
    return timeSelectionCoversTrack(track);
}

void SongView::announceTimeSelection()
{
    if (!m_timeSel.active() || !m_timeline)
        return;
    const double beats = double(m_timeSel.endTick - m_timeSel.startTick)
                         / double(std::max<uint32_t>(1, m_timeline->ticksPerBeat));
    QString scope;
    switch (m_timeSel.scope) {
    case TimeSelection::AllTracks:
        scope = tr("all tracks");
        break;
    case TimeSelection::Tracks: {
        int n = 0;
        for (int t = 0; t < 16; t++)
            n += (m_timeSel.trackMask >> t) & 1;
        scope = tr("%n track(s)", nullptr, n);
        break;
    }
    case TimeSelection::Lanes:
        scope = tr("%n lane(s)", nullptr, int(m_timeSel.lanes.size()));
        break;
    }
    emit statusMessage(tr("Time selection: %1 beats · %2 · Ctrl+C/X copies, "
                          "Del clears, Ctrl+V pastes at the edit cursor")
                           .arg(beats, 0, 'g', 4)
                           .arg(scope));
}

std::vector<int> SongView::timeSelectionTracks() const
{
    std::vector<int> tracks;
    if (!m_timeline || !m_document)
        return tracks;
    for (int t = 0; t < 16; t++) {
        if (!m_timeline->tracks[t].used)
            continue;
        if (m_timeSel.scope == TimeSelection::Tracks
            && !(m_timeSel.trackMask & (1u << t)))
            continue;
        if (m_document->smfTrackFor(t) < 0)
            continue;
        tracks.push_back(t);
    }
    return tracks;
}

std::vector<uint8_t> SongView::trackCcs(int track) const
{
    std::vector<uint8_t> ccs;
    for (const AutoLane &lane : m_model.lanes)
        if (lane.track == track)
            ccs.push_back(lane.cc); // LANE_CC_BEND == DOC_CC_BEND
    ccs.push_back(DOC_CC_VOICE);
    return ccs;
}

void SongView::copyTimeSelection()
{
    if (!m_document || !m_timeSel.active())
        return;
    const uint64_t s = m_timeSel.startTick;
    const uint64_t e = m_timeSel.endTick;
    Clip clip;
    clip.span = e - s;
    int noteCount = 0;
    int pointCount = 0;
    const auto copyLanePoints = [&](int track, uint8_t cc) {
        ClipLane lane{track, cc, {}};
        const int query = track < 0 ? m_selectedTrack : track;
        for (const DocLanePoint &pt : m_document->lanePoints(query, cc)) {
            if (pt.tick >= s && pt.tick < e)
                lane.points.push_back({uint32_t(pt.tick - s), pt.value});
        }
        pointCount += int(lane.points.size());
        // Empty segments are kept: they carry "this span is silent" so paste
        // clears the destination range.
        clip.lanes.push_back(std::move(lane));
    };
    if (m_timeSel.scope == TimeSelection::Lanes) {
        for (const std::pair<int, uint8_t> &id : m_timeSel.lanes)
            copyLanePoints(id.first, id.second);
    } else {
        for (int t : timeSelectionTracks()) {
            ClipTrack ct{t, {}};
            for (const DocNote &note : m_document->notesForTrack(t)) {
                if (note.tick < s || note.tick >= e)
                    continue;
                ct.notes.push_back({uint32_t(note.tick - s), note.key,
                                    note.duration ? note.duration
                                                  : uint32_t(gridTicks()),
                                    note.velocity});
            }
            noteCount += int(ct.notes.size());
            clip.tracks.push_back(std::move(ct));
            for (uint8_t cc : trackCcs(t))
                copyLanePoints(t, cc);
        }
    }
    m_clip = std::move(clip);
    announce(tr("Copied range: %1 note(s), %2 automation point(s)")
                 .arg(noteCount)
                 .arg(pointCount));
}

void SongView::deleteTimeSelection()
{
    if (!m_document || !m_timeSel.active())
        return;
    const uint64_t s = m_timeSel.startTick;
    const uint64_t e = m_timeSel.endTick;
    SongDocument::RangeEdit edit;
    const auto removeLanePoints = [&](int track, uint8_t cc) {
        const int query = track < 0 ? m_selectedTrack : track;
        for (const DocLanePoint &pt : m_document->lanePoints(query, cc)) {
            if (pt.tick >= s && pt.tick < e)
                edit.removePoints.push_back(pt);
        }
    };
    if (m_timeSel.scope == TimeSelection::Lanes) {
        for (const std::pair<int, uint8_t> &id : m_timeSel.lanes)
            removeLanePoints(id.first, id.second);
    } else {
        for (int t : timeSelectionTracks()) {
            for (const DocNote &note : m_document->notesForTrack(t)) {
                if (note.tick >= s && note.tick < e)
                    edit.removeNotes.push_back(note);
            }
            for (uint8_t cc : trackCcs(t))
                removeLanePoints(t, cc);
        }
    }
    if (edit.empty()) {
        announce(tr("Nothing to delete in the time selection"));
        return;
    }
    const int notes = int(edit.removeNotes.size());
    const int points = int(edit.removePoints.size());
    m_document->applyRangeEdit(tr("delete range"), edit);
    announce(tr("Deleted range: %1 note(s), %2 automation point(s)")
                 .arg(notes)
                 .arg(points));
}

void SongView::pasteRangeAtEditCursor()
{
    if (!m_document || m_clip.span == 0 || m_clip.empty())
        return;
    const uint64_t s = snapTick(double(m_editCursorTick));
    const uint64_t e = s + m_clip.span;

    // A clip whose content came from one track retargets to the selected
    // track (cross-track copy); multi-track clips paste back in place.
    int sole = -2;
    bool multi = false;
    const auto consider = [&](int track) {
        if (track < 0)
            return; // tempo is global
        if (sole == -2)
            sole = track;
        else if (sole != track)
            multi = true;
    };
    for (const ClipTrack &ct : m_clip.tracks)
        consider(ct.track);
    for (const ClipLane &cl : m_clip.lanes)
        consider(cl.track);
    const auto mapTrack = [&](int track) {
        return track < 0 ? -1 : (multi ? track : m_selectedTrack);
    };

    SongDocument::RangeEdit edit;
    uint32_t pastedMask = 0;
    for (const ClipTrack &ct : m_clip.tracks) {
        const int t = mapTrack(ct.track);
        if (t < 0 || m_document->smfTrackFor(t) < 0)
            continue;
        pastedMask |= 1u << t;
        // Replace: whatever notes start inside the destination span go away.
        for (const DocNote &note : m_document->notesForTrack(t)) {
            if (note.tick >= s && note.tick < e)
                edit.removeNotes.push_back(note);
        }
        if (!ct.notes.empty()) {
            SongDocument::RangeEdit::TrackNotes tn{t, {}};
            for (const ClipNote &cn : ct.notes)
                tn.notes.push_back(
                    {s + cn.relTick, cn.key, cn.duration, cn.velocity});
            edit.addNotes.push_back(std::move(tn));
        }
    }
    std::vector<std::pair<int, uint8_t>> pastedLanes;
    for (const ClipLane &cl : m_clip.lanes) {
        const int t = mapTrack(cl.track);
        if (t >= 0 && m_document->smfTrackFor(t) < 0)
            continue;
        pastedLanes.push_back({t, cl.cc});
        const int query = t < 0 ? m_selectedTrack : t;
        for (const DocLanePoint &pt : m_document->lanePoints(query, cl.cc)) {
            if (pt.tick >= s && pt.tick < e)
                edit.removePoints.push_back(pt);
        }
        if (!cl.points.empty()) {
            SongDocument::RangeEdit::LaneWrite lw{t, cl.cc, {}};
            for (const std::pair<uint32_t, int> &pv : cl.points)
                lw.points.push_back({s + pv.first, pv.second});
            edit.addPoints.push_back(std::move(lw));
        }
    }
    if (edit.empty() && m_clip.tracks.empty() && pastedLanes.empty())
        return;
    m_document->applyRangeEdit(tr("paste range"), edit);

    // Select the pasted region so it can be immediately re-copied or moved.
    TimeSelection sel;
    sel.startTick = s;
    sel.endTick = e;
    if (m_clip.tracks.empty()) {
        sel.scope = TimeSelection::Lanes;
        sel.lanes = std::move(pastedLanes);
    } else {
        sel.scope = TimeSelection::Tracks;
        sel.trackMask = pastedMask;
    }
    setTimeSelection(sel);
    announce(tr("Pasted range at the edit cursor"));
}

bool SongView::handleEditKey(QKeyEvent *event)
{
    if (!m_document)
        return false;
    const bool sel = m_timeSel.active();
    if (sel && event->matches(QKeySequence::Copy)) {
        copyTimeSelection();
        event->accept();
        return true;
    }
    if (sel && event->matches(QKeySequence::Cut)) {
        copyTimeSelection();
        deleteTimeSelection();
        event->accept();
        return true;
    }
    if (sel
        && (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)) {
        deleteTimeSelection();
        event->accept();
        return true;
    }
    if (event->matches(QKeySequence::Paste) && m_clip.span > 0 && !m_clip.empty()) {
        pasteRangeAtEditCursor();
        event->accept();
        return true;
    }
    return false;
}

void SongView::showTimeSelectionMenu(const QPoint &globalPos)
{
    if (!m_document || !m_timeSel.active())
        return;
    QMenu menu(this);
    QAction *copy = menu.addAction(tr("Copy range"));
    copy->setShortcut(QKeySequence::Copy);
    QAction *cut = menu.addAction(tr("Cut range"));
    cut->setShortcut(QKeySequence::Cut);
    QAction *del = menu.addAction(tr("Delete range"));
    QAction *paste = menu.addAction(tr("Paste at edit cursor"));
    paste->setShortcut(QKeySequence::Paste);
    paste->setEnabled(m_clip.span > 0 && !m_clip.empty());
    menu.addSeparator();
    QAction *clear = menu.addAction(tr("Clear selection"));
    QAction *chosen = menu.exec(globalPos);
    if (chosen == copy) {
        copyTimeSelection();
    } else if (chosen == cut) {
        copyTimeSelection();
        deleteTimeSelection();
    } else if (chosen == del) {
        deleteTimeSelection();
    } else if (chosen == paste) {
        pasteRangeAtEditCursor();
    } else if (chosen == clear) {
        clearTimeSelection();
    }
}

void SongView::announceNote(const ViewNote &note)
{
    if (!m_timeline)
        return;
    const bool ext = m_document && m_document->cfg().extendedClocks;
    const bool exact = m_document && m_document->cfg().exactGate;
    const int64_t ticks = int64_t(note.endTick) - int64_t(note.startTick);
    emit statusMessage(tr("%1 · velocity %2 → plays %3 · length %4 ticks → %5 clocks")
                           .arg(keyName(note.key))
                           .arg(note.velocity)
                           .arg(mid2agbEffectiveVelocity(note.velocity))
                           .arg(ticks)
                           .arg(mid2agbEffectiveDuration(ticks, m_timeline->ticksPerBeat,
                                                         ext, exact)));
}

void SongView::setPlayheadSample(uint64_t samplePos, bool playing)
{
    if (!m_timeline)
        return;
    m_playheadTick = m_timeline->tickForSample(samplePos);
    m_playing = playing;
    if (playing) {
        const int px = contentX(m_playheadTick);
        const int vw = viewportWidth();
        if (px < 0 || px > vw * 85 / 100)
            setHScroll(int(m_playheadTick * m_pxPerTick) - vw / 10);
    }
    refreshTimelineViews();
}

void SongView::setEditCursorTick(uint64_t tick)
{
    if (m_editCursorTick == tick)
        return;
    m_editCursorTick = tick;
    refreshTimelineViews();
}

void SongView::commitEditCursor(uint64_t tick)
{
    setEditCursorTick(tick);
    emit editCursorMoved(tick);
}

void SongView::goToStart()
{
    setHScroll(0);
    commitEditCursor(0);
}

double SongView::pxPerBeat() const
{
    return m_timeline ? m_pxPerTick * m_timeline->ticksPerBeat : m_pxPerTick * 24.0;
}

void SongView::selectTrack(int track)
{
    if (track == m_selectedTrack || track < 0 || track > 15)
        return;
    m_selectedTrack = track;
    // Programmatic selection collapses the multi-track scope;
    // trackHeaderClicked restores it for modifier clicks.
    m_trackSelMask = 1u << track;
    m_selection.clear();
    m_headers->syncSelection();
    m_lanes->rebuildRows();
    // Switching tracks readies the roll for keyboard editing (e.g. copy on
    // one track, click another's header, paste), wherever focus was.
    m_roll->setFocus();
    m_roll->update();
    emit selectedTrackChanged(track);
}

uint32_t SongView::trackSelectionMask() const
{
    uint32_t used = 0;
    if (m_timeline) {
        for (int t = 0; t < 16; t++)
            if (m_timeline->tracks[t].used)
                used |= 1u << t;
    }
    const uint32_t mask = (m_trackSelMask | (1u << m_selectedTrack)) & used;
    return mask ? mask : (1u << m_selectedTrack);
}

void SongView::trackHeaderClicked(int track, Qt::KeyboardModifiers modifiers)
{
    if (track < 0 || track > 15)
        return;
    if (modifiers & Qt::ControlModifier) {
        uint32_t mask = trackSelectionMask() ^ (1u << track);
        if (mask == 0)
            return; // the scope can't go empty
        if (!(mask & (1u << m_selectedTrack))) {
            // The primary track was toggled out; hand primary to the lowest
            // remaining scoped track (selectTrack collapses the mask, so
            // restore it after).
            int next = 0;
            while (!(mask & (1u << next)))
                next++;
            selectTrack(next);
        }
        m_trackSelMask = mask;
        m_headers->syncSelection();
    } else if (modifiers & Qt::ShiftModifier) {
        const int lo = std::min(track, m_selectedTrack);
        const int hi = std::max(track, m_selectedTrack);
        uint32_t mask = 0;
        for (int t = lo; t <= hi; t++) {
            if (m_timeline && m_timeline->tracks[t].used)
                mask |= 1u << t;
        }
        m_trackSelMask = mask | (1u << m_selectedTrack);
        m_headers->syncSelection();
    } else {
        selectTrack(track);
        m_trackSelMask = 1u << track; // collapse even when already primary
        m_headers->syncSelection();
    }
}

void SongView::setTrackMute(int track, bool on)
{
    const uint32_t bit = 1u << track;
    const uint32_t mask = on ? (m_muteMask | bit) : (m_muteMask & ~bit);
    if (mask != m_muteMask) {
        m_muteMask = mask;
        emit muteMaskChanged(mask);
    }
}

void SongView::setTrackSolo(int track, bool on)
{
    const uint32_t bit = 1u << track;
    const uint32_t mask = on ? (m_soloMask | bit) : (m_soloMask & ~bit);
    if (mask != m_soloMask) {
        m_soloMask = mask;
        emit soloMaskChanged(mask);
    }
}

QColor SongView::trackColor(int track)
{
    // Golden-angle hue spacing keeps adjacent tracks visually distinct.
    return QColor::fromHsv(int(track * 137.508) % 360, 150, 205);
}

QString SongView::instrumentLabel(int track) const
{
    if (!m_timeline)
        return QString();
    const int prog = m_timeline->tracks[track].firstProgram;
    if (prog < 0)
        return tr("(no voice set)");
    QString name = voiceShortName(uint8_t(prog));
    return QStringLiteral("%1 %2").arg(prog, 3, 10, QLatin1Char('0')).arg(name);
}

QString SongView::voiceShortName(uint8_t program) const
{
    QString name;
    QString type;
    if (m_voicegroup && program < VOICEGROUP_SIZE) {
        name = QString::fromUtf8(m_voicegroup->voiceNames[program]).trimmed();
        type = m4aVoiceTypeName(m_voicegroup->voices[program].type);
    }
    if (name.isEmpty())
        return type.isEmpty() ? tr("Voice") : type;
    return QStringLiteral("%1 (%2)").arg(name, type);
}

bool SongView::pickVoice(const QString &title, int initialVoice, int *outVoice)
{
    VoicePickerDialog dialog(this, title, initialVoice, [this](int voice, int velocity) {
        emit auditionVoice(voice, kVoiceAuditionKey, velocity);
    });
    if (dialog.exec() != QDialog::Accepted)
        return false;
    *outVoice = dialog.selectedVoice();
    return true;
}

void SongView::editTrackVoice(int track)
{
    if (!m_document || track < 0 || track > 15)
        return;
    const std::vector<DocLanePoint> changes = m_document->lanePoints(track, DOC_CC_VOICE);
    const int initial = changes.empty() ? 0 : changes.front().value;
    int voice = initial;
    if (!pickVoice(tr("Track %1 voice").arg(track + 1), initial, &voice))
        return;
    if (changes.empty())
        m_document->addLanePoint(track, DOC_CC_VOICE, 0, voice);
    else if (voice != initial)
        m_document->moveLanePoint(track, DOC_CC_VOICE, changes.front(),
                                  changes.front().tick, voice);
}

void SongView::addTrack()
{
    if (!m_document || !m_document->canAddTrack())
        return;
    int voice = 0;
    if (!pickVoice(tr("New track voice"), 0, &voice))
        return;
    const int track = m_document->addTrack(voice); // rebuilds via documentChanged
    if (track >= 0) {
        selectTrack(track);
        announce(tr("Added track %1").arg(track + 1));
    }
}

void SongView::deleteTrack(int track)
{
    if (!m_document || track < 0 || track > 15 || m_document->smfTrackFor(track) < 0)
        return;
    if (m_document->smf().format != 0) {
        // Removing a format-1 chunk shifts every higher engine slot down by
        // one; move the per-track view state with it, before the document
        // edit rebuilds the headers and lanes. (Format 0 slots are fixed
        // channels: just drop the deleted track's state.)
        const uint32_t low = (1u << track) - 1;
        const uint32_t mute = (m_muteMask & low) | ((m_muteMask >> 1) & ~low);
        const uint32_t solo = (m_soloMask & low) | ((m_soloMask >> 1) & ~low);
        if (mute != m_muteMask) {
            m_muteMask = mute;
            emit muteMaskChanged(mute);
        }
        if (solo != m_soloMask) {
            m_soloMask = solo;
            emit soloMaskChanged(solo);
        }
        for (auto it = m_emptyLanes.begin(); it != m_emptyLanes.end();) {
            if (it->first == track) {
                it = m_emptyLanes.erase(it);
            } else {
                if (it->first > track)
                    it->first--;
                ++it;
            }
        }
        if (m_selectedTrack > track)
            m_selectedTrack--;
    } else {
        const uint32_t bit = 1u << track;
        if (m_muteMask & bit) {
            m_muteMask &= ~bit;
            emit muteMaskChanged(m_muteMask);
        }
        if (m_soloMask & bit) {
            m_soloMask &= ~bit;
            emit soloMaskChanged(m_soloMask);
        }
        m_emptyLanes.erase(std::remove_if(m_emptyLanes.begin(), m_emptyLanes.end(),
                                          [track](const std::pair<int, uint8_t> &lane) {
                                              return lane.first == track;
                                          }),
                           m_emptyLanes.end());
    }
    // Track slots shift (format 1) or empty out (format 0); collapse the
    // multi-track scope and drop the time selection rather than remap them.
    m_trackSelMask = 1u << m_selectedTrack;
    clearTimeSelection();
    m_document->deleteTrack(track); // rebuilds via documentChanged
    announce(tr("Deleted track %1").arg(track + 1));
}

void SongView::forEachGridLine(uint64_t tickBegin, uint64_t tickEnd,
                               const std::function<void(uint64_t, bool, int)> &fn) const
{
    if (!m_timeline || tickEnd <= tickBegin)
        return;
    const uint32_t tpb = m_timeline->ticksPerBeat;

    struct Seg {
        uint64_t tick;
        uint64_t beatTicks;
        int beatsPerBar;
    };
    std::vector<Seg> segs;
    segs.push_back({0, tpb, 4});
    for (const TimeSigPoint &ts : m_timeline->timeSigs) {
        uint64_t beatTicks = (uint64_t(tpb) * 4) >> ts.denomPow2;
        if (beatTicks < 1)
            beatTicks = 1;
        const Seg seg{ts.tick, beatTicks, ts.numerator ? ts.numerator : 4};
        if (ts.tick == segs.back().tick)
            segs.back() = seg;
        else
            segs.push_back(seg);
    }

    int bar = 1;
    for (size_t i = 0; i < segs.size(); i++) {
        const Seg &seg = segs[i];
        const uint64_t segEnd =
            i + 1 < segs.size() ? segs[i + 1].tick : std::max<uint64_t>(tickEnd, seg.tick);
        const uint64_t clampedEnd = std::min(segEnd, tickEnd);
        if (seg.tick < clampedEnd) {
            uint64_t k = tickBegin > seg.tick ? (tickBegin - seg.tick) / seg.beatTicks : 0;
            for (uint64_t tick = seg.tick + k * seg.beatTicks; tick < clampedEnd;
                 tick += seg.beatTicks, k++) {
                if (tick < tickBegin)
                    continue;
                fn(tick, k % seg.beatsPerBar == 0, bar + int(k / seg.beatsPerBar));
            }
        }
        if (i + 1 < segs.size()) {
            const uint64_t segTicks = segs[i + 1].tick - seg.tick;
            const uint64_t barTicks = seg.beatTicks * seg.beatsPerBar;
            bar += int((segTicks + barTicks - 1) / barTicks);
        }
    }
}

void SongView::zoomAroundContentX(double factor, int anchorContentX)
{
    if (!m_timeline)
        return;
    const double tpb = double(m_timeline->ticksPerBeat);
    const double anchorTick = tickAtContentX(anchorContentX);
    m_pxPerTick = std::clamp(m_pxPerTick * factor, kMinPxPerBeat / tpb, kMaxPxPerBeat / tpb);
    m_scrollPx = std::max(0, int(anchorTick * m_pxPerTick) - anchorContentX);
    updateScrollbars();
    refreshTimelineViews();
}

void SongView::zoomKeyHeight(int wheelDelta, int anchorY)
{
    if (!m_timeline)
        return;
    // One key-height pixel per wheel notch; the accumulator makes fine
    // trackpad deltas add up instead of stepping on every event.
    m_keyZoomAccum += wheelDelta;
    const int steps = m_keyZoomAccum / 120;
    if (steps == 0)
        return;
    m_keyZoomAccum -= steps * 120;
    const int newH = std::clamp(m_keyHeight + steps, kMinKeyHeight, kMaxKeyHeight);
    if (newH == m_keyHeight)
        return;
    // Pin the key under the cursor: same content row before and after.
    const double row = double(anchorY + m_scrollY) / double(m_keyHeight);
    m_keyHeight = newH;
    m_scrollY = std::max(0, int(std::lround(row * newH)) - anchorY);
    updateScrollbars();
    m_roll->update();
}

void SongView::scrollByPx(int dx)
{
    setHScroll(m_scrollPx + dx);
}

void SongView::scrollRollBy(int dy)
{
    m_vbar->setValue(m_vbar->value() + dy);
}

void SongView::setHScroll(int px)
{
    px = std::clamp(px, 0, m_hbar->maximum());
    if (px == m_scrollPx)
        return;
    m_scrollPx = px;
    m_hbar->blockSignals(true);
    m_hbar->setValue(px);
    m_hbar->blockSignals(false);
    refreshTimelineViews();
}

int SongView::viewportWidth() const
{
    return std::max(50, m_roll->width() - kKeyboardW);
}

void SongView::updateScrollbars()
{
    const int lengthPx =
        m_timeline ? int(double(m_timeline->lengthTicks) * m_pxPerTick) + 100 : 0;
    m_hbar->setRange(0, std::max(0, lengthPx - viewportWidth()));
    m_hbar->setPageStep(viewportWidth());
    m_hbar->blockSignals(true);
    m_hbar->setValue(std::min(m_scrollPx, m_hbar->maximum()));
    m_hbar->blockSignals(false);
    m_scrollPx = m_hbar->value();

    const int rollContentH = 128 * m_keyHeight;
    m_vbar->setRange(0, std::max(0, rollContentH - m_roll->height()));
    m_vbar->setPageStep(m_roll->height());
    m_vbar->blockSignals(true);
    m_vbar->setValue(std::min(m_scrollY, m_vbar->maximum()));
    m_vbar->blockSignals(false);
    m_scrollY = m_vbar->value();
}

void SongView::refreshTimelineViews()
{
    m_ruler->update();
    m_roll->update();
    m_lanes->update();
    m_strip->update();
}

void SongView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // The splitter starts with the lanes area at its classic fixed height;
    // sizes can only be applied once real geometry exists.
    if (!m_splitInit && m_splitter->height() > 0) {
        m_splitInit = true;
        m_splitter->setSizes(
            {std::max(120, m_splitter->height() - kLanesAreaH), kLanesAreaH});
    }
    updateScrollbars();
}
