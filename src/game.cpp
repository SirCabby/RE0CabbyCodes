#include "game.h"

#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "dti.h"
#include "image.h"
#include "items.h"
#include "log.h"
#include "script.h"

namespace re0cc::game {
namespace {

char g_status[160] = "not started";
bool g_ready = false;
char g_build[96] = {};
bool g_build_ok = false;

void set_status(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(g_status, sizeof(g_status), fmt, args);
  va_end(args);
}

// --- singletons --------------------------------------------------------------------
struct Single {
  const char* name;
  const dti::ClassInfo* cls;
  dti::Singleton s;
  bool found;
};
Single g_singles[] = {{"sPlayer", nullptr, {}, false},       {"sItem", nullptr, {}, false},       {"sGamePause", nullptr, {}, false},
                      {"sEnemy", nullptr, {}, false},        {"sGameInfo", nullptr, {}, false},   {"sSavedata", nullptr, {}, false},
                      {"sInGameSystem", nullptr, {}, false}, {"sGameMain", nullptr, {}, false},   {"sGameScene", nullptr, {}, false},
                      {"sFlagManager", nullptr, {}, false},  {"sExtraData", nullptr, {}, false},  {"sSaveManager", nullptr, {}, false},
                      {"sGameGUI", nullptr, {}, false},      {"sSubMenu", nullptr, {}, false},
                      {"sGameChara", nullptr, {}, false},    {"sEventScript", nullptr, {}, false},
                      {"sGamePresence", nullptr, {}, false}, {"sRoomControl", nullptr, {}, false},
                      {"sGameArea", nullptr, {}, false}};
constexpr int kSingles = sizeof(g_singles) / sizeof(g_singles[0]);

Single* single_entry(const char* name) {
  for (Single& s : g_singles)
    if (!std::strcmp(s.name, name)) return &s;
  return nullptr;
}
uintptr_t single(const char* name) {
  Single* s = single_entry(name);
  if (!s || !s->found) return 0;
  return dti::resolve(*s->cls, s->s);
}

// --- IAT bookkeeping for the dump ----------------------------------------------------
struct IatNote { uintptr_t slot, original; };
IatNote g_iat[8];
int g_iat_n = 0;

// --- characters ------------------------------------------------------------------------
const dti::ClassInfo* g_cls_rebecca = nullptr;
const dti::ClassInfo* g_cls_billy = nullptr;
const dti::ClassInfo* g_cls_player_base = nullptr;
const dti::ClassInfo* g_cls_enemy_base = nullptr;
// Offsets verified on the Jan-2025 build: sPlayer's two unit pointers are what the
// script selectors 0x20000/0x30000 read (`mov eax,[ecx+2C]` / `mov eax,[ecx+3C]`);
// on a unit, +0x100 is the character type, +0x1030 the HP and +0x68C4 a status
// dword whose bit 0 is poison (PlayerMutekiSet clears bit 0x4000 in it).
constexpr int kOffType = 0x100, kOffHp = 0x1030, kOffStatus = 0x68C4, kOffActive = 0x2C, kOffPartner = 0x3C;
constexpr uint32_t kStatusPoison = 1u;
uintptr_t g_char[2] = {};
DWORD g_char_scan = 0;
bool g_char_layout_warned = false;
int g_hp_seen[2] = {150, 250};

bool is_player_unit(uintptr_t p) {
  if (!mem::readable(p, kOffStatus + 4)) return false;
  const char* n = dti::class_name_of(reinterpret_cast<void*>(p));
  return n && (!std::strcmp(n, "uPlayerRebecca") || !std::strcmp(n, "uPlayerBilly"));
}

// Pointers inside [obj, obj+span) to objects for which `pred` holds.
template <typename Pred>
int find_ptr_fields(uintptr_t obj, size_t span, Pred pred, uintptr_t* out, int max) {
  int n = 0;
  if (!mem::readable(obj, span)) return 0;
  for (size_t off = 0; off + 4 <= span && n < max; off += 4) {
    const uint32_t p = mem::read<uint32_t>(obj + off);
    if (p < 0x10000 || (p & 3)) continue;
    bool dup = false;
    for (int i = 0; i < n; ++i) dup |= out[i] == p;
    if (dup) continue;
    if (pred(p)) out[n++] = p;
  }
  return n;
}

void refresh_characters() {
  const DWORD now = GetTickCount();
  bool valid = true;
  for (uintptr_t c : g_char) valid &= c == 0 || is_player_unit(c);
  if (valid && (g_char[0] || g_char[1]) && now - g_char_scan < 2000) return;
  g_char_scan = now;
  uintptr_t player = single("sPlayer");
  uintptr_t next[2] = {};
  int k = 0;
  if (player) {
    // The two slots the game's own selectors use; a scan of the object is the fallback.
    uint32_t a = 0, b = 0;
    if (mem::read_safe(player + kOffActive, &a) && is_player_unit(a)) next[k++] = a;
    if (mem::read_safe(player + kOffPartner, &b) && b != a && is_player_unit(b)) next[k++] = b;
    if (!k) {
      uintptr_t found[4] = {};
      const int n = find_ptr_fields(player, 0x200, is_player_unit, found, 4);
      for (int i = 0; i < n && k < 2; ++i) next[k++] = found[i];
      if (n) logf("characters: units found by scanning sPlayer rather than at +0x2C/+0x3C");
    }
  }
  if (next[0] != g_char[0] || next[1] != g_char[1]) {
    g_char[0] = next[0];
    g_char[1] = next[1];
    if (k) {
      logf("characters: %d player unit(s) from sPlayer: %p (%s) %p (%s)", k, reinterpret_cast<void*>(next[0]),
           next[0] ? dti::class_name_of(reinterpret_cast<void*>(next[0])) : "-", reinterpret_cast<void*>(next[1]),
           next[1] ? dti::class_name_of(reinterpret_cast<void*>(next[1])) : "-");
    } else if (player && config::get().trace) {
      logf("trace: no player units reachable from sPlayer %p yet", reinterpret_cast<void*>(player));
    }
  }
}

bool read_character(uintptr_t obj, Character* c) {
  if (!obj || !is_player_unit(obj)) return false;
  c->obj = obj;
  c->cls = dti::class_name_of(reinterpret_cast<void*>(obj));
  c->type = mem::read<int32_t>(obj + kOffType);
  c->hp = mem::read<int32_t>(obj + kOffHp);
  const uint32_t status = mem::read<uint32_t>(obj + kOffStatus);
  c->poison = (status & kStatusPoison) ? 1 : 0;
  const bool rebecca = !std::strcmp(c->cls, "uPlayerRebecca");
  const bool layout_ok = (rebecca ? c->type == 3 : c->type == 5) && c->hp >= -10 && c->hp <= 1000;
  if (!layout_ok) {
    if (!g_char_layout_warned) {
      g_char_layout_warned = true;
      logf("ERROR: character layout lead does not fit %s %p: type=%d hp=%d status=%08X - HP features off", c->cls,
           reinterpret_cast<void*>(obj), c->type, c->hp, status);
    }
    return false;
  }
  uintptr_t player = single("sPlayer");
  uint32_t active = 0;
  c->active = player && mem::read_safe(player + kOffActive, &active) && active == obj;
  return true;
}

// --- bags ---------------------------------------------------------------------------------
constexpr int kOffBagRebecca = 0x20, kOffBagBilly = 0x60;
bool g_bags_ok = false;
bool g_bags_warned = false;

void hexdump(const char* what, uintptr_t at, size_t n);  // defined below

// A rejected bag used to be silent after the first one ever, so a value that
// should not exist left no trace of itself. Rate-limited to one every 5 s, with
// the field, the value and the bytes it came from.
bool bag_rejected(uintptr_t base, const char* what, int index, int value) {
  static DWORD last = 0;
  const DWORD now = GetTickCount();
  if (!last || now - last >= 5000) {
    last = now;
    logf("bags: reading at %p thrown away: %s %d = %d is out of range", reinterpret_cast<void*>(base), what, index, value);
    hexdump("bag", base, 0x40);
  }
  return false;
}

// A count is 0..9999 - far above any stack the game builds, low enough that a
// pointer or a float read by mistake does not pass - or exactly the game's own
// infinite (items::kInfiniteCount). That one used to be thrown out with the
// garbage: a new game with the Rocket Launcher unlocked gives Rebecca id 23 x
// 0xFFFF, and the whole inventory was gone for that session, Wesker mode and
// Once Again alike (`slot count 0 = 65535 is out of range`, every 5 s).
bool count_fits(int c) { return (c >= 0 && c <= 9999) || items::infinite(c); }

bool read_bag(uintptr_t base, Bag* b) {
  if (!mem::readable(base, 0x40)) return false;
  b->base = base;
  for (int i = 0; i < 6; ++i) {
    b->id[i] = mem::read<int32_t>(base + 4 + 8 * i);
    b->count[i] = mem::read<int32_t>(base + 8 + 8 * i);
    if (b->id[i] < 0 || b->id[i] > 255) return bag_rejected(base, "slot id", i, b->id[i]);
    if (!count_fits(b->count[i])) return bag_rejected(base, "slot count", i, b->count[i]);
  }
  b->personal_id = mem::read<int32_t>(base + 0x34);
  b->personal_count = mem::read<int32_t>(base + 0x38);
  b->equipped = mem::read<int32_t>(base + 0x3C);
  // The personal slot is the seventh entry of the same array (+0x04 + 8*6), so
  // it is an id/count pair like the other six (123/1 mixing set, 106/1 lighter)
  // and is held to the same range. It used to be the one count the panel could
  // draw that no slot reading would have been allowed to produce.
  if (b->personal_id < 0 || b->personal_id > 255) return bag_rejected(base, "personal id", 6, b->personal_id);
  if (!count_fits(b->personal_count)) return bag_rejected(base, "personal count", 6, b->personal_count);
  if (b->equipped < -1 || b->equipped > 6) return bag_rejected(base, "equipped index", -1, b->equipped);
  return true;
}

// --- how many of one item fit in one slot ------------------------------------------------
// One routine answers that for the whole game (exe+0xDC880), and every count
// the inventory deals with goes through it: the pickup that tops a stack up,
// the reload that asks how much room the equipped weapon has left, the item
// screen that greys a full slot out.
//
//   int item_max(int id) {
//     if (id >= 0x38) return (id == 0x5A || id == 0x5B) ? 10 : 1;
//     return table[id];              // 0x38 dwords in .rdata: 15 for a handgun,
//   }                                // 255 for a box of ammo, 1 for a herb
//
// The tail is what a hand-written list gets wrong. Every id from 0x38 up looks
// like a key item that does not stack, and the two that are not - the blue and
// green leech charms Leech Hunter is played with - stack ten to a slot. Held at
// 1, a typed count was silently clamped to one charm and a ten-charm stack came
// back out of the storage box as a single charm.
//
// The threshold, the two ids, their value, the default and the table's address
// are all read out of the routine, and the table is copied once: it is const
// .rdata in an image that does not move, and the alternative is a VirtualQuery
// per item per frame.
constexpr int kItemMaxTableMax = 256;
int g_item_max_tab[kItemMaxTableMax] = {};
int g_item_max_n = 0;          // entries the table has (the routine's threshold)
int g_item_max_default = 0;    // what an id at or above it gets
int g_item_max_special_id[2] = {-1, -1};
int g_item_max_special = 0;    // what those two get instead

void find_item_max() {
  //  mov eax,[esp+4]; cmp eax,<thresh>; jl .table; cmp eax,<id1>; je .special;
  //  cmp eax,<id2>; je .special; mov eax,<default>; ret 4;
  //  .special: mov eax,<value>; ret 4; .table: mov eax,[eax*4+<table>]; ret 4
  const std::vector<uintptr_t> hits = mem::find_all(
      image::text(),
      "8B 44 24 04 83 F8 ?? 7C ?? 83 F8 ?? 74 ?? 83 F8 ?? 74 ?? B8 ?? ?? ?? ?? C2 04 00 B8 ?? ?? ?? ?? C2 04 00 "
      "8B 04 85 ?? ?? ?? ?? C2 04 00",
      4);
  if (hits.size() != 1) {
    logf("items: the game's per-slot maximum routine was not found uniquely (%u matches) - the mod's own item table "
         "is used instead",
         static_cast<unsigned>(hits.size()));
    return;
  }
  const uintptr_t h = hits[0];
  const int thresh = mem::read<uint8_t>(h + 6);
  const int id1 = mem::read<uint8_t>(h + 11), id2 = mem::read<uint8_t>(h + 16);
  const uint32_t def = mem::read<uint32_t>(h + 20), special = mem::read<uint32_t>(h + 28);
  const uintptr_t table = mem::read<uint32_t>(h + 38);
  bool in_data = false;
  for (const mem::Range& r : image::data_sections())
    if (r.contains(table) && r.contains(table + 4u * static_cast<unsigned>(thresh))) in_data = true;
  if (thresh < 8 || thresh > kItemMaxTableMax || id1 < thresh || id2 < thresh || def < 1 || def > items::kMaxCount ||
      special < 1 || special > 0xFFFF || !in_data || !mem::readable(table, 4u * static_cast<unsigned>(thresh))) {
    logf("items: the per-slot maximum routine at exe+0x%06X does not read as one (ids 0..%d from exe+0x%06X, %u for "
         "%d/%d, %u otherwise) - the mod's own item table is used instead",
         image::rva(h), thresh - 1, image::rva(table), special, id1, id2, def);
    return;
  }
  for (int i = 0; i < thresh; ++i) {
    const uint32_t v = mem::read<uint32_t>(table + 4u * static_cast<unsigned>(i));
    // 0xFFFF is the game's "this item has no meaningful count" (the knife); the
    // panel's own ceiling stands in for it, as it does for an id the table has
    // nothing for.
    g_item_max_tab[i] = v == 0 || v > static_cast<uint32_t>(items::kMaxCount) ? 0 : static_cast<int>(v);
  }
  g_item_max_n = thresh;
  g_item_max_default = static_cast<int>(def);
  g_item_max_special_id[0] = id1;
  g_item_max_special_id[1] = id2;
  g_item_max_special = special > static_cast<uint32_t>(items::kMaxCount) ? items::kMaxCount : static_cast<int>(special);
  const int ink = items::kInkRibbonId < thresh ? g_item_max_tab[items::kInkRibbonId] : 0;
  logf("items: one slot holds table[id] of ids 0..%d (exe+0x%06X, %d ink ribbons), %d of id %d (%s) and id %d (%s), "
       "%d of every other id (routine exe+0x%06X)",
       thresh - 1, image::rva(table), ink, g_item_max_special, id1, items::name(id1), id2, items::name(id2),
       g_item_max_default, image::rva(h));
}

// --- which items stack ----------------------------------------------------------------------
// A pickup joins a slot that already holds the same item only for the few items
// the game stacks, and one small routine (exe+0xDD470) names them: it hands each
// one its row of the pickup table (exe+0x8C69E0) and every other item -1, and
// the pickup (Bag::add, exe+0xDC070) gives up on stacking at a -1 before it looks
// at a single slot.
//
//   int stack_row(int id) {
//     if (id == 0x0E) return 0;               // Molotov Cocktail
//     if (id - 0x20 <= 8u) return id - 0x1F;  // the ammo boxes, bottle, gas tank, machine gun ammo
//     if (id == 0x37) return 0x0B;            // Ink Ribbon
//     if (id == 0x5A) return 0x0C;            // Blue Leech Charm
//     return id == 0x5B ? 0x0D : -1;          // Green Leech Charm; nothing else stacks
//   }
//
// Every other item - a weapon, whose count is its magazine, a herb, a key -
// takes a slot of its own for each one, so two of them are two things and
// adding their counts together would destroy one. The ids are read out of the
// routine; how many one stack holds is item_max's answer, which the pickup
// table's own numbers agree with for every one of them.
bool g_stacks[kItemMaxTableMax] = {};
int g_stacks_n = 0;  // ids the routine names (0 = not read; items.h stands in)

void find_stack_rows() {
  //  mov ecx,[esp+4]; cmp ecx,<a>; jne; xor eax,eax; ret 4;
  //  lea eax,[ecx-<lo>]; cmp eax,<span>; ja; lea eax,[ecx+<d>]; ret 4;
  //  cmp ecx,<b>; jne; lea eax,[ecx+<d>]; ret 4;
  //  cmp ecx,<c>; jne; mov eax,<row>; ret 4;
  //  or eax,-1; mov edx,<row>; cmp ecx,<d>; cmove eax,edx; ret 4
  const std::vector<uintptr_t> hits = mem::find_all(
      image::text(),
      "8B 4C 24 04 83 F9 ?? 75 05 33 C0 C2 04 00 8D 41 ?? 83 F8 ?? 77 06 8D 41 ?? C2 04 00 83 F9 ?? 75 06 8D 41 ?? "
      "C2 04 00 83 F9 ?? 75 08 B8 ?? ?? ?? ?? C2 04 00 83 C8 FF BA ?? ?? ?? ?? 83 F9 ?? 0F 44 C2 C2 04 00",
      4);
  if (hits.size() != 1) {
    logf("items: the game's list of the items that stack was not found uniquely (%u matches) - the mod's own list is "
         "used instead",
         static_cast<unsigned>(hits.size()));
    return;
  }
  const uintptr_t h = hits[0];
  // Every compare and displacement in it is a sign-extended byte.
  const auto s8 = [h](int off) { return static_cast<int>(mem::read<int8_t>(h + off)); };
  struct Named {
    int id, row;
  };
  std::vector<Named> named = {
      {s8(6), 0},
      {s8(30), s8(30) + s8(35)},
      {s8(41), mem::read<int32_t>(h + 45)},
      {s8(62), mem::read<int32_t>(h + 56)},
  };
  const int lo = -s8(16), span = s8(19), d = s8(24);
  const bool span_ok = span >= 0 && span < 32;
  for (int k = 0; span_ok && k <= span; ++k) named.push_back({lo + k, lo + k + d});
  // Each one a real item with a row of its own, and each one an item a slot
  // holds more than one of: a stacking item that fits one to a slot, or a row
  // two items share, would be a misreading.
  bool ok = span_ok;
  bool seen[kItemMaxTableMax] = {};
  uint64_t rows = 0;
  for (const Named& n : named) {
    if (!ok) break;
    ok = n.id > 0 && n.id < kItemMaxTableMax && n.id != items::kFillerId && !seen[n.id] && n.row >= 0 && n.row < 64 &&
         !(rows >> n.row & 1) && item_max(n.id) > 1;
    if (ok) {
      seen[n.id] = true;
      rows |= uint64_t{1} << n.row;
    }
  }
  if (!ok) {
    logf("items: the routine at exe+0x%06X does not read as the game's list of the items that stack - the mod's own "
         "list is used instead",
         image::rva(h));
    return;
  }
  for (const Named& n : named) g_stacks[n.id] = true;
  g_stacks_n = static_cast<int>(named.size());
  char list[480];
  int p = 0;
  for (int id = 1; id < kItemMaxTableMax && p < static_cast<int>(sizeof(list)); ++id)
    if (g_stacks[id]) p += std::snprintf(list + p, sizeof(list) - p, "%s%s %d", p ? ", " : "", items::name(id), item_max(id));
  logf("items: %d items stack, each up to what one slot holds (routine exe+0x%06X): %s", g_stacks_n, image::rva(h),
       list);
}

void hexdump(const char* what, uintptr_t at, size_t n) {
  if (!mem::readable(at, n)) return;
  for (size_t off = 0; off < n; off += 32) {
    char line[200];
    int p = std::snprintf(line, sizeof(line), "%s+0x%03X:", what, static_cast<unsigned>(off));
    for (size_t i = off; i < off + 32 && i < n; i += 4)
      p += std::snprintf(line + p, sizeof(line) - p, " %08X", mem::read<uint32_t>(at + i));
    logf("%s", line);
  }
}

// --- status object (save count + play time) ---------------------------------------------
constexpr int kOffSaveCount = 0x38, kOffPlaytime = 0x3C;
struct StatusCandidate {
  char name[32];
  uintptr_t obj;
  float first_v, last_v;
  DWORD first_t;
  bool rejected;
};
StatusCandidate g_cands[24];
int g_ncands = 0;
uintptr_t g_status_obj = 0;
char g_status_cls[32] = {};
float g_rate = 0.0f;
bool g_rate_confirmed = false;
DWORD g_cand_refresh = 0;
// The play-time updater (sGameInfo's) clamps +0x3C to 10799999 = 99:59:59 at
// 30 units per second; the live measurement below confirms it.
constexpr float kExpectedRate = 30.0f;

void add_candidate(const char* name, uintptr_t obj) {
  if (!obj || g_ncands >= 24) return;
  for (int i = 0; i < g_ncands; ++i)
    if (g_cands[i].obj == obj) return;
  StatusCandidate& c = g_cands[g_ncands++];
  std::snprintf(c.name, sizeof(c.name), "%s", name);
  c.obj = obj;
  c.first_v = c.last_v = 0.0f;
  c.first_t = 0;
  c.rejected = false;
}

void refresh_candidates() {
  const DWORD now = GetTickCount();
  if (now - g_cand_refresh < 5000) return;
  g_cand_refresh = now;
  if (!g_status_obj) {
    // sGameInfo: its save-count getter is `lea eax,[ecx+38]`, the save routine
    // increments through it, and its play-time updater stores to +0x3C.
    if (const uintptr_t gi = single("sGameInfo")) {
      g_status_obj = gi;
      g_rate = kExpectedRate;
      std::snprintf(g_status_cls, sizeof(g_status_cls), "sGameInfo");
      int sc = 0;
      float pt = 0.0f;
      mem::read_safe(gi + kOffSaveCount, &sc);
      mem::read_safe(gi + kOffPlaytime, &pt);
      logf("status object: sGameInfo %p (save count %d, play time %.1f raw = %.0f s at the expected 30/s; measuring)",
           reinterpret_cast<void*>(gi), sc, pt, pt / kExpectedRate);
    }
  }
  for (const Single& s : g_singles) {
    const uintptr_t o = s.found ? dti::resolve(*s.cls, s.s) : 0;
    if (o) add_candidate(s.name, o);
  }
  // cSaveData objects pointed to from inside sSavedata (mpSavedata) or the other singletons.
  if (const dti::ClassInfo* sd = dti::find_class("cSaveData")) {
    if (sd->vtable) {
      auto is_save = [&](uintptr_t p) {
        uint32_t vt = 0;
        return mem::read_safe(p, &vt) && vt == sd->vtable;
      };
      for (const Single& s : g_singles) {
        const uintptr_t o = s.found ? dti::resolve(*s.cls, s.s) : 0;
        if (!o) continue;
        uintptr_t found[4] = {};
        const int n = find_ptr_fields(o, 0x100, is_save, found, 4);
        for (int i = 0; i < n; ++i) {
          char nm[32];
          std::snprintf(nm, sizeof(nm), "%s->cSaveData", s.name);
          add_candidate(nm, found[i]);
        }
      }
    }
  }
}

// Watch every candidate's +0x3C float while playing: the game clock advances
// at a fixed rate, and the candidate that does so is the status object. The
// rate (units per second) doubles as the unit for the editor.
void validate_status(bool in_game) {
  if (g_rate_confirmed || !in_game) return;
  const DWORD now = GetTickCount();
  for (int i = 0; i < g_ncands; ++i) {
    StatusCandidate& c = g_cands[i];
    if (c.rejected) continue;
    if (g_status_obj && c.obj != g_status_obj) continue;  // only confirm the chosen object's rate
    float v = 0.0f;
    if (!mem::read_safe(c.obj + kOffPlaytime, &v) || !std::isfinite(v) || v < 0.0f || v > 1.0e8f) {
      c.rejected = true;
      continue;
    }
    if (!c.first_t || v < c.last_v - 0.5f) {  // start (or restart after a jump backwards: a load)
      c.first_t = now;
      c.first_v = v;
      c.last_v = v;
      continue;
    }
    c.last_v = v;
    const DWORD dt = now - c.first_t;
    if (dt < 4000) continue;
    const float rate = (v - c.first_v) * 1000.0f / static_cast<float>(dt);
    float unit = 0.0f;
    if (rate > 25.0f && rate < 35.0f) unit = 30.0f;
    else if (rate > 55.0f && rate < 65.0f) unit = 60.0f;
    else if (rate > 0.9f && rate < 1.1f) unit = 1.0f;
    else if (rate > 950.0f && rate < 1050.0f) unit = 1000.0f;
    if (unit > 0.0f) {
      if (g_status_obj && unit != g_rate) logf("status object: measured %g units/s, expected %g - using the measured rate", unit, g_rate);
      g_status_obj = c.obj;
      g_rate = unit;
      g_rate_confirmed = true;
      std::snprintf(g_status_cls, sizeof(g_status_cls), "%s", c.name);
      int sc = 0;
      mem::read_safe(c.obj + kOffSaveCount, &sc);
      logf("status object: %s %p confirmed (+0x3C advances %.1f/s -> %g units per second; +0x38 save count = %d)", c.name,
           reinterpret_cast<void*>(c.obj), rate, unit, sc);
      return;
    }
    // Not a clock: restart the window so a candidate that starts moving later still gets its chance.
    c.first_t = now;
    c.first_v = v;
  }
}

// --- countdown timer -----------------------------------------------------------------------
// The scripted countdowns (the train's brakes, and every other timed section)
// are one float in sEventScript at +0x2C, counted in the same 1/30 s units as
// the play-time clock: the HUD element uGUITimer turns it into m:ss.cc with
// the engine's own 1/1800 and 1/30 constants, and the per-frame steppers
// subtract 30/fps * timescale from it. -1 means "no timer": one script handler
// sets the value when a timed section starts, another puts the sentinel back
// when it ends (see CLAUDE.md - the command table's names do not line up with
// those two handlers, so nothing here is anchored on a command name).
constexpr int kOffCountdown = 0x2C;
constexpr float kCountdownRate = 30.0f;
constexpr float kCountdownMax = (99.0f * 60.0f + 59.0f) * kCountdownRate;  // 99:59, all the HUD's m:ss can show

uintptr_t countdown_object() {
  const uintptr_t o = single("sEventScript");
  return o && mem::readable(o + kOffCountdown, 4) ? o : 0;
}

// --- save files -----------------------------------------------------------------------------
// The six methods that ask the save system to do something all have the same
// shape, so the field offsets and the operation numbers are read out of the
// game's own code rather than assumed:
//   cmp [ecx+mode],0; jne busy; cmp [ecx+state],0; jne busy;   (the guard)
//   [mov [ecx+op],arg1; [mov [ecx+arg2],arg2;]]
//   mov [ecx+result],0; mov [ecx+mode],<M>; mov [ecx+state],1; mov al,1; ret
// M is 3 for a save, 4 for a save that copies another file over this one and 5
// for a load; 1, 2 and 6 are whole-file operations that carry no file number.
// The task that runs the request then steps state 1 -> 2 (the disk) -> 0.
//
// The file records themselves are only used to check that the object found is
// the right one: the routine that applies one file to the game starts with
//   cmp esi,<slots-1>; ja out; imul esi,esi,<stride>; lea eax,[edi+<records B>];
//   add eax,esi; push <stride>; push eax; lea eax,[edi+<records A>]; add eax,esi; push eax
// which gives the number of save files, the size of a record and where the two
// record arrays sit inside the object.
struct SaveFields {
  bool ok = false;         // the request methods were found and agree
  bool layout_ok = false;  // the record array bounds were found too
  int mode = 0x04, state = 0x08, result = 0x0C, op = 0x10;
  int mode_save = 3, mode_copy = 4, mode_load = 5;
  int arg2 = -1;   // the save request's second argument (-1 = not found)
  int slots = 20;  // the file list's length; replaced by the derived one below
  uint32_t rec_a = 0, rec_b = 0, stride = 0;
};
SaveFields g_save;
uintptr_t g_save_obj = 0;
DWORD g_save_scan = 0;
bool g_save_missing_warned = false;  // sSaveManager has no cSaveManager yet
bool g_save_shape_warned = false;    // the object does not read like the state machine

void find_save_fields() {
  const std::vector<uintptr_t> hits =
      mem::find_all(image::text(), "C7 41 ?? 00 00 00 00 C7 41 ?? ?? 00 00 00 C7 41 ?? 01 00 00 00 B0 01", 16);
  SaveFields f;
  int found = 0, modes = 0;
  char list[128] = {};
  for (uintptr_t h : hits) {
    const int result = mem::read<uint8_t>(h + 2), mode = mem::read<uint8_t>(h + 9);
    const int value = mem::read<uint8_t>(h + 10), state = mem::read<uint8_t>(h + 16);
    // The guard at the top of the same method names the same two fields; it is
    // what tells a request method apart from any other pair of stores.
    char guard[64];
    std::snprintf(guard, sizeof(guard), "83 79 %02X 00 75 ?? 83 79 %02X 00 75 ??", mode, state);
    const uintptr_t head = mem::find_pattern(mem::Range{h - 0x40, h}, guard);
    if (!head) continue;
    if (found && (result != f.result || mode != f.mode || state != f.state)) {
      logf("saves: request methods disagree about the field layout - save/load watching off");
      return;
    }
    f.result = result;
    f.mode = mode;
    f.state = state;
    ++found;
    modes |= 1 << (value & 31);
    std::snprintf(list + std::strlen(list), sizeof(list) - std::strlen(list), " %d@exe+0x%06X", value, image::rva(head));
    // The ones that take a file number store it before the guard's fields;
    // the save stores its second argument after it.
    if (const uintptr_t arg = mem::find_pattern(mem::Range{head, h}, "89 41 ??")) f.op = mem::read<uint8_t>(arg + 2);
    if (value == f.mode_save)
      if (const uintptr_t arg = mem::find_pattern(mem::Range{head, h}, "8B 44 24 08 89 41 ??")) f.arg2 = mem::read<uint8_t>(arg + 6);
  }
  const int want = (1 << f.mode_save) | (1 << f.mode_copy) | (1 << f.mode_load);
  if (found < 3 || (modes & want) != want) {
    logf("saves: %d request method(s) found (modes mask 0x%X) - save/load watching off", found, modes);
    return;
  }
  f.ok = true;
  logf("saves: request methods:%s -> mode+0x%X state+0x%X result+0x%X file+0x%X", list, f.mode, f.state, f.result, f.op);

  const std::vector<uintptr_t> apply = mem::find_all(
      image::text(),
      "83 FE ?? 0F 87 ?? ?? ?? ?? 69 F6 ?? ?? ?? ?? 8D 87 ?? ?? ?? ?? 03 C6 68 ?? ?? ?? ?? 50 8D 87 ?? ?? ?? ?? 03 C6 50",
      4);
  if (apply.size() == 1) {
    const uintptr_t a = apply[0];
    const int slots = mem::read<uint8_t>(a + 2) + 1;
    const uint32_t stride = mem::read<uint32_t>(a + 11);
    const uint32_t rec_b = mem::read<uint32_t>(a + 17), rec_a = mem::read<uint32_t>(a + 31);
    const bool sane = slots > 0 && slots <= 64 && stride >= 0x100 && stride <= 0x400000 && rec_b > rec_a &&
                      rec_b - rec_a >= static_cast<uint32_t>(slots) * stride;
    if (sane) {
      f.slots = slots;
      f.stride = stride;
      f.rec_a = rec_a;
      f.rec_b = rec_b;
      f.layout_ok = true;
      logf("saves: %d save files of 0x%X bytes (records at +0x%X and +0x%X, from exe+0x%06X)", slots, stride, rec_a, rec_b,
           image::rva(a));
    } else {
      logf("saves: the file-apply routine at exe+0x%06X does not describe a sane record array (%d x 0x%X) - "
           "the object will only be size-checked",
           image::rva(a), slots, stride);
    }
  } else {
    logf("saves: the file-apply routine was not found uniquely (%u matches) - the object will only be size-checked",
         static_cast<unsigned>(apply.size()));
  }
  g_save = f;
}

// --- the save point, and the cleared-game save ------------------------------------------------
// Every save records which save point it was made at: a byte in sSaveManager
// that the typewriter's script sets, that the capture routine copies into the
// record at +0xCC, and that the save screen turns into its background
// (ui\13_save\tex\type0N_ID_HQ, asserting "the save point value is invalid" on
// 0). The ending is the one place that does not use a typewriter's number: it
// zeroes the save count and writes its own value there before handing over to
// the Save phase (exe+0x17F4EF..0x17F544), and that value appears nowhere else
// in .text. So a save carrying it is the cleared-game save.
//
// Both the field and that value are read out of the two sites that store an
// immediate byte into it behind the singleton's is_initialized() check - the
// ending's, and the title screen's, which puts a 0 back. They must agree about
// the offset, and exactly one of them must write something other than 0.
int g_point_off = -1;
int g_point_clear = -1;

void find_save_point() {
  const Single* sm = single_entry("sSaveManager");
  const uintptr_t slot = sm && sm->found ? sm->s.slot : 0;
  if (!slot) {
    logf("saves: sSaveManager has no static slot to anchor on - the cleared-game save cannot be told apart");
    return;
  }
  //  mov eax,[slot]; test eax,eax; jne +; <is_initialized() assert>; mov eax,[esp+..]; add esp,0xC;
  //  mov byte [eax+<off>],<value>
  const std::vector<uintptr_t> all = mem::find_all(
      image::text(),
      "A1 ?? ?? ?? ?? 85 C0 75 1B 68 ?? ?? ?? ?? 68 ?? ?? ?? ?? 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 44 24 ?? 83 C4 0C C6 40 ?? ??",
      64);
  int off = -1, value = -1, sites = 0, marks = 0;
  uintptr_t mark = 0;
  for (uintptr_t h : all) {
    if (mem::read<uint32_t>(h + 1) != slot) continue;  // a different singleton's field
    const int o = mem::read<uint8_t>(h + 38), v = mem::read<uint8_t>(h + 39);
    ++sites;
    if (off < 0) off = o;
    else if (o != off) {
      logf("saves: the save-point writers disagree about the field (+0x%X vs +0x%X) - the cleared-game save cannot be told apart",
           off, o);
      return;
    }
    if (v != 0) {
      ++marks;
      value = v;
      mark = h;
    }
  }
  if (sites < 2 || marks != 1) {
    logf("saves: %d save-point writer(s) with %d value(s) other than 0 - the cleared-game save cannot be told apart", sites,
         marks);
    return;
  }
  g_point_off = off;
  g_point_clear = value;
  logf("saves: save point at sSaveManager+0x%X; the ending writes %d there (exe+0x%06X), which marks a cleared-game save", off,
       value, image::rva(mark));
}

// The live cSaveManager: the one pointer inside sSaveManager that points at an
// object with that class's vtable and is big enough to hold the file records.
uintptr_t save_object() {
  if (!g_save.ok) return 0;
  static const dti::ClassInfo* cls = nullptr;
  if (!cls) cls = dti::find_class("cSaveManager");
  if (!cls || !cls->vtable) return 0;
  uint32_t vt = 0;
  if (g_save_obj && mem::read_safe(g_save_obj, &vt) && vt == cls->vtable) return g_save_obj;
  g_save_obj = 0;
  const DWORD now = GetTickCount();
  if (now - g_save_scan < 1000) return 0;
  g_save_scan = now;
  const uintptr_t sm = single("sSaveManager");
  if (!sm) return 0;
  size_t need = static_cast<size_t>(cls->size_word & 0x7FFFFFu) * 4;  // the DTI's own size, in dwords
  if (g_save.layout_ok) need = g_save.rec_b + static_cast<size_t>(g_save.slots) * g_save.stride;
  if (need < 0x1000 || need > 0x4000000) need = 0x1000;
  for (uintptr_t off = 0; off + 4 <= 0x40; off += 4) {
    uint32_t p = 0;
    if (!mem::read_safe(sm + off, &p) || p < 0x10000 || (p & 3)) continue;
    if (!mem::read_safe(p, &vt) || vt != cls->vtable) continue;
    if (!mem::readable(p, need)) {
      logf("saves: cSaveManager %p at sSaveManager+0x%X is not %u bytes of readable memory - ignored",
           reinterpret_cast<void*>(p), static_cast<unsigned>(off), static_cast<unsigned>(need));
      continue;
    }
    g_save_obj = p;
    logf("saves: cSaveManager %p at sSaveManager+0x%X (%u bytes)", reinterpret_cast<void*>(p), static_cast<unsigned>(off),
         static_cast<unsigned>(need));
    return p;
  }
  if (!g_save_missing_warned) {
    g_save_missing_warned = true;
    logf("saves: sSaveManager %p holds no cSaveManager yet - save/load watching idle", reinterpret_cast<void*>(sm));
  }
  return 0;
}

// --- the save files themselves ---------------------------------------------------------------
// Every file is kept twice (see game.h): the first array of records is what the
// game plays from and what its file list shows, the second is what data0.bin is
// read into and written from - the routine that names the file's sections
// ("VERSION", "SYSTEMDATA", "GAMEDATA%d", exe+0x213640) points them all into
// the second one. A save captures the game into the first copy and copies it
// over the second before the write (exe+0x2136D0); a load reads the whole file
// into the second copy and copies the file it wants across (exe+0x2127D0).
//
// A file is in use when the save-point byte of its record is set. The capture
// copies sSaveManager's save point there, the routine that lays a new data0.bin
// out writes 0 there for every file, and the file list's own test - the one
// that prints NO DATA and refuses a load (exe+0x1D28C0) - is that byte and
// nothing else. So the game's own "empty" is a record whose save point is 0.
//
// Nothing here reaches the disk by itself. The save state machine has a save
// that captures only the system data and then writes data0.bin whole from the
// second copy - the one a typewriter save chases the file with - and asking for
// it is the same five field writes the game's save request makes.
struct RecordFields {
  bool ok = false;
  int point = -1;      // the save-point byte inside a record
  int wesker = -1;     // the Wesker-mode flag byte (-1 = not found)
  int saves = -1;      // sGameInfo's save count inside a record (-1 = not found)
  int playtime = -1;   // sGameInfo's play time inside a record
  int system_op = -1;  // the op of the save that writes only the system data - and with it the whole file
};
RecordFields g_rec;

void find_record_fields() {
  if (!g_save.ok || !g_save.layout_ok || g_point_off < 0) {
    logf("saves: no record layout or save point to go on - the load list cannot delete or copy save files");
    return;
  }
  const Single* sm = single_entry("sSaveManager");
  const uintptr_t sm_slot = sm && sm->found ? sm->s.slot : 0;
  RecordFields f;
  // The capture routine opens with the record's header:
  //   imul edi,edi,<stride>; add edi,ecx; lea esi,[edi+<records>]; mov dword [esi],0;
  //   mov eax,[sSaveManager]; <is_initialized() assert>; mov al,[eax+<save point>]; mov [esi+<point>],al
  const std::vector<uintptr_t> cap = mem::find_all(
      image::text(),
      "69 FF ?? ?? ?? ?? 03 F9 8D B7 ?? ?? ?? ?? C7 06 00 00 00 00 A1 ?? ?? ?? ?? 85 C0 75 1B 68 ?? ?? ?? ?? "
      "68 ?? ?? ?? ?? 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 44 24 ?? 83 C4 0C 8A 40 ?? 88 46 ??",
      4);
  if (cap.size() != 1) {
    logf("saves: the capture routine was not found uniquely (%u matches) - the load list cannot delete or copy save "
         "files",
         static_cast<unsigned>(cap.size()));
    return;
  }
  const uintptr_t c = cap[0];
  const uint32_t stride = mem::read<uint32_t>(c + 2), rec = mem::read<uint32_t>(c + 10);
  const uint32_t slot = mem::read<uint32_t>(c + 21);
  const int from = mem::read<uint8_t>(c + 58), point = mem::read<uint8_t>(c + 61);
  if (stride != g_save.stride || rec != g_save.rec_a || (sm_slot && slot != sm_slot) || from != g_point_off ||
      point >= 0x40) {
    logf("saves: the capture routine at exe+0x%06X does not read as the one the layout came from (stride 0x%X, "
         "records +0x%X, save point +0x%X -> record+0x%X) - the load list cannot delete or copy save files",
         image::rva(c), stride, rec, from, point);
    return;
  }
  f.point = point;
  // The file list's own test reads that byte:
  //   imul esi,esi,<stride>; call <records: mov eax,[ecx+X]; add eax,Y; ret>; movzx ecx,bl;
  //   cmp byte [eax+esi+<disp>],0; mov edx,1; cmovne ecx,edx
  const std::vector<uintptr_t> tests = mem::find_all(
      image::text(), "69 F6 ?? ?? ?? ?? E8 ?? ?? ?? ?? 0F B6 CB 80 BC 30 ?? ?? ?? ?? 00 BA 01 00 00 00 0F 45 CA", 4);
  uintptr_t test = 0;
  for (uintptr_t h : tests) {
    const uintptr_t fn = h + 11 + static_cast<uintptr_t>(mem::read<int32_t>(h + 7));
    if (mem::read<uint32_t>(h + 2) != stride || !image::text().contains(fn) ||
        !mem::matches(fn, mem::parse_pattern("8B 41 ?? 83 C0 ?? C3")))
      continue;
    const int64_t at = static_cast<int64_t>(mem::read<int8_t>(fn + 5)) + mem::read<uint32_t>(h + 17);
    if (at == static_cast<int64_t>(rec) + point) test = h;
  }
  if (!test) {
    logf("saves: the file list's own \"is this file in use\" test was not found reading record+0x%X (%u candidates) "
         "- the load list cannot delete or copy save files",
         point, static_cast<unsigned>(tests.size()));
    return;
  }
  // Two more of the capture's stores, both only for the panel to show:
  //   call <sGameInfo: mov al,[ecx+X]; ret>; test al,al; setne al; mov [esi+<Wesker mode>],al
  //   mov ecx,[sGameInfo]; test ecx,ecx; je +C; lea eax,[edi+<block>]; push eax; call <serialise>
  // and the serialiser copies the save count and the play time with one movq:
  //   movq xmm0,[ecx+<save count>]; movq [edx+<at>],xmm0
  const mem::Range body{c, c + 0x300};
  if (const uintptr_t w = mem::find_pattern(body, "E8 ?? ?? ?? ?? 84 C0 0F 95 C0 88 46 ??")) {
    const uintptr_t fn = w + 5 + static_cast<uintptr_t>(mem::read<int32_t>(w + 1));
    if (image::text().contains(fn) && mem::matches(fn, mem::parse_pattern("8A 41 ?? C3")))
      f.wesker = mem::read<uint8_t>(w + 12);
  }
  if (const uintptr_t g = mem::find_pattern(body, "8B 0D ?? ?? ?? ?? 85 C9 74 0C 8D 87 ?? ?? ?? ?? 50 E8 ?? ?? ?? ??")) {
    const Single* gi = single_entry("sGameInfo");
    const uintptr_t gi_slot = gi && gi->found ? gi->s.slot : 0;
    const uintptr_t ser = g + 22 + static_cast<uintptr_t>(mem::read<int32_t>(g + 18));
    const int block = static_cast<int>(mem::read<uint32_t>(g + 12)) - static_cast<int>(rec);
    const mem::Pattern shape =
        mem::parse_pattern("F3 0F 7E 41 ?? 8B 54 24 04 66 0F D6 02 8B 41 ?? 89 42 ?? F3 0F 7E 41 ?? 66 0F D6 42 ??");
    if ((!gi_slot || mem::read<uint32_t>(g + 2) == gi_slot) && image::text().contains(ser) && mem::matches(ser, shape) &&
        mem::read<uint8_t>(ser + 23) == kOffSaveCount && block > point && block + 0x40 < static_cast<int>(stride)) {
      f.saves = block + mem::read<uint8_t>(ser + 28);
      f.playtime = f.saves + (kOffPlaytime - kOffSaveCount);
    }
  }
  // The system-data save, in the save task (exe+0x213080):
  //   mov eax,[esi+<op>]; cmp eax,<system>; jne +9; mov ecx,esi; call <capture the system data>; jmp;
  //   test eax,eax; js; cmp eax,<files>; jge; cmp dword [esi+<mode>],<copy>; jne
  // It captures the system data, stamps the Steam user into it and writes the
  // whole second copy out, the twenty records included, like every save does.
  const std::vector<uintptr_t> st = mem::find_all(
      image::text(), "8B 46 ?? 83 F8 ?? 75 09 8B CE E8 ?? ?? ?? ?? EB ?? 85 C0 78 ?? 83 F8 ?? 7D ?? 83 7E ?? ?? 75 ??", 4);
  if (st.size() != 1) {
    logf("saves: the save task's system-data branch was not found uniquely (%u matches) - the load list cannot delete "
         "or copy save files",
         static_cast<unsigned>(st.size()));
    return;
  }
  const uintptr_t t = st[0];
  const int op = mem::read<uint8_t>(t + 2), sys = mem::read<uint8_t>(t + 5), files = mem::read<uint8_t>(t + 23);
  const int mode = mem::read<uint8_t>(t + 28), copy = mem::read<uint8_t>(t + 29);
  if (op != g_save.op || files != g_save.slots || mode != g_save.mode || copy != g_save.mode_copy || sys < files) {
    logf("saves: the save task at exe+0x%06X names its fields differently from the requests (op +0x%X, %d files, mode "
         "+0x%X, copy %d) - the load list cannot delete or copy save files",
         image::rva(t), op, files, mode, copy);
    return;
  }
  f.system_op = sys;
  f.ok = true;
  g_rec = f;
  logf("saves: a file is in use while record+0x%X is set (capture exe+0x%06X, the list's own test exe+0x%06X); op 0x%X "
       "is the system-data save that writes data0.bin whole (save task exe+0x%06X)",
       f.point, image::rva(c), image::rva(test), f.system_op, image::rva(t));
  logf("saves: record+0x%X is the Wesker-mode flag, +0x%X the save count, +0x%X the play time (-1 = not found)",
       f.wesker, f.saves, f.playtime);
}

// A record of one of the two copies, when the object holds it.
uintptr_t record_at(uintptr_t obj, bool second, int slot) {
  return obj + (second ? g_save.rec_b : g_save.rec_a) + static_cast<uintptr_t>(slot) * g_save.stride;
}

// --- the title screen's load list -----------------------------------------------------------
// The file list is a uGUISave (the screen the typewriter opens too). The title
// screen makes one for itself when it starts (uGUITitle's init, exe+0x1EDC40)
// and opens it for loading from its main menu; the object comes and goes with
// the title, and a typewriter makes one of its own, so it is only ever reached
// through the title area: sArea's stack of running areas -> aTitle ->
// uGUITitle -> uGUISave, each step checked by class.
//
// The list runs on a mode dword and, inside some modes, a state dword:
//   open(kind)  (exe+0x1D4AD0)  kind = the title's "load" or the typewriter's "save", mode 1, state 0
//   1..3  lay the screen out and check the file on disk; mode 3's state 2
//         rebuilds all six rows from the records, puts the cursor on
//         sSaveManager's current file and hands the list over:
//   4     the list, waiting for the player (mode 7 loads the file picked)
// The rows are text the list sets when it builds or scrolls them, not a live
// view of the records, so after a file changes the list is rebuilt through
// that one state - which only shows what is already shown, re-reads the
// records and puts the list back in mode 4.
struct ListFields {
  bool ok = false;
  int kind = -1, mode = -1, state = -1, active = -1, cursor = -1;  // the list's own fields
  int load_kind = -1;                         // what the title opens it with
  int browse = -1;                            // the mode it waits in for the player
  int rebuild_mode = -1, rebuild_state = -1;  // where it rebuilds its rows
  int current = -1;                           // sSaveManager's current file, which the rebuild puts the cursor on
  int title_gui = -1;                         // uGUITitle's field holding the list
  int area_count = -1, area_list = -1;        // sArea's stack of running areas
};
ListFields g_list;

// A dword as a pattern, for a signature built around a field already derived.
void pattern_dword(char* out, size_t n, uint32_t v) {
  std::snprintf(out, n, "%02X %02X %02X %02X", v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24);
}

void find_load_list() {
  const Single* sm = single_entry("sSaveManager");
  const uintptr_t sm_slot = sm && sm->found ? sm->s.slot : 0;
  if (!sm_slot || g_point_off < 0) {
    logf("load list: no sSaveManager slot or save point to anchor on - the panel stays off the title screen");
    return;
  }
  const dti::ClassInfo* gui = dti::find_class("uGUISave");
  const uint32_t gui_size = gui ? (gui->size_word & 0x7FFFFFu) * 4 : 0;
  ListFields f;
  // open(kind): mov eax,[ecx+<mode>]; test eax,eax; je +5; cmp eax,<closed>; jne +25; mov eax,[esp+4];
  //   or dword [ecx+C],0x400; mov [ecx+<kind>],eax; mov [ecx+<mode>],1; mov [ecx+<state>],0; ret 4
  const std::vector<uintptr_t> opens = mem::find_all(
      image::text(),
      "8B 81 ?? ?? ?? ?? 85 C0 74 05 83 F8 ?? 75 ?? 8B 44 24 04 81 49 ?? 00 04 00 00 89 81 ?? ?? ?? ?? "
      "C7 81 ?? ?? ?? ?? 01 00 00 00 C7 81 ?? ?? ?? ?? 00 00 00 00 C2 04 00",
      4);
  if (opens.size() != 1) {
    logf("load list: uGUISave's open() was not found uniquely (%u matches) - the panel stays off the title screen",
         static_cast<unsigned>(opens.size()));
    return;
  }
  const uintptr_t open = opens[0];
  f.mode = static_cast<int>(mem::read<uint32_t>(open + 2));
  f.kind = static_cast<int>(mem::read<uint32_t>(open + 28));
  f.state = static_cast<int>(mem::read<uint32_t>(open + 44));
  const auto fits = [gui_size](int off) { return off > 0 && (gui_size == 0 || static_cast<uint32_t>(off) + 4 <= gui_size); };
  if (static_cast<int>(mem::read<uint32_t>(open + 34)) != f.mode || !fits(f.mode) || !fits(f.kind) || !fits(f.state)) {
    logf("load list: open() at exe+0x%06X does not name its fields consistently - the panel stays off the title screen",
         image::rva(open));
    return;
  }
  // The title opens it for loading from its main menu, right after it clears
  // the save point (the second of find_save_point's two writers):
  //   mov eax,[sSaveManager]; <assert>; mov byte [eax+<save point>],0; mov ecx,[esi+<list>]; push <kind>; call open
  int titles = 0;
  for (uintptr_t h : mem::find_all(image::text(),
                                   "A1 ?? ?? ?? ?? 85 C0 75 1B 68 ?? ?? ?? ?? 68 ?? ?? ?? ?? 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
                                   "8B 44 24 ?? 83 C4 0C C6 40 ?? 00 8B 8E ?? ?? ?? ?? 6A ?? E8 ?? ?? ?? ??",
                                   16)) {
    if (mem::read<uint32_t>(h + 1) != sm_slot || mem::read<uint8_t>(h + 38) != g_point_off) continue;
    if (h + 53 + static_cast<uintptr_t>(mem::read<int32_t>(h + 49)) != open) continue;
    ++titles;
    f.title_gui = static_cast<int>(mem::read<uint32_t>(h + 42));
    f.load_kind = mem::read<uint8_t>(h + 47);
  }
  if (titles != 1 || f.title_gui <= 0 || f.title_gui > 0x1000) {
    logf("load list: %d place(s) where the title opens the list for loading - the panel stays off the title screen",
         titles);
    return;
  }
  // The update: cmp byte [esi+<active>],0; je; mov eax,[esi+<mode>]; dec eax; cmp eax,<modes-1>; ja; jmp [eax*4+<table>]
  char mode_bytes[16], state_bytes[16], pat[200];
  pattern_dword(mode_bytes, sizeof(mode_bytes), static_cast<uint32_t>(f.mode));
  pattern_dword(state_bytes, sizeof(state_bytes), static_cast<uint32_t>(f.state));
  std::snprintf(pat, sizeof(pat), "80 BE ?? ?? ?? ?? 00 74 ?? 8B 86 %s 48 83 F8 ?? 77 ?? FF 24 85 ?? ?? ?? ??", mode_bytes);
  const std::vector<uintptr_t> ups = mem::find_all(image::text(), pat, 4);
  if (ups.size() != 1) {
    logf("load list: uGUISave's update was not found uniquely (%u matches) - the panel stays off the title screen",
         static_cast<unsigned>(ups.size()));
    return;
  }
  f.active = static_cast<int>(mem::read<uint32_t>(ups[0] + 2));
  const int modes = mem::read<uint8_t>(ups[0] + 18) + 1;
  const uintptr_t table = mem::read<uint32_t>(ups[0] + 24);
  if (!fits(f.active) || modes < 4 || modes > 32 || !mem::readable(table, 4u * static_cast<unsigned>(modes))) {
    logf("load list: the update at exe+0x%06X does not read as a mode dispatch - the panel stays off the title screen",
         image::rva(ups[0]));
    return;
  }
  // Each mode's entry is `mov ecx,esi; pop esi; jmp <handler>`.
  const auto handler = [table, modes](int m) -> uintptr_t {
    if (m < 1 || m > modes) return 0;
    const uintptr_t e = mem::read<uint32_t>(table + 4u * static_cast<unsigned>(m - 1));
    if (!image::text().contains(e) || !mem::matches(e, mem::parse_pattern("8B CE 5E E9 ?? ?? ?? ??"))) return 0;
    return e + 8 + static_cast<uintptr_t>(mem::read<int32_t>(e + 4));
  };
  // The rebuild, and the hand-over that follows it:
  //   mov eax,[sSaveManager]; <assert>; mov eax,[eax+<current>]; mov [esi+<cursor>],eax; lea ecx,[eax-<n>];
  //   mov [esi+<scroll>],0; mov eax,[esi+<scroll>]; test ecx,ecx; cmovg eax,ecx; mov ecx,esi;
  //   mov [esi+<scroll>],eax; call <rows>; ... mov [esi+<mode>],<browse>; mov [esi+<state>],0
  uintptr_t rb = 0;
  int rbs = 0;
  for (uintptr_t h : mem::find_all(image::text(),
                                   "A1 ?? ?? ?? ?? 85 C0 75 1B 68 ?? ?? ?? ?? 68 ?? ?? ?? ?? 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
                                   "8B 44 24 ?? 83 C4 0C 8B 40 ?? 89 86 ?? ?? ?? ?? 8D 48 ?? C7 86 ?? ?? ?? ?? 00 00 00 00 "
                                   "8B 86 ?? ?? ?? ?? 85 C9 0F 4F C1 8B CE 89 86 ?? ?? ?? ?? E8 ?? ?? ?? ??",
                                   8)) {
    const uint32_t scroll = mem::read<uint32_t>(h + 50);
    if (mem::read<uint32_t>(h + 1) != sm_slot || mem::read<uint32_t>(h + 60) != scroll ||
        mem::read<uint32_t>(h + 73) != scroll)
      continue;
    rb = h;
    ++rbs;
  }
  if (rbs != 1) {
    logf("load list: the rebuild of the rows was not found uniquely (%d matches) - the panel stays off the title "
         "screen",
         rbs);
    return;
  }
  f.current = mem::read<uint8_t>(rb + 38);
  f.cursor = static_cast<int>(mem::read<uint32_t>(rb + 41));
  std::snprintf(pat, sizeof(pat), "C7 86 %s ?? 00 00 00 C7 86 %s 00 00 00 00", mode_bytes, state_bytes);
  const uintptr_t hand = mem::find_pattern(mem::Range{rb, rb + 0xC0}, pat);
  if (!hand || !fits(f.cursor)) {
    logf("load list: the rebuild at exe+0x%06X does not hand the list over - the panel stays off the title screen",
         image::rva(rb));
    return;
  }
  f.browse = mem::read<uint8_t>(hand + 6);
  // Which mode and state run it: that mode's handler switches on the state,
  //   sub esp,N; push esi; mov esi,ecx; mov eax,[esi+<state>]; cmp eax,<n-1>; ja; jmp [eax*4+<cases>]
  // and the case whose code the rebuild is in is the one to ask for.
  std::snprintf(pat, sizeof(pat), "83 EC ?? 56 8B F1 8B 86 %s 83 F8 ?? 0F 87 ?? ?? ?? ?? FF 24 85 ?? ?? ?? ??", state_bytes);
  const mem::Pattern sw = mem::parse_pattern(pat);
  int runs = 0;
  for (int m = 1; m <= modes; ++m) {
    const uintptr_t h = handler(m);
    if (!h || !mem::matches(h, sw)) continue;
    const uintptr_t cases = mem::read<uint32_t>(h + 24);
    const int n = mem::read<uint8_t>(h + 14) + 1;
    if (n > 32 || !mem::readable(cases, 4u * static_cast<unsigned>(n))) continue;
    int best = -1;
    uintptr_t best_at = 0;
    for (int k = 0; k < n; ++k) {
      const uintptr_t e = mem::read<uint32_t>(cases + 4u * static_cast<unsigned>(k));
      if (e <= rb && e > best_at) {
        best_at = e;
        best = k;
      }
    }
    if (best < 0 || rb - best_at >= 0x200) continue;
    f.rebuild_mode = m;
    f.rebuild_state = best;
    ++runs;
  }
  if (runs != 1 || f.browse == f.rebuild_mode || !handler(f.browse)) {
    logf("load list: %d mode(s) run the rebuild at exe+0x%06X (browsing is mode %d) - the panel stays off the title "
         "screen",
         runs, image::rva(rb), f.browse);
    return;
  }
  // sArea's stack of running areas (its slot 9, exe+0x357D60):
  //   cmp [ebx+<count>],esi; jbe; lea edi,[ebx+<list>]; mov ecx,[edi]; mov eax,[ecx]; call [eax+10];
  //   mov [esp+esi*4+C],eax; inc esi; lea edi,[edi+4]; cmp esi,[ebx+<count>]; jb
  const std::vector<uintptr_t> ar = mem::find_all(
      image::text(), "39 B3 ?? ?? ?? ?? 76 ?? 8D BB ?? ?? ?? ?? 8B 0F 8B 01 FF 50 ?? 89 44 B4 ?? 46 8D 7F 04 3B B3 ?? ?? ?? ?? 72 ??",
      4);
  if (ar.size() != 1 || mem::read<uint32_t>(ar[0] + 2) != mem::read<uint32_t>(ar[0] + 31) ||
      mem::read<uint32_t>(ar[0] + 10) <= mem::read<uint32_t>(ar[0] + 2) || mem::read<uint32_t>(ar[0] + 10) > 0x100000) {
    logf("load list: sArea's area stack was not found uniquely (%u matches) - the panel stays off the title screen",
         static_cast<unsigned>(ar.size()));
    return;
  }
  f.area_count = static_cast<int>(mem::read<uint32_t>(ar[0] + 2));
  f.area_list = static_cast<int>(mem::read<uint32_t>(ar[0] + 10));
  f.ok = true;
  g_list = f;
  logf("load list: uGUISave kind +0x%X, mode +0x%X, state +0x%X, active +0x%X, cursor +0x%X (open exe+0x%06X, update "
       "exe+0x%06X)",
       f.kind, f.mode, f.state, f.active, f.cursor, image::rva(open), image::rva(ups[0]));
  logf("load list: the title keeps it at uGUITitle+0x%X and opens it with kind %d; it waits for the player in mode %d, "
       "and mode %d state %d rebuilds its rows with the cursor on sSaveManager+0x%X (exe+0x%06X); sArea's areas at "
       "+0x%X, %s+0x%X",
       f.title_gui, f.load_kind, f.browse, f.rebuild_mode, f.rebuild_state, f.current, image::rva(rb), f.area_list,
       "count at ", f.area_count);
}

// The list object, found through the title area every time: a pointer is
// trusted only as far as the chain it was read from.
//
// The area system is registered by its concrete class. The game keeps an
// sGameArea - sArea's one child, with no fields of its own - in sArea's static
// slot (exe+0xA2D8C0), and a singleton is matched by its exact vtable, so the
// base class's name never finds it: the first version asked for sArea, never
// found the list, and the panel never came up on the load screen.
const dti::ClassInfo* g_cls_title_area = nullptr;
const dti::ClassInfo* g_cls_title_gui = nullptr;
const dti::ClassInfo* g_cls_list = nullptr;
int g_title_gui_off = -1;  // aTitle's field holding its uGUITitle (found once, by class)
DWORD g_title_scan = 0;

// Which link of the chain was missing the last time, logged whenever it
// changes: a link that never resolves otherwise leaves nothing in the log but
// a panel that does not come up.
const char* g_list_miss = "";
uintptr_t g_list_found = 0;

uintptr_t list_miss(const char* why) {
  if (std::strcmp(why, g_list_miss) != 0) {
    g_list_miss = why;
    logf("load list: %s", why);
  }
  g_list_found = 0;
  return 0;
}

bool has_vtable(uintptr_t p, const dti::ClassInfo* c) {
  uint32_t vt = 0;
  return c && c->vtable && p >= 0x10000 && !(p & 3) && mem::read_safe(p, &vt) && vt == c->vtable;
}

uintptr_t load_list_object() {
  if (!g_list.ok) return 0;
  if (!g_cls_title_area || !g_cls_title_gui || !g_cls_list) {
    static bool looked = false;
    if (looked) return 0;
    looked = true;
    g_cls_title_area = dti::find_class("aTitle");
    g_cls_title_gui = dti::find_class("uGUITitle");
    g_cls_list = dti::find_class("uGUISave");
    if (!g_cls_title_area || !g_cls_title_gui || !g_cls_list) {
      logf("load list: aTitle, uGUITitle or uGUISave has no vtable - the panel stays off the title screen");
      return 0;
    }
  }
  const uintptr_t area = single("sGameArea");
  uint32_t count = 0;
  if (!area) return list_miss("no sGameArea (the area system) yet");
  if (!mem::read_safe(area + static_cast<uintptr_t>(g_list.area_count), &count) || count > 16)
    return list_miss("sGameArea's area stack does not read as one");
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t a = 0;
    if (!mem::read_safe(area + static_cast<uintptr_t>(g_list.area_list) + 4u * i, &a) || !has_vtable(a, g_cls_title_area))
      continue;
    if (g_title_gui_off < 0) {
      // The title area makes its uGUITitle as it starts (exe+0x82E0) and keeps
      // it in one of its few fields; which one is found once, by class.
      const DWORD now = GetTickCount();
      if (now - g_title_scan < 1000) return 0;
      g_title_scan = now;
      const uint32_t span = (g_cls_title_area->size_word & 0x7FFFFFu) * 4;
      for (uint32_t off = 4; off + 4 <= span && off < 0x400 && g_title_gui_off < 0; off += 4) {
        uint32_t p = 0;
        if (mem::read_safe(a + off, &p) && has_vtable(p, g_cls_title_gui)) g_title_gui_off = static_cast<int>(off);
      }
      if (g_title_gui_off < 0) return list_miss("the title area holds no uGUITitle yet");
      logf("load list: aTitle %p keeps its uGUITitle at +0x%X", reinterpret_cast<void*>(a), g_title_gui_off);
    }
    uint32_t t = 0, g = 0;
    if (!mem::read_safe(a + static_cast<uintptr_t>(g_title_gui_off), &t) || !has_vtable(t, g_cls_title_gui))
      return list_miss("the title area holds no uGUITitle");
    if (!mem::read_safe(t + static_cast<uintptr_t>(g_list.title_gui), &g) || !has_vtable(g, g_cls_list))
      return list_miss("the title screen has not made its file list yet");
    if (g != g_list_found) {
      g_list_found = g;
      g_list_miss = "";
      logf("load list: the title's file list is uGUISave %p (uGUITitle %p, aTitle %p)", reinterpret_cast<void*>(g),
           reinterpret_cast<void*>(t), reinterpret_cast<void*>(a));
    }
    return g;
  }
  return list_miss("no title area among the running areas");
}

// --- code sites ---------------------------------------------------------------------------
struct SiteInfo {
  const char* name;
  const char* pattern;   // the signature (the site must be unique in .text)
  int patch_off;         // where the patched bytes start, relative to the match
  const char* expect;    // the bytes replaced, as a pattern
  int nop_len;           // how many there are
  mem::Patch patch;
  int matches = 0;
  const char* replace = nullptr;  // what is written instead (null: nop_len NOPs)
};
SiteInfo g_sites[kSiteCount] = {
    {"playtime store (movss [esi+3C],xmm0)", "F3 0F 11 46 3C 72", 0, "F3 0F 11 46 3C", 5, {}, 0},
    {"save count inc ([eax]++ after the getter call)", "E8 ?? ?? ?? ?? FF 00 8B 0D ?? ?? ?? ??", 5, "FF 00", 2, {}, 0},
    // ... and its take-back: the Save phase adds the save when it starts and, if the
    // save screen is cancelled, subtracts it again (when the count is above 0), so
    // with only the increment skipped every cancel would lower the count.
    {"save count take-back (dec ecx; mov [eax],ecx)",
     "83 78 0C 01 74 ?? 8B 0D ?? ?? ?? ?? 85 C9 75 1B 68 ?? ?? ?? ?? 68 8B 01 00 00 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
     "8B 4C 24 ?? 83 C4 0C E8 ?? ?? ?? ?? 8B 08 85 C9 7E 03 49 89 08",
     54, "49 89 08", 3, {}, 0},
    // Infinite ink ribbons. The two places a save takes its ribbon: take-by-id
    // called with (0x37, 1), each behind a "does the active character carry one"
    // guard. The call becomes what that guard's no-ribbon branch leaves behind:
    // the two arguments the callee would pop (`ret 8`) dropped with two `pop ecx`,
    // and -1 in eax (`or eax,-1`). The code after both calls ignores the result.
    {"ink ribbon take A (call take(0x37,1))",
     "6A 01 6A 37 8B CF E8 ?? ?? ?? ?? 50 8D 44 24 ?? 50 E8 ?? ?? ?? ?? 83 C4 04 8B C8 E8 ?? ?? ?? ?? 8B C8 E8 ?? ?? ?? ?? "
     "8B C8 E8 ?? ?? ?? ??",
     41, "E8 ?? ?? ?? ??", 5, {}, 0, "59 59 83 C8 FF"},
    {"ink ribbon take B (call take(0x37,1))",
     "6A 01 6A 37 8B CD E8 ?? ?? ?? ?? 50 8B CF E8 ?? ?? ?? ?? 8B C8 E8 ?? ?? ?? ??", 21, "E8 ?? ?? ?? ??", 5, {}, 0,
     "59 59 83 C8 FF"},
    // ... and the typewriter's own question: its save command looks the ribbon up
    // in the active character's bag and on -1 branches to "you need an ink
    // ribbon". Without that branch it asks "use a ribbon?" either way.
    {"typewriter ink ribbon check (je no ribbon)",
     "E8 ?? ?? ?? ?? 85 C0 74 ?? 6A 37 8B C8 E8 ?? ?? ?? ?? 50 8D 44 24 ?? 50 E8 ?? ?? ?? ?? 83 C4 04 8B C8 E8 ?? ?? ?? ?? "
     "8B C8 E8 ?? ?? ?? ?? 8B C8 E8 ?? ?? ?? ?? 83 F8 FF 74 ??",
     56, "74 ??", 2, {}, 0},
    // The countdown's two per-frame steppers. Both compute the same
    // 30/fps * timescale step the play-time clock uses and subtract it from
    // sEventScript+0x2C; they differ only in which registers the compiler
    // picked, so each needs its own signature. NOPping the store leaves the
    // comparison that follows it looking at the decremented value, which is
    // still positive - so the "timer expired" branch is never taken either.
    {"countdown step (movss [ecx+2C],xmm1)",
     "F3 0F 10 49 2C F3 0F 59 05 ?? ?? ?? ?? F3 0F 59 40 68 F3 0F 5C C8 0F 57 C0 0F 2F C1 F3 0F 11 49 2C", 28,
     "F3 0F 11 49 2C", 5, {}, 0},
    {"countdown step (movss [ecx+2C],xmm0)",
     "F3 0F 10 41 2C F3 0F 59 0D ?? ?? ?? ?? F3 0F 59 48 68 F3 0F 5C C1 0F 57 C9 0F 2F C8 F3 0F 11 41 2C", 28,
     "F3 0F 11 41 2C", 5, {}, 0},
};

// Leads from the community tables: only counted and logged, never acted on.
struct Lead {
  const char* what;
  const char* pattern;
};
const Lead kLeads[] = {
    {"HP setter (mov [ecx+1030],eax; ret)", "89 81 30 10 00 00 C2"},
    {"item count setter (mov [eax+4],esi; movzx eax,bx)", "89 70 04 0F B7 C3"},
    {"slot subtract (sub [ebx+esi*8+8],edi; pop edi)", "29 7C F3 08 5F"},
    {"inventory root capture (mov eax,[ecx]; call [eax+18]; mov eax,[esi+70])", "8B 01 FF 50 18 8B 46 70 25 80"},
};

void find_sites() {
  for (SiteInfo& s : g_sites) {
    const std::vector<uintptr_t> hits = mem::find_all(image::text(), s.pattern, 4);
    s.matches = static_cast<int>(hits.size());
    if (hits.size() == 1) {
      const uintptr_t at = hits[0] + s.patch_off;
      std::vector<uint8_t> bytes = mem::nops(s.nop_len);
      if (s.replace) {
        const mem::Pattern r = mem::parse_pattern(s.replace);
        bytes.clear();
        for (const int16_t b : r.bytes) bytes.push_back(static_cast<uint8_t>(b));
      }
      if (s.patch.prepare(at, s.expect, bytes, s.name))
        logf("site: %-48s exe+0x%06X (patch %d byte(s) at exe+0x%06X)", s.name, image::rva(hits[0]),
             static_cast<int>(bytes.size()), image::rva(at));
      else
        logf("ERROR: site: %s matched at exe+0x%06X but the bytes to patch do not fit", s.name, image::rva(hits[0]));
    } else if (hits.empty()) {
      logf("site: %-48s NOT FOUND (pattern %s)", s.name, s.pattern);
    } else {
      char list[160] = {};
      for (uintptr_t h : hits) std::snprintf(list + std::strlen(list), sizeof(list) - std::strlen(list), " exe+0x%06X", image::rva(h));
      logf("site: %-48s %d matches - not unique, not used:%s", s.name, s.matches, list);
    }
  }
  // The ink ribbon sites vouch for each other: both takes must call one and the
  // same function (take-by-id), and the typewriter's no-ribbon branch must land
  // where its no-unit branch does - on the "you need an ink ribbon" message.
  const auto drop = [](SiteInfo& s, const char* why) {
    logf("ERROR: site: %s %s - not used", s.name, why);
    s.patch = mem::Patch{};
  };
  const auto call_dest = [](uintptr_t a) { return a + 5 + static_cast<uintptr_t>(mem::read<int32_t>(a + 1)); };
  const auto jcc_dest = [](uintptr_t a) { return a + 2 + static_cast<uintptr_t>(static_cast<int32_t>(mem::read<int8_t>(a + 1))); };
  SiteInfo& take_a = g_sites[kSiteInkTakeA];
  SiteInfo& take_b = g_sites[kSiteInkTakeB];
  if (take_a.patch.prepared() && take_b.patch.prepared()) {
    const uintptr_t fa = call_dest(take_a.patch.at), fb = call_dest(take_b.patch.at);
    if (fa != fb) {
      drop(take_a, "does not call what the other take calls");
      drop(take_b, "does not call what the other take calls");
    } else {
      logf("site: both ink ribbon takes call take-by-id at exe+0x%06X", image::rva(fa));
    }
  }
  SiteInfo& check = g_sites[kSiteInkCheck];
  if (check.patch.prepared() && jcc_dest(check.patch.at - check.patch_off + 7) != jcc_dest(check.patch.at))
    drop(check, "branches somewhere other than its no-unit test does");
  // The save count's take-back must subtract from the counter the increment adds
  // to - both call the same getter (sGameInfo's lea eax,[ecx+38]), 11 bytes before
  // the patched dec - and is no use without the increment.
  SiteInfo& inc = g_sites[kSiteSaveCountInc];
  SiteInfo& back = g_sites[kSiteSaveCountBack];
  if (back.patch.prepared() && (!inc.patch.prepared() || call_dest(inc.patch.at - 5) != call_dest(back.patch.at - 11)))
    drop(back, "does not go through the getter the increment does");
  for (const Lead& l : kLeads) {
    const std::vector<uintptr_t> hits = mem::find_all(image::text(), l.pattern, 4);
    char list[160] = {};
    for (uintptr_t h : hits) std::snprintf(list + std::strlen(list), sizeof(list) - std::strlen(list), " exe+0x%06X", image::rva(h));
    logf("lead: %-64s %u match(es)%s", l.what, static_cast<unsigned>(hits.size()), list);
  }
}

// --- enemies ------------------------------------------------------------------------------------
// sGameChara keeps the live units in a table the EnemyDeath command's lookup walks:
//   push ebx; mov ebx,[esp+8]; push ebp; push esi; mov ebp,ecx; xor esi,esi; push edi;
//   lea edi,[ebp+OFF]; cmp esi,COUNT; jb ...   ...   inc esi; add edi,STRIDE; cmp esi,COUNT; jb ...
// Every entry starts with the unit pointer (0 = empty). HP is +0x1030 on any
// character unit (the same field the players use; EnemyDeath's alive check reads it).
int g_chara_off = -1, g_chara_stride = 0, g_chara_count = 0;
struct EnemySlot {
  uintptr_t obj = 0;
  bool is_enemy = false;
  bool has_pool = false;  // uEnemy43: a second health pool beside HP (below)
  const char* cls = "";
  DWORD checked = 0;
};
EnemySlot g_enemy_slots[128];

void find_chara_layout() {
  // Several tables share the lookup's shape; the unit table is the one the
  // EnemyDeath script command walks, so anchor on that handler's calls.
  const mem::Pattern shape = mem::parse_pattern("53 8B 5C 24 08 55 56 8B E9 33 F6 57 8D 7D ?? 83 FE ?? 72 14");
  script::Command death;
  if (!script::find("EnemyDeath", &death) || !mem::readable(death.handler, 0x300)) {
    logf("enemies: EnemyDeath command not found - one hit kills off");
    return;
  }
  uintptr_t fn = 0;
  for (uintptr_t a = death.handler; a < death.handler + 0x300 && !fn; ++a) {
    if (mem::read<uint8_t>(a) != 0xE8) continue;
    const uintptr_t target = a + 5 + static_cast<uintptr_t>(mem::read<int32_t>(a + 1));
    if (image::text().contains(target) && mem::matches(target, shape)) fn = target;
  }
  if (!fn) {
    logf("enemies: EnemyDeath (exe+0x%06X) calls no unit-table lookup of the expected shape - one hit kills off",
         image::rva(death.handler));
    return;
  }
  const int off = mem::read<uint8_t>(fn + 14);
  const int count = mem::read<uint8_t>(fn + 17);
  const uintptr_t tail = mem::find_pattern(mem::Range{fn, fn + 0x80}, "46 83 C7 ?? 83 FE ?? 72 CE");
  if (!tail || mem::read<uint8_t>(tail + 6) != count) {
    logf("enemies: unit-table lookup at exe+0x%06X has an unexpected loop shape - one hit kills off", image::rva(fn));
    return;
  }
  g_chara_off = off;
  g_chara_stride = mem::read<uint8_t>(tail + 3);
  g_chara_count = count > 128 ? 128 : count;
  logf("enemies: sGameChara unit table at +0x%X, %d entries of 0x%X bytes (lookup exe+0x%06X)", off, count, g_chara_stride,
       image::rva(fn));
}

// Most enemy classes are uEnemyXX < uEnemyBase, but not all of them: an enemy
// that wears a player model is built on the player class instead - in this
// build uEnemy3bRebecca is uPlayerBase's child (its code even reaches into
// pl_damage.inl), so the parent chain alone would leave it out of the list.
// The name is the other half of the test: prog\game\chara\enemy\uEnemyNN.cpp
// is what the class is, whoever it inherits from.
bool is_enemy_unit(uintptr_t p) {
  if (!mem::readable(p, kOffHp + 4)) return false;
  void* const obj = reinterpret_cast<void*>(p);
  if (dti::is_a(obj, "uEnemyBase")) return true;
  const char* const n = dti::class_name_of(obj);
  return n && !std::strncmp(n, "uEnemy", 6);
}

// Every other class that turns up in the unit table is logged once, so a unit
// the filter above walks past is visible in an ordinary log and not only under
// Trace. The players are in the table too and are not worth a line.
const char* g_other_class[16] = {};
int g_nother = 0;
void note_other_class(const char* name) {
  if (!name || !std::strncmp(name, "uPlayer", 7)) return;
  for (int i = 0; i < g_nother; ++i)
    if (!std::strcmp(g_other_class[i], name)) return;
  if (g_nother < 16) g_other_class[g_nother++] = name;
  logf("enemies: unit table also holds %s - not treated as an enemy", name);
}

// --- the leech-man's second health pool ---------------------------------------------------------
// One class keeps two of them: uEnemy43, the leech humanoid that collapses into
// a heap of leeches when it is shot enough. Its class init copies the HP the
// spawn table just handed it into a second field - `call getHp; mov [reg+POOL],
// eax` - and its damage handler routes every hit by where it landed: with bit 3
// of the unit's flags (+0x6840) set, only a hit to the top zone, or of kind
// 7/0xE/0x1A, subtracts from HP. Anything else drains POOL and never looks at
// HP, which is why holding HP at 1 left the thing standing however often it was
// shot. Draining POOL is not a death either - it collapses the form into its 18
// leeches - so both are held: whichever way the next hit is routed, it ends the
// form it lands on.
//
// The offset is read out of the code, not assumed: uEnemy43's vtable comes from
// its DTI, and the site is the one call to the HP getter inside its own methods
// whose result is stored straight back into the object.
const dti::ClassInfo* g_cls_leechman = nullptr;
int g_pool_off = -1;

void find_enemy_pool() {
  g_cls_leechman = dti::find_class("uEnemy43");
  if (!g_cls_leechman || !g_cls_leechman->vtable) {
    logf("enemies: uEnemy43 (the leech-man) has no vtable - its second health pool stays unknown");
    return;
  }
  // getHp itself, so the store that follows it is the one that matters.
  const std::vector<uintptr_t> getters = mem::find_all(image::text(), "8B 81 30 10 00 00 C3", 4);
  if (getters.size() != 1) {
    logf("enemies: the HP getter (mov eax,[ecx+0x%X]; ret) was not found uniquely (%u matches) - uEnemy43's second "
         "pool stays unknown",
         kOffHp, static_cast<unsigned>(getters.size()));
    return;
  }
  const uintptr_t get_hp = getters[0];
  const uint32_t obj_size = (g_cls_leechman->size_word & 0xFFFFFFu) * 4;
  int off = 0;
  uintptr_t site = 0;
  for (int slot = 0; slot < 80 && !off; ++slot) {
    uintptr_t fn = 0;
    if (!mem::read_safe(g_cls_leechman->vtable + static_cast<uintptr_t>(slot) * 4, &fn) || !image::text().contains(fn))
      continue;
    for (uintptr_t a = fn; a < fn + 0x400 && image::text().contains(a + 11); ++a) {
      if (mem::read<uint8_t>(a) != 0xE8) continue;
      if (a + 5 + static_cast<uintptr_t>(mem::read<int32_t>(a + 1)) != get_hp) continue;
      // mov [reg+disp32],eax: opcode 89, mod 10, reg field eax, any base but a SIB.
      const uint8_t modrm = mem::read<uint8_t>(a + 6);
      if (mem::read<uint8_t>(a + 5) != 0x89 || modrm < 0x80 || modrm > 0x87 || modrm == 0x84) continue;
      off = static_cast<int>(mem::read<uint32_t>(a + 7));
      site = a;
      break;
    }
  }
  if (off <= kOffHp || (obj_size && static_cast<uint32_t>(off) + 4 > obj_size)) {
    logf("enemies: uEnemy43's methods do not copy its HP into a second field (found 0x%X, object 0x%X) - one hit "
         "kills will not reach the leech-man's body damage",
         off, obj_size);
    return;
  }
  g_pool_off = off;
  logf("enemies: uEnemy43 keeps a second health pool at +0x%X (init store exe+0x%06X, vtable exe+0x%06X, object 0x%X)",
       g_pool_off, image::rva(site), image::rva(g_cls_leechman->vtable), obj_size);
}

// Cached per unit-table slot: is_a() walks a parent chain, and this is asked of
// every unit in the table.
bool unit_has_pool(uintptr_t p) {
  return g_pool_off > 0 && mem::readable(p, static_cast<size_t>(g_pool_off) + 4) &&
         dti::is_a(reinterpret_cast<void*>(p), "uEnemy43");
}

// --- pause -----------------------------------------------------------------------------------
// The play-time updater skips its increment while the game is paused:
//   call isPause; test al,al; jne skip; mov eax,[sMain]; cmp [eax+60],0 ...
// isPause is `mov ecx,[flags]; push <bit>; call testBit; ret`, and testBit is
// `mov edx,ecx; mov ecx,[esp+4]; mov eax,1; shl eax,cl; test [edx+disp32],eax;
// setne al; ret 4`. Reading that bit is the pause predicate.
uintptr_t g_pause_slot = 0;  // static slot holding the flags object
uint32_t g_pause_disp = 0;   // offset of the flags dword inside it
int g_pause_bit = -1;
uint32_t g_last_flags = 0xFFFFFFFFu;

void find_pause_flag() {
  const std::vector<uintptr_t> sites = mem::find_all(image::text(), "E8 ?? ?? ?? ?? 84 C0 75 ?? A1 ?? ?? ?? ?? 83 78 60 00 75 06 80 78 64 00", 4);
  if (sites.size() != 1) {
    logf("pause: the play-time updater's isPause call was not found uniquely (%u matches) - pause detection off",
         static_cast<unsigned>(sites.size()));
    return;
  }
  const uintptr_t is_pause = sites[0] + 5 + static_cast<uintptr_t>(mem::read<int32_t>(sites[0] + 1));
  if (!image::text().contains(is_pause) || !mem::matches(is_pause, mem::parse_pattern("8B 0D ?? ?? ?? ?? 6A ?? E8 ?? ?? ?? ?? C3"))) {
    logf("pause: isPause at exe+0x%06X does not have the expected shape - pause detection off", image::rva(is_pause));
    return;
  }
  const uintptr_t slot = mem::read<uint32_t>(is_pause + 2);
  const int bit = mem::read<uint8_t>(is_pause + 7);
  const uintptr_t test_bit = is_pause + 13 + static_cast<uintptr_t>(mem::read<int32_t>(is_pause + 9));
  if (!image::data().contains(slot) || !image::text().contains(test_bit) ||
      !mem::matches(test_bit, mem::parse_pattern("8B D1 8B 4C 24 04 B8 01 00 00 00 D3 E0 85 82 ?? ?? ?? ?? 0F 95 C0 C2 04 00"))) {
    logf("pause: testBit at exe+0x%06X does not have the expected shape - pause detection off", image::rva(test_bit));
    return;
  }
  g_pause_slot = slot;
  g_pause_disp = mem::read<uint32_t>(test_bit + 15);
  g_pause_bit = bit;
  logf("pause: isPause exe+0x%06X reads bit %d of [[exe+0x%06X]+0x%X] (testBit exe+0x%06X)", image::rva(is_pause), bit,
       image::rva(slot), g_pause_disp, image::rva(test_bit));
}

bool read_flags(uint32_t* out) {
  if (!g_pause_slot) return false;
  uint32_t obj = 0;
  if (!mem::read_safe(g_pause_slot, &obj) || !obj) return false;
  return mem::read_safe(obj + g_pause_disp, out);
}

// --- which game is running -----------------------------------------------------------------
// sGamePresence is what the game hands Steam's rich presence, and it is the
// only place in the image that says which of the three modes the player is
// in. The three entry points each ask for their value as they commit:
//   the main game (new game and continue)  -> 1, or 2 when sGameInfo+0x60 says
//                                             Wesker mode (the flag the chosen
//                                             save file carries at record+0xD0)
//   Leech Hunter                           -> 3
// and the title screen's own menus ask for 4..8 as the player moves through them.
//
// That is two fields, not one. The setter only files a request; the presence
// update takes it the next time it runs, publishes it into a field of its own
// and zeroes the request (exe+0x201F76). It publishes only a reading that
// changed, and holds the main game's back until the room is known. So either
// field alone is wrong for a while - the request reads 0 once it is taken
// (the first version of this read only the request, and saw 0 through a whole
// Leech Hunter run), the published value lags - and the update's own rule,
// `request ? request : published`, is right throughout. mode() reads that.
//
// Both offsets come out of the three sites that use them, which must agree:
// the setter (the request), the update's head, which picks the request over
// the published value with a cmovne, and the update's tail, which publishes
// and zeroes the request.
int g_mode_off = -1;      // the request
int g_mode_pub_off = -1;  // the published value

void find_mode_field() {
  // head: `mov ebx,[esi+<pub>]; test ebx,ebx; jne +7; mov ebx,7; jmp +8; mov eax,[esi+<req>]; test eax,eax; cmovne ebx,eax`
  const std::vector<uintptr_t> rd =
      mem::find_all(image::text(), "8B 5E ?? 85 DB 75 07 BB 07 00 00 00 EB 08 8B 46 ?? 85 C0 0F 45 D8", 4);
  // setter: `mov esi,ecx; cmp byte [esi+1C],0; ... EnterCriticalSection ...; mov eax,[esp+8]; mov [esi+<req>],eax`
  const std::vector<uintptr_t> wr = mem::find_all(
      image::text(), "8B F1 80 7E ?? 00 75 09 80 3D ?? ?? ?? ?? 00 74 0A 8D 46 04 50 FF 15 ?? ?? ?? ?? 80 7E ?? 00 8B 44 24 08 89 46 ??",
      4);
  // tail: `mov [esi+<pub>],ebx; mov [esi+..],ebp; mov [esi+..],edi; mov dword [esi+<req>],0`
  const std::vector<uintptr_t> pb = mem::find_all(image::text(), "89 5E ?? 89 6E ?? 89 7E ?? C7 46 ?? 00 00 00 00", 4);
  if (rd.size() != 1 || wr.size() != 1 || pb.size() != 1) {
    logf("mode: the presence sites are not unique (head %u, setter %u, publish %u matches) - "
         "Leech Hunter will look like the main game",
         static_cast<unsigned>(rd.size()), static_cast<unsigned>(wr.size()), static_cast<unsigned>(pb.size()));
    return;
  }
  const int req = mem::read<uint8_t>(rd[0] + 16), pub = mem::read<uint8_t>(rd[0] + 2);
  const int req_set = mem::read<uint8_t>(wr[0] + 37);
  const int req_tail = mem::read<uint8_t>(pb[0] + 11), pub_tail = mem::read<uint8_t>(pb[0] + 2);
  const bool sane = req >= 4 && req <= 0x80 && pub >= 4 && pub <= 0x80 && req != pub;
  if (!sane || req != req_set || req != req_tail || pub != pub_tail) {
    logf("mode: the presence sites disagree (request +0x%X/+0x%X/+0x%X, published +0x%X/+0x%X) - "
         "the game mode stays unknown",
         req, req_set, req_tail, pub, pub_tail);
    return;
  }
  g_mode_off = req;
  g_mode_pub_off = pub;
  logf("mode: sGamePresence+0x%X is the requested game mode, +0x%X the published one "
       "(head exe+0x%06X, setter exe+0x%06X, publish exe+0x%06X)",
       req, pub, image::rva(rd[0]), image::rva(wr[0]), image::rva(pb[0]));
}

// --- the room's phase machine ---------------------------------------------------------------
// sRoomControl runs every screen of a game as a stack of phases, named in the
// game's own table ({const char*, int} rows in .rdata: Init, Main, ..., Save,
// ..., Opening, ..., WeskerTitle, OmakeTitle, ...). A phase is pushed through
// one routine, reached through a wrapper that adds the machine's offset inside
// sRoomControl:
//   push:    mov edx,[ecx+<next>]; cmp edx,-1; jne +1A; mov edx,[ecx+<depth>];
//            mov eax,[ecx+<current>]; mov [ecx+edx*4+<stack>],eax; mov eax,[esp+4];
//            inc [ecx+<depth>]; mov [ecx+<next>],eax; mov eax,[ecx+<current>]; ret 4
//   wrapper: add ecx,<machine>; jmp push
// Every new game pushes Opening: the fresh start (exe+0x60BC), the start from a
// save file that Once Again uses (exe+0x6D41), and the end of WeskerTitle
// (exe+0x20CE42), which Wesker mode's fresh start pushes instead. A continue
// pushes nothing. The ids are read from the name table, not from those pushes.
int g_phase_mgr = -1, g_phase_cur = -1, g_phase_next = -1;
int g_phase_opening = -1, g_phase_wesker_title = -1;

// The id the phase-name table gives `name`: the one row whose first dword
// points at that exact string, with an id the machine can hold (it bounds-
// checks them to 0..0x17 at exe+0x20A3D0).
int phase_id(const char* name) {
  char pat[96] = "00";
  for (const char* c = name; *c && std::strlen(pat) + 4 < sizeof(pat); ++c)
    std::snprintf(pat + std::strlen(pat), sizeof(pat) - std::strlen(pat), " %02X", static_cast<unsigned char>(*c));
  std::snprintf(pat + std::strlen(pat), sizeof(pat) - std::strlen(pat), " 00");
  int id = -1, rows = 0;
  for (uintptr_t str : mem::find_all(image::rdata(), pat, 8)) {
    for (uintptr_t row : mem::find_dwords(image::rdata(), static_cast<uint32_t>(str + 1), 8)) {
      uint32_t v = 0;
      if (!mem::read_safe(row + 4, &v) || v >= 0x18) continue;
      ++rows;
      id = static_cast<int>(v);
    }
  }
  return rows == 1 ? id : -1;
}

void find_phase_fields() {
  const std::vector<uintptr_t> push = mem::find_all(
      image::text(), "8B 51 ?? 83 FA FF 75 1A 8B 51 ?? 8B 41 ?? 89 44 91 ?? 8B 44 24 04 FF 41 ?? 89 41 ?? 8B 41 ?? C2 04 00", 4);
  if (push.size() != 1) {
    logf("phase: the phase push routine was not found uniquely (%u matches) - a new game started after a load "
         "will be taken for a continue",
         static_cast<unsigned>(push.size()));
    return;
  }
  const uintptr_t p = push[0];
  const int next = mem::read<uint8_t>(p + 2), depth = mem::read<uint8_t>(p + 10), cur = mem::read<uint8_t>(p + 13);
  if (mem::read<uint8_t>(p + 27) != next || mem::read<uint8_t>(p + 24) != depth || mem::read<uint8_t>(p + 30) != cur ||
      next == cur) {
    logf("phase: the push routine at exe+0x%06X names its fields inconsistently - phase reading off", image::rva(p));
    return;
  }
  int mgr = -1, wrappers = 0;
  uintptr_t wrapper = 0;
  for (uintptr_t w : mem::find_all(image::text(), "81 C1 ?? ?? ?? ?? E9 ?? ?? ?? ??", 64)) {
    if (w + 11 + static_cast<uintptr_t>(mem::read<int32_t>(w + 7)) != p) continue;
    ++wrappers;
    wrapper = w;
    mgr = static_cast<int>(mem::read<uint32_t>(w + 2));
  }
  if (wrappers != 1 || mgr <= 0 || mgr > 0x10000) {
    logf("phase: %d wrapper(s) reach the push routine exe+0x%06X - phase reading off", wrappers, image::rva(p));
    return;
  }
  const int opening = phase_id("Opening"), wesker = phase_id("WeskerTitle");
  if (opening < 0 || wesker < 0 || opening == wesker) {
    logf("phase: the name table does not give Opening (%d) and WeskerTitle (%d) one id each - phase reading off", opening,
         wesker);
    return;
  }
  g_phase_mgr = mgr;
  g_phase_cur = cur;
  g_phase_next = next;
  g_phase_opening = opening;
  g_phase_wesker_title = wesker;
  logf("phase: sRoomControl+0x%X is the phase machine (current +0x%X, next +0x%X; push exe+0x%06X, wrapper exe+0x%06X); "
       "a new game opens with phase %d (Opening) or %d (WeskerTitle)",
       mgr, cur, next, image::rva(p), image::rva(wrapper), opening, wesker);
}

// --- diagnostics -------------------------------------------------------------------------------
struct Watch {
  char name[32];
  uint32_t size;
  uintptr_t obj;
  std::vector<uint8_t> last;
};
Watch g_watch[8];
int g_nwatch = 0;
DWORD g_last_watch = 0;
DWORD g_last_trace = 0;
DWORD g_last_status_trace = 0;

void parse_watches() {
  const char* s = config::get().dump_objects;
  while (*s && g_nwatch < 8) {
    while (*s == ' ' || *s == ',') ++s;
    if (!*s) break;
    Watch& w = g_watch[g_nwatch];
    int n = 0;
    while (*s && *s != ':' && *s != ',' && n < 31) w.name[n++] = *s++;
    w.name[n] = '\0';
    w.size = 0x80;
    if (*s == ':') w.size = static_cast<uint32_t>(std::strtoul(s + 1, nullptr, 0));
    while (*s && *s != ',') ++s;
    if (w.size < 4 || w.size > 0x2000) w.size = 0x80;
    w.obj = 0;
    ++g_nwatch;
  }
  for (int i = 0; i < g_nwatch; ++i) logf("watching %s (0x%X bytes) for changes", g_watch[i].name, g_watch[i].size);
}

void watch_tick() {
  const DWORD now = GetTickCount();
  if (now - g_last_watch < 100) return;
  g_last_watch = now;
  for (int i = 0; i < g_nwatch; ++i) {
    Watch& w = g_watch[i];
    const uintptr_t obj = single(w.name);
    if (!obj || !mem::readable(obj, w.size)) continue;
    if (obj != w.obj) {
      w.obj = obj;
      w.last.assign(reinterpret_cast<const uint8_t*>(obj), reinterpret_cast<const uint8_t*>(obj) + w.size);
      hexdump(w.name, obj, w.size);
      continue;
    }
    std::vector<uint8_t> cur(reinterpret_cast<const uint8_t*>(obj), reinterpret_cast<const uint8_t*>(obj) + w.size);
    int shown = 0;
    char line[240] = {};
    for (uint32_t off = 0; off + 4 <= w.size; off += 4) {
      uint32_t a = 0, b = 0;
      std::memcpy(&a, &w.last[off], 4);
      std::memcpy(&b, &cur[off], 4);
      if (a == b) continue;
      if (shown < 10)
        std::snprintf(line + std::strlen(line), sizeof(line) - std::strlen(line), " +0x%03X:%08X->%08X", off, a, b);
      ++shown;
    }
    if (shown) logf("watch %s:%s%s", w.name, line, shown > 10 ? " ..." : "");
    w.last.swap(cur);
  }
}

void trace_leads() {
  const DWORD now = GetTickCount();
  if (now - g_last_trace < 250) return;
  g_last_trace = now;
  uint32_t flags = 0;
  if (read_flags(&flags) && flags != g_last_flags) {
    char bits[160] = {};
    for (int b = 0; b < 32; ++b)
      if ((flags >> b) & 1u) std::snprintf(bits + std::strlen(bits), sizeof(bits) - std::strlen(bits), " %d", b);
    logf("flags: %08X (bits set:%s) pause=%d", flags, bits, pause_menu_showing());
    g_last_flags = flags;
  }
  if (now - g_last_status_trace > 5000) {
    g_last_status_trace = now;
    if (g_mode_off > 0) {
      const uintptr_t p = single("sGamePresence");
      int req = -1, pub = -1;
      if (p) {
        mem::read_safe(p + static_cast<uintptr_t>(g_mode_off), &req);
        mem::read_safe(p + static_cast<uintptr_t>(g_mode_pub_off), &pub);
      }
      logf("trace: sGamePresence %p request +%X=%d published +%X=%d (%s)", reinterpret_cast<void*>(p), g_mode_off, req,
           g_mode_pub_off, pub, mode_name(mode()));
    }
    if (g_phase_mgr > 0) {
      int cur = -1, next = -1;
      if (const uintptr_t rc = single("sRoomControl")) {
        mem::read_safe(rc + static_cast<uintptr_t>(g_phase_mgr + g_phase_cur), &cur);
        mem::read_safe(rc + static_cast<uintptr_t>(g_phase_mgr + g_phase_next), &next);
      }
      logf("trace: room phase %d, next %d", cur, next);
    }
    for (int i = 0; i < g_ncands; ++i) {
      const StatusCandidate& c = g_cands[i];
      if (g_status_obj && c.obj != g_status_obj) continue;
      int s38 = 0;
      float f3c = 0.0f;
      mem::read_safe(c.obj + kOffSaveCount, &s38);
      mem::read_safe(c.obj + kOffPlaytime, &f3c);
      logf("trace: status %-22s %p +38=%d +3C=%.2f%s", c.name, reinterpret_cast<void*>(c.obj), s38, f3c, c.rejected ? " (rejected)" : "");
    }
    if (const uintptr_t es = countdown_object()) {
      float cd = -1.0f;
      int mode = 0;
      mem::read_safe(es + kOffCountdown, &cd);
      mem::read_safe(es + 0x64, &mode);
      logf("trace: sEventScript %p +2C=%.2f (%.2f s) +64=%d", reinterpret_cast<void*>(es), cd, cd / kCountdownRate, mode);
    }
    Unit list[128];
    const int n = units(list, 128);
    for (int i = 0; i < n; ++i) {
      char pool[24] = {};
      if (list[i].pool >= 0) std::snprintf(pool, sizeof(pool), " pool=%d", list[i].pool);
      logf("trace: unit %2d %p %-18s hp=%d%s%s", list[i].index, reinterpret_cast<void*>(list[i].obj), list[i].cls,
           list[i].hp, pool, list[i].enemy ? " (enemy)" : "");
    }
  }
}

}  // namespace

