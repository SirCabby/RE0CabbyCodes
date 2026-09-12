#pragma once

#include <cstdint>
#include <vector>

#include "mem.h"

// The game side: what the mod knows about re0hd.exe at runtime. Everything is
// found by name (the event-script command table, MT Framework DTI) or by
// signature, verified against the live bytes, and re-validated on every use.
// Nothing here is an absolute address; the community leads that carry RVAs are
// only read back for the log, never acted on.
namespace re0cc::game {

// --- discovery (mod thread) ------------------------------------------------------
bool decrypted(int* prologues, int* pads);  // the SteamStub gate: is .text plaintext yet?
bool discover();                            // after the gate; sets ready() when the core anchors resolved
bool ready();
const char* status_text();
const char* build_string();
bool build_supported();
bool dump_image(const char* dir);           // write the decrypted image beside the DLL
void note_iat_slot(uintptr_t slot, uintptr_t original);  // so the dump carries a pristine IAT
void background_tick();                     // mod thread, every 2 s: retry singletons that were not found

// --- main-thread state -------------------------------------------------------------
bool in_game();             // the two player units exist (never true on the title screen)
bool pause_menu_showing();  // the calibrated pause flag; false until one is known
void trace_tick();          // throttled diagnostics (Trace / DumpObject), main thread only

// --- which game is running (sGamePresence) ------------------------------------------
// The game tells Steam what the player is doing through sGamePresence, and the
// places a game can start are the only ones that put a game value in it: 1 when
// the main game starts (new or continued), 2 for Wesker mode, 3 for Leech
// Hunter. The title screen's own menus set 4..8. The value is kept as a request
// and a published copy (see game.cpp), and mode() reads the pair the way the
// game's own update does, so it changes at the moment the menu commits.
enum class Mode { kUnknown = 0, kMenu, kMainGame, kWesker, kLeechHunter };
bool mode_ready();            // the fields were located in the code (the object can still be absent)
Mode mode();                  // kUnknown when it could not be read this tick
const char* mode_name(Mode m);

// --- the room's phase machine (sRoomControl) -----------------------------------------
// Every new game - a fresh one, or one started from a save file (Once Again) -
// opens by pushing the Opening phase (Wesker mode's fresh start pushes
// WeskerTitle, which pushes Opening when it ends); a continue pushes no phase
// at all. The title loads the chosen file before either, so asked on the tick
// a game starts, this is what tells them apart.
bool phase_ready();           // the phase machine and the two phase ids were derived
bool new_game_starting();     // the phase just pushed or entered is Opening or WeskerTitle

// --- characters (uPlayerRebecca / uPlayerBilly from sPlayer) ------------------------
struct Character {
  uintptr_t obj = 0;
  const char* cls = "";
  int type = -1;      // 3 Rebecca, 5 Billy
  int hp = -1;
  int poison = -1;
  bool active = false;
};
int characters(Character out[2]);  // validated units only (0..2)
bool set_hp(const Character& c, int hp);
bool set_poison(const Character& c, int v);
int max_hp(const Character& c);

// --- units and enemies (sGameChara's unit table) -----------------------------------------
// The table holds every live character unit: the two players, the enemies, and
// whatever else a scene puts there. An enemy is a class under uEnemyBase or one
// named uEnemy* (uEnemy3bRebecca is an enemy that inherits from uPlayerBase).
// `pool` is the leech-man's second health pool (uEnemy43, see game.cpp): -1 for
// every other unit, and for that one too until the offset has been derived.
struct Unit {
  uintptr_t obj = 0;
  const char* cls = "";
  int hp = -1;
  int pool = -1;
  int index = -1;   // slot in the table
  bool enemy = false;
};
struct Enemy {
  uintptr_t obj = 0;
  const char* cls = "";
  int hp = -1;
  int pool = -1;
};
int units(Unit* out, int max);     // every live unit, enemy or not (Trace lists these)
int enemies(Enemy* out, int max);  // live enemy units (validated by class), 0 when the table is unknown
bool set_enemy_hp(const Enemy& e, int hp);
bool set_enemy_pool(const Enemy& e, int v);  // no-op unless e.pool >= 0
bool enemy_table_known();

// --- inventory bags (sItem) ----------------------------------------------------------
struct Bag {
  uintptr_t base = 0;
  int id[6] = {};
  int count[6] = {};
  int personal_id = 0;
  int personal_count = 0;
  int equipped = -1;
};
bool bags(Bag out[2]);                                  // false until the layout validated
bool write_slot(int bag, int slot, int id, int count);  // slot 0..5, 6 = personal item
bool write_equipped(int bag, int index);

// --- how many of one item fit in one slot ------------------------------------------------
// The game answers that from a single routine, and the mod reads that routine's
// threshold, its two exceptions and its table out of the code rather than
// carrying a list of its own: ids below the threshold each have their own
// number (a weapon's loaded ammo, everything else's stack size), ids above it
// are key items that do not stack - except the two leech charms, which Leech
// Hunter stacks ten to a slot. items.h is only the fallback for when the
// routine is not found.
bool item_max_known();
int item_max(int id);  // 0 for "no item"; never more than items::kMaxCount

// --- which items stack -----------------------------------------------------------------
// A pickup only tops up a slot that already holds the same item, and only for
// the few items the game stacks: the ammo boxes, ink ribbons, Molotov
// cocktails, the empty bottle and the gas tank, the two leech charms. Every
// other item takes a slot of its own for each one, whatever its count says (a
// weapon's is its magazine). Read out of the pickup code; items.h stands in
// when it is not found.
bool item_stacks_known();
bool item_stacks(int id);  // a slot of this item takes more of it, up to item_max(id)

// --- game status: save count and play time -------------------------------------------
bool status_ready();
const char* status_class();
int save_count();
bool set_save_count(int v);
float playtime_raw();
float playtime_rate();  // raw units per second (0 until measured in play)
bool set_playtime_raw(float v);

// --- countdown timer (the scripted ones: the train brakes, the self-destruct) -------------
// A countdown lives in sEventScript at +0x2C as a float in the same 1/30 s
// units as the play-time clock; -1 means no timer is running. The HUD element
// (uGUITimer) reads that field and the per-frame steppers subtract
// 30/fps * timescale from it, exactly like sGameInfo::updateTime does to the clock.
bool countdown_ready();   // sEventScript was found and is readable
bool countdown_active();  // a timer is running right now
float countdown_raw();    // raw units, or -1 when no timer is running
float countdown_rate();   // raw units per second (30, the engine's own step)
bool set_countdown_raw(float v);

// --- save files (the engine's save/load state machine) ----------------------------------
// Saving and loading go through a cSaveManager object hanging off the
// sSaveManager singleton: a request sets {mode, state = 1} on it and the
// per-frame task steps state 1 -> 2 (the disk) -> 0, leaving the outcome in
// `result`. The file the operation is for stays in `op` throughout, so polling
// those four fields is enough to see what the game saved or loaded, and when.
struct SaveOp {
  bool valid = false;
  int mode = 0;    // 0 = nothing running; the rest are the engine's own numbering
  int state = 0;   // 0 idle, 1 starting, 2 waiting for the disk, 3 applying
  int result = 0;  // 0 = the operation succeeded
  int slot = -1;   // the save file, or -1 for an operation that is not about one file
  bool is_save = false;
  bool is_load = false;
  bool is_copy = false;   // a save that copies another file over this one
  int point = -1;         // the save point this save would record (-1 = not known)
  bool is_clear = false;  // that save point is the one the ending sets: a cleared-game save
  int op = -1;            // the raw file/operation number (`slot` is only set for a file)
  bool is_system = false; // the system-data save, which writes data0.bin whole (see below)
};
bool save_watch_ready();  // the field layout and the live object were both found
int save_slot_count();    // save files the build has (0 until known)
bool save_point_known();  // the save-point field and the ending's value were both derived
bool save_op(SaveOp* out);

// --- the save files themselves ----------------------------------------------------------
// cSaveManager holds every save file twice, in two arrays of the same records:
// the first is what the game plays from and what its file list shows, the
// second is what data0.bin is read into and written from. A save captures the
// game into the first copy and copies it over the second before the disk
// write; a load reads the whole file into the second copy and copies the one
// file it wants across. A file is in use when the save-point byte of its record
// is set: the file list's own test - the one that prints NO DATA and refuses a
// load - reads that byte and nothing else.
struct SaveFile {
  bool used = false;       // the game lists it
  int point = 0;           // the save point it was made at (0 = not in use)
  bool cleared = false;    // the ending's save point: a cleared game
  bool wesker = false;     // made in Wesker mode (false when the flag was not found)
  int saves = -1;          // the save count it records (-1 = not known)
  float seconds = -1.0f;   // its play time (-1 = not known)
};
bool save_files_ready();                         // the record layout and the system-data save were derived
int save_files(SaveFile* out, int max);          // main thread: every file, or 0 when they cannot be read
bool save_idle();                                // nothing is pending or running in the save state machine
// Changes to the records: main thread, and only while save_idle() - the disk
// thread reads the second copy while a save runs. None of them reaches the
// disk by itself: request_system_save() asks the game for its own system-data
// save (the one a typewriter save chases the file with), which writes data0.bin
// whole from the second copy.
bool backup_save_file(int slot, std::vector<uint8_t>* out);  // both copies of one record
bool restore_save_file(int slot, const std::vector<uint8_t>& in);
bool clear_save_file(int slot);         // its save-point byte to 0, in both copies: the game's own "no data"
bool copy_save_file(int from, int to);  // one record over another, in both copies
bool request_system_save();
int same_save_file(int a, int b);       // 1 when the two records are byte for byte the same save, 0 not, -1 unknown

// --- the title screen's load list ---------------------------------------------------------
// The file list is a uGUISave - the screen a typewriter opens to save - that
// the title screen keeps for loading, reached through the title area (sArea's
// area stack -> aTitle -> uGUITitle -> uGUISave). It runs on a mode dword:
// open() starts it, it waits in one mode for the player to pick a file, and one
// state of the mode before that rebuilds every row from the records and puts
// the cursor on sSaveManager's current file.
struct LoadList {
  bool found = false;       // the title's list object exists
  bool up = false;          // it is on screen, waiting for the player to pick a file
  bool rebuilding = false;  // it is about to rebuild its rows (refresh_load_list)
  int cursor = -1;          // the file the game's cursor is on
  int mode = -1, kind = -1; // raw, for the log
};
bool load_list_ready();         // its fields were derived
bool load_list(LoadList* out);  // main thread
int save_current_file();        // sSaveManager's current file (-1 = not known)
bool set_save_current_file(int slot);
bool refresh_load_list();       // the rows rebuilt from the records, the cursor on save_current_file()

// --- code sites --------------------------------------------------------------------------
enum Site {
  kSitePlaytimeStore = 0,
  kSiteSaveCountInc,
  kSiteSaveCountBack,  // the Save phase taking the count back when a save is cancelled
  kSiteInkTakeA,  // the two places a save takes its ink ribbon
  kSiteInkTakeB,
  kSiteInkCheck,  // the typewriter's "no ribbon" branch
  kSiteCountdownStepA,
  kSiteCountdownStepB,
  kSiteCount
};
mem::Patch* site(Site s);  // a prepared patch, or null when the site was not found (or not unique)
const char* site_name(Site s);

}  // namespace re0cc::game
