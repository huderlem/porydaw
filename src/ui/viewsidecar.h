#pragma once

#include <QString>

#include "ui/songview.h"

// Per-song sidecar view state (SPEC §4.4), stored as JSON in
// <projectroot>/.porydaw/<song>.json. Sidecars are cosmetic only: a missing
// or unreadable file just means default view state, and a failed save is
// silent — never worth interrupting the user over. Creating `.porydaw/`
// also adds it to the project's .gitignore (project/sidecar.h).
namespace ViewSidecar {

QString pathFor(const QString &projectRoot, const QString &songLabel);

// False (state untouched) when the sidecar is missing or malformed.
bool load(const QString &projectRoot, const QString &songLabel,
          SongView::ViewState *state);

// Creates <projectroot>/.porydaw/ on demand; no-op for an invalid state
// (no song loaded).
bool save(const QString &projectRoot, const QString &songLabel,
          const SongView::ViewState &state);

} // namespace ViewSidecar