// --- discovery --------------------------------------------------------------------------
bool decrypted(int* prologues, int* pads) {
  image::init();
  return mem::text_looks_decrypted(image::text(), prologues, pads);
}

const char* build_string() { return g_build; }
bool build_supported() { return g_build_ok; }

bool discover() {
  image::init();
  set_status("discovering");
  if (!g_build[0]) {
    const uintptr_t s = mem::find_cstring_prefix(image::data_sections(), "MasterRelease ");
    if (s) std::snprintf(g_build, sizeof(g_build), "%s", reinterpret_cast<const char*>(s));
    else std::snprintf(g_build, sizeof(g_build), "(no MasterRelease string)");
    g_build_ok = std::strstr(g_build, "Jan 28 2025 16:45:59") != nullptr;
    logf("build: \"%s\" -> %s", g_build, g_build_ok ? "supported (Steam build 17178773)" : "NOT the build this mod was made for");
  }
  static bool listed = false;
  const bool script_ok = script::init();
  if (script_ok && !listed) {
    listed = true;
    static const char* const kCmds[] = {"PlayerMutekiSet", "PlayerMutekiReset", "PlayerDamage", "PlayerEquip", "item_get",
                                        "item_sub", "item_check", "key_check", "ItemPut", "save_point", "CharChange",
                                        "EnemyDeath", "EnemyDeathTypeAll", "GameOver", "DeathCheck", "TimerSet",
                                        "TimeAttack", "TimeAttackEnd", "SetDeltaTimer", "RequestAchievement",
                                        "MTHPPause", "UseKey"};
    for (const char* c : kCmds) {
      script::Command cmd;
      if (script::find(c, &cmd)) logf("script: %-20s (%-8s) handler exe+0x%06X", c, cmd.argsig, image::rva(cmd.handler));
      else logf("script: %-20s NOT in the table", c);
    }
  }
  const bool dti_ok = dti::init();
  if (dti_ok) {
    g_cls_rebecca = dti::find_class("uPlayerRebecca");
    g_cls_billy = dti::find_class("uPlayerBilly");
    g_cls_player_base = dti::find_class("uPlayerBase");
    g_cls_enemy_base = dti::find_class("uEnemyBase");
    dti::find_class("uGUIPause");
    dti::find_class("uGUISave");
    dti::find_class("cSaveData");
    dti::find_class("aTitle");     // the load list's chain (load_list_object), looked up here on the
    dti::find_class("uGUITitle");  // mod thread so the main thread only ever hits the cache
    for (Single& s : g_singles) {
      s.cls = dti::find_class(s.name);
      s.found = s.cls && dti::find_singleton(*s.cls, &s.s);
      if (s.cls && !s.found) logf("dti: %s: no live instance found yet", s.name);
    }
  }
  static bool sites_done = false;
  if (!sites_done) {
    sites_done = true;
    find_sites();
    find_pause_flag();
    find_mode_field();
    find_phase_fields();
    find_chara_layout();
    find_item_max();
    find_stack_rows();
    find_enemy_pool();
    find_save_fields();
    find_save_point();
    find_record_fields();
    find_load_list();
    parse_watches();
  }
  g_ready = script_ok && dti_ok;
  set_status(g_ready ? "ready" : (!dti_ok ? "DTI discovery failed - see log" : "script table missing - see log"));
  logf("game layer %s", g_status);
  return g_ready;
}

