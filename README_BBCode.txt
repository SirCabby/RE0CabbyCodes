[size=6][b]RE0CabbyCodes[/b][/size]

A [b]cheat panel[/b] for Resident Evil 0 HD Remaster: pause the game while playing and tick what you want - God mode, One hit kills, Infinite ammo, Infinite ink ribbons, Save without counting, save count, play time and countdown timer editors, an inventory editor for both characters, and an item storage box that follows your save files. On the title screen's Load Game list it also lets you delete a save file or copy one onto another slot.

This mod is open source! Check it out at [url=https://github.com/SirCabby/RE0CabbyCodes]https://github.com/SirCabby/RE0CabbyCodes[/url]

[size=5][b]What it does[/b][/size]
[list]
[*][b]God mode[/b] - both characters held at full health and cured of poison every frame.
[*][b]One hit kills[/b] - every live enemy is held at 1 HP; your next hit kills it. Leech-men take body shots on a second pool, which is held at 1 too, so one shot collapses them.
[*][b]Infinite ammo[/b] - weapons keep their loaded count; each shot is put straight back.
[*][b]Infinite ink ribbons[/b] - typewriter saves never use up a ribbon, not even your last one, and you can save without carrying one at all.
[*][b]Save without counting[/b] - the save counter the ranking uses is not incremented.
[*][b]Save count / Play time[/b] - edit the counter and the clock; freeze the clock.
[*][b]Countdown timer[/b] - freeze or set the timer of a timed section (the train's brakes and the rest), while one is running.
[*][b]Inventory editor[/b] - every slot of Rebecca's and Billy's inventory, item and count, two-slot weapons handled.
[*][b]Item storage[/b] - the item box RE0 never had: push items out of an inventory slot into a box of any size and pull them back into a free slot. Stored items are filed automatically into five banks - key items, weapons, ammo, heals (herbs & first aid sprays), and other - each kept sorted by name, with tabs to pick the bank you want. Ammo, ink ribbons and everything else the game itself stacks are merged into full stacks (255 handgun rounds, 300 machine gun rounds, ten leech charms), so storing a few rounds tops up the stack already in the box, and taking them back tops up the stack your character already carries. The box follows your save files - saving stores it with that file, loading puts it back - and it is kept in the mod's own file, so your save is never touched.
[*][b]Save file manager[/b] - on the title screen's Load Game list: all twenty save files with their play time, save count and cleared/Wesker marks. [b]Delete[/b] empties a file (it shows NO DATA, like a file that was never used); [b]Copy to...[/b] copies a save into any other file, empty or not. Both ask first and write the game's save file at once, through the game's own save; the item storage box follows the file.
[*][b]F7[/b] hides or shows the panel while paused. Start-up defaults live in the ini.
[/list]

[size=5][b]Install[/b][/size]

Copy into your Resident Evil 0 folder (the one with re0hd.exe):
[list=1]
[*]Rename the stock [b]steam_api.dll[/b] to [b]steam_api_orig.dll[/b]
[*]Drop this mod's [b]steam_api.dll[/b] in its place
[/list]

That is the whole install, on [b]Windows and Linux/Proton alike[/b] - no launch options, no WINEDLLOVERRIDES, no ASI loader. It leaves dinput8.dll alone, so it works side by side with FusionFix and re0box.

To uninstall: delete steam_api.dll and rename steam_api_orig.dll back.

[size=5][b]Please read before using[/b][/size]
[list]
[*][b]The cheats change what the game ranks you on[/b] (clear time, saves) and what achievements check. Back up Steam/userdata/<id>/339340/remote/data0.bin before editing counters or inventories.
[*][b]Deleting or copying a save file cannot be undone[/b] - it rewrites data0.bin the moment you confirm. Back it up first if in doubt.
[*]One hit kills can make scripted boss phases skip straight to the death.
[*][b]Steam's "Verify integrity of game files" removes the mod[/b] by restoring the stock DLL. Just re-copy the file.
[/list]

[size=5][b]Config[/b][/size]

[code]
ToggleKey          = 0x76  ; virtual-key code that hides/shows the panel while paused (0x76 = F7)
GodMode            = 0     ; cheats switched on when the game starts
OneHitKills        = 0
InfiniteAmmo       = 0
InfiniteInkRibbons = 0
NoSaveCount        = 0
FreezePlaytime     = 0
FreezeCountdown    = 0     ; stop the scripted countdown timers (the train brakes, ...)
[/code]

Lives in [b]RE0CabbyCodes.ini[/b] next to re0hd.exe (created on first run). [b]RE0CabbyCodes.log[/b] beside it records what the mod found and did - attach it when reporting a problem. [b]RE0CabbyCodes.storage.ini[/b] holds the item storage, one plain-text line per save file.
