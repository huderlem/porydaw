#pragma once

#include "theme.h"


namespace themes {

// This is the only boundary where editable source colors become complete UI
// roles; controllers and paint code consume the resolved Theme.


/// Returns the deterministic fixed Vanilla role table.
Theme vanilla();
/// Returns the fixed Dark Neutral Medium role table.
Theme darkNeutralMedium();
/// Returns the fixed Dark Neutral High role table.
Theme darkNeutralHigh();
/// Returns the fixed Immaterial role table.
Theme immaterial();

} // namespace themes
