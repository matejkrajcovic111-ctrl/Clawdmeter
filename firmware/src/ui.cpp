#include "ui.h"
#include "performer.h"
#include "mascot.h"
#include "account_tint.h"
#include "face.h"
#include "history.h"
#include "clawd_anim.h"
#include "tool_icons.h"
#include "party.h"
#include "idle.h"
#include "chime.h"
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
#include "clawd_policy.h"
#include "minimal_mode.h"
#include <lvgl.h>
#include <time.h>
#include <stdio.h>
#include <Arduino.h>
#include "logo.h"
#include "clawd_still.h"
#include "icons.h"
#include "hal/board_caps.h"
#include "layout.h"
#include "ui_text.h"
#include "pace.h"

// Where everything goes, from the panel's shape (layout.h). Computed once in
// ui_init() and treated as const for the rest of the program; a new panel
// size never needs an edit to the screen builders below.
static Layout L = {};

static_assert(LAYOUT_LOGO_SLOT == LOGO_HEIGHT && LAYOUT_LOGO_SLOT_SMALL == LOGO_SMALL_HEIGHT, "logo slot");
static_assert(LAYOUT_LOGO_W == CLAWD_STILL_W && LAYOUT_LOGO_W_SMALL == CLAWD_STILL_SMALL_W, "corner Clawd");
static_assert(LAYOUT_BATT_W == ICON_BATTERY_W && LAYOUT_BATT_W_SMALL == ICON_BATTERY_SMALL_W, "battery icon");

// Anthropic brand palette — design tokens live in theme.h. Account 2 swaps
// the whole set for the neon one (ui_set_account); the COL_* names read the
// palette in force, and apply_palette() restyles what's already built.
#include "theme.h"
struct Palette {
    lv_color_t panel, text, dim, accent, green, amber, red, bar_bg;
    const char *green_hex, *amber_hex, *red_hex, *text_hex;   // for recoloured labels
};
static Palette pal_original() {
    return { THEME_PANEL, THEME_TEXT, THEME_DIM, THEME_ACCENT, THEME_GREEN, THEME_AMBER, THEME_RED,
             THEME_BAR_BG, "788c5d", "d97757", "c0392b", "faf9f5" };
}
static Palette pal_neon() {
    return { NEON_PANEL, NEON_TEXT, NEON_DIM, NEON_ACCENT, NEON_GREEN, NEON_AMBER, NEON_RED,
             NEON_BAR_BG, "4aa3ff", "ffb347", "ff3355", "dcebff" };
}
static Palette P = pal_original();
static int palette_idx = 1;
#define COL_BG        THEME_BG
#define COL_PANEL     P.panel
#define COL_TEXT      P.text
#define COL_DIM       P.dim
#define COL_ACCENT    P.accent
#define COL_GREEN     P.green
#define COL_AMBER     P.amber
#define COL_RED       P.red
#define COL_BAR_BG    P.bar_bg

// ---- Usage screen widgets (single non-splash view) ----
static lv_obj_t* usage_container;
static lv_obj_t* lbl_title;
static lv_obj_t* lbl_acct = nullptr;   // "#N" in the bottom-right corner, with two or more accounts
static int acct_shown = 1, acct_count = 1;
static void place_badge(void);
static int  status_room(void);
// clawdmeter2: is Claude working in the shown account (`wk`)? -1 = the app
// doesn't say. Working: the status line runs like Claude Code's spinner.
// Idle: it says how long the last stretch of work took.
static int8_t   working = -1;
// clawdmeter2: nobody is logged into the shown account (`off`): its last
// reading is on the cards in the palette's dim colours, status line "Logged out".
static bool     offline = false;
static bool     offline_line_up = false;   // "Logged out" is what the status line shows now
static uint32_t working_since = 0;    // lv_tick the current stretch began
static uint32_t worked_ms = 0;        // the last finished stretch; 0 = none seen
static int32_t  turn_tokens = -1;     // `wt`: this prompt's output tokens; -1 = not said
// Clock fed by the daemon: base epoch (local wall-clock seconds) + the lv_tick at
// which it landed, so the title ticks forward locally between 60s payloads.
static long     clock_base_epoch = 0;
static uint32_t clock_base_ms = 0;
static int      local_hour = -1;      // from the app's clock; -1 until it sends one

int ui_local_hour(void) { return local_hour; }
static int      clock_fmt = 24;   // 12 or 24, set from the daemon payload
static int      clock_last_min = -1;   // last rendered minute; avoids redrawing the title every tick
static uint32_t date_until = 0;        // clawdmeter2: the title shows the date until then (0 = clock)
#define DATE_SHOW_MS 4000
static lv_obj_t* usage_group;   // the two usage panels — shown when connected
static lv_obj_t* pair_group;    // pairing hint — shown when disconnected
static lv_obj_t* pair_lines[4]; // its lines: heading, two steps, the hold note
static PairHint  pair_hint = PAIR_HINT_NORMAL;
static uint32_t  pair_hint_since = 0;
#define PAIR_CLEARED_SHOW_MS 8000   // "Ready to pair" stays this long
static lv_obj_t* bar_session;
static lv_obj_t* lbl_session_pct;
static lv_obj_t* lbl_session_label;
static lv_obj_t* lbl_session_reset;
static lv_obj_t* bar_weekly;
static lv_obj_t* lbl_weekly_pct;
static lv_obj_t* lbl_weekly_label;
static lv_obj_t* lbl_weekly_reset;
static lv_obj_t* panel_session = nullptr;
static lv_obj_t* panel_weekly = nullptr;
// Enterprise-only widgets inside panel_session
static lv_obj_t* lbl_session_pct_sym = nullptr;  // "%" in smaller font
static lv_obj_t* lbl_spending_desc = nullptr;     // "of spending limit"
static lv_obj_t* lbl_spending_status = nullptr;   // "Under pace" / "On pace" / "Over pace"
static lv_obj_t* lbl_anim;      // status line: connection state + whimsical idle

// ---- Battery indicator (shared, on top) ----
static lv_obj_t* battery_img;
static lv_obj_t* logo_img;
static lv_image_dsc_t battery_dscs[6];
static lv_image_dsc_t battery_empty_red;   // clawdmeter2: the empty outline in red, <= 10 %  // empty, low, medium, full, charging, plug-zap (no battery)
// clawdmeter2: a tap on the icon shows the percentage in its place for a while.
static lv_obj_t* lbl_batt_pct = nullptr;
static uint32_t  batt_pct_until = 0;   // 0 = the icon is shown
static int       batt_last_pct = -1;
static bool      batt_last_chg = false;
static bool      batt_last_usb = false;
#define BATT_PCT_SHOW_MS 4000
static void init_battery_icons_white(void);
#define BATT_GREEN 0x2BFF5E   // the charging bar: a bright, vivid green
static void apply_battery_visibility(void);
// clawdmeter2: burn-in guard. Everything that never moves (the meters, the
// clock, the badge, the battery) sits a few px off its spot, a new spot every
// SHIFT_EVERY_MS, walking a small ring so each pixel takes turns.
#define SHIFT_EVERY_MS (3u * 60u * 1000u)
static int8_t   shift_x = 0, shift_y = 0;
static void batt_shift_parts(void);   // the charging fill / pulse / bolt (defined with them)
static void batt_icon_pop(void);
static void batt_charge_init(lv_obj_t* scr);

// ---- Live-data freshness → which usage sub-view to show ----
// Live meters when data is flowing; the same meters dimmed when the host is
// connected but no usage update landed within DATA_FRESH_MS (stale: the last
// numbers stay, visibly not live, and the status line says so); the pairing
// hint when BLE is down. The corner Clawd stays in his seat in all three:
// there is only ever one Clawd. Re-evaluated every loop in ui_tick_anim().
#define STALE_OPA LV_OPA_40
static uint32_t  last_data_ms = 0;      // lv_tick when the last valid usage update landed
static bool      data_received = false; // any valid update since boot
static bool      data_ok = true;        // last payload's ok flag; a {"ok":false} beat = "no fresh data"
static int       view_state = -1;       // -1 unknown / 0 pair / 1 stale / 2 live
static const uint32_t DATA_FRESH_MS = 90000;  // usage counts as "live" within this window (daemon sends ~60s)

// ---- Shared ----
static lv_image_dsc_t logo_dsc;
static bool     s_ble_connected = false;   // cached BLE connection state
static uint32_t connected_at_ms = 0;       // when we last entered CONNECTED ("Connected" dwell)

// Animation state
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;    // counts verb changes (the stale line alternates on it)
static uint8_t anim_verb_idx = 0;   // the verb shown: the next one that fits the line
static uint32_t anim_msg_start = 0;
static const char* frozen_shown = nullptr;   // spinner held still: the word on screen

static const char* const spinner_frames[] = {
    "\xC2\xB7", "\xE2\x9C\xBB", "\xE2\x9C\xBD",
    "\xE2\x9C\xB6", "\xE2\x9C\xB3", "\xE2\x9C\xA2",
};
#define SPINNER_COUNT 6
#define SPINNER_PHASES (2 * (SPINNER_COUNT - 1))  // 10: ping-pong 0..5..0

static const uint16_t spinner_ms[SPINNER_COUNT] = {
    260, 130, 130, 130, 130, 260,
};

static const char* const anim_messages[] = {
    "Accomplishing", "Elucidating", "Perusing",
    "Actioning", "Enchanting", "Philosophising",
    "Actualizing", "Envisioning", "Pondering",
    "Baking", "Finagling", "Pontificating",
    "Booping", "Flibbertigibbeting", "Processing",
    "Brewing", "Forging", "Puttering",
    "Calculating", "Forming", "Puzzling",
    "Cerebrating", "Frolicking", "Reticulating",
    "Channelling", "Generating", "Ruminating",
    "Churning", "Germinating", "Scheming",
    "Clauding", "Hatching", "Schlepping",
    "Coalescing", "Herding", "Shimmying",
    "Cogitating", "Honking", "Shucking",
    "Combobulating", "Hustling", "Simmering",
    "Computing", "Ideating", "Smooshing",
    "Concocting", "Imagining", "Spelunking",
    "Conjuring", "Incubating", "Spinning",
    "Considering", "Inferring", "Stewing",
    "Contemplating", "Jiving", "Sussing",
    "Cooking", "Manifesting", "Synthesizing",
    "Crafting", "Marinating", "Thinking",
    "Creating", "Meandering", "Tinkering",
    "Crunching", "Moseying", "Transmuting",
    "Deciphering", "Mulling", "Unfurling",
    "Deliberating", "Mustering", "Unravelling",
    "Determining", "Musing", "Vibing",
    "Discombobulating", "Noodling", "Wandering",
    "Divining", "Percolating", "Whirring",
    "Doing", "Wibbling",
    "Effecting", "Wizarding",
    "Working", "Wrangling",
};
#define ANIM_MSG_COUNT (sizeof(anim_messages) / sizeof(anim_messages[0]))

