# RE0CabbyCodes — project guide

A client-side mod for **Resident Evil 0 HD Remaster** (Steam appid 339340, 32-bit `re0hd.exe`,
Capcom MT Framework 2.x, SteamStub v3.1.2 wrapped), cross-built on Linux with mingw-w64. It draws a
Dear ImGui panel over the in-game pause menu with God mode, One hit kills, Infinite ammo, Infinite ink
ribbons, Save without counting, save-count / play-time / countdown-timer editors, an inventory editor
for both characters and an item storage box, listed in five sorted banks, that merges what the game
stacks and follows the game's own save files - and, on the title screen's load list, a save-file
manager that deletes a file or copies one over another. `README.md` is the user-facing doc; this file records **what was reverse-engineered**. It
is a sibling of `../Fear3CabbyCodes` (same architecture: proxy DLL, IAT + vtable hooks in
`src/mem.h`, main-thread tick in `src/dispatch.cpp`, ImGui from `EndScene`).

## Build & deploy

```sh
make            # -> build/steam_api.dll   (config.mk sets GAME_DIR; gitignored)
make install    # rename stock steam_api.dll -> steam_api_orig.dll (once), deploy ours atomically
make uninstall  # restore the stock DLL
make version X.Y.Z   # set the version;  make package -> dist/RE0CabbyCodes_vX.Y.Z.zip
make proxy      # regenerate steam_api.def + src/proxy_exports.inc from the stock DLL's export table
# reverse engineering (DumpImage=1 in the ini writes re0hd.dumped.exe beside the DLL):
python3 tools/fix_dump.py --exe "$GAME_DIR/re0hd.exe" --dump "$GAME_DIR/re0hd.dumped.exe" --out re0hd.fixed.exe
python3 tools/find_dti.py --image "$GAME_DIR/re0hd.dumped.exe" --list | --class sPlayer | --script | --strref mPause
MAXMEM=12G /opt/ghidra/support/analyzeHeadless ghidra_proj RE0 -import re0hd.fixed.exe -overwrite -loader PeLoader
MAXMEM=8G /opt/ghidra/support/analyzeHeadless ghidra_proj RE0 -process re0hd.fixed.exe -noanalysis \
    -scriptPath tools/ghidra -postScript DecompileAddrs.java 0x573290 0x529310   # decompile a reading list
i686-w64-mingw32-objdump -d -M intel --start-address=0x574b90 --stop-address=0x574c80 re0hd.fixed.exe
```

