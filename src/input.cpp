#define DIRECTINPUT_VERSION 0x0800

#include "input.h"

#include <dinput.h>

#include <cstring>

#include "config.h"
#include "log.h"
#include "mem.h"
#include "overlay.h"

namespace re0cc::input {
namespace {

// The two device GUIDs, spelled out so the build needs no dxguid import
// library: {6F1D2B60-D5A0-11CF-BFC7-444553540000} mouse, ...61 keyboard.
constexpr GUID kSysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr GUID kSysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

using Create8Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using CreateDeviceFn = HRESULT(__stdcall*)(IDirectInput8A*, REFGUID, LPDIRECTINPUTDEVICE8A*, LPUNKNOWN);
using GetStateFn = HRESULT(__stdcall*)(IDirectInputDevice8A*, DWORD, LPVOID);
using GetDataFn = HRESULT(__stdcall*)(IDirectInputDevice8A*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
using GetAsyncKeyStateFn = SHORT(WINAPI*)(int);

Create8Fn g_real_create8 = nullptr;
CreateDeviceFn g_orig_create_device = nullptr;
GetStateFn g_orig_get_state = nullptr;
GetDataFn g_orig_get_data = nullptr;
GetAsyncKeyStateFn g_orig_gaks = nullptr;
uintptr_t g_di_vt = 0;      // IDirectInput8 vtable (CreateDevice patched)
uintptr_t g_dev_vt = 0;     // IDirectInputDevice8 vtable (GetDeviceState/Data patched)
uintptr_t g_iat_slot = 0;
uintptr_t g_gaks_slot = 0;

// Every DirectInput device the game opens, so the shared vtable hook can tell
// the mouse from the keyboard from a wheel by the object it was called on.
enum Kind { kOther = 0, kMouse, kKeyboard };
struct Device {
  void* obj;
  Kind kind;
};
constexpr int kMaxDevices = 32;
Device g_devices[kMaxDevices];
volatile LONG g_ndevices = 0;

Kind kind_of(void* obj) {
  const LONG n = g_ndevices;
  for (LONG i = 0; i < n && i < kMaxDevices; ++i)
    if (g_devices[i].obj == obj) return g_devices[i].kind;
  return kOther;
}

// A released device's address can come back as a new one, so an address that is
// already known is updated rather than appended.
void remember(void* obj, Kind kind) {
  const LONG n = g_ndevices;
  for (LONG i = 0; i < n && i < kMaxDevices; ++i)
    if (g_devices[i].obj == obj) {
      g_devices[i].kind = kind;
      return;
    }
  const LONG idx = InterlockedIncrement(&g_ndevices) - 1;
  if (idx >= kMaxDevices) return;
  g_devices[idx].obj = obj;
  g_devices[idx].kind = kind;
}

HRESULT __stdcall hk_get_state(IDirectInputDevice8A* self, DWORD size, LPVOID data) {
  const HRESULT hr = g_orig_get_state(self, size, data);
  if (FAILED(hr) || !data || !size) return hr;
  const Kind kind = kind_of(self);
  if (kind == kMouse && overlay::capturing_mouse()) {
    std::memset(data, 0, size);  // no movement, no buttons while the panel has the pointer
  } else if (kind == kKeyboard && overlay::capturing_keyboard() && size >= 256) {
    // Escape and Alt stay the game's: they are how the pause menu is left.
    auto* keys = static_cast<unsigned char*>(data);
    const unsigned char esc = keys[DIK_ESCAPE], lalt = keys[DIK_LMENU], ralt = keys[DIK_RMENU];
    std::memset(data, 0, size);
    keys[DIK_ESCAPE] = esc;
    keys[DIK_LMENU] = lalt;
    keys[DIK_RMENU] = ralt;
  }
  return hr;
}

HRESULT __stdcall hk_get_data(IDirectInputDevice8A* self, DWORD obj_size, LPDIDEVICEOBJECTDATA data, LPDWORD inout,
                              DWORD flags) {
  const HRESULT hr = g_orig_get_data(self, obj_size, data, inout, flags);
  if (FAILED(hr) || !inout) return hr;
  const Kind kind = kind_of(self);
  // The buffer is still drained (the call went through), the game just does not
  // get told about it.
  if ((kind == kMouse && overlay::capturing_mouse()) || (kind == kKeyboard && overlay::capturing_keyboard())) *inout = 0;
  return hr;
}

HRESULT __stdcall hk_create_device(IDirectInput8A* self, REFGUID guid, LPDIRECTINPUTDEVICE8A* out, LPUNKNOWN outer) {
  const HRESULT hr = g_orig_create_device(self, guid, out, outer);
  if (FAILED(hr) || !out || !*out) return hr;
  const Kind kind = !std::memcmp(&guid, &kSysMouse, sizeof(GUID))      ? kMouse
                    : !std::memcmp(&guid, &kSysKeyboard, sizeof(GUID)) ? kKeyboard
                                                                       : kOther;
  remember(*out, kind);
  const uintptr_t vt = mem::read<uintptr_t>(reinterpret_cast<uintptr_t>(*out));
  if (!g_dev_vt && vt) {
    // Both devices come from the same class, so one pair of hooks covers them.
    g_orig_get_state = reinterpret_cast<GetStateFn>(mem::hook_vtable(vt, 9, reinterpret_cast<void*>(&hk_get_state)));
    g_orig_get_data = reinterpret_cast<GetDataFn>(mem::hook_vtable(vt, 10, reinterpret_cast<void*>(&hk_get_data)));
    g_dev_vt = vt;
    logf("input: DirectInput device vtable %p hooked (GetDeviceState/GetDeviceData)", reinterpret_cast<void*>(vt));
  } else if (vt && vt != g_dev_vt) {
    logf("input: device %p has a different vtable %p - its input is not guarded", static_cast<void*>(*out),
         reinterpret_cast<void*>(vt));
  }
  logf("input: game opened DirectInput device %p (%s)", static_cast<void*>(*out),
       kind == kMouse ? "mouse" : kind == kKeyboard ? "keyboard" : "other");
  return hr;
}

HRESULT WINAPI hk_create8(HINSTANCE inst, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
  const HRESULT hr = g_real_create8(inst, version, riid, out, outer);
  if (FAILED(hr) || !out || !*out || g_orig_create_device) return hr;
  const uintptr_t vt = mem::read<uintptr_t>(reinterpret_cast<uintptr_t>(*out));
  if (!vt) return hr;
  g_orig_create_device =
      reinterpret_cast<CreateDeviceFn>(mem::hook_vtable(vt, 3, reinterpret_cast<void*>(&hk_create_device)));
  g_di_vt = vt;
  logf("input: DirectInput8Create intercepted (version 0x%X, thread %lu); hooked CreateDevice", version,
       GetCurrentThreadId());
  return hr;
}

SHORT WINAPI hk_get_async_key_state(int vk) {
  switch (vk) {
    case VK_LBUTTON:
    case VK_RBUTTON:
    case VK_MBUTTON:
    case VK_XBUTTON1:
    case VK_XBUTTON2:
      if (overlay::capturing_mouse()) return 0;
      break;
    case VK_ESCAPE:
    case VK_MENU:
      break;  // always the game's
    default:
      if (overlay::capturing_keyboard()) return 0;
      break;
  }
  return g_orig_gaks(vk);
}

}  // namespace

bool install(HMODULE exe) {
  bool ok = false;
  if (void* prev = mem::iat_hook(exe, "DINPUT8.dll", "DirectInput8Create", reinterpret_cast<void*>(&hk_create8))) {
    g_real_create8 = reinterpret_cast<Create8Fn>(prev);
    g_iat_slot = mem::iat_slot(exe, "DINPUT8.dll", "DirectInput8Create");
    logf("input: DirectInput8Create import hooked (original %p)", prev);
    ok = true;
  } else {
    logf("input: DirectInput8Create is not in the exe's import table - mouse clicks may reach the game through it");
  }
  if (void* prev = mem::iat_hook(exe, "USER32.dll", "GetAsyncKeyState", reinterpret_cast<void*>(&hk_get_async_key_state))) {
    g_orig_gaks = reinterpret_cast<GetAsyncKeyStateFn>(prev);
    g_gaks_slot = mem::iat_slot(exe, "USER32.dll", "GetAsyncKeyState");
    logf("input: GetAsyncKeyState import hooked (original %p)", prev);
    ok = true;
  } else {
    logf("input: GetAsyncKeyState is not in the exe's import table");
  }
  return ok;
}

FARPROC wrap(const char* name, FARPROC real) {
  if (!std::strcmp(name, "DirectInput8Create") && !g_real_create8) {
    g_real_create8 = reinterpret_cast<Create8Fn>(real);
    logf("input: game resolved DirectInput8Create dynamically - wrapping it");
    return reinterpret_cast<FARPROC>(&hk_create8);
  }
  if (!std::strcmp(name, "GetAsyncKeyState") && !g_orig_gaks) {
    g_orig_gaks = reinterpret_cast<GetAsyncKeyStateFn>(real);
    logf("input: game resolved GetAsyncKeyState dynamically - wrapping it");
    return reinterpret_cast<FARPROC>(&hk_get_async_key_state);
  }
  return nullptr;
}

void uninstall() {
  if (g_dev_vt && mem::readable(reinterpret_cast<void*>(g_dev_vt), 16 * sizeof(void*))) {
    if (g_orig_get_state) mem::write<void*>(g_dev_vt + 9 * sizeof(void*), reinterpret_cast<void*>(g_orig_get_state));
    if (g_orig_get_data) mem::write<void*>(g_dev_vt + 10 * sizeof(void*), reinterpret_cast<void*>(g_orig_get_data));
  }
  if (g_di_vt && g_orig_create_device && mem::readable(reinterpret_cast<void*>(g_di_vt), 4 * sizeof(void*)))
    mem::write<void*>(g_di_vt + 3 * sizeof(void*), reinterpret_cast<void*>(g_orig_create_device));
  if (g_iat_slot && g_real_create8 && mem::read<uintptr_t>(g_iat_slot) == reinterpret_cast<uintptr_t>(&hk_create8))
    mem::write<uintptr_t>(g_iat_slot, reinterpret_cast<uintptr_t>(g_real_create8));
  if (g_gaks_slot && g_orig_gaks && mem::read<uintptr_t>(g_gaks_slot) == reinterpret_cast<uintptr_t>(&hk_get_async_key_state))
    mem::write<uintptr_t>(g_gaks_slot, reinterpret_cast<uintptr_t>(g_orig_gaks));
  g_orig_get_state = nullptr;
  g_orig_get_data = nullptr;
  g_orig_create_device = nullptr;
  g_dev_vt = g_di_vt = 0;
  logf("input hooks removed");
}

}  // namespace re0cc::input
