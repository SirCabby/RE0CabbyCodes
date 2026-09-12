#include "storage.h"

#include <io.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "items.h"
#include "log.h"

namespace re0cc::storage {

// An item the game stacks tops up a slot of the same item to one slot's worth
// (game::item_stacks, game::item_max), and the box does what a slot would.
// Every other item is one thing per slot whatever its count - a weapon's count
// is its magazine - so two of them stay two entries: adding their counts
// together would destroy one.
int stack_size(int id) {
  if (!game::item_stacks(id)) return 0;
  const int mx = game::item_max(id);
  return mx > 1 ? mx : 0;
}

Fit fit(const game::Bag& bag, int id, int count) {
  Fit f;
  const int want = count < 0 ? 0 : count;
  int rest = want;
  // The game's infinite is a marker, not an amount: it tops nothing up, and
  // an infinite slot is never topped up either (Bag::add skips one too).
  const int cap = items::infinite(want) ? 0 : stack_size(id);
  for (int s = 0; cap && s < 6 && rest > 0; ++s) {
    if (bag.id[s] != id || items::infinite(bag.count[s]) || bag.count[s] >= cap) continue;
    f.onto[s] = cap - bag.count[s] < rest ? cap - bag.count[s] : rest;
    rest -= f.onto[s];
  }
  // A free slot for what is left - or for the whole entry when nothing was
  // topped up, which is how an empty gun (x0) still goes back.
  if (rest > 0 || rest == want) {
    const bool two = items::two_slot(id);
    for (int s = 0; s < 6 && f.slot < 0; ++s) {
      if (bag.id[s] != 0) continue;
      if (!two) f.slot = s;
      else if ((s % 2) == 0 && s < 5 && bag.id[s + 1] == 0) f.slot = s;  // two-slot items sit on an even slot
    }
    if (f.slot >= 0) {
      // One slot's worth. The game's infinite sits above every per-slot
      // maximum on purpose, so it goes back exactly as it came out: split like
      // a stack, the unlocked Rocket Launcher would come back with the two
      // rounds a slot holds and leave 65533 of them behind in the box.
      const int mx = game::item_max(id);
      f.into = rest > mx && !items::infinite(rest) ? mx : rest;
      rest -= f.into;
    }
  }
  f.left = rest;
  return f;
}

namespace {

// The order the box keeps: bank by bank and by name (items::listed_before),
// and of one item the fullest stack first - a weapon's count is its magazine,
// and the game's infinite marker sits above every count, so an infinite weapon
// heads its own name. Kept on every change rather than sorted for the panel,
// so an index the panel draws is the box's own.
bool stored_before(const Entry& a, const Entry& b) {
  if (a.id != b.id) return items::listed_before(a.id, b.id);
  return a.count > b.count;
}

// `total` of an item laid out the way inventory slots would hold it: full
// stacks of `cap`, then whatever is left. A store of a count of 0 still gets an
// entry, so it never vanishes without a trace.
int stacks_for(int total, int cap) { return total > cap ? (total + cap - 1) / cap : 1; }

// The part of an item the box merges: every entry of it but the game's
// infinite, which is a marker rather than an amount and stays as it came.
bool merges(const Entry& e, int id) { return e.id == id && !items::infinite(e.count); }

struct Box {
  std::vector<Entry> items;  // in stored_before order, always

  int size() const { return static_cast<int>(items.size()); }
  void clear() { items.clear(); }
  bool full() const { return items.size() >= static_cast<size_t>(kMaxItems); }
  void place(const Entry& e) { items.insert(std::upper_bound(items.begin(), items.end(), e, stored_before), e); }
  // One entry exactly as the ini has it. A record is merged when its file is
  // loaded (consolidate), not when it is read.
  bool insert(int id, int count) {
    if (full()) return false;
    place({id, count});
    return true;
  }
  void remove(int i) {
    if (i >= 0 && i < size()) items.erase(items.begin() + i);
  }
  int held(int bank) const {  // entries listed under that bank
    int n = 0;
    for (const Entry& e : items)
      if (items::bank(e.id) == bank) ++n;
    return n;
  }

