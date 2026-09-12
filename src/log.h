#pragma once

// Minimal file logger. The mod runs inside a 2016 game under Proton, where a
// debugger is awkward and stdout goes nowhere, so a log file beside the DLL is
// the primary diagnostic channel.
namespace re0cc {

// Called once from DllMain with the directory the DLL was loaded from.
void log_init(const char* dir);
void log_shutdown();

void logf(const char* fmt, ...);

}  // namespace re0cc
