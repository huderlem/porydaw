#include "companionwidget.h"

#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QEvent>
#include <QImageReader>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace {

constexpr int kSpriteSize = 12;
// Custom images up to this many pixels on their longer side are treated as
// pixel art: scaled by nearest neighbour and rotated without smoothing.
constexpr int kPixelArtMaxSide = 64;
// Larger sources are downscaled once at load to this longer side: the
// biggest sprite box is kSpriteSize × baseUnit × 400 % (× DPR), well under
// this, so a phone photo isn't kept resident and re-sampled per raster.
constexpr int kMaxSourceSide = 512;
// The default sprite: the app logo.
const char *const kDefaultImage = ":/icons/porydaw-256.png";
// The cell is the sprite plus 3 units of headroom for hops and turns; the
// moves' amplitudes are sized to that headroom (in units), so every scale
// keeps the sprite inside the cell.
constexpr int kCellUnits = kSpriteSize + 3;
constexpr int kFrameIntervalMs = 33; // ~30 fps; plenty for a 30 px cell
// The critter steps once per felt beat, and nobody feels 252 eighths a
// minute: above kMaxStepBpm the meter's beats are grouped the way a
// player would count them (6/8 → 2 dotted quarters, 4/4 → 2 halves) until
// the rate fits. kMinBpm/kMaxBpm are the sanity clamp after that.
constexpr double kMaxStepBpm = 200.0;
constexpr double kMinBpm = 20.0;
constexpr double kMaxBpm = 400.0;
// Meter used to sample the pose envelope for cell sizing; the bar-phase
// keyframes are the same for any meter, only their speed differs.
constexpr double kBoundsBeatsPerBar = 4.0;
constexpr double kMinHoldBars = 2.0;     // a move sticks at least this long...
constexpr double kMaxHoldBars = 4.0;     // ...and at most this long before rotating
constexpr double kMaxHoldSeconds = 10.0; // hard cap on one move, for slow songs
constexpr double kShimmyBpm = 150.0;     // fast songs shimmy
constexpr double kSwayBpm = 90.0;        // slow songs sway
constexpr double kSwayMaxLoad = 2.5;     // ...as do sparse ones (mean channels sounding)
constexpr double kHeadbangLoad = 0.75;   // mean PCM use ≥ this share of the pool is "loud"
constexpr double kSpinBeats = 4.0;

double easeInOut(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return t < 0.5 ? 2 * t * t : 1 - std::pow(-2 * t + 2, 2) / 2;
}

double easeOut(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return 1 - (1 - t) * (1 - t);
}

using Key = std::pair<double, double>;

// Piecewise keyframe interpolation on phase ∈ [0,1] with an easing between
// neighbouring keys. Cheap enough to run several times per frame.
template <size_t N>
double keyed(double phase, const std::array<Key, N> &keys, double (*ease)(double) = easeInOut)
{
    for (size_t i = 1; i < N; i++) {
        if (phase <= keys[i].first) {
            const double span = keys[i].first - keys[i - 1].first;
            const double t = span > 0 ? (phase - keys[i - 1].first) / span : 1.0;
            return keys[i - 1].second + (keys[i].second - keys[i - 1].second) * ease(t);
        }
    }
    return keys[N - 1].second;
}

double frac(double v)
{
    return v - std::floor(v);
}

