#pragma once

#include <windows.h>

// The main-thread tick. Everything that talks to the engine runs on the thread
// that pumps window messages (the game updates on that thread): an import-table
// hook on PeekMessageA gives a callback there once per pump, and the cheats are
// applied from it. Its first call is also the mod's "the SteamStub wrapper has
// decrypted the game and handed over" marker.
namespace re0cc::dispatch {

bool install();  // from DllMain (import-table patch only); records the primary thread
void uninstall();

struct Snapshot {
  bool decrypted = false;   // the mod thread confirmed .text is plaintext
  bool game_ready = false;  // discovery finished
  bool in_game = false;     // player units exist
  bool paused = false;      // the game's pause menu is up
  bool file_screen = false; // the title screen's load list is up (the panel manages the save files)
  bool show_panel = false;  // the visibility rule, evaluated on the main thread
  unsigned long main_thread = 0;
  unsigned long long ticks = 0;
};
Snapshot snapshot();

bool first_tick_seen();
HANDLE first_tick_event();     // signalled once, from the first main-thread pump
void set_decrypted(bool on);   // the mod thread's gate result

}  // namespace re0cc::dispatch
