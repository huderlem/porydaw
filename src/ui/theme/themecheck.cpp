#include "ui/theme/themecheck.h"

#include "ui/theme/color_math.h"

#include "ui/theme/themecontroller.h"
#include "ui/theme/themepage.h"
#include "ui/theme/themeresolver.h"
#include "ui/theme/themeruntime.h"
#include "ui/theme/trackidentitycolors.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPaintEvent>
#include <QPixmap>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QSettings>
#include <QSlider>
#include <QStyleOptionComboBox>
#include <QTabBar>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QWizard>

#include <cmath>

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

namespace {

class Reporter
{
  public:
    void check(bool condition, const char *message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "themecheck: FAIL: %s\n", message);
        ++m_failures;
    }

    int finish() const
    {
        std::printf(m_failures == 0 ? "themecheck: PASS\n" : "themecheck: FAIL\n");
        return m_failures == 0 ? 0 : 1;
    }

  private:
    int m_failures = 0;
};

class StyleChangeCounter final : public QObject
{
  public:
    int count = 0;

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event && event->type() == QEvent::StyleChange)
            ++count;
        return QObject::eventFilter(watched, event);
    }
};

class ThemeRefreshProbe final : public QWidget
{
  public:
    int themeChangeCount = 0;
    int paintCount = 0;
    QColor gridColor;

  protected:
    bool event(QEvent *event) override
    {
        if (event && event->type() == QEvent::ThemeChange) {
            ++themeChangeCount;
            gridColor = themes::color(themes::Role::song_view_grid);
        }
        return QWidget::event(event);
    }

    void paintEvent(QPaintEvent *) override { ++paintCount; }
};

// Paint code has no fallback path: every public role must resolve to a valid
// color. Only the grid may be translucent.
bool isComplete(const themes::Theme &theme)
{
    for (std::size_t index = 0; index < theme.colors.size(); ++index) {
        const auto color = theme.colors[index];
        const auto role = static_cast<themes::Role>(index);
        if (!color.isValid() || (role != themes::Role::song_view_grid && color.alpha() != 255))
            return false;
    }
    return true;
}

// Track identity colors are application-wide content identity, not theme
// state, so validate the single shared palette directly. Note labels ink with
// the stronger piano-key color over the fill, and the keyboard reads the same
// black-and-white in every theme, so vanilla's keys stand in for all of them.
bool isCompleteTrackIdentityPalette()
{
    const auto theme = themes::vanilla();
    const auto &light = theme.color(themes::Role::song_view_piano_keyboard_natural_key);
    const auto &dark = theme.color(themes::Role::song_view_piano_keyboard_black_key);
    for (std::size_t index = 0; index < themes::trackIdentityColorCount; ++index) {
        const auto &fill = themes::trackIdentityColor(index);
        if (!fill.isValid() || fill.alpha() != 255 ||
            std::max(themes::contrastRatio(fill, light), themes::contrastRatio(fill, dark)) < 3.0)
            return false;
    }
    return true;
}

void checkMenuBarStateContrast(Reporter &reporter, const themes::Theme &theme)
{
    reporter.check(themes::contrastRatio(theme.color(themes::Role::menu_bar_text),
                                         theme.color(themes::Role::menu_bar_background)) >= 4.5,
                   "menu-bar text is unreadable in its resting state");
    reporter.check(themes::contrastRatio(theme.color(themes::Role::button_hover_text),
                                         theme.color(themes::Role::button_hover_background)) >= 4.5,
                   "menu-bar text is unreadable in its hover state");
    reporter.check(themes::contrastRatio(theme.color(themes::Role::button_pressed_text),
                                         theme.color(themes::Role::button_pressed_background)) >=
                       4.5,
                   "menu-bar text is unreadable in its pressed state");
}

