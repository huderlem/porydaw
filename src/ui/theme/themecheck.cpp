#include "ui/theme/themecheck.h"

#include "ui/theme/color_math.h"

#include "ui/theme/themecontroller.h"
#include "ui/theme/themedialog.h"
#include "ui/theme/themeresolver.h"
#include "ui/theme/themeruntime.h"
#include "ui/theme/trackidentitycolors.h"

#include <QApplication>
#include <QComboBox>
#include <QCursor>
#include <QEvent>
#include <QHeaderView>
#include <QLineEdit>
#include <QPaintEvent>
#include <QPixmap>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSlider>
#include <QStyleOptionComboBox>
#include <QTabBar>
#include <QTableWidget>
#include <QTemporaryDir>

#include <array>
#include <cstdio>
#include <utility>
#include <vector>

namespace {

class Reporter {
public:
  void check(bool condition, const char *message) {
    if (condition)
      return;
    std::fprintf(stderr, "themecheck: FAIL: %s\n", message);
    ++m_failures;
  }

  int finish() const {
    std::printf(m_failures == 0 ? "themecheck: PASS\n" : "themecheck: FAIL\n");
    return m_failures == 0 ? 0 : 1;
  }

private:
  int m_failures = 0;
};

class StyleChangeCounter final : public QObject {
public:
  int count = 0;

protected:
  bool eventFilter(QObject *watched, QEvent *event) override {
    if (event && event->type() == QEvent::StyleChange)
      ++count;
    return QObject::eventFilter(watched, event);
  }
};

class PaintCounter final : public QWidget {
public:
  int count = 0;

protected:
  void paintEvent(QPaintEvent *) override { ++count; }
};

// Paint code has no fallback path: every public role must resolve to an
// opaque color.
bool isComplete(const themes::Theme &theme) {
  for (const auto &color : theme.colors) {
    if (!color.isValid() || color.alpha() != 255)
      return false;
  }
  return true;
}

// Track identity colors are application-wide content identity, not theme
// state, so validate the single shared palette directly.
bool isCompleteTrackIdentityPalette() {
  for (std::size_t index = 0; index < themes::trackIdentityColorCount;
       ++index) {
    if (!themes::trackIdentityColor(index).isValid() ||
        themes::trackIdentityColor(index).alpha() != 255 ||
        !themes::trackIdentityTextColor(index).isValid() ||
        themes::trackIdentityTextColor(index).alpha() != 255)
      return false;
  }
  return true;
}

void checkDerivedThemes(Reporter &reporter) {
  reporter.check(themes::isValidColorPair(QColor("#000000"), QColor("#FFFFFF")),
                 "a valid color pair was rejected");
  reporter.check(
      !themes::isValidColorPair(QColor("#777777"), QColor("#888888")),
      "a low-contrast color pair was accepted");
  auto translucent = QColor("#FFFFFF");
  translucent.setAlpha(128);
  reporter.check(!themes::isValidColorPair(QColor("#000000"), translucent),
                 "a translucent color was accepted");

  reporter.check(
      isCompleteTrackIdentityPalette(),
      "the shared track identity palette has an unset or translucent color");

  // Exercise both dark and light Primary directions through the same contract.
  const auto customPairs = std::array{
      std::pair{QColor("#2B2D31"), QColor("#66CCFF")},
      std::pair{QColor("#F2F2F2"), QColor("#0055AA")},
  };
  for (const auto &[primary, accent] : customPairs) {
    const auto theme = themes::derive(primary, accent);
    reporter.check(isComplete(theme),
                   "a derived theme has an unset or translucent role");
    // Disabled text must be legible on the window surface yet clearly
    // dimmer than enabled text; a derived theme once returned the most
    // text-like readable candidate, making disabled items look enabled.
    const auto disabledText = theme.color(themes::Role::disabled_text);
    const auto windowText = theme.color(themes::Role::window_text);
    reporter.check(
        themes::contrastRatio(disabledText,
                              theme.color(themes::Role::window_background)) >=
            4.5,
        "derived disabled text is not readable on the window surface");
    reporter.check(themes::contrastRatio(disabledText, windowText) >= 1.3,
                   "derived disabled text is indistinguishable from enabled");
    const auto softened = themes::withGridLineContrast(theme, 0);
    const auto unchanged = themes::withGridLineContrast(theme, 50);
    const auto strengthened = themes::withGridLineContrast(theme, 100);
    const auto background =
        theme.color(themes::Role::song_view_piano_roll_background);
    reporter.check(unchanged.color(themes::Role::song_view_grid) ==
                       theme.color(themes::Role::song_view_grid),
                   "the default grid line contrast changed a derived theme");
    reporter.check(
        themes::contrastRatio(softened.color(themes::Role::song_view_grid),
                              background) <
                themes::contrastRatio(theme.color(themes::Role::song_view_grid),
                                      background) &&
            themes::contrastRatio(
                strengthened.color(themes::Role::song_view_grid), background) >
                themes::contrastRatio(theme.color(themes::Role::song_view_grid),
                                      background),
        "grid line contrast is not adjustable around the default");
  }
  const auto fixedThemes = std::array{
      themes::vanilla(),
      themes::darkNeutralHigh(),
      themes::immaterial(),
  };
  for (const auto &theme : fixedThemes) {
    reporter.check(isComplete(theme),
                   "a fixed theme has an unset or translucent role");
    reporter.check(
        theme.color(themes::Role::combo_drop_down_hover_background) ==
            theme.color(themes::Role::button_hover_background),
        "a fixed theme gives the ComboBox arrow lane a selection hover color");
    reporter.check(isComplete(themes::withGridLineContrast(theme, 0)) &&
                       isComplete(themes::withGridLineContrast(theme, 100)),
                   "adjusting grid line contrast made a theme incomplete");
    const auto grid = theme.color(themes::Role::song_view_grid);
    const auto background =
        theme.color(themes::Role::song_view_piano_roll_background);
    const auto strengthened = themes::withGridLineContrast(theme, 100)
                                  .color(themes::Role::song_view_grid);
    reporter.check(
        (themes::relativeLuminance(grid) <=
             themes::relativeLuminance(background) &&
         themes::relativeLuminance(strengthened) <
             themes::relativeLuminance(grid)) ||
            (themes::relativeLuminance(grid) >
                 themes::relativeLuminance(background) &&
             themes::relativeLuminance(strengthened) >
                 themes::relativeLuminance(grid)),
        "higher contrast reversed a fixed theme's grid line direction");
    reporter.check(theme.color(themes::Role::focus_outline) ==
                       theme.color(themes::Role::palette_outline),
                   "a fixed theme does not use a neutral focus outline");
    // Same bar the derived path holds: on the dark presets disabled text
    // once sat a bare 1.26:1 from enabled, so disabled transport glyphs
    // looked live.
    reporter.check(
        themes::contrastRatio(theme.color(themes::Role::disabled_text),
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
    const auto surface =
        theme.color(themes::Role::song_view_piano_roll_background);
    for (const auto role : {themes::Role::song_view_edit_preview_outline,
                            themes::Role::song_view_add_automation_lane_action})
      reporter.check(themes::contrastRatio(theme.color(role), surface) >= 3.0,
                     what);
  };
  laneLegible(themes::vanilla(), "a Vanilla lane readout is unreadable");
  laneLegible(themes::darkNeutralHigh(),
              "a Dark Neutral High lane readout is unreadable");
  laneLegible(themes::immaterial(), "an Immaterial lane readout is unreadable");
  laneLegible(themes::derive(QColor("#2B2D31"), QColor("#66CCFF")),
              "a dark derived lane readout is unreadable");
  laneLegible(themes::derive(QColor("#F2F2F2"), QColor("#0055AA")),
              "a light derived lane readout is unreadable");
}


void checkThemeWorkflow(Reporter &reporter, QApplication &application) {
  QTemporaryDir directory;
  reporter.check(directory.isValid(),
                 "could not create temporary theme settings");
  if (!directory.isValid())
    return;
  reporter.check(
      application.styleSheet().contains(
          QStringLiteral("QHeaderView::section{border:0;}")),
      "Layout did not install permanent zero-border headers at startup");
  const auto settingsPath = directory.filePath(QStringLiteral("settings.ini"));
  QSettings settings(settingsPath, QSettings::IniFormat);
  settings.setValue(QStringLiteral("theme/mode"), QStringLiteral("custom"));
  settings.setValue(QStringLiteral("theme/primary"), QStringLiteral("#000000"));
  settings.setValue(QStringLiteral("theme/grid-line-contrast"),
                    QStringLiteral("invalid"));
  themes::ThemeController controller(application, settings);
  controller.restore();
  reporter.check(
      application.styleSheet().contains(
          QStringLiteral("QHeaderView::section{border:0;}")),
      "applying a theme dropped Layout's zero-border header rule");
  reporter.check(controller.committedSelection().mode ==
                     themes::ThemeMode::Vanilla,
                 "invalid settings did not restore the Vanilla theme");
  reporter.check(
      settings.value(QStringLiteral("theme/mode")).toString() ==
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
  const auto arrowRect = combo.style()->subControlRect(
      QStyle::CC_ComboBox, &comboOption, QStyle::SC_ComboBoxArrow, &combo);
  const auto comboImage = combo.grab().toImage();
  const auto arrowColor = themes::color(themes::Role::combo_text);
  auto foundArrowColor = false;
  for (auto y = arrowRect.top(); y <= arrowRect.bottom(); ++y) {
    for (auto x = arrowRect.left(); x <= arrowRect.right(); ++x)
      foundArrowColor =
          foundArrowColor || comboImage.pixelColor(x, y) == arrowColor;
  }
  reporter.check(foundArrowColor,
                 "the ComboBox arrow was not painted with theme text color");
  combo.setAttribute(Qt::WA_UnderMouse);
  QCursor::setPos(combo.mapToGlobal(arrowRect.center()));
  combo.update();
  application.processEvents();
  const auto hoveredComboImage = combo.grab().toImage();
  const auto hoverColor =
      themes::color(themes::Role::combo_drop_down_hover_background);
  auto foundHoverColor = false;
  for (auto y = arrowRect.top(); y <= arrowRect.bottom(); ++y) {
    for (auto x = arrowRect.left(); x <= arrowRect.right(); ++x)
      foundHoverColor =
          foundHoverColor || hoveredComboImage.pixelColor(x, y) == hoverColor;
  }
  reporter.check(foundHoverColor,
                 "the painted ComboBox arrow lane lost its hover color");
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
  reporter.check(
      cellColor(1) == themes::color(themes::Role::item_alternate_background),
      "an alternate item view row lost the theme alternate fill");
  const auto flash = QColor(255, 0, 0);
  table.item(0, 0)->setBackground(flash);
  reporter.check(cellColor(0) == flash,
                 "a model background brush is painted over by the item "
                 "stylesheet (playhead tint, polyphony flash)");
  themes::ThemeDialog dialog(controller);
  auto *custom =
      dialog.findChild<QRadioButton *>(QStringLiteral("customModeButton"));
  auto *darkNeutralHigh = dialog.findChild<QRadioButton *>(
      QStringLiteral("darkNeutralHighModeButton"));
  auto *immaterial = dialog.findChild<QRadioButton *>(
      QStringLiteral("immaterialModeButton"));
  auto *primary =
      dialog.findChild<QLineEdit *>(QStringLiteral("primaryHexEdit"));
  auto *accent = dialog.findChild<QLineEdit *>(QStringLiteral("accentHexEdit"));
  auto *gridLineContrast =
      dialog.findChild<QSlider *>(QStringLiteral("gridLineContrastSlider"));
  auto *apply =
      dialog.findChild<QPushButton *>(QStringLiteral("themeApplyButton"));
  auto *close =
      dialog.findChild<QPushButton *>(QStringLiteral("themeCloseButton"));
  const auto controlsFound = custom && darkNeutralHigh && immaterial &&
                             primary && accent && gridLineContrast && apply &&
                             close;
  reporter.check(controlsFound, "the Theme dialog is missing a core control");
  if (!controlsFound)
    return;
  dialog.show();
  for (int i = 0; i < 3; ++i)
    application.processEvents();
  const auto modeButtons = dialog.findChildren<QRadioButton *>();
  reporter.check(modeButtons.size() == 4,
                 "the Theme dialog does not contain four mode options");
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
      const QRect geometry(modeButtons[i]->mapTo(&dialog, QPoint{}),
                           modeButtons[i]->size());
      unchanged = unchanged && geometry == modeGeometry[size_t(i)];
    }
    reporter.check(unchanged,
                   "Theme dialog options moved while previewing a theme");
  };
  QTabBar tabBar;
  tabBar.addTab(QStringLiteral("Open Song"));
  tabBar.show();
  PaintCounter gridPaintTarget;
  gridPaintTarget.resize(10, 10);
  gridPaintTarget.show();
  application.processEvents();
  themes::registerGridLineRepaintTarget(gridPaintTarget);
  gridPaintTarget.count = 0;
  StyleChangeCounter tabStyleChanges;
  tabBar.installEventFilter(&tabStyleChanges);
  const auto tabBeforeContrastChange = tabBar.grab().toImage();
  const auto defaultGrid = themes::color(themes::Role::song_view_grid);
  const auto gridBackground =
      themes::color(themes::Role::song_view_piano_roll_background);
  gridLineContrast->setValue(100);
  const auto strengthenedGrid = themes::color(themes::Role::song_view_grid);
  gridLineContrast->setValue(0);
  const auto softenedGrid = themes::color(themes::Role::song_view_grid);
  reporter.check(themes::contrastRatio(strengthenedGrid, gridBackground) >
                         themes::contrastRatio(defaultGrid, gridBackground) &&
                     themes::contrastRatio(softenedGrid, gridBackground) <
                         themes::contrastRatio(defaultGrid, gridBackground),
                 "the Grid Line Contrast dial did not preview both directions");
  gridLineContrast->setValue(themes::defaultGridLineContrast);
  application.processEvents();
  reporter.check(tabStyleChanges.count == 0,
                 "changing grid line contrast restyled the open-song tab bar");
  reporter.check(
      tabBar.grab().toImage() == tabBeforeContrastChange,
      "changing grid line contrast changed the open-song tab bar pixels");
  reporter.check(gridPaintTarget.count > 0,
                 "grid contrast did not repaint its registered paint target");
  darkNeutralHigh->click();
  checkGeometry();
  custom->click();
  primary->setText(QStringLiteral("#000000"));
  accent->setText(QStringLiteral("#FFFFFF"));
  reporter.check(apply->isEnabled(), "a valid Custom theme cannot be applied");
  gridLineContrast->setValue(80);
  apply->click();
  const auto &committed = controller.committedSelection();
  reporter.check(committed.mode == themes::ThemeMode::Custom &&
                     committed.customColors &&
                     committed.customColors->primary == QColor("#000000") &&
                     committed.customColors->accent == QColor("#FFFFFF") &&
                     committed.gridLineContrast == 80,
                 "Apply did not commit the Custom theme");
  darkNeutralHigh->click();
  reporter.check(
      themes::color(themes::Role::toolbar_background) ==
          themes::darkNeutralHigh().color(themes::Role::toolbar_background),
      "selecting Dark Neutral High did not preview it");
  immaterial->click();
  reporter.check(themes::color(themes::Role::toolbar_background) ==
                     themes::immaterial().color(
                         themes::Role::toolbar_background),
                 "selecting Immaterial did not preview it");
  gridLineContrast->setValue(10);
  close->click();
  reporter.check(themes::color(themes::Role::link_text) == QColor("#FFFFFF"),
                 "closing the dialog did not restore the committed theme");
  reporter.check(
      themes::color(themes::Role::song_view_grid) ==
          themes::withGridLineContrast(
              themes::derive(QColor("#000000"), QColor("#FFFFFF")), 80)
              .color(themes::Role::song_view_grid),
      "closing the dialog did not restore committed grid line contrast");
  settings.sync();
  QSettings restoredSettings(settingsPath, QSettings::IniFormat);
  themes::ThemeController restoredController(application, restoredSettings);
  restoredController.restore();
  const auto &restored = restoredController.committedSelection();
  reporter.check(restored.mode == themes::ThemeMode::Custom &&
                     restored.customColors &&
                     restored.customColors->primary == QColor("#000000") &&
                     restored.customColors->accent == QColor("#FFFFFF") &&
                     restored.gridLineContrast == 80,
                 "the committed Custom theme did not survive restore");
  struct StoredMode {
    themes::ThemeMode mode;
    const char *value;
  };
  constexpr auto storedModes = std::array{
      StoredMode{themes::ThemeMode::DarkNeutralHigh, "dark-neutral-high"},
      StoredMode{themes::ThemeMode::Immaterial, "immaterial"},
  };
  for (const auto &storedMode : storedModes) {
    const auto path = directory.filePath(QString::fromLatin1(storedMode.value) +
                                         QStringLiteral(".ini"));
    auto writeSettings = QSettings{path, QSettings::IniFormat};
    auto writeController = themes::ThemeController{application, writeSettings};
    writeController.restore();
    reporter.check(
        writeController.commit(themes::ThemeSelection{storedMode.mode}),
        "a theme could not be committed");
    writeSettings.sync();
    auto readSettings = QSettings{path, QSettings::IniFormat};
    auto readController = themes::ThemeController{application, readSettings};
    readController.restore();
    reporter.check(
        readController.committedSelection().mode == storedMode.mode &&
            readSettings.value(QStringLiteral("theme/mode")).toString() ==
                QString::fromLatin1(storedMode.value),
        "a theme did not survive restore");
  }
}

} // namespace

int runThemeCheck() {
  Reporter reporter;
  checkDerivedThemes(reporter);
  auto *application = qobject_cast<QApplication *>(QApplication::instance());
  reporter.check(application != nullptr, "themecheck requires QApplication");
  if (application)
    checkThemeWorkflow(reporter, *application);
  return reporter.finish();
}
