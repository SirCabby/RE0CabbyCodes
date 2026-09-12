#include "overlay.h"

#include <cstdio>
#include <cstring>

#include "cheats.h"
#include "config.h"
#include "dispatch.h"
#include "game.h"
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "input.h"
#include "items.h"
#include "log.h"
#include "mem.h"
#include "savefiles.h"
#include "storage.h"
#include "version.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace re0cc::overlay {
namespace {

using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
GetProcAddressFn g_orig_gpa = nullptr;

HWND g_hwnd = nullptr;
WNDPROC g_orig_wndproc = nullptr;
bool g_context_ready = false;
bool g_visible = false;
bool g_user_hidden = false;  // the toggle key, while the pause menu is up

// The context lock (see overlay.h). Created in install(), from DllMain, before
// either thread that takes it exists; never destroyed, because the render
// thread can be inside a frame while the process tears down.
CRITICAL_SECTION g_imgui_cs;
bool g_imgui_cs_ready = false;

// What the panel is taking this frame, published for the input guard. It is
// asked from whichever thread the game polls DirectInput on, which must not
// block on a frame in progress - nor read ImGui's state without the lock.
volatile LONG g_capture_mouse = 0;
volatile LONG g_capture_keyboard = 0;

// An inventory row being edited (see draw_bag). A dirty row stops following the
// live bag - that is the point of it - so the buffer must not outlive the panel
// it was typed into: reopening has to show the inventory as it is now, not an
// edit that was abandoned before a save, a load or a pickup moved it on.
int g_edit_id[2][7], g_edit_cnt[2][7];
bool g_edit_dirty[2][7];
int g_ask_slot[2] = {-1, -1};  // the row a confirmation is open for, per bag

void forget_bag_edits() {
  std::memset(g_edit_dirty, 0, sizeof(g_edit_dirty));
  g_ask_slot[0] = g_ask_slot[1] = -1;
}

// The save file a Delete or Copy asked about, and what the panel saw of the
// files when it was clicked (see draw_save_files). Like a bag edit it belongs
// to the panel it was clicked in: a list that closed and came back is asked
// again.
enum { kAskNone = 0, kAskDelete, kAskCopy };
int g_file_ask = kAskNone;
int g_file_from = -1, g_file_to = -1;
savefiles::File g_file_from_seen, g_file_to_seen;

void forget_file_dialogs() {
  g_file_ask = kAskNone;
  g_file_from = g_file_to = -1;
}

// "slot 3", or the personal item, for a confirmation to name.
const char* slot_label(int s, char* buf, size_t n) {
  if (s >= 6) return "the personal item slot";
  std::snprintf(buf, n, "slot %d", s + 1);
  return buf;
}

// With Trace=1 the first distinct names resolved through the exe's
// GetProcAddress import are logged: the DRM stub bootstraps through this
// slot, so this shows what it looks at (module verification, Steam, ...).
constexpr int kGpaSeen = 192;
char g_gpa_seen[kGpaSeen][48];
volatile LONG g_gpa_n = 0;

void trace_gpa(HMODULE module, const char* name) {
  if (!config::get().trace) return;
  const LONG n = g_gpa_n;
  for (LONG i = 0; i < n && i < kGpaSeen; ++i)
    if (!std::strncmp(g_gpa_seen[i], name, 47)) return;
  const LONG idx = InterlockedIncrement(&g_gpa_n) - 1;
  if (idx >= kGpaSeen) return;
  std::snprintf(g_gpa_seen[idx], sizeof(g_gpa_seen[idx]), "%s", name);
  char path[MAX_PATH] = "?";
  GetModuleFileNameA(module, path, MAX_PATH);
  const char* leaf = std::strrchr(path, '\\');
  logf("trace: GetProcAddress(%s, %s) thread %lu", leaf ? leaf + 1 : path, name, GetCurrentThreadId());
}

FARPROC WINAPI hk_get_proc_address(HMODULE module, LPCSTR name) {
  FARPROC real = g_orig_gpa(module, name);
  if (!real || !name || !HIWORD(reinterpret_cast<uintptr_t>(name))) return real;
  trace_gpa(module, name);
  if (FARPROC w = dx9::wrap(name, real)) return w;
  if (!config::get().disable_input)
    if (FARPROC w = input::wrap(name, real)) return w;
  return real;
}

LRESULT CALLBACK hk_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  const dispatch::Snapshot s = dispatch::snapshot();
  const bool showable = s.show_panel || config::get().always_show;
  // F-keys above F9 arrive as WM_SYSKEYDOWN; accept both, ignore auto-repeat.
  if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !(lp & (1 << 30)) &&
      static_cast<int>(wp) == config::get().toggle_key && showable) {
    ImGuiLock guard;  // wants_draw reads and clears this on the render thread
    g_user_hidden = !g_user_hidden;
    logf("panel %s by the toggle key", g_user_hidden ? "hidden" : "shown");
    return 0;
  }
  // ImGui sees every message, visible or not: a button release that arrives
  // after the panel is hidden would otherwise never be delivered, leaving it
  // convinced the button is still held (and the window stuck to the pointer).
  if (g_context_ready) {
    bool swallow = false;
    {
      // Everything below queues into, or reads, the one ImGui context, which
      // the render thread is drawing from at the same time.
      ImGuiLock guard;
      if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
      // Swallowing is the visible panel's business, and only for what it is
      // using: clicks on the panel and typing into a field. Escape and Alt (the
      // game's pause key) always go through. The game reads the mouse from
      // DirectInput rather than from here, so this is a formality - input.cpp
      // does the real work.
      if (g_visible) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse && msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) swallow = true;
        if (io.WantTextInput && msg >= WM_KEYFIRST && msg <= WM_KEYLAST && wp != VK_ESCAPE && wp != VK_MENU)
          swallow = true;
      }
    }
    if (swallow) return 0;
  }
  // The game's own window procedure runs outside the lock: it is none of
  // ImGui's business, and it must never be able to hold the render thread up.
  return CallWindowProcA(g_orig_wndproc, hwnd, msg, wp, lp);
}

