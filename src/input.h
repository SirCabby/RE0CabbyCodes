#pragma once

#include <windows.h>

// The input guard. The game does not read the mouse or the keyboard from the
// window message queue: it opens DirectInput 8 devices (GUID_SysMouse and
// GUID_SysKeyboard) and also polls GetAsyncKeyState. Swallowing WM_LBUTTONDOWN
// in the window procedure therefore does nothing - a click on the panel still
// reached the pause menu underneath. This hides the input the panel is using
// from those two paths instead:
//
//   DirectInput8Create (exe import) -> IDirectInput8::CreateDevice (slot 3)
//   -> IDirectInputDevice8::GetDeviceState (9) / GetDeviceData (10)
//
// While the pointer is over the panel the mouse device reads as "nothing
// happened"; while a panel field has keyboard focus the keyboard does too
// (except Escape and Alt, which stay the game's). `Disable = input` in the ini
// turns the whole guard off.
namespace re0cc::input {

bool install(HMODULE exe);                      // from DllMain (import-table patch only)
FARPROC wrap(const char* name, FARPROC real);   // GetProcAddress fallback
void uninstall();

}  // namespace re0cc::input
