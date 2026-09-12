#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <vector>

// Small memory helpers, in the spirit of the F.E.A.R. siblings' mem.h:
// everything the mod pokes at lives in another module's pages, so every write
// has to unprotect, write, restore and flush - and every read of game memory
// has to be guarded, because a wrong guess must produce a log line, not a crash.
namespace re0cc::mem {

struct Range {
  uintptr_t begin = 0;
  uintptr_t end = 0;
  bool empty() const { return end <= begin; }
  size_t size() const { return empty() ? 0 : end - begin; }
  bool contains(uintptr_t a) const { return a >= begin && a < end; }
};

inline bool write_bytes(uintptr_t target, const void* data, size_t n) {
  DWORD old = 0;
  if (!VirtualProtect(reinterpret_cast<void*>(target), n, PAGE_EXECUTE_READWRITE, &old)) return false;
  std::memcpy(reinterpret_cast<void*>(target), data, n);
  VirtualProtect(reinterpret_cast<void*>(target), n, old, &old);
  FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target), n);
  return true;
}

template <typename T>
inline bool write(uintptr_t target, const T& value) {
  return write_bytes(target, &value, sizeof(T));
}

template <typename T>
inline T read(uintptr_t target) {
  T v{};
  std::memcpy(&v, reinterpret_cast<const void*>(target), sizeof(T));
  return v;
}

// True when [p, p+n) is committed, readable memory. VirtualQuery-based, so it
// is safe to call on any value that merely looks like a pointer.
inline bool readable(const void* p, size_t n) {
  auto a = reinterpret_cast<uintptr_t>(p);
  if (!a) return false;
  while (true) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                     PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & ok)) return false;
    const uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    const size_t avail = region_end - a;
    if (avail >= n) return true;
    n -= avail;
    a = region_end;
  }
}
inline bool readable(uintptr_t p, size_t n) { return readable(reinterpret_cast<const void*>(p), n); }

template <typename T>
inline bool read_safe(uintptr_t target, T* out) {
  if (!readable(reinterpret_cast<const void*>(target), sizeof(T))) return false;
  std::memcpy(out, reinterpret_cast<const void*>(target), sizeof(T));
  return true;
}

// Follow a pointer chain: base -> [base+o0] -> [..+o1] ... The last offset is
// added but not dereferenced; the result is the address of the final field.
inline bool follow(uintptr_t base, const int* offsets, int n, uintptr_t* out) {
  uintptr_t a = base;
  for (int i = 0; i < n; ++i) {
    if (i > 0) {
      uintptr_t next = 0;
      if (!read_safe(a, &next) || !next) return false;
      a = next;
    }
    a += static_cast<uintptr_t>(offsets[i]);
  }
  *out = a;
  return true;
}

// Replace one entry of a vtable and hand back what was there.
inline void* hook_vtable(uintptr_t vtable, int slot, void* replacement) {
  uintptr_t entry = vtable + static_cast<uintptr_t>(slot) * sizeof(void*);
  void* original = read<void*>(entry);
  if (!write<void*>(entry, replacement)) return nullptr;
  return original;
}

// Replace one entry in a module's import address table; returns what was there.
inline void* iat_hook(HMODULE module, const char* dll_name, const char* fn_name,
                      void* replacement) {
  auto base = reinterpret_cast<uintptr_t>(module);
  auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  if (!module || dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
  auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return nullptr;

  for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
       imp->Name; ++imp) {
    const char* name = reinterpret_cast<const char*>(base + imp->Name);
    if (dll_name && _stricmp(name, dll_name) != 0) continue;
    auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
    auto* orig = reinterpret_cast<IMAGE_THUNK_DATA*>(
        base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
    for (; orig->u1.AddressOfData; ++orig, ++thunk) {
      if (IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal)) continue;
      auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + orig->u1.AddressOfData);
      if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), fn_name) != 0) continue;
      void* previous = reinterpret_cast<void*>(thunk->u1.Function);
      if (!write<void*>(reinterpret_cast<uintptr_t>(&thunk->u1.Function), replacement))
        return nullptr;
      return previous;
    }
  }
  return nullptr;
}

// The address of a module's IAT slot for an import (0 when absent), so a dump
// can put the original value back and a watchdog can see whether it changed.
inline uintptr_t iat_slot(HMODULE module, const char* dll_name, const char* fn_name) {
  auto base = reinterpret_cast<uintptr_t>(module);
  auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  if (!module || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return 0;
  for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
    if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll_name) != 0) continue;
    auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
    auto* orig = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
    for (; orig->u1.AddressOfData; ++orig, ++thunk) {
      if (IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal)) continue;
      auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + orig->u1.AddressOfData);
      if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), fn_name) == 0)
        return reinterpret_cast<uintptr_t>(&thunk->u1.Function);
    }
  }
  return 0;
}

