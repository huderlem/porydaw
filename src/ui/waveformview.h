#pragma once

#include <QWidget>

#include <vector>

#include "audio/sampledata.h"
#include "audio/sampledsp.h"

// The Sample Studio waveform view (docs/sample-studio/PLAN.md §Modules):
// zoomable source-domain waveform over a peak pyramid, with draggable crop
// and loop handles, a loop-seam overlay (the PROCESSED pre-loop-end and
// pre-loop-start windows superimposed, fed by the owner via setSeamOverlay
// so render-stage effects like the crossfade bake are visible), and a
// playhead during audition. Pure view: it emits marker-change signals during drags;
// the owner applies them to the document and reflects state back through
// setMarkers.
class WaveformView : public QWidget
{
    Q_OBJECT

public:
    explicit WaveformView(QWidget *parent = nullptr);

    // Not owned; the owner keeps the sample alive for this view's lifetime.
    void setSample(const ImportedSample *sample);
    void setMarkers(qint64 cropStart, qint64 cropEnd, qint64 loopStart,
                    qint64 loopEnd, bool loopOn);
    // The seam-inset traces, in FINAL render domain (equal lengths; empty
    // hides the inset): the window leading into the loop end and the one
    // leading into the loop start. Owner-fed from the processed output so
    // the crossfade bake / normalize / resample all show up in the inset.
    void setSeamOverlay(std::vector<float> endWindow,
                        std::vector<float> startWindow);
    // Display gain for the trace (the normalize stage's applied gain), so
    // the waveform shows the amplitude the render actually has. Clamped
    // to full scale when the gain would clip past the widget.
    void setGain(double gain);
    const std::vector<float> &seamEndWindow() const { return m_seamEnd; }
    const std::vector<float> &seamStartWindow() const { return m_seamStart; }
    // Source-domain playhead; -1 hides it.
    void setPlayhead(qint64 sourceSample);
    bool gestureActive() const { return m_drag != NoHandle || m_panning; }

    enum Handle { NoHandle, CropStartHandle, CropEndHandle, LoopStartHandle,
                  LoopEndHandle };

    // View mapping + handle geometry (also the harness's drag targets).
    int xForSample(qint64 sample) const;
    qint64 sampleForX(int x) const;
    QPoint handlePoint(Handle handle) const;

    QSize sizeHint() const override { return {640, 220}; }
    QSize minimumSizeHint() const override { return {320, 120}; }

signals:
    void gestureStarted();
    // Emitted live while a handle drags; the owner applies to the document.
    void markersDragged(qint64 cropStart, qint64 cropEnd, qint64 loopStart,
                        qint64 loopEnd);
    void gestureFinished();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    Handle hitHandle(const QPoint &pos) const;
    qint64 handleSample(Handle handle) const;
    void dragHandleTo(qint64 sample);
    void clampView();

    const ImportedSample *m_sample = nullptr;
    SampleDsp::PeakPyramid m_pyramid;
    double m_spp = 1.0;    // samples per pixel
    double m_scroll = 0.0; // first visible sample
    qint64 m_cropStart = 0, m_cropEnd = 0;
    qint64 m_loopStart = 0, m_loopEnd = 0;
    bool m_loopOn = false;
    double m_gain = 1.0;
    std::vector<float> m_seamEnd;
    std::vector<float> m_seamStart;
    qint64 m_playhead = -1;
    // Fit-to-width until the user zooms: the initial fit happens before the
    // layout settles, so resizes must re-fit or the view shows dead space.
    bool m_userZoomed = false;

    Handle m_drag = NoHandle;
    Handle m_hover = NoHandle;
    bool m_panning = false;
    int m_panX = 0;
    double m_panScroll = 0.0;
};
