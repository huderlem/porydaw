#include "playheadoverlay.h"
#include "theme/themeruntime.h"

#include <QEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QtMath>
#include <algorithm>
#include <utility>

namespace songview {

namespace {

// Paused = centered on the bar, dimmer. Playing = 1 unit less radius and
// left-trailing only (the half-line-width right extent keeps room for the
// core in the pre-rendered platform strip). Core is 1px in both states.
constexpr qreal playheadGlowLeftExtent(bool playing)
{
    return playing ? qreal(kPlayheadGlowRadius - 1) : qreal(kPlayheadGlowRadius);
}

constexpr qreal playheadGlowRightExtent(bool playing)
{
    return playing ? (kPlayheadLineWidth / 2.0) : qreal(kPlayheadGlowRadius);
}

constexpr qreal playheadPeakAlpha(bool playing)
{
    return playing ? kPlayheadPeakPlaying : kPlayheadPeakPaused;
}

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

// Draws the glow + 1px core with the bar at x. All coordinates may be
// fractional: the playhead position is sample-accurate and the antialiased
// vector fill keeps its subpixel motion (rollcheck asserts this at dpr 1).
void paintPlayheadBody(QPainter &painter, qreal x, int top, int height,
                       bool playing, const QColor &color)
{
    const qreal left = playheadGlowLeftExtent(playing);
    const qreal right = playing ? 0.0 : qreal(kPlayheadGlowRadius);
    const qreal peak = playheadPeakAlpha(playing);
    if (left > 0.0) {
        QLinearGradient gradient(x - left, 0, x, 0);
        setQuadStops(gradient, color, peak);
        painter.fillRect(QRectF(x - left, top, left, height), gradient);
    }
    if (right > 0.0) {
        QLinearGradient gradient(x + right, 0, x, 0);
        setQuadStops(gradient, color, peak);
        painter.fillRect(QRectF(x, top, right, height), gradient);
    }

    QPen core(color, kPlayheadLineWidth, Qt::SolidLine, Qt::FlatCap);
    painter.setPen(core);
    painter.drawLine(QPointF(x, top), QPointF(x, top + height));
}

const QPainterPath kPlayheadTrianglePath = [] {
    QPainterPath path;
    path.moveTo(-kPlayheadTriangleHalfWidth, 0);
    path.lineTo(kPlayheadTriangleHalfWidth, 0);
    path.lineTo(0, kPlayheadTriangleHeight);
    path.closeSubpath();
    return path;
}();

} // namespace
bool platformPlayheadRendererEnabled()
{
#ifdef PORYDAW_USE_DIRECT_PLAYHEAD
    return !qEnvironmentVariableIsSet("PORYDAW_FORCE_WIDGET_PLAYHEAD");
#else
    return false;
#endif
}

PlayheadOverlay::PlayheadOverlay(QWidget *owner, TimelineSurfaces surfaces)
    : QWidget(owner)
    , m_surfaces(surfaces)
    , m_color(themes::color(themes::Role::song_view_playhead))
{
    Q_ASSERT(owner);

    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);

    observeSurfaceGeometry();

    synchronizeGeometry();
    show();
}

#ifndef PORYDAW_USE_DIRECT_PLAYHEAD
PlayheadOverlay::~PlayheadOverlay() = default;
#endif

void PlayheadOverlay::setPlayhead(qreal timelineX, bool visible, bool playing)
{
    if (m_timelineX == timelineX && m_visible == visible && m_playing == playing)
        return;

#ifdef PORYDAW_USE_DIRECT_PLAYHEAD
    const bool playingChanged = m_playing != playing;
#endif

    m_timelineX = timelineX;
    m_visible = visible;
    m_playing = playing;

#ifdef PORYDAW_USE_DIRECT_PLAYHEAD
    if (playingChanged && updateImages() && m_platform)
        setPlatformImages();
#endif
    updatePlayhead();
}

