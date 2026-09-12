# RE0CabbyCodes

A cheat panel for **Resident Evil 0 HD Remaster** (Steam, PC). Pause the game (Alt, or Start on a
pad) while playing and a small panel appears beside the pause menu - and on the title screen it comes
up beside the *Load Game* list to manage your save files:

- **God mode** - both characters are held at full health and cured of poison every frame.
- **One hit kills** - every live enemy is held at 1 HP, so your next hit kills it (death animations,
  drops and scripted deaths still play out through the game's own code). The leech-men are the one
  enemy that does not read its health for a body shot: they keep a second pool for those, and that
  is held at 1 as well, so a single shot anywhere collapses them into their leeches.
- **Infinite ammo** - every weapon in both inventories keeps its loaded count; each shot is put
  straight back. Pickups and reloads still raise it.
- **Infinite ink ribbons** - saving at a typewriter never uses up a ribbon, not even your last one,
  and the typewriter lets you save without carrying one at all (the game's own "take one ribbon"
  step and its "do you have a ribbon?" check are both skipped).
- **Save without counting** - the save counter the ranking looks at is not incremented when you save.
- **Save count** and **play time** editors - change the counter and the clock (hours, minutes,
  seconds) the file screen and the final rank use.
- **Freeze play time** - the game clock stops advancing.
- **Freeze countdown timer** and a countdown editor - the timed sections (the train's brakes at
  3:30, and every other countdown the game puts on screen) stop counting down, or can be set to
  whatever is left. The freeze belongs to the section that is running: it lets go when that section
  ends, and the next one is frozen again at its own starting value. The editor is only available
  while a section is actually running.
- **Inventory editor** - every slot of both Rebecca's and Billy's inventory plus the personal item:
  pick any item and set its count. Open a slot's list and type to narrow it (by name or item number),
  Enter takes the first match. Two-slot weapons are placed correctly (even slot, filler in the next
  one). Counts are held to what the game itself fits in one slot - that is the game's own number, read
  out of the running game, so a stack is what it would be in play (ten leech charms, one herb, 255
  handgun rounds, a weapon's own magazine). The game's own infinite weapons (the unlocked Rocket
  Launcher a new game gives Rebecca) show as *infinite* and stay infinite through the item storage.
- **Item storage** - the item box RE0 does not have. *Store* beside any inventory slot puts that item
  in the box and frees the slot; *-> Rebecca* / *-> Billy* puts one back into a free slot
  (two-slot weapons come back with their filler, or are refused when there is no room for both).
  The items the game itself stacks - ammo, ink ribbons, Molotov cocktails, empty bottles and gas
  tanks, the leech charms - are merged both ways, in stacks as big as one inventory slot of them
  (255 handgun rounds, 300 machine gun rounds, ten leech charms): storing a few rounds tops up the
  stack already in the box, and taking a stack back tops up the one that character already carries
  before a free slot is used. Only what goes past a full stack starts a new one, and whatever fits
  nowhere stays in the box. Weapons (their count is what is loaded), herbs, sprays and key items
  keep an entry and a slot each.
  The box keeps five **banks** - key items, weapons, ammo, heals (herbs & first aid sprays), and
  other - and its tabs pick the one on show, each tab with a count of what that bank holds. Whatever
  you store is filed under its bank on its own, and every bank stays sorted by name, the fullest stack
  of an item first. The box has its own filter box - type a name or an item number to narrow the bank
  on show, the same way the inventory's item lists work.
  **The box follows your save files**: when you save, whatever is in it is written under that file's
  line in `RE0CabbyCodes.storage.ini` beside the mod, and loading that file puts those items back.
  The storage never touches your actual save file, so removing the mod just leaves the box behind.
  A save file has one box: starting a new game gives you an empty one - *Once Again* from a cleared
  file included, whatever that file's line held - and saving a cleared game at the main game's ending
  leaves that file with none. Leech Hunter always starts with an empty box that lasts only as long as
  the run, never the box of the file you started it from; saving from its result screen neither keeps
  the run's box nor touches the file's.
- **Save file manager** - on the title screen's *Load Game* list the panel shows all twenty save
  files (play time, save count, *cleared* and *Wesker mode* marks, and how many items each keeps in the
  item storage), with the one the game's cursor is on highlighted. **Delete** empties a file: the game
  shows it as NO DATA from then on, exactly like a file that was never used, and the next save or copy
  into it fills it again. **Copy to...** puts a copy of a save into any other file, empty or not. Both
  ask first, and both write the game's save file straight away through the game's own save - the list
  on screen updates as soon as it is written. The item storage follows the file: a deleted file's box
  is emptied, and a copy takes the box of the file it came from - always matching what actually
  reached the save file, even if the game is closed while it writes.

Tick or edit what you want and unpause. The switches keep their state for the session (start-up
defaults live in the ini). **F7** hides or shows the panel while the game is paused.

Works on Windows and on Linux/Proton with no launch options. It leaves `dinput8.dll` alone, so it
coexists with Ultimate-ASI-Loader mods such as FusionFix and re0box.

## Install

1. Open your Resident Evil 0 folder (the one containing `re0hd.exe`).
2. Rename the existing `steam_api.dll` to `steam_api_orig.dll`.
3. Copy the mod's `steam_api.dll` in beside it.

Steam's *Verify integrity of game files* puts the stock DLL back; just repeat step 3 if that happens.
To uninstall, delete the mod's `steam_api.dll` and rename `steam_api_orig.dll` back.

## Please read before using

- The cheats change what the game ranks you on (clear time, saves) and what its achievements check.
  A save made while cheating records the game state as it was - back up
  `Steam/userdata/<id>/339340/remote/data0.bin` before editing counters or inventories.
- **Deleting or copying a save file cannot be undone**: it rewrites `data0.bin` the moment you confirm.
  Back that file up first if in doubt. If the game cannot write it (a save file that belongs to another
  Steam account, say) nothing is changed, and the panel says so.
- God mode holds health; a single lethal scripted hit can still land in the frame it happens.
- One hit kills changes boss fights: a boss whose phases are scripted around damage thresholds may
  skip straight to its death.
- Freezing a countdown holds the timer where it is; the section still ends the way the game intends
  when you finish it, and the timer disappears with it. Leaving the switch on simply freezes the
  next timed section too, at whatever it starts from.
- The inventory editor writes the bag directly. It refuses edits that would break the two-slot rule
  and tells you why in the panel.
- The item storage keeps items outside the game's save file, so the two only agree at the moment you
  save. Items you put in the box after saving are gone if you quit without saving again - exactly
  like anything else you did since the last save. Starting a new game empties the box; loading a file
  fills it with what that file had; saving a cleared game leaves that file with an empty one. The
  records on disk only change when you save.

## Config

`RE0CabbyCodes.ini` is created next to the DLL on first run:

| Key | Default | Meaning |
| --- | --- | --- |
| `ToggleKey` | `0x76` (F7) | virtual-key code that hides/shows the panel while paused |
| `GodMode`, `OneHitKills`, `InfiniteAmmo`, `InfiniteInkRibbons`, `NoSaveCount`, `FreezePlaytime`, `FreezeCountdown` | `0` | cheats switched on when the game starts |
| `AlwaysShow` | `0` | debug: draw the panel outside the pause menu too |
| `Trace` | `0` | verbose diagnostics in `RE0CabbyCodes.log` |
| `DumpImage` | `0` | write the decrypted game image beside the DLL once (for reverse engineering) |
| `DumpObject` | | debug: `sGamePause:0x100,...` logs dwords of those engine objects as they change |
| `DtiSlot` | `-1` | override the derived MT Framework `getDTI` vtable slot |
| `Disable` | | comma list of subsystems to turn off: `overlay,dispatch,game,gpa,input` |

`RE0CabbyCodes.log` beside the DLL records what the mod found and did; attach it when reporting a
problem. `RE0CabbyCodes.storage.ini` beside it holds the item storage, one line per save file - in
`file3 = 6:7,33:15` save file 3 has a shotgun loaded with 7 shells and a box of 15 shotgun shells
stored. A `pending = copy 2 7` or `pending = delete 3` line is a save file the load list was still
deleting or copying when the game closed; the mod settles it from the save files themselves the next
time they are loaded, so the boxes end up with whatever reached the save file. The banks and their
order come from the items themselves, so the order of a line does not matter, and the items the game
stacks are kept merged - a line an older version wrote with an entry
for every store is merged the next time its file is loaded. It is plain text, and deleting it only
empties the storage.

## How it works

The mod ships as a proxy `steam_api.dll` (every export of the game's Steam API DLL is forwarded
untouched through generated jump thunks), so it loads before the game starts and needs no injector.
The SteamStub-wrapped executable is never modified on disk; the mod waits for the wrapper to unpack
the game in memory, then finds what it needs by name and shape: MT Framework's own class registry
(the `MtDTI` objects) for the engine singletons and unit classes, the event-script command table
for anchor functions, and short byte signatures for the five code sites it patches in memory
(the play-time store, the save-counter increment, the ink-ribbon consumption and the countdown
timer's two per-frame steppers). Nothing is a fixed address. The save file manager finds the title
screen's file list through the game's own area and screen objects, empties or copies a file in the
game's own memory, and has the game write `data0.bin` with its own system-data save - the same save
it makes after every typewriter save. The item storage follows your saves by
watching the engine's own save/load state machine: the mod sees which file the game just wrote or
read, and the storage never touches the save data itself. When a game starts comes from the same dword the game
hands Steam's rich presence, the one thing in the image that says whether you are in the main game,
Wesker mode or Leech Hunter, and a cleared-game save is recognised by the save point the ending
writes. The panel is Dear ImGui drawn from a hook on the
Direct3D 9 device's `EndScene`. See `CLAUDE.md` for the reverse-engineering record.

## Building from source

Linux with mingw-w64 (`i686-w64-mingw32-g++`); no Windows or MSVC needed.

```sh
cp config.mk.example config.mk   # set GAME_DIR
make                             # build/steam_api.dll
make install                     # deploy into GAME_DIR (renames the stock DLL once)
make version 1.1.0               # set the version (VERSION file, baked into the DLL)
make package                     # dist/RE0CabbyCodes_v<version>.zip
make proxy                       # regenerate the export list from the stock DLL
```

Dear ImGui (MIT) is vendored under `contrib/imgui`. Licensed under the GPL-3.0; see `LICENSE`.
