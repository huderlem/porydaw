#pragma once

#include "themecontroller.h"
#include <QColor>
#include <QDialog>
#include <QString>

class QButtonGroup;
class QCloseEvent;
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

/// Modeless editor for a theme draft.
///
/// The dialog manages controls, text, and the picker. ThemeController owns
/// applying, saving, and restoring the application's committed theme.
class ThemeDialog final : public QDialog {
  Q_OBJECT

public:
  ThemeDialog(ThemeController &controller, QWidget *parent = nullptr);

protected:
  void reject() override;
  void closeEvent(QCloseEvent *event) override;

private slots:
  void primaryHexEdited();
  void accentHexEdited();
  void primarySwatchClicked();
  void accentSwatchClicked();
  void pickerColorSelected(const QColor &color);
  void previewDraft();
  void applyClicked();
  void closeClicked();

private:
  bool eventFilter(QObject *watched, QEvent *event) override;
  void addModeButton(QVBoxLayout &layout, QWidget &parent, ThemeMode mode,
                     const QString &label, const QString &objectName = {});
  void setCheckedMode(ThemeMode mode);
  void modeChanged(ThemeMode mode);

  void setField(QLineEdit *field, const QColor &color);
  void readDraftFromFields();
  void schedulePreview();
  void updateUi();
  void updatePicker();
  void rollbackToCommitted();
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
  QTimer *m_previewTimer = nullptr;
  QPushButton *m_applyButton = nullptr;
  QPushButton *m_closeButton = nullptr;
  bool m_ignoreFieldSignals = false;
};
} // namespace themes