bool ready() { return g_ready; }
const char* status_text() { return g_status; }

void background_tick() {
  if (!g_ready) return;
  for (Single& s : g_singles) {
    if (!s.cls || s.found) continue;
    dti::Singleton found;
    if (dti::find_singleton(*s.cls, &found)) {
      s.s = found;
      s.found = true;
    }
  }
}

void note_iat_slot(uintptr_t slot, uintptr_t original) {
  if (g_iat_n < 8 && slot) g_iat[g_iat_n++] = {slot, original};
}

bool dump_image(const char* dir) {
  image::init();
  IMAGE_NT_HEADERS* nt = mem::nt_headers(image::module());
  if (!nt) return false;
  const size_t size = nt->OptionalHeader.SizeOfImage;
  std::vector<uint8_t> buf(size, 0);
  SIZE_T got = 0;
  // Headers first, then each section separately (a section can have uncommitted tail pages).
  ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(image::base()), buf.data(), nt->OptionalHeader.SizeOfHeaders, &got);
  auto* sec = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    const uintptr_t va = image::base() + sec[i].VirtualAddress;
    const size_t n = sec[i].Misc.VirtualSize;
    for (size_t off = 0; off < n; off += 0x1000) {
      const size_t chunk = (n - off) < 0x1000 ? (n - off) : 0x1000;
      ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(va + off), buf.data() + sec[i].VirtualAddress + off, chunk, &got);
    }
  }
  // Put our own import-table patches back so the dump is the game's image, not ours.
  for (int i = 0; i < g_iat_n; ++i) {
    const uintptr_t off = g_iat[i].slot - image::base();
    if (off + 4 <= size) std::memcpy(buf.data() + off, &g_iat[i].original, 4);
  }
  // Section headers: raw = virtual, so the file maps 1:1 to the loaded image.
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(buf.data());
  auto* nt2 = reinterpret_cast<IMAGE_NT_HEADERS*>(buf.data() + dos->e_lfanew);
  auto* s2 = IMAGE_FIRST_SECTION(nt2);
  for (unsigned i = 0; i < nt2->FileHeader.NumberOfSections; ++i) {
    s2[i].PointerToRawData = s2[i].VirtualAddress;
    s2[i].SizeOfRawData = (s2[i].Misc.VirtualSize + 0xFFF) & ~0xFFFu;
  }
  nt2->OptionalHeader.FileAlignment = 0x1000;
  char path[MAX_PATH];
  std::snprintf(path, sizeof(path), "%sre0hd.dumped.exe", dir);
  FILE* f = std::fopen(path, "wb");
  if (!f) {
    logf("ERROR: dump: cannot write %s", path);
    return false;
  }
  std::fwrite(buf.data(), 1, size, f);
  std::fclose(f);
  logf("dump: wrote %s (%u bytes, entry point RVA 0x%X still points at the stub - run tools/fix_dump.py)", path,
       static_cast<unsigned>(size), static_cast<unsigned>(nt->OptionalHeader.AddressOfEntryPoint));
  return true;
}

