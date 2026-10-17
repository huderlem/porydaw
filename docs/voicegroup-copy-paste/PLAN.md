# Voicegroup entry copy / paste / clear + context menu

User-submitted ask: copy one voicegroup entry and paste it over another,
via Ctrl+C / Ctrl+V or a right-click menu in the Voicegroup dock, plus Clear
and quick Sample-variant switching. Right-click stops auditioning and opens
the menu instead.

## Decisions (confirmed 2026-10-17)

- **D1** Paste works on **any editable slot**, not only placeholder ones.
  Undoable like every other voice edit.
- **D2** **System clipboard.** Copy writes the voice's macro line as
  `text/plain` (e.g. `voice_directsound 60, 0, Sample_X, 255, 0, 255, 0`), so
  it pastes into a text editor and across tabs / instances. Paste parses
  `text/plain` with the existing voicegroup source parser, so a line copied
  out of a `.inc` file pastes too. A private MIME type
  (`application/x-porydaw-voice`) rides alongside and carries what the text
  can't (see Synth voices); paste prefers it when present.
- **D3** Sample-variant switching is a **radio group** in the menu, not
  independent checkmarks (no macro combines no-resample and reverse).
- **D4** **One slot** at a time; the tree stays single-selection.
- **D5** **No shortcut for Clear** (menu only). Copy / Paste get rebindable
  shortcuts (Ctrl+C / Ctrl+V) in a new keymap context.
- **D6** Drumset sub-voicegroups / the Keysplit Editor dialog are **out of
  scope**: only the dock's voice list gets the menu.

## Behavior

Context menu on a voice row (right-click selects the row, never auditions):

| Item | Enabled when |
|---|---|
| Copy (Ctrl+C) | slot is Editable |
| Paste (Ctrl+V) | slot is Editable, not view-only, clipboard holds a pasteable voice |
| Clear | slot is Editable, not view-only, voice isn't already the placeholder |
| Variant radio group (step 2) | see step 2 |

- **Placeholder** (what Clear writes): `voice_square_1 60, 0, 0, 2, 0, 0, 15, 0`
  — the vanilla unused-slot line.
- **Paste replaces the whole voice**, key and pan included (what you copied is
  what you get). A paste that equals the current voice is a no-op.
- **Validation** — paste refuses (status-bar message, nothing pushed) when:
  the parsed macro isn't editable or isn't offered in this project (e.g. a
  compressed macro in a vanilla project); the symbol isn't known here
  (samples, cries, waves, keysplit sub-voicegroups, drumkits, synth
  definitions); a keysplit's table doesn't match the project's table for that
  sub-voicegroup (adopt the project's table instead of refusing, if simpler —
  implementer's call, note it).
- **Synth voices**: a pending (minted-but-unsaved) synth symbol exists only in
  its own voicegroup. The private MIME carries the synth descriptor; on paste,
  a symbol not known here but with a descriptor resolves through the same
  lookup/mint path `commitEdit` uses (on-disk def → pending def → mint).
- **Undo**: one entry per Paste / Clear / variant change, labelled
  "paste voice N" / "clear voice N". These must **never merge** with an
  adjacent spin-box run in `VoiceEditCommand::mergeWith` (a scalar-only paste
  would otherwise fold into an earlier ADSR drag).
- **Structural** flag: `vgVoiceStructuralChange(before, after)`, as for any
  edit.
- **Shortcuts**: new `keymap::Context::Voicegroup` (focus on the voice tree),
  entries `voicegroup.copy` / `voicegroup.paste`, default Copy / Paste, listed
  in Settings → Keyboard Shortcuts. They must not conflict with `roll.copy` /
  `roll.paste` (`contextsOverlap` already keeps distinct local contexts
  apart). Space must keep toggling playback with the tree focused.
- **Audition**: only the left button auditions; right/middle press does not.

## Steps

Each step: Opus subagent implements → Opus subagent reviews → Opus subagent
fixes → commit → user Windows check.

### Step 1 — context menu, Copy / Paste / Clear, shortcuts
- `VoicegroupBrowser`: context menu, right-click no-audition, Copy / Paste /
  Clear, clipboard read/write, validation, shortcut handling.
- `VoicegroupSource`: a public way to render one `VgVoice` as its macro line,
  and to parse one pasted line back (reuse `parseSource`).
- `MainWindow` / `VoiceEditCommand`: non-merging edit path with its own undo
  label (a new signal or an extra argument — implementer's call).
- `keymap`: `Context::Voicegroup` + the two entries.
- Harness coverage (extend `vgcheck` or the closest existing harness): copy →
  paste round-trip through the clipboard, paste of a hand-written `.inc` line,
  refusal of an unknown symbol, Clear writes the placeholder, one undo step
  restores the voice, paste after a spin edit does not merge, view-only
  disables Paste/Clear. Negative-test the new assertions.

### Step 2 — Variant radio group, docs
- Radio group, shown only when the voice has variants:
  - plain sample voices: **Normal / No Resample / Reversed**
    (`voice_directsound` / `_no_resample` / `_alt`; a `_reverse` voice shows
    as Reversed and stays `_reverse` unless changed);
  - compressed voices: **Normal / Reversed** (`_compressed` /
    `_compressed_reverse`);
  - Square 1, Square 2, Programmable Wave: **Normal / Detuned** (`_alt`).
  - Hidden for synth, keysplit, drumkit, noise, cry voices.
  - Variants share an envelope family, so ADSR carries over unchanged.
- **Reverse macro follows the project** (user decision 2026-10-17): Reversed
  writes whichever reverse macro the project defines — `_alt` or
  `_reverse` (current upstream pokeemerald defines `_reverse` but not
  `_alt`). Applies to the radio group AND the existing Type dropdown, which
  today always writes `_alt`; fix the stale "_alt, which every project has"
  comment. When both exist, keep `_alt` (vanilla).
- CHANGELOG `Added` line (simple, user-facing).
- `docsrc/manual/voicegroups.md`: a TODO-outline section for the user's
  prose (menu items, clipboard text interop, variants). `mkdocs build
  --strict` passes.
