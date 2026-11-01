#include "detail.h"
#include "history.h"
#include "theme.h"
#include "layout.h"
#include "ui.h"
#include "hal/board_caps.h"
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

// clawdmeter2: the screens behind the Session and Weekly cards, laid out like
// Claude Code's /usage: a monospace heading, a square bar with "N% used"
// beside it, "Resets in ..." below; then the history (30-day chart under
// the session, this week's tokens under the week).

LV_FONT_DECLARE(font_mono_18);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_12);

static lv_obj_t* root;
static lv_obj_t* lbl_head;      // "Current session" / "Current week (all models)"
static lv_obj_t* lbl_acct;      // "#N"
static lv_obj_t* bar_track;
static lv_obj_t* bar_fill;
static lv_obj_t* lbl_used;      // "35% used"
static lv_obj_t* lbl_reset;     // "Resets in 2h 14m"
static lv_obj_t* rule;          // the line between usage and history
static lv_obj_t* lbl_sub;       // "Last 30 days" / "This week"
static lv_obj_t* lbl_stats;     // busiest day + 30-day total
static lv_obj_t* lbl_cap_l;     // "30d ago"
static lv_obj_t* lbl_cap_r;     // "today"
static lv_obj_t* lbl_big;       // week total
static lv_obj_t* lbl_joke;
static lv_obj_t* lbl_empty;     // "No history yet"
static lv_obj_t* bars[HISTORY_DAYS];
static lv_obj_t* hbars[HISTORY_HOURS];   // "Today" strip, one bar per local hour
static lv_obj_t* lbl_today;              // "Today"
static lv_obj_t* lbl_tick[4];            // 0 / 6 / 12 / 18

// Content rectangle (inside the panel's safe area) and fonts, derived from
// BoardCaps once in detail_init.
static int cx, cy, cw, ch;
static const lv_font_t *f_mono, *f_small, *f_big, *f_joke;

static bool shown;
static DetailMode cur_mode;
static int cur_acct;
static bool cur_neon;

// The shown account's numbers (detail_set_usage).
static bool  have_usage;
static bool  u_enterprise;
static float u_session, u_week;
static int   u_session_reset, u_week_reset;

static lv_obj_t* make_label(const lv_font_t* f) {
    lv_obj_t* l = lv_label_create(root);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    return l;
}

static lv_obj_t* make_rect(void) {
    lv_obj_t* r = lv_obj_create(root);
    lv_obj_remove_style_all(r);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);
    return r;
}

void detail_init(void) {
    if (root) return;
    const BoardCaps& c = board_caps();
    const int w = c.width, h = c.height;
    if (c.round_panel) {
        // Stay inside the inscribed square: ~15 % inset on each side.
        const int ix = w * 15 / 100, iy = h * 15 / 100;
        cx = ix; cy = iy; cw = w - 2 * ix; ch = h - 2 * iy;
    } else {
        const int pad = w / 24;
        cx = pad; cy = pad; cw = w - 2 * pad; ch = h - 2 * pad;
    }
    const bool big = w >= 400;
    f_mono  = &font_mono_18;
    f_small = big ? &font_styrene_16 : &font_styrene_12;
    f_big   = big ? &font_styrene_48 : &font_styrene_28;
    f_joke  = big ? &font_styrene_20 : &font_styrene_16;

    root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, w, h);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, THEME_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);

    lbl_head  = make_label(f_mono);
    lbl_acct  = make_label(f_mono);
    bar_track = make_rect();
    bar_fill  = make_rect();
    lbl_used  = make_label(f_mono);
    lbl_reset = make_label(f_mono);
    rule      = make_rect();
    lbl_sub   = make_label(f_mono);
    lbl_stats = make_label(f_mono);
    lbl_cap_l = make_label(f_small);
    lbl_cap_r = make_label(f_small);
    lbl_big   = make_label(f_big);
    lbl_joke  = make_label(f_joke);
    lbl_empty = make_label(f_mono);
    for (auto& b : bars) b = make_rect();
    for (auto& b : hbars) b = make_rect();
    lbl_today   = make_label(f_small);
    for (auto& t : lbl_tick) t = make_label(f_small);

    lv_label_set_long_mode(lbl_big, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(lbl_big, cw);
    lv_obj_set_style_text_align(lbl_big, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lbl_joke, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(lbl_joke, cw);
    lv_obj_set_style_text_align(lbl_joke, LV_TEXT_ALIGN_CENTER, 0);
}

static void hide_content(void) {
    lv_obj_t* all[] = { lbl_head, lbl_acct, bar_track, bar_fill, lbl_used, lbl_reset, rule,
                        lbl_sub, lbl_stats, lbl_cap_l, lbl_cap_r, lbl_big, lbl_joke, lbl_empty };
    for (lv_obj_t* o : all) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    for (lv_obj_t* b : bars) lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    for (lv_obj_t* b : hbars) lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    for (lv_obj_t* t : lbl_tick) lv_obj_add_flag(t, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_today, LV_OBJ_FLAG_HIDDEN);
}

static void show(lv_obj_t* o) { lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN); }