// The pose for one frame, as a transform from sprite-box pixels (0..extent
// square, feet at bottom centre) to pixels relative to the feet point.
// Everything is in units of u, so the same math sizes the cell (see
// poseBounds) and paints every scale.
QTransform poseTransform(CompanionWidget::Move current, bool playing, double beat,
                         double beatsPerBar, double spinStart, double u)
{
    using Move = CompanionWidget::Move;
    const double extent = kSpriteSize * u;
    QTransform xf;
    if (!playing) {
        // Rest: a relaxed little squat.
        xf.scale(1.08, 0.92);
    } else {
        const double b1 = frac(beat);               // per beat
        const double b2 = frac(beat / 2.0);         // per two beats
        const double b4 = frac(beat / beatsPerBar); // per bar
        const bool spinning = spinStart >= 0 && beat >= spinStart;
        const Move move = spinning ? Move::Spin : current;

        switch (move) {
        case Move::Groove: {
            // Three nested layers: a bar-line bump, a lean about the feet
            // over two beats, and a per-beat hop with stretch on the way up
            // and squash on landing.
            const double barY = keyed(
                b4, std::array<Key, 4>{{{0.0, 0.0}, {0.06, -0.5 * u}, {0.14, 0.0}, {1.0, 0.0}}});
            const double lean =
                keyed(b2, std::array<Key, 5>{
                              {{0.0, 0.0}, {0.25, -8.0}, {0.5, 0.0}, {0.75, 8.0}, {1.0, 0.0}}});
            const double hopY = keyed(
                b1, std::array<Key, 4>{{{0.0, 0.0}, {0.22, -1.5 * u}, {0.48, 0.0}, {1.0, 0.0}}});
            const double sx =
                keyed(b1, std::array<Key, 5>{
                              {{0.0, 1.0}, {0.22, 0.9}, {0.48, 1.12}, {0.7, 1.0}, {1.0, 1.0}}});
            const double sy =
                keyed(b1, std::array<Key, 5>{
                              {{0.0, 1.0}, {0.22, 1.06}, {0.48, 0.88}, {0.7, 1.0}, {1.0, 1.0}}});
            xf.translate(0.0, barY);
            xf.rotate(lean);
            xf.translate(0.0, hopY);
            xf.scale(sx, sy);
            break;
        }
        case Move::Headbang: {
            // A sharp nod forward on every beat, pivoting near the feet's
            // left side, with a small rebound.
            const double nod =
                keyed(b1,
                      std::array<Key, 5>{
                          {{0.0, 0.0}, {0.26, -26.0}, {0.5, -18.0}, {0.78, 4.0}, {1.0, 0.0}}},
                      easeOut);
            xf.translate(-0.15 * extent, 0.0);
            xf.rotate(nod);
            xf.translate(0.15 * extent, 0.0);
            break;
        }
        case Move::Shimmy: {
            // Side-to-side skew with a half-unit shuffle, once per beat.
            const double skew =
                keyed(b1, std::array<Key, 5>{
                              {{0.0, 0.0}, {0.25, 18.0}, {0.5, 0.0}, {0.75, -18.0}, {1.0, 0.0}}});
            const double shuffle = keyed(
                b1, std::array<Key, 5>{
                        {{0.0, 0.0}, {0.25, 0.5 * u}, {0.5, 0.0}, {0.75, -0.5 * u}, {1.0, 0.0}}});
            xf.translate(shuffle, 0.0);
            xf.shear(std::tan(skew * M_PI / 180.0), 0.0);
            break;
        }
        case Move::Sway: {
            // A slow rock across two beats, stretching at the extremes and
            // settling in the middle.
            const double rock =
                keyed(b2, std::array<Key, 5>{
                              {{0.0, 0.0}, {0.25, -14.0}, {0.5, 0.0}, {0.75, 14.0}, {1.0, 0.0}}});
            const double sx =
                keyed(b2, std::array<Key, 5>{
                              {{0.0, 1.05}, {0.25, 0.94}, {0.5, 1.05}, {0.75, 0.94}, {1.0, 1.05}}});
            const double sy =
                keyed(b2, std::array<Key, 5>{
                              {{0.0, 0.95}, {0.25, 1.06}, {0.5, 0.95}, {0.75, 1.06}, {1.0, 0.95}}});
            xf.rotate(rock);
            xf.scale(sx, sy);
            break;
        }
        case Move::Spin: {
            // One bar: leap and turn a full circle by the first quarter,
            // (tucking small mid-air so the turned sprite stays inside the
            // cell), land with a squash, then two little bobs while settling.
            const double p = std::clamp((beat - spinStart) / kSpinBeats, 0.0, 1.0);
            const double y = keyed(p, std::array<Key, 9>{{{0.0, 0.0},
                                                          {0.11, -2.0 * u},
                                                          {0.22, 0.0},
                                                          {0.46, 0.0},
                                                          {0.48, -0.7 * u},
                                                          {0.50, 0.0},
                                                          {0.71, 0.0},
                                                          {0.73, -0.7 * u},
                                                          {0.75, 0.0}}});
            const double turn =
                keyed(p, std::array<Key, 3>{{{0.0, 0.0}, {0.22, 360.0}, {1.0, 360.0}}}, easeOut);
            const double sx =
                keyed(p, std::array<Key, 5>{
                             {{0.0, 1.0}, {0.11, 0.75}, {0.22, 1.0}, {0.26, 1.15}, {0.34, 1.0}}});
            const double sy =
                keyed(p, std::array<Key, 5>{
                             {{0.0, 1.0}, {0.11, 0.75}, {0.22, 1.0}, {0.26, 0.85}, {0.34, 1.0}}});
            xf.translate(0.0, y);
            // Turn about the sprite's centre, scale about the feet.
            xf.scale(sx, sy);
            xf.translate(0.0, -extent / 2.0);
            xf.rotate(turn);
            xf.translate(0.0, extent / 2.0);
            break;
        }
        }
    }
    xf.translate(-extent / 2.0, -extent);
    return xf;
}