  // How much of an item the box holds in its merged stacks, and in how many.
  struct Tally {
    int total = 0, stacks = 0;
  };
  Tally tally(int id) const {
    Tally t;
    for (const Entry& e : items)
      if (merges(e, id)) t.total += e.count, ++t.stacks;
    return t;
  }
  // Whether `count` of `id` still fits under the cap, merged the way put would
  // merge it: topping up a stack that is already there takes no new entry.
  bool room_for(int id, int count) const {
    const int cap = items::infinite(count) ? 0 : stack_size(id);
    if (!cap) return !full();
    const Tally t = tally(id);
    return size() - t.stacks + stacks_for(t.total + count, cap) <= kMaxItems;
  }
  // `count` of `id` into the box. An item the game stacks joins the stacks of
  // it already there and the lot is laid out again the way slots would hold it
  // - full stacks of one slot's worth and at most one short one - so a few
  // rounds top up a stack instead of adding an entry. Anything else, and the
  // game's infinite, goes in as an entry of its own. It takes whatever it is
  // given: the cap is room_for's to ask, before an item leaves the inventory.
  // Returns how many stacks of the item were already there to join.
  int put(int id, int count) {
    const int cap = items::infinite(count) ? 0 : stack_size(id);
    if (!cap) {
      place({id, count});
      return 0;
    }
    const Tally t = tally(id);
    items.erase(std::remove_if(items.begin(), items.end(), [id](const Entry& e) { return merges(e, id); }),
                items.end());
    lay_out(id, t.total + count, cap);
    return t.stacks;
  }
  // The whole box laid out that way at once, for a record out of the ini: one
  // written before the box merged anything holds an entry for every store.
  // Every item and every count stays; only the entries they sit in change.
  // Returns how many entries fewer the box has.
  int consolidate() {
    const int before = size();
    int total[256] = {};
    bool seen[256] = {};
    std::vector<Entry> rest;  // what is left of a sorted list is still sorted
    for (const Entry& e : items) {
      if (e.id > 0 && e.id < 256 && !items::infinite(e.count) && stack_size(e.id)) {
        total[e.id] += e.count;
        seen[e.id] = true;
      } else {
        rest.push_back(e);
      }
    }
    items.swap(rest);
    for (int id = 1; id < 256; ++id)
      if (seen[id]) lay_out(id, total[id], stack_size(id));
    return before - size();
  }
  void lay_out(int id, int total, int cap) {
    for (int n = stacks_for(total, cap); n > 0; --n) {
      const int c = total > cap ? cap : total;
      place({id, c});
      total -= c;
    }
  }
};

Box g_box;                // what the box holds right now
Box g_records[kSlots];    // what each save file's record holds - one box per file, always
CRITICAL_SECTION g_lock;  // the box, the records and the request queue
bool g_ready = false;
int g_bound_mode = -1;    // the game mode the box belongs to (-1 = nothing yet)
int g_bound_slot = -1;    // the save file it belongs to
bool g_file_ok = false;
char g_path[MAX_PATH] = {};
char g_tmp_path[MAX_PATH] = {};

int mode_index(game::Mode m) {
  switch (m) {
    case game::Mode::kMainGame: return 0;
    case game::Mode::kWesker: return 1;
    case game::Mode::kLeechHunter: return 2;
    default: break;
  }
  return -1;  // the menus, or a reading that says nothing
}
constexpr int kLeech = 2;  // mode_index's Leech Hunter

// The operation the engine's save/load task is working on, remembered for as
// long as it runs (see watch_saves).
struct Running {
  bool active = false;
  int mode = 0;
  int slot = -1;
  bool is_save = false;
  bool is_copy = false;
  bool is_clear = false;  // the main game's ending: the file keeps no box (see game::SaveOp)
  bool is_leech = false;  // Leech Hunter's result screen: the file's box is left as it was
};
Running g_run;

// A save file the load list's panel is deleting or copying (savefiles.cpp):
// noted before the game writes data0.bin, made the records' own when the game
// says the write succeeded. One read back from the ini at start-up belongs to
// a run that ended before the game answered (left_over), and is settled from
// the game's own save files instead (settle_left_over).
struct PendingChange {
  bool active = false;
  bool copy = false;
  int from = -1, to = -1;
  bool left_over = false;
};
PendingChange g_pending;

const char* change_name(const PendingChange& c, char* buf, size_t n) {
  if (c.copy) std::snprintf(buf, n, "the copy of save file %d to save file %d", c.from + 1, c.to + 1);
  else std::snprintf(buf, n, "the deletion of save file %d", c.to + 1);
  return buf;
}

// Session tracking (see watch_session).
bool g_in_game = false;
int g_mode = -1;              // the mode the presence named last (-1 = no game is running)
bool g_started = false;       // that mode change already bound the box; the units need not
bool g_load_pending = false;  // a load event that has not reached its game yet
int g_load_slot = -1;
DWORD g_load_at = 0;
DWORD g_out_since = 0;  // when the last session ended (0 = none has yet)

// The player units can be gone for a moment inside a running game (a cutscene,
// a chapter change); only a gap long enough to have been the menus counts.
constexpr DWORD kUnitGapMs = 3000;
// How long a finished load may wait for the game it brings up. The game starts
// within a frame or two of the load on the continue path (the file is applied
// and the presence set right after the disk is done); the window only has to
// cover a load the player abandons, which nothing else would ever clear.
constexpr DWORD kLoadWaitMs = 120000;

void note(const char* fmt, ...) {
  char text[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  logf("storage: %s", text);
}

// --- requests (render thread -> main thread) -------------------------------------------------
struct Request {
  enum Type { kStore, kTake, kDrop } type;
  int a, b, c, d;
};
Request g_req[32];
int g_req_n = 0;

void push(Request r) {
  if (!g_ready) return;
  EnterCriticalSection(&g_lock);
  if (g_req_n < 32) g_req[g_req_n++] = r;
  LeaveCriticalSection(&g_lock);
}

int drain(Request* out, int max) {
  if (!g_ready) return 0;
  EnterCriticalSection(&g_lock);
  const int n = g_req_n < max ? g_req_n : max;
  std::memcpy(out, g_req, sizeof(Request) * static_cast<size_t>(n));
  g_req_n = 0;
  LeaveCriticalSection(&g_lock);
  return n;
}

// --- the ini beside the DLL ------------------------------------------------------------------
// file<N> = <item id>:<count>,<item id>:<count>...   (N is 1-based, as the panel shows it)
void write_file() {
  FILE* f = std::fopen(g_tmp_path, "w");
  if (!f) {
    g_file_ok = false;
    logf("ERROR: storage: cannot write %s", g_tmp_path);
    return;
  }
  std::fprintf(f,
               "# RE0CabbyCodes - item storage\n"
               "#\n"
               "# One line per save file: what the mod's storage box held when that file was last\n"
               "# saved. Loading the file puts those items back in the box. The game's own save\n"
               "# file is never touched - delete this file and you only lose the box.\n"
               "#\n"
               "#   file<N> = <item id>:<count>,<item id>:<count>, ...\n"
               "#\n"
               "# A file has one box, whatever was played into it. Saving a cleared game leaves\n"
               "# that file with an empty one, the way it leaves you a file to start over from.\n"
               "# N counts save files from 1; the ids are the game's own (the panel names them).\n"
               "# A count of 65535 is the game's own \"infinite\" (the unlocked Rocket Launcher).\n"
               "# Ammo, ink ribbons and the other items the game stacks are kept merged, in stacks\n"
               "# of what one inventory slot holds; a line from before that is merged on loading.\n"
               "# The panel files each item under its bank and keeps every bank sorted by name,\n"
               "# so the order of a line does not matter.\n"
               "#\n"
               "# Deleting a save file from the title screen's load list empties its box, and\n"
               "# copying one gives the copy the box of the file it came from. A line\n"
               "#   pending = copy <from> <to>   or   pending = delete <file>\n"
               "# is such a change the game was still writing when it last closed: it is settled\n"
               "# from the game's own save files the next time they are loaded.\n");
  int used = 0;
  for (int s = 0; s < kSlots; ++s) {
    const Box& b = g_records[s];
    if (b.items.empty()) continue;
    ++used;
    std::fprintf(f, "file%d =", s + 1);
    for (int i = 0; i < b.size(); ++i) std::fprintf(f, "%s%d:%d", i ? "," : " ", b.items[i].id, b.items[i].count);
    std::fprintf(f, "\n");
  }
  if (g_pending.active) {
    if (g_pending.copy) std::fprintf(f, "pending = copy %d %d\n", g_pending.from + 1, g_pending.to + 1);
    else std::fprintf(f, "pending = delete %d\n", g_pending.to + 1);
  }
  // Onto the disk before the rename: fclose only hands the bytes to the OS
  // cache, so an unclean shutdown can land the rename with nothing behind it
  // and lose a record that the game's own save file kept.
  std::fflush(f);
  const HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(f)));
  if (h != INVALID_HANDLE_VALUE) FlushFileBuffers(h);
  std::fclose(f);
  // Replace in one step: a crash mid-write must not leave a half-written file.
  if (!MoveFileExA(g_tmp_path, g_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    g_file_ok = false;
    logf("ERROR: storage: cannot replace %s (error %lu)", g_path, GetLastError());
    return;
  }
  g_file_ok = true;
  logf("storage: wrote %s (%d record(s) with items)", g_path, used);
}

void read_file() {
  FILE* f = std::fopen(g_path, "r");
  if (!f) {
    g_file_ok = true;  // nothing stored yet is not a failure
    logf("storage: no %s yet - the box starts empty", g_path);
    return;
  }
  char line[4096];
  int files = 0, entries = 0, dropped = 0;
  while (std::fgets(line, sizeof(line), f)) {
    char* p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '#' || *p == ';' || *p == '[' || *p == '\n' || *p == '\r' || !*p) continue;
    char* eq = std::strchr(p, '=');
    if (!eq) continue;
    *eq = '\0';
    for (char* e = p + std::strlen(p); e > p && (e[-1] == ' ' || e[-1] == '\t');) *--e = '\0';
    if (!_stricmp(p, "pending")) {
      char what[16] = {};
      int a = 0, b = 0;
      const int got = std::sscanf(eq + 1, " %15s %d %d", what, &a, &b);
      if (got >= 2 && !_stricmp(what, "delete") && a >= 1 && a <= kSlots)
        g_pending = {true, false, -1, a - 1, true};
      else if (got == 3 && !_stricmp(what, "copy") && a >= 1 && a <= kSlots && b >= 1 && b <= kSlots && a != b)
        g_pending = {true, true, a - 1, b - 1, true};
      else
        logf("storage: 'pending =%s' in %s is not a change this version knows - ignored", eq + 1, g_path);
      continue;
    }
    if (_strnicmp(p, "file", 4) != 0) {
      logf("storage: unknown key '%s' in %s ignored", p, g_path);
      continue;
    }
    const int slot = static_cast<int>(std::strtol(p + 4, nullptr, 10)) - 1;
    if (slot < 0 || slot >= kSlots) {
      logf("storage: '%s' is not a save file this game has (1..%d) - ignored", p, kSlots);
      continue;
    }
    Box& b = g_records[slot];
    b.clear();
    for (const char* v = eq + 1; *v;) {
      while (*v == ' ' || *v == ',' || *v == '\t') ++v;
      if (!*v || *v == '\n' || *v == '\r') break;
      char* end = nullptr;
      const long id = std::strtol(v, &end, 10);
      long count = 1;
      if (end && *end == ':') count = std::strtol(end + 1, &end, 10);
      v = end ? end : v + 1;
      if (id <= 0 || id > 255 || id == items::kFillerId || count < 0 ||
          (count > items::kMaxCount && count != items::kInfiniteCount) || b.full()) {
        ++dropped;
        continue;
      }
      b.insert(static_cast<int>(id), static_cast<int>(count));
      ++entries;
    }
    if (!b.items.empty()) ++files;
  }
  std::fclose(f);
  g_file_ok = true;
  logf("storage: read %s - %d item(s) across %d record(s)%s", g_path, entries, files,
       dropped ? " (some entries did not make sense and were dropped)" : "");
  if (g_pending.active) {
    char what[80];
    logf("storage: %s was still being written when the game last closed - it is settled from the game's own save "
         "files once they are loaded",
         change_name(g_pending, what, sizeof(what)));
  }
}

// --- moving items between a bag and the box --------------------------------------------------
const char* who(int bag) { return bag == 0 ? "Rebecca" : "Billy"; }

void do_store(int bag, int slot) {
  game::Bag bags[2];
  if (!game::bags(bags) || bag < 0 || bag > 1 || slot < 0 || slot > 5) {
    note("cannot store: the inventory is not available");
    return;
  }
  const game::Bag& b = bags[bag];
  const int id = b.id[slot], count = b.count[slot];
  if (id == 0) {
    note("%s slot %d is empty", who(bag), slot + 1);
    return;
  }
  if (id == items::kFillerId) {
    note("%s slot %d is the second half of a two-slot item - store slot %d instead", who(bag), slot + 1, slot);
    return;
  }
  if (!g_box.room_for(id, count)) {
    note("the storage is full (%d items)", kMaxItems);
    return;
  }
  const bool two = items::two_slot(id) && slot < 5 && b.id[slot + 1] == items::kFillerId;
  if (!game::write_slot(bag, slot, 0, 0)) {
    note("could not clear %s slot %d", who(bag), slot + 1);
    return;
  }
  if (two) game::write_slot(bag, slot + 1, 0, 0);
  if (b.equipped == slot) game::write_equipped(bag, -1);
  const int joined = g_box.put(id, count);
  const int bank = items::bank(id);
  if (joined) {
    const Box::Tally t = g_box.tally(id);
    note("stored %s x%d from %s slot %d with the rest of it under %s: x%d now, in %d stack%s of up to %d (%d in the box)",
         items::name(id), count, who(bag), slot + 1, items::bank_name(bank), t.total, t.stacks, t.stacks == 1 ? "" : "s",
         stack_size(id), g_box.size());
  } else {
    note("stored %s%s from %s slot %d under %s (%d there, %d in the box)", items::name(id), items::quantity(count).text,
         who(bag), slot + 1, items::bank_name(bank), g_box.held(bank), g_box.size());
  }
}

// The entry the panel clicked, found again. The box can move on between the
// click and the tick - every store re-files it, which shifts the entries after
// the new one - and acting on the wrong item is worse than not acting. Two
// entries of one item with the same count are the same thing, so either will
// do; nothing else is what was clicked. -1 when it is gone.
int find_entry(int index, int id, int count) {
  const auto is = [&](int i) { return g_box.items[i].id == id && g_box.items[i].count == count; };
  if (index >= 0 && index < g_box.size() && is(index)) return index;
  for (int i = 0; i < g_box.size(); ++i)
    if (is(i)) return i;
  note("the storage moved on since that was clicked - try again");
  return -1;
}

void do_take(int index, int id, int seen, int bag) {
  game::Bag bags[2];
  if (!game::bags(bags) || bag < 0 || bag > 1) {
    note("cannot take: the inventory is not available");
    return;
  }
  index = find_entry(index, id, seen);
  if (index < 0) return;
  const Entry e = g_box.items[index];
  const game::Bag& b = bags[bag];
  const bool two = items::two_slot(e.id);
  // Onto the stacks of it the character carries, then into a free slot (fit).
  // What fits nowhere stays in the box. A box entry can hold more than the
  // inventory has room for, and the overflow used to be dropped on the floor
  // here: taking a ten-charm stack back once handed over one charm and
  // destroyed the other nine.
  const Fit f = fit(b, e.id, e.count);
  if (!f.ok()) {
    if (stack_size(e.id) && !items::infinite(e.count))
      note("%s has no free slot for %s, and no stack of it with room", who(bag), items::name(e.id));
    else
      note("%s has no free %s for %s", who(bag), two ? "pair of slots" : "slot", items::name(e.id));
    return;
  }
  // What leaves the box is what reached the inventory, should a write fail on
  // the way.
  int placed = 0;
  bool written = true;
  char where[128] = "";
  int len = 0;
  const auto said = [&](int s, int now) {
    if (len < static_cast<int>(sizeof(where)))
      len += std::snprintf(where + len, sizeof(where) - len, "%sslot %d now x%d", len ? ", " : "", s + 1, now);
  };
  for (int s = 0; s < 6 && written; ++s) {
    if (!f.onto[s]) continue;
    const int now = b.count[s] + f.onto[s];
    written = game::write_slot(bag, s, e.id, now);
    if (!written) break;
    placed += f.onto[s];
    said(s, now);
  }
  bool into = false;
  if (written && f.slot >= 0) {
    into = game::write_slot(bag, f.slot, e.id, f.into);
    if (into) {
      placed += f.into;
      said(f.slot, f.into);
      if (two) game::write_slot(bag, f.slot + 1, items::kFillerId, 1);
    }
  }
  if (!placed && !into) {
    note("could not write %s's inventory", who(bag));
    return;
  }
  // What stays behind goes back in with the rest of that item (Box::put).
  g_box.remove(index);
  const int left = items::infinite(e.count) ? 0 : e.count - placed;
  if (left > 0) g_box.put(e.id, left);
  if (f.topped() == 0 && left > 0)
    note("took %s x%d into %s slot %d - a slot holds %d, so x%d stayed in the box (%d item(s) in it)",
         items::name(e.id), f.into, who(bag), f.slot + 1, game::item_max(e.id), left, g_box.size());
  else if (f.topped() == 0)
    note("took %s%s into %s slot %d (%d left in the box)", items::name(e.id), items::quantity(f.into).text, who(bag),
         f.slot + 1, g_box.size());
  else if (left > 0)
    note("took %s x%d into %s's inventory: %s - x%d had no room and stayed in the box (%d item(s) in it)",
         items::name(e.id), placed, who(bag), where, left, g_box.size());
  else
    note("took %s x%d into %s's inventory: %s (%d left in the box)", items::name(e.id), placed, who(bag), where,
         g_box.size());
}

void do_drop(int index, int id, int seen) {
  index = find_entry(index, id, seen);
  if (index < 0) return;
  const Entry e = g_box.items[index];
  g_box.remove(index);
  note("threw away %s%s (%d left in the box)", items::name(e.id), items::quantity(e.count).text, g_box.size());
}

// --- the game's own save and load ------------------------------------------------------------
// A game with no box of its own yet starts with an empty one. What the box
// held is the last game's, and stays with whatever save that game made.
void empty_box(const char* why) {
  const int had = g_box.size();
  g_box.clear();
  g_bound_slot = -1;
  if (had) note("%s: the storage starts empty (the %d item(s) of the last game are not carried over)", why, had);
  else logf("storage: %s - the box was already empty", why);
}

// Every file keeps exactly one box, whatever was played into it - Wesker mode
// writes the same twenty files the main game does, so a file is a file.
//
// Two saves do not hand the box on:
//  - the main game's cleared-game save. It is the game's own "start again from
//    here" file: the next thing it is good for is a new game, which starts with
//    nothing, so the record it leaves behind has to start with nothing too;
//  - Leech Hunter's save, from its result screen. The run's box is the run's
//    and ends with it, and whatever box the file already had is none of the
//    run's business either: the record is left exactly as it was, and the ini
//    is not written at all.
void on_saved(int slot, bool copy, bool clear, bool leech) {
  if (leech) {
    const int had = g_box.size();
    g_box.clear();
    g_bound_slot = -1;
    note("Leech Hunter saved save file %d: its box (%d item(s)) is not kept, and the file's box is left as it was",
         slot + 1, had);
    return;
  }
  if (clear) {
    g_records[slot].clear();
    write_file();
    empty_box("the game was beaten and saved");
    g_bound_slot = slot;  // empty_box unbinds; the box is that file's, and that file's is empty
    note("save file %d is a cleared game: it keeps no storage, and a new game from it starts empty", slot + 1);
    return;
  }
  g_records[slot] = g_box;
  g_bound_slot = slot;
  write_file();
  note("save file %d saved: %d item(s) kept with it%s", slot + 1, g_box.size(), copy ? " (file copy)" : "");
}

// A load is only half the story: it says the game read a file off the disk, not
// which game it is about to bring up. The box is put together when that game
// starts (begin_game below), which is when the mode is known.
void on_loaded(int slot) {
  g_load_pending = true;
  g_load_slot = slot;
  g_load_at = GetTickCount();
  logf("storage: the game loaded save file %d - the box follows when that game comes up", slot + 1);
}

// A game is starting. Only a continue gets a box back: the one the load that
// just finished brought up, which has that file's. Everything else starts
// empty, and a load right before it does not change that - the title reads the
// chosen file off the disk before every kind of start that uses one:
//
//  - a new game from a save file (Once Again, from a cleared one) carries that
//    file's unlocks into a new game, and a new game starts with nothing. The
//    new-game routines push their opening phase in the same breath as they file
//    the mode, so asking the phase machine now tells them from a continue
//    (game::new_game_starting);
//  - Leech Hunter is laid out on top of the chosen (cleared) file and is not
//    that file's game: its one save, from the result screen, leaves the file's
//    box alone (on_saved). It gets an empty box of its own, tied to no file,
//    and nothing of a file's box goes into it.
//
// Leech Hunter's box ends with the run the same way: whatever starts after it
// does not get it back.
void settle_left_over();  // below

void begin_game(int mode, const char* why) {
  // A deletion or copy the last run never saw written decides what the file
  // this game may load holds: settled first, from the save files the title has
  // just read.
  settle_left_over();
  if (mode < 0) mode = g_bound_mode >= 0 ? g_bound_mode : 0;
  const DWORD now = GetTickCount();
  const bool staged = g_load_pending && now - g_load_at <= kLoadWaitMs && g_load_slot >= 0 && g_load_slot < kSlots;
  const int slot = g_load_slot;
  g_load_pending = false;
  g_load_slot = -1;
  const bool fresh = staged && mode != kLeech && game::new_game_starting();
  EnterCriticalSection(&g_lock);
  if (g_bound_mode == kLeech && mode != kLeech && g_box.size()) {
    logf("storage: Leech Hunter is over - its box (%d item(s)) is not kept", g_box.size());
    g_box.clear();
  }
  g_bound_mode = mode;
  if (mode == kLeech) {
    g_box.clear();
    g_bound_slot = -1;
    if (staged)
      note("%s: Leech Hunter, from save file %d - it gets an empty box of its own, and that file's stays out of it", why,
           slot + 1);
    else
      note("%s: Leech Hunter - it gets an empty box of its own, which is not kept", why);
  } else if (fresh) {
    char reason[128];
    std::snprintf(reason, sizeof(reason), "%s (a new game from save file %d - that file's box stays out of it)", why,
                  slot + 1);
    empty_box(reason);
  } else if (staged) {
    // A record the ini kept from before the box merged anything is merged now,
    // and the next save writes it back that way.
    g_box = g_records[slot];
    g_bound_slot = slot;
    const int merged = g_box.consolidate();
    if (merged)
      note("%s: save file %d is back, with %d item(s) in the storage (%d stack(s) merged with the rest of their item)",
           why, slot + 1, g_box.size(), merged);
    else
      note("%s: save file %d is back, with %d item(s) in the storage", why, slot + 1, g_box.size());
  } else {
    char reason[96];
    std::snprintf(reason, sizeof(reason), "%s (no file was loaded, so it is a new game)", why);
    empty_box(reason);
  }
  LeaveCriticalSection(&g_lock);
}

// A load the game never turned into a session must not be left to vouch for
// whatever starts next - that is how a new game used to come up holding the
// items of the file the player had looked at before changing their mind.
void drop_pending_load(const char* why) {
  if (!g_load_pending) return;
  g_load_pending = false;
  logf("storage: save file %d was loaded but no game came up (%s) - it is not held against the next one",
       g_load_slot + 1, why);
  g_load_slot = -1;
}

// The latched operation is over: apply it to the box.
void finish_run(int result) {
  const Running r = g_run;
  g_run = Running{};
  if (result != 0) {
    logf("storage: the game's %s of save file %d failed (result %d) - storage left alone",
         r.is_save ? "save" : "load", r.slot + 1, result);
    return;
  }
  EnterCriticalSection(&g_lock);
  if (r.is_save) on_saved(r.slot, r.is_copy, r.is_clear, r.is_leech);
  else on_loaded(r.slot);
  LeaveCriticalSection(&g_lock);
}

// The engine's save/load task: mode names the operation, state runs 1 (start)
// -> 2 (the disk) -> 0, and the file number stays in `op` throughout.
//
// The finished reading - a mode with no state - lasts exactly one frame, since
// the task clears the mode the next time it runs, and the tick that polls this
// runs at about that rate: watching for that one reading missed about every
// other save, and left the box on disk a save behind the game's own file. The
// running phase spans the whole 2.3 MB write instead, tens of frames, so the
// operation is latched while it runs and applied the first time it is seen to
// be over - whichever of mode or state was observed to drop first.
void watch_saves() {
  game::SaveOp op;
  if (!game::save_op(&op)) return;

  const bool running = op.mode != 0 && op.state != 0;
  // Still the same operation? Both mode and the file number hold their value
  // for as long as the task is working on it.
  if (g_run.active && !(running && op.mode == g_run.mode && op.slot == g_run.slot)) {
    // `result` is zeroed by a request and holds the outcome until the next one,
    // so it still reads true one operation later. If the next request has
    // already zeroed it - two operations inside one poll interval, which is how
    // a typewriter save chases the file with the system data - it reads as
    // success, which is the answer that keeps the box with a save that landed.
    finish_run(op.result);
  }
  // Two saves are not the box's to take. The main game's ending marks its
  // cleared-game save with a save point of its own (game::SaveOp::is_clear):
  // that file keeps no box. Leech Hunter's result screen writes save point 0
  // and saves as a file copy (cRoomPhaseOmakeResult, exe+0x20AFF0) - the only
  // save Leech Hunter has, so any save made while it runs is that one - and it
  // leaves the file's box exactly as it was. Both are decided as the operation
  // starts, which is when the game captures the save point.
  if (running && !g_run.active && (op.is_save || op.is_load) && op.slot >= 0) {
    const bool leech = op.is_save && g_mode == kLeech;
    const bool clear = op.is_save && op.is_clear && !leech;
    g_run = {true, op.mode, op.slot, op.is_save, op.is_copy, clear, leech};
    if (op.is_save)
      logf("storage: the game started to save save file %d (save point %d)%s", op.slot + 1, op.point,
           leech ? " - Leech Hunter's save" : clear ? " - a cleared game" : "");
    else
      logf("storage: the game started to load save file %d", op.slot + 1);
  }
}

// The box belongs to the game that is running: a game a load brought up keeps
// that file's items, any other one is a new game and starts empty. Neither a
// clock nor the game's own counters can tell them apart - a new game can begin
// seconds after a load - so the engine's load event is paired with the game it
// brings up.
//
// Two things say a game has started, and the first of them to speak wins:
//
//  - the mode the game hands Steam (game::mode). It is set at the moment the
//    menu commits - the same statement that applies the save file or lays out
//    a new game - so it is both the earliest and the most exact signal, and it
//    is the only one that notices a switch between the three modes;
//  - the player units appearing. That is up to a minute and a half later (the
//    intro, the room), and it is all there is when the presence field could
//    not be found, or when a game is reloaded without the mode ever changing -
//    from the death screen, say.
void watch_session(bool in_game) {
  const DWORD now = GetTickCount();

  // 1. The mode. kUnknown is "this tick says nothing" (the object is not up
  //    yet, or the value is one this build does not use) and must not be read
  //    as "no game is running", or a single unlucky tick would empty the box.
  const game::Mode m = game::mode();
  if (m != game::Mode::kUnknown) {
    const int mode = mode_index(m);
    if (mode != g_mode) {
      g_mode = mode;
      if (mode >= 0) {
        begin_game(mode, "a game started");
        g_started = true;  // the units are about to arrive; they are the same start
      } else {
        g_started = false;
        drop_pending_load("back in the menus");
      }
    }
  }

  // 2. The player units.
  if (in_game == g_in_game) {
    if (g_load_pending && now - g_load_at > kLoadWaitMs) drop_pending_load("nothing came of it");
    return;
  }
  g_in_game = in_game;
  if (!in_game) {
    g_out_since = now;
    return;
  }
  // The units can vanish for a moment inside a running game (a cutscene, a
  // chapter change): only a gap long enough to have been the menus counts as
  // having left the game.
  if (g_out_since && now - g_out_since < kUnitGapMs) {
    logf("storage: the game came back after %lu ms - not treated as a new session", now - g_out_since);
    return;
  }
  if (g_started) {
    g_started = false;  // the mode change already bound the box for this one
    logf("storage: the game the mode named is up - %d item(s) in the box", g_box.size());
    return;
  }
  // A game whose mode was never left is still the same game: the units went
  // away for something long (a cutscene the 3 s above did not cover), or the
  // player reloaded without passing the title screen - from the death screen,
  // say - and only the second of those has a file waiting to go back in the
  // box. Without a mode to go on there is no telling the two apart, and a gap
  // this long has to count as a new game, which is what the mod did before.
  if (game::mode_ready() && g_mode >= 0 && !g_load_pending) {
    logf("storage: the units came back after %lu ms and %s is still the game running - the box is left alone",
         now - g_out_since, mode_label(g_mode));
    return;
  }
  begin_game(g_mode, "the game came up");
}

}  // namespace