// Redirect a relative CALL (E8 rel32) at `site` to `replacement`, returning the
// address it used to reach.
inline void* hook_call_site(uintptr_t site, void* replacement) {
  if (read<uint8_t>(site) != 0xE8) return nullptr;
  const int32_t old_rel = read<int32_t>(site + 1);
  void* previous = reinterpret_cast<void*>(site + 5 + old_rel);
  const int32_t new_rel =
      static_cast<int32_t>(reinterpret_cast<uintptr_t>(replacement) - (site + 5));
  if (!write<int32_t>(site + 1, new_rel)) return nullptr;
  return previous;
}

// --- PE sections -------------------------------------------------------------
inline IMAGE_NT_HEADERS* nt_headers(HMODULE m) {
  auto base = reinterpret_cast<uintptr_t>(m);
  auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  if (!m || dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  return nt->Signature == IMAGE_NT_SIGNATURE ? nt : nullptr;
}

inline Range section_range(HMODULE m, const IMAGE_SECTION_HEADER& s) {
  auto base = reinterpret_cast<uintptr_t>(m);
  DWORD size = s.Misc.VirtualSize ? s.Misc.VirtualSize : s.SizeOfRawData;
  return Range{base + s.VirtualAddress, base + s.VirtualAddress + size};
}

// The section with this (up to 8 character) name, or an empty range.
inline Range section(HMODULE m, const char* name) {
  auto nt = nt_headers(m);
  if (!nt) return {};
  auto* s = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    if (std::strncmp(reinterpret_cast<const char*>(s[i].Name), name, 8) == 0)
      return section_range(m, s[i]);
  return {};
}

// Every readable, non-executable, initialised section (where strings live).
inline std::vector<Range> data_sections(HMODULE m) {
  std::vector<Range> out;
  auto nt = nt_headers(m);
  if (!nt) return out;
  auto* s = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    const DWORD c = s[i].Characteristics;
    if (!(c & IMAGE_SCN_MEM_READ) || (c & IMAGE_SCN_MEM_EXECUTE)) continue;
    if (!s[i].SizeOfRawData) continue;  // pure .bss holds no strings
    out.push_back(section_range(m, s[i]));
  }
  return out;
}

inline Range module_range(HMODULE m) {
  auto nt = nt_headers(m);
  if (!nt) return {};
  auto base = reinterpret_cast<uintptr_t>(m);
  return Range{base, base + nt->OptionalHeader.SizeOfImage};
}

// --- pattern scanning --------------------------------------------------------
// Patterns are IDA/CE style: "B8 ?? ?? ?? ?? 89 47 20". A "??" is a wildcard.
struct Pattern {
  std::vector<int16_t> bytes;  // -1 = wildcard
};

inline Pattern parse_pattern(const char* s) {
  Pattern p;
  for (const char* c = s; *c;) {
    while (*c == ' ') ++c;
    if (!*c) break;
    if (c[0] == '?') {
      p.bytes.push_back(-1);
      while (*c == '?') ++c;
      continue;
    }
    auto hex = [](char h) -> int {
      if (h >= '0' && h <= '9') return h - '0';
      if (h >= 'a' && h <= 'f') return h - 'a' + 10;
      if (h >= 'A' && h <= 'F') return h - 'A' + 10;
      return -1;
    };
    const int hi = hex(c[0]), lo = c[1] ? hex(c[1]) : -1;
    if (hi < 0 || lo < 0) break;  // malformed; stop where it stops making sense
    p.bytes.push_back(static_cast<int16_t>(hi * 16 + lo));
    c += 2;
  }
  return p;
}

// True when the bytes at `a` match the pattern (no bounds check beyond readability).
inline bool matches(uintptr_t a, const Pattern& p) {
  if (!readable(a, p.bytes.size())) return false;
  auto* m = reinterpret_cast<const uint8_t*>(a);
  for (size_t i = 0; i < p.bytes.size(); ++i)
    if (p.bytes[i] >= 0 && m[i] != static_cast<uint8_t>(p.bytes[i])) return false;
  return true;
}

