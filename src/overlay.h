#pragma once

#include <windows.h>

// The Dear ImGui panel and the machinery that gets it on screen. The renderer
// is captured through the exe's Direct3DCreate9 import (the game links d3d9
// statically); the GetProcAddress import is hooked too, as a fallback for a
// dynamically resolved D3D9 and as a trace of what the DRM stub resolves.
namespace re0cc::overlay {

bool install();  // from DllMain (import-table patches only)
void uninstall();

bool ensure_context(HWND hwnd);   // ImGui context + Win32 backend + WndProc, once
bool wants_draw();                // the visibility rule for this frame
void draw_panel();                // between ImGui::NewFrame() and ImGui::Render()
void shutdown_imgui();            // Win32 backend + context (renderer already gone)

// One ImGui context, two threads: the window procedure feeds it messages on
// the game's main thread while the D3D9 present hook draws on the render
// thread. ImGui is not thread-safe - its event queue is a plain vector - so
// every touch of the context is taken under this lock. Without it a frame that
// did not draw freed that queue (ImVector::clear releases the storage and
// nulls the pointer) from under a WM_MOUSEMOVE that was half-way through
// reading it: AddMousePosEvent had already loaded size 1 and then loaded a
// null data pointer, and dereferenced address 0.
void lock_imgui();
void unlock_imgui();
struct ImGuiLock {
  ImGuiLock() { lock_imgui(); }
  ~ImGuiLock() { unlock_imgui(); }
  ImGuiLock(const ImGuiLock&) = delete;
  ImGuiLock& operator=(const ImGuiLock&) = delete;
};

// What the panel is using this frame, for the input guard (input.cpp): the
// game reads the mouse and keyboard straight from DirectInput and
// GetAsyncKeyState, so it has to be told to ignore what the panel is taking.
bool capturing_mouse();     // the pointer is over the panel
bool capturing_keyboard();  // a panel field has keyboard focus

namespace dx9 {
bool install_import(HMODULE exe);            // hook d3d9.dll!Direct3DCreate9 in the exe's IAT
FARPROC wrap(const char* name, FARPROC real);  // GetProcAddress fallback
void uninstall();
}  // namespace dx9

}  // namespace re0cc::overlay