void checkSharedControlColors(Reporter &reporter, const themes::Theme &theme)
{
    reporter.check(theme.color(themes::Role::combo_drop_down_pressed_background) ==
                       theme.color(themes::Role::button_pressed_background),
                   "a ComboBox arrow lane does not use the pressed-button color");
    reporter.check(theme.color(themes::Role::menu_background) ==
                           theme.color(themes::Role::item_background) &&
                       theme.color(themes::Role::menu_text) == theme.color(themes::Role::item_text),
                   "a popup menu does not use the item-view surface and text colors");
    reporter.check(theme.color(themes::Role::menu_item_hover_background) ==
                           theme.color(themes::Role::item_hover_background) &&
                       theme.color(themes::Role::menu_item_hover_text) ==
                           theme.color(themes::Role::item_hover_text),
                   "a popup menu does not use the item-view hover colors");
    reporter.check(themes::contrastRatio(theme.color(themes::Role::menu_item_hover_text),
                                         theme.color(themes::Role::menu_item_hover_background)) >=
                       4.5,
                   "popup-menu text is unreadable in its hover state");
    reporter.check(theme.color(themes::Role::splitter_handle_hover_background) ==
                       theme.color(themes::Role::button_pressed_background),
                   "an active splitter handle does not use the pressed-button color");
    reporter.check(
        theme.color(themes::Role::input_background) ==
                theme.color(themes::Role::combo_background) &&
            theme.color(themes::Role::input_text) == theme.color(themes::Role::combo_text) &&
            theme.color(themes::Role::input_outline) == theme.color(themes::Role::combo_outline),
        "a text field does not share the combo field surface");
    reporter.check(
        theme.color(themes::Role::spin_box_background) ==
                theme.color(themes::Role::combo_background) &&
            theme.color(themes::Role::spin_box_text) == theme.color(themes::Role::combo_text) &&
            theme.color(themes::Role::spin_box_outline) == theme.color(themes::Role::combo_outline),
        "a spin-box field does not share the combo field surface");
    reporter.check(themes::contrastRatio(theme.color(themes::Role::combo_text),
                                         theme.color(themes::Role::combo_background)) >= 4.5,
                   "field text is unreadable on the shared field surface");
}

