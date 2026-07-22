#include "waveformview.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

// Handle hit tolerance, and the y-band at the top where the crop handles
// grab (loop handles grab in the bottom band, so overlapping markers stay
// individually reachable).
constexpr int kHitPx = 5;

QColor handleColor(WaveformView::Handle handle)
{
    switch (handle) {
    case WaveformView::CropStartHandle:
    case WaveformView::CropEndHandle:
        return QColor(0xE0, 0xA0, 0x30);
    case WaveformView::LoopStartHandle:
    case WaveformView::LoopEndHandle:
        return QColor(0x40, 0xB0, 0xE0);
    default:
        return Qt::gray;
    }
}

} // namespace

WaveformView::WaveformView(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("sampleWaveform"));
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
}

void WaveformView::setSample(const ImportedSample *sample)
{
    m_sample = sample;
    if (sample && sample->frameCount() > 0) {
        m_pyramid.build(sample->buffer.data(), sample->frameCount());
        m_spp = double(sample->frameCount()) / std::max(1, width() - 2);
        m_scroll = 0.0;
    } else {
        m_pyramid = SampleDsp::PeakPyramid();
    }
    update();
}

void WaveformView::setMarkers(qint64 cropStart, qint64 cropEnd,
                              qint64 loopStart, qint64 loopEnd, bool loopOn)
{
    m_cropStart = cropStart;
    m_cropEnd = cropEnd;
    m_loopStart = loopStart;
    m_loopEnd = loopEnd;
    m_loopOn = loopOn;
    update();
}

void WaveformView::setSeamOverlay(std::vector<float> endWindow,
                                  std::vector<float> startWindow)
{
    if (m_seamEnd == endWindow && m_seamStart == startWindow)
        return;
    m_seamEnd = std::move(endWindow);
    m_seamStart = std::move(startWindow);
    update();
}

void WaveformView::setPlayhead(qint64 sourceSample)
{
    if (m_playhead == sourceSample)
        return;
    m_playhead = sourceSample;
    update();
}

int WaveformView::xForSample(qint64 sample) const
{
    return int(std::lround((double(sample) - m_scroll) / m_spp));
}

qint64 WaveformView::sampleForX(int x) const
{
    const qint64 n = m_sample ? m_sample->frameCount() : 0;
    return qBound<qint64>(0, qint64(std::llround(m_scroll + double(x) * m_spp)),
                          std::max<qint64>(0, n - 1));
}

qint64 WaveformView::handleSample(Handle handle) const
{
    switch (handle) {
    case CropStartHandle:
        return m_cropStart;
    case CropEndHandle:
        return m_cropEnd;
    case LoopStartHandle:
        return m_loopStart;
    case LoopEndHandle:
        return m_loopEnd;
    default:
        return 0;
    }
}

QPoint WaveformView::handlePoint(Handle handle) const
{
    const bool loop = handle == LoopStartHandle || handle == LoopEndHandle;
    return {xForSample(handleSample(handle)),
            loop ? height() * 3 / 4 : height() / 4};
}

WaveformView::Handle WaveformView::hitHandle(const QPoint &pos) const
{
    // Loop handles own the bottom half, crop handles the top half, so
    // coincident markers stay separately grabbable.
    const bool bottom = pos.y() >= height() / 2;
    const auto near = [&](Handle h) {
        return std::abs(pos.x() - xForSample(handleSample(h))) <= kHitPx;
    };
    if (bottom && m_loopOn) {
        if (near(LoopStartHandle))
            return LoopStartHandle;
        if (near(LoopEndHandle))
            return LoopEndHandle;
    }
    if (near(CropStartHandle))
        return CropStartHandle;
    if (near(CropEndHandle))
        return CropEndHandle;
    if (!bottom && m_loopOn) {
        if (near(LoopStartHandle))
            return LoopStartHandle;
        if (near(LoopEndHandle))
            return LoopEndHandle;
    }
    return NoHandle;
}

void WaveformView::clampView()
{
    const qint64 n = m_sample ? m_sample->frameCount() : 0;
    if (n <= 0)
        return;
    const double minSpp = 0.05;
    const double maxSpp = double(n) / std::max(1, width() - 2);
    m_spp = qBound(minSpp, m_spp, std::max(minSpp, maxSpp));
    m_scroll = qBound(0.0, m_scroll,
                      std::max(0.0, double(n) - m_spp * width()));
}

