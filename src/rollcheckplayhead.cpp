#include "rollcheckplayhead.h"

#include <QCoreApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QObject>
#include <QPaintEvent>
#include <QPainter>
#include <QPixmap>
#include <QRegion>
#include <QWidget>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#ifdef __APPLE__
#include <QGuiApplication>
#endif

#include "core/miditimeline.h"
#include "ui/eventlistview.h"
#include "ui/playheadoverlay.h"
#include "ui/songview.h"
#include "ui/timelinesurface.h"

namespace {
#ifdef __APPLE__
bool usesNativeMacPlayheadRenderer()
{
    return QGuiApplication::platformName() == QLatin1String("cocoa")
        && songview::platformPlayheadRendererEnabled();
}
#endif

QPixmap grabPlayheadOverlay(SongView &view, songview::PlayheadOverlay &marker,
                            QStringList &failures)
{
#ifdef __APPLE__
    if (usesNativeMacPlayheadRenderer())
        return renderMacPlayheadOverlay(view, failures);
#else
    (void)view;
    (void)failures;
#endif
    return marker.grab();
}

QPixmap grabSongViewWithPlayhead(SongView &view, songview::PlayheadOverlay &marker,
                                 QStringList &failures)
{
    QPixmap pixmap = view.grab();
#ifdef __APPLE__
    if (usesNativeMacPlayheadRenderer()) {
        const QPixmap overlay = renderMacPlayheadOverlay(view, failures);
        if (!overlay.isNull()) {
            QPainter painter(&pixmap);
            painter.drawPixmap(marker.mapTo(&view, QPoint()), overlay);
        }
    }
#else
    (void)marker;
    (void)failures;
#endif
    return pixmap;
}

class PaintRegionProbe : public QObject
{
public:
    void clear() { m_regions.clear(); }

    bool repainted(const QWidget *widget) const
    {
        return std::any_of(
            m_regions.cbegin(), m_regions.cend(),
            [=](const DirtyRegion &region) { return region.widget == widget; });
    }

    int maxPaintWidth(const QWidget *widget) const
    {
        int maxWidth = 0;
        for (const DirtyRegion &region : m_regions) {
            if (region.widget == widget)
                maxWidth = std::max(maxWidth, region.bounds.width());
        }
        return maxWidth;
    }

    bool repaintedAnyBroadly(const QWidget *allowed, int maxWidth) const
    {
        return std::any_of(
            m_regions.cbegin(), m_regions.cend(), [=](const DirtyRegion &region) {
                return region.widget != allowed && region.bounds.width() > maxWidth;
            });
    }

    bool repaintedBroadly(const QWidget *widget, int maxWidth) const
    {
        return std::any_of(
            m_regions.cbegin(), m_regions.cend(), [=](const DirtyRegion &region) {
                return region.widget == widget && region.bounds.width() > maxWidth;
            });
    }

private:
    struct DirtyRegion
    {
        QWidget *widget;
        QRect bounds;
    };

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Paint) {
            m_regions.push_back(
                {static_cast<QWidget *>(watched),
                 static_cast<QPaintEvent *>(event)->region().boundingRect()});
        }
        return QObject::eventFilter(watched, event);
    }

    std::vector<DirtyRegion> m_regions;
};

songview::PlayheadOverlay *findPlayheadOverlay(SongView &view)
{
    for (QWidget *widget : view.findChildren<QWidget *>()) {
        if (auto *overlay = dynamic_cast<songview::PlayheadOverlay *>(widget))
            return overlay;
    }
    return nullptr;
}

bool isPlayheadRed(const QColor &pixel, const QColor &playheadColor)
{
    const int colorDistance = std::abs(pixel.red() - playheadColor.red())
                              + std::abs(pixel.green() - playheadColor.green())
                              + std::abs(pixel.blue() - playheadColor.blue());
    return colorDistance <= 12 && pixel.alpha() > 0;
}

bool isCompositedPlayheadRed(const QColor &pixel, const QColor &playheadColor)
{
    return isPlayheadRed(pixel, playheadColor)
           || (pixel.red() - pixel.green() >= 24
               && pixel.red() - pixel.blue() >= 24);
}

qreal playheadCenter(const QPixmap &pixmap, const QColor &playheadColor)
{
    const QImage image = pixmap.toImage();
    const qreal devicePixelRatio = pixmap.devicePixelRatio();
    qreal weightedX = 0.0;
    qreal totalWeight = 0.0;
    for (int x = 0; x < image.width(); ++x) {
        for (int y = 0; y < image.height(); ++y) {
            const QColor pixel = image.pixelColor(x, y);
            if (isPlayheadRed(pixel, playheadColor) && pixel.alpha() > 80) {
                weightedX += qreal(x) * pixel.alpha();
                totalWeight += pixel.alpha();
            }
        }
    }
    return totalWeight > 0.0 ? weightedX / totalWeight / devicePixelRatio : -1.0;
}

bool hasPlayheadRedLine(const QImage &image, qreal devicePixelRatio, qreal logicalX,
                        const QRect &logicalArea, const QColor &playheadColor)
{
    if (logicalArea.isEmpty())
        return false;

    const int left = std::max(0, qFloor((logicalX - 1.0) * devicePixelRatio));
    const int right =
        std::min(image.width() - 1, qCeil((logicalX + 1.0) * devicePixelRatio));
    const int top = std::max(0, qFloor(logicalArea.top() * devicePixelRatio));
    const int bottom =
        std::min(image.height() - 1,
                 qCeil((logicalArea.bottom() + 1) * devicePixelRatio) - 1);
    for (int x = left; x <= right; ++x) {
        int consecutivePixels = 0;
        for (int y = top; y <= bottom; ++y) {
            if (isCompositedPlayheadRed(image.pixelColor(x, y), playheadColor)) {
                if (++consecutivePixels >= 3)
                    return true;
            } else {
                consecutivePixels = 0;
            }
        }
    }
    return false;
}