// --- main-thread state -----------------------------------------------------------------
bool in_game() {
  if (!g_ready) return false;
  refresh_characters();
  return g_char[0] != 0 || g_char[1] != 0;
}

bool pause_menu_showing() {
  uint32_t flags = 0;
  if (g_pause_bit < 0 || !read_flags(&flags)) return false;
  return ((flags >> g_pause_bit) & 1u) != 0;
}

bool mode_ready() { return g_mode_off > 0; }

Mode mode() {
  if (g_mode_off <= 0 || g_mode_pub_off <= 0) return Mode::kUnknown;
  const uintptr_t o = single("sGamePresence");
  int req = 0, pub = 0;
  if (!o || !mem::read_safe(o + static_cast<uintptr_t>(g_mode_off), &req) ||
      !mem::read_safe(o + static_cast<uintptr_t>(g_mode_pub_off), &pub))
    return Mode::kUnknown;
  const int v = req ? req : pub;  // the update's own rule: a pending request wins
  switch (v) {
    case 1: return Mode::kMainGame;
    case 2: return Mode::kWesker;
    case 3: return Mode::kLeechHunter;
    default: break;
  }
  // 4..8 are the title screen's own menus: a definite "no game is running".
  // Anything else (0 before the first request, or a value this build does not
  // have) says nothing, and a caller must not act on it.
  return (v >= 4 && v <= 8) ? Mode::kMenu : Mode::kUnknown;
}

