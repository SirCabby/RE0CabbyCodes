#pragma once

#include "game.h"

// The cheats. Their switches are shared between the render thread (the panel)
// and the game's main thread (dispatch.cpp, which applies them), so the
// switches are plain atomics, edits travel through a small request queue, and
// everything that touches the game runs from tick() on the main thread.
namespace re0cc::cheats {

enum Kind : int {
  kGodMode = 0,
  kOneHitKills,
  kInfiniteAmmo,
  kInfiniteInk,
  kNoSaveCount,
  kFreezePlaytime,
  kFreezeCountdown,
  kCount
};

void init();  // from DllMain: the request queue's lock
const char* name(Kind k);
bool enabled(Kind k);
void set_enabled(Kind k, bool on);  // any thread; takes effect on the next tick

// What the panel shows (written by tick, read anywhere).
struct Status {
  int nchars = 0;
  game::Character chars[2];
  bool bags_ok = false;
  game::Bag bags[2];
  bool status_ok = false;
  char status_class[32] = {};
  int save_count = -1;
  float playtime_raw = -1.0f;
  float playtime_rate = 0.0f;
  bool site_playtime = false;    // the play-time store patch site is known
  bool site_savecount = false;   // the save-count increment patch site is known
  bool site_ink = false;         // both ink-ribbon take sites are known: a save never takes one
  bool site_ink_check = false;   // the typewriter's ribbon check is known: a save needs none
  bool ink_patched = false;      // both takes skipped right now
  bool ink_check_patched = false;
  bool playtime_patched = false;
  bool playtime_hold = false;    // freeze is running as a hold (no patch site)
  bool savecount_patched = false;
  bool countdown_ready = false;    // sEventScript was found
  bool countdown_active = false;   // a scripted countdown is running right now
  float countdown_raw = -1.0f;
  float countdown_rate = 0.0f;
  bool site_countdown = false;     // both countdown step patch sites are known
  bool countdown_patched = false;
  bool countdown_hold = false;     // freeze is running as a hold (no patch sites)
  int enemies = 0;               // live enemy units seen this tick
  bool enemy_table = false;      // the unit table layout is known
  int ammo_held = 0;             // weapon slots being held
  int ink_restored = 0;          // ribbons put back this session
  bool ink_tracked = false;
  char last_action[120] = {};
};
Status status();

// Requests from the panel (applied on the next tick, on the main thread).
void request_save_count(int v);
void request_playtime_seconds(int seconds);
void request_countdown_seconds(float seconds);
void request_slot(int bag, int slot, int id, int count);  // slot 0..5, 6 = personal item
void request_equipped(int bag, int index);

// Main thread only.
void tick(bool in_game);
void remove_hooks();  // teardown: revert every patch

}  // namespace re0cc::cheats
