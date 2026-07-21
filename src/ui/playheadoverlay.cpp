#include "playheadoverlay.h"

#include <QEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

namespace songview {

namespace {

// Paused = centered on the bar, dimmer. Playing = 1 unit less radius and
// left-trailing only. Core is 1px in both states.
constexpr qreal kLineWidth = 1.0;
constexpr qreal kPeakPlaying = 0.13;
constexpr qreal kPeakPaused = 0.06;

const QPainterPath kPlayheadTriangle = [] {
    QPainterPath path;
    path.moveTo(-kPlayheadTriangleHalfWidth, 0);
    path.lineTo(kPlayheadTriangleHalfWidth, 0);
    path.lineTo(0, kPlayheadTriangleHeight);
    path.closeSubpath();
    return path;
}();

// Quadratic bloom: t=0 outer (α=0) → t=1 at the bar (α=peak).
void setQuadStops(QLinearGradient &g, const QColor &color, qreal peakAlpha)
{
    QColor stopColor = color;
    for (int i = 0; i <= 8; ++i) {
        const qreal t = qreal(i) / 8.0;
        stopColor.setAlphaF(peakAlpha * t * t);
        g.setColorAt(t, stopColor);
    }
}

void paintGlow(QPainter &painter, qreal x, int top, int height, qreal left,
               qreal right, const QColor &color, qreal peakAlpha)
{
    if (left > 0.0) {
        QLinearGradient gradient(x - left, 0, x, 0);
        setQuadStops(gradient, color, peakAlpha);
        painter.fillRect(QRectF(x - left, top, left, height), gradient);
    }
    if (right > 0.0) {
        QLinearGradient gradient(x + right, 0, x, 0);
        setQuadStops(gradient, color, peakAlpha);
        painter.fillRect(QRectF(x, top, right, height), gradient);
    }
}

} // namespace

PlayheadOverlay::PlayheadOverlay(QWidget *owner, const Surfaces &surfaces,
                                 const QColor &color)
    : QWidget(owner)
    , m_surfaces(surfaces)
    , m_color(color)
{
    Q_ASSERT(owner);
    Q_ASSERT(m_surfaces.ruler);
    Q_ASSERT(m_surfaces.roll);
    Q_ASSERT(m_surfaces.lanes);
    Q_ASSERT(m_surfaces.strip);

    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);

    const auto observeSurfaceGeometry = [this, owner](QWidget *surface) {
        for (QWidget *widget = surface; widget; widget = widget->parentWidget()) {
            widget->installEventFilter(this);
            if (widget == owner)
                break;
        }
    };
    observeSurfaceGeometry(m_surfaces.ruler);
    observeSurfaceGeometry(m_surfaces.roll);
    observeSurfaceGeometry(m_surfaces.lanes);
    observeSurfaceGeometry(m_surfaces.strip);

    synchronizeGeometry();
    show();
}

void PlayheadOverlay::setPlayhead(qreal timelineX, bool visible, bool playing)
{
    const qreal oldX = qreal(m_timelineOrigin) + m_timelineX;
    const bool oldVisible = m_visible;
    const bool oldPlaying = m_playing;

    m_timelineX = timelineX;
    m_visible = visible;
    m_playing = playing;
    const qreal newX = qreal(m_timelineOrigin) + m_timelineX;

    if (oldX == newX && oldVisible == m_visible && oldPlaying == m_playing)
        return;

    QRegion dirty;
    if (oldVisible)
        dirty += playheadRegion(oldX);
    if (m_visible)
        dirty += playheadRegion(newX);
    if (!dirty.isEmpty())
        update(dirty);
}

bool PlayheadOverlay::eventFilter(QObject *, QEvent *event)
{
    switch (event->type()) {
    case QEvent::Move:
    case QEvent::Resize:
    case QEvent::Show:
    case QEvent::Hide:
    case QEvent::LayoutRequest:
        synchronizeGeometry();
        break;
    default:
        break;
    }
    return false;
}