int playheadRedWidth(const QImage &image, qreal devicePixelRatio, qreal logicalX,
                     int logicalY, const QColor &playheadColor)
{
    const int left = std::max(0, qFloor((logicalX - 4.0) * devicePixelRatio));
    const int right =
        std::min(image.width() - 1, qCeil((logicalX + 4.0) * devicePixelRatio));
    const int y =
        std::clamp(qRound(logicalY * devicePixelRatio), 0, image.height() - 1);
    int width = 0;
    for (int x = left; x <= right; ++x) {
        if (isCompositedPlayheadRed(image.pixelColor(x, y), playheadColor))
            ++width;
    }
    return width;
}

void processPaints()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::UpdateRequest);
    QCoreApplication::processEvents();
}

void checkPianoRollKeyboardCacheUpdate(songview::TimelineSurface &pianoRoll,
                                       PaintRegionProbe &paintProbe,
                                       QStringList &failures)
{
    QEvent leaveEvent(QEvent::Leave);
    QCoreApplication::sendEvent(&pianoRoll, &leaveEvent);
    processPaints();
    const QImage beforeHover = pianoRoll.grab().toImage();
    processPaints();

    const qreal dpr = pianoRoll.devicePixelRatioF();
    const int cacheKeyboardPixelWidth =
        std::min(qCeil(pianoRoll.width() * dpr), qCeil(songview::kKeyboardW * dpr));
    const int maxReadoutPixelHeight =
        std::min(qCeil(pianoRoll.height() * dpr), qCeil(72 * dpr));
    const quint64 maxKeyboardReadoutPaintPixels =
        quint64(cacheKeyboardPixelWidth) * quint64(maxReadoutPixelHeight);
    const auto checkPaintScope =
        [&](const songview::TimelineSurfaceDiagnostics &before,
            const songview::TimelineSurfaceDiagnostics &after,
            const QString &action) {
            if (after.contentPaintCount <= before.contentPaintCount
                || after.contentPaintPixelCount <= before.contentPaintPixelCount) {
                failures.append(
                    QStringLiteral("piano-roll hover %1 painted no content")
                        .arg(action));
                return;
            }
            const quint64 painted =
                after.contentPaintPixelCount - before.contentPaintPixelCount;
            if (painted > maxKeyboardReadoutPaintPixels) {
                failures.append(
                    QStringLiteral("piano-roll hover %1 painted %2 device pixels "
                                   "(readout budget %3)")
                        .arg(action)
                        .arg(painted)
                        .arg(maxKeyboardReadoutPaintPixels));
            }
        };

    const QPoint firstPosition(1, pianoRoll.height() / 2);
    const songview::TimelineSurfaceDiagnostics beforeFirst = pianoRoll.diagnostics();
    paintProbe.clear();
    QMouseEvent firstEvent(QEvent::MouseMove, QPointF(firstPosition),
                           QPointF(pianoRoll.mapToGlobal(firstPosition)),
                           Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&pianoRoll, &firstEvent);
    processPaints();
    const bool firstRepainted = paintProbe.repainted(&pianoRoll);
    const int firstRepaintWidth = paintProbe.maxPaintWidth(&pianoRoll);
    const songview::TimelineSurfaceDiagnostics afterFirst = pianoRoll.diagnostics();
    const QImage afterFirstHover = pianoRoll.grab().toImage();
    if (!firstRepainted || firstRepaintWidth > songview::kKeyboardW) {
        failures.append(
            QStringLiteral("piano-roll hover repainted %1 px (budget %2)")
                .arg(firstRepaintWidth)
                .arg(songview::kKeyboardW));
    }
    checkPaintScope(beforeFirst, afterFirst, QStringLiteral("entry"));

    if (beforeHover.size() != afterFirstHover.size()
        || beforeHover.devicePixelRatio() != afterFirstHover.devicePixelRatio()) {
        failures.append("piano-roll hover changed image geometry");
        return;
    }
    const int keyboardPixelWidth =
        std::min(afterFirstHover.width(),
                 qCeil(songview::kKeyboardW * afterFirstHover.devicePixelRatio()));
    bool keyboardChanged = false;
    bool timelineChanged = false;
    for (int y = 0; y < afterFirstHover.height(); ++y) {
        for (int x = 0; x < afterFirstHover.width(); ++x) {
            if (beforeHover.pixel(x, y) == afterFirstHover.pixel(x, y))
                continue;
            if (x < keyboardPixelWidth)
                keyboardChanged = true;
            else
                timelineChanged = true;
        }
    }
    if (!keyboardChanged)
        failures.append("piano-roll hover did not change the keyboard");
    if (timelineChanged)
        failures.append("piano-roll hover changed pixels outside the keyboard");

    const int moveDistance = std::max(12, pianoRoll.height() / 6);
    const QPoint secondPosition(
        1, std::clamp(firstPosition.y() + moveDistance, 0, pianoRoll.height() - 1));
    const songview::TimelineSurfaceDiagnostics beforeMove = pianoRoll.diagnostics();
    paintProbe.clear();
    QMouseEvent secondEvent(QEvent::MouseMove, QPointF(secondPosition),
                            QPointF(pianoRoll.mapToGlobal(secondPosition)),
                            Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&pianoRoll, &secondEvent);
    processPaints();
    const bool moveRepainted = paintProbe.repainted(&pianoRoll);
    const int moveRepaintWidth = paintProbe.maxPaintWidth(&pianoRoll);
    const songview::TimelineSurfaceDiagnostics afterMove = pianoRoll.diagnostics();
    const QImage afterMoveHover = pianoRoll.grab().toImage();
    if (afterMoveHover == afterFirstHover)
        failures.append("piano-roll hover move did not move its key readout");
    if (!moveRepainted || moveRepaintWidth > songview::kKeyboardW) {
        failures.append(
            QStringLiteral("piano-roll hover move repainted %1 px (budget %2)")
                .arg(moveRepaintWidth)
                .arg(songview::kKeyboardW));
    }
    checkPaintScope(beforeMove, afterMove, QStringLiteral("move"));

    const songview::TimelineSurfaceDiagnostics beforeClear = pianoRoll.diagnostics();
    paintProbe.clear();
    QCoreApplication::sendEvent(&pianoRoll, &leaveEvent);
    processPaints();
    const bool clearRepainted = paintProbe.repainted(&pianoRoll);
    const int clearRepaintWidth = paintProbe.maxPaintWidth(&pianoRoll);
    const songview::TimelineSurfaceDiagnostics afterClear = pianoRoll.diagnostics();
    const QImage afterClearImage = pianoRoll.grab().toImage();
    if (afterClearImage != beforeHover)
        failures.append("piano-roll hover pixels did not restore after leave");
    if (!clearRepainted || clearRepaintWidth > songview::kKeyboardW) {
        failures.append(
            QStringLiteral("piano-roll hover clear repainted %1 px (budget %2)")
                .arg(clearRepaintWidth)
                .arg(songview::kKeyboardW));
    }
    checkPaintScope(beforeClear, afterClear, QStringLiteral("clear"));
}

