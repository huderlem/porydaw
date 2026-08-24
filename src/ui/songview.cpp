#include "songview.h"
#include "layout.h"
#include "theme/color_math.h"
#include "theme/themeruntime.h"
#include "theme/trackidentitycolors.h"
#include "typography.h"
#include "ui/contextmenu.h"
#include "ui/layout.h"
#include "ui/timelinesurface.h"
#include "ui/velocityaxis.h"
#include "ui/velocitygesturemodel.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QObject>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <utility>

#include <optional>

#include "core/mid2agbtables.h"
#include "core/songdocument.h"
#include "ui/eventlistview.h"
#include "ui/keymap.h"
#include "ui/playheadoverlay.h"

namespace lyt = ::layout;
using Space = lyt::Space;

namespace songview {

namespace {

// The ruler stacks a marker row (time-signature chips, loop brackets,
// selection handles) above the bar-number/tick row, so marker text never
// collides with the bar numbers.
// Pencil-key releases at or past this hold time (or after a lane gesture
// during the hold) revert the mode: hold-to-draw instead of a sticky tap.
constexpr auto kPencilMomentaryHold = std::chrono::milliseconds(500);

constexpr int kLaneH = 48; // default row height; Ctrl+wheel rescales
constexpr int kMinLaneH = 28;
constexpr int kMaxLaneH = 128;
constexpr int kAddLaneH = 20;
constexpr int kLanesAreaH = 150;
constexpr int kVelLaneH = 120;   // initial velocity-lane pane height
constexpr int kVelLaneMinH = 56; // enough for the sparsest ruler band
constexpr double kMinPxPerBeat = 4.0;
constexpr double kMaxPxPerBeat = 640.0;
constexpr double kMinKeyHeight = 4.0;
constexpr double kMaxKeyHeight = 32.0;
constexpr int kScrollUnitsPerDip = 16;

qreal logicalPhysicalPixel(qreal dpr)
{
    return dpr > 0.0 ? 1.0 / dpr : 1.0;
}

int scrollUnits(double dip)
{
    // Negative units carry the pre-roll pad (scroll positions left of
    // tick 0) into the scrollbar's range.
    const double units = std::clamp(dip * kScrollUnitsPerDip, double(INT_MIN), double(INT_MAX));
    return int(std::lround(units));
}

double scrollDips(int units)
{
    return double(units) / kScrollUnitsPerDip;
}

QPoint wheelDelta(const QWheelEvent *event)
{
    const QPoint pixelDelta = event->pixelDelta();
    return pixelDelta.isNull() ? event->angleDelta() : pixelDelta;
}

double wheelAngleUnits(const QWheelEvent *event)
{
    if (event->phase() == Qt::ScrollMomentum)
        return 0.0;
    const QPoint delta = wheelDelta(event);
    return double(delta.y()) * (event->pixelDelta().isNull() ? 1.0 : 5.0);
}

double cursorAnchoredScroll(double anchor, double oldScale, double oldScroll, double newScale)
{
    const double content = (anchor + oldScroll) / oldScale;
    return content * newScale - anchor;
}
constexpr int kVoiceAuditionKey = 60; // middle C, matching the voicegroup browser
constexpr int kVoiceAuditionVel = 112;
// Resize hit-zone reach at a note's left/right edges (rollcheck probes
// 2.8 DIPs inside the ends, so the zone must reach past that). Outside the
// note the full reach always applies; inside, both zones shrink to leave at
// least kMoveZoneMin between them so short notes keep a grabbable middle
// for move drags.
constexpr qreal kEdgeGripReach = 3.0;
constexpr qreal kMoveZoneMin = 6.0;
qreal edgeGripInnerReach(const QRectF &noteRect)
{
    return std::clamp((noteRect.width() - kMoveZoneMin) / 2.0, 0.0, kEdgeGripReach);
}

bool isBlackKey(int key)
{
    switch (key % 12) {
    case 1:
    case 3:
    case 6:
    case 8:
    case 10:
        return true;
    default:
        return false;
    }
}

QString keyName(int key)
{
    static const char *const names[] = {"C",  "C#", "D",  "D#", "E",  "F",
                                        "F#", "G",  "G#", "A",  "A#", "B"};
    return QStringLiteral("%1%2").arg(QLatin1String(names[key % 12])).arg(key / 12 - 1);
}

QString timeSigLabel(int numerator, int denomPow2)
{
    return QStringLiteral("%1/%2").arg(numerator).arg(1 << std::min(denomPow2, 6));
}

// Modal numerator/denominator editor for a ruler time-signature marker.
bool askTimeSignature(QWidget *parent, int *numerator, int *denomPow2)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(SongView::tr("Time Signature"));
    auto *num = new QSpinBox(&dlg);
    num->setRange(1, 32);
    num->setValue(std::clamp(*numerator, 1, 32));
    auto *den = new QComboBox(&dlg);
    for (int p = 0; p <= 5; p++)
        den->addItem(QString::number(1 << p), p);
    den->setCurrentIndex(std::clamp(*denomPow2, 0, 5));
    auto *row = new QHBoxLayout;
    row->addWidget(num);
    row->addWidget(new QLabel(QStringLiteral("/"), &dlg));
    row->addWidget(den);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *layout = new QVBoxLayout(&dlg);
    layout->addLayout(row);
    layout->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted)
        return false;
    *numerator = num->value();
    *denomPow2 = den->currentData().toInt();
    return true;
}

// SongView paint paths request canvas-specific roles directly, making each
// visible element traceable without knowing a shared theme alias.
QLinearGradient loopGlow(qreal edgeX, qreal transparentX)
{
    auto color = themes::color(themes::Role::song_view_loop_marker);
    auto transparent = color;
    color.setAlpha(150);
    transparent.setAlpha(0);
    QLinearGradient gradient(edgeX, 0, transparentX, 0);
    gradient.setColorAt(0.0, color);
    color.setAlpha(18);
    gradient.setColorAt(0.2, color);
    gradient.setColorAt(1.0, transparent);
    return gradient;
}
QColor loopEdge()
{
    return themes::color(themes::Role::song_view_loop_marker);
}

QColor pianoRollAccidentalLaneColor()
{
    return themes::color(themes::Role::song_view_piano_roll_accidental_lane);
}
QColor trackHeaderAlsoSelectedColor()
{
    auto color = themes::color(themes::Role::song_view_track_header_selection);
    color.setAlpha(99);
    return color;
}
// Perceptual blend for receding a color into its backdrop (silent-in-game
// track headers): t = 0 keeps `color`, t = 1 lands on `backdrop`.
QColor mixTowardOklab(const QColor &color, const QColor &backdrop, double t)
{
    const themes::Oklab from = themes::oklabFromColor(color);
    const themes::Oklab to = themes::oklabFromColor(backdrop);
    return themes::colorFromOklab({from.lightness + (to.lightness - from.lightness) * t,
                                   from.a + (to.a - from.a) * t, from.b + (to.b - from.b) * t});
}
std::size_t trackIdentityIndex(int track)
{
    const auto count = static_cast<int>(themes::trackIdentityColorCount);
    return static_cast<std::size_t>(((track % count) + count) % count);
}

// The higher-contrast piano-key color over a note fill.
QColor contrastingTextColor(const QColor &backdrop)
{
    const auto light = themes::color(themes::Role::song_view_piano_keyboard_natural_key);
    const auto dark = themes::color(themes::Role::song_view_piano_keyboard_black_key);
    return themes::contrastRatio(backdrop, light) >= themes::contrastRatio(backdrop, dark) ? light
                                                                                           : dark;
}

// Ghost notes (unselected tracks) mix 24% of their track identity into the
// row background in OKLab. Cap only the lightness offset so bright identities
// stay equally recessive on light and dark themes.
QColor ghostNoteColor(int track, bool accidentalRow)
{
    const auto &identity = themes::trackIdentityColor(trackIdentityIndex(track));
    const auto background =
        themes::color(accidentalRow ? themes::Role::song_view_piano_roll_accidental_lane
                                    : themes::Role::song_view_piano_roll_background);
    const auto identityLab = themes::oklabFromColor(identity);
    const auto backgroundLab = themes::oklabFromColor(background);
    constexpr auto kIdentityWeight = 60.0 / 255.0;
    constexpr auto kMaxLightnessOffset = 0.055;
    const auto lightnessOffset =
        std::clamp((identityLab.lightness - backgroundLab.lightness) * kIdentityWeight,
                   -kMaxLightnessOffset, kMaxLightnessOffset);
    return themes::colorFromOklab(
        {backgroundLab.lightness + lightnessOffset,
         backgroundLab.a + (identityLab.a - backgroundLab.a) * kIdentityWeight,
         backgroundLab.b + (identityLab.b - backgroundLab.b) * kIdentityWeight});
}

// Draw the loop-region band across rect. x positions are
// computed with origin = local x of timeline tick 0's content position.
// timeSelCovered says whether this widget (or row) is inside the active time
// selection's scope, so the selection band tints exactly the covered content.
void drawOverlays(QPainter &p, const SongView *sv, const QRect &rect, qreal origin,
                  bool timeSelCovered)
{
    const MidiTimeline *tl = sv->timeline();
    if (!tl)
        return;

    const qreal dpr = p.device()->devicePixelRatioF();
    const SongView::TimeSelection &tsel = sv->timeSelection();
    if (timeSelCovered && tsel.active()) {
        const qreal x0 = sv->displayX(double(tsel.startTick), origin, dpr);
        const qreal x1 = sv->displayX(double(tsel.endTick), origin, dpr);
        if (x1 > rect.left() && x0 < rect.right()) {
            QColor fill = themes::color(themes::Role::song_view_selection_fill);
            fill.setAlpha(30);
            const QRectF selectionRect(x0, rect.top(), x1 - x0, rect.height());
            p.fillRect(selectionRect.intersected(QRectF(rect)), fill);
            p.setPen(QPen(themes::color(themes::Role::song_view_selection_edge), 1));
            p.drawLine(QLineF(x0, rect.top(), x0, rect.bottom()));
            p.drawLine(QLineF(x1, rect.top(), x1, rect.bottom()));
        }
    }
    if (tl->loopStartTick != UINT64_MAX || tl->loopEndTick != UINT64_MAX) {
        const bool hasStart = tl->loopStartTick != UINT64_MAX;
        const bool hasEnd = tl->loopEndTick != UINT64_MAX;
        const qreal x0 =
            hasStart ? sv->displayX(double(tl->loopStartTick), origin, dpr) : rect.left();
        const qreal x1 = hasEnd ? sv->displayX(double(tl->loopEndTick), origin, dpr) : rect.right();
        if (x1 > rect.left() && x0 < rect.right()) {
            const qreal glowWidth = std::min<qreal>(lyt::space(Space::Eight), x1 - x0);
            if (hasStart && glowWidth > 0) {
                const QRectF glowRect(x0, rect.top(), glowWidth, rect.height());
                p.fillRect(glowRect.intersected(QRectF(rect)), loopGlow(x0, x0 + glowWidth));
            }
            if (hasEnd && glowWidth > 0) {
                const QRectF glowRect(x1 - glowWidth, rect.top(), glowWidth, rect.height());
                p.fillRect(glowRect.intersected(QRectF(rect)), loopGlow(x1, x1 - glowWidth));
            }
            p.setPen(QPen(loopEdge(), 1));
            if (hasStart)
                p.drawLine(QLineF(x0, rect.top(), x0, rect.bottom()));
            if (hasEnd)
                p.drawLine(QLineF(x1, rect.top(), x1, rect.bottom()));
        }
    }
    const qreal cursorX = sv->displayX(double(sv->editCursorTick()), origin, dpr);
    if (cursorX >= rect.left() && cursorX <= rect.right()) {
        p.setPen(QPen(themes::color(themes::Role::song_view_edit_cursor), 1, Qt::DashLine));
        p.drawLine(QLineF(cursorX, rect.top(), cursorX, rect.bottom()));
    }
}

// Subdivision level of a sub-beat grid tick (relative to its segment's
// start): 1 = the beat's first split (half beat, or a third in triplet
// feel), 2 = the next, 3 = finer. Cosmetic only (drives the line fade).
int subGridLevel(uint64_t relTick, uint64_t beatTicks, bool triplet)
{
    if (relTick % std::max<uint64_t>(1, beatTicks / (triplet ? 3 : 2)) == 0)
        return 1;
    if (relTick % std::max<uint64_t>(1, beatTicks / (triplet ? 6 : 4)) == 0)
        return 2;
    return 3;
}

// Calls fn(tick, level) for every sub-beat visible-grid position in [t0, t1)
// that is not a beat line, at the current zoom's drawn resolution
// (SongView::gridTicksAt, which bottoms out at the mid2agb clock grid; the
// snap grid runs one ladder step finer between these lines).
// Walks time-signature segments so the positions stay snappable and match
// the beat lines. No callbacks in segments whose grid is at (or coarser
// than) whole beats.
void forEachSubGridLine(const SongView *sv, double t0, double t1,
                        const std::function<void(uint64_t, int)> &fn)
{
    const bool triplet = sv->gridFeel() == SongView::GridFeel::Triplet;
    uint64_t at = uint64_t(std::max(0.0, t0));
    const uint64_t end = t1 <= 0.0 ? 0 : uint64_t(t1);
    while (at < end) {
        const SongView::GridSeg seg = sv->gridSegAt(at);
        const uint64_t segEnd = std::min(seg.next, end);
        const uint64_t g = sv->gridTicksAt(at);
        if (g > 0 && g < seg.beatTicks && sv->pxPerTick() * double(seg.beatTicks) >= 10.0) {
            const uint64_t k = at > seg.start ? (at - seg.start + g - 1) / g : 0;
            for (uint64_t tick = seg.start + k * g; tick < segEnd; tick += g) {
                if ((tick - seg.start) % seg.beatTicks == 0)
                    continue; // beat/bar lines are drawn separately
                fn(tick, subGridLevel(tick - seg.start, seg.beatTicks, triplet));
            }
        }
        if (seg.next >= end)
            break;
        at = seg.next;
    }
}

QColor gridLineColor(int alpha = 255)
{
    auto color = themes::color(themes::Role::song_view_grid);
    color.setAlpha((color.alpha() * alpha + 127) / 255);
    return color;
}

// Flat fill over the camera's pre-roll pad (the scrollable dead space left
// of tick 0). Opaque and stripe-free so it reads as "outside the song";
// blending the surface's own background toward the grid ink dims it in
// light themes and lifts it in dark ones. Painted before drawGrid so the
// tick-0 bar line stays a crisp boundary on top.
void drawPreRoll(QPainter &p, const SongView *sv, const QRect &rect, qreal origin,
                 const QColor &background)
{
    const qreal dpr = p.device()->devicePixelRatioF();
    const qreal x0 = sv->displayX(0.0, origin, dpr);
    if (x0 <= rect.left())
        return;
    p.fillRect(QRectF(rect.left(), rect.top(), x0 - rect.left(), rect.height()),
               mixTowardOklab(background, gridLineColor(), 0.15));
}

// Vertical bar/beat grid lines inside rect, with zoom-adaptive sub-beat
// lines at the snap grid's positions fading lighter per subdivision level.
// Lines are batched per level so each color is a single drawLines() call.
void drawGrid(QPainter &p, const SongView *sv, const QRect &rect, qreal origin)
{
    if (!sv->timeline())
        return;
    const qreal dpr = p.device()->devicePixelRatioF();
    const qreal physicalPixel = logicalPhysicalPixel(dpr);
    const qreal roundingMargin = physicalPixel / 2.0;
    const double t0 = std::max(0.0, sv->tickAtContentX(rect.left() - origin - roundingMargin));
    const double t1 =
        sv->tickAtContentX(rect.x() + rect.width() - physicalPixel - origin + roundingMargin) + 1;
    const bool drawBeats = sv->pxPerBeat() >= 10.0;
    // Batches 0-2 hold sub-grid levels 1-3 (fading lighter), 3 beats, 4
    // finest-grid beats, 5 bars; painted in that order so beats and bars
    // land on top.
    const std::array<QColor, 6> colors = {gridLineColor(125), gridLineColor(100), gridLineColor(75),
                                          gridLineColor(160), gridLineColor(200), gridLineColor()};
    std::array<QVector<QLineF>, 6> batches;
    forEachSubGridLine(sv, t0, t1, [&](uint64_t tick, int level) {
        const qreal x = sv->displayX(double(tick), origin, dpr);
        batches[level - 1].append(QLineF(x, rect.top(), x, rect.bottom()));
    });
    sv->forEachGridLine(uint64_t(t0), uint64_t(t1), [&](uint64_t tick, bool isBar, int, int) {
        if (!isBar && !drawBeats)
            return;
        const qreal x = sv->displayX(double(tick), origin, dpr);
        const bool atFinestGrid = sv->document() && sv->gridTicksAt(tick) == sv->fineGridTicks();
        batches[isBar ? 5 : atFinestGrid ? 4 : 3].append(QLineF(x, rect.top(), x, rect.bottom()));
    });
    for (size_t i = 0; i < batches.size(); ++i) {
        if (batches[i].isEmpty())
            continue;
        // Grid lines are two physical pixels on every display scale.
        p.setPen(QPen(colors[i], 2.0 * physicalPixel));
        p.drawLines(batches[i]);
    }
}

enum class NoteMenuChoice {
    None,
    Velocity,
    Copy,
    Cut,
    Delete,
};
// Kept alive by PianoRoll so opening it does not reconstruct its actions.
// The outside-right-click retarget gesture and the popup style live in the
// shared ui::ContextMenu base.
class NoteContextMenu final : public ui::ContextMenu
{
  public:
    explicit NoteContextMenu(QWidget *parent, std::function<bool(QPointF)> onOutsideRightClick)
        : ui::ContextMenu(parent, std::move(onOutsideRightClick))
    {
        m_velocityAction = addAction(QString());
        addSeparator();
        // Display-only hints (the real bindings live in keyPressEvent):
        // mirror the keymap so a rebind doesn't leave the menu lying.
        const auto &keys = keymap::Registry::instance();
        m_copyAction = addAction(SongView::tr("Copy"));
        m_copyAction->setShortcut(keys.bindings(QStringLiteral("roll.copy")).value(0));
        m_cutAction = addAction(SongView::tr("Cut"));
        m_cutAction->setShortcut(keys.bindings(QStringLiteral("roll.cut")).value(0));
        m_deleteAction = addAction(SongView::tr("Delete"));
    }

    void showMenuAt(QPoint globalPos, int velocity)
    {
        m_velocityAction->setText(SongView::tr("Set velocity… (%1)").arg(velocity));
        popup(globalPos);
    }

    NoteMenuChoice handleAction(QAction *action) const
    {
        if (action == m_velocityAction)
            return NoteMenuChoice::Velocity;
        if (action == m_copyAction)
            return NoteMenuChoice::Copy;
        if (action == m_cutAction)
            return NoteMenuChoice::Cut;
        if (action == m_deleteAction)
            return NoteMenuChoice::Delete;
        return NoteMenuChoice::None;
    }

  private:
    QAction *m_velocityAction = nullptr;
    QAction *m_copyAction = nullptr;
    QAction *m_cutAction = nullptr;
    QAction *m_deleteAction = nullptr;
};
QFont timeRulerFont(const QFont &source)
{
    auto font = typography::bodyMono(typography::caption(source));
    font.setPixelSize(std::max(1, font.pixelSize() - lyt::singlePixel()));
    font.setLetterSpacing(QFont::AbsoluteSpacing, -0.5);
    return font;
}
// "bar.beat" labels sit one size below the bar numbers so the two label
// tiers read apart even where the deemphasized color alone wouldn't.
QFont beatRulerFont(const QFont &source)
{
    auto font = timeRulerFont(source);
    font.setPixelSize(std::max(1, font.pixelSize() - lyt::singlePixel()));
    return font;
}

std::optional<QFont> velocityLabelFont(const QFont &source, int availableHeight)
{
    auto font = typography::fitted(source, availableHeight);
    if (font)
        font->setPixelSize(std::max(lyt::singlePixel(), font->pixelSize() - lyt::singlePixel()));
    return font;
}
// Note text sits on a plate of the note's own fill: the velocity bar can
// cross the text rows, and both the bar and a dark picked ink derive from
// the fill, so glyphs painted straight over the bar lose their contrast.
// The plate is a no-op wherever the backdrop is already the plain fill.
void drawPlatedNoteText(QPainter &painter, const QRectF &rect, int flags, const QString &text,
                        const QColor &fill, const QColor &ink)
{
    const QRectF plate = painter.boundingRect(rect, flags, text);
    const qreal pad = lyt::singlePixel();
    painter.fillRect(plate.adjusted(-pad, 0.0, pad, 0.0), fill);
    painter.setPen(ink);
    painter.drawText(rect, flags, text);
}

QFont fixedNoteNameFont(const QFont &source)
{
    auto font = typography::noteName(source);
    font.setPixelSize(std::max(lyt::singlePixel(), font.pixelSize() - 2 * lyt::singlePixel()));
    return font;
}

std::optional<QFont> noteNameFont(const QFont &source, qreal noteBoxHeight)
{
    const auto textHeight = int(std::floor(noteBoxHeight - 2.0 * lyt::space(Space::Half)));
    const auto font = fixedNoteNameFont(source);
    // The face is fixed: when its padded height misses the row, labels hide
    // rather than shrink.
    const QFontMetrics metrics(font);
    if (metrics.ascent() + metrics.descent() > textHeight)
        return std::nullopt;
    return font;
}

} // namespace

// ---------------------------------------------------------------- TimeRuler

class TimeRuler : public QWidget
{
  public:
    explicit TimeRuler(SongView *sv) : QWidget(sv), m_sv(sv)
    {
        // The ruler has a bold marker row and a regular bar-number/tick row.
        // Each row reserves its face's occupied glyph height plus semantic
        // padding, so a narrow ruler never clips text or relies on pixels.
        const auto markerRowPadding = lyt::singlePixel();
        const auto tickRowPadding = lyt::singlePixel();
        const auto rulerFont = timeRulerFont(font());
        const QFontMetrics markerMetrics(typography::bold(rulerFont));
        const QFontMetrics tickMetrics(rulerFont);
        m_markerHeight = markerMetrics.height() + markerRowPadding;
        const auto rulerHeight = m_markerHeight + tickMetrics.height() + tickRowPadding;
        setFixedHeight(rulerHeight);
        setMouseTracking(true);

        // Snap-grid controls in the gutter left of the timeline: minimum
        // subdivision (Auto = zoom-adaptive down to the clock grid) and
        // straight-vs-triplet feel. NoFocus for the same reason the scroll
        // areas have it: keyboard editing must stay in the roll.
        auto *gridBox = new QWidget(this);
        gridBox->setGeometry(0, 0, kGutterW - lyt::space(Space::One), rulerHeight);
        auto *row = new QHBoxLayout(gridBox);
        row->setContentsMargins(lyt::space(Space::Two), 0, 0, 0);
        row->setSpacing(lyt::space(Space::One));
        row->addWidget(new QLabel(SongView::tr("Grid"), gridBox));
        m_divCombo = new QComboBox(gridBox);
        m_divCombo->addItem(SongView::tr("Auto"), 0);
        for (int denom : {4, 8, 16, 32})
            m_divCombo->addItem(QStringLiteral("1/%1").arg(denom), denom);
        m_divCombo->setToolTip(
            SongView::tr("Finest drawn subdivision. Auto follows the zoom down to "
                         "the mid2agb clock grid; edits snap one step finer than "
                         "the drawn grid."));
        m_feelCombo = new QComboBox(gridBox);
        m_feelCombo->addItem(SongView::tr("Straight"));
        m_feelCombo->addItem(SongView::tr("Triplet"));
        m_feelCombo->setToolTip(SongView::tr("Straight or triplet beat subdivisions."));
        for (QComboBox *combo : {m_divCombo, m_feelCombo}) {
            combo->setFocusPolicy(Qt::NoFocus);
            row->addWidget(combo);
        }
        row->addStretch(1);
        QObject::connect(m_divCombo, &QComboBox::activated, m_sv, [this](int index) {
            m_sv->setGridMinDenom(m_divCombo->itemData(index).toInt());
        });
        QObject::connect(m_feelCombo, &QComboBox::activated, m_sv, [this](int index) {
            m_sv->setGridFeel(index == 1 ? SongView::GridFeel::Triplet
                                         : SongView::GridFeel::Straight);
        });
    }

    // Combo state from the view (setters, setSong reset, sidecar apply);
    // setCurrentIndex is safe because the handlers hang off activated(),
    // which only fires on user picks.
    void syncGridControls()
    {
        m_divCombo->setCurrentIndex(std::max(0, m_divCombo->findData(m_sv->gridMinDenom())));
        m_feelCombo->setCurrentIndex(m_sv->gridFeel() == SongView::GridFeel::Triplet ? 1 : 0);
    }

    // A mouse gesture is live (marker/time-sig/selection-edge drag, cursor
    // scrub, right-press sweep); the playhead follow-scroll pauses while
    // one runs so the view doesn't jump under the cursor.
    bool gestureActive() const
    {
        return m_dragMarker >= 0 || m_dragTimeSig || m_placingCursor || m_rightPress ||
               m_dragSelEdge >= 0;
    }

  protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const qreal dpr = p.device()->devicePixelRatioF();
        const QFont rulerFont = timeRulerFont(p.font());
        const QFont beatFont = beatRulerFont(p.font());
        p.setFont(rulerFont);
        const QColor chrome = themes::color(themes::Role::song_view_timeline_chrome_background);
        p.fillRect(rect(), chrome);
        p.setPen(QPen(themes::color(themes::Role::song_view_separator), lyt::singlePixel()));
        p.drawLine(0, rect().bottom(), width(), rect().bottom());

        if (!m_sv->timeline()) {
            p.setPen(palette().color(QPalette::PlaceholderText));
            p.drawText(rect().adjusted(kGutterW + lyt::space(Space::Two), 0, 0, 0),
                       Qt::AlignVCenter,
                       SongView::tr("No song loaded — double-click a song in the browser."));
            return;
        }

        const QRect area(kGutterW, 0, width() - kGutterW, height());
        p.setClipRect(area);
        drawPreRoll(p, m_sv, area, kGutterW, chrome);

        // Loop band across the whole ruler height.
        drawOverlays(p, m_sv, area, kGutterW, true);

        const qreal physicalPixel = logicalPhysicalPixel(dpr);
        const qreal roundingMargin = physicalPixel / 2.0;
        const double t0 =
            std::max(0.0, m_sv->tickAtContentX(area.left() - kGutterW - roundingMargin));
        const double t1 = m_sv->tickAtContentX(area.x() + area.width() - physicalPixel - kGutterW +
                                               roundingMargin) +
                          1;
        const auto indicatorColor = gridLineColor();
        // Beat labels recede a step past secondary text: blended a quarter of
        // the way into the ruler chrome so they read as texture next to the
        // bar numbers, in every theme.
        const QColor secondary = themes::color(themes::Role::song_view_secondary_text);
        const auto recede = [&](int fg, int bg) { return (191 * fg + 64 * bg + 127) / 255; };
        const QColor textColor(recede(secondary.red(), chrome.red()),
                               recede(secondary.green(), chrome.green()),
                               recede(secondary.blue(), chrome.blue()));

        const QRect ticks = tickRow();
        const int tickBottom = ticks.bottom();
        const QFontMetrics tickMetrics(p.font());
        const QFontMetrics beatMetrics(beatFont);
        const int tickBaseline = ticks.top() + tickMetrics.ascent();
        const auto barCapWidth = lyt::space(Space::Half);
        const auto indicatorRise = lyt::space(Space::Half);
        const auto labelGap = lyt::singlePixel();
        const auto beatDetailReserve = lyt::space(Space::Two);
        const bool drawBeatTicks = m_sv->pxPerBeat() >= 10.0;

        // Short sub-beat ticks at the snap grid, mirroring the roll's grid.
        p.setPen(indicatorColor);
        forEachSubGridLine(m_sv, t0, t1, [&](uint64_t tick, int level) {
            const qreal x = m_sv->displayX(double(tick), kGutterW, dpr);
            const int tickHeight = level == 1 ? lyt::space(Space::Half) : lyt::singlePixel();
            p.drawLine(QLineF(x, tickBottom - tickHeight + lyt::singlePixel(), x, tickBottom));
        });

        // Bar numbers are the primary labels; the in-between beats only earn
        // "bar.beat" labels once a beat spans several label-widths, so the
        // ruler stays sparse until the zoom genuinely has room for detail.
        // One decision per paint, sized to the widest label in view, so a
        // ruler never shows some beat labels while suppressing others.
        const QColor barTextColor = themes::color(themes::Role::song_view_primary_text);
        const double beatLabelZoomFactor = 3.0;
        int widestDetailWidth = 0;
        m_sv->forEachGridLine(
            uint64_t(t0), uint64_t(t1), [&](uint64_t, bool, int barNumber, int beatNumber) {
                widestDetailWidth = std::max(
                    widestDetailWidth, beatMetrics.horizontalAdvance(
                                           QStringLiteral("%1.%2").arg(barNumber).arg(beatNumber)));
            });
        const bool showBeatLabels =
            m_sv->pxPerBeat() >= beatLabelZoomFactor * (barCapWidth + 2 * labelGap +
                                                        beatDetailReserve + widestDetailWidth);
        qreal lastLabelRight = area.left() - labelGap;
        m_sv->forEachGridLine(
            uint64_t(t0), uint64_t(t1),
            [&](uint64_t tick, bool isBar, int barNumber, int beatNumber) {
                const qreal x = m_sv->displayX(double(tick), kGutterW, dpr);
                const auto detailedLabel = QStringLiteral("%1.%2").arg(barNumber).arg(beatNumber);
                if (!isBar && !showBeatLabels) {
                    if (drawBeatTicks) {
                        p.setPen(indicatorColor);
                        p.drawLine(QLineF(x, ticks.center().y() - indicatorRise, x, tickBottom));
                    }
                    return;
                }
                const auto label = isBar ? QString::number(barNumber) : detailedLabel;
                const int labelWidth = (isBar ? tickMetrics : beatMetrics).horizontalAdvance(label);
                const qreal labelX = x + barCapWidth;
                if (labelX < lastLabelRight + labelGap) {
                    if (!isBar && drawBeatTicks) {
                        p.setPen(indicatorColor);
                        p.drawLine(QLineF(x, ticks.center().y() - indicatorRise, x, tickBottom));
                    }
                    return;
                }
                p.setPen(indicatorColor);
                if (isBar) {
                    const int indicatorTop = ticks.top() - indicatorRise;
                    p.drawLine(QLineF(x, indicatorTop, x, tickBottom));
                    p.drawLine(QLineF(x, indicatorTop, x + barCapWidth, indicatorTop));
                } else {
                    p.drawLine(QLineF(x, ticks.center().y() - indicatorRise, x, tickBottom));
                }
                p.setPen(isBar ? barTextColor : textColor);
                p.setFont(isBar ? rulerFont : beatFont);
                p.drawText(QPointF(labelX, tickBaseline), label);
                p.setFont(rulerFont);
                lastLabelRight = labelX + labelWidth;
            });

        const MidiTimeline *tl = m_sv->timeline();
        p.setFont(typography::bold(rulerFont));

        const QRect markers = markerRow();
        const int markerBaseline = textBaseline(markers, p.fontMetrics());

        // Time-signature chips in the marker row; a placeholder 4/4 shows
        // at tick 0 while no 0x58 meta governs the opening bars.
        for (const SigChip &chip : sigChips()) {
            if (chip.x > area.right() || chip.labelX + chip.labelW < area.left())
                continue;
            p.setPen(
                palette().color(chip.implicit ? QPalette::PlaceholderText : QPalette::WindowText));
            p.drawLine(QLineF(chip.x, markers.top(), chip.x, markers.bottom()));
            if (chip.labelW > 0)
                p.drawText(QPointF(chip.labelX, markerBaseline),
                           timeSigLabel(chip.numerator, chip.denomPow2));
        }

        // Loop bracket glyphs above the band edges.
        p.setPen(loopEdge());
        if (tl->loopStartTick != UINT64_MAX) {
            const qreal x =
                m_sv->displayX(double(tl->loopStartTick), kGutterW, dpr) + lyt::space(Space::Half);
            p.drawText(QPointF(x, markerBaseline), QStringLiteral("["));
        }
        if (tl->loopEndTick != UINT64_MAX) {
            const qreal x =
                m_sv->displayX(double(tl->loopEndTick), kGutterW, dpr) + lyt::space(Space::Half);
            p.drawText(QPointF(x, markerBaseline), QStringLiteral("]"));
        }

        // Marker / time-signature drag preview.
        const auto markerStroke = lyt::space(Space::Half);
        if (m_dragMarker >= 0 || m_dragTimeSig) {
            const qreal x = m_sv->displayX(double(m_dragTick), kGutterW, dpr);
            p.setPen(QPen(m_dragMarker >= 0 ? loopEdge() : palette().color(QPalette::WindowText),
                          markerStroke));
            p.drawLine(QLineF(x, 0, x, height()));
        }

        // Time-selection edge handles (the 1px band edges come from
        // drawOverlays); the marker row is their grab zone, while the tick
        // row stays scrub territory.
        const SongView::TimeSelection &tsel = m_sv->timeSelection();
        if (tsel.active()) {
            p.setPen(QPen(themes::color(themes::Role::song_view_selection_edge), markerStroke));
            const qreal sx0 = m_sv->displayX(double(tsel.startTick), kGutterW, dpr);
            const qreal sx1 = m_sv->displayX(double(tsel.endTick), kGutterW, dpr);
            p.drawLine(QLineF(sx0, markers.top(), sx0, markers.bottom()));
            p.drawLine(QLineF(sx1, markers.top(), sx1, markers.bottom()));
        }
    }

    void wheelEvent(QWheelEvent *event) override
    {
        // Same bindings as the roll's notes area: plain wheel zooms the
        // timeline; Shift (or a trackpad's horizontal delta) scrolls it.
        const QPoint delta = wheelDelta(event);
        if (event->modifiers() & Qt::ShiftModifier) {
            m_sv->scrollByPx(-(delta.y() ? delta.y() : delta.x()));
        } else if (delta.x() && !delta.y()) {
            m_sv->scrollByPx(-delta.x());
        } else {
            const double zoomDelta = wheelAngleUnits(event);
            if (zoomDelta != 0.0)
                m_sv->zoomAroundContentX(std::pow(1.0015, zoomDelta),
                                         event->position().x() - kGutterW);
        }
        event->accept();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        SongDocument *doc = m_sv->document();
        const MidiTimeline *tl = m_sv->timeline();
        if (!tl || event->position().x() < kGutterW)
            return;
        const uint64_t clickTick =
            m_sv->snapTick(m_sv->tickAtContentX(event->position().x() - kGutterW));

        if (event->button() == Qt::RightButton) {
            // Deferred: a drag from here sweeps out a time selection;
            // releasing in place opens the loop/selection menu. Resolved in
            // mouseReleaseEvent.
            if (!doc)
                return;
            m_rightPress = true;
            m_rightPressPos = event->position();
            m_selAnchor = clickTick;
            return;
        }
        if (event->button() != Qt::LeftButton)
            return;
        m_dragMarker = doc ? hitMarker(event->position()) : -1;
        if (m_dragMarker >= 0) {
            m_dragTick = clickTick;
            update();
            return;
        }
        uint64_t sigTick;
        int sigNum, sigDen;
        bool sigImplicit;
        if (doc && hitTimeSigChip(event->position(), &sigTick, &sigNum, &sigDen, &sigImplicit) &&
            !sigImplicit) {
            // Drag moves the signature; starting at its own tick keeps a
            // plain click (and the first half of a double-click) a no-op.
            m_dragTimeSig = true;
            m_dragTimeSigFrom = sigTick;
            m_dragTick = sigTick;
            update();
            return;
        }
        m_dragSelEdge = doc ? hitSelEdge(event->position()) : -1;
        if (m_dragSelEdge >= 0)
            return;
        // Elsewhere on the ruler: place the edit cursor (drag scrubs it;
        // playback follows on release).
        m_placingCursor = true;
        m_sv->setEditCursorTick(clickTick);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const auto dragTick = [this, event] {
            return m_sv->snapTick(
                m_sv->tickAtContentX(std::max(qreal(kGutterW), event->position().x()) - kGutterW));
        };
        if (m_rightPress) {
            if (!m_selSweep &&
                (event->position().toPoint() - m_rightPressPos.toPoint()).manhattanLength() >=
                    QApplication::startDragDistance())
                m_selSweep = true;
            if (m_selSweep) {
                const uint64_t tick = dragTick();
                SongView::TimeSelection sel;
                sel.startTick = std::min(m_selAnchor, tick);
                sel.endTick = std::max(m_selAnchor, tick);
                m_sv->setTimeSelection(sel); // scope: the selected tracks
            }
            return;
        }
        if (m_dragMarker >= 0 || m_dragTimeSig) {
            m_dragTick = dragTick();
            update();
            return;
        }
        if (m_dragSelEdge >= 0) {
            // Selection edges move live (view state, unlike the loop
            // markers' commit-on-release document edit).
            SongView::TimeSelection sel = m_sv->timeSelection();
            const uint64_t tick = dragTick();
            if (m_dragSelEdge == 0)
                sel.startTick = tick;
            else
                sel.endTick = tick;
            if (sel.startTick > sel.endTick) {
                std::swap(sel.startTick, sel.endTick);
                m_dragSelEdge ^= 1;
            }
            m_sv->setTimeSelection(sel);
            return;
        }
        if (m_placingCursor) {
            m_sv->setEditCursorTick(dragTick());
            return;
        }
        uint64_t sigTick;
        int sigNum, sigDen;
        bool sigImplicit;
        setCursor(
            m_sv->document() &&
                    (hitMarker(event->position()) >= 0 || hitSelEdge(event->position()) >= 0 ||
                     hitTimeSigChip(event->position(), &sigTick, &sigNum, &sigDen, &sigImplicit))
                ? Qt::SplitHCursor
                : Qt::ArrowCursor);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::RightButton && m_rightPress) {
            m_rightPress = false;
            if (m_selSweep) {
                m_selSweep = false;
                if (m_sv->timeSelection().active())
                    m_sv->announceTimeSelection();
                else
                    m_sv->clearTimeSelection();
            } else {
                showRulerMenu(m_selAnchor, event->globalPosition().toPoint());
            }
            return;
        }
        if (event->button() != Qt::LeftButton)
            return;
        if (m_placingCursor) {
            m_placingCursor = false;
            m_sv->commitEditCursor(m_sv->editCursorTick());
            return;
        }
        if (m_dragSelEdge >= 0) {
            m_dragSelEdge = -1;
            if (m_sv->timeSelection().active())
                m_sv->announceTimeSelection();
            else
                m_sv->clearTimeSelection(); // edges dragged together
            return;
        }
        if (m_dragTimeSig) {
            m_dragTimeSig = false;
            if (SongDocument *doc = m_sv->document())
                doc->moveTimeSig(m_dragTimeSigFrom, m_dragTick);
            update();
            return;
        }
        if (m_dragMarker < 0)
            return;
        const bool endMarker = m_dragMarker == 1;
        m_dragMarker = -1;
        if (SongDocument *doc = m_sv->document())
            doc->setLoopTick(endMarker, int64_t(m_dragTick));
        update();
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        SongDocument *doc = m_sv->document();
        uint64_t sigTick;
        int numerator, denomPow2;
        bool implicit;
        if (event->button() != Qt::LeftButton || !doc ||
            !hitTimeSigChip(event->position(), &sigTick, &numerator, &denomPow2, &implicit))
            return;
        // The first press of the double-click armed a chip drag or cursor
        // placement; cancel it before the modal editor swallows the release.
        m_dragTimeSig = false;
        m_placingCursor = false;
        if (askTimeSignature(this, &numerator, &denomPow2))
            doc->setTimeSig(sigTick, numerator, denomPow2);
        update();
    }

  private:
    QRect markerRow() const { return QRect(0, 0, width(), m_markerHeight); }

    QRect tickRow() const { return QRect(0, m_markerHeight, width(), height() - m_markerHeight); }

    int textBaseline(const QRect &row, const QFontMetrics &metrics) const
    {
        return row.top() + (row.height() - metrics.height()) / 2 + metrics.ascent();
    }

    // Loop-marker and selection-edge grab zones live in the marker row —
    // where the bracket glyphs and edge handles are drawn — so the tick row
    // always scrubs the edit cursor even directly on a marker line.

    // 0 = start marker, 1 = end marker, -1 = neither near pos.
    int hitMarker(QPointF pos) const
    {
        const MidiTimeline *tl = m_sv->timeline();
        if (!tl || !QRectF(markerRow()).contains(pos))
            return -1;
        const auto markerHitHalfWidth = lyt::space(Space::Two);
        const qreal dpr = devicePixelRatioF();
        if (tl->loopStartTick != UINT64_MAX &&
            std::abs(m_sv->displayX(double(tl->loopStartTick), kGutterW, dpr) - pos.x()) <=
                markerHitHalfWidth)
            return 0;
        if (tl->loopEndTick != UINT64_MAX &&
            std::abs(m_sv->displayX(double(tl->loopEndTick), kGutterW, dpr) - pos.x()) <=
                markerHitHalfWidth)
            return 1;
        return -1;
    }

    // One time-signature chip as laid out in the marker row.
    struct SigChip {
        uint64_t tick;
        int numerator;
        int denomPow2;
        bool implicit; // no 0x58 meta behind it (editing one creates the event)
        qreal x;       // stem position (widget coords)
        qreal labelX;  // label left edge, nudged right past a loop bracket
        qreal labelW;  // 0: label hidden behind the next chip (stem only)
    };

    // Chip layout shared by paint and hit-testing: shadowed same-tick
    // duplicates dropped, labels nudged past a loop bracket glyph sitting on
    // the same spot, and a label hidden (stem only) when it would run into
    // the next chip — zooming in separates them again.
    std::vector<SigChip> sigChips() const
    {
        std::vector<SigChip> chips;
        const MidiTimeline *tl = m_sv->timeline();
        if (!tl)
            return chips;
        const qreal dpr = devicePixelRatioF();
        const auto boldFont = typography::bold(font());
        const QFontMetrics fm(boldFont);
        const auto labelInset = lyt::space(Space::Half);
        const auto add = [&](uint64_t tick, int numerator, int denomPow2, bool implicit) {
            const qreal x = m_sv->displayX(double(tick), kGutterW, dpr);
            chips.push_back({tick, numerator, denomPow2, implicit, x, x + labelInset,
                             qreal(fm.horizontalAdvance(timeSigLabel(numerator, denomPow2)))});
        };
        if (tl->timeSigs.empty() || tl->timeSigs.front().tick != 0)
            add(0, 4, 2, true);
        for (size_t i = 0; i < tl->timeSigs.size(); i++) {
            if (i + 1 < tl->timeSigs.size() && tl->timeSigs[i + 1].tick == tl->timeSigs[i].tick)
                continue; // shadowed duplicate: the last at a tick wins
            const TimeSigPoint &ts = tl->timeSigs[i];
            add(ts.tick, ts.numerator ? ts.numerator : 4, ts.denomPow2, false);
        }
        const uint64_t loops[2] = {tl->loopStartTick, tl->loopEndTick};
        const qreal bracketWidth = fm.horizontalAdvance(QStringLiteral("["));
        for (SigChip &chip : chips) {
            for (uint64_t loopTick : loops) {
                if (loopTick == UINT64_MAX)
                    continue;
                const qreal bracketStart =
                    m_sv->displayX(double(loopTick), kGutterW, dpr) + labelInset;
                const qreal bracketRight = bracketStart + bracketWidth;
                if (bracketRight > chip.labelX && bracketStart < chip.labelX + chip.labelW)
                    chip.labelX = bracketRight + labelInset;
            }
        }
        for (size_t i = 0; i + 1 < chips.size(); i++) {
            if (chips[i].labelX + chips[i].labelW + labelInset > chips[i + 1].x)
                chips[i].labelW = 0;
        }
        return chips;
    }

    // Chip hit-test in the ruler's top half, including the placeholder 4/4
    // at tick 0. Fills the chip's tick and values.
    bool hitTimeSigChip(QPointF pos, uint64_t *tick, int *numerator, int *denomPow2,
                        bool *implicit) const
    {
        if (!QRectF(markerRow()).contains(pos))
            return false;
        const std::vector<SigChip> chips = sigChips();
        const auto stemHitHalfWidth = lyt::space(Space::One);
        const auto hitFuzz = lyt::singlePixel();
        // Back to front so the rightmost chip wins where chips crowd.
        for (auto it = chips.rbegin(); it != chips.rend(); ++it) {
            const bool onStem = std::abs(it->x - pos.x()) <= stemHitHalfWidth;
            const bool onLabel = it->labelW > 0 && pos.x() >= it->labelX - hitFuzz &&
                                 pos.x() <= it->labelX + it->labelW + hitFuzz;
            if (onStem || onLabel) {
                *tick = it->tick;
                *numerator = it->numerator;
                *denomPow2 = it->denomPow2;
                *implicit = it->implicit;
                return true;
            }
        }
        return false;
    }

    // Values in effect at tick (4/4 before any 0x58 meta).
    void sigAtTick(uint64_t tick, int *numerator, int *denomPow2) const
    {
        *numerator = 4;
        *denomPow2 = 2;
        for (const TimeSigPoint &ts : m_sv->timeline()->timeSigs) {
            if (ts.tick > tick)
                break;
            *numerator = ts.numerator ? ts.numerator : 4;
            *denomPow2 = ts.denomPow2;
        }
    }

    // 0 = selection start edge, 1 = end edge, -1 = neither near pos.
    int hitSelEdge(QPointF pos) const
    {
        const SongView::TimeSelection &sel = m_sv->timeSelection();
        if (!sel.active() || !QRectF(markerRow()).contains(pos))
            return -1;
        const auto markerHitHalfWidth = lyt::space(Space::Two);
        const qreal dpr = devicePixelRatioF();
        if (std::abs(m_sv->displayX(double(sel.startTick), kGutterW, dpr) - pos.x()) <=
            markerHitHalfWidth)
            return 0;
        if (std::abs(m_sv->displayX(double(sel.endTick), kGutterW, dpr) - pos.x()) <=
            markerHitHalfWidth)
            return 1;
        return -1;
    }

    void showRulerMenu(uint64_t clickTick, const QPoint &globalPos)
    {
        SongDocument *doc = m_sv->document();
        const MidiTimeline *tl = m_sv->timeline();
        if (!doc || !tl)
            return;
        QMenu menu(this);
        QAction *setStart = menu.addAction(SongView::tr("Set loop start here"));
        QAction *setEnd = menu.addAction(SongView::tr("Set loop end here"));
        QAction *remove = menu.addAction(SongView::tr("Remove loop markers"));
        remove->setEnabled(tl->loopStartTick != UINT64_MAX || tl->loopEndTick != UINT64_MAX);
        QAction *loopFromSel = nullptr;
        QAction *removeContents = nullptr;
        QAction *clearSel = nullptr;
        const SongView::TimeSelection sel = m_sv->timeSelection();
        if (sel.active()) {
            menu.addSeparator();
            loopFromSel = menu.addAction(SongView::tr("Set loop to selection"));
            removeContents = menu.addAction(SongView::tr("Remove selection contents (shift left)"));
            clearSel = menu.addAction(SongView::tr("Clear time selection"));
        }
        menu.addSeparator();
        uint64_t sigTick = clickTick;
        int sigNum, sigDen;
        bool sigImplicit = true;
        const bool onChip =
            hitTimeSigChip(m_rightPressPos, &sigTick, &sigNum, &sigDen, &sigImplicit);
        if (!onChip)
            sigAtTick(clickTick, &sigNum, &sigDen);
        QAction *editSig = menu.addAction(onChip ? SongView::tr("Edit time signature…")
                                                 : SongView::tr("Set time signature here…"));
        QAction *removeSig = menu.addAction(SongView::tr("Remove time signature"));
        removeSig->setEnabled(onChip && !sigImplicit);
        QAction *chosen = menu.exec(globalPos);
        if (chosen == setStart) {
            doc->setLoopTick(false, int64_t(clickTick));
        } else if (chosen == setEnd) {
            doc->setLoopTick(true, int64_t(clickTick));
        } else if (chosen == remove) {
            // Two commands; undo restores them one at a time.
            if (tl->loopStartTick != UINT64_MAX)
                doc->setLoopTick(false, -1);
            if (m_sv->timeline()->loopEndTick != UINT64_MAX)
                doc->setLoopTick(true, -1);
        } else if (chosen && chosen == loopFromSel) {
            // Same two-command shape as "Remove loop markers".
            doc->setLoopTick(false, int64_t(sel.startTick));
            doc->setLoopTick(true, int64_t(sel.endTick));
        } else if (chosen && chosen == removeContents) {
            m_sv->removeTimeSelectionContents();
        } else if (chosen && chosen == clearSel) {
            m_sv->clearTimeSelection();
        } else if (chosen == editSig) {
            if (askTimeSignature(this, &sigNum, &sigDen))
                doc->setTimeSig(sigTick, sigNum, sigDen);
        } else if (chosen == removeSig) {
            doc->deleteTimeSig(sigTick);
        }
    }

    SongView *m_sv;
    int m_markerHeight = 0;
    int m_dragMarker = -1;
    uint64_t m_dragTick = 0;
    bool m_dragTimeSig = false;     // chip drag is live; commits moveTimeSig
    uint64_t m_dragTimeSigFrom = 0; // the dragged signature's original tick
    bool m_placingCursor = false;
    bool m_rightPress = false; // right button held; sweep vs. menu undecided
    bool m_selSweep = false;   // right-drag time-selection sweep is live
    QPointF m_rightPressPos;
    uint64_t m_selAnchor = 0;         // snapped tick of the right press
    int m_dragSelEdge = -1;           // selection edge being left-dragged (0/1)
    QComboBox *m_divCombo = nullptr;  // minimum snap subdivision (gutter)
    QComboBox *m_feelCombo = nullptr; // straight / triplet
};

namespace {

// The edge cursors are baked pixmaps, so they carry the screen DPR they were
// rendered at. Qt 6.2 has no QEvent::DevicePixelRatioChange; the hover path
// re-bakes them whenever the widget's DPR no longer matches.
struct MidiCursors {
    qreal dpr;
    QCursor leftEdge;
    QCursor rightEdge;
};

// A pixmap cursor's default hotspot is its logical center, which most
// backends scale to device pixels themselves — but Qt's xcb backend (through
// at least the current dev branch) forwards the hotspot to X unscaled beside
// the full device-pixel image, so at DPR > 1 the glyph would hang down-right
// of the pointer. Hand xcb the center in device pixels; keep the logical
// default elsewhere (e.g. Windows multiplies by the screen scale itself).
QCursor centeredCursor(const QPixmap &pm)
{
    const qreal dpr =
        QGuiApplication::platformName() == QLatin1String("xcb") ? 1.0 : pm.devicePixelRatio();
    return QCursor(pm, qRound(pm.width() / (2.0 * dpr)), qRound(pm.height() / (2.0 * dpr)));
}

// The same xcb quirk for a cursor with an explicit logical-pixel hotspot
// (the pencil's tip): xcb wants it pre-scaled to device pixels, every other
// backend scales the logical value itself.
QCursor hotspotCursor(const QPixmap &pm, int hotX, int hotY)
{
    const qreal scale =
        QGuiApplication::platformName() == QLatin1String("xcb") ? pm.devicePixelRatio() : 1.0;
    return QCursor(pm, qRound(hotX * scale), qRound(hotY * scale));
}

MidiCursors loadMidiCursors(qreal devicePixelRatio)
{
    const QSize cursorSize(24, 24);
    const QIcon leftEdge(QStringLiteral(":/cursors/left-drag.png"));
    const QIcon rightEdge(QStringLiteral(":/cursors/right-drag.png"));
    return {devicePixelRatio, centeredCursor(leftEdge.pixmap(cursorSize, devicePixelRatio)),
            centeredCursor(rightEdge.pixmap(cursorSize, devicePixelRatio))};
}

QRectF noteFrame(const QPainter &painter, const QRectF &noteRect, int insetPixels)
{
    const qreal physicalPixel = logicalPhysicalPixel(painter.device()->devicePixelRatioF());
    const qreal insetDips = insetPixels * physicalPixel;
    return noteRect.adjusted(0, 0, -physicalPixel, -physicalPixel)
        .adjusted(insetDips, insetDips, -insetDips, -insetDips);
}

// Largest frame thickness (up to requestedPixels) that still leaves at
// least one physical pixel of face visible between the top and bottom
// strips; 0 when not even a one-pixel frame fits. Fitting uses the row
// height ONLY: rows are uniform at a given zoom, so every note at that
// zoom carries the same frame weight — a narrow note lets its side strips
// overlap into a solid sliver rather than shedding the frame its wider
// neighbors keep.
int fittedFrameThickness(const QPainter &painter, const QRectF &rect, int requestedPixels,
                         int insetPixels)
{
    const qreal devicePixelRatio = painter.device()->devicePixelRatioF();
    const int heightPixels = qRound(rect.height() * devicePixelRatio);
    return std::clamp((heightPixels - 1) / 2 - insetPixels, 0, requestedPixels);
}

// Returns the thickness actually painted (0 = nothing fit).
int drawRectFrame(QPainter &painter, const QRectF &rect, const QColor &color, int thicknessPixels,
                  int insetPixels = 0)
{
    thicknessPixels = fittedFrameThickness(painter, rect, thicknessPixels, insetPixels);
    if (thicknessPixels <= 0)
        return 0;

    // Paint one solid ring around the note box. Separate cosmetic outlines
    // can quantize onto non-adjacent device rows at fractional scale
    // factors, exposing the note face between them.
    const qreal devicePixelRatio = painter.device()->devicePixelRatioF();
    const qreal physicalPixel = logicalPhysicalPixel(devicePixelRatio);
    const qreal insetDips = insetPixels * physicalPixel;
    const qreal thicknessDips = thicknessPixels * physicalPixel;
    const QRectF frame = rect.adjusted(insetDips, insetDips, -insetDips, -insetDips);
    painter.fillRect(QRectF(frame.left(), frame.top(), frame.width(), thicknessDips), color);
    painter.fillRect(
        QRectF(frame.left(), frame.bottom() - thicknessDips, frame.width(), thicknessDips), color);
    const qreal sideHeight = std::max(0.0, frame.height() - 2 * thicknessDips);
    painter.fillRect(QRectF(frame.left(), frame.top() + thicknessDips, thicknessDips, sideHeight),
                     color);
    painter.fillRect(QRectF(frame.right() - thicknessDips, frame.top() + thicknessDips,
                            thicknessDips, sideHeight),
                     color);
    return thicknessPixels;
}

void drawNoteBoxBorder(QPainter &painter, const QRectF &noteBox, bool unterminated,
                       int insetPixels = 0)
{
    const int borderPixels = songview::noteBorderPixels(painter.device()->devicePixelRatioF());
    if (!unterminated) {
        drawRectFrame(painter, noteBox, Qt::black, borderPixels, insetPixels);
        return;
    }
    const int thickness = fittedFrameThickness(painter, noteBox, borderPixels, insetPixels);
    if (thickness <= 0)
        return;

    painter.save();
    QPen borderPen(Qt::black, 0);
    borderPen.setCapStyle(Qt::FlatCap);
    borderPen.setJoinStyle(Qt::MiterJoin);
    borderPen.setDashPattern({4, 2});
    painter.setPen(borderPen);
    painter.setBrush(Qt::NoBrush);
    for (int pixel = 0; pixel < thickness; ++pixel)
        painter.drawRect(noteFrame(painter, noteBox, insetPixels + pixel));
    painter.restore();
}

} // namespace

// ---------------------------------------------------------------- PianoRoll

class PianoRoll : public TimelineSurface
{
  public:
    explicit PianoRoll(SongView *sv)
        : TimelineSurface(sv)
        , m_sv(sv)
        , m_cursors(loadMidiCursors(devicePixelRatioF()))
    {
        setObjectName(QStringLiteral("pianoRoll")); // findChild for tests
        setMinimumHeight(120);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMouseTracking(true);
        setFocusPolicy(Qt::ClickFocus);
        m_noteMenu = new NoteContextMenu(
            this, [this](QPointF globalPos) { return moveNoteMenu(globalPos); });
        connect(m_noteMenu, &QMenu::triggered, this, [this](QAction *action) {
            handleNoteMenuChoice(m_noteMenu->handleAction(action));
        });
    }

    // A mouse gesture is live (pan, note move/resize/velocity/draw, band or
    // time-selection sweep, a still-undecided press, keyboard gliss); the
    // playhead follow-scroll pauses while one runs so the view doesn't jump
    // under the cursor.
    bool gestureActive() const
    {
        return m_panning || m_drag != Drag::None || m_leftPress || m_rightPress || m_kbdKey >= 0;
    }

    // The value a live velocity drag is holding a note at, if it holds this
    // one: the same rule drawNotes paints by, so the velocity lane's node
    // and the roll's fill can never disagree mid-drag.
    std::optional<uint8_t> velocityDragPreview(const ViewNote &note) const
    {
        if (m_drag != Drag::Velocity || m_dVel == 0 || !m_sv->isSelected(note))
            return std::nullopt;
        return uint8_t(std::clamp(int(note.velocity) + m_dVel, 1, 127));
    }

  protected:
    void paintContent(QPainter &p) override
    {
        p.fillRect(rect(), themes::color(themes::Role::song_view_piano_roll_background));
        if (!m_sv->timeline()) {
            drawKeyboard(p);
            return;
        }

        const QRect grid(kKeyboardW, 0, width() - kKeyboardW, height());
        // Narrow, never replace: the cached-surface painter arrives clipped
        // to the dirty region and partial repaints must stay inside it.
        p.save();
        p.setClipRect(grid, Qt::IntersectClip);

        // Pitch row shading plus a hairline under every semitone row; C rows
        // keep the stronger octave delineator, on the same snapped edge as
        // the keyboard column's separators.
        const QColor accidentalRow = pianoRollAccidentalLaneColor();
        const QColor octaveLine = themes::color(themes::Role::song_view_piano_keyboard_separator);
        const QPen keyLinePen(gridLineColor(50), 0);
        const QPen octavePen(octaveLine, 0);
        for (int key = 0; key < 128; key++) {
            const QRectF row = keyRect(key, grid.left(), grid.width());
            if (row.bottom() <= 0 || row.top() >= height())
                continue;
            if (isBlackKey(key))
                p.fillRect(row, accidentalRow);
            p.setPen(key % 12 == 0 ? octavePen : keyLinePen);
            p.drawLine(QLineF(grid.left(), row.bottom(), grid.right(), row.bottom()));
        }

        drawPreRoll(p, m_sv, grid, kKeyboardW,
                    themes::color(themes::Role::song_view_piano_roll_background));
        drawGrid(p, m_sv, grid, kKeyboardW);

        // Notes: ghost pass (unselected tracks), then the selected track.
        const SongViewModel &model = m_sv->model();
        const int selected = m_sv->selectedTrack();
        drawNotes(p, model, selected, true);
        drawNotes(p, model, selected, false);
        drawDragPreview(p, model, selected);

        if (m_drag == Drag::Band) {
            const QRectF band = QRectF(m_pressPos, m_curPos).normalized();
            QColor c = themes::color(themes::Role::song_view_selection_edge);
            p.setPen(QPen(c, 1, Qt::DashLine));
            c.setAlpha(30);
            p.fillRect(band, c);
            p.drawRect(band);
        }

        drawOverlays(p, m_sv, grid, kKeyboardW,
                     m_sv->timeSelectionCoversTrack(m_sv->selectedTrack()));

        p.restore();
        drawKeyboard(p);
    }

    void wheelEvent(QWheelEvent *event) override
    {
        // Reaper-style bindings: plain wheel over the notes area zooms the
        // timeline, over the keyboard column it scrolls the note range.
        // Ctrl+wheel zooms the key height (the track-height analog); Shift
        // (or a trackpad's horizontal delta) scrolls horizontally.
        const QPoint delta = wheelDelta(event);
        const int d = delta.y() ? delta.y() : delta.x();
        if (event->modifiers() & Qt::ControlModifier) {
            m_sv->zoomKeyHeight(event);
        } else if (event->modifiers() & Qt::ShiftModifier) {
            m_sv->scrollByPx(-d);
        } else if (delta.x() && !delta.y()) {
            m_sv->scrollByPx(-delta.x());
        } else if (event->position().x() < kKeyboardW) {
            m_sv->scrollRollBy(-delta.y() / 2.0);
        } else {
            const double zoomDelta = wheelAngleUnits(event);
            if (zoomDelta != 0.0)
                m_sv->zoomAroundContentX(std::pow(1.0015, zoomDelta),
                                         event->position().x() - kKeyboardW);
        }
        event->accept();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        setFocus();
        if (!m_sv->timeline())
            return;

        if (event->button() == Qt::MiddleButton) {
            // Reaper-style pan: drag scrolls the roll on both axes.
            m_panning = true;
            m_panPos = event->globalPosition();
            setCursor(Qt::ClosedHandCursor);
            return;
        }

        // Keyboard column: audition the clicked key on the selected track.
        if (event->position().x() < kKeyboardW) {
            if (event->button() == Qt::LeftButton) {
                m_kbdKey = yToKey(event->position().y());
                auditionKey(m_kbdKey, 100);
            }
            return;
        }

        SongDocument *doc = m_sv->document();
        const ViewNote *hit = doc ? hitNote(event->position()) : nullptr;

        if (event->button() == Qt::RightButton) {
            // Deferred: a drag from here rubber-band-selects (with Shift, it
            // sweeps a full-height time selection instead); releasing in
            // place context-acts on the pressed note (or on the time
            // selection under the click, or clears the selections over empty
            // space). Resolved in mouseReleaseEvent.
            if (!doc)
                return;
            m_pressPos = m_curPos = event->position();
            m_rightPress = true;
            m_rightShift = event->modifiers() & Qt::ShiftModifier;
            m_rightAnchorTick =
                m_sv->snapTick(m_sv->tickAtContentX(event->position().x() - kKeyboardW));
            m_rightHit = hit != nullptr;
            if (hit)
                m_rightHitId = {hit->startTick, hit->key};
            return;
        }
        if (event->button() != Qt::LeftButton)
            return;

        m_pressPos = m_curPos = event->position();
        m_pressTick = m_sv->tickAtContentX(event->position().x() - kKeyboardW);
        m_pressKey = yToKey(event->position().y());
        m_dTick = 0;
        m_dKey = 0;
        m_dDur = 0;
        m_dVel = 0;
        m_velModDrag = false;

        if (hit) {
            const bool rightEdge = nearRightEdge(*hit, event->position());
            const bool leftEdge = nearLeftEdge(*hit, event->position());
            // Ableton-style velocity gesture: with the bound modifier chord
            // held (Ctrl by default), a vertical drag from anywhere on the
            // note adjusts velocity. Deferred like the empty-space press:
            // the click action (Ctrl's selection toggle) resolves on
            // release, a drag past the threshold in mouseMoveEvent.
            const auto &keys = keymap::Registry::instance();
            const auto pressMods = event->modifiers();
            if (keys.matchesModifier(pressMods, QStringLiteral("roll.velocity_drag")) &&
                !rightEdge && !leftEdge) {
                m_velModPress = true;
                m_velModMods = keymap::Registry::instance().modifierBinding(
                    QStringLiteral("roll.velocity_drag"));
                m_velAnchor = *hit;
                m_velAudEff = mid2agbEffectiveVelocity(hit->velocity);
                m_sv->announceNote(*hit);
                m_lastVelocity = hit->velocity;
                auditionKey(hit->key, hit->velocity, hit->startTick);
                m_auditioned = true;
                invalidateContent();
                return;
            }
            std::vector<SongView::NoteKey> ids = m_sv->selection();
            const SongView::NoteKey id{hit->startTick, hit->key};
            if ((event->modifiers() & Qt::ControlModifier) && !rightEdge && !leftEdge) {
                const auto it = std::find(ids.begin(), ids.end(), id);
                if (it != ids.end())
                    ids.erase(it);
                else
                    ids.push_back(id);
                m_sv->setSelection(std::move(ids));
            } else if (event->modifiers() & Qt::ControlModifier) {
                // Ctrl+edge grab: the grip still starts a resize of the
                // whole selection, so a bulk-select click landing on an
                // edge must join the note to the selection, not replace it.
                if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
                    ids.push_back(id);
                    m_sv->setSelection(std::move(ids));
                }
            } else if (!m_sv->isSelected(*hit)) {
                m_sv->setSelection({id});
            }
            m_sv->announceNote(*hit);
            // Reaper-style velocity latch: touching a note makes its velocity
            // the default for the next drawn note.
            m_lastVelocity = hit->velocity;
            if (rightEdge) {
                m_drag = Drag::Resize;
                m_gripTick = hit->endTick;
                m_gripOpposite = hit->startTick;
            } else if (leftEdge) {
                m_drag = Drag::ResizeLeft;
                m_gripTick = hit->startTick;
                m_gripOpposite = hit->endTick;
            } else if (nearVelocityHandle(*hit, event->position())) {
                m_drag = Drag::Velocity;
                m_velAnchor = *hit;
                m_velAudEff = mid2agbEffectiveVelocity(hit->velocity);
            } else {
                m_drag = Drag::Move;
            }
            // Sound the grabbed note so a press gives the same pitch feedback
            // a drag already does.
            auditionKey(hit->key, hit->velocity, hit->startTick);
            m_auditioned = true;
        } else if (doc) {
            // Empty space: deferred, Reaper-style. A horizontal drag from
            // here draws a note (resolved in mouseMoveEvent); releasing in
            // place parks the edit cursor at the click instead. A
            // double-click draws immediately (mouseDoubleClickEvent).
            m_leftPress = true;
            m_sv->clearSelection();
            // Sound the clicked row at the latched velocity so a plain
            // press gives the same pitch feedback a draw already does.
            auditionKey(m_pressKey, m_lastVelocity, m_sv->snapTickDown(m_pressTick));
            m_auditioned = true;
        } else {
            // Read-only (no document): park the edit cursor at the click,
            // like the ruler; playback follows when running.
            m_sv->commitEditCursor(m_sv->snapTick(m_pressTick));
        }
        invalidateContent();
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        // Double-click on empty space drops a grid-sized note (committed on
        // release; dragging before release still sizes it); on a note it
        // deletes that note. Anywhere else a fast click-click behaves as two
        // presses — Qt replaces the second press with this event.
        SongDocument *doc = m_sv->document();
        if (event->button() == Qt::LeftButton && doc && event->position().x() >= kKeyboardW) {
            setFocus();
            if (const ViewNote *hit = hitNote(event->position())) {
                DocNote note;
                if (doc->findNote(m_sv->selectedTrack(), hit->startTick, hit->key, &note)) {
                    doc->deleteNotes({note});
                    m_sv->clearSelection();
                }
                return;
            }
            m_pressPos = m_curPos = event->position();
            m_pressTick = m_sv->tickAtContentX(event->position().x() - kKeyboardW);
            m_pressKey = yToKey(event->position().y());
            beginDraw();
            return;
        }
        mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        // A velocity drag moves the cursor vertically while the note's
        // pitch stays put; the mark pins to the note so the readout
        // doesn't wander off its row.
        setHoverKey(m_drag == Drag::Velocity ? m_velAnchor.key : yToKey(event->position().y()));
        if (m_panning) {
            const QPointF d = event->globalPosition() - m_panPos;
            m_panPos = event->globalPosition();
            m_sv->scrollByPx(-d.x());
            m_sv->scrollRollBy(-d.y());
            return;
        }
        if (m_kbdKey >= 0) {
            // Keyboard column: dragging glisses — the sounding key follows
            // the cursor (the engine's mono preview releases the old key).
            const int key = yToKey(event->position().y());
            if (key != m_kbdKey) {
                m_kbdKey = key;
                auditionKey(m_kbdKey, 100);
            }
            return;
        }
        m_curPos = event->position();
        if (m_rightPress && m_drag == Drag::None &&
            (event->pos() - m_pressPos.toPoint()).manhattanLength() >=
                QApplication::startDragDistance()) {
            m_drag = m_rightShift ? Drag::TimeSel : Drag::Band;
            m_bandAud.clear();
        }
        if (m_leftPress && m_drag == Drag::None) {
            // The pressed row's preview glisses with the cursor, like the
            // keyboard column; a draw started below anchors on the new row.
            const int key = yToKey(event->position().y());
            if (key != m_pressKey) {
                m_pressKey = key;
                auditionKey(key, m_lastVelocity, m_sv->snapTickDown(m_pressTick));
                m_auditioned = true;
            }
        }
        if (m_leftPress && m_drag == Drag::None &&
            std::abs(event->position().x() - m_pressPos.x()) >= lyt::space(Space::One)) {
            // The deferred empty-space press turns out to be a draw gesture.
            // Space::One of horizontal travel starts it — enough to filter
            // click jitter while staying well under the platform drag
            // threshold, so the pending note still appears near-immediately;
            // this same event falls through to the Draw branch, which sizes
            // it from the cursor (one snap cell until the drag crosses the
            // next snap line).
            beginDraw();
        }
        if (m_velModPress && m_drag == Drag::None) {
            // The deferred modifier press becomes a velocity drag once it
            // travels vertically past the click threshold (so a jittery
            // Ctrl+click stays a selection toggle). The same event falls
            // through to the Velocity branch, which measures from the press.
            if (std::abs(event->pos().y() - m_pressPos.toPoint().y()) <
                QApplication::startDragDistance())
                return;
            m_velModPress = false;
            const SongView::NoteKey id{m_velAnchor.startTick, m_velAnchor.key};
            if (m_velReplaceNote.isAssigned() && m_velAnchor.noteId != m_velReplaceNote) {
                // A modifier velocity edit already committed during this
                // uninterrupted chord hold: a second drag on a DIFFERENT
                // note is a request to edit that note, so it replaces the
                // selection instead of joining and nudging both. Same
                // anchor falls through, so a deliberate bulk selection
                // survives repeated drags on its own anchor — compared by
                // NoteId, so a mid-hold Ctrl+arrow transpose/nudge that
                // re-keys the selection can't disguise the anchor.
                m_velReplaceNote = {};
                m_sv->setSelection({id});
            } else if (!m_sv->isSelected(m_velAnchor)) {
                if (m_velModMods & Qt::ControlModifier) {
                    // Ctrl in the chord: like the Ctrl+edge grab, the
                    // gesture joins the note to the bulk selection built
                    // with the same modifier instead of replacing it, and
                    // the drag then nudges the whole selection.
                    std::vector<SongView::NoteKey> ids = m_sv->selection();
                    ids.push_back(id);
                    m_sv->setSelection(std::move(ids));
                } else {
                    m_sv->setSelection({id});
                }
            }
            m_velModDrag = true;
            m_drag = Drag::Velocity;
            // The pass at the top of this event ran before the drag
            // existed; re-pin the mark to the note's row now.
            setHoverKey(m_velAnchor.key);
        }
        if (m_drag == Drag::None) {
            // Hover cursor: resize handle at note left/right edges, velocity
            // handle along the note's velocity bar (when zoomed in enough).
            if (m_cursors.dpr != devicePixelRatioF())
                m_cursors = loadMidiCursors(devicePixelRatioF());
            const ViewNote *hit = m_sv->document() && event->position().x() >= kKeyboardW
                                      ? hitNote(event->position())
                                      : nullptr;
            // Resize edges win over both velocity-hover paths.
            const auto &keys = keymap::Registry::instance();
            const auto hoverMods = event->modifiers();
            if (hit && nearRightEdge(*hit, event->position()))
                setCursor(m_cursors.rightEdge);
            else if (hit && nearLeftEdge(*hit, event->position()))
                setCursor(m_cursors.leftEdge);
            else if (hit && keys.matchesModifier(hoverMods, QStringLiteral("roll.velocity_drag")))
                setCursor(Qt::SizeVerCursor);
            else if (hit && nearVelocityHandle(*hit, event->position()))
                setCursor(Qt::SizeVerCursor);
            else
                setCursor(Qt::ArrowCursor);
            return;
        }

        const double tick = m_sv->tickAtContentX(event->position().x() - kKeyboardW);
        const int64_t grid = int64_t(m_sv->snapTicksAt(uint64_t(std::max(0.0, m_pressTick))));
        const int64_t snappedD = int64_t(std::llround((tick - m_pressTick) / double(grid))) * grid;

        if (m_drag == Drag::Move) {
            const int dKey = yToKey(event->position().y()) - m_pressKey;
            if (snappedD != m_dTick || dKey != m_dKey) {
                const bool keyMoved = dKey != m_dKey;
                m_dTick = snappedD;
                m_dKey = dKey;
                // Audition the new pitch while dragging vertically, and the
                // same pitch again when a horizontal move lands it under a
                // different VOL.
                const std::vector<DocNote> notes = resolveSelection();
                if (!notes.empty()) {
                    const int key = std::clamp(int(notes.front().key) + m_dKey, 0, 127);
                    const uint64_t at =
                        uint64_t(std::max<int64_t>(0, int64_t(notes.front().tick) + m_dTick));
                    if (keyMoved || auditionVolumeAt(at) != m_soundingVol ||
                        auditionPanAt(at) != m_soundingPan) {
                        auditionKey(key, notes.front().velocity, at);
                        m_auditioned = true;
                    }
                }
                invalidateContent();
            }
        } else if (m_drag == Drag::Resize || m_drag == Drag::ResizeLeft) {
            // Snap the dragged edge to absolute ruler grid lines, not offsets from
            // its original (possibly off-grid) position. Keep at least one tick.
            const double desired = double(m_gripTick) + (tick - m_pressTick);
            const uint64_t snapped =
                m_drag == Drag::Resize ? std::max(m_sv->snapTick(desired),
                                                  m_sv->snapTickUp(double(m_gripOpposite) + 1.0))
                                       : std::min(m_sv->snapTick(desired),
                                                  m_sv->snapTickDown(double(m_gripOpposite) - 1.0));
            const int64_t delta =
                std::abs(desired - double(m_gripTick)) < std::abs(desired - double(snapped))
                    ? 0
                    : int64_t(snapped) - int64_t(m_gripTick);
            int64_t &target = m_drag == Drag::Resize ? m_dDur : m_dTick;
            if (delta != target) {
                target = delta;
                invalidateContent();
            }
        } else if (m_drag == Drag::Velocity) {
            const int dv = m_pressPos.toPoint().y() - event->pos().y(); // up = louder
            if (dv != m_dVel) {
                m_dVel = dv;
                const int vel = std::clamp(int(m_velAnchor.velocity) + m_dVel, 1, 127);
                ViewNote preview = m_velAnchor;
                preview.velocity = uint8_t(vel);
                m_sv->announceNote(preview);
                // Re-audition whenever the effective (played) velocity moves
                // to the next mid2agb step.
                const int eff = mid2agbEffectiveVelocity(vel);
                if (eff != m_velAudEff) {
                    m_velAudEff = eff;
                    auditionKey(m_velAnchor.key, vel, m_velAnchor.startTick);
                    m_auditioned = true;
                }
                invalidateContent();
                // The lane plots the same notes: move its nodes with the drag.
                m_sv->rollVelocityPreviewChanged();
            }
        } else if (m_drag == Drag::Draw) {
            // The edge under the cursor follows it: right of the anchor cell
            // the end grows (rounded up to the next snap line, never shorter
            // than one snap cell); left of it the start moves back (snapped
            // down) with the end pinned to the anchor cell. The key follows the
            // cursor vertically — a slight misclick on mouse-down is fixable
            // mid-gesture, with the new pitch auditioned.
            const uint64_t anchor = m_drawAnchor;
            uint64_t start = anchor;
            int64_t dur;
            if (tick >= double(anchor)) {
                const uint64_t end = std::max(anchor + uint64_t(grid), m_sv->snapTickUp(tick));
                dur = int64_t(end - anchor);
            } else {
                start = m_sv->snapTickDown(tick);
                dur = int64_t(anchor + uint64_t(grid) - start);
            }
            const int key = yToKey(event->position().y());
            if (start != m_drawTick || dur != m_drawDur || key != m_drawKey) {
                const bool keyMoved = key != m_drawKey;
                m_drawTick = start;
                m_drawDur = dur;
                m_drawKey = key;
                // Dragging the start back past a VOL change moves the pending
                // note under a new volume; re-attack so the preview follows.
                if (keyMoved || auditionVolumeAt(m_drawTick) != m_soundingVol ||
                    auditionPanAt(m_drawTick) != m_soundingPan) {
                    auditionKey(m_drawKey, m_lastVelocity, m_drawTick);
                    m_auditioned = true;
                }
                invalidateContent();
            }
        } else if (m_drag == Drag::TimeSel) {
            // Full-height sweep: a time selection over the selected tracks
            // (notes and automation together), same scope as a ruler sweep.
            const uint64_t t = m_sv->snapTick(tick);
            SongView::TimeSelection sel;
            sel.startTick = std::min(m_rightAnchorTick, t);
            sel.endTick = std::max(m_rightAnchorTick, t);
            m_sv->setTimeSelection(sel);
        } else if (m_drag == Drag::Band) {
            auditionBandEntrants(QRectF(m_pressPos, m_curPos).normalized());
            invalidateContent();
        }
    }

    void leaveEvent(QEvent *) override { setHoverKey(-1); }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::MiddleButton && m_panning) {
            m_panning = false;
            setCursor(Qt::ArrowCursor);
            return;
        }
        if (m_kbdKey >= 0) {
            auditionKey(m_kbdKey, 0);
            m_kbdKey = -1;
        }
        if (m_auditioned) {
            auditionKey(0, 0);
            m_auditioned = false;
        }
        SongDocument *doc = m_sv->document();
        if (event->button() == Qt::RightButton && m_rightPress) {
            const Drag drag = m_drag;
            m_rightPress = false;
            m_drag = Drag::None;
            if (drag == Drag::TimeSel) {
                if (m_sv->timeSelection().active())
                    m_sv->announceTimeSelection();
                else
                    m_sv->clearTimeSelection();
            } else if (drag == Drag::Band) {
                stopBandAuditions();
                selectBand(QRectF(m_pressPos, m_curPos).normalized(),
                           event->modifiers() & Qt::ControlModifier);
            } else if (doc && m_rightHit) {
                const std::vector<SongView::NoteKey> &sel = m_sv->selection();
                if (std::find(sel.begin(), sel.end(), m_rightHitId) == sel.end())
                    m_sv->setSelection({m_rightHitId});
                showNoteMenu(event->position());
            } else if (insideTimeSelection(event->position().x())) {
                m_sv->showTimeSelectionMenu(event->globalPosition().toPoint());
            } else {
                m_sv->clearSelection();
                m_sv->clearTimeSelection();
            }
            invalidateContent();
            return;
        }
        if (event->button() == Qt::LeftButton && m_leftPress) {
            m_leftPress = false;
            if (m_drag == Drag::None) {
                // Click without a drag: park the edit cursor at the click,
                // like the ruler; playback follows when running.
                m_sv->commitEditCursor(m_sv->snapTick(m_pressTick));
                invalidateContent();
                return;
            }
        }
        if (event->button() == Qt::LeftButton && m_velModPress) {
            // The modifier press never grew into a velocity drag: give the
            // click its undeferred meaning — Ctrl in the chord keeps its
            // selection toggle, any other chord selects like a plain click.
            m_velModPress = false;
            const SongView::NoteKey id{m_velAnchor.startTick, m_velAnchor.key};
            if (m_velModMods & Qt::ControlModifier) {
                std::vector<SongView::NoteKey> ids = m_sv->selection();
                const auto it = std::find(ids.begin(), ids.end(), id);
                if (it != ids.end())
                    ids.erase(it);
                else
                    ids.push_back(id);
                m_sv->setSelection(std::move(ids));
            } else if (!m_sv->isSelected(m_velAnchor)) {
                m_sv->setSelection({id});
            }
            invalidateContent();
            return;
        }
        if (event->button() != Qt::LeftButton || m_drag == Drag::None)
            return;

        const Drag drag = m_drag;
        m_drag = Drag::None;
        const bool velModDrag = m_velModDrag;
        m_velModDrag = false;

        if (doc && drag == Drag::Draw) {
            doc->addNote(m_sv->selectedTrack(), m_drawTick, uint8_t(m_drawKey), uint32_t(m_drawDur),
                         m_lastVelocity);
            m_sv->setSelection({{uint32_t(m_drawTick), uint8_t(m_drawKey)}});
        } else if (doc && drag == Drag::Move && (m_dTick != 0 || m_dKey != 0)) {
            const std::vector<DocNote> notes = resolveSelection();
            doc->moveNotes(notes, m_dTick, m_dKey);
            // Follow the notes with the selection.
            std::vector<SongView::NoteKey> ids;
            for (const DocNote &note : notes)
                ids.push_back({uint32_t(std::max<int64_t>(0, int64_t(note.tick) + m_dTick)),
                               uint8_t(std::clamp(int(note.key) + m_dKey, 0, 127))});
            m_sv->setSelection(std::move(ids));
        } else if (doc && drag == Drag::Resize && m_dDur != 0) {
            doc->resizeNotes(resolveSelection(), m_dDur);
        } else if (doc && drag == Drag::ResizeLeft && m_dTick != 0) {
            const std::vector<DocNote> notes = resolveSelection();
            doc->resizeNotesLeft(notes, m_dTick);
            // Selection ids key on the start tick, which just moved; follow
            // it (same clamp as the document: the note-off pins the drag).
            std::vector<SongView::NoteKey> ids;
            for (const DocNote &note : notes) {
                const int64_t maxTick =
                    note.unterminated() ? INT64_MAX : int64_t(note.tick + note.duration) - 1;
                ids.push_back(
                    {uint32_t(std::clamp<int64_t>(int64_t(note.tick) + m_dTick, 0, maxTick)),
                     note.key});
            }
            m_sv->setSelection(std::move(ids));
        } else if (doc && drag == Drag::Velocity && m_dVel != 0) {
            doc->nudgeNotesVelocity(resolveSelection(), m_dVel);
            // Latch the dragged note's final velocity for the next draw.
            m_lastVelocity = uint8_t(std::clamp(int(m_velAnchor.velocity) + m_dVel, 1, 127));
            // A modifier drag that committed, released with the chord still
            // held, arms the one-shot: the next modifier drag on a different
            // note replaces the selection instead of joining (see the
            // promotion in mouseMoveEvent).
            if (velModDrag && keymap::Registry::instance().matchesModifier(
                                  event->modifiers(), QStringLiteral("roll.velocity_drag")))
                m_velReplaceNote = m_velAnchor.noteId;
        }
        m_dTick = 0;
        m_dKey = 0;
        m_dDur = 0;
        m_dVel = 0;
        invalidateContent();
        // A committed drag reaches the lane through the document rebuild, but
        // one that ended on no change (or was abandoned) has to drop its
        // preview here or the lane keeps drawing the last dragged value.
        if (drag == Drag::Velocity)
            m_sv->rollVelocityPreviewChanged();
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (!event->isAutoRepeat() && keymap::Registry::isModifierKey(event->key()))
            invalidateContent();
        // Time-selection range ops (and range-clip paste) win over the
        // note-selection shortcuts; the two selections are mutually
        // exclusive, so there is never a real conflict.
        if (m_sv->handleEditKey(event))
            return;
        const auto &keys = keymap::Registry::instance();
        SongDocument *doc = m_sv->document();
        const bool cut = keys.matches(event, QStringLiteral("roll.cut"));
        if (doc && (cut || keys.matches(event, QStringLiteral("roll.copy")))) {
            const std::vector<DocNote> notes = resolveSelection();
            if (!notes.empty()) {
                copyNotes(notes);
                if (cut) {
                    doc->deleteNotes(notes);
                    m_sv->clearSelection();
                }
            }
            event->accept();
            return;
        }
        if (doc && keys.matches(event, QStringLiteral("roll.paste"))) {
            pasteAtEditCursor();
            event->accept();
            return;
        }
        if (doc && keys.matches(event, QStringLiteral("roll.select_all"))) {
            selectAllNotes();
            event->accept();
            return;
        }
        if (doc && keys.matches(event, QStringLiteral("roll.delete"))) {
            const std::vector<DocNote> notes = resolveSelection();
            if (!notes.empty()) {
                doc->deleteNotes(notes);
                m_sv->clearSelection();
            }
            event->accept();
            return;
        }
        if (doc) {
            const int transpose = m_sv->transposeStepFor(event);
            if (transpose != 0) {
                transposeSelection(transpose);
                event->accept();
                return;
            }
        }
        if (doc && (keys.matches(event, QStringLiteral("roll.nudge_left")) ||
                    keys.matches(event, QStringLiteral("roll.nudge_right")))) {
            nudgeSelection(keys.matches(event, QStringLiteral("roll.nudge_right")));
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Escape) {
            m_drag = Drag::None;
            m_velModDrag = false;
            // An undecided modifier press dies too — without this the
            // "cancelled" press still promotes to a drag or toggles the
            // selection Escape just cleared on its release.
            m_velModPress = false;
            m_leftPress = false;
            m_rightPress = false;
            stopBandAuditions();
            m_sv->clearSelection();
            m_sv->clearTimeSelection();
            invalidateContent();
            // An abandoned velocity drag takes its lane preview with it.
            m_sv->rollVelocityPreviewChanged();
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void keyReleaseEvent(QKeyEvent *event) override
    {
        if (!event->isAutoRepeat() && keymap::Registry::isModifierKey(event->key())) {
            // Any modifier coming up ends the "uninterrupted hold" the
            // one-shot selection replace is scoped to; the next modifier
            // velocity drag joins again.
            m_velReplaceNote = {};
            invalidateContent();
        }
        if (m_sv->handleEditKeyRelease(event))
            return;
        // End the transpose audition when the shortcut's keys come up.
        // Autorepeat releases are skipped so a held Ctrl+Up keeps sounding
        // the moving pitch; the Drag::None guard keeps a stray key release
        // from cutting a mouse gesture's preview short.
        if (!event->isAutoRepeat() && m_auditioned && m_drag == Drag::None) {
            auditionKey(0, 0);
            m_auditioned = false;
        }
        QWidget::keyReleaseEvent(event);
    }

    bool event(QEvent *event) override
    {
        // Losing focus (or the window) is as much an interruption as
        // releasing the chord: the one-shot selection replace must not
        // survive an Alt+Tab and fire on a drag minutes later.
        const QEvent::Type type = event->type();
        if (type == QEvent::FocusOut || type == QEvent::Hide || type == QEvent::WindowDeactivate)
            m_velReplaceNote = {};
        return TimelineSurface::event(event);
    }

  private:
    enum class Drag { None, Band, TimeSel, Move, Resize, ResizeLeft, Velocity, Draw };

    // Whether pos falls inside the active time selection's band as this
    // widget draws it (the selection must cover the shown track).
    bool insideTimeSelection(qreal x) const
    {
        const SongView::TimeSelection &sel = m_sv->timeSelection();
        if (!sel.active() || !m_sv->timeSelectionCoversTrack(m_sv->selectedTrack()))
            return false;
        const qreal dpr = devicePixelRatioF();
        const qreal startX = m_sv->displayX(double(sel.startTick), kKeyboardW, dpr);
        const qreal endX = m_sv->displayX(double(sel.endTick), kKeyboardW, dpr);
        return x >= startX && x < endX;
    }

    // The roll has one vertical projection. Every row edge is independently
    // snapped from the continuous camera, so adjacent rows meet exactly at
    // fractional display scales without accumulated rounding error.
    const std::array<qreal, 129> &rowEdges() const
    {
        const qreal dpr = devicePixelRatioF();
        const qreal keyHeight = m_sv->keyHeight();
        const qreal scrollY = m_sv->scrollY();
        if (!m_rowEdgesValid || m_rowEdgesDpr != dpr || m_rowEdgesKeyHeight != keyHeight ||
            m_rowEdgesScrollY != scrollY) {
            for (int row = 0; row <= 128; ++row) {
                const qreal ideal = row * keyHeight - scrollY;
                m_rowEdges[row] = std::round(ideal * dpr) / dpr;
            }
            m_rowEdgesDpr = dpr;
            m_rowEdgesKeyHeight = keyHeight;
            m_rowEdgesScrollY = scrollY;
            m_rowEdgesValid = true;
        }
        return m_rowEdges;
    }

    qreal keyTop(int key) const { return rowEdges()[127 - key]; }
    qreal keyBottom(int key) const { return rowEdges()[128 - key]; }

    QRectF keyRect(int key, qreal x, qreal width) const
    {
        const qreal top = keyTop(key);
        return QRectF(x, top, width, keyBottom(key) - top);
    }

    int yToKey(qreal y) const
    {
        const std::array<qreal, 129> &edges = rowEdges();
        const auto edge = std::upper_bound(edges.begin(), edges.end(), y);
        const int row = std::clamp(int(edge - edges.begin()) - 1, 0, 127);
        return 127 - row;
    }

    qreal physicalPixel() const { return logicalPhysicalPixel(devicePixelRatioF()); }

    struct KeyboardHoverGeometry {
        QRectF highlightRect;
        QString name;
        QFont chipFont;
        QRectF chipRect;
        QRegion paintRegion;
    };

    std::optional<KeyboardHoverGeometry> keyboardHoverGeometry(int key) const
    {
        if (key < 0 || key > 127)
            return std::nullopt;

        const QRectF highlight = keyRect(key, 0, kKeyboardW);
        const QString name = midiKeyName(key);
        QFont chipFont = font();
        chipFont.setPixelSize(10);
        const QFontMetrics metrics(chipFont);
        const int chipWidth = metrics.horizontalAdvance(name) + 8;
        const int chipHeight = metrics.height() + 2;
        const qreal chipY = std::clamp(highlight.center().y() - chipHeight / 2.0, 0.0,
                                       qreal(std::max(0, height() - chipHeight)));
        const QRectF chip(kKeyboardW - 2 - chipWidth, chipY, chipWidth, chipHeight);
        QRegion paintRegion(chip.toAlignedRect());
        if (key != m_soundingKey)
            paintRegion |= QRegion(highlight.toAlignedRect());
        paintRegion &= QRegion(0, 0, kKeyboardW, height());
        return KeyboardHoverGeometry{highlight, name, chipFont, chip, paintRegion};
    }

    // Key row under the cursor: the keyboard column mirrors it with a tint
    // and a note-name chip so the row reads at any zoom (-1 = cursor left
    // the roll). Exposed as a dynamic property for the check harness.
    void setHoverKey(int key)
    {
        if (key == m_hoverKey)
            return;
        const auto oldGeometry = keyboardHoverGeometry(m_hoverKey);
        const QRegion oldRegion = oldGeometry ? oldGeometry->paintRegion : QRegion();
        m_hoverKey = key;
        setProperty("hoverKey", m_hoverKey);
        const auto newGeometry = keyboardHoverGeometry(m_hoverKey);
        const QRegion newRegion = newGeometry ? newGeometry->paintRegion : QRegion();
        invalidateContent(oldRegion | newRegion);
    }

    // All roll auditions go through here so the keyboard column can mark the
    // sounding key (velocity 0 releases and clears the mark). atTick is the
    // start of the note being sounded, so it previews at the track volume in
    // force there; gestures with no note behind them (the keyboard column)
    // leave it at the default and sound at the edit cursor's volume.
    void auditionKey(int key, int velocity, uint64_t atTick = SongView::kAuditionAtCursor)
    {
        m_soundingVol = auditionVolumeAt(atTick);
        m_soundingPan = auditionPanAt(atTick);
        m_sv->audition(m_sv->selectedTrack(), key, velocity, atTick);
        const int sounding = velocity > 0 ? key : -1;
        if (sounding != m_soundingKey) {
            m_soundingKey = sounding;
            invalidateContent(QRegion(0, 0, kKeyboardW, height()));
        }
    }

    // The VOL byte an audition aimed at atTick would sound under. Drag
    // gestures compare it as they move so a note dragged horizontally out of
    // a loud passage into a quiet one re-attacks at the volume it now sits
    // under, instead of holding the origin's for the rest of the gesture.
    int auditionVolumeAt(uint64_t atTick) const
    {
        return m_sv->auditionVolume(m_sv->selectedTrack(), atTick);
    }

    // The same for PAN, compared alongside it: a note dragged across a PAN
    // point moves to the other side of the stereo field (and, on a CGB
    // channel, to a different level), so it re-attacks there too.
    int auditionPanAt(uint64_t atTick) const
    {
        return m_sv->auditionPan(m_sv->selectedTrack(), atTick);
    }

    // Begin the pencil gesture: a pending grid-cell note at the press
    // position that sounds while the button is held; the document note is
    // committed on release (one undo entry).
    void beginDraw()
    {
        m_drawAnchor = m_sv->snapTickDown(m_pressTick);
        m_drawTick = m_drawAnchor;
        m_drawDur = int64_t(m_sv->gridTicksAt(m_drawAnchor));
        m_drawKey = m_pressKey;
        m_drag = Drag::Draw;
        m_sv->clearSelection();
        ViewNote pending{};
        pending.startTick = uint32_t(m_drawTick);
        pending.endTick = uint32_t(m_drawTick + uint64_t(m_drawDur));
        pending.key = uint8_t(m_drawKey);
        pending.velocity = m_lastVelocity;
        pending.track = uint8_t(m_sv->selectedTrack());
        m_sv->announceNote(pending);
        // The empty-space press already sounds this row; don't re-attack it.
        if (m_soundingKey != m_drawKey)
            auditionKey(m_drawKey, m_lastVelocity, m_drawTick);
        m_auditioned = true;
        invalidateContent();
    }

    QRectF noteRect(qreal x0, qreal x1, int key) const
    {
        const qreal pixel = physicalPixel();
        return QRectF(x0, keyTop(key) + pixel, std::max<qreal>(2.0, x1 - x0),
                      std::max(2.0 * pixel, keyBottom(key) - keyTop(key) - pixel));
    }

    QRectF noteRect(const ViewNote &note) const
    {
        const qreal dpr = devicePixelRatioF();
        return noteRect(m_sv->displayX(double(note.startTick), kKeyboardW, dpr),
                        m_sv->displayX(double(note.endTick), kKeyboardW, dpr), note.key);
    }

    // The painted box: flush with the note's end on the right so
    // consecutive notes abut (their black borders separate them), one
    // pixel short on the bottom so the row hairline stays visible.
    QRectF noteBox(const QRectF &rect) const { return rect.adjusted(0, 0, 0, -physicalPixel()); }

    // The height a velocity value may occupy: the note box, never the full
    // row pitch (rounding the pitch up would let digit ink cross the box's
    // bottom border into the hairline gap).
    int velocityLabelHeight() const { return int(std::floor(m_sv->keyHeight() - physicalPixel())); }

    // Topmost (last-drawn) note of the selected track under pos. The rect is
    // widened a little on both sides so the edge resize handles can be
    // grabbed from just outside the note. When that outer reach lands inside
    // a neighboring note that has pos on one of its own edge grips (abutting
    // notes), the neighbor wins: each side of the shared boundary resizes
    // its own note.
    const ViewNote *hitNote(QPointF pos) const
    {
        const int selected = m_sv->selectedTrack();
        const ViewNote *hit = nullptr;
        bool hitInside = false;
        const ViewNote *gripHit = nullptr; // pos inside the note, on an edge grip
        const qreal reach = kEdgeGripReach;
        for (const ViewNote &note : m_sv->model().notes) {
            if (note.track != selected)
                continue;
            const QRectF r = noteRect(note);
            if (pos.y() < r.top() || pos.y() >= r.bottom())
                continue;
            const bool inside = pos.x() >= r.left() && pos.x() < r.right();
            if (inside || (pos.x() >= r.left() - reach && pos.x() < r.right() + reach)) {
                hit = &note;
                hitInside = inside;
            }
            if (inside && (nearRightEdge(note, pos) || nearLeftEdge(note, pos)))
                gripHit = &note;
        }
        return (gripHit && !hitInside) ? gripHit : hit;
    }

    bool nearRightEdge(const ViewNote &note, QPointF pos) const
    {
        const QRectF r = noteRect(note);
        return pos.x() >= r.right() - edgeGripInnerReach(r) &&
               pos.x() <= r.right() + kEdgeGripReach;
    }

    bool nearLeftEdge(const ViewNote &note, QPointF pos) const
    {
        const QRectF r = noteRect(note);
        return pos.x() >= r.left() - kEdgeGripReach && pos.x() <= r.left() + edgeGripInnerReach(r);
    }

    bool nearVelocityHandle(const ViewNote &note, QPointF pos) const
    {
        if (m_sv->keyHeight() < kVelHandleMinKeyH)
            return false;
        const QRectF r = noteRect(note);
        // The bar itself is 1-2px; grab within a few pixels of it, more
        // generously on taller notes.
        const QRectF bar = velBarRect(r, note.velocity, devicePixelRatioF());
        const qreal pad =
            std::clamp(qRound(r.height() / physicalPixel()) / 6, 2, 4) * physicalPixel();
        const qreal inner = edgeGripInnerReach(r);
        return pos.x() > r.left() + inner && pos.x() < r.right() - inner &&
               pos.y() >= bar.top() - pad && pos.y() < bar.bottom() + pad;
    }

    // Resolves the current selection to document notes (skips stale ids).
    std::vector<DocNote> resolveSelection() const
    {
        std::vector<DocNote> notes;
        SongDocument *doc = m_sv->document();
        if (!doc)
            return notes;
        for (const SongView::NoteKey &id : m_sv->selection()) {
            DocNote note;
            if (doc->findNote(m_sv->selectedTrack(), id.tick, id.key, &note))
                notes.push_back(note);
        }
        return notes;
    }

    // Ctrl+Up/Down (Shift: octave). Transposes keep intervals: if any
    // selected note would clamp at the key range, the whole move is a
    // no-op. The first note sounds at its new pitch, like a vertical
    // drag; the key release ends it (keyReleaseEvent).
    void transposeSelection(int dKey)
    {
        SongDocument *doc = m_sv->document();
        const std::vector<DocNote> notes = resolveSelection();
        if (!doc || notes.empty())
            return;
        for (const DocNote &note : notes) {
            const int key = int(note.key) + dKey;
            if (key < 0 || key > 127)
                return;
        }
        doc->moveNotes(notes, 0, dKey, /*mergeable=*/true);
        // Follow the notes with the selection.
        std::vector<SongView::NoteKey> ids;
        for (const DocNote &note : notes)
            ids.push_back({uint32_t(note.tick), uint8_t(int(note.key) + dKey)});
        m_sv->setSelection(std::move(ids));
        // Keep the moved notes in sight: the row the move headed toward
        // scrolls into view just enough (no re-centering).
        int edge = int(notes.front().key) + dKey;
        for (const DocNote &note : notes) {
            const int key = int(note.key) + dKey;
            edge = dKey > 0 ? std::max(edge, key) : std::min(edge, key);
        }
        m_sv->ensureKeyVisible(edge);
        auditionKey(int(notes.front().key) + dKey, notes.front().velocity, notes.front().tick);
        m_auditioned = true;
        invalidateContent();
    }

    // Ctrl+Left/Right. The earliest selected note's start moves to the
    // previous/next ruler grid line — absolute positions, like a draw or
    // edge resize, so an off-grid selection lands on the grid first —
    // and the rest keep their offsets from it.
    void nudgeSelection(bool right)
    {
        SongDocument *doc = m_sv->document();
        const std::vector<DocNote> notes = resolveSelection();
        if (!doc || notes.empty())
            return;
        uint64_t anchor = UINT64_MAX;
        for (const DocNote &note : notes)
            anchor = std::min(anchor, note.tick);
        const uint64_t snapped = right ? m_sv->snapTickUp(double(anchor) + 1.0)
                                       : m_sv->snapTickDown(double(anchor) - 1.0);
        const int64_t dTick = int64_t(snapped) - int64_t(anchor);
        if (dTick == 0)
            return;
        doc->moveNotes(notes, dTick, 0, /*mergeable=*/true);
        // Follow the notes with the selection.
        std::vector<SongView::NoteKey> ids;
        for (const DocNote &note : notes)
            ids.push_back({uint32_t(int64_t(note.tick) + dTick), note.key});
        m_sv->setSelection(std::move(ids));
        // Keep the moved notes in sight, scrolling just enough.
        uint64_t lo = UINT64_MAX, hi = 0;
        for (const DocNote &note : notes) {
            const uint64_t tick = uint64_t(int64_t(note.tick) + dTick);
            lo = std::min(lo, tick);
            hi = std::max(hi, tick + note.duration);
        }
        m_sv->ensureRangeVisible(lo, hi, right);
        invalidateContent();
    }

    // Fills the clipboard with the notes as a plain note clip (span 0,
    // additive paste), ticks relative to the block start.
    void copyNotes(const std::vector<DocNote> &notes)
    {
        uint64_t base = UINT64_MAX;
        for (const DocNote &note : notes)
            base = std::min(base, note.tick);
        SongView::Clip clip;
        SongView::ClipTrack ct{m_sv->selectedTrack(), {}};
        for (const DocNote &note : notes)
            ct.notes.push_back(
                {uint32_t(note.tick - base), note.key,
                 note.duration ? note.duration : uint32_t(m_sv->gridTicksAt(note.tick)),
                 note.velocity});
        clip.tracks.push_back(std::move(ct));
        m_sv->setClipboard(std::move(clip));
        m_sv->announce(SongView::tr("Copied %n note(s)", nullptr, int(notes.size())));
    }

    // Pastes a plain note clip onto the selected track, anchored at the edit
    // cursor (snapped to the grid), and selects the pasted notes. Range
    // clips (span > 0) are handled by SongView::pasteRangeAtEditCursor.
    void pasteAtEditCursor()
    {
        SongDocument *doc = m_sv->document();
        const SongView::Clip clip = m_sv->clipForPaste();
        if (!doc || clip.span != 0 || clip.tracks.empty() || clip.tracks.front().notes.empty())
            return;
        const uint64_t base = m_sv->snapTick(double(m_sv->editCursorTick()));
        std::vector<SongDocument::NewNote> notes;
        std::vector<SongView::NoteKey> ids;
        uint64_t end = base;
        for (const SongView::ClipNote &cn : clip.tracks.front().notes) {
            const uint64_t tick = base + cn.relTick;
            notes.push_back({tick, cn.key, cn.duration, cn.velocity});
            ids.push_back({uint32_t(tick), cn.key});
            end = std::max(end, tick + cn.duration);
        }
        doc->addNotes(m_sv->selectedTrack(), notes);
        m_sv->setSelection(std::move(ids));
        // Like pasteRangeAtEditCursor: advance the edit cursor past the pasted
        // notes so repeated Ctrl+V lays copies back-to-back, but keep the view
        // anchored on the content that just landed.
        m_sv->commitEditCursor(end);
        m_sv->ensureTickVisible(base);
        m_sv->announce(SongView::tr("Pasted %n note(s)", nullptr, int(notes.size())));
    }

    void selectAllNotes()
    {
        std::vector<SongView::NoteKey> ids;
        for (const ViewNote &note : m_sv->model().notes) {
            if (note.track == m_sv->selectedTrack())
                ids.push_back({note.startTick, note.key});
        }
        m_sv->setSelection(std::move(ids));
    }

    void drawNotes(QPainter &painter, const SongViewModel &model, int selectedTrack,
                   bool drawingGhostNotes)
    {
        const double keyHeight = m_sv->keyHeight();
        const bool velocityShortcut = keymap::Registry::instance().matchesModifier(
            QApplication::queryKeyboardModifiers(), QStringLiteral("roll.velocity_drag"));
        const bool showVelocityHandles =
            keyHeight >= kVelHandleMinKeyH || velocityShortcut || m_drag == Drag::Velocity;
        const bool showVelocityValues =
            !drawingGhostNotes && (m_drag == Drag::Velocity || velocityShortcut);
        // Velocity values are optional at tight zoom levels; never force a
        // minimum face that can clip vertically. The face fits the note box,
        // not the row pitch: the row includes the hairline gap under the box,
        // and a face fitted to the rounded pitch pushes digit ink across the
        // note's bottom border on 1x displays.
        const auto velocityFont = showVelocityValues
                                      ? velocityLabelFont(painter.font(), velocityLabelHeight())
                                      : std::optional<QFont>{};
        if (velocityFont)
            painter.setFont(*velocityFont);

        // Note-name labels use a fixed face two layout pixels below caption.
        // Each visible active-track note independently shows its label only
        // when its complete name fits with two trailing spaces; the velocity
        // shortcut replaces it with the note's velocity value.
        const auto nameFont = !drawingGhostNotes && !showVelocityValues && m_sv->noteNameMode() &&
                                      keyHeight >= kNoteNameMinKeyH
                                  ? noteNameFont(painter.font(), keyHeight - physicalPixel())
                                  : std::optional<QFont>{};
        if (nameFont)
            painter.setFont(*nameFont);

        for (size_t noteIndex = 0; noteIndex < model.notes.size(); ++noteIndex) {
            const ViewNote &note = model.notes[noteIndex];
            const bool isGhostNote = note.track != selectedTrack;
            if (isGhostNote != drawingGhostNotes)
                continue;
            const QRectF noteRect = displayedNoteRect(note);
            if (noteRect.right() < kKeyboardW || noteRect.left() > width())
                continue;
            if (noteRect.bottom() < 0 || noteRect.top() > height())
                continue;

            const QRectF noteBox = this->noteBox(noteRect);
            if (isGhostNote) {
                painter.fillRect(noteBox, ghostNoteColor(note.track, isBlackKey(note.key)));
                continue;
            }

            int renderedVelocity = note.velocity;
            if (m_drag == Drag::Velocity && m_sv->isSelected(note)) {
                renderedVelocity = std::clamp(int(note.velocity) + m_dVel, 1, 127);
            } else if (const std::optional<uint8_t> preview = m_sv->velocityLanePreview(note)) {
                // A velocity-lane gesture is holding this note: the roll shows
                // the value its release will write, so fill, handle and label
                // follow the lane as it is dragged.
                renderedVelocity = *preview;
            }
            const QColor fill = m_sv->noteFillColor(note.track, renderedVelocity);
            painter.fillRect(noteBox, fill);

            // Mixing one-third toward black in OKLab keeps the bar distinct
            // without rotating the identity hue. In velocity-color mode the
            // full-strength fill already is the identity.
            if (showVelocityHandles) {
                const QColor identity =
                    m_sv->velocityColorMode() ? fill : SongView::trackColor(note.track);
                painter.fillRect(velBarRect(noteRect, renderedVelocity, devicePixelRatioF()),
                                 mixTowardOklab(identity, Qt::black, 1.0 / 3.0));
            }
            if (nameFont)
                drawNoteName(painter, noteRect, noteBox, displayedNoteKey(note), fill);

            // While velocity is active, every current-track note shows its
            // value instead of the pitch label.
            if (showVelocityValues && velocityFont) {
                const QString velocityText = QString::number(renderedVelocity);
                if (noteRect.width() >= painter.fontMetrics().horizontalAdvance(velocityText) + 4) {
                    painter.save();
                    painter.setClipRect(noteBox, Qt::IntersectClip);
                    drawPlatedNoteText(painter, noteBox, Qt::AlignCenter, velocityText, fill,
                                       contrastingTextColor(fill));
                    painter.restore();
                }
            }

            if (m_sv->isSelected(note)) {
                const QColor selectionColor = themes::color(themes::Role::item_selected_background);
                // The ring thins before it disappears; the black border
                // insets by whatever ring actually fit. Insets are physical
                // pixels too, so fractional display scale cannot change the
                // ring or inner border thickness.
                const int ringThickness =
                    drawRectFrame(painter, noteBox, selectionColor,
                                  songview::selectionRingPixels(devicePixelRatioF()));
                if (ringThickness > 0) {
                    drawNoteBoxBorder(painter, noteBox, note.unterminated, ringThickness);
                } else {
                    // At extreme zoom there is no room for a frame plus a
                    // face. Keep the note visible as a solid selection mark.
                    painter.fillRect(noteBox, selectionColor);
                }
            } else {
                drawNoteBoxBorder(painter, noteBox, note.unterminated);
            }
        }
    }

    // The pitch label stays inside the note face. A note that cannot fit the
    // complete name with the shared Space::Two reserve remains unlabeled.
    bool noteNameFits(const QRectF &noteRect, int key, const QFontMetricsF &metrics) const
    {
        const auto textInset = lyt::space(Space::Half);
        const QString name = keyName(key);
        return noteRect.width() >=
               textInset + metrics.horizontalAdvance(name) + lyt::space(Space::Two);
    }

    void drawNoteName(QPainter &painter, const QRectF &noteRect, const QRectF &noteBox, int key,
                      const QColor &fill)
    {
        const QString name = keyName(key);
        if (!noteNameFits(noteRect, key, QFontMetricsF(painter.font())))
            return;
        const auto textInset = lyt::space(Space::Half);
        const QRectF labelRect(noteBox.left() + textInset, noteBox.top() + textInset, 512.0,
                               noteBox.height() - 2.0 * textInset);
        painter.save();
        painter.setClipRect(noteBox, Qt::IntersectClip);
        drawPlatedNoteText(painter, labelRect, Qt::AlignLeft | Qt::AlignVCenter, name, fill,
                           contrastingTextColor(fill));
        painter.restore();
    }

    // The pending note of a draw gesture, solid like the real note. (Move and
    // resize gestures need no extra pass: drawNotes paints the selected notes
    // at their dragged geometry via displayedNoteRect.)
    void drawDragPreview(QPainter &p, const SongViewModel &model, int selected)
    {
        Q_UNUSED(model);
        if (m_drag != Drag::Draw)
            return;
        const qreal dpr = p.device()->devicePixelRatioF();
        const qreal x0 = m_sv->displayX(double(m_drawTick), kKeyboardW, dpr);
        const qreal x1 = m_sv->displayX(double(m_drawTick + uint64_t(m_drawDur)), kKeyboardW, dpr);
        const QRectF r = noteRect(x0, x1, m_drawKey);
        const QRectF box = noteBox(r);
        const QColor fill = m_sv->noteFillColor(selected, m_lastVelocity);
        p.fillRect(box, fill);
        drawNoteBoxBorder(p, box, false);
        // While the velocity shortcut is held, the pending note follows the
        // same value-instead-of-pitch policy as existing notes.
        const auto &keys = keymap::Registry::instance();
        if (keys.matchesModifier(QApplication::queryKeyboardModifiers(),
                                 QStringLiteral("roll.velocity_drag"))) {
            if (const auto font = velocityLabelFont(p.font(), velocityLabelHeight())) {
                p.setFont(*font);
                const auto velocityText = QString::number(m_lastVelocity);
                if (r.width() >= p.fontMetrics().horizontalAdvance(velocityText) + 4) {
                    p.save();
                    p.setClipRect(box, Qt::IntersectClip);
                    drawPlatedNoteText(p, box, Qt::AlignCenter, velocityText, fill,
                                       contrastingTextColor(fill));
                    p.restore();
                }
            }
        } else if (m_sv->noteNameMode()) {
            // The pencil's live pitch readout: unlike settled labels it must
            // stay visible while the gesture chooses a pitch, so it skips the
            // fit rules — the plate keeps it readable where it overruns the
            // pending note or a short row.
            p.setFont(fixedNoteNameFont(p.font()));
            const QRectF labelRect(box.left() + lyt::space(Space::Half), r.top(), 512.0,
                                   r.height());
            drawPlatedNoteText(p, labelRect, Qt::AlignLeft | Qt::AlignVCenter, keyName(m_drawKey),
                               fill, contrastingTextColor(fill));
        }
    }

    // Where the note sits on screen right now: its stored geometry, displaced
    // by the live move/resize deltas when it's part of the gesture. Mirrors
    // the clamping applied on release in mouseReleaseEvent.
    QRectF displayedNoteRect(const ViewNote &note) const
    {
        const bool dragging =
            m_drag == Drag::Move || m_drag == Drag::Resize || m_drag == Drag::ResizeLeft;
        if (!dragging || !m_sv->isSelected(note))
            return noteRect(note);
        int64_t tick, endTick;
        if (m_drag == Drag::ResizeLeft) {
            // The note-off pins the gesture; only the start moves.
            endTick = int64_t(note.endTick);
            tick = std::clamp<int64_t>(int64_t(note.startTick) + m_dTick, 0, endTick - 1);
        } else {
            tick = std::max<int64_t>(0, int64_t(note.startTick) + m_dTick);
            endTick = std::max<int64_t>(tick + 1, int64_t(note.endTick) + m_dTick + m_dDur);
        }
        const int key = displayedNoteKey(note);
        const qreal dpr = devicePixelRatioF();
        const qreal x0 = m_sv->displayX(double(tick), kKeyboardW, dpr);
        const qreal x1 = m_sv->displayX(double(endTick), kKeyboardW, dpr);
        return noteRect(x0, x1, key);
    }

    // The pitch row the note occupies on screen right now — its stored key,
    // displaced by the live move delta when it's part of the gesture.
    int displayedNoteKey(const ViewNote &note) const
    {
        const bool dragging =
            m_drag == Drag::Move || m_drag == Drag::Resize || m_drag == Drag::ResizeLeft;
        if (!dragging || !m_sv->isSelected(note))
            return note.key;
        return std::clamp(int(note.key) + m_dKey, 0, 127);
    }

    void showNoteMenu(QPointF localPos)
    {
        SongDocument *doc = m_sv->document();
        if (!doc)
            return;
        const std::vector<DocNote> notes = resolveSelection();
        if (notes.empty())
            return;
        m_noteMenu->showMenuAt(mapToGlobal(localPos.toPoint()), notes.front().velocity);
    }

    // Retargets the open note menu to the note under an outside right-click.
    // Returns false when nothing was hit (empty space, the keyboard strip,
    // another widget) so the caller can dismiss the popup instead.
    bool moveNoteMenu(QPointF globalPos)
    {
        const QPointF pos = globalPos - QPointF(mapToGlobal(QPoint(0, 0)));
        const ViewNote *hit = m_sv->document() && pos.x() >= kKeyboardW ? hitNote(pos) : nullptr;
        if (!hit)
            return false;
        if (!m_sv->isSelected(*hit))
            m_sv->setSelection({{hit->startTick, hit->key}});
        showNoteMenu(pos);
        invalidateContent();
        return true;
    }

    void handleNoteMenuChoice(NoteMenuChoice choice)
    {
        SongDocument *doc = m_sv->document();
        if (!doc)
            return;
        const std::vector<DocNote> notes = resolveSelection();
        if (notes.empty())
            return;
        switch (choice) {
        case NoteMenuChoice::Copy:
            copyNotes(notes);
            break;
        case NoteMenuChoice::Cut:
            copyNotes(notes);
            doc->deleteNotes(notes);
            m_sv->clearSelection();
            break;
        case NoteMenuChoice::Velocity: {
            bool ok = false;
            const int velocity = QInputDialog::getInt(this, SongView::tr("Note velocity"),
                                                      SongView::tr("Velocity (1-127):"),
                                                      notes.front().velocity, 1, 127, 1, &ok);
            if (ok) {
                doc->setNotesVelocity(notes, uint8_t(velocity));
                m_lastVelocity = uint8_t(velocity);
            }
            break;
        }
        case NoteMenuChoice::Delete:
            doc->deleteNotes(notes);
            m_sv->clearSelection();
            break;
        case NoteMenuChoice::None:
            break;
        }
    }

    void drawKeyboard(QPainter &p)
    {
        const int keyH = int(std::lround(m_sv->keyHeight()));
        p.fillRect(QRect(0, 0, kKeyboardW, height()),
                   themes::color(themes::Role::song_view_piano_keyboard_natural_key));
        // Natural-key labels disappear when no real font face fits the lane.
        const auto labelFont = typography::fitted(p.font(), keyH);
        if (labelFont)
            p.setFont(*labelFont);
        const int hovered = m_hoverKey;
        const QPen separatorPen(themes::color(themes::Role::song_view_piano_keyboard_separator), 0);
        const auto hoverGeometry = keyboardHoverGeometry(hovered);
        for (int key = 0; key < 128; key++) {
            const QRectF keyRect = this->keyRect(key, 0, kKeyboardW);
            if (keyRect.bottom() <= 0 || keyRect.top() >= height())
                continue;
            const bool sounding = key == m_soundingKey;
            if (isBlackKey(key)) {
                p.fillRect(keyRect,
                           sounding
                               ? themes::color(themes::Role::song_view_piano_keyboard_active_key)
                               : themes::color(themes::Role::song_view_piano_keyboard_black_key));
            } else {
                if (sounding) {
                    p.fillRect(keyRect,
                               themes::color(themes::Role::song_view_piano_keyboard_active_key));
                }
                // B/C and E/F are the only spots where two natural
                // keys touch, so those bottom edges get a separator.
                if (key % 12 == 0 || key % 12 == 5) {
                    p.setPen(separatorPen);
                    p.drawLine(QLineF(0, keyRect.bottom(), kKeyboardW, keyRect.bottom()));
                }
                if (key % 12 == 0) {
                    p.setPen(themes::color(themes::Role::song_view_piano_keyboard_label));
                    if (labelFont) {
                        p.drawText(QRectF(0, keyRect.top(), kKeyboardW - 3, keyRect.height()),
                                   Qt::AlignRight | Qt::AlignVCenter, keyName(key));
                    }
                }
            }
            if (key == hovered && !sounding && hoverGeometry) {
                QColor h = m_sv->palette().color(QPalette::Highlight);
                h.setAlpha(80);
                p.fillRect(hoverGeometry->highlightRect, h);
            }
        }
        // Note-name chip on the hovered row: keys can be as short as 4px,
        // so the name gets its own fixed-size readout instead of in-row
        // text, vertically clamped so edge rows stay readable.
        if (hoverGeometry) {
            p.setFont(hoverGeometry->chipFont);
            p.save();
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0x30, 0x30, 0x30, 230));
            p.drawRoundedRect(hoverGeometry->chipRect, 3, 3);
            p.setPen(Qt::white);
            p.drawText(hoverGeometry->chipRect, Qt::AlignCenter, hoverGeometry->name);
            p.restore();
        }
        p.setPen(themes::color(themes::Role::song_view_separator));
        p.drawLine(0, 0, 0, height());
    }

    // Selects the selected track's notes intersecting the band rect.
    // Ableton-style sweep audition: each note sounds the moment the rubber
    // band first covers it and stops when the band leaves it (its own
    // length is the ceiling), so sweeping across a chord hears its notes
    // together without long notes ringing on. A note swept out and back in
    // re-auditions.
    void auditionBandEntrants(const QRectF &band)
    {
        std::vector<SongView::NoteKey> inBand;
        for (const ViewNote &note : m_sv->model().notes) {
            if (note.track != m_sv->selectedTrack() || !noteRect(note).intersects(band))
                continue;
            const SongView::NoteKey id{note.startTick, note.key};
            if (std::find(m_bandAud.begin(), m_bandAud.end(), id) == m_bandAud.end())
                m_sv->auditionTimed(note.track, note.key, note.velocity, note.startTick,
                                    note.endTick);
            inBand.push_back(id);
        }
        for (const SongView::NoteKey &old : m_bandAud) {
            if (std::find(inBand.begin(), inBand.end(), old) != inBand.end())
                continue;
            // Previews are one-per-key: keep the key sounding while the band
            // still covers another note of the same pitch.
            const bool keyCovered =
                std::any_of(inBand.begin(), inBand.end(),
                            [&](const SongView::NoteKey &id) { return id.key == old.key; });
            if (!keyCovered)
                m_sv->auditionTimedOff(m_sv->selectedTrack(), old.key);
        }
        m_bandAud = std::move(inBand);
    }

    // Release every preview the band still covers (drag ended or cancelled).
    void stopBandAuditions()
    {
        for (const SongView::NoteKey &id : m_bandAud)
            m_sv->auditionTimedOff(m_sv->selectedTrack(), id.key);
        m_bandAud.clear();
    }

    void selectBand(const QRectF &band, bool additive)
    {
        std::vector<SongView::NoteKey> ids =
            additive ? m_sv->selection() : std::vector<SongView::NoteKey>();
        for (const ViewNote &note : m_sv->model().notes) {
            if (note.track != m_sv->selectedTrack())
                continue;
            if (!noteRect(note).intersects(band))
                continue;
            const SongView::NoteKey id{note.startTick, note.key};
            if (std::find(ids.begin(), ids.end(), id) == ids.end())
                ids.push_back(id);
        }
        m_sv->setSelection(std::move(ids));
    }

    SongView *m_sv;
    MidiCursors m_cursors;
    mutable std::array<qreal, 129> m_rowEdges{};
    mutable qreal m_rowEdgesDpr = 0.0;
    mutable qreal m_rowEdgesKeyHeight = 0.0;
    mutable qreal m_rowEdgesScrollY = 0.0;
    mutable bool m_rowEdgesValid = false;
    Drag m_drag = Drag::None;
    QPointF m_pressPos;
    QPointF m_curPos;
    double m_pressTick = 0.0;
    int m_pressKey = 0;
    uint64_t m_gripTick = 0;     // edge tick grabbed by a resize drag
    uint64_t m_gripOpposite = 0; // the note's other edge (the pivot)
    int64_t m_dTick = 0;
    int m_dKey = 0;
    int64_t m_dDur = 0;
    int m_dVel = 0;
    uint64_t m_drawTick = 0; // pending note of a draw gesture
    int64_t m_drawDur = 0;
    int m_drawKey = 0;                        // follows the cursor vertically mid-draw
    uint64_t m_drawAnchor = 0;                // grid cell pressed; drags pivot around it
    bool m_leftPress = false;                 // left button held on empty space; cursor
                                              // move vs. draw undecided
    bool m_rightPress = false;                // right button held; band vs. menu undecided
    bool m_rightShift = false;                // …with Shift: drag sweeps a time selection
    uint64_t m_rightAnchorTick = 0;           // snapped tick of the right press
    bool m_rightHit = false;                  // that press landed on a note…
    SongView::NoteKey m_rightHitId{};         // …this one
    std::vector<SongView::NoteKey> m_bandAud; // notes the band currently
                                              // covers; entrants audition
    ViewNote m_velAnchor{};                   // pressed note of a velocity drag (a copy)
    int m_velAudEff = -1;                     // last effective velocity auditioned mid-drag
    bool m_velModPress = false;               // velocity-modifier press on a note; click
                                              // vs. vertical velocity drag undecided
    Qt::KeyboardModifiers m_velModMods = Qt::NoModifier; // that press's chord
    bool m_velModDrag = false;                           // the live drag grew from that press
    NoteId m_velReplaceNote{};                           // one-shot (unassigned = disarmed): a
                                                         // modifier drag committed during the
                                                         // still-held chord on this note; the
                                                         // next such drag on a different note
                                                         // replaces the selection. NoteId, not
                                                         // {tick,key}: the anchor must stay
                                                         // recognizable after a mid-hold
                                                         // transpose/nudge re-keys the selection
    int m_kbdKey = -1;                         // key sounding from a keyboard-column press
    int m_soundingKey = -1;                    // auditioned key highlighted on the keyboard
    int m_soundingVol = -1;                    // VOL byte it was sounded under (-1 = track's)
    int m_soundingPan = M4A_AUDITION_PAN_NONE; // PAN it was sounded under
    int m_hoverKey = -1;                       // key row under the cursor; -1 = no mark
    bool m_auditioned = false;                 // a drag/draw preview note is sounding
    uint8_t m_lastVelocity = 100;              // latches to touched/velocity-edited notes
    bool m_panning = false;                    // middle-drag pan
    QPointF m_panPos;                          // last pan sample, global coords
    NoteContextMenu *m_noteMenu = nullptr;
};

// ----------------------------------------------------------- AutomationArea

// Shift axis lock for dragging an existing point, ported from the source
// branch's automationgesture.cpp (resolveAxisLock/applyAxisLock). The first
// travel past the activation distance picks the axis by dominant direction:
// Time freezes the value and lets the tick follow the cursor, Value the
// reverse. Sticky once resolved — wobbling past 45° later doesn't flip it;
// releasing Shift frees the drag, and re-pressing re-resolves from the
// total travel since the press.
enum class AxisLock { None, Time, Value };

static AxisLock resolveAxisLock(AxisLock current, bool shiftHeld, const QPointF &origin,
                                const QPointF &position, int activationDistance)
{
    if (!shiftHeld)
        return AxisLock::None;
    if (current != AxisLock::None)
        return current;
    const qreal dx = position.x() - origin.x();
    const qreal dy = position.y() - origin.y();
    if (std::abs(dx) + std::abs(dy) < qreal(activationDistance))
        return AxisLock::None;
    return std::abs(dx) >= std::abs(dy) ? AxisLock::Time : AxisLock::Value;
}

static void applyAxisLock(AxisLock lock, uint64_t originalTick, int originalValue, uint64_t *tick,
                          int *value)
{
    if (lock == AxisLock::Time)
        *value = originalValue;
    else if (lock == AxisLock::Value)
        *tick = originalTick;
}

// Display name of a CC/bend lane derived from its identity, for the sites
// with no live model lane to carry one (empty-lane merge, lane menus).
static QString laneDisplayName(uint8_t cc)
{
    if (cc == LANE_CC_BEND)
        return m4aLaneName(M4aLane::PitchBend);
    return QString::fromLatin1(m4aClassifyCc(cc).display);
}

class AutomationArea : public TimelineSurface
{
  public:
    AutomationArea(SongView *sv, QScrollArea *scroll)
        : TimelineSurface(nullptr)
        , m_sv(sv)
        , m_scroll(scroll) // parented by the scroll area
    {
        setObjectName(QStringLiteral("automationArea")); // findChild for tests
        setProperty("hoverNodeTick", qlonglong(-1));     // test mirror, like hoverKey
        setMinimumHeight(kLaneH);
        setMouseTracking(true); // divider hover cursor
        // Range shortcuts (copy/cut/delete/paste on the time selection) work
        // from the lanes area too; a click focuses it, like the roll.
        setFocusPolicy(Qt::ClickFocus);
        m_pointMenu = new ui::ContextMenu(
            this, [this](QPointF globalPos) { return movePointMenu(globalPos); });
        m_pointMenu->setObjectName(QStringLiteral("automationPointMenu")); // findChild for tests
        m_pointSetValue = m_pointMenu->addAction(SongView::tr("Set value…"));
        m_pointDelete = m_pointMenu->addAction(SongView::tr("Delete"));
        connect(m_pointMenu, &QMenu::triggered, this,
                [this](QAction *action) { handlePointMenuAction(action); });
        // The aimed node's ring lifts with the popup (the aim itself must
        // survive the hide: QMenu hides before it emits triggered).
        connect(m_pointMenu, &QMenu::aboutToHide, this, [this] { invalidateContent(); });
    }

    // View-state plumbing for the .porydaw sidecar: the shared row height
    // plus the individually-resized rows (keyed by rowKey). laneH <= 0
    // resets to the default.
    int laneHeight() const { return m_laneH; }
    const QHash<QString, int> &rowHeightOverrides() const { return m_rowHeights; }

    // Per-lane value-axis zoom (rowKey → display max; 0 = auto-fit). Same
    // sidecar plumbing as the row heights.
    const QHash<QString, int> &rowRangeOverrides() const { return m_rowRanges; }
    void setRowRanges(const QHash<QString, int> &ranges)
    {
        m_rowRanges.clear();
        for (auto it = ranges.begin(); it != ranges.end(); ++it)
            m_rowRanges.insert(it.key(), std::clamp(it.value(), 0, 127));
        invalidateContent();
    }
    void setLaneRange(int track, uint8_t cc, int value)
    {
        if (value == laneRangeDefault(cc))
            m_rowRanges.remove(laneKey(track, cc));
        else
            m_rowRanges.insert(laneKey(track, cc), std::clamp(value, 0, 127));
        invalidateContent();
    }

    // Hidden CC lanes (laneKey strings): the rows drop out of rebuildRows,
    // the lane data untouched. Same sidecar plumbing as the row heights,
    // including their track-move policy — a moved track's keys go stale and
    // sit harmlessly rather than follow the track.
    const QSet<QString> &hiddenLaneKeys() const { return m_hiddenLanes; }
    void setHiddenLanes(const QSet<QString> &keys)
    {
        m_hiddenLanes = keys;
        rebuildRows(); // the rows leave/return now, not on a later rebuild
    }
    // The global tempo row, hidden by default (SongView::tempoLaneVisible).
    bool tempoLaneVisible() const { return m_tempoLaneVisible; }
    void setTempoLaneVisible(bool visible)
    {
        if (m_tempoLaneVisible == visible)
            return;
        // Same rule as hiding a CC lane: a range edit must never touch an
        // invisible row's events, so a lane-scope selection covering the
        // row goes with it.
        if (!visible && m_sv->timeSelectionCoversRow(-1, DOC_CC_TEMPO))
            m_sv->clearTimeSelection();
        m_tempoLaneVisible = visible;
        rebuildRows();
    }

    // Pencil mode (automation.pencil_mode, default B): a left drag always
    // freehand-draws — never a point grab or a Shift ramp — and holding
    // Shift locks the stroke to a horizontal line. Session view state, not
    // persisted to the sidecar.
    bool pencilMode() const { return m_pencilMode; }
    void setPencilMode(bool enabled)
    {
        if (m_pencilMode == enabled)
            return;
        // The hover geometry depends on the mode (the ring is arrow-only),
        // so the hover clears while the flag still matches what was
        // painted: the erase region is re-derived, and deriving it under
        // the new mode strands ghost ring pixels in the content cache.
        clearHover();
        m_pencilMode = enabled;
        // A live gesture owns the cursor (split/closed-hand/drag); the next
        // idle move re-derives it. Mid-gesture semantics are untouched too:
        // the press latched the tool into m_gesturePencil.
        if (!gestureActive())
            setCursor(modeCursor());
        m_sv->announce(enabled ? SongView::tr("Pencil mode on") : SongView::tr("Pencil mode off"));
    }

    // A mouse gesture is live (pan, point/sweep/line edit, row resize,
    // right-press sweep); the playhead follow-scroll pauses while one runs
    // so the view doesn't jump under the cursor.
    bool gestureActive() const
    {
        return m_panning || m_gesture != Gesture::None || m_dragRow >= 0 || m_resizeRow >= 0 ||
               m_rightPress;
    }
    void setViewHeights(int laneH, const QHash<QString, int> &overrides)
    {
        m_laneH = laneH > 0 ? std::clamp(laneH, kMinLaneH, kMaxLaneH) : kLaneH;
        m_rowHeights.clear();
        for (auto it = overrides.begin(); it != overrides.end(); ++it)
            m_rowHeights.insert(it.key(), std::clamp(it.value(), kMinLaneH, kMaxLaneH));
        applyHeight();
        invalidateContent();
    }

    void rebuildRows()
    {
        m_rows.clear();
        m_dragRow = -1;
        m_resizeRow = -1;
        m_gesture = Gesture::None;
        clearAxisLock();
        m_group.clear();
        m_sweep.clear();
        m_sweepArmed = false;
        m_pointShiftSeen = false;
        m_pointTraveled = false;
        m_rightPress = false;
        m_selSweep = false;
        m_hoverRow = -1;
        // The hover reset bypasses clearHover (the rows repaint wholesale
        // anyway), so the test mirror resets here too — otherwise it would
        // advertise a phantom node hover until the next mouse move.
        setProperty("hoverNodeTick", qlonglong(-1));
        // A rebuild means the rows (or the document behind them) changed:
        // the open point menu's aim is stale, so it closes like every other
        // live interaction here.
        if (m_pointMenu && m_pointMenu->isVisible())
            m_pointMenu->hide();
        m_pointMenuTarget.reset();
        if (m_sv->timeline()) {
            if (m_tempoLaneVisible)
                m_rows.push_back({Row::Tempo});
            const SongViewModel &model = m_sv->model();
            const int selected = m_sv->selectedTrack();
            // The voice row shows whenever the track has changes; with a
            // document attached it is always present as the place to add one.
            bool voiceRow = m_sv->document() != nullptr;
            for (const VoiceChange &vc : model.voices) {
                if (vc.track == selected) {
                    voiceRow = true;
                    break;
                }
            }
            if (voiceRow)
                m_rows.push_back({Row::Voice});
            for (const AutoLane &lane : model.lanes)
                if (lane.track == selected && !m_hiddenLanes.contains(laneKey(lane.track, lane.cc)))
                    m_rows.push_back({Row::Lane, lane.track, lane.cc});
        }
        applyHeight();
        invalidateContent();
    }

  protected:
    void paintContent(QPainter &p) override
    {
        const qreal dpr = p.device()->devicePixelRatioF();
        p.fillRect(rect(), themes::color(themes::Role::song_view_piano_roll_background));
        if (!m_sv->timeline())
            return;

        // With two or more nodes selected the edit clearly spans lanes, so
        // the unselected lanes' nodes recede; a lone selected node keeps
        // every lane at full strength (its ring is emphasis enough).
        const bool dimUnselected = selectedNodeCount(2) >= 2;
        int rowY = 0;
        for (size_t i = 0; i < m_rows.size(); i++) {
            const int h = rowHeight(m_rows[i]);
            paintRow(p, m_rows[i], int(i), QRect(0, rowY, width(), h), dimUnselected);
            rowY += h;
        }

        if (m_sv->document()) {
            const QRect strip = addLaneRect();
            p.setPen(themes::color(themes::Role::song_view_add_automation_lane_action));
            p.drawText(strip.adjusted(8, 0, -8, 0), Qt::AlignLeft | Qt::AlignVCenter,
                       SongView::tr("+ Add lane"));
        }

        // Group drag preview: every affected row's curve redrawn with the
        // selected nodes at their pending positions, so a cross-lane move
        // is visible in full before it commits.
        if (m_gesture == Gesture::Point && !m_group.empty() && m_dragRow >= 0) {
            const int64_t dTick = int64_t(m_dragTick) - m_dragOrigTick;
            const int dValue = m_dragValue - m_dragOrigValue;
            p.save();
            p.setPen(QPen(themes::color(themes::Role::song_view_edit_preview_outline), 1));
            p.setBrush(Qt::NoBrush);
            auto tickX = [&](uint64_t t) { return m_sv->displayX(double(t), kGutterW, dpr); };
            for (int ri = 0; ri < int(m_rows.size()); ri++) {
                const RowPreview preview = previewRow(ri, dTick, dValue);
                if (preview.moved.empty())
                    continue;
                auto valueY = [&](int v) { return valueYFor(ri, v, preview.minV, preview.maxV); };
                p.save();
                p.setClipRect(
                    QRect(kGutterW, rowTop(ri), width() - kGutterW, rowHeight(m_rows[ri])),
                    Qt::IntersectClip);
                for (size_t i = 0; i < preview.curve.size(); i++) {
                    const int y = valueY(preview.curve[i].second);
                    // The trailing hold ends where paintCurve's does
                    // (plot.right()), so the preview never paints a column
                    // the committed curve won't.
                    const qreal xNext = i + 1 < preview.curve.size()
                                            ? tickX(preview.curve[i + 1].first)
                                            : qreal(width() - 1);
                    p.drawLine(QLineF(tickX(preview.curve[i].first), y, xNext, y));
                    if (i + 1 < preview.curve.size())
                        p.drawLine(QLineF(xNext, y, xNext, valueY(preview.curve[i + 1].second)));
                }
                // Mark each mover's destination, like the grabbed marker.
                for (const std::pair<uint64_t, int> &moved : preview.moved)
                    paintNode(p, themes::color(themes::Role::song_view_edit_preview_outline),
                              QPointF(tickX(moved.first), valueY(moved.second)));
                p.restore();
            }
            p.restore();
        }

        // Drag preview: the pending stream (sweep) or ramp (line), plus a
        // marker with the value the gesture will commit at the cursor. A
        // sweep still inside its activation slop is a click until proven
        // otherwise, and a click commits nothing — so it shows nothing.
        if (m_dragRow >= 0 && m_dragRow < int(m_rows.size()) &&
            (m_gesture != Gesture::Sweep || m_sweepArmed)) {
            int minV, maxV;
            rowRange(m_rows[m_dragRow], &minV, &maxV);
            auto valueY = [&](int v) { return valueYFor(m_dragRow, v, minV, maxV); };
            auto tickX = [&](uint64_t t) { return m_sv->displayX(double(t), kGutterW, dpr); };
            const QRect plot(kGutterW, rowTop(m_dragRow), width() - kGutterW,
                             rowHeight(m_rows[m_dragRow]));
            p.save();
            p.setClipRect(plot, Qt::IntersectClip);
            p.setPen(QPen(themes::color(themes::Role::song_view_edit_preview_outline), 1));
            p.setBrush(Qt::NoBrush);
            // A pencil stroke's own row already painted the whole rewrite
            // (see paintPencilPreview), so only the arrow tool's sweep needs
            // its stream traced here.
            if (m_gesture == Gesture::Sweep && !m_gesturePencil && m_sweep.size() > 1) {
                // Hold-value steps, like paintCurve draws committed points.
                for (size_t i = 0; i + 1 < m_sweep.size(); i++) {
                    const int y = valueY(m_sweep[i].second);
                    p.drawLine(QLineF(tickX(m_sweep[i].first), y, tickX(m_sweep[i + 1].first), y));
                    p.drawLine(QLineF(tickX(m_sweep[i + 1].first), y, tickX(m_sweep[i + 1].first),
                                      valueY(m_sweep[i + 1].second)));
                }
            } else if (m_gesture == Gesture::Line) {
                p.drawLine(QLineF(tickX(m_lineStartTick), valueY(m_lineStartValue),
                                  tickX(m_dragTick), valueY(m_dragValue)));
            }
            const qreal x = tickX(m_dragTick);
            const int y = valueY(m_dragValue);
            paintNode(p, themes::color(themes::Role::song_view_edit_preview_outline),
                      QPointF(x, y));
            // The committed value rides on a filled chip (backdrop + text)
            // clamped inside the row's plot, so it stays legible over the
            // curve and never leaves the row at its edges. In a group drag
            // the marker above is the grabbed node, so the chip follows it.
            const QString chipText = formatRowValue(m_rows[m_dragRow], m_dragValue);
            const QFont chipFont = typography::caption(font());
            const QFontMetrics chipMetrics(chipFont);
            QRect chip(qCeil(x) + 6, y - 4 - chipMetrics.height(),
                       chipMetrics.horizontalAdvance(chipText), chipMetrics.height());
            if (chip.right() > plot.right())
                chip.moveRight(plot.right());
            if (chip.left() < plot.left())
                chip.moveLeft(plot.left());
            if (chip.top() < plot.top())
                chip.moveTop(plot.top());
            if (chip.bottom() > plot.bottom())
                chip.moveBottom(plot.bottom());
            p.setFont(chipFont);
            p.fillRect(chip.adjusted(-1, -1, 1, 1),
                       themes::color(themes::Role::song_view_piano_roll_accidental_lane));
            p.setPen(themes::color(themes::Role::song_view_primary_text));
            p.drawText(chip, Qt::AlignHCenter | Qt::AlignVCenter, chipText);
            p.restore();
        }

        paintHoverReadout(p);
    }

  private:
    struct HoverReadoutGeometry {
        std::optional<QPointF> marker; // absent on the voice row: text-only
        QPointF textBaseline;
        QFont font;
        QString text;
        QRect clipRect;
        QRegion paintRegion;
        qint64 onPointTick = -1; // >= 0: the cursor is on this point's dot,
                                 // so the marker is a ring on the dot itself
    };

    // Shared tail of the curve and voice readouts — the caption font, the
    // side-flip that keeps the text on-screen at the right edge, and the
    // padded ink bounds — one home, so the two readouts' repaint rules
    // can't drift apart. boundingRect is the FONT's idea of the ink
    // extents; the platform raster can exceed it (DirectWrite/ClearType
    // glyphs bleed a pixel or two past GDI-style metrics on Windows), and
    // any painted pixel outside the bounds becomes a permanent trail
    // behind the moving readout. Pad generously — the bounds only size a
    // repaint.
    struct ReadoutText {
        QFont font;
        QPointF baseline;
        QRect paddedBounds;
    };
    template <typename BaselineY> // qreal(const QFontMetrics &)
    ReadoutText readoutText(const QString &text, qreal anchorX, BaselineY baselineY) const
    {
        const QFont readoutFont = typography::caption(font());
        const QFontMetrics metrics(readoutFont);
        const int textWidth = metrics.horizontalAdvance(text);
        const qreal textX =
            anchorX + 6 + textWidth > width() ? anchorX - 6 - textWidth : anchorX + 6;
        const QPointF baseline(textX, baselineY(metrics));
        const QRect paddedBounds = QRectF(metrics.boundingRect(text))
                                       .translated(baseline)
                                       .toAlignedRect()
                                       .adjusted(-3, -3, 3, 3);
        return {readoutFont, baseline, paddedBounds};
    }

    std::optional<HoverReadoutGeometry> hoverReadoutGeometry(int rowIndex, double tick, qreal x,
                                                             int y) const
    {
        if (rowIndex < 0 || rowIndex >= int(m_rows.size()))
            return std::nullopt;
        const Row &row = m_rows[rowIndex];
        if (row.kind == Row::Voice)
            return voiceHoverGeometry(rowIndex, tick, x);
        const std::vector<LanePoint> *points = rowPoints(row);
        if (!points || points->empty())
            return std::nullopt;
        int minV, maxV;
        rowRange(row, &minV, &maxV);
        // On a point's dot the readout marks the point itself — a ring at
        // its exact position and value, saying "this press moves this
        // point" before the press. The hit is the left-press grab test on
        // the RAW cursor x — never one reconstructed from the tick, whose
        // pre-roll clamp (and device-pixel rounding) would let ring and
        // grab disagree. The pencil never grabs, so it never rings.
        // Off-dot, the marker rides the curve at the cursor's tick.
        const LanePoint *on = m_pencilMode ? nullptr : grabPoint(row, rowIndex, x, y);
        QPointF marker;
        int value;
        if (on) {
            value = on->value;
            marker = QPointF(m_sv->displayX(double(on->tick), kGutterW, devicePixelRatioF()),
                             valueYFor(rowIndex, value, minV, maxV));
        } else {
            const auto it =
                std::upper_bound(points->begin(), points->end(), tick,
                                 [](double t, const LanePoint &pt) { return t < double(pt.tick); });
            if (it == points->begin())
                return std::nullopt;
            value = (it - 1)->value;
            marker = QPointF(m_sv->displayX(tick, kGutterW, devicePixelRatioF()),
                             valueYFor(rowIndex, value, minV, maxV));
        }
        const QString text = formatRowValue(row, value);
        const ReadoutText readout = readoutText(text, marker.x(), [&](const QFontMetrics &metrics) {
            return std::max(marker.y() - 4, qreal(rowTop(rowIndex) + metrics.ascent() + 2));
        });
        const QRect clip(kGutterW, rowTop(rowIndex), width() - kGutterW, rowHeight(row));
        // A one-pixel antialiased ellipse stroke can cover the pixel just
        // outside its nominal three-pixel radii; the ring (radius 4.5 at
        // pen width 2) inks out to 5.5 and gets the same safety margin.
        const QRect markerBounds =
            on ? QRectF(marker.x() - 7, marker.y() - 7, 15, 15).toAlignedRect()
               : QRectF(marker.x() - 4, marker.y() - 4, 9, 9).toAlignedRect();
        const QRegion paintRegion =
            (QRegion(markerBounds) | QRegion(readout.paddedBounds)) & QRegion(clip);
        if (paintRegion.isEmpty())
            return std::nullopt;
        return HoverReadoutGeometry{
            marker,      readout.baseline,          readout.font, text, clip,
            paintRegion, on ? qint64(on->tick) : -1};
    }

    // Voice-row hover readout: "→ NNN name" for the voice in effect at the
    // cursor's tick — programAtTick, so before the first change it shows
    // the track's priming firstProgram, agreeing with the header label and
    // the engine. Suppressed within the marker hit radius: a press there
    // edits that marker, whose own label already names it.
    std::optional<HoverReadoutGeometry> voiceHoverGeometry(int rowIndex, double tick, qreal x) const
    {
        // The nearness test runs against the model's voice list —
        // voiceChangeNear resolves the same events from the document, but
        // re-scans the raw SMF track (allocating a vector) on every call,
        // too hot for a per-mouse-move path.
        const int track = m_sv->selectedTrack();
        const qreal dpr = devicePixelRatioF();
        for (const VoiceChange &vc : m_sv->model().voices) {
            if (vc.track != track)
                continue;
            // voiceChangeNear's marker hit radius.
            if (std::abs(m_sv->displayX(double(vc.tick), kGutterW, dpr) - x) < 9.0)
                return std::nullopt;
        }
        const int prog = m_sv->programAtTick(track, uint64_t(tick));
        if (prog < 0)
            return std::nullopt;
        const QString text = QStringLiteral("→ ") + m_sv->voiceLabel(uint8_t(prog));
        const Row &row = m_rows[rowIndex];
        const int top = rowTop(rowIndex);
        const int h = rowHeight(row);
        const ReadoutText readout = readoutText(text, x, [&](const QFontMetrics &metrics) {
            return qreal(top + (h + metrics.ascent() - metrics.descent()) / 2);
        });
        const QRect clip(kGutterW, top, width() - kGutterW, h);
        const QRegion paintRegion = QRegion(readout.paddedBounds) & QRegion(clip);
        if (paintRegion.isEmpty())
            return std::nullopt;
        return HoverReadoutGeometry{std::nullopt, readout.baseline, readout.font, text,
                                    clip,         paintRegion};
    }

    // Idle-hover readout: a marker on the curve with the value in effect at
    // the cursor's tick (the last point at or before it — lanes hold their
    // value until the next point, so this matches what the curve shows), a
    // ring on the dot when the cursor is on one, and the voice in effect on
    // the voice row.
    void paintHoverReadout(QPainter &p)
    {
        if (m_dragRow >= 0 || m_selSweep)
            return;
        const auto geometry = hoverReadoutGeometry(m_hoverRow, m_hoverTick, m_hoverX, m_hoverY);
        if (!geometry)
            return;
        p.save();
        p.setFont(geometry->font);
        p.setClipRect(geometry->clipRect, Qt::IntersectClip);
        p.setPen(QPen(themes::color(themes::Role::song_view_edit_preview_outline),
                      geometry->onPointTick >= 0 ? 2 : 1));
        p.setBrush(Qt::NoBrush);
        if (geometry->marker) {
            if (geometry->onPointTick >= 0) {
                // Same shape and weight as the selection ring (aliased —
                // exact pixels keep it probeable in rollcheck), in the
                // preview color so the two rings stay tellable apart.
                p.drawEllipse(*geometry->marker, 4.5, 4.5);
            } else {
                p.drawEllipse(*geometry->marker, 3, 3);
            }
        }
        p.drawText(geometry->textBaseline, geometry->text);
        p.restore();
    }

  protected:
    void wheelEvent(QWheelEvent *event) override
    {
        // Scroll/zoom moves the content under a stationary cursor, so the
        // stored hover (a content-space tick, a widget-space x) goes stale
        // both ways; the readout clears like on a press, and the next idle
        // move re-derives it.
        clearHover();
        // Same bindings as the roll's notes area: plain wheel over the plot
        // zooms the timeline; Ctrl+wheel resizes the lane rows (the roll's
        // key-height analog); Shift (or a trackpad's horizontal delta)
        // scrolls horizontally. Over the gutter the wheel pages the lane
        // list vertically via the scroll area.
        const QPoint delta = wheelDelta(event);
        const int d = delta.y() ? delta.y() : delta.x();
        if (event->modifiers() & Qt::ControlModifier) {
            zoomLaneHeight(d, int(event->position().y()));
        } else if (event->modifiers() & Qt::ShiftModifier) {
            m_sv->scrollByPx(-d);
        } else if (delta.x() && !delta.y()) {
            m_sv->scrollByPx(-delta.x());
        } else if (event->position().x() < kGutterW) {
            event->ignore();
            return;
        } else {
            const double zoomDelta = wheelAngleUnits(event);
            if (zoomDelta != 0.0)
                m_sv->zoomAroundContentX(std::pow(1.0015, zoomDelta),
                                         event->position().x() - kGutterW);
        }
        event->accept();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        // Any press starts a gesture (or a menu); the idle readout would
        // paint stale under it. The next idle move restores it.
        clearHover();
        // A fresh press ends the click-delete's double-click window (the
        // pair's second press arrives as MouseButtonDblClick, never here).
        m_clickDeleted = false;
        if (event->button() == Qt::MiddleButton) {
            // Reaper-style pan: drag scrolls the timeline and the lane list.
            // Tracked in global coords — the vertical scroll moves this
            // widget under the cursor, so local deltas would double-count.
            m_panning = true;
            m_panPos = event->globalPosition();
            setCursor(Qt::ClosedHandCursor);
            return;
        }
        const int boundary =
            event->button() == Qt::LeftButton ? rowBoundaryAt(event->pos().y()) : -1;
        if (boundary >= 0) {
            // Dragging the divider under a row gives it an individual
            // height, overriding the shared Ctrl+wheel height.
            m_resizeRow = boundary;
            m_resizeOrigH = rowHeight(m_rows[boundary]);
            m_resizePressY = event->pos().y();
            return;
        }
        SongDocument *doc = m_sv->document();
        if (!doc)
            return;
        if ((event->button() == Qt::LeftButton || event->button() == Qt::RightButton) &&
            addLaneRect().contains(event->pos())) {
            showAddLaneMenu(event->globalPosition().toPoint());
            return;
        }
        const int ri = rowIndexAt(event->pos().y());
        if (ri < 0)
            return;
        setFocus();
        const Row &row = m_rows[ri];
        if (event->position().x() < kGutterW) {
            // The menu works from the row's identity, not a live model lane,
            // so a CC row keeps its menu even when its lane left the model.
            if (event->button() == Qt::LeftButton || event->button() == Qt::RightButton) {
                if (row.kind == Row::Lane)
                    showLaneMenu(row, event->globalPosition().toPoint());
                else if (row.kind == Row::Tempo)
                    showTempoMenu(event->globalPosition().toPoint());
            }
            return;
        }
        if (event->button() == Qt::RightButton) {
            // Deferred: a drag from here sweeps a time selection across the
            // crossed rows; releasing in place context-acts (menu inside the
            // selection, point/voice-marker delete elsewhere). Resolved in
            // mouseReleaseEvent.
            m_rightPress = true;
            m_rightPressPos = event->pos();
            m_rightRow = ri;
            m_selAnchorTick = m_sv->snapTick(rawTickAt(event->position().x()),
                                             event->modifiers() & Qt::AltModifier);
            return;
        }
        if (row.kind == Row::Voice) {
            voiceRowPress(event);
            return;
        }
        uint8_t cc;
        int track;
        if (!rowTarget(row, &cc, &track))
            return;

        if (event->button() != Qt::LeftButton)
            return;
        m_dragRow = ri;
        m_dragPos = event->position();
        m_group.clear(); // every press starts single until the grab proves otherwise
        // Latch the tool for the whole gesture: toggling B mid-drag must
        // not change an in-flight stroke's semantics. A gesture during a
        // pencil-key hold also makes that hold momentary.
        m_gesturePencil = m_pencilMode;
        m_sv->markPencilKeyGesture();
        const bool fine = event->modifiers() & Qt::AltModifier;
        updateDrag(event->position().x(), event->pos().y(), fine,
                   event->modifiers() & Qt::ControlModifier);
        const LanePoint *grab =
            m_pencilMode ? nullptr : grabPoint(row, ri, event->position().x(), event->pos().y());
        if (grab) {
            // Grabbing requires hitting the point's dot (x and y), so a
            // freehand redraw over a dense curve isn't captured by every
            // cell's point — sweeping overwrites them instead. Checked
            // before the Shift ramp: on a dot, Shift means an axis-locked
            // drag (a constrained nudge of a dot is common; a ramp anchored
            // exactly on one is not — it can start anywhere off-dot).
            m_gesture = Gesture::Point;
            m_dragOrigTick = int64_t(grab->tick);
            m_dragOrigValue = grab->value;
            m_pointPressPos = event->position();
            m_axisLock = AxisLock::None;
            // Shift means an axis-locked drag, so it disarms the click-
            // delete on the release: a modifier slip must not destroy the
            // node it was aiming to constrain. Tracked for the whole
            // gesture, not just the press — Shift often lands after the
            // button, and a parked Shift press alone pins the drag back to
            // the node's original position, which would otherwise read as
            // a click.
            m_pointShiftSeen = event->modifiers() & Qt::ShiftModifier;
            m_pointTraveled = false;
            // Start from the point's exact position, not the pixel-derived
            // one: a no-motion click (or the first half of a double-click)
            // must not quantize the value to the pixel grid.
            m_dragTick = grab->tick;
            m_dragValue = grab->value;
            // Grabbing a node inside the derived selection drags the whole
            // selection: one shared dTick/dValue for every selected node,
            // taken from this one. Outside it, a single-point move as ever.
            // The selection is snapshotted with the group so the commit's
            // band-follow can tell whether the band it would shift is still
            // the one this gesture latched.
            if (pointSelected(row, grab->tick)) {
                m_group = collectSelectedNodes();
                m_groupSelAtPress = m_sv->timeSelection();
            }
        } else if (!m_pencilMode && (event->modifiers() & Qt::ShiftModifier)) {
            // Line ramp: the press anchors one end, release commits the
            // interpolated segment.
            m_gesture = Gesture::Line;
            m_lineStartTick = m_dragTick;
            m_lineStartValue = m_dragValue;
        } else {
            // Freehand sweep. With the pencil, Shift locks the stroke to a
            // horizontal line at the pressed value (the arrow tool's Shift
            // means a ramp instead); the lock itself lives in
            // mouseMoveEvent.
            m_gesture = Gesture::Sweep;
            m_dragOrigTick = -1;
            m_sweepPressPos = event->position();
            m_sweepSlopOrigin = m_sweepPressPos;
            // The pencil draws from the first pixel — that is its contract,
            // and a pencil click is meant to leave a point. The arrow tool
            // waits out the activation slop instead, so its plain click
            // writes nothing at all (the release parks the edit cursor) and
            // sub-slop hand jitter leaves the document byte-identical. The
            // first sample is seeded here only for the pencil; the arrow's
            // stroke seeds itself from m_prevTick once it arms.
            m_sweepArmed = m_gesturePencil;
            m_pencilSlopOrigin = event->position();
            m_pencilSlopExceeded = false;
            m_sweep.clear();
            if (m_sweepArmed)
                m_sweep.assign(1, {m_dragTick, m_dragValue});
            m_prevTick = rawTickAt(event->position().x());
            m_prevValue = m_dragValue;
        }
        invalidateContent();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_panning) {
            const QPointF d = event->globalPosition() - m_panPos;
            m_panPos = event->globalPosition();
            m_sv->scrollByPx(-d.x());
            if (m_scroll) {
                QScrollBar *vbar = m_scroll->verticalScrollBar();
                vbar->setValue(vbar->value() - int(d.y()));
            }
            return;
        }
        if (m_resizeRow >= 0 && m_resizeRow < int(m_rows.size())) {
            const int newH =
                std::clamp(m_resizeOrigH + event->pos().y() - m_resizePressY, kMinLaneH, kMaxLaneH);
            if (newH != rowHeight(m_rows[m_resizeRow])) {
                m_rowHeights.insert(rowKey(m_rows[m_resizeRow]), newH);
                applyHeight();
                invalidateContent();
            }
            return;
        }
        if (m_rightPress) {
            if (!m_selSweep && (event->pos() - m_rightPressPos).manhattanLength() >=
                                   QApplication::startDragDistance())
                m_selSweep = true;
            if (m_selSweep)
                updateSelSweep(event);
            return;
        }
        if (m_dragRow < 0) {
            if (rowBoundaryAt(event->pos().y()) >= 0)
                setCursor(Qt::SplitVCursor);
            else if (m_pencilMode && drawableAt(event->position().x(), event->pos().y()))
                setCursor(pencilCursor());
            else
                setCursor(Qt::ArrowCursor);
            updateHover(event->position().x(), event->pos().y());
            return;
        }
        m_dragPos = event->position();
        // Arrow-tool sweep activation: the stroke stays empty until the
        // travel passes the activation distance, and the slop itself is
        // then subtracted from every later sample — crossing the threshold
        // is what starts the stroke, not motion the stroke should draw. The
        // pencil arms at the press, so this is a no-op for it.
        qreal posX = event->position().x();
        int posY = event->pos().y();
        if (m_gesture == Gesture::Sweep) {
            if (!m_sweepArmed) {
                const QPointF travel = event->position() - m_sweepPressPos;
                const qreal distance = std::abs(travel.x()) + std::abs(travel.y());
                const qreal slop = qreal(nodeDragActivationDistance());
                if (distance < slop)
                    return;
                m_sweepArmed = true;
                // Subtract only the slop itself, never the whole first
                // sample: mouse moves arrive coalesced, so a fast flick's
                // first event can be tens of pixels past the threshold and
                // that offset would follow the stroke for its whole length.
                m_sweepSlopOrigin = m_sweepPressPos + travel * (slop / distance);
                return;
            }
            posX += m_sweepPressPos.x() - m_sweepSlopOrigin.x();
            posY += qRound(m_sweepPressPos.y() - m_sweepSlopOrigin.y());
        }
        const bool fine = event->modifiers() & Qt::AltModifier;
        // Pencil horizontal lock: while Shift is held the value stays
        // whatever the stroke held when the lock engaged (the press value
        // when Shift was already down — m_dragValue carries it between
        // events); releasing Shift resumes following the cursor. Keyed off
        // the press-latched tool, so toggling B mid-drag changes nothing.
        const bool lock = m_gesturePencil && m_gesture == Gesture::Sweep &&
                          (event->modifiers() & Qt::ShiftModifier);
        // Pencil vertical slop: a stroke meant to be horizontal shouldn't
        // pick up the hand's vertical wobble, so the value stays pinned at
        // the press until the stroke commits to going somewhere vertically.
        // Once it breaks out it follows the cursor for the rest of the
        // stroke — the resistance is a starting behavior, not a filter.
        // Shift already pins the value, so it skips the gate (and keeps the
        // origin under the cursor, so releasing Shift doesn't hand the gate
        // a stale, far-away origin that reads as an instant breakout).
        const bool slopHold = m_gesturePencil && m_gesture == Gesture::Sweep && !lock &&
                              pencilSlopHolds(event->position());
        const int heldValue = m_dragValue;
        updateDrag(posX, posY, fine, event->modifiers() & Qt::ControlModifier);
        if (lock || slopHold)
            m_dragValue = heldValue;
        if (lock && !m_pencilSlopExceeded)
            m_pencilSlopOrigin = event->position();
        if (m_gesture == Gesture::Point) {
            noteShift(event->modifiers());
            noteTravel(event->position());
            updatePointAxisLock(event->position(), event->modifiers() & Qt::ShiftModifier);
        }
        if (m_gesture == Gesture::Sweep)
            extendSweep(posX, fine);
        invalidateContent();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::MiddleButton && m_panning) {
            m_panning = false;
            setCursor(dragCursor());
            return;
        }
        if (event->button() == Qt::RightButton && m_rightPress) {
            m_rightPress = false;
            if (m_selSweep) {
                m_selSweep = false;
                if (m_sv->timeSelection().active())
                    m_sv->announceTimeSelection();
                else
                    m_sv->clearTimeSelection();
            } else {
                rightClickInPlace(event);
            }
            return;
        }
        if (event->button() == Qt::LeftButton && m_resizeRow >= 0) {
            m_resizeRow = -1;
            return;
        }
        if (event->button() != Qt::LeftButton || m_dragRow < 0 || m_dragRow >= int(m_rows.size()))
            return;
        const Row &row = m_rows[m_dragRow];
        const Gesture gesture = m_gesture;
        m_gesture = Gesture::None;
        m_dragRow = -1;
        clearAxisLock();
        std::vector<GroupNode> group = std::move(m_group);
        m_group.clear();
        invalidateContent();

        SongDocument *doc = m_sv->document();
        uint8_t cc;
        int track;
        if (!doc || !rowTarget(row, &cc, &track))
            return;
        if (gesture == Gesture::Point) {
            if (m_dragOrigTick < 0)
                return;
            if (!group.empty()) {
                // Group commit: one undoable batch move by the shared
                // deltas (m_dragTick already carries the earliest-node
                // clamp; a batch that changes nothing pushes nothing).
                const int64_t dTick = int64_t(m_dragTick) - m_dragOrigTick;
                const int dValue = m_dragValue - m_dragOrigValue;
                // A plain click on a selected node deletes the node it
                // grabbed — the rest of the selection stays put — instead
                // of committing a zero move. (moveLanePoints' own no-op
                // detection is byte-based, so a non-canonical event, say a
                // fractional-BPM tempo blob, would otherwise be silently
                // rewritten with a spurious undo entry.)
                if (dTick == 0 && dValue == 0) {
                    clickDeleteNode(track, cc);
                    return;
                }
                std::vector<SongDocument::LanePointMove> moves;
                moves.reserve(group.size());
                for (const GroupNode &n : group) {
                    const Row &nodeRow = m_rows[n.row];
                    int minV, maxV;
                    rowRange(nodeRow, &minV, &maxV);
                    moves.push_back({n.track, n.cc, n.point,
                                     uint64_t(int64_t(n.point.tick) + dTick),
                                     clampRowValue(nodeRow, n.point.value + dValue, minV, maxV)});
                }
                doc->moveLanePoints(moves);
                // The selection follows the moved nodes and republishes, so
                // a follow-up drag or Delete finds them at their new home —
                // but only while the live band is still the one this
                // gesture latched; a band re-swept mid-drag is not ours to
                // shift.
                const SongView::TimeSelection &cur = m_sv->timeSelection();
                if (dTick != 0 && cur.active() && cur.scope == m_groupSelAtPress.scope &&
                    cur.startTick == m_groupSelAtPress.startTick &&
                    cur.endTick == m_groupSelAtPress.endTick &&
                    cur.lanes == m_groupSelAtPress.lanes) {
                    SongView::TimeSelection moved = cur;
                    moved.startTick =
                        uint64_t(std::max<int64_t>(0, int64_t(moved.startTick) + dTick));
                    moved.endTick = uint64_t(std::max<int64_t>(0, int64_t(moved.endTick) + dTick));
                    if (moved.startTick < moved.endTick)
                        m_sv->setTimeSelection(moved);
                }
                return;
            }
            DocLanePoint pt;
            if (!grabbedPoint(track, cc, &pt))
                return;
            // A drag that landed nowhere is a click: it deletes the node.
            if (pt.tick == m_dragTick && pt.value == m_dragValue)
                clickDeleteNode(track, cc);
            else
                doc->moveLanePoint(track, cc, pt, m_dragTick, m_dragValue);
        } else if (gesture == Gesture::Sweep) {
            if (m_sweep.empty()) {
                // The arrow tool's plain click on empty lane space writes
                // nothing; like the roll's, it parks the edit cursor at the
                // click instead. A drag that armed the stroke but sampled
                // nothing after it (exactly the activation distance, then
                // released) commits nothing either, so it parks too — the
                // click boundary stays continuous. (A pencil sweep seeds a
                // sample at the press, so it never reaches here.)
                m_sv->commitEditCursor(m_sv->snapTick(rawTickAt(m_sweepPressPos.x())));
                return;
            }
            // writeLanePoints even for a one-sample stroke: it overwrites
            // any point already sitting on the tick instead of duplicating
            // it.
            doc->writeLanePoints(track, cc, m_sweep.front().first, m_sweep.back().first,
                                 sweepPoints(strokeSamples()));
            m_sweep.clear();
        } else if (gesture == Gesture::Line) {
            const bool fine = event->modifiers() & Qt::AltModifier;
            uint64_t a = m_lineStartTick, b = m_dragTick;
            int va = m_lineStartValue, vb = m_dragValue;
            if (a > b) {
                std::swap(a, b);
                std::swap(va, vb);
            }
            if (a == b) {
                doc->writeLanePoints(track, cc, a, a, {{a, vb}});
                return;
            }
            // Meter-aware walk (nextGridTick): each point lands on its own
            // segment's grid across a signature change, and the last
            // iteration is exactly t == b, where the interpolation yields
            // vb — the endpoint stays exact at the release tick.
            std::vector<SongDocument::LanePointValue> pts;
            for (uint64_t t = a;;) {
                pts.push_back(
                    {t, va + int(std::llround(double(vb - va) * double(t - a) / double(b - a)))});
                if (t >= b)
                    break;
                t = nextGridTick(t, fine, b);
            }
            doc->writeLanePoints(track, cc, a, b, pts);
        }
    }

    // Double-click on empty lane space: type-in for the exact value the
    // pixel grid can't hit (e.g. pan dead-center), written as an
    // overwriting single-point write at the snapped tick. On a node it is a
    // no-op — the pair's first click already deleted the node (or, with
    // Shift, grabbed it) — so a node's value type-in lives on the point
    // menu's Set value… instead.
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        SongDocument *doc = m_sv->document();
        if (!doc || event->button() != Qt::LeftButton || event->position().x() < kGutterW)
            return;
        const int ri = rowIndexAt(event->pos().y());
        if (ri < 0)
            return;
        const Row &row = m_rows[ri];
        uint8_t cc;
        int track;
        if (!rowTarget(row, &cc, &track))
            return;
        // The double-click replaced this pair's second press; drop any
        // half-open gesture so its release is a no-op.
        m_gesture = Gesture::None;
        m_dragRow = -1;
        m_group.clear();
        m_sweep.clear();
        invalidateContent();

        // On a node the pair is spent: the first click deleted it (so the
        // grab test can no longer see it — the delete flag says so
        // directly), or Shift kept it and grabbed it for an axis-locked
        // drag. Either way, no type-in. The pencil never grabs, so its
        // double-click keeps opening the type-in as before.
        const bool deleted = m_clickDeleted;
        m_clickDeleted = false;
        if (!m_pencilMode &&
            (deleted || grabPoint(row, ri, event->position().x(), event->pos().y())))
            return;

        uint64_t tick;
        int value;
        if (const LanePoint *nearPt = nearestPoint(row, event->position().x())) {
            tick = nearPt->tick;
            value = nearPt->value;
        } else {
            // The click's point can sit farther than nearestPoint's radius
            // when the snap grid is coarse; re-derive its tick the same way.
            tick = m_sv->snapTick(rawTickAt(event->position().x()),
                                  event->modifiers() & Qt::AltModifier);
            DocLanePoint pt;
            value = doc->findLanePoint(track, cc, tick, &pt) ? pt.value
                                                             : valueAtY(row, ri, event->pos().y());
        }
        if (!editValue(row, &value))
            return;
        doc->writeLanePoints(track, cc, tick, tick, {{tick, value}});
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (m_sv->handleEditKey(event))
            return;
        if (event->key() == Qt::Key_Shift && m_gesture == Gesture::Point &&
            !event->isAutoRepeat()) {
            pointDragShiftChanged(true, event->modifiers());
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Escape) {
            // A live group drag dies with the selection it came from: the
            // user just watched the band and the rings vanish, so the
            // release must not commit the latched multi-node move. (A
            // plain single-point drag keeps its longstanding behavior:
            // Escape leaves it running.)
            if (m_gesture == Gesture::Point && !m_group.empty()) {
                m_gesture = Gesture::None;
                m_dragRow = -1;
                clearAxisLock();
                m_group.clear();
                invalidateContent();
            }
            m_rightPress = false;
            m_selSweep = false;
            m_sv->clearTimeSelection();
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void keyReleaseEvent(QKeyEvent *event) override
    {
        if (m_sv->handleEditKeyRelease(event))
            return;
        if (event->key() == Qt::Key_Shift && m_gesture == Gesture::Point &&
            !event->isAutoRepeat()) {
            pointDragShiftChanged(false, event->modifiers());
            event->accept();
            return;
        }
        QWidget::keyReleaseEvent(event);
    }

    void leaveEvent(QEvent *) override { clearHover(); }

  private:
    // Lane rows carry their identity BY VALUE, never a pointer into
    // model.lanes: rows outlive a model rebuild (setSong resets view
    // heights before rebuildRows repopulates them), so a cached pointer
    // dangles the moment the model is reassigned. Anything needing the
    // live lane (points, name) resolves it through rowLane().
    struct Row {
        enum Kind { Tempo, Voice, Lane } kind;
        int track = 0;
        uint8_t cc = 0;
    };

    // One node of a live group drag: a press on a node inside the derived
    // selection moves every selected node by the grabbed node's delta.
    // Resolved at the press and safe for the gesture's lifetime — any
    // document change aborts the drag through rebuildRows.
    struct GroupNode {
        int row;   // index into m_rows, for the row's value clamp
        int track; // rowTarget's engine track (tempo: the selected track)
        uint8_t cc;
        DocLanePoint point;
    };

    const AutoLane *rowLane(const Row &row) const
    {
        if (row.kind != Row::Lane)
            return nullptr;
        for (const AutoLane &lane : m_sv->model().lanes) {
            if (lane.track == row.track && lane.cc == row.cc)
                return &lane;
        }
        return nullptr; // stale row: its lane left the model
    }

    // The lane identity a row contributes to a lane-scoped time selection:
    // (engine track, cc), with the global tempo row as track -1 so it
    // survives track switches.
    std::pair<int, uint8_t> rowIdentity(const Row &row) const
    {
        switch (row.kind) {
        case Row::Tempo:
            return {-1, DOC_CC_TEMPO};
        case Row::Voice:
            return {m_sv->selectedTrack(), DOC_CC_VOICE};
        case Row::Lane:
            return {row.track, row.cc};
        }
        return {-1, 0};
    }

    // Derived node selection: a lane point is "selected" iff its row's
    // identity is one of a lane-scoped time selection's lanes and its tick
    // is inside the half-open [startTick, endTick). Never stored, so it can
    // never go stale against the document — and the right-drag band
    // publishes the selection live, so nodes light up while the sweep is
    // still running through this same predicate.
    bool rowInLaneSelection(const Row &row) const
    {
        const SongView::TimeSelection &sel = m_sv->timeSelection();
        if (!sel.active() || sel.scope != SongView::TimeSelection::Lanes)
            return false;
        return std::find(sel.lanes.begin(), sel.lanes.end(), rowIdentity(row)) != sel.lanes.end();
    }

    // The one statement of the half-open tick rule; paint, press, count,
    // and collection all route through it so they can never diverge.
    static bool tickSelected(const SongView::TimeSelection &sel, uint64_t tick)
    {
        return tick >= sel.startTick && tick < sel.endTick;
    }

    bool pointSelected(const Row &row, uint64_t tick) const
    {
        return rowInLaneSelection(row) && tickSelected(m_sv->timeSelection(), tick);
    }

    // The open point menu's aim rings like a selected node, so the
    // retarget gesture reads — the ring jumps to whichever node the menu
    // re-aims at. The value is part of the match: of same-tick duplicates,
    // only the aimed one rings. Gated on the popup's visibility, not the
    // stored aim — QMenu hides before it emits triggered, so the aim
    // itself must outlive the popup.
    bool pointMenuAimedAt(const Row &row, const LanePoint &pt) const
    {
        return m_pointMenuTarget && m_pointMenu->isVisible() &&
               rowIdentity(row) == rowIdentity(m_pointMenuTarget->row) &&
               pt.tick == m_pointMenuTarget->point.tick &&
               pt.value == m_pointMenuTarget->point.value;
    }

    // Selected-node count, stopping at limit (the callers only distinguish
    // "none", "one", and "two or more").
    int selectedNodeCount(int limit) const
    {
        const SongView::TimeSelection &sel = m_sv->timeSelection();
        int n = 0;
        for (const Row &row : m_rows) {
            if (!rowInLaneSelection(row))
                continue;
            const std::vector<LanePoint> *points = rowPoints(row);
            if (!points)
                continue; // the voice row has markers, not value nodes
            for (const LanePoint &pt : *points) {
                if (pt.tick >= sel.endTick)
                    break; // sorted by tick
                if (tickSelected(sel, pt.tick) && ++n >= limit)
                    return n;
            }
        }
        return n;
    }

    // The derived selection as document points, with the row each node came
    // from. Skips the voice row: its markers are voice identities, not value
    // nodes, and have no drag editing.
    std::vector<GroupNode> collectSelectedNodes() const
    {
        std::vector<GroupNode> nodes;
        SongDocument *doc = m_sv->document();
        if (!doc)
            return nodes;
        const SongView::TimeSelection &sel = m_sv->timeSelection();
        for (size_t i = 0; i < m_rows.size(); i++) {
            uint8_t cc;
            int track;
            if (!rowInLaneSelection(m_rows[i]) || !rowTarget(m_rows[i], &cc, &track))
                continue;
            for (const DocLanePoint &pt : doc->lanePoints(track, cc))
                if (tickSelected(sel, pt.tick))
                    nodes.push_back({int(i), track, cc, pt});
        }
        return nodes;
    }

    // Per-row value clamp for a group-dragged node: the row's display range
    // (what a direct drag of that row can reach), with tempo floored at 1
    // like updateDrag floors it (the document clamps BPM to 1 as well; the
    // floor here keeps the preview from ever showing a 0-BPM curve). The
    // range comes in from the caller so per-frame loops don't rescan the
    // lane through rowRange for every node.
    static int clampRowValue(const Row &row, int value, int minV, int maxV)
    {
        value = std::clamp(value, minV, maxV);
        if (row.kind == Row::Tempo)
            value = std::max(1, value);
        return value;
    }

    // paintCurve's value->y mapping for row ri (the 5/4 plot insets),
    // shared by the gesture previews so a pending node lands exactly where
    // the committed curve will draw it.
    int valueYFor(int ri, int v, int minV, int maxV) const
    {
        const int top = rowTop(ri) + 5;
        const int bottom = rowBottom(ri) - 1 - 4;
        return bottom - (v - minV) * (bottom - top) / std::max(1, maxV - minV);
    }

    // The row's curve as it would look if the live group drag committed
    // now: unmoved points merged with the moved ones, a mover replacing
    // whatever sits on its landing tick and same-tick shadowed duplicates
    // collapsing last-wins (moveLanePoints' rules). Movers are identified
    // by their press-time origins in m_group — never the live selection,
    // which the user can clear or re-sweep mid-drag.
    struct RowPreview {
        std::vector<std::pair<uint64_t, int>> curve; // the pending curve
        std::vector<std::pair<uint64_t, int>> moved; // this row's destinations
        int minV = 0, maxV = 127;                    // row range, for valueYFor
    };
    RowPreview previewRow(int ri, int64_t dTick, int dValue) const
    {
        RowPreview out;
        const Row &row = m_rows[ri];
        rowRange(row, &out.minV, &out.maxV);
        std::vector<uint64_t> origins; // sorted: m_group holds document order
        for (const GroupNode &n : m_group) {
            if (n.row != ri)
                continue;
            origins.push_back(n.point.tick);
            const uint64_t t = uint64_t(int64_t(n.point.tick) + dTick);
            const int v = clampRowValue(row, n.point.value + dValue, out.minV, out.maxV);
            if (!out.moved.empty() && out.moved.back().first == t)
                out.moved.back().second = v; // shadowed duplicates: last wins
            else
                out.moved.push_back({t, v});
        }
        if (out.moved.empty())
            return out;
        size_t o = 0, m = 0;
        if (const std::vector<LanePoint> *points = rowPoints(row)) {
            out.curve.reserve(points->size());
            for (const LanePoint &pt : *points) {
                while (o < origins.size() && origins[o] < pt.tick)
                    o++;
                if (o < origins.size() && origins[o] == pt.tick)
                    continue; // a mover's origin: drawn at its destination
                while (m < out.moved.size() && out.moved[m].first < pt.tick)
                    out.curve.push_back(out.moved[m++]);
                if (m < out.moved.size() && out.moved[m].first == pt.tick)
                    continue; // the landing mover replaces this point
                out.curve.push_back({pt.tick, pt.value});
            }
        }
        while (m < out.moved.size())
            out.curve.push_back(out.moved[m++]);
        return out;
    }

    // Live update of a right-drag selection sweep: the tick span between the
    // press anchor and the cursor, across every row the drag crosses.
    void updateSelSweep(QMouseEvent *event)
    {
        if (m_rightRow < 0 || m_rightRow >= int(m_rows.size()))
            return;
        const bool fine = event->modifiers() & Qt::AltModifier;
        const uint64_t tick = m_sv->snapTick(rawTickAt(event->position().x()), fine);
        SongView::TimeSelection sel;
        sel.startTick = std::min(m_selAnchorTick, tick);
        sel.endTick = std::max(m_selAnchorTick, tick);
        sel.scope = SongView::TimeSelection::Lanes;
        int r0 = m_rightRow;
        int r1 = rowIndexAt(std::clamp(event->pos().y(), 0, rowTop(int(m_rows.size())) - 1));
        if (r1 < 0)
            r1 = r0;
        if (r0 > r1)
            std::swap(r0, r1);
        for (int ri = r0; ri <= r1 && ri < int(m_rows.size()); ri++)
            sel.lanes.push_back(rowIdentity(m_rows[ri]));
        m_sv->setTimeSelection(sel);
    }

    // Right release without a drag: menu inside the time selection, voice-
    // marker delete or the point context menu elsewhere, and clearing the
    // selection over empty space (mirroring the roll).
    void rightClickInPlace(QMouseEvent *event)
    {
        SongDocument *doc = m_sv->document();
        if (!doc || m_rightRow < 0 || m_rightRow >= int(m_rows.size()))
            return;
        const Row &row = m_rows[m_rightRow];
        const std::pair<int, uint8_t> id = rowIdentity(row);
        const SongView::TimeSelection &sel = m_sv->timeSelection();
        const qreal x = event->position().x();
        const qreal dpr = devicePixelRatioF();
        const qreal startX = m_sv->displayX(double(sel.startTick), kGutterW, dpr);
        const qreal endX = m_sv->displayX(double(sel.endTick), kGutterW, dpr);
        if (sel.active() && m_sv->timeSelectionCoversRow(id.first, id.second) && x >= startX &&
            x < endX) {
            m_sv->showTimeSelectionMenu(event->globalPosition().toPoint());
            return;
        }
        if (row.kind == Row::Voice) {
            DocLanePoint hit;
            if (voiceChangeNear(event->position().x(), &hit))
                doc->deleteLanePoints(m_sv->selectedTrack(), DOC_CC_VOICE, {hit});
            return;
        }
        // A menu instead of the old instant delete: Delete is one click
        // away inside it, Set value covers the other common point edit,
        // and the Delete key on a node selection stays the fast path. The
        // open gesture shares the retarget path's aiming wholesale, so the
        // two can never disagree about which point a click means.
        if (movePointMenu(event->globalPosition()))
            return;
        m_sv->clearTimeSelection();
    }

    // Aims and pops the point menu at the point under a right-click —
    // both the opening click and, mirroring the roll's note-menu gesture,
    // an outside right-click retargeting the open popup. Returns false
    // when the click is not the point menu's to take (empty lane space,
    // the gutter, a covering time selection, another widget), so the
    // callers fall through: ui::ContextMenu dismisses the popup,
    // rightClickInPlace clears the selection.
    bool movePointMenu(QPointF globalPos)
    {
        SongDocument *doc = m_sv->document();
        const QPointF pos = globalPos - QPointF(mapToGlobal(QPoint(0, 0)));
        // floor, not int(): truncation toward zero would fold the fringe
        // just above the widget onto row 0.
        const int ri = doc ? rowIndexAt(int(std::floor(pos.y()))) : -1;
        if (ri < 0)
            return false;
        const Row &row = m_rows[ri];
        // The lanes' right-click precedence holds while the popup is open
        // too: inside a covering time selection the range menu owns the
        // click, so a retarget onto a covered point declines.
        const std::pair<int, uint8_t> id = rowIdentity(row);
        const SongView::TimeSelection &sel = m_sv->timeSelection();
        const qreal dpr = devicePixelRatioF();
        if (sel.active() && m_sv->timeSelectionCoversRow(id.first, id.second) &&
            pos.x() >= m_sv->displayX(double(sel.startTick), kGutterW, dpr) &&
            pos.x() < m_sv->displayX(double(sel.endTick), kGutterW, dpr))
            return false;
        DocLanePoint hit;
        if (!pointMenuHit(row, ri, pos, &hit))
            return false;
        m_pointMenuTarget = {row, hit, doc->revision()};
        invalidateContent(); // the aimed node's ring
        m_pointMenu->popup(globalPos.toPoint());
        return true;
    }

    // The chosen action runs against the point the popup was aimed at when
    // it opened (or last retargeted). The revision pins that aim: an edit
    // landing while the popup is open would leave the captured DocLanePoint
    // pointing at reshuffled events, so a stale aim voids the action rather
    // than editing through it.
    void handlePointMenuAction(QAction *action)
    {
        SongDocument *doc = m_sv->document();
        if (!m_pointMenuTarget || !doc || doc->revision() != m_pointMenuTarget->revision)
            return;
        const PointMenuTarget target = *m_pointMenuTarget;
        m_pointMenuTarget.reset();
        uint8_t cc;
        int track;
        if (!rowTarget(target.row, &cc, &track))
            return;
        if (action == m_pointSetValue) {
            int value = target.point.value;
            // The same type-in the lanes' double-click opens (per-row
            // BPM/bend/c_v semantics live in that one implementation).
            if (!editValue(target.row, &value) || value == target.point.value)
                return;
            // The dialog's modal loop can let a queued edit land; the aim
            // is stale then, so void the action rather than edit through
            // it (moveLanePoints would drop the move as a silent no-op).
            if (doc->revision() != target.revision)
                return;
            // A value edit on the aimed point. Like every value edit on a
            // tick — drags, the double-click write — moveLanePoints heals
            // shadowed same-tick duplicates: the tick ends up holding one
            // point with the typed value. The aim picks which point seeds
            // the dialog, not a surviving sibling.
            doc->moveLanePoint(track, cc, target.point, target.point.tick, value);
        } else if (action == m_pointDelete) {
            doc->deleteLanePoints(track, cc, {target.point});
        }
    }

    // Voice change within the marker hit radius of x, if any.
    bool voiceChangeNear(qreal x, DocLanePoint *out) const
    {
        SongDocument *doc = m_sv->document();
        if (!doc)
            return false;
        bool found = false;
        qreal bestDist = 9.0; // same radius as nearestPoint
        const qreal dpr = devicePixelRatioF();
        for (const DocLanePoint &pt : doc->lanePoints(m_sv->selectedTrack(), DOC_CC_VOICE)) {
            const qreal dist = std::abs(m_sv->displayX(double(pt.tick), kGutterW, dpr) - x);
            // Ties go to the later point: of same-tick duplicates it is the
            // audible winner.
            if (dist < bestDist || (found && dist == bestDist)) {
                bestDist = dist;
                *out = pt;
                found = true;
            }
        }
        return found;
    }

    // Per-row geometry: individually-resized rows (divider drag) override
    // the shared m_laneH. Keys survive track switches, so each lane keeps
    // its height when the user comes back to the track.
    QString rowKey(const Row &row) const
    {
        switch (row.kind) {
        case Row::Tempo:
            return QStringLiteral("tempo");
        case Row::Voice:
            return QStringLiteral("voice:%1").arg(m_sv->selectedTrack());
        case Row::Lane:
            return laneKey(row.track, row.cc);
        }
        return QString();
    }

    static QString laneKey(int track, uint8_t cc)
    {
        return QStringLiteral("cc:%1:%2").arg(track).arg(cc);
    }

    // Inverse of laneKey, for walking the hidden set (a key that fails to
    // parse — hand-edited sidecar — is simply never listed or matched).
    // Canonical spellings only: toInt also accepts "01"/"+1", but such a
    // key would never equal the laneKey the rest of the code compares and
    // removes, so it must not surface as an undismissable menu entry.
    static bool parseLaneKey(const QString &key, int *track, uint8_t *cc)
    {
        const QStringList parts = key.split(QLatin1Char(':'));
        if (parts.size() != 3 || parts[0] != QLatin1String("cc"))
            return false;
        bool trackOk = false, ccOk = false;
        const int t = parts[1].toInt(&trackOk);
        const int c = parts[2].toInt(&ccOk);
        if (!trackOk || !ccOk || t < 0 || t > 15 || c < 0 || c > 255 ||
            key != laneKey(t, uint8_t(c)))
            return false;
        *track = t;
        *cc = uint8_t(c);
        return true;
    }

    int rowHeight(const Row &row) const
    {
        const auto it = m_rowHeights.constFind(rowKey(row));
        return it != m_rowHeights.constEnd() ? it.value() : m_laneH;
    }

    // Top of row `index`; index == m_rows.size() gives the total height.
    int rowTop(int index) const
    {
        int y = 0;
        for (int i = 0; i < index && i < int(m_rows.size()); i++)
            y += rowHeight(m_rows[i]);
        return y;
    }

    int rowBottom(int index) const { return rowTop(index) + rowHeight(m_rows[index]); }

    int rowIndexAt(int y) const
    {
        if (y < 0)
            return -1;
        int bottom = 0;
        for (size_t i = 0; i < m_rows.size(); i++) {
            bottom += rowHeight(m_rows[i]);
            if (y < bottom)
                return int(i);
        }
        return -1;
    }

    // Divider hit test: the bottom edge of row i (±3 px) starts an
    // individual-height drag for that row.
    int rowBoundaryAt(int y) const
    {
        int bottom = 0;
        for (size_t i = 0; i < m_rows.size(); i++) {
            bottom += rowHeight(m_rows[i]);
            if (std::abs(y - bottom) <= 3)
                return int(i);
        }
        return -1;
    }

    QRect addLaneRect() const { return QRect(0, rowTop(int(m_rows.size())), width(), kAddLaneH); }

    void applyHeight()
    {
        // Minimum, not fixed: the scroll area stretches the widget to fill
        // its viewport when the user drags the lanes area taller.
        const int addH = m_sv->timeline() && m_sv->document() ? kAddLaneH : 0;
        setMinimumHeight(std::max(m_laneH, rowTop(int(m_rows.size())) + addH));
    }

    // Ctrl+wheel: rescale the lane rows (the roll's key-height analog),
    // keeping the row under the cursor pinned. anchorY is widget-local, so
    // it already includes the scroll offset. Individually-resized rows
    // scale by the same factor, keeping their proportions.
    void zoomLaneHeight(int wheelDelta, int anchorY)
    {
        m_laneZoomAccum += wheelDelta;
        const int steps = m_laneZoomAccum / 120;
        if (steps == 0)
            return;
        m_laneZoomAccum -= steps * 120;
        const int newH = std::clamp(m_laneH + steps * 4, kMinLaneH, kMaxLaneH);
        if (newH == m_laneH)
            return;
        const double factor = double(newH) / double(m_laneH);
        for (auto it = m_rowHeights.begin(); it != m_rowHeights.end(); ++it)
            it.value() = std::clamp(int(std::lround(it.value() * factor)), kMinLaneH, kMaxLaneH);
        m_laneH = newH;
        applyHeight();
        if (m_scroll) {
            QScrollBar *vbar = m_scroll->verticalScrollBar();
            const int viewportY = anchorY - vbar->value();
            vbar->setValue(int(std::lround(anchorY * factor)) - viewportY);
        }
        invalidateContent();
    }

    // The add-lane / hidden-lane menu label for a CC (or bend) lane.
    static QString ccLaneLabel(uint8_t cc)
    {
        return QStringLiteral("%1 (%2)").arg(
            laneDisplayName(cc),
            cc == LANE_CC_BEND ? QStringLiteral("BEND") : QLatin1String(m4aClassifyCc(cc).name));
    }

    // Menu of §4.2 audible parameters without a lane on the selected track,
    // plus a section restoring this track's hidden lanes (which are skipped
    // as plain candidates — a hidden lane already has its data, so "adding"
    // it must go through Show, not a second empty lane).
    void showAddLaneMenu(const QPoint &globalPos)
    {
        const int track = m_sv->selectedTrack();
        QMenu menu;
        static constexpr uint8_t kAudibleCcs[] = {0x01, 0x07, 0x0A, 0x14, 0x15, LANE_CC_BEND};
        for (uint8_t cc : kAudibleCcs) {
            if (m_sv->model().findLane(track, cc) || m_hiddenLanes.contains(laneKey(track, cc)))
                continue;
            menu.addAction(ccLaneLabel(cc))->setData(int(cc));
        }
        // Any CC can be hidden (imported songs carry lanes beyond the
        // addable list), so walk the hidden set itself, sorted for a stable
        // menu order. Show actions are keyed past the CC range.
        std::vector<uint8_t> hidden;
        for (const QString &key : m_hiddenLanes) {
            int keyTrack;
            uint8_t keyCc;
            if (parseLaneKey(key, &keyTrack, &keyCc) && keyTrack == track)
                hidden.push_back(keyCc);
        }
        std::sort(hidden.begin(), hidden.end());
        // The global tempo row is hidden by default (the transport bar's
        // Tempo spinner covers the constant-tempo case), so it shows up here
        // as one more hidden lane. Keyed past the Show range.
        static constexpr int kShowTempo = 512;
        const bool tempoHidden = !m_tempoLaneVisible;
        // The placeholder would be a lie right above a "Show: …" entry, so
        // it only stands in when both sections are empty.
        if (menu.isEmpty() && hidden.empty() && !tempoHidden)
            menu.addAction(SongView::tr("All parameters already have lanes"))->setEnabled(false);
        if (!hidden.empty() || tempoHidden) {
            if (!menu.isEmpty())
                menu.addSeparator();
            menu.addAction(SongView::tr("Hidden lanes"))->setEnabled(false);
            if (tempoHidden)
                menu.addAction(SongView::tr("Show: Tempo (hidden)"))->setData(kShowTempo);
            for (const uint8_t cc : hidden)
                menu.addAction(SongView::tr("Show: %1 (hidden)").arg(ccLaneLabel(cc)))
                    ->setData(256 + int(cc));
        }
        QAction *chosen = menu.exec(globalPos);
        if (!chosen || !chosen->data().isValid())
            return;
        const int value = chosen->data().toInt();
        if (value == kShowTempo) {
            m_sv->setTempoLaneVisible(true);
            m_sv->announce(SongView::tr("Showed the Tempo lane"));
        } else if (value >= 256) {
            const uint8_t cc = uint8_t(value - 256);
            m_hiddenLanes.remove(laneKey(track, cc));
            if (!m_sv->model().findLane(track, cc)) {
                // The data vanished while hidden (event-list delete, undo of
                // its insertion): come back as an empty lane, not as nothing.
                m_sv->addEmptyLane(track, cc); // rebuilds the rows
            } else {
                rebuildRows();
            }
            m_sv->announce(SongView::tr("Showed the %1 lane").arg(ccLaneLabel(cc)));
        } else {
            m_sv->addEmptyLane(track, uint8_t(value));
        }
    }

    // Gutter menu on the global tempo row: hide it (view-only, the tempo
    // metas stay; the add-lane menu's "Hidden lanes" section and the
    // transport bar's tempo warning bring it back).
    void showTempoMenu(const QPoint &globalPos)
    {
        QMenu menu;
        QAction *hide = menu.addAction(SongView::tr("Hide lane"));
        if (menu.exec(globalPos) != hide)
            return;
        m_sv->setTempoLaneVisible(false);
        m_sv->announce(SongView::tr("Hid the Tempo lane"));
    }

    // Gutter menu on a CC/bend lane: clear its events (the lane stays,
    // empty), delete the lane outright — confirmed first while it still
    // has events, since that throws the whole curve away in one step — or
    // hide the row, keeping its events.
    void showLaneMenu(const Row &row, const QPoint &globalPos)
    {
        // Copies: the menu's actions mutate the model. The live lane is
        // looked up for its display name only, so a row whose lane left the
        // model still gets a working menu.
        const int track = row.track;
        const uint8_t cc = row.cc;
        const AutoLane *lane = rowLane(row);
        const QString name = lane ? lane->name : laneDisplayName(cc);
        const bool empty = !lane || lane->points.empty();

        QMenu menu;
        QAction *copyLane = menu.addAction(SongView::tr("Copy lane"));
        copyLane->setEnabled(!empty);
        QAction *pasteLane = menu.addAction(SongView::tr("Paste lane (replace)"));
        // Only whole-lane clips (their ticks are absolute) paste here; range
        // clips are anchored at the edit cursor instead.
        {
            const SongView::Clip &clip = m_sv->clipboard();
            pasteLane->setEnabled(clip.wholeLane && clip.lanes.size() == 1 &&
                                  !clip.lanes.front().points.empty());
        }
        menu.addSeparator();
        QAction *clear = menu.addAction(SongView::tr("Clear events"));
        clear->setEnabled(!empty);
        QAction *del =
            menu.addAction(empty ? SongView::tr("Remove empty lane") : SongView::tr("Delete lane"));
        QAction *hide = menu.addAction(SongView::tr("Hide lane"));
        // Value-axis zoom: display range only, the events keep their values.
        std::vector<std::pair<QAction *, int>> rangeActions;
        if (laneRangeZoomable(cc)) {
            QMenu *range = menu.addMenu(SongView::tr("Value range"));
            const int current = m_rowRanges.value(laneKey(track, cc), laneRangeDefault(cc));
            const std::pair<int, QString> options[] = {
                {0, SongView::tr("Auto (fit to data)")},
                {16, QStringLiteral("0–16")},
                {32, QStringLiteral("0–32")},
                {64, QStringLiteral("0–64")},
                {127, SongView::tr("0–127 (full)")},
            };
            for (const auto &opt : options) {
                QAction *action = range->addAction(opt.second);
                action->setCheckable(true);
                action->setChecked(opt.first == current);
                rangeActions.emplace_back(action, opt.first);
            }
        }
        QAction *chosen = menu.exec(globalPos);
        if (!chosen)
            return;
        for (const std::pair<QAction *, int> &opt : rangeActions) {
            if (chosen == opt.first) {
                setLaneRange(track, cc, opt.second);
                return;
            }
        }

        SongDocument *doc = m_sv->document();
        const std::vector<DocLanePoint> points = doc->lanePoints(track, cc);
        if (chosen == copyLane) {
            if (points.empty())
                return;
            SongView::Clip clip;
            clip.wholeLane = true;
            clip.span = points.back().tick + 1;
            SongView::ClipLane cl{track, cc, {}};
            for (const DocLanePoint &pt : points)
                cl.points.push_back({uint32_t(pt.tick), pt.value});
            clip.lanes.push_back(std::move(cl));
            m_sv->setClipboard(std::move(clip));
            m_sv->announce(
                SongView::tr("Copied the %1 lane (%n point(s))", nullptr, int(points.size()))
                    .arg(name));
            return;
        }
        if (chosen == pasteLane) {
            // Replace this lane's whole contents with the clipboard lane at
            // its original ticks (values clamp to this lane's range), as one
            // undoable command.
            // Re-check: the clipboard is app-shared and the menu's nested
            // event loop could have replaced it since the enablement check.
            const SongView::Clip clip = m_sv->clipForPaste();
            if (!clip.wholeLane || clip.lanes.empty())
                return;
            SongDocument::RangeEdit edit;
            edit.removePoints = points;
            SongDocument::RangeEdit::LaneWrite lw{track, cc, {}};
            for (const std::pair<uint32_t, int> &pv : clip.lanes.front().points)
                lw.points.push_back({uint64_t(pv.first), pv.second});
            edit.addPoints.push_back(std::move(lw));
            doc->applyRangeEdit(SongView::tr("paste lane"), edit);
            const int foreign = cc == DOC_CC_VOICE ? m_sv->foreignVoiceCount(clip) : 0;
            if (foreign > 0)
                m_sv->announce(SongView::tr("Replaced the %1 lane · %n voice change(s) name a "
                                            "different instrument in this voicegroup",
                                            nullptr, foreign)
                                   .arg(name));
            else
                m_sv->announce(SongView::tr("Replaced the %1 lane").arg(name));
            return;
        }
        if (chosen == clear) {
            if (points.empty())
                return;
            // Keep the row alive as an empty lane once its events go.
            m_sv->addEmptyLane(track, cc);
            doc->deleteLanePoints(track, cc, points);
        } else if (chosen == del) {
            if (!points.empty() &&
                QMessageBox::question(this, SongView::tr("Delete lane"),
                                      SongView::tr("Delete the %1 lane and its %2 events?")
                                          .arg(name)
                                          .arg(points.size())) != QMessageBox::Yes)
                return;
            m_sv->removeEmptyLane(track, cc); // forget the view state first
            if (!points.empty())
                doc->deleteLanePoints(track, cc, points);
        } else if (chosen == hide) {
            // View-only: the row leaves the lanes area, its points stay in
            // the document (no undo entry). The add-lane menu's "Hidden
            // lanes" section restores it. A time selection covering the
            // lane is dropped (collapsed, like deleteTrack's) — a range
            // Delete must never edit an invisible row's events.
            if (m_sv->timeSelectionCoversRow(track, cc))
                m_sv->clearTimeSelection();
            m_hiddenLanes.insert(laneKey(track, cc));
            rebuildRows();
            m_sv->announce(SongView::tr("Hid the %1 lane").arg(name));
        }
    }

    // Voice row, left button only (right-button gestures are handled by the
    // shared deferral): click a marker to re-pick its voice, click empty
    // space to insert a change at the snapped tick; release-in-place
    // right-clicks delete a marker via rightClickInPlace. The value axis is
    // meaningless here (a voice is an identity, not a level), so the picker
    // dialog replaces the lanes' drag editing.
    void voiceRowPress(QMouseEvent *event)
    {
        SongDocument *doc = m_sv->document();
        const int track = m_sv->selectedTrack();
        if (event->button() != Qt::LeftButton)
            return;
        DocLanePoint hitPt;
        if (voiceChangeNear(event->position().x(), &hitPt)) {
            const DocLanePoint *hit = &hitPt;
            int voice = hit->value;
            if (m_sv->pickVoice(SongView::tr("Change voice"), hit->value, &voice) &&
                voice != hit->value)
                doc->moveLanePoint(track, DOC_CC_VOICE, *hit, hit->tick, voice);
        } else {
            const std::vector<DocLanePoint> changes = doc->lanePoints(track, DOC_CC_VOICE);
            const uint64_t tick = m_sv->snapTick(
                m_sv->tickAtContentX(std::max(qreal(kGutterW), event->position().x()) - kGutterW));
            // Preselect the voice already sounding at that tick.
            int voice = 0;
            for (const DocLanePoint &pt : changes) {
                if (pt.tick > tick)
                    break;
                voice = pt.value;
            }
            if (m_sv->pickVoice(SongView::tr("Insert voice change"), voice, &voice))
                doc->addLanePoint(track, DOC_CC_VOICE, tick, voice);
        }
    }

    // Document target of an editable row; false for the voice row.
    bool rowTarget(const Row &row, uint8_t *cc, int *track) const
    {
        if (row.kind == Row::Tempo) {
            *cc = DOC_CC_TEMPO;
            *track = m_sv->selectedTrack();
            return true;
        }
        if (row.kind == Row::Lane) {
            *cc = row.cc; // LANE_CC_BEND == DOC_CC_BEND
            *track = row.track;
            return true;
        }
        return false;
    }

    const std::vector<LanePoint> *rowPoints(const Row &row) const
    {
        if (row.kind == Row::Tempo)
            return &m_sv->model().tempoLane;
        if (const AutoLane *lane = rowLane(row))
            return &lane->points;
        return nullptr;
    }

    // Lanes whose value axis can zoom: 0-based CC lanes. Centered lanes
    // (PAN/TUNE swing around 64) and bend keep their fixed scales.
    static bool laneRangeZoomable(uint8_t cc)
    {
        return cc != LANE_CC_BEND && cc != 0x0A && cc != 0x18;
    }

    // Default axis mode per CC (0 = auto-fit): MOD is only musical in the
    // bottom stretch of 0..127 (vibrato past ~20 is cartoony), so it fits
    // to the data by default; everything else keeps the full axis.
    static int laneRangeDefault(uint8_t cc) { return cc == 0x01 ? 0 : 127; }

    // Auto-fit rung: the smallest of 16/32/64/127 that holds the lane's
    // data. Coarse rungs keep the scale from twitching while points are
    // edited. 16 is the floor because typical MOD curves live in 0..15.
    static int laneAutoMax(int dataMax)
    {
        if (dataMax <= 16)
            return 16;
        if (dataMax <= 32)
            return 32;
        if (dataMax <= 64)
            return 64;
        return 127;
    }

    void rowRange(const Row &row, int *minV, int *maxV) const
    {
        *minV = 0;
        *maxV = 127;
        if (row.kind == Row::Tempo) {
            *maxV = 200;
            for (const LanePoint &pt : m_sv->model().tempoLane)
                *maxV = std::max(*maxV, pt.value + 20);
        } else if (row.kind == Row::Lane && row.cc == LANE_CC_BEND) {
            *minV = -8192;
            *maxV = 8191;
        } else if (row.kind == Row::Lane && laneRangeZoomable(row.cc)) {
            // Zoomed value axis: the display max shrinks so a small useful
            // range (MOD's 0..20, say) gets the row's pixels. Data outside
            // the chosen range always grows the axis back — points never
            // draw off-scale, and the gutter label shows the live range.
            int dataMax = 0;
            if (const AutoLane *lane = rowLane(row)) {
                for (const LanePoint &pt : lane->points)
                    dataMax = std::max(dataMax, pt.value);
            }
            const int mode = m_rowRanges.value(rowKey(row), laneRangeDefault(row.cc));
            *maxV = mode > 0 ? std::max(mode, dataMax) : laneAutoMax(dataMax);
        }
    }

    QString rowTitle(const Row &row) const
    {
        switch (row.kind) {
        case Row::Tempo:
            return SongView::tr("Tempo (BPM)");
        case Row::Voice:
            return SongView::tr("Voice");
        case Row::Lane: {
            if (row.cc == LANE_CC_BEND)
                return SongView::tr("Pitch bend (BEND)");
            const M4aCcInfo info = m4aClassifyCc(row.cc);
            const AutoLane *lane = rowLane(row);
            return QStringLiteral("%1 (%2)").arg(lane ? lane->name : QLatin1String(info.name),
                                                 QLatin1String(info.name));
        }
        }
        return QString();
    }

    // The m4a display convention used elsewhere in the app (PAN/TUNE as
    // c_v±, bend signed): a raw "64" for pan hides that it IS center.
    QString formatRowValue(const Row &row, int v) const
    {
        if (row.kind == Row::Lane) {
            if (row.cc == LANE_CC_BEND)
                return m4aFormatBend(v);
            return m4aFormatCcValue(row.cc, uint8_t(v));
        }
        return QString::number(v);
    }

    // Neutral value a Ctrl-drag magnetizes to; only lanes where "centered"
    // is meaningful and hard to hit by eye.
    bool rowDetent(const Row &row, int *value) const
    {
        if (row.kind != Row::Lane)
            return false;
        if (row.cc == 0x0A || row.cc == 0x18) { // PAN/TUNE: c_v 0
            *value = 64;
            return true;
        }
        if (row.cc == LANE_CC_BEND) {
            *value = 0;
            return true;
        }
        return false;
    }

    // Type-in editor: the only way to hit an arbitrary exact value, since a
    // pixel spans several value units at normal lane heights. Input uses the
    // display convention; PAN/TUNE entry is c_v (stored value minus 64).
    bool editValue(const Row &row, int *value)
    {
        int minShown = 0, maxShown = 127, offset = 0; // stored = shown + offset
        QString label = SongView::tr("Value:");
        if (row.kind == Row::Tempo) {
            minShown = 1;
            maxShown = 999;
            label = SongView::tr("BPM:");
        } else if (row.cc == LANE_CC_BEND) {
            minShown = -8192;
            maxShown = 8191;
            label = SongView::tr("Bend (0 = none):");
        } else if (row.cc == 0x0A || row.cc == 0x18) {
            minShown = -64;
            maxShown = 63;
            offset = 64;
            label = SongView::tr("c_v value (0 = center):");
        }
        bool ok = false;
        const int shown = QInputDialog::getInt(this, rowTitle(row), label, *value - offset,
                                               minShown, maxShown, 1, &ok);
        if (ok)
            *value = shown + offset;
        return ok;
    }

    const LanePoint *nearestPoint(const Row &row, qreal x) const
    {
        const std::vector<LanePoint> *points = rowPoints(row);
        if (!points)
            return nullptr;
        const LanePoint *best = nullptr;
        qreal bestDist = 9.0;
        const qreal dpr = devicePixelRatioF();
        for (const LanePoint &pt : *points) {
            const qreal dist = std::abs(m_sv->displayX(double(pt.tick), kGutterW, dpr) - x);
            // Ties go to the later point: of same-tick duplicates it is the
            // audible winner.
            if (dist < bestDist || (best && dist == bestDist)) {
                bestDist = dist;
                best = &pt;
            }
        }
        return best;
    }

    // Right-click aim test: a circular radius around each dot, nearest one
    // wins. Resolved against the document, not the view lane, so same-tick
    // duplicates keep their identity — x alone can only ever say "the later
    // one"; the cursor's y is what tells duplicates apart. The 9 px reach is
    // the old x-only right-click radius (nearestPoint, still the
    // double-click rule); grabPoint's ±7 box stays the left-drag rule.
    bool pointMenuHit(const Row &row, int ri, QPointF pos, DocLanePoint *hit) const
    {
        SongDocument *doc = m_sv->document();
        uint8_t cc;
        int track;
        if (!doc || pos.x() < kGutterW || !rowTarget(row, &cc, &track))
            return false;
        int minV, maxV;
        rowRange(row, &minV, &maxV);
        const qreal dpr = devicePixelRatioF();
        constexpr qreal kRadius = 9.0;
        qreal bestDist = kRadius * kRadius;
        bool found = false;
        for (const DocLanePoint &pt : doc->lanePoints(track, cc)) {
            const qreal dx = m_sv->displayX(double(pt.tick), kGutterW, dpr) - pos.x();
            const qreal dy = valueYFor(ri, pt.value, minV, maxV) - pos.y();
            const qreal dist = dx * dx + dy * dy;
            // Ties go to the later point: of same-tick duplicates it is the
            // audible winner.
            if (dist < bestDist || (found && dist == bestDist)) {
                bestDist = dist;
                *hit = pt;
                found = true;
            }
        }
        return found;
    }

    // Left-press grab test: near the point's dot in BOTH x and y. A dense
    // freehand curve has a point on every grid cell, so an x-only radius
    // (nearestPoint, kept for the double-click type-in) would capture every
    // press and make redrawing impossible; grab the dot itself to move a point.
    const LanePoint *grabPoint(const Row &row, int ri, qreal x, int y) const
    {
        const std::vector<LanePoint> *points = rowPoints(row);
        if (!points)
            return nullptr;
        int minV, maxV;
        rowRange(row, &minV, &maxV);
        // paintCurve's valueY mapping for this row.
        const int top = rowTop(ri) + 5;
        const int bottom = rowBottom(ri) - 1 - 4;
        const LanePoint *best = nullptr;
        qreal bestDist = qreal(INT_MAX);
        const qreal dpr = devicePixelRatioF();
        for (const LanePoint &pt : *points) {
            const qreal dx = m_sv->displayX(double(pt.tick), kGutterW, dpr) - x;
            const int dy =
                bottom - (pt.value - minV) * (bottom - top) / std::max(1, maxV - minV) - y;
            if (std::abs(dx) > 7 || std::abs(dy) > 7)
                continue;
            const qreal dist = dx * dx + qreal(dy * dy);
            // Ties go to the later point: of same-tick duplicates it is the
            // audible winner.
            if (dist < bestDist || (best && dist == bestDist)) {
                bestDist = dist;
                best = &pt;
            }
        }
        return best;
    }

    double rawTickAt(qreal x) const
    {
        return std::max(0.0, m_sv->tickAtContentX(std::max(qreal(kGutterW), x) - kGutterW));
    }

    // Idle cursor position for the hover readout: raw (unsnapped) tick, so
    // the readout tracks the pixel, not the grid. The gutter (and a stale
    // lane row) clears it. The cursor's raw x and y ride along: the on-dot
    // ring's grab test needs the true pixel (the tick round-trips through
    // the pre-roll clamp), and the voice readout anchors to the cursor.
    void updateHover(qreal x, int y)
    {
        const int ri = x >= kGutterW ? rowIndexAt(y) : -1;
        if (ri < 0 || (m_rows[ri].kind != Row::Voice && !rowPoints(m_rows[ri]))) {
            clearHover();
            return;
        }
        if (ri == m_hoverRow && x == m_hoverX && y == m_hoverY)
            return;
        const auto oldGeometry = hoverReadoutGeometry(m_hoverRow, m_hoverTick, m_hoverX, m_hoverY);
        const QRegion oldRegion = oldGeometry ? oldGeometry->paintRegion : QRegion();
        m_hoverRow = ri;
        m_hoverTick = rawTickAt(x);
        m_hoverX = x;
        m_hoverY = y;
        const auto newGeometry = hoverReadoutGeometry(m_hoverRow, m_hoverTick, m_hoverX, m_hoverY);
        const QRegion newRegion = newGeometry ? newGeometry->paintRegion : QRegion();
        // Mirrored for the test harness, like the roll's hoverKey.
        setProperty("hoverNodeTick",
                    newGeometry ? qlonglong(newGeometry->onPointTick) : qlonglong(-1));
        invalidateContent(oldRegion | newRegion);
    }

    void clearHover()
    {
        if (m_hoverRow < 0)
            return;
        const auto oldGeometry = hoverReadoutGeometry(m_hoverRow, m_hoverTick, m_hoverX, m_hoverY);
        const QRegion oldRegion = oldGeometry ? oldGeometry->paintRegion : QRegion();
        m_hoverRow = -1;
        setProperty("hoverNodeTick", qlonglong(-1));
        invalidateContent(oldRegion);
    }

    // The widget-level cursor for the current tool: the pencil while pencil
    // mode is on, the arrow otherwise. Position-aware callers (the idle
    // hover) additionally demote to the arrow over non-drawable regions.
    QCursor modeCursor() { return m_pencilMode ? pencilCursor() : QCursor(Qt::ArrowCursor); }

    // A left press here would start a draw gesture: a tempo/lane row's plot
    // area. The gutter, the Voice row, and the add-lane strip route to
    // menus and marker gestures instead, so the pencil would be a false
    // promise there.
    bool drawableAt(qreal x, int y) const
    {
        if (x < kGutterW)
            return false;
        const int ri = rowIndexAt(y);
        return ri >= 0 && m_rows[ri].kind != Row::Voice;
    }

    // Lazily-built pencil cursor, rebuilt when the device pixel ratio
    // changes (moving between monitors). The icon engine picks the @2x
    // asset on high-DPI screens; the hotspot is the pencil's tip in the
    // bottom-left corner.
    const QCursor &pencilCursor()
    {
        const qreal dpr = devicePixelRatioF();
        if (m_pencilCursorDpr != dpr) {
            constexpr int kCursorExtent = 16;
            const QIcon icon(QStringLiteral(":/cursors/pencil.png"));
            const QPixmap pixmap = icon.pixmap(QSize(kCursorExtent, kCursorExtent), dpr);
            const qreal pixmapDpr = std::max<qreal>(1.0, pixmap.devicePixelRatio());
            const int hotspotY = std::max(0, qRound(pixmap.height() / pixmapDpr) - 1);
            m_pencilCursor = hotspotCursor(pixmap, 0, hotspotY);
            m_pencilCursorDpr = dpr;
        }
        return m_pencilCursor;
    }

    // Invert paintCurve's valueY mapping; ri indexes the row for geometry.
    int valueAtY(const Row &row, int ri, int yPos) const
    {
        int minV, maxV;
        rowRange(row, &minV, &maxV);
        const int top = rowTop(ri) + 5;
        const int bottom = rowBottom(ri) - 1 - 4;
        const int y = std::clamp(yPos, top, bottom);
        return minV + (bottom - y) * (maxV - minV) / std::max(1, bottom - top);
    }

    void updateDrag(qreal x, int y, bool fine, bool detent)
    {
        if (m_dragRow < 0 || m_dragRow >= int(m_rows.size()))
            return;
        const Row &row = m_rows[m_dragRow];
        m_dragValue = valueAtY(row, m_dragRow, y);
        if (row.kind == Row::Tempo)
            m_dragValue = std::max(1, m_dragValue);
        // Ctrl detent: magnetize to the lane's neutral value, so dead-center
        // doesn't require pixel-perfect aim. The window is a font-scaled
        // pixel radius (fontPx(2/3) — the old hard-coded 8 px at the
        // reference font) over the full row height, the source branch's
        // formula: the realized magnet runs slightly inside the nominal
        // radius (the plot insets the row by 10 px) but stays near-constant
        // across lane heights and scales with the user's font size.
        int neutral;
        if (detent && rowDetent(row, &neutral)) {
            int minV, maxV;
            rowRange(row, &minV, &maxV);
            if (std::abs(m_dragValue - neutral) <=
                (maxV - minV) * lyt::fontPx(2.0 / 3.0) / std::max(1, rowHeight(row)))
                m_dragValue = neutral;
        }
        m_dragTick = m_sv->snapTick(rawTickAt(x), fine);
    }

    // The travel a press must clear before it counts as a drag: the source
    // branch's font-scaled radius (5 px at the reference font), shared by
    // the Shift axis lock's activation and the arrow-tool sweep's. Below
    // it a press is a click, and a click never draws.
    static int nodeDragActivationDistance() { return lyt::fontPx(5.0 / 12.0); }

    // How much wider than tall a pencil stroke has to be for its vertical
    // travel to still read as slop once it has passed the activation
    // distance: a 4:1 wedge, so a deliberate diagonal breaks out promptly
    // while a long horizontal drag tolerates a quarter of its length in
    // wobble.
    static constexpr qreal kPencilSlopAspect = 4.0;

    // True while the pencil stroke's value should stay pinned at the press.
    // The hold ends — permanently, for this stroke — the first time the
    // travel from the slop origin looks like real vertical intent.
    bool pencilSlopHolds(const QPointF &pos)
    {
        if (m_pencilSlopExceeded)
            return false;
        const qreal dx = std::abs(pos.x() - m_pencilSlopOrigin.x());
        const qreal dy = std::abs(pos.y() - m_pencilSlopOrigin.y());
        const qreal threshold = qreal(nodeDragActivationDistance());
        if (dy < threshold && (dx + dy < threshold || dy == 0.0 || dx > dy * kPencilSlopAspect))
            return true;
        m_pencilSlopExceeded = true;
        m_pencilSlopOrigin = pos;
        return false;
    }

    // Shift anywhere in a point drag (press modifiers, a move's, or a key
    // press while the pointer is parked) means the axis lock, not a click.
    void noteShift(Qt::KeyboardModifiers mods)
    {
        if (mods & Qt::ShiftModifier)
            m_pointShiftSeen = true;
    }

    // Travel past the activation distance makes a point gesture a drag for
    // good, however it ends up landing: a drag that wanders back to the
    // node's own tick and value — easy inside one snap cell, and the
    // group drag's tick-0 clamp produces it outright — must commit nothing
    // rather than read as a click and delete the node.
    void noteTravel(const QPointF &position)
    {
        if (m_pointTraveled)
            return;
        const QPointF d = position - m_pointPressPos;
        m_pointTraveled = std::abs(d.x()) + std::abs(d.y()) >= qreal(nodeDragActivationDistance());
    }

    // Arrow-tool click on a node: the click deletes it, one undo entry —
    // the redesign's replacement for the right-click instant delete the
    // point menu took over. The flag makes the pair's double-click a
    // no-op; without it the type-in would fire on the empty space the
    // delete just made.
    // The document point the press grabbed. Aimed the way grabPoint aimed
    // it — by tick AND value — because findLanePoint resolves a tick to
    // its last point, so on same-tick duplicates (the case the point menu
    // exists to disambiguate) it would answer with the sibling the cursor
    // was nowhere near. Falls back to that rule when no value matches.
    bool grabbedPoint(int track, uint8_t cc, DocLanePoint *out) const
    {
        SongDocument *doc = m_sv->document();
        if (!doc || m_dragOrigTick < 0)
            return false;
        bool found = false, exact = false;
        for (const DocLanePoint &pt : doc->lanePoints(track, cc)) {
            if (pt.tick > uint64_t(m_dragOrigTick))
                break;
            if (pt.tick != uint64_t(m_dragOrigTick) || exact)
                continue;
            *out = pt;
            found = true;
            exact = pt.value == m_dragOrigValue;
        }
        return found;
    }

    void clickDeleteNode(int track, uint8_t cc)
    {
        SongDocument *doc = m_sv->document();
        DocLanePoint victim;
        if (m_pointShiftSeen || m_pointTraveled || !doc || !grabbedPoint(track, cc, &victim))
            return;
        clearHover();
        m_clickDeleted = true;
        doc->deleteLanePoints(track, cc, {victim});
    }

    // Shift axis lock on a live point drag, applied after updateDrag mapped
    // the cursor. While Shift is down but the travel hasn't resolved an axis
    // yet the point holds still (no jump before the lock lands); once it
    // has, the off-axis coordinate pins to the point's original position.
    // The activation distance is the source branch's font-scaled radius
    // (fontPx(5/12) — 5 px at the reference font). The cursor mirrors the
    // state: SizeHor moving in time, SizeVer moving in value, the tool
    // cursor for a free drag.
    void updatePointAxisLock(const QPointF &position, bool shiftHeld)
    {
        const AxisLock previous = m_axisLock;
        m_axisLock = resolveAxisLock(m_axisLock, shiftHeld, m_pointPressPos, position,
                                     nodeDragActivationDistance());
        if (shiftHeld && m_axisLock == AxisLock::None) {
            m_dragTick = uint64_t(m_dragOrigTick);
            m_dragValue = m_dragOrigValue;
        } else {
            applyAxisLock(m_axisLock, uint64_t(m_dragOrigTick), m_dragOrigValue, &m_dragTick,
                          &m_dragValue);
        }
        // Group drag: clamp the shared dTick so the earliest selected node
        // can't go below tick 0, and land the clamp back on the grabbed
        // marker so the preview and the commit agree.
        if (!m_group.empty()) {
            int64_t dTick = int64_t(m_dragTick) - m_dragOrigTick;
            uint64_t earliest = m_group.front().point.tick;
            for (const GroupNode &n : m_group)
                earliest = std::min(earliest, n.point.tick);
            dTick = std::max(dTick, -int64_t(earliest));
            m_dragTick = uint64_t(m_dragOrigTick + dTick);
        }
        if (m_axisLock != previous)
            setCursor(dragCursor());
    }

    // The cursor the live gesture wants right now: the axis-lock shape
    // while a locked point drag runs, the tool cursor otherwise. Gestures
    // that interrupt a drag (a middle-button pan) restore through this so
    // they can't strand the wrong cursor on a still-locked drag.
    QCursor dragCursor()
    {
        if (m_axisLock == AxisLock::Time)
            return QCursor(Qt::SizeHorCursor);
        if (m_axisLock == AxisLock::Value)
            return QCursor(Qt::SizeVerCursor);
        return modeCursor();
    }

    // Drop the axis lock and hand the cursor back to the tool. Every path
    // that ends or aborts a point drag (the release, rebuildRows) funnels
    // through here so an aborted gesture can't strand the lock cursor.
    void clearAxisLock()
    {
        if (m_axisLock == AxisLock::None)
            return;
        m_axisLock = AxisLock::None;
        setCursor(modeCursor());
    }

    // Shift pressed or released while the pointer is parked mid point-drag:
    // no mouse move will run the lock update, so re-derive the drag from
    // the last cursor sample under the new Shift state (freeing must also
    // un-pin the coordinates the lock overwrote).
    void pointDragShiftChanged(bool shiftHeld, Qt::KeyboardModifiers mods)
    {
        if (shiftHeld)
            m_pointShiftSeen = true;
        updateDrag(m_dragPos.x(), qRound(m_dragPos.y()), mods & Qt::AltModifier,
                   mods & Qt::ControlModifier);
        updatePointAxisLock(m_dragPos, shiftHeld);
        invalidateContent();
    }

    // One meter-aware grid step: the next drawn-grid position after tick,
    // clamped to limit (which is always emitted as the final step). The
    // drawn grid restarts at every time-signature change (gridSegAt), so a
    // fixed-spacing walk would keep the old meter's spacing and phase past
    // the boundary and land points off the new grid; landing on seg.next
    // re-anchors the walk there. Fine mode is exempt: the clock grid is
    // absolute and doesn't restart at signature changes.
    uint64_t nextGridTick(uint64_t tick, bool fine, uint64_t limit) const
    {
        if (tick >= limit)
            return limit;
        const uint64_t g =
            std::max<uint64_t>(1, fine ? m_sv->fineGridTicks() : m_sv->gridTicksAt(tick));
        uint64_t next = g < limit - tick ? tick + g : limit;
        if (!fine)
            next = std::min(next, m_sv->gridSegAt(tick).next);
        return next; // > tick: g >= 1 and both limit and seg.next exceed tick
    }

    // Freehand sweep bookkeeping: fills every grid cell crossed since the
    // last mouse sample (linear interpolation, so a fast drag leaves no
    // gaps), overwriting cells swept more than once.
    void extendSweep(qreal x, bool fine)
    {
        const double rawTick = rawTickAt(x);
        const double from = m_prevTick;
        const double to = rawTick;
        const uint64_t t0 = m_sv->snapTick(std::min(from, to), fine);
        const uint64_t t1 = m_sv->snapTick(std::max(from, to), fine);
        for (uint64_t t = t0;;) {
            int v = m_dragValue;
            if (to != from) {
                const double f = std::clamp((double(t) - from) / (to - from), 0.0, 1.0);
                v = m_prevValue + int(std::llround(f * (m_dragValue - m_prevValue)));
            }
            sweepUpsert(t, v);
            if (t >= t1)
                break;
            t = nextGridTick(t, fine, t1);
        }
        m_prevTick = rawTick;
        m_prevValue = m_dragValue;
    }

    void sweepUpsert(uint64_t tick, int value)
    {
        auto it = std::lower_bound(
            m_sweep.begin(), m_sweep.end(), tick,
            [](const std::pair<uint64_t, int> &a, uint64_t t) { return a.first < t; });
        if (it != m_sweep.end() && it->first == tick)
            it->second = value;
        else
            m_sweep.insert(it, {tick, value});
    }

    // The samples a pencil stroke actually writes: a run of cells at one
    // value is one node, because the lane holds its value between points
    // and the rest are inert duplicates — a horizontal line leaves a single
    // point, not one per grid cell it crossed. The stroke's own first
    // sample is always kept, even when the lane already holds that value:
    // a pencil press is meant to leave a point (its click contract), and a
    // stroke that starts by restating the level is still an edit the user
    // can see and grab. The arrow tool's sweep keeps every sample, matching
    // the source branch, where only the pencil's stroke is canonicalized.
    std::vector<std::pair<uint64_t, int>> strokeSamples() const
    {
        if (!m_gesturePencil)
            return m_sweep;
        std::vector<std::pair<uint64_t, int>> kept;
        kept.reserve(m_sweep.size());
        for (const std::pair<uint64_t, int> &s : m_sweep) {
            if (!kept.empty() && kept.back().second == s.second)
                continue;
            kept.push_back(s);
        }
        return kept;
    }

    std::vector<SongDocument::LanePointValue>
    sweepPoints(const std::vector<std::pair<uint64_t, int>> &samples) const
    {
        std::vector<SongDocument::LanePointValue> pts;
        pts.reserve(samples.size());
        for (const std::pair<uint64_t, int> &s : samples)
            pts.push_back({s.first, s.second});
        return pts;
    }

    void paintRow(QPainter &p, const Row &row, int rowIndex, const QRect &r, bool dimUnselected)
    {
        const QRect plot(kGutterW, r.top(), width() - kGutterW, r.height());
        p.save();
        p.setClipRect(r, Qt::IntersectClip);
        p.setPen(themes::color(themes::Role::song_view_separator));
        p.drawLine(r.left(), r.bottom(), r.right(), r.bottom());

        // Gutter label.
        const QString name = rowTitle(row);
        int minV = 0, maxV = 127;
        const std::vector<LanePoint> *points = nullptr;
        QColor curve = themes::color(themes::Role::song_view_automation_default_curve);
        rowRange(row, &minV, &maxV);
        switch (row.kind) {
        case Row::Tempo:
            points = &m_sv->model().tempoLane;
            curve = themes::color(themes::Role::song_view_automation_tempo_curve);
            break;
        case Row::Voice:
            break;
        case Row::Lane:
            if (const AutoLane *lane = rowLane(row))
                points = &lane->points;
            curve = SongView::trackColor(row.track);
            break;
        }

        p.setPen(themes::color(themes::Role::song_view_primary_text));
        const auto titleFont = typography::bold(p.font());
        const auto captionFont = typography::caption(p.font());
        const auto textLayout =
            ::layout::twoLineText(titleFont, titleFont, captionFont, ::layout::Space::Zero);
        const auto textBoxes = textLayout.align(QRect(8, r.top(), kGutterW - 16, r.height()),
                                                ::layout::VerticalAlignment::Center);
        p.setFont(titleFont);
        p.drawText(textBoxes.primary, Qt::AlignLeft | Qt::AlignVCenter, name);
        p.setFont(captionFont);
        if (points && !points->empty()) {
            p.setPen(themes::color(themes::Role::song_view_secondary_text));
            p.drawText(textBoxes.secondary, Qt::AlignLeft | Qt::AlignVCenter,
                       SongView::tr("%1 points · %2..%3").arg(points->size()).arg(minV).arg(maxV));
        } else if (points && row.kind == Row::Lane) {
            p.setPen(themes::color(themes::Role::song_view_secondary_text));
            p.drawText(textBoxes.secondary, Qt::AlignLeft | Qt::AlignVCenter,
                       SongView::tr("empty · click to add points"));
        } else if (row.kind == Row::Voice && m_sv->document()) {
            int count = 0;
            for (const VoiceChange &vc : m_sv->model().voices)
                if (vc.track == m_sv->selectedTrack())
                    count++;
            p.setPen(themes::color(themes::Role::song_view_secondary_text));
            p.drawText(textBoxes.secondary, Qt::AlignLeft | Qt::AlignVCenter,
                       count ? SongView::tr("%n change(s) · click to edit", nullptr, count)
                             : SongView::tr("no voice set · click to add"));
        }

        p.setClipRect(plot, Qt::IntersectClip);
        drawPreRoll(p, m_sv, plot, kGutterW,
                    themes::color(themes::Role::song_view_piano_roll_background));
        drawGrid(p, m_sv, plot, kGutterW);

        if (row.kind == Row::Voice) {
            paintVoiceRow(p, plot);
        } else if (points) {
            const bool centerLine = row.kind == Row::Lane && row.cc == LANE_CC_BEND;
            if (pencilStrokeOnRow(rowIndex))
                paintPencilPreview(p, plot, row, *points, minV, maxV, curve, centerLine);
            else
                paintCurve(p, plot, row, *points, minV, maxV, curve, centerLine, dimUnselected,
                           nodeDragOnRow(rowIndex));
        }

        const std::pair<int, uint8_t> id = rowIdentity(row);
        drawOverlays(p, m_sv, plot, kGutterW, m_sv->timeSelectionCoversRow(id.first, id.second));
        p.restore();
    }

    // Node markers only earn their space once the curve is wide enough for
    // them to read as separate points rather than as beading on the line.
    bool nodeMarkersVisible() const { return m_sv->pxPerBeat() >= 24.0; }

    // Node geometry, font-relative so the marker keeps its weight against
    // the curve at any font scale: a ring of nodeRadius drawn with a stroke
    // of twice nodeOutlineWidth, so its outer edge lands just inside the
    // selection ring.
    static qreal nodeRadius() { return ::layout::fontPxF(3.0 / 16.0); }
    static qreal nodeOutlineWidth() { return ::layout::fontPxF(1.0 / 12.0); }
    static qreal nodeExtent() { return std::max(nodeRadius() + nodeOutlineWidth(), 4.5) + 1.0; }

    bool nodeVisible(const QRect &plot, qreal x) const
    {
        return x + nodeExtent() >= plot.left() && x - nodeExtent() <= plot.right();
    }

    // A node is a hollow ring — the curve's color over the row background —
    // not a filled dot: the ring reads as a grabbable point at a glance,
    // where a dot on a same-colored line reads as a thickening of it. Drawn
    // antialiased, since a circle this small is mostly corners without it.
    // A dimmed node fills solid instead: it is receding, so it wants less
    // structure, not more.
    void paintNode(QPainter &p, const QColor &color, const QPointF &center, bool selected = false,
                   const QColor &dimmed = QColor())
    {
        p.save();
        if (selected) {
            // Aliased like the rest of the lane painting (and exact pixels
            // keep it probeable in rollcheck).
            p.setPen(QPen(palette().highlight().color(), 2));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(center, 4.5, 4.5);
        }
        p.setRenderHint(QPainter::Antialiasing, true);
        if (dimmed.isValid()) {
            p.setPen(Qt::NoPen);
            p.setBrush(dimmed);
        } else {
            p.setPen(QPen(color, nodeOutlineWidth() * 2.0));
            p.setBrush(themes::color(themes::Role::song_view_piano_roll_background));
        }
        p.drawEllipse(center, nodeRadius(), nodeRadius());
        p.restore();
    }

    // A lone node being dragged: where the document still has it, and where
    // the gesture currently holds it.
    struct NodeDragPreview {
        uint64_t origTick = 0;
        int origValue = 0;
        uint64_t curTick = 0;
        int curValue = 0;
    };

    // The single-node Point drag in flight on this row, if any. A group drag
    // has its own whole-row preview (see paintContent), so it stays out of
    // this one — the two would otherwise draw the same move twice.
    std::optional<NodeDragPreview> nodeDragOnRow(int rowIndex) const
    {
        if (m_gesture != Gesture::Point || !m_group.empty() || rowIndex != m_dragRow ||
            m_dragOrigTick < 0)
            return std::nullopt;
        return NodeDragPreview{uint64_t(m_dragOrigTick), m_dragOrigValue, m_dragTick, m_dragValue};
    }

    // True while a live pencil stroke owns this row, so the row paints its
    // in-progress rewrite instead of the committed curve.
    bool pencilStrokeOnRow(int rowIndex) const
    {
        return m_gesture == Gesture::Sweep && m_gesturePencil && m_sweepArmed &&
               rowIndex == m_dragRow && !m_sweep.empty();
    }

    // A live pencil stroke splits the row's curve in two. The ticks the
    // stroke will overwrite — the span it has covered so far, plus the hold
    // that carries its last value to the next surviving point — paint in the
    // edit-preview color, segments, joins and nodes alike; everything
    // outside keeps the lane's own. The committed nodes inside the span are
    // left out entirely: they are what the release is about to replace, so
    // showing them would advertise points that are already gone. The result
    // is that the whole affected stretch lights up while the gesture runs.
    void paintPencilPreview(QPainter &p, const QRect &plot, const Row &row,
                            const std::vector<LanePoint> &points, int minV, int maxV,
                            const QColor &color, bool centerLine)
    {
        const int top = plot.top() + 5;
        const int bottom = plot.bottom() - 4;
        auto valueY = [&](int v) {
            return bottom - (v - minV) * (bottom - top) / std::max(1, maxV - minV);
        };
        const qreal dpr = p.device()->devicePixelRatioF();
        auto tickX = [&](uint64_t t) { return m_sv->displayX(double(t), kGutterW, dpr); };
        if (centerLine) {
            p.setPen(QPen(themes::color(themes::Role::song_view_separator), 1, Qt::DashLine));
            p.drawLine(plot.left(), valueY(0), plot.right(), valueY(0));
        }

        // The stroke overwrites exactly the ticks it has sampled, which is
        // the range writeLanePoints gets on release.
        const uint64_t begin = m_sweep.front().first;
        const uint64_t end = m_sweep.back().first;
        const QColor previewColor = themes::color(themes::Role::song_view_edit_preview_outline);
        auto drawHeld = [&](uint64_t first, uint64_t last, int value, const QColor &stroke) {
            if (first >= last)
                return;
            p.setPen(QPen(stroke, 2));
            p.drawLine(QLineF(tickX(first), valueY(value), tickX(last), valueY(value)));
        };

        // The value held into the span and the first point that survives
        // past it: the preview has to join up with both.
        std::optional<int> heldBefore;
        const LanePoint *nextAfter = nullptr;
        for (const LanePoint &point : points) {
            if (point.tick < begin)
                heldBefore = point.value;
            if (point.tick > end) {
                nextAfter = &point;
                break;
            }
        }

        // Surviving committed steps, clipped at the span's edges.
        for (size_t i = 0; i < points.size(); i++) {
            const uint64_t tick = points[i].tick;
            const uint64_t nextTick = i + 1 < points.size() ? points[i + 1].tick : begin;
            if (tick < begin) {
                drawHeld(tick, std::min(nextTick, begin), points[i].value, color);
                if (nextTick < begin)
                    p.drawLine(QLineF(tickX(nextTick), valueY(points[i].value), tickX(nextTick),
                                      valueY(points[i + 1].value)));
            } else if (tick > end) {
                if (i + 1 < points.size()) {
                    drawHeld(tick, nextTick, points[i].value, color);
                    p.drawLine(QLineF(tickX(nextTick), valueY(points[i].value), tickX(nextTick),
                                      valueY(points[i + 1].value)));
                } else {
                    p.setPen(QPen(color, 2));
                    p.drawLine(QLineF(tickX(tick), valueY(points[i].value), plot.right(),
                                      valueY(points[i].value)));
                }
            }
        }

        // The stroke itself, held-value steps like the committed curve's.
        // Painted from the samples the release will write, duplicates
        // already dropped, so the preview's nodes are the nodes that land.
        const std::vector<std::pair<uint64_t, int>> samples = strokeSamples();
        std::optional<int> value = heldBefore;
        uint64_t cursor = begin;
        for (const std::pair<uint64_t, int> &sample : samples) {
            if (value) {
                drawHeld(cursor, sample.first, *value, previewColor);
                if (*value != sample.second)
                    p.drawLine(QLineF(tickX(sample.first), valueY(*value), tickX(sample.first),
                                      valueY(sample.second)));
            }
            value = sample.second;
            cursor = sample.first;
        }
        if (value) {
            drawHeld(cursor, end, *value, previewColor);
            if (nextAfter) {
                drawHeld(end, nextAfter->tick, *value, previewColor);
                if (*value != nextAfter->value)
                    p.drawLine(QLineF(tickX(nextAfter->tick), valueY(*value),
                                      tickX(nextAfter->tick), valueY(nextAfter->value)));
            } else {
                p.setPen(QPen(previewColor, 2));
                p.drawLine(QLineF(tickX(end), valueY(*value), plot.right(), valueY(*value)));
            }
        }

        if (!nodeMarkersVisible())
            return;
        for (const LanePoint &point : points) {
            if (point.tick >= begin && point.tick <= end)
                continue;
            const qreal x = tickX(point.tick);
            if (nodeVisible(plot, x))
                paintNode(p, color, QPointF(x, valueY(point.value)));
        }
        for (const std::pair<uint64_t, int> &sample : samples) {
            const qreal x = tickX(sample.first);
            if (nodeVisible(plot, x))
                paintNode(p, previewColor, QPointF(x, valueY(sample.second)));
        }
    }

    void paintCurve(QPainter &p, const QRect &plot, const Row &row,
                    const std::vector<LanePoint> &points, int minV, int maxV, const QColor &color,
                    bool centerLine, bool dimUnselected,
                    const std::optional<NodeDragPreview> &drag = std::nullopt)
    {
        const int top = plot.top() + 5;
        const int bottom = plot.bottom() - 4;
        auto valueY = [&](int v) {
            return bottom - (v - minV) * (bottom - top) / std::max(1, maxV - minV);
        };
        const qreal dpr = p.device()->devicePixelRatioF();
        auto tickX = [&](uint64_t t) { return m_sv->displayX(double(t), kGutterW, dpr); };
        // A drag paints the row's PENDING points, not its committed ones: the
        // curve has to read exactly as it will the instant the mouse comes up,
        // so the node's old position closes up behind it (its neighbors join
        // across the gap) and its new neighbors are the ones it connects to.
        // The point it lands on top of drops out — the release overwrites
        // that one, so drawing it would advertise a node already gone.
        std::vector<LanePoint> pending;
        size_t movedIndex = 0; // where the dragged node sits in `pending`
        if (drag) {
            pending.reserve(points.size() + 1);
            for (const LanePoint &point : points) {
                if (point.tick == drag->origTick && point.value == drag->origValue)
                    continue;
                if (point.tick == drag->curTick)
                    continue;
                pending.push_back(point);
            }
            const LanePoint moved{uint32_t(drag->curTick), drag->curValue};
            const auto at = std::lower_bound(
                pending.begin(), pending.end(), moved,
                [](const LanePoint &a, const LanePoint &b) { return a.tick < b.tick; });
            movedIndex = size_t(at - pending.begin());
            pending.insert(at, moved);
        }
        const std::vector<LanePoint> &curve = drag ? pending : points;
        // The dragged node and the segments it owns — the neighbor's hold
        // carried into it and its own hold out again — carry the edit-preview
        // color, so the pending edit still reads as pending. Its marker and
        // value chip ride on top from paintContent.
        const QColor previewColor = themes::color(themes::Role::song_view_edit_preview_outline);
        auto movedHere = [&](size_t i) { return drag && i == movedIndex; };

        if (centerLine) {
            p.setPen(QPen(themes::color(themes::Role::song_view_separator), 1, Qt::DashLine));
            p.drawLine(plot.left(), valueY(0), plot.right(), valueY(0));
        }
        const SongView::TimeSelection &sel = m_sv->timeSelection();
        const bool laneSelected = rowInLaneSelection(row);
        const QColor dimColor = palette().mid().color();
        for (size_t i = 0; i < curve.size(); i++) {
            const qreal x0 = tickX(curve[i].tick);
            const qreal x1 = i + 1 < curve.size() ? tickX(curve[i + 1].tick) : qreal(plot.right());
            if (x1 < plot.left() || x0 > plot.right())
                continue;
            // The hold into the dragged node belongs to the edit as much as
            // the hold out of it: both change with every mouse move.
            p.setPen(QPen(movedHere(i) || movedHere(i + 1) ? previewColor : color, 2));
            const int y = valueY(curve[i].value);
            p.drawLine(QLineF(x0, y, x1, y)); // hold value until the next point
            if (i + 1 < curve.size())
                p.drawLine(QLineF(x1, y, x1, valueY(curve[i + 1].value)));
        }

        if (!nodeMarkersVisible())
            return;
        // Nodes come after the whole curve, and selected ones after the
        // rest, so a later segment can never paint over an earlier node and
        // rings always sit on top of their neighbors' markers.
        for (int selectedPass = 0; selectedPass < 2; selectedPass++) {
            for (size_t i = 0; i < curve.size(); i++) {
                const LanePoint &point = curve[i];
                // Each node draws in exactly one style: the edit-preview
                // color while it is the one being dragged, a highlight ring
                // when it is in the derived selection (or under the open
                // point menu's aim), dimmed when a multi-node selection is
                // elsewhere, its curve color otherwise.
                const bool selected =
                    (laneSelected && tickSelected(sel, point.tick)) || pointMenuAimedAt(row, point);
                if (selected != bool(selectedPass))
                    continue;
                const qreal x = tickX(point.tick);
                if (!nodeVisible(plot, x))
                    continue;
                paintNode(p, movedHere(i) ? previewColor : color, QPointF(x, valueY(point.value)),
                          selected, dimUnselected && !laneSelected ? dimColor : QColor());
            }
        }
    }

    void paintVoiceRow(QPainter &p, const QRect &plot)
    {
        const SongViewModel &model = m_sv->model();
        const int selected = m_sv->selectedTrack();
        std::vector<const VoiceChange *> changes;
        for (const VoiceChange &vc : model.voices)
            if (vc.track == selected)
                changes.push_back(&vc);

        const QColor color = SongView::trackColor(selected);
        const qreal dpr = p.device()->devicePixelRatioF();
        for (size_t i = 0; i < changes.size(); i++) {
            const qreal x = m_sv->displayX(double(changes[i]->tick), kGutterW, dpr);
            const qreal xEnd = i + 1 < changes.size()
                                   ? m_sv->displayX(double(changes[i + 1]->tick), kGutterW, dpr)
                                   : plot.right();
            if (xEnd < plot.left() || x > plot.right())
                continue;
            p.setPen(QPen(color, 2));
            p.drawLine(QLineF(x, plot.top() + 4, x, plot.bottom() - 4));
            p.setPen(themes::color(themes::Role::song_view_primary_text));
            const QString text = m_sv->voiceLabel(changes[i]->program);
            // Keep the label readable while its voice region is scrolled
            // partially off the left edge.
            const qreal textX = std::max<qreal>(x + 4, plot.left() + 4);
            const qreal textW = std::max<qreal>(10.0, xEnd - textX - 4);
            p.drawText(QRectF(textX, plot.top() + 4, textW, plot.height() - 8),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       fontMetrics().elidedText(text, Qt::ElideRight, qFloor(textW)));
        }
    }

    // Left-drag gestures: Point moves an existing point (press landed on
    // its dot; Shift locks the drag to the axis of first travel), Sweep
    // freehand-draws a stream of points, Line (Shift off-dot) commits an
    // interpolated ramp between press and release. Alt snaps to the clock
    // grid instead of the visible grid throughout; Ctrl magnetizes the value
    // to the lane's neutral (pan/tune center, zero bend); double-click opens
    // a type-in for the exact value.
    enum class Gesture { None, Point, Sweep, Line };

    SongView *m_sv;
    QScrollArea *m_scroll; // hosting scroll area, for lane-zoom pinning
    std::vector<Row> m_rows;
    int m_laneH = kLaneH;             // shared row height; Ctrl+wheel rescales
    int m_laneZoomAccum = 0;          // sub-notch wheel remainder, like zoomKeyHeight
    QHash<QString, int> m_rowHeights; // individual row heights (rowKey → px)
    QHash<QString, int> m_rowRanges;  // display max per lane (rowKey → value;
                                      // 0 = auto-fit); absent = CC default
    bool m_tempoLaneVisible = false;  // the global tempo row is in m_rows
    QSet<QString> m_hiddenLanes;      // hidden CC lanes (laneKey); their rows
                                      // are skipped by rebuildRows
    int m_resizeRow = -1;             // row whose bottom divider is being dragged
    int m_resizeOrigH = 0;
    int m_resizePressY = 0;
    bool m_panning = false;    // middle-drag pan
    QPointF m_panPos;          // last pan sample, global coords
    bool m_rightPress = false; // right button held; sweep vs. click undecided
    bool m_selSweep = false;   // right-drag time-selection sweep is live
    QPoint m_rightPressPos;
    int m_rightRow = -1; // row of the right press
    uint64_t m_selAnchorTick = 0;
    Gesture m_gesture = Gesture::None;
    int m_dragRow = -1;
    int64_t m_dragOrigTick = -1;          // existing point being moved, -1 = new point
    int m_dragOrigValue = 0;              // that point's original value, the axis-lock pin
    QPointF m_pointPressPos;              // point-drag press, the axis-lock travel origin
    QPointF m_dragPos;                    // last cursor sample of the live left drag, for
                                          // re-running it on a stationary Shift change
    AxisLock m_axisLock = AxisLock::None; // Shift lock on the live point drag
    uint64_t m_dragTick = 0;
    int m_dragValue = 0;
    std::vector<GroupNode> m_group;                // the live Point drag moves these together
                                                   // (empty = plain single-point move)
    SongView::TimeSelection m_groupSelAtPress;     // the selection m_group latched, so the
                                                   // commit's band-follow shifts only that band
    std::vector<std::pair<uint64_t, int>> m_sweep; // tick-sorted freehand samples
    bool m_gesturePencil = false;                  // tool latched at the press for the gesture
    QPointF m_sweepPressPos;                       // sweep press, the activation-slop origin
    QPointF m_sweepSlopOrigin;                     // where the slop was cleared; the offset from
                                                   // m_sweepPressPos is subtracted from the stroke
    bool m_sweepArmed = false;         // the sweep cleared its slop (always, for a pencil)
    QPointF m_pencilSlopOrigin;        // where the pencil's vertical slop wedge is measured from
    bool m_pencilSlopExceeded = false; // the stroke broke out of the wedge (latched)
    bool m_pointShiftSeen = false;     // Shift during the point drag: no click-delete
    bool m_pointTraveled = false;      // the point drag cleared the activation slop
    bool m_clickDeleted = false;       // the last click deleted a node (the pair's
                                       // double-click is spent)
    uint64_t m_lineStartTick = 0;      // Shift-drag anchor
    int m_lineStartValue = 0;
    double m_prevTick = 0.0; // last raw (unsnapped) sweep sample
    int m_prevValue = 0;
    int m_hoverRow = -1;      // row under an idle cursor; -1 = no readout
    double m_hoverTick = 0.0; // raw tick under the idle cursor
    qreal m_hoverX = 0.0;     // raw cursor x of the idle hover — the dot hit
                              // and the voice anchor need the true pixel
    int m_hoverY = 0;         // cursor y of the idle hover, for the dot hit
    // The point the context menu is aimed at. The row is copied by value
    // (its identity, not an index) so a row rebuild can't dangle it — the
    // action re-derives the document target from it — and the document
    // revision pins the DocLanePoint, whose index goes stale the moment
    // any edit lands.
    struct PointMenuTarget {
        Row row;
        DocLanePoint point;
        uint64_t revision = 0;
    };
    ui::ContextMenu *m_pointMenu = nullptr; // kept alive like the roll's note menu
    QAction *m_pointSetValue = nullptr;
    QAction *m_pointDelete = nullptr;
    std::optional<PointMenuTarget> m_pointMenuTarget;
    bool m_pencilMode = false;
    QCursor m_pencilCursor; // cached per device pixel ratio
    qreal m_pencilCursorDpr = 0.0;
};

// --------------------------------------------------------------- VelocityLane

namespace {

// Node geometry and line weights in DIPs, so the lane keeps its designed
// look at any display scale (Qt scales the logical units it paints in).
constexpr double kVelNodeRadius = 3.5;
constexpr double kVelNodeOutline = 1.0;
constexpr double kVelSelRingRadius = 5.0;
constexpr double kVelSelRingWidth = 1.5;
constexpr double kVelStemWidth = 1.5;
constexpr double kVelSelStemWidth = 2.5;
// Ruler density steps, as multiples of the base font size: the taller the
// lane, the finer the graduations (VelocityAxis picks the band).
constexpr double kVelDensityD1 = 6.0;
constexpr double kVelDensityD2 = 25.0 / 3.0;
constexpr double kVelDensityD3 = 12.0;
constexpr double kVelDensityD4 = 24.0;
// Pointer targets, in DIPs: how near a node's center counts as grabbing it,
// and how far above/below (and past either end of) a stem still grabs the
// stem. The node's reach is wider than the painted circle so a small target
// stays clickable; the stem's is thin so a node behind it still wins.
constexpr double kVelNodeGrabRadius = 6.0;
constexpr double kVelStemGrabRadius = 4.0;
constexpr double kVelStemGrabSlop = 2.0;

// A node outline that survives any theme and any fill: whichever of black or
// white holds up better against both the plot background and the fill it
// rings. The grid ink a node is normally outlined with can be dialed away
// entirely (View → Grid Line Contrast), which a flat track color survives on
// its own — the velocity ramp does not, since its midrange sits at 1.0:1
// against a light roll background and both its ends near 2:1 against a dark
// one. A 7-DIP dot needs the edge more than the roll's note blocks do.
QColor velNodeOutlineInk(const QColor &fill, const QColor &background)
{
    const auto worst = [&fill, &background](const QColor &ink) {
        return std::min(themes::contrastRatio(ink, fill), themes::contrastRatio(ink, background));
    };
    const QColor white(Qt::white);
    const QColor black(Qt::black);
    return worst(white) > worst(black) ? white : black;
}

// Whether a press's modifiers ask for exact velocities instead of the PSG
// voice's detents. allowShift lets the Shift a ramp is already holding ride
// along with the bound chord, so a ramp can be unlocked too; the dialog
// refuses to bind this command to a Shift-bearing chord for the same reason.
bool velDetentUnlockHeld(Qt::KeyboardModifiers modifiers, bool allowShift)
{
    const Qt::KeyboardModifiers binding =
        keymap::Registry::instance().modifierBinding(QStringLiteral("velocity.detent_unlock"));
    if (binding == Qt::NoModifier)
        return false;
    const Qt::KeyboardModifiers held =
        modifiers & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);
    return held == binding || (allowShift && held == (binding | Qt::ShiftModifier));
}

} // namespace

// Velocity lane (View → Velocity Lane, default V): the selected track's
// notes as nodes on the shared timeline — a filled circle at (start tick,
// velocity) with a stem spanning the note's duration — beside a value ruler
// over the whole 1-127 velocity domain. There is no vertical zoom; the lane's
// height only decides how densely the ruler is graduated.
//
// The lane mirrors the roll's note selection (selected nodes ring, and the
// rest dim once more than one is selected) and shares the timeline's camera.
// Nodes carry the track's color, or the roll's velocity ramp under View →
// Color Notes by Velocity — see paintNodes for what that costs in contrast.
// A left drag on a node or its stem moves the whole selection's velocities
// together, a drag from empty plot paints across the selected notes it
// crosses, Shift ramps them along a line, and a click on a printed ruler
// value sets them all to it. Every gesture is deferred — the pointer only
// moves a preview, and the document mutates once, as one undo entry, when the
// button comes up.
//
// On a PSG voice the lane switches to that channel's real loudness detents
// (VelocityMap): the ruler becomes one labeled row per level, nodes sit at
// their level's center, the level boundaries paint across the plot for as
// far as the voice lasts, and edits move whole levels instead of stored
// values. Those levels are computed from the track's compiled VOL byte as
// well as the velocity — the song's master volume is folded into it — so a
// quiet song has fewer of them, and a section too quiet to reach the first
// envelope step has none at all and keeps the plain ruler. The header's
// Detents checkbox turns that off for the track, and the velocity.detent_unlock
// chord (Ctrl by default, read at the press) unlocks exact values for one
// gesture. Still to come: the marquee.
class VelocityLane : public TimelineSurface
{
  public:
    explicit VelocityLane(SongView *sv) : TimelineSurface(nullptr), m_sv(sv)
    {
        setObjectName(QStringLiteral("velocityLane")); // findChild for tests
        setMinimumHeight(kVelLaneMinH);
        setMouseTracking(true);
        // Range shortcuts (and the lane's own V toggle) work from here too,
        // so a click focuses the lane like the roll and the lanes area.
        setFocusPolicy(Qt::ClickFocus);
        // A real checkbox rather than a painted chip: detents are a plain
        // per-track on/off, and a checkbox is what says so — hover, press,
        // focus ring, keyboard and screen-reader name all come from the
        // style. It lives in the header column, laid out by syncDetentCheck.
        m_detentCheck = new QCheckBox(SongView::tr("Detents"), this);
        m_detentCheck->setObjectName(QStringLiteral("velocityDetentCheck")); // findChild for tests
        m_detentCheck->setFont(typography::caption(font()));
        // Tab reaches it, a click does not take focus — like the track
        // headers' M/S buttons, so clicking it never quietly moves the
        // lane's keyboard shortcuts off the lane.
        m_detentCheck->setFocusPolicy(Qt::TabFocus);
        m_detentCheck->setChecked(m_useDetents);
        m_detentCheck->setToolTip(
            SongView::tr("Snap velocities to the voice's own volume levels."));
        m_detentCheck->hide(); // shown once a context with detents is painted
        connect(m_detentCheck, &QCheckBox::toggled, this, [this](bool on) {
            m_useDetents = on;
            const VelocityMap context = currentContext();
            m_sv->announce(on ? SongView::tr("Velocity detents on — %1 has %2 volume levels.")
                                    .arg(QString::fromLatin1(context.voiceName()))
                                    .arg(context.levelCount())
                              : SongView::tr("Velocity detents off — exact velocities."));
            invalidateContent();
        });
    }

    // A mouse gesture is live (the pan or a velocity edit); follow-scroll
    // pauses so the camera cannot slide out from under the pointer.
    bool gestureActive() const { return m_panning || m_gesture != Gesture::None; }

    // The preview a live gesture holds a note at, for the roll: nothing when
    // no gesture holds this note, so the roll falls back on the stored value.
    std::optional<uint8_t> previewVelocity(NoteId noteId) const
    {
        return m_gestureModel.previewVelocity(noteId);
    }

    // The document changed under a live gesture (an edit from elsewhere, or
    // undo/redo): the preview describes notes that may no longer be there,
    // so it dies rather than committing blind.
    void documentChanged()
    {
        if (m_gesture == Gesture::None)
            return;
        const bool hadPreview = m_previewed;
        cancelGesture();
        if (hadPreview)
            m_sv->announce(SongView::tr("Velocity edit cancelled because notes changed."));
    }

  protected:
    void paintContent(QPainter &p) override
    {
        const qreal dpr = p.device()->devicePixelRatioF();
        p.fillRect(rect(), themes::color(themes::Role::song_view_piano_roll_background));
        p.fillRect(QRect(0, 0, std::min(kGutterW, width()), height()),
                   themes::color(themes::Role::song_view_timeline_chrome_background));
        p.setPen(themes::color(themes::Role::song_view_separator));
        p.drawLine(0, 0, width(), 0);
        p.drawLine(kGutterW - 1, 0, kGutterW - 1, height());

        const int textInset = lyt::space(Space::Two);
        p.setFont(font());
        p.setPen(themes::color(themes::Role::song_view_primary_text));
        p.drawText(QRect(textInset, 0, kHeaderW - 2 * textInset, height()), Qt::AlignVCenter,
                   SongView::tr("Velocity"));
        if (!m_sv->timeline())
            return;

        const VelocityMap context = currentContext();
        // Leaving PSG rearms the detents: the checkbox is a per-track choice
        // about a voice, not a mode the lane keeps once that voice is gone
        // (or once the volume has left it nothing to snap between).
        // Read off the track's own context, never the hover — passing the
        // pointer over a DirectSound node must not undo the choice. Doing it
        // here is safe because a continuous context paints the same either
        // way, so nothing needs repainting.
        if (!trackContext().hasDetents())
            m_useDetents = true;
        syncDetentCheck(context);
        const std::vector<uint8_t> active = activeVelocities();
        const VelocityAxis axis(axisMap(context), axisGeometry(), active.data(), active.size());
        // Test mirrors of the ruler being painted, like the lanes' own
        // hoverNodeTick: the checks read node placement off the same axis.
        setProperty("velocityAxisTop", axis.top());
        setProperty("velocityAxisBottom", axis.bottom());
        setProperty("velocityTickCount", int(axis.tickCount()));
        setProperty("velocityDimmed", dimUnselectedNodes());
        setProperty("velocityMarkerCount", int(axis.markerCount()));
        setProperty("velocityIntrinsic", axis.mode() == VelocityAxis::Mode::Intrinsic);
        setProperty("velocityLevelCount", int(axis.map().levelCount()));
        // -1 where the checkbox is not offered at all, so a check can tell
        // "no detents to switch here" from "detents turned off"; read off the
        // widget itself, so the mirror cannot disagree with what is on screen.
        setProperty("velocityDetents",
                    m_detentCheck->isHidden() ? -1 : (m_detentCheck->isChecked() ? 1 : 0));
        VelocityAxisPaintStyle style;
        style.labelColor = themes::color(themes::Role::song_view_secondary_text);
        style.accentColor = themes::color(themes::Role::song_view_selection_edge);
        style.labelFont = typography::caption(font());
        style.emphasizedFont = typography::bold(style.labelFont);
        style.separatorX = kGutterW - 1;
        style.labelLeft = kHeaderW + textInset;
        style.labelWidth = std::max(0, kKeyboardW - 2 * textInset);
        style.labelHeight = QFontMetrics(style.labelFont).height();
        style.tickWidth = lyt::singlePixel();
        style.markerWidth = 1.5;
        style.minorTickLength = lyt::space(Space::One);
        style.majorTickLength = lyt::space(Space::Two);
        style.markerTickLength = lyt::space(Space::Two);
        axis.paintRuler(p, style);

        const QRect plot(kGutterW, 0, width() - kGutterW, height());
        p.save();
        p.setClipRect(plot, Qt::IntersectClip);
        drawPreRoll(p, m_sv, plot, kGutterW,
                    themes::color(themes::Role::song_view_piano_roll_background));
        drawGrid(p, m_sv, plot, kGutterW);
        paintLevelLines(p, plot, axis, dpr);
        paintNodes(p, plot, axis, dpr);
        if (m_gesture == Gesture::Ramp) {
            // The line the ramp is reading its values off, drawn in the
            // lanes' preview ink so the gesture is legible over the nodes.
            p.setPen(QPen(themes::color(themes::Role::song_view_edit_preview_outline),
                          lyt::singlePixel()));
            p.setBrush(Qt::NoBrush);
            p.drawLine(m_pressPos, m_prevPos);
        }
        if (m_gesture == Gesture::Band) {
            // The roll's own band: a dashed selection edge over a wash of
            // the same color.
            QColor band = themes::color(themes::Role::song_view_selection_edge);
            p.setPen(QPen(band, lyt::singlePixel(), Qt::DashLine));
            band.setAlpha(30);
            p.setBrush(Qt::NoBrush);
            p.fillRect(m_bandRect, band);
            p.drawRect(m_bandRect);
        }
        drawOverlays(p, m_sv, plot, kGutterW,
                     m_sv->timeSelectionCoversTrack(m_sv->selectedTrack()));
        p.restore();
    }

    void wheelEvent(QWheelEvent *event) override
    {
        // The roll's bindings: plain wheel over the plot zooms the timeline
        // at the cursor, Shift (or a trackpad's horizontal delta) scrolls.
        // The lane has no vertical zoom, so the ruler column swallows
        // nothing and simply passes the wheel on.
        const QPoint delta = wheelDelta(event);
        const int d = delta.y() ? delta.y() : delta.x();
        if (event->modifiers() & Qt::ShiftModifier) {
            m_sv->scrollByPx(-d);
        } else if (delta.x() && !delta.y()) {
            m_sv->scrollByPx(-delta.x());
        } else if (event->position().x() < kGutterW) {
            event->ignore();
            return;
        } else {
            const double zoomDelta = wheelAngleUnits(event);
            if (zoomDelta != 0.0)
                m_sv->zoomAroundContentX(std::pow(1.0015, zoomDelta),
                                         event->position().x() - kGutterW);
        }
        event->accept();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::RightButton) {
            // The marquee, deferred like the roll's: a drag bands the nodes
            // it covers, and a release in place is a click on whatever was
            // under the press. Never over a live edit, and never in the
            // ruler or header columns — there are no nodes to band there.
            const QPointF pos = event->position();
            if (m_gesture != Gesture::None || m_panning || !m_sv->document() || !m_sv->timeline() ||
                pos.x() < kGutterW) {
                event->ignore();
                return;
            }
            m_pressPos = m_prevPos = pos;
            m_bandRect = QRectF(pos, pos);
            m_selBeforePress = m_sv->selection();
            m_ctrlPress = event->modifiers() & Qt::ControlModifier;
            const ViewNote *hit = noteAt(pos);
            m_pressed = hit ? std::optional<ViewNote>(*hit) : std::nullopt;
            // The roll's rule: a press on an unselected node takes the
            // selection over at once, so the band that may follow starts
            // from what the user just pointed at.
            if (hit && !m_ctrlPress && !m_sv->isSelected(*hit))
                m_sv->setSelection({{hit->startTick, hit->key}});
            m_gesture = Gesture::PendingBand;
            event->accept();
            return;
        }
        if (event->button() == Qt::MiddleButton) {
            // Reaper-style pan, tracked in global coordinates like the lanes'.
            // Never while an edit is live: panning moves the ticks the
            // gesture's press coordinates were measured against.
            if (m_gesture != Gesture::None) {
                event->ignore();
                return;
            }
            m_panning = true;
            m_panPos = event->globalPosition();
            setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
        if (event->button() != Qt::LeftButton || m_panning || m_gesture != Gesture::None ||
            !m_sv->document() || !m_sv->timeline()) {
            // Nothing here edits without a document, and a live marquee owns
            // the pointer; leave the press to the parent rather than
            // swallowing it.
            event->ignore();
            return;
        }
        const QPointF pos = event->position();
        m_pressPos = m_prevPos = pos;
        m_selBeforePress = m_sv->selection();
        m_pressed.reset();
        m_ctrlPress = event->modifiers() & Qt::ControlModifier;
        // Read the unlock chord once, at the press: the values a gesture
        // lands on must not change because the chord arrived mid-drag. Only
        // a press in the plot may bring its own Shift (the ramp's).
        m_detentUnlock = detentsUnlocked(event->modifiers(), pos.x() >= kGutterW);
        if (pos.x() < kHeaderW) {
            // The track header column is not the lane's. Its one control, the
            // Detents checkbox, is a child widget and took the press itself.
            event->ignore();
            return;
        }
        if (pos.x() < kGutterW) {
            // The ruler: a click on one of its printed values sets the whole
            // selection to it outright — press to release, one edit.
            rulerClick(pos);
            event->accept();
            return;
        }
        if (event->modifiers() & Qt::ShiftModifier) {
            // Ramp: the press anchors one end of a line, the pointer is the
            // other, and every selected note under it takes the line's value.
            m_gesture = Gesture::Ramp;
            m_activated = false;
            openSession();
            updateRamp(pos);
            event->accept();
            return;
        }
        const ViewNote *hit = noteAt(pos);
        if (!hit) {
            // Empty plot: a paint stroke, brushing the selected notes it
            // crosses. It starts painting at the press — a click straight
            // below a selected node is how you set that one note — and a
            // stroke that touches nothing is a click, which clears the
            // selection on the release.
            m_gesture = Gesture::Paint;
            m_activated = false;
            paintBetween(pos, pos);
            event->accept();
            return;
        }
        m_pressed = *hit;
        // An unselected node's press takes the selection over; Ctrl adds to
        // it. Either way the drag that may follow moves everything selected.
        if (m_ctrlPress) {
            std::vector<SongView::NoteKey> selection = m_selBeforePress;
            if (!m_sv->isSelected(*hit))
                selection.push_back({hit->startTick, hit->key});
            m_sv->setSelection(std::move(selection));
        } else if (!m_sv->isSelected(*hit)) {
            m_sv->setSelection({{hit->startTick, hit->key}});
        }
        m_gesture = Gesture::Relative;
        m_activated = false;
        openSession();
        // Sound the grabbed node so a press gives the same feedback the roll's
        // does; the drag re-auditions from here as the value moves.
        auditionNote(hit->key, hit->velocity, hit->startTick);
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const QPointF pos = event->position();
        if (m_panning) {
            const QPointF d = event->globalPosition() - m_panPos;
            m_panPos = event->globalPosition();
            m_sv->scrollByPx(-d.x());
            event->accept();
            return;
        }
        if (m_gesture == Gesture::None) {
            // Idle pointer: the node under it is what the ruler describes.
            setHovered(pos.x() >= kGutterW ? noteAt(pos) : nullptr);
            event->ignore();
            return;
        }
        if (m_gesture == Gesture::PendingBand &&
            (pos - m_pressPos).manhattanLength() >= QApplication::startDragDistance()) {
            // Two dimensions of travel here, so the platform's own drag
            // distance decides — unlike the edits, which only move on y.
            m_gesture = Gesture::Band;
        }
        if (m_gesture == Gesture::Relative)
            updateRelative(pos);
        else if (m_gesture == Gesture::Paint)
            paintBetween(m_prevPos, pos);
        else if (m_gesture == Gesture::Ramp)
            updateRamp(pos);
        else if (m_gesture == Gesture::Band)
            updateBand(pos);
        m_prevPos = pos;
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::MiddleButton && m_panning) {
            m_panning = false;
            unsetCursor();
            event->accept();
            return;
        }
        // Whatever the release turns out to mean, the gesture stops sounding
        // with the button, exactly like the roll's.
        stopAudition();
        if (event->button() == Qt::RightButton &&
            (m_gesture == Gesture::Band || m_gesture == Gesture::PendingBand)) {
            if (m_gesture == Gesture::Band) {
                // The band replaces the selection; Ctrl adds to what the
                // press found instead.
                std::vector<SongView::NoteKey> selection =
                    m_ctrlPress ? m_selBeforePress : std::vector<SongView::NoteKey>();
                for (const SongView::NoteKey &id : m_bandPreview) {
                    if (std::find(selection.begin(), selection.end(), id) == selection.end())
                        selection.push_back(id);
                }
                m_sv->setSelection(std::move(selection));
            } else {
                // A right-click in place: Ctrl toggles the node under it,
                // and a bare click on empty plot clears the selection. A
                // plain click on a node has already selected it.
                if (m_ctrlPress && m_pressed)
                    clickSelect();
                else if (!m_ctrlPress && !m_pressed)
                    m_sv->setSelection({});
            }
            finishGesture(false);
            event->accept();
            return;
        }
        if (event->button() != Qt::LeftButton || m_gesture == Gesture::None ||
            m_gesture == Gesture::Band || m_gesture == Gesture::PendingBand) {
            event->ignore();
            return;
        }
        if (m_gesture == Gesture::Paint) {
            // A stroke that never reached a selected node is a click, and a
            // click on empty space clears the selection and parks the edit
            // cursor at the press, exactly like the roll's.
            const bool painted = !m_frozen.empty();
            if (!painted) {
                m_sv->setSelection({});
                m_sv->commitEditCursor(m_sv->snapTick(
                    m_sv->tickAtContentX(std::max(qreal(kGutterW), m_pressPos.x()) - kGutterW)));
            }
            finishGesture(painted);
        } else if (m_gesture == Gesture::Ramp) {
            // A Shift click is still a click: without the activation travel
            // the ramp asked for nothing.
            finishGesture((m_prevPos - m_pressPos).manhattanLength() >= dragActivationDistance());
        } else {
            // Under the activation slop the press was a click: Ctrl toggles
            // this node's membership, a plain click collapses onto it.
            if (!m_activated)
                clickSelect();
            finishGesture(m_activated);
        }
        event->accept();
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape && m_gesture != Gesture::None) {
            // Abandon the preview and put back the selection the press found.
            cancelGesture();
            event->accept();
            return;
        }
        // Shared roll/lanes shortcuts (the V toggle included) reach the
        // focused surface first, exactly like the lanes area.
        if (m_sv->handleEditKey(event))
            return;
        if (event->key() == Qt::Key_Escape) {
            // Nothing live: Escape drops the selections, like the roll's.
            m_sv->clearSelection();
            m_sv->clearTimeSelection();
            invalidateContent();
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void focusOutEvent(QFocusEvent *event) override
    {
        cancelGesture();
        TimelineSurface::focusOutEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        setHovered(nullptr);
        TimelineSurface::leaveEvent(event);
    }

    bool event(QEvent *event) override
    {
        // Losing the grab (a modal opening, a drag leaving the window) ends
        // the gesture without a release ever arriving.
        if (event->type() == QEvent::UngrabMouse)
            cancelGesture();
        return TimelineSurface::event(event);
    }

    void keyReleaseEvent(QKeyEvent *event) override
    {
        if (!m_sv->handleEditKeyRelease(event))
            QWidget::keyReleaseEvent(event);
    }

  private:
    void contextMenuEvent(QContextMenuEvent *event) override
    {
        // The right button is the marquee's here; there is no lane menu for
        // it to fight with, and the press must not raise one from a parent.
        event->accept();
    }

    // Left-drag gestures. Relative moves every selected note's velocity by
    // the drag's own vertical distance (and is what a ruler click commits
    // through); Paint is the empty-space stroke that brushes the selected
    // notes it crosses, and whose bare click clears the selection; Ramp is
    // the Shift drag, a straight line laid across the selection. The right
    // button's marquee waits as PendingBand until the pointer travels, since
    // a right-click in place means something else.
    enum class Gesture { None, Relative, Paint, Ramp, PendingBand, Band };

    // Travel that turns a press into a drag; the lanes' own figure, so a
    // click in either surface tolerates the same hand jitter.
    static int dragActivationDistance() { return lyt::fontPx(5.0 / 12.0); }

    // The value ruler's mapping. It only needs the geometry and the voice
    // context — the markers the painted axis also carries move no graduation
    // — so a gesture can build one without knowing the selection.
    VelocityAxis plotAxis() const
    {
        return VelocityAxis(axisMap(currentContext()), axisGeometry());
    }

    // The voice whose detents the lane is editing in, or a continuous map
    // when there is none to agree on. The pointer's own note wins (it is the
    // note being asked about), then the selection — which must resolve to
    // one compatible PSG channel, since the channels' level tables differ —
    // and with neither, the voice in effect at the display position.
    VelocityMap currentContext() const
    {
        if (const ViewNote *hovered = hoveredNote())
            return contextForNote(*hovered);
        return trackContext();
    }

    // The context without the pointer in it: what the lane is editing when
    // nothing is hovered. The Detents checkbox is a choice about this, not about
    // wherever the pointer happens to rest.
    VelocityMap trackContext() const
    {
        std::optional<VelocityMap> shared;
        for (const ViewNote &note : m_sv->model().notes) {
            if (note.track != m_sv->selectedTrack() || !m_sv->isSelected(note))
                continue;
            const VelocityMap map = contextForNote(note);
            if (!map.hasDetents() || (shared && !shared->compatibleWith(map)))
                return VelocityMap();
            shared = map;
        }
        if (shared)
            return *shared;
        const SongView::VoiceContext context = m_sv->voiceContext(m_sv->displayTick());
        return VelocityMap::resolve(context.voice, std::nullopt, context.trackVolume,
                                    context.trackPan);
    }

    // A note's own voice, resolved with its key so a keysplit answers for
    // the sound this note actually makes — and at the volume and pan in force
    // where it starts, since those are half of what its channel can be heard
    // doing.
    VelocityMap contextForNote(const ViewNote &note) const
    {
        const SongView::VoiceContext context = m_sv->voiceContext(note.startTick);
        return VelocityMap::resolve(context.voice, note.key, context.trackVolume, context.trackPan);
    }

    // The map the ruler is built from: the context, unless the track's
    // detents are switched off, which puts the plain 1-127 ruler back.
    VelocityMap axisMap(const VelocityMap &context) const
    {
        return m_useDetents ? context : VelocityMap();
    }

    // Whether this gesture writes exact stored velocities instead of the
    // voice's representatives — the chip switched off, or the unlock chord
    // held at the press.
    bool detentsUnlocked(Qt::KeyboardModifiers modifiers, bool allowShift) const
    {
        return !m_useDetents || velDetentUnlockHeld(modifiers, allowShift);
    }

    // Where a note's node sits: on a PSG voice the center of its level's
    // row, since every stored value in that row sounds the same, and its
    // exact velocity otherwise (or while this gesture is writing exact
    // values, so an unlocked drag can be seen moving between the detents).
    double yForNote(const VelocityAxis &axis, const ViewNote &note) const
    {
        const uint8_t velocity = displayVelocity(note);
        const std::optional<VelocityMap> frozen = frozenMap(note.noteId);
        if (!m_useDetents || (frozen && m_detentUnlock))
            return axis.velocityToY(velocity);
        const VelocityMap map = frozen.value_or(contextForNote(note));
        if (const std::optional<std::size_t> level = map.levelOf(velocity))
            return levelCenterY(axis, map, int(*level));
        return axis.velocityToY(velocity);
    }

    // The y where two of a map's levels meet, and the center of the band a
    // level owns. Stated for any map, not just the axis's own: the plot
    // paints the boundaries of whichever voice each section of the song
    // plays on.
    static double levelBoundaryY(const VelocityAxis &axis, const VelocityMap &map, int lowerLevel)
    {
        return (axis.velocityToY(map.levelRange(lowerLevel).last) +
                axis.velocityToY(map.levelRange(lowerLevel + 1).first)) /
               2.0;
    }

    static double levelCenterY(const VelocityAxis &axis, const VelocityMap &map, int level)
    {
        const double lower = level == 0 ? axis.bottom() : levelBoundaryY(axis, map, level - 1);
        const double upper =
            level + 1 == int(map.levelCount()) ? axis.top() : levelBoundaryY(axis, map, level);
        return (lower + upper) / 2.0;
    }

    // The velocity a preview writes for one note under this gesture: the
    // exact value while unlocked, the note's own representative otherwise
    // (which is why a mixed selection still detents each PSG note it holds).
    uint8_t resolveVelocity(const VelocityMap &map, int proposed) const
    {
        if (m_detentUnlock)
            return uint8_t(std::clamp(proposed, 1, 127));
        return map.canonicalize(proposed);
    }

    // The note under the pointer. It keeps answering through the press and
    // the whole gesture that follows — the hover decides the context, and
    // the context decides whether an edit moves levels or values, so a
    // gesture must go on meaning what the ruler said when it started. Moves
    // stop updating it while a gesture runs, and its end drops it (the
    // pointer having travelled unwatched meanwhile).
    const ViewNote *hoveredNote() const
    {
        if (!m_hovered.isAssigned())
            return nullptr;
        for (const ViewNote &note : m_sv->model().notes) {
            if (note.track == m_sv->selectedTrack() && note.noteId == m_hovered)
                return &note;
        }
        return nullptr;
    }

    void setHovered(const ViewNote *note)
    {
        const NoteId id = note ? note->noteId : NoteId();
        if (id == m_hovered)
            return;
        m_hovered = id;
        invalidateContent();
    }

    // The map a gesture froze for a note, so its edits keep answering to the
    // voice the press found even if the document moves underneath.
    std::optional<VelocityMap> frozenMap(NoteId noteId) const
    {
        for (const Frozen &frozen : m_frozen) {
            if (frozen.note.noteId == noteId)
                return frozen.map;
        }
        return std::nullopt;
    }

    // The note a press at pos grabs, or null for empty space. A node's
    // circle outranks a bare stem, a selected note outranks an unselected
    // one (so a group drag survives a stacked node), then the nearest wins
    // and, tied, the later-painted one.
    const ViewNote *noteAt(const QPointF &pos) const
    {
        const qreal dpr = devicePixelRatioF();
        const VelocityAxis axis = plotAxis();
        const int track = m_sv->selectedTrack();
        const ViewNote *best = nullptr;
        bool bestCircle = false;
        bool bestSelected = false;
        double bestDistance = 0.0;
        for (const ViewNote &note : m_sv->model().notes) {
            if (note.track != track)
                continue;
            const double x = m_sv->displayX(double(note.startTick), kGutterW, dpr);
            const double y = yForNote(axis, note);
            const double dx = x - pos.x();
            const double dy = y - pos.y();
            const double distance = dx * dx + dy * dy;
            const bool circle = distance <= kVelNodeGrabRadius * kVelNodeGrabRadius;
            const double end = m_sv->displayX(double(note.endTick), kGutterW, dpr);
            const bool stem = pos.x() >= x - kVelStemGrabSlop &&
                              pos.x() <= end + kVelStemGrabSlop &&
                              std::abs(dy) <= kVelStemGrabRadius;
            if (!circle && !stem)
                continue;
            const bool selected = m_sv->isSelected(note);
            const bool better =
                !best || (circle && !bestCircle) ||
                (circle == bestCircle && selected && !bestSelected) ||
                (circle == bestCircle && selected == bestSelected && distance <= bestDistance);
            if (better) {
                best = &note;
                bestCircle = circle;
                bestSelected = selected;
                bestDistance = distance;
            }
        }
        return best;
    }

    // The velocity a note is drawn at: its preview while a gesture holds it,
    // its stored value otherwise.
    uint8_t displayVelocity(const ViewNote &note) const
    {
        if (const std::optional<uint8_t> preview = m_gestureModel.previewVelocity(note.noteId))
            return *preview;
        // No lane gesture: a roll velocity drag may still be holding it.
        if (const std::optional<uint8_t> preview = m_sv->rollVelocityPreview(note))
            return *preview;
        return note.velocity;
    }

    // Opens a deferred edit over the current selection. The frozen copies
    // are the notes as the press found them — the model rebuilds under an
    // edit, and a gesture measures from where it started, never from the
    // last preview.
    bool openSession()
    {
        SongDocument *doc = m_sv->document();
        m_frozen.clear();
        if (!doc)
            return false;
        std::vector<NoteVelocity> targets;
        for (const ViewNote &note : m_sv->model().notes) {
            if (note.track != m_sv->selectedTrack() || !m_sv->isSelected(note) ||
                !note.noteId.isAssigned())
                continue;
            // Each note's own voice comes along: a selection can straddle a
            // voice change, and a level move means different values either
            // side of it.
            m_frozen.push_back({note, contextForNote(note)});
            targets.push_back({note.noteId, int(note.velocity)});
        }
        if (!m_gestureModel.begin(doc->revision(), std::move(targets))) {
            m_frozen.clear();
            return false;
        }
        if (m_pressed)
            m_announced = m_pressed->noteId;
        return true;
    }

    // The paint stroke: only notes already selected are brushed, and only
    // where the segment from first to last crosses their tick. A note's new
    // velocity is the segment's own height there, so one sweep can shape a
    // whole phrase without touching anything outside the selection.
    void paintBetween(const QPointF &first, const QPointF &last)
    {
        const qreal dpr = devicePixelRatioF();
        const VelocityAxis axis = plotAxis();
        const double travel = last.x() - first.x();
        // Collected first, written after the session opens: the values a
        // note takes depend on the voice it plays on, so each note carries
        // its own map (the frozen one once a session holds it) rather than
        // a pointer into a model that an edit may rebuild.
        struct Brushed {
            NoteId noteId;
            VelocityMap map;
            double y = 0.0;
        };
        std::vector<Brushed> brushed;
        for (const ViewNote &note : m_sv->model().notes) {
            if (note.track != m_sv->selectedTrack() || !m_sv->isSelected(note) ||
                !note.noteId.isAssigned())
                continue;
            const double x = m_sv->displayX(double(note.startTick), kGutterW, dpr);
            double y = last.y();
            if (travel == 0.0) {
                // A stationary sample only reaches what it is standing on.
                if (std::abs(x - last.x()) > kVelNodeGrabRadius)
                    continue;
            } else {
                if (x < std::min(first.x(), last.x()) - kVelNodeGrabRadius ||
                    x > std::max(first.x(), last.x()) + kVelNodeGrabRadius)
                    continue;
                y = first.y() +
                    std::clamp((x - first.x()) / travel, 0.0, 1.0) * (last.y() - first.y());
            }
            brushed.push_back(
                {note.noteId, frozenMap(note.noteId).value_or(contextForNote(note)), y});
        }
        if (brushed.empty())
            return;
        // The session opens on the first note the stroke actually reaches:
        // until then the press is still a click, and a click commits nothing.
        if (m_frozen.empty() && !openSession())
            return;
        std::vector<NoteVelocity> updates;
        updates.reserve(brushed.size());
        for (const Brushed &brush : brushed) {
            // An intrinsic stroke reads the row it passes through, not the
            // value under it: between two detents there is nothing to write.
            const int proposed = intrinsic(axis) && !m_detentUnlock
                                     ? brush.map.representative(axis.yToLevel(brush.y))
                                     : axis.yToVelocity(brush.y);
            updates.push_back({brush.noteId, resolveVelocity(brush.map, proposed)});
        }
        m_announced = updates.front().noteId;
        m_previewed |= m_gestureModel.update(updates);
        announcePreview();
        auditionPreview();
        invalidatePreview();
    }

    // The ramp: a straight line from the press to the pointer, read as the
    // velocity for every selected note whose tick it spans. Notes outside it
    // keep the values the press froze.
    void updateRamp(const QPointF &pos)
    {
        // The guide line follows the pointer even when there is no selection
        // to lay values on, so it never freezes mid-drag.
        invalidateContent();
        if (m_frozen.empty())
            return;
        const qreal dpr = devicePixelRatioF();
        const VelocityAxis axis = plotAxis();
        const double first = std::min(m_pressPos.x(), pos.x()) - kVelNodeGrabRadius;
        const double last = std::max(m_pressPos.x(), pos.x()) + kVelNodeGrabRadius;
        std::vector<NoteVelocity> updates;
        updates.reserve(m_frozen.size());
        for (const Frozen &frozen : m_frozen) {
            const ViewNote &note = frozen.note;
            const double x = m_sv->displayX(double(note.startTick), kGutterW, dpr);
            int velocity = note.velocity;
            if (x >= first && x <= last) {
                const double span = pos.x() - m_pressPos.x();
                const double y =
                    span == 0.0
                        ? pos.y()
                        : m_pressPos.y() + std::clamp((x - m_pressPos.x()) / span, 0.0, 1.0) *
                                               (pos.y() - m_pressPos.y());
                velocity = intrinsic(axis) && !m_detentUnlock
                               ? frozen.map.representative(axis.yToLevel(y))
                               : resolveVelocity(frozen.map, axis.yToVelocity(y));
                m_announced = note.noteId;
            }
            updates.push_back({note.noteId, velocity});
        }
        m_previewed |= m_gestureModel.update(updates);
        announcePreview();
        auditionPreview();
        m_sv->velocityPreviewChanged();
    }

    // A click on a printed ruler value: the whole selection goes there at
    // once. Only the labels are targets — the bare graduations between them
    // have no value on screen to aim at — and the click is the whole
    // gesture, so it commits without waiting for the release.
    void rulerClick(const QPointF &pos)
    {
        const VelocityAxisGeometry geometry = axisGeometry();
        // Built from the selection, like the painted ruler: the markers are
        // printed values too, and they hide the fixed labels they cover.
        const std::vector<uint8_t> active = activeVelocities();
        const VelocityAxis axis(axisMap(currentContext()), geometry, active.data(), active.size());
        // The unlock chord turns an intrinsic ruler back into the continuous
        // scale it is drawn over, so a click can still ask for a value
        // between two detents. The continuous ruler keeps its printed-value
        // rule either way — there is nothing else on it to aim at.
        const int velocity = m_detentUnlock && intrinsic(axis)
                                 ? axis.yToVelocity(pos.y())
                                 : axis.rulerVelocityAt(pos.y(), geometry.labelHeight);
        if (velocity < 1)
            return;
        m_gesture = Gesture::Relative;
        m_activated = false;
        if (!openSession()) {
            m_gesture = Gesture::None;
            return;
        }
        std::vector<NoteVelocity> updates;
        updates.reserve(m_frozen.size());
        for (const Frozen &frozen : m_frozen) {
            // The ruler names one value; each note still answers to its own
            // voice, so a selection across a voice change lands on the level
            // that value means on each side.
            updates.push_back({frozen.note.noteId, resolveVelocity(frozen.map, velocity)});
        }
        m_gestureModel.update(updates);
        // The click is the whole edit, so it sounds here; the release that
        // follows stops it, like the press-and-drag gestures'.
        if (!m_frozen.empty())
            auditionNote(m_frozen.front().note.key, updates.front().velocity,
                         m_frozen.front().note.startTick);
        finishGesture(true);
    }

    void updateRelative(const QPointF &pos)
    {
        if (m_frozen.empty())
            return;
        const VelocityAxis axis = plotAxis();
        const bool levels = intrinsic(axis) && !m_detentUnlock;
        if (!m_activated) {
            // Measured on y alone: the gesture only moves velocities, so a
            // sideways wobble must stay a click rather than arming a drag
            // that would then commit nothing and select nothing. A pointer
            // that has already crossed into another level is past arguing
            // about, however short the travel: that is a whole step.
            const bool crossed = levels && axis.yToLevel(pos.y()) != axis.yToLevel(m_pressPos.y());
            if (!crossed && std::abs(pos.y() - m_pressPos.y()) < dragActivationDistance())
                return;
            m_activated = true;
        }
        if (levels) {
            // Whole levels, from each note's own starting value: a drag that
            // comes back to the level it started in restores the exact
            // velocity it found there instead of snapping to the detent.
            const int delta = axis.yToLevel(pos.y()) - axis.yToLevel(m_pressPos.y());
            std::vector<NoteVelocity> updates;
            updates.reserve(m_frozen.size());
            for (const Frozen &frozen : m_frozen) {
                updates.push_back(
                    {frozen.note.noteId, frozen.map.moveLevels(frozen.note.velocity, delta)});
            }
            m_previewed |= m_gestureModel.update(updates);
        } else {
            // The drag's own travel in velocity units, applied to every
            // frozen origin: a selection keeps its internal shape, and notes
            // that clamp at 1 or 127 come back when the pointer does. Unless
            // unlocked, each note still answers to its own voice, so a PSG
            // note in a mixed selection lands on a real detent.
            const int delta = axis.yToVelocity(pos.y()) - axis.yToVelocity(m_pressPos.y());
            std::vector<NoteVelocity> updates;
            updates.reserve(m_frozen.size());
            for (const Frozen &frozen : m_frozen) {
                updates.push_back({frozen.note.noteId,
                                   resolveVelocity(frozen.map, int(frozen.note.velocity) + delta)});
            }
            m_previewed |= m_gestureModel.update(updates);
        }
        announcePreview();
        auditionPreview();
        invalidatePreview();
    }

    // The marquee: the nodes the band covers, previewed as selected while it
    // is swept so the release holds no surprises. Nodes only — a stem is
    // where a note is, not what its velocity is, and the band is a statement
    // about velocities.
    void updateBand(const QPointF &pos)
    {
        const qreal dpr = devicePixelRatioF();
        const VelocityAxis axis = plotAxis();
        m_bandRect = QRectF(m_pressPos, pos).normalized();
        // A band swept along one axis is flat, and an empty rectangle
        // intersects nothing at all in Qt — yet sweeping a row of nodes on
        // an intrinsic ruler, where a run of them shares one y exactly, is
        // the obvious gesture. Give the hit test a hair of thickness.
        const QRectF reach = m_bandRect.adjusted(-0.5, -0.5, 0.5, 0.5);
        m_bandPreview.clear();
        for (const ViewNote &note : m_sv->model().notes) {
            if (note.track != m_sv->selectedTrack())
                continue;
            const QPointF center(m_sv->displayX(double(note.startTick), kGutterW, dpr),
                                 yForNote(axis, note));
            if (reach.intersects(QRectF(center.x() - kVelNodeGrabRadius,
                                        center.y() - kVelNodeGrabRadius, 2 * kVelNodeGrabRadius,
                                        2 * kVelNodeGrabRadius)))
                m_bandPreview.push_back({note.startTick, note.key});
        }
        invalidateContent();
    }

    // Whether a note reads as selected on screen: the selection, plus what a
    // band being swept right now is about to add to it.
    bool showsSelected(const ViewNote &note) const
    {
        if (m_sv->isSelected(note))
            return true;
        const SongView::NoteKey id{note.startTick, note.key};
        return std::find(m_bandPreview.begin(), m_bandPreview.end(), id) != m_bandPreview.end();
    }

    // Release inside the activation slop: the press was a click.
    void clickSelect()
    {
        if (!m_pressed) {
            m_sv->setSelection({});
            return;
        }
        const SongView::NoteKey id{m_pressed->startTick, m_pressed->key};
        if (!m_ctrlPress) {
            m_sv->setSelection({id});
            return;
        }
        std::vector<SongView::NoteKey> selection = m_selBeforePress;
        const auto existing = std::find(selection.begin(), selection.end(), id);
        if (existing == selection.end())
            selection.push_back(id);
        else
            selection.erase(existing);
        m_sv->setSelection(std::move(selection));
    }

    // Ends the gesture. The document mutates exactly here, once, and only
    // when the preview is still describing the revision it started from —
    // setNotesVelocities refuses anything else rather than landing values on
    // notes that moved.
    void finishGesture(bool commit)
    {
        if (m_gesture == Gesture::None)
            return;
        const Gesture kind = m_gesture;
        m_gesture = Gesture::None;
        m_activated = false;
        m_previewed = false;
        m_pressed.reset();
        m_selBeforePress.clear();
        m_frozen.clear();
        m_announced = NoteId();
        m_detentUnlock = false;
        m_hovered = NoteId();
        m_bandPreview.clear();
        m_bandRect = QRectF();
        const std::optional<VelocityGestureModel::Completion> completion =
            m_gestureModel.takeCompletion();
        if (!completion || !commit) {
            invalidatePreview();
            return;
        }
        SongDocument *doc = m_sv->document();
        const std::optional<uint64_t> revision =
            doc ? doc->setNotesVelocities(completion->expectedRevision, completion->targets)
                : std::nullopt;
        if (!revision) {
            // Only the document refusing the batch means the notes moved; a
            // song closed mid-gesture simply has nothing to write to.
            if (doc)
                m_sv->announce(SongView::tr("Velocity edit cancelled because notes changed."));
        } else if (*revision != completion->expectedRevision) {
            m_sv->announce(kind == Gesture::Paint  ? SongView::tr("Painted note velocities.")
                           : kind == Gesture::Ramp ? SongView::tr("Ramped note velocities.")
                                                   : SongView::tr("Set note velocities."));
        }
        invalidatePreview();
    }

    // Abandons the gesture: the preview never reaches the document, and the
    // selection the press took over goes back the way it was (minus any note
    // that stopped existing meanwhile).
    void cancelGesture()
    {
        if (m_gesture == Gesture::None)
            return;
        std::vector<SongView::NoteKey> restore;
        for (const SongView::NoteKey &id : m_selBeforePress) {
            for (const ViewNote &note : m_sv->model().notes) {
                if (note.track == m_sv->selectedTrack() && note.startTick == id.tick &&
                    note.key == id.key) {
                    restore.push_back(id);
                    break;
                }
            }
        }
        m_gesture = Gesture::None;
        m_activated = false;
        m_previewed = false;
        m_pressed.reset();
        m_selBeforePress.clear();
        m_frozen.clear();
        m_announced = NoteId();
        m_detentUnlock = false;
        m_hovered = NoteId();
        m_bandPreview.clear();
        m_bandRect = QRectF();
        m_gestureModel.cancel();
        stopAudition();
        m_sv->setSelection(std::move(restore));
        invalidatePreview();
    }

    // Status line for the note the gesture is aimed at, in the roll's own
    // wording: key, the stored velocity the release will write, what the
    // engine will actually play, and the note's length.
    void announcePreview() const
    {
        for (const Frozen &frozen : m_frozen) {
            const ViewNote &note = frozen.note;
            if (note.noteId != m_announced)
                continue;
            ViewNote shown = note;
            shown.velocity = displayVelocity(note);
            m_sv->announceNote(shown);
            return;
        }
    }

    // A preview moved: the lane redraws its nodes, and so does the roll —
    // its notes are drawn at the lane's preview while a gesture holds them.
    void invalidatePreview()
    {
        invalidateContent();
        m_sv->velocityPreviewChanged();
    }

    // Sounds the note the gesture is aimed at, at the velocity the release
    // would write. The roll's velocity-drag rule exactly: re-audition only
    // when the value the engine will actually play moves to the next mid2agb
    // step, since nothing about the sound changes in between.
    void auditionPreview()
    {
        for (const Frozen &frozen : m_frozen) {
            if (frozen.note.noteId != m_announced)
                continue;
            auditionNote(frozen.note.key, displayVelocity(frozen.note), frozen.note.startTick);
            return;
        }
    }

    // atTick is the auditioned note's start: the preview sounds at the track
    // volume in force where the note lives, not at the edit cursor's.
    void auditionNote(int key, int velocity, uint64_t atTick)
    {
        const int effective = mid2agbEffectiveVelocity(velocity);
        // The VOL and PAN in force at atTick are part of what the preview
        // sounds like, so they belong in the dedupe: two notes at the same
        // pitch and step under different CC7 or CC10 values are different
        // sounds.
        const int vol = m_sv->auditionVolume(m_sv->selectedTrack(), atTick);
        const int pan = m_sv->auditionPan(m_sv->selectedTrack(), atTick);
        if (key == m_audKey && effective == m_audEff && vol == m_audVol && pan == m_audPan)
            return;
        m_audKey = key;
        m_audEff = effective;
        m_audVol = vol;
        m_audPan = pan;
        m_sv->audition(m_sv->selectedTrack(), key, velocity, atTick);
    }

    // Releases whatever the gesture is sounding. The engine's preview slot
    // holds one note, so the velocity-0 form is all it takes.
    void stopAudition()
    {
        if (m_audKey < 0)
            return;
        m_audKey = -1;
        m_audEff = -1;
        m_audVol = -1;
        m_audPan = M4A_AUDITION_PAN_NONE;
        m_sv->audition(m_sv->selectedTrack(), 0, 0);
    }

    VelocityAxisGeometry axisGeometry() const
    {
        VelocityAxisGeometry geometry;
        geometry.height = height();
        // Half a node plus its ring, so the 1 and 127 nodes still paint
        // whole inside the lane.
        geometry.verticalInset = std::ceil(kVelSelRingRadius) + lyt::singlePixel();
        geometry.labelHeight = QFontMetrics(typography::caption(font())).height();
        geometry.densityD1 = lyt::fontPx(kVelDensityD1);
        geometry.densityD2 = lyt::fontPx(kVelDensityD2);
        geometry.densityD3 = lyt::fontPx(kVelDensityD3);
        geometry.densityD4 = lyt::fontPx(kVelDensityD4);
        return geometry;
    }

    // Whether unselected nodes recede: true once the selection is something a
    // bulk edit would act on, counted over the whole track (see paintNodes).
    bool dimUnselectedNodes() const
    {
        int selected = 0;
        for (const ViewNote &note : m_sv->model().notes) {
            if (note.track == m_sv->selectedTrack() && showsSelected(note))
                selected++;
        }
        return selected > 1;
    }

    // The velocities the ruler describes: the hovered note's on its own —
    // the pointer is asking about that note — else the selected notes', and
    // nothing at all with neither.
    std::vector<uint8_t> activeVelocities() const
    {
        if (const ViewNote *hovered = hoveredNote())
            return {displayVelocity(*hovered)};
        std::vector<uint8_t> values;
        for (const ViewNote &note : m_sv->model().notes) {
            if (note.track == m_sv->selectedTrack() && m_sv->isSelected(note))
                values.push_back(displayVelocity(note));
        }
        return values;
    }

    static bool intrinsic(const VelocityAxis &axis)
    {
        return axis.mode() == VelocityAxis::Mode::Intrinsic;
    }

    // The PSG levels themselves, drawn per section across the plot: one line
    // along each level's own row, which is where that level's nodes sit and
    // where a drag lets go — so a line is a rail a node can be read against
    // rather than a fence between two of them. They are the same rows the
    // ruler graduates, and a voice or volume change mid-song moves them (or
    // takes them away entirely).
    void paintLevelLines(QPainter &p, const QRect &plot, const VelocityAxis &axis, qreal dpr)
    {
        if (!m_useDetents)
            return;
        const double firstTick = std::max(0.0, m_sv->tickAtContentX(plot.left() - kGutterW));
        const double lastTick = m_sv->tickAtContentX(plot.right() - kGutterW);
        if (lastTick <= firstTick)
            return;
        p.setPen(
            QPen(themes::color(themes::Role::song_view_psg_velocity_levels), lyt::singlePixel()));
        uint64_t sectionTick = uint64_t(firstTick);
        while (double(sectionTick) < lastTick) {
            const SongView::VoiceContext context = m_sv->voiceContext(sectionTick);
            const uint64_t sectionEnd = std::min(uint64_t(std::ceil(lastTick)), context.endTick);
            if (sectionEnd <= sectionTick)
                break;
            VelocityMap map = VelocityMap::resolve(context.voice, std::nullopt, context.trackVolume,
                                                   context.trackPan);
            // A keysplit answers per key, so its section has no single level
            // table to draw — but its notes resolved with their own keys are
            // what put the ruler on levels, so the ruler's map is the one
            // they snapped to and the one the lines belong to.
            if (map.isKeyless() && intrinsic(axis))
                map = axis.map();
            if (map.hasDetents()) {
                const double left = std::max<double>(
                    plot.left(), m_sv->displayX(double(sectionTick), kGutterW, dpr));
                const double right = std::min<double>(
                    plot.right(), m_sv->displayX(double(sectionEnd), kGutterW, dpr));
                for (std::size_t level = 0; level < map.levelCount(); level++) {
                    const double y = levelCenterY(axis, map, int(level));
                    p.drawLine(QPointF(left, y), QPointF(right, y));
                }
            }
            sectionTick = sectionEnd;
        }
    }

    // The header column's Detents checkbox: offered only where there are
    // detents to switch off. Empty when the voice has none, or when the lane
    // is too short to hold both the checkbox and its own label — the label is
    // the one that stays. Sized from the style's own hint, so a longer
    // translation and a bigger indicator both still fit; clamped to the
    // header column rather than hidden, so it never silently disappears.
    QRect detentCheckRect(const VelocityMap &context) const
    {
        if (!context.hasDetents())
            return {};
        const int inset = lyt::space(Space::Two);
        const QSize hint = m_detentCheck->sizeHint();
        const QRect box(inset, height() - hint.height() - inset,
                        std::min(hint.width(), kHeaderW - 2 * inset), hint.height());
        if (box.top() < QFontMetrics(font()).height())
            return {};
        return box;
    }

    // Called from the paint, where the context is already resolved: the
    // widget follows the same recomputation as everything else the lane
    // draws. Nothing here repaints synchronously, so it is safe under a live
    // painter — geometry and visibility only post updates.
    void syncDetentCheck(const VelocityMap &context)
    {
        const QRect box = detentCheckRect(context);
        if (box.isEmpty()) {
            m_detentCheck->hide();
            return;
        }
        m_detentCheck->setGeometry(box);
        // Programmatic: following m_useDetents (the rearm off a non-PSG
        // voice, above) must not announce, nor re-enter invalidateContent
        // from inside a paint.
        {
            const QSignalBlocker block(m_detentCheck);
            m_detentCheck->setChecked(m_useDetents);
        }
        m_detentCheck->show();
    }

    void paintNodes(QPainter &p, const QRect &plot, const VelocityAxis &axis, qreal dpr)
    {
        const int track = m_sv->selectedTrack();
        std::vector<const ViewNote *> visible;
        for (const ViewNote &note : m_sv->model().notes) {
            if (note.track != track)
                continue;
            const qreal x = m_sv->displayX(double(note.startTick), kGutterW, dpr);
            const qreal end = m_sv->displayX(double(note.endTick), kGutterW, dpr);
            if (end < plot.left() - kVelSelRingRadius || x > plot.right() + kVelSelRingRadius)
                continue;
            visible.push_back(&note);
        }
        // Counted over the whole track, never the visible slice: scrolling
        // must not change how the nodes on screen are colored.
        const bool dimUnselected = dimUnselectedNodes();
        const QColor trackColor = SongView::trackColor(track);
        // Stems stay on the track's ink even under velocity colors: darkening
        // the ramp by a third takes its low end to 1.05:1 on a dark theme,
        // and the stem is the note's identity and duration, not its value.
        const QColor stemColor = mixTowardOklab(trackColor, Qt::black, 1.0 / 3.0);
        const QColor selectedColor = palette().highlight().color();
        const QColor nodeColor = dimUnselected ? palette().mid().color() : trackColor;
        // View → Color Notes by Velocity, the same ramp the roll fills notes
        // with, so a node and its note read as the same thing. Dimming still
        // wins: an unselected node that has receded keeps the recessive ink,
        // or the ramp would talk over the selection the lane is about to edit.
        const bool velocityInk = m_sv->velocityColorMode();
        const QColor background = themes::color(themes::Role::song_view_piano_roll_background);
        const auto nodeFill = [&](const ViewNote &note, bool selectedPass) {
            if (dimUnselected && !selectedPass)
                return nodeColor;
            if (velocityInk)
                return SongView::velocityNoteColor(displayVelocity(note));
            return trackColor;
        };
        // A DIP weight can still fall under one device pixel on a downscaled
        // display; never let a stem or a ring vanish.
        const qreal physicalPixel = logicalPhysicalPixel(dpr);
        const auto weight = [physicalPixel](double dips) { return std::max(dips, physicalPixel); };

        // Stems first, so a node always sits on top of its neighbor's line.
        for (int pass = 0; pass < 2; pass++) {
            const bool selectedPass = pass == 1;
            p.setPen(QPen(selectedPass ? selectedColor : stemColor,
                          weight(selectedPass ? kVelSelStemWidth : kVelStemWidth), Qt::SolidLine,
                          Qt::FlatCap));
            for (const ViewNote *note : visible) {
                if (showsSelected(*note) != selectedPass)
                    continue;
                const qreal y = yForNote(axis, *note);
                const qreal x = m_sv->displayX(double(note->startTick), kGutterW, dpr);
                const qreal end = std::max(x + physicalPixel,
                                           m_sv->displayX(double(note->endTick), kGutterW, dpr));
                p.drawLine(QPointF(x, y), QPointF(end, y));
            }
        }
        p.setRenderHint(QPainter::Antialiasing, true);
        for (int pass = 0; pass < 2; pass++) {
            const bool selectedPass = pass == 1;
            for (const ViewNote *note : visible) {
                if (showsSelected(*note) != selectedPass)
                    continue;
                const QPointF center(m_sv->displayX(double(note->startTick), kGutterW, dpr),
                                     yForNote(axis, *note));
                if (selectedPass) {
                    p.setPen(QPen(selectedColor, weight(kVelSelRingWidth)));
                    p.setBrush(Qt::NoBrush);
                    p.drawEllipse(center, kVelSelRingRadius, kVelSelRingRadius);
                }
                const QColor fill = nodeFill(*note, selectedPass);
                p.setPen(dimUnselected && !selectedPass
                             ? QPen(Qt::NoPen)
                             : QPen(velocityInk ? velNodeOutlineInk(fill, background)
                                                : themes::color(themes::Role::song_view_grid),
                                    weight(kVelNodeOutline)));
                p.setBrush(fill);
                p.drawEllipse(center, kVelNodeRadius, kVelNodeRadius);
            }
        }
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setBrush(Qt::NoBrush);
    }

    SongView *m_sv;
    bool m_panning = false;
    QPointF m_panPos;
    // The deferred edit: identities and preview values only, so nothing the
    // pointer does reaches the document before the release.
    VelocityGestureModel m_gestureModel;
    Gesture m_gesture = Gesture::None;
    // A gesture's target as the press found it: the note, and the voice its
    // velocities answer to.
    struct Frozen {
        ViewNote note;
        VelocityMap map;
    };
    std::vector<Frozen> m_frozen;
    std::optional<ViewNote> m_pressed;  // the note under the press, if any
    NoteId m_announced;                 // the frozen note the status line describes
    NoteId m_hovered;                   // the node under the idle pointer, if any
    QCheckBox *m_detentCheck = nullptr; // the header column's Detents checkbox
    bool m_useDetents = true;           // that checkbox; rearms off a PSG context
    bool m_detentUnlock = false;        // this gesture writes exact values (press-time)
    std::vector<SongView::NoteKey> m_selBeforePress; // restored by a cancel
    std::vector<SongView::NoteKey> m_bandPreview;    // what a live marquee would select
    QRectF m_bandRect;
    QPointF m_pressPos;
    QPointF m_prevPos;
    bool m_activated = false;             // the drag cleared the activation slop
    bool m_previewed = false;             // an update actually moved a preview value
    bool m_ctrlPress = false;             // Ctrl at the press; the click adds instead of collapsing
    int m_audKey = -1;                    // key the gesture is sounding; -1 = silent
    int m_audEff = -1;                    // effective velocity it was last sounded at
    int m_audVol = -1;                    // VOL byte it was last sounded under (-1 = track's)
    int m_audPan = M4A_AUDITION_PAN_NONE; // PAN it was last sounded under
};

// -------------------------------------------------------------- LaneToggleBar

// Glyphs for the lane toggles, drawn rather than shipped: main links no SVG
// module, and a painted mask tints from the theme for free (the transport
// icons take the same route through tintedStandardIcon). Each draws white on
// transparent at `extent` square; the caller recolors with SourceIn.
QPixmap laneToggleMask(int extent, qreal dpr, bool automation)
{
    QPixmap mask(QSize(extent, extent) * dpr);
    mask.setDevicePixelRatio(dpr);
    mask.fill(Qt::transparent);
    QPainter p(&mask);
    p.setRenderHint(QPainter::Antialiasing, true);
    if (automation) {
        // automation.svg traced in its own 512 viewBox: two hollow nodes on a
        // step — a run into the low node, a rise, and a run off the right
        // edge. Outer radius 80 with a 48 hole is a ring of stroke 32 at
        // radius 64, and the connectors are bands of about the same width.
        p.scale(extent / 512.0, extent / 512.0);
        const qreal stroke = 32.0;
        const qreal radius = 64.0;
        const QPointF low(88.0, 368.0);
        const QPointF high(280.0, 144.0);
        p.setPen(QPen(Qt::white, stroke, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
        p.drawPolyline(
            QPolygonF({low, QPointF(high.x(), low.y()), high, QPointF(504.0, high.y())}));
        // The connectors run under both nodes, so clear each node's disc
        // before stroking its ring — otherwise the hole fills in and the
        // glyph reads as two blobs.
        for (const QPointF &node : {low, high}) {
            p.setCompositionMode(QPainter::CompositionMode_Clear);
            p.setPen(Qt::NoPen);
            p.setBrush(Qt::white);
            p.drawEllipse(node, radius + stroke / 2.0, radius + stroke / 2.0);
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
            p.setPen(QPen(Qt::white, stroke));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(node, radius, radius);
        }
    } else {
        // velocity.svg traced in its own 360 viewBox: a filled handle sitting
        // at the left end of a rail that runs off the right edge, the way a
        // velocity value rides its lane.
        p.scale(extent / 360.0, extent / 360.0);
        const QPointF handle(79.8, 180.0);
        const qreal radius = 79.8;
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::white);
        p.drawRect(QRectF(handle.x(), 167.5, 360.0 - handle.x(), 25.0));
        p.drawEllipse(handle, radius, radius);
    }
    return mask;
}

// Two checkable toggles for the automation lanes and the velocity lane. It
// rides in the "other events" strip's gutter, leading the strip's row, and is
// never hidden, so the toggles stay where the user left them even with both
// panes shut. Sized and placed by its host (OtherStrip::paintContent) —
// TimelineSurface::resizeEvent is final, so a surface's children cannot be
// laid out from a resize override.
class LaneToggleBar : public QWidget
{
  public:
    explicit LaneToggleBar(SongView *sv, QWidget *parent) : QWidget(parent)
    {
        setObjectName(QStringLiteral("laneToggleBar")); // findChild for tests
        const int extent = lyt::fontPx(1.5);
        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(lyt::space(Space::Two), lyt::space(Space::Half),
                                lyt::space(Space::Two), lyt::space(Space::Half));
        row->setSpacing(lyt::space(Space::One));
        const auto makeToggle = [&](const QString &objectName) {
            auto *button = new QToolButton(this);
            button->setObjectName(objectName);
            button->setAutoRaise(false);
            button->setCheckable(true);
            button->setFixedSize(extent, extent);
            // Tab reaches them, a click does not take focus: the roll and
            // lanes own the bare-letter shortcuts, and moving focus onto the
            // bar would quietly stop A/V/M/S/B from working.
            button->setFocusPolicy(Qt::TabFocus);
            row->addWidget(button);
            return button;
        };
        m_automation = makeToggle(QStringLiteral("automationLanesToggle"));
        m_velocity = makeToggle(QStringLiteral("velocityLaneToggle"));
        row->addStretch();
        refreshIcons();
        m_automation->setChecked(sv->automationLanesVisible());
        m_velocity->setChecked(sv->velocityLaneVisible());
        connect(m_automation, &QToolButton::toggled, sv,
                [sv](bool on) { sv->setAutomationLanesVisible(on); });
        connect(m_velocity, &QToolButton::toggled, sv,
                [sv](bool on) { sv->setVelocityLaneVisible(on); });
        // The View menu and the keyboard flip the same panes; follow them.
        // Re-entry through toggled is harmless — the setters no-op when the
        // pane already matches.
        connect(sv, &SongView::automationLanesVisibilityChanged, this,
                [this](bool on) { m_automation->setChecked(on); });
        connect(sv, &SongView::velocityLaneVisibilityChanged, this,
                [this](bool on) { m_velocity->setChecked(on); });
        // Display-only binding hints like the track headers': live, so the
        // shortcuts dialog can rebind without rebuilding the bar.
        const auto retip = [this] {
            const auto &keys = keymap::Registry::instance();
            const auto hint = [&keys](const QString &id, const QString &name) {
                const QKeySequence seq = keys.bindings(id).value(0);
                return seq.isEmpty() ? name
                                     : QStringLiteral("%1 (%2)").arg(
                                           name, seq.toString(QKeySequence::NativeText));
            };
            m_automation->setToolTip(hint(QStringLiteral("view.automation_lanes"),
                                          SongView::tr("Show or hide the automation lanes")));
            m_velocity->setToolTip(hint(QStringLiteral("view.velocity_lane"),
                                        SongView::tr("Show or hide the velocity lane")));
        };
        retip();
        connect(&keymap::Registry::instance(), &keymap::Registry::bindingsChanged, this, retip);
        m_automation->setAccessibleName(SongView::tr("Automation lanes"));
        m_velocity->setAccessibleName(SongView::tr("Velocity lane"));
    }

  protected:
    // No paintEvent: the strip underneath paints the chrome background and the
    // row's top rule, and the buttons draw their own fills over it.

    void changeEvent(QEvent *event) override
    {
        QWidget::changeEvent(event);
        switch (event->type()) {
        case QEvent::ApplicationPaletteChange:
        case QEvent::PaletteChange:
        case QEvent::StyleChange:
        case QEvent::ThemeChange:
            refreshIcons();
            break;
        default:
            break;
        }
    }

  private:
    // Tinted the same way as the transport glyphs: the hover (Active) and
    // checked (On) variants follow the button ramp's paired text colors, so
    // the glyph stays readable on whatever fill the theme sheet paints.
    void refreshIcons()
    {
        const int extent = std::max(1, int(m_automation->height() * 0.8));
        const qreal dpr = devicePixelRatioF();
        for (bool automation : {true, false}) {
            const QPixmap mask = laneToggleMask(extent, dpr, automation);
            const auto tinted = [&mask](themes::Role role) {
                QPixmap pixmap = mask;
                QPainter painter(&pixmap);
                painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
                painter.fillRect(pixmap.rect(), themes::color(role));
                painter.end();
                return pixmap;
            };
            QIcon icon(tinted(themes::Role::button_text));
            icon.addPixmap(tinted(themes::Role::button_hover_text), QIcon::Active, QIcon::Off);
            icon.addPixmap(tinted(themes::Role::button_pressed_text), QIcon::Normal, QIcon::On);
            icon.addPixmap(tinted(themes::Role::button_pressed_text), QIcon::Active, QIcon::On);
            QToolButton *button = automation ? m_automation : m_velocity;
            button->setIcon(icon);
            button->setIconSize(QSize(extent, extent));
        }
    }

    QToolButton *m_automation = nullptr;
    QToolButton *m_velocity = nullptr;
};

// ---------------------------------------------------------------- OtherStrip

class OtherStrip : public TimelineSurface
{
  public:
    explicit OtherStrip(SongView *sv) : TimelineSurface(sv), m_sv(sv)
    {
        setObjectName(QStringLiteral("otherEventsStrip")); // findChild for tests
        // The strip's gutter carries the lane toggles, so the row has to clear
        // the buttons as well as its own label.
        m_toggles = new LaneToggleBar(sv, this);
        setFixedHeight(std::max(QFontMetrics(font()).height() + lyt::space(Space::Two),
                                m_toggles->sizeHint().height()));
        setMouseTracking(true);
    }

    LaneToggleBar *laneToggles() const { return m_toggles; }

  protected:
    void paintContent(QPainter &p) override
    {
        const qreal dpr = p.device()->devicePixelRatioF();
        p.fillRect(rect(), themes::color(themes::Role::song_view_timeline_chrome_background));
        p.setPen(themes::color(themes::Role::song_view_separator));
        p.drawLine(0, 0, width(), 0);

        // The toggles lead the row; laying them out here rather than from a
        // resize override is forced (TimelineSurface::resizeEvent is final)
        // and safe — setGeometry only posts an update for the child.
        m_toggles->setGeometry(0, 0, m_toggles->sizeHint().width(), height());

        const SongViewModel &model = m_sv->model();
        p.setPen(themes::color(themes::Role::song_view_primary_text));
        const auto textInset = lyt::space(Space::Two);
        // The label closes the gutter from the right, so the buttons and the
        // text read as one row instead of crowding the same corner.
        const int labelLeft = m_toggles->geometry().right() + textInset;
        p.drawText(QRect(labelLeft, 0, kGutterW - textInset - labelLeft, height()),
                   Qt::AlignVCenter | Qt::AlignRight,
                   SongView::tr("Other events (%1)").arg(model.strip.size()));
        if (!m_sv->timeline())
            return;

        const QRect area(kGutterW, 0, width() - kGutterW, height());
        p.setClipRect(area, Qt::IntersectClip);
        drawPreRoll(p, m_sv, area, kGutterW,
                    themes::color(themes::Role::song_view_timeline_chrome_background));
        drawOverlays(p, m_sv, area, kGutterW, false);

        const int cy = height() / 2;
        for (const StripItem &item : model.strip) {
            const qreal x = m_sv->displayX(double(item.tick), kGutterW, dpr);
            if (x < area.left() - 4 || x > area.right() + 4)
                continue;
            QColor c = item.track >= 0 ? SongView::trackColor(item.track)
                                       : themes::color(themes::Role::song_view_file_event_marker);
            QPainterPath diamond;
            diamond.moveTo(x, cy - 5);
            diamond.lineTo(x + 4, cy);
            diamond.lineTo(x, cy + 5);
            diamond.lineTo(x - 4, cy);
            diamond.closeSubpath();
            p.fillPath(diamond, c);
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const MidiTimeline *tl = m_sv->timeline();
        if (!tl || event->position().x() < kGutterW) {
            QToolTip::hideText();
            return;
        }
        QStringList lines;
        for (const StripItem &item : m_sv->model().strip) {
            const qreal x = m_sv->displayX(double(item.tick), kGutterW, devicePixelRatioF());
            if (std::abs(x - event->position().x()) > 4)
                continue;
            const double seconds = double(tl->sampleForTick(item.tick)) / tl->sampleRate;
            QString where = item.track >= 0 ? SongView::tr("Track %1").arg(item.track + 1)
                                            : SongView::tr("File");
            lines << QStringLiteral("%1:%2 · %3 · %4")
                         .arg(int(seconds) / 60)
                         .arg(int(seconds) % 60, 2, 10, QLatin1Char('0'))
                         .arg(where, item.label);
            if (lines.size() >= 12) {
                lines << SongView::tr("…");
                break;
            }
        }
        if (lines.isEmpty())
            QToolTip::hideText();
        else
            QToolTip::showText(event->globalPosition().toPoint(), lines.join(QStringLiteral("\n")),
                               this);
    }

  private:
    SongView *m_sv;
    LaneToggleBar *m_toggles = nullptr;
};

// ---------------------------------------------------------- VoicePickerDialog

// Modal instrument picker (SPEC §4.2): the voicegroup's 128 entries, the same
// list the import wizard's mapping combo renders. Press-and-hold auditions
// through the preview engine; double-click chooses.
class VoicePickerDialog : public QDialog
{
  public:
    VoicePickerDialog(SongView *sv, const QString &title, int initialVoice,
                      std::function<void(int, int)> audition)
        : QDialog(sv)
        , m_audition(std::move(audition))
    {
        setWindowTitle(title);
        resize(lyt::fontPx(30), lyt::fontPx(110.0 / 3.0));
        auto *dialogLayout = new QVBoxLayout(this);
        auto *searchField = new QLineEdit(this);
        searchField->setPlaceholderText(tr("Search voices..."));
        searchField->setClearButtonEnabled(true);
        dialogLayout->addWidget(searchField);
        m_list = new QListWidget(this);
        m_list->setUniformItemSizes(true);
        m_list->setToolTip(SongView::tr("Click and hold to audition (middle C)."));
        for (int v = 0; v < VOICEGROUP_SIZE; v++)
            m_list->addItem(QStringLiteral("%1  %2")
                                .arg(v, 3, 10, QLatin1Char('0'))
                                .arg(sv->voiceShortName(uint8_t(v))));
        m_list->setCurrentRow(std::clamp(initialVoice, 0, VOICEGROUP_SIZE - 1));
        m_list->scrollToItem(m_list->currentItem(), QAbstractItemView::PositionAtCenter);
        dialogLayout->addWidget(m_list, 1);

        auto *dialogButtons =
            new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        connect(dialogButtons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(dialogButtons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        dialogLayout->addWidget(dialogButtons);
        connect(searchField, &QLineEdit::textChanged, this,
                [this, dialogButtons](const QString &query) {
                    QListWidgetItem *firstMatchingVoice = nullptr;
                    for (int voiceIndex = 0; voiceIndex < m_list->count(); ++voiceIndex) {
                        QListWidgetItem *voiceItem = m_list->item(voiceIndex);
                        const bool matchesQuery =
                            voiceItem->text().contains(query, Qt::CaseInsensitive);
                        voiceItem->setHidden(!matchesQuery);
                        if (matchesQuery && !firstMatchingVoice)
                            firstMatchingVoice = voiceItem;
                    }
                    m_list->setCurrentItem(firstMatchingVoice);
                    dialogButtons->button(QDialogButtonBox::Ok)->setEnabled(firstMatchingVoice);
                });
        searchField->setFocus();

        connect(m_list, &QListWidget::itemPressed, this, [this](QListWidgetItem *item) {
            releaseVoice();
            if (item) {
                m_sounding = m_list->row(item);
                m_audition(m_sounding, kVoiceAuditionVel);
            }
        });
        connect(m_list, &QListWidget::itemDoubleClicked, this, [this] { accept(); });
        m_list->viewport()->installEventFilter(this);
    }

    ~VoicePickerDialog() override { releaseVoice(); }

    int selectedVoice() const { return std::max(0, m_list->currentRow()); }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_list->viewport() && event->type() == QEvent::MouseButtonRelease)
            releaseVoice();
        return QDialog::eventFilter(watched, event);
    }

  private:
    void releaseVoice()
    {
        if (m_sounding < 0)
            return;
        m_audition(m_sounding, 0);
        m_sounding = -1;
    }

    QListWidget *m_list;
    std::function<void(int, int)> m_audition;
    int m_sounding = -1;
};

// ---------------------------------------------------------- TrackHeaderPanel

class TrackHeaderRow : public QWidget
{
  public:
    TrackHeaderRow(SongView *sv, int track, QWidget *parent)
        : QWidget(parent)
        , m_sv(sv)
        , m_track(track)
    {
        const auto buttonExtent = ::layout::fontPx(1.5);
        setFixedHeight(::layout::fontPx(4.0));
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(::layout::space(::layout::Space::Zero),
                                   ::layout::space(::layout::Space::Zero),
                                   ::layout::space(::layout::Space::One), ::layout::singlePixel());
        layout->addStretch();

        auto *buttons = new QVBoxLayout;
        buttons->setSpacing(::layout::space(::layout::Space::Zero));
        m_mute = new QToolButton(this);
        m_mute->setAutoRaise(false);
        m_mute->setText(QStringLiteral("M"));
        m_mute->setCheckable(true);
        m_mute->setFixedSize(buttonExtent, buttonExtent);
        m_mute->setObjectName(QStringLiteral("trackMuteButton"));
        // Headers are rebuilt on every document edit; keep the persistent
        // mute/solo state (checked before connect, so nothing re-emits).
        m_mute->setChecked(sv->trackMuted(track));
        connect(m_mute, &QToolButton::toggled, this,
                [this](bool on) { m_sv->setTrackMute(m_track, on); });
        m_solo = new QToolButton(this);
        m_solo->setAutoRaise(false);
        m_solo->setText(QStringLiteral("S"));
        m_solo->setCheckable(true);
        m_solo->setFixedSize(buttonExtent, buttonExtent);
        m_solo->setObjectName(QStringLiteral("trackSoloButton"));
        m_solo->setChecked(sv->trackSoloed(track));
        connect(m_solo, &QToolButton::toggled, this,
                [this](bool on) { m_sv->setTrackSolo(m_track, on); });
        // The keyboard toggles change the masks without a header rebuild;
        // follow them. Re-entry through toggled is safe: setTrackMute/Solo
        // no-op when the bit already matches.
        connect(sv, &SongView::muteMaskChanged, this,
                [this](uint32_t mask) { m_mute->setChecked(mask & (1u << m_track)); });
        connect(sv, &SongView::soloMaskChanged, this,
                [this](uint32_t mask) { m_solo->setChecked(mask & (1u << m_track)); });
        // Display-only binding hints, like the context menus'. Live: the
        // shortcuts dialog can rebind without a header rebuild.
        const auto retip = [this] {
            const auto &keys = keymap::Registry::instance();
            const auto hint = [&keys](const QString &id, const QString &name) {
                const QKeySequence seq = keys.bindings(id).value(0);
                return seq.isEmpty() ? name
                                     : QStringLiteral("%1 (%2)").arg(
                                           name, seq.toString(QKeySequence::NativeText));
            };
            m_mute->setToolTip(hint(QStringLiteral("roll.mute_tracks"), SongView::tr("Mute")));
            m_solo->setToolTip(hint(QStringLiteral("roll.solo_tracks"), SongView::tr("Solo")));
        };
        retip();
        connect(&keymap::Registry::instance(), &keymap::Registry::bindingsChanged, this, retip);
        buttons->addStretch();
        buttons->addWidget(m_mute);
        buttons->addStretch();
        buttons->addWidget(m_solo);
        buttons->addStretch();
        layout->addLayout(buttons);
        layout->setAlignment(buttons, Qt::AlignVCenter);
    }

    int track() const { return m_track; }

    // True when the song's music player never starts this track in-game
    // (track index at or beyond SongDocument::trackBudget).
    bool isSilentInGame() const
    {
        const SongDocument *doc = m_sv->document();
        return doc && m_track >= doc->trackBudget();
    }

  protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const bool selected = m_sv->selectedTrack() == m_track;
        if (selected) {
            // The derived selection fill has the required lightness gap. Keep
            // it opaque so the visible header reaches that target.
            p.fillRect(rect(), themes::color(themes::Role::song_view_track_header_selection));
        } else if (m_sv->trackSelectionMask() & (1u << m_track)) {
            // Part of the multi-track scope (Ctrl/Shift+click), lighter than
            // the primary selection.
            p.fillRect(rect(), trackHeaderAlsoSelectedColor());
        }
        p.fillRect(QRect(0, 0, lyt::space(Space::One), height()), SongView::trackColor(m_track));
        p.setPen(QPen(themes::color(themes::Role::song_view_separator), lyt::singlePixel()));
        p.drawLine(0, height() - lyt::singlePixel(), width(), height() - lyt::singlePixel());

        const MidiTimeline *tl = m_sv->timeline();
        QString name = tl ? tl->tracks[m_track].name : QString();
        if (name.isEmpty())
            name = SongView::tr("Track %1").arg(m_track + 1);
        const auto textInset = lyt::space(Space::Two);
        const auto textW = width() - lyt::fontPx(2) - textInset;
        const auto title = QStringLiteral("%1 · %2").arg(m_track + 1).arg(name);
        const auto normalTitleFont = p.font();
        const auto titleFont = selected ? typography::bold(normalTitleFont) : normalTitleFont;
        const auto titleMetrics = QFontMetrics(titleFont);
        const auto visibleTitle = titleMetrics.elidedText(title, Qt::ElideRight, textW);
        // The song's music player never starts this track in-game
        // (MPlayStart), so playback mutes it; the header must read as inert
        // at a glance: text recedes most of the way into the backdrop and a
        // faint cross spans the row, under the text so labels stay legible.
        const bool silentInGame = isSilentInGame();
        const QColor backdrop =
            selected ? themes::color(themes::Role::song_view_track_header_selection)
            : (m_sv->trackSelectionMask() & (1u << m_track)) ? trackHeaderAlsoSelectedColor()
                                                             : palette().color(QPalette::Window);
        QColor titleColor = selected
                                ? themes::color(themes::Role::song_view_track_header_selection_text)
                                : themes::color(themes::Role::song_view_primary_text);
        QColor subtitleColor =
            selected ? themes::color(themes::Role::song_view_track_header_selection_text)
                     : themes::color(themes::Role::song_view_secondary_text);
        if (silentInGame) {
            titleColor = mixTowardOklab(titleColor, backdrop, selected ? 0.35 : 0.6);
            subtitleColor = mixTowardOklab(subtitleColor, backdrop, selected ? 0.35 : 0.6);
            QColor cross = mixTowardOklab(titleColor, backdrop, 0.3);
            p.save();
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(QPen(cross, lyt::singlePixel()));
            const QRectF box = QRectF(rect()).adjusted(
                lyt::space(Space::One), lyt::space(Space::One), -lyt::space(Space::One),
                -lyt::space(Space::One) - lyt::singlePixel());
            p.drawLine(box.topLeft(), box.bottomRight());
            p.drawLine(box.bottomLeft(), box.topRight());
            p.restore();
        }
        p.setFont(titleFont);
        p.setPen(titleColor);
        const auto subtitleFont = typography::caption(normalTitleFont);
        const auto subtitleMetrics = QFontMetrics(subtitleFont);
        const auto textLayout =
            ::layout::twoLineText(normalTitleFont, typography::bold(normalTitleFont), subtitleFont,
                                  ::layout::Space::Half);
        // The bottom pixel belongs to the separator, not the row's content.
        const auto textBounds = QRect(10, 0, textW, height() - lyt::singlePixel());
        const auto textBoxes = textLayout.align(textBounds, ::layout::VerticalAlignment::Center);
        // Bold and regular glyph bounds differ. Translate the selected title
        // so changing weight does not make the visible text jump.
        const auto titleBox = QRectF(textBoxes.primary)
                                  .translated(typography::glyphCenteringOffset(
                                      normalTitleFont, titleFont, visibleTitle));
        p.drawText(titleBox, Qt::AlignLeft | Qt::AlignVCenter, visibleTitle);

        p.setFont(subtitleFont);
        p.setPen(subtitleColor);
        m_shownProgram = m_sv->currentProgram(m_track);
        const QString subtitle =
            silentInGame ? SongView::tr("silent in-game · %1").arg(m_sv->instrumentLabel(m_track))
                         : m_sv->instrumentLabel(m_track);
        p.drawText(textBoxes.secondary, Qt::AlignLeft | Qt::AlignVCenter,
                   subtitleMetrics.elidedText(subtitle, Qt::ElideRight, textW));
    }

    // The painted voice line (paintEvent's instrument-label rect): a plain
    // click here also reveals the voice in the voicegroup dock.
    QRect voiceLineRect() const { return QRect(10, 22, width() - 36, 16); }

    void mousePressEvent(QMouseEvent *event) override
    {
        m_sv->trackHeaderClicked(m_track, event->modifiers());
        // A plain left press may become a reorder drag (the track's chunk
        // moves — AGB track order is chunk order).
        m_dragArmed = event->button() == Qt::LeftButton &&
                      !(event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) &&
                      m_sv->document();
        m_voiceClickArmed = event->button() == Qt::LeftButton &&
                            !(event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) &&
                            voiceLineRect().contains(event->pos());
        m_pressPos = event->pos();
    }

    // Defined below TrackHeaderPanel (they drive its drag state).
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

  public:
    // Inline rename: a line edit overlaid on the row's name line. Return
    // commits, Escape cancels (both restore the roll's focus), focus-out
    // commits Reaper-style. The document edit itself is queued by
    // commitTrackRename — it rebuilds the header panel, which would delete
    // this row and the editor mid-signal.
    // The voice line follows the song's program changes as the playhead (or
    // edit cursor) moves; repaint only when the shown program flips.
    void syncVoice()
    {
        if (m_shownProgram == m_sv->currentProgram(m_track))
            return;
        update();
        updateToolTip();
    }

    void updateToolTip()
    {
        const MidiTimeline *tl = m_sv->timeline();
        if (!tl)
            return;
        QString tip = SongView::tr("%1 notes · %2")
                          .arg(tl->tracks[m_track].noteCount)
                          .arg(m_sv->instrumentLabel(m_track));
        if (isSilentInGame()) {
            tip += SongView::tr("\nSilent in-game: this song's music player only allocates "
                                "%1 track(s) (sound/music_player_table.inc), and the game "
                                "never starts the tracks beyond them. porydaw plays it the "
                                "same way. Raise the player's track count in the project to "
                                "use this track.")
                       .arg(m_sv->document()->trackBudget());
        }
        if (m_sv->document()) {
            tip += SongView::tr("\nDouble-click to rename · right-click "
                                "to change voice, duplicate, or delete"
                                " · drag to reorder"
                                "\nClick the voice name to show it in the "
                                "voicegroup dock · double-click it to "
                                "change the voice");
        }
        setToolTip(tip);
    }

    void beginRename()
    {
        SongDocument *doc = m_sv->document();
        if (!doc)
            return;
        if (!m_editor) {
            m_editor = new QLineEdit(this);
            m_editor->setObjectName(QStringLiteral("trackRenameEditor"));
            m_editor->installEventFilter(this);
            connect(m_editor, &QLineEdit::editingFinished, this,
                    [this] { finishRename(true, false); });
        }
        m_editor->setText(doc->trackName(m_track));
        // What an empty name falls back to (mirrors the painted default).
        m_editor->setPlaceholderText(SongView::tr("Track %1").arg(m_track + 1));
        m_editor->setGeometry(editorRect());
        m_editor->show();
        m_editor->setFocus();
        m_editor->selectAll();
    }

    // Reaper-style commit for gestures that will rebuild the panel: header
    // rows take no focus, so pressing one never gives the editor a
    // focus-out — without this, the rebuild would destroy the editor and
    // silently drop the typed name.
    void commitOpenRename() { finishRename(true, false); }

  protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        m_sv->selectTrack(m_track);
        // The voice line opens the voice picker (its single click already
        // revealed the voice in the dock); anywhere else renames. Queued:
        // the picked voice's edit rebuilds the header panel, deleting this
        // row out from under its own event handler.
        if (voiceLineRect().contains(event->pos())) {
            QMetaObject::invokeMethod(
                m_sv, [sv = m_sv, t = m_track] { sv->editTrackVoice(t); }, Qt::QueuedConnection);
            return;
        }
        beginRename();
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        if (!m_sv->document())
            return;
        // A right-click with the left button still down is a mid-drag
        // cancel (mouseReleaseEvent), not a menu request.
        if (QApplication::mouseButtons() & Qt::LeftButton)
            return;
        m_sv->selectTrack(m_track);
        QMenu menu(this);
        QAction *voiceAction = menu.addAction(SongView::tr("Change voice..."));
        QAction *showVoiceAction = menu.addAction(SongView::tr("Show voice in voicegroup"));
        QAction *renameAction = menu.addAction(SongView::tr("Rename track..."));
        QAction *duplicateAction = menu.addAction(SongView::tr("Duplicate track"));
        duplicateAction->setEnabled(m_sv->document()->canAddTrack());
        QAction *deleteAction = menu.addAction(SongView::tr("Delete track"));
        QAction *chosen = menu.exec(event->globalPos());
        // Queued: these edits rebuild the header panel, which deletes this
        // row out from under its own event handler. (Rename just opens the
        // inline editor — no edit until it commits — so it's direct.)
        if (chosen == renameAction) {
            beginRename();
        } else if (chosen == showVoiceAction) {
            // No document edit — nothing rebuilds, so no queue needed.
            m_sv->revealTrackVoice(m_track);
        } else if (chosen == voiceAction) {
            QMetaObject::invokeMethod(
                m_sv, [sv = m_sv, t = m_track] { sv->editTrackVoice(t); }, Qt::QueuedConnection);
        } else if (chosen == duplicateAction) {
            QMetaObject::invokeMethod(
                m_sv, [sv = m_sv, t = m_track] { sv->duplicateTrack(t); }, Qt::QueuedConnection);
        } else if (chosen == deleteAction) {
            QMetaObject::invokeMethod(
                m_sv, [sv = m_sv, t = m_track] { sv->deleteTrack(t); }, Qt::QueuedConnection);
        }
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_editor && event->type() == QEvent::KeyPress) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            if (keyEvent->key() == Qt::Key_Escape) {
                finishRename(false, true);
                return true;
            }
            if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
                finishRename(true, true);
                return true;
            }
        }
        return QWidget::eventFilter(watched, event);
    }

    void resizeEvent(QResizeEvent *) override
    {
        // Rows are born 100px wide and only get their real width on the
        // deferred layout pass; an open editor must follow.
        if (m_editor)
            m_editor->setGeometry(editorRect());
    }

  private:
    // The row's name line, clear of the color strip and the M/S column.
    QRect editorRect() const { return QRect(6, 2, width() - 32, 20); }

    void finishRename(bool commit, bool restoreFocus)
    {
        // isHidden, not isVisible: the guard must also hold when the view
        // itself isn't shown (offscreen harnesses). m_finishing blocks the
        // editingFinished that hide()'s focus-out re-emits.
        if (!m_editor || m_editor->isHidden() || m_finishing)
            return;
        m_finishing = true;
        const QString text = m_editor->text();
        m_editor->hide();
        m_finishing = false;
        if (restoreFocus)
            m_sv->focusContent();
        if (commit)
            m_sv->commitTrackRename(m_track, text);
    }

    SongView *m_sv;
    int m_track;
    QToolButton *m_mute;
    QToolButton *m_solo;
    QLineEdit *m_editor = nullptr;
    bool m_finishing = false;
    // Program painted on the voice line, for syncVoice's changed check
    // (-2 = never painted; distinct from -1, "no voice set").
    int m_shownProgram = -2;
    QPoint m_pressPos;
    bool m_dragArmed = false;
    bool m_dragging = false;
    bool m_voiceClickArmed = false;
};

class TrackHeaderPanel : public QWidget
{
  public:
    explicit TrackHeaderPanel(SongView *sv) : QWidget(nullptr), m_sv(sv)
    {
        setObjectName(QStringLiteral("trackHeaderPanel"));
        setAttribute(Qt::WA_StyledBackground);
        m_layout = new QVBoxLayout(this);
        m_layout->setContentsMargins(0, 0, 0, 0);
        m_layout->setSpacing(0);
        m_layout->addStretch();
        // Reorder-drag drop indicator: a thin line floating over the rows at
        // the insertion point.
        m_indicator = new QWidget(this);
        m_indicator->setFixedHeight(3);
        m_indicator->setStyleSheet(QStringLiteral("background: palette(highlight);"));
        m_indicator->hide();
    }

    void rebuild()
    {
        // A document edit mid-drag rebuilds the rows, deleting the dragged
        // one out from under its own gesture; abandon the drag first.
        endRowDrag(false);
        // Deferred deletion: a rebuild can arrive from inside a row's own
        // mouse press (clicking a header focuses the roll, which fires an
        // editor field's editingFinished; a structural voice commit then
        // swaps the voicegroup into every view). Freeing the rows here
        // would leave that row's event handler running on freed memory.
        // Keep them parented (their mouse handlers cast parentWidget())
        // but hidden and anonymous until the event loop collects them.
        for (QWidget *row : m_rows) {
            row->hide();
            // Anonymous, children included: name lookups (the rename
            // editor, harness hooks) must only ever see the live rows.
            row->setObjectName(QString());
            for (QWidget *child : row->findChildren<QWidget *>())
                child->setObjectName(QString());
            m_layout->removeWidget(row);
            row->deleteLater();
        }
        m_rows.clear();
        m_rowByTrack.clear();
        m_trackRows.clear();
        const MidiTimeline *tl = m_sv->timeline();
        if (tl) {
            for (int t = 0; t < 16; t++) {
                if (!tl->tracks[t].used)
                    continue;
                auto *row = new TrackHeaderRow(m_sv, t, this);
                row->setObjectName(QStringLiteral("trackHeaderRow%1").arg(t));
                m_rowByTrack[t] = row;
                row->updateToolTip();
                m_layout->insertWidget(m_layout->count() - 1, row);
                m_rows.push_back(row);
                m_trackRows.push_back(row);
            }
            SongDocument *doc = m_sv->document();
            if (doc && doc->canAddTrack()) {
                auto *add = new QPushButton(SongView::tr("+ Add track"), this);
                add->setFocusPolicy(Qt::NoFocus);
                add->setToolTip(SongView::tr("Add a track (picks its voice first)"));
                // Queued: the edit rebuilds this panel, deleting the button
                // out from under its own clicked handler.
                connect(
                    add, &QPushButton::clicked, m_sv, [sv = m_sv] { sv->addTrack(); },
                    Qt::QueuedConnection);
                m_layout->insertWidget(m_layout->count() - 1, add);
                m_rows.push_back(add);
            }
        }
    }

    void syncSelection()
    {
        for (QWidget *row : m_rows)
            row->update();
    }

    void beginRename(int track)
    {
        const auto it = m_rowByTrack.find(track);
        if (it != m_rowByTrack.end())
            it->second->beginRename();
    }

    // Called on every playhead/cursor move; each row repaints only when its
    // shown program actually changes.
    void syncVoices()
    {
        for (const auto &entry : m_rowByTrack)
            entry.second->syncVoice();
    }

    // --- header-row reorder drag (driven by TrackHeaderRow's mouse events;
    // the panel owns the state so a mid-drag rebuild can abandon it) ---

    bool beginRowDrag(int track)
    {
        if (m_dragFrom >= 0 || m_trackRows.size() < 2)
            return false;
        m_dragFrom = track;
        m_dropSlot = -1;
        QApplication::setOverrideCursor(Qt::ClosedHandCursor);
        return true;
    }

    void dragRowTo(QPoint pos)
    {
        if (m_dragFrom < 0)
            return;
        // Insertion slot: before the first row whose center is below the
        // cursor; past the last row otherwise.
        int slot = 0;
        for (const TrackHeaderRow *row : m_trackRows) {
            if (pos.y() > row->y() + row->height() / 2)
                slot++;
        }
        m_dropSlot = slot;
        const int y = slot < int(m_trackRows.size())
                          ? m_trackRows[size_t(slot)]->y()
                          : m_trackRows.back()->y() + m_trackRows.back()->height();
        m_indicator->setGeometry(0, y - 1, width(), 3);
        m_indicator->raise();
        m_indicator->show();
    }

    void endRowDrag(bool commit)
    {
        if (m_dragFrom < 0)
            return;
        const int from = m_dragFrom;
        const int slot = m_dropSlot;
        m_dragFrom = -1;
        m_dropSlot = -1;
        m_indicator->hide();
        QApplication::restoreOverrideCursor();
        if (!commit || slot < 0)
            return;
        int fromIdx = -1;
        for (size_t i = 0; i < m_trackRows.size(); i++) {
            if (m_trackRows[i]->track() == from)
                fromIdx = int(i);
        }
        // The slots adjacent to the dragged row leave it where it was.
        if (fromIdx < 0 || slot == fromIdx || slot == fromIdx + 1)
            return;
        const int target = m_trackRows[size_t(slot > fromIdx ? slot - 1 : slot)]->track();
        // The move's rebuild would destroy an open rename editor without a
        // focus-out (rows take no focus): commit it Reaper-style first. Its
        // queued commit runs before the queued move below, and renameTrack
        // renumbers nothing, so both captured track numbers stay valid.
        for (const auto &entry : m_rowByTrack)
            entry.second->commitOpenRename();
        // Queued: the edit rebuilds this panel, deleting the dragged row out
        // from under its own mouse-release handler.
        QMetaObject::invokeMethod(
            m_sv, [sv = m_sv, from, target] { sv->moveTrack(from, target); }, Qt::QueuedConnection);
    }

  private:
    SongView *m_sv;
    QVBoxLayout *m_layout;
    std::vector<QWidget *> m_rows;
    std::map<int, TrackHeaderRow *> m_rowByTrack;
    std::vector<TrackHeaderRow *> m_trackRows;
    QWidget *m_indicator = nullptr;
    int m_dragFrom = -1; // dragged engine track; -1 = no drag live
    int m_dropSlot = -1; // insertion slot the indicator marks
};

// The drag handlers live below TrackHeaderPanel because they drive it.

void TrackHeaderRow::mouseMoveEvent(QMouseEvent *event)
{
    auto *panel = static_cast<TrackHeaderPanel *>(parentWidget());
    if (!m_dragging) {
        if (!m_dragArmed || !(event->buttons() & Qt::LeftButton) ||
            (event->pos() - m_pressPos).manhattanLength() < QApplication::startDragDistance())
            return;
        m_dragging = panel->beginRowDrag(m_track);
        if (!m_dragging)
            return;
    }
    panel->dragRowTo(mapTo(panel, event->pos()));
}

void TrackHeaderRow::mouseReleaseEvent(QMouseEvent *event)
{
    // Only a left release drops the row; any other button mid-drag is a
    // cancel gesture (matching the ruler's and roll's release handling).
    if (event->button() != Qt::LeftButton) {
        if (m_dragging) {
            m_dragging = false;
            m_dragArmed = false;
            static_cast<TrackHeaderPanel *>(parentWidget())->endRowDrag(false);
        }
        return;
    }
    m_dragArmed = false;
    const bool voiceClick = m_voiceClickArmed;
    m_voiceClickArmed = false;
    if (!m_dragging) {
        // A completed plain click on the voice line (not a drag, released
        // where it pressed) surfaces the track's voice in the dock.
        if (voiceClick && voiceLineRect().contains(event->pos()))
            m_sv->revealTrackVoice(m_track);
        return;
    }
    m_dragging = false;
    static_cast<TrackHeaderPanel *>(parentWidget())->endRowDrag(true);
}

} // namespace songview

// ------------------------------------------------------------------ SongView

using namespace songview;

songview::TimelineSurfaces SongView::timelineSurfaces() noexcept
{
    return {
        {*m_ruler, kGutterW}, {*m_roll, kKeyboardW}, {*m_velocityLane, kGutterW},
        {*m_lanes, kGutterW}, {*m_strip, kGutterW},
    };
}

SongView::SongView(QWidget *parent) : QWidget(parent)
{
    auto *vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    m_ruler = new TimeRuler(this);
    vbox->addWidget(m_ruler);

    // Roll (with headers) above, automation lanes below, split by a
    // draggable boundary; kLanesAreaH is only the initial lanes height.
    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->setChildrenCollapsible(false);
    auto *rollPane = new QWidget(m_splitter);
    auto *mid = new QHBoxLayout(rollPane);
    mid->setContentsMargins(0, 0, 0, 0);
    mid->setSpacing(0);
    auto *headerScroll = new QScrollArea(this);
    headerScroll->setFixedWidth(kHeaderW);
    headerScroll->setFrameShape(QFrame::NoFrame);
    headerScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    headerScroll->setWidgetResizable(true);
    // The roll owns keyboard editing (delete, copy/paste); the scroll areas
    // must not steal its focus on click (QAbstractScrollArea defaults to
    // StrongFocus, which broke shortcuts right after a track switch).
    headerScroll->setFocusPolicy(Qt::NoFocus);
    m_headers = new TrackHeaderPanel(this);
    headerScroll->setWidget(m_headers);
    mid->addWidget(headerScroll);
    // The note grid and raw event list share the roll's slot. Headers, ruler,
    // and automation lanes remain visible on either page.
    m_rollStack = new QStackedWidget(this);
    auto *rollPage = new QWidget(m_rollStack);
    auto *rollBox = new QHBoxLayout(rollPage);
    rollBox->setContentsMargins(0, 0, 0, 0);
    rollBox->setSpacing(0);
    m_roll = new PianoRoll(this);
    rollBox->addWidget(m_roll, 1);
    m_vbar = new QScrollBar(Qt::Vertical, this);
    ::layout::configureListPositionIndicator(*m_vbar);
    m_vbar->setSingleStep(kScrollUnitsPerDip);
    rollBox->addWidget(m_vbar);
    m_rollStack->addWidget(rollPage);
    m_events = new EventListView(this);
    m_rollStack->addWidget(m_events);
    mid->addWidget(m_rollStack, 1);
    m_splitter->addWidget(rollPane);

    // The velocity lane sits directly under the roll — it addresses the same
    // notes — and above the automation lanes, which keep their own pane.
    // Hidden until the View toggle asks for it.
    m_velocityLane = new VelocityLane(this);
    m_velocityLane->hide();
    m_splitter->addWidget(m_velocityLane);

    m_lanesScroll = new QScrollArea(this);
    m_lanesScroll->setObjectName(QStringLiteral("automationLanesPane")); // findChild for tests
    m_lanesScroll->setMinimumHeight(kLaneH + kAddLaneH);
    m_lanesScroll->setFrameShape(QFrame::NoFrame);
    m_lanesScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_lanesScroll->setWidgetResizable(true);
    m_lanesScroll->setFocusPolicy(Qt::NoFocus);
    m_lanes = new AutomationArea(this, m_lanesScroll);
    m_lanesScroll->setWidget(m_lanes);
    m_splitter->addWidget(m_lanesScroll);
    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 0);
    m_splitter->setStretchFactor(2, 0);
    vbox->addWidget(m_splitter, 1);

    // The view's last row, outside the splitter, and the host of the pane
    // toggles: they hold still whether or not either pane is showing.
    m_strip = new OtherStrip(this);
    vbox->addWidget(m_strip);
    m_laneToggles = m_strip->laneToggles();
    const auto surfaces = timelineSurfaces();
    themes::registerGridLineRefreshTarget(surfaces.ruler.widget);
    themes::registerGridLineRefreshTarget(surfaces.roll.widget);
    themes::registerGridLineRefreshTarget(surfaces.lanes.widget);
    themes::registerGridLineRefreshTarget(*m_velocityLane);
    m_playheadOverlay = new PlayheadOverlay(this, surfaces);

    m_hbar = new QScrollBar(Qt::Horizontal, this);
    m_hbar->setSingleStep(kScrollUnitsPerDip);
    auto *hbarRow = new QHBoxLayout;
    hbarRow->addSpacing(kGutterW);
    hbarRow->addWidget(m_hbar);
    vbox->addLayout(hbarRow);

    connect(m_hbar, &QScrollBar::valueChanged, this, [this](int v) { setHScroll(scrollDips(v)); });
    connect(m_vbar, &QScrollBar::valueChanged, this, [this](int v) { setVScroll(scrollDips(v)); });

    // Mouse presses go to the widget under the pointer, never to this view,
    // so catching the ones on focus-less children takes an application
    // filter (refocusAfterDeadClick). Qt drops the filter with the view.
    qApp->installEventFilter(this);
}

bool SongView::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress)
        refocusAfterDeadClick(qobject_cast<QWidget *>(watched));
    return QWidget::eventFilter(watched, event);
}

void SongView::refocusAfterDeadClick(QWidget *clicked)
{
    if (!clicked || !isAncestorOf(clicked))
        return;
    // Qt has already walked this chain (before the filter saw the press) and
    // focused the nearest click-focusable widget on it; if there is one,
    // the press was on an input and the focus is right where it belongs.
    for (QWidget *w = clicked; w && w != this; w = w->parentWidget()) {
        if (w->isEnabled() && (w->focusPolicy() & Qt::ClickFocus))
            return;
    }
    if (QWidget *const focused = window() ? window()->focusWidget() : nullptr) {
        if (focused == m_roll || focused == m_lanes || focused == m_velocityLane ||
            focused == m_events || m_events->isAncestorOf(focused))
            return;
    }
    focusContent();
}

void SongView::setSong(const MidiTimeline *timeline, const LoadedVoiceGroup *voicegroup)
{
    m_timeline = timeline;
    m_voicegroup = voicegroup;
    m_model = timeline ? buildSongViewModel(*timeline) : SongViewModel();
    m_emptyLanes.clear();
    m_selection.clear();
    m_timeSel = TimeSelection();
    // The clipboard is deliberately NOT cleared: it is app-shared and
    // self-contained, so a copy made in another song (or this one, before
    // the swap) pastes here — clipForPaste rescales its ticks.
    m_muteMask = 0;
    m_soloMask = 0;
    emit muteMaskChanged(0);
    emit soloMaskChanged(0);
    m_playheadTick = 0.0;
    m_editCursorTick = 0;
    m_playing = false;
    // Fresh songs open at the camera's home position, pre-roll pad showing.
    m_scrollX = minHScroll();
    m_events->setPlayheadTick(-1.0, false); // another song's ticks are stale
    // Lane heights/value ranges/hidden lanes and the snap grid are per-song
    // view state; back to defaults until a sidecar (applyViewState) says
    // otherwise.
    m_lanes->setViewHeights(0, {});
    m_lanes->setRowRanges({});
    m_lanes->setHiddenLanes({});
    m_gridFeel = GridFeel::Straight;
    m_gridMinDenom = 0;
    m_ruler->syncGridControls();

    m_selectedTrack = 0;
    if (timeline) {
        for (int t = 0; t < 16; t++) {
            if (timeline->tracks[t].used) {
                m_selectedTrack = t;
                break;
            }
        }
    }
    m_trackSelMask = 1u << m_selectedTrack;

    rebuildAfterSongChange();
}

void SongView::rebuildAfterSongChange()
{
    double initialScrollY = 0.0;
    if (m_timeline) {
        // Default zoom: 32 px per beat, scrolled so the notes' pitch range
        // is centered in the roll.
        m_pxPerTick = 32.0 / double(m_timeline->ticksPerBeat);
        const int midKey = m_model.minNoteKey <= m_model.maxNoteKey
                               ? (m_model.minNoteKey + m_model.maxNoteKey) / 2
                               : 60;
        initialScrollY =
            std::max(0.0, (127 - midKey) * m_keyHeight - std::max(200, m_roll->height()) / 2.0);
    } else {
        m_pxPerTick = 1.0;
    }
    m_headers->rebuild();
    m_lanes->rebuildRows();
    updateScrollbars();
    setVScroll(initialScrollY);
    refreshTimelineViews();
}

void SongView::updateSong(const MidiTimeline *timeline)
{
    m_timeline = timeline;
    m_model = timeline ? buildSongViewModel(*timeline) : SongViewModel();
    mergeEmptyLanes();

    if (timeline && !timeline->tracks[m_selectedTrack].used) {
        // The edited track disappeared (e.g. undo of its only events).
        m_selectedTrack = 0;
        for (int t = 0; t < 16; t++) {
            if (timeline->tracks[t].used) {
                m_selectedTrack = t;
                break;
            }
        }
    }

    // Keep only selection ids that still resolve to a note.
    std::vector<NoteKey> keep;
    for (const NoteKey &id : m_selection) {
        for (const ViewNote &note : m_model.notes) {
            if (note.track == m_selectedTrack && note.startTick == id.tick && note.key == id.key) {
                keep.push_back(id);
                break;
            }
        }
    }
    m_selection = std::move(keep);

    m_headers->rebuild();
    m_lanes->rebuildRows();
    // A rebuilt model retires the notes a live velocity preview was holding
    // (the lane's own commit has already ended its gesture by this point).
    m_velocityLane->documentChanged();
    updateScrollbars();
    refreshTimelineViews();
}

void SongView::setDocument(SongDocument *document)
{
    if (m_document != document) {
        if (m_document)
            disconnect(m_document, &SongDocument::trackMoved, this, nullptr);
        if (document)
            connect(document, &SongDocument::trackMoved, this, &SongView::onTrackMoved);
    }
    m_document = document;
    m_selection.clear();
    m_headers->rebuild();   // the "+ Add track" button follows editability
    m_lanes->rebuildRows(); // the "+ Add lane" strip follows editability
    m_events->setDocument(document);
}

bool SongView::eventListVisible() const
{
    return m_rollStack->currentIndex() == 1;
}

void SongView::setEventListVisible(bool visible)
{
    if (eventListVisible() == visible)
        return;
    m_rollStack->setCurrentIndex(visible ? 1 : 0);
    if (visible) {
        // The list skips refreshes while hidden; catch up when shown.
        m_events->refresh();
        m_events->syncTrackSelection();
    }
    focusContent();
    emit eventListVisibilityChanged(visible);
}

bool SongView::velocityLaneVisible() const
{
    return m_velocityLane && !m_velocityLane->isHidden();
}

void SongView::setVelocityLaneVisible(bool visible)
{
    if (!m_velocityLane || velocityLaneVisible() == visible)
        return;
    // Every timeline surface is ClickFocus, so Qt has nowhere to send the
    // focus a hidden lane was holding — and the next V (or M/S/B) would land
    // outside the view entirely. Hand it back to the editing surface.
    const QWidget *const focused = window() ? window()->focusWidget() : nullptr;
    const bool laneHadFocus = focused == m_velocityLane;
    m_velocityLane->setVisible(visible);
    if (!visible && laneHadFocus)
        focusContent();
    if (visible) {
        // The splitter hands a freshly shown pane only its minimum (and a
        // sidecar that predates the lane restores it at zero), so open it at
        // the default height, borrowed from the roll pane above it.
        QList<int> sizes = m_splitter->sizes();
        const int borrow = kVelLaneH - sizes[1];
        if (borrow > 0 && sizes[0] - borrow >= kVelLaneMinH) {
            sizes[0] -= borrow;
            sizes[1] += borrow;
            m_splitter->setSizes(sizes);
        }
    }
    emit velocityLaneVisibilityChanged(visible);
}

bool SongView::automationLanesVisible() const
{
    return m_lanesScroll && !m_lanesScroll->isHidden();
}

void SongView::setAutomationLanesVisible(bool visible)
{
    if (!m_lanesScroll || automationLanesVisible() == visible)
        return;
    // The lanes area is ClickFocus like every other surface; hiding it while
    // it holds the focus would strand the bare-letter shortcuts outside the
    // view. Hand the focus back to the editing surface first.
    const QWidget *const focused = window() ? window()->focusWidget() : nullptr;
    const bool lanesHadFocus = focused == m_lanes;
    m_lanesScroll->setVisible(visible);
    if (!visible && lanesHadFocus)
        focusContent();
    if (visible) {
        // A freshly shown splitter pane gets only its minimum, so reopen it
        // at the classic lanes height, borrowed from the roll pane above.
        QList<int> sizes = m_splitter->sizes();
        const int borrow = kLanesAreaH - sizes[2];
        if (borrow > 0 && sizes[0] - borrow >= kVelLaneMinH) {
            sizes[0] -= borrow;
            sizes[2] += borrow;
            m_splitter->setSizes(sizes);
        }
    }
    emit automationLanesVisibilityChanged(visible);
}

bool SongView::tempoLaneVisible() const
{
    return m_lanes && m_lanes->tempoLaneVisible();
}

void SongView::setTempoLaneVisible(bool visible)
{
    if (!m_lanes)
        return;
    // A row shown into a closed pane is shown nowhere: open the pane too —
    // before the no-change return, because the row flag can already be set
    // with the pane closed (closing the pane leaves the flag alone, and a
    // sidecar restore sets the row directly), and showing the row must mean
    // showing it.
    if (visible)
        setAutomationLanesVisible(true);
    if (tempoLaneVisible() == visible)
        return;
    m_lanes->setTempoLaneVisible(visible);
}

void SongView::focusContent()
{
    if (eventListVisible())
        m_events->setFocus();
    else
        m_roll->setFocus();
}

void SongView::addEmptyLane(int track, uint8_t cc)
{
    if (track < 0 || track > 15 || !m_timeline)
        return;
    const std::pair<int, uint8_t> key(track, cc);
    if (std::find(m_emptyLanes.begin(), m_emptyLanes.end(), key) == m_emptyLanes.end())
        m_emptyLanes.push_back(key);
    mergeEmptyLanes();
    m_lanes->rebuildRows();
}

void SongView::removeEmptyLane(int track, uint8_t cc)
{
    m_emptyLanes.erase(
        std::remove(m_emptyLanes.begin(), m_emptyLanes.end(), std::pair<int, uint8_t>(track, cc)),
        m_emptyLanes.end());
    for (auto it = m_model.lanes.begin(); it != m_model.lanes.end(); ++it) {
        if (it->track == track && it->cc == cc && it->points.empty()) {
            m_model.lanes.erase(it);
            break;
        }
    }
    m_lanes->rebuildRows();
}

void SongView::setLaneDisplayRange(int track, uint8_t cc, int maxValue)
{
    m_lanes->setLaneRange(track, cc, maxValue);
}

void SongView::mergeEmptyLanes()
{
    bool added = false;
    for (const std::pair<int, uint8_t> &key : m_emptyLanes) {
        if (m_model.findLane(key.first, key.second))
            continue;
        AutoLane lane;
        lane.track = uint8_t(key.first);
        lane.cc = key.second;
        lane.lane =
            key.second == LANE_CC_BEND ? M4aLane::PitchBend : m4aClassifyCc(key.second).lane;
        lane.name = laneDisplayName(key.second);
        m_model.lanes.push_back(std::move(lane));
        added = true;
    }
    if (added) {
        // Same order buildSongViewModel establishes.
        std::stable_sort(m_model.lanes.begin(), m_model.lanes.end(),
                         [](const AutoLane &a, const AutoLane &b) {
                             if (a.track != b.track)
                                 return a.track < b.track;
                             return a.cc < b.cc;
                         });
    }
}

SongView::ViewState SongView::viewState() const
{
    ViewState state;
    if (!m_timeline)
        return state;
    state.valid = true;
    state.pxPerBeat = m_pxPerTick * double(m_timeline->ticksPerBeat);
    state.keyHeight = m_keyHeight;
    state.scrollPx = m_scrollX;
    state.scrollY = m_scrollY;
    state.selectedTrack = m_selectedTrack;
    state.editCursorTick = m_editCursorTick;
    state.laneHeight = m_lanes->laneHeight();
    state.laneHeights = m_lanes->rowHeightOverrides();
    state.laneRanges = m_lanes->rowRangeOverrides();
    state.hiddenLanes = m_lanes->hiddenLaneKeys();
    state.tempoLane = m_lanes->tempoLaneVisible();
    state.splitterSizes = m_splitter->sizes();
    state.emptyLanes = m_emptyLanes;
    state.gridMinDenom = m_gridMinDenom;
    state.gridTriplet = m_gridFeel == GridFeel::Triplet;
    state.eventList = eventListVisible();
    return state;
}

void SongView::applyViewState(const ViewState &state)
{
    if (!state.valid || !m_timeline)
        return;
    const double tpb = double(m_timeline->ticksPerBeat);
    m_pxPerTick = std::clamp(state.pxPerBeat, kMinPxPerBeat, kMaxPxPerBeat) / tpb;
    m_keyHeight = std::clamp(state.keyHeight, kMinKeyHeight, kMaxKeyHeight);
    setGridMinDenom(state.gridMinDenom); // setter validates the denominator
    setGridFeel(state.gridTriplet ? GridFeel::Triplet : GridFeel::Straight);
    m_editCursorTick = std::min<uint64_t>(state.editCursorTick, m_timeline->lengthTicks);
    for (const std::pair<int, uint8_t> &lane : state.emptyLanes)
        if (lane.first >= 0 && lane.first < 16 &&
            std::find(m_emptyLanes.begin(), m_emptyLanes.end(), lane) == m_emptyLanes.end())
            m_emptyLanes.push_back(lane);
    mergeEmptyLanes();
    m_lanes->setViewHeights(state.laneHeight, state.laneHeights);
    m_lanes->setRowRanges(state.laneRanges);
    m_lanes->setHiddenLanes(state.hiddenLanes); // rebuildRows below drops the rows
    // Restored straight into the rows, not through SongView's setter: a
    // restore must not force the (app-wide) automation pane open.
    m_lanes->setTempoLaneVisible(state.tempoLane);
    if (state.selectedTrack >= 0 && state.selectedTrack < 16 &&
        m_timeline->tracks[state.selectedTrack].used)
        selectTrack(state.selectedTrack);
    // Sidecars written before the velocity lane hold two sizes (roll, lanes);
    // the lane's pane opens at zero there and setVelocityLaneVisible borrows
    // its height when the user asks for it.
    QList<int> splitterSizes = state.splitterSizes;
    if (splitterSizes.size() == 2) {
        const int velocityH = velocityLaneVisible() ? kVelLaneH : 0;
        splitterSizes = {std::max(kVelLaneMinH, splitterSizes[0] - velocityH), velocityH,
                         splitterSizes[1]};
    }
    // A hidden pane reports (and restores at) zero, so only require a real
    // height from the panes that are actually showing.
    if (splitterSizes.size() == m_splitter->count() && splitterSizes.first() > 0 &&
        (splitterSizes.last() > 0 || !automationLanesVisible())) {
        // Real sizes exist; skip resizeEvent's default split.
        m_splitInit = true;
        m_splitter->setSizes(splitterSizes);
    }
    m_lanes->rebuildRows();
    updateScrollbars();
    setHScroll(state.scrollPx); // setHScroll clamps to the camera's range
    setVScroll(state.scrollY);
    setEventListVisible(state.eventList);
    refreshTimelineViews();
}

void SongView::setVoicegroup(const LoadedVoiceGroup *voicegroup)
{
    m_voicegroup = voicegroup;
    m_headers->rebuild();
    refreshTimelineViews();
}

void SongView::setGridFeel(GridFeel feel)
{
    if (m_gridFeel == feel)
        return;
    m_gridFeel = feel;
    m_ruler->syncGridControls();
    refreshTimelineViews();
}

void SongView::setGridMinDenom(int denom)
{
    if (denom != 4 && denom != 8 && denom != 16 && denom != 32)
        denom = 0;
    if (m_gridMinDenom == denom)
        return;
    m_gridMinDenom = denom;
    m_ruler->syncGridControls();
    refreshTimelineViews();
}

SongView::GridSeg SongView::gridSegAt(uint64_t tick) const
{
    GridSeg seg;
    if (!m_timeline)
        return seg;
    const uint64_t tpb = std::max<uint32_t>(1, m_timeline->ticksPerBeat);
    seg.beatTicks = tpb;
    for (const TimeSigPoint &ts : m_timeline->timeSigs) { // tick-sorted
        if (ts.tick > tick) {
            seg.next = ts.tick;
            break;
        }
        // Same-tick duplicates overwrite: the last at a tick wins, matching
        // forEachGridLine.
        seg.start = ts.tick;
        seg.beatTicks =
            std::max<uint64_t>(1, (uint64_t(tpb) * 4) >> std::min<int>(ts.denomPow2, 63));
    }
    return seg;
}

uint64_t SongView::gridTicksAt(uint64_t tick) const
{
    if (!m_timeline)
        return 24;
    return gridTicksIn(gridSegAt(tick));
}

uint64_t SongView::snapTicksAt(uint64_t tick) const
{
    if (!m_timeline)
        return 24;
    return gridTicksIn(gridSegAt(tick), /*snap=*/true);
}

uint64_t SongView::gridTicksIn(const GridSeg &seg, bool snap) const
{
    const uint64_t clock = m_document ? m_document->ticksPerClock() : 1;
    // Finest visible subdivision at least kAutoGridMinCellPx wide from the
    // feel's ladder
    // (divisions per beat), floored at the mid2agb clock grid and at the
    // user's minimum note value. The floor is one division per beat of the
    // governing signature (1/4 = the beat); triplet feel fits three notes
    // where straight fits two, so the same denominator allows 3/2 the
    // divisions.
    static constexpr uint64_t kStraight[] = {32, 16, 8, 4, 2, 1};
    static constexpr uint64_t kTriplet[] = {48, 24, 12, 6, 3, 1};
    const bool triplet = m_gridFeel == GridFeel::Triplet;
    const uint64_t maxDiv =
        m_gridMinDenom == 0
            ? UINT64_MAX
            : std::max<uint64_t>(1, uint64_t(m_gridMinDenom) * (triplet ? 3 : 2) / 8);
    const double pxPerSegBeat = m_pxPerTick * double(seg.beatTicks);
    const uint64_t *ladder = triplet ? kTriplet : kStraight;
    constexpr int kSteps = 6;
    int step = kSteps - 1; // whole beats when even one-per-beat cells are
                           // too narrow (ladder[kSteps - 1] == 1)
    for (int i = 0; i < kSteps; i++) {
        if (ladder[i] > maxDiv)
            continue;
        if (pxPerSegBeat / double(ladder[i]) >= songview::kAutoGridMinCellPx) {
            step = i;
            break;
        }
    }
    // Snapping runs one ladder step finer than the drawn grid, so edits
    // aren't limited to visible lines. The minimum subdivision is a display
    // floor only — snapping steps past it too. gcd keeps the snap grid a
    // divisor of the drawn grid when a beat's ticks don't split evenly, so
    // every drawn line stays snappable.
    const uint64_t vis = std::max(std::max<uint64_t>(1, seg.beatTicks / ladder[step]), clock);
    if (!snap || step == 0)
        return vis;
    const uint64_t fine = std::max<uint64_t>(1, seg.beatTicks / ladder[step - 1]);
    return std::max(std::gcd(vis, fine), clock);
}

uint64_t SongView::fineGridTicks() const
{
    return m_document ? std::max<uint32_t>(1, m_document->ticksPerClock()) : gridTicksAt(0);
}

uint64_t SongView::snapTick(double tick, bool fine) const
{
    tick = std::max(0.0, tick);
    if (fine) {
        // The clock grid is the document's absolute resolution; it does not
        // restart at time-signature changes.
        const double g = double(fineGridTicks());
        return uint64_t(std::round(tick / g) * g);
    }
    const GridSeg seg = gridSegAt(uint64_t(tick));
    const uint64_t g = std::max<uint64_t>(1, gridTicksIn(seg, /*snap=*/true));
    const uint64_t k = uint64_t((tick - double(seg.start)) / double(g));
    const uint64_t lo = seg.start + k * g;
    // The next signature's tick is itself a grid position (the grid
    // restarts there), so the upper candidate never crosses it.
    const uint64_t hi = std::min(lo + g, seg.next);
    return tick - double(lo) <= double(hi) - tick ? lo : hi;
}

uint64_t SongView::snapTickDown(double tick) const
{
    tick = std::max(0.0, tick);
    const GridSeg seg = gridSegAt(uint64_t(tick));
    const uint64_t g = std::max<uint64_t>(1, gridTicksIn(seg, /*snap=*/true));
    return seg.start + uint64_t((tick - double(seg.start)) / double(g)) * g;
}

uint64_t SongView::snapTickUp(double tick) const
{
    tick = std::max(0.0, tick);
    const GridSeg seg = gridSegAt(uint64_t(tick));
    const uint64_t g = std::max<uint64_t>(1, gridTicksIn(seg, /*snap=*/true));
    const uint64_t lo = seg.start + uint64_t((tick - double(seg.start)) / double(g)) * g;
    if (double(lo) >= tick)
        return lo;
    // The next signature's tick is itself a grid position, so the upper
    // candidate never crosses it.
    return std::min(lo + g, seg.next);
}

bool SongView::isSelected(const ViewNote &note) const
{
    if (note.track != m_selectedTrack)
        return false;
    const NoteKey id{note.startTick, note.key};
    return std::find(m_selection.begin(), m_selection.end(), id) != m_selection.end();
}

void SongView::setSelection(std::vector<NoteKey> ids)
{
    m_selection = std::move(ids);
    // The two selection kinds are mutually exclusive, so Ctrl+C is never
    // ambiguous.
    if (!m_selection.empty() && m_timeSel.active())
        clearTimeSelection();
    m_roll->invalidateContent();
    m_velocityLane->invalidateContent();
}

void SongView::clearSelection()
{
    if (!m_selection.empty()) {
        m_selection.clear();
        m_roll->invalidateContent();
        m_velocityLane->invalidateContent();
    }
}

void SongView::setTimeSelection(const TimeSelection &sel)
{
    m_timeSel = sel;
    if (m_timeSel.active() && !m_selection.empty())
        m_selection.clear();
    refreshTimelineViews();
}

void SongView::clearTimeSelection()
{
    m_timeSel = TimeSelection();
    refreshTimelineViews();
}

bool SongView::timeSelectionCoversTrack(int track) const
{
    if (!m_timeSel.active() || track < 0 || track > 15)
        return false;
    if (m_timeSel.scope == TimeSelection::Lanes)
        return false;
    // Track scope is live: it always mirrors the header selection.
    return trackSelectionMask() & (1u << track);
}

bool SongView::timeSelectionCoversRow(int track, uint8_t cc) const
{
    if (!m_timeSel.active())
        return false;
    if (m_timeSel.scope == TimeSelection::Lanes)
        return std::find(m_timeSel.lanes.begin(), m_timeSel.lanes.end(),
                         std::pair<int, uint8_t>(track, cc)) != m_timeSel.lanes.end();
    // Track scopes cover the track's CC/voice rows, never the global tempo.
    if (cc == DOC_CC_TEMPO)
        return false;
    return timeSelectionCoversTrack(track);
}

void SongView::announceTimeSelection()
{
    if (!m_timeSel.active() || !m_timeline)
        return;
    const double beats = double(m_timeSel.endTick - m_timeSel.startTick) /
                         double(std::max<uint32_t>(1, m_timeline->ticksPerBeat));
    QString scope;
    if (m_timeSel.scope == TimeSelection::Lanes) {
        scope = tr("%n lane(s)", nullptr, int(m_timeSel.lanes.size()));
    } else {
        const uint32_t mask = trackSelectionMask();
        int n = 0;
        for (int t = 0; t < 16; t++)
            n += (mask >> t) & 1;
        scope = tr("%n track(s)", nullptr, n);
    }
    emit statusMessage(tr("Time selection: %1 beats · %2 · Ctrl+C/X copies, "
                          "Del clears, Ctrl+V pastes at the edit cursor")
                           .arg(beats, 0, 'g', 4)
                           .arg(scope));
}

std::vector<int> SongView::timeSelectionTracks() const
{
    std::vector<int> tracks;
    if (!m_timeline || !m_document)
        return tracks;
    const uint32_t mask = trackSelectionMask();
    for (int t = 0; t < 16; t++) {
        if (!m_timeline->tracks[t].used || !(mask & (1u << t)))
            continue;
        if (m_document->smfTrackFor(t) < 0)
            continue;
        tracks.push_back(t);
    }
    return tracks;
}

std::vector<uint8_t> SongView::trackCcs(int track) const
{
    std::vector<uint8_t> ccs;
    for (const AutoLane &lane : m_model.lanes)
        if (lane.track == track)
            ccs.push_back(lane.cc); // LANE_CC_BEND == DOC_CC_BEND
    ccs.push_back(DOC_CC_VOICE);
    return ccs;
}

uint32_t SongView::songTicksPerBeat() const
{
    return m_timeline ? std::max<uint32_t>(1u, m_timeline->ticksPerBeat)
                      : MidiTimeline::kDefaultTicksPerBeat;
}

namespace {
// App-shared: one clipboard for every tab/SongView, so a copy made in one
// song pastes into another (a different tab, or a song opened over this
// one). Purely value data — no pointers into a document, timeline, or
// voicegroup — so it safely outlives its source song.
SongView::Clip &clipboardStorage()
{
    static SongView::Clip clip;
    return clip;
}
} // namespace

const SongView::Clip &SongView::clipboard()
{
    return clipboardStorage();
}

void SongView::storeClipboard(Clip clip)
{
    clipboardStorage() = std::move(clip);
}

void SongView::setClipboard(Clip clip)
{
    clip.ticksPerBeat = songTicksPerBeat();
    clip.voiceNames.clear();
    for (const ClipLane &cl : clip.lanes) {
        if (cl.cc != DOC_CC_VOICE)
            continue;
        for (const std::pair<uint32_t, int> &pv : cl.points) {
            if (pv.second < 0 || pv.second >= VOICEGROUP_SIZE)
                continue;
            clip.voiceNames[pv.second] =
                m_voicegroup ? QString::fromUtf8(m_voicegroup->voiceNames[pv.second]) : QString();
        }
    }
    storeClipboard(std::move(clip));
}

int SongView::foreignVoiceCount(const Clip &clip) const
{
    int count = 0;
    for (const ClipLane &cl : clip.lanes) {
        if (cl.cc != DOC_CC_VOICE)
            continue;
        for (const std::pair<uint32_t, int> &pv : cl.points) {
            const auto it = clip.voiceNames.find(pv.second);
            if (it == clip.voiceNames.end())
                continue;
            const QString here = m_voicegroup && pv.second >= 0 && pv.second < VOICEGROUP_SIZE
                                     ? QString::fromUtf8(m_voicegroup->voiceNames[pv.second])
                                     : QString();
            if (here != it->second)
                ++count;
        }
    }
    return count;
}

SongView::Clip SongView::clipForPaste() const
{
    Clip clip = clipboard();
    const uint32_t dst = songTicksPerBeat();
    const uint32_t src = std::max<uint32_t>(1u, clip.ticksPerBeat);
    if (src == dst)
        return clip;
    // Cross-song paste between differing MIDI divisions: rescale so the
    // music keeps its length (a quarter note stays a quarter note).
    // Nearest rounding, unlike rescaleDivision (midiimport.cpp), which
    // floors to reproduce mid2agb's import arithmetic; a paste has no such
    // contract and nearest keeps the pasted music closest to the original.
    // Rounding can land two events on one tick — the later one wins (the
    // editor's same-tick convention), deduped here so the document never
    // sees colliding inserts. Note ends are scaled as ticks rather than
    // durations: scaling is monotonic, so a note that ended at or before
    // the next same-key note's start still does, and rounding can never
    // create an overlap between pasted notes (a note collapsed to zero
    // length keeps one tick, colliding only with a note at that same tick,
    // which the dedup settles).
    const auto scale = [&](uint64_t tick) {
        return uint64_t(std::llround(double(tick) * double(dst) / double(src)));
    };
    uint64_t lastTick = 0; // furthest scaled event start
    for (ClipTrack &ct : clip.tracks) {
        for (ClipNote &cn : ct.notes) {
            const uint64_t start = scale(cn.relTick);
            const uint64_t end = scale(uint64_t(cn.relTick) + cn.duration);
            cn.relTick = uint32_t(start);
            cn.duration = uint32_t(std::max<uint64_t>(1, end - start));
            lastTick = std::max(lastTick, start);
        }
        if (src > dst) {
            std::set<std::pair<uint32_t, uint8_t>> seen;
            std::vector<ClipNote> kept;
            for (auto it = ct.notes.rbegin(); it != ct.notes.rend(); ++it)
                if (seen.insert({it->relTick, it->key}).second)
                    kept.push_back(*it);
            std::reverse(kept.begin(), kept.end());
            ct.notes = std::move(kept);
        }
    }
    for (ClipLane &cl : clip.lanes) {
        for (std::pair<uint32_t, int> &pv : cl.points) {
            pv.first = uint32_t(scale(pv.first));
            lastTick = std::max<uint64_t>(lastTick, pv.first);
        }
        if (src > dst) {
            std::set<uint32_t> seen;
            std::vector<std::pair<uint32_t, int>> kept;
            for (auto it = cl.points.rbegin(); it != cl.points.rend(); ++it)
                if (seen.insert(it->first).second)
                    kept.push_back(*it);
            std::reverse(kept.begin(), kept.end());
            cl.points = std::move(kept);
        }
    }
    // The span rounds independently of the events inside it, so an event in
    // the source span's last fraction of a tick can round to the scaled
    // span itself — outside the [start, start + span) window a range paste
    // clears. Grow the span to keep every event inside it.
    if (clip.span > 0)
        clip.span = std::max<uint64_t>({1, scale(clip.span), lastTick + 1});
    clip.ticksPerBeat = dst;
    return clip;
}

void SongView::copyTimeSelection()
{
    if (!m_document || !m_timeSel.active())
        return;
    const uint64_t s = m_timeSel.startTick;
    const uint64_t e = m_timeSel.endTick;
    Clip clip;
    clip.span = e - s;
    int noteCount = 0;
    int pointCount = 0;
    const auto copyLanePoints = [&](int track, uint8_t cc) {
        ClipLane lane{track, cc, {}};
        const int query = track < 0 ? m_selectedTrack : track;
        for (const DocLanePoint &pt : m_document->lanePoints(query, cc)) {
            if (pt.tick >= s && pt.tick < e)
                lane.points.push_back({uint32_t(pt.tick - s), pt.value});
        }
        pointCount += int(lane.points.size());
        // Empty segments are kept: they carry "this span is silent" so paste
        // clears the destination range.
        clip.lanes.push_back(std::move(lane));
    };
    if (m_timeSel.scope == TimeSelection::Lanes) {
        for (const std::pair<int, uint8_t> &id : m_timeSel.lanes)
            copyLanePoints(id.first, id.second);
    } else {
        for (int t : timeSelectionTracks()) {
            ClipTrack ct{t, {}};
            for (const DocNote &note : m_document->notesForTrack(t)) {
                if (note.tick < s || note.tick >= e)
                    continue;
                ct.notes.push_back(
                    {uint32_t(note.tick - s), note.key,
                     note.duration ? note.duration : uint32_t(gridTicksAt(note.tick)),
                     note.velocity});
            }
            noteCount += int(ct.notes.size());
            clip.tracks.push_back(std::move(ct));
            for (uint8_t cc : trackCcs(t))
                copyLanePoints(t, cc);
        }
    }
    setClipboard(std::move(clip));
    announce(tr("Copied range: %1 note(s), %2 automation point(s)").arg(noteCount).arg(pointCount));
}

void SongView::deleteTimeSelection()
{
    if (!m_document || !m_timeSel.active())
        return;
    const uint64_t s = m_timeSel.startTick;
    const uint64_t e = m_timeSel.endTick;
    SongDocument::RangeEdit edit;
    const auto removeLanePoints = [&](int track, uint8_t cc) {
        const int query = track < 0 ? m_selectedTrack : track;
        for (const DocLanePoint &pt : m_document->lanePoints(query, cc)) {
            if (pt.tick >= s && pt.tick < e)
                edit.removePoints.push_back(pt);
        }
    };
    if (m_timeSel.scope == TimeSelection::Lanes) {
        for (const std::pair<int, uint8_t> &id : m_timeSel.lanes)
            removeLanePoints(id.first, id.second);
    } else {
        for (int t : timeSelectionTracks()) {
            for (const DocNote &note : m_document->notesForTrack(t)) {
                if (note.tick >= s && note.tick < e)
                    edit.removeNotes.push_back(note);
            }
            for (uint8_t cc : trackCcs(t))
                removeLanePoints(t, cc);
        }
    }
    if (edit.empty()) {
        announce(tr("Nothing to delete in the time selection"));
        return;
    }
    const int notes = int(edit.removeNotes.size());
    const int points = int(edit.removePoints.size());
    m_document->applyRangeEdit(tr("delete range"), edit);
    announce(tr("Deleted range: %1 note(s), %2 automation point(s)").arg(notes).arg(points));
}

void SongView::transposeTimeSelection(int dKey)
{
    if (!m_document || !m_timeSel.active() || dKey == 0 || m_timeSel.scope == TimeSelection::Lanes)
        return;
    const uint64_t s = m_timeSel.startTick;
    const uint64_t e = m_timeSel.endTick;
    std::vector<DocNote> notes;
    for (int t : timeSelectionTracks()) {
        for (const DocNote &note : m_document->notesForTrack(t)) {
            if (note.tick >= s && note.tick < e)
                notes.push_back(note);
        }
    }
    if (notes.empty()) {
        announce(tr("No notes in the time selection"));
        return;
    }
    for (const DocNote &note : notes) {
        const int key = int(note.key) + dKey;
        if (key < 0 || key > 127) {
            announce(tr("Transpose out of range"));
            return;
        }
    }
    m_document->moveNotes(notes, 0, dKey, /*mergeable=*/true);
    // Keep the moved notes in sight: the row the move headed toward
    // scrolls into view just enough (no re-centering).
    int edge = int(notes.front().key) + dKey;
    for (const DocNote &note : notes) {
        const int key = int(note.key) + dKey;
        edge = dKey > 0 ? std::max(edge, key) : std::min(edge, key);
    }
    ensureKeyVisible(edge);
    announce(tr("Transposed %n note(s) by %1", nullptr, int(notes.size()))
                 .arg(dKey > 0 ? QStringLiteral("+%1").arg(dKey) : QString::number(dKey)));
}

void SongView::nudgeTimeSelection(bool right)
{
    if (!m_document || !m_timeSel.active())
        return;
    const uint64_t s = m_timeSel.startTick;
    const uint64_t e = m_timeSel.endTick;
    const uint64_t snapped = right ? snapTickUp(double(s) + 1.0) : snapTickDown(double(s) - 1.0);
    const int64_t dTick = int64_t(snapped) - int64_t(s);
    if (dTick == 0)
        return;
    std::vector<DocNote> notes;
    std::vector<DocLanePoint> points;
    const auto gatherLanePoints = [&](int track, uint8_t cc) {
        const int query = track < 0 ? m_selectedTrack : track;
        for (const DocLanePoint &pt : m_document->lanePoints(query, cc)) {
            if (pt.tick >= s && pt.tick < e)
                points.push_back(pt);
        }
    };
    if (m_timeSel.scope == TimeSelection::Lanes) {
        for (const std::pair<int, uint8_t> &id : m_timeSel.lanes)
            gatherLanePoints(id.first, id.second);
    } else {
        for (int t : timeSelectionTracks()) {
            for (const DocNote &note : m_document->notesForTrack(t)) {
                if (note.tick >= s && note.tick < e)
                    notes.push_back(note);
            }
            for (uint8_t cc : trackCcs(t))
                gatherLanePoints(t, cc);
        }
    }
    m_document->moveRange(notes, points, dTick);
    // The band follows even over empty content, so repeated nudges keep
    // aiming at the same region.
    TimeSelection moved = m_timeSel;
    moved.startTick = uint64_t(int64_t(s) + dTick);
    moved.endTick = uint64_t(int64_t(e) + dTick);
    setTimeSelection(moved);
    ensureRangeVisible(moved.startTick, moved.endTick, right);
}

void SongView::removeTimeSelectionContents()
{
    if (!m_document || !m_timeline || !m_timeSel.active())
        return;
    const uint64_t s = m_timeSel.startTick;
    const uint64_t e = m_timeSel.endTick;
    SongDocument::RippleScope scope;
    QString scopeText;
    if (m_timeSel.scope == TimeSelection::Lanes) {
        scope.lanes = m_timeSel.lanes;
        scopeText = tr("%n lane(s)", nullptr, int(scope.lanes.size()));
    } else {
        scope.tracks = timeSelectionTracks();
        if (scope.tracks.empty())
            return;
        int used = 0;
        for (int t = 0; t < 16; t++)
            used += m_timeline->tracks[t].used ? 1 : 0;
        scope.wholeSong = int(scope.tracks.size()) == used;
        scopeText = scope.wholeSong ? tr("all tracks")
                                    : tr("%n track(s)", nullptr, int(scope.tracks.size()));
    }
    if (!m_document->removeTimeRange(s, e, scope)) {
        announce(tr("Nothing to remove in the time selection"));
        return;
    }
    // The span is gone and later content now sits under where the selection
    // was; clear it and park the edit cursor at the seam.
    clearTimeSelection();
    commitEditCursor(s);
    const double beats = double(e - s) / double(std::max<uint32_t>(1, m_timeline->ticksPerBeat));
    announce(tr("Removed %1 beats on %2 — later events shifted left")
                 .arg(beats, 0, 'g', 4)
                 .arg(scopeText));
}

void SongView::pasteRangeAtEditCursor()
{
    if (!m_document || clipboard().span == 0 || clipboard().empty())
        return;
    const Clip clip = clipForPaste();
    const uint64_t s = snapTick(double(m_editCursorTick));
    const uint64_t e = s + clip.span;

    // A clip whose content came from one track retargets to the selected
    // track (cross-track copy); multi-track clips paste back in place.
    int sole = -2;
    bool multi = false;
    const auto consider = [&](int track) {
        if (track < 0)
            return; // tempo is global
        if (sole == -2)
            sole = track;
        else if (sole != track)
            multi = true;
    };
    for (const ClipTrack &ct : clip.tracks)
        consider(ct.track);
    for (const ClipLane &cl : clip.lanes)
        consider(cl.track);
    const auto mapTrack = [&](int track) {
        return track < 0 ? -1 : (multi ? track : m_selectedTrack);
    };

    SongDocument::RangeEdit edit;
    // Cross-song, a multi-track clip can name tracks this song doesn't
    // have; they're skipped, and the announcement says so.
    std::set<int> skippedTracks;
    bool landed = false; // any of the clip's content found a track here
    for (const ClipTrack &ct : clip.tracks) {
        const int t = mapTrack(ct.track);
        if (t < 0 || m_document->smfTrackFor(t) < 0) {
            if (!ct.notes.empty())
                skippedTracks.insert(ct.track);
            continue;
        }
        landed = true;
        // Replace: whatever notes start inside the destination span go away.
        for (const DocNote &note : m_document->notesForTrack(t)) {
            if (note.tick >= s && note.tick < e)
                edit.removeNotes.push_back(note);
        }
        if (!ct.notes.empty()) {
            SongDocument::RangeEdit::TrackNotes tn{t, {}};
            for (const ClipNote &cn : ct.notes)
                tn.notes.push_back({s + cn.relTick, cn.key, cn.duration, cn.velocity});
            edit.addNotes.push_back(std::move(tn));
        }
    }
    for (const ClipLane &cl : clip.lanes) {
        const int t = mapTrack(cl.track);
        if (t >= 0 && m_document->smfTrackFor(t) < 0) {
            if (!cl.points.empty())
                skippedTracks.insert(cl.track);
            continue;
        }
        landed = true;
        const int query = t < 0 ? m_selectedTrack : t;
        for (const DocLanePoint &pt : m_document->lanePoints(query, cl.cc)) {
            if (pt.tick >= s && pt.tick < e)
                edit.removePoints.push_back(pt);
        }
        if (!cl.points.empty()) {
            SongDocument::RangeEdit::LaneWrite lw{t, cl.cc, {}};
            for (const std::pair<uint32_t, int> &pv : cl.points)
                lw.points.push_back({s + pv.first, pv.second});
            edit.addPoints.push_back(std::move(lw));
        }
    }
    if (!landed) {
        // Nothing to paste (cross-song, every source track is missing here):
        // no edit, so no cursor move or selection change either — those
        // would read as a paste that never happened.
        announce(tr("Nothing pasted · none of the copied tracks exist in this song"));
        return;
    }
    m_document->applyRangeEdit(tr("paste range"), edit);

    // Set up for tiling: the edit cursor advances to the end of the pasted
    // span so repeated Ctrl+V lays copies back-to-back, and the selection is
    // cleared so its band doesn't sit in the way of the next ruler click.
    clearTimeSelection();
    commitEditCursor(e);
    // Anchor on the start of the pasted span, not the advanced cursor:
    // seeing the content that just landed is what confirms the paste.
    ensureTickVisible(s);
    QStringList notes;
    if (!skippedTracks.empty())
        notes << tr("%n source track(s) skipped (no matching track here)", nullptr,
                    int(skippedTracks.size()));
    if (const int foreign = foreignVoiceCount(clip); foreign > 0)
        notes << tr("%n voice change(s) name a different instrument in this voicegroup", nullptr,
                    foreign);
    if (notes.isEmpty())
        announce(tr("Pasted range · edit cursor moved to its end — paste again to repeat"));
    else
        announce(tr("Pasted range · %1").arg(notes.join(QStringLiteral(" · "))));
}

// Maps the four transpose commands to their semitone step, 0 when the event
// matches none. Shared by the note-selection and time-selection key paths so
// a rebinding changes both at once.
int SongView::transposeStepFor(const QKeyEvent *event) const
{
    const auto &keys = keymap::Registry::instance();
    if (keys.matches(event, QStringLiteral("roll.transpose_up")))
        return 1;
    if (keys.matches(event, QStringLiteral("roll.transpose_down")))
        return -1;
    if (keys.matches(event, QStringLiteral("roll.transpose_up_octave")))
        return 12;
    if (keys.matches(event, QStringLiteral("roll.transpose_down_octave")))
        return -12;
    return 0;
}

bool SongView::handleEditKey(QKeyEvent *event)
{
    if (!m_document)
        return false;
    const auto &keys = keymap::Registry::instance();
    const bool sel = m_timeSel.active();
    if (sel && keys.matches(event, QStringLiteral("roll.copy"))) {
        copyTimeSelection();
        event->accept();
        return true;
    }
    if (sel && keys.matches(event, QStringLiteral("roll.cut"))) {
        copyTimeSelection();
        deleteTimeSelection();
        event->accept();
        return true;
    }
    if (sel && keys.matches(event, QStringLiteral("roll.delete"))) {
        deleteTimeSelection();
        event->accept();
        return true;
    }
    if (sel) {
        const int transpose = transposeStepFor(event);
        if (transpose != 0) {
            transposeTimeSelection(transpose);
            event->accept();
            return true;
        }
    }
    if (sel && (keys.matches(event, QStringLiteral("roll.nudge_left")) ||
                keys.matches(event, QStringLiteral("roll.nudge_right")))) {
        nudgeTimeSelection(keys.matches(event, QStringLiteral("roll.nudge_right")));
        event->accept();
        return true;
    }
    if (keys.matches(event, QStringLiteral("roll.paste")) && clipboard().span > 0 &&
        !clipboard().empty()) {
        pasteRangeAtEditCursor();
        event->accept();
        return true;
    }
    if (keys.matches(event, QStringLiteral("roll.mute_tracks"))) {
        toggleMuteOnSelectedTracks();
        event->accept();
        return true;
    }
    if (keys.matches(event, QStringLiteral("roll.solo_tracks"))) {
        toggleSoloOnSelectedTracks();
        event->accept();
        return true;
    }
    if (keys.matches(event, QStringLiteral("view.velocity_lane"))) {
        // A held key auto-repeats; only the real press flips the pane.
        if (!event->isAutoRepeat())
            setVelocityLaneVisible(!velocityLaneVisible());
        event->accept();
        return true;
    }
    if (keys.matches(event, QStringLiteral("view.automation_lanes"))) {
        // A held key auto-repeats; only the real press flips the pane.
        if (!event->isAutoRepeat())
            setAutomationLanesVisible(!automationLanesVisible());
        event->accept();
        return true;
    }
    if (keys.matches(event, QStringLiteral("automation.pencil_mode"))) {
        // A held key auto-repeats; only the real press flips the mode. The
        // press state sticks around for handleEditKeyRelease, which decides
        // sticky tap vs momentary hold.
        if (!event->isAutoRepeat()) {
            m_pencilKeyHeld = true;
            m_pencilKeyGesture = false;
            m_pencilKeyPrior = automationPencilMode();
            m_pencilKey = event->key();
            m_pencilKeyPressedAt = std::chrono::steady_clock::now();
            setAutomationPencilMode(!m_pencilKeyPrior);
        }
        event->accept();
        return true;
    }
    return false;
}

// Ableton-style momentary pencil: a quick tap toggles and stays, while a
// hold — past the threshold, or with a lane gesture drawn during it —
// reverts to the pre-press mode on release, so the key works as a
// hold-to-draw (or, from pencil mode, hold-to-arrow) chord.
bool SongView::handleEditKeyRelease(QKeyEvent *event)
{
    if (!m_pencilKeyHeld || event->key() != m_pencilKey)
        return false;
    // A held key's synthesized auto-repeat releases are not the real one.
    if (!event->isAutoRepeat()) {
        m_pencilKeyHeld = false;
        if (m_pencilKeyGesture ||
            std::chrono::steady_clock::now() - m_pencilKeyPressedAt >= kPencilMomentaryHold)
            setAutomationPencilMode(m_pencilKeyPrior);
    }
    event->accept();
    return true;
}

void SongView::markPencilKeyGesture()
{
    if (m_pencilKeyHeld)
        m_pencilKeyGesture = true;
}

bool SongView::automationPencilMode() const
{
    return m_lanes && m_lanes->pencilMode();
}

void SongView::setAutomationPencilMode(bool on)
{
    if (m_lanes)
        m_lanes->setPencilMode(on);
}

void SongView::showTimeSelectionMenu(const QPoint &globalPos)
{
    if (!m_document || !m_timeSel.active())
        return;
    QMenu menu(this);
    // Display-only hints mirroring the keymap, like the note context menu.
    const auto &keys = keymap::Registry::instance();
    QAction *copy = menu.addAction(tr("Copy range"));
    copy->setShortcut(keys.bindings(QStringLiteral("roll.copy")).value(0));
    QAction *cut = menu.addAction(tr("Cut range"));
    cut->setShortcut(keys.bindings(QStringLiteral("roll.cut")).value(0));
    QAction *del = menu.addAction(tr("Delete range"));
    QAction *removeContents = menu.addAction(tr("Remove contents (shift left)"));
    QAction *paste = menu.addAction(tr("Paste at edit cursor"));
    paste->setShortcut(keys.bindings(QStringLiteral("roll.paste")).value(0));
    paste->setEnabled(clipboard().span > 0 && !clipboard().empty());
    menu.addSeparator();
    QAction *clear = menu.addAction(tr("Clear selection"));
    QAction *chosen = menu.exec(globalPos);
    if (chosen == copy) {
        copyTimeSelection();
    } else if (chosen == cut) {
        copyTimeSelection();
        deleteTimeSelection();
    } else if (chosen == del) {
        deleteTimeSelection();
    } else if (chosen == removeContents) {
        removeTimeSelectionContents();
    } else if (chosen == paste) {
        pasteRangeAtEditCursor();
    } else if (chosen == clear) {
        clearTimeSelection();
    }
}

void SongView::announceNote(const ViewNote &note)
{
    if (!m_timeline)
        return;
    const bool ext = m_document && m_document->cfg().extendedClocks;
    const bool exact = m_document && m_document->cfg().exactGate;
    const int64_t ticks = int64_t(note.endTick) - int64_t(note.startTick);
    emit statusMessage(
        tr("%1 · velocity %2 → plays %3 · length %4 ticks → %5 clocks")
            .arg(keyName(note.key))
            .arg(note.velocity)
            .arg(mid2agbEffectiveVelocity(note.velocity))
            .arg(ticks)
            .arg(mid2agbEffectiveDuration(ticks, m_timeline->ticksPerBeat, ext, exact)));
}

void SongView::auditionTimed(int track, int key, int velocity, uint64_t startTick, uint64_t endTick)
{
    if (!m_timeline || endTick <= startTick)
        return;
    uint64_t dur = m_timeline->sampleForTick(endTick) - m_timeline->sampleForTick(startTick);
    // Safety cap: an unterminated note's span runs to the end of the song,
    // which is not a useful audition length.
    const uint64_t cap = uint64_t(m_timeline->sampleRate * 10.0);
    if (cap > 0)
        dur = std::min(dur, cap);
    if (dur > 0)
        emit auditionNoteTimed(track, key, velocity, quint32(std::min<uint64_t>(dur, UINT32_MAX)),
                               auditionVolume(track, startTick), auditionPan(track, startTick));
}

void SongView::setPlayheadSample(uint64_t samplePos, bool playing)
{
    if (!m_timeline)
        return;
    m_playheadTick = m_timeline->tickForSample(samplePos);
    m_playing = playing;
    // Follow the playhead — unless following is switched off (transport
    // bar), and never while the user is mid-gesture (panning, dragging notes
    // or selections, sweeping automation): yanking the view out from under a
    // held mouse button is disorienting.
    if (playing && m_followPlayhead && !userGestureActive()) {
        const qreal px = contentX(m_playheadTick);
        const qreal vw = viewportWidth();
        if (px < 0.0 || px > vw * 85.0 / 100.0)
            setHScroll(m_playheadTick * m_pxPerTick - vw / 10.0);
    }
    m_events->setPlayheadTick(m_playheadTick, playing);
    m_headers->syncVoices();
    syncPlayheadOverlay();
}

bool SongView::userGestureActive() const
{
    return (m_ruler && m_ruler->gestureActive()) || (m_roll && m_roll->gestureActive()) ||
           (m_lanes && m_lanes->gestureActive()) ||
           (m_velocityLane && m_velocityLane->gestureActive());
}

void SongView::syncPlayheadOverlay()
{
    if (m_playheadOverlay) {
        m_playheadOverlay->setPlayhead(contentX(m_playheadTick), m_timeline != nullptr, m_playing);
    }
}

void SongView::setEditCursorTick(uint64_t tick)
{
    if (m_editCursorTick == tick)
        return;
    m_editCursorTick = tick;
    m_headers->syncVoices();
    refreshTimelineViews();
}

void SongView::commitEditCursor(uint64_t tick)
{
    setEditCursorTick(tick);
    emit editCursorMoved(tick);
}

void SongView::goToStart()
{
    // Home shows the pre-roll pad so tick 0 sits inside the viewport, not
    // flush against its edge.
    setHScroll(minHScroll());
    commitEditCursor(0);
}

qreal SongView::displayX(double tick, qreal origin, qreal dpr) const
{
    const qreal widgetX = origin + contentX(tick);
    return dpr > 0.0 ? std::round(widgetX * dpr) / dpr : widgetX;
}

double SongView::pxPerBeat() const
{
    return m_pxPerTick * double(songTicksPerBeat());
}

void SongView::selectTrack(int track)
{
    if (track == m_selectedTrack || track < 0 || track > 15)
        return;
    m_selectedTrack = track;
    // Programmatic selection collapses the multi-track scope;
    // trackHeaderClicked restores it for modifier clicks.
    m_trackSelMask = 1u << track;
    m_selection.clear();
    // A track-scoped time selection would keep rendering only in the ruler
    // once the shown track leaves its scope; drop it on any track switch.
    clearTimeSelection();
    m_headers->syncSelection();
    m_lanes->rebuildRows();
    // Switching tracks readies the roll for keyboard editing (e.g. copy on
    // one track, click another's header, paste), wherever focus was.
    m_roll->setFocus();
    m_roll->invalidateContent();
    emit selectedTrackChanged(track);
}

bool SongView::revealNote(int track, uint8_t key, uint64_t tick)
{
    if (track < 0 || track > 15)
        return false;
    selectTrack(track);
    // Notes are sorted by startTick, so the last match is the note that was
    // sounding (or had just finished fading) at the event's position.
    const ViewNote *found = nullptr;
    for (const ViewNote &note : m_model.notes) {
        if (note.startTick > tick)
            break;
        if (note.track == track && note.key == key)
            found = &note;
    }
    if (!found)
        return false;
    setSelection({NoteKey{found->startTick, found->key}});
    ensureKeyVisible(key);
    return true;
}

uint32_t SongView::trackSelectionMask() const
{
    uint32_t used = 0;
    if (m_timeline) {
        for (int t = 0; t < 16; t++)
            if (m_timeline->tracks[t].used)
                used |= 1u << t;
    }
    const uint32_t mask = (m_trackSelMask | (1u << m_selectedTrack)) & used;
    return mask ? mask : (1u << m_selectedTrack);
}

void SongView::trackHeaderClicked(int track, Qt::KeyboardModifiers modifiers)
{
    if (track < 0 || track > 15)
        return;
    if (modifiers & Qt::ControlModifier) {
        uint32_t mask = trackSelectionMask() ^ (1u << track);
        if (mask == 0)
            return; // the scope can't go empty
        if (!(mask & (1u << m_selectedTrack))) {
            // The primary track was toggled out; hand primary to the lowest
            // remaining scoped track. This is a scope adjustment, not a
            // track switch, so the time selection survives (selectTrack
            // clears it and collapses the mask — restore both after).
            int next = 0;
            while (!(mask & (1u << next)))
                next++;
            const TimeSelection keep = m_timeSel;
            selectTrack(next);
            m_timeSel = keep;
        }
        m_trackSelMask = mask;
    } else if (modifiers & Qt::ShiftModifier) {
        const int lo = std::min(track, m_selectedTrack);
        const int hi = std::max(track, m_selectedTrack);
        uint32_t mask = 0;
        for (int t = lo; t <= hi; t++) {
            if (m_timeline && m_timeline->tracks[t].used)
                mask |= 1u << t;
        }
        m_trackSelMask = mask | (1u << m_selectedTrack);
    } else {
        selectTrack(track);
        m_trackSelMask = 1u << track; // collapse even when already primary
    }
    m_headers->syncSelection();
    // The time selection's track scope is live; repaint its bands.
    refreshTimelineViews();
}

void SongView::setTrackMute(int track, bool on)
{
    const uint32_t bit = 1u << track;
    const uint32_t mask = on ? (m_muteMask | bit) : (m_muteMask & ~bit);
    if (mask != m_muteMask) {
        m_muteMask = mask;
        emit muteMaskChanged(mask);
    }
}

void SongView::setTrackSolo(int track, bool on)
{
    const uint32_t bit = 1u << track;
    const uint32_t mask = on ? (m_soloMask | bit) : (m_soloMask & ~bit);
    if (mask != m_soloMask) {
        m_soloMask = mask;
        emit soloMaskChanged(mask);
    }
}

// Names the scoped tracks for the status line: "track 3" or "tracks 1, 3".
static QString scopedTracksText(uint32_t mask)
{
    QStringList nums;
    for (int t = 0; t < 16; t++) {
        if (mask & (1u << t))
            nums << QString::number(t + 1);
    }
    return nums.size() == 1 ? SongView::tr("track %1").arg(nums.first())
                            : SongView::tr("tracks %1").arg(nums.join(QStringLiteral(", ")));
}

void SongView::toggleMuteOnSelectedTracks()
{
    const uint32_t scope = trackSelectionMask();
    const bool allOn = (m_muteMask & scope) == scope;
    const uint32_t mask = allOn ? (m_muteMask & ~scope) : (m_muteMask | scope);
    if (mask == m_muteMask)
        return;
    m_muteMask = mask;
    emit muteMaskChanged(mask);
    announce(allOn ? tr("Unmuted %1").arg(scopedTracksText(scope))
                   : tr("Muted %1").arg(scopedTracksText(scope)));
}

void SongView::toggleSoloOnSelectedTracks()
{
    const uint32_t scope = trackSelectionMask();
    const bool allOn = (m_soloMask & scope) == scope;
    const uint32_t mask = allOn ? (m_soloMask & ~scope) : (m_soloMask | scope);
    if (mask == m_soloMask)
        return;
    m_soloMask = mask;
    emit soloMaskChanged(mask);
    announce(allOn ? tr("Unsoloed %1").arg(scopedTracksText(scope))
                   : tr("Soloed %1").arg(scopedTracksText(scope)));
}

QColor SongView::trackColor(int track)
{
    return themes::trackIdentityColor(trackIdentityIndex(track));
}

QColor SongView::noteColor(int track, int velocity)
{
    if (velocity <= 0)
        return themes::color(themes::Role::song_view_note_velocity_zero);
    if (velocity >= 127)
        return trackColor(track);
    const double t = 1.0 - (double(velocity) / 127.0);
    return mixTowardOklab(trackColor(track),
                          themes::color(themes::Role::song_view_note_velocity_zero), t);
}

QColor SongView::velocityNoteColor(int velocity)
{
    if (velocity <= 0)
        return themes::color(themes::Role::song_view_note_velocity_zero);
    // Purple's hue (~250°) interpolates linearly down to red's (~1°), which
    // is the long way around the wheel — through blue, green, and yellow —
    // so the full spectrum spreads across the velocity range.
    static const QColor kMinVelocity(0x5f, 0x44, 0xe9);
    static const QColor kMaxVelocity(0xe9, 0x09, 0x04);
    if (velocity <= 1)
        return kMinVelocity;
    if (velocity >= 127)
        return kMaxVelocity;
    const float t = float(velocity - 1) / 126.0f;
    float h0, s0, v0, h1, s1, v1;
    kMinVelocity.getHsvF(&h0, &s0, &v0);
    kMaxVelocity.getHsvF(&h1, &s1, &v1);
    // Quantize to 8-bit RGB: QColor equality is spec- and depth-sensitive,
    // and callers compare against rendered pixels.
    return QColor(
        QColor::fromHsvF(h0 + (h1 - h0) * t, s0 + (s1 - s0) * t, v0 + (v1 - v0) * t).rgb());
}

void SongView::setFollowPlayhead(bool on)
{
    m_followPlayhead = on;
    m_events->setFollowPlayhead(on);
}

void SongView::setVelocityColorMode(bool on)
{
    if (m_velocityColorMode == on)
        return;
    m_velocityColorMode = on;
    m_roll->invalidateContent();
    // The lane's nodes take the same ramp, so they turn over with the roll.
    if (m_velocityLane)
        m_velocityLane->invalidateContent();
}

void SongView::setNoteNameMode(bool on)
{
    if (m_noteNameMode == on)
        return;
    m_noteNameMode = on;
    m_roll->invalidateContent();
}

QColor SongView::noteFillColor(int track, int velocity) const
{
    return m_velocityColorMode ? velocityNoteColor(velocity) : noteColor(track, velocity);
}

std::optional<uint8_t> SongView::velocityLanePreview(const ViewNote &note) const
{
    if (!m_velocityLane || !note.noteId.isAssigned())
        return std::nullopt;
    return m_velocityLane->previewVelocity(note.noteId);
}

void SongView::velocityPreviewChanged()
{
    // The lane repaints itself; this is the roll's half of the same preview.
    m_roll->invalidateContent();
}

std::optional<uint8_t> SongView::rollVelocityPreview(const ViewNote &note) const
{
    if (!m_roll)
        return std::nullopt;
    return m_roll->velocityDragPreview(note);
}

void SongView::rollVelocityPreviewChanged()
{
    // The roll repaints itself; this is the lane's half of the same preview.
    if (m_velocityLane)
        m_velocityLane->invalidateContent();
}

int SongView::programAtTick(int track, uint64_t tick) const
{
    if (!m_timeline)
        return -1;
    int prog = m_timeline->tracks[track].firstProgram;
    for (const VoiceChange &vc : m_model.voices) {
        if (vc.tick > tick)
            break; // sorted by tick
        if (vc.track == track)
            prog = vc.program;
    }
    return prog;
}

uint64_t SongView::displayTick() const
{
    return m_playing ? uint64_t(std::max(0.0, m_playheadTick)) : m_editCursorTick;
}

int SongView::currentProgram(int track) const
{
    return programAtTick(track, displayTick());
}

uint8_t SongView::trackVolumeAt(int track, uint64_t tick, uint64_t *nextChangeTick) const
{
    const int master = m_document ? m_document->cfg().masterVolume : kM4aMaxVolume;
    return m4aEffectiveTrackVolume(trackRawVolumeAt(track, tick, nextChangeTick), master);
}

namespace {

// The value a track's CC automation holds at a tick, starting from the value
// the engine primes the controller with. When nextChangeTick is given it is
// lowered to the next point on that lane past the tick, if that comes first.
int ccValueAt(const SongViewModel &model, int track, uint8_t cc, int primed, uint64_t tick,
              uint64_t *nextChangeTick)
{
    int value = primed;
    for (const AutoLane &lane : model.lanes) {
        if (lane.track != track || lane.cc != cc)
            continue;
        for (const LanePoint &point : lane.points) { // sorted by tick
            if (point.tick <= tick) {
                value = point.value;
                continue;
            }
            if (nextChangeTick)
                *nextChangeTick = std::min(*nextChangeTick, uint64_t(point.tick));
            break;
        }
        break;
    }
    return value;
}

} // namespace

int SongView::trackRawVolumeAt(int track, uint64_t tick, uint64_t *nextChangeTick) const
{
    // mid2agb primes every track with VOL 127 before its first event
    // (agb.cpp PrintTrackHeader), so a track that never sets a volume still
    // plays at full track volume — scaled by the song's master volume.
    return ccValueAt(m_model, track, 7, kM4aMaxVolume, tick, nextChangeTick);
}

int8_t SongView::trackPanAt(int track, uint64_t tick, uint64_t *nextChangeTick) const
{
    // PAN is primed centered (c_v = 64), and the engine stores it as the byte
    // less 64 (m4a_engine_cc case 0xA).
    const int value = ccValueAt(m_model, track, 10, 64, tick, nextChangeTick);
    return int8_t(std::clamp(value, 0, 127) - 64);
}

SongView::VoiceContext SongView::voiceContext(uint64_t tick) const
{
    if (!m_timeline || !m_voicegroup || m_selectedTrack < 0 || m_selectedTrack >= 16)
        return {};
    const int program = programAtTick(m_selectedTrack, tick);
    uint64_t endTick = UINT64_MAX;
    for (const VoiceChange &change : m_model.voices) {
        if (change.track == m_selectedTrack && change.tick > tick) {
            endTick = change.tick; // sorted by tick: the first one past it
            break;
        }
    }
    // The track's VOL and PAN both move the CGB envelope goal just as the
    // voice choice does, so a change to either ends this context too: past it
    // the same channel has a different set of loudness levels.
    const uint8_t volume = trackVolumeAt(m_selectedTrack, tick, &endTick);
    const int8_t pan = trackPanAt(m_selectedTrack, tick, &endTick);
    if (program < 0 || program >= VOICEGROUP_SIZE)
        return {nullptr, -1, endTick, volume, pan};
    return {&m_voicegroup->voices[program], program, endTick, volume, pan};
}

void SongView::revealVoice(int program)
{
    if (program >= 0 && program < 128)
        emit revealVoiceRequested(program);
}

void SongView::revealTrackVoice(int track)
{
    if (!m_timeline || track < 0 || track > 15)
        return;
    const int prog = currentProgram(track);
    if (prog < 0) {
        emit statusMessage(tr("Track %1 has no voice set.").arg(track + 1));
        return;
    }
    revealVoice(prog);
}

QSet<int> SongView::usedVoices() const
{
    QSet<int> used;
    if (!m_timeline)
        return used;
    for (int t = 0; t < 16; t++) {
        if (m_timeline->tracks[t].used && m_timeline->tracks[t].firstProgram >= 0)
            used.insert(m_timeline->tracks[t].firstProgram);
    }
    for (const VoiceChange &vc : m_model.voices)
        used.insert(vc.program);
    return used;
}

QString SongView::voiceLabel(uint8_t program) const
{
    return QStringLiteral("%1 %2")
        .arg(int(program), 3, 10, QLatin1Char('0'))
        .arg(voiceShortName(program));
}

QString SongView::instrumentLabel(int track) const
{
    if (!m_timeline)
        return QString();
    const int prog = currentProgram(track);
    if (prog < 0)
        return tr("(no voice set)");
    return voiceLabel(uint8_t(prog));
}

QString SongView::voiceShortName(uint8_t program) const
{
    QString name;
    QString type;
    if (m_voicegroup && program < VOICEGROUP_SIZE) {
        name = QString::fromUtf8(m_voicegroup->voiceNames[program]).trimmed();
        type = m4aVoiceTypeName(m_voicegroup->voices[program].type);
    }
    if (name.isEmpty())
        return type.isEmpty() ? tr("Voice") : type;
    return QStringLiteral("%1 (%2)").arg(name, type);
}

bool SongView::pickVoice(const QString &title, int initialVoice, int *outVoice)
{
    VoicePickerDialog dialog(this, title, initialVoice, [this](int voice, int velocity) {
        emit auditionVoice(voice, kVoiceAuditionKey, velocity);
    });
    if (dialog.exec() != QDialog::Accepted)
        return false;
    *outVoice = dialog.selectedVoice();
    return true;
}

void SongView::editTrackVoice(int track)
{
    if (!m_document || track < 0 || track > 15)
        return;
    const std::vector<DocLanePoint> changes = m_document->lanePoints(track, DOC_CC_VOICE);
    // The track's initial voice is the LAST change on the first change's
    // tick: same-tick duplicates are audibly last-wins, and the header label
    // (currentProgram) already reads them that way — edit what it shows.
    const DocLanePoint *target = nullptr;
    for (const DocLanePoint &pt : changes) {
        if (pt.tick != changes.front().tick)
            break;
        target = &pt;
    }
    const int initial = target ? target->value : 0;
    int voice = initial;
    if (!pickVoice(tr("Track %1 voice").arg(track + 1), initial, &voice))
        return;
    if (!target)
        m_document->addLanePoint(track, DOC_CC_VOICE, 0, voice);
    else if (voice != initial)
        m_document->moveLanePoint(track, DOC_CC_VOICE, *target, target->tick, voice);
}

void SongView::renameTrack(int track)
{
    if (!m_document || track < 0 || track > 15 || m_document->smfTrackFor(track) < 0)
        return;
    m_headers->beginRename(track);
}

void SongView::commitTrackRename(int track, const QString &name)
{
    if (!m_document || track < 0 || track > 15 || m_document->smfTrackFor(track) < 0)
        return;
    const QString trimmed = name.trimmed();
    if (nameIsLoopMarker(trimmed)) {
        announce(tr("\"%1\" is read by the song build as a loop or label "
                    "marker, so it can't be a track name.")
                     .arg(trimmed));
        return;
    }
    // Queued: the commit arrives from the header row's editor signal, and
    // the edit rebuilds the header panel — deleting that editor mid-signal.
    QMetaObject::invokeMethod(
        this,
        [this, track, trimmed] {
            if (m_document)
                m_document->renameTrack(track, trimmed);
        },
        Qt::QueuedConnection);
}

void SongView::addTrack()
{
    if (!m_document || !m_document->canAddTrack())
        return;
    int voice = 0;
    if (!pickVoice(tr("New track voice"), 0, &voice))
        return;
    const int track = m_document->addTrack(voice); // rebuilds via documentChanged
    if (track >= 0) {
        selectTrack(track);
        announce(tr("Added track %1").arg(track + 1));
    }
}

void SongView::duplicateTrack(int track)
{
    if (!m_document || track < 0 || track > 15 || m_document->smfTrackFor(track) < 0)
        return;
    const int copy = m_document->duplicateTrack(track); // rebuilds via documentChanged
    if (copy >= 0) {
        selectTrack(copy);
        announce(tr("Duplicated track %1 as track %2").arg(track + 1).arg(copy + 1));
    }
}

void SongView::deleteTrack(int track)
{
    if (!m_document || track < 0 || track > 15 || m_document->smfTrackFor(track) < 0)
        return;
    // Removing a chunk shifts every higher engine slot down by one; move
    // the per-track view state with it, before the document edit rebuilds
    // the headers and lanes.
    const uint32_t low = (1u << track) - 1;
    const uint32_t mute = (m_muteMask & low) | ((m_muteMask >> 1) & ~low);
    const uint32_t solo = (m_soloMask & low) | ((m_soloMask >> 1) & ~low);
    if (mute != m_muteMask) {
        m_muteMask = mute;
        emit muteMaskChanged(mute);
    }
    if (solo != m_soloMask) {
        m_soloMask = solo;
        emit soloMaskChanged(solo);
    }
    for (auto it = m_emptyLanes.begin(); it != m_emptyLanes.end();) {
        if (it->first == track) {
            it = m_emptyLanes.erase(it);
        } else {
            if (it->first > track)
                it->first--;
            ++it;
        }
    }
    if (m_selectedTrack > track)
        m_selectedTrack--;
    // Track slots shift; collapse the multi-track scope and drop the time
    // selection rather than remap them.
    m_trackSelMask = 1u << m_selectedTrack;
    clearTimeSelection();
    m_document->deleteTrack(track); // rebuilds via documentChanged
    announce(tr("Deleted track %1").arg(track + 1));
}

void SongView::moveTrack(int from, int to)
{
    if (!m_document)
        return;
    // The document decides validity; the per-track view state follows in
    // onTrackMoved, which the reorder op signals through — undo and redo
    // replay the same permutation, so the masks stay on their tracks.
    if (m_document->moveTrack(from, to)) // rebuilds via documentChanged
        announce(tr("Moved track %1 to slot %2").arg(from + 1).arg(to + 1));
}

void SongView::onTrackMoved(int, int, const QVector<int> &map)
{
    // A reorder op is applying or reverting (interactive move, undo, or
    // redo — the document emits each direction with the inverse map): rotate
    // the per-track view state along with the renumbered engine slots
    // (deleteTrack's shift, generalized). The note selection needs nothing:
    // it is (tick, key) on the selected track, and the selected track's
    // number moves with its notes. The document is mid-mutation: remap
    // state only, don't read it back.
    if (map.size() < 16)
        return;
    const auto newIndex = [&map](int t) { return t >= 0 && t < 16 ? map[t] : t; };
    const auto permuteMask = [&newIndex](uint32_t mask) {
        uint32_t out = 0;
        for (int t = 0; t < 16; t++) {
            if (mask & (1u << t))
                out |= 1u << newIndex(t);
        }
        return out;
    };
    const uint32_t mute = permuteMask(m_muteMask);
    const uint32_t solo = permuteMask(m_soloMask);
    if (mute != m_muteMask) {
        m_muteMask = mute;
        emit muteMaskChanged(mute);
    }
    if (solo != m_soloMask) {
        m_soloMask = solo;
        emit soloMaskChanged(solo);
    }
    for (auto &lane : m_emptyLanes)
        lane.first = newIndex(lane.first);
    m_selectedTrack = newIndex(m_selectedTrack);
    // The multi-track scope and time selection are track-addressed;
    // collapse them like deleteTrack does rather than remap.
    m_trackSelMask = 1u << m_selectedTrack;
    clearTimeSelection();
}

void SongView::forEachGridLine(uint64_t tickBegin, uint64_t tickEnd,
                               const std::function<void(uint64_t, bool, int, int)> &fn) const
{
    if (!m_timeline || tickEnd <= tickBegin)
        return;
    const uint32_t tpb = m_timeline->ticksPerBeat;

    struct Seg {
        uint64_t tick;
        uint64_t beatTicks;
        int beatsPerBar;
    };
    std::vector<Seg> segs;
    segs.push_back({0, tpb, 4});
    for (const TimeSigPoint &ts : m_timeline->timeSigs) {
        uint64_t beatTicks = (uint64_t(tpb) * 4) >> std::min<int>(ts.denomPow2, 63);
        if (beatTicks < 1)
            beatTicks = 1;
        const Seg seg{ts.tick, beatTicks, ts.numerator ? ts.numerator : 4};
        if (ts.tick == segs.back().tick)
            segs.back() = seg;
        else
            segs.push_back(seg);
    }

    int bar = 1;
    for (size_t i = 0; i < segs.size(); i++) {
        const Seg &seg = segs[i];
        const uint64_t segEnd =
            i + 1 < segs.size() ? segs[i + 1].tick : std::max<uint64_t>(tickEnd, seg.tick);
        const uint64_t clampedEnd = std::min(segEnd, tickEnd);
        if (seg.tick < clampedEnd) {
            uint64_t k = tickBegin > seg.tick ? (tickBegin - seg.tick) / seg.beatTicks : 0;
            for (uint64_t tick = seg.tick + k * seg.beatTicks; tick < clampedEnd;
                 tick += seg.beatTicks, k++) {
                if (tick < tickBegin)
                    continue;
                fn(tick, k % seg.beatsPerBar == 0, bar + int(k / seg.beatsPerBar),
                   int(k % seg.beatsPerBar) + 1);
            }
        }
        if (i + 1 < segs.size()) {
            const uint64_t segTicks = segs[i + 1].tick - seg.tick;
            const uint64_t barTicks = seg.beatTicks * seg.beatsPerBar;
            bar += int((segTicks + barTicks - 1) / barTicks);
        }
    }
}

void SongView::zoomAroundContentX(double factor, qreal anchorContentX)
{
    if (!m_timeline)
        return;
    const double tpb = double(m_timeline->ticksPerBeat);
    const double oldPxPerTick = m_pxPerTick;
    m_pxPerTick = std::clamp(oldPxPerTick * factor, kMinPxPerBeat / tpb, kMaxPxPerBeat / tpb);
    m_scrollX = std::clamp(
        cursorAnchoredScroll(double(anchorContentX), oldPxPerTick, m_scrollX, m_pxPerTick),
        minHScroll(), maxHScroll());
    updateScrollbars();
    refreshTimelineViews();
}

void SongView::zoomKeyHeight(const QWheelEvent *event)
{
    if (!m_timeline)
        return;
    const double zoomDelta = wheelAngleUnits(event);
    if (zoomDelta == 0.0)
        return;
    const double oldH = m_keyHeight;
    const double newH =
        std::clamp(oldH * std::exp2(zoomDelta / 1200.0), kMinKeyHeight, kMaxKeyHeight);
    if (newH == m_keyHeight)
        return;
    // Pin the content row under the cursor before projecting to the scrollbar.
    const double anchorY = event->position().y();
    const double anchoredScroll = cursorAnchoredScroll(anchorY, oldH, m_scrollY, newH);
    m_keyHeight = newH;
    updateScrollbars();
    setVScroll(std::clamp(anchoredScroll, 0.0, maxRollScroll()));
    // The camera scale changed even when the cursor anchor keeps its scroll
    // offset numerically unchanged.
    m_roll->invalidateContent();
}

void SongView::scrollByPx(double dx)
{
    setHScroll(m_scrollX + dx);
}

void SongView::scrollRollBy(double dy)
{
    setVScroll(m_scrollY + dy);
}

void SongView::setHScroll(double px)
{
    const double newX = std::clamp(px, minHScroll(), maxHScroll());
    const bool cameraChanged = newX != m_scrollX;
    m_scrollX = newX;
    const int scrollbarValue = scrollUnits(m_scrollX);
    if (m_hbar->value() != scrollbarValue) {
        m_hbar->blockSignals(true);
        m_hbar->setValue(scrollbarValue);
        m_hbar->blockSignals(false);
    }
    if (cameraChanged)
        refreshTimelineViews();
}

void SongView::ensureTickVisible(uint64_t tick)
{
    const qreal vw = viewportWidth();
    const qreal dpr = m_roll->devicePixelRatioF();
    const qreal physicalPixel = logicalPhysicalPixel(dpr);
    const qreal displayedX = displayX(double(tick), 0.0, dpr);
    if (displayedX >= 0.0 && displayedX <= vw - physicalPixel)
        return;
    setHScroll(double(tick) * m_pxPerTick - vw / 3.0);
}

void SongView::ensureRangeVisible(uint64_t startTick, uint64_t endTick, bool preferEnd)
{
    const qreal x0 = contentX(double(startTick));
    const qreal x1 = contentX(double(endTick));
    const qreal vw = viewportWidth();
    const qreal dpr = m_roll->devicePixelRatioF();
    const qreal physicalPixel = logicalPhysicalPixel(dpr);
    const qreal displayedX0 = displayX(double(startTick), 0.0, dpr);
    const qreal displayedX1 = displayX(double(endTick), 0.0, dpr);
    const qreal rightEdge = vw - physicalPixel;
    qreal dx = 0.0;
    if (displayedX1 - displayedX0 > rightEdge)
        // Wider than the viewport: the leading edge wins.
        dx = preferEnd ? x1 - rightEdge : x0;
    else if (displayedX1 > rightEdge)
        dx = x1 - rightEdge;
    else if (displayedX0 < 0.0)
        dx = x0;
    if (dx != 0.0)
        setHScroll(m_scrollX + dx);
}

void SongView::ensureKeyVisible(int key)
{
    const double y0 = (127 - key) * m_keyHeight - m_scrollY;
    const double y1 = y0 + m_keyHeight;
    const int vh = m_roll->height();
    if (y0 < 0)
        setVScroll(m_scrollY + y0);
    else if (y1 > vh)
        setVScroll(m_scrollY + y1 - vh);
}

int SongView::viewportWidth() const
{
    return std::max(50, m_roll->width() - kKeyboardW);
}

double SongView::leadPadPx() const
{
    // Whole DIPs: the pad is a camera resting position (fresh songs and
    // "go to start" home here), and an integral camera keeps note edges
    // on the same raster seams as the classic scroll-0 home.
    return std::clamp(std::round(double(viewportWidth()) * 0.10), 48.0, 256.0);
}

double SongView::minHScroll() const
{
    return m_timeline ? -leadPadPx() : 0.0;
}

double SongView::maxHScroll() const
{
    // A full viewport of scratch space past the song: max scroll rests the
    // song's end at the content area's left edge. Notes pasted out there
    // grow lengthTicks on the next timeline rebuild, renewing the space.
    return m_timeline ? double(m_timeline->lengthTicks) * m_pxPerTick : 0.0;
}

double SongView::maxRollScroll() const
{
    return std::max(0.0, 128.0 * m_keyHeight - m_roll->height());
}

void SongView::setVScroll(double y)
{
    const double newY = std::clamp(y, 0.0, maxRollScroll());
    const bool cameraChanged = m_scrollY != newY;
    m_scrollY = newY;
    const int scrollbarValue = scrollUnits(m_scrollY);
    if (m_vbar->value() != scrollbarValue) {
        m_vbar->blockSignals(true);
        m_vbar->setValue(scrollbarValue);
        m_vbar->blockSignals(false);
    }
    if (cameraChanged)
        m_roll->invalidateContent();
}

void SongView::updateScrollbars()
{
    m_hbar->blockSignals(true);
    m_hbar->setRange(scrollUnits(minHScroll()), scrollUnits(maxHScroll()));
    m_hbar->setPageStep(scrollUnits(double(viewportWidth())));
    m_hbar->blockSignals(false);
    setHScroll(m_scrollX);

    m_vbar->blockSignals(true);
    m_vbar->setRange(0, scrollUnits(maxRollScroll()));
    m_vbar->setPageStep(scrollUnits(double(m_roll->height())));
    m_vbar->blockSignals(false);
    setVScroll(m_scrollY);
}

void SongView::refreshTimelineViews()
{
    m_ruler->update();
    m_roll->invalidateContent();
    m_lanes->invalidateContent();
    m_velocityLane->invalidateContent();
    m_strip->invalidateContent();
    syncPlayheadOverlay();
}

void SongView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // The splitter starts with the lanes area at its classic fixed height;
    // sizes can only be applied once real geometry exists.
    if (!m_splitInit && m_splitter->height() > 0) {
        m_splitInit = true;
        const int velocityH = velocityLaneVisible() ? kVelLaneH : 0;
        const int lanesH = automationLanesVisible() ? kLanesAreaH : 0;
        m_splitter->setSizes(
            {std::max(120, m_splitter->height() - lanesH - velocityH), velocityH, lanesH});
    }
    updateScrollbars();
    syncPlayheadOverlay();
}
