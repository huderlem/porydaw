#include "songview.h"

#include <QApplication>
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
// Velocity drag handle just inboard of a note's left resize zone: hit-zone
// width, and the minimum note width that gets one (narrow notes stay
// all-Move; the right-click menu remains the fallback). The minimum keeps
// the zone clear of the right edge's resize zone.
constexpr int kVelHandleW = 8;
constexpr int kVelHandleMinNoteW = 18;

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

QColor loopFill() { return QColor(255, 200, 60, 34); }
QColor loopEdge() { return QColor(224, 168, 0); }
QColor playheadColor() { return QColor(226, 66, 66); }

// Draw the loop-region band and the playhead line across rect. x positions
// are computed with origin = local x of timeline tick 0's content position.
void drawOverlays(QPainter &p, const SongView *sv, const QRect &rect, int origin)
{
    const MidiTimeline *tl = sv->timeline();
    if (!tl)
        return;
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
        drawOverlays(p, m_sv, area, kGutterW);

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
            if (!doc)
                return;
            QMenu menu(this);
            QAction *setStart = menu.addAction(SongView::tr("Set loop start here"));
            QAction *setEnd = menu.addAction(SongView::tr("Set loop end here"));
            QAction *remove = menu.addAction(SongView::tr("Remove loop markers"));
            remove->setEnabled(tl->loopStartTick != UINT64_MAX
                               || tl->loopEndTick != UINT64_MAX);
            QAction *chosen = menu.exec(event->globalPosition().toPoint());
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
            }
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
        if (m_dragMarker >= 0) {
            m_dragTick = dragTick();
            update();
            return;
        }
        if (m_placingCursor) {
            m_sv->setEditCursorTick(dragTick());
            return;
        }
        setCursor(m_sv->document() && hitMarker(event->pos().x()) >= 0 ? Qt::SplitHCursor
                                                                       : Qt::ArrowCursor);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton)
            return;
        if (m_placingCursor) {
            m_placingCursor = false;
            m_sv->commitEditCursor(m_sv->editCursorTick());
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

    SongView *m_sv;
    int m_dragMarker = -1;
    uint64_t m_dragTick = 0;
    bool m_placingCursor = false;
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

        drawOverlays(p, m_sv, grid, kKeyboardW);

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

        // Keyboard column: audition the clicked key on the selected track.
        if (event->pos().x() < kKeyboardW) {
            if (event->button() == Qt::LeftButton) {
                m_kbdKey = yToKey(event->pos().y());
                m_sv->audition(m_sv->selectedTrack(), m_kbdKey, 100);
            }
            return;
        }

        SongDocument *doc = m_sv->document();
        const ViewNote *hit = doc ? hitNote(event->pos()) : nullptr;

        if (event->button() == Qt::RightButton) {
            // Deferred: a drag from here rubber-band-selects; releasing in
            // place context-acts on the pressed note (or clears the
            // selection over empty space). Resolved in mouseReleaseEvent.
            if (!doc)
                return;
            m_pressPos = m_curPos = event->pos();
            m_rightPress = true;
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
        } else if (doc) {
            // Empty space: draw a note (FL/Reaper pencil style). It starts
            // at the grid cell under the cursor and sounds while the button
            // is held; dragging right before release sets its duration. The
            // document note is committed on release (one undo entry).
            const double grid = double(m_sv->gridTicks());
            m_drawTick =
                uint64_t(std::floor(std::max(0.0, m_pressTick) / grid) * grid);
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
            m_sv->audition(m_sv->selectedTrack(), m_drawKey, m_lastVelocity);
            m_auditioned = true;
        } else {
            // Read-only (no document): park the edit cursor at the click,
            // like the ruler; playback follows when running.
            m_sv->commitEditCursor(m_sv->snapTick(m_pressTick));
        }
        update();
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        // A fast click-click should behave as two presses (draw, then grab
        // the drawn note) — Qt replaces the second press with this event.
        mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        m_curPos = event->pos();
        if (m_rightPress && m_drag == Drag::None
            && (event->pos() - m_pressPos).manhattanLength()
                   >= QApplication::startDragDistance()) {
            m_drag = Drag::Band;
        }
        if (m_drag == Drag::None) {
            // Hover cursor: resize handle at note right edges, velocity
            // handle at their left ends.
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
            // Repaint when the hovered note changes so its velocity grip
            // appears/disappears.
            const bool hasHover = hit != nullptr;
            if (hasHover != m_hasHover
                || (hit && (hit->startTick != m_hoverTick || hit->key != m_hoverKey))) {
                m_hasHover = hasHover;
                if (hit) {
                    m_hoverTick = hit->startTick;
                    m_hoverKey = hit->key;
                }
                update();
            }
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
                        m_sv->audition(m_sv->selectedTrack(), key,
                                                notes.front().velocity);
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
                    m_sv->audition(m_sv->selectedTrack(), m_velAnchor.key, vel);
                    m_auditioned = true;
                }
                update();
            }
        } else if (m_drag == Drag::Draw) {
            // The note's right edge follows the cursor, rounded up to the
            // next grid line (never shorter than one grid cell), and its key
            // follows the cursor vertically — a slight misclick on mouse-down
            // is fixable mid-gesture, with the new pitch auditioned.
            const int64_t past = int64_t(std::llround(tick)) - int64_t(m_drawTick);
            const int64_t dur = std::max(grid, (past + grid - 1) / grid * grid);
            const int key = yToKey(event->pos().y());
            if (dur != m_drawDur || key != m_drawKey) {
                m_drawDur = dur;
                if (key != m_drawKey) {
                    m_drawKey = key;
                    m_sv->audition(m_sv->selectedTrack(), m_drawKey, m_lastVelocity);
                    m_auditioned = true;
                }
                update();
            }
        } else if (m_drag == Drag::Band) {
            update();
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (m_kbdKey >= 0) {
            m_sv->audition(m_sv->selectedTrack(), m_kbdKey, 0);
            m_kbdKey = -1;
        }
        if (m_auditioned) {
            m_sv->audition(m_sv->selectedTrack(), 0, 0);
            m_auditioned = false;
        }
        SongDocument *doc = m_sv->document();
        if (event->button() == Qt::RightButton && m_rightPress) {
            const Drag drag = m_drag;
            m_rightPress = false;
            m_drag = Drag::None;
            if (drag == Drag::Band) {
                selectBand(QRect(m_pressPos, m_curPos).normalized(),
                           event->modifiers() & Qt::ControlModifier);
            } else if (doc && m_rightHit) {
                const std::vector<SongView::NoteId> &sel = m_sv->selection();
                if (std::find(sel.begin(), sel.end(), m_rightHitId) == sel.end())
                    m_sv->setSelection({m_rightHitId});
                showNoteMenu(event->pos());
            } else {
                m_sv->clearSelection();
            }
            update();
            return;
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
            m_rightPress = false;
            m_sv->clearSelection();
            update();
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void leaveEvent(QEvent *) override
    {
        if (m_hasHover) {
            m_hasHover = false;
            update();
        }
    }

private:
    enum class Drag { None, Band, Move, Resize, ResizeLeft, Velocity, Draw };

    int keyToY(int key) const
    {
        return (127 - key) * m_sv->keyHeight() - m_sv->scrollY();
    }

    int yToKey(int y) const
    {
        return std::clamp(127 - (y + m_sv->scrollY()) / m_sv->keyHeight(), 0, 127);
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
        const QRect r = noteRect(note);
        return r.width() >= kVelHandleMinNoteW && pos.x() > r.left() + kEdgeW
            && pos.x() <= r.left() + kEdgeW + kVelHandleW;
    }

    QRect velHandleRect(const QRect &noteRect) const
    {
        return QRect(noteRect.left() + kEdgeW + 2, noteRect.top() + 1, 3,
                     std::max(2, noteRect.height() - 2));
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

    // Fills the clipboard with the notes, ticks relative to the block start.
    void copyNotes(const std::vector<DocNote> &notes)
    {
        uint64_t base = UINT64_MAX;
        for (const DocNote &note : notes)
            base = std::min(base, note.tick);
        std::vector<SongView::ClipNote> &clip = m_sv->noteClipboard();
        clip.clear();
        for (const DocNote &note : notes)
            clip.push_back({uint32_t(note.tick - base), note.key,
                            note.duration ? note.duration
                                          : uint32_t(m_sv->gridTicks()),
                            note.velocity});
        m_sv->announce(SongView::tr("Copied %n note(s)", nullptr, int(notes.size())));
    }

    // Pastes the clipboard onto the selected track, anchored at the edit
    // cursor (snapped to the grid), and selects the pasted notes.
    void pasteAtEditCursor()
    {
        SongDocument *doc = m_sv->document();
        const std::vector<SongView::ClipNote> &clip = m_sv->noteClipboard();
        if (!doc || clip.empty())
            return;
        const uint64_t base = m_sv->snapTick(double(m_sv->editCursorTick()));
        std::vector<SongDocument::NewNote> notes;
        std::vector<SongView::NoteId> ids;
        for (const SongView::ClipNote &cn : clip) {
            const uint64_t tick = base + cn.relTick;
            notes.push_back({tick, cn.key, cn.duration, cn.velocity});
            ids.push_back({uint32_t(tick), cn.key});
        }
        doc->addNotes(m_sv->selectedTrack(), notes);
        m_sv->setSelection(std::move(ids));
        m_sv->announce(SongView::tr("Pasted %n note(s)", nullptr, int(clip.size())));
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
                // Velocity grip on the hovered/selected notes (drag it
                // vertically to adjust the whole selection).
                const bool hovered = m_hasHover && note.startTick == m_hoverTick
                                     && note.key == m_hoverKey;
                if ((hovered || m_sv->isSelected(note))
                    && r.width() >= kVelHandleMinNoteW)
                    p.fillRect(velHandleRect(r),
                               SongView::trackColor(note.track).darker(170));
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
            if (isBlackKey(key)) {
                p.fillRect(QRect(0, y, kKeyboardW * 3 / 5, keyH), QColor(0x2e, 0x2e, 0x2e));
            } else if (key % 12 == 0) {
                p.setPen(QColor(0x9a, 0x9a, 0x9a));
                p.drawLine(0, y + keyH, kKeyboardW, y + keyH);
                p.setPen(QColor(0x50, 0x50, 0x50));
                if (keyH >= 7)
                    p.drawText(QRect(0, y, kKeyboardW - 3, keyH),
                               Qt::AlignRight | Qt::AlignVCenter, keyName(key));
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
    bool m_rightPress = false; // right button held; band vs. menu undecided
    bool m_rightHit = false;   // that press landed on a note…
    SongView::NoteId m_rightHitId{}; // …this one
    ViewNote m_velAnchor{};    // pressed note of a velocity drag (a copy)
    int m_velAudEff = -1;      // last effective velocity auditioned mid-drag
    uint32_t m_hoverTick = 0;  // hovered note, for its velocity grip
    uint8_t m_hoverKey = 0;
    bool m_hasHover = false;
    int m_kbdKey = -1;         // key sounding from a keyboard-column press
    bool m_auditioned = false; // a drag/draw preview note is sounding
    uint8_t m_lastVelocity = 100;
};

// ----------------------------------------------------------- AutomationArea

class AutomationArea : public QWidget
{
public:
    AutomationArea(SongView *sv, QScrollArea *scroll)
        : QWidget(nullptr), m_sv(sv), m_scroll(scroll) // parented by the scroll area
    {
        setMinimumHeight(kLaneH);
    }

    void rebuildRows()
    {
        m_rows.clear();
        m_dragRow = -1;
        m_gesture = Gesture::None;
        m_sweep.clear();
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

        for (size_t i = 0; i < m_rows.size(); i++)
            paintRow(p, m_rows[i], QRect(0, int(i) * m_laneH, width(), m_laneH));

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
            const int top = m_dragRow * m_laneH + 5;
            const int bottom = (m_dragRow + 1) * m_laneH - 1 - 4;
            auto valueY = [&](int v) {
                return bottom - (v - minV) * (bottom - top) / std::max(1, maxV - minV);
            };
            auto tickX = [&](uint64_t t) {
                return kGutterW + m_sv->contentX(double(t));
            };
            p.setClipRect(
                QRect(kGutterW, m_dragRow * m_laneH, width() - kGutterW, m_laneH));
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
            p.drawText(QPoint(x + 6, y - 4), QString::number(m_dragValue));
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
        SongDocument *doc = m_sv->document();
        if (!doc)
            return;
        if (event->button() == Qt::LeftButton
            && addLaneRect().contains(event->pos())) {
            showAddLaneMenu(event->globalPosition().toPoint());
            return;
        }
        const int ri = event->pos().y() / m_laneH;
        if (ri < 0 || ri >= int(m_rows.size()))
            return;
        const Row &row = m_rows[ri];
        if (event->pos().x() < kGutterW) {
            if (row.kind == Row::Lane
                && (event->button() == Qt::LeftButton
                    || event->button() == Qt::RightButton))
                showLaneMenu(*row.lane, event->globalPosition().toPoint());
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

        const LanePoint *nearPt = nearestPoint(row, event->pos().x());
        if (event->button() == Qt::RightButton) {
            if (nearPt) {
                DocLanePoint pt;
                if (doc->findLanePoint(track, cc, nearPt->tick, &pt))
                    doc->deleteLanePoints(track, cc, {pt});
            }
            return;
        }
        if (event->button() != Qt::LeftButton)
            return;
        m_dragRow = ri;
        const bool fine = event->modifiers() & Qt::AltModifier;
        updateDrag(event->pos(), fine);
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
        if (m_dragRow < 0)
            return;
        const bool fine = event->modifiers() & Qt::AltModifier;
        updateDrag(event->pos(), fine);
        if (m_gesture == Gesture::Sweep)
            extendSweep(event->pos(), fine);
        update();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
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
            if (doc->findLanePoint(track, cc, uint64_t(m_dragOrigTick), &pt))
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

private:
    struct Row {
        enum Kind { Tempo, Voice, Lane } kind;
        const AutoLane *lane;
    };

    QRect addLaneRect() const
    {
        return QRect(0, int(m_rows.size()) * m_laneH, width(), kAddLaneH);
    }

    void applyHeight()
    {
        // Minimum, not fixed: the scroll area stretches the widget to fill
        // its viewport when the user drags the lanes area taller.
        const int addH = m_sv->timeline() && m_sv->document() ? kAddLaneH : 0;
        setMinimumHeight(std::max(m_laneH, int(m_rows.size()) * m_laneH + addH));
    }

    // Ctrl+wheel: rescale the lane rows (the roll's key-height analog),
    // keeping the row under the cursor pinned. anchorY is widget-local, so
    // it already includes the scroll offset.
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
        const double row = double(anchorY) / double(m_laneH);
        m_laneH = newH;
        applyHeight();
        if (m_scroll) {
            QScrollBar *vbar = m_scroll->verticalScrollBar();
            const int viewportY = anchorY - vbar->value();
            vbar->setValue(int(std::lround(row * newH)) - viewportY);
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
        QAction *clear = menu.addAction(SongView::tr("Clear events"));
        clear->setEnabled(!empty);
        QAction *del = menu.addAction(empty ? SongView::tr("Remove empty lane")
                                            : SongView::tr("Delete lane"));
        QAction *chosen = menu.exec(globalPos);
        if (!chosen)
            return;

        SongDocument *doc = m_sv->document();
        const std::vector<DocLanePoint> points = doc->lanePoints(track, cc);
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

    // Voice row: left-click a marker re-picks its voice, left-click on empty
    // space inserts a change at the snapped tick, right-click deletes a
    // marker. The value axis is meaningless here (a voice is an identity, not
    // a level), so the picker dialog replaces the lanes' drag editing.
    void voiceRowPress(QMouseEvent *event)
    {
        SongDocument *doc = m_sv->document();
        const int track = m_sv->selectedTrack();
        const std::vector<DocLanePoint> changes = doc->lanePoints(track, DOC_CC_VOICE);

        const DocLanePoint *hit = nullptr;
        int bestDist = 9; // same radius as nearestPoint
        for (const DocLanePoint &pt : changes) {
            const int dist =
                std::abs(kGutterW + m_sv->contentX(double(pt.tick)) - event->pos().x());
            if (dist < bestDist) {
                bestDist = dist;
                hit = &pt;
            }
        }

        if (event->button() == Qt::RightButton) {
            if (hit)
                doc->deleteLanePoints(track, DOC_CC_VOICE, {*hit});
            return;
        }
        if (event->button() != Qt::LeftButton)
            return;
        if (hit) {
            int voice = hit->value;
            if (m_sv->pickVoice(SongView::tr("Change voice"), hit->value, &voice)
                && voice != hit->value)
                doc->moveLanePoint(track, DOC_CC_VOICE, *hit, hit->tick, voice);
        } else {
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
        const int top = ri * m_laneH + 5;
        const int bottom = (ri + 1) * m_laneH - 1 - 4;
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

    void updateDrag(QPoint pos, bool fine)
    {
        if (m_dragRow < 0 || m_dragRow >= int(m_rows.size()))
            return;
        const Row &row = m_rows[m_dragRow];
        int minV, maxV;
        rowRange(row, &minV, &maxV);
        // Invert paintCurve's valueY mapping.
        const int top = m_dragRow * m_laneH + 5;
        const int bottom = (m_dragRow + 1) * m_laneH - 1 - 4;
        const int y = std::clamp(pos.y(), top, bottom);
        m_dragValue = minV + (bottom - y) * (maxV - minV) / std::max(1, bottom - top);
        if (row.kind == Row::Tempo)
            m_dragValue = std::max(1, m_dragValue);
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
        QString name;
        QString range;
        int minV = 0, maxV = 127;
        const std::vector<LanePoint> *points = nullptr;
        QColor curve = palette().color(QPalette::Highlight);
        rowRange(row, &minV, &maxV);
        switch (row.kind) {
        case Row::Tempo:
            name = SongView::tr("Tempo (BPM)");
            points = &m_sv->model().tempoLane;
            curve = QColor(0xb0, 0x60, 0xd0);
            break;
        case Row::Voice:
            name = SongView::tr("Voice");
            break;
        case Row::Lane: {
            const M4aCcInfo info = m4aClassifyCc(row.lane->cc);
            name = row.lane->cc == LANE_CC_BEND
                       ? SongView::tr("Pitch bend (BEND)")
                       : QStringLiteral("%1 (%2)").arg(row.lane->name,
                                                       QLatin1String(info.name));
            points = &row.lane->points;
            curve = SongView::trackColor(row.lane->track);
            break;
        }
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

        drawOverlays(p, m_sv, plot, kGutterW);
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
    // grid instead of the visible grid throughout.
    enum class Gesture { None, Point, Sweep, Line };

    SongView *m_sv;
    QScrollArea *m_scroll;      // hosting scroll area, for lane-zoom pinning
    std::vector<Row> m_rows;
    int m_laneH = kLaneH;       // row height; Ctrl+wheel rescales
    int m_laneZoomAccum = 0;    // sub-notch wheel remainder, like zoomKeyHeight
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
        drawOverlays(p, m_sv, area, kGutterW);

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

    void mousePressEvent(QMouseEvent *) override { m_sv->selectTrack(m_track); }

    void mouseDoubleClickEvent(QMouseEvent *) override
    {
        m_sv->selectTrack(m_track);
        m_sv->editTrackVoice(m_track);
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
                    tip += SongView::tr("\nDouble-click to change the voice");
                row->setToolTip(tip);
                m_layout->insertWidget(m_layout->count() - 1, row);
                m_rows.push_back(row);
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
    m_noteClipboard.clear();
    m_muteMask = 0;
    m_soloMask = 0;
    emit muteMaskChanged(0);
    emit soloMaskChanged(0);
    m_playheadTick = 0.0;
    m_editCursorTick = 0;
    m_playing = false;
    m_scrollPx = 0;

    m_selectedTrack = 0;
    if (timeline) {
        for (int t = 0; t < 16; t++) {
            if (timeline->tracks[t].used) {
                m_selectedTrack = t;
                break;
            }
        }
    }

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
    m_roll->update();
}

void SongView::clearSelection()
{
    if (!m_selection.empty()) {
        m_selection.clear();
        m_roll->update();
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

double SongView::pxPerBeat() const
{
    return m_timeline ? m_pxPerTick * m_timeline->ticksPerBeat : m_pxPerTick * 24.0;
}

void SongView::selectTrack(int track)
{
    if (track == m_selectedTrack || track < 0 || track > 15)
        return;
    m_selectedTrack = track;
    m_selection.clear();
    m_headers->syncSelection();
    m_lanes->rebuildRows();
    // Switching tracks readies the roll for keyboard editing (e.g. copy on
    // one track, click another's header, paste), wherever focus was.
    m_roll->setFocus();
    m_roll->update();
    emit selectedTrackChanged(track);
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