// A dense serpentine cursor sweep across the whole surface, then leave: the
// hover ink must restore every pixel it touched. Guards the partial-repaint
// path of the content cache — at fractional device pixel ratios (125%/150%),
// misaligned patch transforms or clip regions leave one-device-pixel "cursor
// trails" that this catches (originally reproduced with 83 ghost pixels at
// 1.25x; run rollcheck under QT_SCALE_FACTOR=1.25 to exercise that case).
void checkHoverSweepRestores(songview::TimelineSurface &surface,
                             const QString &name, QStringList &failures)
{
    QEvent leaveEvent(QEvent::Leave);
    QCoreApplication::sendEvent(&surface, &leaveEvent);
    processPaints();
    const QImage baseline = surface.grab().toImage();
    processPaints();

    bool leftToRight = true;
    for (int y = 4; y < surface.height(); y += 8) {
        const int x0 = 2;
        const int x1 = surface.width() - 2;
        for (int i = 0; i <= 20; ++i) {
            const int x = leftToRight ? x0 + (x1 - x0) * i / 20
                                      : x1 - (x1 - x0) * i / 20;
            const QPoint pos(x, y);
            QMouseEvent move(QEvent::MouseMove, QPointF(pos),
                             QPointF(surface.mapToGlobal(pos)), Qt::NoButton,
                             Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(&surface, &move);
            processPaints();
        }
        leftToRight = !leftToRight;
    }
    QCoreApplication::sendEvent(&surface, &leaveEvent);
    processPaints();

    const QImage after = surface.grab().toImage();
    if (after == baseline)
        return;
    int ghostPixels = 0;
    for (int y = 0; y < baseline.height(); ++y) {
        for (int x = 0; x < baseline.width(); ++x) {
            if (after.pixel(x, y) != baseline.pixel(x, y))
                ++ghostPixels;
        }
    }
    failures.append(
        QStringLiteral("%1 cursor sweep left %2 ghost pixels behind")
            .arg(name)
            .arg(ghostPixels));
}

void checkAutomationHoverCacheUpdate(songview::TimelineSurface &lanes,
                                     PaintRegionProbe &paintProbe,
                                     QStringList &failures)
{
    QEvent leaveEvent(QEvent::Leave);
    QCoreApplication::sendEvent(&lanes, &leaveEvent);
    processPaints();
    const QImage baseline = lanes.grab().toImage();
    processPaints();

    const int plotLeft = songview::kGutterW;
    const int plotWidth = lanes.width() - plotLeft;
    if (plotWidth <= 32 || lanes.height() <= 0) {
        failures.append("automation hover check has no visible curve area");
        return;
    }

    struct HoverResult
    {
        QPoint position;
        QImage image;
        songview::TimelineSurfaceDiagnostics before;
        songview::TimelineSurfaceDiagnostics after;
        int repaintWidth = 0;
        bool repainted = false;
    };
    HoverResult hover;
    bool foundReadout = false;
    const std::array<int, 3> candidateXs{{
        plotLeft + (plotWidth * 3) / 4,
        plotLeft + plotWidth / 2,
        plotLeft + plotWidth - std::min(24, plotWidth / 8),
    }};
    constexpr int candidateRowStep = 12;
    for (int y = candidateRowStep / 2; y < lanes.height() && !foundReadout;
         y += candidateRowStep) {
        for (const int candidateX : candidateXs) {
            const QPoint position(
                std::clamp(candidateX, plotLeft, lanes.width() - 1), y);
            hover.position = position;
            hover.before = lanes.diagnostics();
            paintProbe.clear();
            QMouseEvent hoverEvent(QEvent::MouseMove, QPointF(position),
                                   QPointF(lanes.mapToGlobal(position)),
                                   Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(&lanes, &hoverEvent);
            processPaints();
            hover.repainted = paintProbe.repainted(&lanes);
            hover.repaintWidth = paintProbe.maxPaintWidth(&lanes);
            hover.after = lanes.diagnostics();
            hover.image = lanes.grab().toImage();
            foundReadout =
                hover.image.size() == baseline.size()
                && hover.image.devicePixelRatio() == baseline.devicePixelRatio()
                && hover.image != baseline;
            if (foundReadout)
                break;
            QCoreApplication::sendEvent(&lanes, &leaveEvent);
            processPaints();
        }
    }
    if (!foundReadout) {
        failures.append("automation idle hover did not render a visible readout");
        paintProbe.clear();
        return;
    }

    const qreal dpr = lanes.devicePixelRatioF();
    const int maxReadoutWidth = std::min(192, std::max(64, lanes.width() / 3));
    const quint64 maxReadoutPaintPixels =
        quint64(qCeil(maxReadoutWidth * dpr))
        * quint64(qCeil(std::min(64, lanes.height()) * dpr));
    const quint64 wholeSurfacePixels =
        quint64(qCeil(lanes.width() * dpr)) * quint64(qCeil(lanes.height() * dpr));
    if (!hover.repainted || hover.repaintWidth > maxReadoutWidth) {
        failures.append(
            QStringLiteral("automation hover repainted %1 px (budget %2)")
                .arg(hover.repaintWidth)
                .arg(maxReadoutWidth));
    }
    if (hover.after.contentPaintCount <= hover.before.contentPaintCount
        || hover.after.contentPaintPixelCount
               <= hover.before.contentPaintPixelCount) {
        failures.append("automation hover did not paint lane content");
    } else {
        const quint64 hoverPaintPixels =
            hover.after.contentPaintPixelCount - hover.before.contentPaintPixelCount;
        if (hoverPaintPixels > maxReadoutPaintPixels
            || hoverPaintPixels * 2 >= wholeSurfacePixels) {
            failures.append(
                QStringLiteral("automation hover painted %1 device pixels "
                               "(readout budget %2)")
                    .arg(hoverPaintPixels)
                    .arg(maxReadoutPaintPixels));
        }
    }

    const int moveDistance = std::min(96, std::max(48, plotWidth / 6));
    int secondX = std::min(lanes.width() - 1, hover.position.x() + moveDistance);
    if (secondX - hover.position.x() < std::min(24, plotWidth / 4))
        secondX = std::max(plotLeft, hover.position.x() - moveDistance);
    const QPoint secondPosition(secondX, hover.position.y());
    const songview::TimelineSurfaceDiagnostics beforeMove = lanes.diagnostics();
    paintProbe.clear();
    QMouseEvent moveEvent(QEvent::MouseMove, QPointF(secondPosition),
                          QPointF(lanes.mapToGlobal(secondPosition)), Qt::NoButton,
                          Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&lanes, &moveEvent);
    processPaints();
    const bool moveRepainted = paintProbe.repainted(&lanes);
    const int moveRepaintWidth = paintProbe.maxPaintWidth(&lanes);
    const songview::TimelineSurfaceDiagnostics afterMove = lanes.diagnostics();
    const QImage moved = lanes.grab().toImage();
    bool firstReadoutPixelsRestored = false;
    if (moved.size() == baseline.size()
        && moved.devicePixelRatio() == baseline.devicePixelRatio()) {
        for (int y = 0; y < baseline.height() && !firstReadoutPixelsRestored; ++y) {
            for (int x = 0; x < baseline.width(); ++x) {
                if (hover.image.pixel(x, y) != baseline.pixel(x, y)
                    && moved.pixel(x, y) == baseline.pixel(x, y)) {
                    firstReadoutPixelsRestored = true;
                    break;
                }
            }
        }
    }
    if (moved == hover.image || moved == baseline)
        failures.append("automation hover move did not move its visible readout");
    if (!firstReadoutPixelsRestored) {
        failures.append(
            "automation hover move did not restore the old readout pixels");
    }
    const int maxMoveRepaintWidth = std::min(
        lanes.width(), maxReadoutWidth + std::abs(secondX - hover.position.x()));
    if (!moveRepainted || moveRepaintWidth > maxMoveRepaintWidth) {
        failures.append(
            QStringLiteral("automation hover move repainted %1 px (budget %2)")
                .arg(moveRepaintWidth)
                .arg(maxMoveRepaintWidth));
    }
    if (afterMove.contentPaintCount <= beforeMove.contentPaintCount
        || afterMove.contentPaintPixelCount <= beforeMove.contentPaintPixelCount) {
        failures.append("automation hover move did not paint lane content");
    } else {
        const quint64 movePaintPixels =
            afterMove.contentPaintPixelCount - beforeMove.contentPaintPixelCount;
        const quint64 maxMovePaintPixels = maxReadoutPaintPixels * 2;
        if (movePaintPixels > maxMovePaintPixels
            || movePaintPixels * 2 >= wholeSurfacePixels) {
            failures.append(
                QStringLiteral("automation hover move painted %1 device pixels "
                               "(old/new readout budget %2)")
                    .arg(movePaintPixels)
                    .arg(maxMovePaintPixels));
        }
    }

    const songview::TimelineSurfaceDiagnostics beforeClear = lanes.diagnostics();
    paintProbe.clear();
    QCoreApplication::sendEvent(&lanes, &leaveEvent);
    processPaints();
    const bool clearRepainted = paintProbe.repainted(&lanes);
    const int clearRepaintWidth = paintProbe.maxPaintWidth(&lanes);
    const songview::TimelineSurfaceDiagnostics afterClear = lanes.diagnostics();
    const QImage cleared = lanes.grab().toImage();
    if (cleared != baseline)
        failures.append("automation hover pixels did not restore after leave");
    if (!clearRepainted || clearRepaintWidth > maxReadoutWidth) {
        failures.append(
            QStringLiteral("automation hover clear repainted %1 px (budget %2)")
                .arg(clearRepaintWidth)
                .arg(maxReadoutWidth));
    }
    if (afterClear.contentPaintCount <= beforeClear.contentPaintCount
        || afterClear.contentPaintPixelCount <= beforeClear.contentPaintPixelCount) {
        failures.append("automation hover clear did not paint lane content");
    } else {
        const quint64 clearPaintPixels =
            afterClear.contentPaintPixelCount - beforeClear.contentPaintPixelCount;
        if (clearPaintPixels > maxReadoutPaintPixels
            || clearPaintPixels * 2 >= wholeSurfacePixels) {
            failures.append(
                QStringLiteral("automation hover clear painted %1 device pixels "
                               "(readout budget %2)")
                    .arg(clearPaintPixels)
                    .arg(maxReadoutPaintPixels));
        }
    }
}

void checkEventListRendering(SongView &view, songview::PlayheadOverlay &marker,
                             qreal stoppedMarkerCenter, const QRect &rulerArea,
                             const QColor &playheadColor, QStringList &failures)
{
    auto *events = view.findChild<EventListView *>();
    if (!events) {
        failures.append("EventListView child not found");
        return;
    }
    const QPoint markerOffset = marker.mapTo(&view, QPoint());
    const qreal playheadXInView = markerOffset.x() + stoppedMarkerCenter;
    view.setEventListVisible(true);
    processPaints();
    const QRect eventListArea = QRect(events->mapTo(&view, QPoint()), events->size())
                                    .intersected(view.rect());
    const QPixmap overlayPixmap = grabPlayheadOverlay(view, marker, failures);
    if (overlayPixmap.isNull())
        return;
    const QImage overlayImage = overlayPixmap.toImage();
    const qreal overlayDpr = overlayPixmap.devicePixelRatio();
    if (!events->isVisible() || eventListArea.isEmpty()) {
        failures.append("event list is not visible for the playhead check");
        return;
    }
    if (playheadXInView < eventListArea.left()
        || playheadXInView > eventListArea.right()) {
        failures.append("could not map playhead into the event list");
        return;
    }
    const QRect eventListOverlayArea = eventListArea.translated(-markerOffset);
    const int triangleHeight = std::min(songview::kPlayheadTriangleHeight + 1,
                                        eventListOverlayArea.height());
    const QRect triangleArea(eventListOverlayArea.left(), eventListOverlayArea.top(),
                             eventListOverlayArea.width(), triangleHeight);
    const auto hasLine = [&](const QRect &area) {
        return hasPlayheadRedLine(overlayImage, overlayDpr, stoppedMarkerCenter,
                                  area, playheadColor);
    };
    const auto redWidth = [&](int y) {
        return playheadRedWidth(overlayImage, overlayDpr, stoppedMarkerCenter, y,
                                playheadColor);
    };
    if (!hasLine(triangleArea))
        failures.append("playhead triangle did not render below the time ruler");
    if (redWidth(triangleArea.bottom() - 1) <= redWidth(triangleArea.top()))
        failures.append("playhead triangle did not point up in the event list");
    if (hasLine(QRect(eventListOverlayArea.left(),
                      eventListOverlayArea.top() + triangleHeight,
                      eventListOverlayArea.width(),
                      eventListOverlayArea.height() - triangleHeight))) {
        failures.append("playhead line overpainted the event list");
    }
    if (hasLine(rulerArea.translated(-markerOffset)))
        failures.append("playhead rendered in the event-list time ruler");
    const QRect upperTimelineArea =
        QRect(0, 0, view.width(), eventListArea.top()).translated(-markerOffset);
    const QRect lowerTimelineArea =
        QRect(0, eventListArea.bottom() + 1, view.width(),
              view.height() - eventListArea.bottom() - 1)
            .translated(-markerOffset);
    if (!hasLine(upperTimelineArea) && !hasLine(lowerTimelineArea)) {
        failures.append("playhead overlay did not render on visible timeline "
                        "surfaces");
    }
}

// A full invalidation of the automation lanes — every drag move does one —
// must rasterize only the rows the scroll viewport exposes; off-viewport
// dirt stays pending until scrolled in. Shrinks the view so the lanes are
// guaranteed taller than their viewport, then bounds the painted pixels.
void checkLanesViewportBoundedRepaint(SongView &view,
                                      songview::TimelineSurface &lanes,
                                      QStringList &failures)
{
    const QSize savedSize = view.size();
    view.resize(savedSize.width(), 360);
    processPaints();
    const QWidget *viewport = lanes.parentWidget();
    if (!viewport || lanes.height() <= viewport->height()) {
        failures.append("could not make the lanes taller than their viewport "
                        "for the bounded-repaint check");
        view.resize(savedSize);
        processPaints();
        return;
    }

    // Settle the resize-induced full repaint before measuring.
    (void)lanes.grab();
    processPaints();

    const songview::TimelineSurfaceDiagnostics before = lanes.diagnostics();
    lanes.invalidateContent();
    processPaints();
    const songview::TimelineSurfaceDiagnostics after = lanes.diagnostics();
    const qreal dpr = lanes.devicePixelRatioF();
    const quint64 painted =
        after.contentPaintPixelCount - before.contentPaintPixelCount;
    // + 9: room for the cache's device-alignment expansion (up to 4 logical
    // px per edge at quarter scale factors) plus edge rounding.
    const quint64 viewportBudget = quint64(qCeil(lanes.width() * dpr))
                                   * quint64(qCeil((viewport->height() + 9) * dpr));
    if (after.contentPaintCount <= before.contentPaintCount || painted == 0) {
        failures.append("lanes full invalidation painted no content");
    } else if (painted > viewportBudget) {
        failures.append(
            QStringLiteral("lanes full invalidation painted %1 device pixels "
                           "(viewport budget %2): off-viewport rows were "
                           "rasterized")
                .arg(painted)
                .arg(viewportBudget));
    }
    view.resize(savedSize);
    processPaints();
}

void checkFractionalMovement(SongView &view, const MidiTimeline &timeline,
                             songview::PlayheadOverlay &marker,
                             const QColor &playheadColor, uint64_t firstTick,
                             QStringList &failures)
{
    uint64_t fractionalStartSample = timeline.sampleForTick(firstTick);
    uint64_t fractionalEndSample = fractionalStartSample;
    double playheadTick = timeline.tickForSample(fractionalStartSample);
    int fractionalBucketX = view.contentX(playheadTick);
    double fractionalStartX = playheadTick * view.pxPerTick();
    const uint64_t fractionalSearchEnd = timeline.sampleForTick(firstTick + 2);
    for (uint64_t sample = fractionalStartSample + 1; sample <= fractionalSearchEnd;
         ++sample) {
        playheadTick = timeline.tickForSample(sample);
        const int x = view.contentX(playheadTick);
        const double exactX = playheadTick * view.pxPerTick();
        if (x != fractionalBucketX) {
            fractionalStartSample = sample;
            fractionalBucketX = x;
            fractionalStartX = exactX;
        } else if (exactX - fractionalStartX >= 0.4) {
            fractionalEndSample = sample;
            break;
        }
    }
    if (fractionalEndSample == fractionalStartSample) {
        failures.append("could not choose fractional playhead positions");
        return;
    }
    view.setPlayheadSample(fractionalStartSample, true);
    processPaints();
    const QPixmap fractionalStartPixmap =
        grabPlayheadOverlay(view, marker, failures);
    const qreal fractionalStart =
        playheadCenter(fractionalStartPixmap, playheadColor);
    view.setPlayheadSample(fractionalEndSample, true);
    processPaints();
    const QPixmap fractionalEndPixmap = grabPlayheadOverlay(view, marker, failures);
    const qreal fractionalEnd = playheadCenter(fractionalEndPixmap, playheadColor);
    const qreal expectedDelta = (timeline.tickForSample(fractionalEndSample)
                                 - timeline.tickForSample(fractionalStartSample))
                                * view.pxPerTick();
    // Unconditional, including dpr 1 (main asserted 0.2px there): the widget
    // path must place the strips subpixel-accurately, not snap to pixels.
    if (std::abs((fractionalEnd - fractionalStart) - expectedDelta) > 0.2) {
        failures.append("fractional playhead movement did not match its timeline "
                        "position");
    }
}

void checkPlayheadRendering(SongView &view, const MidiTimeline &timeline,
                            songview::PlayheadOverlay &marker, QStringList &failures)
{
    const int plotWidth = view.width() - songview::kGutterW;
    if (plotWidth <= 64) {
        failures.append("timeline plot is too narrow for the playhead check");
        return;
    }
    const auto tickAtContentX = [&view](int x) {
        return uint64_t(std::ceil(std::max(0.0, view.tickAtContentX(x))));
    };
    uint64_t firstTick = 0;
    uint64_t firstSample = 0, secondSample = 0;
    int firstX = 0, secondX = 0;
    bool foundInterval = false;
    for (int x = plotWidth / 3; x + 12 < plotWidth; ++x) {
        const uint64_t candidateFirstTick = tickAtContentX(x);
        const uint64_t candidateSecondTick = tickAtContentX(x + 12);
        const int candidateFirstX = view.contentX(double(candidateFirstTick));
        const int candidateSecondX = view.contentX(double(candidateSecondTick));
        if (candidateFirstX < 0 || candidateSecondX >= plotWidth
            || candidateSecondX <= candidateFirstX
            || candidateSecondX - candidateFirstX > 32)
            continue;
        // Skip intervals straddling a program change: the frames at the two
        // probe positions would then differ beyond the playhead itself.
        const uint64_t candidateFirstSample =
            timeline.sampleForTick(candidateFirstTick);
        const uint64_t candidateSecondSample =
            timeline.sampleForTick(candidateSecondTick);
        const uint64_t firstDisplayTick =
            uint64_t(timeline.tickForSample(candidateFirstSample));
        const uint64_t secondDisplayTick =
            uint64_t(timeline.tickForSample(candidateSecondSample));
        const bool crossesProgramChange =
            std::any_of(timeline.events.cbegin(), timeline.events.cend(),
                        [=](const TimelineEvent &event) {
                            return event.type == 0xC && event.tick > firstDisplayTick
                                   && event.tick <= secondDisplayTick;
                        });
        if (crossesProgramChange)
            continue;
        firstTick = candidateFirstTick;
        firstSample = candidateFirstSample;
        secondSample = candidateSecondSample;
        firstX = candidateFirstX;
        secondX = candidateSecondX;
        foundInterval = true;
        break;
    }
    if (!foundInterval) {
        failures.append("could not choose nearby visible playhead ticks");
        return;
    }

    const songview::TimelineSurfaces surfaces = view.timelineSurfaces();
    struct CachedSurfaceCheck
    {
        const char *name;
        songview::CachedTimelineBand band;
    };
    const std::array<CachedSurfaceCheck, 3> cachedSurfaces{{
        {"piano roll", surfaces.roll},
        {"automation lanes", surfaces.lanes},
        {"event strip", surfaces.strip},
    }};

    PaintRegionProbe probe;
    view.installEventFilter(&probe);
    for (QWidget *child : view.findChildren<QWidget *>())
        child->installEventFilter(&probe);
    const QColor playheadColor(226, 66, 66);
    const auto expectedCenter = [&](uint64_t sample) {
        const QPoint timelineOrigin = surfaces.ruler.widget.mapTo(
            &view, QPoint(surfaces.ruler.timelineOrigin, 0));
        return qreal(marker.mapFrom(&view, timelineOrigin).x())
               + view.contentX(timeline.tickForSample(sample));
    };
    const auto checkCenter = [&](qreal center, uint64_t sample,
                                 const QString &state) {
        const qreal expected = expectedCenter(sample);
        if (!marker.isVisible() || center < 0.0
            || std::abs(center - expected) > 1.0) {
            failures.append(
                QStringLiteral("%1 playhead did not render at its expected position")
                    .arg(state));
        }
    };

    view.setPlayheadSample(0, false);
    for (const CachedSurfaceCheck &surface : cachedSurfaces)
        surface.band.widget.update();
    processPaints();
    for (const CachedSurfaceCheck &surface : cachedSurfaces) {
        if (surface.band.widget.diagnostics().estimatedContentCacheBytes == 0)
            (void)surface.band.widget.grab();
    }
    processPaints();
    for (const CachedSurfaceCheck &surface : cachedSurfaces) {
        const songview::TimelineSurfaceDiagnostics diagnostics =
            surface.band.widget.diagnostics();
        const QString surfaceName = QString::fromLatin1(surface.name);
        if (diagnostics.contentPaintCount == 0
            || diagnostics.contentPaintPixelCount == 0) {
            failures.append(
                QStringLiteral("%1 did not warm its timeline content cache")
                    .arg(surfaceName));
        }

        const qreal dpr = surface.band.widget.devicePixelRatioF();
        const quint64 expectedCacheBytes =
            quint64(qCeil(surface.band.widget.width() * dpr))
            * quint64(qCeil(surface.band.widget.height() * dpr)) * quint64(4);
        constexpr quint64 maxEstimatedCacheBytes = 256ULL * 1024ULL * 1024ULL;
        if (expectedCacheBytes > 0 && expectedCacheBytes <= maxEstimatedCacheBytes
            && diagnostics.estimatedContentCacheBytes != expectedCacheBytes) {
            failures.append(
                QStringLiteral("%1 reported %2 estimated cache bytes (expected %3)")
                    .arg(surfaceName)
                    .arg(diagnostics.estimatedContentCacheBytes)
                    .arg(expectedCacheBytes));
        }
    }

    checkPianoRollKeyboardCacheUpdate(surfaces.roll.widget, probe, failures);
    checkAutomationHoverCacheUpdate(surfaces.lanes.widget, probe, failures);
    checkHoverSweepRestores(surfaces.lanes.widget,
                            QStringLiteral("automation lanes"), failures);
    checkHoverSweepRestores(surfaces.roll.widget, QStringLiteral("piano roll"),
                            failures);
    probe.clear();
    const auto diagnosticsBefore = [&cachedSurfaces] {
        std::array<songview::TimelineSurfaceDiagnostics, 3> diagnostics;
        for (std::size_t i = 0; i < diagnostics.size(); ++i)
            diagnostics[i] = cachedSurfaces[i].band.widget.diagnostics();
        return diagnostics;
    }();
    view.setPlayheadSample(firstSample, false);
    processPaints();
    const qreal firstMarkerCenter =
        playheadCenter(grabPlayheadOverlay(view, marker, failures), playheadColor);
    checkCenter(firstMarkerCenter, firstSample, QStringLiteral("stopped"));
    const QRect rulerArea(surfaces.ruler.widget.mapTo(&view, QPoint()),
                          surfaces.ruler.widget.size());
    if (firstMarkerCenter >= 0.0) {
        const QPixmap composedPixmap =
            grabSongViewWithPlayhead(view, marker, failures);
        const qreal playheadX =
            marker.mapTo(&view, QPoint()).x() + firstMarkerCenter;
        if (hasPlayheadRedLine(composedPixmap.toImage(),
                               composedPixmap.devicePixelRatio(), playheadX,
                               rulerArea, playheadColor)) {
            failures.append("playhead rendered in the time ruler");
        }
    }
    view.setPlayheadSample(firstSample, true);
    processPaints();
    probe.clear();
#ifdef __APPLE__
    if (usesNativeMacPlayheadRenderer()
        && renderMacPlayheadOverlay(view, failures).isNull())
        return;
#endif
    view.setPlayheadSample(secondSample, true);
    processPaints();
    const auto diagnosticsAfter = [&cachedSurfaces] {
        std::array<songview::TimelineSurfaceDiagnostics, 3> diagnostics;
        for (std::size_t i = 0; i < diagnostics.size(); ++i)
            diagnostics[i] = cachedSurfaces[i].band.widget.diagnostics();
        return diagnostics;
    }();
    bool cacheRegenerated = false;
    for (std::size_t i = 0; i < diagnosticsBefore.size(); ++i) {
        if (diagnosticsAfter[i] != diagnosticsBefore[i]) {
            cacheRegenerated = true;
            break;
        }
    }
    if (cacheRegenerated)
        failures.append("playhead move regenerated cached timeline content");
    // Dirty strip: move delta + full glow diameter + full triangle width.
    const int maxPlayheadExposureWidth = secondX - firstX
                                         + 2 * songview::kPlayheadGlowRadius
                                         + 2 * songview::kPlayheadTriangleHalfWidth;
    const bool overlayPaintedBroadly =
        probe.repaintedBroadly(&marker, maxPlayheadExposureWidth);
    const bool anotherWidgetPaintedBroadly =
        probe.repaintedAnyBroadly(&marker, maxPlayheadExposureWidth);
    const QPixmap playingPixmap = grabPlayheadOverlay(view, marker, failures);
    const qreal playingMarkerCenter = playheadCenter(playingPixmap, playheadColor);
    checkCenter(playingMarkerCenter, secondSample, QStringLiteral("playing"));
    if (overlayPaintedBroadly)
        failures.append("playhead move repainted the overlay broadly");
    if (anotherWidgetPaintedBroadly)
        failures.append("playhead move repainted another timeline widget broadly");
    view.setPlayheadSample(secondSample, false);
    processPaints();
    const QPixmap stoppedPixmap = grabPlayheadOverlay(view, marker, failures);
    const qreal stoppedMarkerCenter = playheadCenter(stoppedPixmap, playheadColor);
    checkCenter(stoppedMarkerCenter, secondSample, QStringLiteral("stopped"));
    if (playingPixmap.toImage() == stoppedPixmap.toImage())
        failures.append("playing and stopped playheads rendered identically");
    checkEventListRendering(view, marker, stoppedMarkerCenter, rulerArea,
                            playheadColor, failures);
    view.setEventListVisible(false);
    processPaints();
    checkFractionalMovement(view, timeline, marker, playheadColor, firstTick,
                            failures);
    checkLanesViewportBoundedRepaint(view, surfaces.lanes.widget, failures);
}

// The transport bar's Follow Playhead toggle: on (the default), playback
// scrolls once the playhead crosses 85% of the viewport; off, the camera
// stays exactly where the user put it however far the playhead runs.
void checkFollowScroll(SongView &view, const MidiTimeline &timeline,
                       QStringList &failures)
{
    const SongView::ViewState saved = view.viewState();
    SongView::ViewState parked = saved;
    parked.scrollPx = 0.0;
    // Zoom in far enough that the content is guaranteed wider than the
    // viewport — the follow-scroll clamps to the scrollable range, so a
    // short song at a narrow zoom would make the follow-on case a no-op.
    parked.pxPerBeat = 512.0;
    view.applyViewState(parked);
    // A tick well past the right edge of the parked viewport.
    const uint64_t farTick =
        uint64_t(double(view.width()) * 4.0 / view.pxPerTick()) + 1;
    const uint64_t farSample = timeline.sampleForTick(farTick);
    view.setPlayheadSample(farSample, true);
    if (view.viewState().scrollPx <= 0.0)
        failures.append("follow-on playback did not scroll to the playhead");
    view.applyViewState(parked);
    view.setFollowPlayhead(false);
    view.setPlayheadSample(farSample, true);
    if (view.viewState().scrollPx != 0.0)
        failures.append("follow-off playback still scrolled the view");
    view.setFollowPlayhead(true);
    view.setPlayheadSample(farSample, true);
    if (view.viewState().scrollPx <= 0.0)
        failures.append("re-enabled follow did not scroll to the playhead");
    view.applyViewState(saved);
}
} // namespace

QStringList playheadOverlayCheckFailures(SongView &view,
                                         const MidiTimeline &timeline)
{
    QStringList failures;
#ifdef __APPLE__
    if (usesNativeMacPlayheadRenderer())
        checkMacPlayheadLifecycle(failures);
#endif
    const bool viewWasVisible = view.isVisible();
    const bool viewHadDontShowOnScreen = view.testAttribute(Qt::WA_DontShowOnScreen);
    if (auto *marker = findPlayheadOverlay(view)) {
        if (!viewWasVisible) {
            view.setAttribute(Qt::WA_DontShowOnScreen);
            view.show();
            processPaints();
            (void)view.grab();
            processPaints();
        }
        checkPlayheadRendering(view, timeline, *marker, failures);
        checkFollowScroll(view, timeline, failures);
    } else {
        failures.append("unified playhead overlay not found");
    }
    view.setPlayheadSample(0, false);
    processPaints();
    if (!viewWasVisible)
        view.hide();
    view.setAttribute(Qt::WA_DontShowOnScreen, viewHadDontShowOnScreen);
    return failures;
}
