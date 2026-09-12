#include "crash.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "game.h"
#include "log.h"
#include "mem.h"

namespace re0cc {
namespace {

LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;
using SetFilterFn = LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI*)(LPTOP_LEVEL_EXCEPTION_FILTER);
SetFilterFn g_orig_set_filter = nullptr;

const char* code_name(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
    default: return "exception";
  }
}

void describe(void* addr, char* out, size_t n) {
  HMODULE owner = nullptr;
  char path[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCSTR>(addr), &owner) &&
      owner) {
    GetModuleFileNameA(owner, path, MAX_PATH);
    const char* leaf = std::strrchr(path, '\\');
    std::snprintf(out, n, "%s+0x%X", leaf ? leaf + 1 : path,
                  static_cast<unsigned>(reinterpret_cast<uintptr_t>(addr) -
                                        reinterpret_cast<uintptr_t>(owner)));
  } else {
    std::snprintf(out, n, "%p (no module)", addr);
  }
}

LONG WINAPI on_exception(EXCEPTION_POINTERS* info) {
  if (info && info->ExceptionRecord) {
    void* at = info->ExceptionRecord->ExceptionAddress;
    char where[MAX_PATH + 32];
    describe(at, where, sizeof(where));
    logf("CRASH: %s (0x%08lX) at %s (thread %lu)", code_name(info->ExceptionRecord->ExceptionCode),
         info->ExceptionRecord->ExceptionCode, where, GetCurrentThreadId());
    // For an access violation the record carries what was touched and how; a
    // null or freed address there is the difference between a bad pointer and
    // a bad instruction, and it is not in the register dump below.
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        info->ExceptionRecord->NumberParameters >= 2) {
      const ULONG_PTR kind = info->ExceptionRecord->ExceptionInformation[0];
      logf("       %s address %08lX", kind == 1 ? "writing" : kind == 8 ? "executing" : "reading",
           static_cast<unsigned long>(info->ExceptionRecord->ExceptionInformation[1]));
    }
    if (info->ContextRecord) {
      logf("       eip=%08lX esp=%08lX ebp=%08lX eax=%08lX ecx=%08lX edx=%08lX",
           info->ContextRecord->Eip, info->ContextRecord->Esp, info->ContextRecord->Ebp,
           info->ContextRecord->Eax, info->ContextRecord->Ecx, info->ContextRecord->Edx);
      // A few return addresses off the stack, for the ones that land in a module.
      auto* sp = reinterpret_cast<void**>(info->ContextRecord->Esp);
      for (int i = 0, shown = 0; i < 128 && shown < 8; ++i) {
        if (!mem::readable(sp + i, sizeof(void*))) break;
        void* v = sp[i];
        HMODULE owner = nullptr;
        if (v && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    static_cast<LPCSTR>(v), &owner) &&
            owner) {
          describe(v, where, sizeof(where));
          logf("       stack[%03d] %s", i, where);
          ++shown;
        }
      }
    }
  }
  return g_previous ? g_previous(info) : EXCEPTION_CONTINUE_SEARCH;
}

// SetUnhandledExceptionFilter has one global slot, and the game takes it twice
// over: its CRT installs its own filter during start-up, after our DllMain has
// run, and the CRT's fatal-error path (exe+0x853CD1) then sets the filter to
// null before calling UnhandledExceptionFilter, on purpose, so that nothing
// intercepts the report. Either way ours would never run and a crash would
// leave no line in the log. Taking the exe's import instead lets both calls do
// what they meant to - the value they pass becomes the filter we chain to, and
// the value they get back is the one they set before - while ours stays on top.
LPTOP_LEVEL_EXCEPTION_FILTER WINAPI hk_set_filter(LPTOP_LEVEL_EXCEPTION_FILTER next) {
  LPTOP_LEVEL_EXCEPTION_FILTER previous = g_previous;
  g_previous = next;
  if (g_orig_set_filter) g_orig_set_filter(&on_exception);
  logf("crash: the game set its top-level exception filter to %p; ours stays on top (chaining to it)", next);
  return previous;
}

}  // namespace

void describe_address(void* addr, char* out, unsigned n) { describe(addr, out, n); }

void install_crash_logger() {
  g_previous = SetUnhandledExceptionFilter(&on_exception);
  HMODULE exe = GetModuleHandleA(nullptr);
  void* prev = mem::iat_hook(exe, "KERNEL32.dll", "SetUnhandledExceptionFilter", reinterpret_cast<void*>(&hk_set_filter));
  if (prev) {
    g_orig_set_filter = reinterpret_cast<SetFilterFn>(prev);
    game::note_iat_slot(mem::iat_slot(exe, "KERNEL32.dll", "SetUnhandledExceptionFilter"),
                        reinterpret_cast<uintptr_t>(prev));
    logf("crash logger installed (SetUnhandledExceptionFilter import hooked, original %p)", prev);
  } else {
    logf("crash logger installed, but SetUnhandledExceptionFilter is not in the exe's import table - "
         "the game's own filter will replace ours and a crash may go unlogged");
  }
}

}  // namespace re0cc
