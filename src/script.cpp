#include "script.h"

#include <cstring>

#include "image.h"
#include "log.h"
#include "mem.h"

namespace re0cc::script {
namespace {

constexpr size_t kRow = 12;
uintptr_t g_begin = 0, g_end = 0;

// Argument signatures are short alphanumerics: "", "U4", "U2S2", "FnU1U1U1U1".
bool argsig_ok(uintptr_t s) {
  if (!image::rdata().contains(s) || !mem::readable(s, 1)) return false;
  auto* c = reinterpret_cast<const char*>(s);
  for (int i = 0; i < 32; ++i) {
    if (c[i] == '\0') return true;  // "" is a valid signature (no arguments)
    const bool alnum = (c[i] >= '0' && c[i] <= '9') || (c[i] >= 'A' && c[i] <= 'Z') || (c[i] >= 'a' && c[i] <= 'z');
    if (!alnum) return false;
  }
  return false;
}

bool name_ok(uintptr_t s) {
  if (!image::rdata().contains(s) || !mem::readable(s, 1)) return false;
  auto* c = reinterpret_cast<const char*>(s);
  for (int i = 0; i < 64; ++i) {
    if (c[i] == '\0') return i > 0;
    if (c[i] < 0x21 || c[i] > 0x7E) return false;
  }
  return false;
}

bool row_ok(uintptr_t row) {
  if (!image::rdata().contains(row) || !mem::readable(row, kRow)) return false;
  const uint32_t sig = mem::read<uint32_t>(row), name = mem::read<uint32_t>(row + 4), fn = mem::read<uint32_t>(row + 8);
  return argsig_ok(sig) && name_ok(name) && image::text().contains(fn);
}

}  // namespace

bool init() {
  if (g_begin) return true;
  static const char* const kAnchors[] = {"PlayerMutekiSet", "item_get", "save_point"};
  uintptr_t anchor = 0;
  for (const char* a : kAnchors) {
    for (uintptr_t s : mem::find_cstrings(image::data_sections(), a, 4)) {
      for (uintptr_t hit : mem::find_dwords(image::rdata(), static_cast<uint32_t>(s), 8)) {
        if (row_ok(hit - 4)) { anchor = hit - 4; break; }
      }
      if (anchor) break;
    }
    if (anchor) break;
  }
  if (!anchor) {
    logf("ERROR: script: no command-table row found for the anchor names");
    return false;
  }
  uintptr_t b = anchor, e = anchor;
  while (row_ok(b - kRow)) b -= kRow;
  while (row_ok(e + kRow)) e += kRow;
  g_begin = b;
  g_end = e + kRow;
  logf("script: command table exe+0x%06X..exe+0x%06X, %d rows", image::rva(g_begin), image::rva(g_end), count());
  return true;
}

int count() { return g_begin ? static_cast<int>((g_end - g_begin) / kRow) : 0; }
uintptr_t table_begin() { return g_begin; }
uintptr_t table_end() { return g_end; }

bool find(const char* name, Command* out) {
  if (!g_begin) return false;
  for (uintptr_t row = g_begin; row < g_end; row += kRow) {
    auto* nm = reinterpret_cast<const char*>(mem::read<uint32_t>(row + 4));
    if (std::strcmp(nm, name) != 0) continue;
    if (out) {
      out->name = nm;
      out->argsig = reinterpret_cast<const char*>(mem::read<uint32_t>(row));
      out->handler = mem::read<uint32_t>(row + 8);
      out->entry = row;
    }
    return true;
  }
  return false;
}

}  // namespace re0cc::script