void checkDerivedThemes(Reporter &reporter)
{
    reporter.check(themes::isValidColorPair(QColor("#000000"), QColor("#FFFFFF")),
                   "a valid color pair was rejected");
    reporter.check(!themes::isValidColorPair(QColor("#777777"), QColor("#888888")),
                   "a low-contrast color pair was accepted");
    auto translucent = QColor("#FFFFFF");
    translucent.setAlpha(128);
    reporter.check(!themes::isValidColorPair(QColor("#000000"), translucent),
                   "a translucent color was accepted");

    reporter.check(isCompleteTrackIdentityPalette(),
                   "the shared track identity palette has an unreadable label color");

    // Exercise both dark and light Primary directions through the same contract.
    const auto customPairs = std::array{
        std::pair{QColor("#2B2D31"), QColor("#66CCFF")},
        std::pair{QColor("#F2F2F2"), QColor("#0055AA")},
    };
    for (const auto &[primary, accent] : customPairs) {
        const auto theme = themes::derive(primary, accent);
        reporter.check(isComplete(theme), "a derived theme has an unset or translucent role");
        checkMenuBarStateContrast(reporter, theme);
        checkSharedControlColors(reporter, theme);
        // Disabled text must be legible on the window surface yet clearly
        // dimmer than enabled text; a derived theme once returned the most
        // text-like readable candidate, making disabled items look enabled.
        const auto disabledText = theme.color(themes::Role::disabled_text);
        const auto windowText = theme.color(themes::Role::window_text);
        reporter.check(themes::contrastRatio(disabledText,
                                             theme.color(themes::Role::window_background)) >= 4.5,
                       "derived disabled text is not readable on the window surface");
        reporter.check(themes::contrastRatio(disabledText, windowText) >= 1.3,
                       "derived disabled text is indistinguishable from enabled");
        const auto softened = themes::withGridLineContrast(theme, 0);
        const auto unchanged = themes::withGridLineContrast(theme, 50);
        const auto strengthened = themes::withGridLineContrast(theme, 100);
        const auto background = theme.color(themes::Role::song_view_piano_roll_background);
        reporter.check(unchanged.color(themes::Role::song_view_grid) ==
                           theme.color(themes::Role::song_view_grid),
                       "the default grid line contrast changed a derived theme");
        reporter.check(
            themes::contrastRatio(softened.color(themes::Role::song_view_grid), background) <
                    themes::contrastRatio(theme.color(themes::Role::song_view_grid), background) &&
                themes::contrastRatio(strengthened.color(themes::Role::song_view_grid),
                                      background) >
                    themes::contrastRatio(theme.color(themes::Role::song_view_grid), background),
            "grid line contrast is not adjustable around the default");
    }
    const auto fixedThemes = std::array{
        themes::vanilla(),
        themes::darkNeutralHigh(),
        themes::immaterial(),
    };
    reporter.check(themes::vanilla().color(themes::Role::song_view_grid) ==
                       QColor::fromRgb(0x04, 0x00, 0x00, 0x3F),
                   "Vanilla does not use its authored grid line color");
    reporter.check(themes::darkNeutralHigh().color(themes::Role::song_view_grid) ==
                       QColor::fromRgb(0x03, 0x03, 0x03, 0x54),
                   "Dark Neutral High does not use its authored grid line color");
    reporter.check(themes::immaterial().color(themes::Role::song_view_grid) ==
                       QColor::fromRgb(0x03, 0x06, 0x06, 0x54),
                   "Immaterial does not use its authored grid line color");
    for (const auto &theme : fixedThemes) {
        reporter.check(isComplete(theme), "a fixed theme has an unset or invalid role");
        checkMenuBarStateContrast(reporter, theme);
        checkSharedControlColors(reporter, theme);
        reporter.check(theme.color(themes::Role::combo_drop_down_hover_background) ==
                           theme.color(themes::Role::button_hover_background),
                       "a fixed theme gives the ComboBox arrow lane a selection hover color");
        reporter.check(isComplete(themes::withGridLineContrast(theme, 0)) &&
                           isComplete(themes::withGridLineContrast(theme, 100)),
                       "adjusting grid line contrast made a theme incomplete");
        const auto grid = theme.color(themes::Role::song_view_grid);
        const auto background = theme.color(themes::Role::song_view_piano_roll_background);
        const auto softened =
            themes::withGridLineContrast(theme, 0).color(themes::Role::song_view_grid);
        const auto strengthened =
            themes::withGridLineContrast(theme, 100).color(themes::Role::song_view_grid);
        reporter.check(softened.alpha() < grid.alpha() && strengthened.alpha() > grid.alpha(),
                       "grid line contrast does not adjust opacity");
        reporter.check(
            (themes::relativeLuminance(grid) <= themes::relativeLuminance(background) &&
             themes::relativeLuminance(strengthened) < themes::relativeLuminance(grid)) ||
                (themes::relativeLuminance(grid) > themes::relativeLuminance(background) &&
                 themes::relativeLuminance(strengthened) > themes::relativeLuminance(grid)),
            "higher contrast reversed a fixed theme's grid line direction");
        reporter.check(theme.color(themes::Role::focus_outline) ==
                           theme.color(themes::Role::palette_outline),
                       "a fixed theme does not use a neutral focus outline");
        // Same bar the derived path holds: on the dark presets disabled text
        // once sat a bare 1.26:1 from enabled, so disabled transport glyphs
        // looked live.
        reporter.check(themes::contrastRatio(theme.color(themes::Role::disabled_text),
                                             theme.color(themes::Role::window_text)) >= 1.3,
                       "a fixed theme's disabled text is indistinguishable from enabled");
        reporter.check(theme.color(themes::Role::tab_pane_background) ==
                           theme.color(themes::Role::toolbar_background),
                       "the fixed theme open-song tab gutter does not use theme "
                       "chrome");
    }
    // Roles that render text or affordances directly on the automation lane
    // surface must clear the WCAG UI-component contrast bar in every theme;
    // the fixed themes shipped with accent-on-surface readouts once already.
    auto laneLegible = [&](const themes::Theme &theme, const char *what) {
        const auto surface = theme.color(themes::Role::song_view_piano_roll_background);
        for (const auto role : {themes::Role::song_view_edit_preview_outline,
                                themes::Role::song_view_add_automation_lane_action})
            reporter.check(themes::contrastRatio(theme.color(role), surface) >= 3.0, what);
    };
    laneLegible(themes::vanilla(), "a Vanilla lane readout is unreadable");
    laneLegible(themes::darkNeutralHigh(), "a Dark Neutral High lane readout is unreadable");
    laneLegible(themes::immaterial(), "an Immaterial lane readout is unreadable");
    laneLegible(themes::derive(QColor("#2B2D31"), QColor("#66CCFF")),
                "a dark derived lane readout is unreadable");
    laneLegible(themes::derive(QColor("#F2F2F2"), QColor("#0055AA")),
                "a light derived lane readout is unreadable");
    // Sample Editor inks paint on the item surface (waveform trace, crop and
    // loop handles) and the seam inset's alternate surface (loop and seam-end
    // traces); Vanilla once shipped the trace at 1.16:1 and the handles near
    // 1.5:1 by borrowing palette colors and identity tints.
    auto waveformLegible = [&](const themes::Theme &theme, const char *what) {
        const auto item = theme.color(themes::Role::item_background);
        const auto alternate = theme.color(themes::Role::item_alternate_background);
        const auto legible = [&](themes::Role role, const QColor &surface) {
            return themes::contrastRatio(theme.color(role), surface) >= 3.0;
        };
        reporter.check(legible(themes::Role::sample_waveform_ink, item) &&
                           legible(themes::Role::sample_crop_handle, item) &&
                           legible(themes::Role::sample_loop_handle, item) &&
                           legible(themes::Role::sample_loop_handle, alternate) &&
                           legible(themes::Role::sample_seam_end_ink, alternate),
                       what);
    };
    waveformLegible(themes::vanilla(), "a Vanilla Sample Editor ink is unreadable");
    waveformLegible(themes::darkNeutralHigh(),
                    "a Dark Neutral High Sample Editor ink is unreadable");
    waveformLegible(themes::immaterial(), "an Immaterial Sample Editor ink is unreadable");
    waveformLegible(themes::derive(QColor("#2B2D31"), QColor("#66CCFF")),
                    "a dark derived Sample Editor ink is unreadable");
    waveformLegible(themes::derive(QColor("#F2F2F2"), QColor("#0055AA")),
                    "a light derived Sample Editor ink is unreadable");
}

