#pragma once

#include <QBrush>
#include <QColor>
#include <QRegion>
#include <QWidget>

class QEvent;
class QPaintEvent;

namespace songview {

class PlayheadOverlay final : public QWidget
{
public:
    struct Surfaces
    {
        QWidget *ruler;
        int rulerOrigin;
        QWidget *roll;
        int rollOrigin;
        QWidget *lanes;
        int lanesOrigin;
        QWidget *strip;
        int stripOrigin;
    };

    PlayheadOverlay(QWidget *owner, const Surfaces &surfaces, const QColor &color);

    void setPlayhead(qreal timelineX, bool visible, bool playing);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    QRect surfaceGeometry(const QWidget *surface, QWidget *owner) const;
    QRect visibleSurfaceRegion(const QWidget *surface, int origin) const;
    QRegion playheadRegion(qreal x) const;
    void synchronizeGeometry();

    Surfaces m_surfaces;
    QColor m_color;
    QBrush m_playingGlow;
    QBrush m_stoppedGlow;
    QRegion m_visibleSurfaceRegion;
    QRect m_playheadGeometry;
    QRect m_triangleClip;
    qreal m_timelineX = 0.0;
    qreal m_playheadX = 0.0;
    int m_timelineOrigin = 0;
    bool m_visible = false;
    bool m_playing = false;
};

} // namespace songview