// First match at or after `from` (0 = the range start), or 0 when absent.
inline uintptr_t find_pattern(const Range& r, const Pattern& p, uintptr_t from = 0) {
  const size_t n = p.bytes.size();
  if (!n || r.size() < n) return 0;
  uintptr_t a = from > r.begin ? from : r.begin;
  const uint8_t first = static_cast<uint8_t>(p.bytes[0]);
  const bool first_wild = p.bytes[0] < 0;
  for (; a + n <= r.end; ++a) {
    auto* m = reinterpret_cast<const uint8_t*>(a);
    if (!first_wild && m[0] != first) continue;
    size_t i = 1;
    for (; i < n; ++i)
      if (p.bytes[i] >= 0 && m[i] != static_cast<uint8_t>(p.bytes[i])) break;
    if (i == n) return a;
  }
  return 0;
}

inline uintptr_t find_pattern(const Range& r, const char* s, uintptr_t from = 0) {
  return find_pattern(r, parse_pattern(s), from);
}

inline std::vector<uintptr_t> find_all(const Range& r, const Pattern& p, size_t limit = 64) {
  std::vector<uintptr_t> out;
  for (uintptr_t a = find_pattern(r, p); a && out.size() < limit; a = find_pattern(r, p, a + 1))
    out.push_back(a);
  return out;
}

inline std::vector<uintptr_t> find_all(const Range& r, const char* s, size_t limit = 64) {
  return find_all(r, parse_pattern(s), limit);
}

// A NUL-terminated string that starts on a NUL boundary (so "Foo" does not
// match the tail of "GetFoo"). Every occurrence, in address order.
inline std::vector<uintptr_t> find_cstrings(const std::vector<Range>& ranges, const char* s, size_t limit = 8) {
  std::vector<uintptr_t> out;
  const size_t n = std::strlen(s) + 1;  // include the terminator
  for (const Range& r : ranges) {
    if (r.size() < n) continue;
    for (uintptr_t a = r.begin; a + n <= r.end && out.size() < limit; ++a) {
      auto* m = reinterpret_cast<const char*>(a);
      if (m[0] != s[0] || std::memcmp(m, s, n) != 0) continue;
      if (a > r.begin && m[-1] != '\0') continue;
      out.push_back(a);
    }
  }
  return out;
}

inline uintptr_t find_cstring(const std::vector<Range>& ranges, const char* s) {
  const std::vector<uintptr_t> all = find_cstrings(ranges, s, 1);
  return all.empty() ? 0 : all[0];
}

// A string that starts with `prefix` on a NUL boundary (for "MasterRelease ...").
inline uintptr_t find_cstring_prefix(const std::vector<Range>& ranges, const char* prefix) {
  const size_t n = std::strlen(prefix);
  for (const Range& r : ranges) {
    if (r.size() < n + 1) continue;
    for (uintptr_t a = r.begin; a + n < r.end; ++a) {
      auto* m = reinterpret_cast<const char*>(a);
      if (m[0] != prefix[0] || std::memcmp(m, prefix, n) != 0) continue;
      if (a > r.begin && m[-1] != '\0') continue;
      return a;
    }
  }
  return 0;
}

// Every 4-byte-aligned dword in `r` equal to `value` (pointers, vtables, immediates in tables).
inline std::vector<uintptr_t> find_dwords(const Range& r, uint32_t value, size_t limit = 64) {
  std::vector<uintptr_t> out;
  for (uintptr_t a = (r.begin + 3) & ~uintptr_t(3); a + 4 <= r.end && out.size() < limit; a += 4)
    if (*reinterpret_cast<const uint32_t*>(a) == value) out.push_back(a);
  return out;
}

inline Pattern imm32_pattern(uint32_t v) {
  Pattern p;
  for (int i = 0; i < 4; ++i) p.bytes.push_back(static_cast<int16_t>((v >> (8 * i)) & 0xFF));
  return p;
}

// Heuristic for the SteamStub gate: an encrypted .text has no x86 prologues
// (55 8B EC) and no int3 padding runs; a decrypted one has thousands.
inline bool text_looks_decrypted(const Range& text, int* prologues, int* pads) {
  const size_t n = text.size() < (256u << 10) ? text.size() : (256u << 10);
  int pro = 0, pad = 0;
  auto* m = reinterpret_cast<const uint8_t*>(text.begin);
  for (size_t i = 0; i + 4 <= n; ++i) {
    if (m[i] == 0x55 && m[i + 1] == 0x8B && m[i + 2] == 0xEC) ++pro;
    if (m[i] == 0xCC && m[i + 1] == 0xCC && m[i + 2] == 0xCC && m[i + 3] == 0xCC) ++pad;
  }
  if (prologues) *prologues = pro;
  if (pads) *pads = pad;
  // MT Framework code rarely uses frame pointers, so lean on the padding too.
  return pro >= 32 || pad >= 256;
}