// The first verb from `i` on that fits the status line as "<glyph> <verb>…";
// a narrow panel skips the long ones rather than clipping them.
static uint8_t fitting_verb(uint8_t i) {
    for (uint8_t n = 0; n < ANIM_MSG_COUNT; n++, i = (i + 1) % ANIM_MSG_COUNT) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6", spinner_frames[1], anim_messages[i]);
        if (layout_text_width(L.status_font, buf) <= status_room()) return i;
    }
    return i;
}

// ---- clawdmeter2: the status line while Claude works, like Claude Code's ----
// The glyph spins, a lighter shimmer sweeps across the verb, and the time
// spent so far follows in grey. Idle, it says how long the last stretch took
// with a past-tense verb ("Brewed for 1m 23s"). Drawn with LVGL's recolor
// markup ("#rrggbb text#"), switched on only for these lines: other lines
// (facts, alerts) can hold a literal '#'.
#define SHIMMER_STEP_MS 80    // the shimmer moves one letter per step
#define SHIMMER_TAIL    4     // steps it rests past the end before coming round
#define STATS_EVERY_MS  15000 // the numbers take over the line this often...
#define STATS_SHOW_MS   4000  // ...for this long
static uint32_t spin_last_ms = 0;
static uint8_t  past_idx = 0;
static const char* const past_verbs[] = {
    "Worked", "Baked", "Brewed", "Churned", "Cooked", "Crunched",
    "Mulled", "Pondered", "Simmered", "Stewed", "Noodled", "Whirred",
};
#define PAST_COUNT (sizeof(past_verbs) / sizeof(past_verbs[0]))

static void format_span(uint32_t ms, char* out, size_t n) {
    const uint32_t s = ms / 1000;
    if (s < 60)        snprintf(out, n, "%us", (unsigned)s);
    else if (s < 3600) snprintf(out, n, "%um %us", (unsigned)(s / 60), (unsigned)(s % 60));
    else               snprintf(out, n, "%uh %um", (unsigned)(s / 3600), (unsigned)(s % 3600 / 60));
}

static void hex_of(lv_color_t c, char out[7]) {
    snprintf(out, 7, "%02x%02x%02x", c.red, c.green, c.blue);
}

// In LVGL's lv_label.c, though no header of this version declares it.
extern "C" void lv_label_set_recolor(lv_obj_t* obj, bool en);
static bool status_recolor = false;
static char idle_shown[64];   // the idle line on screen; "" = something else is

// clawdmeter2: the tool Claude is running (`tl`), as an icon after the verb
// (helpers after status_room()).
static char cur_tool[6] = "";
struct ToolIcon;
static const ToolIcon* tool_icon_for(const char* code);
static void tool_icon_off(void);
static void tool_icon_on(const ToolIcon* ic, int plain_w, lv_color_t c);

static void set_status(const char* text, bool recolor) {
    tool_icon_off();
    idle_shown[0] = 0;
    offline_line_up = false;
    if (status_recolor != recolor) {
        lv_label_set_recolor(lbl_anim, recolor);
        status_recolor = recolor;
    }
    lv_label_set_text(lbl_anim, text);
}

// Claude Code's spinner colours: the verb and glyph in its "claude" orange,
// the shimmer in its lighter "claudeShimmer". Account 2's blue gets the same
// treatment.
#define CC_SPINNER       0xD77757
#define CC_SHIMMER       0xEB9F7F
#define CC_SPINNER_BLUE  0x4AA3FF
#define CC_SHIMMER_BLUE  0x9CCBFF
// Thinking: Claude Code's dark-theme "warning" amber and its shimmer.
#define CC_THINK         0xFFC107
#define CC_THINK_SHIMMER 0xFFDF39
#define THINK_FADE_MS    600   // orange <-> amber fade
static int8_t thinking = -1;
void ui_set_thinking(int t) { thinking = t < 0 ? -1 : t > 0 ? 1 : 0; }

// "<glyph> <Verb>…", the verb with a shimmer at `now`.
static void draw_working_line(const char* glyph, const char* verb, uint32_t now) {
    static char buf[420];
    char base[7], mid[7], hi[7], dim[7], gc[7];
    // clawdmeter2: thinking and not fade into each other (THINK_FADE_MS)
    // instead of flipping colour from one frame to the next.
    static uint16_t think_mix = 0;     // 0 = the account colour, 255 = thinking amber
    static uint32_t think_mix_ms = 0;
    const uint16_t want = thinking == 1 ? 255 : 0;
    const uint32_t dt = now - think_mix_ms;
    think_mix_ms = now;
    if (think_mix != want) {
        const uint32_t step = dt >= THINK_FADE_MS ? 255 : dt * 255 / THINK_FADE_MS;
        if (want > think_mix) think_mix = think_mix + step >= want ? want : think_mix + step;
        else                  think_mix = think_mix <= want + step ? want : think_mix - step;
    }
    const lv_color_t nb = lv_color_hex(g_account_tint ? CC_SPINNER_BLUE : CC_SPINNER);
    const lv_color_t ns = lv_color_hex(g_account_tint ? CC_SHIMMER_BLUE : CC_SHIMMER);
    const lv_color_t b = lv_color_mix(lv_color_hex(CC_THINK), nb, (uint8_t)think_mix);
    const lv_color_t s = lv_color_mix(lv_color_hex(CC_THINK_SHIMMER), ns, (uint8_t)think_mix);
    hex_of(b, base);
    hex_of(lv_color_mix(s, b, 128), mid);
    hex_of(s, hi);
    hex_of(COL_DIM, dim);
    hex_of(b, gc);

    // Mostly the verb; every STATS_EVERY_MS the line swaps to the numbers for
    // STATS_SHOW_MS: time on this stretch and today's tokens for the account.
    const uint32_t elapsed = now - working_since;
    if (elapsed >= STATS_EVERY_MS && elapsed % STATS_EVERY_MS < STATS_SHOW_MS) {
        char span[16], tok[16], plain[64];
        format_span(elapsed, span, sizeof(span));
        if (turn_tokens >= 0) {   // this prompt's tokens, as Claude Code counts them
            history_format_tokens((uint64_t)turn_tokens, tok, sizeof(tok));
            snprintf(plain, sizeof(plain), "%s %s \xC2\xB7 %s tokens", glyph, span, tok);
            if (layout_text_width(L.status_font, plain) > status_room())
                snprintf(plain, sizeof(plain), "%s %s \xC2\xB7 %s", glyph, span, tok);
            if (layout_text_width(L.status_font, plain) > status_room())
                snprintf(plain, sizeof(plain), "%s %s", glyph, tok);
        } else {
            snprintf(plain, sizeof(plain), "%s %s", glyph, span);
        }
        snprintf(buf, sizeof(buf), "#%s %s# #%s %s#", gc, glyph, dim, plain + strlen(glyph) + 1);
        set_status(buf, true);
        return;
    }

    const int len = (int)strlen(verb);
    const int pos = (int)((now / SHIMMER_STEP_MS) % (uint32_t)(len + SHIMMER_TAIL * 2)) - SHIMMER_TAIL;
    int o = snprintf(buf, sizeof(buf), "#%s %s# ", gc, glyph);
    const char* run = nullptr;
    for (int i = 0; i < len && o < (int)sizeof(buf) - 24; i++) {
        const int d = i > pos ? i - pos : pos - i;
        const char* c = d == 0 ? hi : d == 1 ? mid : base;
        if (c != run) {
            if (run) o += snprintf(buf + o, sizeof(buf) - o, "#");
            o += snprintf(buf + o, sizeof(buf) - o, "#%s ", c);
            run = c;
        }
        buf[o++] = verb[i];
    }
    o += snprintf(buf + o, sizeof(buf) - o, "#");
    snprintf(buf + o, sizeof(buf) - o, "#%s \xE2\x80\xA6#", base);
    set_status(buf, true);
    if (const ToolIcon* ic = tool_icon_for(cur_tool)) {
        char plain[64];
        snprintf(plain, sizeof(plain), "%s %s\xE2\x80\xA6", glyph, verb);
        tool_icon_on(ic, layout_text_width(L.status_font, plain), b);
    }
}

// Idle: "✻ Brewed for 1m 23s" (or "Idle" before any stretch), in grey.
// Returns false when the line is already up.
static bool draw_idle_line(void) {
    char buf[64];
    char dim[7];
    hex_of(COL_DIM, dim);
    if (worked_ms >= 1000) {
        // The longest form that fits beside the #N badge: "✻ Churned for
        // 1h 2m 3s", then without "for", then just the time.
        char span[16], plain[48];
        format_span(worked_ms, span, sizeof(span));
        const char* verb = past_verbs[past_idx];
        snprintf(plain, sizeof(plain), "\xE2\x9C\xBB %s for %s", verb, span);
        if (layout_text_width(L.status_font, plain) > status_room())
            snprintf(plain, sizeof(plain), "\xE2\x9C\xBB %s %s", verb, span);
        if (layout_text_width(L.status_font, plain) > status_room())
            snprintf(plain, sizeof(plain), "\xE2\x9C\xBB %s", span);
        snprintf(buf, sizeof(buf), "#%s %s#", dim, plain);
    } else {
        snprintf(buf, sizeof(buf), "#%s \xE2\x9C\xBB Idle#", dim);
    }
    if (strcmp(buf, idle_shown) == 0) return false;
    set_status(buf, true);
    snprintf(idle_shown, sizeof(idle_shown), "%s", buf);
    return true;
}

