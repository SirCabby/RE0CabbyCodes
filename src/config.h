#pragma once

namespace re0cc::config {

struct Settings {
  int toggle_key = 0x76;      // VK_F7: hide/show the panel while the pause menu is up
  // Initial state of each cheat when the game starts; the panel changes them
  // for the session.
  bool god_mode = false;
  bool one_hit_kills = false;
  bool infinite_ammo = false;
  bool infinite_ink = false;
  bool no_save_count = false;
  bool freeze_playtime = false;
  bool freeze_countdown = false;
  bool trace = false;         // verbose diagnostics
  bool always_show = false;   // debug: draw the panel outside the pause menu too
  bool dump_image = false;    // write the decrypted exe image beside the DLL once
  int dti_slot = -1;          // override for the derived getDTI vtable slot (-1 = derive)
  char dump_objects[256] = {};  // "sGamePause:0x100,sPlayer:0x200" - log changing dwords
  // "Disable = overlay,dispatch,game,gpa,input" turns whole subsystems off so a
  // fault can be bisected to one of them.
  bool disable_overlay = false;
  bool disable_dispatch = false;
  bool disable_game = false;
  bool disable_gpa = false;    // the GetProcAddress import hook (the DRM stub calls through it)
  bool disable_input = false;  // the GetAsyncKeyState guard while the panel captures input
};

const Settings& get();

// RE0CabbyCodes.ini next to the DLL. Written with documented defaults the
// first time so the options are discoverable.
void load(const char* dir);
const char* dir();  // the DLL's directory, with a trailing separator

}  // namespace re0cc::config