const char* mode_name(Mode m) {
  switch (m) {
    case Mode::kMainGame: return "the main game";
    case Mode::kWesker: return "Wesker mode";
    case Mode::kLeechHunter: return "Leech Hunter";
    case Mode::kMenu: return "the menus";
    default: break;
  }
  return "unknown";
}

bool phase_ready() { return g_phase_mgr > 0 && g_phase_opening >= 0 && g_phase_wesker_title >= 0; }

// Only the top of the machine counts - the phase just pushed (next) or the one
// it has already become (current) - and only on the tick a game names itself.
// The stack below says nothing about when a phase got there: every push leaves
// the phase it came from on it.
bool new_game_starting() {
  if (!phase_ready()) return false;
  const uintptr_t rc = single("sRoomControl");
  if (!rc) return false;
  const uintptr_t m = rc + static_cast<uintptr_t>(g_phase_mgr);
  int cur = -1, next = -1;
  if (!mem::read_safe(m + static_cast<uintptr_t>(g_phase_cur), &cur) ||
      !mem::read_safe(m + static_cast<uintptr_t>(g_phase_next), &next))
    return false;
  const auto opens = [](int id) { return id == g_phase_opening || id == g_phase_wesker_title; };
  return opens(next) || opens(cur);
}

void trace_tick() {
  if (!g_ready) return;
  const bool playing = in_game();
  refresh_candidates();
  validate_status(playing);
  if (config::get().trace) trace_leads();
  if (g_nwatch) watch_tick();
}

