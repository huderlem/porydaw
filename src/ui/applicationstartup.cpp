#include "applicationstartup.h"

#include "layout.h"
#include "theme/themeruntime.h"
#include "typography.h"

#include <QApplication>

namespace ui {
// Kept separate so startup ordering can be tested.
bool initializeApplication(QApplication &application)
{
    // Ordering is load-bearing: typography first captures the platform's
    // runtime font size before installing the bundled fonts. Using that size as
    // Layout's base font pixel size keeps controls and spacing proportional to
    // text across platform DPI and system text-scale settings. Layout installs
    // geometry and popup behavior, then Theme captures the resulting
    // application presentation baseline.
    if (!typography::installBundledFonts(application))
        return false;
    const auto baseFontPx = typography::baseFontPx();
    if (!baseFontPx || !layout::initialize(application, *baseFontPx))
        return false;
    themes::initialize(application);
    return true;
}

} // namespace ui
