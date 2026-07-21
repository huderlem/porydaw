#include "playheadoverlay.h"

#include <QEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

namespace songview {

namespace {

constexpr int kGlowWidth = 7;
constexpr int kPlayheadHalfWidth = kGlowWidth;
constexpr int kLineWidth = 1;

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

    QColor transparent = color;
    transparent.setAlpha(0);
    QColor stoppedGlowColor = color;
    stoppedGlowColor.setAlphaF(color.alphaF() * 0.2);
    QColor playingGlowColor = color;
    playingGlowColor.setAlphaF(color.alphaF() * 0.3);
    QColor glowAt15Percent = playingGlowColor;
    glowAt15Percent.setAlphaF(playingGlowColor.alphaF() * 0.15);
    QColor glowAt75Percent = playingGlowColor;
    glowAt75Percent.setAlphaF(playingGlowColor.alphaF() * 0.75);
    QLinearGradient playingGlow(0, 0, kGlowWidth, 0);
    playingGlow.setColorAt(0, transparent);
    playingGlow.setColorAt(0.5, glowAt15Percent);
    playingGlow.setColorAt(0.75, glowAt75Percent);
    playingGlow.setColorAt(1.0 - 1.0 / kGlowWidth, playingGlowColor);
    m_playingGlow = QBrush(playingGlow);
    QLinearGradient stoppedGlow(0, 0, kGlowWidth, 0);
    stoppedGlow.setColorAt(0, transparent);
    stoppedGlow.setColorAt(0.5, stoppedGlowColor);
    stoppedGlow.setColorAt(0.65, transparent);
    m_stoppedGlow = QBrush(stoppedGlow);

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

void PlayheadOverlay::setPlayhead(qreal contentX, bool visible, bool playing)
{
    const qreal oldX = m_playheadX;
    const bool oldVisible = m_visible;
    const bool oldPlaying = m_playing;

    m_playheadContentX = contentX;
    m_playheadX = qreal(m_timelineOrigin) + contentX;
    m_visible = visible;
    m_playing = playing;

    if (oldX == m_playheadX && oldVisible == m_visible && oldPlaying == m_playing)
        return;

    QRegion dirty;
    if (oldVisible)
        dirty += playheadRegion(oldX);
    if (m_visible)
        dirty += playheadRegion(m_playheadX);
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
    if (!m_visible || m_visibleSurfaceRegion.isEmpty()
        || m_playheadGeometry.isEmpty())
        return;

    QPainter painter(this);
    painter.setClipRegion(m_visibleSurfaceRegion);
    painter.setRenderHint(QPainter::Antialiasing);

    const int playheadTop = m_playheadGeometry.top();
    const qreal glowOffset = m_playing ? kGlowWidth : kGlowWidth / 2.0;
    const qreal glowStartX = m_playheadX - glowOffset;
    const QBrush &glow = m_playing ? m_playingGlow : m_stoppedGlow;
    painter.save();
    painter.translate(glowStartX, playheadTop);
    painter.fillRect(0, 0, kGlowWidth, m_playheadGeometry.height(), glow);
    painter.restore();

    painter.setPen(QPen(m_color, kLineWidth));
    painter.drawLine(QPointF(m_playheadX, playheadTop),
                     QPointF(m_playheadX, m_playheadGeometry.bottom()));

    QPainterPath triangle;
    triangle.moveTo(m_playheadX - 4, playheadTop);
    triangle.lineTo(m_playheadX + 4, playheadTop);
    triangle.lineTo(m_playheadX, playheadTop + 8);
    triangle.closeSubpath();
    painter.fillPath(triangle, m_color);
}

QRect PlayheadOverlay::surfaceGeometry(const QWidget *surface) const
{
    const QWidget *owner = parentWidget();
    return QRect(surface->mapTo(owner, QPoint(0, 0)), surface->size());
}

QRegion PlayheadOverlay::visibleSurfaceRegion(const QWidget *surface, int origin) const
{
    const QWidget *owner = parentWidget();
    if (origin >= surface->width())
        return {};

    QRegion visible(QRect(surface->mapTo(owner, QPoint(origin, 0)),
                          QSize(surface->width() - origin, surface->height())));
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
    const QRect bounds =
        QRectF(x - kPlayheadHalfWidth - kAntialiasPadding,
               m_playheadGeometry.top(),
               kPlayheadHalfWidth * 2.0 + kAntialiasPadding * 2.0,
               m_playheadGeometry.height())
            .toAlignedRect()
            .intersected(m_playheadGeometry);
    return QRegion(bounds).intersected(m_visibleSurfaceRegion);
}

void PlayheadOverlay::synchronizeGeometry()
{
    QWidget *owner = parentWidget();
    Q_ASSERT(owner);

    const QRegion oldVisibleSurfaceRegion = m_visibleSurfaceRegion;
    const QRect rollGeometry = surfaceGeometry(m_surfaces.roll);
    const QRect playheadGeometry(0, rollGeometry.top(), owner->width(),
                                 owner->height() - rollGeometry.top());
    const QRegion visibleSurfaces =
        visibleSurfaceRegion(m_surfaces.roll, m_surfaces.rollOrigin)
        + visibleSurfaceRegion(m_surfaces.lanes, m_surfaces.lanesOrigin)
        + visibleSurfaceRegion(m_surfaces.strip, m_surfaces.stripOrigin);
    const int timelineOrigin =
        m_surfaces.ruler->mapTo(owner, QPoint(m_surfaces.rulerOrigin, 0)).x();
    const bool overlayGeometryChanged = geometry() != owner->rect();
    const bool surfaceGeometryChanged =
        m_playheadGeometry != playheadGeometry
        || m_visibleSurfaceRegion != visibleSurfaces
        || m_timelineOrigin != timelineOrigin;

    if (overlayGeometryChanged)
        setGeometry(owner->rect());

    m_visibleSurfaceRegion = visibleSurfaces;
    m_playheadGeometry = playheadGeometry;
    m_timelineOrigin = timelineOrigin;
    m_playheadX = qreal(m_timelineOrigin) + m_playheadContentX;

    if (overlayGeometryChanged || surfaceGeometryChanged) {
        update(oldVisibleSurfaceRegion.united(m_visibleSurfaceRegion));
        raise();
    }
}

} // namespace songview