// --- characters -------------------------------------------------------------------------
int characters(Character out[2]) {
  if (!g_ready) return 0;
  refresh_characters();
  int n = 0;
  for (int i = 0; i < 2; ++i) {
    Character c;
    if (read_character(g_char[i], &c)) {
      const int idx = c.type == 3 ? 0 : 1;
      if (c.hp > g_hp_seen[idx]) g_hp_seen[idx] = c.hp;
      out[n++] = c;
    }
  }
  return n;
}

int max_hp(const Character& c) { return c.type == 3 ? g_hp_seen[0] : g_hp_seen[1]; }

bool set_hp(const Character& c, int hp) {
  if (!c.obj || !is_player_unit(c.obj)) return false;
  return mem::write<int32_t>(c.obj + kOffHp, hp);
}

bool set_poison(const Character& c, int v) {
  if (!c.obj || !is_player_unit(c.obj)) return false;
  uint32_t status = mem::read<uint32_t>(c.obj + kOffStatus);
  status = v ? (status | kStatusPoison) : (status & ~kStatusPoison);
  return mem::write<uint32_t>(c.obj + kOffStatus, status);
}

// --- enemies ---------------------------------------------------------------------------------
bool enemy_table_known() { return g_chara_off >= 0; }

int units(Unit* out, int max) {
  if (g_chara_off < 0) return 0;
  const uintptr_t chara = single("sGameChara");
  if (!chara) return 0;
  const DWORD now = GetTickCount();
  int n = 0;
  for (int i = 0; i < g_chara_count && n < max; ++i) {
    uint32_t p = 0;
    if (!mem::read_safe(chara + static_cast<uintptr_t>(g_chara_off) + static_cast<uintptr_t>(i) * g_chara_stride, &p) || !p) continue;
    EnemySlot& slot = g_enemy_slots[i];
    if (slot.obj != p || now - slot.checked > 1000) {
      slot.obj = p;
      slot.checked = now;
      slot.is_enemy = is_enemy_unit(p);
      slot.has_pool = unit_has_pool(p);
      slot.cls = dti::class_name_of(reinterpret_cast<void*>(p));
      if (!slot.is_enemy) note_other_class(slot.cls);
    }
    int hp = 0;
    if (!mem::read_safe(p + kOffHp, &hp)) continue;
    int pool = -1;
    if (slot.has_pool && !mem::read_safe(p + static_cast<uintptr_t>(g_pool_off), &pool)) pool = -1;
    out[n].obj = p;
    out[n].cls = slot.cls ? slot.cls : "?";
    out[n].hp = hp;
    out[n].pool = pool;
    out[n].index = i;
    out[n].enemy = slot.is_enemy;
    ++n;
  }
  return n;
}

