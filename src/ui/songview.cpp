#include "songview.h"

#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QScrollBar>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <climits>
#include <cmath>

namespace songview {

namespace {

constexpr int kRulerH = 26;
constexpr int kLaneH = 48;
constexpr int kLanesAreaH = 150;
constexpr int kStripH = 24;
constexpr int kTrackRowH = 46;
constexpr double kMinPxPerBeat = 4.0;
constexpr double kMaxPxPerBeat = 640.0;

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

QString voiceTypeName(uint8_t type)
{
    if (type == VOICE_KEYSPLIT)
        return SongView::tr("Keysplit");
    if (type == VOICE_KEYSPLIT_ALL)
        return SongView::tr("Drumkit");
    switch (type & VOICE_TYPE_CGB_MASK) {
    case VOICE_SQUARE_1: return SongView::tr("Square 1");
    case VOICE_SQUARE_2: return SongView::tr("Square 2");
    case VOICE_PROGRAMMABLE_WAVE: return SongView::tr("Wave");
    case VOICE_NOISE: return SongView::tr("Noise");
    default: return SongView::tr("Sample");
    }
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

    const int px = origin + sv->contentX(sv->playheadTick());
    if (px >= rect.left() && px <= rect.right()) {
        p.setPen(QPen(playheadColor(), 1));
        p.drawLine(px, rect.top(), px, rect.bottom());
    }
}

// Vertical bar/beat grid lines inside rect.
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
        const int dy = event->angleDelta().y();
        if (event->modifiers() & Qt::ControlModifier)
            m_sv->zoomAroundContentX(std::pow(1.0015, dy),
                                     int(event->position().x()) - kGutterW);
        else
            m_sv->scrollByPx(-dy);
        event->accept();
    }

private:
    SongView *m_sv;
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

        drawOverlays(p, m_sv, grid, kKeyboardW);

        p.setClipping(false);
        drawKeyboard(p);
    }

    void wheelEvent(QWheelEvent *event) override
    {
        const int dy = event->angleDelta().y();
        if (event->modifiers() & Qt::ControlModifier)
            m_sv->zoomAroundContentX(std::pow(1.0015, dy),
                                     int(event->position().x()) - kKeyboardW);
        else if (event->modifiers() & Qt::ShiftModifier)
            m_sv->scrollByPx(-dy);
        else
            m_sv->scrollRollBy(-dy / 2);
        event->accept();
    }