static void text(lv_obj_t* l, const char* s, lv_color_t c) {
    lv_label_set_text(l, s);
    lv_obj_set_style_text_color(l, c, 0);
    show(l);
}

// "Resets in 2h 14m", counting down like /usage; mins < 0 = not known.
static void format_reset(int mins, bool session, char* out, size_t n) {
    if (mins < 0) { snprintf(out, n, "%s", session ? "Starts with your next prompt" : "Reset time not known"); return; }
    const int d = mins / 1440, h = (mins % 1440) / 60, m = mins % 60;
    if (d > 0)      snprintf(out, n, "Resets in %dd %dh", d, h);
    else if (h > 0) snprintf(out, n, "Resets in %dh %dm", h, m);
    else            snprintf(out, n, "Resets in %dm", m);
}

// The usage block: heading, bar + "N% used", reset line. Returns the y below it.
static int draw_usage(lv_color_t accent, lv_color_t textc, lv_color_t dim, lv_color_t track,
                      lv_color_t warn, lv_color_t crit) {
    const bool session = cur_mode == DETAIL_CHART;
    const int line = lv_font_get_line_height(f_mono);
    char buf[48];

    const char* head = session ? (u_enterprise ? "Spend limit" : "Current session")
                               : "Current week (all models)";
    text(lbl_head, head, textc);
    lv_obj_set_pos(lbl_head, cx, cy);
    snprintf(buf, sizeof(buf), "#%d", cur_acct);
    text(lbl_acct, buf, accent);
    lv_obj_align(lbl_acct, LV_ALIGN_TOP_RIGHT, -(board_caps().width - cx - cw), cy);

    int y = cy + line + line / 3;
    if (!have_usage || (!session && u_enterprise)) {
        text(lbl_used, have_usage ? "No weekly limit" : "No reading yet", dim);
        lv_obj_set_pos(lbl_used, cx, y);
        return y + line;
    }
    float pct = session ? u_session : u_week;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    snprintf(buf, sizeof(buf), "%d%% used", (int)(pct + 0.5f));
    const int used_w = layout_text_width(f_mono, buf);
    const int slot_w = layout_text_width(f_mono, "100% used");
    int bw = cw - slot_w - line / 2;
    if (bw < 40) bw = 40;
    const int bh = line - 4;
    lv_obj_set_size(bar_track, bw, bh);
    lv_obj_set_pos(bar_track, cx, y + 2);
    lv_obj_set_style_bg_color(bar_track, track, 0);
    show(bar_track);
    const int fw = (int)(bw * pct / 100.0f + 0.5f);
    if (fw > 0) {
        lv_obj_set_size(bar_fill, fw, bh);
        lv_obj_set_pos(bar_fill, cx, y + 2);
        lv_obj_set_style_bg_color(bar_fill, pct >= 90 ? crit : pct >= 70 ? warn : accent, 0);
        show(bar_fill);
    }
    text(lbl_used, buf, textc);
    lv_obj_set_pos(lbl_used, cx + cw - used_w, y);

    y += line + line / 4;
    format_reset(session ? u_session_reset : u_week_reset, session, buf, sizeof(buf));
    text(lbl_reset, buf, dim);
    lv_obj_set_pos(lbl_reset, cx, y);
    return y + line;
}

static int draw_rule(int y, lv_color_t track) {
    const int gap = lv_font_get_line_height(f_mono) * 2 / 3;
    lv_obj_set_size(rule, cw, 1);
    lv_obj_set_pos(rule, cx, y + gap);
    lv_obj_set_style_bg_color(rule, track, 0);
    show(rule);
    return y + 2 * gap;
}

