#pragma once
#include <stdint.h>
#include <stddef.h>

// Per-account usage history, sent by the desktop app as its own BLE message:
//   {"h":1,"i":2,"d":[30 ints],"wt":1834000,"j":"joke","hd":[24 ints, optional]}
// Pure logic (no LVGL/Arduino) so it stays testable on the host.
#define HISTORY_DAYS 30
#define HISTORY_HOURS 24

struct AccountHistory {
    bool     have;
    uint32_t days_k[HISTORY_DAYS];   // billed tokens per day in thousands, oldest first
    uint64_t week_tokens;            // tokens burned in the last 7 days
    char     joke[48];
    bool     has_hours;              // optional "hd" was present
    uint32_t hours_h[HISTORY_HOURS]; // today's tokens per local hour, in hundreds
};

// Cheap top-level-key check, run on every BLE message before usage parsing.
bool history_is_message(const char* json);

// Parses a history message. account is 1..9. d is padded with zeros on the
// oldest side or truncated to its last 30 entries.
bool history_parse(const char* json, int* account, AccountHistory* out);

// Static storage for accounts 1..9. get returns NULL when nothing is stored.
void history_store(int account, const AccountHistory* h);
const AccountHistory* history_get(int account);
// Today's per-hour tokens (hundreds, 24 entries, hour 0 first); NULL when the
// account has no history or the message carried no "hd".
const uint32_t* history_hours_get(int account);

// 950 -> "950", 12400 -> "12.4K", 1.8M, 2.3B: one decimal, ".0" dropped.
void history_format_tokens(uint64_t n, char* buf, size_t len);
