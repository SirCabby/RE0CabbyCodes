#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

#include "mem.h"

// The game executable's sections, resolved once. Everything that scans the
// image (the script table, the DTI walker, the signature finder) goes through
// here so section bounds are consistent and cheap to reuse.
namespace re0cc::image {

void init();
HMODULE module();
uintptr_t base();
const mem::Range& text();
const mem::Range& rdata();
const mem::Range& data();   // includes .bss (VirtualSize > SizeOfRawData)
const std::vector<mem::Range>& data_sections();  // .rdata, .data, .rsrc... (initialised, non-executable)
uint32_t rva(uintptr_t va);  // 0 when outside the image

}  // namespace re0cc::image