void PlayheadOverlay::observeSurfaceGeometry()
{
    QWidget *owner = parentWidget();
    Q_ASSERT(owner);

    const auto observe = [this, owner](QWidget *surface) {
        for (QWidget *widget = surface; widget; widget = widget->parentWidget()) {
            widget->installEventFilter(this);
            if (widget == owner) {
                break;
            }
        }
    };
    observe(&m_surfaces.ruler.widget);
    observe(&m_surfaces.roll.widget);
    observe(&m_surfaces.lanes.widget);
    observe(&m_surfaces.strip.widget);
}

bool PlayheadOverlay::eventFilter(QObject *, QEvent *event)
{
    switch (event->type()) {
    case QEvent::Show:
    case QEvent::WinIdChange:
        synchronizeGeometry();
        break;
    case QEvent::ParentChange:
        observeSurfaceGeometry();
        synchronizeGeometry();
        break;
    case QEvent::Hide:
    case QEvent::Move:
    case QEvent::Resize:
    case QEvent::LayoutRequest:
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    case QEvent::DevicePixelRatioChange:
#else
    case QEvent::ScreenChangeInternal:
#endif
        synchronizeGeometry();
        break;
    default:
        break;
    }
    return false;
}

void PlayheadOverlay::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    switch (event->type()) {
    case QEvent::ApplicationPaletteChange:
    case QEvent::PaletteChange:
    case QEvent::StyleChange: {
        const QColor newColor = themes::color(themes::Role::song_view_playhead);
        if (m_color != newColor) {
            m_color = newColor;
#ifdef PORYDAW_USE_DIRECT_PLAYHEAD
            if (updateImages() && m_platform)
                setPlatformImages();
#endif
            updatePlayhead();
        }
        break;
    }
    default:
        break;
    }
}

