#include "settingsdialog.h"

#include "appearancesettingspage.h"
#include "audiosettingspage.h"
#include "keyboardshortcutspage.h"
#include "layout.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace {

const QString kLastPageKey = QStringLiteral("settings/lastPage");

// Stored by name, not index, so reordering sections never reopens the
// wrong one.
QString pageId(SettingsDialog::Page page)
{
    switch (page) {
    case SettingsDialog::Page::Audio:
        return QStringLiteral("audio");
    case SettingsDialog::Page::Appearance:
        return QStringLiteral("appearance");
    case SettingsDialog::Page::Shortcuts:
        return QStringLiteral("shortcuts");
    }
    return QStringLiteral("audio");
}

constexpr int kPageRole = Qt::UserRole;

} // namespace

SettingsDialog::SettingsDialog(themes::ThemeController &themes, int outputLevel,
                               const EngineSettings &engine, bool useSystemFont, QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("settingsDialog"));
    setWindowTitle(tr("Settings"));
    setModal(false);
    setAttribute(Qt::WA_DeleteOnClose, false);

    m_sections = new QListWidget(this);
    m_sections->setObjectName(QStringLiteral("settingsSections"));
    m_sections->setSelectionMode(QAbstractItemView::SingleSelection);
    m_sections->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_sections->setUniformItemSizes(true);
    m_stack = new QStackedWidget(this);
    m_stack->setObjectName(QStringLiteral("settingsStack"));

    m_audio = new AudioSettingsPage(outputLevel, engine, this);
    m_audio->setObjectName(QStringLiteral("settingsAudioPage"));
    m_appearance = new AppearanceSettingsPage(themes, useSystemFont, this);
    m_appearance->setObjectName(QStringLiteral("settingsAppearancePage"));
    m_shortcuts = new KeyboardShortcutsPage(this);
    m_shortcuts->setObjectName(QStringLiteral("settingsShortcutsPage"));
    addPage(Page::Audio, tr("Audio"), m_audio);
    addPage(Page::Appearance, tr("Appearance"), m_appearance);
    addPage(Page::Shortcuts, tr("Keyboard Shortcuts"), m_shortcuts);

    // The list is as wide as its widest title and no wider: it is a
    // navigator, not a column.
    int widest = 0;
    for (int i = 0; i < m_sections->count(); ++i)
        widest = std::max(widest,
                          m_sections->fontMetrics().horizontalAdvance(m_sections->item(i)->text()));
    m_sections->setFixedWidth(widest + ::layout::space(::layout::Space::Six));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setObjectName(QStringLiteral("settingsCloseButton"));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *body = new QHBoxLayout;
    body->setSpacing(::layout::space(::layout::Space::Three));
    body->addWidget(m_sections);
    body->addWidget(m_stack, 1);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(body, 1);
    layout->addWidget(buttons);

    connect(m_sections, &QListWidget::currentRowChanged, this, &SettingsDialog::pageChanged);
    connect(m_audio, &AudioSettingsPage::outputLevelChanged, this,
            &SettingsDialog::outputLevelChanged);
    connect(m_audio, &AudioSettingsPage::engineSettingsChanged, this,
            &SettingsDialog::engineSettingsChanged);
    connect(m_appearance, &AppearanceSettingsPage::useSystemFontChanged, this,
            &SettingsDialog::useSystemFontChanged);

    const QString remembered = QSettings().value(kLastPageKey).toString();
    Page initial = Page::Audio;
    for (int i = 0; i < m_sections->count(); ++i) {
        const auto page = Page(m_sections->item(i)->data(kPageRole).toInt());
        if (pageId(page) == remembered)
            initial = page;
    }
    showPage(initial);
    m_remember = true;
    resize(sizeHint().expandedTo(QSize(::layout::fontPx(44), ::layout::fontPx(36))));
}

void SettingsDialog::addPage(Page page, const QString &title, QWidget *widget)
{
    auto *item = new QListWidgetItem(title, m_sections);
    item->setData(kPageRole, int(page));
    m_stack->addWidget(widget);
}

void SettingsDialog::present()
{
    show();
    raise();
    activateWindow();
}

void SettingsDialog::showPage(Page page)
{
    for (int i = 0; i < m_sections->count(); ++i) {
        if (Page(m_sections->item(i)->data(kPageRole).toInt()) == page) {
            m_sections->setCurrentRow(i);
            return;
        }
    }
}

SettingsDialog::Page SettingsDialog::currentPage() const
{
    const auto *item = m_sections->currentItem();
    return item ? Page(item->data(kPageRole).toInt()) : Page::Audio;
}

void SettingsDialog::pageChanged(int index)
{
    if (index < 0)
        return;
    m_stack->setCurrentIndex(index);
    if (m_remember)
        QSettings().setValue(kLastPageKey, pageId(currentPage()));
}

// Closing is the one moment a page can hold state the app doesn't: a
// half-typed Custom theme. Everything else already landed as it changed.
void SettingsDialog::leave()
{
    m_appearance->rollback();
}

void SettingsDialog::reject()
{
    leave();
    QDialog::reject();
}
