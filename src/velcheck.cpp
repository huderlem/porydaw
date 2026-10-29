#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <vector>

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QFontMetrics>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPixmap>
#include <QSettings>
#include <QSplitter>
#include <QTemporaryDir>
#include <QToolButton>
#include <QWheelEvent>

#include "core/noteid.h"
#include "core/songdocument.h"
#include "core/velocitymodel.h"
#include "project/decompproject.h"
#include "ui/keymap.h"
#include "ui/layout.h"
#include "ui/songview.h"
#include "ui/songviewmodel.h"
#include "ui/theme/color_math.h"
#include "ui/theme/themeruntime.h"
#include "ui/typography.h"
#include "ui/velocityaxis.h"
#include "ui/velocitygesturemodel.h"

extern "C" {
#include "voicegroup_loader.h"
}

// --velmodelcheck: PSG velocity model check (self-contained, no project
// needed). VelocityMap is the engine-behavior half of the velocity lane: it
// decides which voice a note plays on and which stored velocities that voice
// cannot tell apart. The numbers here are the engine's, not a design choice —
// the effective-velocity rounding is m4a's, the 16 envelope steps are the CGB
// channels', and the wave channel's five loudness classes are gCgb3Vol's
// distinct outputs (external/poryaaaa/plugin/m4a_tables.c). Volume is part of
// the question: the engine derives a CGB note's loudness from velocity and
// the track's compiled VOL byte together, and mid2agb has already folded the
// song's master volume into that byte, so a quieter song genuinely has fewer
// detents rather than the same ones played softer.
// Ported from specker/cleanup/psg-velocity-history-pr's velocity-model check.

namespace {

bool equals(const std::optional<std::size_t> &value, std::size_t expected)
{
    return value && *value == expected;
}

// A track at full volume in a song at full master volume: the level tables
// most of this check describes are that case, which is also the only one the
// detents used to model.
constexpr uint8_t kFullVolume = uint8_t(kM4aMaxVolume);

} // namespace

