#pragma once

#include <QColor>
#include <QRect>
#include <QRegion>
#include <QWidget>
#include <memory>

#ifdef PORYDAW_USE_DIRECT_PLAYHEAD
#include <QImage>
#endif

#include "timelinesurface.h"

class QEvent;
class QPaintEvent;

namespace songview {

// Shared playhead metrics: the widget painter, the platform renderers, and
// the check harness must all agree on these.
constexpr int kPlayheadGlowRadius = 10;
constexpr int kPlayheadTriangleHalfWidth = 4;
constexpr int kPlayheadTriangleHeight = 8;
constexpr qreal kPlayheadLineWidth = 1.0;
constexpr qreal kPlayheadPeakPlaying = 0.13;
constexpr qreal kPlayheadPeakPaused = 0.06;

/// Whether the platform-native playhead renderer is enabled for this process.
bool platformPlayheadRendererEnabled();

class PlayheadOverlay final : public QWidget
{
public:
    explicit PlayheadOverlay(QWidget *owner, TimelineSurfaces surfaces);
    void setPlayhead(qreal timelineX, bool visible, bool playing);
    ~PlayheadOverlay() override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    qreal finalX() const
    {
        return static_cast<qreal>(m_timelineOrigin) + m_timelineX;
    }

    QRect visibleSurfaceRect(const QWidget *surface, QWidget *owner,
                             int origin) const;
    void observeSurfaceGeometry();
    void synchronizeGeometry();
#ifdef PORYDAW_USE_DIRECT_PLAYHEAD
    class Platform;
    struct PlatformDeleter
    {
        void operator()(Platform *platform) const;
    };

    void initializePlatform(QWidget &owner);
    void setPlatformLayout();
    void setPlatformImages();
    bool setPlatformPosition();
    bool updateImages();
#endif
    void updatePlayhead();

    QRegion playheadRegion() const;
    void updatePaintRegion();

    TimelineSurfaces m_surfaces;
    QColor m_color;

#ifdef PORYDAW_USE_DIRECT_PLAYHEAD
    // Pre-rendered strips for the platform compositors. macOS uploads them
    // as CALayer contents; on Windows updateImages() only serves as the
    // change detector that triggers re-uploading the DComp surfaces. The
    // widget fallback below paints vectors and never reads these.
    QImage m_bodyImage;
    qreal m_bodyImageLeftExtent = 0.0;
    QImage m_triangleImage;

    int m_cachedBodyHeight = -1;
    bool m_cachedBodyPlaying = false;
    qreal m_cachedBodyDpr = 0.0;
    QColor m_cachedBodyThemeColor;
    bool m_cachedBodyValid = false;

    bool m_cachedTrianglePointsUp = false;
    qreal m_cachedTriangleDpr = 0.0;
    QColor m_cachedTriangleThemeColor;
    bool m_cachedTriangleValid = false;

    std::unique_ptr<Platform, PlatformDeleter> m_platform;
#endif
    QRegion m_lastPaintedRegion;

    QRegion m_visibleSurfaceRegion;
    QRect m_playheadGeometry;
    QRect m_triangleClip;
    qreal m_timelineX = 0.0;
    int m_timelineOrigin = 0;
    bool m_visible = false;
    bool m_playing = false;

    bool m_trianglePointsUp = false;
    qreal m_devicePixelRatio = 1.0;
    bool m_platformApplied = false;
#ifdef PORYDAW_USE_DIRECT_PLAYHEAD
    bool m_platformAttempted = false;
#endif
};

} // namespace songview
