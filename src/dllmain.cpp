// RE0CabbyCodes - proxy entry point.
//
// We ship as steam_api.dll (see proxy.cpp for why). Because the exe imports
// us statically, DllMain runs before the game's own entry point - and before
// the SteamStub wrapper has decrypted the game's code. So DllMain only patches
// import-table slots (Direct3DCreate9 for the renderer, PeekMessageA for the
// main-thread tick, GetProcAddress as a fallback) and starts a thread; that
// thread waits for the first main-thread pump, checks that .text is plaintext,
// and only then reads anything of the game.

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "cheats.h"
#include "config.h"
#include "crash.h"
#include "dispatch.h"
#include "game.h"
#include "log.h"
#include "overlay.h"
#include "proxy.h"
#include "savefiles.h"
#include "storage.h"
#include "version.h"

namespace {

HMODULE g_self = nullptr;
char g_dir[MAX_PATH] = {};

// Directory this DLL was loaded from, with a trailing separator.
void resolve_own_dir() {
  char path[MAX_PATH] = {};
  GetModuleFileNameA(g_self, path, MAX_PATH);
  char* slash = std::strrchr(path, '\\');
  if (!slash) slash = std::strrchr(path, '/');
  if (slash) {
    size_t len = static_cast<size_t>(slash - path) + 1;
    if (len < sizeof(g_dir)) {
      std::memcpy(g_dir, path, len);
      g_dir[len] = '\0';
    }
  }
}

DWORD WINAPI mod_thread(LPVOID) {
  using namespace re0cc;
  logf("mod thread started (thread %lu)", GetCurrentThreadId());
  // 1. The first main-thread pump: the DRM stub has decrypted the game and
  //    handed over to it (the stub never pumps the exe's PeekMessageA slot).
  if (HANDLE ev = dispatch::first_tick_event()) {
    if (WaitForSingleObject(ev, 180000) != WAIT_OBJECT_0)
      logf("no main-thread tick after 180 s - continuing with the byte check alone");
  } else {
    Sleep(3000);
  }
  // 2. The byte check: an encrypted .text has no prologues at all.
  int prologues = 0, pads = 0;
  bool decrypted = false;
  for (int i = 0; i < 120 && !decrypted; ++i) {
    decrypted = game::decrypted(&prologues, &pads);
    if (!decrypted) Sleep(500);
  }
  logf(".text %s: %d prologues, %d int3 pads in the first 256 KB", decrypted ? "decrypted" : "STILL ENCRYPTED after 60 s",
       prologues, pads);
  dispatch::set_decrypted(decrypted);
  if (!decrypted) return 0;
  if (config::get().dump_image) game::dump_image(g_dir);
  if (config::get().disable_game) {
    logf("game hooks disabled by config");
    return 0;
  }
  for (int attempt = 1; attempt <= 10; ++attempt) {
    if (game::discover()) break;
    logf("discovery attempt %d did not complete - retrying in 2 s", attempt);
    Sleep(2000);
  }
  for (;;) {
    Sleep(2000);
    game::background_tick();
  }
  return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  using namespace re0cc;
  switch (reason) {
    case DLL_PROCESS_ATTACH: {
      g_self = module;
      DisableThreadLibraryCalls(module);
      resolve_own_dir();
      log_init(g_dir);
      logf("RE0CabbyCodes " RE0CC_VERSION " loaded (steam_api.dll proxy), dir=%s pid=%lu", g_dir, GetCurrentProcessId());

      if (!proxy::load_original(g_dir)) {
        MessageBoxA(nullptr,
                    "RE0CabbyCodes: steam_api_orig.dll is missing or broken.\n\n"
                    "The mod ships as steam_api.dll and needs the game's original Steam API "
                    "DLL beside it, renamed to steam_api_orig.dll. See INSTALL.txt / README.md, "
                    "or verify the game files in Steam and reinstall the mod.",
                    "RE0CabbyCodes", MB_OK | MB_ICONERROR);
        return FALSE;
      }

      install_crash_logger();
      config::load(g_dir);
      cheats::init();
      storage::init(g_dir);
      savefiles::init();

      // Import-table patches only - no LoadLibrary and no game reads under the loader lock.
      if (!config::get().disable_overlay) overlay::install();
      if (!config::get().disable_dispatch) dispatch::install();

      if (HANDLE t = CreateThread(nullptr, 0, mod_thread, nullptr, 0, nullptr)) CloseHandle(t);
      break;
    }
    case DLL_PROCESS_DETACH:
      // Undo every patch before this image goes away: anything still pointing
      // into the DLL would fault the moment the game touched it during its own
      // teardown.
      logf("unloading - removing hooks");
      dispatch::uninstall();
      cheats::remove_hooks();
      overlay::uninstall();
      logf("hooks removed cleanly");
      log_shutdown();
      break;
    default:
      break;
  }
  return TRUE;
}
