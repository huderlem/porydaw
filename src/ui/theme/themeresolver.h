#pragma once

#include "theme.h"

#include <QColor>

namespace themes {

// This is the only boundary where editable source colors become complete UI
// roles; controllers and paint code consume the resolved Theme.

/// Primary and Accent must be valid opaque colors with at least 3:1 contrast.
bool isValidColorPair(const QColor &primary, const QColor &accent);

/// Derives the complete runtime role table from Primary and Accent. The input
/// pair must satisfy isValidColorPair().
Theme derive(const QColor &primary, const QColor &accent);

/// Adjusts the Song View grid line color around the theme default at 50.
/// Values below 50 reduce contrast; values above 50 increase it.
Theme withGridLineContrast(Theme theme, int contrast);

/// Returns the deterministic fixed Vanilla role table.
Theme vanilla();
/// Returns the fixed Dark Neutral High role table.
Theme darkNeutralHigh();
/// Returns the fixed Immaterial role table.
Theme immaterial();

} // namespace themes
