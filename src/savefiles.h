#pragma once

#include "game.h"

// The save files, managed from the title screen's load list: while the list is
// on screen the panel shows every file, and can delete one or copy one over
// another. Nothing new is invented for either: an empty file is the game's own
// "no data" (a record whose save point is 0), a copy is the record itself, and
// data0.bin is written by the game's own system-data save, which writes the
// whole file. A change is made to both of the game's copies of the record, and
// put back if the write fails, so the list, the next load and the disk agree.
//
// The mod's storage box follows the file: a deleted file's box is emptied and
// a copy takes the box of the file it came from - once the disk write has
// succeeded, never before.
namespace re0cc::savefiles {

constexpr int kMaxFiles = 20;  // the file list's length in this build (the game's own count is derived)

struct File {
  bool known = false;     // the record could be read
  bool used = false;      // the game lists it (it would print NO DATA otherwise)
  bool cleared = false;   // the ending's save: Once Again and Leech Hunter start from it
  bool wesker = false;    // Wesker mode's
  int saves = -1;         // the save count it records (-1 = not known)
  float seconds = -1.0f;  // its play time (-1 = not known)
  int box = -1;           // items in the mod's storage box for it (-1 = not known)
};
// Two readings of a file are the same save: what a request checks before it acts.
bool same_save(const File& a, const File& b);

// A copy of everything the panel draws, taken under the lock.
struct View {
  bool up = false;          // the load list is on screen, waiting for the player
  bool ready = false;       // the save files can be changed in this build
  int cursor = -1;          // the file the game's cursor is on
  int files = 0;            // how many the game has
  File file[kMaxFiles];
  bool busy = false;        // a change is being written to data0.bin
  char note[200] = {};      // what the last change came to
  bool note_error = false;
};

void init();     // from DllMain: the lock
View view();     // any thread
bool showing();  // main thread: the load list is up, so the panel belongs on screen

// Requests from the panel, carried out on the next main-thread tick. What the
// panel saw of the files goes with them: the list can move on between the
// click and the tick, and changing a file that is not the one clicked is
// worse than not changing anything.
void request_delete(int slot, const File& seen);
void request_copy(int from, int to, const File& from_seen, const File& to_seen);

void tick();  // main thread, every tick

}  // namespace re0cc::savefiles
