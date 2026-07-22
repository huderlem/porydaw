#include "rollcheckplayhead.h"

#include <QCoreApplication>
#include <QEvent>
#include <QObject>
#include <QPaintEvent>
#include <QRegion>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <vector>

#include "core/miditimeline.h"
#include "ui/eventlistview.h"
#include "ui/playheadoverlay.h"
#include "ui/songview.h"

namespace
{

class PaintRegionProbe : public QObject
{
public:
    void clear()
    {
        m_regions.clear();
    }

    bool repaintedAnyBroadly(const QWidget *allowed, int maxWidth) const
    {
        return std::any_of(
            m_regions.cbegin(), m_regions.cend(),
            [=](const DirtyRegion &region)
            {
                return region.widget != allowed
                       && region.bounds.width() > maxWidth;
            });
    }

    bool repaintedBroadly(const QWidget *widget, int maxWidth) const
    {
        return std::any_of(
            m_regions.cbegin(), m_regions.cend(),
            [=](const DirtyRegion &region)
            {
                return region.widget == widget
                       && region.bounds.width() > maxWidth;
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
        if (event->type() == QEvent::Paint)
        {
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
QWidget *findTimeRuler(SongView &view, const songview::PlayheadOverlay *overlay)
{
    for (QWidget *child : view.findChildren<QWidget *>(
             QString(), Qt::FindDirectChildrenOnly))
    {
        const QRect childArea(child->mapTo(&view, QPoint()), child->size());
        if (child != overlay && child->isVisible() && childArea.top() == 0
            && childArea.width() == view.width())
            return child;
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

qreal playheadCenter(const QPixmap &pixmap, const QColor &playheadColor) {
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

bool hasPlayheadRedLine(const QImage &image, qreal devicePixelRatio,
                        qreal logicalX, const QRect &logicalArea,
                        const QColor &playheadColor)
{
    if (logicalArea.isEmpty())
    {
        return false;
    }

    const int left = std::max(
        0, qFloor((logicalX - 1.0) * devicePixelRatio));
    const int right = std::min(
        image.width() - 1, qCeil((logicalX + 1.0) * devicePixelRatio));
    const int top = std::max(0, qFloor(logicalArea.top() * devicePixelRatio));
    const int bottom = std::min(
        image.height() - 1,
        qCeil((logicalArea.bottom() + 1) * devicePixelRatio) - 1);
    for (int x = left; x <= right; ++x)
    {
        int consecutivePixels = 0;
        for (int y = top; y <= bottom; ++y)
        {
            if (isCompositedPlayheadRed(image.pixelColor(x, y), playheadColor))
            {
                if (++consecutivePixels >= 3)
                {
                    return true;
                }
            }
            else
            {
                consecutivePixels = 0;
            }
        }
    }
    return false;
}

int playheadRedWidth(const QImage &image, qreal devicePixelRatio,
                     qreal logicalX, int logicalY,
                     const QColor &playheadColor)
{
    const int left = std::max(
        0, qFloor((logicalX - 4.0) * devicePixelRatio));
    const int right = std::min(
        image.width() - 1, qCeil((logicalX + 4.0) * devicePixelRatio));
    const int y = std::clamp(qRound(logicalY * devicePixelRatio),
                             0, image.height() - 1);
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

void checkEventListRendering(SongView &view,
                             songview::PlayheadOverlay &marker,
                             qreal stoppedMarkerCenter, const QRect &rulerArea,
                             const QColor &playheadColor, QStringList &failures)
{
    auto *events = view.findChild<EventListView *>();
    if (!events) {
        failures.append("EventListView child not found");
        return;
    }
    const qreal playheadX = marker.mapTo(&view, QPoint()).x() + stoppedMarkerCenter;
    view.setEventListVisible(true);
    processPaints();
    const QRect eventListArea =
        QRect(events->mapTo(&view, QPoint()), events->size())
            .intersected(view.rect());
    const QPixmap composedPixmap = view.grab();
    const QImage composedImage = composedPixmap.toImage();
    const qreal composedDpr = composedPixmap.devicePixelRatio();
    if (!events->isVisible() || eventListArea.isEmpty()) {
        failures.append("event list is not visible for the playhead check");
        return;
    }
    if (playheadX < eventListArea.left() || playheadX > eventListArea.right()) {
        failures.append("could not map playhead into the event list");
        return;
    }
    const int triangleHeight = std::min(songview::kPlayheadTriangleHeight + 1,
                                        eventListArea.height());
    const QRect triangleArea(eventListArea.left(), eventListArea.top(),
                             eventListArea.width(), triangleHeight);
    const auto hasLine = [&](const QRect &area) {
        return hasPlayheadRedLine(composedImage, composedDpr, playheadX, area,
                                  playheadColor);
    };
    const auto redWidth = [&](int y) {
        return playheadRedWidth(composedImage, composedDpr, playheadX, y,
                                playheadColor);
    };
    if (!hasLine(triangleArea)) {
        failures.append("playhead triangle did not render below the time ruler");
    }
    if (redWidth(triangleArea.bottom() - 1) <= redWidth(triangleArea.top())) {
        failures.append("playhead triangle did not point up in the event list");
    }
    if (hasLine(QRect(eventListArea.left(),
                      eventListArea.top() + triangleHeight,
                      eventListArea.width(),
                      eventListArea.height() - triangleHeight))) {
        failures.append("playhead line overpainted the event list");
    }
    if (hasLine(rulerArea)) {
        failures.append("playhead rendered in the event-list time ruler");
    }
    if (!hasLine(QRect(0, 0, view.width(), eventListArea.top()))
        && !hasLine(QRect(0, eventListArea.bottom() + 1, view.width(),
                          view.height() - eventListArea.bottom() - 1))) {
        failures.append("playhead overlay did not render on visible timeline "
                        "surfaces");
    }
}

void checkFractionalMovement(SongView &view, const MidiTimeline &timeline,
                             songview::PlayheadOverlay &marker,
                             const QColor &playheadColor, uint64_t firstTick,
                             QStringList &failures) {
    uint64_t fractionalStartSample = timeline.sampleForTick(firstTick);
    uint64_t fractionalEndSample = fractionalStartSample;
    double playheadTick = timeline.tickForSample(fractionalStartSample);
    int fractionalBucketX = view.contentX(playheadTick);
    double fractionalStartX = playheadTick * view.pxPerTick();
    const uint64_t fractionalSearchEnd = timeline.sampleForTick(firstTick + 2);
    for (uint64_t sample = fractionalStartSample + 1;
         sample <= fractionalSearchEnd; ++sample) {
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
    const qreal fractionalStart = playheadCenter(marker.grab(), playheadColor);
    view.setPlayheadSample(fractionalEndSample, true);
    processPaints();
    const qreal fractionalEnd = playheadCenter(marker.grab(), playheadColor);
    const qreal expectedDelta =
        (timeline.tickForSample(fractionalEndSample)
         - timeline.tickForSample(fractionalStartSample)) * view.pxPerTick();
    if (std::abs((fractionalEnd - fractionalStart) - expectedDelta) > 0.2) {
        failures.append("fractional playhead movement did not match its timeline "
                        "position");
    }
}

void checkPlayheadRendering(SongView &view, const MidiTimeline &timeline,
                            songview::PlayheadOverlay &marker, QStringList &failures) {
    const int plotWidth = view.width() - songview::kGutterW;
    if (plotWidth <= 64) {
        failures.append("timeline plot is too narrow for the playhead check");
        return;
    }
    const auto tickAtContentX = [&view](int x) {
        return uint64_t(std::ceil(std::max(0.0, view.tickAtContentX(x))));
    };
    const uint64_t firstTick = tickAtContentX(plotWidth / 3);
    const uint64_t secondTick = tickAtContentX(plotWidth / 3 + 12);
    const int firstX = view.contentX(double(firstTick));
    const int secondX = view.contentX(double(secondTick));
    if (firstX < 0 || secondX >= plotWidth || secondX <= firstX
        || secondX - firstX > 32) {
        failures.append("could not choose nearby visible playhead ticks");
        return;
    }
    QWidget *ruler = findTimeRuler(view, &marker);
    if (!ruler) {
        failures.append("time ruler not found for the playhead check");
        return;
    }
    PaintRegionProbe probe;
    view.installEventFilter(&probe);
    for (QWidget *child : view.findChildren<QWidget *>())
        child->installEventFilter(&probe);
    const QColor playheadColor(226, 66, 66);
    const auto expectedCenter = [&](uint64_t sample) {
        const QPoint timelineOrigin = ruler->mapTo(&view, QPoint(songview::kGutterW, 0));
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
    const uint64_t firstSample = timeline.sampleForTick(firstTick);
    const uint64_t secondSample = timeline.sampleForTick(secondTick);
    view.setPlayheadSample(firstSample, false);
    processPaints();
    const qreal firstMarkerCenter = playheadCenter(marker.grab(), playheadColor);
    checkCenter(firstMarkerCenter, firstSample, QStringLiteral("stopped"));
    const QRect rulerArea(ruler->mapTo(&view, QPoint()), ruler->size());
    if (firstMarkerCenter >= 0.0) {
        const QPixmap composedPixmap = view.grab();
        const qreal playheadX = marker.mapTo(&view, QPoint()).x() + firstMarkerCenter;
        if (hasPlayheadRedLine(composedPixmap.toImage(),
                               composedPixmap.devicePixelRatio(), playheadX,
                               rulerArea, playheadColor)) {
            failures.append("playhead rendered in the time ruler");
        }
    }
    view.setPlayheadSample(firstSample, true);
    processPaints();
    probe.clear();
    view.setPlayheadSample(secondSample, true);
    processPaints();
    // Dirty strip: move delta + full glow diameter + full triangle width.
    const int maxPlayheadExposureWidth =
        secondX - firstX + 2 * songview::kPlayheadGlowRadius
        + 2 * songview::kPlayheadTriangleHalfWidth;
    const bool overlayPaintedBroadly = probe.repaintedBroadly(&marker, maxPlayheadExposureWidth);
    const bool anotherWidgetPaintedBroadly = probe.repaintedAnyBroadly(&marker, maxPlayheadExposureWidth);
    const QPixmap playingPixmap = marker.grab();
    const qreal playingMarkerCenter = playheadCenter(playingPixmap, playheadColor);
    checkCenter(playingMarkerCenter, secondSample, QStringLiteral("playing"));
    if (overlayPaintedBroadly)
        failures.append("playhead move repainted the overlay broadly");
    if (anotherWidgetPaintedBroadly)
        failures.append("playhead move repainted another timeline widget broadly");
    view.setPlayheadSample(secondSample, false);
    processPaints();
    const QPixmap stoppedPixmap = marker.grab();
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
}

} // namespace

QStringList playheadOverlayCheckFailures(SongView &view, const MidiTimeline &timeline) {
    QStringList failures;
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