void checkThemeWorkflow(Reporter &reporter, QApplication &application)
{
    QTemporaryDir directory;
    reporter.check(directory.isValid(), "could not create temporary theme settings");
    if (!directory.isValid())
        return;
    reporter.check(
        application.styleSheet().contains(QStringLiteral("QHeaderView::section{border:0;}")),
        "Layout did not install permanent zero-border headers at startup");
    QWizard wizard;
    wizard.ensurePolished();
    reporter.check(wizard.wizardStyle() == QWizard::ClassicStyle,
                   "the application did not enforce ClassicStyle for QWizard");
    const auto settingsPath = directory.filePath(QStringLiteral("settings.ini"));
    QSettings settings(settingsPath, QSettings::IniFormat);
    settings.setValue(QStringLiteral("theme/mode"), QStringLiteral("custom"));
    settings.setValue(QStringLiteral("theme/primary"), QStringLiteral("#000000"));
    settings.setValue(QStringLiteral("theme/grid-line-contrast"), QStringLiteral("invalid"));
    themes::ThemeController controller(application, settings);
    controller.restore();
    reporter.check(
        application.styleSheet().contains(QStringLiteral("QHeaderView::section{border:0;}")),
        "applying a theme dropped Layout's zero-border header rule");
    reporter.check(controller.committedSelection().mode == themes::ThemeMode::Vanilla,
                   "invalid settings did not restore the Vanilla theme");
    reporter.check(settings.value(QStringLiteral("theme/mode")).toString() ==
                           QStringLiteral("vanilla") &&
                       !settings.contains(QStringLiteral("theme/primary")) &&
                       !settings.contains(QStringLiteral("theme/accent")) &&
                       settings.value(QStringLiteral("theme/grid-line-contrast")).toInt() ==
                           themes::defaultGridLineContrast,
                   "invalid stored theme settings were not repaired");
    QComboBox combo;
    combo.addItem(QStringLiteral("Arrow"));
    combo.resize(120, 30);
    combo.show();
    application.processEvents();
    QStyleOptionComboBox comboOption;
    comboOption.initFrom(&combo);
    const auto arrowRect = combo.style()->subControlRect(QStyle::CC_ComboBox, &comboOption,
                                                         QStyle::SC_ComboBoxArrow, &combo);
    const auto comboImage = combo.grab().toImage();
    const auto arrowColor = themes::color(themes::Role::combo_text);
    auto foundArrowColor = false;
    for (auto y = arrowRect.top(); y <= arrowRect.bottom(); ++y) {
        for (auto x = arrowRect.left(); x <= arrowRect.right(); ++x)
            foundArrowColor = foundArrowColor || comboImage.pixelColor(x, y) == arrowColor;
    }
    reporter.check(foundArrowColor, "the ComboBox arrow was not painted with theme text color");
    combo.setAttribute(Qt::WA_UnderMouse);
    QCursor::setPos(combo.mapToGlobal(arrowRect.center()));
    combo.update();
    application.processEvents();
    const auto hoveredComboImage = combo.grab().toImage();
    const auto hoverColor = themes::color(themes::Role::combo_drop_down_hover_background);
    auto foundHoverColor = false;
    for (auto y = arrowRect.top(); y <= arrowRect.bottom(); ++y) {
        for (auto x = arrowRect.left(); x <= arrowRect.right(); ++x)
            foundHoverColor = foundHoverColor || hoveredComboImage.pixelColor(x, y) == hoverColor;
    }
    reporter.check(foundHoverColor, "the painted ComboBox arrow lane lost its hover color");
    // The open popup floats over arbitrary content, so its list carries the
    // menu outline. The view fills its popup container, making the
    // container's corner pixel the list's border.
    combo.showPopup();
    application.processEvents();
    auto *popup = combo.view()->window();
    const auto popupOpen = popup != combo.window() && popup->isVisible();
    reporter.check(popupOpen, "the ComboBox popup did not open");
    if (popupOpen) {
        const auto popupImage = popup->grab().toImage();
        reporter.check(popupImage.pixelColor(0, 0) == themes::color(themes::Role::menu_outline),
                       "the open ComboBox popup list is missing the menu outline");
    }
    combo.hidePopup();
    application.processEvents();
    // The event list's playhead tint and the polyphony debugger's drop
    // flash arrive as model background brushes; a stylesheet ::item
    // background once painted over both. Alternate-row fills must still
    // come from the theme.
    QTableWidget table(2, 1);
    table.horizontalHeader()->hide();
    table.verticalHeader()->hide();
    table.setShowGrid(false);
    table.setAlternatingRowColors(true);
    table.setItem(0, 0, new QTableWidgetItem);
    table.setItem(1, 0, new QTableWidgetItem);
    table.resize(80, 80);
    table.show();
    application.processEvents();
    const auto cellColor = [&](int row) {
        const auto rect = table.visualItemRect(table.item(row, 0));
        return table.viewport()->grab().toImage().pixelColor(rect.center());
    };
    reporter.check(cellColor(0) == themes::color(themes::Role::item_background),
                   "an item view row is not filled with the theme item "
                   "background");
    reporter.check(cellColor(1) == themes::color(themes::Role::item_alternate_background),
                   "an alternate item view row lost the theme alternate fill");
    const auto flash = QColor(255, 0, 0);
    table.item(0, 0)->setBackground(flash);
    reporter.check(cellColor(0) == flash, "a model background brush is painted over by the item "
                                          "stylesheet (playhead tint, polyphony flash)");
    const auto ink = QColor(255, 0, 255);
    table.item(1, 0)->setText(QStringLiteral("XXXX"));
    table.item(1, 0)->setForeground(ink);
    // The scan below wants a pixel that is exactly the ink color, but whether
    // an antialiased glyph keeps any full-coverage pixel depends on the host
    // rasterizer (FreeType version, fontconfig hinting). Render this probe
    // aliased and large so every drawn pixel carries the foreground verbatim.
    auto inkFont = table.font();
    inkFont.setPixelSize(24);
    inkFont.setStyleStrategy(QFont::NoAntialias);
    table.item(1, 0)->setFont(inkFont);
    const auto inkImage = table.viewport()->grab().toImage();
    auto foundInk = false;
    for (auto y = 0; y < inkImage.height() && !foundInk; ++y) {
        for (auto x = 0; x < inkImage.width() && !foundInk; ++x)
            foundInk = inkImage.pixelColor(x, y) == ink;
    }
    reporter.check(foundInk, "a model foreground brush is overridden by the item "
                             "stylesheet (polyphony event log severity colors)");
    themes::ThemePage dialog(controller);
    auto *custom = dialog.findChild<QRadioButton *>(QStringLiteral("customModeButton"));
    auto *darkNeutralHigh =
        dialog.findChild<QRadioButton *>(QStringLiteral("darkNeutralHighModeButton"));
    auto *immaterial = dialog.findChild<QRadioButton *>(QStringLiteral("immaterialModeButton"));
    auto *primary = dialog.findChild<QLineEdit *>(QStringLiteral("primaryHexEdit"));
    auto *accent = dialog.findChild<QLineEdit *>(QStringLiteral("accentHexEdit"));
    auto *gridLineContrast = dialog.findChild<QSlider *>(QStringLiteral("gridLineContrastSlider"));
    const auto controlsFound =
        custom && darkNeutralHigh && immaterial && primary && accent && gridLineContrast;
    reporter.check(controlsFound, "the Theme page is missing a core control");
    if (!controlsFound)
        return;
    // Custom edits commit after a short debounce; presets and the contrast
    // dial commit at once.
    const auto settleCustomCommit = [&] {
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 1000) {
            application.processEvents(QEventLoop::AllEvents, 20);
            if (controller.committedSelection().mode == themes::ThemeMode::Custom)
                return;
        }
    };
    dialog.show();
    for (int i = 0; i < 3; ++i)
        application.processEvents();
    const auto modeButtons = dialog.findChildren<QRadioButton *>();
    reporter.check(modeButtons.size() == 4, "the Theme dialog does not contain four mode options");
    if (modeButtons.size() != 4)
        return;
    std::vector<QRect> modeGeometry;
    modeGeometry.reserve(modeButtons.size());
    for (const auto *button : modeButtons)
        modeGeometry.emplace_back(button->mapTo(&dialog, QPoint{}), button->size());
    const QSize dialogSize = dialog.size();
    const auto checkGeometry = [&] {
        application.processEvents();
        bool unchanged = dialog.size() == dialogSize;
        for (qsizetype i = 0; i < modeButtons.size(); ++i) {
            const QRect geometry(modeButtons[i]->mapTo(&dialog, QPoint{}), modeButtons[i]->size());
            unchanged = unchanged && geometry == modeGeometry[size_t(i)];
        }
        reporter.check(unchanged, "Theme dialog options moved while previewing a theme");
    };
    QTabBar tabBar;
    tabBar.addTab(QStringLiteral("Open Song"));
    tabBar.show();
    ThemeRefreshProbe gridPaintTarget;
    gridPaintTarget.resize(10, 10);
    gridPaintTarget.show();
    application.processEvents();
    themes::registerGridLineRefreshTarget(gridPaintTarget);
    themes::registerGridLineRefreshTarget(gridPaintTarget);
    gridPaintTarget.paintCount = 0;
    gridPaintTarget.themeChangeCount = 0;
    StyleChangeCounter tabStyleChanges;
    tabBar.installEventFilter(&tabStyleChanges);
    const auto tabBeforeContrastChange = tabBar.grab().toImage();
    const auto paletteBeforeContrastChange = application.palette();
    const auto styleSheetBeforeContrastChange = application.styleSheet();
    const auto defaultGrid = themes::color(themes::Role::song_view_grid);
    const auto gridBackground = themes::color(themes::Role::song_view_piano_roll_background);
    const auto initialRefreshCount = gridPaintTarget.themeChangeCount;
    gridLineContrast->setValue(100);
    const auto strengthenedGrid = themes::color(themes::Role::song_view_grid);
    reporter.check(gridPaintTarget.themeChangeCount == initialRefreshCount + 1,
                   "duplicate grid target registration caused multiple refreshes");
    reporter.check(gridPaintTarget.gridColor == strengthenedGrid,
                   "grid refresh observed the old theme color");
    gridLineContrast->setValue(0);
    const auto softenedGrid = themes::color(themes::Role::song_view_grid);
    reporter.check(gridPaintTarget.themeChangeCount == initialRefreshCount + 2,
                   "grid contrast did not refresh its target once");
    reporter.check(gridPaintTarget.gridColor == softenedGrid,
                   "grid refresh did not observe the softened theme color");
    reporter.check(themes::contrastRatio(strengthenedGrid, gridBackground) >
                           themes::contrastRatio(defaultGrid, gridBackground) &&
                       themes::contrastRatio(softenedGrid, gridBackground) <
                           themes::contrastRatio(defaultGrid, gridBackground),
                   "the Grid Line Contrast dial did not preview both directions");
    auto destroyedTarget = std::make_unique<ThemeRefreshProbe>();
    QPointer<ThemeRefreshProbe> destroyedTargetGuard(destroyedTarget.get());
    themes::registerGridLineRefreshTarget(*destroyedTarget);
    destroyedTarget.reset();
    reporter.check(destroyedTargetGuard.isNull(), "destroyed grid target was not cleaned up");
    const auto refreshCountBeforeCleanup = gridPaintTarget.themeChangeCount;
    gridLineContrast->setValue(10);
    reporter.check(gridPaintTarget.themeChangeCount == refreshCountBeforeCleanup + 1,
                   "grid refresh failed after a registered target was destroyed");
    gridLineContrast->setValue(themes::defaultGridLineContrast);
    application.processEvents();
    reporter.check(tabStyleChanges.count == 0,
                   "changing grid line contrast restyled the open-song tab bar");
    reporter.check(tabBar.grab().toImage() == tabBeforeContrastChange,
                   "changing grid line contrast changed the open-song tab bar pixels");
    reporter.check(application.palette() == paletteBeforeContrastChange &&
                       application.styleSheet() == styleSheetBeforeContrastChange,
                   "changing grid line contrast churned global application style");
    reporter.check(gridPaintTarget.paintCount > 0,
                   "grid contrast did not repaint its registered paint target");
    darkNeutralHigh->click();
    checkGeometry();
    reporter.check(controller.committedSelection().mode == themes::ThemeMode::DarkNeutralHigh,
                   "selecting a preset did not commit it");
    custom->click();
    primary->setText(QStringLiteral("#000000"));
    // Half a Custom pair is a draft, not a theme: the committed preset stays
    // applied and nothing is written.
    reporter.check(controller.committedSelection().mode == themes::ThemeMode::DarkNeutralHigh &&
                       themes::color(themes::Role::toolbar_background) ==
                           themes::darkNeutralHigh().color(themes::Role::toolbar_background),
                   "a partial Custom pair left the committed preset");
    accent->setText(QStringLiteral("#FFFFFF"));
    gridLineContrast->setValue(80);
    settleCustomCommit();
    const auto committed = controller.committedSelection();
    reporter.check(committed.mode == themes::ThemeMode::Custom && committed.customColors &&
                       committed.customColors->primary == QColor("#000000") &&
                       committed.customColors->accent == QColor("#FFFFFF") &&
                       committed.gridLineContrast == 80,
                   "a valid Custom pair did not commit");
    reporter.check(themes::color(themes::Role::link_text) == QColor("#FFFFFF"),
                   "the committed Custom theme is not applied");
    darkNeutralHigh->click();
    reporter.check(themes::color(themes::Role::toolbar_background) ==
                       themes::darkNeutralHigh().color(themes::Role::toolbar_background),
                   "selecting Dark Neutral High did not apply it");
    immaterial->click();
    reporter.check(themes::color(themes::Role::toolbar_background) ==
                       themes::immaterial().color(themes::Role::toolbar_background),
                   "selecting Immaterial did not apply it");
    gridLineContrast->setValue(10);
    // Presets keep the last Custom pair so returning to Custom restores it.
    custom->click();
    reporter.check(primary->text() == QStringLiteral("#000000") &&
                       accent->text() == QStringLiteral("#FFFFFF"),
                   "returning to Custom did not restore the last pair");
    settleCustomCommit();
    immaterial->click();
    // Closing the Settings window drops a partial draft and returns the
    // controls to the committed selection; a committed one is untouched.
    custom->click();
    primary->setText(QStringLiteral("#12"));
    reporter.check(themes::color(themes::Role::toolbar_background) ==
                       themes::immaterial().color(themes::Role::toolbar_background),
                   "a partial Custom edit replaced the committed theme");
    dialog.rollback();
    reporter.check(immaterial->isChecked() && primary->text().isEmpty(),
                   "rollback did not return the controls to the committed selection");
    reporter.check(themes::color(themes::Role::song_view_grid) ==
                       themes::withGridLineContrast(themes::immaterial(), 10)
                           .color(themes::Role::song_view_grid),
                   "rollback did not keep the committed grid line contrast");
    // A valid Custom pair still inside its commit debounce is a selection,
    // not a draft: closing the window lands it instead of dropping it.
    custom->click();
    primary->setText(QStringLiteral("#000000"));
    accent->setText(QStringLiteral("#FFFFFF"));
    dialog.rollback();
    reporter.check(controller.committedSelection().mode == themes::ThemeMode::Custom &&
                       custom->isChecked() && accent->text() == QStringLiteral("#FFFFFF"),
                   "rollback dropped a valid Custom pair pending its commit");
    immaterial->click();
    settings.sync();
    QSettings restoredSettings(settingsPath, QSettings::IniFormat);
    themes::ThemeController restoredController(application, restoredSettings);
    restoredController.restore();
    const auto &restored = restoredController.committedSelection();
    reporter.check(restored.mode == themes::ThemeMode::Immaterial && restored.customColors &&
                       restored.customColors->primary == QColor("#000000") &&
                       restored.customColors->accent == QColor("#FFFFFF") &&
                       restored.gridLineContrast == 10,
                   "the committed theme (and retained Custom pair) did not survive restore");
    struct StoredMode {
        themes::ThemeMode mode;
        const char *value;
    };
    constexpr auto storedModes = std::array{
        StoredMode{themes::ThemeMode::DarkNeutralHigh, "dark-neutral-high"},
        StoredMode{themes::ThemeMode::Immaterial, "immaterial"},
    };
    for (const auto &storedMode : storedModes) {
        const auto path =
            directory.filePath(QString::fromLatin1(storedMode.value) + QStringLiteral(".ini"));
        auto writeSettings = QSettings{path, QSettings::IniFormat};
        auto writeController = themes::ThemeController{application, writeSettings};
        writeController.restore();
        reporter.check(writeController.commit(themes::ThemeSelection{storedMode.mode}),
                       "a theme could not be committed");
        writeSettings.sync();
        auto readSettings = QSettings{path, QSettings::IniFormat};
        auto readController = themes::ThemeController{application, readSettings};
        readController.restore();
        reporter.check(readController.committedSelection().mode == storedMode.mode &&
                           readSettings.value(QStringLiteral("theme/mode")).toString() ==
                               QString::fromLatin1(storedMode.value),
                       "a theme did not survive restore");
    }
}

} // namespace

