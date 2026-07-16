#include "ui/layout.h"
#include "ui/typography.h"

#include <QApplication>
#include <QFontInfo>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QtGlobal>

#include <array>
#include <cstdio>

int runFontCheck(int expectedBaseFontPx) {
  auto *application = qobject_cast<QApplication *>(QApplication::instance());
  if (!application)
    return 1;
  auto failures = 0;
  const auto check = [&failures](bool condition, const char *message) {
    if (!condition) {
      std::fprintf(stderr, "fontcheck: FAIL: %s\n", message);
      ++failures;
    }
  };
  const auto baseFontPx = typography::baseFontPx();
  check(baseFontPx.has_value(),
        "Typography did not capture the base font pixel size");
  if (!baseFontPx)
    return 1;
  struct ExpectedSpace {
    layout::Space token;
    double multiplier;
  };
  constexpr auto expectedSpaces = std::array{
      ExpectedSpace{layout::Space::Zero, 0.0},
      ExpectedSpace{layout::Space::Half, 0.125},
      ExpectedSpace{layout::Space::One, 0.25},
      ExpectedSpace{layout::Space::Two, 0.5},
      ExpectedSpace{layout::Space::Three, 0.75},
      ExpectedSpace{layout::Space::Four, 1.0},
      ExpectedSpace{layout::Space::Six, 1.5},
      ExpectedSpace{layout::Space::Eight, 2.0},
  };
  const auto checkSpaces = [&] {
    for (const auto &expected : expectedSpaces) {
      const auto value = expected.multiplier == 0.0
                             ? 0
                             : qMax(1, qRound(*baseFontPx * expected.multiplier));
      check(layout::space(expected.token) == value,
            "Layout spacing token has the wrong resolved value");
    }
  };
  const auto styleSheet = application->styleSheet();
  checkSpaces();
  check(layout::initialize(*application, *baseFontPx),
        "Layout initialization is not idempotent");
  check(!layout::initialize(*application, 0),
        "Layout accepted an invalid base font pixel size");
  check(!layout::initialize(*application, *baseFontPx + 1),
        "Layout accepted a conflicting base font pixel size");
  check(application->styleSheet() == styleSheet,
        "Repeated Layout initialization changed established geometry");
  checkSpaces();
  check(expectedBaseFontPx > 0 && *baseFontPx == expectedBaseFontPx,
        "Typography did not preserve the pre-install application font size");
  const auto body = QApplication::font();
  const auto bodyInfo = QFontInfo(body);
  const auto expectedBodySize = qMax(1, qRound(*baseFontPx * 1.25));
  check(bodyInfo.family() == QStringLiteral("Atkinson Hyperlegible Next") &&
            bodyInfo.pixelSize() == expectedBodySize &&
            bodyInfo.weight() == QFont::Normal,
        "Body has the wrong face, size, or weight");
  const auto monoInfo = QFontInfo(typography::bodyMono(body));
  check(monoInfo.family() == QStringLiteral("Atkinson Hyperlegible Mono") &&
            monoInfo.pixelSize() == expectedBodySize &&
            monoInfo.weight() == QFont::Normal,
        "Body Mono has the wrong face, size, or weight");
  const auto caption = typography::caption(body);
  const auto captionInfo = QFontInfo(caption);
  check(captionInfo.family() == QStringLiteral("Atkinson Hyperlegible Next") &&
            captionInfo.pixelSize() == *baseFontPx &&
            captionInfo.weight() == QFont::Normal,
        "Caption has the wrong face, size, or weight");
  const auto bodyBoldInfo = QFontInfo(typography::bold(body));
  check(bodyBoldInfo.pixelSize() == bodyInfo.pixelSize() &&
            bodyBoldInfo.weight() > bodyInfo.weight(),
        "Body Bold changed size or failed to increase weight");
  const auto captionBoldInfo = QFontInfo(typography::bold(caption));
  check(captionBoldInfo.pixelSize() == captionInfo.pixelSize() &&
            captionBoldInfo.weight() > captionInfo.weight(),
        "Caption Bold changed size or failed to increase weight");
  const auto captionHeight = QFontMetrics(caption).height();
  for (auto height = 0; height <= captionHeight + 4; ++height) {
    const auto fitted = typography::fitted(body, height);
    if (!fitted)
      continue;
    const auto size = QFontInfo(*fitted).pixelSize();
    const auto metrics = QFontMetrics(*fitted);
    check(size <= captionInfo.pixelSize() &&
              metrics.ascent() + metrics.descent() <= height,
          "Fitted text exceeds its size or height limit");
    if (size < captionInfo.pixelSize()) {
      auto larger = *fitted;
      larger.setPixelSize(size + 1);
      const auto largerMetrics = QFontMetrics(larger);
      check(largerMetrics.ascent() + largerMetrics.descent() > height,
            "Fitted text is not maximal");
    }
  }
  check(!typography::fitted(body, 0),
        "Fitted text exists without positive available height");
  const auto selected = typography::bold(body);
  const auto text = QStringLiteral("1 · Tést g̦");
  const auto referenceBounds = QFontMetricsF(body).tightBoundingRect(text);
  const auto displayedBounds = QFontMetricsF(selected).tightBoundingRect(text);
  check(displayedBounds
                .translated(
                    typography::glyphCenteringOffset(body, selected, text))
                .center() == referenceBounds.center(),
        "Displayed glyph bounds are not centered");
  if (failures == 0)
    std::printf("fontcheck: PASS\n");
  return failures == 0 ? 0 : 1;
}
