#include "appearancesettingspage.h"

#include "layout.h"
#include "theme/themepage.h"

#include <QCheckBox>
#include <QFontMetrics>
#include <QGroupBox>
#include <QVBoxLayout>

namespace {

// Group boxes on the Settings pages share the Theme editor's inner metrics.
void styleGroupLayout(QGroupBox &group, QVBoxLayout &layout)
{
    const auto margin = ::layout::space(::layout::Space::Two);
    const auto top =
        QFontMetrics(group.font()).lineSpacing() + ::layout::space(::layout::Space::One);
    layout.setContentsMargins(margin, top, margin, margin);
}

} // namespace

AppearanceSettingsPage::AppearanceSettingsPage(themes::ThemeController &themes, bool useSystemFont,
                                               QWidget *parent)
    : QWidget(parent)
{
    auto *themeGroup = new QGroupBox(tr("Theme"), this);
    m_theme = new themes::ThemePage(themes, themeGroup);
    auto *themeLayout = new QVBoxLayout(themeGroup);
    styleGroupLayout(*themeGroup, *themeLayout);
    themeLayout->addWidget(m_theme);

    // The typeface: the bundled Atkinson Hyperlegible scale, or the platform
    // font other Qt applications use.
    auto *fontGroup = new QGroupBox(tr("Font"), this);
    m_systemFont = new QCheckBox(tr("Use the &system font"), fontGroup);
    m_systemFont->setObjectName(QStringLiteral("settingsSystemFont"));
    m_systemFont->setToolTip(tr("Use the platform's font, as other applications do, instead "
                                "of the bundled Atkinson Hyperlegible typeface."));
    m_systemFont->setChecked(useSystemFont);
    auto *fontLayout = new QVBoxLayout(fontGroup);
    styleGroupLayout(*fontGroup, *fontLayout);
    fontLayout->addWidget(m_systemFont);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(themeGroup, 1);
    layout->addWidget(fontGroup);

    connect(m_systemFont, &QCheckBox::toggled, this, &AppearanceSettingsPage::useSystemFontChanged);
}

bool AppearanceSettingsPage::useSystemFont() const
{
    return m_systemFont->isChecked();
}

void AppearanceSettingsPage::rollback()
{
    m_theme->rollback();
}
