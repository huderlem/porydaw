# Draft Songs — Import MIDI / New Song without touching the project

Status: **decisions confirmed 2026-09-26**; steps 1–4 done 2026-09-26. File/line references verified
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
  shared by `loadSong` and `openDraftSong`. Step 3: it loads through
  `loadVoicegroupFor(root, cfg, tried, draft)` with the incoming draft,
  since the session may not exist yet and the draft is installed only
  after the document is read.)*
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
  *(Step 3: every session load goes through
  `loadVoicegroupForSession(session, cfg, tried)` →
  `loadVoicegroupFor(root, cfg, tried, draft)`, which puts
  `SongDraft::folderRelative` (`.porydaw/drafts/<uuid>`) in
  `voicegroupPaths[0]` while `draft->voicegroupPending()`. The bundle-tab
  load keeps the plain call: bundles are never drafts.
  `SongSession::editsDraftVoicegroup()` is true while the open
  `vgSource` is the draft's unwritten voicegroup (`openDraft`, target
  `sound/voicegroups/<name>.inc`, no mtime); `saveSession`'s
  dirty-voicegroup block skips it and `commitDraftVoicegroup` writes it.
  `VoicegroupSource::appendIncludeLine` is now idempotent.)*
  *(Step 4: a draft's `saveSession` calls `resolveDraftNameConflicts`
  first, before the dirty-voicegroup block, so a cancelled Rename writes
  nothing — not even an edited existing voicegroup. `commitDraft` assumes
  the names are settled.)*
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
  *(Step 2: the label/constant rows are now `SongNameFields`, shared with
  `SongRenameDialog`; every name decision — label, `.mid`, constant, new
  voicegroup file — goes through `SongRegistry::checkNewSongNames(root,
  songs, ReservedSongNames, label, constant, newVg[, ownMidPath]) ->
  SongNameConflicts`, which reads `song_table.inc`/`songs.h`/the files on
  disk; `ownMidPath` waives a `.mid` the draft itself wrote in a failed
  commit (`SongDraft::wroteMidPath`). The wizard gets
  `MainWindow::reservedSongNames()`: every open project tab's label, and
  each draft's constant and `newVoicegroup`. The Sound page still checks
  the cached `-G` catalog first, then the shared check's voicegroup
  field.)*
  *(Step 3: the voicegroup test also asks `VoicegroupSource::isDeclared`
  whether any voicegroup file declares `voicegroup_<name>` — a full,
  substring-gated read of the voicegroup files, so the wizard, the Rename
  dialog and the commit all see a symbol declared inside another file.
  (Step 3 review B: the Rename dialog's per-keystroke check instead reads
  `VoicegroupSource::declaredSymbols` once per dialog and passes the set
  through `checkNewSongNames`' optional `declaredVoicegroups`.)
  `resolveDraftNameConflicts` passes the new voicegroup only while the
  commit would write it: pending and still named by the cfg.)*
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
   *(Done in step 3. Step 2 hooks: `reservedSongNames` already reserves
   `draft->newVoicegroup`, and `resolveDraftNameConflicts` already passes it
   to the shared check and the Rename dialog (`renamesVoicegroup`), then
   sets `draft->newVoicegroup = <new label>` on accept. Step 3 must extend
   that accept path to rename the draft voicegroup's file in the draft
   folder, its `voice_group`/symbol, and the cfg's `-G` arg — **only if**
   the cfg still names the draft voicegroup — before the commit continues.
   The harness should then rename a draft that has a new voicegroup.)*
   *(Review of step 2, 2026-09-26, E: `checkNewSongNames`' voicegroup test
   is file-name-only — it asks whether `sound/voicegroups/<name>.inc`
   exists (`songregistry.cpp`, the `newVoicegroup` branch). But
   `voicegroup_<name>` can be defined inside another file (a multi-group
   `.inc`, monolithic layouts), so a free file name can still collide on
   the symbol. Step 3 must feed the catalog's group args (`vgCatalog(root)
   .groupArgs`) or a voice_group label index into the shared check, so the
   wizard, the Rename dialog and `commitDraft` all see it; the harness must
   cover a name whose file doesn't exist but whose symbol does. Done in
   step 3.)*
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

**Changes** *(all done in step 4; see its progress entry)*

1. ✅ A banner strip above the ruler on draft tabs. Reuse the bundle banner
   code (`:2080-2110`). Text: "*<label>* isn't in your project yet. Save
   adds it; closing the tab discards it." Include a **Save to project**
   button.
2. ✅ The tab title gets the same kind of marker a bundle tab has (for
   example `label*` plus an italic font, or a `[draft]` prefix; pick one
   that fits `bundleTabTitle`). The tooltip explains it in place of the
   `.mid` path, which doesn't exist yet. The window title
   (`updateWindowTitle`, `:4007`) follows.
3. ✅ The `maybeSaveSession` prompt for a draft reads "Add *<label>* to the
   project?" with the buttons **Add to Project / Discard / Cancel**
   (Discard stays the destructive action).
4. ✅ After commit, the status message says what the old write-through
   `finishCreateSong` said ("Created and registered … (song ID n)",
   plus " — configure its new voicegroup in the Voicegroup dock" when one
   was created; step 3 deleted that function). *(Step 1:
   `commitDraft` already shows the "Created and registered" message; only
   the voicegroup hint is left. `openDraftSong` shows a placeholder
   "Opened … — nothing is in the project until you save it" to replace.)* Import itself
   says "Imported *<label>* as a draft — Save to add it to the project."
5. ✅ *(Review of step 2, 2026-09-26, F.)* The Rename dialog says "The song
   hasn't been written yet." That is not the whole truth when the draft
   edited an **existing** voicegroup: `saveSession`'s dirty-voicegroup
   block runs before `commitDraft`'s name check, so that `.inc` was already
   saved when the dialog opens (and stays saved on Cancel). Either the
   dialog says so when it happened, or this step moves the name check ahead
   of the voicegroup save (so Cancel really writes nothing) — decide and
   cover it in the harness.
6. ✅ Manual docs: add a short section to `docsrc/manual/` wherever Import
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
      *(Step 2: `--draftcheck` section 7 already asserts the wizard
      rejects an open draft's label and constant; tick this with a
      reference to it.)*
- [ ] **`cleanupVgPreview`** doesn't touch `drafts/`. *(Step 3:
      `--draftcheck` section 13 already asserts the draft folder survives
      `cleanupVgPreview`; tick this with a reference to it.)*
- [ ] **New Voicegroup (dock) / `project.createVoicegroup` (scripting)**
      while a draft reserves a voicegroup name (step 3 note): both write
      through and don't consult `reservedSongNames`, so they can create
      the draft's voicegroup name first. The commit's name check then
      catches it (Rename), so nothing is overwritten; decide whether
      either should refuse a reserved name up front.
- [ ] **New voicegroup vs. other writers and scripts** (step 3 review,
      H; extends the item above): `createVoicegroupNamed`, bundle import,
      scripting `project.voicegroups()` and `edit.setSettings({voicegroup})`
      (`scriptapi.cpp` ~2336, ~2858) use the raw catalog, not
      `voicegroupChoices` — a script can't switch a draft back to its own
      new voicegroup, and the other writers don't refuse a draft-reserved
      name up front (the commit's Rename still catches it).
- [ ] **Failed commit after the `.mid` write** (review 2026-09-26, E):
      `commitDraft` calls `doc.save()`, which writes the `.mid` and then the
      flags; if the flags write fails, the `.mid` stays on disk while the
      session is still a draft. A later Discard then leaves a stray,
      unregistered `.mid` in the project. Decide: either the commit removes
      a `.mid` it wrote itself when the rest of the commit failed (only if
      the file did not exist before the commit), or the close prompt says
      the `.mid` was already written. Add a harness case with an unwritable
      `midi.cfg`/`songs.mk`.
      *(Step 3 review, F: the same holds for the new voicegroup. Once
      `vgSource->save()` succeeds, `voicegroupWritten` is set and the draft
      folder removed, so a commit that then fails (the `.mid` or flags
      write) and is followed by Discard leaves
      `sound/voicegroups/<name>.inc` and its `voice_groups.inc` include line
      orphaned. Decide roll-back vs. warn-on-Discard for the `.mid` and the
      `.inc` together; `--draftcheck` section 18 already produces that
      state.)*
- [ ] **Voicegroup symbols the name check can't see** (step 3 review, G;
      known limitation, consistent with `VoicegroupSource::open()` and the
      catalog): `isDeclared` / `declaredSymbols` / the catalog scan only
      `.inc` files and the `voicegroup…::` / `voice_group` forms. They miss
      `sound/voicegroups/<name>.s` (the loader resolves it,
      `voicegroup_loader.c:2108`) and single-colon `voicegroup_x:` labels
      (`bundleimport.cpp:140` accepts them), so such a name can pass the
      check and fail the ROM build on a duplicate symbol. Fix all four
      together if ever.
- [ ] **Register Song after a failed reload** (review 2026-09-26, F): if
      `reloadProjectOrWarn()` fails inside `commitDraft`, the committed
      session keeps `songId == -1`, so Register Song stays disabled even
      though the registration-failure warning may just have said "use
      File → Register Song to retry". Either enable Register Song for a
      committed session whose ID is unresolved (by label), or resolve
      `songId` directly from the registration result.
- [ ] **`storage.song` on a draft** (review 2026-09-26, G; part of the
      Scripting item above, called out because it writes today):
      `storage.song.set`/`remove` (`StorageApi::songSet` →
      `writeSongStore`, `src/scripting/scriptapi.cpp` ~1819–1884) resolve
      `ViewSidecar::pathFor(root, label)` and so write
      `.porydaw/<label>.json` for a draft — a project write before the
      commit. Gate writes on a draft (throw, like a bundle tab, or hold the
      store in memory and flush it at commit). Reads (`songStore`) can
      also pick up a stale sidecar left by an earlier song of the same
      name; a draft should read an empty (or in-memory) store.

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
    (fact added to §3) is defensive and NOT exercised by the harness on
    vanilla pokeemerald: `registerSong` appends the new song after the
    existing IDs (vanilla has no free slots), so a commit's reload never
    shifts a registered song's ID, and the reload happened not to free the
    referenced entry either (no ASAN report without the fix). Section 5
    passes with or without it; only a project where registration fills a
    freed slot, or where the reload reallocates the list, would tell. (5) `commitDraft` reports doc-save failure
    through `saveSession`'s existing "Save Song" box; the registration
    failure keeps its own warning (re-titled "Save Song") and still
    commits.
  - Owed: nothing for step 1. The draft tab tooltip is the future
    `.mid` path and the close prompt still reads "has unsaved changes"
    (step 4). A draft can't yet be recognised by any UI beyond `*`.
  - **Review fixes (2026-09-26).** A code review of step 1 found:
    (A) two open drafts could share a label — Import MIDI twice as the same
    name opened two draft tabs whose commits overwrote one `.mid`. Stopgap:
    `openDraftSong` now refuses when `sessionForLabel(label)` already holds
    a tab (draft or committed), with an error the wizard's caller shows in
    its warning box; proper name reservation stays with step 2. (B) The
    harness never answered Discard for a draft replaced in place (section 5
    answers Save, so `draft` was already cleared before `populateSession`
    reassigned it). (C) The log entry above overstated section 5's coverage
    of the `loadSong` re-resolve; reworded. (D) `answerNextPrompt` could
    fire late into an unrelated box, and a surprise warning box hung the
    harness. Harness changes (`src/draftcheck.cpp`): a scoped
    `PromptAnswerer` polls `QApplication::activeModalWidget()` every 10 ms
    while the action runs, answers only a box titled "Unsaved Changes",
    records any other modal as a failure and dismisses it, and stops in its
    destructor (so nothing leaks into a later section); each prompt is
    asserted to have appeared, and the save calls run under a no-prompt
    guard. New section 2b: a second draft under an open draft's label is
    refused, the first draft untouched (tabs, notes, tree); plus a refusal
    under a committed song's open label in section 4. New section 6: a
    draft replaced in place by a browser load, answering Discard — the tab
    is an ordinary song (`!isDraft()`, resolved `songId`, persisted),
    nothing of the draft is on disk or in the song list, and its next edit
    + save rewrites the `.mid` without touching `song_table.inc`/`songs.h`.
    Mutation-tested: removing the label guard fails 2b and section 4's
    refusal; making `populateSession` keep the old draft on a Discard fails
    section 6; forcing `commitDraft`'s registration-failure warning fails
    the harness (as "unexpected modal") instead of hanging it. Results:
    `--draftcheck` PASS normal + ASAN; full `tools/run_checks.sh` PASS on
    both builds (mkcheck skipped, no fork). Deferred to step 5's checklist
    (E–G above): stray `.mid` after a half-failed commit, Register Song
    disabled after a failed reload, and `storage.song` writing a draft's
    sidecar — each needs a product decision or belongs with the step-5
    feature it touches, not a step-1 regression.
- **2026-09-26 — Step 2 (name reservation and commit-time conflicts).**
  Commit: see `git log` on branch `draft-song` ("Draft songs step 2 …").
  - `SongRegistry::checkNewSongNames` + `ReservedSongNames` +
    `SongNameConflicts` (`src/project/songregistry.{h,cpp}`): label (song
    list, open tabs, `song_table.inc` on disk), `.mid` on disk, constant
    (song list, drafts, any `#define` of it in `songs.h`), new voicegroup
    (drafts, `sound/voicegroups/<name>.inc`). Empty arguments skip their
    checks.
  - `NewSongWizard` takes a `ReservedSongNames` (defaulted, so harness
    callers are unchanged); `newSong`/`importMidi` pass
    `MainWindow::reservedSongNames()`. The Identity page's name rows moved
    into `SongNameFields` (file-local in `newsongwizard.cpp`), shared with
    the new `SongRenameDialog` (declared in `newsongwizard.h`, title
    "Rename Song", OK = "Rename and Save", disabled while any name
    conflicts). The Sound page's new-voicegroup validation adds the shared
    check after its catalog check.
  - `commitDraft` first calls `MainWindow::resolveDraftNameConflicts`: on a
    conflict, nothing is written; the Rename dialog lists the conflicts and,
    on accept, `SongDocument::setDraftIdentity(label, midPath)` renames the
    document, `draft->constant` follows, and tab title/tooltip/window title
    update; the commit then continues. Cancel → `commitDraft` returns false
    with an empty error, and `saveSession` shows no second box (the dialog
    already explained), so Save fails and a close prompt stays open.
  - `SongDraft::wroteMid`: set when a commit attempt wrote the `.mid` and
    then failed (the flags write), so a retry's name check doesn't count
    the draft's own file as taken (stance 5). A rename after such a
    failure removes that stale `.mid` under the old name.
  - `openDraftSong`'s step-1 label refusal stays as the last line of
    defense (comment updated).
  - Harness (`src/draftcheck.cpp`): `PromptAnswerer::onDialog(title,
    action)` / `handled(title)` drive a registered dialog by title
    (anything else still fails and is dismissed). Section 7: the wizard,
    driven offscreen like `onboardcheck`, rejects an open draft's label
    (same "A song named … already exists." hint) and constant, and a
    constant `songs.h` defines; without the reservation the label passes;
    `checkNewSongNames` with an empty song list still finds
    `mus_littleroot`'s label/`.mid`/constant on disk, an existing
    voicegroup file, and a reserved voicegroup, and accepts free names.
    Section 8: a `.mid` planted under an open draft's label → Save opens
    Rename (OK disabled on the taken name, enabled on a free one) → the
    commit lands under the new label and its label-derived constant
    (registered, flags, `songId`, persisted, tab text/tooltip), the old
    names are in no registration file, and the planted file's bytes and
    mtime are untouched. Section 9: Cancel on Rename → `saveSession`
    false, no other modal, tree fingerprint unchanged, still a dirty draft
    under its old label, not persisted.
  - Results: `--draftcheck` PASS on the normal and ASAN builds;
    `--onboardcheck` PASS; full `tools/run_checks.sh` PASS on both builds
    (mkcheck skipped, no fork given). Mutation-tested: skipping the name
    check in `commitDraft` fails sections 8–9; dropping labels from
    `reservedSongNames` fails section 7.
  - Deviations: (1) `checkNewSongNames` returns a `SongNameConflicts`
    struct (one message per name, `messages()` for the list) instead of a
    `QStringList`, so the wizard's pages can each show their own field and
    `commitDraft` can waive the draft's own `.mid`. It also takes the
    reserved names, so the Rename dialog can't rename into another draft's
    names. (2) New wizard behavior: the constant is now checked (the
    wizard never checked it before) — against `songs.h` and open drafts.
    Message: "Another song already uses the constant …". (3) The
    explanation is the Rename dialog's own header, not a separate message
    box before it (one modal instead of two). (4) The Rename dialog is a
    small `QDialog` on the shared `SongNameFields`, not the wizard's
    Identity page itself, which also carries the player combo and wizard
    page chrome. (5) Reserved labels include every open project tab, not
    only drafts (matches the step-1 refusal in `openDraftSong`).
  - Owed: step 3 must finish the voicegroup rename (see the note under
    step 3's Changes, "Step 2 hooks"). The wizard's Sound-page voicegroup
    rejection is a modal warning and is covered only through
    `checkNewSongNames` directly, not by driving the page.
  - **Review fixes (2026-09-26).** A code review of step 2 found:
    (A) the `wroteMid` waiver was incomplete — it cleared only the `.mid`
    conflict, but after any project reload `discoverUnregisteredSongs`
    lists the draft's own leftover `.mid` as an unregistered song with the
    draft's label (and label-derived constant), so the retry still opened
    Rename, where `SongNameFields` knew nothing of the waiver and disabled
    OK on the original label (stance 5 broken). Fix: the waiver is a
    first-class `ownMidPath` argument of `checkNewSongNames` — that exact
    `.mid` doesn't count as existing, and an **unregistered** song whose
    `midPath` is it doesn't count for the label or constant (`song_table.inc`
    and `songs.h` still do). `SongDraft::wroteMid` became `QString
    wroteMidPath`, passed to the check and through `SongRenameDialog` to
    `SongNameFields`. (B) Stale label caches after a rename: the script
    host's session label (so every `song.changed` for the tab was dropped
    and no `song.activated` fired for the new name) and the transport's
    song label. Fix: `MainWindow::refreshSessionIdentity(session)` — tab
    title/tooltip and, for the active session, window title, transport
    label and `ScriptHost::setSession` (a no-op unless the label changed,
    then `song.activated` with the new label; no engine rebind). Called
    after a rename and at the end of every commit. (C) The old `.mid`'s
    delete after a rename ignored failure: now a "Save Song" warning names
    the stray file and `wroteMidPath` keeps it (a rename back waives it
    again). (D) Harness gaps, below.
  - Harness (`src/draftcheck.cpp`): section 7 also rejects a `songs.h`
    define absent from the song list (vanilla's hex `MUS_ROUTE118`; a
    planted alias otherwise). Section 8 asserts the transport label, window
    title, a `song.activated` with the new label and a `song.changed` after
    an edit (console-engine listeners). Section 10: a constant-only conflict
    (an alias define planted in `songs.h`) → Rename keeps the label, changes
    only the constant, commits under the original label. Section 11:
    `midi.cfg` made read-only (probed; skipped with a note if the chmod
    doesn't bite, e.g. as root) → Save fails after the `.mid` lands →
    `reloadProject` lists it unregistered (asserted, and the un-waived check
    flags label/`.mid`/constant) → retry with no changes commits under the
    original names with no dialog. Section 12: the same partial commit plus
    a planted constant → Rename opens for the constant alone, OK enabled on
    the original label once the constant changes.
  - Mutation-tested: dropping the waiver from the check fails 11, from the
    dialog fails 12; dropping the script-host refresh fails 8. C has no
    harness case: making the old `.mid` undeletable needs a read-only
    `midi/` directory, which also blocks the `.mid` write itself.
  - Results: `--draftcheck` PASS on the normal and ASAN builds (sections
    11–12 ran, not skipped); full `tools/run_checks.sh` PASS on both builds
    (scriptcheck included; mkcheck skipped, no fork given).
  - Deferred into the plan: the voicegroup-symbol check (step 3, review E)
    and the Rename wording after an existing voicegroup was saved (step 4,
    review F).
- **2026-09-26 — Step 3 (deferred new voicegroup).** Commit: see `git log`
  on branch `draft-song` ("Draft songs step 3 …").
  - `VoicegroupSource`: `renderNewVoicegroup` / `renderNewVoicegroupFromLines`
    (the byte-producing halves; `createVoicegroup` / `createVoicegroupFromLines`
    keep their signatures and write exactly those bytes, now also checking
    the write), `newVoicegroupPath`, `openDraft(root, name, bytes,
    targetPath, error)`, `isDeclared(root, arg)`; `appendIncludeLine` is a
    no-op when the hub already includes the file.
  - `SongDraft` (`src/songsession.h`): `voicegroupTarget`,
    `voicegroupBytes`, `folder` / `folderRelative`, `lock` (`QLockFile`
    `<folder>/.lock`, stale time 0), `voicegroupWritten`,
    `voicegroupPending()`, `voicegroupArg()`, `removeFolder()` (also run by
    its destructor, so discard / replace / commit / quit all remove the
    folder). `SongSession::editsDraftVoicegroup()`.
    `SongDocument::renameDraftVoicegroupArg(old, new)` (current cfg, saved
    cfg, and the `SongCfgCommand`s in the undo history).
  - `MainWindow`: `openDraftSong` renders the voicegroup, creates the
    locked folder (`createDraftFolder`) and writes `<name>.inc` there;
    `openVoicegroupSource` opens the draft's bytes via `openDraft`
    (`vgFileTime` stays null); `onVoiceEdited` and the -G switch's replay
    rewrite the folder copy (`syncDraftVoicegroupFile`);
    `loadVoicegroupForSession` at the activation staleness reload,
    `refreshSessionsAfterVgSave`, the -G switch and `saveSession`'s reload;
    `voicegroupChoices(session)` feeds the dock and Song Settings (the draft
    arg stays choosable while the draft lives); `pendingSynthsReferencedBy`
    is shared by `saveSession` and the commit (item 7: the commit writes the
    draft voicegroup's pending synth definitions first and graduates them
    after the file lands; `saveSession`'s own block skips the draft
    voicegroup). `commitDraftVoicegroup` runs before the `.mid`: only if
    the cfg names the draft voicegroup, `vgSource->save()` → mark written,
    remove the folder (loads now resolve to the project copy), set
    `vgFileTime`, invalidate the catalog → `appendIncludeLine`; a retry
    only re-runs the (idempotent) include. `renameDraftVoicegroup` on a
    Rename that changes the label: re-renders under the new name, carries
    the edited voices into a new draft source, rewrites the folder file,
    retargets the `VoiceEditCommand`s' load name, renames the -G arg (doc +
    applied). `sweepStaleDraftFolders` in `openProjectDir` after the
    teardown. `finishCreateSongWriteThrough` and its branch are gone.
  - Harness (`src/draftcheck.cpp`): 13 — draft with a new voicegroup:
    tree unchanged, locked folder holding exactly `renderNewVoicegroup`'s
    bytes, source = unwritten target with no mtime, dock choices offer the
    arg (catalog doesn't); a structural + a scalar voice edit are heard,
    the folder copy follows, a reload after `cleanupVgPreview` (the folder
    survives it) reads the edits from the folder, nothing outside
    `.porydaw/`; Discard → folder gone. 14 — Save → `.inc` equals the
    edited source (header, `voice_square_2`), one include line, folder
    gone, `vgFileTime` = the file's mtime, catalog lists it, the song's
    `-G` names it, `VoicegroupSource::open` finds it, the voice edit still
    undoes/redoes. 15 — cfg switched away, back (edits replayed, heard),
    away again → Save writes no `.inc`/include, folder gone, `-G` = the
    existing voicegroup. 16 — `voicegroup_<name>` planted in another file
    (`draftcheck_multi.inc`; `<name>.inc` absent): the shared check rejects
    it; Save → Rename (OK disabled on the name, enabled on a free one) →
    committed under the new name with its edits, header and `-G` renamed,
    old name nowhere, a pre-rename voice edit still undoes. 17 — a project
    reopen sweeps an unlocked `drafts/` folder and keeps one whose lock the
    harness holds.
  - Results: `--draftcheck` PASS (normal build, standalone); full
    `tools/run_checks.sh` PASS on the normal and ASAN builds, which runs
    `--draftcheck`, `--vgcheck`, `--vgsavecheck` and `--onboardcheck` on
    fresh scratches (mkcheck skipped, no fork given). Mutation-tested:
    dropping `isDeclared` from the check fails 16; dropping the load-name
    retarget fails 16's undo; dropping the folder sync in `onVoiceEdited`
    fails 13; dropping `saveSession`'s draft-voicegroup guard fails 14
    (early write → Rename); dropping the sweep fails 17; dropping the -G
    rename fails 16.
  - Deviations: (1) Two render functions (`renderNewVoicegroup` and
    `…FromLines`), mirroring the two writers. (2) `populateSession` calls
    `loadVoicegroupFor(…, draft.get())` directly rather than
    `loadVoicegroupForSession` (no session/draft installed yet); the
    bundle-tab load is unchanged. (3) The draft folder is made with plain
    `mkpath`, not `Sidecar::ensureDir`, which would append `.porydaw/` to
    the project's `.gitignore` — a project write before the commit (same
    as `vgpreview`). (4) The voicegroup's name conflict (and so its rename)
    counts only while the commit would write it — pending and still named
    by the cfg; a draft switched away keeps its old (reserved) name. (5)
    The undo history is rewritten on a rename (voice edits' load name,
    settings edits' -G) so pre-rename edits still undo after the commit.
    (6) Item E is a live disk read (`isDeclared`), not the cached catalog,
    so a symbol created after the scan is seen too; it runs per keystroke
    in the Rename dialog only while that dialog renames a voicegroup.
    (7) The lock file lives inside the folder (`.lock`; the loader skips
    dot files). (8) The commit status message still lacks the voicegroup
    hint (step 4 item 4, unchanged scope).
  - Owed: step 4's voicegroup hint; step 5's new checklist item (dock New
    Voicegroup / scripting `createVoicegroup` vs. a reserved draft name).
    No harness case for a crash-stale lock (a dead PID): faking another
    process's lock file is fragile across Qt's lock-info format; the
    unlocked-folder case covers the removal path.
  - **Review fixes (2026-09-26).** A code review of step 3 found:
    (A) Undo after committing a draft switched away from its new voicegroup
    restored a `-G` that was never written (the `SongCfgCommand`s still
    named it): the song failed to load and the next save put an undefined
    symbol into `midi.cfg`. Fix: `commitDraftVoicegroup`'s abandon path now
    calls `MainWindow::abandonDraftVoicegroup`, which rewrites the abandoned
    arg in the cfgs and the undo history to the committed one
    (`SongDocument::renameDraftVoicegroupArg`, so those settings edits
    become `-G` no-ops) and drops the voicegroup from the draft
    (`newVoicegroup` cleared, folder removed) — so it is no longer in
    `voicegroupChoices`, reserved, or searched by the loader, even when the
    rest of the commit then fails. Its voice edits stay in the history as
    inert entries. (B) The Rename dialog re-read every voicegroup file per
    keystroke: `SongNameFields` now computes
    `VoicegroupSource::declaredSymbols(root)` once, lazily on the first
    check that includes a voicegroup, and passes it to
    `checkNewSongNames(…, declaredVoicegroups)`; one-shot callers (the
    commit, the wizard's Sound page) keep `isDeclared`. (C) Case-only
    rename: no code change. The voicegroup name is always the song label,
    which only ever comes from the name fields' `[a-z_][a-z0-9_]*`
    lowercasing validator (or the import suggestion, lowercased), so two
    names can never differ only by case; a comment at the
    `QFile::remove(oldFile)` says so. (D) `maybeRefreshVoicegroup` (and
    `refreshSessionsAfterVgSave`, same misfire when a same-named file is
    saved) skip a session while `editsDraftVoicegroup()`. (E) The
    `syncDraftVoicegroupFile` after `replayVoiceEdits` on a `-G` switch
    is kept, with a comment: it is normally a no-op (every edit applied to
    the draft voicegroup already synced the folder, and the undo order
    means replay can't apply one the folder lacks), but `replayVoiceEdits`
    itself changes the source without syncing, and the call also retries an
    earlier sync whose write failed — not provably redundant.
  - Harness (`src/draftcheck.cpp`): section 15 then undoes every command
    of the committed draft — the `-G` stays the existing voicegroup (and
    applied) throughout — saves, and asserts `midi.cfg` has no
    `-G_<abandoned>` and the reloaded song names the existing voicegroup;
    the abandoned arg is not a choice. Section 18: the midi directory made
    read-only (probed; skipped with a note if the chmod doesn't bite) → Save
    fails after the `.inc` and its include line land, no `.mid`; the
    session edits the project copy → a retry with no changes commits under
    the same names with no dialog, the `.inc` byte- and mtime-identical,
    one include line. 18b: the same failure on a draft switched away from
    its voicegroup → still a draft, but the voicegroup is abandoned (no
    folder, not offered, not reserved), undo keeps the existing `-G`, and
    the retry commits with it.
  - Mutation-tested: skipping `abandonDraftVoicegroup` fails section 15's
    undo/save assertions and 18b.
  - Results: `--draftcheck` PASS on a fresh scratch (sections 11–12 and
    18 ran, not skipped); full `tools/run_checks.sh` PASS on the normal
    and ASAN builds (draftcheck, vgcheck, vgsavecheck, onboardcheck and
    scriptcheck included; mkcheck skipped, no fork given).
  - Deviation: section 18 fails the `.mid` write with a read-only midi
    directory rather than the read-only `midi.cfg` of sections 11–12, since
    the latter lets the `.mid` land (the review's scenario asserts it
    didn't). Deferred into step 5's checklist: F (orphaned `.inc` after a
    partial commit + Discard), G (`.s` files and single-colon labels
    unseen by the symbol check), H (scripts/other writers vs. the draft
    voicegroup).
- **2026-09-26 — Step 4 (draft UX).** Commit: see `git log` on branch
  `draft-song` ("Draft songs step 4 …").
  - Banner: `MainWindow::updateDraftBanner(session)` builds a `QFrame`
    `draftBanner` (label `draftBannerText`: "*<label>* isn't in your project
    yet. Save adds it; closing the tab discards it.", button
    `draftSaveButton` "Save to project", no focus, queued click →
    `saveSession`) through `SongView::setTopBanner`, like the bundle banner,
    and deletes it once the session is no longer a draft. Held in
    `SongSession::draftBanner`; called from `populateSession` (so a draft
    replaced in place loses it) and `refreshSessionIdentity` (the text
    follows a rename; the commit removes it). The theme's
    `bundleBannerStyleSheet` colors `#draftBanner`/`#draftBannerText` too.
  - Titles: `draftTabTitle(label)` = "[draft] <label>" (mirrors
    `bundleTabTitle`); the tab adds `*` (a draft is always dirty), the
    window title is "[draft] <label>[*] — <project> — porydaw", modified.
    `sessionTabToolTip(session)`: the `.mid` path, or for a draft "Draft: not
    in the project yet. Saving writes sound/songs/midi/<label>.mid and
    registers the song; closing the tab discards it." — used by
    `populateSession` and `refreshSessionIdentity`.
  - Close prompt for a draft: title "Unsaved Draft", text "Add <label> to
    the project?", informative "It isn't in the project yet. Discarding it
    closes the song for good.", buttons Add to Project (the standard Save
    button, relabeled, default) / Discard (destructive role) / Cancel. The
    non-draft "Unsaved Changes" prompt is unchanged.
  - Status messages: `commitDraft` appends " — configure its new
    voicegroup in the Voicegroup dock" when the commit (or an earlier
    attempt) wrote the draft's new voicegroup; `createSongFromWizard(wizard,
    bool imported)` says "Imported/Created <label> as a draft — Save to add
    it to the project." The step-1 placeholder in `openDraftSong` is gone.
  - Item 5: chose **(a)**. `saveSession` runs `resolveDraftNameConflicts`
    before its dirty-voicegroup block (moved out of `commitDraft`): the
    check reads only the song list, reserved names and the disk, none of
    which the existing-voicegroup save changes (it rewrites an existing
    file; voice edits don't touch symbols), and the Rename's voicegroup
    rename concerns only the draft's own voicegroup, which that block
    skips. So a Rename cancelled on a first save writes nothing, and the
    dialog's "The song hasn't been written yet." holds then — but not on a
    retry after a partial commit (the `.inc` + include line or the `.mid`
    already on disk); the review fixes below make the wording conditional.
  - Docs: `docsrc/manual/new-song.md` "Draft songs" section,
    `midi-import.md` "After the import" paragraph linking to it; CHANGELOG
    "Changed" entry under Unreleased.
  - Harness (`src/draftcheck.cpp`): `PromptAnswerer` also answers "Unsaved
    Draft" and gains `inspect(fn)`. Section 1: tab title, tooltip, window
    title (+ modified), banner by object name with the plan's text and an
    enabled focusless Save to project button. Section 3: the close prompt's
    title, text, button labels, default and destructive role. Section 4:
    the commit goes through the banner button; banner gone, tab
    title/tooltip/window title ordinary, the exact "Created and registered"
    message. Sections 5–6: banner gone and ordinary title after a replace in
    place (Save and Discard). 8: banner gone after rename + commit. 9: the
    cancelled draft keeps its [draft] title and banner. 14/15: the
    voicegroup hint present / absent. New 19: an existing voicegroup edited
    in a draft whose `.mid` is planted → Rename Cancel → tree fingerprint and
    the `.inc` unchanged, still dirty; Rename accept → the `.inc` is saved
    with the edit. New 20: `createSongFromWizard` for New Song and Import
    (blank / import wizards filled offscreen) opens a draft, writes nothing,
    and shows the status message. `PORYDAW_DRAFTCHECK_SHOTS=<dir>` (shipped,
    like `PORYDAW_BUNDLE_SHOT`) saves `draft-tab.png` and
    `draft-close-prompt.png`.
  - Screenshots (session scratchpad, not committed): `step4-draft-tab.png`
    (window with the draft tab, banner and `[draft] mus_draftcheck_a*`
    title) and `step4-close-prompt.png` (the Add to Project / Discard /
    Cancel prompt).
  - Mutation-tested: keeping the name check after the voicegroup block
    fails section 19; not deleting the banner fails 4, 5, 6 and 8.
  - Results: `--draftcheck` PASS on a fresh scratch (with the shots env
    var); full `tools/run_checks.sh` PASS on the normal and ASAN builds
    (draftcheck, tabcheck, onboardcheck, vgsavecheck, bundlecheck and
    scriptcheck included; mkcheck skipped, no fork given).
  - Owed: nothing for step 4. No manual pass in a real (non-offscreen)
    window yet — the screenshots are the visual check; step 6 covers the
    real-project pass.
  - Deviations: (1) The window title uses the same "[draft]" prefix as the
    tab, not a separate wording. (2) The prompt's title is "Unsaved Draft"
    (the plan named only its text and buttons), and it adds a one-line
    informative text. (3) `createSongFromWizard` takes `bool imported`
    instead of a dialog title, to pick the status wording. (4) The
    screenshot env var ships (existing pattern).
  - **Review fixes (2026-09-26).** A code review of step 4 found six small
    issues; all fixed in one commit on `draft-song` ("Draft songs step 4
    review fixes …").
    - A. The draft close prompt hid edits to an existing (shared)
      voicegroup. `maybeSaveSession`'s draft branch now shows "It isn't in
      the project yet, but it has edits to voicegroup <name>, which other
      songs may use. Add to Project saves those too; Discard drops them."
      when `vgSource` is dirty and is not the draft's own new voicegroup
      (`editsDraftVoicegroup`); the plain text otherwise. `<name>` is
      `loadName()` without a `voicegroup_` prefix. Harness: section 3 asserts
      the plain text, 13 (own new voicegroup edited) the plain text, 19 the
      vg-dirty text (Cancel keeps the tab, writes nothing).
    - B. `populateSession` now calls `updateDraftBanner` before the view
      sidecar's `applyViewState`. Harness: sections 5 and 6 plant a sidecar
      for the target (roll pane at its minimum, scrolled to the bottom)
      through a fresh load in its own tab, then assert the replaced-in-place
      view has the same splitter sizes and vertical scroll. The window is
      shown (1280×800) around those checks only — a hidden window never lays
      its views out (every splitter size reads 0). Finding: the old order
      does NOT fail this check — QSplitter restores its requested sizes when
      the view grows and the scroll clamp gives the same value — so the
      reorder is a correctness tidy-up with no observable effect found
      offscreen, and the assertion is a regression guard, not a mutation
      test.
    - C. `SongRenameDialog` takes `alreadyWritten` (display paths):
      `resolveDraftNameConflicts` passes the voicegroup `.inc` ("(and its
      include line)") when `voicegroupWritten`, and `wroteMidPath`; the
      dialog then says "An earlier save that failed partway already wrote:"
      with the list instead of "The song hasn't been written yet."
      Harness: section 9 asserts the unwritten wording, 12 (a `.mid` from a
      partial commit) the list with that `.mid`. The manual no longer says
      Discard leaves the project "exactly as it was" unconditionally
      (partial saves, Import Sample is write-through, shared voicegroup
      edits), and the step 4 entry above no longer calls the old wording
      "now true".
    - D. `NoPromptGuard` (RAII around a Cancel `PromptAnswerer`) fails on
      destruction when a close prompt appeared, naming its line; every
      former `PromptAnswerer noPrompt(Cancel, …)` uses it (11 sites, plus two
      in the section-5/6 view reference).
    - E. `MainWindow::m_saveInProgress` (a `QScopedValueRollback` in
      `saveSession`): a save started while another runs — a queued banner
      click, Save Song/Ctrl+S, a script — returns false at once. Harness 9b:
      two banner clicks plus `m_saveAction->trigger()` and a direct
      `saveSession` from inside the Rename dialog; one dialog only, the
      nested save refused. (The nested calls run from a `singleShot`, not the
      answerer's timer slot: Qt doesn't re-enter a timer, so a dialog opened
      from that slot is never answered and the harness hangs.)
      Mutation-tested: without the guard 9b fails with three unexpected
      "Rename Song" modals.
    - F. With `PORYDAW_DRAFTCHECK_SHOTS`, the window is hidden again after
      the `draft-tab.png` grab. The run also saves
      `draft-close-prompt-vg.png` (section 19's prompt).
    - Mutation-tested A (always the plain text) and C (always "hasn't been
      written yet"): sections 19 and 12 fail.
    - Screenshot (session scratchpad, not committed):
      `step4-close-prompt-vg.png`.
    - Results: `--draftcheck` PASS on fresh scratches, with and without the
      shots env var; full `tools/run_checks.sh` on the normal and ASAN
      builds: PASS (draftcheck, tabcheck, onboardcheck, vgsavecheck,
      bundlecheck and scriptcheck included; mkcheck skipped, no fork given).
