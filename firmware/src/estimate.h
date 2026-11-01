#pragma once
#include <stdint.h>

// Session % estimate between real readings. The device gets a reading every
// few minutes; between them the bar would freeze. This keeps the last reading
// and lets the bar creep forward at the current rate (usage_rate.h), and
// estimates when the 5-hour session runs out. "No data" is never a zero:
// functions return <0 / -1 when there is nothing to say. Pure logic, no
// Arduino/LVGL (test_estimate). Times are millis()-style uint32 and wrap-safe.

// ─── Tunables ────────────────────────────────────────────────────────────────
#define EST_MAX_AHEAD   5.0f                    // points: an estimate is a hint, so it never runs far
                                                //   ahead of what the app really reported
#define EST_MAX_AGE_MS  (10UL * 60UL * 1000UL)  // older than this the link is probably down: do not invent progress
#define EST_MIN_RATE    2.0f                    // %/h: below this a run-out is hours away and just noise

struct Estimate {
    bool     has;          // a real reading has landed
    uint32_t at;           // when (ms)
    float    pct;          // session % of that reading
    bool     has_reset;    // reset time known
    uint32_t reset_at;     // absolute ms deadline of the window reset
};

void  estimate_reset(Estimate* e);

// A real reading landed (not a replay). reset_mins: minutes until the 5h window
// resets, <0 unknown / not started.
void  estimate_reading(Estimate* e, uint32_t now, float session_pct, int reset_mins);

// Session % to display now, or <0 when there is no reading yet. Frozen at the
// reading when rate <= 0, the reading is stale or the reset deadline passed.
float estimate_session_pct(const Estimate* e, uint32_t now, float rate_pct_per_hour);

// Minutes until 100 % at the current rate; -1 when unknown, the rate is below
// EST_MIN_RATE, the estimate is frozen, or it would not run out before the reset.
int   estimate_runout_mins(const Estimate* e, uint32_t now, float rate_pct_per_hour);

// "out in 25m", "out in 1h40m", "out in 2h"; "" when mins < 0. buf >= 16.
void  estimate_runout_label(int mins, char* buf, int len);
