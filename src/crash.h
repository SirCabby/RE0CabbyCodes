#pragma once

namespace re0cc {

// Install a last-resort exception filter that records where a fatal fault
// happened, and in which module. Injected mods are the usual suspect for
// crashes at shutdown, so it needs to be possible to tell from the log whether
// the fault is in this DLL or somewhere else entirely.
void install_crash_logger();

// "module+0xRVA" for an address, for log lines.
void describe_address(void* addr, char* out, unsigned n);

}  // namespace re0cc
