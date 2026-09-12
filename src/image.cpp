#include "image.h"

namespace re0cc::image {
namespace {

HMODULE g_module = nullptr;
uintptr_t g_base = 0;
mem::Range g_text, g_rdata, g_data, g_all;
std::vector<mem::Range> g_data_sections;

}  // namespace

void init() {
  if (g_module) return;
  g_module = GetModuleHandleA(nullptr);
  g_base = reinterpret_cast<uintptr_t>(g_module);
  g_text = mem::section(g_module, ".text");
  g_rdata = mem::section(g_module, ".rdata");
  g_data = mem::section(g_module, ".data");
  g_all = mem::module_range(g_module);
  g_data_sections = mem::data_sections(g_module);
}

HMODULE module() { return g_module; }
uintptr_t base() { return g_base; }
const mem::Range& text() { return g_text; }
const mem::Range& rdata() { return g_rdata; }
const mem::Range& data() { return g_data; }
const std::vector<mem::Range>& data_sections() { return g_data_sections; }
uint32_t rva(uintptr_t va) { return g_all.contains(va) ? static_cast<uint32_t>(va - g_base) : 0; }

}  // namespace re0cc::image