// The union of every pose's sprite box over full cycles of every move, in
// units, relative to the feet point. The cell is sized from this so no
// transform ever reaches outside it — for the built-in sprite or a custom
// image that fills the whole box.
QRectF poseBounds()
{
    static const QRectF bounds = [] {
        using Move = CompanionWidget::Move;
        const QRectF box(0, 0, kSpriteSize, kSpriteSize);
        QRectF total =
            poseTransform(Move::Groove, false, 0.0, kBoundsBeatsPerBar, -1.0, 1.0).mapRect(box);
        constexpr int kSamples = 512; // per bar; keyframes are far coarser
        for (Move move : {Move::Groove, Move::Headbang, Move::Shimmy, Move::Sway}) {
            for (int i = 0; i < kSamples; i++) {
                const double beat = kBoundsBeatsPerBar * i / kSamples;
                total |=
                    poseTransform(move, true, beat, kBoundsBeatsPerBar, -1.0, 1.0).mapRect(box);
            }
        }
        for (int i = 0; i <= kSamples; i++) {
            const double beat = kSpinBeats * i / kSamples;
            total |=
                poseTransform(Move::Groove, true, beat, kBoundsBeatsPerBar, 0.0, 1.0).mapRect(box);
        }
        return total;
    }();
    return bounds;
}

} // namespace

CompanionWidget::CompanionWidget(int cellSize, QWidget *parent)
    : QWidget(parent)
    , m_cell(std::max(cellSize, kSpriteSize))
{
    m_baseUnit = std::max(1, m_cell / kCellUnits);
    m_unit = m_baseUnit;
    applyCellSize();
    // Transparent around the sprite: whatever it floats over shows through.
    // Each frame invalidates only this cell, and the timeline surfaces
    // beneath are pixmap-cached, so the repaint under us is a small blit.
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::OpenHandCursor);
    if (parent)
        parent->installEventFilter(this);

    m_frameTimer = new QTimer(this);
    m_frameTimer->setTimerType(Qt::CoarseTimer);
    m_frameTimer->setInterval(kFrameIntervalMs);
    connect(m_frameTimer, &QTimer::timeout, this, &CompanionWidget::frame);

    m_sinceSync.start();
    setCustomImage(QString());
}

bool CompanionWidget::frameTimerActive() const
{
    return m_frameTimer->isActive();
}

QString CompanionWidget::debugState() const
{
    return QStringLiteral("beat=%1 sinceBar=%2 hold=%3 lastBar=%4 spinStart=%5 cued=%6 "
                          "sinceMs=%7 syncs=%8")
        .arg(beatNow(), 0, 'f', 1)
        .arg(m_moveSinceBar)
        .arg(m_holdBars)
        .arg(m_lastBar)
        .arg(m_spinStartBeat)
        .arg(m_spinCued)
        .arg(m_moveSince.isValid() ? m_moveSince.elapsed() : -1)
        .arg(m_barSyncs);
}

const QList<int> &CompanionWidget::scalePresets()
{
    static const QList<int> presets{100, 150, 200, 300, 400};
    return presets;
}