int runThemeCheck()
{
    Reporter reporter;
    checkDerivedThemes(reporter);
    auto *application = qobject_cast<QApplication *>(QApplication::instance());
    reporter.check(application != nullptr, "themecheck requires QApplication");
    if (application)
        checkThemeWorkflow(reporter, *application);
    return reporter.finish();
}

// See kDarkBaselinePoison in themecheck.h: every platform palette role was
// poisoned before initializeApplication captured the baseline, and the
// default Vanilla theme has been applied over it. Themed chrome must not
// show the baseline anywhere — this is what a light preset over a dark
// macOS system appearance relies on.
int runDarkBaselineCheck()
{
    Reporter reporter;
    const auto vanillaTheme = themes::vanilla();
    const QPalette palette = QApplication::palette();
    reporter.check(palette.color(QPalette::Active, QPalette::Window) ==
                       vanillaTheme.color(themes::Role::window_background),
                   "window background did not mask the dark baseline");
    reporter.check(palette.color(QPalette::Active, QPalette::WindowText) ==
                       vanillaTheme.color(themes::Role::window_text),
                   "window text did not mask the dark baseline");
    reporter.check(palette.color(QPalette::Active, QPalette::Base) ==
                       vanillaTheme.color(themes::Role::item_background),
                   "item background did not mask the dark baseline");

    // A zoo of the stock controls porydaw's chrome uses, grabbed and scanned
    // for surviving poison pixels. Roles the theme leaves to the platform
    // (Midlight, BrightText, Shadow, tooltips, Qt 6.6's Accent) surface here
    // if any style actually paints with them.
    QWidget zoo;
    zoo.setAttribute(Qt::WA_DontShowOnScreen);
    auto *layout = new QVBoxLayout(&zoo);
    layout->addWidget(new QLabel(QStringLiteral("Label"), &zoo));
    layout->addWidget(new QPushButton(QStringLiteral("Button"), &zoo));
    auto *lineEdit = new QLineEdit(QStringLiteral("edit"), &zoo);
    layout->addWidget(lineEdit);
    auto *checkBox = new QCheckBox(QStringLiteral("Check"), &zoo);
    checkBox->setChecked(true);
    layout->addWidget(checkBox);
    auto *radio = new QRadioButton(QStringLiteral("Radio"), &zoo);
    radio->setChecked(true);
    layout->addWidget(radio);
    auto *combo = new QComboBox(&zoo);
    combo->addItem(QStringLiteral("Combo"));
    layout->addWidget(combo);
    auto *progress = new QProgressBar(&zoo);
    progress->setRange(0, 100);
    progress->setValue(60);
    layout->addWidget(progress);
    auto *slider = new QSlider(Qt::Horizontal, &zoo);
    slider->setValue(40);
    layout->addWidget(slider);
    auto *tabs = new QTabBar(&zoo);
    tabs->addTab(QStringLiteral("One"));
    tabs->addTab(QStringLiteral("Two"));
    layout->addWidget(tabs);
    auto *group = new QGroupBox(QStringLiteral("Group"), &zoo);
    auto *groupLayout = new QVBoxLayout(group);
    groupLayout->addWidget(new QLabel(QStringLiteral("inside"), group));
    layout->addWidget(group);
    auto *scroll = new QScrollBar(Qt::Horizontal, &zoo);
    scroll->setRange(0, 100);
    layout->addWidget(scroll);
    zoo.show();
    QApplication::processEvents();

    const QImage image = zoo.grab().toImage();
    int poisonPixels = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            const int distance = std::abs(pixel.red() - kDarkBaselinePoison.red()) +
                                 std::abs(pixel.green() - kDarkBaselinePoison.green()) +
                                 std::abs(pixel.blue() - kDarkBaselinePoison.blue());
            if (distance <= 24)
                ++poisonPixels;
        }
    }
    if (poisonPixels > 0) {
        std::fprintf(stderr, "themecheck: darkbase: %d poison pixels in the control zoo\n",
                     poisonPixels);
    }
    reporter.check(poisonPixels == 0, "dark platform baseline leaked into themed chrome");
    return reporter.finish();
}