The log (`RE0CabbyCodes.log`), ini (`RE0CabbyCodes.ini`), the storage records
(`RE0CabbyCodes.storage.ini`) and the ImGui layout file sit beside the DLL in the game folder.
`Trace = 1` adds diagnostics (the flags dword, status values, the countdown field, enemy units, every
name the DRM stub resolves through `GetProcAddress`); `AlwaysShow = 1` draws the panel outside the
pause menu; `DumpObject = sGamePause:0x100` logs an object's changing dwords; `Disable =
overlay,dispatch,game,gpa,input` bisects a fault.

## ⛔ Rules

- **Never patch `re0hd.exe` on disk.** Its `.text` is SteamStub-encrypted on disk anyway; the stub
  decrypts it in memory after our `DllMain` has run. `DllMain` only patches import-table slots and
  starts a thread; nothing reads game memory before the first main-thread `PeekMessageA` tick and
  the prologue/padding check in `mem::text_looks_decrypted`.
- **Never use absolute addresses.** Everything is found at runtime by name (MT Framework DTI for
  classes and singletons, the event-script command table for anchor functions) or by byte signature
  verified at the match; the VAs/RVAs in this file document build 17178773 (`MasterRelease Jan 28
  2025 16:45:59`) only.
- **Game memory only from the main thread** (`dispatch.cpp`'s `PeekMessageA` hook). The panel
  (render thread) flips atomics and posts requests; `cheats::tick` applies them. The readings go back
  the same way: `cheats::Status` is ~250 bytes and a struct assignment is not atomic, so `tick`
  hands a finished one to `cheats::g_published` under a lock and `status()` copies it out under the
  same one. Reading the working copy directly paired an item with another tick's count and blanked a
  bag whose reading had just been rejected - transient, self-healing, and invisible in a log.
- **Never call a game function.** Every feature is a field write, a hold, or a byte patch of a
  site whose bytes were verified first (`mem::Patch::prepare`); patches are reverted on toggle-off
  and unload.
- Back up `Steam/userdata/<id>/339340/remote/data0.bin` into `.save-backup-<date>/` (gitignored)
  before experiments that write counters or bags.
- The user commits every repo himself — do not `git commit`/`push` unless asked.

## Why steam_api.dll
Of the exe's imports only `steam_api.dll` ships with the game and is not a Wine builtin (`d3d9`,
`dinput8`, `xinput1_3`, `winmm` would need `WINEDLLOVERRIDES` under Proton); `dinput8.dll` also
stays free for the community's Ultimate ASI Loader mods. The exe imports 16 of its 59 functions;
`tools/gen_proxy.py --from-dll` emits thunks for all 59 plus a PE forwarder for the data export
`g_pSteamClientGameServer`. SteamStub v3.1.2 (header flags 0 = every check on) accepted the proxy
without complaint: its `GetProcAddress` trace shows only CRT/kernel32 names, no signature checks.

## What the game does

### Executable
- PE32, ImageBase 0x400000, no ASLR. `.text` 0x1000 (0x8AF88B), `.rdata` 0x8B1000, `.data`
  0x96A000 (VirtualSize 0xCB778 includes .bss), `.bind` 0xA38000 (the stub). Stub entry 0xA38310,
  original entry point **0x848248** (header +0x20; app id at +0x38, flags at +0x3C).
- D3D9 only (`Direct3DCreate9` imported statically → IAT hook, then `IDirect3D9::CreateDevice`
  slot 16, `EndScene` 42 / `Reset` 16 on the device; fine under DXVK). Message pump on the primary
  thread; the render thread is separate.

### Input (src/input.cpp)
The game does **not** read the mouse or the keyboard from the window message queue, so swallowing
`WM_LBUTTONDOWN` in the window procedure does nothing - the click still reaches the pause menu. It
imports `DINPUT8.dll!DirectInput8Create` and opens both DirectInput devices (`GUID_SysMouse` is
referenced at exe+0x408042, `GUID_SysKeyboard` at exe+0x40767B), and it also imports
`GetAsyncKeyState`, `GetCursorPos`, `ScreenToClient`, `ClipCursor` and `ShowCursor` from USER32.
The guard therefore hooks both paths: the exe's `DirectInput8Create` import ->
`IDirectInput8::CreateDevice` (vtable slot 3, one hook, devices told apart by the GUID they were
created with and by `this`) -> `IDirectInputDevice8::GetDeviceState` (slot 9, zeroed) and
`GetDeviceData` (slot 10, count set to 0); plus the `GetAsyncKeyState` import. Blocking is gated on
the panel actually using that input (`overlay::capturing_mouse/keyboard`, i.e. the panel is drawn
*and* ImGui reports `WantCaptureMouse` / `WantTextInput`), so the pause menu keeps its mouse
everywhere except over the panel, and Escape and Alt are never taken.

### MT Framework DTI (src/dti.cpp)
`MtDTI` records live in `.data`, built at static init. Layout (32-bit): `+0` a **per-class DTI
vtable** (entry 1 = `newInstance`), `+4` name, `+8` next, `+C` child, `+0x10` parent, `+0x14` link,
`+0x18` size in dwords | attributes, `+0x1C` id = `~crc32(name) & 0x7FFFFFFF`, or an explicit id
with the top bit set (`uPlayerRebecca` 0x80000002, `uPlayerBilly` 0x80000005). Every `MtObject`
vtable's slot **4** is `getDTI()` = `B8 <dti> C3`; the class vtable base is that slot − 16
(constructor stores prove it). Singletons: `newInstance` (or the constructor it calls) stores the
object into a static slot, `mov [slot], reg` — `dti::find_singleton` parses that, falls back to a
`.data` scan for a pointer to an object with the vtable, then a heap scan (own stack excluded).
A `newInstance` with nothing left to do reaches its constructor by a **tail `jmp`**, not a call, so
the follow-the-callee step takes `E9` as well as `E8`; that is what turns `sEnemy`, `sGameInfo`,
`sSaveManager`, `sSubMenu`, `sGameChara` and `sEventScript` from scan hits into slots read out of the
code.

| class | static slot | notes |
| --- | --- | --- |
| `sPlayer` | exe+0x9CBF3C | `+0x2C` active unit, `+0x3C` partner unit (the `0x20000`/`0x30000` script selectors) |
| `sItem` | exe+0x9CBF44 | `+0x20` Rebecca's bag, `+0x60` Billy's bag |
| `sGameInfo` | exe+0x9CBE9C | `+0x38` save count (int), `+0x3C` play time (float, 1/30 s units), `+0x50` bit 4 stops the clock, bit 6 shows the countdown |
| `sGamePause` | exe+0x9CBEA8 | `isPause` (exe+0x2014B0) reads bit 6 of the flags dword below |
| `sGameChara` | exe+0x9CC0D0 | `+0x2C`: 90 entries × 0x10 bytes, first dword = unit pointer (players and enemies) |
| `sEnemy` | exe+0x9CDC78 | enemy placement records (175 × 0x5C at exe+0x10FE10's table); not used |
| `sEventScript` | exe+0x9CBEBC | `+0x2C` countdown (float, 1/30 s units, −1 = none), `+0x64` its mode, `+0x30` the script's own wait timer (frames), `+0x20/+0x24` a second, unrelated counter |
| flags object | exe+0xA2D434 | `+0x33D8D0` system flags dword: bit 6 pause menu, 7+8 STATUS screen, 7+12 file/map, 10 door transition |
| `sMain` | exe+0xA2D904 | `+0x38` fps, `+0x68` time scale, `+0x60/+0x64` freeze conditions of the clock |
| `sGamePresence` | exe+0x9CC01C | `+0x30` the requested game mode, `+0x24` the published one (see below) |
| `sGameArea` (an `sArea`) | exe+0xA2D8C0 | `+0x3824` how many areas run, `+0x382C` up to 8 of them (`aTitle` on the title screen) |

### Characters (`uPlayerRebecca` / `uPlayerBilly` < `uPlayerBase` < `uCharaBase`; enemies `uEnemyXX` < `uEnemyBase` < `uCharaBase`)
- `+0x100` type (3 Rebecca, 5 Billy), **`+0x1030` HP** (max 150 / 250; `setHp` = exe+0x129310
  `mov [ecx+1030],eax`; `EnemyDeath` uses the same getter exe+0x122DF0 for enemies),
  `+0x68C4` status dword: bit 0 poison (cure = clear), 0x4, 0x80, 0x800 set by other code; the
  `PlayerMutekiSet` script command (handler exe+0x174B90) clears **0x4000** through exe+0x112050
  (`flags &= ~mask & 0x1FFFF` under the unit's critical section at `+0x67A0`) — its semantics were
  not resolved, so god mode holds HP instead. Enemy type id `+0xFE8` / `+0xFF0`.
- One hit kills: walk `sGameChara+0x2C`, keep enemy units with HP > 1 at 1. **An enemy is not
  always `is_a(uEnemyBase)`**: the 38 enemy classes are `uEnemy10`..`uEnemy6b` (the DTI id is
  `0x800000NN`, NN = the `em` number, and `arc\game\em\emNN` is the model), but `uEnemy3bRebecca`
  — an enemy that wears a *player* model, with no `arc\game\em\em3b` of its own and code that
  reaches into `pl_damage.inl` — is a child of **`uPlayerBase`**, so the parent chain alone walks
  past it. `game::is_enemy_unit` therefore takes `is_a(uEnemyBase)` **or** a class name beginning
  `uEnemy`; every other class seen in the unit table is logged once (`enemies: unit table also holds X`), and `Trace = 1` lists the whole table (`trace: unit NN ptr cls
  hp=..`), players and all.
- **The leech-man keeps two health pools, and a body shot never touches the first one.** `uEnemy43`
  — the mimicry that collapses into a heap of leeches — is the only class in the image whose damage
  can bypass HP. Its init (vtable slot **32**, exe+0xB05B0) sets the kind index `+0x67A0 = 0x15`,
  takes its HP from the per-kind table (exe+0xCA860 → 16 `ushort`s indexed by the rank; 360 here)
  and copies that same number into a second pool at **`+0x6AB4`** (`call getHp; mov [edi+0x6AB4],eax`
  at exe+0xB0869 — the only store of its kind in the image apart from `uEnemy3a`'s `+0x6CC8`). Its
  damage handler (exe+0xAF1F0) routes every hit: with **bit 3 of the unit's flags at `+0x6840`** set
  — a bit only this class ever tests (exe+0xA407A, exe+0xB0B5A) — only a hit to the top body zone or
  of kind 7/0xE/0x1A reaches `hp -= dmg`; anything else subtracts from `+0x6AB4` and never reads HP.
  Death is `hp <= 0` **at the moment a hit lands** (`setHp(-1)`, then vtable+0xFC(2,…)), so holding
  HP at 1 left the thing standing however often it was shot in the body. Emptying `+0x6AB4` is not a
  death either: it scatters **18** pieces (vtable+0xFC(1,3,…)) — the `uEnemy45` units that fill the
  table in that room. One hit kills holds **both** at 1, so whichever way the next hit is routed it
  ends the form it lands on. `game::find_enemy_pool` derives the offset rather than assuming it:
  `uEnemy43`'s vtable from its DTI, then the one `call getHp` inside its methods whose result is
  stored straight back into the object (`getHp` = `8B 81 30 10 00 00 C3`, unique in `.text`),
  bounds-checked against the object size in the DTI.
- HP conventions from the same code: **`-1` is the dead sentinel** (`setHp(0xFFFFFFFF)` on every
  death path), an enemy killed from 1 HP sits at `1 - dmg`, and **`0` means the class never set any
  HP** — `uEnemy44` and `uEnemy45` (kinds 0x16 and 0x17) write only their kind index and live at
  0 HP for ever, so nothing held can reach them: they are the fight's leech props, not enemies with
  health. `Trace = 1` prints the second pool as `pool=N` for the units that carry one.
- The tree under `uEnemyBase`: `uEnemy10` is the base of the humanoid family (`11 12 14 15 16 17 19
  1a 50 51 5a 5b 6a 6b`), `uEnemy37` of `38`, `uEnemy3a` of `3e`, and **`uLeechBase`** of `uEnemy40
  41 42` (their code carries `effect\epv\em\em40/41/42`); the rest are direct children.

### Which game is running (`sGamePresence+0x30` / `+0x24`, src/game.cpp)
The main game, Wesker mode and Leech Hunter share the twenty save files, and nothing in `sGameInfo`
separates them cleanly. What does is the value the game hands **Steam's rich presence**, kept by
`sGamePresence` (exe+0x9CC01C, `cSystem` child) in **two fields**. The setter (exe+0x202020,
`mov [esi+0x30],eax` between `EnterCriticalSection` and `LeaveCriticalSection`) only files a
**request** at **`+0x30`**. The presence update (vtable slot 6, exe+0x201D40) takes it the next time
it runs - `ebx = [+0x24] ? ([+0x30] ? [+0x30] : [+0x24]) : 7` - and its tail (exe+0x201F76)
**publishes** it into **`+0x24`** (with `+0x28`/`+0x2C`, a difficulty and an area for the main game)
and zeroes the request. It publishes only a reading that changed, and holds the main game's back
until the room is known (exe+0x201CD0, or a non-zero area). So the request reads 0 once it is taken
and the published value lags: the first version of the mod read only `+0x30`, and the trace showed
`7` pending for 15 s at the title, then `0` through the load, the start and the whole Leech Hunter
run - it caught the start only because a tick landed inside the window. **The mod reads
`+0x30 ? +0x30 : +0x24`**, the update's own rule. The three entry points each ask for their value as
they commit, long before the room is on screen:

| value | set at | what it is |
| --- | --- | --- |
| 1 / 2 | exe+0x6126 (new game, in exe+0x6430), exe+0x6D95 (continue, in exe+0x5460) | the main game; 2 when `sGameInfo+0x60` says Wesker mode |
| 3 | exe+0x63EE (in exe+0x6180, "start Leech Hunter") | Leech Hunter |
| 4..8 | exe+0x1EACE4, 0x1EB017, 0x1EB257, 0x1EB506, 0x1EB717, 0x1EBA61, exe+0x7427, exe+0x8357 | the title screen's own menus (that region draws `ui\50_title\tex\title_00_jpn_win_ID`) |

`0` in both is the constructor's value, before the first request. So while a game is up the
reading names the mode, and it changes at the exact statement that applies a save file or lays out
a new game - which makes it both the earliest and the most exact "a game is starting" the mod can
see. `game::find_mode_field` reads both offsets out of the three sites that use them - the setter
(request), the update's head (`8B 5E pub ... 8B 46 req 85 C0 0F 45 D8`) and its tail
(`89 5E pub 89 6E ?? 89 7E ?? C7 46 req 00 00 00 00`, one match) - and requires them to agree; it
never assumes 0x30 or 0x24. It is used for *when* a game starts and for *which* - Leech Hunter's
box is never a file's (see Saving and loading) - not for keying the records: Leech Hunter cannot
save, so a file's box is a file's box.

The four starts, for reference (a dispatcher at exe+0x5140 tail-jumps to the first three). All
four apply the save buffer directly, **not** through the save/load state machine - but the title
reads the chosen file off the disk with a mode-5 load first, before a continue, before **Once
Again** (a new game from a cleared file: load of file 1 at 21:49:17, the game at 21:50:46) and
before **Leech Hunter from a cleared file** (load at 21:28:51, the run's request at 21:29:05), so a
load is not evidence of a continue:
- **continue** exe+0x5460: `cSaveManager::0x2127D0(file)` - the full apply (`sGameInfo::load(+0xD4)`,
  `+0x100`, `+0x21C`, `sItem::load(+0x1C854)`, `sPlayer::load(+0x1C8D0)`), the saved room into
  `sRoomControl+0x138`, and **no phase at all**.
- **new game from a save file** (Once Again) exe+0x6430: `cSaveManager::0x2128A0(file)` (copy
  recB→recA, then `sGameInfo::loadPart(rec+0xD4)` = only `+0x2C/+0x30/+0x34`), save count and play
  time to 0, characters set, the costume/unlock bits OR-ed into `sGameInfo+0x30` and `+0x40`, room
  0x58 into `sRoomControl+0x138`, phase 15 (`Opening`, exe+0x6D41).
- **Leech Hunter** exe+0x6180: the same partial apply, save count and play time to 0,
  `sGameInfo+0x30 |= 1`, phase **20** (`OmakeTitle`).
- **fresh new game** exe+0x57A0 (no file: `sSaveManager+0x24 = 0`, no partial apply), save count,
  play time and `sGameInfo+0x50` to 0, the bags reset (exe+0xDDA30), room 0x58, phase 15
  (`Opening`, exe+0x60BC) - or 18 (`WeskerTitle`) in Wesker mode, whose own end pushes `Opening`
  (`cRoomPhaseWeskerTitle` slot 7, exe+0x20CE42).

`sRoomControl` (exe+0x9CBEB4) carries the phase machine at `mPhaseManager` = `+0xB8`: `+0x14`
current, `+0x18` next (−1 = none), `+0x1C..` an 8-deep stack with the depth at `+0x3C`, ids 0..23
bounds-checked at exe+0x20A3D0. The id→name table is at exe+0x8E73E0 (`{name, id}`, 8 bytes each):
Init, Main, Message, MessageImm, DoorLoad, SubScreen, Save, UpCut, Option, Change, EventDemo, Map,
Movie, Event, Dead, Opening, StaffRoll, Ranking, WeskerTitle, WeskerRanking, **OmakeTitle**,
**OmakeResult**, PlayDemo, Exit. Not a mode - Leech Hunter runs in `Main` like everything else - but
it is what tells **a new game from a continue**: every new game pushes `Opening` (15), the fresh one
and Wesker mode's through `WeskerTitle` (18), and a continue pushes nothing (see the starts above).
`game::find_phase_fields` derives it all: the one push routine (`8B 51 next 83 FA FF 75 1A 8B 51 depth
8B 41 cur 89 44 91 stack 8B 44 24 04 FF 41 depth 89 41 next 8B 41 cur C2 04 00`, exe+0x20A340), the
one wrapper that reaches it (`81 C1 <machine> E9 <push>`, exe+0x210E00 - `add ecx,0xB8`), and the ids
of `Opening` and `WeskerTitle` from the name table by string. Only the top counts (`current` and
`next`), on the tick a game starts - never the stack below: every push leaves the phase it came from
on it, so what sits under the top says nothing about when it got there. `Save` (6) is entered by
three sites: the typewriter's save command (exe+0x17AE36, see Typewriter and ink ribbons), the main
game's ending (exe+0x17F544) and Leech Hunter's result (exe+0x20B047).

### Inventory (bag at `sItem+0x20` / `+0x60`, 0x40 bytes)
`+0x04+8i` item id, `+0x08+8i` count (i = 0..5), `+0x34/+0x38` personal item, `+0x3C` equipped
index (−1 none). Two-slot items {5,6,7,8,9,11,12,23,104} sit at an even index followed by filler id
180 count 1 (`src/items.h`). Loaded ammo is the weapon slot's count; ammo boxes are items too - so
anything that remembers a count has to remember **which item** it belonged to as well: the slot is
reused when a weapon is exchanged for another one, and a count kept per slot alone follows the slot
into the next gun. The subtraction itself is `Bag::take(slot, n)` at exe+0xDC3E0: `count <= n` zeroes
the slot and returns "used up", otherwise `sub [ebx+esi*8+8],edi` at exe+0xDC483 (once the ink cheat's site, see Dead ends)
takes it off - one routine for every item, which is why infinite ammo is a hold and not a patch.

**How many of an item fit in one slot** is not a property the mod may guess: one routine answers it
for the whole game (exe+0xDC880), and the pickup that tops a stack up, the reload that asks how much
room the equipped weapon has left and the item screen that greys a full slot out all go through it.

```c
int item_max(int id) {              // exe+0xDC880
  if (id >= 0x38) return (id == 0x5A || id == 0x5B) ? 10 : 1;
  return table[id];                 // 0x38 dwords at exe+0x8C6E40
}
```

The table: knife (and ids 1, 24) **0xFFFF** = no meaningful count; handguns and both custom handguns
15; hunting gun 2; shotgun 7; grenade launcher (all three rounds) 255; magnum 8; sub-machine gun 300;
magnum revolver 5; rocket launcher 2; molotov 255; every ammo box 255 **except machine gun ammo,
300**; empty bottle and gas tank 255; **every herb, mixed herb and first aid spray 1** (RE0 does not
stack them); ink ribbons 255. The tail is what a hand-written list gets wrong: everything from 0x38
up looks like a key item that does not stack, and the two that are not - the **blue and green leech
charms, id 0x5A/0x5B, which Leech Hunter stacks ten to a slot** - are the routine's own exception.
`game::find_item_max` reads the threshold, the two ids, their value, the default and the table's
address out of the routine (signature `8B 44 24 04 83 F8 ?? 7C ?? 83 F8 ?? 74 ?? 83 F8 ?? 74 ?? B8 ??
?? ?? ?? C2 04 00 B8 ?? ?? ?? ?? C2 04 00 8B 04 85 ?? ?? ?? ?? C2 04 00`, one match) and copies the
table once - it is const `.rdata` in an image that does not move. `src/items.h`'s `max` column is now
only the fallback for a build where that routine is not found.

**Which items stack** is the pickup's own answer, not the per-slot maximum's: a weapon's 15 is a
magazine, not fifteen guns. A second table, exe+0x8C69E0, belongs to the pickup path alone
(`Bag::add`, exe+0xDC070): 14 rows x 15 dwords, `max[row of the item picked up][column of the item
the slot holds]`. The row comes from `stack_row` (exe+0xDD470) - `14 -> 0`, `32..40 -> 1..9`,
`55 -> 11`, `90 -> 12`, `91 -> 13`, anything else -1, which `Bag::add` checks before it looks at a
slot - so **13 ids stack**: the Molotov cocktail, the six ammo boxes, the empty bottle, the gas tank,
machine gun ammo, ink ribbons and the two leech charms. Row 10 would be id 41's (the column map gives
it column 11) but the row map never returns 10, so nothing picked up can join one; no item list names
id 41. The column comes from `stack_col` (exe+0xDD3D0: the category routine exe+0xDD4D0 - 1..24
weapons, 31..41 ammo, 42..53 heals, 54..55, 56..141 keys, 149..179 files, 180 the filler - the id's
index inside its category and three small tables, the two charms by id): 0 for an empty slot,
`row + 1` for the same 13 ids, -1 for everything else. The table holds numbers only in column 0 and
on that diagonal, so a stack can only be topped up with **its own** item, and the diagonal agrees
with the per-item table everywhere - 255, 300 for machine gun ammo, 10 for the charms.

Where a pickup goes is exe+0xDAFB0's answer (the pickup itself is exe+0xDB4C0): files elsewhere, a
few key items to the personal slot, a two-slot item to a free pair, and for the rest **10** - "onto
the stacks", which calls `Bag::add` - only when `Bag::canStack` (exe+0xDBC70) finds room for **all**
of it among the slots already holding that id (the sum of `item_max - count`), otherwise the first
free slot, whole. `Bag::add` walks slots 0..5, fills the first one that is empty or holds a short
stack of the same item, carries what does not fit on to the next, and skips an infinite slot
(`cmp [edi],0xFFFF; je` at exe+0xDC139). So the game never splits a pickup between a short stack
and a new slot: what the stacks cannot take in full goes to a slot of its own.

The storage box has no slots to run out of, so it merges fully: an item `game::item_stacks` names is
kept as full stacks of `item_max` and at most one short one (`Box::put` on a store, `Box::consolidate`
on a record when its file is loaded - a line written before the box merged anything holds an entry per
store and is merged then, and written back that way by the next save); every other item keeps an
entry per store. A take (`storage::fit`) does the same to the inventory: it tops up the slots already
holding that item, in slot order, to `item_max` each - as `Bag::add` would - then puts the rest into
the first free slot, and what fits nowhere goes back into the box. Unlike the pickup it splits
freely, because a take is the player asking for it and the box keeps what does not fit. The panel
enables and describes a take button from the same `fit`, so it offers exactly what the take does.
The game's infinite (below) is never merged, and an infinite slot is never topped up. `game::find_stack_rows` reads the ids
out of `stack_row` (signature `8B 4C 24 04 83 F9 ?? 75 05 33 C0 C2 04 00 8D 41 ?? 83 F8 ?? 77 06 8D
41 ?? C2 04 00 83 F9 ?? 75 06 8D 41 ?? C2 04 00 83 F9 ?? 75 08 B8 ?? ?? ?? ?? C2 04 00 83 C8 FF BA
?? ?? ?? ?? 83 F9 ?? 0F 44 C2 C2 04 00`, one match) and requires a row of its own for each and an
`item_max` above 1; `items::stacks` is the list it falls back to.

**A count of exactly 0xFFFF is the game's own "infinite"**, a marker rather than an amount, and it sits
above every per-slot maximum on purpose. `Bag::isInfinite(slot)` (exe+0xDBE50) is asked before every
subtraction: unless `sGameInfo+0x5C` is 1 (Leech Hunter - the bag reset hands out the run's loadout
from that branch) or `[exe+0x9CC06C]+0x5C` is set, unlock bits in `sGameInfo+0x30` make whole groups
of weapons infinite whatever their count (0x80 ids 3/4/17/19, the four handguns; 0x40 id 5; 0x10
every id 0..24), and after that a slot is infinite when its count is 0xFFFF (`cmp dword [esi+4],
0xFFFF; sete al`). `Bag::take` returns "not used up" for it without touching the count, `Bag::count`
(exe+0xDC290) reports 0xFFFF for it, and `Bag::add` never tops it up (`cmp [edi],0xFFFF; je` at
exe+0xDC139). The new-game extras put one in a bag: exe+0xDD880, the tail of the bag reset
(exe+0xDDA30) when `sGameInfo+0x5C` is 0, gives Rebecca the Rocket Launcher, id 23, **x0xFFFF** for
bit 0x08 of `sGameInfo+0x40` (exe+0xDD8E2), then the closet key (141) x1 for 0x02, machine gun ammo
x300 for 0x100 and Billy a sub-machine gun x300 for 0x04. So a new game with the Rocket Launcher
unlocked starts with a 65535 in Rebecca's bag - her first slot in both logged runs, Once Again and
Wesker mode (both took the extras, so `sGameInfo+0x5C` was 0 in each). The mod
treats it as the marker everywhere: `read_bag` accepts it beside 0..9999, the panel and the storage
box say "infinite", the box hands it back exactly as it was stored instead of splitting it at the
per-slot maximum, and the editor turns it into the new item's maximum the moment another item is
picked for the slot - it never writes one of its own.

### Countdown timer (`sEventScript+0x2C`, src/game.cpp)
The timed sections (the train's brakes at 3:30, etc.) share **one float**: `sEventScript+0x2C`, in
the same **1/30 s** units as the play-time clock, with **−1.0 for "no timer"**. It was found from the
HUD, not from the script table: `uGUITimer` (`arc\ui\timer`, vtable exe+0x8DE668) draws it — slot 8
tests **bit 6 of `sGameInfo+0x50`** (`call exe+0x6885B0` = `lea eax,[ecx+50]`, `shr eax,6`) and, when
set, calls exe+0x1BF070, which passes `[sEventScript+0x2C]` to the formatter exe+0x1BEF70 when it is
≥ 0 and `+0x64` is 0 (`[+0x20] / (fps+1)` is the other, unused path). The formatter is what fixes the
unit: it multiplies by 1/1800 for the minutes and by 1/30 for the seconds, and takes the hundredths
from `v % 30`.

Two per-frame steppers subtract `30/fps * timescale` from it — the same expression
`sGameInfo::updateTime` adds to the clock — and kill the player when it reaches 0. They are the same
code compiled twice into different state machines (exe+0x1653C0, an entry of the handler table at
exe+0x5973B0, and exe+0x25D3E0 = `sUpCut` vtable slot 6), differing only in registers, so each needs
its own signature. `+0x2C` is written by three script handlers: exe+0x16AB70 sets it from an int and
stores its second argument in `+0x64`, exe+0x16AC60 and exe+0x1653F8 put the −1 sentinel back,
exe+0x1816F8 (the event-script reset) sets both `+0x2C` and `+0x30` to −1 at the start.

`TimerSet` (exe+0x16FBD0) only *shows* the timer (`sGameInfo+0x50 |= 0x40`) — it never reads its own
S2 argument. The script table's names do not line up with these handlers around row 143
(`FadeColor`'s handler takes two arguments and behaves like `TimeAttack`), so nothing here is
anchored on a command name.

### Typewriter and ink ribbons (src/game.cpp sites)
A typewriter runs the event script's save command - the handler in the `sewait` row, exe+0x17A9D0
(the names are off there, see Event-script command table) - a state machine on a float at `+0x10`.
State 1 asks `sPlayer` for the active unit (exe+0xEC780), looks item 0x37 up in that character's bag
(`Bag::findSlot`, exe+0xDB130: the slot, 6 for the personal item, -1 for none) and branches: -1, or no
unit, shows message 0xD - "you need an ink ribbon" - and ends; otherwise message 0xC, the "use a
ribbon?" question, and on a yes it writes its save point into `sSaveManager+0x28` and pushes phase 6,
`Save` (exe+0x17AE36). **The command never takes the ribbon.** `cRoomPhaseSave` does (vtable
exe+0x8E7190; the class name follows it in `.rdata`). The Save phase adds the save to the count before
the file is written (the **Save without counting** site, exe+0x20B951), and its update (exe+0x20BA30)
reads the save screen's answer when it closes (exe+0x5FED20 on the screen at `+8`): **1, cancelled** -
the count is taken back if it is above 0 (exe+0x20BBC4, the second **Save without counting** site),
unless the save point is the ending's 0x0C or `sGameInfo+0x5C` is 1 (Leech Hunter); **2, saved** - the ribbon is looked up in the active character's bag again and take-by-id
(exe+0xDB910) is called with (0x37, 1), again unless `sGameInfo+0x5C` is 1. So the file keeps the
ribbon that paid for it, and a second take, exe+0x20F1FD, takes (0x37, 1) when a game is set up: step 2
(exe+0x20EC10 -> exe+0x20EC90, unless `sGameInfo+0x5C` is 2) of the room-setup sequence exe+0x20EA10,
whose callers include the room areas' update and the Main, Event, EventDemo and UpCut phases (not
DoorLoad), and only when the save count is not 0. That it is "the loaded file's ribbon, taken again"
is a reading of the code; no log has shown it yet.

take-by-id finds the slot and calls `Bag::take` (exe+0xDC3E0); when that reports "used up" it clears
the slot (exe+0xDA9D0) - see Dead ends for why patching `Bag::take` was wrong. Both take calls discard
the result: the code after each is also where the no-ribbon guard in front of it jumps, with -1 in eax.

**Infinite ink ribbons** patches all three while it is on: the command's `je` to NOPs, so it always
asks the question (a save with no ribbon on you then passes the takes' own guards and takes nothing),
and each take's `call` to `59 59 83 C8 FF` - `pop ecx` twice for the two arguments the callee would
have popped with `ret 8`, then `or eax,-1`, what the guard's branch leaves. `find_sites` makes them
vouch for each other: both takes must call the same function, and the check's branch must land where
the command's no-unit branch does (exe+0x17AD78, message 0xD). If take B is what it looks like, a file
saved with the cheat on and loaded with it off gives its ribbon up at the load; the panel's switch is
not remembered, so only `InfiniteInkRibbons = 1` in the ini has the patch in place before the title
loads a file.

### Code sites (patched in memory, verified bytes, reverted on toggle-off/unload)
| what | signature | site |
| --- | --- | --- |
| play-time store (`sGameInfo::updateTime`, exe+0x19D540: `+0x3C += 30/fps*scale`, clamp 10799999 = 99:59:59) | `F3 0F 11 46 3C 72` | exe+0x19D5E0, 5 NOPs |
| save-count increment (`inc [eax]` after `call exe+0x19CF40` = `lea eax,[ecx+38]`, as `cRoomPhaseSave` starts, exe+0x20B8C0) | `E8 ?? ?? ?? ?? FF 00 8B 0D ?? ?? ?? ??` | exe+0x20B951, 2 NOPs |
| save-count take-back on a cancel (`dec ecx; mov [eax],ecx` after the same getter, in `cRoomPhaseSave`'s exe+0x20BA30; applied only while the increment is) | `83 78 0C 01 74 ?? 8B 0D ?? ?? ?? ?? 85 C9 75 1B 68 ?? ?? ?? ?? 68 8B 01 00 00 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 4C 24 ?? 83 C4 0C E8 ?? ?? ?? ?? 8B 08 85 C9 7E 03 49 89 08` | exe+0x20BBC4, 3 NOPs |
| ink-ribbon take A (take-by-id exe+0xDB910 called with (0x37, 1), in `cRoomPhaseSave`'s exe+0x20BA30) | `6A 01 6A 37 8B CF E8 ?? ?? ?? ?? 50 8D 44 24 ?? 50 E8 ?? ?? ?? ?? 83 C4 04 8B C8 E8 ?? ?? ?? ?? 8B C8 E8 ?? ?? ?? ?? 8B C8 E8 ?? ?? ?? ??` | exe+0x20BB30, the `call` -> `59 59 83 C8 FF` |
| ink-ribbon take B (the same call, in exe+0x20EC90) | `6A 01 6A 37 8B CD E8 ?? ?? ?? ?? 50 8B CF E8 ?? ?? ?? ?? 8B C8 E8 ?? ?? ?? ??` | exe+0x20F1FD, the `call` -> `59 59 83 C8 FF` |
| typewriter ribbon check (the save command's `je` when `findSlot(0x37)` is -1) | `E8 ?? ?? ?? ?? 85 C0 74 ?? 6A 37 8B C8 E8 ?? ?? ?? ?? 50 8D 44 24 ?? 50 E8 ?? ?? ?? ?? 83 C4 04 8B C8 E8 ?? ?? ?? ?? 8B C8 E8 ?? ?? ?? ?? 8B C8 E8 ?? ?? ?? ?? 83 F8 FF 74 ??` | exe+0x17AD3B, 2 NOPs |
| countdown step A (in exe+0x1653C0) | `F3 0F 10 49 2C F3 0F 59 05 ?? ?? ?? ?? F3 0F 59 40 68 F3 0F 5C C8 0F 57 C0 0F 2F C1 F3 0F 11 49 2C` | exe+0x1654CD, 5 NOPs |
| countdown step B (in exe+0x25D3E0) | `F3 0F 10 41 2C F3 0F 59 0D ?? ?? ?? ?? F3 0F 59 48 68 F3 0F 5C C1 0F 57 C9 0F 2F C8 F3 0F 11 41 2C` | exe+0x25DCBA, 5 NOPs |

NOPping a countdown store leaves the comparison after it looking at the decremented value in the
register, which is still positive, so the "expired" branch is not taken either. Both steppers are
patched or neither: patching one would leave the countdown running through the other. Only the
*step* is patched, never the script's own write of a new value or of the −1 sentinel, so the freeze
is per timed section on its own: the section that is running stops where it is, the section that
starts next is frozen at its starting value. The hold fallback has to reproduce that by hand — it
re-bases whenever the field moves further than a frame's step from the held value **in either
direction**, since the next section's timer can be shorter as easily as longer.

The pause predicate is derived from the play-time updater: `E8 ?? ?? ?? ?? 84 C0 75 ?? A1 ?? ?? ?? ?? 83 78 60 00 75 06 80 78 64 00`
→ `isPause` (`8B 0D slot 6A bit E8 testBit C3`) → `testBit` (`... 85 82 disp32 0F 95 C0 C2 04 00`).
The enemy table comes from the lookup the `EnemyDeath` handler calls (four tables share the shape; the handler picks the right one): `53 8B 5C 24 08 55 56 8B E9 33 F6 57 8D 7D off 83 FE count 72 14` … `46 83 C7 stride 83 FE count 72 CE`.

### Saving and loading (`sSaveManager` -> `cSaveManager`, src/game.cpp + src/storage.cpp)
`sSaveManager` (exe+0x9CC018) holds at **`+0x20`** a **`cSaveManager`** object (DTI size 0x11D3B0
dwords = 0x474EC0 bytes, vtable exe+0x8E7914) that is both the state machine and the whole save file
in memory. Header: `+0x04` **mode**, `+0x08` **state**, `+0x0C` **result** (0 = ok), `+0x10` **op**
(a file 0..19, or 0x15 system data / 0x16 / 0x18), `+0x14` second argument, `+0x20` 0x2A dwords of
system data; then **20 file records of 0x1C850 bytes at `+0xC8`** and a second array of the same at
`+0x23A7C8` (`+0x18` and `+0x23A718` are two `cSaveData` sub-objects, 0x23A700 each). Inside a
record (the capture routine exe+0x2136D0 writes them in order): `+0xC8` 0, **`+0xCC` the save point**
(`sSaveManager+0x28`, the U1 the `save_point` script command sets; `uGUISave` turns it into the save
screen's `ui\13_save\tex\type0N_ID_HQ` and asserts *"the save point value is invalid"* on 0, and
the ending sets it to 0x0C for the cleared-game save), `+0xCD` the save count (`sGameInfo+0x38`),
`+0xCF` `sGameInfo+0x34`, **`+0xD0` the Wesker-mode flag** (`sGameInfo+0x60`, which the save GUI
puts back from here when a file is picked - exe+0x1D39F0), `+0xD1` a byte of exe+0x9CC06C, then
**`+0xD4` the serialised `sGameInfo`** (`+0x2C`..`+0x4F`, `+0x64`, `+0x68`; so `+0xD8` is
`sGameInfo+0x30`, whose bit 0 the Leech Hunter start sets) and the other systems the two routines
below hand to each other - offsets are given from the object base, so subtract `+0xC8` for the
record-relative one.

Six request methods share one shape and are the mod's anchor - it reads the field offsets and the
mode numbers out of them rather than assuming any:
`cmp [ecx+4],0; jne busy; cmp [ecx+8],0; jne busy; [mov [ecx+10],arg;] mov [ecx+0C],0;
mov [ecx+04],<M>; mov [ecx+08],1; mov al,1; ret` - signature
`C7 41 ?? 00 00 00 00 C7 41 ?? ?? 00 00 00 C7 41 ?? 01 00 00 00 B0 01`, 6 matches, one per mode:

| mode | request | task | what it does |
| --- | --- | --- | --- |
| 1 | exe+0x212A50 | exe+0x212AD0 | create the file, all records defaulted (op 0x15) |
| 2 | exe+0x212A90 | exe+0x212C40 | whole-file operation (op 0x15) |
| 3 | exe+0x2134C0 | exe+0x213080 | **save** file `op` (`op` 0x15 = system data only) |
| 4 | exe+0x213500 | exe+0x213080 | save, copying the current file over file `op` |
| 5 | exe+0x213390 | exe+0x212E30 | **load** file `op` (0x15 = read every record) |
| 6 | exe+0x212A10 | exe+0x212D20 | whole-file operation (op 0x18) |

The task dispatcher is `mov eax,[ecx+4]; dec eax; cmp eax,5; ja out; jmp [eax*4+0x613408]` at
exe+0x2133D0. A task runs state 1 (capture / start the disk) -> 2 (waiting on `sSavedata+0x24`, whose
`+0x28` is the result and `+0x20` is 2 for a write, 3 for a read) -> 0, then clears the mode on its
next frame. Both tasks write the finished file number to `sSaveManager+0x24`.

*One file was just saved or loaded* is therefore mode 3/4/5 with `op` < 20, but it must **not** be
read off the finished sample (mode set, state 0): that sample is one frame wide, and the main-thread
tick that polls it runs at about the same rate, so it was missed on roughly every other save - the
box on disk silently stayed a save behind the game's own file. `storage.cpp`'s `watch_saves` latches
mode + `op` while the operation is **running** (state != 0), which spans the whole 2.3 MB write, and
applies it the first time the reading is no longer that operation - whichever of mode or state was
seen to drop. `result` survives until the next request zeroes it, so it still reads true one
operation later (a typewriter save chases the file with a system-data save, `op` 0x15).

Record layout constants come from the routine that applies one file to the game (exe+0x2127D0,
`memcpy(recA[i], recB[i]); sGameInfo::load(+0xD4); ...; sItem::load(+0x1C854); sPlayer::load(+0x1C8D0)`;
its counterpart exe+0x2136D0 captures the game into a record): signature
`83 FE ?? 0F 87 ?? ?? ?? ?? 69 F6 ?? ?? ?? ?? 8D 87 ?? ?? ?? ?? 03 C6 68 ?? ?? ?? ?? 50 8D 87 ?? ?? ?? ?? 03 C6 50`
-> file count (`cmp esi,0x13` + 1), stride 0x1C850, the two record array offsets. The storage only
uses them to prove the object it found is the save buffer; the save-file manager (below) is the one
feature that reads and writes records - in memory, and it has the game write them: the file on disk is
Blowfish-ish encrypted in 8-byte blocks (`data0.bin`, 2,337,008 bytes, repeated ciphertext blocks for
repeated plaintext) and the mod never opens it. The storage box is kept in the mod's own
`RE0CabbyCodes.storage.ini`, **one `file<N>` line per save file** - a file has one box, whatever was
played into it. Leech Hunter has no save at all, so it only ever holds a box in memory; Wesker mode
writes the same twenty files the main game does.

**The cleared-game save wipes its file's record.** That file is the one the player starts over from,
and a new game starts with nothing, so its box has to start with nothing too. The ending is the one
place that does not take its save point from a typewriter: exe+0x17F4EF zeroes the save count and
writes **0x0C** into `sSaveManager+<save point>` before handing over to phase 6 (`Save`), and
`C6 ?? 28 0C` - a byte store of 0x0C at that offset - occurs **exactly once in `.text`**.
`game::find_save_point` derives both the field and that value rather than assuming them: of the
sites that store an immediate byte into the singleton behind its `is_initialized()` check, the ones
whose `A1` operand is `sSaveManager`'s static slot must agree on the offset, and exactly one of them
must write something other than 0 (the other is the title screen putting a 0 back, exe+0x1EDEC4).
The value is read as the save *starts*, which is when the capture routine copies it into the record.

**Leech Hunter's result screen makes the other save**, and the 0x0C marker never sees it:
`cRoomPhaseOmakeResult` (vtable slot 7, exe+0x20AFF0) writes save point **0**, then pushes `Exit`
and `Save` (exe+0x20B038..0x20B047), and the save that follows is a **mode-4 file copy**. It is the
only save Leech Hunter has, so `watch_saves` takes any save made while the mode reads Leech Hunter
for that one - and **the storage leaves it alone**: the file's record is not touched and the ini is
not written, and the run's box ends with the run. The first version wrote the run's 4
items into `file1` (`save file 1 saved: 4 item(s) kept with it (file copy)`); the second wiped the
file's record instead - both wrong, the file's box is none of the run's business. The log line
carries the save point.

**Which game the box belongs to** (src/storage.cpp `watch_session`). Two things say a game has
started, and the first to speak wins:
- **the presence reading changing into 1/2/3** (above). This is the tightest signal there is - the
  same statement that applies the save file or lays out a new game. It also fires the moment the
  reading leaves a game mode, which is when a load that never became a game stops counting. A
  reading that is neither a mode nor a menu value is *"this tick says nothing"* and is ignored, or
  one unlucky tick would empty the box.
- **the player units appearing** (`game::in_game`), up to a minute and a half later, and all there
  is when the presence field was not found or when a game restarts without the mode changing -
  reloading from the death screen. A units gap while the mode never left its game value is *not* a
  new game: it is a cutscene, or that reload, and only the reload has a file waiting.

A *load* (state-machine mode 5, `op` < 20) is only half the story - it says a file was read off the
disk, not that a game came up - so it is staged and the box is put together when one does. A staged
load that no game ever claimed is dropped, both when the mode goes back to the menus and after two
minutes, which is the bug that used to carry the previous game's box into a new one: the old code
only let a pending load expire while a game was already running.

**Leech Hunter never takes a file's box**, staged load or not. It is started *from* a save file
(a cleared one - that is what unlocks it), and the title loads that file first exactly as it would
for a continue, so the staged load is real and still not this game's: the run cannot save. It gets
an empty box of its own, tied to no file; every file's box is still in its record on disk. (The
first version handed it the file's 79 items: `the game loaded save file 1` ->
`a game started: save file 1 is back`.)

**A new game never takes a file's box either** - Once Again from a cleared file included, whatever
that file's record still holds. On the tick the presence names the game, `begin_game` asks
`game::new_game_starting()`: the new-game routines push `Opening`/`WeskerTitle` in the same call
that files the mode, so the machine's top shows it; a continue leaves it alone. Only a staged load
whose game is *not* a new game is a continue and gets its file's box back. When Leech Hunter is
followed by anything else, its box is dropped first, so none of the run leaks into that game.

### Deleting and copying save files (the title's load list, src/savefiles.cpp)
**The two record arrays are not two versions of the truth: the second is the disk.** The routine that
names data0.bin's sections (exe+0x213640) points `VERSION` (4 bytes), `SYSTEMDATA` (0xA8) and
`GAMEDATA%d` (20 x 0x1C850) at `+0x23A718`, `+0x23A720` and `+0x23A7C8` - all in the second copy. The
capture (exe+0x2136D0) fills the first copy's record and ends `memcpy(second[i], first[i])`; applying a
file (exe+0x2127D0) is `memcpy(first[i], second[i])` and then the systems' loaders; the load task
(exe+0x212E30) reads the whole file into the second copy and then applies op `0x15` = all twenty
(what the title does as it starts, exe+0x1EB150 - so the last file's systems are what is loaded
there), op < 20 = one (and `sSaveManager+0x24 = op`), op `0x16` = copy the second over the first, no
apply. The file list and the game read the first copy; the disk is written from the second.

**A file is empty when the save-point byte of its record, `record+4`, is 0** - and nothing else says
so. The capture copies `sSaveManager+0x28` there; creating data0.bin (mode 1, exe+0x212AD0) writes
default records (exe+0x212390) and forces it to 0 for all twenty; and the list's own test
(exe+0x1D28C0: `cmp byte [records+0xB4+i*stride],0`, records = `cSaveManager+0x18`) is what prints
NO DATA (message 0xE, exe+0x1D4640) and makes a confirm on that row play the buzzer (0x805) instead of
loading (exe+0x1D42B0). So **Delete** writes 0 to that byte in both copies and leaves the rest of the
record alone: a record whose save point is 0 is never read again but by the title's apply-all, which
the same contents went through at every boot before. A real default record needs three systems'
default initialisers (inside exe+0x212390); the mod would have to call them or reproduce them.
**Copy** is both copies of the record, verbatim. The rest of the header, for the panel: `+5` save
count (byte), `+6` a costume/character index, `+7` `sGameInfo+0x34`'s low byte, `+8` the Wesker-mode
flag (`sGameInfo+0x60`, exe+0x1E379B), `+9` a byte of exe+0x9CC06C, then from `+0xC` the serialised
`sGameInfo` (exe+0x19D4A0: `+0x2C`..`+0x33` -> `+0`, `+0x34` -> `+8`, `+0x38`..`+0x3F` -> `+0xC` in one
`movq`, ...), so `record+0x18` is the save count and `record+0x1C` the play time.

**Writing is the game's own system-data save**: mode 3 with op `0x15`, which the game itself chases
every typewriter save with. The save task (exe+0x213080) checks the Steam user stamped into the second
copy (exe+0x212600, result 5 on a mismatch), captures the system data (exe+0x2139F0: exe+0x9CC06C's
and `sExtraData`'s state into `+0x20`.., copied to `+0x23A720`, the Steam user stamped in), copies the
VERSION dword and starts `sSavedata`'s write of the second copy (exe+0x8B3280) - the whole file, twenty
records included. The manager asks for it with the request method's own five stores, in its order
(`+0x10` op, `+0x14` second argument 0, `+0xC` result 0, `+0x4` mode 3, `+0x8` state 1), only while
mode and state are both 0; `watch_saves` ignores it (op is not a file). It backs both copies of the
record up first and puts them back if the write's `result` is not 0. The game's own file copy is not
usable here - see Dead ends.

**The storage boxes follow what reached the disk, even across a crash.** A deleted file keeps no box
and a copy takes its source's box, but only for a write that succeeded - and the mod only learns that
on the tick after the game's task finishes. So the change is journaled: `storage::file_change_begin`
writes `pending = copy <from> <to>` / `pending = delete <file>` into the storage ini before the request
fields are written (the game's task only starts on the next frame), `file_change_end(true)` makes it
the records' own and drops the line, `file_change_end(false)` just drops it. A `pending` line read at
start-up belongs to a run that ended before the game answered (the game closed while it wrote) and is
settled from the game's own records by `settle_left_over`: a deleted file that reads as unused, or a
copied one that is byte for byte its source (`game::same_save_file`), means the write had landed.
It is settled only while the records are data0.bin's: when the load list is up (`savefiles::tick`,
with nothing of this run in flight - the title reads the whole file before its menu), and first thing
in `begin_game`, so a game never picks its box from an unsettled record. The integration test for all
of this runs the real `savefiles.cpp`/`storage.cpp` against a fake save machine under Wine (see the
workflow memory).

**The load list** is a `uGUISave` (DTI exe+0x9D0974, vtable exe+0x8E07E0, 0x400 bytes) - the screen
the typewriter opens too, which the Save phase makes for itself (exe+0x20B8C0, `open(0)`). The title's
lives at `uGUITitle+0x264` (uGUITitle's init, vtable slot 5, exe+0x1EDC40); `uGUITitle` at `aTitle+0x38`
(`aTitle` slot 6, exe+0x82E0); `aTitle` on `sArea`'s area stack (count `+0x3824`, areas `+0x382C`,
sArea slot 9 exe+0x357D60 walks them; the live object is an `sGameArea` - see Dead ends). The title's main menu (state 0x24, exe+0x1EDEB0, choice
`+0x280` == 4) clears the save point and calls `open(1)` (exe+0x1D4AD0: only from mode 0 or 0xC; sets
`+0xC |= 0x400`, kind `+0x3C4`, mode `+0x3E8` = 1, state `+0x3EC` = 0). The update (slot 8,
exe+0x1D2A70) runs while the byte `+0x254` is set and dispatches on the mode: 1 lay out, 2 fade in,
3 check the file on disk (state 0 starts the check - a mode 6 request, 1 waits, **2 shows the list,
builds its six rows from the records (exe+0x1D4B70 -> exe+0x1D4640 per row), puts the cursor `+0x3D0`
on `sSaveManager+0x24` with the scroll `+0x3D4` = max(0, file - 14), and sets mode 4**), 4 the list
waiting for the player (exe+0x1D42B0), 5 overwrite? (saving), 6 save, 7 load (exe+0x1D3520), 8 the menu
after a load (Continue / Once Again / Leech Hunter / back to 4), 9 quit?, 10 close, 11 restart; 12 is
closed. **The rows are text, not a view**: they are set when the list is built or scrolled, so the
manager rebuilds them after a change by writing state 2 and mode 3 - the visibility helpers that state
calls (exe+0x480D30, exe+0x495190) only act on a change, so from mode 4 it re-shows nothing and only
rebuilds the rows - with `sSaveManager+0x24` set to the file that changed, and put back once the
rebuild has run (the title only uses it as the list's first cursor; every start sets it anyway).
The panel shows while the list is in mode 4 with kind 1 and `+0x254` set (`savefiles::showing`,
`dispatch` `file_screen`), and only then does anything.

`game::find_record_fields` and `game::find_load_list` derive all of it, each signature unique and
cross-checked against what was already derived: the capture's head (`69 FF <stride> 03 F9 8D B7
<records> C7 06 00 00 00 00 A1 <sSaveManager> ... 8A 40 <save point> 88 46 <record's byte>`), the
list's test (`69 F6 <stride> E8 <mov eax,[ecx+X]; add eax,Y; ret> 0F B6 CB 80 BC 30 <disp> 00 ...`,
`Y + disp` must be the record's byte), the Wesker store and the sGameInfo block inside the capture, the
save task's branch (`8B 46 <op> 83 F8 <0x15> 75 09 8B CE E8 .. EB .. 85 C0 78 .. 83 F8 <files> 7D ..
83 7E <mode> <copy> 75 ..`), `open()` (`8B 81 <mode> 85 C0 74 05 83 F8 <closed> 75 .. 8B 44 24 04 81 49
.. 00 04 00 00 89 81 <kind> C7 81 <mode> 01 00 00 00 C7 81 <state> 00 00 00 00 C2 04 00`), the title's
call (the second save-point writer, then `8B 8E <list> 6A <kind> E8 <open>`), the update's dispatch
(`80 BE <active> 00 74 .. 8B 86 <mode> 48 83 F8 .. 77 .. FF 24 85 <table>`, each entry `8B CE 5E E9
<handler>`), the rebuild (`A1 <sSaveManager> ... 8B 40 <current> 89 86 <cursor> 8D 48 .. C7 86 <scroll>
00 00 00 00 8B 86 <scroll> 85 C9 0F 4F C1 8B CE 89 86 <scroll> E8 <rows>`, then `C7 86 <mode> <browse>
00 00 00 C7 86 <state> 00 00 00 00`), which mode and state run it (the handler whose own state switch
has a case containing it), and sArea's walk (`39 B3 <count> 76 .. 8D BB <areas> 8B 0F 8B 01 FF 50 ..
89 44 B4 .. 46 8D 7F 04 3B B3 <count> 72 ..`). `aTitle`'s uGUITitle field is found at runtime by class.

### Event-script command table
313 rows × 12 bytes at exe+0x8D57DC..0x8D6688 in `.rdata`: `{argsig, name, handler}` (argsig tokens
`U1 U2 U4 S2 S4 F4 Fn`). Anchors used: `PlayerMutekiSet` exe+0x174B90, `PlayerMutekiReset`
exe+0x174C80, `PlayerDamage` exe+0x173290, `EnemyDeath` exe+0x16DDB0, `item_get` exe+0x179390,
`item_sub` exe+0x179610, `save_point` exe+0x17AF30, `PlayerEquip` exe+0x16C620, `CharChange` exe+0x169750.
`save_point` is an anchor by name only: its row's handler is a wait timer (see Dead ends), and the
typewriter's save command - the one that does set the save point - is the handler in the `sewait`
row (exe+0x8D5B9C -> exe+0x17A9D0, see Typewriter and ink ribbons).

## Dead ends worth not repeating
- A heap scan for a vtable value finds its own argument on the calling thread's stack first; the
  first launch cached that bogus hit for six singletons. `mem::find_objects_by_vtable` now skips the
  current stack and the scan buffer, and singletons are re-resolved on every use.
- The DTI id is not always a CRC: `uPlayer*` classes carry explicit ids (top bit set).
- `sInGameSystem`'s constructor sits right after `sGameInfo::updateTime` in `.text`, which made it
  look like the clock's owner; the live data (save count 0→1 on a save, +0x3C at 29.2/s) says
  `sGameInfo`.
- The `mPause`/`mPauseStatus` property strings belong to other classes (sound/animation), not to
  `sGamePause`; the pause flag came from the code that skips the clock.
- `sGameInfo`'s save count and play time cannot tell a new game from a loaded one. The new-game path
  does zero both (exe+0x6430: `*(int*)(sGameInfo+0x38) = 0`, `*(u32*)(sGameInfo+0x3C) = 0`) - an
  earlier note here said it did not, from a live reading that the code does not support and that
  **No save count** and **Freeze play time** would both have produced on their own - but a file
  saved early enough reads the same, and the mod's own switches can pin either to 0 whenever they
  are on. Nor can a clock: a new game can begin seconds after a load. The storage uses the game
  mode and the engine's load event instead (src/storage.cpp `watch_session`).
- `sSaveManager+0x28` is the **save point**, not a game mode: a U1 the typewriter's script sets,
  which only picks the save screen's background (`uGUISave`, three `type0N` textures). It is
  captured into every record at `+0xCC`, which makes it look like a per-file "kind" - it is not
  one. Its one special value is the ending's 0x0C, which is what marks a cleared-game save.
- `sGameInfo+0x30` bit 0 is set by the Leech Hunter start (exe+0x6180) and is in the serialised
  block, so a Leech Hunter save carries it - but the **new-game and Leech Hunter paths both copy
  `sGameInfo+0x2C/+0x30/+0x34` in from the chosen file's record first** (exe+0x2128A0 ->
  exe+0x19D3E0), so a new main game started over a file that once ran Leech Hunter inherits the bit.
  It is a per-file unlock flag, not "Leech Hunter is running"; `sGamePresence+0x30` is.
- `sRoomControl`'s phase machine is not a mode either: Leech Hunter passes through phase 20
  (`OmakeTitle`) and then runs in phase 1 (`Main`) like the main game.
- `sGamePresence+0x30` alone is not the mode: it is the setter's request, which the presence update
  zeroes once it has published it into `+0x24`. Read the pair the way the update does.
- The script table's `OmakeStart` row (exe+0x8D63D0) and its `save_point` row (exe+0x8D5BA8) share
  one handler, exe+0x17AF30, and that handler is a `wait`-shaped timer (`sMain+0x38` fps,
  `+0x68` timescale) that is neither - the same name/handler misalignment as the countdown rows.
  Leech Hunter is not started from a script command. The command that does set the save point is
  the handler in the `sewait` row (exe+0x17A9D0): it asks the ink ribbon question and writes
  `sSaveManager+0x28` on a yes.
- Swallowing mouse and key messages in the window procedure does not keep input from the game (see
  Input above), and while the panel is hidden the messages must still be fed to ImGui: a button
  release that arrives after the panel is hidden is otherwise never delivered, and ImGui then drags
  the window around for ever because it still believes the button is held. **But handing them over
  is not the same as consuming them.** ImGui only queues what the backend gives it and drains that
  queue in `NewFrame`, which the mod runs only when it draws: everything clicked while the panel was
  hidden was banked and replayed onto the panel the moment it reappeared, at the positions the
  cursor had held during play. It looked like the inventory changing on its own - the log caught two
  storage takes firing 33 ms and 95 ms after the pause menu opened, one to two frames. `wants_draw`
  now calls `io.ClearEventsQueue/ClearInputMouse/ClearInputKeys` on every frame it does not draw.
- **The panel and the window procedure are on different threads, and ImGui is not thread-safe.**
  The fix above (clearing the queue from `wants_draw`) then crashed the game: `ImGuiIO::ClearEventsQueue`
  is `ImVector::clear()`, which *frees* the storage and nulls the pointer, and it ran on the render
  thread (484) while a `WM_MOUSEMOVE` was inside `AddMousePosEvent` on the window thread (356) —
  which is also the game's main thread. That function reads `InputEventsQueue.Size` and
  `.Data` in two separate loads: size came back 1, data came back null, and `&Data[Size-1]`
  dereferenced address 0 (`steam_api.dll+0xEE426`, `cmp [edx],1` with `edx=0`, one hour into a
  session with the panel hidden). `push_back` reallocating under `NewFrame`'s drain is the same
  bug with a wider window. Every touch of the context — the backend's `WndProcHandler`, the
  `io.Want*` reads, `wants_draw`, `NewFrame`..`Render`, the DX9 backend's device-object calls —
  now goes under one critical section (`overlay::ImGuiLock`). What must stay outside it: the
  game's own window procedure, the real `IDirect3DDevice9::Reset`, and the input guard's
  `capturing_mouse/keyboard`, which is asked from the game's input thread and reads a pair of
  atomics published by the frame instead.
- The save watcher cannot poll for the state machine's *finished* reading: it lasts one frame, and so
  does the gap between two main-thread ticks (`dispatch.cpp` throttles to 16 ms). Watch the running
  phase, which lasts as long as the disk does, and act on its end.
- The save "file list" is not a separate structure: the 20 records inside `cSaveManager` are the file
  list, the save menu and the loaded game all at once. Do not look for a smaller per-file summary.
- `sSaveManager` is 44 bytes and only points at the real thing; `cSaveData` (2.3 MB) is one of the two
  record arrays *inside* `cSaveManager` (4.6 MB), not the object the singleton hands out.
- The script command table's `name` column does not line up with its `handler` column everywhere:
  around row 143 the handler at `FadeColor`'s row takes two arguments and is the one that starts a
  countdown, `TimeAttack`'s row holds a no-argument handler that clears it, and two adjacent rows
  share exe+0x16FAC0. The anchors the mod uses (`PlayerMutekiSet`, `EnemyDeath`, `item_get`,
  `save_point`, …) were each checked against their handler's code and do line up — but a new anchor
  has to be verified the same way rather than trusted.
- `tools/find_dti.py --list` finds exactly one class: every `MtDTI` has its **own** DTI vtable, so
  scanning `.data` for `MtObject`'s DTI vtable value cannot enumerate the rest. Walk the tree from
  `MtObject` through `+8` (next) and `+0xC` (child) instead; `--class`, `--script`, `--xref` and
  `--strref` are unaffected.
- **The per-slot maximum is the game's, not a list's.** `src/items.h`'s `max` column was written from
  the community item lists, which have names but no limits: every id from 0x38 up was held at 1 and
  every stackable was left at "unknown" (clamped to the panel's own 999). Both were wrong in a way
  that destroyed items rather than refusing them - the inventory editor cut a typed count to 1 for
  the leech charms Leech Hunter is played with, and the storage box handed a ten-charm stack back as
  one charm and dropped the other nine on the floor (`storage: stored Green Leech Charm x10 ...` then
  `took Green Leech Charm x1`). The numbers come from exe+0xDC880 now, a take leaves what does not
  fit in the box, and a clamp says so in the panel and the log instead of quietly writing a smaller
  number. The `kind` column is still the mod's own and can stay so: it only picks the bank the
  storage box lists an entry under (the box is one list, the ini one line per file) and the slots
  infinite ammo holds, so a wrong kind misfiles an item rather than destroying one.
- **A count above every per-slot maximum is not garbage.** `read_bag` held counts to 0..9999 to catch
  a wrong layout, and the game's own infinite, 0xFFFF, failed that test: a new game with the Rocket
  Launcher unlocked lost the whole inventory for the session (`bags: reading at … thrown away: slot
  count 0 = 65535 is out of range`, Once Again at 21:50:58 and Wesker mode at 21:55:58 on 2026-09-10;
  it was reported as Wesker mode's inventory not being found). Whatever bounds, clamps or splits a
  count has to let exactly 0xFFFF through as what it is (see Inventory above).
- **A singleton is registered by its concrete class.** `dti::resolve` matches an object's exact
  vtable, and the area system the game keeps in `sArea`'s slot (exe+0xA2D8C0, stored by the base
  constructor at exe+0x3576F6) is an `sGameArea` - `sArea`'s one child, with no fields of its own and
  a vtable of its own (exe+0x8B9A94). The first load-list build asked for `sArea`: it was never found
  (`dti: sArea: no live instance found yet`, and nothing after), so the panel never came up on the load
  screen, and `background_tick` heap-scanned 833 MB for it every 2 s. `sGameMain`, `sGameGUI` and
  `sGameScene` are the same case. Walk the DTI tree for children before registering a singleton;
  `load list:` lines now say which link of the chain is missing whenever that changes.
- **The game's own file copy is not a plain copy.** Mode 4 (exe+0x213930) copies the current file
  (`sSaveManager+0x24`) over the target and then writes the running game's `sGameInfo+0x30` unlock
  bits into the target (`record+0x10`) before copying it to the second array - right for Leech
  Hunter's result screen, which is what uses it, wrong on the title screen, where the running
  `sGameInfo` is whatever the apply-all left there (the last file's). The manager copies the records.
- **The load list does not watch the records.** A record changed under an open list keeps its old
  text until the list scrolls, because the rows are text set when they are built; the rebuild is mode
  3 state 2 (see Deleting and copying save files).
- **The ink ribbon is not taken by one subtraction.** The first Infinite ink ribbons NOPped the `sub`
  in `Bag::take` (exe+0xDC483), recorded here as "in exe+0xDC3B0, single caller exe+0x1E50BA in the
  typewriter flow" - but exe+0xDC3B0 is the tail of the function before `Bag::take`, and exe+0x1E50BA
  calls the item screen's combine check (exe+0x1DC3B0). `Bag::take` has a second exit: when the count
  is no more than what is taken it writes 0 and returns "used up", and take-by-id clears the slot - so
  a save made with the **last** ribbon used it up with the cheat on (2026-09-12 16:42:21; the four
  saves before it had 4, 5, 8 and 12 ribbons and kept them). And that `sub` is every take-by-id's:
  seven callers - both ribbon takes, four key-item takes (ids 102 and 127) and a script handler - so
  while the cheat was on, any partial take through that path was skipped too. The cheat now patches
  the two ribbon takes themselves (see Typewriter and ink ribbons).
- **Save without counting once skipped only the increment.** The Save phase adds the save as it starts
  (exe+0x20B951, before the screen opens) and takes it back when the screen is cancelled (`if (count >
  0) count--`, exe+0x20BBC4), so with the increment skipped every cancel lowered the count by one -
  never below 0, which is why a test at a count of 0 showed nothing (2026-09-12). Both are patched now,
  the take-back only while the increment is.
- Community cheat-table addresses for this build (status `[exe+0x9CBE9C]`, bags `[exe+0x9CBF44]`,
  character manager `[exe+0x9CBF3C]`, `menuId` chain from `[exe+0xA2F688]`) all checked out but are
  not used: the DTI path finds the same objects by name.

## Tooling notes
- `DumpImage=1` writes the decrypted image with raw offsets == RVAs and our IAT patches undone;
  `tools/fix_dump.py` sets the entry point to the OEP so Ghidra starts at the CRT entry.
- Ghidra headless analysis of the 11 MB dump takes ~3 minutes on 16 cores; `tools/ghidra/DecompileAddrs.java`
  decompiles a list of addresses in one run (output lines are prefixed `INFO  DecompileAddrs.java> `).
- `tools/find_dti.py` enumerates every DTI (name, parent chain, size, id, `getDTI` stub, vtable) and
  the script table from the dump, and finds code/data references to a string or an immediate.
- x86 encodes small displacements in one byte: write patterns from the raw bytes (`objdump -d` on the
  fixed dump), never from mnemonics.