void WaveformView::dragHandleTo(qint64 sample)
{
    const qint64 n = m_sample ? m_sample->frameCount() : 0;
    if (n <= 0)
        return;
    switch (m_drag) {
    case CropStartHandle:
        m_cropStart = qBound<qint64>(0, sample, m_cropEnd - 1);
        break;
    case CropEndHandle:
        // Crop end is exclusive: it may sit at n.
        m_cropEnd = qBound<qint64>(m_cropStart + 1, sample, n);
        break;
    case LoopStartHandle:
        m_loopStart = qBound<qint64>(0, sample, m_loopEnd - 1);
        break;
    case LoopEndHandle:
        m_loopEnd = qBound<qint64>(m_loopStart + 1, sample, n - 1);
        break;
    default:
        return;
    }
    update();
    emit markersDragged(m_cropStart, m_cropEnd, m_loopStart, m_loopEnd);
}

void WaveformView::mousePressEvent(QMouseEvent *event)
{
    if (!m_sample) {
        QWidget::mousePressEvent(event);
        return;
    }
    if (event->button() == Qt::LeftButton) {
        const Handle h = hitHandle(event->pos());
        if (h != NoHandle) {
            m_drag = h;
            emit gestureStarted();
            dragHandleTo(sampleForX(event->pos().x()));
            return;
        }
    }
    if (event->button() == Qt::LeftButton
        || event->button() == Qt::MiddleButton) {
        m_panning = true;
        m_panX = event->pos().x();
        m_panScroll = m_scroll;
        setCursor(Qt::ClosedHandCursor);
    }
}

void WaveformView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_drag != NoHandle) {
        dragHandleTo(sampleForX(event->pos().x()));
        return;
    }
    if (m_panning) {
        m_scroll = m_panScroll - double(event->pos().x() - m_panX) * m_spp;
        clampView();
        update();
        return;
    }
    const Handle h = hitHandle(event->pos());
    if (h != m_hover) {
        m_hover = h;
        setCursor(h != NoHandle ? Qt::SizeHorCursor : Qt::ArrowCursor);
    }
}

void WaveformView::mouseReleaseEvent(QMouseEvent *event)
{
    Q_UNUSED(event);
    if (m_drag != NoHandle) {
        m_drag = NoHandle;
        emit gestureFinished();
    }
    if (m_panning) {
        m_panning = false;
        setCursor(m_hover != NoHandle ? Qt::SizeHorCursor : Qt::ArrowCursor);
    }
}

void WaveformView::mouseDoubleClickEvent(QMouseEvent *event)
{
    Q_UNUSED(event);
    // Zoom to fit.
    if (!m_sample || m_sample->frameCount() <= 0)
        return;
    m_userZoomed = false;
    m_spp = double(m_sample->frameCount()) / std::max(1, width() - 2);
    m_scroll = 0.0;
    update();
}

void WaveformView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (!m_userZoomed && m_sample && m_sample->frameCount() > 0) {
        m_spp = double(m_sample->frameCount()) / std::max(1, width() - 2);
        m_scroll = 0.0;
    }
}

void WaveformView::wheelEvent(QWheelEvent *event)
{
    if (!m_sample || m_sample->frameCount() <= 0)
        return;
    m_userZoomed = true;
    const double factor =
        std::pow(1.2, -event->angleDelta().y() / 120.0);
    const double anchorX = event->position().x();
    const double anchorSample = m_scroll + anchorX * m_spp;
    m_spp *= factor;
    clampView();
    m_scroll = anchorSample - anchorX * m_spp;
    clampView();
    update();
    event->accept();
}