void PlayheadOverlay::paintEvent(QPaintEvent *event)
{
    (void)event;
    if (m_platformApplied || !m_visible || m_playheadGeometry.isEmpty()
        || (m_visibleSurfaceRegion.isEmpty() && m_triangleClip.isEmpty()))
        return;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const qreal x = finalX();
    const int top = m_playheadGeometry.top();
    const int height = m_playheadGeometry.height();

    if (!m_visibleSurfaceRegion.isEmpty()) {
        painter.setClipRegion(m_visibleSurfaceRegion);
        paintPlayheadBody(painter, x, top, height, m_playing, m_color);
    }

    if (!m_triangleClip.isEmpty()) {
        painter.setClipRect(m_triangleClip, Qt::ReplaceClip);
        painter.translate(
            x, top + (m_trianglePointsUp ? kPlayheadTriangleHeight : 0));
        if (m_trianglePointsUp)
            painter.scale(1.0, -1.0);
        painter.fillPath(kPlayheadTrianglePath, m_color);
    }
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

void PlayheadOverlay::synchronizeGeometry()
{
    QWidget *ownerWidget = parentWidget();
    Q_ASSERT(ownerWidget);
    QWidget &owner = *ownerWidget;

    const QRect rulerGeometry(m_surfaces.ruler.widget.mapTo(&owner, QPoint(0, 0)),
                              m_surfaces.ruler.widget.size());
    const int playheadTop = rulerGeometry.bottom() + 1;
    const QRect playheadGeometry(0, playheadTop, owner.width(),
                                 std::max(0, owner.height() - playheadTop));
    const QRect rulerVisible = visibleSurfaceRect(&m_surfaces.ruler.widget, &owner,
                                                  m_surfaces.ruler.timelineOrigin);
    const QRect triangleClip(rulerVisible.left(), playheadTop, rulerVisible.width(),
                             kPlayheadTriangleHeight + 1);

    const QRegion visibleSurfaces =
        QRegion(visibleSurfaceRect(&m_surfaces.roll.widget, &owner,
                                   m_surfaces.roll.timelineOrigin))
        + visibleSurfaceRect(&m_surfaces.lanes.widget, &owner,
                             m_surfaces.lanes.timelineOrigin)
        + visibleSurfaceRect(&m_surfaces.strip.widget, &owner,
                             m_surfaces.strip.timelineOrigin);

    const int timelineOrigin =
        m_surfaces.ruler.widget
            .mapTo(&owner, QPoint(m_surfaces.ruler.timelineOrigin, 0))
            .x();

    const bool overlayGeometryChanged = geometry() != owner.rect();
    if (overlayGeometryChanged)
        setGeometry(owner.rect());

    m_visibleSurfaceRegion = visibleSurfaces;
    m_playheadGeometry = playheadGeometry;
    m_triangleClip = triangleClip;
    m_timelineOrigin = timelineOrigin;
    m_trianglePointsUp = !m_surfaces.roll.widget.isVisible();
    m_devicePixelRatio = owner.devicePixelRatioF();
#ifdef PORYDAW_USE_DIRECT_PLAYHEAD
    bool platformCreated = false;
    if (!m_platform && !m_platformAttempted && owner.isVisible()) {
        m_platformAttempted = true;
        if (platformPlayheadRendererEnabled()) {
            initializePlatform(owner);
            platformCreated = true;
        }
    }
    const bool imagesChanged = updateImages();
    if (m_platform && (imagesChanged || platformCreated))
        setPlatformImages();
    if (m_platform)
        setPlatformLayout();
    updatePlayhead();
#else
    updatePlayhead();
#endif
    raise();
}

void PlayheadOverlay::updatePlayhead()
{
#ifdef PORYDAW_USE_DIRECT_PLAYHEAD
    m_platformApplied = m_platform && setPlatformPosition();
#else
    m_platformApplied = false;
#endif
    updatePaintRegion();
}

#ifdef PORYDAW_USE_DIRECT_PLAYHEAD
bool PlayheadOverlay::updateImages()
{
    const QColor currentThemeColor = m_color;
    const int currentHeight = m_playheadGeometry.height();
    const qreal currentDpr = m_devicePixelRatio > 0.0 ? m_devicePixelRatio : 1.0;
    const bool geometryValid = !m_playheadGeometry.isEmpty() && currentHeight > 0;
    bool imagesChanged = false;

    if (!geometryValid) {
        imagesChanged = !m_bodyImage.isNull() || !m_triangleImage.isNull();
        m_bodyImage = QImage();
        m_triangleImage = QImage();
        m_cachedBodyValid = false;
        m_cachedTriangleValid = false;
        return imagesChanged;
    }

    const bool bodyNeedsUpdate =
        !m_cachedBodyValid || m_cachedBodyHeight != currentHeight
        || m_cachedBodyPlaying != m_playing || m_cachedBodyDpr != currentDpr
        || m_cachedBodyThemeColor != currentThemeColor;

    if (bodyNeedsUpdate) {
        imagesChanged = true;
        m_cachedBodyHeight = currentHeight;
        m_cachedBodyPlaying = m_playing;
        m_cachedBodyDpr = currentDpr;
        m_cachedBodyThemeColor = currentThemeColor;
        m_cachedBodyValid = true;

        const qreal leftExtent = playheadGlowLeftExtent(m_playing);
        const qreal rightExtent = playheadGlowRightExtent(m_playing);
        m_bodyImageLeftExtent = leftExtent;

        const qreal bodyWidthLogical = leftExtent + rightExtent;
        const int bodyPixelWidth = qCeil(bodyWidthLogical * currentDpr);
        const int bodyPixelHeight = qCeil(currentHeight * currentDpr);

        if (bodyPixelWidth > 0 && bodyPixelHeight > 0) {
            QImage bodyImg(bodyPixelWidth, bodyPixelHeight,
                           QImage::Format_ARGB32_Premultiplied);
            bodyImg.setDevicePixelRatio(currentDpr);
            bodyImg.fill(Qt::transparent);

            QPainter painter(&bodyImg);
            painter.setRenderHint(QPainter::Antialiasing);
            // The bar sits leftExtent from the image's left edge; the
            // platform renderers position the image so it lands on finalX.
            paintPlayheadBody(painter, leftExtent, 0, currentHeight, m_playing,
                              currentThemeColor);
            painter.end();

            m_bodyImage = std::move(bodyImg);
        } else {
            m_bodyImage = QImage();
        }
    }

    const bool triangleNeedsUpdate =
        !m_cachedTriangleValid || m_cachedTrianglePointsUp != m_trianglePointsUp
        || m_cachedTriangleDpr != currentDpr
        || m_cachedTriangleThemeColor != currentThemeColor;

    if (triangleNeedsUpdate) {
        imagesChanged = true;
        m_cachedTrianglePointsUp = m_trianglePointsUp;
        m_cachedTriangleDpr = currentDpr;
        m_cachedTriangleThemeColor = currentThemeColor;
        m_cachedTriangleValid = true;

        const qreal triWidthLogical = 2.0 * kPlayheadTriangleHalfWidth;
        const qreal triHeightLogical = kPlayheadTriangleHeight;
        const int triPixelWidth = qCeil(triWidthLogical * currentDpr);
        const int triPixelHeight = qCeil(triHeightLogical * currentDpr);

        if (triPixelWidth > 0 && triPixelHeight > 0) {
            QImage triImg(triPixelWidth, triPixelHeight,
                          QImage::Format_ARGB32_Premultiplied);
            triImg.setDevicePixelRatio(currentDpr);
            triImg.fill(Qt::transparent);

            QPainter painter(&triImg);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.translate(kPlayheadTriangleHalfWidth,
                              m_trianglePointsUp ? triHeightLogical : 0);
            if (m_trianglePointsUp)
                painter.scale(1.0, -1.0);
            painter.fillPath(kPlayheadTrianglePath, currentThemeColor);
            painter.end();

            m_triangleImage = std::move(triImg);
        } else {
            m_triangleImage = QImage();
        }
    }
    return imagesChanged;
}
#endif // PORYDAW_USE_DIRECT_PLAYHEAD

QRegion PlayheadOverlay::playheadRegion() const
{
    // Maximum extents (the paused centered bloom) plus antialias padding:
    // one state-independent bound keeps play/pause transitions covered.
    constexpr qreal kAntialiasPadding = 1.0;
    const qreal x = finalX();
    const int top = m_playheadGeometry.top();

    const QRect bodyRect =
        QRectF(x - kPlayheadGlowRadius - kAntialiasPadding, top,
               2.0 * kPlayheadGlowRadius + kAntialiasPadding * 2.0,
               m_playheadGeometry.height())
            .toAlignedRect();
    QRegion region = QRegion(bodyRect).intersected(m_visibleSurfaceRegion);

    const QRect triangleRect =
        QRectF(x - kPlayheadTriangleHalfWidth - kAntialiasPadding, top,
               2.0 * kPlayheadTriangleHalfWidth + kAntialiasPadding * 2.0,
               kPlayheadTriangleHeight + 1)
            .toAlignedRect();
    region += QRegion(triangleRect).intersected(m_triangleClip);
    return region;
}

void PlayheadOverlay::updatePaintRegion()
{
    // Update regions become backing-store flush regions, which Qt scales to
    // device pixels with its own rounding — snap them to whole device pixels
    // so fractional scale factors cannot shave boundary pixels on screen
    // (same discipline as TimelineSurface::invalidateContent).
    const int grid = deviceAlignmentGrid(m_devicePixelRatio > 0.0
                                             ? m_devicePixelRatio
                                             : devicePixelRatioF());
    const auto snapped = [grid, this](const QRegion &region) {
        return grid == 0 ? QRegion(rect())
                         : expandRegionToDeviceGrid(region, grid);
    };

    if (m_platformApplied) {
        if (!m_lastPaintedRegion.isEmpty()) {
            update(snapped(m_lastPaintedRegion));
            m_lastPaintedRegion = QRegion();
        }
        return;
    }

    QRegion currentRegion;
    if (m_visible && !m_playheadGeometry.isEmpty()) {
        currentRegion = playheadRegion();
    }

    const QRegion dirty = m_lastPaintedRegion + currentRegion;
    m_lastPaintedRegion = currentRegion;
    if (!dirty.isEmpty()) {
        update(snapped(dirty));
    }
}

} // namespace songview
