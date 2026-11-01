#pragma once
#include "data.h"
#include "ble.h"

// The meters room: clock, the two usage cards, the status line, battery and
// the corner Clawd's seat. Rooms, transitions and attention sequencing belong
// to the director (clawd_director.h); ui only draws the meters.

void ui_init(void);
void ui_update(const UsageData* data);
// Pace marker on the Pro/Max usage bars (`pm`, pace.h); redraws the last data.
void ui_set_pace_marker(bool on);
void ui_tick_anim(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging, bool usb_power);  // percent < 0 = no battery

// True while the meters show live numbers (connected, data fresh) rather than
// the pairing hint or the stale-data idle view.
bool ui_meters_live(void);

// The pairing hint (shown while disconnected) is on screen.
bool ui_pair_hint_visible(void);

enum PairHint {
    PAIR_HINT_NORMAL,    // how to pair on this board
    PAIR_HINT_HOLDING,   // a touch hold toward clearing the pairing is running
    PAIR_HINT_CLEARED,   // bonds were just cleared: ready to pair (reverts by itself)
};
void ui_set_pair_hint(PairHint hint);

// Attention message on the status line (scrolls when long); NULL clears it
// and the whimsical status line resumes.
void ui_set_alert_text(const char* message);

// A standing notice on the status line (below alerts, above the whimsy),
// e.g. the open pairing window; NULL clears it.
void ui_set_notice(const char* text);

// ---- Accounts (clawdmeter2) ----
// Which account the meters show (1-based) out of `count`. Account 1 keeps
// the original palette, account 2 is blue (Clawd too); with two or more accounts
// a "#N" badge sits in the bottom-right corner.
void ui_set_account(int shown, int count);
// Is Claude working in the shown account (1/0; -1 = the app doesn't say)?
// Working, the status line spins and shimmers like Claude Code's; idle, it
// holds still and says how long the last stretch took.
void ui_set_working(int working);
// Is nobody logged into the shown account (`off`)? Its last reading is then
// drawn in the dim colours and the status line says "Logged out". Call before
// ui_show / ui_update: the bars take their colour from that render.
void ui_set_offline(bool offline);
// Is Claude thinking in the shown account (1/0; -1 not said)? While working
// and thinking the status line turns amber, as Claude Code's spinner does.
void ui_set_thinking(int thinking);
// clawdmeter2: the tool Claude is running (`tl` code, "" = none): an icon
// after the status line's verb.
void ui_set_tool(const char* code);
// The shown account's current prompt: its output tokens and the seconds since
// it began (-1 = not said). Call after ui_set_working.
void ui_set_turn(int32_t tokens, int32_t secs);
// Redraw the meters from `view` (another account's numbers after a swipe)
// without counting as fresh data.
void ui_show(const UsageData* view);
int  ui_account_palette(void);   // 1 = original, 2 = neon (other modules tint to match)

// Short facts that take turns with the whimsical status verb while the
// numbers are live; "" or NULL clears a slot. Copied (<= 39 chars).
enum UiInfo { UI_INFO_RUNOUT, UI_INFO_USE_NEXT, UI_INFO_MACHINES, UI_INFO_COUNT };
void ui_set_info(UiInfo slot, const char* text);

// The local hour (0..23) from the app's wall clock (`t`), -1 when it sends none.
int  ui_local_hour(void);

// What a tap at (x, y) on the meters landed on.
// UI_HIT_CLAWD is his seat in the top-left corner; UI_HIT_TITLE the clock.
// UI_HIT_BATTERY is the battery icon in the top-right corner.
enum UiHit { UI_HIT_NONE, UI_HIT_TITLE, UI_HIT_SESSION, UI_HIT_WEEKLY, UI_HIT_CLAWD, UI_HIT_BATTERY };
UiHit ui_hit_test(int x, int y);
// The title shows today's date and year for a few seconds, then the clock.
void ui_show_date(void);
// The battery icon turns into its percentage for a few seconds (a second tap
// brings the icon straight back).
void ui_show_battery_pct(void);
// A swipe's transition: the cards slide in moving with the finger
// (+1 = the finger went right) and fade up.
void ui_slide_in(int finger_dir);