int enemies(Enemy* out, int max) {
  Unit all[128];  // g_chara_count is capped at 128 when the table is found
  const int n = units(all, 128);
  int k = 0;
  for (int i = 0; i < n && k < max; ++i) {
    if (!all[i].enemy) continue;
    out[k].obj = all[i].obj;
    out[k].cls = all[i].cls;
    out[k].hp = all[i].hp;
    out[k].pool = all[i].pool;
    ++k;
  }
  return k;
}

bool set_enemy_hp(const Enemy& e, int hp) {
  if (!e.obj || !mem::readable(e.obj, kOffHp + 4)) return false;
  return mem::write<int32_t>(e.obj + kOffHp, hp);
}

// The leech-man's second pool. Only ever written for a unit whose reading came
// back (pool >= 0), i.e. one of the class the offset was derived from.
bool set_enemy_pool(const Enemy& e, int v) {
  if (!e.obj || g_pool_off <= 0 || e.pool < 0 || !mem::readable(e.obj, static_cast<size_t>(g_pool_off) + 4)) return false;
  return mem::write<int32_t>(e.obj + static_cast<uintptr_t>(g_pool_off), v);
}

// --- bags ----------------------------------------------------------------------------------
bool bags(Bag out[2]) {
  const uintptr_t item = single("sItem");
  if (!item) {
    // This used to be the one way the inventory could go away without saying
    // so: no log, and g_bags_ok left alone, so the recovery was silent too.
    static DWORD last_gone = 0;
    const DWORD now = GetTickCount();
    if (!last_gone || now - last_gone >= 5000) {
      last_gone = now;
      logf("bags: sItem could not be resolved this tick - inventory reading skipped");
    }
    g_bags_ok = false;
    return false;
  }
  Bag a, b;
  const bool ok = read_bag(item + kOffBagRebecca, &a) && read_bag(item + kOffBagBilly, &b);
  if (!ok) {
    if (!g_bags_warned) {
      g_bags_warned = true;
      logf("ERROR: the bag layout lead (sItem+0x20 / +0x60) does not fit - inventory features off; sItem dump follows");
      hexdump("sItem", item, 0x100);
    }
    g_bags_ok = false;
    return false;
  }
  if (!g_bags_ok) {
    g_bags_ok = true;
    logf("bags: sItem %p: Rebecca [%d/%d %d/%d %d/%d %d/%d %d/%d %d/%d] personal %d/%d eq %d | Billy [%d/%d %d/%d %d/%d %d/%d %d/%d %d/%d] personal %d/%d eq %d",
         reinterpret_cast<void*>(item), a.id[0], a.count[0], a.id[1], a.count[1], a.id[2], a.count[2], a.id[3], a.count[3], a.id[4],
         a.count[4], a.id[5], a.count[5], a.personal_id, a.personal_count, a.equipped, b.id[0], b.count[0], b.id[1], b.count[1],
         b.id[2], b.count[2], b.id[3], b.count[3], b.id[4], b.count[4], b.id[5], b.count[5], b.personal_id, b.personal_count,
         b.equipped);
  }
  out[0] = a;
  out[1] = b;
  return true;
}

