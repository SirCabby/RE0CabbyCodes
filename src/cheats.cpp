#include "cheats.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "items.h"
#include "log.h"
#include "mem.h"
#include "storage.h"

namespace re0cc::cheats {
namespace {

volatile LONG g_enabled[kCount] = {};
// g_status is the main thread's own working copy - tick() reads the previous
// tick's values out of it while it builds the next. g_published is the only
// thing another thread may look at, and it is swapped over under the lock: the
// panel runs on the render thread and used to copy this struct while tick was
// half-way through overwriting it, so a frame could pair an item with another
// tick's count, or draw a bag that a failed reading had just zeroed. It came
// and went on its own, which is what an unsynchronised read looks like.
Status g_status;
Status g_published;
CRITICAL_SECTION g_status_lock;
bool g_status_lock_ready = false;
const char* kNames[kCount] = {"God mode",         "One hit kills",    "Infinite ammo",         "Infinite ink ribbons",
                              "Save without counting", "Freeze play time", "Freeze countdown timer"};

bool on(Kind k) { return g_enabled[k] != 0; }

// --- requests (render thread -> main thread) --------------------------------------------
struct Request {
  enum Type { kSaveCount, kPlaytime, kCountdown, kSlot, kEquipped } type;
  int a, b, c, d;
  float f;
};
Request g_req[32];
int g_req_n = 0;
CRITICAL_SECTION g_req_lock;
bool g_lock_ready = false;

void push(Request r) {
  if (!g_lock_ready) return;
  EnterCriticalSection(&g_req_lock);
  if (g_req_n < 32) g_req[g_req_n++] = r;
  LeaveCriticalSection(&g_req_lock);
}
int drain(Request* out, int max) {
  if (!g_lock_ready) return 0;
  EnterCriticalSection(&g_req_lock);
  const int n = g_req_n < max ? g_req_n : max;
  std::memcpy(out, g_req, sizeof(Request) * static_cast<size_t>(n));
  g_req_n = 0;
  LeaveCriticalSection(&g_req_lock);
  return n;
}

// Main thread: keep the working copy and hand a whole one to the panel.
void publish(const Status& st) {
  g_status = st;
  if (!g_status_lock_ready) return;
  EnterCriticalSection(&g_status_lock);
  g_published = st;
  LeaveCriticalSection(&g_status_lock);
}

void note(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(g_status.last_action, sizeof(g_status.last_action), fmt, args);
  va_end(args);
  logf("%s", g_status.last_action);
}

// --- inventory edits ------------------------------------------------------------------------
// The count is held to what the game itself fits in one slot (game::item_max),
// and a count that had to come down says so: it used to be cut silently, which
// is how a stack of ten leech charms - an id the mod's own table wrongly held
// at one - became a single charm with nothing in the log but the number that
// was written.
int clamp_count(int id, int count) {
  if (id == 0) return 0;
  const int mx = game::item_max(id);
  if (count < 0) return 0;
  return count > mx ? mx : count;
}

// " (12 asked for; a slot holds 10)", or nothing at all when none was needed.
void clamp_note(char* out, size_t n, int asked, int got) {
  if (asked == got) {
    out[0] = '\0';
    return;
  }
  std::snprintf(out, n, " (%d asked for; a slot holds %d)", asked, got);
}

void apply_slot(int bag, int slot, int id, int count) {
  game::Bag b[2];
  if (!game::bags(b) || bag < 0 || bag > 1 || slot < 0 || slot > 6) {
    note("inventory edit ignored: inventory not available");
    return;
  }
  const char* who = bag == 0 ? "Rebecca" : "Billy";
  const int asked = count;
  count = clamp_count(id, count);
  char capped[64];
  clamp_note(capped, sizeof(capped), asked, count);
  if (slot == 6) {
    game::write_slot(bag, 6, id, count);
    note("%s personal item -> %s x%d%s", who, items::name(id), count, capped);
    return;
  }
  const game::Bag& g = b[bag];
  const int cur = g.id[slot];
  if (cur == items::kFillerId && id != items::kFillerId) {
    note("%s slot %d is the second half of %s - change slot %d instead", who, slot + 1, items::name(slot ? g.id[slot - 1] : 0), slot);
    return;
  }
  if (items::two_slot(id)) {
    if ((slot % 2) != 0 || slot > 4) {
      note("%s: %s takes two slots and must go in slot 1, 3 or 5", who, items::name(id));
      return;
    }
    const int nxt = g.id[slot + 1];
    if (nxt != 0 && nxt != items::kFillerId) {
      note("%s: slot %d must be empty for %s (it holds %s)", who, slot + 2, items::name(id), items::name(nxt));
      return;
    }
    game::write_slot(bag, slot, id, count);
    game::write_slot(bag, slot + 1, items::kFillerId, 1);
  } else {
    if (items::two_slot(cur) && slot + 1 <= 5 && g.id[slot + 1] == items::kFillerId) game::write_slot(bag, slot + 1, 0, 0);
    game::write_slot(bag, slot, id, count);
  }
  if (g.equipped == slot && !items::is_weapon(id)) game::write_equipped(bag, -1);
  note("%s slot %d -> %s x%d%s", who, slot + 1, items::name(id), count, capped);
}

// --- holds ---------------------------------------------------------------------------------------
bool g_god_logged[2] = {};
int g_ammo_held[2][6];
int g_ammo_id[2][6];  // the weapon the remembered count belongs to
bool g_ammo_valid[2][6];
int g_bags_carried = 0;  // ticks the panel has been shown the last good bag reading
int g_ink_slot[2] = {-1, -1};
int g_ink_count[2] = {0, 0};
float g_frozen_raw = -1.0f;
bool g_hold_active = false;
float g_frozen_countdown = -1.0f;
bool g_countdown_hold_active = false;

void hold_god(int nchars, const game::Character* chars) {
  for (int i = 0; i < nchars; ++i) {
    const game::Character& c = chars[i];
    const int mx = game::max_hp(c);
    const int idx = c.type == 3 ? 0 : 1;
    if (c.hp < mx) {
      game::set_hp(c, mx);
      if (!g_god_logged[idx]) {
        g_god_logged[idx] = true;
        logf("god mode: holding %s at %d HP", c.cls, mx);
      }
    }
    if (c.poison == 1) game::set_poison(c, 0);
  }
}

// One hit kills: every live enemy is held at 1 HP, so the game's own hit path
// kills it on the next hit (death animations, drops and scripts still run).
// The leech-man (uEnemy43) is the one enemy that hit path can bypass: unless
// the shot lands in its top zone or comes from fire, the damage goes to a
// second pool and its HP is never read, so that pool is held at 1 as well. It
// is not a health bar shared with the rest of the roster - only a unit whose
// reading came back carries one - and emptying it collapses the form into its
// leeches rather than killing it, which is the game's own way of putting one
// down.
int g_ohk_logged = 0, g_ohk_pool_logged = 0;
int hold_enemies() {
  game::Enemy list[128];
  const int n = game::enemies(list, 128);
  for (int i = 0; i < n; ++i) {
    if (list[i].hp > 1) {
      game::set_enemy_hp(list[i], 1);
      if (g_ohk_logged < 8) {
        ++g_ohk_logged;
        logf("one hit kills: %s %p held at 1 HP (was %d)", list[i].cls, reinterpret_cast<void*>(list[i].obj), list[i].hp);
      }
    }
    if (list[i].pool > 1) {
      game::set_enemy_pool(list[i], 1);
      if (g_ohk_pool_logged < 8) {
        ++g_ohk_pool_logged;
        logf("one hit kills: %s %p second pool held at 1 (was %d)", list[i].cls, reinterpret_cast<void*>(list[i].obj),
             list[i].pool);
      }
    }
  }
  return n;
}

// Infinite ammo: a weapon's loaded rounds are the inventory slot's count, so a
// drop in that count is put straight back. What is put back is never a number
// of the mod's own: it is what this weapon had in this slot a moment ago. The
// baseline is therefore tied to the item that is in the slot - put a different
// weapon there and it starts again, rather than pouring the last gun's magazine
// into the new one (a Hunting Gun that took over the handgun's slot came out
// holding fifteen rounds before that).
void hold_ammo(const game::Bag* bags, bool ok) {
  int held = 0;
  for (int b = 0; b < 2 && ok; ++b) {
    for (int s = 0; s < 6; ++s) {
      const int id = bags[b].id[s], count = bags[b].count[s];
      if (!items::is_weapon(id)) {
        g_ammo_valid[b][s] = false;
        continue;
      }
      if (!g_ammo_valid[b][s] || g_ammo_id[b][s] != id) {
        g_ammo_valid[b][s] = true;
        g_ammo_id[b][s] = id;
        g_ammo_held[b][s] = count;
      } else if (count < g_ammo_held[b][s]) {
        game::write_slot(b, s, id, g_ammo_held[b][s]);
      } else {
        g_ammo_held[b][s] = count;
      }
      ++held;
    }
  }
  g_status.ammo_held = held;
}

void hold_ink(const game::Bag* bags, bool ok) {
  bool tracked = false;
  for (int b = 0; b < 2 && ok; ++b) {
    int found = -1;
    for (int s = 0; s < 6; ++s)
      if (bags[b].id[s] == items::kInkRibbonId) { found = s; break; }
    if (found >= 0) {
      const int count = bags[b].count[found];
      if (g_ink_slot[b] == found && count < g_ink_count[b]) {
        game::write_slot(b, found, items::kInkRibbonId, g_ink_count[b]);
        ++g_status.ink_restored;
        logf("ink ribbons: %s used one (%d -> %d) - put back", b == 0 ? "Rebecca" : "Billy", g_ink_count[b], count);
      } else {
        g_ink_count[b] = count;
      }
      g_ink_slot[b] = found;
      tracked = true;
    } else if (g_ink_slot[b] >= 0 && bags[b].id[g_ink_slot[b]] == 0) {
      // The last ribbon was consumed and the slot emptied: put one back.
      game::write_slot(b, g_ink_slot[b], items::kInkRibbonId, 1);
      ++g_status.ink_restored;
      logf("ink ribbons: %s used the last one - put one back in slot %d", b == 0 ? "Rebecca" : "Billy", g_ink_slot[b] + 1);
      g_ink_count[b] = 1;
      tracked = true;
    } else {
      g_ink_slot[b] = -1;
    }
  }
  g_status.ink_tracked = tracked;
}

void toggle_patch(game::Site s, bool want, bool* applied_out) {
  mem::Patch* p = game::site(s);
  if (!p) {
    *applied_out = false;
    return;
  }
  if (want && !p->applied) {
    if (p->apply()) logf("patch applied: %s", p->label);
    else logf("ERROR: patch %s could not be applied (site changed?)", p->label);
  } else if (!want && p->applied) {
    if (p->revert()) logf("patch restored: %s", p->label);
    else logf("ERROR: patch %s could not be restored (site changed?)", p->label);
  }
  *applied_out = p->applied;
}

}  // namespace

void init() {
  if (g_lock_ready) return;
  InitializeCriticalSection(&g_req_lock);
  InitializeCriticalSection(&g_status_lock);
  g_status_lock_ready = true;
  g_lock_ready = true;
}

const char* name(Kind k) { return k >= 0 && k < kCount ? kNames[k] : "?"; }
bool enabled(Kind k) { return k >= 0 && k < kCount && g_enabled[k] != 0; }
void set_enabled(Kind k, bool on) {
  if (k >= 0 && k < kCount) InterlockedExchange(&g_enabled[k], on ? 1 : 0);
}
Status status() {
  if (!g_status_lock_ready) return Status{};
  EnterCriticalSection(&g_status_lock);
  const Status s = g_published;
  LeaveCriticalSection(&g_status_lock);
  return s;
}

void request_save_count(int v) { push({Request::kSaveCount, v, 0, 0, 0, 0.0f}); }
void request_playtime_seconds(int seconds) { push({Request::kPlaytime, seconds, 0, 0, 0, 0.0f}); }
void request_countdown_seconds(float seconds) { push({Request::kCountdown, 0, 0, 0, 0, seconds}); }
void request_slot(int bag, int slot, int id, int count) { push({Request::kSlot, bag, slot, id, count, 0.0f}); }
void request_equipped(int bag, int index) { push({Request::kEquipped, bag, index, 0, 0, 0.0f}); }

void tick(bool in_game) {
  static bool init_done = false;
  if (!init_done) {
    init_done = true;
    const config::Settings& c = config::get();
    set_enabled(kGodMode, c.god_mode);
    set_enabled(kOneHitKills, c.one_hit_kills);
    set_enabled(kInfiniteAmmo, c.infinite_ammo);
    set_enabled(kInfiniteInk, c.infinite_ink);
    set_enabled(kNoSaveCount, c.no_save_count);
    set_enabled(kFreezePlaytime, c.freeze_playtime);
    set_enabled(kFreezeCountdown, c.freeze_countdown);
  }
  // The item storage watches the game's own save and load, which happen on the
  // title screen too: it ticks before the in-game gate below.
  storage::tick(in_game);

  // A save or a load rewrites both bags, so the ammo baselines start again: a
  // weapon that happens to land in the slot it was in before must not be given
  // the count it had in the game that was running a moment ago.
  game::SaveOp op;
  if (game::save_op(&op) && op.mode != 0) std::memset(g_ammo_valid, 0, sizeof(g_ammo_valid));

  Status st;
  std::memcpy(st.last_action, g_status.last_action, sizeof(st.last_action));
  st.ink_restored = g_status.ink_restored;
  st.site_playtime = game::site(game::kSitePlaytimeStore) != nullptr;
  st.site_savecount = game::site(game::kSiteSaveCountInc) != nullptr;
  // Both takes or neither: a save path left taking ribbons would look like the
  // cheat working right up to the first save that goes through it.
  st.site_ink = game::site(game::kSiteInkTakeA) != nullptr && game::site(game::kSiteInkTakeB) != nullptr;
  st.site_ink_check = game::site(game::kSiteInkCheck) != nullptr;
  // Both steppers or neither: patching only one of them would silently leave
  // the countdown running through whichever of the two is the live one.
  st.site_countdown = game::site(game::kSiteCountdownStepA) != nullptr && game::site(game::kSiteCountdownStepB) != nullptr;
  st.status_ok = game::status_ready();
  if (st.status_ok) {
    std::snprintf(st.status_class, sizeof(st.status_class), "%s", game::status_class());
    st.save_count = game::save_count();
    st.playtime_raw = game::playtime_raw();
    st.playtime_rate = game::playtime_rate();
  }

  // Patches are independent of a session: toggle them whenever asked.
  toggle_patch(game::kSiteSaveCountInc, on(kNoSaveCount), &st.savecount_patched);
  {
    // The take-back rides on the increment: skipped on its own, it would leave
    // every cancelled save counted.
    bool back = false;
    toggle_patch(game::kSiteSaveCountBack, on(kNoSaveCount) && st.savecount_patched, &back);
  }
  if (st.site_ink) {
    bool a = false, b = false;
    toggle_patch(game::kSiteInkTakeA, on(kInfiniteInk), &a);
    toggle_patch(game::kSiteInkTakeB, on(kInfiniteInk), &b);
    st.ink_patched = a && b;
  }
  if (st.site_ink_check) toggle_patch(game::kSiteInkCheck, on(kInfiniteInk), &st.ink_check_patched);
  const bool freeze = on(kFreezePlaytime);
  if (st.site_playtime) {
    toggle_patch(game::kSitePlaytimeStore, freeze, &st.playtime_patched);
    g_hold_active = false;
  }
  const bool freeze_cd = on(kFreezeCountdown);
  if (st.site_countdown) {
    bool a = false, b = false;
    toggle_patch(game::kSiteCountdownStepA, freeze_cd, &a);
    toggle_patch(game::kSiteCountdownStepB, freeze_cd, &b);
    st.countdown_patched = a && b;
    g_countdown_hold_active = false;
  }
  st.countdown_ready = game::countdown_ready();
  st.countdown_active = game::countdown_active();
  st.countdown_raw = game::countdown_raw();
  st.countdown_rate = game::countdown_rate();

  if (!in_game) {
    g_status.ammo_held = 0;
    g_status.ink_tracked = false;
    std::memset(g_ammo_valid, 0, sizeof(g_ammo_valid));
    g_ink_slot[0] = g_ink_slot[1] = -1;
    g_god_logged[0] = g_god_logged[1] = false;
    g_countdown_hold_active = false;
    st.ammo_held = 0;
    st.ink_tracked = false;
    publish(st);
    return;
  }

  // Requests first, so the readings below reflect them.
  Request reqs[32];
  const int n = drain(reqs, 32);
  for (int i = 0; i < n; ++i) {
    const Request& r = reqs[i];
    switch (r.type) {
      case Request::kSaveCount:
        if (game::set_save_count(r.a)) note("save count set to %d", r.a);
        else note("save count not set: status object unknown");
        break;
      case Request::kPlaytime: {
        const float rate = game::playtime_rate();
        if (rate > 0.0f && game::set_playtime_raw(static_cast<float>(r.a) * rate)) {
          g_frozen_raw = static_cast<float>(r.a) * rate;
          note("play time set to %d:%02d:%02d", r.a / 3600, (r.a / 60) % 60, r.a % 60);
        } else {
          note("play time not set: clock not identified yet (play a few seconds unpaused)");
        }
        break;
      }
      case Request::kCountdown: {
        // Only a timer the game itself started can be edited: writing the field
        // when nothing is counting down would put a timer on screen that no
        // script is going to take away again.
        const float raw = r.f * game::countdown_rate();
        const int mins = static_cast<int>(r.f) / 60;
        if (!game::countdown_active()) {
          note("countdown not set: no timer is running");
        } else if (game::set_countdown_raw(raw)) {
          g_frozen_countdown = raw;
          note("countdown set to %d:%05.2f", mins, r.f - static_cast<float>(mins * 60));
        } else {
          note("countdown not set: %.0f s is out of range, or sEventScript is unavailable", r.f);
        }
        break;
      }
      case Request::kSlot: apply_slot(r.a, r.b, r.c, r.d); break;
      case Request::kEquipped:
        if (game::write_equipped(r.a, r.b)) note("%s equipped slot -> %d", r.a == 0 ? "Rebecca" : "Billy", r.b + 1);
        break;
    }
  }

  st.nchars = game::characters(st.chars);
  st.bags_ok = game::bags(st.bags);
  st.status_ok = game::status_ready();
  if (st.status_ok) {
    st.save_count = game::save_count();
    st.playtime_raw = game::playtime_raw();
    st.playtime_rate = game::playtime_rate();
  }

  if (on(kGodMode)) hold_god(st.nchars, st.chars);
  else g_god_logged[0] = g_god_logged[1] = false;

  st.enemy_table = game::enemy_table_known();
  if (on(kOneHitKills) && st.enemy_table) {
    st.enemies = hold_enemies();
  } else {
    game::Enemy list[128];
    st.enemies = st.enemy_table ? game::enemies(list, 128) : 0;
    g_ohk_logged = g_ohk_pool_logged = 0;
  }

  if (on(kInfiniteAmmo)) {
    hold_ammo(st.bags, st.bags_ok);
    st.ammo_held = g_status.ammo_held;
  } else {
    std::memset(g_ammo_valid, 0, sizeof(g_ammo_valid));
    st.ammo_held = 0;
  }
  if (on(kInfiniteInk) && !st.site_ink) {  // no take sites: put ribbons back instead
    hold_ink(st.bags, st.bags_ok);
    st.ink_tracked = g_status.ink_tracked;
    st.ink_restored = g_status.ink_restored;
  } else {
    g_ink_slot[0] = g_ink_slot[1] = -1;
    st.ink_tracked = false;
  }

  // Freeze without a patch site: hold the value (write only on drift; re-base
  // on a jump backwards, which is a load).
  if (!st.site_playtime && st.status_ok) {
    if (freeze) {
      const float raw = game::playtime_raw();
      if (!g_hold_active || g_frozen_raw < 0.0f) {
        g_hold_active = true;
        g_frozen_raw = raw;
        logf("play time frozen (hold) at %.1f raw units", raw);
      } else if (raw < g_frozen_raw - 2.0f * st.playtime_rate) {
        g_frozen_raw = raw;  // a load moved it: hold the new value
      } else if (raw > g_frozen_raw) {
        game::set_playtime_raw(g_frozen_raw);
      }
      st.playtime_hold = true;
      st.playtime_raw = g_frozen_raw;
    } else if (g_hold_active) {
      g_hold_active = false;
      logf("play time released at %.1f raw units", game::playtime_raw());
    }
  }

  // The same fallback for the countdown, and the same rule as the patch: the
  // freeze belongs to the timed section that is running, not to the field. It
  // lets go the moment the engine's -1 sentinel comes back (the section ended)
  // and takes hold again at whatever the next section starts with.
  st.countdown_active = game::countdown_active();
  st.countdown_raw = game::countdown_raw();
  if (!st.site_countdown && st.countdown_ready) {
    if (freeze_cd && st.countdown_active) {
      const float raw = st.countdown_raw;
      const float rate = game::countdown_rate();
      // Between two ticks the engine can only have taken a frame's step off the
      // held value. Anything beyond that - in either direction, because the next
      // section's timer can be shorter as easily as longer - is a script setting
      // a new one, so the hold re-bases on it instead of dragging the last
      // section's value into this one.
      const bool restarted = g_countdown_hold_active &&
                             (raw > g_frozen_countdown + 0.5f || raw < g_frozen_countdown - 2.0f * rate);
      if (!g_countdown_hold_active || restarted) {
        g_countdown_hold_active = true;
        g_frozen_countdown = raw;
        logf("countdown %s (hold) at %.2f s", restarted ? "re-frozen for a new timer" : "frozen", raw / rate);
      } else if (raw < g_frozen_countdown) {
        game::set_countdown_raw(g_frozen_countdown);
      }
      st.countdown_hold = true;
      st.countdown_raw = g_frozen_countdown;
    } else if (g_countdown_hold_active) {
      g_countdown_hold_active = false;
      if (st.countdown_active) logf("countdown released at %.2f s", st.countdown_raw / game::countdown_rate());
      else logf("countdown released: the timed section ended");
    }
  }

  // The panel is a picture, not a decision. A reading that failed for a tick or
  // two - sItem unavailable through a load, a chapter change, a cutscene - is
  // not an inventory that emptied, and blanking it there is what made an item
  // look like it had vanished. Carry the last good reading into the copy the
  // panel gets, but only here: this is after the holds, which ran on the live
  // reading and must never act on a stale one. About a second, then let it go,
  // so a bag that really is gone is not drawn for ever.
  if (st.bags_ok) {
    g_bags_carried = 0;
  } else if (g_status.bags_ok && g_bags_carried < 60) {
    if (g_bags_carried == 0) logf("bags: reading unavailable - the panel keeps showing the last good one");
    ++g_bags_carried;
    st.bags[0] = g_status.bags[0];
    st.bags[1] = g_status.bags[1];
    st.bags_ok = true;
  } else if (g_bags_carried) {
    logf("bags: reading still unavailable after %d ticks - the panel stops showing the old one", g_bags_carried);
    g_bags_carried = 0;
  }
  publish(st);
}

void remove_hooks() {
  for (int s = 0; s < game::kSiteCount; ++s) {
    if (mem::Patch* p = game::site(static_cast<game::Site>(s)))
      if (p->applied) p->revert();
  }
}

}  // namespace re0cc::cheats