static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

// ---- Pace marker (device setting `pm`, pace.h). Off: the bars use
// pct_color(). On: each Pro/Max bar gets a thin ivory line at the elapsed
// share of its window and the fill follows pace_level(). A bar with no reset
// time hides its line and keeps pct_color(). Spend-capped accounts are
// untouched: their Period bar already is the elapsed share.
static bool pace_marker = false;
static lv_obj_t* tick_session = nullptr;
static lv_obj_t* tick_weekly  = nullptr;
static UsageData last_data;                 // recoloured when the setting changes
static bool have_last_data = false;

static lv_color_t bar_color(float used_pct, int reset_mins, int window_mins) {
    if (pace_marker) {
        switch (pace_level(used_pct, pace_elapsed_pct(reset_mins, window_mins))) {
        case PACE_LIMIT: return COL_RED;
        case PACE_AHEAD: return COL_AMBER;
        case PACE_ON:    return COL_GREEN;
        case PACE_UNKNOWN: break;
        }
    }
    return pct_color(used_pct);
}

// A line inside the bar, exactly its height, drawn over the fill.
static lv_obj_t* make_pace_tick(lv_obj_t* bar) {
    lv_obj_t* tick = lv_obj_create(bar);
    lv_obj_remove_style_all(tick);
    lv_obj_set_size(tick, L.bar_h >= 20 ? 3 : 2, L.bar_h);
    lv_obj_set_style_bg_color(tick, COL_TEXT, 0);
    lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tick, 1, 0);
    lv_obj_clear_flag(tick, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(tick, LV_OBJ_FLAG_HIDDEN);
    return tick;
}

// Centre the line on elapsed_pct, kept clear of the bar's rounded ends;
// hidden while the setting is off or the elapsed share is unknown.
static void place_pace_tick(lv_obj_t* tick, float elapsed_pct) {
    if (!pace_marker || elapsed_pct < 0) {
        lv_obj_add_flag(tick, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_t* bar = lv_obj_get_parent(tick);
    const int bar_w = lv_obj_get_width(bar);
    const int w = lv_obj_get_width(tick);
    const int inset = lv_obj_get_style_radius(bar, LV_PART_MAIN) / 2;
    int x = (int)(elapsed_pct * bar_w / 100.0f + 0.5f) - w / 2;
    if (x < inset) x = inset;
    if (x > bar_w - inset - w) x = bar_w - inset - w;
    lv_obj_set_x(tick, x);
    lv_obj_clear_flag(tick, LV_OBJ_FLAG_HIDDEN);
}

// `none`: what a missing reset time means (-1 in the payload): the session
// window hasn't started, the weekly one is unknown.
static void format_reset_time(int mins, char* buf, size_t len, const char* none) {
    if (mins < 0) {
        snprintf(buf, len, "%s", none);
    } else if (mins < 60) {
        snprintf(buf, len, "Resets in %dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "Resets in %dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "Resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, L.pad_x, 0);
    lv_obj_set_style_pad_right(panel, L.pad_x, 0);
    lv_obj_set_style_pad_top(panel, L.pad_y, 0);
    lv_obj_set_style_pad_bottom(panel, L.pad_y, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    return panel;
}

static lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    return bar;
}

static void init_icon_dsc_rgb565a8(lv_image_dsc_t* dsc, int w, int h, const uint8_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data = data;
    dsc->data_size = w * h * 3;
}

static lv_obj_t* make_pill(lv_obj_t* parent, const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, L.pill_font, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl, L.pill_pad_x, 0);
    lv_obj_set_style_pad_right(lbl, L.pill_pad_x, 0);
    lv_obj_set_style_pad_top(lbl, L.pill_pad_y, 0);
    lv_obj_set_style_pad_bottom(lbl, L.pill_pad_y, 0);
    return lbl;
}

// clawdmeter2: the level bars in colour (3 green, 2 orange, 1 red), the
// outline stays white. A bar pixel is one the empty battery doesn't have, so
// each level icon is copied to RAM once and just those pixels repainted.
#define BATT_ORANGE 0xFF9F1C
#define BATT_RED    0xFF453A
static void tint_battery_bars(lv_image_dsc_t* lvl, const lv_image_dsc_t* empty, uint32_t rgb) {
    const int n = lvl->header.w * lvl->header.h;
    if (empty->header.w != lvl->header.w || empty->header.h != lvl->header.h) return;
    uint8_t* buf = (uint8_t*)malloc(lvl->data_size);
    if (!buf) return;   // keep the white icon
    memcpy(buf, lvl->data, lvl->data_size);
    const uint8_t* a_lvl = buf + n * 2;
    const uint8_t* a_empty = empty->data + n * 2;
    const uint16_t c = lv_color_to_u16(lv_color_hex(rgb));
    uint16_t* px = (uint16_t*)buf;
    for (int i = 0; i < n; i++)
        if (a_lvl[i] > 0 && a_empty[i] < 128) px[i] = c;
    lvl->data = buf;
}

static void init_battery_icons(void) {
    init_battery_icons_white();
    tint_battery_bars(&battery_dscs[1], &battery_dscs[0], BATT_RED);
    tint_battery_bars(&battery_dscs[2], &battery_dscs[0], BATT_ORANGE);
    tint_battery_bars(&battery_dscs[3], &battery_dscs[0], BATT_GREEN);
    // The empty battery (<= 10 %): the whole outline in red.
    battery_empty_red = battery_dscs[0];
    uint8_t* buf = (uint8_t*)malloc(battery_dscs[0].data_size);
    if (buf) {
        const int n = battery_dscs[0].header.w * battery_dscs[0].header.h;
        memcpy(buf, battery_dscs[0].data, battery_dscs[0].data_size);
        const uint16_t c = lv_color_to_u16(lv_color_hex(BATT_RED));
        for (int i = 0; i < n; i++) ((uint16_t*)buf)[i] = c;
        battery_empty_red.data = buf;
    }
}

static void init_battery_icons_white(void) {
    if (L.small_icons) {
        init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_SMALL_W, ICON_BATTERY_SMALL_H, icon_battery_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_SMALL_W, ICON_BATTERY_LOW_SMALL_H, icon_battery_low_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_SMALL_W, ICON_BATTERY_MEDIUM_SMALL_H, icon_battery_medium_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_SMALL_W, ICON_BATTERY_FULL_SMALL_H, icon_battery_full_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_SMALL_W, ICON_BATTERY_CHARGING_SMALL_H, icon_battery_charging_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[5], ICON_PLUG_ZAP_SMALL_W, ICON_PLUG_ZAP_SMALL_H, icon_plug_zap_small_data);
        return;
    }
    init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_data);
    init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_W, ICON_BATTERY_LOW_H, icon_battery_low_data);
    init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_W, ICON_BATTERY_MEDIUM_H, icon_battery_medium_data);
    init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_W, ICON_BATTERY_FULL_H, icon_battery_full_data);
    init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_W, ICON_BATTERY_CHARGING_H, icon_battery_charging_data);
    init_icon_dsc_rgb565a8(&battery_dscs[5], ICON_PLUG_ZAP_W, ICON_PLUG_ZAP_H, icon_plug_zap_data);
}

// ======== Usage Screen ========

static lv_obj_t* make_usage_panel(lv_obj_t* parent, const LayoutRect& r, const char* pill_text,
                                  lv_obj_t** out_pct, lv_obj_t** out_pill,
                                  lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, r.x, r.y, r.w, r.h);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "--%");
    lv_obj_set_style_text_font(*out_pct, L.pct_font, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, L.bar_y, r.w - 2 * L.pad_x, L.bar_h);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, L.reset_font, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, L.reset_y);

    return panel;
}

// Pairing hint — shown when disconnected so the screen isn't empty and the
// user knows how to (re)pair. Boards with a PWR button name the 3-second
// release gesture (main.cpp pair_tick); boards without one (LCD-4) name the
// 5 s touch hold on this hint that clears an old pairing (pair_hold.h), and
// only those lines change while the hold runs and after a clear.
static void apply_pair_text(void) {
    if (!pair_lines[0]) return;
    const bool pwr = board_caps().has_pwr_button;
    const char* const* lines = pwr ? PAIR_TEXT_PWR : PAIR_TEXT_TOUCH;
    const char* note = "";
    // Only the touch hold talks back; the PWR hint stays as it is.
    if (!pwr && pair_hint == PAIR_HINT_HOLDING) note = PAIR_TEXT_HOLDING;
    else if (!pwr && pair_hint == PAIR_HINT_CLEARED) lines = PAIR_TEXT_CLEARED;
    for (int i = 0; i < 3; i++) lv_label_set_text(pair_lines[i], lines[i]);
    lv_label_set_text(pair_lines[3], note);
    lv_obj_set_style_text_color(pair_lines[3], pair_hint == PAIR_HINT_NORMAL ? COL_DIM : COL_ACCENT, 0);
}

// A centred line; on panels where the steps wrap, it wraps at pair_w.
static lv_obj_t* make_pair_line(int i, lv_color_t color) {
    lv_obj_t* l = lv_label_create(pair_group);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, L.pair_font[i], 0);
    lv_obj_set_style_text_color(l, color, 0);
    if (L.pair_wrap) {
        lv_obj_set_width(l, L.pair_w);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, L.pair_line_y[i]);
    return l;
}

static void build_pair_group(lv_obj_t* parent) {
    pair_group = lv_obj_create(parent);
    lv_obj_set_size(pair_group, L.w, L.h - L.pair_y);
    lv_obj_set_pos(pair_group, 0, L.pair_y);
    lv_obj_set_style_bg_opa(pair_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pair_group, 0, 0);
    lv_obj_set_style_pad_all(pair_group, 0, 0);
    lv_obj_clear_flag(pair_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    pair_lines[0] = make_pair_line(0, COL_TEXT);
    pair_lines[1] = make_pair_line(1, COL_DIM);
    pair_lines[2] = make_pair_line(2, COL_DIM);
    pair_lines[3] = make_pair_line(3, COL_DIM);
    apply_pair_text();

    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);  // ui_update_ble_status decides
}

