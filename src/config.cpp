#include "config.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "log.h"

namespace re0cc::config {
namespace {

Settings g_settings;
char g_path[MAX_PATH] = {};
char g_dir[MAX_PATH] = {};

bool truthy(const char* v) {
  return !_stricmp(v, "1") || !_stricmp(v, "on") || !_stricmp(v, "true") || !_stricmp(v, "yes");
}

void write_defaults() {
  FILE* f = std::fopen(g_path, "w");
  if (!f) return;
  std::fprintf(f,
               "# RE0CabbyCodes\n"
               "#\n"
               "#   ToggleKey          = 0x76  ; virtual-key code that hides/shows the panel while paused (0x76 = F7)\n"
               "#   GodMode            = 0     ; cheats switched on when the game starts (the panel changes\n"
               "#   OneHitKills        = 0     ; them for the session)\n"
               "#   InfiniteAmmo       = 0\n"
               "#   InfiniteInkRibbons = 0\n"
               "#   NoSaveCount        = 0     ; save without incrementing the save counter\n"
               "#   FreezePlaytime     = 0\n"
               "#   FreezeCountdown    = 0     ; stop the scripted countdown timers (the train brakes, ...)\n"
               "#   AlwaysShow         = 0     ; debug: draw the panel outside the pause menu too\n"
               "#   Trace              = 0     ; verbose diagnostics in RE0CabbyCodes.log\n"
               "#   DumpImage          = 0     ; write the decrypted re0hd image beside the DLL once (re0hd.dumped.exe)\n"
               "#   DumpObject         =       ; e.g. sGamePause:0x100,sPlayer:0x200 - log dwords of those objects as they change\n"
               "#   DtiSlot            = -1    ; override the derived getDTI vtable slot (-1 = derive it)\n"
               "#   Disable            =       ; comma list of subsystems to turn off: overlay,dispatch,game,gpa,input\n"
               "ToggleKey = 0x76\n"
               "GodMode = 0\n"
               "OneHitKills = 0\n"
               "InfiniteAmmo = 0\n"
               "InfiniteInkRibbons = 0\n"
               "NoSaveCount = 0\n"
               "FreezePlaytime = 0\n"
               "FreezeCountdown = 0\n"
               "AlwaysShow = 0\n"
               "Trace = 0\n"
               "DumpImage = 0\n"
               "DumpObject =\n"
               "DtiSlot = -1\n"
               "Disable =\n");
  std::fclose(f);
}

}  // namespace

const Settings& get() { return g_settings; }
const char* dir() { return g_dir; }

void load(const char* dir) {
  std::snprintf(g_dir, sizeof(g_dir), "%s", dir);
  std::snprintf(g_path, sizeof(g_path), "%sRE0CabbyCodes.ini", dir);
  FILE* f = std::fopen(g_path, "r");
  if (!f) {
    write_defaults();
    logf("config: no %s - wrote one with the defaults", g_path);
    return;
  }
  char line[512];
  while (std::fgets(line, sizeof(line), f)) {
    char* p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '#' || *p == ';' || *p == '[' || *p == '\n' || *p == '\r' || !*p) continue;
    char* eq = std::strchr(p, '=');
    if (!eq) continue;
    *eq = '\0';
    char* key = p;
    char* val = eq + 1;
    for (char* e = key + std::strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t');) *--e = '\0';
    while (*val == ' ' || *val == '\t') ++val;
    for (char* e = val + std::strlen(val);
         e > val && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t');)
      *--e = '\0';

    if (!_stricmp(key, "ToggleKey")) {
      int v = static_cast<int>(std::strtol(val, nullptr, 0));
      if (v > 0 && v < 256) g_settings.toggle_key = v;
    } else if (!_stricmp(key, "GodMode")) {
      g_settings.god_mode = truthy(val);
    } else if (!_stricmp(key, "OneHitKills")) {
      g_settings.one_hit_kills = truthy(val);
    } else if (!_stricmp(key, "InfiniteAmmo")) {
      g_settings.infinite_ammo = truthy(val);
    } else if (!_stricmp(key, "InfiniteInkRibbons")) {
      g_settings.infinite_ink = truthy(val);
    } else if (!_stricmp(key, "NoSaveCount")) {
      g_settings.no_save_count = truthy(val);
    } else if (!_stricmp(key, "FreezePlaytime")) {
      g_settings.freeze_playtime = truthy(val);
    } else if (!_stricmp(key, "FreezeCountdown")) {
      g_settings.freeze_countdown = truthy(val);
    } else if (!_stricmp(key, "AlwaysShow")) {
      g_settings.always_show = truthy(val);
    } else if (!_stricmp(key, "Trace")) {
      g_settings.trace = truthy(val);
    } else if (!_stricmp(key, "DumpImage")) {
      g_settings.dump_image = truthy(val);
    } else if (!_stricmp(key, "DumpObject")) {
      std::snprintf(g_settings.dump_objects, sizeof(g_settings.dump_objects), "%s", val);
    } else if (!_stricmp(key, "DtiSlot")) {
      int v = static_cast<int>(std::strtol(val, nullptr, 0));
      g_settings.dti_slot = (v >= 0 && v < 16) ? v : -1;
    } else if (!_stricmp(key, "Disable")) {
      g_settings.disable_overlay = std::strstr(val, "overlay") != nullptr;
      g_settings.disable_dispatch = std::strstr(val, "dispatch") != nullptr;
      g_settings.disable_game = std::strstr(val, "game") != nullptr;
      g_settings.disable_gpa = std::strstr(val, "gpa") != nullptr;
      g_settings.disable_input = std::strstr(val, "input") != nullptr;
    } else {
      logf("config: unknown key '%s' ignored", key);
    }
  }
  std::fclose(f);
  logf("config: ToggleKey=0x%02X GodMode=%d OneHitKills=%d InfiniteAmmo=%d InfiniteInkRibbons=%d NoSaveCount=%d "
       "FreezePlaytime=%d FreezeCountdown=%d AlwaysShow=%d Trace=%d DumpImage=%d DumpObject='%s' DtiSlot=%d Disable=%s%s%s%s%s",
       g_settings.toggle_key, g_settings.god_mode, g_settings.one_hit_kills, g_settings.infinite_ammo,
       g_settings.infinite_ink, g_settings.no_save_count, g_settings.freeze_playtime, g_settings.freeze_countdown,
       g_settings.always_show, g_settings.trace, g_settings.dump_image, g_settings.dump_objects, g_settings.dti_slot,
       g_settings.disable_overlay ? "overlay " : "", g_settings.disable_dispatch ? "dispatch " : "",
       g_settings.disable_game ? "game " : "", g_settings.disable_gpa ? "gpa " : "",
       g_settings.disable_input ? "input" : "");
}

}  // namespace re0cc::config
