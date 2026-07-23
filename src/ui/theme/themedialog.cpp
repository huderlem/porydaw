#include "themedialog.h"

#include "oklchpicker.h"
#include "themeresolver.h"
#include "ui/layout.h"
#include <QSlider>

#include <QButtonGroup>
#include <QCloseEvent>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>


namespace themes {

void ThemeDialog::addModeButton(QVBoxLayout &layout, QWidget &parent,
                                ThemeMode mode, const QString &label,
                                const QString &objectName) {
  const auto id = static_cast<int>(mode);
  Q_ASSERT(m_modeButtons->button(id) == nullptr);
  auto *button = new QRadioButton(label, &parent);
  if (!objectName.isEmpty())
    button->setObjectName(objectName);
  m_modeButtons->addButton(button, id);
  button->setAttribute(Qt::WA_LayoutUsesWidgetRect);
  layout.addWidget(button);
}

void ThemeDialog::setCheckedMode(ThemeMode mode) {
  auto *button = m_modeButtons->button(static_cast<int>(mode));
  Q_ASSERT(button);
  button->setChecked(true);
}

ThemeDialog::ThemeDialog(ThemeController &controller, QWidget *parent)
    : QDialog(parent), m_controller(controller), m_draft{} {
  setWindowTitle(tr("Theme"));
  setModal(false);
  setAttribute(Qt::WA_DeleteOnClose, false);

  auto *modeGroup = new QWidget(this);
  auto *modeLayout = new QVBoxLayout(modeGroup);
  modeLayout->setContentsMargins(0, 0, 0, 0);
  m_modeButtons = new QButtonGroup(this);
  m_modeButtons->setExclusive(true);
  addModeButton(*modeLayout, *modeGroup, ThemeMode::Vanilla, tr("Vanilla"));
  addModeButton(*modeLayout, *modeGroup, ThemeMode::DarkNeutralHigh,
                tr("Dark Neutral High"),
                QStringLiteral("darkNeutralHighModeButton"));
  addModeButton(*modeLayout, *modeGroup, ThemeMode::Immaterial,
                tr("Immaterial"), QStringLiteral("immaterialModeButton"));
  addModeButton(*modeLayout, *modeGroup, ThemeMode::Custom, tr("Custom"),
                QStringLiteral("customModeButton"));

  m_gridLineContrastSlider = new QSlider(Qt::Horizontal, this);
  m_gridLineContrastSlider->setObjectName(
      QStringLiteral("gridLineContrastSlider"));
  m_gridLineContrastSlider->setRange(0, 100);
  m_gridLineContrastSlider->setValue(defaultGridLineContrast);
  m_gridLineContrastSlider->setTickPosition(QSlider::TicksBelow);
  m_gridLineContrastSlider->setTickInterval(10);
  m_gridLineContrastSlider->setAccessibleName(tr("Grid Line Contrast"));
  m_gridLineContrastSlider->setToolTip(
      tr("50 uses the theme default. Lower values soften grid lines; higher "
         "values strengthen them."));
  m_gridLineContrastValueLabel =
      new QLabel(QString::number(defaultGridLineContrast), this);
  m_gridLineContrastValueLabel->setMinimumWidth(
      QFontMetrics(m_gridLineContrastValueLabel->font())
          .horizontalAdvance(QStringLiteral("100")));
  auto *contrastRow = new QHBoxLayout;
  contrastRow->setContentsMargins(0, 0, 0, 0);
  contrastRow->addWidget(m_gridLineContrastSlider, 1);
  contrastRow->addWidget(m_gridLineContrastValueLabel);
  auto *contrastLayout = new QFormLayout;
  contrastLayout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
  contrastLayout->addRow(tr("Grid Line Contrast:"), contrastRow);

  m_customEditorGroup = new QGroupBox(tr("Custom colors"), this);

  m_primaryHexEdit = new QLineEdit(this);
  m_primaryHexEdit->setObjectName(QStringLiteral("primaryHexEdit"));
  m_primaryHexEdit->setPlaceholderText(QStringLiteral("#RRGGBB"));
  m_primaryHexEdit->setMaxLength(7);
  m_primaryHexEdit->setInputMethodHints(Qt::ImhPreferLatin);
  m_primaryHexEdit->installEventFilter(this);
  m_accentHexEdit = new QLineEdit(this);
  m_accentHexEdit->setObjectName(QStringLiteral("accentHexEdit"));
  m_accentHexEdit->setPlaceholderText(QStringLiteral("#RRGGBB"));
  m_accentHexEdit->setMaxLength(7);
  m_accentHexEdit->setInputMethodHints(Qt::ImhPreferLatin);
  m_accentHexEdit->installEventFilter(this);

  m_primarySwatch = new QPushButton(this);
  m_primarySwatch->setFixedWidth(::layout::fontPx(3));
  m_primarySwatch->setToolTip(tr("Edit primary color"));
  m_accentSwatch = new QPushButton(this);
  m_accentSwatch->setFixedWidth(::layout::fontPx(3));
  m_accentSwatch->setToolTip(tr("Edit accent color"));

  auto *colors = new QFormLayout;
  colors->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
  auto *primaryRow = new QHBoxLayout;
  primaryRow->setContentsMargins(0, 0, 0, 0);
  primaryRow->addWidget(m_primarySwatch);
  primaryRow->addWidget(m_primaryHexEdit, 1);
  auto *primaryHolder = new QWidget(this);
  primaryHolder->setLayout(primaryRow);
  colors->addRow(tr("Primary:"), primaryHolder);
  auto *accentRow = new QHBoxLayout;
  accentRow->setContentsMargins(0, 0, 0, 0);
  accentRow->addWidget(m_accentSwatch);
  accentRow->addWidget(m_accentHexEdit, 1);
  auto *accentHolder = new QWidget(this);
  accentHolder->setLayout(accentRow);
  colors->addRow(tr("Accent:"), accentHolder);
  // Describe Accent by intent; the resolver remains the source of truth for
  // which concrete controls consume it.
  auto *accentCaption = new QLabel(
      tr("Accent colors focus, selection, and edit emphasis."), this);
  accentCaption->setForegroundRole(QPalette::PlaceholderText);
  accentCaption->setWordWrap(true);
  colors->addRow(QString(), accentCaption);

  m_pickerTargetLabel = new QLabel(this);
  m_picker = new OklchPicker(this);
  auto *maskSample = new QLabel(this);
  const auto maskExtent = ::layout::fontPx(1.333);
  maskSample->setFixedSize(maskExtent, maskExtent);
  maskSample->setAccessibleName(tr("Unavailable color pattern"));
  QPixmap maskPattern(maskSample->size());
  QPainter maskPatternPainter(&maskPattern);
  for (int y = 0; y < maskPattern.height(); y += 4) {
    for (int x = 0; x < maskPattern.width(); x += 4) {
      const auto dark = ((x + y) / 4) % 2 == 0;
      maskPatternPainter.fillRect(
          x, y, 4, 4, dark ? QColor(64, 64, 64) : QColor(232, 232, 232));
    }
  }
  maskSample->setPixmap(maskPattern);
  auto *pickerLabel = new QLabel(
      tr("Patterned colors cannot be selected. They are outside sRGB or have "
         "less than 3:1 contrast with the other color."),
      this);
  pickerLabel->setWordWrap(true);
  auto *pickerLegend = new QHBoxLayout;
  pickerLegend->setContentsMargins(0, 0, 0, 0);
  pickerLegend->addWidget(maskSample, 0, Qt::AlignTop);
  pickerLegend->addWidget(pickerLabel, 1);
  auto *customLayout = new QVBoxLayout(m_customEditorGroup);
  const auto groupMargin = ::layout::space(::layout::Space::Two);
  const auto groupTop =
      QFontMetrics(m_customEditorGroup->font()).lineSpacing() +
      ::layout::space(::layout::Space::One);
  customLayout->setContentsMargins(groupMargin, groupTop, groupMargin,
                                   groupMargin);
  customLayout->addLayout(colors);
  customLayout->addWidget(m_pickerTargetLabel);
  customLayout->addLayout(pickerLegend);
  customLayout->addWidget(m_picker, 1);

  m_applyButton = new QPushButton(tr("Apply"), this);
  m_applyButton->setObjectName(QStringLiteral("themeApplyButton"));
  m_closeButton = new QPushButton(tr("Close"), this);
  m_closeButton->setObjectName(QStringLiteral("themeCloseButton"));
  auto *buttons = new QHBoxLayout;
  buttons->addStretch(1);
  buttons->addWidget(m_applyButton);
  buttons->addWidget(m_closeButton);

  auto *layout = new QVBoxLayout(this);
  layout->addWidget(modeGroup);
  layout->addLayout(contrastLayout);
  layout->addWidget(m_customEditorGroup, 1);
  layout->addLayout(buttons);

  m_previewTimer = new QTimer(this);
  m_previewTimer->setSingleShot(true);
  m_previewTimer->setInterval(50);

  connect(m_modeButtons, &QButtonGroup::idClicked, this, [this](int id) {
    const auto mode = static_cast<ThemeMode>(id);
    if (mode != m_draft.mode)
      modeChanged(mode);
  });
  connect(m_primaryHexEdit, &QLineEdit::textChanged, this,
          &ThemeDialog::primaryHexEdited);
  connect(m_accentHexEdit, &QLineEdit::textChanged, this,
          &ThemeDialog::accentHexEdited);
  connect(m_primarySwatch, &QPushButton::clicked, this,
          &ThemeDialog::primarySwatchClicked);
  connect(m_accentSwatch, &QPushButton::clicked, this,
          &ThemeDialog::accentSwatchClicked);
  connect(static_cast<OklchPicker *>(m_picker), &OklchPicker::colorSelected,
          this, &ThemeDialog::pickerColorSelected);
  connect(m_gridLineContrastSlider, &QSlider::valueChanged, this,
          [this](int value) {
            m_draft.gridLineContrast = value;
            m_gridLineContrastValueLabel->setText(QString::number(value));
            schedulePreview();
          });
  connect(m_previewTimer, &QTimer::timeout, this, &ThemeDialog::previewDraft);
  connect(m_applyButton, &QPushButton::clicked, this,
          &ThemeDialog::applyClicked);
  connect(m_closeButton, &QPushButton::clicked, this,
          &ThemeDialog::closeClicked);

  resetDraftToCommitted();
  setCheckedMode(m_draft.mode);
  updateUi();
  updatePicker();
  resize(sizeHint());
}

void ThemeDialog::setField(QLineEdit *field, const QColor &color) {
  const bool oldIgnore = m_ignoreFieldSignals;
  m_ignoreFieldSignals = true;
  field->setText(canonicalHex(color));
  m_ignoreFieldSignals = oldIgnore;
}

QString ThemeDialog::canonicalHex(const QColor &color) {
  return color.name(QColor::HexRgb).toUpper();
}

QColor ThemeDialog::parseField(const QLineEdit *field) const {
  const auto text = field->text().trimmed();
  const auto parsed = QColor::fromString(text);
  if (!parsed.isValid() ||
      parsed.name(QColor::HexRgb).compare(text, Qt::CaseInsensitive) != 0)
    return {};
  return parsed;
}

void ThemeDialog::readDraftFromFields() {
  m_draft.primary = parseField(m_primaryHexEdit);
  m_draft.accent = parseField(m_accentHexEdit);
}

std::optional<ThemeSelection> ThemeDialog::draftSelection() const {
  auto selection = ThemeSelection{m_draft.mode};
  if (isValidColorPair(m_draft.primary, m_draft.accent))
    selection.customColors = ColorPair{m_draft.primary, m_draft.accent};
  selection.gridLineContrast = m_draft.gridLineContrast;
  if (m_draft.mode == ThemeMode::Custom && !selection.customColors)
    return std::nullopt;
  return selection;
}

void ThemeDialog::schedulePreview() {
  m_previewTimer->stop();
  const auto selection = draftSelection();
  // Never leave an invalid partial Custom draft installed as the application
  // preview; fall back to the committed selection until both fields are valid.
  if (!selection) {
    m_controller.discardPreview();
    return;
  }
  // Continuous Custom edits perform color derivation, so debounce them. Fixed
  // presets are already resolved constants and should respond immediately.
  if (m_draft.mode == ThemeMode::Custom)
    m_previewTimer->start();
  else
    m_controller.preview(*selection);
}

void ThemeDialog::updatePicker() {
  auto *picker = static_cast<OklchPicker *>(m_picker);
  const bool targetPrimary = m_pickerTargetsPrimary;
  m_pickerTargetLabel->setText(targetPrimary ? tr("Editing Primary color")
                                             : tr("Editing Accent color"));
  const bool selectedSet =
      targetPrimary ? m_draft.primary.isValid() : m_draft.accent.isValid();
  const bool otherSet =
      targetPrimary ? m_draft.accent.isValid() : m_draft.primary.isValid();
  const QColor selected =
      selectedSet ? (targetPrimary ? m_draft.primary : m_draft.accent)
                  : QColor(128, 128, 128);
  const QColor other =
      otherSet ? (targetPrimary ? m_draft.accent : m_draft.primary) : QColor();
  picker->setSelection(selected, other, otherSet);
}

void ThemeDialog::updateUi() {
  const bool custom = m_draft.mode == ThemeMode::Custom;
  m_customEditorGroup->setEnabled(custom);
  m_applyButton->setEnabled(draftSelection().has_value());

  const auto setSwatch = [](QPushButton *button, const QColor &color,
                            bool set) {
    const auto current = button->property("color").toString();
    if (!set) {
      if (current.isEmpty())
        return;
      button->setStyleSheet(QString());
      button->setProperty("color", QString());
      return;
    }
    const auto hex = color.name(QColor::HexRgb).toUpper();
    if (current == hex)
      return;
    button->setStyleSheet(
        QStringLiteral("background-color: %1; border: %2px solid palette(mid);")
            .arg(hex)
            .arg(::layout::singlePixel()));
    button->setProperty("color", hex);
  };
  setSwatch(m_primarySwatch, m_draft.primary, m_draft.primary.isValid());
  setSwatch(m_accentSwatch, m_draft.accent, m_draft.accent.isValid());
}

void ThemeDialog::primaryHexEdited() {
  if (m_ignoreFieldSignals)
    return;
  readDraftFromFields();
  if (m_draft.primary.isValid() &&
      m_primaryHexEdit->text() != canonicalHex(m_draft.primary))
    setField(m_primaryHexEdit, m_draft.primary);
  updatePicker();
  updateUi();
  schedulePreview();
}

void ThemeDialog::accentHexEdited() {
  if (m_ignoreFieldSignals)
    return;
  readDraftFromFields();
  if (m_draft.accent.isValid() &&
      m_accentHexEdit->text() != canonicalHex(m_draft.accent))
    setField(m_accentHexEdit, m_draft.accent);
  updatePicker();
  updateUi();
  schedulePreview();
}

void ThemeDialog::primarySwatchClicked() {
  m_pickerTargetsPrimary = true;
  m_primaryHexEdit->setFocus(Qt::MouseFocusReason);
  updatePicker();
}

void ThemeDialog::accentSwatchClicked() {
  m_pickerTargetsPrimary = false;
  m_accentHexEdit->setFocus(Qt::MouseFocusReason);
  updatePicker();
}

void ThemeDialog::pickerColorSelected(const QColor &color) {
  if (m_draft.mode != ThemeMode::Custom || !color.isValid())
    return;
  if (m_pickerTargetsPrimary)
    m_draft.primary = color;
  else
    m_draft.accent = color;
  setField(m_pickerTargetsPrimary ? m_primaryHexEdit : m_accentHexEdit, color);
  updateUi();
  schedulePreview();
}

void ThemeDialog::modeChanged(ThemeMode mode) {
  m_draft.mode = mode;
  if (mode == ThemeMode::Custom) {
    // Both invalid fields mean a clean return to Custom. A single valid field
    // is an in-progress edit and must not be overwritten by the saved pair.
    const auto &lastCustom = m_controller.committedSelection().customColors;
    if (!m_draft.primary.isValid() && !m_draft.accent.isValid() && lastCustom) {
      m_draft.primary = lastCustom->primary;
      m_draft.accent = lastCustom->accent;
      setField(m_primaryHexEdit, m_draft.primary);
      setField(m_accentHexEdit, m_draft.accent);
    }
  }
  updateUi();
  updatePicker();
  schedulePreview();
}

void ThemeDialog::previewDraft() {
  const auto selection = draftSelection();
  if (!selection) {
    m_controller.discardPreview();
    return;
  }
  m_controller.preview(*selection);
}

void ThemeDialog::applyClicked() {
  const auto selection = draftSelection();
  if (!selection)
    return;
  m_previewTimer->stop();
  if (!m_controller.commit(*selection))
    return;
  resetDraftToCommitted();
  updateUi();
  updatePicker();
}

void ThemeDialog::clearPartialCustomDraft() {
  m_draft.primary = QColor();
  m_draft.accent = QColor();
  const bool oldIgnore = m_ignoreFieldSignals;
  m_ignoreFieldSignals = true;
  m_primaryHexEdit->clear();
  m_accentHexEdit->clear();
  m_ignoreFieldSignals = oldIgnore;
}

void ThemeDialog::resetDraftToCommitted() {
  const auto &committed = m_controller.committedSelection();
  m_draft.mode = committed.mode;
  m_draft.gridLineContrast = committed.gridLineContrast;
  const QSignalBlocker blocker(m_gridLineContrastSlider);
  m_gridLineContrastSlider->setValue(m_draft.gridLineContrast);
  m_gridLineContrastValueLabel->setText(
      QString::number(m_draft.gridLineContrast));
  clearPartialCustomDraft();
  if (committed.mode == ThemeMode::Custom && committed.customColors) {
    m_draft.primary = committed.customColors->primary;
    m_draft.accent = committed.customColors->accent;
    setField(m_primaryHexEdit, m_draft.primary);
    setField(m_accentHexEdit, m_draft.accent);
  }
}

void ThemeDialog::rollbackToCommitted() {
  m_previewTimer->stop();
  m_controller.discardPreview();
  resetDraftToCommitted();
  setCheckedMode(m_draft.mode);
  updateUi();
  updatePicker();
}

void ThemeDialog::reject() {
  rollbackToCommitted();
  QDialog::reject();
}

void ThemeDialog::closeEvent(QCloseEvent *event) {
  rollbackToCommitted();
  QDialog::closeEvent(event);
}

void ThemeDialog::closeClicked() { reject(); }

bool ThemeDialog::eventFilter(QObject *watched, QEvent *event) {
  if (event && event->type() == QEvent::FocusIn) {
    if (watched == m_primaryHexEdit) {
      m_pickerTargetsPrimary = true;
      updatePicker();
    } else if (watched == m_accentHexEdit) {
      m_pickerTargetsPrimary = false;
      updatePicker();
    }
  }
  return QDialog::eventFilter(watched, event);
}

} // namespace themes
