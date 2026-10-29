#pragma once

#include <QPixmap>
#include <QRegion>
#include <QWidget>
#include <QtGlobal>

class QEvent;
class QPaintEvent;
class QResizeEvent;
class QPainter;

namespace songview {

// Smallest logical grid whose device size is integral at this device pixel
// ratio (1 at integer scales, 2 at 150%, 4 at any quarter scale); 0 when no
// grid up to 64 works. Partial updates and clips snapped to this grid keep
// every scaled edge on whole device pixels, so neither Qt's update-region
// rounding nor partial clip coverage can shave boundary pixels at
// fractional scale factors.
int deviceAlignmentGrid(qreal dpr) noexcept;
// Expands every rect of the region outward to the grid. grid <= 1 returns
// the region unchanged.
QRegion expandRegionToDeviceGrid(const QRegion &region, int grid);

// Paint counters for the rollcheck harness: how often and how many device
// pixels paintContent() actually rasterized, plus the cache's estimated
// footprint. Playhead sweeps must leave these untouched (pure cache blits).
struct TimelineSurfaceDiagnostics {
    quint64 contentPaintCount = 0;
    quint64 contentPaintPixelCount = 0;
    quint64 estimatedContentCacheBytes = 0;

    friend bool operator==(const TimelineSurfaceDiagnostics &lhs,
                           const TimelineSurfaceDiagnostics &rhs) noexcept
    {
        return lhs.contentPaintCount == rhs.contentPaintCount &&
               lhs.contentPaintPixelCount == rhs.contentPaintPixelCount &&
               lhs.estimatedContentCacheBytes == rhs.estimatedContentCacheBytes;
    }

    friend bool operator!=(const TimelineSurfaceDiagnostics &lhs,
                           const TimelineSurfaceDiagnostics &rhs) noexcept
    {
        return !(lhs == rhs);
    }
};

// Pixmap-backed paint cache for timeline-aligned widgets. Subclasses render
// through paintContent() into a cached pixmap and paint events blit it, so
// the playhead overlay sweeping slivers across a surface at 60 Hz costs
// blits instead of full note/lane rasters.
//
// CONTRACT: every content change must go through invalidateContent() — a
// plain update() schedules a repaint of the STALE cache and the change
// silently never appears. The cache self-invalidates on resize and on
// appearance changes (palette/font/style/theme). Re-rasterization is
// bounded to the regions paint events expose: off-viewport dirt (the
// automation lanes are a full-height scroll-area content widget) stays
// pending until scrolling exposes it, while QWidget::render/grab() covers
// the full rect. Surfaces whose pixel estimate exceeds a 256 MB budget, and
// pixmap-allocation failures, paint uncached.
class TimelineSurface : public QWidget
{
  public:
    explicit TimelineSurface(QWidget *parent = nullptr);

    void invalidateContent();
    void invalidateContent(const QRegion &region);
    TimelineSurfaceDiagnostics diagnostics() const noexcept;

  protected:
    virtual void paintContent(QPainter &painter) = 0;

  private:
    void paintEvent(QPaintEvent *event) final;
    void changeEvent(QEvent *event) final;
    void resizeEvent(QResizeEvent *event) final;
    void countContentPaint(quint64 pixelCount) noexcept;

    QPixmap m_contentCache;
    QRegion m_dirtyContentRegion;
    TimelineSurfaceDiagnostics m_diagnostics;
};

struct TimelineBand {
    QWidget &widget;
    int timelineOrigin;
};

struct CachedTimelineBand {
    TimelineSurface &widget;
    int timelineOrigin;
};

struct TimelineSurfaces {
    TimelineBand ruler;
    CachedTimelineBand roll;
    // Hidden until the View toggle opens it; a hidden band contributes no
    // visible rect, so the playhead simply skips it.
    CachedTimelineBand velocity;
    CachedTimelineBand lanes;
    CachedTimelineBand strip;
    // The CPU profile strip directly under the ruler: part of the header
    // while it shows, so the playhead (its triangle included) starts below
    // it, as below the ruler when it is hidden.
    CachedTimelineBand cpuStrip;
};

} // namespace songview
