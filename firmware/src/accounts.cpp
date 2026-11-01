#include "accounts.h"
#include <string.h>

struct Snapshot {
    bool  have;
    float session_pct;
    int   session_reset_mins;
    float weekly_pct;
    int   weekly_reset_mins;
    int8_t working;  // `wk`: 1 = Claude is working in it now, 0 = not, -1 = not said
    int32_t turn_tokens, turn_secs;   // `wt`, `wd`; -1 = not said
    int8_t thinking;                  // `th`: 1 thinking, 0 not, -1 not said
    int8_t offline;                   // `off`: 1 logged out, 0 live, -1 not said
};

static Snapshot snaps[ACCOUNTS_MAX + 1];   // 1-based
static int  count = 0;
static int  active = 0;
static int  chosen = 0;                    // 0 = follow the app

void accounts_reset(void) {
    memset(snaps, 0, sizeof(snaps));
    for (Snapshot& s : snaps) { s.working = -1; s.turn_tokens = -1; s.turn_secs = -1; s.thinking = -1; s.offline = -1; }
    count = 0;
    active = 0;
    chosen = 0;
}

static bool valid_idx(int i) { return i >= 1 && i <= ACCOUNTS_MAX; }

void accounts_ingest(const UsageData* d) {
    if (!d->valid) return;
    if (d->ok) {
        if (d->account_idx < 1) return;
        if (d->account_count > 0) count = d->account_count;
        if (d->account_idx > count) count = d->account_idx;
        active = d->account_idx;
        if (!d->enterprise && valid_idx(active)) {
            snaps[active] = { true, d->session_pct, d->session_reset_mins, d->weekly_pct, d->weekly_reset_mins,
                              d->working, d->turn_tokens, d->turn_secs, d->thinking, 0 };
        } else if (valid_idx(active)) {
            snaps[active].working = d->working;
            snaps[active].turn_tokens = d->turn_tokens;
            snaps[active].turn_secs = d->turn_secs;
            snaps[active].thinking = d->thinking;
            snaps[active].offline = 0;
        }
    } else {
        // {"ok":false,"ac":N,"o":[...]}: every account is logged out. There is
        // no top-level account, but the `o` pages still ride along. A bare
        // {"ok":false} carries no account info and changes nothing.
        if (d->other_count == 0) return;
        if (d->account_count > 0) count = d->account_count;
        active = 0;
    }
    for (int k = 0; k < d->other_count; k++) {
        const OtherAccount& o = d->others[k];
        if (!valid_idx(o.index) || o.index == active) continue;
        if (o.index > count) count = o.index;
        Snapshot& s = snaps[o.index];
        // w < 0: the app has no weekly reading for it; keep the last one.
        const float w = o.weekly_pct >= 0 ? o.weekly_pct : (s.have ? s.weekly_pct : 0.0f);
        const int wr = o.weekly_pct >= 0 ? o.weekly_reset_mins : (s.have ? s.weekly_reset_mins : -1);
        s = { true, o.session_pct, o.session_reset_mins, w, wr, o.working, o.turn_tokens, o.turn_secs, o.thinking, o.offline };
    }
    if (chosen > count) chosen = 0;
}

int accounts_count(void) { return count; }
int accounts_active(void) { return active; }

int accounts_shown(void) {
    if (chosen > 0) return chosen;
    return active > 0 ? active : 1;
}

bool accounts_user_chose(void) { return chosen > 0; }

void accounts_follow_app(void) { chosen = 0; }

void accounts_restore(int shown) { chosen = valid_idx(shown) ? shown : 0; }

int accounts_step(int dir) {
    if (count < 2) return 0;
    int next = accounts_shown() + (dir >= 0 ? 1 : -1);
    if (next > count) next = 1;
    if (next < 1) next = count;
    chosen = next;
    return next;
}

bool accounts_have(int idx) { return valid_idx(idx) && snaps[idx].have; }

int accounts_working(int idx) {
    // Nothing received for it yet (snaps start zeroed at boot): not said.
    return valid_idx(idx) && (snaps[idx].have || idx == active) ? snaps[idx].working : -1;
}

int accounts_offline(int idx) {
    return valid_idx(idx) && (snaps[idx].have || idx == active) ? snaps[idx].offline : -1;
}

int accounts_thinking(int idx) {
    return valid_idx(idx) && (snaps[idx].have || idx == active) ? snaps[idx].thinking : -1;
}

void accounts_turn(int idx, int32_t* tokens, int32_t* secs) {
    const bool known = valid_idx(idx) && (snaps[idx].have || idx == active);
    *tokens = known ? snaps[idx].turn_tokens : -1;
    *secs = known ? snaps[idx].turn_secs : -1;
}

// The tighter of the two limits is what stops you.
static float used(const Snapshot& s) { return s.session_pct > s.weekly_pct ? s.session_pct : s.weekly_pct; }

int accounts_use_next(void) {
    if (count < 2 || !accounts_have(active)) return 0;
    const float mine = used(snaps[active]);
    if (mine < ACCOUNTS_HINT_FROM_PCT) return 0;
    int best = 0;
    float best_used = mine - ACCOUNTS_HINT_MARGIN_PCT;   // must beat this to be worth a switch
    for (int i = 1; i <= count; i++) {
        if (i == active || !snaps[i].have) continue;
        const float u = used(snaps[i]);
        if (u < best_used) { best = i; best_used = u; }
    }
    return best;
}

bool accounts_view(const UsageData* latest, UsageData* out) {
    *out = *latest;
    const int idx = accounts_shown();
    // Single-account apps (no `ai`) and the active account: as received.
    if (!latest->ok) {
        // All logged out: only a logged-out account's last reading may be
        // shown (dimmed). Anything else stays "no data", never zeros.
        if (!accounts_have(idx) || snaps[idx].offline != 1) return true;   // out = latest, ok:false
        out->ok = true;
    } else if (latest->account_idx < 1 || idx == latest->account_idx) {
        return true;
    } else if (!accounts_have(idx)) {
        return false;
    }
    const Snapshot& s = snaps[idx];
    out->session_pct = s.session_pct;
    out->session_reset_mins = s.session_reset_mins;
    out->weekly_pct = s.weekly_pct;
    out->weekly_reset_mins = s.weekly_reset_mins;
    out->enterprise = false;
    out->account_idx = (int8_t)idx;
    return true;
}
