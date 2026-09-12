#pragma once

#include <cstdio>

// Resident Evil 0 HD item ids, as the inventory bag stores them (id + count per
// slot). Names follow the community lists (UnIoN's cheat table, save-editor.com,
// the RE0 autosplitter). `max` is how many of that item fit in one slot - a
// weapon's loaded ammo, everything else's stack size - and the numbers are the
// game's own, read off the table its per-slot maximum routine uses (see
// game::item_max, which supersedes this column whenever that routine is found).
// 0 means the game gives the item no meaningful count (the knife and the two
// ids around it answer 0xFFFF), which the panel draws as its own 999 ceiling.
// Two-slot items occupy an even slot and the next one, which holds the filler
// id 180 with count 1.
//
// `kind` is the mod's own grouping, not the game's: it picks the slots infinite
// ammo holds (weapons) and the storage box bank an item is listed under (Bank,
// below). Every id from 0x38 up is a key item bar the files - the game's
// per-slot routine treats that whole range as one kind (see game::item_max) -
// so the Hookshot, the lighter, its fluid and the mixing set are key items too.
namespace re0cc::items {

enum Kind { kNone = 0, kWeapon, kAmmo, kHeal, kKey, kFile, kOther, kFiller };

struct Def {
  int id;
  const char* name;
  Kind kind;
  int max;
};

inline const Def kTable[] = {
    {0, "None", kNone, 0},
    {2, "Knife", kWeapon, 0},
    {3, "Handgun (Billy)", kWeapon, 15},
    {4, "Handgun (Rebecca)", kWeapon, 15},
    {5, "Hunting Gun", kWeapon, 2},
    {6, "Shotgun", kWeapon, 7},
    {7, "Grenade Launcher (Grenade)", kWeapon, 255},
    {8, "Grenade Launcher (Napalm)", kWeapon, 255},
    {9, "Grenade Launcher (Acid)", kWeapon, 255},
    {10, "Magnum", kWeapon, 8},
    {11, "Sub-machine Gun", kWeapon, 300},
    {14, "Molotov Cocktail", kWeapon, 255},
    {17, "Custom Handgun (Billy)", kWeapon, 15},
    {19, "Custom Handgun (Rebecca)", kWeapon, 15},
    {22, "Magnum Revolver", kWeapon, 5},
    {23, "Rocket Launcher", kWeapon, 2},
    {26, "Handgun Parts", kOther, 1},
    {32, "Handgun Ammo", kAmmo, 255},
    {33, "Shotgun Shells", kAmmo, 255},
    {34, "Magnum Rounds", kAmmo, 255},
    {35, "Grenade Rounds", kAmmo, 255},
    {36, "Acid Rounds", kAmmo, 255},
    {37, "Napalm Rounds", kAmmo, 255},
    {38, "Empty Bottle", kOther, 255},
    {39, "Gas Tank", kOther, 255},
    {40, "Machine Gun Ammo", kAmmo, 300},
    {43, "Green Herb", kHeal, 1},
    {44, "Blue Herb", kHeal, 1},
    {45, "Red Herb", kHeal, 1},
    {46, "Mixed Herb (G+G)", kHeal, 1},
    {47, "Mixed Herb (G+G+G)", kHeal, 1},
    {48, "Mixed Herb (G+R)", kHeal, 1},
    {49, "Mixed Herb (G+B)", kHeal, 1},
    {50, "Mixed Herb (G+G+B)", kHeal, 1},
    {51, "Mixed Herb (G+R+B)", kHeal, 1},
    {53, "First Aid Spray", kHeal, 1},
    {55, "Ink Ribbon", kOther, 255},
    {57, "Gold Ring", kKey, 1},
    {58, "Silver Ring", kKey, 1},
    {59, "Briefcase", kKey, 1},
    {60, "Briefcase (+Gold Ring)", kKey, 1},
    {61, "Briefcase (+Silver Ring)", kKey, 1},
    {62, "Briefcase (+both rings)", kKey, 1},
    {63, "Lighter Fluid", kKey, 1},
    {65, "Train Key (Conductor's)", kKey, 1},
    {66, "Breeding Room Key", kKey, 1},
    {67, "Elevator Key", kKey, 1},
    {68, "Facility Key (Fire)", kKey, 1},
    {70, "Facility Key (Water)", kKey, 1},
    {71, "Blue Leech Charm (raw)", kKey, 1},
    {72, "Green Leech Charm (raw)", kKey, 1},
    {73, "Factory Key (Up)", kKey, 1},
    {75, "Blue Keycard", kKey, 1},
    {76, "Keycard (Umbrella)", kKey, 1},
    {77, "Magnetic Card (Brakes)", kKey, 1},
    {78, "Locker Key", kKey, 1},
    {79, "Unity Tablet", kKey, 1},
    {80, "Obedience Tablet", kKey, 1},
    {81, "Discipline Tablet", kKey, 1},
    {82, "Panel Opener", kKey, 1},
    {83, "Vise Handle", kKey, 1},
    {86, "Crank Handle", kKey, 1},
    {87, "Handle", kKey, 1},
    {88, "Book of Good", kKey, 1},
    {89, "Book of Evil", kKey, 1},
    {90, "Blue Leech Charm", kKey, 10},
    {91, "Green Leech Charm", kKey, 10},
    {92, "Input Reg. Coil", kKey, 1},
    {93, "Output Reg. Coil", kKey, 1},
    {94, "Battery", kKey, 1},
    {95, "Hi-Power Battery (filled)", kKey, 1},
    {96, "Sterilizing Agent", kKey, 1},
    {97, "Motherboard", kKey, 1},
    {99, "Microfilm A", kKey, 1},
    {100, "Microfilm B", kKey, 1},
    {102, "Shaft Key (L)", kKey, 1},
    {103, "Train Key (Dining Car)", kKey, 1},
    {104, "Hookshot", kKey, 1},
    {105, "Lighter (empty)", kKey, 1},
    {106, "Lighter", kKey, 1},
    {107, "Conductor's Key", kKey, 1},
    {108, "Fire Key", kKey, 1},
    {110, "Water Key", kKey, 1},
    {111, "Up Key", kKey, 1},
    {113, "Dining Car Key", kKey, 1},
    {117, "Leech Capsule", kKey, 1},
    {118, "Dial", kKey, 1},
    {119, "Duralumin Case", kKey, 1},
    {120, "Statue of Evil", kKey, 1},
    {121, "Statue of Good", kKey, 1},
    {122, "Jewelry Box", kKey, 1},
    {123, "Mixing Set", kKey, 1},
    {124, "MO Disk", kKey, 1},
    {125, "Ice Pick", kKey, 1},
    {126, "Iron Needle", kKey, 1},
    {127, "Shaft Key (R)", kKey, 1},
    {128, "Industrial Water", kKey, 1},
    {129, "Empty Battery", kKey, 1},
    {130, "Angel Wings", kKey, 1},
    {131, "Black Wings", kKey, 1},
    {132, "White Statue", kKey, 1},
    {133, "Black Statue", kKey, 1},
    {134, "Red Chemical", kKey, 1},
    {135, "Blue Chemical", kKey, 1},
    {136, "Green Chemical", kKey, 1},
    {137, "Stripping Agent", kKey, 1},
    {139, "Sulfuric Acid", kKey, 1},
    {140, "Battery Fluid", kKey, 1},
    {141, "Closet Key", kKey, 1},
    {150, "File: Player's Manual 1", kFile, 1},
    {151, "File: Player's Manual 2", kFile, 1},
    {152, "File: Court Order for Transportation", kFile, 1},
    {153, "File: Investigation Orders", kFile, 1},
    {154, "File: Notice to Supervisors", kFile, 1},
    {155, "File: Passenger's Diary", kFile, 1},
    {156, "File: Brake Operation Manual", kFile, 1},
    {157, "File: Note from Conductor", kFile, 1},
    {158, "File: Regulations for Trainees", kFile, 1},
    {159, "File: Notice to All Staff", kFile, 1},
    {160, "File: Marcus' Diary 1", kFile, 1},
    {161, "File: Assistant Director's Diary", kFile, 1},
    {162, "File: About the Power Regulator", kFile, 1},
    {163, "File: A Verse of Poetry", kFile, 1},
    {164, "File: Management Trainee's Diary", kFile, 1},
    {166, "File: Correctional Institute Inmates List", kFile, 1},
    {167, "File: First Investigation Unit Notes", kFile, 1},
    {168, "File: Marcus' Diary 2", kFile, 1},
    {169, "File: Old Photograph", kFile, 1},
    {170, "File: Investigator's Report", kFile, 1},
    {171, "File: Leech Growth Records", kFile, 1},
    {172, "File: Laboratory Manager's Diary", kFile, 1},
    {173, "File: B.O.W. Report", kFile, 1},
    {174, "File: Microfilm Image", kFile, 1},
    {175, "File: Investigator's Report 2", kFile, 1},
    {176, "File: Treatment Plant Manager's Diary", kFile, 1},
    {177, "File: Gate Operation Manual", kFile, 1},
    {178, "File: About Battery Fluid", kFile, 1},
    {179, "File: Hookshot Operator's Manual", kFile, 1},
    {180, "--- (second half of a 2-slot item)", kFiller, 1},
};
inline constexpr int kCount = sizeof(kTable) / sizeof(kTable[0]);
inline constexpr int kFillerId = 180;
inline constexpr int kInkRibbonId = 55;
inline constexpr int kMaxCount = 999;

// The game's own "infinite": a slot count of exactly 0xFFFF. Bag::isInfinite
// (exe+0xDBE50) ends in `cmp dword [slot+4],0xFFFF`, Bag::take (exe+0xDC3E0)
// takes nothing from such a slot, Bag::count (exe+0xDC290) reports 0xFFFF for
// it and Bag::add never tops it up. A new game with the Rocket Launcher
// unlocked hands it to Rebecca that way (exe+0xDD8E2). It is a marker, not a
// number: it sits above every per-slot maximum on purpose, and clamping it to
// one turns an infinite weapon into a two-round one.
inline constexpr int kInfiniteCount = 0xFFFF;
inline bool infinite(int count) { return count == kInfiniteCount; }

// " x12" after an item's name - or " (infinite)" for the marker above.
struct Quantity {
  char text[16];
};
inline Quantity quantity(int count) {
  Quantity q;
  if (infinite(count)) std::snprintf(q.text, sizeof(q.text), " (infinite)");
  else std::snprintf(q.text, sizeof(q.text), " x%d", count);
  return q;
}

inline const Def* find(int id) {
  for (const Def& d : kTable)
    if (d.id == id) return &d;
  return nullptr;
}
inline const char* name(int id) {
  const Def* d = find(id);
  return d ? d->name : "(unknown)";
}
inline bool two_slot(int id) {
  static const int k[] = {5, 6, 7, 8, 9, 11, 12, 23, 104};
  for (int v : k)
    if (v == id) return true;
  return false;
}
// The items the game stacks: a slot holding one of them takes more of the
// same, up to max_count, where every other item needs a slot of its own for
// each one - a weapon's count is its magazine, not a number of guns.
// game::item_stacks reads the game's own list out of its pickup code and
// supersedes this one, which is that list as build 17178773 has it.
inline bool stacks(int id) {
  static const int k[] = {14, 32, 33, 34, 35, 36, 37, 38, 39, 40, 55, 90, 91};
  for (int v : k)
    if (v == id) return true;
  return false;
}
inline bool is_weapon(int id) {
  const Def* d = find(id);
  return d && d->kind == kWeapon;
}
inline int max_count(int id) {
  const Def* d = find(id);
  return d && d->max > 0 ? d->max : kMaxCount;
}

// The storage box's banks, in the order the panel's tabs show them. A bank is
// only where an entry is listed - the box is one list, kept in bank order - so
// an item filed under the wrong bank is misplaced, never mishandled. Anything
// the table does not know is listed under Other.
enum Bank { kBankKey = 0, kBankWeapons, kBankAmmo, kBankHeal, kBankOther, kBankCount };
inline const char* bank_name(int b) {
  static const char* const kNames[kBankCount] = {"Key items", "Weapons", "Ammo", "Heals", "Other"};
  return b >= 0 && b < kBankCount ? kNames[b] : kNames[kBankOther];
}
inline int bank(int id) {
  const Def* d = find(id);
  switch (d ? d->kind : kOther) {
    case kKey: return kBankKey;
    case kWeapon: return kBankWeapons;
    case kAmmo: return kBankAmmo;
    case kHeal: return kBankHeal;
    default: return kBankOther;
  }
}

// Names compared the way a reader sorts them, case aside: "MO Disk" lands
// between "Microfilm B" and "Motherboard" rather than ahead of every other M.
inline int compare_names(const char* a, const char* b) {
  const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + 32 : static_cast<int>(c); };
  for (;; ++a, ++b) {
    const int d = lower(*a) - lower(*b);
    if (d || !*a) return d;
  }
}

// The order the storage box lists items in: bank by bank, and by name within
// a bank. An id the table has no name for goes last, by number.
inline bool listed_before(int a, int b) {
  const int ka = bank(a), kb = bank(b);
  if (ka != kb) return ka < kb;
  const Def* da = find(a);
  const Def* db = find(b);
  if (!da || !db) return da ? true : db ? false : a < b;
  const int c = compare_names(da->name, db->name);
  return c ? c < 0 : a < b;
}

}  // namespace re0cc::items