bool ui_pair_hint_visible(void) { return view_state == 0; }

void ui_set_pair_hint(PairHint hint) {
    if (hint == pair_hint) return;
    pair_hint = hint;
    pair_hint_since = lv_tick_get();
    apply_pair_text();
}

// The title's font follows the clock format (a 12-hour clock may need a
// smaller one); hidden where the header has no room for it at all.
static void apply_title_font(void) {
    if (date_until && clock_base_epoch > 0) return;   // the date has the title for now
    const int fmt = clock_base_epoch > 0 && clock_fmt == 12 ? 1 : 0;
    const lv_font_t* f = L.title_font[fmt];
    if (!f) {
        lv_obj_add_flag(lbl_title, LV_OBJ_FLAG_HIDDEN);
        place_badge();
        return;
    }
    lv_obj_set_style_text_font(lbl_title, f, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, L.title_cx - L.w / 2, L.title_y[fmt]);
    lv_obj_clear_flag(lbl_title, LV_OBJ_FLAG_HIDDEN);
    place_badge();
}

// The "#N" badge just right of the title's baseline; hidden with one
// account, or where it would run into the battery.
static void place_badge(void) {
    if (!lbl_acct) return;
    if (acct_count < 2) {
        lv_obj_add_flag(lbl_acct, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    // Bottom-right, on the status line's centre line, at the cards' right edge
    // (or the status line's, on a round panel where the corner is cut off).
    lv_label_set_text_fmt(lbl_acct, "#%d", acct_shown);
    int right = L.card[0].x + L.card[0].w;
    if (L.w / 2 + L.status_w / 2 < right) right = L.w / 2 + L.status_w / 2;
    const int status_h = lv_font_get_line_height(L.status_font);
    const int lift = (status_h - lv_font_get_line_height(L.pill_font)) / 2;
    lv_obj_align(lbl_acct, LV_ALIGN_BOTTOM_RIGHT, right - L.w, L.status_y - (lift > 0 ? lift : 0));
    lv_obj_clear_flag(lbl_acct, LV_OBJ_FLAG_HIDDEN);
}

// The status line's usable width: with the badge up it keeps clear of it on
// both sides, so the centred text never runs under it.
static int status_room(void) {
    if (acct_count < 2) return L.status_w;
    const int badge = layout_text_width(L.pill_font, "#9") + (L.small_icons ? 4 : 10);
    return L.status_w - 2 * badge;
}

// clawdmeter2: the tool Claude is running (`tl`), as an icon after the verb.
static lv_obj_t* tool_img = nullptr;
static lv_image_dsc_t tool_dsc;
static const ToolIcon* tool_shown = nullptr;
static bool      tool_on = false;

void ui_set_tool(const char* code) {
    snprintf(cur_tool, sizeof(cur_tool), "%s", code ? code : "");
}

static const ToolIcon* tool_icon_for(const char* code) {
    if (!code || !code[0]) return nullptr;
    const ToolIcon* table = L.small_icons ? TOOL_ICONS_SMALL : TOOL_ICONS;
    const int n = (int)(sizeof(TOOL_ICONS) / sizeof(TOOL_ICONS[0]));
    for (int i = 0; i < n; i++)
        if (strcmp(table[i].code, code) == 0) return &table[i];
    return &table[n - 1];   // "ot"
}

// Hide the icon and put the line back in the middle.
static void tool_icon_off(void) {
    if (!tool_on) return;
    tool_on = false;
    if (tool_img) lv_obj_add_flag(tool_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, L.status_y);
}

// Show `ic` after the status text (`plain_w` px wide), tinted `c`; the pair
// stays centred. Skipped when it wouldn't fit beside the badges.
static void tool_icon_on(const ToolIcon* ic, int plain_w, lv_color_t c) {
    const int gap = L.small_icons ? 4 : 8;
    if (!ic || plain_w + gap + ic->w > status_room()) { tool_icon_off(); return; }
    if (!tool_img) {
        tool_img = lv_image_create(usage_container);
        lv_obj_clear_flag(tool_img, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_image_recolor_opa(tool_img, LV_OPA_COVER, 0);
    }
    if (ic != tool_shown) {
        init_icon_dsc_rgb565a8(&tool_dsc, ic->w, ic->h, ic->data);
        lv_image_set_src(tool_img, &tool_dsc);
        tool_shown = ic;
    }
    lv_obj_set_style_image_recolor(tool_img, c, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, -(gap + ic->w) / 2, L.status_y);
    lv_obj_update_layout(lbl_anim);
    lv_obj_align_to(tool_img, lbl_anim, LV_ALIGN_OUT_RIGHT_MID, gap, 0);
    lv_obj_clear_flag(tool_img, LV_OBJ_FLAG_HIDDEN);
    tool_on = true;
}

static void init_usage_screen(lv_obj_t* scr) {
    usage_container = lv_obj_create(scr);
    lv_obj_set_size(usage_container, L.w, L.h);
    lv_obj_set_pos(usage_container, 0, 0);
    lv_obj_set_style_bg_opa(usage_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_container, 0, 0);
    lv_obj_set_style_pad_all(usage_container, 0, 0);
    lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_SCROLLABLE);

    lbl_title = lv_label_create(usage_container);
    lv_label_set_text(lbl_title, "Usage");
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    apply_title_font();

    lbl_acct = lv_label_create(usage_container);
    lv_label_set_text(lbl_acct, "#1");
    lv_obj_set_style_text_font(lbl_acct, L.pill_font, 0);
    lv_obj_set_style_text_color(lbl_acct, COL_ACCENT, 0);
    lv_obj_add_flag(lbl_acct, LV_OBJ_FLAG_HIDDEN);

    // Usage panels (shown when connected) live in a transparent full-size group
    // so they can be toggled against the pairing hint as one unit.
    usage_group = lv_obj_create(usage_container);
    lv_obj_set_size(usage_group, L.w, L.h);
    lv_obj_set_pos(usage_group, 0, 0);
    lv_obj_set_style_bg_opa(usage_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_group, 0, 0);
    lv_obj_set_style_pad_all(usage_group, 0, 0);
    lv_obj_clear_flag(usage_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    panel_session = make_usage_panel(usage_group, L.card[0], L.pills[0],
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset);

    // Enterprise-only overlays inside panel_session — hidden until enterprise data arrives
    lbl_session_pct_sym = lv_label_create(panel_session);
    lv_label_set_text(lbl_session_pct_sym, "%");
    lv_obj_set_style_text_font(lbl_session_pct_sym, L.reset_font, 0);
    lv_obj_set_style_text_color(lbl_session_pct_sym, COL_TEXT, 0);
    lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_desc = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_desc, "of spending limit");
    lv_obj_set_style_text_font(lbl_spending_desc, L.reset_font, 0);
    lv_obj_set_style_text_color(lbl_spending_desc, COL_DIM, 0);
    lv_obj_set_pos(lbl_spending_desc, 0, L.reset_y);
    lv_obj_add_flag(lbl_spending_desc, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_status = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_status, "");
    lv_obj_set_style_text_font(lbl_spending_status, L.pace_font, 0);
    lv_obj_set_pos(lbl_spending_status, 0, L.reset_y + 20);
    lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);

    panel_weekly = make_usage_panel(usage_group, L.card[1], L.pills[1],
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset);
    // Recolor enabled so enterprise period box can color pace and reset separately
    lv_label_set_recolor(lbl_weekly_reset, true);

    tick_session = make_pace_tick(bar_session);
    tick_weekly  = make_pace_tick(bar_weekly);

    build_pair_group(usage_container);

    // Status line — always visible on the usage view. Driven by ui_tick_anim().
    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, L.status_font, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, L.status_y);
}

// ---- Attention message on the status line (the director sequences the rest).
static bool alert_active = false;
static const char* still_shown = nullptr;   // minimal mode: status word on screen
static const char* notice_text = nullptr;   // ui_set_notice
static const char* notice_shown = nullptr;

void ui_set_notice(const char* text) {
    notice_text = text;
    notice_shown = nullptr;
    // Back to the usual line: make every path redraw on the next tick.
    anim_last_ms = 0;
    still_shown = nullptr;
    frozen_shown = nullptr;
}

// clawdmeter2 facts that take turns with the whimsy (run-out, use-next, machines).
static char info_text[UI_INFO_COUNT][40];

void ui_set_info(UiInfo slot, const char* text) {
    if (slot < 0 || slot >= UI_INFO_COUNT) return;
    const char* t = text ? text : "";
    if (strcmp(info_text[slot], t) == 0) return;
    snprintf(info_text[slot], sizeof(info_text[slot]), "%s", t);
    // Same buffer, new words: the frozen-line check compares pointers.
    if (frozen_shown == info_text[slot]) frozen_shown = nullptr;
}

// ======== Public API ========

