#include "color_math.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace themes {
namespace {

constexpr auto kPi = 3.141592653589793238462643383279502884;
constexpr auto kGamutEpsilon = 1e-6;

struct LinearRgb {
  double red;
  double green;
  double blue;
};

double clamp(double value, double minimum, double maximum) {
  return std::max(minimum, std::min(maximum, value));
}

double srgbToLinear(double channel) {
  return channel <= 0.04045 ? channel / 12.92
                            : std::pow((channel + 0.055) / 1.055, 2.4);
}

double linearToSrgb(double channel) {
  return channel <= 0.0031308
             ? 12.92 * channel
             : 1.055 * std::pow(std::max(0.0, channel), 1.0 / 2.4) - 0.055;
}

LinearRgb toLinearRgb(const Oklab &lab) {
  const auto l = lab.lightness + 0.3963377774 * lab.a + 0.2158037573 * lab.b;
  const auto m = lab.lightness - 0.1055613458 * lab.a - 0.0638541728 * lab.b;
  const auto s = lab.lightness - 0.0894841775 * lab.a - 1.2914855480 * lab.b;
  const auto lCubed = l * l * l;
  const auto mCubed = m * m * m;
  const auto sCubed = s * s * s;
  return {
      4.0767416621 * lCubed - 3.3077115913 * mCubed + 0.2309699292 * sCubed,
      -1.2684380046 * lCubed + 2.6097574011 * mCubed - 0.3413193965 * sCubed,
      -0.0041960863 * lCubed - 0.7034186147 * mCubed + 1.7076147010 * sCubed};
}

int gammaChannel(double channel) {
  const auto encoded = linearToSrgb(channel);
  const auto scaled = clamp(encoded * 255.0, 0.0, 255.0);
  return static_cast<int>(std::floor(scaled + 0.5));
}

QColor colorFromLinearRgb(const LinearRgb &rgb, int alpha = 255) {
  return QColor::fromRgb(gammaChannel(rgb.red), gammaChannel(rgb.green),
                         gammaChannel(rgb.blue), alpha);
}

double linearChannel(int channel) {
  static const auto channels = [] {
    std::array<double, 256> result{};
    for (auto index = std::size_t{0}; index < result.size(); ++index)
      result[index] =
          srgbToLinear(static_cast<double>(index) / (result.size() - 1));
    return result;
  }();
  Q_ASSERT(channel >= 0 && channel < static_cast<int>(channels.size()));
  return channels[static_cast<std::size_t>(channel)];
}

SrgbSample encodedSample(const LinearRgb &rgb) {
  return {gammaChannel(clamp(rgb.red, 0.0, 1.0)),
          gammaChannel(clamp(rgb.green, 0.0, 1.0)),
          gammaChannel(clamp(rgb.blue, 0.0, 1.0))};
}

bool finite(double value) { return std::isfinite(value); }

bool inUnitInterval(double value) {
  return finite(value) && value >= -kGamutEpsilon &&
         value <= 1.0 + kGamutEpsilon;
}

} // namespace

Oklab oklabFromColor(const QColor &color) {
  const auto rgb = color.toRgb();
  const auto red = srgbToLinear(rgb.redF());
  const auto green = srgbToLinear(rgb.greenF());
  const auto blue = srgbToLinear(rgb.blueF());
  const auto l = std::cbrt(0.4122214708 * red + 0.5363325363 * green +
                           0.0514459929 * blue);
  const auto m = std::cbrt(0.2119034982 * red + 0.6806995451 * green +
                           0.1073969566 * blue);
  const auto s = std::cbrt(0.0883024619 * red + 0.2817188376 * green +
                           0.6299787005 * blue);
  return {0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
          1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
          0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
}

QColor colorFromOklab(const Oklab &color, int alpha) {
  return colorFromLinearRgb(toLinearRgb(color), alpha);
}

Oklch oklchFromColor(const QColor &color) {
  const auto lab = oklabFromColor(color);
  const auto chroma = std::hypot(lab.a, lab.b);
  auto hue = std::atan2(lab.b, lab.a) * 180.0 / kPi;
  if (hue < 0.0)
    hue += 360.0;
  if (hue >= 360.0)
    hue -= 360.0;
  return {lab.lightness, chroma, chroma == 0.0 ? 0.0 : hue};
}

QColor colorFromOklch(const Oklch &color) {
  SrgbSample sample;
  if (!sampleSrgb(color, sample))
    return {};
  return QColor::fromRgb(sample.red, sample.green, sample.blue);
}

bool sampleSrgb(const Oklab &color, SrgbSample &sample) {
  if (!finite(color.lightness) || !finite(color.a) || !finite(color.b))
    return false;
  const auto rgb = toLinearRgb(color);
  if (!inUnitInterval(rgb.red) || !inUnitInterval(rgb.green) ||
      !inUnitInterval(rgb.blue))
    return false;
  sample = encodedSample(rgb);
  return true;
}

bool sampleSrgb(const Oklch &color, SrgbSample &sample) {
  if (!finite(color.lightness) || !finite(color.chroma) || !finite(color.hue) ||
      color.lightness < 0.0 || color.lightness > 1.0 || color.chroma < 0.0 ||
      color.hue < 0.0 || color.hue >= 360.0)
    return false;
  const auto radians = color.hue * kPi / 180.0;
  return sampleSrgb(Oklab{color.lightness, color.chroma * std::cos(radians),
                          color.chroma * std::sin(radians)},
                    sample);
}

double oklabLightness(const QColor &color) {
  return oklabFromColor(color).lightness;
}

QColor shiftOklabLightness(const QColor &color, double distance) {
  const auto lab = oklabFromColor(color);
  return colorFromOklab(
      {clamp(lab.lightness + distance, 0.0, 1.0), lab.a, lab.b}, color.alpha());
}

double relativeLuminance(const QColor &color) {
  const auto rgb = color.toRgb();
  return 0.2126 * srgbToLinear(rgb.redF()) +
         0.7152 * srgbToLinear(rgb.greenF()) +
         0.0722 * srgbToLinear(rgb.blueF());
}

double relativeLuminance(const SrgbSample &sample) {
  return 0.2126 * linearChannel(sample.red) +
         0.7152 * linearChannel(sample.green) +
         0.0722 * linearChannel(sample.blue);
}

double contrastRatioFromLuminance(double first, double second) {
  const auto lighter = std::max(first, second);
  const auto darker = std::min(first, second);
  return (lighter + 0.05) / (darker + 0.05);
}

double contrastRatio(const QColor &first, const QColor &second) {
  return contrastRatioFromLuminance(relativeLuminance(first),
                                    relativeLuminance(second));
}

} // namespace themes