void CompanionWidget::setScale(int percent)
{
    int nearest = scalePresets().first();
    for (int preset : scalePresets()) {
        if (std::abs(preset - percent) < std::abs(nearest - percent))
            nearest = preset;
    }
    if (nearest == m_scale && m_unit == std::max(1, qRound(m_baseUnit * m_scale / 100.0)))
        return;
    m_scale = nearest;
    m_unit = std::max(1, qRound(m_baseUnit * m_scale / 100.0));
    applyCellSize();
    m_rasterDpr = 0.0; // sprite pixmaps are per-unit; redraw on next paint
    relayout();
    update();
    emit scaleChanged(m_scale);
}

void CompanionWidget::wheelEvent(QWheelEvent *event)
{
    if (!(event->modifiers() & Qt::ControlModifier)) {
        // We float over the timeline but are a sibling of it (parented to
        // the window), so an ignored wheel would climb to the window and
        // die. Hand it to whatever sits beneath the cursor instead.
        forwardWheelToUnderlying(event);
        return;
    }
    // Trackpads and free-spin wheels deliver deltas well under one notch
    // per event; accumulate to a notch before stepping (like the roll).
    if (event->phase() != Qt::ScrollMomentum) {
        const QPoint pixel = event->pixelDelta();
        m_wheelAccum += pixel.isNull() ? event->angleDelta().y() : pixel.y() * 5;
    }
    constexpr int kNotch = 120;
    const int steps = m_wheelAccum / kNotch;
    m_wheelAccum -= steps * kNotch;
    event->accept();
    if (steps == 0)
        return;
    const auto &presets = scalePresets();
    const int index = std::clamp(int(presets.indexOf(m_scale)) + (steps > 0 ? 1 : -1), 0,
                                 int(presets.size()) - 1);
    setScale(presets[index]);
}

void CompanionWidget::forwardWheelToUnderlying(QWheelEvent *event)
{
    event->accept(); // never let it bubble to the window
    QWidget *host = parentWidget();
    if (!host)
        return;
    const QPoint hostPos = mapToParent(event->position().toPoint());
    // Look through ourselves: childAt() would return this widget.
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    QWidget *target = host->childAt(hostPos);
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    if (!target || target == this)
        return;
    QWheelEvent forwarded(target->mapFrom(host, hostPos), event->globalPosition(),
                          event->pixelDelta(), event->angleDelta(), event->buttons(),
                          event->modifiers(), event->phase(), event->inverted(), event->source());
    QCoreApplication::sendEvent(target, &forwarded);
}

void CompanionWidget::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    auto *sizeMenu = menu.addMenu(tr("Size"));
    for (int preset : scalePresets()) {
        QAction *action = sizeMenu->addAction(tr("%1%").arg(preset));
        action->setCheckable(true);
        action->setChecked(preset == m_scale);
        connect(action, &QAction::triggered, this, [this, preset] { setScale(preset); });
    }
    menu.addSeparator();
    menu.addSeparator();
    connect(menu.addAction(tr("Choose Image…")), &QAction::triggered, this,
            &CompanionWidget::chooseImageRequested);
    QAction *reset = menu.addAction(tr("Use Default Image"));
    reset->setEnabled(hasCustomImage());
    connect(reset, &QAction::triggered, this, &CompanionWidget::defaultImageRequested);
    menu.addSeparator();
    connect(menu.addAction(tr("Hide")), &QAction::triggered, this, &CompanionWidget::hideRequested);
    menu.exec(event->globalPos());
    event->accept();
}

void CompanionWidget::applyCellSize()
{
    // Whole pixels, with a one-unit safety ring around the exact bounds.
    const QRectF b = poseBounds();
    m_cellSize =
        QSize(int(std::ceil((b.width() + 2) * m_unit)), int(std::ceil((b.height() + 2) * m_unit)));
    m_feet = QPointF((1 - b.left()) * m_unit, (1 - b.top()) * m_unit);
    m_cell = m_cellSize.height();
    setFixedSize(m_cellSize);
}

QPointF CompanionWidget::feetPoint() const
{
    return m_feet;
}

