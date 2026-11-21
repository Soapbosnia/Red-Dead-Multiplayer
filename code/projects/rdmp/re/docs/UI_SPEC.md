# UI specification — native frontend changes

Reference frames: [`images/options-menu.png`](images/options-menu.png),
[`images/title-logo.png`](images/title-logo.png).

## 1. Title text → "Red Dead Multiplayer"

The title is **not text** — it is the red brush-stroke logo image (`images/title-logo.png`).
There is no string to rename.

Two native routes:

- **Texture swap**: replace the logo texture inside the Flash/UI archive (`flash.rpf`,
  198 MB, RPF6). Cleanest visually; requires RPF6 extraction and repack, plus authoring
  the new logo in the same format.
- **Runtime overlay**: draw "RED DEAD MULTIPLAYER" over the logo from the client DLL
  (D3D12/CEF/ImGui). No archive edits, but it is an overlay, not native.

Decide once the menu movie is mapped.

## 2. Play → multiplayer menu

Current behaviour: `Play [Enter]` on the title menu opens the mode picker
(Normal / Hardcore / Undead Nightmare).

Wanted: `Play` opens our own multiplayer menu instead.

Native route: intercept the menu action that Play raises and redirect it to our state.
The mode picker itself is untouched. The menu is Scaleform Flash; labels and handlers
are inside the SWF, so the interception point is the frontend action handler, not a string.

## 3. Settings → new "Multiplayer" entry

The Options screen is a vertical list: GAMEPLAY, GAMEPAD, KEYBOARD, AUDIO, DISPLAY,
GRAPHICS (see `images/options-menu.png`). We add a seventh entry, MULTIPLAYER, with its
own page for nickname and connection settings.

Native route: inject the item into the Options menu's list model at construction (or
insert it into the Scaleform list) and give it our own page. Adding to the list is the
same class of work as (2), which is why both wait on mapping the menu model.

## Technical anchors found so far

- Menu system: Scaleform (GFx). Exe strings: `MovieClip`, `attachMovie`, `fs_movieName`,
  `fuiMovieData`, `FSCommand:IsMovieLoaded`.
- Name-hash routine at RVA `0xC7510` (`hash = hash * 0x401; hash ^= hash >> 6`, quoted and
  unquoted paths) — used when registering/looking up AS2 member names such as
  `fs_movieName` and `attachMovie`. This is the most promising probe point for logging
  which movie variables and members the frontend touches.
- UI archives: `game/flash.rpf` (RPF6, ~198 MB, encrypted/compressed TOC),
  `game/strings_d11generic.rpf` (localisation, 95 MB).
- Title/menu flow: title (Bink intro) → `Settings [Y] / Play [Enter] / Quit [Esc]`
  → Play opens the mode picker.

## Build order

1. Probe DLL: hook Bink opens + the GFx name hash, log to file. One game run gives the
   menu movie name, its variables and the action names behind Play/Settings.
2. Play redirect (feature 2) — smallest visible change, proves the hook.
3. Options entry (feature 3) — needs the list model from step 1.
4. Title (feature 1) — texture swap if RPF6 extraction works, overlay otherwise.