// 24 thin hourly bars for today (relative to the busiest hour), ticks at
// 0/6/12/18, the current local hour highlighted.
static void draw_today(const AccountHistory* hist, lv_color_t accent, lv_color_t textc,
                       lv_color_t dim, lv_color_t track) {
    const int small_h = lv_font_get_line_height(f_small);
    const int hbar_h = 32;
    const int lab_y = cy + ch - small_h;
    const int bar_bottom = lab_y - 2;
    const int title_y = bar_bottom - hbar_h - 2 - small_h;

    uint32_t mx = 1;
    uint64_t total_h = 0;
    for (int i = 0; i < HISTORY_HOURS; i++) {
        if (hist->hours_h[i] > mx) mx = hist->hours_h[i];
        total_h += hist->hours_h[i];
    }
    const int cur = ui_local_hour();

    const int gap = 2;
    int bw = (cw - gap * (HISTORY_HOURS - 1)) / HISTORY_HOURS;
    if (bw < 1) bw = 1;
    const int used = bw * HISTORY_HOURS + gap * (HISTORY_HOURS - 1);
    const int x0 = cx + (cw - used) / 2;
    for (int i = 0; i < HISTORY_HOURS; i++) {
        const uint32_t v = hist->hours_h[i];
        int bh = (int)((uint64_t)v * hbar_h / mx);
        if (v > 0 && bh < 3) bh = 3;      // non-zero hour stays visible
        if (v == 0) bh = 1;               // empty hour = hairline baseline
        lv_obj_t* r = hbars[i];
        lv_obj_set_size(r, bw, bh);
        lv_obj_set_pos(r, x0 + i * (bw + gap), bar_bottom - bh);
        lv_obj_set_style_bg_color(r, i == cur ? textc : (v == 0 ? track : accent), 0);
        show(r);
    }

    char n[16], buf[24];
    history_format_tokens(total_h * 100u, n, sizeof(n));
    snprintf(buf, sizeof(buf), "Today %s", n);
    text(lbl_today, buf, dim);
    lv_obj_set_pos(lbl_today, x0, title_y);

    static const char* const names[4] = { "0", "6", "12", "18" };
    for (int k = 0; k < 4; k++) {
        text(lbl_tick[k], names[k], dim);
        const int hx = x0 + (k * 6) * (bw + gap);
        lv_obj_set_pos(lbl_tick[k], hx, lab_y);
    }
}

static void draw_chart(const AccountHistory* hist, int top, lv_color_t accent, lv_color_t textc,
                       lv_color_t dim, lv_color_t track) {
    char buf[64], a[16], b[16];
    const int line = lv_font_get_line_height(f_mono);
    const int small_h = lv_font_get_line_height(f_small);

    uint32_t mx = 1;   // at least 1 so an all-zero month stays flat
    uint64_t total_k = 0;
    for (int i = 0; i < HISTORY_DAYS; i++) {
        if (hist->days_k[i] > mx) mx = hist->days_k[i];
        total_k += hist->days_k[i];
    }

    text(lbl_sub, "Last 30 days", textc);
    lv_obj_set_pos(lbl_sub, cx, top);
    history_format_tokens((uint64_t)mx * 1000u, a, sizeof(a));
    history_format_tokens(total_k * 1000u, b, sizeof(b));
    snprintf(buf, sizeof(buf), "Peak %s  Total %s", a, b);
    text(lbl_stats, buf, dim);
    lv_obj_set_pos(lbl_stats, cx, top + line + line / 4);

    const int gap_y = line / 2;
    const int plot_top = top + 2 * line + line / 4 + gap_y;

    // "Today" strip at the very bottom, only when hourly data exists, the
    // layout isn't the small one and the 30-day plot keeps >= 70 px.
    const int hbar_h = 32, sgap = 6;
    const int reserve = small_h + 2 + hbar_h + 2 + small_h + sgap;
    const bool strip = hist->has_hours && board_caps().width >= 300 &&
                       (cy + ch - reserve - small_h - gap_y - plot_top) >= 70;
    const int base = strip ? reserve : 0;
    const int bottom = cy + ch - base - small_h - gap_y;
    int plot_h = bottom - plot_top;
    if (plot_h < 8) plot_h = 8;

    const int gap = cw >= 300 ? 3 : 2;
    int bw = (cw - gap * (HISTORY_DAYS - 1)) / HISTORY_DAYS;
    if (bw < 1) bw = 1;
    const int used = bw * HISTORY_DAYS + gap * (HISTORY_DAYS - 1);
    const int x0 = cx + (cw - used) / 2;
    for (int i = 0; i < HISTORY_DAYS; i++) {
        const uint32_t v = hist->days_k[i];
        int bh = (int)((uint64_t)v * plot_h / mx);
        if (bh < 2) bh = 2;                       // a zero day stays a visible stub
        lv_obj_t* r = bars[i];
        lv_obj_set_size(r, bw, bh);
        lv_obj_set_pos(r, x0 + i * (bw + gap), bottom - bh);
        lv_obj_set_style_bg_color(r, v == 0 ? track : (i == HISTORY_DAYS - 1 ? textc : accent), 0);
        show(r);
    }

    const int cap_y = cy + ch - base - small_h;
    text(lbl_cap_l, "30d ago", dim);
    text(lbl_cap_r, "today", dim);
    lv_obj_set_pos(lbl_cap_l, x0, cap_y);
    lv_obj_align(lbl_cap_r, LV_ALIGN_TOP_RIGHT, -(board_caps().width - (x0 + used)), cap_y);
    if (strip) draw_today(hist, accent, textc, dim, track);
}