void ui_init(void) {
    const BoardCaps& caps = board_caps();
    L = layout_compute({ caps.width, caps.height, caps.round_panel });

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    // The screen is a fixed room, not a scroll view. Left scrollable (the
    // LVGL default), it grows a theme scrollbar (a faint 4 px grey bar ~6 px
    // above the bottom edge) whenever Clawd walks past an edge, e.g. on a
    // lurk trip or the screensaver's walk-off, and a drag could scroll it.
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    // Static corner mascot (see clawd_still.h): fallback when the animated
    // one can't get its buffer.
    if (L.small_icons) init_icon_dsc_rgb565a8(&logo_dsc, CLAWD_STILL_SMALL_W, CLAWD_STILL_SMALL_H, clawd_still_small_data);
    else               init_icon_dsc_rgb565a8(&logo_dsc, CLAWD_STILL_W, CLAWD_STILL_H, clawd_still_data);
    init_battery_icons();

    init_usage_screen(scr);

    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    lv_obj_set_pos(battery_img, L.batt_x, L.batt_y);
    // Boards without battery telemetry never show the indicator (per the HAL
    // contract).
    if (!board_caps().has_battery) {
        lv_obj_del(battery_img);
        battery_img = nullptr;
    } else {
        batt_charge_init(scr);
        lbl_batt_pct = lv_label_create(scr);
        lv_obj_set_style_text_font(lbl_batt_pct, L.pill_font, 0);
        lv_obj_add_flag(lbl_batt_pct, LV_OBJ_FLAG_HIDDEN);
    }

    // Corner mascot in the old logo slot. The still Clawd is shorter than the
    // 80/40 px slot the spark logo used; center it vertically in that slot.
    {
        const int slot  = L.small_icons ? LOGO_SMALL_HEIGHT : LOGO_HEIGHT;
        const int art_h = L.small_icons ? CLAWD_STILL_SMALL_H : CLAWD_STILL_H;
        const int top   = L.logo_y + (slot - art_h) / 2;
        const int cell  = L.small_icons ? 2 : 3;
        // His tallest act (jumping happy, the fresh-window joy) rises well
        // above the idle pose. Lower the seat just enough for it to clear the
        // top edge, without sitting on the first panel. Where the layout has
        // no room for that (240 px panels) the mascot swaps in a shorter
        // official act instead of clipping (mascot.cpp).
        const ClawdAnim* jump = clawd_anim("jumping happy");
        const int feet = jump ? corner_seat_feet(top + art_h, jump->h * cell, L.card[0].y - 4, 2)
                              : top + art_h;
        // Animated: idles, does acts, and takes walk-off/lurk trips (the
        // trips need PSRAM; without it he stays in his seat).
        // Created after the panels so he walks over the battery.
        if (!mascot_create(scr, L.logo_x, feet, cell)) {
            logo_img = lv_image_create(scr);
            lv_image_set_src(logo_img, &logo_dsc);
            lv_obj_set_pos(logo_img, L.logo_x, top);
        }
    }

    // Clawd performs in front of the meters, fully drawn, like the
    // attention walk-on (a tap sends him off). Created last so he's on top
    // of everything on the screen. His feet stand on the bottom edge of the
    // last panel, clear of the (Clawd-orange) status line.
    performer_init(scr, L.card[1].y + L.card[1].h);

    lv_obj_set_style_text_align(lbl_anim, LV_TEXT_ALIGN_CENTER, 0);
    anim_verb_idx = fitting_verb(0);
}

// The Pro/Max bars' fill colours and pace markers.
static void apply_bar_colors(const UsageData* data) {
    if (offline) {   // logged out: dim fills, no pace markers
        lv_obj_set_style_bg_color(bar_session, COL_DIM, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(bar_weekly, COL_DIM, LV_PART_INDICATOR);
        place_pace_tick(tick_session, -1.0f);
        place_pace_tick(tick_weekly, -1.0f);
        return;
    }
    if (data->enterprise) {
        // Spending has no 5-hour window, and the Period bar (coloured in
        // ui_update) already is the elapsed share: no markers.
        lv_obj_set_style_bg_color(bar_session, pct_color(data->session_pct), LV_PART_INDICATOR);
        place_pace_tick(tick_session, -1.0f);
        place_pace_tick(tick_weekly, -1.0f);
        return;
    }
    lv_obj_set_style_bg_color(bar_session,
        bar_color(data->session_pct, data->session_reset_mins, PACE_SESSION_MINS), LV_PART_INDICATOR);
    place_pace_tick(tick_session, pace_elapsed_pct(data->session_reset_mins, PACE_SESSION_MINS));
    lv_obj_set_style_bg_color(bar_weekly,
        bar_color(data->weekly_pct, data->weekly_reset_mins, PACE_WEEK_MINS), LV_PART_INDICATOR);
    place_pace_tick(tick_weekly, pace_elapsed_pct(data->weekly_reset_mins, PACE_WEEK_MINS));
}

void ui_set_pace_marker(bool on) {
    if (on == pace_marker) return;
    pace_marker = on;
    if (have_last_data) apply_bar_colors(&last_data);
}

static void render_usage(const UsageData* data);

void ui_update(const UsageData* data) {
    if (!data->valid) return;
    data_ok = data->ok;
    if (!data->ok) return;          // a {"ok":false} "no data" beat → fall through to idle, keep last numbers
    last_data_ms = lv_tick_get();   // a real usage update just landed
    data_received = true;

    if (data->clock_epoch > 0) {    // daemon supplied wall-clock time → drive the title clock
        if (clock_fmt != data->clock_fmt) clock_last_min = -1;
        clock_base_epoch = data->clock_epoch;
        clock_base_ms = last_data_ms;
        clock_fmt = data->clock_fmt;
        apply_title_font();
    } else if (clock_base_epoch != 0) {   // clock turned off daemon-side → revert title to "Usage"
        clock_base_epoch = 0;
        clock_last_min = -1;
        lv_label_set_text(lbl_title, "Usage");
        apply_title_font();
    }
    render_usage(data);
}

// clawdmeter2: the title (clock <-> date) drops in from just above with a
// little bounce. Only translate + text opacity: no layer buffer needed.
static void title_exec(void* obj, int32_t v) {   // v: 0..1000
    lv_obj_t* o = (lv_obj_t*)obj;
    lv_obj_set_style_translate_y(o, -(int32_t)L.w / 24 * (1000 - v) / 1000, 0);
    lv_obj_set_style_text_opa(o, (lv_opa_t)(v > 1000 ? 255 : v * 255 / 1000), 0);
}
static void title_pop(void) {
    if (!lbl_title) return;
    lv_anim_delete(lbl_title, title_exec);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, lbl_title);
    lv_anim_set_exec_cb(&a, title_exec);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, 380);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_start(&a);
}

// clawdmeter2: a swipe's transition. The two cards fly in one after the
// other from the side the finger came from, overshoot a touch and settle,
// like dealt cards. Only translate_x moves (a whole-group opacity forces a
// full-screen layer per frame, which made it stutter); the title, Clawd and
// the status line stay put.
#define SLIDE_MS     340
#define SLIDE_STAGGER 70
static void slide_exec(void* obj, int32_t v) { lv_obj_set_style_translate_x((lv_obj_t*)obj, v, 0); }

void ui_slide_in(int finger_dir) {
    if (!usage_group || minimal_mode()) return;
    const int32_t from = -(finger_dir >= 0 ? 1 : -1) * (L.w * 3 / 4);
    const uint32_t n = lv_obj_get_child_count(usage_group);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* c = lv_obj_get_child(usage_group, i);
        lv_anim_delete(c, slide_exec);
        lv_obj_set_style_translate_x(c, from, 0);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, c);
        lv_anim_set_exec_cb(&a, slide_exec);
        lv_anim_set_values(&a, from, 0);
        lv_anim_set_duration(&a, SLIDE_MS);
        // Cards in reading order: the top one leads.
        lv_anim_set_delay(&a, (lv_obj_get_y(c) > L.card[0].y + L.card[0].h / 2) ? SLIDE_STAGGER : 0);
        lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
        lv_anim_start(&a);
    }
}

void ui_show(const UsageData* view) {
    if (!view->valid || !view->ok || !have_last_data) return;
    render_usage(view);
}

// clawdmeter2: session usage warning. Crossing 80 % glows the edge amber,
// 95 % red with a chime; once per level per account until the session resets
// (the % drops back under 50).
#define WARN_SLOTS 8
static void check_usage_warning(const UsageData* d) {
    static uint8_t warned[WARN_SLOTS] = {};   // 0 none, 1 = 80 % done, 2 = 95 % done
    if (d->enterprise) return;
    const int slot = d->account_idx > 0 ? (d->account_idx - 1) % WARN_SLOTS : 0;
    const float p = d->session_pct;
    if (p < 50.0f) { warned[slot] = 0; return; }
    if (p >= 95.0f && warned[slot] < 2) {
        warned[slot] = 2;
        if (!minimal_mode()) party_glow(lv_color_hex(0xFF453A), 3);
        if (!idle_is_asleep()) chime_play();
    } else if (p >= 80.0f && warned[slot] < 1) {
        warned[slot] = 1;
        if (!minimal_mode()) party_glow(lv_color_hex(0xFFC107), 2);
    }
}

static void render_usage(const UsageData* data) {
    last_data = *data;
    have_last_data = true;
    check_usage_warning(data);
    int s_pct = (int)(data->session_pct + 0.5f);

    if (data->enterprise) {
        // Spending box: big number-only label + small "%" symbol + desc + pace
        lv_obj_set_style_text_font(lbl_session_pct, L.ent_pct_font, 0);
        lv_label_set_text(lbl_session_label, L.pills[2]);
        lv_obj_add_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status,   LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_style_text_font(lbl_session_pct, L.pct_font, 0);
        lv_label_set_text(lbl_session_label, L.pills[0]);
        lv_obj_clear_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
    }

    char buf[48];

    // Pace vars used in both enterprise blocks below
    const char* pace_text = "Under pace";
    lv_color_t  pace_color = COL_GREEN;
    const char* pace_hex   = P.green_hex;
    if (data->session_pct > (float)data->time_pct + 15.0f) {
        pace_text = "Over pace";  pace_color = COL_RED;   pace_hex = P.red_hex;
    } else if (data->session_pct > (float)data->time_pct - 15.0f) {
        pace_text = "On pace";    pace_color = COL_AMBER; pace_hex = P.amber_hex;
    }

    if (data->enterprise) {
        lv_label_set_text_fmt(lbl_session_pct, "%d", s_pct);
        lv_obj_align_to(lbl_session_pct_sym, lbl_session_pct,
                        LV_ALIGN_OUT_RIGHT_TOP, 4, 12);
    } else {
        lv_label_set_text_fmt(lbl_session_pct, "%d%%", s_pct);
        format_reset_time(data->session_reset_mins, buf, sizeof(buf), "Not started");
        lv_label_set_text(lbl_session_reset, buf);
    }

    lv_bar_set_value(bar_session, s_pct, LV_ANIM_ON);

    if (data->enterprise) {
        // Period box: time % + dynamic pace color + "Resets <date>" label
        lv_label_set_text(lbl_weekly_label, L.pills[3]);
        lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", data->time_pct);
        lv_bar_set_value(bar_weekly, data->time_pct, LV_ANIM_ON);
        lv_color_t bar_pace = (data->session_pct <= (float)data->time_pct) ? COL_GREEN :
                              (data->session_pct <= (float)data->time_pct + 15.0f) ? COL_AMBER :
                              COL_RED;
        lv_obj_set_style_bg_color(bar_weekly, bar_pace, LV_PART_INDICATOR);
        if (L.period_short)
            snprintf(buf, sizeof(buf), "#%s Resets %s#", pace_hex, data->reset_date);
        else
            snprintf(buf, sizeof(buf), "#%s %s# - #%s Resets %s#",
                     pace_hex, pace_text, P.text_hex, data->reset_date);
        lv_obj_set_style_text_font(lbl_weekly_reset, L.period_font, 0);
        lv_label_set_text(lbl_weekly_reset, buf);
    } else {
        int w_pct = (int)(data->weekly_pct + 0.5f);
        lv_label_set_text(lbl_weekly_label, L.pills[1]);
        lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", w_pct);
        lv_bar_set_value(bar_weekly, w_pct, LV_ANIM_ON);
        format_reset_time(data->weekly_reset_mins, buf, sizeof(buf), "---");
        lv_obj_set_style_text_font(lbl_weekly_reset, L.reset_font, 0);
        lv_label_set_text(lbl_weekly_reset, buf);
    }
    apply_bar_colors(data);
}

