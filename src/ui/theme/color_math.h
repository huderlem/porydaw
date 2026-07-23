#pragma once

#include <QColor>

namespace themes {

/// CIE OKLab coordinates. Lightness is in [0, 1].
struct Oklab {
  double lightness = 0.0;
  double a = 0.0;
  double b = 0.0;
};

/// CIE OKLCh coordinates. Lightness is in [0, 1], chroma is non-negative,
/// and hue is expressed in degrees in [0, 360).
struct Oklch {
  double lightness = 0.0;
  double chroma = 0.0;
  double hue = 0.0;
};

/// Encoded sRGB channels. This avoids constructing QColor values in raster
/// loops.
struct SrgbSample {
  int red = 0;
  int green = 0;
  int blue = 0;
};

/// Converts an opaque sRGB color to OKLab.
Oklab oklabFromColor(const QColor &color);
/// Converts OKLab coordinates to sRGB, clipping to gamut and using alpha.
QColor colorFromOklab(const Oklab &color, int alpha = 255);
/// Converts an opaque sRGB color to OKLCh.
Oklch oklchFromColor(const QColor &color);
/// Converts an in-gamut OKLCh value to an opaque sRGB color. An invalid QColor
/// is returned when the requested color is outside the sRGB gamut.
QColor colorFromOklch(const Oklch &color);
/// Converts an in-gamut OKLab or OKLCh value to encoded sRGB channels. Returns
/// false for non-finite coordinates or colors outside the sRGB gamut.
bool sampleSrgb(const Oklab &color, SrgbSample &sample);
bool sampleSrgb(const Oklch &color, SrgbSample &sample);
/// Returns the OKLab lightness component of an sRGB color.
double oklabLightness(const QColor &color);
/// Shifts an sRGB color's OKLab lightness while preserving its chroma and hue.
/// The result is clipped to the sRGB gamut.
QColor shiftOklabLightness(const QColor &color, double distance);
/// Returns the WCAG relative luminance of an sRGB color.
double relativeLuminance(const QColor &color);
double relativeLuminance(const SrgbSample &sample);
/// Returns the WCAG contrast ratio between two relative luminance values.
double contrastRatioFromLuminance(double first, double second);
/// Returns the WCAG contrast ratio between two colors.
double contrastRatio(const QColor &first, const QColor &second);
} // namespace themes