void CompanionWidget::setPlacement(QPointF fraction)
{
    m_placement = {std::clamp(fraction.x(), 0.0, 1.0), std::clamp(fraction.y(), 0.0, 1.0)};
    relayout();
}

void CompanionWidget::relayout()
{
    QWidget *host = parentWidget();
    if (!host)
        return;
    const int freeW = std::max(host->width() - width(), 0);
    const int freeH = std::max(host->height() - height(), 0);
    move(qRound(m_placement.x() * freeW), qRound(m_placement.y() * freeH));
}

bool CompanionWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget()) {
        if (event->type() == QEvent::Resize)
            relayout();
        else if (event->type() == QEvent::WindowStateChange)
            updateTimers(); // no frames while minimized
    }
    return QWidget::eventFilter(watched, event);
}

void CompanionWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_dragging = true;
    m_dragGrab = event->pos();
    setCursor(Qt::ClosedHandCursor);
    raise();
    event->accept();
}

void CompanionWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    QWidget *host = parentWidget();
    if (!host)
        return;
    const QPoint target = host->mapFromGlobal(event->globalPosition().toPoint()) - m_dragGrab;
    const int freeW = std::max(host->width() - width(), 0);
    const int freeH = std::max(host->height() - height(), 0);
    const QPoint clamped(std::clamp(target.x(), 0, freeW), std::clamp(target.y(), 0, freeH));
    move(clamped);
    // Track the fraction live so a resize mid-drag keeps the same spot.
    m_placement = {freeW > 0 ? double(clamped.x()) / freeW : 1.0,
                   freeH > 0 ? double(clamped.y()) / freeH : 0.0};
    event->accept();
}

void CompanionWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_dragging) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    m_dragging = false;
    setCursor(Qt::OpenHandCursor);
    emit placementChanged(m_placement);
    event->accept();
}

void CompanionWidget::sync(double bar, int beatsPerBar, double beatsPerMinute, int activePcm,
                           int maxPcm, int activeCgb)
{
    // Group fast meters into felt beats: threes for compound meters (6/8,
    // 9/8, 12/8), otherwise twos, until the step rate is danceable.
    int stepsPerBar = std::max(beatsPerBar, 1);
    double stepsPerMinute = beatsPerMinute;
    while (stepsPerMinute > kMaxStepBpm && stepsPerBar > 1) {
        const int group = (stepsPerBar % 3 == 0 && stepsPerBar > 3) ? 3
                          : (stepsPerBar % 2 == 0)                  ? 2
                                                                    : stepsPerBar;
        stepsPerBar /= group;
        stepsPerMinute /= group;
    }
    // The internal clock counts felt beats of the current meter; a meter
    // change re-bases it at the next sync, which is far finer than a beat.
    m_beatsPerBar = stepsPerBar;
    m_beatAtSync = bar * m_beatsPerBar;
    m_bpm = std::clamp(stepsPerMinute, kMinBpm, kMaxBpm);
    m_sinceSync.restart();
    m_barLoadSum += activePcm + activeCgb;
    m_barPcmSum += activePcm;
    m_barSyncs++;
    m_maxPcm = std::max(maxPcm, 1);
}

void CompanionWidget::setPlaying(bool playing)
{
    if (m_playing == playing)
        return;
    m_playing = playing;
    if (!playing) {
        m_spinStartBeat = -1.0;
        m_spinCued = false;
    } else {
        m_moveSince.restart();
    }
    updateTimers();
    update();
}

void CompanionWidget::cueSpin()
{
    m_spinCued = true;
}

double CompanionWidget::beatNow() const
{
    return m_beatAtSync + m_sinceSync.elapsed() / 1000.0 * m_bpm / 60.0;
}

void CompanionWidget::updateTimers()
{
    const QWidget *top = window();
    const bool onScreen = isVisible() && !(top && top->isMinimized());
    if (onScreen && m_playing) {
        if (!m_frameTimer->isActive())
            m_frameTimer->start();
    } else {
        m_frameTimer->stop();
    }
}

