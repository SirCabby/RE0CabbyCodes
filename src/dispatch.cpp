#include "dispatch.h"

#include "cheats.h"
#include "config.h"
#include "game.h"
#include "log.h"
#include "mem.h"
#include "savefiles.h"

namespace re0cc::dispatch {
namespace {

using PeekMessageAFn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
PeekMessageAFn g_orig_peek = nullptr;
DWORD g_main_thread = 0;  // the process's primary thread: DllMain(PROCESS_ATTACH) runs on it
uintptr_t g_slot = 0;

Snapshot g_snap;
DWORD g_last_tick = 0;
DWORD g_last_watchdog = 0;
volatile LONG g_first_tick = 0;
volatile LONG g_decrypted = 0;
HANDLE g_first_event = nullptr;
bool g_in_game_prev = false;
bool g_paused_prev = false;

BOOL WINAPI hk_peek_message(LPMSG msg, HWND hwnd, UINT min, UINT max, UINT remove);

void tick() {
  const DWORD now = GetTickCount();
  if (now - g_last_tick < 16) return;  // the pump spins many times per frame
  g_last_tick = now;

  if (!g_first_tick) {
    InterlockedExchange(&g_first_tick, 1);
    if (g_first_event) SetEvent(g_first_event);
    logf("main-thread tick running on thread %lu", GetCurrentThreadId());
  }
  // The DRM stub only reads the import table, but a watchdog costs nothing and
  // makes a re-snapped slot visible in the log (and puts the hook back).
  if (g_slot && now - g_last_watchdog > 1000) {
    g_last_watchdog = now;
    const uintptr_t cur = mem::read<uintptr_t>(g_slot);
    if (cur != reinterpret_cast<uintptr_t>(&hk_peek_message)) {
      logf("dispatch: PeekMessageA import slot was changed to %p - re-hooking", reinterpret_cast<void*>(cur));
      g_orig_peek = reinterpret_cast<PeekMessageAFn>(cur);
      mem::write<uintptr_t>(g_slot, reinterpret_cast<uintptr_t>(&hk_peek_message));
    }
  }

  Snapshot s;
  s.main_thread = GetCurrentThreadId();
  s.ticks = g_snap.ticks + 1;
  s.decrypted = g_decrypted != 0;
  s.game_ready = game::ready();
  if (!s.game_ready) {
    g_snap = s;
    return;
  }
  s.in_game = game::in_game();
  s.paused = s.in_game && game::pause_menu_showing();
  if (s.in_game != g_in_game_prev) {
    logf("game session %s", s.in_game ? "started" : "ended");
    g_in_game_prev = s.in_game;
  }
  if (s.paused != g_paused_prev) {
    if (config::get().trace) logf("pause menu %s", s.paused ? "opened" : "closed");
    g_paused_prev = s.paused;
  }
  // The cheats run every tick, paused or not: holds must be re-asserted every
  // frame, and a switch flipped in the panel takes effect the moment the game
  // resumes (or immediately, for the ones that only write fields).
  cheats::tick(s.in_game);
  // The load list is the title screen's, so it has its own rule: the panel is
  // up while the list waits for the player to pick a file.
  savefiles::tick();
  s.file_screen = !s.in_game && savefiles::showing();
  game::trace_tick();
  s.show_panel = (s.paused && s.in_game) || s.file_screen;
  g_snap = s;
}

BOOL WINAPI hk_peek_message(LPMSG msg, HWND hwnd, UINT min, UINT max, UINT remove) {
  // Other threads pump messages too (the Steam overlay, worker windows); the
  // game runs on the primary thread, and so must everything that calls into it.
  if (GetCurrentThreadId() == g_main_thread) tick();
  return g_orig_peek(msg, hwnd, min, max, remove);
}

}  // namespace

bool install() {
  g_main_thread = GetCurrentThreadId();
  g_first_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  HMODULE exe = GetModuleHandleA(nullptr);
  void* prev = mem::iat_hook(exe, "USER32.dll", "PeekMessageA", reinterpret_cast<void*>(&hk_peek_message));
  if (!prev) {
    logf("ERROR: could not hook PeekMessageA in the exe's import table - no main-thread tick");
    return false;
  }
  g_orig_peek = reinterpret_cast<PeekMessageAFn>(prev);
  g_slot = mem::iat_slot(exe, "USER32.dll", "PeekMessageA");
  game::note_iat_slot(g_slot, reinterpret_cast<uintptr_t>(prev));
  logf("dispatch: PeekMessageA import hooked (original %p, slot exe+0x%X)", prev,
       static_cast<unsigned>(g_slot - reinterpret_cast<uintptr_t>(exe)));
  return true;
}

void uninstall() {
  if (!g_orig_peek) return;
  mem::iat_hook(GetModuleHandleA(nullptr), "USER32.dll", "PeekMessageA", reinterpret_cast<void*>(g_orig_peek));
  g_orig_peek = nullptr;
}

Snapshot snapshot() { return g_snap; }
bool first_tick_seen() { return g_first_tick != 0; }
HANDLE first_tick_event() { return g_first_event; }
void set_decrypted(bool on) { InterlockedExchange(&g_decrypted, on ? 1 : 0); }

}  // namespace re0cc::dispatch
