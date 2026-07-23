#pragma once

class QApplication;

namespace ui {

/// Installs bundled typography and initializes the shared UI modules.
bool initializeApplication(QApplication &application);

} // namespace ui