CompanionWidget::Move CompanionWidget::chooseMove(double bar) const
{
    // Every move that fits the last bar's situation is a candidate, and
    // Groove always fits. The situation is judged on the bar's AVERAGE
    // channel load (a peak is hit by nearly every song for one beat, which
    // is how the critter used to headbang all day). Among the candidates
    // the current move is skipped when there is any alternative, and the
    // one danced least recently wins — so a loud, fast song rotates through
    // Headbang, Shimmy and Groove instead of parking on one.
    const double load = m_barSyncs > 0 ? m_barLoadSum / m_barSyncs : 0.0;
    const double pcmShare = m_barSyncs > 0 ? m_barPcmSum / m_barSyncs / m_maxPcm : 0.0;

    std::array<Move, 4> candidates{};
    size_t n = 0;
    candidates[n++] = Move::Groove;
    if (m_maxPcm >= 2 && pcmShare >= kHeadbangLoad)
        candidates[n++] = Move::Headbang;
    if (m_bpm >= kShimmyBpm)
        candidates[n++] = Move::Shimmy;
    if (m_bpm < kSwayBpm || load <= kSwayMaxLoad)
        candidates[n++] = Move::Sway;
    // A moderate song fits only Groove; rather than groove for the whole
    // song, slip in a flourish between grooves, alternating Sway and
    // Shimmy (only ever offered while grooving, so Groove stays half).
    if (n == 1 && m_move == Move::Groove) {
        const bool swayIsOlder = m_lastDanced[static_cast<size_t>(Move::Sway)] <=
                                 m_lastDanced[static_cast<size_t>(Move::Shimmy)];
        candidates[n++] = swayIsOlder ? Move::Sway : Move::Shimmy;
    }

    // Any alternative beats staying put: ages can be negative right after
    // a backwards jump (see frame's re-anchoring), and must still win.
    Move best = m_move;
    double bestAge = -std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < n; i++) {
        const Move m = candidates[i];
        if (m == m_move && n > 1)
            continue;
        const double age = bar - m_lastDanced[static_cast<size_t>(m)];
        if (age > bestAge) {
            bestAge = age;
            best = m;
        }
    }
    return best;
}

void CompanionWidget::startMove(Move move, double bar)
{
    m_move = move;
    m_moveSinceBar = bar;
    m_moveSince.restart();
    m_lastDanced[static_cast<size_t>(move)] = bar;
    // Vary the hold so phrases don't all switch on the same grid: 2, 3 or 4
    // bars, walking with the bar index.
    m_holdBars = kMinHoldBars + std::fmod(std::fabs(bar), kMaxHoldBars - kMinHoldBars + 1.0);
}

void CompanionWidget::rotateMove(double bar)
{
    const Move next = chooseMove(bar);
    if (next != m_move) {
        startMove(next, bar);
        return;
    }
    // Nothing else fits right now (only possible when the sole candidate is
    // Groove while grooving is impossible by construction, but stay safe):
    // restart the clock so the cap is checked again after another hold.
    m_moveSinceBar = bar;
    m_moveSince.restart();
}

void CompanionWidget::onBarLine(double bar)
{
    if (m_spinStartBeat < 0 && bar - m_moveSinceBar >= m_holdBars)
        rotateMove(bar);
    m_barLoadSum = 0.0;
    m_barPcmSum = 0.0;
    m_barSyncs = 0;
}

