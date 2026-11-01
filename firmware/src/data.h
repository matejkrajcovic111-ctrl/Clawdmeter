#pragma once
#include <stdint.h>

// Plain data, no Arduino types, so payload parsing and the signal logic can be
// unit-tested on the host (test/test_signals).

// Another configured account's numbers riding along in `o` (clawdmeter2:
// two accounts, swiped between on the device). Pro/Max fields only.
#define OTHER_ACCOUNTS_MAX 3
struct OtherAccount {
    int8_t index;              // `i` 1-based account number in the app's config order
    float  session_pct;        // `s`
    int    session_reset_mins; // `sr`, -1 = not started / unknown
    float  weekly_pct;         // `w`
    int    weekly_reset_mins;  // `wr`, -1 = unknown
    int8_t working;            // `wk` 1 = Claude is working in it now, 0 = not; -1 = absent
    int32_t turn_tokens;       // `wt` output tokens of its current prompt; -1 = absent
    int32_t turn_secs;         // `wd` seconds since its latest prompt; -1 = absent
    int8_t thinking;           // `th` 1 = Claude is thinking in it (amber status line), 0 = not; -1 = absent
    int8_t offline;            // `off` 1 = nobody is logged into it (last reading, page dimmed); -1 = absent
};

struct UsageData {
    float session_pct;       // utilization 0-100 (5h window Pro/Max; spending % Enterprise)
    int session_reset_mins;  // minutes until reset
    float weekly_pct;        // 7-day utilization (Pro/Max only; 0 for Enterprise)
    int weekly_reset_mins;   // minutes until weekly reset (Pro/Max only)
    char status[16];         // "allowed", "limited", etc.
    bool chime;              // play the session-reset chime; false unless the app opts in
    bool beep;               // Claude needs attention: play the chime once for this packet
    char alert_msg[128];     // attention message for the status line; empty = no alert
    bool alert_clear;        // the session moved on: end the alert
    bool enterprise;         // true = Enterprise spending-limit account
    int time_pct;            // 0-100: fraction of billing period elapsed (Enterprise)
    int period_days;         // total billing period length in days (Enterprise)
    char reset_date[12];     // formatted reset date e.g. "Jul 1" (Enterprise)
    long clock_epoch;        // local wall-clock epoch (s) from the app; 0 = not provided
    int  clock_fmt;          // 12 or 24 (hour format from the app); defaults to 24
    // ---- Fast clock (Claude Code hooks via the app). All optional: an older
    // app sends none of them and the fields read as "absent" below, which
    // the signal logic treats exactly like "no hooks installed".
    int8_t   activity;       // `a`  0-3 tool-call activity level; -1 = absent
    char     event;          // `e`  latest hook event: 'p' prompt, 's' stop,
                             //      'b' back after a gap; '\0' = absent. Other
                             //      chars pass through for forward compat.
    bool     has_event_seq;  // `es` present
    uint32_t event_seq;      // `es` sequence number; react once per change
    int16_t  sessions;       // `n`  sessions with a hook event in the last 5 min; -1 = absent
    char     machines[32];   // `sm` sessions per machine, e.g. "pc 2 · proxmox 3"; empty = none
    bool     replay;         // `rp` 1 = the usage numbers repeat the last poll;
                             //      the rate math and reset detection skip it
    int8_t   liveliness;     // `lv` 1-5 (clamped); -1 = absent (keep the saved level)
    int16_t  attn_anim_s;    // `aa` attention animation, seconds 0-120 (0 = off); -1 = absent
    int8_t   pace_marker;    // `pm` 1 = pace marker on the usage bars, 0 = off; -1 = absent
    // ---- Accounts (clawdmeter2). Absent = a single-account app.
    int8_t   account_idx;    // `ai` 1-based account the usage fields above belong to; -1 = absent
    int8_t   account_count;  // `ac` accounts configured in the app; -1 = absent
    int8_t   working;        // `wk` 1 = Claude is working in the account above now, 0 = not; -1 = absent
    int32_t  turn_tokens;    // `wt` output tokens of that account's current prompt; -1 = absent
    int32_t  turn_secs;      // `wd` seconds since its latest prompt; -1 = absent
    char     tool[6];        // `tl` the tool Claude is running now ("sh", "ed", ...); "" = none
    int8_t   thinking;       // `th` 1 = Claude is thinking in that account, 0 = not; -1 = absent
    uint8_t  other_count;    // entries used in `others`
    OtherAccount others[OTHER_ACCOUNTS_MAX];   // `o` the other accounts' last readings
    bool ok;                // data parse succeeded
    bool valid;              // false until first successful parse
};
