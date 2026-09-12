#pragma once

#include <windows.h>

#include <vector>

#include "game.h"

// The item storage box: an overflow container the mod keeps beside the game's
// six-slot inventories. Items are pushed in from a bag slot and taken back out
// into a free one, or onto a stack of the same item (see Fit); the box itself
// lives in the mod, not in game memory, so nothing here can corrupt a save.
//
// The box is one list, kept in bank order: every entry is filed under its
// item's bank (key items, weapons, ammo, herbs and sprays, the rest - see
// items::bank) and each bank is sorted by name, the fullest stack of an item
// first. The panel shows one bank at a time. The banks are a way of listing
// the box, not five boxes: the ini keeps one line per save file.
//
// An item the game stacks (game::item_stacks) is merged as it comes in: the
// box keeps it the way inventory slots would, in stacks of one slot's worth
// (game::item_max) with at most one short one, so storing a few rounds tops
// up the stack already there rather than adding an entry, and taking it back
// tops up the stack the character already carries. Everything else - a weapon,
// whose count is its magazine, a herb, a key item - keeps an entry of its own,
// as it keeps a slot of its own in the game.
//
// It follows the game's save files. When the game finishes writing a file the
// box is written to that file's record in RE0CabbyCodes.storage.ini beside the
// DLL; when the game finishes loading a file the record is read back into the
// box. Both events come from the engine's own save/load state machine
// (game::save_op), so the storage stays in step with the game's own saves
// without touching the save file itself.
//
// Every save file keeps exactly one box, and Wesker mode writes the same twenty
// files the main game does. Leech Hunter's box lives only in memory: it starts
// empty rather than with the file's, and its one save (from the result screen)
// neither writes it nor touches the file's box. The main game's cleared-game
// save is the other exception: it is the file you start over from, so it is
// left with an empty box.
//
// The save file manager on the title screen's load list (savefiles.cpp)
// deletes and copies whole save files, and the boxes follow: a deleted file
// keeps no box, a copy takes the box of the file it was copied from. The game
// writes data0.bin a moment after the change is asked for, so the change is
// noted in the ini as pending first and made the records' own when the game
// says the write succeeded (or dropped when it failed). A note that outlives
// its run - the game closed while it wrote - is settled from the game's own
// save files the next time they are known to be what data0.bin holds.
//
// Nothing empties or restores a box by hand. What a box holds follows from
// the saves and the rules above, and the panel only moves single items in and
// out of it (or throws one away); a box that comes out wrong is a rule to fix.
namespace re0cc::storage {

// The box is the mod's own memory and a line of text on disk, so it has no
// natural size; the cap only keeps a corrupted file from being read forever.
constexpr int kMaxItems = 999;
constexpr int kSlots = 20;  // save files the game's file list has

const char* mode_label(int mode);  // "the main game" / "Wesker mode" / "Leech Hunter"

struct Entry {
  int id = 0;
  int count = 0;
};

// A copy of everything the panel draws, taken under the lock.
struct View {
  std::vector<Entry> items;  // what the box holds, in bank order
  int slot = -1;             // the save file the box belongs to (-1 = none yet)
  int mode = -1;             // the game mode running (-1 = none yet); 2 = Leech Hunter, which never saves
  bool file_ok = false;      // the ini beside the DLL was read/written cleanly
  bool clear_ok = false;     // a cleared-game save can be recognised
};

void init(const char* dir);  // from DllMain: read the ini beside the DLL
View view();                 // any thread

// How many of one item a stack in the box holds when the box merges it (one
// slot's worth), or 0 for an item that keeps an entry of its own for each one.
int stack_size(int id);

// Where a box entry goes when it is taken into one character's inventory. An
// item the game stacks tops up the stacks of it the character already carries
// first, slot by slot, each to one slot's worth - what Bag::add does with a
// pickup - and whatever is left goes into the first free slot (a pair of them
// for a two-slot item), one slot's worth again. What still does not fit stays
// in the box. The panel asks this for what a take button offers, and the take
// does what it answered.
struct Fit {
  int onto[6] = {};  // how many go onto the stack already in each slot
  int slot = -1;     // the free slot the rest goes into (-1 = none, or none needed)
  int into = 0;      // how many go into it
  int left = 0;      // how many stay in the box
  int topped() const {
    int n = 0;
    for (int v : onto) n += v;
    return n;
  }
  bool ok() const { return slot >= 0 || topped() > 0; }  // the take moves anything at all
};
Fit fit(const game::Bag& bag, int id, int count);

// Requests from the panel; applied on the next main-thread tick. `id` and
// `count` are what the panel saw in that entry: the box can move on between
// the click and the tick - a store re-files it - and acting on the wrong item
// is worse than not acting.
void request_store(int bag, int slot);                     // bag slot 0..5 -> the box
void request_take(int index, int id, int count, int bag);  // box entry -> that bag, as fit() lays it out
void request_drop(int index, int id, int count);           // throw a box entry away

void tick(bool in_game);  // main thread, from cheats::tick

// A save file the load list's panel deletes or copies (savefiles.cpp). Main
// thread, in this order: begin before the game is asked to write data0.bin, end
// with the game's answer.
void file_change_begin(bool copy, int from, int to);  // `from` is ignored for a delete
struct Followed {
  int had = 0;         // items the file's box held before
  int now = 0;         // items it holds now
  bool saved = false;  // the ini was written
};
Followed file_change_end(bool succeeded);  // the box follows only a write that succeeded
// A change the last run noted and never saw the end of, settled from the game's
// own save files. Only while they are data0.bin's - the load list is up, or a
// game is starting - and never while a change of this run is on its way.
void settle_file_change();
int items_kept(int slot);  // items in that file's box (-1 = not a file)

}  // namespace re0cc::storage
