#include "dti.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "config.h"
#include "image.h"
#include "log.h"
#include "mem.h"

namespace re0cc::dti {
namespace {

constexpr int kMaxClasses = 96;
ClassInfo g_classes[kMaxClasses];
int g_nclasses = 0;

int g_name_off = 4;       // MtDTI field offsets, confirmed on MtObject / the probes
int g_parent_off = 0x10;
uintptr_t g_dti_vtable = 0;
int g_slot = -1;
bool g_ready = false;

bool looks_like_vtable(uintptr_t vt) {
  if (!image::rdata().contains(vt) || !mem::readable(vt, 4)) return false;
  return image::text().contains(mem::read<uint32_t>(vt));
}

// A candidate MtDTI at `base` whose name pointer sits at +name_off. Every
// class has its own DTI vtable (a per-class newInstance), so the vtable is
// only checked for shape; `expect_vt` is kept for callers that know one.
bool dti_ok(uintptr_t base, int name_off, uintptr_t expect_vt) {
  if (!image::data().contains(base) || !mem::readable(base, 0x20)) return false;
  const uintptr_t vt = mem::read<uint32_t>(base);
  if (expect_vt ? vt != expect_vt : !looks_like_vtable(vt)) return false;
  const uintptr_t nm = mem::read<uint32_t>(base + name_off);
  if (!image::rdata().contains(nm)) return false;
  // +0x18 is size (in dwords) | attributes; a class is never 0 dwords.
  return (mem::read<uint32_t>(base + 0x18) & 0x7FFFFF) != 0;
}

// `a` is the address of an imm32 in .text: does the encoding before it store
// (C7 /0) or load (B8+r) that immediate? Constructors store a vtable pointer
// into the object; that is the evidence used to tell which vtable base a
// getDTI slot belongs to.
bool is_store_imm32(uintptr_t a) {
  const uintptr_t lo = image::text().begin;
  auto b = [&](int off) { return mem::read<uint8_t>(a + off); };
  if (a >= lo + 2 && b(-2) == 0xC7) {
    const uint8_t m = b(-1);
    if (((m >> 3) & 7) == 0 && (m >> 6) == 0 && (m & 7) != 4 && (m & 7) != 5) return true;
  }
  if (a >= lo + 3 && b(-3) == 0xC7) {
    const uint8_t m = b(-2);
    const int mod = m >> 6, reg = (m >> 3) & 7, rm = m & 7;
    if (reg == 0 && ((mod == 1 && rm != 4) || (mod == 0 && rm == 4 && (b(-1) & 7) != 5))) return true;
  }
  if (a >= lo + 4 && b(-4) == 0xC7) {
    const uint8_t m = b(-3);
    if (((m >> 3) & 7) == 0 && (m >> 6) == 1 && (m & 7) == 4) return true;
  }
  if (a >= lo + 6 && b(-6) == 0xC7) {
    const uint8_t m = b(-5);
    const int mod = m >> 6, reg = (m >> 3) & 7, rm = m & 7;
    if (reg == 0 && ((mod == 2 && rm != 4) || (mod == 0 && rm == 5))) return true;
  }
  if (a >= lo + 7 && b(-7) == 0xC7) {
    const uint8_t m = b(-6);
    if (((m >> 3) & 7) == 0 && (m >> 6) == 2 && (m & 7) == 4) return true;
  }
  return false;
}
bool is_load_imm32(uintptr_t a) {
  const uint8_t op = mem::read<uint8_t>(a - 1);
  return a > image::text().begin && op >= 0xB8 && op <= 0xBF;
}

// Evidence that `v` is a vtable base: 2 per constructor store, 1 per register load.
int vtable_evidence(uint32_t v, int* stores) {
  int s = 0, l = 0;
  for (uintptr_t a : mem::find_all(image::text(), mem::imm32_pattern(v), 48)) {
    if (is_store_imm32(a)) ++s;
    else if (is_load_imm32(a)) ++l;
  }
  if (stores) *stores = s;
  return s * 2 + l;
}

uint32_t crc32(const char* s) {
  uint32_t c = 0xFFFFFFFFu;
  for (; *s; ++s) {
    c ^= static_cast<uint8_t>(*s);
    for (int i = 0; i < 8; ++i) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}
// The DTI id is the bitwise-not of the CRC32 of the class name with the top bit
// clear - unless the class was given an explicit id, in which case the top bit
// is set (uPlayerRebecca is 0x80000002, uPlayerBilly 0x80000005).
uint32_t expected_id(const char* name) { return (~crc32(name)) & 0x7FFFFFFFu; }
bool id_ok(uint32_t id, const char* name) { return id == expected_id(name) || (id & 0x80000000u) != 0; }
uintptr_t g_root_dti = 0;

bool chain_reaches(uintptr_t dti, int parent_off, uintptr_t root);

ClassInfo* cached(const char* name) {
  for (int i = 0; i < g_nclasses; ++i)
    if (!std::strcmp(g_classes[i].name, name)) return &g_classes[i];
  return nullptr;
}

// Locate the MtDTI for `name`, trying the given name offsets. Fills dti/parent/size/id.
bool locate(const char* name, const int* name_offs, int n_offs, uintptr_t expect_vt, ClassInfo* c, int* found_off) {
  int candidates = 0;
  for (uintptr_t s : mem::find_cstrings(image::data_sections(), name, 6)) {
    for (uintptr_t hit : mem::find_dwords(image::data(), static_cast<uint32_t>(s), 16)) {
      for (int i = 0; i < n_offs; ++i) {
        const uintptr_t base = hit - static_cast<uintptr_t>(name_offs[i]);
        if (!dti_ok(base, name_offs[i], expect_vt)) continue;
        const uint32_t id = mem::read<uint32_t>(base + 0x1C);
        if (!id_ok(id, name)) {
          if (config::get().trace) logf("trace: dti: %s candidate at exe+0x%06X has id %08X, expected %08X - skipped", name, image::rva(base), id, expected_id(name));
          continue;
        }
        // An explicit id proves nothing about the record: insist on a parent chain to MtObject.
        if ((id & 0x80000000u) && g_parent_off && g_root_dti && !chain_reaches(base, g_parent_off, g_root_dti)) {
          if (config::get().trace) logf("trace: dti: %s candidate at exe+0x%06X (explicit id) has no chain to MtObject - skipped", name, image::rva(base));
          continue;
        }
        if (++candidates == 1) {
          c->dti = base;
          *found_off = name_offs[i];
        } else if (config::get().trace) {
          logf("trace: dti: extra candidate for %s at exe+0x%06X (name at +%d)", name, image::rva(base), name_offs[i]);
        }
      }
    }
  }
  return candidates > 0;
}

void fill_from_dti(ClassInfo* c) {
  c->parent = g_parent_off ? mem::read<uint32_t>(c->dti + g_parent_off) : 0;
  c->size_word = mem::read<uint32_t>(c->dti + 0x18);
  c->id = mem::read<uint32_t>(c->dti + 0x1C);
  // getDTI stub: mov eax, <dti>; ret
  char pat[64];
  const uint32_t d = static_cast<uint32_t>(c->dti);
  std::snprintf(pat, sizeof(pat), "B8 %02X %02X %02X %02X C3", d & 0xFF, (d >> 8) & 0xFF, (d >> 16) & 0xFF, (d >> 24) & 0xFF);
  const std::vector<uintptr_t> stubs = mem::find_all(image::text(), pat, 4);
  if (!stubs.empty()) {
    c->stub = stubs[0];
    if (stubs.size() > 1) logf("dti: %s has %u getDTI stubs (using the first)", c->name, static_cast<unsigned>(stubs.size()));
    const std::vector<uintptr_t> slots = mem::find_dwords(image::rdata(), static_cast<uint32_t>(c->stub), 8);
    c->slot_hits = static_cast<int>(slots.size());
    if (g_slot >= 0 && !slots.empty()) {
      // Several classes can share one stub only if they share the DTI, so one
      // hit is the norm; with more, take the base that has constructor stores.
      uintptr_t best = 0;
      int best_ev = -1;
      for (uintptr_t sl : slots) {
        const uintptr_t vt = sl - 4u * static_cast<uintptr_t>(g_slot);
        if (!image::rdata().contains(vt)) continue;
        const int ev = slots.size() == 1 ? 1 : vtable_evidence(static_cast<uint32_t>(vt), nullptr);
        if (ev > best_ev) { best_ev = ev; best = vt; }
      }
      c->vtable = best;
    }
  }
  c->valid = true;
}

bool chain_reaches(uintptr_t dti, int parent_off, uintptr_t root) {
  uintptr_t d = dti;
  for (int hops = 0; hops < 32; ++hops) {
    if (d == root) return true;
    if (!dti_ok(d, g_name_off, g_dti_vtable)) return false;
    d = mem::read<uint32_t>(d + parent_off);
    if (!d) return false;
  }
  return false;
}

}  // namespace

bool init() {
  if (g_ready) return true;
  image::init();
  // 1. The root class fixes the MtDTI vtable and the name offset.
  static const int kNameOffs[] = {4, 8};
  ClassInfo root{};
  std::snprintf(root.name, sizeof(root.name), "MtObject");
  int off = 0;
  if (!locate("MtObject", kNameOffs, 2, 0, &root, &off)) {
    logf("ERROR: dti: no MtDTI object found for MtObject - is .text decrypted and the engine initialised?");
    return false;
  }
  g_name_off = off;
  g_root_dti = root.dti;
  g_dti_vtable = 0;  // per-class DTI vtables: no shared value to insist on
  // 2. The parent offset: the chain from a derived class must end at MtObject.
  static const char* const kProbes[] = {"sGamePause", "sPlayer", "uGUIPause", "cSystem"};
  ClassInfo probes[4]{};
  int nprobes = 0;
  for (const char* p : kProbes) {
    ClassInfo c{};
    std::snprintf(c.name, sizeof(c.name), "%s", p);
    int o = 0;
    if (locate(p, &g_name_off, 1, g_dti_vtable, &c, &o)) probes[nprobes++] = c;
    else logf("dti: probe class %s not found", p);
  }
  if (!nprobes) {
    logf("ERROR: dti: none of the probe classes resolved");
    return false;
  }
  static const int kParentOffs[] = {0x10, 0x0C, 0x08, 0x14, 0x18};
  g_parent_off = 0;
  for (int po : kParentOffs) {
    int ok = 0;
    for (int i = 0; i < nprobes; ++i) ok += chain_reaches(probes[i].dti, po, root.dti) ? 1 : 0;
    if (ok == nprobes) { g_parent_off = po; break; }
  }
  if (!g_parent_off) logf("dti: WARNING: no parent offset makes every probe chain reach MtObject - is_a() unavailable");
  // 3. The getDTI slot: the k for which every probe's `slot - 4k` is stored by a constructor.
  g_slot = config::get().dti_slot;
  fill_from_dti(&root);
  for (int i = 0; i < nprobes; ++i) fill_from_dti(&probes[i]);
  if (g_slot < 0) {
    int best_k = -1, best_total = 0, winners = 0;
    for (int k = 0; k < 8; ++k) {
      int total = 0, all = 1;
      for (int i = 0; i < nprobes; ++i) {
        if (!probes[i].stub || probes[i].slot_hits != 1) continue;
        const uintptr_t sl = mem::find_dwords(image::rdata(), static_cast<uint32_t>(probes[i].stub), 1)[0];
        int stores = 0;
        const int ev = vtable_evidence(static_cast<uint32_t>(sl - 4u * k), &stores);
        if (stores == 0) all = 0;
        total += ev;
      }
      if (config::get().trace) logf("trace: dti: slot candidate k=%d evidence=%d all=%d", k, total, all);
      if (all && total > 0) {
        ++winners;
        if (total > best_total) { best_total = total; best_k = k; }
      }
    }
    if (winners == 1) g_slot = best_k;
    else if (winners > 1) { logf("dti: WARNING: %d slot candidates (best k=%d, evidence %d) - using the best; set DtiSlot to override", winners, best_k, best_total); g_slot = best_k; }
    else logf("ERROR: dti: could not derive the getDTI vtable slot (set DtiSlot in the ini to override)");
  }
  // Refill with the slot known so the vtables come out.
  fill_from_dti(&root);
  for (int i = 0; i < nprobes; ++i) fill_from_dti(&probes[i]);
  g_classes[g_nclasses++] = root;
  for (int i = 0; i < nprobes && g_nclasses < kMaxClasses; ++i) g_classes[g_nclasses++] = probes[i];
  logf("dti: MtObject DTI exe+0x%06X (its DTI vtable exe+0x%06X), name at +%d, parent at +0x%X, getDTI slot %d",
       image::rva(root.dti), image::rva(mem::read<uint32_t>(root.dti)), g_name_off, g_parent_off, g_slot);
  for (int i = 0; i < g_nclasses; ++i) {
    const ClassInfo& c = g_classes[i];
    logf("dti: %-16s dti=exe+0x%06X parent=%-12s size=0x%08X id=0x%08X stub=exe+0x%06X vtable=exe+0x%06X (slots %d)", c.name,
         image::rva(c.dti), c.parent ? dti_name(c.parent) : "-", c.size_word, c.id, image::rva(c.stub), image::rva(c.vtable),
         c.slot_hits);
  }
  g_ready = g_slot >= 0;
  return g_ready;
}

bool ready() { return g_ready; }
int getdti_slot() { return g_slot; }

const ClassInfo* find_class(const char* name) {
  if (ClassInfo* c = cached(name)) return c->valid ? c : nullptr;
  if (g_nclasses >= kMaxClasses) return nullptr;
  ClassInfo c{};
  std::snprintf(c.name, sizeof(c.name), "%s", name);
  int o = 0;
  if (!locate(name, &g_name_off, 1, g_dti_vtable, &c, &o)) {
    logf("dti: class %s not found", name);
    c.valid = false;
    g_classes[g_nclasses++] = c;
    return nullptr;
  }
  fill_from_dti(&c);
  logf("dti: %-16s dti=exe+0x%06X parent=%-12s size=0x%08X id=0x%08X stub=exe+0x%06X vtable=exe+0x%06X (slots %d)", c.name,
       image::rva(c.dti), c.parent ? dti_name(c.parent) : "-", c.size_word, c.id, image::rva(c.stub), image::rva(c.vtable),
       c.slot_hits);
  g_classes[g_nclasses++] = c;
  return &g_classes[g_nclasses - 1];
}

const char* dti_name(uintptr_t dti) {
  if (!dti_ok(dti, g_name_off, g_dti_vtable)) return nullptr;
  auto* s = reinterpret_cast<const char*>(mem::read<uint32_t>(dti + g_name_off));
  if (!mem::readable(s, 64)) return nullptr;
  for (int i = 0; i < 64; ++i) {
    if (s[i] == '\0') {
      if (!i) return nullptr;
      return id_ok(mem::read<uint32_t>(dti + 0x1C), s) ? s : nullptr;
    }
    if (s[i] < 0x21 || s[i] > 0x7E) return nullptr;
  }
  return nullptr;
}

uintptr_t dti_parent(uintptr_t dti) {
  if (!g_parent_off || !dti_ok(dti, g_name_off, g_dti_vtable)) return 0;
  return mem::read<uint32_t>(dti + g_parent_off);
}

uintptr_t dti_of(const void* obj) {
  if (g_slot < 0) return 0;
  uintptr_t vt = 0;
  if (!mem::read_safe(reinterpret_cast<uintptr_t>(obj), &vt) || !image::rdata().contains(vt)) return 0;
  const uintptr_t slot = vt + 4u * static_cast<uintptr_t>(g_slot);
  if (!image::rdata().contains(slot)) return 0;
  const uintptr_t fn = mem::read<uint32_t>(slot);
  if (!image::text().contains(fn) || !mem::readable(fn, 6)) return 0;
  auto* b = reinterpret_cast<const uint8_t*>(fn);
  if (b[0] != 0xB8 || b[5] != 0xC3) return 0;
  const uintptr_t dti = mem::read<uint32_t>(fn + 1);
  return dti_ok(dti, g_name_off, g_dti_vtable) ? dti : 0;
}

const char* class_name_of(const void* obj) {
  const uintptr_t d = dti_of(obj);
  return d ? dti_name(d) : nullptr;
}

bool is_a(const void* obj, const char* base_name) {
  uintptr_t d = dti_of(obj);
  for (int hops = 0; d && hops < 32; ++hops) {
    const char* n = dti_name(d);
    if (n && !std::strcmp(n, base_name)) return true;
    d = dti_parent(d);
  }
  return false;
}

// The class's newInstance (slot 1 of its DTI vtable) allocates the object,
// runs the constructor and stores the pointer into the class's static
// instance slot: `... call ctor; mov [slot], reg ...`. Reading that store
// gives the slot before the object even exists.
// A `mov [abs], reg` store into .data within the first `span` bytes of `fn`.
uintptr_t data_store_in(uintptr_t fn, int span) {
  if (!image::text().contains(fn) || !mem::readable(fn, static_cast<size_t>(span) + 8)) return 0;
  auto* b = reinterpret_cast<const uint8_t*>(fn);
  for (int i = 0; i < span; ++i) {
    if (b[i] == 0xCC && b[i + 1] == 0xCC && b[i + 2] == 0xCC) break;  // padding: past the function
    uint32_t slot = 0;
    if (b[i] == 0xA3) slot = mem::read<uint32_t>(fn + i + 1);                        // mov [abs], eax
    else if (b[i] == 0x89 && (b[i + 1] & 0xC7) == 0x05) slot = mem::read<uint32_t>(fn + i + 2);  // mov [abs], reg
    else continue;
    if (image::data().contains(slot)) return slot;
  }
  return 0;
}

uintptr_t slot_from_newinstance(const ClassInfo& c, uintptr_t* ni_out) {
  uint32_t dvt = 0, ni = 0;
  if (!mem::read_safe(c.dti, &dvt) || !image::rdata().contains(dvt)) return 0;
  if (!mem::read_safe(dvt + 4, &ni) || !image::text().contains(ni) || !mem::readable(ni, 0x120)) return 0;
  if (ni_out) *ni_out = ni;
  if (const uintptr_t slot = data_store_in(ni, 0x110)) return slot;
  // Some classes store the instance in their constructor instead (sGameChara):
  // follow the calls newInstance makes, in order. A newInstance that does
  // nothing after the constructor reaches it by a tail jump instead of a call
  // (sEventScript: `... call operator new; test eax,eax; je fail; mov ecx,eax;
  // jmp ctor`), so E9 counts as well as E8.
  auto* b = reinterpret_cast<const uint8_t*>(ni);
  for (int i = 0; i < 0x110; ++i) {
    if (b[i] == 0xCC && b[i + 1] == 0xCC && b[i + 2] == 0xCC) break;
    if (b[i] != 0xE8 && b[i] != 0xE9) continue;
    const uintptr_t callee = ni + i + 5 + static_cast<uintptr_t>(mem::read<int32_t>(ni + i + 1));
    if (!image::text().contains(callee)) continue;
    if (const uintptr_t slot = data_store_in(callee, 0x40)) return slot;
  }
  return 0;
}

// Some singletons are not built through newInstance at all: sItem's newInstance
// is a `xor eax,eax; ret` stub, and the object is constructed elsewhere. With no
// static slot found above, find_singleton used to fall through to the heap scan,
// which matches any dword equal to the vtable - and on Windows that caught a
// coincidental match (a spot in a DTI table, not a real object) and cached it
// with no slot to re-read, so the inventory read garbage for the whole session
// ("inventory not found"). The constructor, though, both stores the class vtable
// into the object (`mov [reg], <vtable>`) and the object pointer into the class's
// static slot (`mov [abs], reg`) a few bytes apart. That pairing gives the slot
// directly, so the singleton resolves off a slot the game keeps current - the
// same way sGameInfo/sSaveManager already do - instead of a one-shot heap guess.
uintptr_t slot_from_ctor(const ClassInfo& c) {
  if (!c.vtable) return 0;
  const mem::Range text = image::text();
  uintptr_t slot = 0;
  bool ambiguous = false;
  for (uintptr_t a : mem::find_all(text, mem::imm32_pattern(static_cast<uint32_t>(c.vtable)), 64)) {
    // `C7 /0 imm32` storing the vtable into [reg] at offset 0: opcode C7, modrm
    // mod=00 reg=000 rm=reg, with rm != 4 (SIB) and != 5 (disp32-only).
    if (a < text.begin + 2) continue;
    if (mem::read<uint8_t>(a - 2) != 0xC7) continue;
    const uint8_t modrm = mem::read<uint8_t>(a - 1);
    if ((modrm >> 6) != 0 || ((modrm >> 3) & 7) != 0) continue;
    const int reg = modrm & 7;
    if (reg == 4 || reg == 5) continue;
    // A `mov [abs], reg` into .data close to the vtable store: `A3 abs` for eax,
    // else `89 /r` with mod=00 rm=101 (disp32) and the reg field == reg.
    const uintptr_t w0 = a > text.begin + 0x40 ? a - 0x40 : text.begin;
    const uintptr_t w1 = a + 0x40 < text.end - 6 ? a + 0x40 : text.end - 6;
    for (uintptr_t p = w0; p < w1; ++p) {
      uintptr_t abs = 0;
      if (reg == 0 && mem::read<uint8_t>(p) == 0xA3) {
        abs = mem::read<uint32_t>(p + 1);
      } else if (mem::read<uint8_t>(p) == 0x89) {
        const uint8_t mb = mem::read<uint8_t>(p + 1);
        if ((mb >> 6) != 0 || (mb & 7) != 5 || static_cast<int>((mb >> 3) & 7) != reg) continue;
        abs = mem::read<uint32_t>(p + 2);
      } else {
        continue;
      }
      if (!image::data().contains(abs)) continue;
      if (slot && slot != abs) ambiguous = true;
      else slot = abs;
    }
  }
  if (ambiguous) {
    logf("dti: %s: the constructor points at more than one static slot - not using it", c.name);
    return 0;
  }
  return slot;
}

bool find_singleton(const ClassInfo& c, Singleton* out) {
  if (!c.vtable) return false;
  uintptr_t ni = 0;
  uintptr_t slot = slot_from_newinstance(c, &ni);
  char via[64];
  if (slot) std::snprintf(via, sizeof(via), "newInstance exe+0x%06X", image::rva(ni));
  if (!slot && (slot = slot_from_ctor(c))) std::snprintf(via, sizeof(via), "its constructor");
  if (slot) {
    uint32_t cur = 0, vt = 0;
    mem::read_safe(slot, &cur);
    if (cur && (!mem::read_safe(cur, &vt) || vt != c.vtable)) {
      logf("dti: %s: %s stores to exe+0x%06X but it holds %08X (vtable %08X, not %s) - ignoring that slot",
           c.name, via, image::rva(slot), cur, vt, c.name);
    } else {
      out->slot = slot;
      out->obj = cur;
      logf("dti: %s static slot exe+0x%06X (from %s)%s", c.name, image::rva(slot), via,
           cur ? "" : " - object not created yet");
      return true;
    }
  }
  // Static pointer slots in .data whose target starts with the vtable.
  std::vector<uintptr_t> slots;
  MEMORY_BASIC_INFORMATION mbi{};
  uintptr_t cache_lo = 0, cache_hi = 0;
  bool cache_ok = false;
  const mem::Range d = image::data();
  for (uintptr_t a = (d.begin + 3) & ~uintptr_t(3); a + 4 <= d.end && slots.size() < 8; a += 4) {
    const uint32_t p = *reinterpret_cast<const uint32_t*>(a);
    if (p < 0x10000 || p >= 0x7FFF0000u || (p & 3)) continue;
    if (!(p >= cache_lo && p + 4 <= cache_hi)) {
      cache_ok = VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)) != 0 && mbi.State == MEM_COMMIT &&
                 !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                 (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
      cache_lo = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
      cache_hi = cache_lo + mbi.RegionSize;
      if (!cache_ok) { cache_lo = cache_hi = 0; continue; }
    }
    if (*reinterpret_cast<const uint32_t*>(p) == c.vtable) slots.push_back(a);
  }
  if (!slots.empty()) {
    out->slot = slots[0];
    out->obj = mem::read<uint32_t>(slots[0]);
    logf("dti: %s instance %p via static slot exe+0x%06X (found by scanning .data)", c.name, reinterpret_cast<void*>(out->obj),
         image::rva(slots[0]));
    return true;
  }
  // Static objects placed directly in .data.
  const std::vector<uintptr_t> objs = mem::find_dwords(d, static_cast<uint32_t>(c.vtable), 4);
  if (!objs.empty()) {
    out->slot = 0;
    out->obj = objs[0];
    logf("dti: %s is a static object at exe+0x%06X", c.name, image::rva(objs[0]));
    return true;
  }
  // Last resort: a heap scan (our own stack is excluded - the vtable value being
  // searched for lives there too).
  size_t scanned = 0;
  const std::vector<uintptr_t> heap = mem::find_objects_by_vtable(c.vtable, 4, &scanned);
  if (!heap.empty()) {
    out->slot = 0;
    out->obj = heap[0];
    logf("dti: %s instance %p found by heap scan (%u hit(s), %u MB scanned)", c.name, reinterpret_cast<void*>(heap[0]),
         static_cast<unsigned>(heap.size()), static_cast<unsigned>(scanned >> 20));
    return true;
  }
  return false;
}

uintptr_t resolve(const ClassInfo& c, const Singleton& s) {
  uintptr_t obj = s.obj;
  if (s.slot && !mem::read_safe(s.slot, &obj)) return 0;
  uintptr_t vt = 0;
  if (!obj || !mem::read_safe(obj, &vt) || vt != c.vtable) return 0;
  return obj;
}

}  // namespace re0cc::dti
