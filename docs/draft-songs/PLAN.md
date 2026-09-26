# Draft Songs — Import MIDI / New Song without touching the project

Status: **decisions confirmed 2026-09-26**; step 1 done 2026-09-26. File/line references verified
against `main` at `1fc113a`. Work the steps **in order, one agent per step**;
each step must leave `main` green (`tools/run_checks.sh`) and ends with a
progress entry at the bottom of this file.

## 0. Problem and goal

Users try **File → Import MIDI…** to see how a MIDI converts, and it
immediately writes into their decomp project. There is no way to back out
short of Delete Song (which leaves a trashed `.mid` and edits the
registration files a second time). `MainWindow::finishCreateSong`
(`src/mainwindow.cpp:3127`) writes, in order, before the tab even opens:

1. (optional) a new `sound/voicegroups/<name>.inc` from the dummy template
   plus its include line (`VoicegroupSource::createVoicegroup` +
   `appendIncludeLine`);
2. `sound/songs/midi/<label>.mid` and its `midi.cfg`/`songs.mk` flags
   (`SmfFile::writeFile`, `SongRegistry::writeSongFlags`);
3. the registration: `song_table.inc`, `songs.h`, `ld_script.ld`,
   `charmap.txt`, `src/debug.c` (`SongRegistry::registerSong`);
4. then `reloadProject()` and `loadSongByLabel(label, true)`, which reads the
   song back **from disk**.

New Song (`MainWindow::newSong`, `:2782`) goes through the same function.

**Goal:** Import MIDI and New Song open a **draft tab**: a fully editable,
playable song that exists only in memory. Nothing in the project changes
until the user saves that tab. Closing it (or answering Discard) leaves the
project exactly as it was, apart from Porydaw's own gitignored `.porydaw/`
folder (see §2 stance 3).

Out of scope: the bundle tab's **Import** button (`applyBundleImport`). That
button is an explicit "add this to my project" action and stays
write-through.

## 1. Decisions (all confirmed by the user 2026-09-26)

