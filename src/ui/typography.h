#pragma once

#include <QFont>
#include <QPointF>
#include <QStringView>

#include <optional>

class QApplication;

namespace typography {

/// Installs the application typeface while preserving the platform-resolved
/// base font pixel size for the semantic font scale.
bool installBundledFonts(QApplication &application);
/// Returns the platform-resolved base font pixel size captured during font
/// installation.
std::optional<int> baseFontPx();
QFont bodyMono(const QFont &body);
QFont caption(const QFont &source);
QFont bold(const QFont &source);
/// Finds the largest caption-or-smaller face that fits the available height.
/// Failure is explicit so callers cannot silently paint clipped text.
std::optional<QFont> fitted(const QFont &base, int availableHeight);
/// Aligns the visible bounds of a substituted glyph with the reference face.
QPointF glyphCenteringOffset(const QFont &reference, const QFont &displayed,
                             QStringView text);

} // namespace typography