// Pick the usage-view sub-screen: pairing hint (BLE down), stale meters
// (connected but data has gone stale: dimmed, last values kept), or the live
// meters. Only restyles on an actual change. The animated status line stays
// visible everywhere and names the state.
static void update_view_state(void) {
    if (!usage_group || !pair_group) return;
    int v;
    if (!s_ble_connected) {
        v = 0;  // pairing hint
    } else if (data_received && data_ok && (lv_tick_get() - last_data_ms) < DATA_FRESH_MS) {
        v = 2;  // live usage
    } else {
        v = 1;  // stale
    }
    if (v == view_state) return;
    view_state = v;
    if (v == 0) {
        lv_obj_add_flag(usage_group, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(usage_group, LV_OBJ_FLAG_HIDDEN);
        // `opa` (not `opa_layered`) scales every draw of the group and its
        // children: no layer buffer, so it costs nothing on the C6.
        lv_obj_set_style_opa(usage_group, v == 1 ? STALE_OPA : LV_OPA_COVER, 0);
    }
}

bool ui_meters_live(void) { return view_state == 2; }

// Move the still parts to the next spot of the ring (+-2 px, +-1 on small panels).
static void shift_apply(lv_obj_t* o) {
    if (!o) return;
    lv_obj_set_style_translate_x(o, shift_x, 0);
    lv_obj_set_style_translate_y(o, shift_y, 0);
}
static void shift_tick(void) {
    static uint32_t last = 0;
    static uint8_t  spot = 0;
    const uint32_t now = lv_tick_get();
    if (last && now - last < SHIFT_EVERY_MS) return;
    last = now ? now : 1;
    static const int8_t RING[8][2] = { {0,0}, {1,0}, {1,1}, {0,1}, {-1,1}, {-1,0}, {-1,-1}, {0,-1} };
    const int k = L.small_icons ? 1 : 2;
    spot = (uint8_t)((spot + 1) % 8);
    shift_x = (int8_t)(RING[spot][0] * k);
    shift_y = (int8_t)(RING[spot][1] * k);
    shift_apply(usage_container);
    shift_apply(battery_img);
    batt_shift_parts();
    shift_apply(lbl_batt_pct);
}

// clawdmeter2: a long run (time or tokens) glows the screen edge once.
#define LONG_RUN_MS     (15u * 60u * 1000u)
#define LONG_RUN_TOKENS 200000
static uint32_t long_run_for = 0;   // working_since of the stretch that already glowed (0 = none)

static void check_long_run(void) {
    if (working != 1 || !working_since) return;
    const uint32_t now = lv_tick_get();
    // A new stretch (its start moved by more than a few seconds) may glow again.
    if (long_run_for && (working_since - long_run_for) > 5000u && (long_run_for - working_since) > 5000u)
        long_run_for = 0;
    if (long_run_for) return;
    if (now - working_since >= LONG_RUN_MS || turn_tokens >= LONG_RUN_TOKENS) {
        long_run_for = working_since ? working_since : 1;
        Serial.printf("ui: long run (%us, %d tokens)\n", (unsigned)((now - working_since) / 1000), (int)turn_tokens);
        if (!minimal_mode()) party_glow(lv_color_hex(CC_THINK), 3);
        if (!idle_is_asleep()) chime_play();
    }
}

void ui_tick_anim(void) {
    check_long_run();
    shift_tick();
    if (batt_pct_until && (int32_t)(lv_tick_get() - batt_pct_until) >= 0) {   // back to the icon
        batt_pct_until = 0;
        apply_battery_visibility();
        batt_icon_pop();
    }
    update_view_state();
    if (pair_hint == PAIR_HINT_CLEARED && lv_tick_get() - pair_hint_since >= PAIR_CLEARED_SHOW_MS)
        ui_set_pair_hint(PAIR_HINT_NORMAL);
    const bool still = minimal_mode();   // minimal: nothing moves but the numbers

    uint32_t now = lv_tick_get();
    if (clock_base_epoch > 0) {
        const long cur = clock_base_epoch + (long)((now - clock_base_ms) / 1000);
        local_hour = (int)((cur % 86400 + 86400) % 86400 / 3600);
    } else {
        local_hour = -1;
    }

    // Title clock: once the daemon has sent wall-clock time, replace "Usage" with
    // the live time, advanced locally so it ticks every minute between payloads.
    if (clock_base_epoch > 0) {
        time_t cur = (time_t)(clock_base_epoch + (now - clock_base_ms) / 1000);
        struct tm tmv;
        gmtime_r(&cur, &tmv);   // epoch is already local wall-clock → gmtime keeps it as-is
        if (date_until && (int32_t)(now - date_until) >= 0) {   // the date's turn is over
            date_until = 0;
            apply_title_font();
            clock_last_min = -1;
            title_pop();
        }
        if (!date_until && tmv.tm_min != clock_last_min) {   // only rewrite the title when the minute changes
            clock_last_min = tmv.tm_min;
            char tbuf[12];
            if (clock_fmt == 12) {
                int h12 = tmv.tm_hour % 12;
                if (h12 == 0) h12 = 12;
                snprintf(tbuf, sizeof(tbuf), "%d:%02d %s", h12, tmv.tm_min,
                         tmv.tm_hour < 12 ? "AM" : "PM");
            } else {
                snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
            }
            lv_label_set_text(lbl_title, tbuf);
            place_badge();
        }
    }

    if (alert_active) return;          // the status line holds the alert message
    if (notice_text) {
        if (notice_text != notice_shown) {
            notice_shown = notice_text;
            static char nbuf[64];
            snprintf(nbuf, sizeof(nbuf), "%s %s", spinner_frames[1], notice_text);
            set_status(nbuf, false);
        }
        return;
    }

    if (offline && s_ble_connected) {
        // Logged out: the line says so, in the dim colour, and holds still.
        if (!offline_line_up) {
            char dimhex[7];
            hex_of(COL_DIM, dimhex);
            static char obuf[40];
            snprintf(obuf, sizeof(obuf), "#%s ✻ Logged out#", dimhex);
            set_status(obuf, true);
            offline_line_up = true;
        }
        return;
    }

    if (still) {
        // One glyph, one word per state; redrawn only when it changes.
        const char* text = !s_ble_connected ? "Waiting"
                         : view_state == 1 ? (data_received ? "Stale" : "No data") : "Connected";
        if (text != still_shown) {
            still_shown = text;
            static char sbuf[40];
            snprintf(sbuf, sizeof(sbuf), "%s %s", spinner_frames[1], text);
            set_status(sbuf, false);
        }
        return;
    }

    // The verb changes every verb_ms; at No-nonsense that's once a minute
    // and the spinner holds still, so the line only redraws when the word
    // does (clawd_policy.h, liveliness).
    const ClawdTuning& tune = clawd_tuning();
    if (now - anim_msg_start >= tune.verb_ms) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_verb_idx = fitting_verb((anim_verb_idx + 1) % ANIM_MSG_COUNT);
        anim_msg_start = now;
    }

    // clawdmeter2: with the app's `wk`, the verbs run only while Claude works
    // (shimmering, like Claude Code); idle, the line holds still.
    const bool settled = s_ble_connected && view_state == 2 && now - connected_at_ms >= 5000;
    const bool idle = settled && working == 0;
    const bool busy = settled && working == 1 && tune.spinner;

    if (tune.spinner && !idle) {
        if (now - anim_last_ms < (busy ? SHIMMER_STEP_MS : spinner_ms[anim_spinner_idx])) return;
        anim_last_ms = now;
        if (now - spin_last_ms >= spinner_ms[anim_spinner_idx]) {
            spin_last_ms = now;
            anim_phase = (anim_phase + 1) % SPINNER_PHASES;
            anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                            : (SPINNER_PHASES - anim_phase);
        }
        frozen_shown = nullptr;
    } else {
        anim_spinner_idx = 1;
    }

    // Status text by priority. Whimsical messages only when connected & settled.
    const char* text;
    if (!s_ble_connected) {
        text = "Waiting";              // advertising / waiting for a host connection
    } else if (view_state == 1) {      // stale — alternate so it reads as alive AND not live
        text = (anim_msg_idx & 1) ? (data_received ? "Stale" : "No data") : "Listening";
    } else if (now - connected_at_ms < 5000) {
        text = "Connected";
    } else {
        text = anim_messages[anim_verb_idx];
        // clawdmeter2: every other verb is a fact (ui_set_info), in turn.
        const char* facts[UI_INFO_COUNT];
        int nf = 0;
        for (int i = 0; i < UI_INFO_COUNT; i++)
            if (info_text[i][0]) facts[nf++] = info_text[i];
        if (nf > 0 && (anim_msg_idx & 1)) {
            const char* f = facts[(anim_msg_idx / 2) % nf];
            if ((!tune.spinner || idle) && f == frozen_shown) return;
            frozen_shown = f;
            static char ibuf[80];
            snprintf(ibuf, sizeof(ibuf), "%s %s", spinner_frames[anim_spinner_idx], f);
            set_status(ibuf, false);
            return;
        }
        if (idle) {
            if (draw_idle_line()) frozen_shown = nullptr;
            return;
        }
        if (busy) {
            draw_working_line(spinner_frames[anim_spinner_idx], text, now);
            return;
        }
    }

    if (!tune.spinner) {
        if (text == frozen_shown) return;
        frozen_shown = text;
    }

    // All states share the whimsical style: "<glyph> <Title-case word>…"
    static char buf[80];
    snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
             spinner_frames[anim_spinner_idx], text);
    set_status(buf, false);
}