void CompanionWidget::frame()
{
    if (window()->isMinimized())
        return;
    const double beat = beatNow();
    const double bar = std::floor(beat / m_beatsPerBar);

    // The beat clock is not monotonic: a loop wrap or a seek moves it
    // anywhere. Anything anchored to a beat must be re-anchored, or a
    // pending Spin whose start now lies in the future would park the
    // dance (and block rotation) until the song plays back up to it.
    if (m_spinStartBeat >= 0 &&
        (beat < m_spinStartBeat - 1e-6 || beat > m_spinStartBeat + kSpinBeats + m_beatsPerBar)) {
        m_spinStartBeat = -1.0;
        m_spinCued = true; // still owed: it starts on the next downbeat here
    }
    if (bar < m_moveSinceBar)
        m_moveSinceBar = bar;
    for (double &danced : m_lastDanced)
        danced = std::min(danced, bar); // "last danced" can't lie in the future

    // A cued Spin starts on the next downbeat and runs for one bar; while
    // it runs, the regular move is parked.
    if (m_spinCued && m_spinStartBeat < 0) {
        m_spinCued = false;
        m_spinStartBeat = std::ceil(beat - 1e-6);
        m_moveSinceBar = -1e9; // free to re-evaluate right after the spin
    }
    if (m_spinStartBeat >= 0 && beat >= m_spinStartBeat + kSpinBeats) {
        m_spinStartBeat = -1.0;
        startMove(chooseMove(bar), bar);
    }
    if (bar != m_lastBar) {
        // A seek backwards (or a loop wrap) also lands here; the load
        // counters just restart for the new bar.
        if (m_lastBar >= 0)
            onBarLine(bar);
        m_lastBar = bar;
    }
    // Hard cap: whatever the bar arithmetic says, one move never outlives
    // kMaxHoldSeconds (slow songs) or kMaxHoldBars. Applied on a beat line
    // so the change lands on the music, not mid-gesture.
    const double beatLine = std::floor(beat);
    if (beatLine != m_lastBeatLine) {
        m_lastBeatLine = beatLine;
        const bool overtime =
            m_moveSince.isValid() && m_moveSince.elapsed() >= qint64(kMaxHoldSeconds * 1000.0);
        if (m_spinStartBeat < 0 && (overtime || bar - m_moveSinceBar >= kMaxHoldBars))
            rotateMove(bar);
    }
    update();
}

void CompanionWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    relayout();
    raise();
    updateTimers();
}

void CompanionWidget::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    updateTimers();
}

void CompanionWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
}

bool CompanionWidget::setCustomImage(const QString &path)
{
    QImageReader reader(path.isEmpty() ? QString::fromLatin1(kDefaultImage) : path);
    reader.setAutoTransform(true); // honour EXIF orientation
    QImage image = reader.read();
    if (image.isNull())
        return false;
    m_imageIsPixelArt = std::max(image.width(), image.height()) <= kPixelArtMaxSide;
    if (std::max(image.width(), image.height()) > kMaxSourceSide)
        image = image.scaled(kMaxSourceSide, kMaxSourceSide, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    m_image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    m_customPath = path;
    m_rasterDpr = 0.0;
    update();
    return true;
}

void CompanionWidget::rasterizeSprite()
{
    const qreal dpr = devicePixelRatioF();
    // Fit into the sprite box, standing on its bottom edge, centred.
    const int box = kSpriteSize * m_unit;
    const QSize fitted = m_image.size().scaled(box, box, Qt::KeepAspectRatio);
    const QSize devFitted = fitted * dpr;
    QImage scaled =
        m_image.scaled(devFitted.isEmpty() ? QSize(1, 1) : devFitted, Qt::IgnoreAspectRatio,
                       m_imageIsPixelArt ? Qt::FastTransformation : Qt::SmoothTransformation);
    // The scaled image holds device pixels; tag it so the logical-unit
    // painter below doesn't draw it dpr× too large on HiDPI.
    scaled.setDevicePixelRatio(dpr);
    QPixmap pixmap(QSize(box, box) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.drawImage(
        QRectF(QPointF((box - fitted.width()) / 2.0, box - fitted.height()), QSizeF(fitted)),
        scaled.isNull() ? m_image : scaled);
    painter.end();
    m_sprite = pixmap;
    m_rasterDpr = dpr;
}

void CompanionWidget::paintEvent(QPaintEvent *)
{
    if (m_rasterDpr != devicePixelRatioF())
        rasterizeSprite();

    QPainter painter(this);
    const double beat = m_playing ? beatNow() : 0.0;
    const QTransform xf =
        poseTransform(m_move, m_playing, beat, m_beatsPerBar, m_spinStartBeat, m_unit) *
        QTransform::fromTranslate(feetPoint().x(), feetPoint().y());
    painter.setRenderHint(QPainter::SmoothPixmapTransform, !m_imageIsPixelArt);
    painter.setTransform(xf);
    painter.drawPixmap(QPointF(0, 0), m_sprite);
}