void init(const char* dir) {
  if (g_ready) return;
  InitializeCriticalSection(&g_lock);
  std::snprintf(g_path, sizeof(g_path), "%sRE0CabbyCodes.storage.ini", dir);
  std::snprintf(g_tmp_path, sizeof(g_tmp_path), "%sRE0CabbyCodes.storage.tmp", dir);
  read_file();
  g_ready = true;
}

const char* mode_label(int mode) {
  switch (mode) {
    case 0: return "the main game";
    case 1: return "Wesker mode";
    case 2: return "Leech Hunter";
    default: break;
  }
  return "the game";
}

View view() {
  View v;
  if (!g_ready) return v;
  EnterCriticalSection(&g_lock);
  v.items = g_box.items;
  v.slot = g_bound_slot;
  v.mode = g_mode >= 0 ? g_mode : g_bound_mode;
  v.file_ok = g_file_ok;
  v.clear_ok = game::save_point_known();
  LeaveCriticalSection(&g_lock);
  return v;
}

// Deleting a file from the load list is the game's own "no data", so the file
// keeps no box - the same as the cleared-game save, which leaves the file to
// start over from. A copy takes the box of the file it was copied from: the box
// is part of the save. The box in memory belongs to the game that was running
// last, and the load list is the title screen, so it is left alone; the next
// game that starts sets it from these records as it always does.
namespace {

// The change made the records' own; the lock is held.
Followed follow(const PendingChange& c) {
  Followed f;
  f.had = g_records[c.to].size();
  if (c.copy) g_records[c.to] = g_records[c.from];
  else g_records[c.to].clear();
  f.now = g_records[c.to].size();
  return f;
}

void say_followed(const PendingChange& c, const Followed& f, const char* how) {
  if (c.copy)
    note("save file %d was copied to save file %d%s: file %d's box now holds file %d's %d item(s)%s", c.from + 1,
         c.to + 1, how, c.to + 1, c.from + 1, f.now, f.had ? " (the ones it had are gone with its old save)" : "");
  else if (f.had)
    note("save file %d was deleted%s: its box (%d item(s)) is emptied with it", c.to + 1, how, f.had);
  else
    note("save file %d was deleted%s - it kept no box", c.to + 1, how);
}

// A change of the last run's, settled from the game's own save files: a
// deleted file reads as empty and a copied one as the same save as the file it
// came from exactly when the game had written the change before it closed.
// Only called while the save files are data0.bin's (see settle_file_change).
void settle_left_over() {
  if (!g_ready || !g_pending.active || !g_pending.left_over) return;
  const PendingChange c = g_pending;
  game::SaveFile files[kSlots];
  const int n = game::save_files(files, kSlots);
  if (n <= c.to || (c.copy && n <= c.from)) return;  // not readable yet: the next chance will do
  bool landed = false;
  if (c.copy) {
    const int same = game::same_save_file(c.from, c.to);
    if (same < 0) return;
    landed = same == 1;
  } else {
    landed = !files[c.to].used;
  }
  char what[80];
  change_name(c, what, sizeof(what));
  EnterCriticalSection(&g_lock);
  g_pending = PendingChange{};
  Followed f;
  if (landed) f = follow(c);
  write_file();
  LeaveCriticalSection(&g_lock);
  if (landed) say_followed(c, f, " before the game last closed");
  else note("%s never reached data0.bin before the game last closed - the boxes stay as they were", what);
}

}  // namespace