int runVelocityModelCheck()
{
    int failures = 0;
    const auto check = [&failures](bool condition, const char *message) {
        if (!condition) {
            std::fprintf(stderr, "velmodelcheck: FAIL: %s\n", message);
            ++failures;
        }
    };

    ToneData squareTone{};
    squareTone.type = VOICE_SQUARE_1;
    ToneData squareTwoTone{};
    squareTwoTone.type = VOICE_SQUARE_2;
    ToneData noiseTone{};
    noiseTone.type = VOICE_NOISE;
    ToneData waveTone{};
    waveTone.type = VOICE_PROGRAMMABLE_WAVE;
    ToneData directSoundTone{};
    directSoundTone.type = VOICE_DIRECTSOUND;
    const VelocityMap square = VelocityMap::resolve(&squareTone, 60, kFullVolume);
    const VelocityMap squareTwo = VelocityMap::resolve(&squareTwoTone, 60, kFullVolume);
    const VelocityMap noise = VelocityMap::resolve(&noiseTone, 60, kFullVolume);
    const VelocityMap wave = VelocityMap::resolve(&waveTone, 60, kFullVolume);
    const VelocityMap directSound = VelocityMap::resolve(&directSoundTone, 60, kFullVolume);
    const VelocityMap unresolved = VelocityMap::resolve(nullptr, std::nullopt, kFullVolume);

    check(square.isPsg() && std::strcmp(square.voiceName(), "Square 1") == 0,
          "Square 1 should resolve intrinsically");
    check(squareTwo.isPsg() && std::strcmp(squareTwo.voiceName(), "Square 2") == 0,
          "Square 2 should resolve intrinsically");
    check(noise.isPsg() && std::strcmp(noise.voiceName(), "Noise") == 0,
          "Noise should resolve intrinsically");
    check(wave.isPsg() && std::strcmp(wave.voiceName(), "Programmable Wave") == 0,
          "Wave should resolve intrinsically");
    check(!directSound.isPsg() && directSound.levelCount() == 0,
          "DirectSound should remain continuous");
    check(!unresolved.isPsg() && unresolved.levelCount() == 0,
          "missing voice should remain unresolved");

    // The CGB mask keeps the alternate voice types on their channel.
    ToneData squareAltTone{};
    squareAltTone.type = VOICE_SQUARE_1_ALT;
    ToneData directSoundAltTone{};
    directSoundAltTone.type = VOICE_DIRECTSOUND_ALT;
    check(VelocityMap::resolve(&squareAltTone, 60, kFullVolume) == square,
          "the alternate Square 1 type should map to the same voice");
    check(!VelocityMap::resolve(&directSoundAltTone, 60, kFullVolume).isPsg() &&
              VelocityMap::resolve(&directSoundAltTone, 60, kFullVolume) == directSound,
          "the alternate DirectSound type should stay continuous");

    ToneData invalidTone{};
    invalidTone.type = VOICE_CRY;
    const VelocityMap invalid = VelocityMap::resolve(&invalidTone, 60, kFullVolume);
    check(!invalid.isPsg() && invalid != directSound,
          "a cry voice masks to no CGB channel and is not DirectSound either");
    // The nested child's CGB bits would classify as Square 1 if the second
    // keysplit hop were followed instead of refused.
    std::array<ToneData, 128> nestedChildren{};
    nestedChildren[60].type = VOICE_KEYSPLIT | VOICE_SQUARE_1;
    ToneData nestedSplit{};
    nestedSplit.type = VOICE_KEYSPLIT_ALL;
    nestedSplit.subGroup = nestedChildren.data();
    check(!VelocityMap::resolve(&nestedSplit, 60, kFullVolume).isPsg(),
          "nested keysplit should be invalid");
    // The children here are PSG, so only refusing to guess a key keeps this
    // keysplit continuous.
    std::array<ToneData, 128> psgChildren{};
    psgChildren[60].type = VOICE_SQUARE_1;
    ToneData keylessSplit{};
    keylessSplit.type = VOICE_KEYSPLIT_ALL;
    keylessSplit.subGroup = psgChildren.data();
    check(!VelocityMap::resolve(&keylessSplit, std::nullopt, kFullVolume).isPsg() &&
              VelocityMap::resolve(&keylessSplit, 60, kFullVolume).isPsg(),
          "keyless keysplit should remain continuous");
    ToneData subgroupLessSplit{};
    subgroupLessSplit.type = VOICE_KEYSPLIT_ALL;
    check(!VelocityMap::resolve(&subgroupLessSplit, 60, kFullVolume).isPsg(),
          "a keysplit without a subgroup must not be dereferenced");
    std::array<ToneData, 128> splitChildren{};
    std::array<uint8_t, 128> splitTable{};
    splitChildren[7].type = VOICE_PROGRAMMABLE_WAVE;
    splitTable[60] = 7;
    ToneData splitTone{};
    splitTone.type = VOICE_KEYSPLIT;
    splitTone.subGroup = splitChildren.data();
    splitTone.keySplitTable = splitTable.data();
    const VelocityMap keyedSplit = VelocityMap::resolve(&splitTone, 60, kFullVolume);
    check(keyedSplit.isPsg() && std::strcmp(keyedSplit.voiceName(), "Programmable Wave") == 0,
          "keyed keysplit should resolve its selected voice");
    ToneData tableLessSplit{};
    tableLessSplit.type = VOICE_KEYSPLIT;
    tableLessSplit.subGroup = splitChildren.data();
    check(!VelocityMap::resolve(&tableLessSplit, 60, kFullVolume).isPsg(),
          "a table-less VOICE_KEYSPLIT must not be indexed");

    const std::array<uint8_t, 16> squareNoiseRepresentatives = {
        1, 12, 20, 28, 36, 44, 52, 60, 68, 76, 84, 92, 100, 108, 116, 127,
    };
    const std::array<uint8_t, 5> waveRepresentatives = {1, 32, 64, 96, 127};
    const auto representativesMatch = [](const VelocityMap &map, const auto &expected) {
        if (map.levelCount() != expected.size())
            return false;
        for (std::size_t level = 0; level < expected.size(); ++level) {
            if (map.representative(int(level)) != expected[level])
                return false;
        }
        return true;
    };
    check(representativesMatch(square, squareNoiseRepresentatives),
          "Square representatives should be exact");
    check(representativesMatch(wave, waveRepresentatives), "Wave representatives should be exact");
    check(representativesMatch(noise, squareNoiseRepresentatives),
          "Noise representatives should describe every hardware level");
    check(equals(square.levelOf(1), 0) && equals(square.levelOf(127), 15) &&
              equals(wave.levelOf(1), 0) && equals(wave.levelOf(127), 4),
          "intrinsic levels should include velocity boundaries");
    check(!directSound.levelOf(1) && !unresolved.levelOf(64),
          "continuous voices should not gain an intrinsic level");
    check(square.compatibleWith(square), "matching maps should be compatible");
    check(!square.compatibleWith(squareTwo) && !square.compatibleWith(noise) &&
              !directSound.compatibleWith(directSound),
          "different PSG identities — and continuous voices — should be incompatible");

    // Every stored velocity must land in the level whose range contains it,
    // and the ranges must tile 1-127 without gaps or overlap.
    const auto rangesTile = [&](const VelocityMap &map) {
        int expectedFirst = 1;
        for (std::size_t level = 0; level < map.levelCount(); ++level) {
            const VelocityLevelRange range = map.levelRange(int(level));
            if (range.first != expectedFirst || range.last < range.first)
                return false;
            for (int velocity = range.first; velocity <= range.last; ++velocity) {
                if (!equals(map.levelOf(velocity), level))
                    return false;
            }
            const uint8_t representative = map.representative(int(level));
            if (representative < range.first || representative > range.last)
                return false;
            expectedFirst = range.last + 1;
        }
        return expectedFirst == 128;
    };
    check(rangesTile(square) && rangesTile(noise) && rangesTile(wave),
          "level ranges should tile the velocity domain and contain their representatives");
    check(square.levelRange(-1).first == 1 && square.levelRange(99).last == 127 &&
              directSound.levelRange(64).first == 64 && directSound.levelRange(64).last == 64 &&
              directSound.representative(64) == 64,
          "level ranges should clamp, and a continuous voice's range is the velocity itself");

    check(square.canonicalize(1) == 1 && square.canonicalize(127) == 127,
          "Square canonicalization should retain endpoints");
    check(square.canonicalize(64) == 60 && wave.canonicalize(80) == 64,
          "canonicalization should select the representative for its hardware class");
    check(square.canonicalize(8) == 1 && square.canonicalize(9) == 12 &&
              noise.canonicalize(8) == 1 && noise.canonicalize(9) == 12 &&
              wave.canonicalize(112) == 96,
          "canonicalization should preserve hardware classes at their boundaries");
    check(wave.canonicalize(65) == 64 && square.canonicalize(65) == 68 &&
              noise.canonicalize(65) == 68 && directSound.canonicalize(65) == 65 &&
              invalid.canonicalize(65) == 65,
          "canonicalization should respect every voice type and fallback");
    check(square.canonicalize(0) == 1 && square.canonicalize(200) == 127 &&
              directSound.canonicalize(0) == 1 && directSound.canonicalize(200) == 127,
          "canonicalization should clamp out-of-range proposals into the MIDI domain");

    const std::optional<std::size_t> originLevel = wave.levelOf(95);
    check(equals(originLevel, 3) && wave.moveLevels(95, 0) == 95 && wave.moveLevels(95, -1) == 64,
          "returning to an origin level should restore its exact value");
    const std::array<uint8_t, 3> moved = {
        wave.moveLevels(95, 1),
        square.moveLevels(60, 1),
        directSound.moveLevels(65, 1),
    };
    check(moved == std::array<uint8_t, moved.size()>{127, 68, 66},
          "level movement should preserve heterogeneous exact origins");
    check(wave.moveLevels(1, -1) == 1 && square.moveLevels(127, 1) == 127 &&
              directSound.moveLevels(1, -1) == 1 && directSound.moveLevels(127, 1) == 127,
          "level movement should clamp at both velocity endpoints");
    // A non-representative origin only survives while the gesture is still on
    // its own level; stepping away and back lands on the representative.
    check(square.moveLevels(square.moveLevels(65, 1), -1) == 68,
          "stepping off a level and back should land on that level's representative");

    // --- the track volume is half of what a CGB channel can be heard doing.
    // A note's loudness is a 4-bit envelope goal the engine derives from
    // velocity and volume TOGETHER, so a quieter song has fewer detents to
    // snap between, not the same detents played quieter.
    check(m4aEffectiveTrackVolume(127, 127) == 127 && m4aEffectiveTrackVolume(127, 64) == 64 &&
              m4aEffectiveTrackVolume(64, 127) == 64 && m4aEffectiveTrackVolume(100, 90) == 70 &&
              m4aEffectiveTrackVolume(127, 0) == 0,
          "the compiled VOL byte should be mid2agb's own vol*mvl/mxv");
    check(m4aEffectiveTrackVolume(-5, 200) == 0 && m4aEffectiveTrackVolume(200, -5) == 0 &&
              m4aEffectiveTrackVolume(200, 200) == 127,
          "the compiled VOL byte should clamp both of its inputs");

    // Level counts across the volume range, each one the engine's own answer
    // (ChnVolSetAsm then CgbModVol) for how many distinct goals velocity 1-127
    // can still reach at that volume.
    const std::array<std::pair<uint8_t, std::size_t>, 7> squareCounts = {
        std::pair<uint8_t, std::size_t>{127, 16},
        {112, 14},
        {96, 12},
        {64, 8},
        {32, 4},
        {16, 2},
        {8, 1},
    };
    bool squareCountsMatch = true;
    for (const auto &[volume, expected] : squareCounts) {
        squareCountsMatch = squareCountsMatch &&
                            VelocityMap::resolve(&squareTone, 60, volume).levelCount() == expected;
    }
    check(squareCountsMatch, "a square channel should lose levels as the track volume falls");
    const std::array<std::pair<uint8_t, std::size_t>, 4> waveCounts = {
        std::pair<uint8_t, std::size_t>{127, 5},
        {96, 4},
        {64, 3},
        {16, 1},
    };
    bool waveCountsMatch = true;
    for (const auto &[volume, expected] : waveCounts) {
        waveCountsMatch =
            waveCountsMatch && VelocityMap::resolve(&waveTone, 60, volume).levelCount() == expected;
    }
    check(waveCountsMatch, "the wave channel's five classes should thin out with the volume too");

    // At exactly half volume the square channel's eight surviving levels are
    // sixteen velocities wide apiece — the full-volume table with every
    // second boundary gone.
    const VelocityMap halfSquare = VelocityMap::resolve(&squareTone, 60, 64);
    check(rangesTile(halfSquare), "a reduced-volume level table should still tile 1-127");
    check(halfSquare.levelRange(0).last == 16 && halfSquare.levelRange(1).first == 17 &&
              halfSquare.levelRange(7).first == 113 && halfSquare.levelRange(7).last == 127 &&
              halfSquare.representative(0) == 1 && halfSquare.representative(1) == 24 &&
              halfSquare.representative(7) == 127,
          "half volume should halve the square channel's levels and widen each one");
    const VelocityMap halfWave = VelocityMap::resolve(&waveTone, 60, 64);
    check(rangesTile(halfWave) && halfWave.levelRange(0).last == 32 &&
              halfWave.levelRange(1).last == 96 && halfWave.levelRange(2).first == 97,
          "half volume should leave the wave channel three of its five classes");

    // Quiet enough and the channel is simply inaudible whatever the velocity
    // says. There is nothing to snap between there, so the lane must not
    // offer detents that would rewrite every velocity it touched to 1.
    const VelocityMap silentSquare = VelocityMap::resolve(&squareTone, 60, 8);
    check(silentSquare.isPsg() && silentSquare.levelCount() == 1 && !silentSquare.hasDetents() &&
              equals(silentSquare.levelOf(127), 0),
          "a volume too low to reach the first envelope step should leave no detents");
    check(square.hasDetents() && wave.hasDetents() && !directSound.hasDetents() &&
              VelocityMap::resolve(&squareTone, 60, 16).hasDetents() &&
              !VelocityMap::resolve(&waveTone, 60, 16).hasDetents(),
          "two or more reachable levels is what makes a channel detented");

    // Volume is part of the ruler's identity: notes under different volumes
    // are on different level tables and cannot share one intrinsic context.
    check(square == VelocityMap::resolve(&squareTone, 60, kFullVolume) && square != halfSquare &&
              !square.compatibleWith(halfSquare) && halfSquare.compatibleWith(halfSquare),
          "maps under different track volumes must not be mistaken for each other");
    check(directSound == VelocityMap::resolve(&directSoundTone, 60, 64),
          "a continuous voice is the same map at any volume");

    // PAN moves the level boundaries as well. TrkVolPitSet scales one side by
    // (y+128) and the other by (127-y), so panning does not simply trade one
    // for the other: hard right leaves the right side at nearly double the
    // centered value and the left at zero, and CgbModVol's sum of the two
    // lands a level away from where the centered chain puts it.
    //
    // The expected levels are the engine's own, read back from
    // M4ACGBChannel::envelopeGoal after m4a_engine_note_on on a square voice
    // at song volume 82 / VOL 127 / PAN c_v+63 — the case that exposed the
    // centered-pan assumption (16 quarter notes up the velocity range).
    const std::array<uint8_t, 16> panVelocities = {1,  12, 20, 28, 36,  44,  52,  60,
                                                   68, 76, 84, 92, 100, 108, 116, 127};
    const std::array<std::size_t, 16> centeredLevels = {0, 0, 1, 2, 2, 3, 4, 4,
                                                        5, 5, 6, 7, 7, 8, 9, 10};
    const std::array<std::size_t, 16> hardRightLevels = {0, 0, 1, 2, 2, 3, 4, 4,
                                                         5, 6, 6, 7, 7, 8, 9, 10};
    const VelocityMap centered = VelocityMap::resolve(&squareTone, 60, 82, 0);
    const VelocityMap hardRight = VelocityMap::resolve(&squareTone, 60, 82, 63);
    bool centeredLevelsMatch = true;
    bool hardRightLevelsMatch = true;
    for (std::size_t i = 0; i < panVelocities.size(); i++) {
        centeredLevelsMatch =
            centeredLevelsMatch && equals(centered.levelOf(panVelocities[i]), centeredLevels[i]);
        hardRightLevelsMatch =
            hardRightLevelsMatch && equals(hardRight.levelOf(panVelocities[i]), hardRightLevels[i]);
    }
    check(centeredLevelsMatch, "a centered track's levels should be the engine's own");
    check(hardRightLevelsMatch,
          "a hard-panned track's levels should follow the engine's, not the centered chain's");
    check(rangesTile(hardRight) && hardRight.levelCount() == 11,
          "a panned level table should still tile 1-127");
    const VelocityMap hardLeft = VelocityMap::resolve(&squareTone, 60, 82, -64);
    check(hardLeft.levelCount() == 11 && rangesTile(hardLeft),
          "panning the other way should reach the same count of levels");
    // Pan is part of the ruler's identity, just as volume is.
    check(centered != hardRight && !centered.compatibleWith(hardRight) &&
              hardRight == VelocityMap::resolve(&squareTone, 60, 82, 63) &&
              directSound == VelocityMap::resolve(&directSoundTone, 60, kFullVolume, 63),
          "maps under different pans must not be mistaken for each other");

    // The deferred-gesture bookkeeping behind every lane edit: it holds
    // identities and preview values, and hands back one all-or-nothing batch
    // plus the revision the gesture began at.
    const NoteId first(1);
    const NoteId second(2);
    const NoteId absent(3);
    VelocityGestureModel gesture;
    check(!gesture.active() && !gesture.update({{first, 60}}) && !gesture.updateByDelta(4) &&
              !gesture.previewVelocity(first) && !gesture.takeCompletion() && !gesture.cancel(),
          "an inactive gesture must reject updates, previews, completion, and cancellation");
    check(!gesture.begin(7, {}) && !gesture.begin(7, {{NoteId(), 60}}) &&
              !gesture.begin(7, {{first, 0}}) && !gesture.begin(7, {{first, 60}, {first, 61}}) &&
              !gesture.active(),
          "empty, unassigned, out-of-range, or duplicated targets must not open a gesture");
    check(gesture.begin(7, {{second, 100}, {first, 40}}) && gesture.active() &&
              gesture.previewVelocity(first) == std::optional<uint8_t>(40) &&
              gesture.previewVelocity(second) == std::optional<uint8_t>(100) &&
              !gesture.previewVelocity(absent) && !gesture.previewVelocity(NoteId()),
          "a gesture must capture its targets whatever order they arrive in");
    check(!gesture.begin(9, {{first, 50}}) && gesture.previewVelocity(first) == 40,
          "a live gesture must not be replaced by another one");
    check(!gesture.update({{absent, 50}}) && !gesture.update({{first, 50}, {first, 51}}) &&
              !gesture.update({}) && gesture.previewVelocity(first) == 40,
          "an update naming an unknown or repeated note must change nothing at all");
    check(gesture.update({{first, 200}, {second, -5}}) && gesture.previewVelocity(first) == 127 &&
              gesture.previewVelocity(second) == 1,
          "updates must clamp into the MIDI velocity domain");
    check(gesture.updateByDelta(-20) && gesture.previewVelocity(first) == 20 &&
              gesture.previewVelocity(second) == 80 && gesture.updateByDelta(0) &&
              gesture.previewVelocity(first) == 40 && gesture.previewVelocity(second) == 100,
          "relative updates must measure from each note's own origin, clamps included");
    const std::optional<VelocityGestureModel::Completion> completion = gesture.takeCompletion();
    check(completion && completion->expectedRevision == 7 && completion->targets.size() == 2 &&
              !gesture.active() && !gesture.previewVelocity(first),
          "taking the completion must hand over the batch and leave the model inactive");
    check(gesture.begin(11, {{first, 40}}) && gesture.cancel() && !gesture.active(),
          "cancelling must discard the session");

    if (failures != 0) {
        std::fprintf(stderr, "velmodelcheck: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("velmodelcheck: PASS (velocity model)\n");
    return 0;
}

// --velcheck <projectRoot> <song> [shot.png]: velocity-lane check. Drives the lane
// widget offscreen: the pane is hidden until the view.velocity_lane command
// (V, dispatched from the focused roll like M/S/B) opens it, the selected
// track's notes paint as nodes at (start tick, velocity) with a stem across
// their duration, the roll's selection rings its nodes and marks their
// velocities on the ruler, the ruler graduates more finely as the pane
// grows, and the lane pans and zooms the shared timeline camera (with the
// ruler column passing the wheel through instead of zooming).

namespace {

void sendLaneMouse(QWidget *widget, QEvent::Type type, QPointF position, Qt::MouseButton button,
                   Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QMouseEvent event(type, position, QPointF(widget->mapToGlobal(position.toPoint())), button,
                      buttons, modifiers);
    QCoreApplication::sendEvent(widget, &event);
}

void sendLaneKey(QWidget *widget, int key)
{
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QCoreApplication::sendEvent(widget, &press);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
    QCoreApplication::sendEvent(widget, &release);
}

void sendLaneWheel(QWidget *widget, QPointF position, int angleDeltaY,
                   Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QWheelEvent event(position, widget->mapToGlobal(position), QPoint(), QPoint(0, angleDeltaY),
                      Qt::NoButton, modifiers, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(widget, &event);
}

// The stem's ink: the track color a third of the way to black in OKLab,
// stated here independently of the paint path's own helper.
QColor stemInk(const QColor &trackColor)
{
    const themes::Oklab from = themes::oklabFromColor(trackColor);
    const themes::Oklab to = themes::oklabFromColor(QColor(Qt::black));
    return themes::colorFromOklab({from.lightness + (to.lightness - from.lightness) / 3.0,
                                   from.a + (to.a - from.a) / 3.0, from.b + (to.b - from.b) / 3.0});
}

// Whether any pixel within radius of center is within tolerance of expected.
bool hasColorNear(const QImage &image, QPointF center, int radius, const QColor &expected,
                  int tolerance)
{
    const int left = std::max(0, int(std::floor(center.x())) - radius);
    const int right = std::min(image.width() - 1, int(std::ceil(center.x())) + radius);
    const int top = std::max(0, int(std::floor(center.y())) - radius);
    const int bottom = std::min(image.height() - 1, int(std::ceil(center.y())) + radius);
    for (int y = top; y <= bottom; y++) {
        for (int x = left; x <= right; x++) {
            const QColor actual = image.pixelColor(x, y);
            if (std::abs(actual.red() - expected.red()) <= tolerance &&
                std::abs(actual.green() - expected.green()) <= tolerance &&
                std::abs(actual.blue() - expected.blue()) <= tolerance) {
                return true;
            }
        }
    }
    return false;
}

// The longest unbroken horizontal run of a color along one row of the lane.
// A level line spans its whole section, so its row carries a long run broken
// only where nodes and stems cross it, while a row it does not touch carries
// at most the stray pixel or two where some other paint lands near the ink.
int longestColorRun(const QImage &image, double y, double fromX, double toX, const QColor &expected,
                    int tolerance)
{
    int longest = 0;
    int run = 0;
    for (int x = int(fromX); x <= int(toX); x++) {
        run = hasColorNear(image, QPointF(x, y), 0, expected, tolerance) ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    return longest;
}

// The lane's ruler mapping, stated independently of VelocityAxis: velocity 1
// sits on the bottom inset, 127 on the top one, linear in between.
double laneVelocityY(const QWidget *lane, int velocity)
{
    const double top = lane->property("velocityAxisTop").toDouble();
    const double bottom = lane->property("velocityAxisBottom").toDouble();
    return bottom - double(std::clamp(velocity, 1, 127) - 1) * (bottom - top) / 126.0;
}

// How near a node's center the lane lets a press land, in DIPs (its own
// kVelNodeGrabRadius): a probe that must miss has to aim further than this.
constexpr double kVelNodeGrabReach = 6.0;

// The y a PSG level's row centers on, stated independently of VelocityAxis:
// the boundaries sit halfway between the adjacent levels' own velocities,
// and the outermost levels run to the ruler's ends.
double laneLevelBoundaryY(const QWidget *lane, const VelocityMap &map, int lowerLevel)
{
    return (laneVelocityY(lane, map.levelRange(lowerLevel).last) +
            laneVelocityY(lane, map.levelRange(lowerLevel + 1).first)) /
           2.0;
}

double laneLevelCenterY(const QWidget *lane, const VelocityMap &map, int level)
{
    const double bottom = lane->property("velocityAxisBottom").toDouble();
    const double top = lane->property("velocityAxisTop").toDouble();
    const double lower = level == 0 ? bottom : laneLevelBoundaryY(lane, map, level - 1);
    const double upper =
        level + 1 == int(map.levelCount()) ? top : laneLevelBoundaryY(lane, map, level);
    return (lower + upper) / 2.0;
}

// Its inverse — the velocity a pointer y asks for, which a gesture writes.
int laneVelocityAt(const QWidget *lane, double y)
{
    const double top = lane->property("velocityAxisTop").toDouble();
    const double bottom = lane->property("velocityAxisBottom").toDouble();
    if (bottom <= top)
        return 1;
    const double clamped = std::clamp(y, top, bottom);
    return std::clamp(1 + int(std::lround((bottom - clamped) * 126.0 / (bottom - top))), 1, 127);
}

} // namespace

// Every printed ruler value must sit on a real graduation: the labels are a
// subset of the ticks in each density band, and neither run may overrun its
// fixed array.
int runVelocityAxisBandCheck(const std::function<void(bool, const char *)> &check)
{
    VelocityAxisGeometry geometry;
    geometry.verticalInset = 6.0;
    geometry.labelHeight = 12.0;
    geometry.densityD1 = 84.0;
    geometry.densityD2 = 112.0;
    geometry.densityD3 = 156.0;
    geometry.densityD4 = 300.0;
    bool everyLabelOnATick = true;
    bool countsInRange = true;
    std::size_t widestTicks = 0;
    for (const double height :
         {40.0, 83.0, 84.0, 111.0, 112.0, 155.0, 156.0, 299.0, 300.0, 900.0}) {
        geometry.height = height;
        const VelocityAxis axis(geometry);
        countsInRange = countsInRange && axis.tickCount() <= VelocityAxis::MaximumTicks &&
                        axis.labelCount() <= VelocityAxis::MaximumLabels;
        widestTicks = std::max(widestTicks, axis.tickCount());
        for (std::size_t label = 0; label < axis.labelCount(); label++) {
            bool onTick = false;
            for (std::size_t tick = 0; tick < axis.tickCount(); tick++) {
                if (axis.ticks()[tick].velocity == axis.labels()[label].velocity)
                    onTick = true;
            }
            everyLabelOnATick = everyLabelOnATick && onTick;
        }
    }
    // The ruler's click targets are exactly the values it prints. In the
    // densest band the labels step by 8, so at this height 64 is printed and
    // 68 is only a graduation; a selection marker at 62 covers the 64 label
    // and takes its row.
    geometry.height = 400.0;
    const VelocityAxis plain(geometry);
    const uint8_t marked62 = 62;
    const VelocityAxis marked(geometry, &marked62, 1);
    check(plain.rulerVelocityAt(plain.velocityToY(64), geometry.labelHeight) == 64 &&
              plain.rulerVelocityAt(plain.velocityToY(68), geometry.labelHeight) == -1,
          "only printed ruler values may be click targets");
    check(marked.rulerVelocityAt(marked.velocityToY(62), geometry.labelHeight) == 62 &&
              marked.rulerVelocityAt(marked.velocityToY(64), geometry.labelHeight) == -1,
          "a marker must own its row and take the label it covers out of reach");

    // The intrinsic ruler: one row per level of the voice, every row a click
    // target, and the rows in the order the velocities are. Both level
    // tables are probed — the pulse channels' 16 steps and the wave
    // channel's 5 — since the ruler is built from whichever one the voice
    // has.
    geometry.height = 400.0;
    ToneData squareTone{};
    squareTone.type = VOICE_SQUARE_1;
    ToneData waveTone{};
    waveTone.type = VOICE_PROGRAMMABLE_WAVE;
    const std::array<VelocityMap, 2> voiceProbes = {
        VelocityMap::resolve(&squareTone, 60, kFullVolume),
        VelocityMap::resolve(&waveTone, 60, kFullVolume)};
    const VelocityAxis continuous(VelocityMap::resolve(nullptr, std::nullopt, kFullVolume),
                                  geometry);
    check(continuous.mode() == VelocityAxis::Mode::Continuous && continuous.graduationCount() == 0,
          "a voice with no levels must leave the ruler continuous");
    bool rowsOrdered = true;
    bool rowsClickable = true;
    bool rowsRoundTrip = true;
    for (const VelocityMap &voice : voiceProbes) {
        const VelocityAxis axis(voice, geometry);
        rowsOrdered = rowsOrdered && axis.mode() == VelocityAxis::Mode::Intrinsic &&
                      axis.graduationCount() == voice.levelCount();
        for (std::size_t level = 0; level + 1 < axis.graduationCount(); level++) {
            // Higher levels are higher up the ruler, and each row's own
            // velocity really belongs to it.
            rowsOrdered =
                rowsOrdered && axis.graduations()[level + 1].y < axis.graduations()[level].y;
            rowsRoundTrip = rowsRoundTrip && axis.yToLevel(axis.levelToY(int(level))) == int(level);
            const double boundary = axis.levelBoundaryToY(int(level));
            rowsOrdered = rowsOrdered && boundary < axis.graduations()[level].y &&
                          boundary > axis.graduations()[level + 1].y;
        }
        for (std::size_t level = 0; level < axis.graduationCount(); level++) {
            // Anywhere in the row asks for that level's value — there is no
            // gap between two rows to miss.
            const double y = axis.levelToY(int(level));
            const uint8_t expected = voice.representative(int(level));
            rowsClickable = rowsClickable &&
                            axis.rulerVelocityAt(y, geometry.labelHeight) == expected &&
                            axis.rulerVelocityAt(y + 1.0, geometry.labelHeight) == expected;
        }
    }
    check(rowsOrdered, "the intrinsic rows must run bottom-up with a boundary between each pair");
    check(rowsRoundTrip, "an intrinsic row's own y must resolve back to its level");
    check(rowsClickable, "every y inside an intrinsic row must ask for that level's value");
    // A lane too short to name every row keeps all of them graduated and
    // drops labels instead.
    geometry.height = 90.0;
    const VelocityAxis crowded(voiceProbes[0], geometry);
    std::size_t labeledRows = 0;
    for (std::size_t index = 0; index < crowded.graduationCount(); index++)
        labeledRows += crowded.graduations()[index].labelVisible ? 1 : 0;
    check(crowded.graduationCount() == voiceProbes[0].levelCount() &&
              labeledRows < crowded.graduationCount(),
          "a short lane must keep every intrinsic row and thin out their labels");

    check(everyLabelOnATick, "every ruler label must land on a graduation");
    check(countsInRange && widestTicks == VelocityAxis::MaximumTicks,
          "the densest band must fill the tick array without overrunning it");
    return 0;
}

int runVelocityLaneCheck(const QString &projectRoot, const QString &songLabel,
                         const QString &screenshotPath)
{
    // The lane's V toggle goes through keymap::Registry, so redirect QSettings
    // into a temp dir first: a user's rebind must not reach these assertions.
    QTemporaryDir settingsDir;
    if (settingsDir.isValid()) {
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QSettings::setDefaultFormat(QSettings::IniFormat);
    }

    DecompProject project;
    QString error;
    if (!project.open(projectRoot, &error)) {
        std::fprintf(stderr, "velcheck: %s\n", qUtf8Printable(error));
        return 1;
    }
    const SongInfo *info = nullptr;
    for (const SongInfo &song : project.songs()) {
        if (song.label == songLabel && song.isPlayable())
            info = &song;
    }
    if (!info) {
        std::fprintf(stderr, "velcheck: no playable song %s\n", qUtf8Printable(songLabel));
        return 1;
    }
    SongDocument doc;
    if (!doc.load(*info, &error)) {
        std::fprintf(stderr, "velcheck: %s\n", qUtf8Printable(error));
        return 1;
    }
    auto timeline = doc.buildTimeline(48000.0);
    SongView view;
    view.resize(1280, 800);
    view.setSong(timeline.get(), nullptr);
    view.setDocument(&doc);
    // The app rebuilds the timeline after every edit
    // (MainWindow::onDocumentChanged); the lane hit-tests against the view
    // model, so the check must keep it fresh the same way.
    QObject::connect(&doc, &SongDocument::documentChanged, &view, [&] {
        auto rebuilt = doc.buildTimeline(48000.0);
        view.updateSong(rebuilt.get());
        timeline = std::move(rebuilt); // frees the old one after the swap
    });
    view.setGridMinDenom(4); // a comfortable 32px grid, as in rollcheck
    (void)view.grab();       // force layout so child geometry is real

    int failures = 0;
    const auto fail = [&](const char *what) {
        std::fprintf(stderr, "velcheck: FAIL %s: %s\n", qUtf8Printable(songLabel), what);
        failures++;
    };
    const auto check = [&](bool condition, const char *what) {
        if (!condition)
            fail(what);
    };

    auto *roll = view.findChild<QWidget *>(QStringLiteral("pianoRoll"));
    auto *lane = view.findChild<QWidget *>(QStringLiteral("velocityLane"));
    if (!roll || !lane) {
        fail("piano roll or velocity lane not found");
        return 1;
    }

    // --- the pane and its toggle
    int visibilitySignals = 0;
    bool lastVisibility = false;
    QObject::connect(&view, &SongView::velocityLaneVisibilityChanged, &view, [&](bool on) {
        visibilitySignals++;
        lastVisibility = on;
    });
    // The harness never shows the window, so isHidden() — the explicit hide
    // flag — is the honest question here, not isVisible().
    check(!view.velocityLaneVisible() && lane->isHidden(), "the velocity lane must start hidden");
    sendLaneKey(roll, Qt::Key_V);
    (void)view.grab();
    check(view.velocityLaneVisible() && !lane->isHidden() && visibilitySignals == 1 &&
              lastVisibility,
          "V from the roll must open the lane and report it");
    check(lane->height() > lane->minimumHeight() && lane->width() > songview::kGutterW,
          "the opened lane must borrow a working height, not just its minimum");
    sendLaneKey(roll, Qt::Key_V);
    (void)view.grab();
    check(!view.velocityLaneVisible() && visibilitySignals == 2 && !lastVisibility,
          "V again must close the lane");
    sendLaneKey(lane, Qt::Key_V);
    (void)view.grab();
    check(view.velocityLaneVisible() && visibilitySignals == 3,
          "the lane's own focus must reach the same toggle");

    // --- the automation lanes' pane and the toggle bar
    // The bar rides in the "other events" strip's gutter, outside the
    // splitter, so both panes can close without it moving.
    auto *toggleBar = view.findChild<QWidget *>(QStringLiteral("laneToggleBar"));
    auto *automationToggle = view.findChild<QToolButton *>(QStringLiteral("automationLanesToggle"));
    auto *velocityToggle = view.findChild<QToolButton *>(QStringLiteral("velocityLaneToggle"));
    auto *lanes = view.findChild<QWidget *>(QStringLiteral("automationArea"));
    // The pane is the scroll area the splitter sizes; `lanes` inside it sizes
    // to its own rows, so only the pane can answer "did it reopen at a
    // working height".
    auto *lanesPane = view.findChild<QWidget *>(QStringLiteral("automationLanesPane"));
    if (!toggleBar || !automationToggle || !velocityToggle || !lanes || !lanesPane) {
        fail("lane toggle bar, its buttons, or the automation area not found");
        return 1;
    }
    int lanesSignals = 0;
    bool lastLanesVisibility = false;
    QObject::connect(&view, &SongView::automationLanesVisibilityChanged, &view, [&](bool on) {
        lanesSignals++;
        lastLanesVisibility = on;
    });
    check(view.automationLanesVisible() && automationToggle->isChecked(),
          "the automation lanes must start shown, with their toggle checked");
    // The bar's whole job is to stay put and stay reachable, so ask where it
    // is in the view's own coordinates, not its parent's.
    // Also the pane-size checks' splitter, further down.
    auto *splitter = view.findChild<QSplitter *>();
    auto *strip = view.findChild<QWidget *>(QStringLiteral("otherEventsStrip"));
    if (!splitter || !strip) {
        fail("the roll/lanes splitter or the other-events strip is missing");
        return 1;
    }
    const auto rectInView = [&](const QWidget *widget) {
        return QRect(widget->mapTo(&view, QPoint(0, 0)), widget->size());
    };
    // The bar leads the strip's row: inside the strip, inside the gutter so it
    // never covers the timeline, and below the splitter along with its host.
    // Ask in the view's own coordinates — the bar's own geometry is
    // parent-relative, so reparenting it would read as unmoved.
    const auto barLeadsTheStrip = [&] {
        if (toggleBar->isHidden() || toggleBar->width() <= 0)
            return false;
        const QRect bar = rectInView(toggleBar);
        const QRect stripRect = rectInView(strip);
        return stripRect.contains(bar) && bar.left() == stripRect.left() &&
               bar.right() < stripRect.left() + songview::kGutterW &&
               stripRect.top() >= rectInView(splitter).bottom();
    };
    check(barLeadsTheStrip(),
          "the toggle bar must lead the strip's row, inside the gutter and under the splitter");
    const int paneHeightOpen = lanesPane->height();
    // Hiding the pane is view-only, so the lane data has to come through it
    // untouched — count the rows and their points before and after.
    const auto lanePointTotal = [&] {
        size_t total = 0;
        for (const AutoLane &laneRow : view.model().lanes)
            total += laneRow.points.size();
        return total;
    };
    const size_t laneRowsOpen = view.model().lanes.size();
    const size_t lanePointsOpen = lanePointTotal();
    const int laneHeightOpen = view.viewState().laneHeight;
    sendLaneKey(roll, Qt::Key_A);
    (void)view.grab();
    check(!view.automationLanesVisible() && lanesSignals == 1 && !lastLanesVisibility &&
              !automationToggle->isChecked(),
          "A from the roll must close the lanes, report it, and uncheck the toggle");
    check(barLeadsTheStrip(), "the toggle bar must keep its place once the lanes close");
    check(view.model().lanes.size() == laneRowsOpen && lanePointTotal() == lanePointsOpen &&
              view.viewState().laneHeight == laneHeightOpen,
          "closing the pane must leave the lane rows, their points, and their height alone");
    sendLaneKey(roll, Qt::Key_A);
    (void)view.grab();
    check(view.automationLanesVisible() && lanesSignals == 2 && automationToggle->isChecked(),
          "A again must reopen the lanes");
    // Reopening a pane the splitter has already sized is the splitter's own
    // doing — it remembers the handle. The height borrow is for the other
    // path: the preference persists off, so the pane is hidden before the
    // first layout and has no remembered size at all. That needs its own
    // view, hidden the way MainWindow::createSession hides it.
    {
        SongView fresh;
        fresh.resize(1280, 800);
        fresh.setSong(timeline.get(), nullptr);
        fresh.setAutomationLanesVisible(false); // before any layout, as the app does
        (void)fresh.grab();
        auto *freshPane = fresh.findChild<QWidget *>(QStringLiteral("automationLanesPane"));
        if (!freshPane) {
            fail("the fresh view has no automation lanes pane");
        } else {
            check(freshPane->isHidden(), "a view built with the preference off must start closed");
            fresh.setAutomationLanesVisible(true);
            (void)fresh.grab();
            check(freshPane->height() > freshPane->minimumHeight(),
                  "a never-sized pane must borrow a working height, not open at its minimum");
        }
    }
    check(lanesPane->height() >= paneHeightOpen,
          "the reopened pane must come back at the height it had");

    // The buttons are children of the bar, so a click aimed at a surface
    // never reaches them; drive each one directly.
    const auto clickButton = [](QToolButton *button) {
        const QPointF center(button->rect().center());
        sendLaneMouse(button, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
        sendLaneMouse(button, QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton);
    };
    clickButton(automationToggle);
    (void)view.grab();
    check(!view.automationLanesVisible() && lanesSignals == 3,
          "the automation toggle button must close the lanes");
    clickButton(automationToggle);
    (void)view.grab();
    check(view.automationLanesVisible() && lanesSignals == 4,
          "the automation toggle button must reopen the lanes");
    const int velocitySignalsBeforeClick = visibilitySignals;
    const bool velocityOpenBeforeClick = view.velocityLaneVisible();
    clickButton(velocityToggle);
    (void)view.grab();
    check(view.velocityLaneVisible() == !velocityOpenBeforeClick &&
              visibilitySignals == velocitySignalsBeforeClick + 1,
          "the velocity toggle button must flip the velocity pane");
    // The keyboard and the buttons are two faces of one state, so a key press
    // has to leave the button showing the truth.
    sendLaneKey(roll, Qt::Key_V);
    (void)view.grab();
    check(velocityToggle->isChecked() == view.velocityLaneVisible() &&
              view.velocityLaneVisible() == velocityOpenBeforeClick,
          "V must flip the pane back and re-sync the toggle button");

    // Closing the focused lanes must hand the keyboard back, or the next
    // bare-letter command would land outside the view entirely.
    lanes->setFocus(Qt::OtherFocusReason);
    sendLaneKey(lanes, Qt::Key_A);
    (void)view.grab();
    check(!view.automationLanesVisible() && view.focusWidget() == roll,
          "closing the focused lanes must hand the keyboard back to the roll");
    // The extreme case the bar exists for: with both panes shut there is no
    // lanes chrome left, so the bar is the only way back without the menu.
    const bool velocityOpenBeforeShut = view.velocityLaneVisible();
    view.setVelocityLaneVisible(false);
    (void)view.grab();
    check(barLeadsTheStrip() && !automationToggle->isChecked() && !velocityToggle->isChecked(),
          "with both panes shut the bar must still be there, showing both toggles off");
    sendLaneKey(roll, Qt::Key_A);
    view.setVelocityLaneVisible(velocityOpenBeforeShut);
    (void)view.grab();
    check(view.automationLanesVisible() && view.velocityLaneVisible() == velocityOpenBeforeShut,
          "both panes must go back as they were for the rest of the check");

    runVelocityAxisBandCheck(check);

    // The playhead overlay clips its line to the registered bands' visible
    // rects; without the lane among them it would break across the pane.
    const songview::TimelineSurfaces surfaces = view.timelineSurfaces();
    check(&surfaces.velocity.widget == lane &&
              surfaces.velocity.timelineOrigin == songview::kGutterW,
          "the lane must be registered as a timeline band at the shared origin");

    lane->setFocus(Qt::OtherFocusReason);
    sendLaneKey(lane, Qt::Key_V);
    (void)view.grab();
    // The harness never shows the window, so ask the view which child holds
    // the focus rather than the activation-sensitive hasFocus().
    check(!view.velocityLaneVisible() && view.focusWidget() == roll,
          "closing the focused lane must hand the keyboard back to the roll");
    sendLaneKey(roll, Qt::Key_V);
    (void)view.grab();

    // --- a press on a focus-less part of the view hands the keyboard back
    // The bare-letter shortcuts dispatch from the focused surface, so a press
    // on the header panel, the strip, or an M/S button (none of which takes
    // the focus itself) must bring the keyboard back from wherever it was —
    // a dock's search field, say — or the next A/V would go nowhere. With a
    // surface already focused, the same press must leave the focus alone.
    {
        auto *headers = view.findChild<QWidget *>(QStringLiteral("trackHeaderPanel"));
        auto *mute = view.findChild<QToolButton *>(QStringLiteral("trackMuteButton"));
        check(headers && mute, "the header panel and a mute button must be findable");
        // Delivered to the deepest child under the point, as a real press is.
        const auto pressAt = [&](QWidget *surface, QPoint pos) {
            QWidget *target = surface->childAt(pos);
            if (!target)
                target = surface;
            const QPointF local(target->mapFrom(surface, pos));
            sendLaneMouse(target, QEvent::MouseButtonPress, local, Qt::LeftButton, Qt::LeftButton);
            sendLaneMouse(target, QEvent::MouseButtonRelease, local, Qt::LeftButton, Qt::NoButton);
        };
        QLineEdit sink(&view); // stands in for a focused input elsewhere
        sink.setFocus(Qt::OtherFocusReason);
        check(view.focusWidget() == &sink, "the stand-in input must take the focus");
        if (headers) {
            pressAt(headers, QPoint(headers->width() / 2, headers->height() - 2));
            check(view.focusWidget() == roll,
                  "a press on the header panel must hand the focus to the roll");
            const bool lanesOpen = view.automationLanesVisible();
            sendLaneKey(view.focusWidget(), Qt::Key_A);
            check(view.automationLanesVisible() != lanesOpen,
                  "A must work right after a press on the header panel");
            if (view.automationLanesVisible() != lanesOpen)
                sendLaneKey(roll, Qt::Key_A); // back as it was, pass or fail
            (void)view.grab();
        }
        if (strip) {
            sink.setFocus(Qt::OtherFocusReason);
            pressAt(strip, QPoint(strip->width() / 2, strip->height() / 2));
            check(view.focusWidget() == roll,
                  "a press on the strip must hand the focus to the roll");
        }
        if (mute) {
            sink.setFocus(Qt::OtherFocusReason);
            pressAt(mute, mute->rect().center());
            pressAt(mute, mute->rect().center()); // and back off again
            check(view.focusWidget() == roll,
                  "a press on a header's mute button must hand the focus to the roll");
            // With the lanes focused, the same press keeps the keyboard on
            // the lanes: the buttons are TabFocus precisely so a click never
            // moves the shortcuts off a surface.
            lanes->setFocus(Qt::OtherFocusReason);
            pressAt(mute, mute->rect().center());
            pressAt(mute, mute->rect().center());
            check(view.focusWidget() == lanes,
                  "a press on a mute button with the lanes focused must leave them focused");
        }
        // A press on a surface is Qt's to focus (ClickFocus, on the real,
        // spontaneous press this harness cannot fake): the view must not
        // second-guess it by dragging the focus to the roll.
        sink.setFocus(Qt::OtherFocusReason);
        pressAt(lane, QPoint(lane->width() / 2, lane->height() / 2));
        check(view.focusWidget() != roll,
              "a press on the velocity lane must not hand the focus to the roll");
        roll->setFocus(Qt::OtherFocusReason);
        (void)view.grab();
    }

    // --- node and stem placement
    const int track = view.selectedTrack();
    const qreal dpr = lane->devicePixelRatioF();
    // A note whose node stands alone: far enough into the plot to be clear of
    // the ruler column, away from the overlays' verticals, and with no other
    // node of the track within a ring's reach.
    // The overlays paint verticals over the nodes (edit cursor, loop markers,
    // playhead), and their ink is close enough to the selection color to fool
    // a pixel probe; keep the probed node clear of all of them.
    const auto overlayContested = [&](qreal x) {
        const qreal cursorX = view.displayX(double(view.editCursorTick()), songview::kGutterW, dpr);
        if (std::abs(x - cursorX) < 16)
            return true;
        if (std::abs(x - view.displayX(view.playheadTick(), songview::kGutterW, dpr)) < 16)
            return true;
        for (const uint64_t loopTick : {timeline->loopStartTick, timeline->loopEndTick}) {
            if (loopTick == UINT64_MAX)
                continue;
            if (std::abs(x - view.displayX(double(loopTick), songview::kGutterW, dpr)) < 52)
                return true;
        }
        return false;
    };
    const ViewNote *probe = nullptr;
    for (const ViewNote &note : view.model().notes) {
        if (note.track != track || note.velocity < 8 || note.velocity > 120)
            continue;
        const qreal x = view.displayX(double(note.startTick), songview::kGutterW, dpr);
        if (x < songview::kGutterW + 60 || x > lane->width() - 60 || overlayContested(x))
            continue;
        bool crowded = false;
        for (const ViewNote &other : view.model().notes) {
            if (&other == &note || other.track != track)
                continue;
            const qreal otherX = view.displayX(double(other.startTick), songview::kGutterW, dpr);
            if (std::abs(otherX - x) < 16)
                crowded = true;
        }
        if (!crowded)
            probe = &note;
        if (probe)
            break;
    }
    if (!probe) {
        fail("no isolated note to probe in the visible span");
        return failures == 0 ? 0 : 1;
    }
    const QPixmap lanePixmap = lane->grab();
    const QImage laneImage = lanePixmap.toImage();
    const qreal rasterDpr = lanePixmap.devicePixelRatio();
    const QColor trackColor = SongView::trackColor(track);
    const QPointF nodeCenter(view.displayX(double(probe->startTick), songview::kGutterW, dpr) *
                                 rasterDpr,
                             laneVelocityY(lane, probe->velocity) * rasterDpr);
    check(hasColorNear(laneImage, nodeCenter, int(std::ceil(2 * rasterDpr)), trackColor, 24),
          "a node must paint in the track color at its tick and velocity");
    // The same x at another velocity's row carries no node: placement is the
    // velocity's, not just "somewhere in the lane".
    const int otherVelocity = probe->velocity > 64 ? probe->velocity - 40 : probe->velocity + 40;
    const QPointF elsewhere(nodeCenter.x(), laneVelocityY(lane, otherVelocity) * rasterDpr);
    check(!hasColorNear(laneImage, elsewhere, int(std::ceil(2 * rasterDpr)), trackColor, 24),
          "no node may paint at a velocity the note does not have");
    // The stem spans the note's duration at the node's height.
    const qreal stemEndX =
        view.displayX(double(probe->endTick), songview::kGutterW, dpr) * rasterDpr;
    if (stemEndX - nodeCenter.x() > 8 * rasterDpr) {
        const QPointF stemMid((nodeCenter.x() + stemEndX) / 2.0, nodeCenter.y());
        const QColor stemColor = stemInk(trackColor);
        check(hasColorNear(laneImage, stemMid, int(std::ceil(2 * rasterDpr)), stemColor, 28),
              "a stem must run from the node across the note's duration");
    }

    // The stem's weight is authored in DIPs, so its painted thickness must
    // grow with the display scale rather than staying a fixed pixel count.
    if (stemEndX - nodeCenter.x() > 8 * rasterDpr) {
        const QColor stemColor = stemInk(trackColor);
        int thickness = 0;
        for (int dy = -6; dy <= 6; dy++) {
            const QColor actual = laneImage.pixelColor(int((nodeCenter.x() + stemEndX) / 2.0),
                                                       int(nodeCenter.y()) + dy);
            if (std::abs(actual.red() - stemColor.red()) <= 28 &&
                std::abs(actual.green() - stemColor.green()) <= 28 &&
                std::abs(actual.blue() - stemColor.blue()) <= 28) {
                thickness++;
            }
        }
        check(thickness >= int(std::floor(1.5 * rasterDpr)),
              "the stem's weight must scale with the display, not stay a device pixel");
    }

    // --- View → Color Notes by Velocity reaches the lane's nodes
    {
        view.setVelocityColorMode(true);
        (void)view.grab();
        const QImage rampImage = lane->grab().toImage();
        const int probeRadius = int(std::ceil(2 * rasterDpr));
        check(hasColorNear(rampImage, nodeCenter, probeRadius,
                           SongView::velocityNoteColor(probe->velocity), 24),
              "velocity colors must fill a node with the roll's ramp for its velocity");
        // The stem keeps the track's ink: darkening the ramp's low end would
        // sink it into a dark theme's background.
        if (stemEndX - nodeCenter.x() > 8 * rasterDpr) {
            const QPointF stemMid((nodeCenter.x() + stemEndX) / 2.0, nodeCenter.y());
            check(hasColorNear(rampImage, stemMid, probeRadius, stemInk(trackColor), 28),
                  "velocity colors must leave the stem on the track's ink");
        }
        view.setVelocityColorMode(false);
        (void)view.grab();
        check(hasColorNear(lane->grab().toImage(), nodeCenter, probeRadius, trackColor, 24),
              "turning velocity colors off must return the node to the track color");
    }

    // --- the roll's selection rings its nodes and marks the ruler
    check(lane->property("velocityMarkerCount").toInt() == 0,
          "an empty selection must leave the ruler unmarked");
    view.setSelection({{probe->startTick, probe->key}});
    (void)view.grab();
    const QImage selectedImage = lane->grab().toImage();
    const QColor ringColor = lane->palette().highlight().color();
    // Probe above the node, not across it: a selected note's stem carries the
    // same color through the center, so only the ring can put it up here.
    const QPointF ringTop(nodeCenter.x(), nodeCenter.y() - 5.0 * rasterDpr);
    const int ringProbe = int(std::ceil(2 * rasterDpr));
    check(!hasColorNear(laneImage, ringTop, ringProbe, ringColor, 24) &&
              hasColorNear(selectedImage, ringTop, ringProbe, ringColor, 24),
          "a selected note's node must gain a ring in the selection color");
    check(lane->property("velocityMarkerCount").toInt() == 1,
          "one selected velocity must mark the ruler once");
    const ViewNote *second = nullptr;
    for (const ViewNote &note : view.model().notes) {
        if (note.track == track && note.velocity != probe->velocity)
            second = &note;
        if (second)
            break;
    }
    if (second) {
        view.setSelection({{probe->startTick, probe->key}, {second->startTick, second->key}});
        (void)view.grab();
        check(lane->property("velocityMarkerCount").toInt() == 2,
              "two selected velocities must mark the ruler's extremes");
    }
    if (second) {
        // Dimming reads the selection, not what happens to be on screen.
        check(lane->property("velocityDimmed").toBool(),
              "two selected notes must dim the rest of the track");
        const double before = view.tickAtContentX(0);
        view.scrollByPx(4000.0);
        (void)view.grab();
        check(view.tickAtContentX(0) != before && lane->property("velocityDimmed").toBool(),
              "scrolling away from the selection must not undim the lane");
        view.scrollByPx(-4000.0);
        (void)view.grab();
    }

    // A selected velocity's marker prints its own value in the label column;
    // any fixed ruler label that close must yield instead of overprinting.
    const QColor labelColor = themes::color(themes::Role::song_view_secondary_text);
    const int labelHeight = QFontMetrics(typography::caption(lane->font())).height();
    // Text only: the ruler's tick marks are drawn in the same ink and reach
    // into the right edge of this column.
    const int labelInset = layout::space(layout::Space::Two);
    const QRect labelColumn(songview::kHeaderW, 0, songview::kKeyboardW - 2 * labelInset,
                            lane->height());
    const auto labelInkAt = [&](const QImage &image, double y) {
        const int top = int((y - labelHeight / 2.0) * rasterDpr);
        const int bottom = int((y + labelHeight / 2.0) * rasterDpr);
        for (int py = std::max(0, top); py <= std::min(image.height() - 1, bottom); py++) {
            for (int px = int(labelColumn.left() * rasterDpr);
                 px <= std::min(image.width() - 1, int(labelColumn.right() * rasterDpr)); px++) {
                const QColor actual = image.pixelColor(px, py);
                if (std::abs(actual.red() - labelColor.red()) <= 12 &&
                    std::abs(actual.green() - labelColor.green()) <= 12 &&
                    std::abs(actual.blue() - labelColor.blue()) <= 12) {
                    return true;
                }
            }
        }
        return false;
    };
    view.clearSelection();
    (void)view.grab();
    const QImage unselectedRuler = lane->grab().toImage();
    const ViewNote *onLabel = nullptr;
    for (const ViewNote &note : view.model().notes) {
        if (note.track == track && labelInkAt(unselectedRuler, laneVelocityY(lane, note.velocity)))
            onLabel = &note;
        if (onLabel)
            break;
    }
    if (onLabel) {
        view.setSelection({{onLabel->startTick, onLabel->key}});
        (void)view.grab();
        check(!labelInkAt(lane->grab().toImage(), laneVelocityY(lane, onLabel->velocity)),
              "a fixed ruler label must yield to the selection's own marker label");
    }
    view.clearSelection();
    (void)view.grab();

    // --- ruler density follows the pane's height
    const int shortTicks = lane->property("velocityTickCount").toInt();
    const int shortHeight = lane->height();
    if (!splitter) {
        fail("the roll/lanes splitter is missing");
        return failures == 0 ? 0 : 1;
    }
    QList<int> sizes = splitter->sizes();
    sizes[0] -= 220;
    sizes[1] += 220;
    splitter->setSizes(sizes);
    (void)view.grab();
    check(lane->height() > shortHeight && lane->property("velocityTickCount").toInt() > shortTicks,
          "a taller lane must graduate its ruler more finely");

    // --- camera: the plot zooms at the cursor, the ruler column does not
    const double beforeZoom = view.pxPerBeat();
    const QPointF overRuler(songview::kGutterW / 2.0, lane->height() / 2.0);
    sendLaneWheel(lane, overRuler, 120);
    check(view.pxPerBeat() == beforeZoom, "the wheel over the ruler column must not zoom");
    const QPointF overPlot(songview::kGutterW + 200.0, lane->height() / 2.0);
    const double anchoredTick = view.tickAtContentX(overPlot.x() - songview::kGutterW);
    sendLaneWheel(lane, overPlot, 120);
    check(view.pxPerBeat() > beforeZoom, "the wheel over the plot must zoom the timeline in");
    check(std::abs(view.tickAtContentX(overPlot.x() - songview::kGutterW) - anchoredTick) < 0.5,
          "zooming must keep the tick under the cursor in place");
    const double beforeScroll = view.tickAtContentX(0);
    const double zoomedPxPerBeat = view.pxPerBeat();
    sendLaneWheel(lane, overPlot, 120, Qt::ShiftModifier);
    check(view.pxPerBeat() == zoomedPxPerBeat && view.tickAtContentX(0) != beforeScroll,
          "Shift+wheel must scroll instead of zoom");

    // --- middle-button pan
    const double beforePan = view.tickAtContentX(0);
    sendLaneMouse(lane, QEvent::MouseButtonPress, overPlot, Qt::MiddleButton, Qt::MiddleButton);
    sendLaneMouse(lane, QEvent::MouseMove, overPlot - QPointF(40.0, 0.0), Qt::NoButton,
                  Qt::MiddleButton);
    sendLaneMouse(lane, QEvent::MouseButtonRelease, overPlot - QPointF(40.0, 0.0), Qt::MiddleButton,
                  Qt::NoButton);
    check(view.tickAtContentX(0) > beforePan, "a middle drag must pan the timeline");

    // --- editing: a node drag moves the whole selection, deferred to the release
    // The camera checks above left the view zoomed in and scrolled away from
    // the notes; put it back where the placement probes were taken.
    view.zoomAroundContentX(beforeZoom / view.pxPerBeat(), 0.0);
    view.scrollByPx(-1e6);
    (void)view.grab();
    const auto noteAt = [&](uint32_t startTick, uint8_t key) -> const ViewNote * {
        for (const ViewNote &note : view.model().notes) {
            if (note.track == track && note.startTick == startTick && note.key == key)
                return &note;
        }
        return nullptr;
    };
    // A note whose node can be pressed without ambiguity under the camera as
    // it stands now: clear of the ruler, the overlays, and any neighbor's
    // node, with room either side of its velocity for a drag that never
    // clamps.
    const auto isolated = [&](const ViewNote *avoid, bool farthest) -> const ViewNote * {
        const ViewNote *found = nullptr;
        for (const ViewNote &note : view.model().notes) {
            if (note.track != track || !note.noteId.isAssigned())
                continue;
            if (note.velocity < 25 || note.velocity > 103)
                continue;
            const qreal x = view.displayX(double(note.startTick), songview::kGutterW, dpr);
            if (x < songview::kGutterW + 60 || x > lane->width() - 60 || overlayContested(x))
                continue;
            if (avoid && std::abs(view.displayX(double(avoid->startTick), songview::kGutterW, dpr) -
                                  x) < 24) {
                continue;
            }
            bool crowded = false;
            for (const ViewNote &other : view.model().notes) {
                if (&other == &note || other.track != track)
                    continue;
                const qreal otherX =
                    view.displayX(double(other.startTick), songview::kGutterW, dpr);
                if (std::abs(otherX - x) < 16)
                    crowded = true;
            }
            if (!crowded) {
                found = &note;
                if (!farthest)
                    break;
            }
        }
        return found;
    };
    // The partner is the farthest such note, so the paint stroke between the
    // two spans other notes it must not touch.
    const ViewNote *dragProbe = isolated(nullptr, false);
    const ViewNote *partnerProbe = dragProbe ? isolated(dragProbe, true) : nullptr;
    if (!dragProbe || !partnerProbe) {
        fail("no pair of drag-safe notes in the visible span");
        return failures == 0 ? 0 : 1;
    }
    const ViewNote dragNote = *dragProbe;
    const ViewNote partner = *partnerProbe;
    const SongView::NoteKey dragId{dragNote.startTick, dragNote.key};
    const SongView::NoteKey partnerId{partner.startTick, partner.key};
    const int ringProbeRadius = int(std::ceil(2 * rasterDpr));
    // The status line the gestures write; the readout and the wording of each
    // commit are part of what the lane promises.
    QString lastStatus;
    QObject::connect(&view, &SongView::statusMessage, &view,
                     [&lastStatus](const QString &text) { lastStatus = text; });
    const QString cancelText = QStringLiteral("Velocity edit cancelled because notes changed.");
    // The travel that turns a press into a drag, resolved the way the lane
    // resolves it.
    const double dragActivation = layout::fontPx(5.0 / 12.0);
    view.setSelection({dragId, partnerId});
    (void)view.grab();
    const QPointF nodePoint(view.displayX(double(dragNote.startTick), songview::kGutterW, dpr),
                            laneVelocityY(lane, dragNote.velocity));
    // The drag's y is the y of a real velocity, so the delta it asks for is
    // exactly this one whatever the pane's height and display scale are. It
    // heads away from the note's own end of the ruler, so nothing clamps.
    const int dragDelta = dragNote.velocity > 64 ? -20 : 20;
    const QPointF partnerPoint(view.displayX(double(partner.startTick), songview::kGutterW, dpr),
                               laneVelocityY(lane, partner.velocity));
    const QPointF dragPoint(nodePoint.x(), laneVelocityY(lane, dragNote.velocity + dragDelta));
    const uint64_t revisionBeforeDrag = doc.revision();
    // The undo INDEX, not the stack's size: these probes undo as they go, and
    // the next commit discards the redo branch instead of growing the stack.
    const int undoBeforeDrag = doc.undoStack()->index();
    sendLaneMouse(lane, QEvent::MouseButtonPress, nodePoint, Qt::LeftButton, Qt::LeftButton);
    sendLaneMouse(lane, QEvent::MouseMove, dragPoint, Qt::NoButton, Qt::LeftButton);
    const QImage draggingImage = lane->grab().toImage();
    check(doc.revision() == revisionBeforeDrag && doc.undoStack()->index() == undoBeforeDrag,
          "a velocity drag must leave the document alone until the release");
    // The ring rides the preview: probe above the node, where only the ring
    // can put the selection color.
    check(hasColorNear(draggingImage,
                       QPointF(dragPoint.x() * rasterDpr, (dragPoint.y() - 5.0) * rasterDpr),
                       ringProbeRadius, ringColor, 24),
          "a velocity drag must preview its node at the pointer's velocity");
    check(lastStatus.contains(QStringLiteral("velocity")) &&
              lastStatus.contains(QStringLiteral("plays")),
          "a live velocity gesture must read its aimed note out to the status bar");
    // The camera must hold still under a live edit: the gesture's press was
    // measured in ticks that panning would move.
    const double tickBeforePanAttempt = view.tickAtContentX(0);
    sendLaneMouse(lane, QEvent::MouseButtonPress, dragPoint, Qt::MiddleButton,
                  Qt::MiddleButton | Qt::LeftButton);
    sendLaneMouse(lane, QEvent::MouseMove, dragPoint - QPointF(60.0, 0.0), Qt::NoButton,
                  Qt::MiddleButton | Qt::LeftButton);
    sendLaneMouse(lane, QEvent::MouseButtonRelease, dragPoint - QPointF(60.0, 0.0),
                  Qt::MiddleButton, Qt::LeftButton);
    check(view.tickAtContentX(0) == tickBeforePanAttempt,
          "a middle drag must not pan while a velocity edit is live");
    sendLaneMouse(lane, QEvent::MouseButtonRelease, dragPoint, Qt::LeftButton, Qt::NoButton);
    (void)view.grab();
    check(lastStatus == QStringLiteral("Set note velocities."),
          "a committed velocity drag must say what it did");
    const ViewNote *draggedNote = noteAt(dragNote.startTick, dragNote.key);
    const ViewNote *draggedPartner = noteAt(partner.startTick, partner.key);
    check(doc.revision() != revisionBeforeDrag && doc.undoStack()->index() == undoBeforeDrag + 1 &&
              draggedNote && draggedPartner &&
              draggedNote->velocity == dragNote.velocity + dragDelta &&
              draggedPartner->velocity == partner.velocity + dragDelta &&
              view.selection().size() == 2,
          "the release must move every selected note by the drag's delta as one undo entry");
    doc.undoStack()->undo();
    (void)view.grab();

    // --- the edit is heard and seen: the gesture auditions its aimed note as
    // the roll's own velocity drag does, and the roll draws the note at the
    // preview while the lane holds it.
    struct AuditionEvent {
        int track;
        int key;
        int velocity;
        int rawVolume;
    };
    std::vector<AuditionEvent> auditions;
    QObject::connect(&view, &SongView::auditionNote, &view,
                     [&auditions](int track, int key, int velocity, int rawVolume) {
                         auditions.push_back({track, key, velocity, rawVolume});
                     });
    // Velocity hues make the roll's fill a statement about the value alone,
    // so the probe reads a color the preview velocity names outright.
    const bool velocityInkBefore = view.velocityColorMode();
    view.setVelocityColorMode(true);
    // The roll scrolls vertically on its own; bring the probed note's row to
    // the middle of the pane so its face is really painted.
    const double rollScrollBefore = view.scrollY();
    view.scrollRollBy((127 - dragNote.key) * view.keyHeight() - view.scrollY() -
                      roll->height() / 2.0);
    view.setSelection({dragId});
    (void)view.grab();
    const QPointF rollNoteCenter(
        (view.displayX(double(dragNote.startTick), songview::kKeyboardW, dpr) +
         view.displayX(double(dragNote.endTick), songview::kKeyboardW, dpr)) /
            2.0,
        (127 - dragNote.key) * view.keyHeight() - view.scrollY() + view.keyHeight() / 2.0);
    const int previewVelocity = dragNote.velocity + dragDelta;
    const QImage rollBefore = roll->grab().toImage();
    const qreal rollDpr = rollBefore.devicePixelRatio();
    const QPointF rollProbe(rollNoteCenter.x() * rollDpr, rollNoteCenter.y() * rollDpr);
    const int rollProbeRadius = int(std::ceil(2 * rollDpr));
    check(hasColorNear(rollBefore, rollProbe, rollProbeRadius,
                       SongView::velocityNoteColor(dragNote.velocity), 24),
          "the probed note must be drawn at its stored velocity's hue before the edit");
    auditions.clear();
    sendLaneMouse(lane, QEvent::MouseButtonPress, nodePoint, Qt::LeftButton, Qt::LeftButton);
    check(auditions.size() == 1 && auditions.back().track == track &&
              auditions.back().key == dragNote.key &&
              auditions.back().velocity == dragNote.velocity,
          "a press on a node must sound that note at its own velocity");
    sendLaneMouse(lane, QEvent::MouseMove, dragPoint, Qt::NoButton, Qt::LeftButton);
    check(auditions.size() == 2 && auditions.back().key == dragNote.key &&
              auditions.back().velocity == previewVelocity,
          "a drag must re-sound the note at the velocity the release would write");
    const QImage rollDuring = roll->grab().toImage();
    check(hasColorNear(rollDuring, rollProbe, rollProbeRadius,
                       SongView::velocityNoteColor(previewVelocity), 24),
          "the roll must draw the note at the lane's preview while the gesture holds it");
    check(!hasColorNear(rollDuring, rollProbe, rollProbeRadius,
                        SongView::velocityNoteColor(dragNote.velocity), 8),
          "the roll must not still show the stored velocity's hue under a live preview");
    // A move that asks for the same velocity again asks for the same sound:
    // the engine's preview slot restarts the note, so nothing may be sent.
    sendLaneMouse(lane, QEvent::MouseMove, dragPoint + QPointF(3.0, 0.0), Qt::NoButton,
                  Qt::LeftButton);
    check(auditions.size() == 2, "a drag must not re-sound a velocity it is already sounding");
    sendLaneMouse(lane, QEvent::MouseButtonRelease, dragPoint + QPointF(3.0, 0.0), Qt::LeftButton,
                  Qt::NoButton);
    check(auditions.size() == 3 && auditions.back().velocity == 0,
          "the release must stop the audition it started");
    (void)view.grab();
    const ViewNote *heardNote = noteAt(dragNote.startTick, dragNote.key);
    check(heardNote && heardNote->velocity == previewVelocity,
          "the audition probe's own drag must still have committed its edit");
    // Nothing holds the note now, so the roll is back on the stored value —
    // which the commit just moved to where the preview was.
    const QImage rollAfter = roll->grab().toImage();
    check(hasColorNear(rollAfter, rollProbe, rollProbeRadius,
                       SongView::velocityNoteColor(previewVelocity), 24),
          "the committed velocity must stay on the roll after the release");
    // An abandoned edit takes its preview and its sound with it.
    auditions.clear();
    sendLaneMouse(lane, QEvent::MouseButtonPress, dragPoint + QPointF(3.0, 0.0), Qt::LeftButton,
                  Qt::LeftButton);
    sendLaneMouse(lane, QEvent::MouseMove, nodePoint, Qt::NoButton, Qt::LeftButton);
    sendLaneKey(lane, Qt::Key_Escape);
    check(!auditions.empty() && auditions.back().velocity == 0,
          "Escape must stop the sound along with the preview");
    (void)view.grab();
    check(hasColorNear(roll->grab().toImage(), rollProbe, rollProbeRadius,
                       SongView::velocityNoteColor(previewVelocity), 24),
          "an abandoned lane edit must leave the roll on the stored velocity");
    sendLaneMouse(lane, QEvent::MouseButtonRelease, nodePoint, Qt::LeftButton, Qt::NoButton);
    doc.undoStack()->undo();
    view.setVelocityColorMode(velocityInkBefore);
    view.scrollRollBy(rollScrollBefore - view.scrollY());
    (void)view.grab();

    // --- and the same link the other way: the lane plots the roll's own
    // notes, so a velocity drag in the ROLL is an edit to the node. It rides
    // the drag rather than sitting on the stored value until the release.
    view.setVelocityColorMode(true);
    const double rollScrollBeforeMirror = view.scrollY();
    view.scrollRollBy((127 - dragNote.key) * view.keyHeight() - view.scrollY() -
                      roll->height() / 2.0);
    view.setSelection({dragId});
    (void)view.grab();
    // Up is louder, and the roll measures the drag in pixels from the press,
    // so the travel IS the velocity delta. It heads away from the note's own
    // end of the ruler, so nothing clamps.
    const int rollDragDelta = dragNote.velocity > 64 ? -20 : 20;
    const int rollPreviewVelocity = dragNote.velocity + rollDragDelta;
    const QPointF rollPressPoint(
        (view.displayX(double(dragNote.startTick), songview::kKeyboardW, dpr) +
         view.displayX(double(dragNote.endTick), songview::kKeyboardW, dpr)) /
            2.0,
        (127 - dragNote.key) * view.keyHeight() - view.scrollY() + view.keyHeight() / 2.0);
    const QPointF rollDragPoint(rollPressPoint.x(), rollPressPoint.y() - rollDragDelta);
    const QPointF laneNodeBefore(
        view.displayX(double(dragNote.startTick), songview::kGutterW, dpr) * rasterDpr,
        laneVelocityY(lane, dragNote.velocity) * rasterDpr);
    const QPointF laneNodeDuring(laneNodeBefore.x(),
                                 laneVelocityY(lane, rollPreviewVelocity) * rasterDpr);
    const int laneProbeRadius = int(std::ceil(2 * rasterDpr));
    check(hasColorNear(lane->grab().toImage(), laneNodeBefore, laneProbeRadius,
                       SongView::velocityNoteColor(dragNote.velocity), 24),
          "the lane's node must start on the stored velocity's row and hue");
    // The modifier chord the roll's velocity drag is bound to, asked of the
    // registry so a rebind moves the probe with it.
    const Qt::KeyboardModifiers velMods =
        keymap::Registry::instance().modifierBinding(QStringLiteral("roll.velocity_drag"));
    sendLaneMouse(roll, QEvent::MouseButtonPress, rollPressPoint, Qt::LeftButton, Qt::LeftButton,
                  velMods);
    sendLaneMouse(roll, QEvent::MouseMove, rollDragPoint, Qt::NoButton, Qt::LeftButton, velMods);
    const QImage laneDuringRollDrag = lane->grab().toImage();
    check(hasColorNear(laneDuringRollDrag, laneNodeDuring, laneProbeRadius,
                       SongView::velocityNoteColor(rollPreviewVelocity), 24),
          "a roll velocity drag must move the lane's node to the value its release will write");
    check(!hasColorNear(laneDuringRollDrag, laneNodeBefore, laneProbeRadius,
                        SongView::velocityNoteColor(dragNote.velocity), 8),
          "the lane must not leave the node behind on the stored velocity mid-drag");
    sendLaneMouse(roll, QEvent::MouseButtonRelease, rollDragPoint, Qt::LeftButton, Qt::NoButton,
                  velMods);
    (void)view.grab();
    const ViewNote *rollDragged = noteAt(dragNote.startTick, dragNote.key);
    check(rollDragged && rollDragged->velocity == rollPreviewVelocity,
          "the roll drag's release must commit the velocity the lane was previewing");
    check(hasColorNear(lane->grab().toImage(), laneNodeDuring, laneProbeRadius,
                       SongView::velocityNoteColor(rollPreviewVelocity), 24),
          "the committed velocity must keep the lane's node where the drag left it");
    doc.undoStack()->undo();
    (void)view.grab();
    check(hasColorNear(lane->grab().toImage(), laneNodeBefore, laneProbeRadius,
                       SongView::velocityNoteColor(dragNote.velocity), 24),
          "undo must take the lane's node back to the stored velocity with the roll");
    view.setVelocityColorMode(velocityInkBefore);
    view.scrollRollBy(rollScrollBeforeMirror - view.scrollY());
    (void)view.grab();

    // --- the audition's loudness belongs to the note, not to the cursor: a
    // VOL change dropped between the two must not reach the preview. The
    // change sits past the probed note, so its own levels — and its node's
    // place in the plot — are exactly where they were.
    {
        const int undoBeforeVolume = doc.undoStack()->index();
        const uint64_t cursorBeforeVolume = view.editCursorTick();
        const int volumeAtNote = view.trackRawVolumeAt(track, dragNote.startTick);
        const int volumeLater = volumeAtNote > 63 ? 30 : 120;
        const uint64_t volumeChangeTick = dragNote.startTick + 1;
        doc.addLanePoint(track, 7, volumeChangeTick, volumeLater);
        view.commitEditCursor(volumeChangeTick);
        (void)view.grab();
        auditions.clear();
        sendLaneMouse(lane, QEvent::MouseButtonPress, nodePoint, Qt::LeftButton, Qt::LeftButton);
        sendLaneMouse(lane, QEvent::MouseButtonRelease, nodePoint, Qt::LeftButton, Qt::NoButton);
        check(view.trackRawVolumeAt(track, volumeChangeTick) == volumeLater &&
                  volumeLater != volumeAtNote,
              "the probe's VOL change must really move the track volume past the note");
        check(!auditions.empty() && auditions.front().velocity == dragNote.velocity &&
                  auditions.front().rawVolume == volumeAtNote,
              "a node press must sound at the VOL in force at its own note, not the cursor's");
        while (doc.undoStack()->index() > undoBeforeVolume)
            doc.undoStack()->undo();
        view.commitEditCursor(cursorBeforeVolume);
        (void)view.grab();
    }

    // --- click semantics: press, no travel, release
    view.setSelection({dragId, partnerId});
    (void)view.grab();
    const uint64_t revisionBeforeClick = doc.revision();
    sendLaneMouse(lane, QEvent::MouseButtonPress, nodePoint, Qt::LeftButton, Qt::LeftButton);
    sendLaneMouse(lane, QEvent::MouseMove, nodePoint + QPointF(1.0, 0.0), Qt::NoButton,
                  Qt::LeftButton);
    sendLaneMouse(lane, QEvent::MouseButtonRelease, nodePoint + QPointF(1.0, 0.0), Qt::LeftButton,
                  Qt::NoButton);
    check(view.selection().size() == 1 && view.selection()[0] == dragId &&
              doc.revision() == revisionBeforeClick,
          "a click on a node must collapse the selection onto it and edit nothing");
    // Only vertical travel means a drag here: velocity has no horizontal
    // axis, so a sideways wobble must still resolve as that click.
    view.setSelection({dragId, partnerId});
    (void)view.grab();
    const QPointF wobble = nodePoint + QPointF(3.0 * dragActivation, 0.0);
    sendLaneMouse(lane, QEvent::MouseButtonPress, nodePoint, Qt::LeftButton, Qt::LeftButton);
    sendLaneMouse(lane, QEvent::MouseMove, wobble, Qt::NoButton, Qt::LeftButton);
    sendLaneMouse(lane, QEvent::MouseButtonRelease, wobble, Qt::LeftButton, Qt::NoButton);
    check(view.selection().size() == 1 && view.selection()[0] == dragId &&
              doc.revision() == revisionBeforeClick,
          "a sideways wobble on a node must still be a click");
    sendLaneMouse(lane, QEvent::MouseButtonPress, nodePoint, Qt::LeftButton, Qt::LeftButton,
                  Qt::ControlModifier);
    sendLaneMouse(lane, QEvent::MouseButtonRelease, nodePoint, Qt::LeftButton, Qt::NoButton,
                  Qt::ControlModifier);
    check(view.selection().empty(), "Ctrl+click must drop a selected node from the selection");
    sendLaneMouse(lane, QEvent::MouseButtonPress, nodePoint, Qt::LeftButton, Qt::LeftButton,
                  Qt::ControlModifier);
    sendLaneMouse(lane, QEvent::MouseButtonRelease, nodePoint, Qt::LeftButton, Qt::NoButton,
                  Qt::ControlModifier);
    check(view.selection().size() == 1 && view.selection()[0] == dragId,
          "Ctrl+click must add an unselected node without dropping the rest");

    // A velocity row at the probe's tick with no node or stem near it.
    const auto emptyRowY = [&](double x) {
        for (int velocity = 120; velocity >= 8; velocity -= 4) {
            const double y = laneVelocityY(lane, velocity);
            bool clear = true;
            for (const ViewNote &note : view.model().notes) {
                if (note.track != track)
                    continue;
                const double start = view.displayX(double(note.startTick), songview::kGutterW, dpr);
                const double end = view.displayX(double(note.endTick), songview::kGutterW, dpr);
                if (x < start - 12.0 || x > end + 12.0)
                    continue;
                if (std::abs(laneVelocityY(lane, note.velocity) - y) < 12.0)
                    clear = false;
            }
            if (clear)
                return y;
        }
        return -1.0;
    };
    const double emptyY = emptyRowY(nodePoint.x());
    if (emptyY < 0.0) {
        fail("no empty plot row at the probe's tick");
        return failures == 0 ? 0 : 1;
    }
    // A stroke only reaches notes that are selected AND under it, so with
    // only the far note selected this press crosses nothing and stays a
    // click.
    const QPointF emptyPoint(nodePoint.x(), emptyY);
    view.setSelection({partnerId});
    view.commitEditCursor(0);
    (void)view.grab();
    sendLaneMouse(lane, QEvent::MouseButtonPress, emptyPoint, Qt::LeftButton, Qt::LeftButton);
    check(view.selection().size() == 1,
          "a press on empty plot must leave the selection alone until the release");
    sendLaneMouse(lane, QEvent::MouseButtonRelease, emptyPoint, Qt::LeftButton, Qt::NoButton);
    check(view.selection().empty() && doc.revision() == revisionBeforeClick,
          "a click on empty plot must clear the selection on the release");
    // The roll's rule, shared: that same click parks the edit cursor.
    const uint64_t clickCursor =
        view.snapTick(view.tickAtContentX(emptyPoint.x() - songview::kGutterW));
    check(clickCursor > 0 && view.editCursorTick() == clickCursor,
          "a click on empty plot must park the edit cursor at the click");

    // Escape with nothing live drops the selection, like the roll's.
    view.setSelection({partnerId});
    (void)view.grab();
    const uint64_t revisionBeforeIdleEscape = doc.revision();
    sendLaneKey(lane, Qt::Key_Escape);
    check(view.selection().empty() && doc.revision() == revisionBeforeIdleEscape,
          "Escape outside a gesture must clear the selection");

    // --- the stem grabs its own note, and Escape abandons a live drag
    const double stemX =
        (nodePoint.x() + view.displayX(double(dragNote.endTick), songview::kGutterW, dpr)) / 2.0;
    if (stemX - nodePoint.x() > 8.0) {
        const QPointF stemPoint(stemX, nodePoint.y());
        sendLaneMouse(lane, QEvent::MouseButtonPress, stemPoint, Qt::LeftButton, Qt::LeftButton);
        sendLaneMouse(lane, QEvent::MouseButtonRelease, stemPoint, Qt::LeftButton, Qt::NoButton);
        check(view.selection().size() == 1 && view.selection()[0] == dragId,
              "a press on a note's stem must grab that note");
    }
    view.setSelection({partnerId});
    (void)view.grab();
    const uint64_t revisionBeforeEscape = doc.revision();
    sendLaneMouse(lane, QEvent::MouseButtonPress, nodePoint, Qt::LeftButton, Qt::LeftButton);
    check(view.selection().size() == 1 && view.selection()[0] == dragId,
          "pressing an unselected node must take the selection over");
    sendLaneMouse(lane, QEvent::MouseMove, dragPoint, Qt::NoButton, Qt::LeftButton);
    sendLaneKey(lane, Qt::Key_Escape);
    check(view.selection().size() == 1 && view.selection()[0] == partnerId &&
              doc.revision() == revisionBeforeEscape,
          "Escape must abandon the drag and put the pre-press selection back");
    sendLaneMouse(lane, QEvent::MouseButtonRelease, dragPoint, Qt::LeftButton, Qt::NoButton);
    const ViewNote *escapedNote = noteAt(dragNote.startTick, dragNote.key);
    check(doc.revision() == revisionBeforeEscape && escapedNote &&
              escapedNote->velocity == dragNote.velocity,
          "the release after an Escape must not commit the abandoned drag");
    // A document edit from elsewhere kills a live preview — but a press that
    // has not previewed anything yet had nothing to cancel.
    DocNote outsideNote;
    if (!doc.findNote(track, partner.startTick, partner.key, &outsideNote)) {
        fail("the outside-edit fixture must resolve its note");
        return failures == 0 ? 0 : 1;
    }
    view.setSelection({dragId});
    (void)view.grab();
    lastStatus.clear();
    sendLaneMouse(lane, QEvent::MouseButtonPress, nodePoint, Qt::LeftButton, Qt::LeftButton);
    doc.setNotesVelocity({outsideNote}, uint8_t(partner.velocity > 64 ? 20 : 110));
    (void)view.grab();
    check(lastStatus != cancelText,
          "a press that previewed nothing must not report a cancelled edit");
    sendLaneMouse(lane, QEvent::MouseButtonRelease, nodePoint, Qt::LeftButton, Qt::NoButton);
    doc.undoStack()->undo();
    (void)view.grab();
    view.setSelection({dragId});
    (void)view.grab();
    if (!doc.findNote(track, partner.startTick, partner.key, &outsideNote)) {
        fail("the outside-edit fixture must resolve its note after the undo");
        return failures == 0 ? 0 : 1;
    }
    sendLaneMouse(lane, QEvent::MouseButtonPress, nodePoint, Qt::LeftButton, Qt::LeftButton);
    sendLaneMouse(lane, QEvent::MouseMove, dragPoint, Qt::NoButton, Qt::LeftButton);
    const uint64_t revisionBeforeOutside = doc.revision();
    doc.setNotesVelocity({outsideNote}, uint8_t(partner.velocity > 64 ? 20 : 110));
    (void)view.grab();
    check(lastStatus == cancelText,
          "an outside edit under a live preview must cancel it and say so");
    sendLaneMouse(lane, QEvent::MouseButtonRelease, dragPoint, Qt::LeftButton, Qt::NoButton);
    const ViewNote *survivor = noteAt(dragNote.startTick, dragNote.key);
    check(survivor && survivor->velocity == dragNote.velocity &&
              doc.revision() == revisionBeforeOutside + 1,
          "the release after an outside edit must not land the abandoned preview");
    doc.undoStack()->undo();
    view.clearSelection();
    (void)view.grab();

    // --- painting, ramps, and the ruler's own click
    const double partnerEmptyY = emptyRowY(partnerPoint.x());
    if (partnerEmptyY < 0.0) {
        fail("no empty plot row at the partner's tick");
        return failures == 0 ? 0 : 1;
    }
    // The stroke starts on an empty row above the near note and runs off to
    // the right past the far one, so it crosses unselected notes on the way.
    const QPointF paintStart(nodePoint.x(), emptyY);
    const QPointF paintEnd(lane->width() - 20.0, partnerEmptyY);
    const double paintSpan = paintEnd.x() - paintStart.x();
    const auto paintedVelocity = [&](double x) {
        const double t = std::clamp((x - paintStart.x()) / paintSpan, 0.0, 1.0);
        return laneVelocityAt(lane, paintStart.y() + t * (paintEnd.y() - paintStart.y()));
    };
    const int paintTarget = paintedVelocity(nodePoint.x());
    const int paintPartnerTarget = paintedVelocity(partnerPoint.x());
    // A note the stroke passes over but never selects: paint must leave it
    // exactly where it was, and it must have somewhere to move to.
    const ViewNote *bystanderProbe = nullptr;
    for (const ViewNote &note : view.model().notes) {
        const double x = view.displayX(double(note.startTick), songview::kGutterW, dpr);
        if (note.track == track && x > paintStart.x() && x < paintEnd.x() &&
            note.startTick != dragNote.startTick && note.startTick != partner.startTick &&
            paintedVelocity(x) != note.velocity)
            bystanderProbe = &note;
        if (bystanderProbe)
            break;
    }
    if (paintSpan <= 0.0 || paintTarget == dragNote.velocity ||
        paintPartnerTarget == partner.velocity || !bystanderProbe) {
        fail("the paint stroke's fixture must ask for new velocities and cross a bystander");
        return failures == 0 ? 0 : 1;
    }
    const ViewNote bystander = *bystanderProbe;
    view.setSelection({dragId, partnerId});
    (void)view.grab();
    const uint64_t revisionBeforePaint = doc.revision();
    const int undoBeforePaint = doc.undoStack()->index();
    auditions.clear();
    sendLaneMouse(lane, QEvent::MouseButtonPress, paintStart, Qt::LeftButton, Qt::LeftButton);
    sendLaneMouse(lane, QEvent::MouseMove, paintEnd, Qt::NoButton, Qt::LeftButton);
    check(doc.revision() == revisionBeforePaint,
          "a paint stroke must leave the document alone until the release");
    check(!auditions.empty() && auditions.back().velocity > 0,
          "a paint stroke must sound the note it is brushing");
    sendLaneMouse(lane, QEvent::MouseButtonRelease, paintEnd, Qt::LeftButton, Qt::NoButton);
    check(auditions.back().velocity == 0, "the release must stop what a paint stroke was sounding");
    (void)view.grab();
    const ViewNote *paintedNote = noteAt(dragNote.startTick, dragNote.key);
    const ViewNote *paintedPartner = noteAt(partner.startTick, partner.key);
    check(lastStatus == QStringLiteral("Painted note velocities."),
          "a committed paint stroke must say what it did");
    check(paintedNote && paintedPartner && paintedNote->velocity == paintTarget &&
              paintedPartner->velocity == paintPartnerTarget &&
              doc.undoStack()->index() == undoBeforePaint + 1 && view.selection().size() == 2,
          "a paint stroke must write each selected note it crosses in one undo entry");
    const ViewNote *untouched = noteAt(bystander.startTick, bystander.key);
    check(untouched && untouched->velocity == bystander.velocity,
          "a paint stroke must not touch an unselected note under it");
    doc.undoStack()->undo();
    (void)view.grab();

    // A click on empty plot straight below a selected node is how one note
    // is set on its own.
    view.setSelection({dragId});
    (void)view.grab();
    const int spotTarget = laneVelocityAt(lane, emptyY);
    const QPointF spotPoint(nodePoint.x(), emptyY);
    const int undoBeforeSpot = doc.undoStack()->index();
    sendLaneMouse(lane, QEvent::MouseButtonPress, spotPoint, Qt::LeftButton, Qt::LeftButton);
    sendLaneMouse(lane, QEvent::MouseButtonRelease, spotPoint, Qt::LeftButton, Qt::NoButton);
    (void)view.grab();
    const ViewNote *spotNote = noteAt(dragNote.startTick, dragNote.key);
    const ViewNote *spotPartner = noteAt(partner.startTick, partner.key);
    check(spotNote && spotPartner && spotNote->velocity == spotTarget &&
              spotPartner->velocity == partner.velocity &&
              doc.undoStack()->index() == undoBeforeSpot + 1 && view.selection().size() == 1,
          "clicking below a selected node must set that note alone");
    doc.undoStack()->undo();
    (void)view.grab();

    // Shift is a ramp: the straight line from the press to the pointer, laid
    // across the selection. Pressing on a node proves it — a relative drag
    // would have moved that node too.
    view.setSelection({dragId, partnerId});
    (void)view.grab();
    const int rampTarget = laneVelocityAt(lane, emptyY);
    const QPointF rampStart(partnerPoint.x(), laneVelocityY(lane, partner.velocity));
    const QPointF rampEnd(nodePoint.x(), emptyY);
    const int undoBeforeRamp = doc.undoStack()->index();
    auditions.clear();
    sendLaneMouse(lane, QEvent::MouseButtonPress, rampStart, Qt::LeftButton, Qt::LeftButton,
                  Qt::ShiftModifier);
    sendLaneMouse(lane, QEvent::MouseMove, rampEnd, Qt::NoButton, Qt::LeftButton,
                  Qt::ShiftModifier);
    const QImage rampImage = lane->grab().toImage();
    const QPointF rampQuarter = rampStart + 0.25 * (rampEnd - rampStart);
    check(hasColorNear(rampImage, QPointF(rampQuarter.x() * rasterDpr, rampQuarter.y() * rasterDpr),
                       int(std::ceil(2 * rasterDpr)),
                       themes::color(themes::Role::song_view_edit_preview_outline), 24),
          "a ramp must draw the line it is reading its values off");
    // The ramp lays a value on every selected note it spans and reads one of
    // them out; the sound follows that same note, whichever it is.
    check(!auditions.empty() &&
              ((auditions.back().key == dragNote.key && auditions.back().velocity == rampTarget) ||
               (auditions.back().key == partner.key &&
                auditions.back().velocity == partner.velocity)),
          "a ramp must sound the note it is reading out");
    sendLaneMouse(lane, QEvent::MouseButtonRelease, rampEnd, Qt::LeftButton, Qt::NoButton,
                  Qt::ShiftModifier);
    check(auditions.back().velocity == 0, "the release must stop what a ramp was sounding");
    (void)view.grab();
    const ViewNote *rampedNote = noteAt(dragNote.startTick, dragNote.key);
    const ViewNote *rampedPartner = noteAt(partner.startTick, partner.key);
    check(lastStatus == QStringLiteral("Ramped note velocities."),
          "a committed ramp must say what it did");
    check(rampedNote && rampedPartner && rampedNote->velocity == rampTarget &&
              rampedPartner->velocity == partner.velocity &&
              doc.undoStack()->index() == undoBeforeRamp + 1,
          "a Shift drag must ramp the selection between its ends instead of moving it");
    // A Shift press inside the activation travel is a click too: hand jitter
    // must not commit a ramp, even though the value under it has moved.
    const QPointF shiftJitter = rampStart - QPointF(0.0, dragActivation - 1.0);
    if (laneVelocityAt(lane, shiftJitter.y()) == partner.velocity) {
        fail("the Shift-click fixture must sit within a velocity of its jitter");
        return failures == 0 ? 0 : 1;
    }
    const int undoBeforeShiftClick = doc.undoStack()->index();
    sendLaneMouse(lane, QEvent::MouseButtonPress, rampStart, Qt::LeftButton, Qt::LeftButton,
                  Qt::ShiftModifier);
    sendLaneMouse(lane, QEvent::MouseMove, shiftJitter, Qt::NoButton, Qt::LeftButton,
                  Qt::ShiftModifier);
    sendLaneMouse(lane, QEvent::MouseButtonRelease, shiftJitter, Qt::LeftButton, Qt::NoButton,
                  Qt::ShiftModifier);
    check(doc.undoStack()->index() == undoBeforeShiftClick,
          "a Shift click that never travels must not commit a ramp");

    // The ruler's printed values are click targets for the whole selection;
    // 127 is labeled in every density band.
    view.setSelection({dragId, partnerId});
    (void)view.grab();
    const int undoBeforeRuler = doc.undoStack()->index();
    const QPointF headerPoint(songview::kHeaderW / 2.0, laneVelocityY(lane, 127));
    sendLaneMouse(lane, QEvent::MouseButtonPress, headerPoint, Qt::LeftButton, Qt::LeftButton);
    sendLaneMouse(lane, QEvent::MouseButtonRelease, headerPoint, Qt::LeftButton, Qt::NoButton);
    check(doc.undoStack()->index() == undoBeforeRuler,
          "a press in the track-header column must not edit velocities");
    const QPointF rulerPoint(songview::kHeaderW + songview::kKeyboardW / 2.0,
                             laneVelocityY(lane, 127));
    sendLaneMouse(lane, QEvent::MouseButtonPress, rulerPoint, Qt::LeftButton, Qt::LeftButton);
    sendLaneMouse(lane, QEvent::MouseButtonRelease, rulerPoint, Qt::LeftButton, Qt::NoButton);
    (void)view.grab();
    const ViewNote *rulerNote = noteAt(dragNote.startTick, dragNote.key);
    const ViewNote *rulerPartner = noteAt(partner.startTick, partner.key);
    check(rulerNote && rulerPartner && rulerNote->velocity == 127 &&
              rulerPartner->velocity == 127 && doc.undoStack()->index() == undoBeforeRuler + 1 &&
              view.selection().size() == 2,
          "clicking a ruler value must set the whole selection to it in one undo entry");
    doc.undoStack()->undo();
    view.clearSelection();
    (void)view.grab();

    // --- the marquee: the right button bands nodes instead of editing them
    // The gestures above left the probe notes wherever their last committed
    // edit put them, so the band aims at where the node is now.
    const ViewNote *bandNote = noteAt(dragNote.startTick, dragNote.key);
    const double bandEmptyY = emptyRowY(nodePoint.x());
    if (!bandNote || bandEmptyY < 0.0) {
        fail("the marquee fixture must find its node and an empty row at its tick");
        return failures == 0 ? 0 : 1;
    }
    const QPointF bandNodePoint(nodePoint.x(), laneVelocityY(lane, bandNote->velocity));
    const QPointF bandEmptyPoint(nodePoint.x(), bandEmptyY);
    const uint64_t revisionBeforeBand = doc.revision();
    const QPointF bandFrom(bandNodePoint.x() - 12.0, bandNodePoint.y() - 25.0);
    const QPointF bandTo(bandNodePoint.x() + 12.0, bandNodePoint.y() + 25.0);
    view.setSelection({partnerId});
    (void)view.grab();
    sendLaneMouse(lane, QEvent::MouseButtonPress, bandFrom, Qt::RightButton, Qt::RightButton);
    check(view.selection().size() == 1 && view.selection()[0] == partnerId,
          "a right press on empty plot must leave the selection alone until the release");
    sendLaneMouse(lane, QEvent::MouseMove, bandTo, Qt::NoButton, Qt::RightButton);
    const QImage bandImage = lane->grab().toImage();
    // The band previews what it would take: the covered node rings while the
    // selection itself has not moved yet.
    check(
        hasColorNear(bandImage,
                     QPointF(bandNodePoint.x() * rasterDpr, (bandNodePoint.y() - 5.0) * rasterDpr),
                     ringProbeRadius, ringColor, 24) &&
            view.selection().size() == 1 && view.selection()[0] == partnerId,
        "a swept band must ring the nodes it covers before the release lands them");
    sendLaneMouse(lane, QEvent::MouseButtonRelease, bandTo, Qt::RightButton, Qt::NoButton);
    (void)view.grab();
    check(view.selection().size() == 1 && view.selection()[0] == dragId,
          "a band must replace the selection with the nodes it covered");
    // A band swept along a row is flat: on an intrinsic ruler a run of
    // nodes shares one y exactly, and a flat rectangle must still catch
    // them.
    view.setSelection({partnerId});
    (void)view.grab();
    const QPointF flatFrom(bandNodePoint.x() - 12.0, bandNodePoint.y());
    const QPointF flatTo(bandNodePoint.x() + 12.0, bandNodePoint.y());
    sendLaneMouse(lane, QEvent::MouseButtonPress, flatFrom, Qt::RightButton, Qt::RightButton);
    sendLaneMouse(lane, QEvent::MouseMove, flatTo, Qt::NoButton, Qt::RightButton);
    sendLaneMouse(lane, QEvent::MouseButtonRelease, flatTo, Qt::RightButton, Qt::NoButton);
    (void)view.grab();
    check(view.selection().size() == 1 && view.selection()[0] == dragId,
          "a flat band must still take the nodes it runs along");
    // Ctrl unions with what the press found instead.
    view.setSelection({partnerId});
    (void)view.grab();
    sendLaneMouse(lane, QEvent::MouseButtonPress, bandFrom, Qt::RightButton, Qt::RightButton,
                  Qt::ControlModifier);
    sendLaneMouse(lane, QEvent::MouseMove, bandTo, Qt::NoButton, Qt::RightButton,
                  Qt::ControlModifier);
    sendLaneMouse(lane, QEvent::MouseButtonRelease, bandTo, Qt::RightButton, Qt::NoButton,
                  Qt::ControlModifier);
    (void)view.grab();
    check(view.selection().size() == 2 &&
              std::find(view.selection().begin(), view.selection().end(), dragId) !=
                  view.selection().end() &&
              std::find(view.selection().begin(), view.selection().end(), partnerId) !=
                  view.selection().end(),
          "Ctrl must add a band's nodes to the selection instead of replacing it");
    // Escape abandons a band exactly as it abandons an edit.
    view.setSelection({partnerId});
    (void)view.grab();
    sendLaneMouse(lane, QEvent::MouseButtonPress, bandFrom, Qt::RightButton, Qt::RightButton);
    sendLaneMouse(lane, QEvent::MouseMove, bandTo, Qt::NoButton, Qt::RightButton);
    sendLaneKey(lane, Qt::Key_Escape);
    sendLaneMouse(lane, QEvent::MouseButtonRelease, bandTo, Qt::RightButton, Qt::NoButton);
    (void)view.grab();
    check(view.selection().size() == 1 && view.selection()[0] == partnerId,
          "Escape must abandon a band and put the pre-press selection back");
    // A right-click in place is a click: on a node it selects it, Ctrl
    // toggles it, and on empty plot it clears.
    sendLaneMouse(lane, QEvent::MouseButtonPress, bandNodePoint, Qt::RightButton, Qt::RightButton);
    sendLaneMouse(lane, QEvent::MouseButtonRelease, bandNodePoint, Qt::RightButton, Qt::NoButton);
    check(view.selection().size() == 1 && view.selection()[0] == dragId,
          "a right-click on an unselected node must select it");
    sendLaneMouse(lane, QEvent::MouseButtonPress, bandNodePoint, Qt::RightButton, Qt::RightButton,
                  Qt::ControlModifier);
    sendLaneMouse(lane, QEvent::MouseButtonRelease, bandNodePoint, Qt::RightButton, Qt::NoButton,
                  Qt::ControlModifier);
    check(view.selection().empty(), "Ctrl+right-click must drop a selected node again");
    view.setSelection({dragId});
    (void)view.grab();
    sendLaneMouse(lane, QEvent::MouseButtonPress, bandEmptyPoint, Qt::RightButton, Qt::RightButton);
    sendLaneMouse(lane, QEvent::MouseButtonRelease, bandEmptyPoint, Qt::RightButton, Qt::NoButton);
    check(view.selection().empty(), "a right-click on empty plot must clear the selection");
    check(doc.revision() == revisionBeforeBand, "the marquee must never touch a velocity");
    view.clearSelection();
    (void)view.grab();

    // --- PSG detents: a CGB voice's real loudness levels drive the ruler,
    // the nodes, and every edit
    const QByteArray rootUtf8 = projectRoot.toLocal8Bit();
    LoadedVoiceGroup *voicegroup = nullptr;
    for (const QString &name : DecompProject::voicegroupCandidates(info->cfg)) {
        voicegroup = voicegroup_load(rootUtf8.constData(), name.toLocal8Bit().constData(), nullptr);
        if (voicegroup)
            break;
    }
    if (!voicegroup) {
        fail("the song's voicegroup must load: the detents are its voices'");
        return 1;
    }
    // The lane reads voices straight out of the loaded group, so the view
    // must let go of it before it is freed — the view outlives this scope.
    struct VoicegroupGuard {
        SongView *view;
        LoadedVoiceGroup *group;
        ~VoicegroupGuard()
        {
            view->setVoicegroup(nullptr);
            voicegroup_free(group);
        }
    } voicegroupGuard{&view, voicegroup};
    view.setVoicegroup(voicegroup);
    view.clearSelection();
    (void)view.grab();

    // The first track that plays on a CGB channel, and one that does not:
    // the lane's context is the selected track's voice.
    int psgTrack = -1;
    int continuousTrack = -1;
    VelocityMap psgMap;
    VelocityMap psgMapAtFullVolume;
    for (int candidate = 0; candidate < 16; candidate++) {
        const ViewNote *first = nullptr;
        for (const ViewNote &note : view.model().notes) {
            if (note.track == candidate)
                first = &note;
            if (first)
                break;
        }
        if (!first)
            continue;
        view.selectTrack(candidate);
        const SongView::VoiceContext context = view.voiceContext(first->startTick);
        const VelocityMap map =
            VelocityMap::resolve(context.voice, first->key, context.trackVolume, context.trackPan);
        if (map.isPsg() && psgTrack < 0) {
            psgTrack = candidate;
            psgMap = map;
            // The same voice as the song would sound with nothing turned
            // down: the yardstick for "the master volume really moved the
            // ruler" below.
            psgMapAtFullVolume =
                VelocityMap::resolve(context.voice, first->key, kFullVolume, context.trackPan);
        } else if (!map.isPsg() && continuousTrack < 0) {
            continuousTrack = candidate;
        }
    }
    if (psgTrack < 0 || continuousTrack < 0) {
        fail("the song must have both a PSG track and a continuous one to compare");
        return failures == 0 ? 0 : 1;
    }
    view.selectTrack(continuousTrack);
    (void)view.grab();
    check(!lane->property("velocityIntrinsic").toBool() &&
              lane->property("velocityDetents").toInt() == -1,
          "a DirectSound track must keep the plain velocity ruler and offer no detent toggle");
    view.selectTrack(psgTrack);
    (void)view.grab();
    check(lane->property("velocityIntrinsic").toBool() &&
              lane->property("velocityLevelCount").toInt() == int(psgMap.levelCount()) &&
              lane->property("velocityDetents").toInt() == 1,
          "a PSG track must put the lane on that channel's own volume levels");

    // --- the ruler is the song's, not a full-volume idealization. The
    // fixture is mastered below mxv, and mid2agb folds that into every VOL
    // byte, so the channel reaches fewer of its envelope steps than the
    // sixteen a song at full volume would.
    check(doc.cfg().masterVolume < kM4aMaxVolume &&
              psgMap.levelCount() < psgMapAtFullVolume.levelCount(),
          "the fixture's reduced master volume must cost the channel levels");
    const int levelsAtSongVolume = lane->property("velocityLevelCount").toInt();
    const int undoBeforeVolume = doc.undoStack()->index();
    SongCfg loudCfg = doc.cfg();
    loudCfg.masterVolume = kM4aMaxVolume;
    doc.setCfg(loudCfg);
    view.updateSong(view.timeline());
    (void)view.grab();
    check(lane->property("velocityLevelCount").toInt() == int(psgMapAtFullVolume.levelCount()) &&
              lane->property("velocityLevelCount").toInt() > levelsAtSongVolume,
          "turning the master volume up must give the ruler its missing levels back");
    doc.undoStack()->undo();
    view.updateSong(view.timeline());
    (void)view.grab();
    check(doc.cfg().masterVolume < kM4aMaxVolume && doc.undoStack()->index() == undoBeforeVolume &&
              lane->property("velocityLevelCount").toInt() == levelsAtSongVolume,
          "undoing the master volume must put the song's own ruler back");

    // --- the track's own VOL is the other half of that byte. A volume change
    // mid-song ends the level table exactly as a voice change does: past it
    // the same channel reaches a different set of steps, so the plot's level
    // lines must stop there rather than run on across the change.
    const AutoLane *volumeLane = nullptr;
    const LanePoint *volumeChange = nullptr;
    for (const AutoLane &candidate : view.model().lanes) {
        if (candidate.cc != 7 || candidate.points.size() < 2)
            continue;
        for (std::size_t index = 1; index < candidate.points.size(); index++) {
            // A change the master volume does not quantize away, so the
            // level table really differs on the two sides of it.
            if (m4aEffectiveTrackVolume(candidate.points[index].value, doc.cfg().masterVolume) !=
                m4aEffectiveTrackVolume(candidate.points[index - 1].value,
                                        doc.cfg().masterVolume)) {
                volumeLane = &candidate;
                volumeChange = &candidate.points[index];
                break;
            }
        }
        if (volumeLane)
            break;
    }
    if (!volumeLane) {
        fail("the fixture must have a track whose VOL really changes mid-song");
        return failures == 0 ? 0 : 1;
    }
    view.selectTrack(volumeLane->track);
    const uint64_t changeTick = volumeChange->tick;
    const SongView::VoiceContext beforeChange = view.voiceContext(changeTick - 1);
    const SongView::VoiceContext atChange = view.voiceContext(changeTick);
    check(beforeChange.trackVolume != atChange.trackVolume &&
              atChange.trackVolume ==
                  m4aEffectiveTrackVolume(volumeChange->value, doc.cfg().masterVolume),
          "a VOL point must move the compiled volume the levels are derived from");
    check(beforeChange.endTick <= changeTick,
          "a voice context must end where the track's volume changes");
    check(view.trackVolumeAt(volumeLane->track, changeTick) == atChange.trackVolume &&
              view.trackVolumeAt(volumeLane->track, 0) ==
                  m4aEffectiveTrackVolume(volumeLane->points.front().tick == 0
                                              ? volumeLane->points.front().value
                                              : kM4aMaxVolume,
                                          doc.cfg().masterVolume),
          "the track volume before the first VOL point is mid2agb's priming 127");

    // --- PAN is the third term in the same byte-for-byte chain. TrkVolPitSet
    // scales the two sides by (y+128) and (127-y), so a pan does not merely
    // trade one side for the other: it moves the boundaries between the
    // channel's levels, and a note can sit a whole level away from where the
    // centered chain would draw it. A PAN point therefore ends the context
    // just as a VOL point does.
    const AutoLane *panLane = nullptr;
    const LanePoint *panChange = nullptr;
    for (const AutoLane &candidate : view.model().lanes) {
        if (candidate.cc != 10 || candidate.points.size() < 2)
            continue;
        for (std::size_t index = 1; index < candidate.points.size(); index++) {
            if (candidate.points[index].value != candidate.points[index - 1].value) {
                panLane = &candidate;
                panChange = &candidate.points[index];
                break;
            }
        }
        if (panLane)
            break;
    }
    if (!panLane) {
        fail("the fixture must have a track whose PAN really changes mid-song");
        return failures == 0 ? 0 : 1;
    }
    view.selectTrack(panLane->track);
    const uint64_t panTick = panChange->tick;
    const SongView::VoiceContext beforePanChange = view.voiceContext(panTick - 1);
    const SongView::VoiceContext atPan = view.voiceContext(panTick);
    check(beforePanChange.trackPan != atPan.trackPan &&
              atPan.trackPan == int8_t(int(panChange->value) - 64),
          "a PAN point must move the pan the levels are derived from");
    check(beforePanChange.endTick <= panTick,
          "a voice context must end where the track's pan changes");
    check(
        view.trackPanAt(panLane->track, panTick) == atPan.trackPan &&
            view.trackPanAt(panLane->track, 0) ==
                int8_t(int(panLane->points.front().tick == 0 ? panLane->points.front().value : 64) -
                       64),
        "the pan before the first PAN point is mid2agb's priming c_v+0");
    view.selectTrack(psgTrack);
    (void)view.grab();

    // A note to edit: isolated under the camera, off the overlays' verticals,
    // with a level either side of its own to move into, and a stored velocity
    // that is neither its level's representative nor near its row's center —
    // so "snapped to the level" and "left where it was" cannot look alike.
    const ViewNote *psgProbe = nullptr;
    for (const ViewNote &note : view.model().notes) {
        if (note.track != psgTrack || !note.noteId.isAssigned())
            continue;
        const std::optional<std::size_t> level = psgMap.levelOf(note.velocity);
        if (!level || *level == 0 || *level + 1 >= psgMap.levelCount())
            continue;
        if (note.velocity == psgMap.representative(int(*level)))
            continue;
        const qreal x = view.displayX(double(note.startTick), songview::kGutterW, dpr);
        if (x < songview::kGutterW + 60 || x > lane->width() - 60 || overlayContested(x))
            continue;
        // Far enough off its own row that a press aimed at the row would
        // MISS the node if the lane were drawing it at its stored velocity:
        // that is what tells "the detents placed this node" apart from "it
        // happened to land near here".
        if (std::abs(laneLevelCenterY(lane, psgMap, int(*level)) -
                     laneVelocityY(lane, note.velocity)) <= kVelNodeGrabReach)
            continue;
        bool crowded = false;
        for (const ViewNote &other : view.model().notes) {
            if (&other == &note || other.track != psgTrack)
                continue;
            const qreal otherX = view.displayX(double(other.startTick), songview::kGutterW, dpr);
            if (std::abs(otherX - x) < 16)
                crowded = true;
        }
        if (!crowded)
            psgProbe = &note;
        if (psgProbe)
            break;
    }
    if (!psgProbe) {
        fail("no PSG note off its level's center to probe");
        return failures == 0 ? 0 : 1;
    }
    const ViewNote psgNote = *psgProbe;
    const SongView::NoteKey psgId{psgNote.startTick, psgNote.key};
    // The earlier lookup is bound to the track the check opened on; these
    // edits land on the PSG track instead.
    const auto psgNoteAt = [&]() -> const ViewNote * {
        for (const ViewNote &note : view.model().notes) {
            if (note.track == psgTrack && note.startTick == psgNote.startTick &&
                note.key == psgNote.key)
                return &note;
        }
        return nullptr;
    };
    const int psgLevel = int(*psgMap.levelOf(psgNote.velocity));
    const double psgX = view.displayX(double(psgNote.startTick), songview::kGutterW, dpr);
    const double psgCenterY = laneLevelCenterY(lane, psgMap, psgLevel);
    const QPixmap psgPixmap = lane->grab();
    const QImage psgImage = psgPixmap.toImage();
    const QColor psgColor = SongView::trackColor(psgTrack);
    const int psgProbeRadius = int(std::ceil(2 * rasterDpr));
    check(hasColorNear(psgImage, QPointF(psgX * rasterDpr, psgCenterY * rasterDpr), psgProbeRadius,
                       psgColor, 24) &&
              !hasColorNear(
                  psgImage,
                  QPointF(psgX * rasterDpr, laneVelocityY(lane, psgNote.velocity) * rasterDpr),
                  psgProbeRadius, psgColor, 24),
          "a PSG note's node must sit on its level's row, not on its stored velocity");
    // The level lines paint across the plot along the rows themselves — where
    // the nodes sit and a drag lets go — and not in the gaps between them, so
    // a line reads as the rail a node is on rather than a fence beside it.
    const QColor levelInk = themes::color(themes::Role::song_view_psg_velocity_levels);
    const double gapY = laneLevelBoundaryY(lane, psgMap, psgLevel);
    const double plotLeft = songview::kGutterW * rasterDpr;
    const double plotRight = lane->width() * rasterDpr - 1.0;
    // A line, not a stray pixel: several nodes' widths of unbroken ink on the
    // row, and nothing that reads as a line in the gap beside it.
    constexpr int kLineRun = 16;
    constexpr int kStrayRun = 4;
    check(longestColorRun(psgImage, psgCenterY * rasterDpr, plotLeft, plotRight, levelInk, 6) >=
                  kLineRun &&
              longestColorRun(psgImage, gapY * rasterDpr, plotLeft, plotRight, levelInk, 6) <=
                  kStrayRun,
          "the PSG level lines must paint along the rows the nodes snap to, not between them");

    // The ruler's rows are the whole column: a click anywhere inside one
    // sets the selection to that level's representative.
    view.setSelection({psgId});
    (void)view.grab();
    const int rulerLevel = psgLevel >= 3 ? psgLevel - 3 : psgLevel + 3;
    const QPointF psgRulerPoint(songview::kHeaderW + songview::kKeyboardW / 2.0,
                                laneLevelCenterY(lane, psgMap, rulerLevel) + 2.0);
    const int undoBeforePsgRuler = doc.undoStack()->index();
    sendLaneMouse(lane, QEvent::MouseButtonPress, psgRulerPoint, Qt::LeftButton, Qt::LeftButton);
    sendLaneMouse(lane, QEvent::MouseButtonRelease, psgRulerPoint, Qt::LeftButton, Qt::NoButton);
    (void)view.grab();
    const ViewNote *psgRulerNote = psgNoteAt();
    check(psgRulerNote && psgRulerNote->velocity == psgMap.representative(rulerLevel) &&
              doc.undoStack()->index() == undoBeforePsgRuler + 1,
          "an intrinsic ruler click must set the selection to that level's own value");
    doc.undoStack()->undo();
    (void)view.grab();

    // A drag moves whole levels, and a drag that comes back to the level it
    // started in restores the exact velocity it found there.
    const QPointF psgFrom(psgX, psgCenterY);
    const QPointF psgUp(psgX, laneLevelCenterY(lane, psgMap, psgLevel + 1));
    const int undoBeforePsgDrag = doc.undoStack()->index();
    sendLaneMouse(lane, QEvent::MouseButtonPress, psgFrom, Qt::LeftButton, Qt::LeftButton);
    sendLaneMouse(lane, QEvent::MouseMove, psgUp, Qt::NoButton, Qt::LeftButton);
    sendLaneMouse(lane, QEvent::MouseButtonRelease, psgUp, Qt::LeftButton, Qt::NoButton);
    (void)view.grab();
    const ViewNote *psgDragged = psgNoteAt();
    check(psgDragged && psgDragged->velocity == psgMap.representative(psgLevel + 1) &&
              doc.undoStack()->index() == undoBeforePsgDrag + 1,
          "a PSG drag must land on the next level's representative");
    doc.undoStack()->undo();
    (void)view.grab();
    sendLaneMouse(lane, QEvent::MouseButtonPress, psgFrom, Qt::LeftButton, Qt::LeftButton);
    sendLaneMouse(lane, QEvent::MouseMove, psgUp, Qt::NoButton, Qt::LeftButton);
    sendLaneMouse(lane, QEvent::MouseMove, psgFrom, Qt::NoButton, Qt::LeftButton);
    sendLaneMouse(lane, QEvent::MouseButtonRelease, psgFrom, Qt::LeftButton, Qt::NoButton);
    (void)view.grab();
    const ViewNote *psgReturned = psgNoteAt();
    check(psgReturned && psgReturned->velocity == psgNote.velocity &&
              doc.undoStack()->index() == undoBeforePsgDrag,
          "a PSG drag back to its own level must restore the exact velocity it started from");

    // The unlock chord, read at the press, writes exact values instead. The
    // press is on the node where the detents put it, so the drag's travel is
    // measured from that row: five velocity units of pointer movement move
    // the note by exactly five.
    const int unlockDelta = psgNote.velocity > 64 ? -5 : 5;
    const QPointF psgExact(psgX, laneVelocityY(lane, psgNote.velocity + unlockDelta));
    const QPointF psgUnlockTo(psgX,
                              laneVelocityY(lane, laneVelocityAt(lane, psgCenterY) + unlockDelta));
    const int unlockTarget = psgNote.velocity + unlockDelta;
    if (unlockTarget == psgMap.canonicalize(unlockTarget)) {
        fail("the unlock fixture must ask for a value the detents would not allow");
        return failures == 0 ? 0 : 1;
    }
    const Qt::KeyboardModifiers unlockMods =
        keymap::Registry::instance().modifierBinding(QStringLiteral("velocity.detent_unlock"));
    const int undoBeforeUnlock = doc.undoStack()->index();
    sendLaneMouse(lane, QEvent::MouseButtonPress, psgFrom, Qt::LeftButton, Qt::LeftButton,
                  unlockMods);
    sendLaneMouse(lane, QEvent::MouseMove, psgUnlockTo, Qt::NoButton, Qt::LeftButton, unlockMods);
    sendLaneMouse(lane, QEvent::MouseButtonRelease, psgUnlockTo, Qt::LeftButton, Qt::NoButton,
                  unlockMods);
    (void)view.grab();
    const ViewNote *unlocked = psgNoteAt();
    check(unlocked && unlocked->velocity == unlockTarget &&
              doc.undoStack()->index() == undoBeforeUnlock + 1,
          "the unlock chord must write the exact velocity the pointer asks for");
    doc.undoStack()->undo();
    (void)view.grab();

    // The header's Detents checkbox turns the whole thing off for the track.
    // A real child widget now, so the clicks go to it, not to the lane.
    auto *detentCheck = lane->findChild<QCheckBox *>(QStringLiteral("velocityDetentCheck"));
    if (!detentCheck || detentCheck->isHidden()) {
        fail("a PSG context must offer the detent toggle");
        return failures == 0 ? 0 : 1;
    }
    const auto clickDetentCheck = [&] {
        const QPointF center(detentCheck->rect().center());
        sendLaneMouse(detentCheck, QEvent::MouseButtonPress, center, Qt::LeftButton,
                      Qt::LeftButton);
        sendLaneMouse(detentCheck, QEvent::MouseButtonRelease, center, Qt::LeftButton,
                      Qt::NoButton);
    };
    clickDetentCheck();
    (void)view.grab();
    check(lane->property("velocityDetents").toInt() == 0 &&
              !lane->property("velocityIntrinsic").toBool(),
          "the Detents checkbox must put the plain ruler back");
    const int undoBeforeChipDrag = doc.undoStack()->index();
    const QPointF chipFrom(psgX, laneVelocityY(lane, psgNote.velocity));
    sendLaneMouse(lane, QEvent::MouseButtonPress, chipFrom, Qt::LeftButton, Qt::LeftButton);
    sendLaneMouse(lane, QEvent::MouseMove, psgExact, Qt::NoButton, Qt::LeftButton);
    sendLaneMouse(lane, QEvent::MouseButtonRelease, psgExact, Qt::LeftButton, Qt::NoButton);
    (void)view.grab();
    const ViewNote *chipDragged = psgNoteAt();
    check(chipDragged && chipDragged->velocity == unlockTarget &&
              doc.undoStack()->index() == undoBeforeChipDrag + 1,
          "with the detents off a drag must write exact velocities");
    doc.undoStack()->undo();
    (void)view.grab();
    // Leaving the PSG context rearms them: the chip is a choice about a
    // voice, not a mode the lane keeps.
    view.selectTrack(continuousTrack);
    (void)view.grab();
    check(lane->property("velocityDetents").toInt() == -1,
          "a continuous track must not carry the detent toggle");
    view.selectTrack(psgTrack);
    (void)view.grab();
    check(lane->property("velocityDetents").toInt() == 1 &&
              lane->property("velocityIntrinsic").toBool(),
          "coming back to a PSG voice must rearm its detents");

    // Hover: the node under the idle pointer is what the ruler describes,
    // ahead of the selection.
    view.clearSelection();
    (void)view.grab();
    check(lane->property("velocityMarkerCount").toInt() == 0,
          "an idle lane with nothing selected must mark nothing");
    sendLaneMouse(lane, QEvent::MouseMove, psgFrom, Qt::NoButton, Qt::NoButton);
    (void)view.grab();
    check(lane->property("velocityMarkerCount").toInt() == 1,
          "hovering a node must mark its value on the ruler");
    const ViewNote *hoverPartner = nullptr;
    for (const ViewNote &note : view.model().notes) {
        if (note.track == psgTrack && note.noteId.isAssigned() &&
            note.startTick != psgNote.startTick && note.velocity != psgNote.velocity)
            hoverPartner = &note;
        if (hoverPartner)
            break;
    }
    if (hoverPartner) {
        view.setSelection({psgId, {hoverPartner->startTick, hoverPartner->key}});
        sendLaneMouse(lane, QEvent::MouseMove, QPointF(psgX + 200.0, 4.0), Qt::NoButton,
                      Qt::NoButton);
        (void)view.grab();
        const int selectionMarkers = lane->property("velocityMarkerCount").toInt();
        sendLaneMouse(lane, QEvent::MouseMove, psgFrom, Qt::NoButton, Qt::NoButton);
        (void)view.grab();
        check(selectionMarkers == 2 && lane->property("velocityMarkerCount").toInt() == 1,
              "the hovered node must describe the ruler ahead of the selection");
    }
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(lane, &leave);
    view.clearSelection();
    (void)view.grab();
    check(lane->property("velocityMarkerCount").toInt() == 0,
          "leaving the lane must drop the hover's mark");
    // A track can change voice mid-song, so "is this PSG?" is a question
    // about a note, not about a track. This one has both kinds.
    const ViewNote *mixedProbe = nullptr;
    for (const ViewNote &note : view.model().notes) {
        if (note.track != psgTrack || !note.noteId.isAssigned())
            continue;
        const SongView::VoiceContext context = view.voiceContext(note.startTick);
        if (!VelocityMap::resolve(context.voice, note.key, context.trackVolume).isPsg())
            mixedProbe = &note;
        if (mixedProbe)
            break;
    }
    if (!mixedProbe) {
        fail("the mixed-voice fixture must find a note off the track's PSG voice");
        return failures == 0 ? 0 : 1;
    }
    const ViewNote mixedNote = *mixedProbe;
    // With the display position inside the sampled section the ruler is
    // continuous — until the pointer asks about a PSG note, which moves that
    // node onto its level's row. The press that follows must hit it there:
    // the hover is still what the lane is drawing.
    view.commitEditCursor(mixedNote.startTick);
    view.clearSelection();
    (void)view.grab();
    check(!lane->property("velocityIntrinsic").toBool(),
          "a display position on a sampled voice must keep the plain ruler");
    // A selection straddling both kinds of voice agrees on nothing, so the
    // ruler is continuous — and the hover is then the only thing that can
    // put it on levels, which is what makes the gesture below a question
    // about the hover alone.
    const SongView::NoteKey mixedId{mixedNote.startTick, mixedNote.key};
    view.setSelection({psgId, mixedId});
    (void)view.grab();
    check(!lane->property("velocityIntrinsic").toBool(),
          "a selection across two kinds of voice must keep the plain ruler");
    const int undoBeforeHoverDrag = doc.undoStack()->index();
    sendLaneMouse(lane, QEvent::MouseMove, psgFrom, Qt::NoButton, Qt::NoButton);
    (void)view.grab();
    check(lane->property("velocityIntrinsic").toBool() &&
              std::abs(laneLevelCenterY(lane, psgMap, psgLevel) - psgFrom.y()) < 0.5,
          "hovering a PSG node must put the ruler on that voice's levels");
    // And the gesture that starts there must mean what that ruler says: a
    // drag inside the hovered note's own level moves it by no levels at all,
    // and so leaves its exact velocity alone. Read continuously instead — as
    // the track's own context would — the same drag would canonicalize it
    // onto the level's representative and push an undo entry.
    sendLaneMouse(lane, QEvent::MouseButtonPress, psgFrom, Qt::LeftButton, Qt::LeftButton);
    const QPointF withinLevel(psgFrom.x(), psgFrom.y() + dragActivation + 2.0);
    if (laneVelocityAt(lane, withinLevel.y()) == psgNote.velocity ||
        psgMap.levelOf(laneVelocityAt(lane, withinLevel.y())) != psgMap.levelOf(psgNote.velocity)) {
        fail("the within-level fixture must move the value without leaving the level");
        return failures == 0 ? 0 : 1;
    }
    sendLaneMouse(lane, QEvent::MouseMove, withinLevel, Qt::NoButton, Qt::LeftButton);
    sendLaneMouse(lane, QEvent::MouseButtonRelease, withinLevel, Qt::LeftButton, Qt::NoButton);
    (void)view.grab();
    const ViewNote *hoverDragged = psgNoteAt();
    check(hoverDragged && hoverDragged->velocity == psgNote.velocity &&
              doc.undoStack()->index() == undoBeforeHoverDrag,
          "a gesture must keep the detents the hovered node's ruler was showing");
    view.commitEditCursor(0);
    view.clearSelection();
    (void)view.grab();

    // The detent choice is the track's, and the pointer is not the track:
    // passing over a note that plays on a sampled voice must not switch the
    // detents back on behind the user's back. The sampled section is off
    // screen, so bring it in and put the camera back after.
    const double mixedScroll = view.displayX(double(mixedNote.startTick), songview::kGutterW, dpr) -
                               double(songview::kGutterW + 200);
    view.scrollByPx(mixedScroll);
    (void)view.grab();
    const QPointF mixedPoint(view.displayX(double(mixedNote.startTick), songview::kGutterW, dpr),
                             laneVelocityY(lane, mixedNote.velocity));
    if (mixedPoint.x() < songview::kGutterW + 20 || mixedPoint.x() > lane->width() - 20) {
        fail("the mixed-voice fixture must be brought on screen");
        return failures == 0 ? 0 : 1;
    }
    clickDetentCheck();
    (void)view.grab();
    check(lane->property("velocityDetents").toInt() == 0,
          "the rearm fixture must start with the detents switched off");
    sendLaneMouse(lane, QEvent::MouseMove, mixedPoint, Qt::NoButton, Qt::NoButton);
    (void)view.grab();
    QEvent hoverLeave(QEvent::Leave);
    QCoreApplication::sendEvent(lane, &hoverLeave);
    (void)view.grab();
    check(lane->property("velocityDetents").toInt() == 0,
          "hovering a note off the detents' voice must not switch them back on");
    clickDetentCheck();
    view.scrollByPx(-mixedScroll);
    view.clearSelection();
    (void)view.grab();

    view.selectTrack(track);
    view.clearSelection();
    (void)view.grab();

    // --- the pane's height is per-song view state, and pre-lane sidecars load
    SongView::ViewState state = view.viewState();
    check(state.splitterSizes.size() == 3 && state.splitterSizes[1] == lane->height(),
          "view state must carry the lane's own pane size");
    SongView::ViewState legacy = state;
    legacy.splitterSizes = {400, 150};
    view.setVelocityLaneVisible(false); // as an old sidecar's song would open
    view.applyViewState(legacy);
    (void)view.grab();
    const QList<int> restored = splitter->sizes();
    // The lanes pane keeps the size the sidecar saved; the roll pane, the
    // stretchy one, absorbs the splitter's remaining height.
    check(restored.size() == 3 && restored[1] == 0 && restored[2] == 150 && restored[0] > 0,
          "a sidecar written before the lane must still restore its two panes");
    // With the app-wide preference on, the same sidecar must still open the
    // lane at a usable height instead of clamping it to its minimum.
    view.setVelocityLaneVisible(true);
    view.applyViewState(legacy);
    (void)view.grab();
    check(lane->height() > lane->minimumHeight() && splitter->sizes()[2] == 150,
          "a pre-lane sidecar must open a visible lane at its default height");

    if (!screenshotPath.isEmpty()) {
        // The PSG track, so the shot shows the detents the lane exists for.
        view.selectTrack(psgTrack);
        view.setVelocityLaneVisible(true);
        (void)view.grab();
        if (!view.grab().save(screenshotPath))
            fail("could not write the screenshot");
    }

    if (failures != 0) {
        std::fprintf(stderr, "velcheck: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("velcheck: PASS (velocity lane)\n");
    return 0;
}