bool write_slot(int bag, int slot, int id, int count) {
  Bag b[2];
  if (!bags(b) || bag < 0 || bag > 1 || slot < 0 || slot > 6) return false;
  const uintptr_t base = b[bag].base;
  const uintptr_t at = slot == 6 ? base + 0x34 : base + 4 + 8 * slot;
  return mem::write<int32_t>(at, id) && mem::write<int32_t>(at + 4, count);
}

bool write_equipped(int bag, int index) {
  Bag b[2];
  if (!bags(b) || bag < 0 || bag > 1 || index < -1 || index > 6) return false;
  return mem::write<int32_t>(b[bag].base + 0x3C, index);
}

bool item_max_known() { return g_item_max_n > 0; }

// The game's own answer, with the mod's table behind it for an id the routine's
// own table has nothing for and for the case where the routine was not found.
int item_max(int id) {
  if (id <= 0) return 0;
  if (g_item_max_n > 0) {
    if (id >= g_item_max_n) {
      if (id == g_item_max_special_id[0] || id == g_item_max_special_id[1]) return g_item_max_special;
      return g_item_max_default;
    }
    if (g_item_max_tab[id] > 0) return g_item_max_tab[id];
  }
  return items::max_count(id);
}

bool item_stacks_known() { return g_stacks_n > 0; }

bool item_stacks(int id) {
  if (id <= 0 || id >= kItemMaxTableMax) return false;
  return g_stacks_n > 0 ? g_stacks[id] : items::stacks(id);
}

// --- save files -------------------------------------------------------------------------------
bool save_watch_ready() { return g_save.ok && g_save_obj != 0; }
int save_slot_count() { return g_save.ok ? g_save.slots : 0; }

bool save_op(SaveOp* out) {
  *out = SaveOp{};
  const uintptr_t o = save_object();
  if (!o) return false;
  int mode = 0, state = 0, result = 0, file = 0;
  if (!mem::read_safe(o + g_save.mode, &mode) || !mem::read_safe(o + g_save.state, &state) ||
      !mem::read_safe(o + g_save.result, &result) || !mem::read_safe(o + g_save.op, &file))
    return false;
  // Anything outside the state machine's own numbering means this is not the
  // object it looks like: say nothing rather than act on a wrong reading.
  if (mode < 0 || mode > 8 || state < 0 || state > 8) {
    if (!g_save_shape_warned) {
      g_save_shape_warned = true;
      logf("saves: cSaveManager %p reads mode=%d state=%d - not the state machine, watching off",
           reinterpret_cast<void*>(o), mode, state);
    }
    g_save_obj = 0;
    return false;
  }
  out->valid = true;
  out->mode = mode;
  out->state = state;
  out->result = result;
  out->slot = (file >= 0 && file < g_save.slots) ? file : -1;
  out->op = file;
  out->is_system = g_rec.system_op >= 0 && file == g_rec.system_op;
  out->is_save = mode == g_save.mode_save || mode == g_save.mode_copy;
  out->is_copy = mode == g_save.mode_copy;
  out->is_load = mode == g_save.mode_load;
  // The save point the game would record with this save. It is read from
  // sSaveManager itself rather than from the record, which the mod never
  // touches; the capture routine copies the same byte in at +0xCC.
  if (g_point_off >= 0) {
    const uintptr_t sm = single("sSaveManager");
    uint8_t v = 0;
    if (sm && mem::read_safe(sm + static_cast<uintptr_t>(g_point_off), &v)) {
      out->point = v;
      out->is_clear = v == g_point_clear;
    }
  }
  return true;
}

bool save_point_known() { return g_point_off >= 0 && g_point_clear >= 0; }

// --- the save files themselves ------------------------------------------------------------------
bool save_files_ready() { return g_rec.ok && g_save.layout_ok; }

int save_files(SaveFile* out, int max) {
  const uintptr_t o = save_object();
  if (!g_rec.ok || !o) return 0;
  const int n = g_save.slots < max ? g_save.slots : max;
  // save_object() found it readable for all of both arrays; one look covers the first again.
  if (!mem::readable(record_at(o, false, 0), static_cast<size_t>(g_save.stride) * static_cast<size_t>(n))) return 0;
  for (int i = 0; i < n; ++i) {
    const uintptr_t r = record_at(o, false, i);
    SaveFile& f = out[i];
    f = SaveFile{};
    f.point = mem::read<uint8_t>(r + static_cast<uintptr_t>(g_rec.point));
    f.used = f.point != 0;
    f.cleared = f.used && g_point_clear >= 0 && f.point == g_point_clear;
    if (g_rec.wesker >= 0) f.wesker = f.used && mem::read<uint8_t>(r + static_cast<uintptr_t>(g_rec.wesker)) != 0;
    if (f.used && g_rec.saves >= 0) {
      const int saves = mem::read<int32_t>(r + static_cast<uintptr_t>(g_rec.saves));
      const float pt = mem::read<float>(r + static_cast<uintptr_t>(g_rec.playtime));
      // A clock past 99:59:59 or a counter in the hundreds of thousands is a misreading, not a save.
      if (saves >= 0 && saves < 100000) f.saves = saves;
      // The clock's own unit: measured once a game has run, 30 per second
      // (the updater's 99:59:59 clamp) until then.
      const float rate = g_rate > 0.0f ? g_rate : kExpectedRate;
      if (std::isfinite(pt) && pt >= 0.0f && pt <= 1.08e7f) f.seconds = pt / rate;
    }
  }
  return n;
}

bool save_idle() {
  SaveOp op;
  return save_op(&op) && op.mode == 0 && op.state == 0;
}

bool backup_save_file(int slot, std::vector<uint8_t>* out) {
  const uintptr_t o = save_object();
  if (!g_rec.ok || !o || slot < 0 || slot >= g_save.slots) return false;
  const size_t n = g_save.stride;
  const uintptr_t a = record_at(o, false, slot), b = record_at(o, true, slot);
  if (!mem::readable(a, n) || !mem::readable(b, n)) return false;
  out->resize(2 * n);
  std::memcpy(out->data(), reinterpret_cast<const void*>(a), n);
  std::memcpy(out->data() + n, reinterpret_cast<const void*>(b), n);
  return true;
}

bool restore_save_file(int slot, const std::vector<uint8_t>& in) {
  const uintptr_t o = save_object();
  const size_t n = g_save.stride;
  if (!g_rec.ok || !o || slot < 0 || slot >= g_save.slots || in.size() != 2 * n || !save_idle()) return false;
  return mem::write_bytes(record_at(o, false, slot), in.data(), n) &&
         mem::write_bytes(record_at(o, true, slot), in.data() + n, n);
}

bool clear_save_file(int slot) {
  const uintptr_t o = save_object();
  if (!g_rec.ok || !o || slot < 0 || slot >= g_save.slots || !save_idle()) return false;
  const uint8_t zero = 0;
  return mem::write_bytes(record_at(o, false, slot) + static_cast<uintptr_t>(g_rec.point), &zero, 1) &&
         mem::write_bytes(record_at(o, true, slot) + static_cast<uintptr_t>(g_rec.point), &zero, 1);
}

bool copy_save_file(int from, int to) {
  const uintptr_t o = save_object();
  if (!g_rec.ok || !o || from < 0 || to < 0 || from >= g_save.slots || to >= g_save.slots || from == to || !save_idle())
    return false;
  const size_t n = g_save.stride;
  for (int second = 0; second < 2; ++second) {
    const uintptr_t src = record_at(o, second != 0, from), dst = record_at(o, second != 0, to);
    if (!mem::readable(src, n) || !mem::readable(dst, n)) return false;
    if (!mem::write_bytes(dst, reinterpret_cast<const void*>(src), n)) return false;
  }
  return true;
}

// Byte for byte, in the copy the game lists: after a copy has been written and
// read back, the two files are the same save.
int same_save_file(int a, int b) {
  const uintptr_t o = save_object();
  if (!g_rec.ok || !o || a < 0 || b < 0 || a >= g_save.slots || b >= g_save.slots) return -1;
  const size_t n = g_save.stride;
  const uintptr_t ra = record_at(o, false, a), rb = record_at(o, false, b);
  if (!mem::readable(ra, n) || !mem::readable(rb, n)) return -1;
  return std::memcmp(reinterpret_cast<const void*>(ra), reinterpret_cast<const void*>(rb), n) == 0 ? 1 : 0;
}

// Field for field what the game's save request does (exe+0x2134C0), in its
// order: the file, the second argument, the result, then the mode and the
// state that set the task going on the next frame.
bool request_system_save() {
  const uintptr_t o = save_object();
  if (!g_rec.ok || !o || !save_idle()) return false;
  return mem::write<int32_t>(o + static_cast<uintptr_t>(g_save.op), g_rec.system_op) &&
         (g_save.arg2 < 0 || mem::write<int32_t>(o + static_cast<uintptr_t>(g_save.arg2), 0)) &&
         mem::write<int32_t>(o + static_cast<uintptr_t>(g_save.result), 0) &&
         mem::write<int32_t>(o + static_cast<uintptr_t>(g_save.mode), g_save.mode_save) &&
         mem::write<int32_t>(o + static_cast<uintptr_t>(g_save.state), 1);
}

// --- the title screen's load list -----------------------------------------------------------------
bool load_list_ready() { return g_list.ok; }

bool load_list(LoadList* out) {
  *out = LoadList{};
  const uintptr_t g = load_list_object();
  if (!g) return false;
  int kind = -1, mode = -1, state = -1, cursor = -1;
  uint8_t active = 0;
  if (!mem::read_safe(g + static_cast<uintptr_t>(g_list.kind), &kind) ||
      !mem::read_safe(g + static_cast<uintptr_t>(g_list.mode), &mode) ||
      !mem::read_safe(g + static_cast<uintptr_t>(g_list.state), &state) ||
      !mem::read_safe(g + static_cast<uintptr_t>(g_list.cursor), &cursor) ||
      !mem::read_safe(g + static_cast<uintptr_t>(g_list.active), &active))
    return false;
  out->found = true;
  out->mode = mode;
  out->kind = kind;
  out->cursor = cursor >= 0 && cursor < g_save.slots ? cursor : -1;
  const bool loading = active != 0 && kind == g_list.load_kind;
  out->up = loading && mode == g_list.browse;
  out->rebuilding = loading && mode == g_list.rebuild_mode && state == g_list.rebuild_state;
  return true;
}

int save_current_file() {
  const uintptr_t sm = single("sSaveManager");
  int v = -1;
  if (!g_list.ok || !sm || !mem::read_safe(sm + static_cast<uintptr_t>(g_list.current), &v)) return -1;
  return v;
}

bool set_save_current_file(int slot) {
  const uintptr_t sm = single("sSaveManager");
  return g_list.ok && sm && mem::readable(sm + static_cast<uintptr_t>(g_list.current), 4) &&
         mem::write<int32_t>(sm + static_cast<uintptr_t>(g_list.current), slot);
}

// Only from the list the player is looking at: the rebuild is the mode before
// it, and the list's next frame runs it and comes straight back.
bool refresh_load_list() {
  LoadList l;
  const uintptr_t g = load_list_object();
  if (!g || !load_list(&l) || !l.up) return false;
  return mem::write<int32_t>(g + static_cast<uintptr_t>(g_list.state), g_list.rebuild_state) &&
         mem::write<int32_t>(g + static_cast<uintptr_t>(g_list.mode), g_list.rebuild_mode);
}

// --- status ---------------------------------------------------------------------------------
bool status_ready() {
  if (!g_status_obj) return false;
  const uintptr_t live = single("sGameInfo");
  if (live && live != g_status_obj) g_status_obj = live;  // re-created: follow it
  return g_status_obj != 0 && mem::readable(g_status_obj, 0x40);
}
const char* status_class() { return g_status_cls; }
int save_count() {
  int v = -1;
  if (status_ready()) mem::read_safe(g_status_obj + kOffSaveCount, &v);
  return v;
}
bool set_save_count(int v) { return status_ready() && mem::write<int32_t>(g_status_obj + kOffSaveCount, v); }
float playtime_raw() {
  float v = -1.0f;
  if (status_ready()) mem::read_safe(g_status_obj + kOffPlaytime, &v);
  return v;
}
float playtime_rate() { return g_rate; }
bool set_playtime_raw(float v) { return status_ready() && mem::write<float>(g_status_obj + kOffPlaytime, v); }

// --- countdown timer --------------------------------------------------------------------------
float countdown_rate() { return kCountdownRate; }

bool countdown_ready() {
  const uintptr_t o = countdown_object();
  return o != 0;
}

float countdown_raw() {
  const uintptr_t o = countdown_object();
  float v = -1.0f;
  if (!o || !mem::read_safe(o + kOffCountdown, &v) || !std::isfinite(v)) return -1.0f;
  return v;
}

// The idle value is -1; a zeroed field (a freshly allocated sEventScript, before
// the reset that writes the sentinel) must not read as a timer standing at 0:00.
bool countdown_active() {
  const float v = countdown_raw();
  return v > 0.0f && v <= kCountdownMax;
}

bool set_countdown_raw(float v) {
  const uintptr_t o = countdown_object();
  if (!o || !std::isfinite(v) || v < 0.0f || v > kCountdownMax) return false;
  return mem::write<float>(o + kOffCountdown, v);
}

// --- sites ------------------------------------------------------------------------------------
mem::Patch* site(Site s) {
  if (s < 0 || s >= kSiteCount) return nullptr;
  return g_sites[s].patch.prepared() ? &g_sites[s].patch : nullptr;
}
const char* site_name(Site s) { return s >= 0 && s < kSiteCount ? g_sites[s].name : "?"; }

}  // namespace re0cc::game