static void draw_tokens(const AccountHistory* hist, int top, lv_color_t accent, lv_color_t textc,
                        lv_color_t dim) {
    char buf[40], n[16];
    const int line = lv_font_get_line_height(f_mono);
    text(lbl_sub, "This week", textc);
    lv_obj_set_pos(lbl_sub, cx, top);

    history_format_tokens(hist->week_tokens, n, sizeof(n));
    snprintf(buf, sizeof(buf), "%s tokens", n);
    text(lbl_big, buf, accent);
    text(lbl_joke, hist->joke, dim);
    lv_obj_update_layout(root);   // wrapped heights are needed to centre the pair

    const int area_top = top + line + line / 2;
    const int gap = lv_obj_get_height(lbl_big) / 4 + 4;
    const int th = lv_obj_get_height(lbl_big) + gap + lv_obj_get_height(lbl_joke);
    int y = area_top + (cy + ch - area_top - th) / 2;
    if (y < area_top) y = area_top;
    lv_obj_set_pos(lbl_big, cx, y);
    lv_obj_set_pos(lbl_joke, cx, y + lv_obj_get_height(lbl_big) + gap);
}

static void redraw(void) {
    hide_content();
    const bool neon = cur_neon;
    const lv_color_t accent = neon ? NEON_ACCENT : THEME_ACCENT;
    const lv_color_t textc = neon ? NEON_TEXT : THEME_TEXT;
    const lv_color_t dim = neon ? NEON_DIM : THEME_DIM;
    const lv_color_t track = neon ? NEON_BAR_BG : THEME_BAR_BG;
    const lv_color_t warn = neon ? NEON_AMBER : THEME_AMBER;
    const lv_color_t crit = neon ? NEON_RED : THEME_RED;

    int y = draw_usage(accent, textc, dim, track, warn, crit);
    y = draw_rule(y, track);

    const AccountHistory* hist = history_get(cur_acct);
    if (!hist) {
        text(lbl_empty, "No history yet", dim);
        lv_obj_set_pos(lbl_empty, cx, y);
        return;
    }
    if (cur_mode == DETAIL_CHART) draw_chart(hist, y, accent, textc, dim, track);
    else draw_tokens(hist, y, accent, textc, dim);
}

void detail_set_usage(const UsageData* view) {
    const bool had = have_usage;
    have_usage = view && view->valid && view->ok;
    if (have_usage) {
        u_enterprise = view->enterprise;
        u_session = view->session_pct;
        u_week = view->weekly_pct;
        u_session_reset = view->session_reset_mins;
        u_week_reset = view->weekly_reset_mins;
    }
    if (shown && (have_usage || had)) redraw();
}

void detail_show(DetailMode mode, int account, bool neon) {
    if (!root) return;
    cur_mode = mode;
    cur_acct = account;
    cur_neon = neon;
    shown = true;
    redraw();
    lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(root);
}

void detail_hide(void) {
    if (!root) return;
    shown = false;
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
}

bool detail_visible(void) { return shown; }

void detail_refresh(void) {
    if (shown) redraw();
}