void cheat_row(cheats::Kind k, const char* label, const char* help, bool available, const char* unavailable_text) {
  bool on = cheats::enabled(k);
  if (!available) ImGui::BeginDisabled();
  if (ImGui::Checkbox(label, &on)) {
    cheats::set_enabled(k, on);
    logf("%s %s (panel)", cheats::name(k), on ? "ON" : "OFF");
  }
  if (!available) ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(380.0f);
    ImGui::TextUnformatted(help);
    if (!available && unavailable_text) {
      ImGui::Separator();
      ImGui::TextUnformatted(unavailable_text);
    }
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
  if (!available && unavailable_text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", unavailable_text);
  }
}

// Case-insensitive substring match against "<id> <name>", so an item list can
// be narrowed by typing either part of a name or an item number. The picker
// and the storage box share it, so both answer to the same typing.
bool item_matches(int id, const char* name, const char* needle) {
  if (!needle || !needle[0]) return true;
  char hay[96];
  std::snprintf(hay, sizeof(hay), "%d %s", id, name);
  auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
  for (const char* p = hay; *p; ++p) {
    size_t i = 0;
    while (needle[i] && lower(p[i]) == lower(needle[i])) ++i;
    if (!needle[i]) return true;
  }
  return false;
}

void hms(float seconds, char* out, size_t n) {
  if (seconds < 0.0f) {
    std::snprintf(out, n, "?");
    return;
  }
  const int t = static_cast<int>(seconds);
  std::snprintf(out, n, "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
}

// The countdown's own format, the one the game's HUD draws: m:ss.cc.
void mmsscc(float seconds, char* out, size_t n) {
  if (seconds < 0.0f) {
    std::snprintf(out, n, "-");
    return;
  }
  const int cs = static_cast<int>(seconds * 100.0f);
  std::snprintf(out, n, "%d:%02d.%02d", cs / 6000, (cs / 100) % 60, cs % 100);
}

// One inventory bag as an editable table. Edits live in a buffer until Apply.
void draw_bag(int b, const game::Bag& bag, bool active) {
  auto& e_id = g_edit_id;
  auto& e_cnt = g_edit_cnt;
  auto& e_dirty = g_edit_dirty;
  const char* who = b == 0 ? "Rebecca" : "Billy";
  bool ask_apply = false, ask_discard = false;
  ImGui::PushID(b);
  if (active) ImGui::TextDisabled("(controlled now)");
  if (ImGui::BeginTable("bag", 5, ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupColumn("slot");
    ImGui::TableSetupColumn("item");
    ImGui::TableSetupColumn("count");
    ImGui::TableSetupColumn("");
    ImGui::TableSetupColumn("");
    for (int s = 0; s < 7; ++s) {
      const int live_id = s < 6 ? bag.id[s] : bag.personal_id;
      const int live_cnt = s < 6 ? bag.count[s] : bag.personal_count;
      if (!e_dirty[b][s]) {
        e_id[b][s] = live_id;
        e_cnt[b][s] = live_cnt;
      }
      ImGui::PushID(s);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      if (s < 6) ImGui::Text("%d%s", s + 1, bag.equipped == s ? " E" : "");
      else ImGui::TextUnformatted("P");
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(230.0f);
      const bool second_half = live_id == items::kFillerId;
      if (second_half) ImGui::BeginDisabled();
      if (ImGui::BeginCombo("##item", items::name(e_id[b][s]), ImGuiComboFlags_HeightLarge)) {
        auto pick = [&](int id) {
          e_id[b][s] = id;
          e_dirty[b][s] = true;
          // The game's infinite belonged to what the slot held, not to whatever
          // goes there instead: the new item starts as full as one slot of it
          // can be, which is also what Apply would have cut the marker down to.
          if (items::infinite(e_cnt[b][s])) e_cnt[b][s] = game::item_max(id);
          if (e_cnt[b][s] <= 0 && id) e_cnt[b][s] = 1;
        };
        // The filter belongs to whichever list is open, so it starts empty and
        // with the caret in it every time one does.
        static char filter[48] = {};
        if (ImGui::IsWindowAppearing()) {
          filter[0] = '\0';
          ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool enter = ImGui::InputTextWithHint("##filter", "type to narrow the list", filter, sizeof(filter),
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Separator();
        int first = -1, shown = 0;
        for (const items::Def& d : items::kTable) {
          if (d.kind == items::kFiller || !item_matches(d.id, d.name, filter)) continue;
          if (first < 0) first = d.id;
          ++shown;
          const bool sel = d.id == e_id[b][s];
          if (ImGui::Selectable(d.name, sel)) pick(d.id);
          if (sel && !filter[0]) ImGui::SetItemDefaultFocus();
        }
        if (!shown) ImGui::TextDisabled("nothing matches \"%s\"", filter);
        if (enter && first >= 0) {  // Enter takes the first match
          pick(first);
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndCombo();
      }
      if (second_half) ImGui::EndDisabled();
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(80.0f);
      // Held to the game's own per-slot maximum as it is typed, so the dialog
      // below asks about the number that will actually be written. A live
      // reading that is already over it is left alone - clamping one would
      // change a slot nobody asked to change.
      const int mx = game::item_max(e_id[b][s]);
      if (second_half) {
        ImGui::TextDisabled("-");
      } else if (!e_dirty[b][s] && items::infinite(e_cnt[b][s])) {
        // Shown as what it is, not as 65535, and not offered as a number to
        // type over: pick() is the way out of it, and it hands back a count.
        ImGui::TextUnformatted("infinite");
        if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
          ImGui::PushTextWrapPos(360.0f);
          ImGui::Text("%s never runs out: the game marks the slot with the count %d. Pick an item for this slot to "
                      "give it an ordinary count instead.",
                      items::name(e_id[b][s]), items::kInfiniteCount);
          ImGui::PopTextWrapPos();
          ImGui::EndTooltip();
        }
      } else {
        if (ImGui::InputInt("##cnt", &e_cnt[b][s])) {
          if (e_cnt[b][s] < 0) e_cnt[b][s] = 0;  // the - stepper and a typed "-" both land here
          if (mx > 0 && e_cnt[b][s] > mx) e_cnt[b][s] = mx;
          e_dirty[b][s] = true;
        }
        // kMaxCount is the panel's own ceiling standing in for an item the game
        // keeps no real count for (the knife), so it is not quoted as a limit.
        if (mx > 0 && mx < items::kMaxCount && ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
          if (mx == 1) ImGui::Text("%s does not stack - one per slot", items::name(e_id[b][s]));
          else ImGui::Text("One slot holds up to %d %s", mx, items::name(e_id[b][s]));
          ImGui::EndTooltip();
        }
      }
      ImGui::TableNextColumn();
      if (second_half) {
        ImGui::TextDisabled("2nd half");
      } else if (e_dirty[b][s]) {
        // Both ask first: these two buttons write to the game's inventory and
        // throw away what was typed, and a single stray click should not be
        // able to do either.
        if (ImGui::SmallButton("Apply")) {
          g_ask_slot[b] = s;
          ask_apply = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
          g_ask_slot[b] = s;
          ask_discard = true;
        }
      } else if (items::two_slot(live_id)) {
        ImGui::TextDisabled("2 slots");
      }
      // Into the storage box. The personal item slot stays where it is: the
      // game hands it out per character and expects it to be there.
      ImGui::TableNextColumn();
      if (s < 6 && !second_half && live_id != 0) {
        if (ImGui::SmallButton("Store")) storage::request_store(b, s);
        if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
          ImGui::Text("Put %s%s in the storage box, under %s, and empty this slot", items::name(live_id),
                      items::quantity(live_cnt).text, items::bank_name(items::bank(live_id)));
          const int stack = items::infinite(live_cnt) ? 0 : storage::stack_size(live_id);
          if (stack > 0)
            ImGui::TextDisabled("It joins any %s already there, in stacks of up to %d - what one slot holds",
                                items::name(live_id), stack);
          ImGui::EndTooltip();
        }
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  // Asked outside the table, so the dialog does not belong to a row that may
  // have been redrawn differently by the time it is answered.
  if (ask_apply) ImGui::OpenPopup("apply this change?");
  if (ask_discard) ImGui::OpenPopup("discard this edit?");
  char label[24];
  const int as = g_ask_slot[b];
  if (ImGui::BeginPopupModal("apply this change?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (as < 0 || as > 6 || !e_dirty[b][as]) {
      ImGui::CloseCurrentPopup();
    } else {
      ImGui::Text("Set %s %s to %s%s?", who, slot_label(as, label, sizeof(label)), items::name(e_id[b][as]),
                  items::quantity(e_cnt[b][as]).text);
      ImGui::TextDisabled("This writes straight into the game's inventory.");
      if (ImGui::Button("Apply")) {
        cheats::request_slot(b, as, e_id[b][as], e_cnt[b][as]);
        e_dirty[b][as] = false;
        g_ask_slot[b] = -1;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        g_ask_slot[b] = -1;
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopupModal("discard this edit?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (as < 0 || as > 6 || !e_dirty[b][as]) {
      ImGui::CloseCurrentPopup();
    } else {
      ImGui::Text("Throw away the edit to %s %s?", who, slot_label(as, label, sizeof(label)));
      ImGui::TextDisabled("The row goes back to showing what the inventory holds.");
      if (ImGui::Button("Discard")) {
        e_dirty[b][as] = false;
        g_ask_slot[b] = -1;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Keep editing")) {
        g_ask_slot[b] = -1;
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
  ImGui::PopID();
}

// The storage box: everything the player pushed out of the two inventories,
// listed one bank at a time.
void draw_storage(const cheats::Status& st) {
  const storage::View v = storage::view();
  const ImVec4 orange(1.0f, 0.6f, 0.3f, 1.0f);
  static int drop_idx = -1, drop_id = 0, drop_count = 0;  // the entry the x button asked about
  bool ask_drop = false;

  const int n = static_cast<int>(v.items.size());
  ImGui::SeparatorText("Item storage");
  if (v.mode == 2) ImGui::Text("%d item(s) - Leech Hunter has no save, so this box is not kept", n);
  else if (v.slot >= 0) ImGui::Text("%d item(s), kept with save file %d", n, v.slot + 1);
  else ImGui::Text("%d item(s)", n);
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(400.0f);
    ImGui::TextUnformatted(
        "An extra container beside the two inventories. Store puts an item in it and frees the slot; "
        "-> Rebecca and -> Billy put one back: onto the stack of it that character already carries, for the "
        "items the game stacks, and into a free slot for everything else.\n\n"
        "Everything in it is filed under one of five banks - key items, weapons, ammo, heals (herbs and "
        "first aid sprays), and everything else - and each bank is kept sorted by name, the fullest stack "
        "of an item first. The tabs pick the bank on show.\n\n"
        "Ammo, ink ribbons and the other items the game stacks are merged with what is already there, in "
        "stacks as big as one inventory slot holds, so a few rounds top up a stack instead of adding an entry. "
        "Weapons, herbs and key items keep an entry each.\n\n"
        "Saving the game writes the box into RE0CabbyCodes.storage.ini beside the mod, under that save "
        "file's line, and loading that file puts those items back. The game's own save file is never "
        "touched.\n\n"
        "A save file has one box. A new game starts with an empty one, saving a cleared game leaves that "
        "file empty for the next playthrough, and Leech Hunter - which has no save of its own - gets a box "
        "that lasts only as long as the run.");
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
  if (!game::save_watch_ready()) {
    ImGui::TextColored(orange, "The game's save/load could not be watched - the box will not follow your saves (see the log).");
  } else if (!v.file_ok) {
    ImGui::TextColored(orange, "RE0CabbyCodes.storage.ini could not be written - see the log.");
  } else if (!v.clear_ok) {
    ImGui::TextColored(orange, "A cleared-game save could not be recognised - it will keep its box instead of leaving the file empty (see the log).");
  } else if (!game::item_stacks_known()) {
    ImGui::TextColored(orange, "Which items the game stacks could not be read - the mod's own list decides what the box merges (see the log).");
  }

  // One bank on show at a time, picked with the tabs. The box keeps itself in
  // bank order (storage.cpp), so a bank is a run of it and an index drawn here
  // is the box's own. Every tab carries its bank's count, so an item stored
  // while another bank is on show can still be seen to arrive - which changes
  // the label, so the tab's ID is pinned after the ###. The count goes in
  // bare: in brackets the five labels were the widest row of the panel, which
  // sizes itself to that row, and pushed it 75 px wider.
  static int shown_bank = items::kBankKey;
  // The box has no size limit, so it gets the item picker's filter: the same
  // "<id> <name>" match, over the bank on show. Unlike the picker's, which
  // belongs to a list that is opened and closed, this one is a property of a
  // list that stays up, so it is kept across frames - and banks - and cleared
  // by hand. It is counted in every bank, so a search that finds nothing in
  // this one can say where it did find something.
  static char filter[48] = {};
  int held[items::kBankCount] = {}, matched[items::kBankCount] = {};
  for (int i = 0; i < n; ++i) {
    const int k = items::bank(v.items[i].id);
    ++held[k];
    if (item_matches(v.items[i].id, items::name(v.items[i].id), filter)) ++matched[k];
  }
  if (ImGui::BeginTabBar("banks")) {
    for (int k = 0; k < items::kBankCount; ++k) {
      char tab[48];
      std::snprintf(tab, sizeof(tab), "%s %d###bank%d", items::bank_name(k), held[k], k);
      if (ImGui::BeginTabItem(tab)) {
        shown_bank = k;
        ImGui::EndTabItem();
      }
    }
    ImGui::EndTabBar();
  }
  const int bank = shown_bank;
  const int shown = matched[bank];
  if (n > 0) {
    ImGui::SetNextItemWidth(230.0f);
    ImGui::InputTextWithHint("##storage filter", "type to narrow the list", filter, sizeof(filter));
    if (filter[0]) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Clear")) filter[0] = '\0';
      ImGui::SameLine();
      ImGui::TextDisabled("%d of %d shown", shown, held[bank]);
    }
  }

  char table_id[16];  // each bank keeps its own scroll position
  std::snprintf(table_id, sizeof(table_id), "storage%d", bank);
  if (n == 0) {
    ImGui::TextDisabled("The box is empty - use Store beside an inventory slot to put something in it.");
  } else if (held[bank] == 0) {
    ImGui::TextDisabled("Nothing is stored under %s.", items::bank_name(bank));
  } else if (shown == 0) {
    char elsewhere[160] = "";
    int len = 0;
    for (int k = 0; k < items::kBankCount; ++k)
      if (k != bank && matched[k] && len < static_cast<int>(sizeof(elsewhere)))
        len += std::snprintf(elsewhere + len, sizeof(elsewhere) - len, "%s%d under %s", len ? ", " : " - ",
                             matched[k], items::bank_name(k));
    ImGui::TextDisabled("nothing under %s matches \"%s\"%s", items::bank_name(bank), filter, elsewhere);
  } else if (ImGui::BeginTable(table_id, 3,
                               ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg,
                               ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * (shown > 12 ? 12.5f : shown + 0.5f)))) {
    ImGui::TableSetupColumn("item");
    ImGui::TableSetupColumn("count");
    ImGui::TableSetupColumn("");
    for (int i = 0; i < n; ++i) {
      if (items::bank(v.items[i].id) != bank || !item_matches(v.items[i].id, items::name(v.items[i].id), filter))
        continue;
      ImGui::PushID(i);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(items::name(v.items[i].id));
      ImGui::TableNextColumn();
      if (items::infinite(v.items[i].count)) ImGui::TextUnformatted("infinite");
      else ImGui::Text("%d", v.items[i].count);
      ImGui::TableNextColumn();
      // What a take would do is worked out the way the take itself does it
      // (storage::fit), so a button that cannot move anything is not offered
      // and its tooltip says where the item would go.
      const int id = v.items[i].id, count = v.items[i].count;
      const bool two = items::two_slot(id);
      for (int b = 0; b < 2; ++b) {
        const char* who = b ? "Billy" : "Rebecca";
        const storage::Fit f = st.bags_ok ? storage::fit(st.bags[b], id, count) : storage::Fit{};
        if (b) ImGui::SameLine();
        ImGui::BeginDisabled(!f.ok());
        if (ImGui::SmallButton(b == 0 ? "-> Rebecca" : "-> Billy")) storage::request_take(i, id, count, b);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::BeginTooltip()) {
          const int topped = f.topped();
          if (!f.ok() && storage::stack_size(id) > 0 && !items::infinite(count))
            ImGui::Text("%s has no free slot, and no stack of %s with room", who, items::name(id));
          else if (!f.ok())
            ImGui::Text("%s has no free %s", who, two ? "pair of slots" : "slot");
          else if (topped > 0 && f.slot >= 0)
            ImGui::Text("Put %d onto the %s %s already carries and %d into a free slot", topped, items::name(id), who,
                        f.into);
          else if (topped > 0)
            ImGui::Text("Put %d onto the %s %s already carries", topped, items::name(id), who);
          else if (f.left > 0)
            ImGui::Text("Put %d of these %s into %s's first free slot - a slot holds %d, so %d stay in the box", f.into,
                        items::name(id), who, game::item_max(id), f.left);
          else
            ImGui::Text("Put %s back into %s's first free slot", items::name(id), who);
          if (topped > 0 && f.left > 0) ImGui::TextDisabled("The other %d do not fit and stay in the box", f.left);
          ImGui::EndTooltip();
        }
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("x")) {
        drop_idx = i;
        drop_id = v.items[i].id;
        drop_count = v.items[i].count;
        ask_drop = true;
      }
      if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
        ImGui::TextUnformatted("Throw this item away");
        ImGui::EndTooltip();
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  // Asked outside the table so the dialog is not scoped to a row that may be
  // gone by the time it is answered.
  if (ask_drop) ImGui::OpenPopup("throw item away?");
  if (ImGui::BeginPopupModal("throw item away?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Throw away %s%s?", items::name(drop_id), items::quantity(drop_count).text);
    ImGui::TextDisabled("The records on disk only change when you save.");
    if (ImGui::Button("Throw away")) {
      storage::request_drop(drop_idx, drop_id, drop_count);
      drop_idx = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      drop_idx = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

// One save file in a line: "2:31:07, 12 saves, cleared", or "no data".
void describe_file(const savefiles::File& f, char* out, size_t n) {
  if (!f.known) {
    std::snprintf(out, n, "?");
    return;
  }
  if (!f.used) {
    std::snprintf(out, n, "no data");
    return;
  }
  char t[32];
  hms(f.seconds, t, sizeof(t));
  int len = std::snprintf(out, n, "%s", t);
  if (f.saves >= 0 && len < static_cast<int>(n))
    len += std::snprintf(out + len, n - len, ", %d save%s", f.saves, f.saves == 1 ? "" : "s");
  if (f.cleared && len < static_cast<int>(n)) len += std::snprintf(out + len, n - len, ", cleared");
  if (f.wesker && len < static_cast<int>(n)) std::snprintf(out + len, n - len, ", Wesker mode");
}

// The title screen's load list: every save file the game has, with a Delete and
// a Copy for each one in use. Both write the game's own save file on disk the
// moment they are confirmed, so both ask first, and the request carries what
// was seen so a file that changed in between is left alone (savefiles.cpp).
void draw_save_files() {
  const savefiles::View v = savefiles::view();
  const ImVec4 orange(1.0f, 0.6f, 0.3f, 1.0f);
  bool open_delete = false, open_copy = false;

  ImGui::SeparatorText("Save files");
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(400.0f);
    ImGui::TextUnformatted(
        "The same twenty save files the game's list shows, the one its cursor is on highlighted.\n\n"
        "Delete empties a file: it shows NO DATA from then on, exactly like a file that was never used, and the next "
        "save or copy into it fills it again. Copy to... puts a copy of a save into another file, empty or not.\n\n"
        "Both write the game's save file (data0.bin) straight away, through the game's own save, so neither can be "
        "undone - back data0.bin up first if in doubt. The item storage box follows the file: a deleted file's box "
        "is emptied, and a copy takes the box of the file it came from.");
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
  if (!v.ready) ImGui::TextColored(orange, "The save files cannot be changed in this build - see the log.");
  const bool can = v.ready && v.up && !v.busy;

  if (v.files > 0 && ImGui::BeginTable("files", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn("file");
    ImGui::TableSetupColumn("save");
    ImGui::TableSetupColumn("storage");
    ImGui::TableSetupColumn("");
    ImGui::TableHeadersRow();
    for (int i = 0; i < v.files; ++i) {
      const savefiles::File& f = v.file[i];
      ImGui::PushID(i);
      ImGui::TableNextRow();
      if (i == v.cursor) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImGuiCol_Header));
      ImGui::TableNextColumn();
      ImGui::Text("%2d", i + 1);
      ImGui::TableNextColumn();
      char d[96];
      describe_file(f, d, sizeof(d));
      if (f.used) ImGui::TextUnformatted(d);
      else ImGui::TextDisabled("%s", d);
      ImGui::TableNextColumn();
      if (f.box > 0) ImGui::Text("%d item%s", f.box, f.box == 1 ? "" : "s");
      ImGui::TableNextColumn();
      if (f.used) {
        ImGui::BeginDisabled(!can);
        if (ImGui::SmallButton("Copy to...")) ImGui::OpenPopup("copy to");
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete")) {
          g_file_ask = kAskDelete;
          g_file_to = i;
          g_file_to_seen = f;
          open_delete = true;
        }
        ImGui::EndDisabled();
        if (ImGui::BeginPopup("copy to")) {
          ImGui::TextDisabled("Copy save file %d to:", i + 1);
          ImGui::Separator();
          for (int j = 0; j < v.files; ++j) {
            if (j == i) continue;
            char dj[96], label[128];
            describe_file(v.file[j], dj, sizeof(dj));
            std::snprintf(label, sizeof(label), "File %d - %s", j + 1, dj);
            if (ImGui::Selectable(label)) {
              g_file_ask = kAskCopy;
              g_file_from = i;
              g_file_to = j;
              g_file_from_seen = f;
              g_file_to_seen = v.file[j];
              open_copy = true;
            }
          }
          ImGui::EndPopup();
        }
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (v.busy) {
    ImGui::TextColored(orange, "Writing the save file...");
  } else if (v.note[0]) {
    if (v.note_error) ImGui::PushStyleColor(ImGuiCol_Text, orange);
    ImGui::TextWrapped("%s", v.note);
    if (v.note_error) ImGui::PopStyleColor();
  }

  // Asked outside the table, so neither dialog belongs to a row.
  if (open_delete) ImGui::OpenPopup("delete this save?");
  if (open_copy) ImGui::OpenPopup("copy this save?");
  char d[96];
  if (ImGui::BeginPopupModal("delete this save?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (g_file_ask != kAskDelete || g_file_to < 0 || g_file_to >= v.files || !can) {
      forget_file_dialogs();
      ImGui::CloseCurrentPopup();
    } else {
      describe_file(g_file_to_seen, d, sizeof(d));
      ImGui::Text("Delete save file %d (%s)?", g_file_to + 1, d);
      ImGui::TextDisabled("It shows NO DATA from now on. The game's save file is written straight away,");
      ImGui::TextDisabled("so this cannot be undone.");
      if (g_file_to_seen.box > 0)
        ImGui::TextDisabled("Its item storage box (%d item%s) is emptied with it.", g_file_to_seen.box,
                            g_file_to_seen.box == 1 ? "" : "s");
      if (ImGui::Button("Delete")) {
        savefiles::request_delete(g_file_to, g_file_to_seen);
        forget_file_dialogs();
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        forget_file_dialogs();
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopupModal("copy this save?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (g_file_ask != kAskCopy || g_file_from < 0 || g_file_from >= v.files || g_file_to < 0 || g_file_to >= v.files ||
        !can) {
      forget_file_dialogs();
      ImGui::CloseCurrentPopup();
    } else {
      describe_file(g_file_from_seen, d, sizeof(d));
      if (g_file_to_seen.used) ImGui::Text("Copy save file %d (%s) over save file %d?", g_file_from + 1, d, g_file_to + 1);
      else ImGui::Text("Copy save file %d (%s) to the empty save file %d?", g_file_from + 1, d, g_file_to + 1);
      if (g_file_to_seen.used) {
        describe_file(g_file_to_seen, d, sizeof(d));
        ImGui::TextDisabled("The save in file %d (%s) is replaced.", g_file_to + 1, d);
      }
      ImGui::TextDisabled("The game's save file is written straight away, so this cannot be undone.");
      if (g_file_from_seen.box > 0 || g_file_to_seen.box > 0)
        ImGui::TextDisabled("The item storage box goes with the save: file %d gets file %d's (%d item%s).", g_file_to + 1,
                            g_file_from + 1, g_file_from_seen.box > 0 ? g_file_from_seen.box : 0,
                            g_file_from_seen.box == 1 ? "" : "s");
      if (ImGui::Button("Copy")) {
        savefiles::request_copy(g_file_from, g_file_to, g_file_from_seen, g_file_to_seen);
        forget_file_dialogs();
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        forget_file_dialogs();
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
}

}  // namespace

bool ensure_context(HWND hwnd) {
  if (g_context_ready) return true;
  if (!hwnd) return false;
  g_hwnd = hwnd;

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  // Window position persists beside the DLL, under the mod's own name rather
  // than a stray imgui.ini in the game folder.
  static char ini_path[MAX_PATH] = {};
  std::snprintf(ini_path, sizeof(ini_path), "%sRE0CabbyCodes.imgui.ini", config::dir());
  io.IniFilename = ini_path;
  // No keyboard navigation: the game's pause menu keeps its keys. Dragging is
  // limited to the title bar so a click on the panel's background cannot pick
  // the window up by accident.
  io.ConfigWindowsMoveFromTitleBarOnly = true;
  ImGui::StyleColorsDark();
  ImGui::GetStyle().WindowRounding = 4.0f;

  if (!ImGui_ImplWin32_Init(hwnd)) {
    logf("ERROR: ImGui Win32 backend init failed");
    ImGui::DestroyContext();
    return false;
  }
  g_orig_wndproc = reinterpret_cast<WNDPROC>(
      SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hk_wndproc)));
  g_context_ready = true;
  logf("overlay ready (hwnd=%p, render thread %lu, window thread %lu)", static_cast<void*>(hwnd), GetCurrentThreadId(),
       GetWindowThreadProcessId(hwnd, nullptr));
  return true;
}

// Published by the frame that drew (draw_panel), cleared by the frame that did
// not (wants_draw): a plain read, from any thread, of what the panel took.
bool capturing_mouse() { return g_capture_mouse != 0; }
bool capturing_keyboard() { return g_capture_keyboard != 0; }

void lock_imgui() {
  if (g_imgui_cs_ready) EnterCriticalSection(&g_imgui_cs);
}

void unlock_imgui() {
  if (g_imgui_cs_ready) LeaveCriticalSection(&g_imgui_cs);
}

// Called from the present hook with the context lock held.
bool wants_draw() {
  const dispatch::Snapshot s = dispatch::snapshot();
  const bool showable = s.show_panel || config::get().always_show;
  if (!showable) {
    g_user_hidden = false;  // the next pause starts visible again
    forget_bag_edits();
    forget_file_dialogs();
  }
  g_visible = g_context_ready && showable && !g_user_hidden;
  if (!g_visible) {  // nothing is being taken from the game until it draws again
    InterlockedExchange(&g_capture_mouse, 0);
    InterlockedExchange(&g_capture_keyboard, 0);
  }
  ImGuiIO& io = ImGui::GetIO();
  io.MouseDrawCursor = g_visible;  // the game may hide the OS cursor
  // If a release was ever missed the panel would follow the pointer for ever;
  // the physical button is the truth.
  if (g_visible) {
    static const int kVk[3] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON};
    for (int b = 0; b < 3; ++b)
      if (io.MouseDown[b] && !(GetAsyncKeyState(kVk[b]) & 0x8000)) io.AddMouseButtonEvent(b, false);
  } else if (g_context_ready) {
    // Input made while the panel is away is not the panel's. The window
    // procedure hands ImGui every message whether it is drawn or not (a
    // release that arrives after it is hidden has to be delivered), but ImGui
    // only *queues* those events: NewFrame drains the queue and runs the
    // widget logic, and NewFrame does not run while the panel is hidden. So
    // nothing was thrown away - it was banked, and replayed onto the panel the
    // instant it came back, at the positions the cursor had been at while the
    // player was using the game. That is how an inventory row got Applied and
    // how items moved into the storage box with nobody touching the panel: the
    // log has two takes firing 33 ms and 95 ms after the pause menu opened,
    // one to two frames, which is not a click anybody made. The events are
    // still accepted above - they are dropped here instead of banked.
    io.ClearEventsQueue();
    io.ClearInputMouse();
    io.ClearInputKeys();
  }
  return g_visible;
}

// Called from the present hook, between NewFrame and Render, with the context
// lock held.
void draw_panel() {
  const dispatch::Snapshot s = dispatch::snapshot();
  const cheats::Status st = cheats::status();
  const ImGuiIO& io = ImGui::GetIO();
  // NewFrame has run, so what ImGui wants this frame is settled; the input
  // guard reads these from its own thread.
  InterlockedExchange(&g_capture_mouse, io.WantCaptureMouse ? 1 : 0);
  InterlockedExchange(&g_capture_keyboard, io.WantTextInput ? 1 : 0);
  const ImVec4 orange(1.0f, 0.6f, 0.3f, 1.0f);
  ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 460, 60), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Resident Evil 0 - Cabby Codes  v" RE0CC_VERSION, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }
  if (!s.decrypted) {
    ImGui::TextColored(orange, "Waiting for the game to unpack (SteamStub)...");
    ImGui::End();
    return;
  }
  if (!s.game_ready) {
    ImGui::TextColored(orange, "Game hooks: %s", game::status_text());
    ImGui::TextDisabled("Build: %s", game::build_string());
    ImGui::End();
    return;
  }
  if (!game::build_supported()) ImGui::TextColored(orange, "Unsupported build: %s", game::build_string());
  if (s.file_screen) {
    // The title screen's load list: the panel is the save file manager, and
    // nothing else on it applies before a game is running.
    draw_save_files();
    ImGui::Separator();
    ImGui::TextDisabled("F%d hides this panel", config::get().toggle_key - 0x6F);
    ImGui::End();
    return;
  }
  if (!s.in_game) {
    ImGui::TextDisabled("Waiting for a game session (load a save or start a new game)...");
    ImGui::Separator();
  }

  ImGui::BeginDisabled(!s.in_game);
  ImGui::SeparatorText("Player");
  cheat_row(cheats::kGodMode, "God mode",
            "Both characters are held at full health and cured of poison every frame. A single lethal hit "
            "can still get through in the frame it lands.",
            st.nchars > 0, "player units not found");
  cheat_row(cheats::kOneHitKills, "One hit kills",
            "Every live enemy is held at 1 HP, so your next hit kills it. Leech-men take body shots on a second "
            "pool, which is held at 1 too, so one of those collapses them. Bosses with scripted phases may behave oddly.",
            st.enemy_table, "unit table not found");
  if (st.enemy_table) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%d enemies)", st.enemies);
  }
  cheat_row(cheats::kInfiniteAmmo, "Infinite ammo",
            "Every weapon in both inventories keeps its loaded count: each shot is put straight back. Pickups "
            "and reloads still raise it.",
            st.bags_ok, "inventory not found");

  ImGui::SeparatorText("Saves & time");
  {
    // What it does depends on which of its sites this build has: with the takes
    // the game never takes a ribbon, without them one is put back after the save;
    // the typewriter's check is what lets you save without carrying one.
    char help[256];
    std::snprintf(help, sizeof(help), "%s %s",
                  st.site_ink ? "Saving at a typewriter never uses up a ribbon, not even the last one."
                              : "A ribbon used at a typewriter is put back in the inventory right after the save.",
                  st.site_ink_check ? "You can save without carrying one at all."
                                    : "You still need one on you to save: the typewriter's check was not found.");
    cheat_row(cheats::kInfiniteInk, "Infinite ink ribbons", help, st.site_ink || st.site_ink_check || st.bags_ok,
              "inventory not found");
  }
  cheat_row(cheats::kNoSaveCount, "Save without counting",
            "Saving does not raise the save counter, and backing out of a save does not lower it (the game's "
            "own increment and its take-back on a cancel are both skipped).",
            st.site_savecount, "patch site not found");
  {
    static int edit_sc = -1;
    if (edit_sc < 0 && st.save_count >= 0) edit_sc = st.save_count;
    ImGui::BeginDisabled(!st.status_ok);
    ImGui::Text("Save count: %d", st.save_count);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    ImGui::InputInt("##sc", &edit_sc);
    ImGui::SameLine();
    if (ImGui::SmallButton("Apply##sc")) cheats::request_save_count(edit_sc < 0 ? 0 : edit_sc);
    ImGui::EndDisabled();
  }
  {
    char t[32];
    hms(st.status_ok && st.playtime_rate > 0.0f ? st.playtime_raw / st.playtime_rate : -1.0f, t, sizeof(t));
    cheat_row(cheats::kFreezePlaytime, "Freeze play time",
              "The game clock stops advancing (the per-frame store is skipped, or held when the site is unknown).",
              st.site_playtime || st.status_ok, "clock not identified yet");
    if (st.playtime_hold) {
      ImGui::SameLine();
      ImGui::TextColored(orange, "(hold mode)");
    }
    static int eh = 0, em = 0, es = 0;
    ImGui::BeginDisabled(!st.status_ok);
    ImGui::Text("Play time: %s", t);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0f);
    ImGui::InputInt("h", &eh, 0, 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0f);
    ImGui::InputInt("m", &em, 0, 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0f);
    ImGui::InputInt("s", &es, 0, 0);
    ImGui::SameLine();
    if (ImGui::SmallButton("Set##pt")) {
      if (eh < 0) eh = 0;
      if (em < 0) em = 0;
      if (es < 0) es = 0;
      cheats::request_playtime_seconds(eh * 3600 + em * 60 + es);
    }
    ImGui::EndDisabled();
    if (!st.status_ok && s.in_game) ImGui::TextDisabled("(play a few seconds unpaused so the clock can be identified)");
  }
  {
    // The scripted countdowns (the train's brakes and the rest): one field that
    // only exists while a timed section is running, so both the switch and the
    // editor stay disabled until the game starts one.
    char t[32];
    mmsscc(st.countdown_active && st.countdown_rate > 0.0f ? st.countdown_raw / st.countdown_rate : -1.0f, t, sizeof(t));
    cheat_row(cheats::kFreezeCountdown, "Freeze countdown timer",
              "Timed sections (the train's brakes, and every other countdown the game puts on screen) stop counting "
              "down. The freeze belongs to the section that is running: it lets go when that section ends, and the "
              "next one is frozen again at its own starting value. Nothing happens while no section is running.",
              st.countdown_ready || st.site_countdown, "timer not identified yet");
    if (st.countdown_hold) {
      ImGui::SameLine();
      ImGui::TextColored(orange, "(hold mode)");
    }
    static int cm = 0, cs2 = 0;
    ImGui::BeginDisabled(!st.countdown_active);
    ImGui::Text("Countdown: %s", t);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0f);
    ImGui::InputInt("m##cd", &cm, 0, 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0f);
    ImGui::InputInt("s##cd", &cs2, 0, 0);
    ImGui::SameLine();
    if (ImGui::SmallButton("Set##cd")) {
      cm = cm < 0 ? 0 : (cm > 99 ? 99 : cm);   // the HUD's own m:ss range
      cs2 = cs2 < 0 ? 0 : (cs2 > 59 ? 59 : cs2);
      cheats::request_countdown_seconds(static_cast<float>(cm * 60 + cs2));
    }
    ImGui::EndDisabled();
    if (st.countdown_ready && !st.countdown_active) ImGui::TextDisabled("(no timer is running)");
  }

  ImGui::SeparatorText("Inventory");
  if (!game::item_max_known()) {
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                       "The game's own per-slot limits could not be read - the mod's own item table stands in for "
                       "them (see the log).");
  }
  if (!st.bags_ok) {
    ImGui::TextDisabled("Inventory not found yet - see the log");
  } else if (ImGui::BeginTabBar("bags")) {
    for (int b = 0; b < 2; ++b) {
      bool active = false;
      for (int i = 0; i < st.nchars; ++i)
        if (st.chars[i].active && (st.chars[i].type == 3) == (b == 0)) active = true;
      if (ImGui::BeginTabItem(b == 0 ? "Rebecca" : "Billy")) {
        draw_bag(b, st.bags[b], active);
        ImGui::EndTabItem();
      }
    }
    ImGui::EndTabBar();
  }
  if (st.last_action[0]) ImGui::TextWrapped("%s", st.last_action);

  draw_storage(st);
  ImGui::EndDisabled();

  ImGui::Separator();
  ImGui::TextDisabled("F%d hides this panel", config::get().toggle_key - 0x6F);
  ImGui::End();
}

void shutdown_imgui() {
  if (!g_context_ready) return;
  ImGui_ImplWin32_Shutdown();
  if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
  g_context_ready = false;
  g_visible = false;
  InterlockedExchange(&g_capture_mouse, 0);
  InterlockedExchange(&g_capture_keyboard, 0);
}

bool install() {
  if (!g_imgui_cs_ready) {  // before the window procedure or the present hook exists
    InitializeCriticalSection(&g_imgui_cs);
    g_imgui_cs_ready = true;
  }
  HMODULE exe = GetModuleHandleA(nullptr);
  const bool d3d = dx9::install_import(exe);
  if (!config::get().disable_input) input::install(exe);
  else logf("input guard disabled by config - clicks on the panel will also reach the game");
  if (!config::get().disable_gpa) {
    void* prev = mem::iat_hook(exe, "KERNEL32.dll", "GetProcAddress", reinterpret_cast<void*>(&hk_get_proc_address));
    if (prev) {
      g_orig_gpa = reinterpret_cast<GetProcAddressFn>(prev);
      game::note_iat_slot(mem::iat_slot(exe, "KERNEL32.dll", "GetProcAddress"), reinterpret_cast<uintptr_t>(prev));
      logf("overlay: GetProcAddress import hooked (original %p) - pass-through%s", prev,
           config::get().trace ? ", tracing names" : "");
    } else {
      logf("overlay: GetProcAddress not in the exe's import table - no fallback capture");
    }
  }
  return d3d;
}

void uninstall() {
  // Window procedure first: a message arriving after our image is gone would
  // jump into freed memory.
  if (g_orig_wndproc && g_hwnd && IsWindow(g_hwnd)) {
    SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_orig_wndproc));
    g_orig_wndproc = nullptr;
  }
  dx9::uninstall();
  input::uninstall();
  shutdown_imgui();
  if (g_orig_gpa) {
    mem::iat_hook(GetModuleHandleA(nullptr), "KERNEL32.dll", "GetProcAddress", reinterpret_cast<void*>(g_orig_gpa));
    g_orig_gpa = nullptr;
  }
  logf("overlay hooks removed");
}

}  // namespace re0cc::overlay
