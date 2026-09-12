#pragma once

#include <cstdint>

// The event-script command table: a plaintext .rdata array of
// {const char* argsig, const char* name, void* handler} rows (313 in the
// Jan-2025 build) that maps script command names such as "PlayerMutekiSet" or
// "item_get" to the functions implementing them. It is the mod's name-based
// way to reach game code without absolute addresses.
namespace re0cc::script {

struct Command {
  const char* name = nullptr;
  const char* argsig = nullptr;  // e.g. "U4", "U2S2", "" for none
  uintptr_t handler = 0;
  uintptr_t entry = 0;           // address of the row
};

bool init();  // finds the table by an anchor row; logs its bounds
int count();
bool find(const char* name, Command* out);
uintptr_t table_begin();
uintptr_t table_end();

}  // namespace re0cc::script