void PlayheadOverlay::paintEvent(QPaintEvent *)
{
    if (!m_visible || m_playheadGeometry.isEmpty()
        || (m_visibleSurfaceRegion.isEmpty() && m_triangleClip.isEmpty()))
        return;

    QPainter painter(this);
    painter.setClipRegion(m_visibleSurfaceRegion);
    painter.setRenderHint(QPainter::Antialiasing);

    const int playheadTop = m_playheadGeometry.top();
    const int height = m_playheadGeometry.height();
    const qreal playheadX = qreal(m_timelineOrigin) + m_timelineX;
    const qreal leftExtent =
        m_playing ? qreal(kPlayheadGlowRadius - 1) : qreal(kPlayheadGlowRadius);
    const qreal rightExtent = m_playing ? 0.0 : qreal(kPlayheadGlowRadius);
    const qreal peak = m_playing ? kPeakPlaying : kPeakPaused;
    paintGlow(painter, playheadX, playheadTop, height, leftExtent, rightExtent,
              m_color, peak);

    QPen core(m_color, kLineWidth, Qt::SolidLine, Qt::FlatCap);
    painter.setPen(core);
    painter.drawLine(QPointF(playheadX, playheadTop),
                     QPointF(playheadX, m_playheadGeometry.bottom()));

    painter.setClipRect(m_triangleClip, Qt::ReplaceClip);
    const bool trianglePointsUp = !m_surfaces.roll->isVisible();
    painter.translate(
        playheadX, playheadTop + (trianglePointsUp ? kPlayheadTriangleHeight : 0));
    if (trianglePointsUp)
        painter.scale(1.0, -1.0);
    painter.fillPath(kPlayheadTriangle, m_color);
}

QRect PlayheadOverlay::visibleSurfaceRect(const QWidget *surface, QWidget *owner,
                                          int origin) const
{
    if (origin >= surface->width())
        return {};
    QPoint offset = surface->mapTo(owner, QPoint(0, 0));
    QRect visible(offset + QPoint(origin, 0),
                  QSize(surface->width() - origin, surface->height()));
    for (const QWidget *widget = surface; widget; widget = widget->parentWidget()) {
        if (!widget->isVisible())
            return {};

        visible &= QRect(widget->mapTo(owner, QPoint(0, 0)), widget->size());
        if (widget == owner)
            break;
    }
    return visible;
}

QRegion PlayheadOverlay::playheadRegion(qreal x) const
{
    if (m_playheadGeometry.isEmpty())
        return {};

    constexpr qreal kAntialiasPadding = 1.0;
    // Max extent is the paused centered bloom (radius each side).
    const QRect bounds =
        QRectF(x - kPlayheadGlowRadius - kAntialiasPadding,
               m_playheadGeometry.top(),
               2.0 * kPlayheadGlowRadius + kPlayheadTriangleHalfWidth
                   + kAntialiasPadding * 2.0,
               m_playheadGeometry.height())
            .toAlignedRect()
            .intersected(m_playheadGeometry);
    return QRegion(bounds).intersected(m_visibleSurfaceRegion + m_triangleClip);
}

void PlayheadOverlay::synchronizeGeometry()
{
    QWidget *owner = parentWidget();
    Q_ASSERT(owner);

    const QRect rulerGeometry(m_surfaces.ruler->mapTo(owner, QPoint(0, 0)),
                              m_surfaces.ruler->size());
    const int playheadTop = rulerGeometry.bottom() + 1;
    const QRect playheadGeometry(0, playheadTop, owner->width(),
                                 owner->height() - playheadTop);
    const QRect rulerVisible =
        visibleSurfaceRect(m_surfaces.ruler, owner, m_surfaces.rulerOrigin);
    const QRect triangleClip(rulerVisible.left(), playheadTop,
                             rulerVisible.width(), kPlayheadTriangleHeight + 1);
    const QRegion visibleSurfaces =
        QRegion(visibleSurfaceRect(m_surfaces.roll, owner, m_surfaces.rollOrigin))
        + visibleSurfaceRect(m_surfaces.lanes, owner, m_surfaces.lanesOrigin)
        + visibleSurfaceRect(m_surfaces.strip, owner, m_surfaces.stripOrigin);
    const int timelineOrigin =
        m_surfaces.ruler->mapTo(owner, QPoint(m_surfaces.rulerOrigin, 0)).x();
    const bool overlayGeometryChanged = geometry() != owner->rect();
    const bool surfaceGeometryChanged =
        m_playheadGeometry != playheadGeometry
        || m_visibleSurfaceRegion != visibleSurfaces
        || m_triangleClip != triangleClip
        || m_timelineOrigin != timelineOrigin;
    if (!overlayGeometryChanged && !surfaceGeometryChanged)
        return;
    const qreal oldX = qreal(m_timelineOrigin) + m_timelineX;
    const QRegion oldDirty = m_visible ? playheadRegion(oldX) : QRegion();

    if (overlayGeometryChanged)
        setGeometry(owner->rect());

    m_visibleSurfaceRegion = visibleSurfaces;
    m_playheadGeometry = playheadGeometry;
    m_triangleClip = triangleClip;
    m_timelineOrigin = timelineOrigin;

    const qreal newX = qreal(m_timelineOrigin) + m_timelineX;
    const QRegion dirty =
        (oldDirty | (m_visible ? playheadRegion(newX) : QRegion())) - rulerGeometry;
    if (!dirty.isEmpty())
        update(dirty);
    raise();
}

} // namespace songview