void file_change_begin(bool copy, int from, int to) {
  if (!g_ready || to < 0 || to >= kSlots || (copy && (from < 0 || from >= kSlots || from == to))) return;
  char what[80];
  EnterCriticalSection(&g_lock);
  if (g_pending.active)
    logf("storage: %s was never settled - it is replaced", change_name(g_pending, what, sizeof(what)));
  g_pending = {true, copy, copy ? from : -1, to, false};
  write_file();
  LeaveCriticalSection(&g_lock);
  logf("storage: %s is noted as pending until the game has written data0.bin",
       change_name(PendingChange{true, copy, copy ? from : -1, to, false}, what, sizeof(what)));
}

Followed file_change_end(bool succeeded) {
  Followed f;
  if (!g_ready) return f;
  EnterCriticalSection(&g_lock);
  if (!g_pending.active || g_pending.left_over) {
    LeaveCriticalSection(&g_lock);
    return f;
  }
  const PendingChange c = g_pending;
  g_pending = PendingChange{};
  if (succeeded) f = follow(c);
  write_file();
  f.saved = g_file_ok;
  LeaveCriticalSection(&g_lock);
  char what[80];
  if (succeeded) say_followed(c, f, "");
  else note("%s did not reach data0.bin - the boxes stay as they were", change_name(c, what, sizeof(what)));
  return f;
}

