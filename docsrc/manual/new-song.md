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

A new song starts out as a **draft**: it opens in its own tab, titled `[draft] <song>`, with a banner across the top that says the song isn't saved to your project yet. You can play and edit a draft like any other song, but nothing is written to your decomp project until you save it.

- **Save** (`Ctrl+S`, or the banner's **Save to project** button) adds the song to your project: Porydaw writes its `.mid` and `midi.cfg` line, registers it, and creates its new voicegroup if you asked for one. The tab stays open and becomes a normal song tab, and your undo history is kept.
- **Closing the tab** asks whether you're sure you want to discard the draft. Choose **Discard** to throw it away; nothing of the draft is written. The prompt has no Save button: to keep the draft, choose **Cancel** and save it (`Ctrl+S` or **Save to project**). If an earlier save failed partway (for example, a file couldn't be written), it may have written the song's `.mid` or its new voicegroup before it stopped: the prompt lists those files, and **Discard** removes them again. A file your project has started using since stays, and the prompt says so: a `.mid` you registered in the meantime, or a voicegroup another song now uses. While the draft is open, its song can't be registered or deleted from the song browser; clicking its entry there just switches to the draft's tab. Some actions write to your project right away, even while a draft is open, and what they write stays there if you discard the draft: sample files from **Tools → Import Sample** and the Voicegroup dock's **New sample…** and **Edit sample…** buttons, and a voicegroup made with the dock's **New Voicegroup** (under any name but the draft's own new voicegroup, which it refuses). The voice that plays a new sample is part of the draft like any other voicegroup edit, so it only reaches the project when you save. The same prompt appears when you open another project or quit Porydaw with a draft open; **Cancel** stops the switch or the quit so you can save the draft first. If the draft uses an existing voicegroup and you edited it, the prompt says so: other songs may use that voicegroup, and discarding drops those edits too (saving the draft saves them).

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