private:
    int keyToY(int key) const
    {
        return (127 - key) * m_sv->keyHeight() - m_sv->scrollY();
    }

    void drawNotes(QPainter &p, const SongViewModel &model, int selected, bool ghostPass)
    {
        const int keyH = m_sv->keyHeight();
        for (const ViewNote &note : model.notes) {
            const bool ghost = note.track != selected;
            if (ghost != ghostPass)
                continue;
            const int x0 = kKeyboardW + m_sv->contentX(double(note.startTick));
            const int x1 = kKeyboardW + m_sv->contentX(double(note.endTick));
            if (x1 < kKeyboardW || x0 > width())
                continue;
            const int y = keyToY(note.key);
            if (y + keyH < 0 || y > height())
                continue;
            QRect r(x0, y + 1, std::max(2, x1 - x0), std::max(2, keyH - 1));
            QColor c = SongView::trackColor(note.track);
            if (ghost) {
                c.setAlpha(60);
                p.fillRect(r, c);
            } else {
                c.setAlpha(120 + note.velocity); // velocity shows as opacity
                p.fillRect(r, c);
                p.setPen(note.unterminated
                             ? QPen(playheadColor(), 1, Qt::DashLine)
                             : QPen(SongView::trackColor(note.track).darker(150), 1));
                p.drawRect(r.adjusted(0, 0, -1, -1));
            }
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

    SongView *m_sv;
};

// ----------------------------------------------------------- AutomationArea

class AutomationArea : public QWidget
{
public:
    explicit AutomationArea(SongView *sv)
        : QWidget(nullptr), m_sv(sv) // parented by the scroll area
    {
        setMinimumHeight(kLaneH);
    }

    void rebuildRows()
    {
        m_rows.clear();
        if (m_sv->timeline()) {
            m_rows.push_back({Row::Tempo, nullptr});
            const SongViewModel &model = m_sv->model();
            const int selected = m_sv->selectedTrack();
            for (const VoiceChange &vc : model.voices) {
                if (vc.track == selected) {
                    m_rows.push_back({Row::Voice, nullptr});
                    break;
                }
            }
            for (const AutoLane &lane : model.lanes)
                if (lane.track == selected)
                    m_rows.push_back({Row::Lane, &lane});
        }
        setFixedHeight(std::max(kLaneH, int(m_rows.size()) * kLaneH));
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
            paintRow(p, m_rows[i], QRect(0, int(i) * kLaneH, width(), kLaneH));
    }

    void wheelEvent(QWheelEvent *event) override
    {
        if (event->modifiers() & Qt::ControlModifier) {
            m_sv->zoomAroundContentX(std::pow(1.0015, event->angleDelta().y()),
                                     int(event->position().x()) - kGutterW);
            event->accept();
        } else {
            event->ignore(); // let the scroll area page vertically
        }
    }

private:
    struct Row {
        enum Kind { Tempo, Voice, Lane } kind;
        const AutoLane *lane;
    };

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
        switch (row.kind) {
        case Row::Tempo: {
            name = SongView::tr("Tempo (BPM)");
            points = &m_sv->model().tempoLane;
            maxV = 200;
            for (const LanePoint &pt : *points)
                maxV = std::max(maxV, pt.value + 20);
            minV = 0;
            curve = QColor(0xb0, 0x60, 0xd0);
            break;
        }
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
            if (row.lane->cc == LANE_CC_BEND) {
                minV = -8192;
                maxV = 8191;
            }
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

    SongView *m_sv;
    std::vector<Row> m_rows;
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
        connect(m_mute, &QToolButton::toggled, this,
                [this](bool on) { m_sv->setTrackMute(m_track, on); });
        m_solo = new QToolButton(this);
        m_solo->setText(QStringLiteral("S"));
        m_solo->setCheckable(true);
        m_solo->setFixedSize(20, 18);
        m_solo->setToolTip(SongView::tr("Solo"));
        m_solo->setStyleSheet(
            QStringLiteral("QToolButton:checked { background: #5cb85c; color: white; }"));
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
                row->setToolTip(SongView::tr("%1 notes · program %2")
                                    .arg(tl->tracks[t].noteCount)
                                    .arg(tl->tracks[t].firstProgram));
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

    auto *mid = new QHBoxLayout;
    mid->setSpacing(0);
    auto *headerScroll = new QScrollArea(this);
    headerScroll->setFixedWidth(kHeaderW);
    headerScroll->setFrameShape(QFrame::NoFrame);
    headerScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    headerScroll->setWidgetResizable(true);
    m_headers = new TrackHeaderPanel(this);
    headerScroll->setWidget(m_headers);
    mid->addWidget(headerScroll);
    m_roll = new PianoRoll(this);
    mid->addWidget(m_roll, 1);
    m_vbar = new QScrollBar(Qt::Vertical, this);
    mid->addWidget(m_vbar);
    vbox->addLayout(mid, 1);

    m_lanesScroll = new QScrollArea(this);
    m_lanesScroll->setFixedHeight(kLanesAreaH);
    m_lanesScroll->setFrameShape(QFrame::NoFrame);
    m_lanesScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_lanesScroll->setWidgetResizable(true);
    m_lanes = new AutomationArea(this);
    m_lanesScroll->setWidget(m_lanes);
    vbox->addWidget(m_lanesScroll);

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
    m_muteMask = 0;
    m_soloMask = 0;
    emit muteMaskChanged(0);
    emit soloMaskChanged(0);
    m_playheadTick = 0.0;
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

double SongView::pxPerBeat() const
{
    return m_timeline ? m_pxPerTick * m_timeline->ticksPerBeat : m_pxPerTick * 24.0;
}

void SongView::selectTrack(int track)
{
    if (track == m_selectedTrack || track < 0 || track > 15)
        return;
    m_selectedTrack = track;
    m_headers->syncSelection();
    m_lanes->rebuildRows();
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
        type = voiceTypeName(m_voicegroup->voices[program].type);
    }
    if (name.isEmpty())
        return type.isEmpty() ? tr("Voice") : type;
    return QStringLiteral("%1 (%2)").arg(name, type);
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
    updateScrollbars();
}
