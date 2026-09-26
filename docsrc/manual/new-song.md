# Creating a New Song

## Before you start

<!-- TODO: Decide: from scratch here, or import a MIDI you made elsewhere?
Link to [Importing MIDI Files](midi-import.md) for the latter. -->

## The New Song wizard

<!-- TODO: File → New Song; each field in order:
- Name (the MUS_ constant — naming rules)
- Voicegroup (reuse vs. new)
- Player / type (BGM vs. SE vs. fanfare — what that means in-game)
- Song settings (reverb, priority, etc. — link to Song Settings)
-->

## Draft songs

A new song starts out as a **draft**: it opens in its own tab, titled `[draft] <song>`, with a banner across the top that says the song isn't in your project yet. You can play and edit a draft like any other song, but nothing is written to your decomp project until you save it.

- **Save** (`Ctrl+S`, or the banner's **Save to project** button) adds the song to your project: Porydaw writes its `.mid` and `midi.cfg` line, registers it, and creates its new voicegroup if you asked for one. The tab stays open and becomes a normal song tab, and your undo history is kept.
- **Closing the tab** asks whether to add the song to the project. Choose **Discard** to throw the draft away; nothing of the draft is written. Two things can already be in your project, though: if a save failed partway (for example, a file couldn't be written), it may have written some of the song's files before it stopped, and samples you imported into the voicegroup with **Import Sample** are written right away. If the draft uses an existing voicegroup and you edited it, the prompt says so: other songs may use that voicegroup, so adding the song saves those edits too, and discarding drops them.

If another song has taken the draft's name by the time you save (for example, after a `git pull`), Porydaw asks you to rename the draft before it writes the song. (If an earlier save failed partway, the rename dialog lists the files it already wrote.)

Drafts only live in memory. They aren't reopened when Porydaw restarts, and they don't appear in the song list until they are saved.

## What "registering" means

<!-- TODO: Plain-language version of the magic: the decomp project needs a
song listed in several places before the game can play it; Porydaw writes
all of those lines for you when it creates the song. Name the files briefly
and link to [Files Porydaw Reads & Writes](project-files.md) for the
full list. Also: Register Song for a .mid that exists but isn't wired up,
and Delete Song as the reverse. -->

## Playing your song in-game

<!-- TODO: The song constant is usable anywhere the game takes one —
map music in porymap, scripts, etc.; quick porymap cross-link. -->

## Suggested workflow for a first song

<!-- TODO: Opinionated mini-guide: start from a copied vanilla voicegroup,
lay down a drum track, loop a 4-bar section, build it up; keep the
polyphony meter visible while you go. -->
