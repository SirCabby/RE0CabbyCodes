#pragma once

#include <cstdint>

// MT Framework's own reflection, used instead of MSVC RTTI (which the game was
// built without). Every class has a static MtDTI object {vtable, name, next,
// child, parent, link, size/attr, id} in .data, constructed at static-init
// time, and every MtObject vtable carries a getDTI() stub of the form
// `mov eax, <MtDTI*>; ret`. From a class name this module finds the DTI, the
// stub, the vtable, and (for singletons) the live instance - all by reading,
// never by calling anything in the game.
namespace re0cc::dti {

struct ClassInfo {
  char name[48] = {};
  uintptr_t dti = 0;
  uintptr_t parent = 0;   // parent MtDTI (0 for MtObject)
  uint32_t size_word = 0; // the +0x18 word (size/allocator/attr bitfield)
  uint32_t id = 0;        // the +0x1C word (a CRC of the name, presumably)
  uintptr_t stub = 0;     // the getDTI() stub `B8 <dti> C3` in .text (0 = none found)
  uintptr_t vtable = 0;   // derived from the stub's vtable slot (0 = unknown)
  int slot_hits = 0;      // how many .rdata dwords point at the stub
  bool valid = false;
};

bool init();                                     // after decryption; derives the layout from MtObject and probe classes
bool ready();
int getdti_slot();                               // vtable index of getDTI(), -1 if unknown
const ClassInfo* find_class(const char* name);   // cached; null (and a log line) when not found

// Pure reads on a live object.
const char* class_name_of(const void* obj);      // null when `obj` is not an MtObject
uintptr_t dti_of(const void* obj);
bool is_a(const void* obj, const char* base_name);
const char* dti_name(uintptr_t dti);
uintptr_t dti_parent(uintptr_t dti);

// Singletons: a static pointer slot in .data that holds the instance, or the
// instance itself when it is a static object, or (fallback) a heap scan.
struct Singleton {
  uintptr_t slot = 0;  // .data dword holding the object pointer (0 = static object / heap-found)
  uintptr_t obj = 0;
};
bool find_singleton(const ClassInfo& c, Singleton* out);
uintptr_t resolve(const ClassInfo& c, const Singleton& s);  // re-validated live pointer, or 0

}  // namespace re0cc::dti