// --- finding objects ---------------------------------------------------------
// The calling thread's stack (TEB StackLimit..StackBase): a scan for a value
// must skip it, or it finds its own argument.
inline void current_stack(uintptr_t* lo, uintptr_t* hi) {
  uintptr_t base = 0, limit = 0;
  asm volatile("movl %%fs:4, %0" : "=r"(base));
  asm volatile("movl %%fs:8, %0" : "=r"(limit));
  *lo = limit;
  *hi = base;
}

// Heap objects whose first dword is `vtable`. Reads go through
// ReadProcessMemory so a page vanishing mid-scan fails a read instead of
// faulting the thread. Image regions are skipped (the RTTI there also points at
// vtables). Returns at most `limit` hits; `scanned` receives the bytes covered.
inline std::vector<uintptr_t> find_objects_by_vtable(uintptr_t vtable, size_t limit, size_t* scanned) {
  std::vector<uintptr_t> out;
  std::vector<uint8_t> buf(1 << 20);
  if (scanned) *scanned = 0;
  uintptr_t stack_lo = 0, stack_hi = 0;
  current_stack(&stack_lo, &stack_hi);
  const uintptr_t buf_lo = reinterpret_cast<uintptr_t>(buf.data()), buf_hi = buf_lo + buf.size();
  MEMORY_BASIC_INFORMATION mbi{};
  uintptr_t a = 0x10000;
  while (a < 0xFFFF0000u && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const uintptr_t end = base + mbi.RegionSize;
    const bool is_stack = base < stack_hi && end > stack_lo;
    const bool want = !is_stack && mbi.State == MEM_COMMIT && mbi.Type != MEM_IMAGE && !(mbi.Protect & PAGE_GUARD) &&
                      (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY));
    if (want) {
      for (uintptr_t p = base; p < end; p += buf.size()) {
        const size_t n = (end - p) < buf.size() ? static_cast<size_t>(end - p) : buf.size();
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(p), buf.data(), n, &got) || !got) continue;
        if (scanned) *scanned += got;
        const uint32_t* w = reinterpret_cast<const uint32_t*>(buf.data());
        for (size_t i = 0; i + 4 <= got; i += 4)
          if (w[i / 4] == vtable) {
            const uintptr_t hit = p + i;
            if (hit >= buf_lo && hit < buf_hi) continue;  // our own scan buffer
            out.push_back(hit);
            if (out.size() >= limit) return out;
          }
      }
    }
    if (end <= a) break;
    a = end;
  }
  return out;
}

// --- code patches -----------------------------------------------------------------
// A byte patch that remembers what it replaced. prepare() insists the live
// bytes match a pattern first (so a site found by signature is re-verified at
// the moment of use), apply() refuses if they changed since, revert() only
// writes when the site still holds the replacement.
struct Patch {
  uintptr_t at = 0;
  std::vector<uint8_t> original, replacement;
  const char* label = "";
  bool applied = false;

  bool prepared() const { return at != 0 && !original.empty(); }

  bool prepare(uintptr_t site, const char* expect_pattern, const std::vector<uint8_t>& bytes, const char* name) {
    const Pattern p = parse_pattern(expect_pattern);
    if (!site || p.bytes.size() != bytes.size() || !matches(site, p)) return false;
    at = site;
    label = name;
    original.assign(reinterpret_cast<const uint8_t*>(site), reinterpret_cast<const uint8_t*>(site) + bytes.size());
    replacement = bytes;
    applied = false;
    return true;
  }
  bool apply() {
    if (!prepared() || applied) return applied;
    if (!readable(at, original.size()) || std::memcmp(reinterpret_cast<const void*>(at), original.data(), original.size()) != 0)
      return false;  // someone else changed the site: leave it alone
    if (!write_bytes(at, replacement.data(), replacement.size())) return false;
    applied = true;
    return true;
  }
  bool revert() {
    if (!prepared() || !applied) return true;
    if (!readable(at, replacement.size()) || std::memcmp(reinterpret_cast<const void*>(at), replacement.data(), replacement.size()) != 0) {
      applied = false;  // not ours any more; do not stomp on whatever is there now
      return false;
    }
    const bool ok = write_bytes(at, original.data(), original.size());
    if (ok) applied = false;
    return ok;
  }
};

inline std::vector<uint8_t> nops(size_t n) { return std::vector<uint8_t>(n, 0x90); }

}  // namespace re0cc::mem
