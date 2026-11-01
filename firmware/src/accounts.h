#pragma once
#include "data.h"

// Several Claude accounts on one device (clawdmeter2). The app sends the
// active account's numbers in the usual fields, tagged `ai`, and the others
// in `o`. This keeps the last reading of each and which one is on screen;
// a swipe steps through them. Pure logic, no LVGL/Arduino (test_accounts).
//
// Until the user swipes, the screen follows the app's active account. After
// a swipe it stays on the chosen one (saved across reboots by the caller).

#define ACCOUNTS_MAX 9

void accounts_reset(void);
// An ok payload (replays included: their countdowns are current), or an
// {"ok":false} one that still carries `o` (every account logged out).
void accounts_ingest(const UsageData* d);
int  accounts_count(void);        // accounts the app has; <= 1 = single-account mode
int  accounts_active(void);       // the app's active account, 1-based; 0 = unknown
int  accounts_shown(void);        // the account on screen, 1-based (1 when unknown)
bool accounts_user_chose(void);   // the user picked `shown` with a swipe
void accounts_restore(int shown); // the saved choice from NVS; 0 = follow the app
// Swipe: +1 next, -1 previous, wrapping. Returns the new shown account, or 0
// when there is nothing to step to (one account).
int  accounts_step(int dir);
bool accounts_have(int idx);      // a reading for this account arrived
int  accounts_thinking(int idx);  // `th`: 1 Claude is thinking in it, 0 not, -1 not said
int  accounts_offline(int idx);   // `off`: 1 logged out (page dimmed), 0 live, -1 not said
int  accounts_working(int idx);   // `wk`: 1 Claude is working in it, 0 not, -1 not said
// `wt` / `wd`: output tokens of its current prompt and seconds since that
// prompt, as of the last payload; -1 = not said.
void accounts_turn(int idx, int32_t* tokens, int32_t* secs);
void accounts_follow_app(void);   // drop a swipe choice: show the app's active account again

// Use-next hint: the account you're on (the app's active one) is getting
// tight and another has clearly more room. "Used" is the tighter of session
// and weekly %. Returns that account (1-based), or 0 for no hint.
#define ACCOUNTS_HINT_FROM_PCT   75.0f   // before this, switching isn't worth mentioning
#define ACCOUNTS_HINT_MARGIN_PCT 20.0f   // the other must have this much more room
int  accounts_use_next(void);
// `latest` with its usage fields replaced by the shown account's reading.
// False when the shown account has no reading yet (out keeps `latest`).
bool accounts_view(const UsageData* latest, UsageData* out);
