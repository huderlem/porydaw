#include "trackidentitycolors.h"

#include <QStringView>

#include <array>
#include <utility>

namespace themes {
namespace {

using TrackIdentityColorPair = std::pair<QColor, QColor>;
using TrackIdentityPalette =
    std::array<TrackIdentityColorPair, trackIdentityColorCount>;

QColor colorFromHex(std::string_view hex) {
  return QColor::fromString(
      QLatin1StringView(hex.data(), static_cast<qsizetype>(hex.size())));
}

const TrackIdentityPalette &resolvedTrackIdentityPalette() {
  static const auto palette = [] {
    TrackIdentityPalette result;
    for (std::size_t index = 0; index < trackIdentityColorCount; ++index) {
      const auto fill = colorFromHex(track_identity_colors::fills[index]);
      const auto text = colorFromHex(track_identity_colors::texts[index]);
      result[index] = {fill, text};
    }
    return result;
  }();
  return palette;
}

} // namespace

const QColor &trackIdentityColor(std::size_t index) {
  return resolvedTrackIdentityPalette().at(index).first;
}

const QColor &trackIdentityTextColor(std::size_t index) {
  return resolvedTrackIdentityPalette().at(index).second;
}

} // namespace themes