void settle_file_change() { settle_left_over(); }

int items_kept(int slot) {
  if (!g_ready || slot < 0 || slot >= kSlots) return -1;
  EnterCriticalSection(&g_lock);
  const int n = g_records[slot].size();
  LeaveCriticalSection(&g_lock);
  return n;
}

void request_store(int bag, int slot) { push({Request::kStore, bag, slot, 0, 0}); }
void request_take(int index, int id, int count, int bag) { push({Request::kTake, index, bag, id, count}); }
void request_drop(int index, int id, int count) { push({Request::kDrop, index, 0, id, count}); }

void tick(bool in_game) {
  if (!g_ready) return;
  watch_saves();
  watch_session(in_game);

  Request reqs[32];
  const int n = drain(reqs, 32);
  if (!n) return;
  // Hold the lock across the whole batch so the panel never sees the box
  // half-way through a move.
  EnterCriticalSection(&g_lock);
  for (int i = 0; i < n; ++i) {
    const Request& r = reqs[i];
    switch (r.type) {
      case Request::kDrop:
        do_drop(r.a, r.c, r.d);
        break;
      case Request::kStore:
        if (in_game) do_store(r.a, r.b);
        else note("the inventory is only there while a game is running");
        break;
      case Request::kTake:
        if (in_game) do_take(r.a, r.c, r.d, r.b);
        else note("the inventory is only there while a game is running");
        break;
    }
  }
  LeaveCriticalSection(&g_lock);
}

}  // namespace re0cc::storage