| # | Question | Default |
|---|---|---|
| D1 | Does **New Song** become a draft too, not just Import MIDI? | ✅ Yes. They share `finishCreateSong`, and two behaviors would be confusing. |
| D2 | What happens when a name (label / constant / `.mid` / voicegroup) is taken by the time the user saves? | ✅ Save is refused with an explanation, and a **Rename** dialog (the wizard's name fields and checks) opens; saving continues if the user accepts. |
| D3 | Where does a draft's **new voicegroup** live before commit? | ✅ `<root>/.porydaw/drafts/<id>/`. That folder is gitignored and owned by Porydaw (`sidecar.cpp:27-44`), and `vgpreview` already works this way. poryaaaa's loader only takes search paths **relative to the root** (`voicegroup_loader.c:752` `build_path`), so a system temp folder would need a submodule change. |
| D4 | Do drafts survive a restart or crash? | ✅ No. They are never written to `lastOpenSongs`, and a crash loses them. Autosave can come later. |
| D5 | Do drafts appear in the song browser? | ✅ No for v1. The tab and its banner are the only place they show. |
| D6 | View state (zoom/scroll) of a committed draft | ✅ Whatever is simplest (user doesn't mind). So the commit writes **no** sidecar: once `draft` is cleared, the ordinary `saveViewState` calls (tab close, song switch, project switch) record it as they do for any song. Nothing is written while it is a draft. |

To change a decision later, update this table and every step it
affects **before** starting the next step.

## 2. Design stances

1. **A draft is a project song that hasn't been written yet**, not a third
   kind of bundle. `session.root` is the project root, so samples,
   keysplits, the catalog and the voicegroup dock all resolve exactly as for
   a real song. The only differences are: no `songId`, and the `.mid`,
   flags, registration and new voicegroup haven't been written.
2. **The first Save is the commit.** Every save path already goes through
   `MainWindow::saveSession` (`:2367`): Ctrl+S (`saveSong`, `:2361`), the
   close/switch/quit prompts (`maybeSaveSession`, `:3973`;
   `promptToSaveAllSessions`, `:1393`) and scripting `saveSong` (`:355`).
   The commit hooks in there and nowhere else.
3. **Nothing outside `.porydaw/` is written before the commit.** Inside
   `.porydaw/` only the draft's own folder (and the existing `vgpreview`)
   may be written, and closing the draft removes its folder.
4. **The commit keeps the tab.** After committing, the same session
   becomes an ordinary project song: its document, undo history, view and
   audio binding all survive. It must **not** go through `loadSongByLabel`,
   which would re-read the song from disk and throw away the undo history.
5. **The commit can be retried.** Every step checks whether it already
   happened, so a Save that failed partway can simply be run again.

## 3. Facts the steps rely on

- `SongSession` (`src/songsession.h:42`): `isDirty()` is
  `!bundle && (doc.isDirty() || vgSource dirty)`. A freshly loaded document
  has a clean undo stack, so a draft **must** add its own term.
- `SongDocument::load(const SongInfo&)` (`src/core/songdocument.cpp:330`)
  only reads the file, then sets `m_smf/m_cfg/m_savedCfg/m_midPath/m_label/
  m_hadCfgLine`, clears undo, mints note IDs, rebuilds the track map and
  bumps the revision. `save()` (`:360`) writes `m_midPath` and **already
  writes the flags when `!m_hadCfgLine`**, so a draft with
  `hasCfg=false` and `midPath=<final path>` gets its `.mid` and flags
  written by the ordinary `doc.save()`.
- `loadSong` (`src/mainwindow.cpp:1845`) is the template for building a
  session: voicegroup load → `createSession` → `doc.load` → timeline →
  `openVoicegroupSource` → view → tab. `trackBudgetFor(SongInfo)` only
  needs `player`. *(Step 1: that body is now
  `MainWindow::populateSession(session, song, draft, readDocument, error)`,
  shared by `loadSong` and `openDraftSong`; its single
  `loadVoicegroupFor(m_project.root(), …)` call is the `loadSong` site
  step 3's `loadVoicegroupForSession` must replace.)*
- *(Step 1)* `loadSong` copies its `SongInfo` and re-resolves it by label
  after the save prompt: answering Save on a draft replaced in place
  commits it, and the commit's project reload replaces the song list a
  caller's reference points into (ids may shift).
- Things keyed by label that write on their own:
  `saveViewState` (`:3998`, called from `:1645`, `:1761`, `:1868`,
  `:4034`) writes `.porydaw/<label>.json`; `persistOpenTabs` (`:1577`)
  records labels; `refreshSessionSongIds` (`:1601`) re-matches by label
  after every reload.
- `sessionForLabel` (`:1276`) matches every non-bundle session, drafts
  included. That's what we want: it stops a second tab from opening the
  same label.
- Voicegroup loading: `loadVoicegroupFor(root, cfg)` (`:1829`) calls
  `voicegroup_load` without a config. Preview reloads
  (`reloadVoicegroupPreview`, `:3774`) pass a config whose
  `voicegroupPaths[0] = ".porydaw/vgpreview"`, which is searched **before**
  the project's own voicegroups. `cleanupVgPreview` (`:3889`) wipes
  `vgpreview` on many occasions, so draft files must **not** live there.
- `VoicegroupSource::open` (`src/project/voicegroupsource.h:218`) finds and
  parses a file on disk. `createVoicegroupFromLines`
  (`voicegroupsource.cpp:1601`) copies formatting from sibling files
  (`:1624`), so the bytes it writes depend on the project.
- `exportBundleByLabel` (`:2485`) already exports an open tab from memory
  (`sessionForLabel` → `open->doc`, `open->vgSource`), so a draft can
  probably be exported as a `.porysong` with no extra work. It passes
  registration hints only from a project `SongInfo`, and a draft has none.
- `refreshRegisterAction` (`:3197`) is already disabled when `songId < 0`.
- `NewSongWizard` checks names at `src/ui/newsongwizard.cpp:97-105`
  (label against `m_project->songs()`, `.mid` existence), and at `:164` /
  `:210` for the new-voicegroup option. It knows nothing about open drafts.
- Harnesses that drive `MainWindow` (`runTabCheck`, `runVgSaveCheck`,
  `runBundleTabCheck`, …) are declared in `src/mainwindow.h:79-149`,
  dispatched from `src/main.cpp`, and listed in `tools/run_checks.sh`.
  Follow that pattern. The file dialog in `importMidi` means a harness has
  to go in **below** the dialog.

## 4. Steps

Each step lists **Scope**, **Changes**, **Acceptance**. "Harness" means the
new `--draftcheck` (step 1 creates it; each later step adds a section).

### Step 1 — Draft sessions for songs using an existing voicegroup

**Scope:** Import MIDI and New Song, **when the wizard did not ask for a
new voicegroup**, open a draft tab, and the first Save commits it. When a
new voicegroup *was* requested, keep today's write-through path for now
(step 3 removes it).

**Changes**

1. `SongDocument::loadDraft(SmfFile smf, const SongInfo &song)`: the same
   post-read steps as `load()` (factor them into a private helper both
   call), with no file read. The caller passes `midPath` = the final
   `sound/songs/midi/<label>.mid` path and `hasCfg=false`.
2. `SongSession` gets `std::unique_ptr<SongDraft> draft` (new struct,
   holding `constant`, `player`, `newVoicegroup` (empty for now), and later
   the draft folder and lock). `isDirty()` becomes
   `!bundle && (draft || doc.isDirty() || vgDirty)`. Add
   `bool isDraft() const`.
3. Split `finishCreateSong` in two:
   - `openDraftSong(SmfFile, label, constant, player, cfg, newVg)`: public,
     so the harness can call it. Builds a `SongInfo`, loads the voicegroup,
     creates the session through the same code as `loadSong` (extract a
     shared helper; don't copy it), then `loadDraft`, the tab, and
     activation. No project writes.
   - `commitDraft(SongSession&, QString *error)`: runs the write sequence
     in the order it runs today (the voicegroup step is a no-op until step
     3): `doc.save()` (writes `.mid` + flags) → `registerSong` (on failure:
     `saveRegistrationMeta` + the existing warning; the session still
     commits, as an unregistered song) → clear `draft` →
     `reloadProjectOrWarn()` (its `refreshSessionSongIds` now finds the
     song) → `persistOpenTabs()` → tab
     title, tooltip, window title, song-list selection. Leave out any step
     whose output already exists and matches (stance 5).
   - `importMidi` / `newSong` call `openDraftSong` when `newVg` is empty,
     and otherwise the old path (rename it
     `finishCreateSongWriteThrough`, to be deleted in step 3).
   - The wizard's `NewSongWizard::songFile()` (`newsongwizard.h:44`) hands over the
     converted SMF. It is what the old path wrote
     (`smf.writeFile` at `:3145`); pass the same `SmfFile` to `loadDraft`.
4. `saveSession`: if `session.draft`, call `commitDraft` in place of the
   `doc.save()` block. The voicegroup-dirty block above it still runs
   first. An existing voicegroup edited in a draft saves exactly as it
   does today.
5. Drafts are left out of: `saveViewState` (return early),
   `persistOpenTabs` (skip, as for bundles), and `refreshSessionSongIds`
   (keep `-1` while it's a draft).
6. Harness `--draftcheck SCRATCH` (`src/draftcheck.cpp`,
   `MainWindow::runDraftCheck`, `src/main.cpp`, `tools/run_checks.sh`,
   CMake). It snapshots a hash of every file under `SCRATCH` except
   `.porydaw/`, and then checks:
   - opening a draft (a small hand-built SMF, an existing voicegroup) →
     tree hash unchanged, `isDirty()`, tab title ends in `*`, `songId==-1`;
   - editing a note → still unchanged on disk;
   - discarding (close the tab through the path `maybeSaveSession`'s
     Discard takes; add a test-only way to answer it without a modal if
     the other harnesses don't already have one) → hash unchanged and no
     `.porydaw/<label>.json`;
   - a second draft, then Save → `.mid` exists, flags present, song
     registered (`m_project.songs()` has it with `registered`), same
     session pointer and view, the undo stack is clean and **still holds
     the pre-save edit** (undo works), `songId>=0`, the label is in
     `lastOpenSongs`.

**Acceptance:** `--draftcheck` passes. The full `tools/run_checks.sh`
passes on the normal and ASAN builds. A manual Import MIDI → close →
`git status` in the project shows nothing.

### Step 2 — Name reservation and commit-time conflicts

**Scope:** Two drafts can't claim the same names, and a name taken
between import and Save is handled (D2).

**Changes**

1. `NewSongWizard` takes a list of **reserved names** (labels, constants,
   voicegroup names of open drafts) from `MainWindow`, and rejects them
   with the same messages it uses for project collisions (`:97-105`,
   `:210`).
2. `SongRegistry` (or a small new helper) gets
   `checkNewSongNames(root, songs, label, constant, newVg) -> QStringList
   conflicts`, looking at the **disk** state (the `.mid` exists; the label
   or constant is in `song_table.inc`/`songs.h`; the voicegroup file
   exists), not only at `m_project`, which may be stale after a `git pull`.
   The wizard and `commitDraft` share this so the rules can't drift.
3. `commitDraft` runs the check first. On a conflict, it writes nothing,
   explains the problem, and opens a **Rename** dialog: reuse the wizard's
   name page if it can be separated cleanly, or otherwise build a small
   dialog on the shared check. Accepting it updates the draft's label
   (`doc` label and `midPath`; add a narrow setter on `SongDocument` for
   drafts only), its constant, and (after step 3) the new voicegroup's
   name, then continues the commit. Cancel leaves the draft dirty, and the
   Save counts as failed (so a close prompt stays open).
4. Harness: a second draft that takes a reserved label is rejected by the
   wizard's check; a draft whose `.mid` is created behind its back hits
   the conflict. Drive Rename with the test-answer hook, then check the
   commit landed under the new name and the other file is untouched.

**Acceptance:** harness sections pass, and the full sweep passes.

### Step 3 — Deferred new voicegroup

**Scope:** "New voicegroup" in the wizard no longer writes anything until
commit. Delete `finishCreateSongWriteThrough` (and the branch to it in
`createSongFromWizard`, step 1's shared New Song / Import finish).

**Changes**

1. `VoicegroupSource::renderNewVoicegroup(projectRoot, name, copyFrom,
   ...) -> QByteArray`: the byte-producing half of
   `createVoicegroup`/`createVoicegroupFromLines`, which keep their
   signatures and now write what it returns. This guarantees that the
   draft's in-memory copy **is** exactly the file the commit would write.
2. `VoicegroupSource::openDraft(projectRoot, name, bytes, targetPath)`:
   parses `bytes` as though they were read from `targetPath` (which doesn't
   exist yet). `dirty()` compares against those bytes, and `save()`
   creates the file.
3. Draft folder: `<root>/.porydaw/drafts/<uuid>/` holds
   `<loadName>.inc`, rewritten after every voicegroup edit before it's
   reloaded. A `QLockFile` held by the session guards it. At project open,
   sweep `drafts/` and remove folders whose lock is stale (skip locked
   ones, since another Porydaw instance may own them). The session
   removes its own folder when it's destroyed and after commit.
4. Voicegroup loading that knows about drafts: every place that loads a
   session's voicegroup (`loadVoicegroupFor` callers, `saveSession`'s
   reload at `:2422`, activation's staleness reload at `:1543`, the cfg
   voicegroup change at `:2300`, and `populateSession` — `loadSong`'s body
   since step 1) goes through a new `loadVoicegroupForSession(session, cfg)`.
   While the draft's new voicegroup exists, it adds `.porydaw/drafts/<uuid>`
   as `voicegroupPaths[0]`. `reloadVoicegroupPreview` is unchanged, since
   its preview file still shadows everything.
5. The catalog / voicegroup dock choices (`updateVoicegroupBrowser`,
   `:3502`) include the draft voicegroup's argument while this draft
   exists. `session.vgFileTime` stays null for a draft voicegroup, so the
   staleness reload never fires on it.
6. `commitDraft`, voicegroup step (before `.mid`, as today): **only if**
   `doc.cfg().voicegroupArg` still names the draft voicegroup (the user
   may have switched in Song Settings). Write the file through the
   vgSource's `save()`, so edits are included, then run
   `appendIncludeLine`, skipping any part that has already been done.
   Retarget the session's loads to the project copy, delete the draft
   folder, and set `vgFileTime`. If it was abandoned, write nothing.
7. The `m_pendingSynths` handling in `saveSession` already runs before the
   voicegroup save. Check that it covers the draft voicegroup too.
8. Harness: draft with a new voicegroup → nothing outside `.porydaw/` is
   written; edit a voice → heard (the voicegroup reloads with the new
   ToneData), still unwritten; discard → the draft folder is gone; draft
   again → Save → `.inc` written **with the edit**, include line present,
   `.mid`'s cfg `-G` matches; draft whose cfg switches away → Save → no
   `.inc` written. Stale-folder sweep: plant a folder with no lock, reopen
   the project, and check it's removed.

**Acceptance:** harness passes; `vgcheck`, `vgsavecheck` and
`onboardcheck` still pass; the full sweep passes on normal and ASAN.

### Step 4 — Draft UX

**Scope:** Users can see that a tab is a draft and what Save and close
will do.

**Changes**

1. A banner strip above the ruler on draft tabs. Reuse the bundle banner
   code (`:2080-2110`). Text: "*<label>* isn't in your project yet. Save
   adds it; closing the tab discards it." Include a **Save to project**
   button.
2. The tab title gets the same kind of marker a bundle tab has (for
   example `label*` plus an italic font, or a `[draft]` prefix; pick one
   that fits `bundleTabTitle`). The tooltip explains it in place of the
   `.mid` path, which doesn't exist yet. The window title
   (`updateWindowTitle`, `:4007`) follows.
3. The `maybeSaveSession` prompt for a draft reads "Add *<label>* to the
   project?" with the buttons **Add to Project / Discard / Cancel**
   (Discard stays the destructive action).
4. After commit, the status message says the same thing
   `finishCreateSong` says today ("Created and registered … (song ID n)",
   plus the configure-voicegroup hint when one was created). *(Step 1:
   `commitDraft` already shows the "Created and registered" message; only
   the voicegroup hint is left. `openDraftSong` shows a placeholder
   "Opened … — nothing is in the project until you save it" to replace.)* Import itself
   says "Imported *<label>* as a draft — Save to add it to the project."
5. Manual docs: add a short section to `docsrc/manual/` wherever Import
   MIDI / New Song are described. Add a CHANGELOG entry that follows the
   existing convention.

**Acceptance:** the harness checks the banner exists on a draft tab and is
gone after commit (find the banner by object name). Take screenshots of
the tab and banner offscreen and include them in the progress entry. Full
sweep.

### Step 5 — The rest of the app

**Scope:** check every other place that has an opinion about a session,
and fix it.

Checklist (each gets a harness assertion, or a written reason why none is
possible):

- [ ] **Song Settings** on a draft: edits `doc.cfg()`. The commit then
      writes the flags from the edited cfg (via `m_hadCfgLine=false`).
      `m_project.setSongCfg` stays skipped while `songId<0`.
- [ ] **Export Bundle** on a draft: exports from memory. Pass the draft's
      constant and player as registration hints
      (`exportBundleByLabel`, `:2524`).
- [ ] **Export WAV** works on a draft.
- [ ] **Register Song** is disabled on a draft (already true via
      `songId<0`; assert it).
- [ ] **Import Sample for slot** into a draft voicegroup: the sample is
      write-through by design (`importSampleForSlot`), but the voice
      assignment rides the draft's voicegroup. Confirm it works and is
      only committed with the draft. The sample file itself is still
      written right away; note this in the manual.
- [ ] **Project switch** with a draft open: the prompt → Add commits into
      the **old** project before the switch; Discard drops it.
- [ ] **Quit** with a draft open: same prompt.
- [ ] **Scripting** (`src/scripting/scriptapi.cpp`): `song.midPath`
      (`:1225`) returns the future path. Add `song.isDraft` (update
      `docs/scripting/API.md` and `porydaw.d.ts`). The view-sidecar path
      API (`:1795-1810`) returns empty for a draft. `openSong(label)`
      (`:2542`) on a draft's label focuses the draft tab. `saveSong`
      commits it. Add a `scriptcheck` case.
- [ ] **Loading the same label from the browser** while a draft holds it:
      impossible after step 2, since the label is reserved. Assert that
      the wizard rejects it.
- [ ] **`cleanupVgPreview`** doesn't touch `drafts/`.

**Acceptance:** checklist ticked in the progress entry. Full sweep on
normal and ASAN.

### Step 6 — Final review

Run `/code-review` over the combined diff since step 1, fix what it finds,
run the full sweep on normal and ASAN, and make a manual pass on a real
project: Import → play → edit → close → `git status` clean; Import → Save
→ build the ROM (`make`) to confirm the committed song compiles.

## 5. Progress log

Append one entry per step: date, commit(s), what deviated from the plan
and why, what's still owed.

- **2026-09-26 — Step 1 (draft sessions, existing voicegroup).** Commit:
  see `git log` on branch `draft-song` ("Draft songs step 1 …").
  - `SongDocument::loadDraft(const SmfFile&, const SongInfo&, QString*)`
    and the private `adopt()` that `load()` now shares.
    `SongDraft` + `SongSession::draft` / `isDraft()` / the draft term in
    `isDirty()` (`src/songsession.h`).
    `MainWindow::populateSession` (extracted from `loadSong`),
    `openDraftSong` (public), `commitDraft`, `createSongFromWizard`
    (the shared `newSong`/`importMidi` finish that picks draft vs.
    write-through), `finishCreateSongWriteThrough` (renamed old path).
    `saveSession` routes drafts to `commitDraft`; `saveViewState`,
    `persistOpenTabs` (incl. `lastSongLabel`) and `refreshSessionSongIds`
    skip drafts.
  - Harness `--draftcheck SCRATCH` (`src/draftcheck.cpp`,
    `MainWindow::runDraftCheck`), in `tools/run_checks.sh`. Sections:
    open (tree unchanged, dirty via draft only, `*`, `songId==-1`, engine
    bound, not persisted) → edit → Discard via the real `closeTab` /
    `maybeSaveSession` prompt → second draft + Save (in place, same
    session/view, `.mid` == document, registered, flags, `songId`,
    persisted, pre-save edit undoable) → **extra section 5**: a draft
    replaced in place by a browser load, answering Save.
  - Results: `--draftcheck` PASS; full `tools/run_checks.sh` PASS on the
    normal and ASAN builds (mkcheck skipped as usual, no fork given).
    Mutation-tested: dropping the `saveViewState`/`persistOpenTabs`
    exclusions fails the harness. Manual: the real File → Import MIDI
    flow (file dialog + wizard driven offscreen from a throwaway patch,
    not committed) → edit → play → close → Discard left the scratch
    project's `git status` clean and wrote no `.porydaw/<label>.json`.
  - Deviations: (1) `loadDraft` takes `const SmfFile&` plus `QString
    *error` and round-trips the SMF through `write()`/`read()` so the
    document is exactly what `load()` would read back after the commit
    (same encoding, format-1 coercion) — hence it can fail. (2) No new
    test hook for the Save/Discard prompt: the harness answers the real
    `QMessageBox` from a timer inside its `exec()`, as `vgsavecheck`
    does for its dialogs. (3) The harness "tree hash" is a digest of
    every file's relative path + size + mtime outside `.porydaw/`, not
    of contents (815 MB tree; any write moves the mtime, so it is
    stricter, not looser). (4) The `loadSong` copy/re-resolve above
    (fact added to §3), covered by harness section 5 — which passes
    with or without the fix on vanilla pokeemerald (no ASAN report:
    the reload happens not to free/shift the referenced entry there),
    so the fix is defensive. (5) `commitDraft` reports doc-save failure
    through `saveSession`'s existing "Save Song" box; the registration
    failure keeps its own warning (re-titled "Save Song") and still
    commits.
  - Owed: nothing for step 1. The draft tab tooltip is the future
    `.mid` path and the close prompt still reads "has unsaved changes"
    (step 4). A draft can't yet be recognised by any UI beyond `*`.