void WaveformView::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter p(this);
    const QRect r = rect();
    p.fillRect(r, palette().base());
    if (!m_sample || m_sample->frameCount() <= 0)
        return;
    const float *x = m_sample->buffer.data();
    const qint64 n = m_sample->frameCount();
    const int h = r.height();
    const int mid = h / 2;
    const double yScale = double(h) * 0.48;

    // Crop-outside dim + loop-region tint.
    const QColor dim(0, 0, 0, 70);
    const int cropL = xForSample(m_cropStart);
    const int cropR = xForSample(m_cropEnd);
    if (m_loopOn) {
        QColor loopTint = handleColor(LoopStartHandle);
        loopTint.setAlpha(28);
        p.fillRect(QRect(QPoint(xForSample(m_loopStart), 0),
                         QPoint(xForSample(m_loopEnd), h)),
                   loopTint);
    }

    // Waveform columns via the pyramid.
    p.setPen(palette().color(QPalette::Highlight));
    for (int px = 0; px < r.width(); px++) {
        const qint64 from = qint64(std::floor(m_scroll + px * m_spp));
        const qint64 to = std::max<qint64>(
            from + 1, qint64(std::ceil(m_scroll + (px + 1) * m_spp)));
        if (to <= 0 || from >= n)
            continue;
        const SampleDsp::PeakPyramid::MinMax mm =
            m_pyramid.query(x, from, to);
        const int y0 = mid - int(std::lround(double(mm.hi) * yScale));
        const int y1 = mid - int(std::lround(double(mm.lo) * yScale));
        p.drawLine(px, y0, px, std::max(y1, y0 + 1));
    }
    p.setPen(QColor(128, 128, 128, 120));
    p.drawLine(0, mid, r.width(), mid);

    if (cropL > 0)
        p.fillRect(QRect(0, 0, cropL, h), dim);
    if (cropR < r.width())
        p.fillRect(QRect(cropR, 0, r.width() - cropR, h), dim);

    // Handles: crop grips at the top, loop grips at the bottom.
    const auto drawHandle = [&](Handle handle) {
        const int hx = xForSample(handleSample(handle));
        if (hx < -kHitPx || hx > r.width() + kHitPx)
            return;
        const QColor c = handleColor(handle);
        p.setPen(QPen(c, m_drag == handle || m_hover == handle ? 2 : 1));
        p.drawLine(hx, 0, hx, h);
        const bool loop =
            handle == LoopStartHandle || handle == LoopEndHandle;
        const bool leftGrip =
            handle == CropStartHandle || handle == LoopStartHandle;
        const int gy = loop ? h - 8 : 0;
        p.fillRect(QRect(leftGrip ? hx : hx - 6, gy, 7, 8), c);
    };
    drawHandle(CropStartHandle);
    drawHandle(CropEndHandle);
    if (m_loopOn) {
        drawHandle(LoopStartHandle);
        drawHandle(LoopEndHandle);
    }

    // Playhead.
    if (m_playhead >= 0) {
        const int px = xForSample(m_playhead);
        if (px >= 0 && px <= r.width()) {
            p.setPen(QPen(QColor(0xE8, 0x50, 0x50), 1));
            p.drawLine(px, 0, px, h);
        }
    }

    // Seam overlay: the PROCESSED window leading into the loop end
    // superimposed on the one leading into the loop start (owner-fed via
    // setSeamOverlay) — matched shapes mean a clean seam, and render-stage
    // effects like the crossfade bake visibly reshape the traces.
    if (m_loopOn && m_seamEnd.size() >= 8
        && m_seamEnd.size() == m_seamStart.size()) {
        const qint64 w = qint64(m_seamEnd.size());
        const QRect inset(r.width() - 236, 6, 228, 56);
        p.fillRect(inset, palette().color(QPalette::AlternateBase));
        p.setPen(palette().color(QPalette::Mid));
        p.drawRect(inset);
        const auto drawWindow = [&](const std::vector<float> &win,
                                    const QColor &c) {
            QPainterPath path;
            for (qint64 i = 0; i < w; i++) {
                const double v = double(win[size_t(i)]);
                const double px = inset.left() + 1
                    + double(i) * (inset.width() - 2) / double(w - 1);
                const double py = inset.center().y()
                    - v * (inset.height() * 0.45);
                if (i == 0)
                    path.moveTo(px, py);
                else
                    path.lineTo(px, py);
            }
            p.setPen(QPen(c, 1));
            p.drawPath(path);
        };
        drawWindow(m_seamEnd, QColor(0xE8, 0x50, 0x50));
        drawWindow(m_seamStart, QColor(0x40, 0xB0, 0xE0));
        p.setPen(palette().color(QPalette::Mid));
        p.drawText(inset.adjusted(4, 2, -4, -2), Qt::AlignBottom,
                   tr("seam"));
    }
}