// ---- clawdmeter2: battery percentage on tap + charging fill ----
// While charging the icon is the plain outline with a green bar inside at the
// charge level, a soft pulse running from there to full (what's left to
// charge) and the bolt on top.
static lv_obj_t* batt_fill = nullptr;   // the charge level
static lv_obj_t* batt_flow = nullptr;   // pulse from the level to full
static lv_obj_t* batt_bolt = nullptr;
static lv_point_precise_t batt_bolt_pts[4];
static int       batt_shown_pct = -2;   // what the fill was last set to
static bool      batt_shown_chg = false;

// Inside of the outline (lucide battery: rect 2..18 x 6..18 in a 24 grid,
// stroke 2), one grid unit in from the stroke.
static int batt_scale(void) { return L.small_icons ? 1 : 2; }
static int batt_in_x(void)  { return L.batt_x + 4 * batt_scale(); }
static int batt_in_y(void)  { return L.batt_y + 8 * batt_scale(); }
static int batt_in_w(void)  { return 12 * batt_scale(); }
static int batt_in_h(void)  { return 8 * batt_scale(); }

static void batt_flow_exec(void* obj, int32_t v) {   // v: 0..1000 of the way to full
    lv_obj_t* o = (lv_obj_t*)obj;
    const int lvl = batt_in_w() * (batt_last_pct < 0 ? 0 : batt_last_pct) / 100;
    const int w = lvl + (batt_in_w() - lvl) * v / 1000;
    lv_obj_set_width(o, w < 1 ? 1 : w);
    lv_obj_set_style_bg_opa(o, (lv_opa_t)(110 * (1000 - v) / 1000), 0);
}

static lv_obj_t* batt_bar(lv_obj_t* parent, lv_opa_t opa) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, lv_color_hex(BATT_GREEN), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_set_style_radius(o, batt_scale(), 0);
    lv_obj_set_pos(o, batt_in_x(), batt_in_y());
    lv_obj_set_size(o, 1, batt_in_h());
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

static void batt_charge_init(lv_obj_t* scr) {
    batt_fill = batt_bar(scr, LV_OPA_COVER);
    batt_flow = batt_bar(scr, 110);
    const int k = batt_scale();
    // The charging icon's bolt: m11 7 -3 5 h4 l-3 5.
    const int16_t bx[4] = { 11, 8, 12, 9 }, by[4] = { 7, 12, 12, 17 };
    for (int i = 0; i < 4; i++) {
        batt_bolt_pts[i].x = bx[i] * k;
        batt_bolt_pts[i].y = by[i] * k;
    }
    batt_bolt = lv_line_create(scr);
    lv_line_set_points(batt_bolt, batt_bolt_pts, 4);
    lv_obj_set_style_line_width(batt_bolt, 2 * k, 0);
    lv_obj_set_style_line_rounded(batt_bolt, true, 0);
    lv_obj_set_style_line_color(batt_bolt, COL_TEXT, 0);
    lv_obj_set_pos(batt_bolt, L.batt_x, L.batt_y);
    lv_obj_clear_flag(batt_bolt, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(batt_bolt, LV_OBJ_FLAG_HIDDEN);
}

// Fill + pulse for the current reading (only touched when it changes).
static void batt_charge_apply(void) {
    if (!batt_fill) return;
    const bool on = batt_last_chg && batt_last_pct >= 0;
    if (on == batt_shown_chg && batt_last_pct == batt_shown_pct) return;
    batt_shown_chg = on;
    batt_shown_pct = batt_last_pct;
    lv_anim_delete(batt_flow, batt_flow_exec);
    if (!on) return;
    int w = batt_in_w() * batt_last_pct / 100;
    lv_obj_set_width(batt_fill, w < 2 ? 2 : w);
    if (batt_last_pct < 100 && !minimal_mode()) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, batt_flow);
        lv_anim_set_exec_cb(&a, batt_flow_exec);
        lv_anim_set_values(&a, 0, 1000);
        lv_anim_set_duration(&a, 1400);
        lv_anim_set_repeat_delay(&a, 500);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    } else {
        lv_obj_set_style_bg_opa(batt_flow, LV_OPA_TRANSP, 0);
    }
}

static void batt_shift_parts(void) {
    lv_obj_t* const parts[3] = { batt_fill, batt_flow, batt_bolt };
    for (lv_obj_t* o : parts) {
        if (!o) continue;
        lv_obj_set_style_translate_x(o, shift_x, 0);
        lv_obj_set_style_translate_y(o, shift_y, 0);
    }
}

// The same rise-and-fade the clock does when the date comes and goes.
static void batt_pop_exec(void* obj, int32_t v) {   // v: 0..1000 (overshoots)
    lv_obj_t* o = (lv_obj_t*)obj;
    lv_obj_set_style_translate_y(o, shift_y - (int32_t)L.w / 24 * (1000 - v) / 1000, 0);
    lv_obj_set_style_opa(o, (lv_opa_t)(v > 1000 ? 255 : v < 0 ? 0 : v * 255 / 1000), 0);
}
static void batt_pop(lv_obj_t* o) {
    if (!o) return;
    lv_anim_delete(o, batt_pop_exec);
    lv_obj_set_style_translate_y(o, shift_y, 0);
    lv_obj_set_style_opa(o, LV_OPA_COVER, 0);
    if (minimal_mode()) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, batt_pop_exec);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, 380);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_start(&a);
}

// The percentage label in the icon's place: text, colour, font and position.
// The font is the clock's family (Styrene), the biggest that fits between
// the title and the right edge, on the title's centre line.
static void batt_pct_refresh(void) {
    if (!lbl_batt_pct || !battery_img) return;
    char buf[8];
    if (batt_last_pct < 0) snprintf(buf, sizeof(buf), batt_last_usb ? "USB" : "--");
    else                   snprintf(buf, sizeof(buf), "%d%%", batt_last_pct);
    lv_label_set_text(lbl_batt_pct, buf);
    lv_obj_set_style_text_color(lbl_batt_pct, lv_color_white(), 0);   // always white

    const int icon = L.small_icons ? LAYOUT_BATT_W_SMALL : LAYOUT_BATT_W;
    const int right = L.batt_x + icon;
    int left = L.batt_x - 4;
    int mid_y = L.batt_y + icon / 2;
    int max_h = 0;
    if (lbl_title && !lv_obj_has_flag(lbl_title, LV_OBJ_FLAG_HIDDEN)) {
        lv_area_t t;
        lv_obj_get_coords(lbl_title, &t);
        if (t.x2 + 8 < left) left = t.x2 + 8;
        mid_y = (t.y1 + t.y2) / 2;
        max_h = lv_font_get_line_height(lv_obj_get_style_text_font(lbl_title, LV_PART_MAIN));
    }
    const lv_font_t* const fonts[] = { &font_styrene_28, &font_styrene_24, &font_styrene_20, &font_styrene_16 };
    const lv_font_t* f = &font_styrene_16;
    for (const lv_font_t* cand : fonts) {
        if (max_h && lv_font_get_line_height(cand) > max_h) continue;
        if (layout_text_width(cand, buf) <= right - left) { f = cand; break; }
    }
    lv_obj_set_style_text_font(lbl_batt_pct, f, 0);
    lv_obj_update_layout(lbl_batt_pct);
    lv_obj_set_pos(lbl_batt_pct, right - lv_obj_get_width(lbl_batt_pct),
                   mid_y - lv_obj_get_height(lbl_batt_pct) / 2);
}

static void batt_set_hidden(lv_obj_t* o, bool hide) {
    if (!o) return;
    if (hide) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void apply_battery_visibility(void) {
    if (!battery_img) return;
    const bool pct = batt_pct_until != 0;
    const bool chg = batt_shown_chg;
    batt_set_hidden(battery_img, pct);
    batt_set_hidden(batt_fill, pct || !chg);
    batt_set_hidden(batt_flow, pct || !chg);
    batt_set_hidden(batt_bolt, pct || !chg);
    batt_set_hidden(lbl_batt_pct, !pct);
}

// The icon (and its charging parts) come back with the pop.
static void batt_icon_pop(void) {
    batt_pop(battery_img);
    if (batt_shown_chg) {
        batt_pop(batt_fill);
        batt_pop(batt_bolt);
    }
}

void ui_show_battery_pct(void) {
    if (!battery_img || !lbl_batt_pct) return;
    if (batt_pct_until) {   // a second tap: straight back to the icon
        batt_pct_until = 0;
        apply_battery_visibility();
        batt_icon_pop();
        return;
    }
    batt_pct_until = lv_tick_get() + BATT_PCT_SHOW_MS;
    if (!batt_pct_until) batt_pct_until = 1;
    batt_pct_refresh();
    apply_battery_visibility();
    batt_pop(lbl_batt_pct);
}

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    (void)name; (void)mac;
    bool was_connected = s_ble_connected;
    s_ble_connected = (state == BLE_STATE_CONNECTED);

    if (s_ble_connected && !was_connected) connected_at_ms = lv_tick_get();
    // pair / idle / usage — picked from connection + data freshness.
    update_view_state();
}

