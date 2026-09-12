#pragma once

namespace re0cc::proxy {

// Load steam_api_orig.dll from `dir` (absolute path), pin it, and resolve every
// export the proxy forwards into the jump table the thunks use. False means
// the game cannot run: the caller should tell the user and refuse to start.
bool load_original(const char* dir);

}  // namespace re0cc::proxy
