#include "savefiles.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include "config.h"
#include "log.h"
#include "storage.h"

namespace re0cc::savefiles {

bool same_save(const File& a, const File& b) {
  return a.known == b.known && a.used == b.used && a.cleared == b.cleared && a.wesker == b.wesker &&
         a.saves == b.saves && a.seconds == b.seconds;
}

namespace {

CRITICAL_SECTION g_lock;  // the view, the note and the request queue
bool g_ready = false;
View g_view;              // what the panel was last handed
char g_note[200] = {};
bool g_note_error = false;
bool g_showing = false;   // main thread only

struct Request {
  enum Type { kDelete, kCopy } type;
  int from, to;
  File from_seen, to_seen;
};
Request g_req[8];
int g_req_n = 0;

// The change being written. There is only ever one: the game has one save
// state machine, and a second change would have to wait for it anyway.
struct Change {
  bool active = false;
  Request::Type type = Request::kDelete;
  int from = -1, to = -1;
  std::vector<uint8_t> before;  // both copies of the record it overwrote, to put back if the write fails
  bool seen_running = false;
  DWORD started = 0;
};
Change g_change;
Change g_undo;               // a failed write, waiting for the state machine to be idle to be put back
bool g_undo_pending = false;
char g_pending_note[200] = {};  // finish_change's note, put together before it is posted
constexpr int kNoCursor = -1000;
int g_cursor_back = kNoCursor;  // sSaveManager's current file, to put back once a rebuild has run
game::LoadList g_last;          // for the log

void note(bool error, const char* fmt, ...) {
  char text[200];
  va_list args;
  va_start(args, fmt);
  vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  logf("save files: %s", text);
  if (!g_ready) return;
  EnterCriticalSection(&g_lock);
  std::snprintf(g_note, sizeof(g_note), "%s", text);
  g_note_error = error;
  LeaveCriticalSection(&g_lock);
}

// "file 3 (1:02:03, 7 saves)", for the log and the notes.
const char* describe(int slot, const File& f, char* buf, size_t n) {
  if (!f.used) {
    std::snprintf(buf, n, "file %d (empty)", slot + 1);
    return buf;
  }
  char t[24] = "?";
  if (f.seconds >= 0.0f) {
    const int s = static_cast<int>(f.seconds);
    std::snprintf(t, sizeof(t), "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
  }
  std::snprintf(buf, n, "file %d (%s, %d save%s%s%s)", slot + 1, t, f.saves, f.saves == 1 ? "" : "s",
                f.cleared ? ", cleared" : "", f.wesker ? ", Wesker mode" : "");
  return buf;
}

int read_files(File* out) {
  game::SaveFile raw[kMaxFiles];
  const int n = game::save_files(raw, kMaxFiles);
  for (int i = 0; i < n; ++i) {
    File& f = out[i];
    f = File{};
    f.known = true;
    f.used = raw[i].used;
    f.cleared = raw[i].cleared;
    f.wesker = raw[i].wesker;
    f.saves = raw[i].saves;
    f.seconds = raw[i].seconds;
    f.box = storage::items_kept(i);
  }
  return n;
}

void push(const Request& r) {
  if (!g_ready) return;
  EnterCriticalSection(&g_lock);
  if (g_req_n < 8) g_req[g_req_n++] = r;
  LeaveCriticalSection(&g_lock);
}

int drain(Request* out, int max) {
  if (!g_ready) return 0;
  EnterCriticalSection(&g_lock);
  const int n = g_req_n < max ? g_req_n : max;
  for (int i = 0; i < n; ++i) out[i] = g_req[i];
  g_req_n = 0;
  LeaveCriticalSection(&g_lock);
  return n;
}

// After a change is on disk the game's list still shows the rows it built
// before: the rows are text it sets while building or scrolling them. It is
// rebuilt through its own code, with the cursor on the file that changed;
// sSaveManager's current file is what that code puts the cursor on, and it goes
// back to what it was as soon as the rebuild has run.
void rebuild_list(int slot) {
  const int was = game::save_current_file();
  if (was == -1 || !game::set_save_current_file(slot)) return;
  if (game::refresh_load_list()) {
    g_cursor_back = was;
  } else {
    game::set_save_current_file(was);
    logf("save files: the load list could not be rebuilt - it shows the change once its cursor moves");
  }
}

void finish_change(const game::LoadList& list) {
  game::SaveOp op;
  if (!game::save_op(&op)) return;  // no reading this tick
  // Still ours? The request is taken on the very next frame, and the task keeps
  // mode and op for as long as it works on it.
  if (op.is_system && op.is_save && !op.is_copy && op.state != 0) {
    if (!g_change.seen_running) {
      g_change.seen_running = true;
      logf("save files: the game is writing data0.bin");
    }
    return;
  }
  Change ch = std::move(g_change);
  g_change = Change{};
  // `result` holds the outcome until the next request zeroes it, so it is
  // still this one's unless something else has already asked - and then the
  // answer that reads is success, which is also what a write that landed says.
  const int result = op.result;
  if (result != 0) {
    storage::file_change_end(false);
    g_undo = std::move(ch);
    g_undo_pending = true;
    note(true, "The game could not write its save file (error %d) - nothing was changed, item storage included.",
         result);
    return;
  }
  // The boxes follow the files, now that the files are on disk.
  const storage::Followed f = storage::file_change_end(true);
  char box[120] = "";
  if (ch.type == Request::kDelete) {
    if (f.had) std::snprintf(box, sizeof(box), ", and its item storage box (%d item%s) with it", f.had, f.had == 1 ? "" : "s");
    std::snprintf(g_pending_note, sizeof(g_pending_note), "Save file %d was deleted%s.", ch.to + 1, box);
  } else {
    if (f.now) std::snprintf(box, sizeof(box), ", item storage box included (%d item%s)", f.now, f.now == 1 ? "" : "s");
    else if (f.had) std::snprintf(box, sizeof(box), " - file %d keeps no item storage, so file %d's box is empty now", ch.from + 1, ch.to + 1);
    std::snprintf(g_pending_note, sizeof(g_pending_note), "Save file %d was copied to save file %d%s.", ch.from + 1,
                  ch.to + 1, box);
  }
  if (f.saved) note(false, "%s", g_pending_note);
  else note(true, "%s But RE0CabbyCodes.storage.ini could not be written - see the log.", g_pending_note);
  logf("save files: data0.bin written in %lu ms", GetTickCount() - ch.started);
  if (list.up) rebuild_list(ch.to);
}

void undo_failed() {
  if (!game::save_idle()) return;
  if (game::restore_save_file(g_undo.to, g_undo.before))
    logf("save files: save file %d is back as it was in memory, as it still is on disk", g_undo.to + 1);
  else
    logf("ERROR: save files: save file %d could not be put back in memory - reload it from the title to be sure",
         g_undo.to + 1);
  g_undo = Change{};
  g_undo_pending = false;
}

void apply(const Request& r, const game::LoadList& list) {
  if (g_change.active || g_undo_pending) {
    note(true, "A change is still being written - try again in a moment.");
    return;
  }
  if (!list.up) {
    note(true, "The load list is not on screen any more - nothing was changed.");
    return;
  }
  if (!game::save_files_ready()) {
    note(true, "The save files cannot be changed in this build - see the log.");
    return;
  }
  if (!game::save_idle()) {
    note(true, "The game is busy with its save file - try again in a moment.");
    return;
  }
  File now[kMaxFiles];
  const int n = read_files(now);
  if (r.to < 0 || r.to >= n || (r.type == Request::kCopy && (r.from < 0 || r.from >= n || r.from == r.to))) {
    note(true, "That is not a save file this game has - nothing was changed.");
    return;
  }
  if (!same_save(now[r.to], r.to_seen) || (r.type == Request::kCopy && !same_save(now[r.from], r.from_seen))) {
    note(true, "The save files changed since that was clicked - nothing was changed.");
    return;
  }
  if (r.type == Request::kDelete && !now[r.to].used) {
    note(true, "Save file %d is already empty.", r.to + 1);
    return;
  }
  if (r.type == Request::kCopy && !now[r.from].used) {
    note(true, "Save file %d has no save to copy.", r.from + 1);
    return;
  }
  Change ch;
  ch.type = r.type;
  ch.from = r.from;
  ch.to = r.to;
  if (!game::backup_save_file(r.to, &ch.before)) {
    note(true, "Save file %d could not be read - nothing was changed.", r.to + 1);
    return;
  }
  const bool copy = r.type == Request::kCopy;
  if (!(copy ? game::copy_save_file(r.from, r.to) : game::clear_save_file(r.to))) {
    game::restore_save_file(r.to, ch.before);
    note(true, "The save file could not be changed in memory - nothing was changed.");
    return;
  }
  // The boxes follow what reaches the disk, so the change is noted in the
  // storage's own file before the game is asked to write it: should the game
  // close before it answers, the next run settles the note from the save files.
  storage::file_change_begin(copy, r.from, r.to);
  if (!game::request_system_save()) {
    game::restore_save_file(r.to, ch.before);
    storage::file_change_end(false);
    note(true, "The game did not take the save request - nothing was changed.");
    return;
  }
  char a[96], b[96];
  if (r.type == Request::kDelete)
    logf("save files: deleting %s - its save point is cleared in both copies, and the game writes data0.bin",
         describe(r.to, now[r.to], a, sizeof(a)));
  else
    logf("save files: copying %s over %s - the record goes across in both copies, and the game writes data0.bin",
         describe(r.from, now[r.from], a, sizeof(a)), describe(r.to, now[r.to], b, sizeof(b)));
  ch.active = true;
  ch.started = GetTickCount();
  g_change = std::move(ch);
  EnterCriticalSection(&g_lock);
  g_note[0] = '\0';
  LeaveCriticalSection(&g_lock);
}

}  // namespace

void init() {
  if (g_ready) return;
  InitializeCriticalSection(&g_lock);
  g_ready = true;
}

View view() {
  View v;
  if (!g_ready) return v;
  EnterCriticalSection(&g_lock);
  v = g_view;
  std::snprintf(v.note, sizeof(v.note), "%s", g_note);
  v.note_error = g_note_error;
  LeaveCriticalSection(&g_lock);
  return v;
}

bool showing() { return g_showing; }

void request_delete(int slot, const File& seen) { push({Request::kDelete, -1, slot, File{}, seen}); }
void request_copy(int from, int to, const File& from_seen, const File& to_seen) {
  push({Request::kCopy, from, to, from_seen, to_seen});
}

void tick() {
  if (!g_ready) return;
  game::LoadList list;
  game::load_list(&list);
  if (list.up != g_last.up || (config::get().trace && (list.found != g_last.found || list.mode != g_last.mode ||
                                                       list.kind != g_last.kind || list.cursor != g_last.cursor))) {
    if (list.up != g_last.up) logf("save files: the load list %s", list.up ? "is up" : "is no longer up");
    if (config::get().trace)
      logf("trace: load list found=%d mode=%d kind=%d cursor=%d up=%d", list.found, list.mode, list.kind, list.cursor,
           list.up);
  }
  g_last = list;

  // A rebuild runs on the list's next frame; once it has, the current file goes back.
  if (g_cursor_back != kNoCursor && !list.rebuilding) {
    game::set_save_current_file(g_cursor_back);
    g_cursor_back = kNoCursor;
  }
  if (g_change.active) finish_change(list);
  else if (g_undo_pending) undo_failed();
  // On the load list the save files are data0.bin's: a change the last run
  // never saw written is settled from them, before anything new is asked for.
  if (list.up && !g_change.active && !g_undo_pending) storage::settle_file_change();

  Request reqs[8];
  const int n = drain(reqs, 8);
  for (int i = 0; i < n; ++i) apply(reqs[i], list);

  // The panel is only up with the list; the records are only read for it.
  g_showing = list.up && game::load_list_ready();
  View v;
  v.up = list.up;
  v.ready = game::save_files_ready();
  v.cursor = list.cursor;
  v.busy = g_change.active || g_undo_pending;
  if (list.up) v.files = read_files(v.file);
  EnterCriticalSection(&g_lock);
  g_view = v;
  LeaveCriticalSection(&g_lock);
}

}  // namespace re0cc::savefiles