void ui_update_battery(int percent, bool charging, bool usb_power) {
    if (!battery_img) return;
    batt_last_pct = percent;
    batt_last_chg = charging;
    batt_last_usb = usb_power;
    if (batt_pct_until) batt_pct_refresh();
    batt_charge_apply();
    int idx;
    if (percent < 0 && usb_power) {
        idx = 5;   // no battery, running on USB: show the plug-zap glyph, not an empty battery
    } else if (charging) {
        idx = batt_fill ? 0 : 4;   // the outline, filled by batt_charge_apply
    } else if (percent < 0) {
        idx = 0;
    } else if (percent <= 10) {
        idx = 6;   // the red outline
    } else if (percent <= 35) {
        idx = 1;
    } else if (percent <= 75) {
        idx = 2;
    } else {
        idx = 3;
    }
    lv_image_set_src(battery_img, idx == 6 ? &battery_empty_red : &battery_dscs[idx]);
    apply_battery_visibility();
}

void ui_set_alert_text(const char* message) {
    if (message && message[0]) {
        alert_active = true;
        // Minimal mode: nothing moves, so a long message is cut, not scrolled.
        lv_label_set_long_mode(lbl_anim, minimal_mode() ? LV_LABEL_LONG_DOT
                                                        : LV_LABEL_LONG_SCROLL_CIRCULAR);
        // One line: without a fixed height LONG_DOT would wrap onto the cards.
        lv_obj_set_size(lbl_anim, L.status_w, lv_font_get_line_height(L.status_font));
        set_status(message, false);
        return;
    }
    if (!alert_active) return;
    alert_active = false;
    lv_label_set_long_mode(lbl_anim, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(lbl_anim, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    anim_last_ms = 0;          // whimsical line redraws on the next tick
    still_shown = nullptr;
    frozen_shown = nullptr;
}

// ======== Accounts (clawdmeter2) ========

static void style_pill(lv_obj_t* pill) {
    lv_obj_set_style_text_color(pill, offline ? COL_DIM : COL_TEXT, 0);
    lv_obj_set_style_bg_color(pill, COL_BAR_BG, 0);
}

// Restyle everything built with the previous palette.
static void apply_palette(void) {
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    lv_obj_set_style_text_color(lbl_acct, COL_ACCENT, 0);
    lv_obj_t* panels[] = { panel_session, panel_weekly };
    for (lv_obj_t* p : panels) if (p) lv_obj_set_style_bg_color(p, COL_PANEL, 0);
    lv_obj_t* bars[] = { bar_session, bar_weekly };
    for (lv_obj_t* b : bars) lv_obj_set_style_bg_color(b, COL_BAR_BG, LV_PART_MAIN);
    style_pill(lbl_session_label);
    style_pill(lbl_weekly_label);
    lv_obj_t* text[] = { lbl_session_pct, lbl_weekly_pct, lbl_session_pct_sym };
    for (lv_obj_t* t : text) lv_obj_set_style_text_color(t, offline ? COL_DIM : COL_TEXT, 0);
    lv_obj_t* dim[] = { lbl_session_reset, lbl_weekly_reset, lbl_spending_desc };
    for (lv_obj_t* t : dim) lv_obj_set_style_text_color(t, COL_DIM, 0);
    lv_obj_t* ticks[] = { tick_session, tick_weekly };
    for (lv_obj_t* t : ticks) lv_obj_set_style_bg_color(t, COL_TEXT, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    if (pair_lines[0]) {
        lv_obj_set_style_text_color(pair_lines[0], COL_TEXT, 0);
        lv_obj_set_style_text_color(pair_lines[1], COL_DIM, 0);
        lv_obj_set_style_text_color(pair_lines[2], COL_DIM, 0);
        apply_pair_text();
    }
    if (have_last_data) render_usage(&last_data);   // bar fills and the recoloured period line
}

void ui_set_offline(bool off) {
    if (off == offline) return;
    offline = off;
    offline_line_up = false;
    // The line that was up is gone or stale: every cached status redraws.
    idle_shown[0] = 0;
    still_shown = nullptr;
    frozen_shown = nullptr;
    anim_last_ms = 0;
    if (!lbl_session_pct) return;   // UI not built yet
    lv_obj_t* text[] = { lbl_session_pct, lbl_weekly_pct, lbl_session_pct_sym };
    for (lv_obj_t* t : text) if (t) lv_obj_set_style_text_color(t, off ? COL_DIM : COL_TEXT, 0);
    style_pill(lbl_session_label);
    style_pill(lbl_weekly_label);
    // Bar fills follow with the render that comes next (ui_show / ui_update).
}

void ui_set_working(int w) {
    const int8_t now_w = w < 0 ? -1 : w > 0 ? 1 : 0;
    const uint32_t t = lv_tick_get();
    if (now_w == 1 && working != 1) working_since = t;
    if (now_w != 1 && working == 1) {   // a stretch ended: "Brewed for 1m 23s"
        worked_ms = t - working_since;
        past_idx = (uint8_t)((past_idx + 1 + t % 5) % PAST_COUNT);
    }
    working = now_w;
}

void ui_set_turn(int32_t tokens, int32_t secs) {
    turn_tokens = tokens;
    // The app knows when the prompt began: the timer matches Claude Code's
    // even across a reboot or a swipe.
    if (secs >= 0 && working == 1) working_since = lv_tick_get() - (uint32_t)secs * 1000u;
}

void ui_set_account(int shown, int count) {
    if (shown < 1) shown = 1;
    if (count < 1) count = 1;
    const int want = count >= 2 && shown == 2 ? 2 : 1;
    const bool changed = shown != acct_shown || count != acct_count;
    acct_shown = shown;
    acct_count = count;
    if (want != palette_idx) {
        palette_idx = want;
        P = want == 2 ? pal_neon() : pal_original();
        g_account_tint = want == 2;   // Clawd matches the account
        mascot_retint();
        mascot_account_changed();     // and he's that account's Clawd, doing his own thing
        performer_retint();
        face_set_tint(want == 2);     // a face already up (peek, pet, screensaver) too
        apply_palette();
    }
    if (changed) {
        place_badge();
        anim_verb_idx = fitting_verb(anim_verb_idx);   // the badge takes room
        // Another account's work: its times aren't known here yet.
        working = -1;
        worked_ms = 0;
    }
}

int ui_account_palette(void) { return palette_idx; }

static bool inside(lv_obj_t* o, int x, int y) {
    if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    return x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2;
}

// clawdmeter2: a tap on the clock shows the date and year for a few seconds,
// in the largest title font that fits between Clawd's seat and the battery.
void ui_show_date(void) {
    if (clock_base_epoch <= 0 || lv_obj_has_flag(lbl_title, LV_OBJ_FLAG_HIDDEN)) return;
    const uint32_t now = lv_tick_get();
    if (date_until) {   // a second tap: straight back to the clock
        date_until = 0;
        apply_title_font();
        clock_last_min = -1;
        title_pop();
        return;
    }
    const time_t cur = (time_t)(clock_base_epoch + (now - clock_base_ms) / 1000);
    struct tm tmv;
    gmtime_r(&cur, &tmv);
    static const char* const DAYS[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char* const MONTHS[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    char longer[24], shorter[16];
    snprintf(longer, sizeof(longer), "%s %d %s %d", DAYS[tmv.tm_wday % 7], tmv.tm_mday,
             MONTHS[tmv.tm_mon % 12], tmv.tm_year + 1900);
    snprintf(shorter, sizeof(shorter), "%d %s %d", tmv.tm_mday, MONTHS[tmv.tm_mon % 12], tmv.tm_year + 1900);

    const int logo_r = L.logo_x + (L.small_icons ? LAYOUT_LOGO_W_SMALL : LAYOUT_LOGO_W);
    const int batt = L.batt_x > 0 ? L.batt_x : L.w;
    const int half = (L.title_cx - logo_r < batt - L.title_cx ? L.title_cx - logo_r : batt - L.title_cx) - 6;
    const lv_font_t* clock_font = lv_obj_get_style_text_font(lbl_title, LV_PART_MAIN);
    const lv_font_t* const fonts[] = { clock_font, &font_styrene_28, &font_styrene_24, &font_styrene_20,
                                       &font_styrene_16 };
    const char* text = nullptr;
    const lv_font_t* f = nullptr;
    // A font that lacks a letter (the big clock face is digits only) measures
    // short and draws the letter from a fallback; skip it.
    auto has_all = [](const lv_font_t* fo, const char* s) {
        lv_font_glyph_dsc_t g;
        for (; *s; s++)
            if (*s != ' ' && !lv_font_get_glyph_dsc(fo, &g, (uint32_t)(uint8_t)*s, 0)) return false;
        return true;
    };
    for (const lv_font_t* cand : fonts) {
        if (lv_font_get_line_height(cand) > lv_font_get_line_height(clock_font)) continue;
        if (!has_all(cand, longer)) continue;
        if (layout_text_width(cand, longer) <= 2 * half) { text = longer; f = cand; break; }
        if (layout_text_width(cand, shorter) <= 2 * half) { text = shorter; f = cand; break; }
    }
    if (!f) return;
    const int fmt = clock_fmt == 12 ? 1 : 0;
    // On the clock's centre line.
    const int y = L.title_y[fmt] + (lv_font_get_line_height(clock_font) - lv_font_get_line_height(f)) / 2;
    lv_label_set_text(lbl_title, text);
    lv_obj_set_style_text_font(lbl_title, f, 0);
    title_pop();
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, L.title_cx - L.w / 2, y);
    date_until = now + DATE_SHOW_MS;
    if (!date_until) date_until = 1;
}

UiHit ui_hit_test(int x, int y) {
    if (view_state == 0) return UI_HIT_NONE;   // the pairing hint owns the screen
    // The header band: Clawd's seat on the left, then the title and the strip
    // it sits in.
    const int logo_r = L.logo_x + (L.small_icons ? LAYOUT_LOGO_W_SMALL : LAYOUT_LOGO_W);
    if (y < L.card[0].y && x <= logo_r) return UI_HIT_CLAWD;
    if (y < L.card[0].y && x > logo_r && (!battery_img || x < L.batt_x))
        return UI_HIT_TITLE;
    if (y < L.card[0].y && battery_img && x >= L.batt_x) return UI_HIT_BATTERY;
    if (inside(panel_session, x, y)) return UI_HIT_SESSION;
    if (inside(panel_weekly, x, y)) return UI_HIT_WEEKLY;
    return UI_HIT_NONE;
}
