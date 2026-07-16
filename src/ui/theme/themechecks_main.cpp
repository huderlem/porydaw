#include "ui/applicationstartup.h"
#include "ui/theme/themecheck.h"

#include <QApplication>
#include <QFontInfo>
#include <QStringList>
#include <QSettings>
#include <QTemporaryDir>

int runFontCheck(int expectedBaseFontPx);

int main(int argc, char *argv[]) {
  QApplication application(argc, argv);
  QTemporaryDir settingsDirectory;
  if (!settingsDirectory.isValid())
    return 1;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     settingsDirectory.path());
  QApplication::setApplicationName(QStringLiteral("porydaw"));
  QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
  QApplication::setOrganizationName(QStringLiteral("huderlem"));
  const auto expectedBaseFontPx = QFontInfo(application.font()).pixelSize();
  if (!ui::initializeApplication(application))
    return 1;
  if (application.arguments().contains(QStringLiteral("--fontcheck")))
    return runFontCheck(expectedBaseFontPx);
  if (application.arguments().contains(QStringLiteral("--themecheck")))
    return runThemeCheck();
  return 2;
}
