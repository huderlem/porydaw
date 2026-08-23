#pragma once

#include "themecontroller.h"
#include <QColor>
#include <QString>
#include <QWidget>

#include <optional>

class QButtonGroup;
class QEvent;
class QSlider;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;
class QVBoxLayout;
class QWidget;

namespace themes {

/// Theme editor for the Settings window's Appearance page.
///
/// The page manages controls, text, and the picker. ThemeController owns
/// applying, saving, and restoring the application's committed theme.
/// Every valid selection commits as it is made — presets and the contrast
/// dial at once, Custom colors after a short debounce. A half-typed Custom
/// pair is the one draft state: while it is invalid the committed theme
/// stays applied, and rollback() (the Settings window closing) returns the
/// controls to it.
class ThemePage final : public QWidget
{
    Q_OBJECT

  public:
    ThemePage(ThemeController &controller, QWidget *parent = nullptr);

    /// Lands a pending (debounced) Custom commit, drops an invalid partial
    /// draft and returns the controls to the committed selection. Called
    /// when the Settings window closes.
    void rollback();

  private slots:
    void primaryHexEdited();
    void accentHexEdited();
    void primarySwatchClicked();
    void accentSwatchClicked();
    void pickerColorSelected(const QColor &color);
    void commitDraft();

  private:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void addModeButton(QVBoxLayout &layout, QWidget &parent, ThemeMode mode, const QString &label,
                       const QString &objectName = {});
    void setCheckedMode(ThemeMode mode);
    void modeChanged(ThemeMode mode);

    void setField(QLineEdit *field, const QColor &color);
    void readDraftFromFields();
    void scheduleCommit();
    void updateUi();
    void updatePicker();
    void clearPartialCustomDraft();
    void resetDraftToCommitted();
    std::optional<ThemeSelection> draftSelection() const;
    QColor parseField(const QLineEdit *field) const;
    static QString canonicalHex(const QColor &color);

    ThemeController &m_controller;
    struct Draft {
        ThemeMode mode = ThemeMode::Vanilla;
        QColor primary;
        QColor accent;
        int gridLineContrast = defaultGridLineContrast;
    } m_draft;

    QButtonGroup *m_modeButtons = nullptr;
    QLineEdit *m_primaryHexEdit = nullptr;
    QGroupBox *m_customEditorGroup = nullptr;
    QLineEdit *m_accentHexEdit = nullptr;
    QPushButton *m_primarySwatch = nullptr;
    QPushButton *m_accentSwatch = nullptr;
    QSlider *m_gridLineContrastSlider = nullptr;
    QLabel *m_gridLineContrastValueLabel = nullptr;
    QLabel *m_pickerTargetLabel = nullptr;
    QWidget *m_picker = nullptr;
    bool m_pickerTargetsPrimary = true;
    QTimer *m_commitTimer = nullptr;
    bool m_ignoreFieldSignals = false;
};
} // namespace themes
