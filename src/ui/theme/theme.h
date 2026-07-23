#pragma once

#include "theme_roles.h"

#include <QColor>

#include <array>
#include <cstddef>

namespace themes {

/// A fully resolved role table. Producers fill every color, so paint code
/// never needs fallbacks or lookups.
struct Theme {
  std::array<QColor, roleCount> colors;

  QColor &color(Role role) { return colors.at(static_cast<std::size_t>(role)); }

  const QColor &color(Role role) const {
    return colors.at(static_cast<std::size_t>(role));
  }
};

} // namespace themes
