#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QList>
#include <QPixmap>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QWidget>
#include <array>

class QTimer;

// A little critter (the app logo, or any image the user picks) that floats
// over the main window (drag it anywhere; it keeps its place as a fraction
// of the window's free area, so a resize never strands it off-screen) and
// dances to the song. Deliberately cheap: the sprite is rasterized once per
// (DPR, cell size) into a pixmap, each frame is a single affine transform +
// drawPixmap over a ~30 px cell, and the frame timer only runs while the
// widget is on screen AND the song is playing. Stopped, it rests.
//
// The beat clock is pushed from the transport (sync): the widget free-runs
// from the last sync at the given BPM so frames stay smooth between pushes,
// and every push corrects drift, so the dance follows the Tempo lane and
// survives seeks. Which move it dances is chosen here, at bar lines, from
// the song's situation (see chooseMove).
class CompanionWidget : public QWidget
{
    Q_OBJECT

  public:
    enum class Move { Groove, Headbang, Shimmy, Sway, Spin };

    explicit CompanionWidget(int cellSize, QWidget *parent = nullptr);

    // Playback position as a fractional 0-based bar count in the current
    // meter (MidiTimeline::barPositionForTick), that meter's beats per bar
    // and beat rate, and how many engine channels are sounding. Call at
    // the playhead cadence while playing.
    void sync(double bar, int beatsPerBar, double beatsPerMinute, int activePcm, int maxPcm,
              int activeCgb);
    // Playing → dancing; otherwise rest pose, frame timer off.
    void setPlaying(bool playing);
    bool isPlaying() const { return m_playing; }
    // Ask for a Spin on the next downbeat (Play pressed, loop wrapped).
    void cueSpin();

    // Where the critter sits, as fractions (0..1) of the parent's free area
    // (parent size minus the cell): (1, 0) is the top-right corner. Applied
    // on every parent resize; a drag changes it and emits placementChanged.
    void setPlacement(QPointF fraction);
    QPointF placement() const { return m_placement; }
    void relayout();

    // Size as a percentage of the base cell (100 = a toolbar button). Only
    // the presets in scalePresets() are offered; other values snap to the
    // nearest. Changing it re-rasterizes the sprite and keeps the placement
    // fraction, so the critter grows in place. Set via the right-click menu
    // or Ctrl+wheel over the cell; emits scaleChanged.
    void setScale(int percent);
    int scale() const { return m_scale; }
    static const QList<int> &scalePresets();

    // Your own critter: any image Qt can read. It is fitted into the sprite
    // box (aspect kept, standing on the feet line) and dances with the same
    // transforms. Small images (pixel art) scale crisp; larger ones scale
    // and turn smoothly. Returns false (and keeps the current image) when
    // the file can't be read. An empty path restores the app logo.
    bool setCustomImage(const QString &path);
    QString customImagePath() const { return m_customPath; }
    bool hasCustomImage() const { return !m_customPath.isEmpty(); }

    Move currentMove() const { return m_move; }
    QSize sizeHint() const override { return m_cellSize; }
    QSize minimumSizeHint() const override { return sizeHint(); }

    // Harness hooks.
    int spriteExtent() const { return m_unit * 12; }
    bool frameTimerActive() const;
    QString debugState() const; // move clock internals, for harness logs

  signals:
    void placementChanged(QPointF fraction);
    void scaleChanged(int percent);
    void hideRequested();
    void chooseImageRequested();
    void defaultImageRequested();

  protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void changeEvent(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

  private:
    void forwardWheelToUnderlying(QWheelEvent *event);
    void rasterizeSprite();
    void applyCellSize();
    QPointF feetPoint() const;
    void frame();
    void updateTimers();
    double beatNow() const;
    void onBarLine(double bar);
    void rotateMove(double bar);
    Move chooseMove(double bar) const;
    void startMove(Move move, double bar);

    int m_baseUnit = 2; // sprite pixel size at 100%
    int m_scale = 100;
    int m_cell = 30; // nominal (toolbar-button) size the base unit derives from
    QSize m_cellSize{30, 30};
    QPointF m_feet;
    QPointF m_placement{1.0, 0.0};
    QPoint m_dragGrab; // press offset inside the cell while dragging
    bool m_dragging = false;
    int m_unit = 2; // sprite pixel size in DIPs
    QPixmap m_sprite;
    QImage m_image; // source (logo or custom), capped to kMaxSourceSide; scaled at raster time
    QString m_customPath; // empty while showing the default logo
    bool m_imageIsPixelArt = false;
    qreal m_rasterDpr = 0.0;
    QTimer *m_frameTimer = nullptr;

    // Beat clock: beats at the last sync plus free-run since.
    bool m_playing = false;
    int m_wheelAccum = 0;      // sub-notch Ctrl+wheel remainder (angle units)
    double m_beatAtSync = 0.0; // beats of the current meter since bar 0
    double m_beatsPerBar = 4.0;
    double m_bpm = 120.0; // beats (of the current meter) per minute
    QElapsedTimer m_sinceSync;

    // Move selection state.
    Move m_move = Move::Groove;
    double m_moveSinceBar = 0.0; // bar index the current move began on
    double m_lastBar = -1.0;
    double m_barLoadSum = 0.0; // Σ (PCM + CGB channels sounding) over the bar's syncs
    double m_barPcmSum = 0.0;  // Σ PCM channels sounding
    int m_barSyncs = 0;
    int m_maxPcm = 5;
    double m_holdBars = 2.0;   // how long the current move runs
    QElapsedTimer m_moveSince; // wall clock since the move began
    double m_lastBeatLine = -1.0;
    std::array<double, 5> m_lastDanced{}; // bar each move last began, by Move index
    bool m_spinCued = false;
    double m_spinStartBeat = -1.0; // beat the running Spin began on
};
