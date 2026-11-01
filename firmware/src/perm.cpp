#include "perm.h"
#include "theme.h"
#include "hal/board_caps.h"
#include <string.h>

LV_FONT_DECLARE(font_mono_18);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);

// clawdmeter2: the Allow/Deny card. Objects: root, 3 labels, bar track + fill,
// 2 buttons with a label each. No fade-in: opacity on a container with
// children needs a layer redraw per frame, too heavy for the pool.

#define FLASH_MS   160
#define TEXT_MAX   120
#define BAR_H      4

static lv_obj_t *root, *lbl_head, *lbl_tool, *lbl_text;
static int text_box_w = 0, text_box_h = 0;   // the command's box, for the fit check
static lv_obj_t *bar_track, *bar_fill;
static lv_obj_t *btn_deny, *btn_allow, *lbl_deny, *lbl_allow;

static int cx, cy, cw, ch;                 // content box
static int bx_deny, bx_allow, by, bw, bh;  // buttons (screen px)
static int bar_w_full;

static bool     shown;
static uint32_t cur_id;
static uint32_t t_start, t_total;          // lv_tick_get() at show, timeout (0 = none)
static int      bar_w_last = -1;
static bool     flashing;
static uint32_t t_flash;

static bool     ans_ready, ans_allow;
static uint32_t ans_id;

static lv_color_t deny_fill(void) { return lv_color_mix(THEME_RED, THEME_PANEL, 64); }

static void style_buttons(bool flash_allow, bool flash_deny) {
    lv_obj_set_style_bg_color(btn_allow, flash_allow ? lv_color_lighten(THEME_GREEN, 90) : THEME_GREEN, 0);
    lv_obj_set_style_bg_color(btn_deny, flash_deny ? THEME_RED : deny_fill(), 0);
}

static lv_obj_t* make_label(const lv_font_t* f, lv_color_t c, lv_obj_t* parent) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t* make_btn(lv_color_t border, int border_w, int radius) {
    lv_obj_t* b = lv_obj_create(root);
    lv_obj_remove_style_all(b);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, radius, 0);
    lv_obj_set_style_border_width(b, border_w, 0);
    lv_obj_set_style_border_color(b, border, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    return b;
}

static void make_rect(lv_obj_t* o, lv_color_t c, int radius) {
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
}

void perm_init(lv_obj_t* parent) {
    if (root) return;
    const BoardCaps& c = board_caps();
    const int w = c.width, h = c.height;
    const bool big = w >= 400;
    if (c.round_panel) {
        const int ix = w * 15 / 100, iy = h * 15 / 100;   // inside the inscribed square
        cx = ix; cy = iy; cw = w - 2 * ix; ch = h - 2 * iy;
    } else {
        const int pad = w / 24;
        cx = pad; cy = pad; cw = w - 2 * pad; ch = h - 2 * pad;
    }
    const lv_font_t* f_head = big ? &font_styrene_20 : &font_styrene_16;
    const lv_font_t* f_tool = big ? &font_styrene_28 : &font_styrene_24;
    const lv_font_t* f_text = &font_mono_18;
    const lv_font_t* f_btn  = big ? &font_styrene_28 : &font_styrene_20;
    const int gap = big ? 14 : 8;
    const int radius = big ? 22 : 14;

    // Buttons: a quarter of the screen high (capped to a third of the box),
    // side by side at the bottom.
    bh = h / 4;
    if (bh > ch / 3) bh = ch / 3;
    bw = (cw - gap) / 2;
    by = cy + ch - bh;
    bx_deny = cx;
    bx_allow = cx + bw + gap;
    bar_w_full = cw;

    root = lv_obj_create(parent ? parent : lv_layer_top());
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, w, h);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, THEME_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);

    lbl_head = make_label(f_head, THEME_DIM, root);
    lv_label_set_text(lbl_head, "Claude wants to");
    lv_obj_set_pos(lbl_head, cx, cy);

    const int tool_y = cy + lv_font_get_line_height(f_head) + 4;
    lbl_tool = make_label(f_tool, THEME_ACCENT, root);
    lv_label_set_long_mode(lbl_tool, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(lbl_tool, cw, lv_font_get_line_height(f_tool));
    lv_obj_set_pos(lbl_tool, cx, tool_y);

    // The progress line sits above the buttons; the text fills the room between.
    const int bar_y = by - gap - BAR_H;
    bar_track = lv_obj_create(root);
    make_rect(bar_track, THEME_BAR_BG, BAR_H / 2);
    lv_obj_set_size(bar_track, cw, BAR_H);
    lv_obj_set_pos(bar_track, cx, bar_y);
    bar_fill = lv_obj_create(root);
    make_rect(bar_fill, THEME_ACCENT, BAR_H / 2);
    lv_obj_set_size(bar_fill, cw, BAR_H);
    lv_obj_set_pos(bar_fill, cx, bar_y);

    const int text_y = tool_y + lv_font_get_line_height(f_tool) + (big ? 14 : 8);
    const int line = lv_font_get_line_height(f_text);
    int lines = (bar_y - gap - text_y) / line;
    if (lines > 4) lines = 4;
    if (lines < 1) lines = 1;
    lbl_text = make_label(f_text, THEME_TEXT, root);
    lv_label_set_long_mode(lbl_text, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(lbl_text, cw, lines * line);
    text_box_w = cw;
    text_box_h = lines * line;
    lv_obj_set_pos(lbl_text, cx, text_y);

    btn_deny = make_btn(THEME_RED, 3, radius);
    lv_obj_set_size(btn_deny, bw, bh);
    lv_obj_set_pos(btn_deny, bx_deny, by);
    lbl_deny = make_label(f_btn, THEME_TEXT, btn_deny);
    lv_label_set_text(lbl_deny, "Deny");
    lv_obj_center(lbl_deny);

    btn_allow = make_btn(THEME_GREEN, 3, radius);
    lv_obj_set_size(btn_allow, bw, bh);
    lv_obj_set_pos(btn_allow, bx_allow, by);
    lbl_allow = make_label(f_btn, THEME_TEXT, btn_allow);
    lv_label_set_text(lbl_allow, "Allow");
    lv_obj_center(lbl_allow);

    style_buttons(false, false);
}

// Copy src into dst (cap bytes incl. NUL), at most max_bytes, without cutting
// a UTF-8 sequence; control characters become spaces so the wrap behaves.
static void copy_utf8(char* dst, size_t cap, const char* src, size_t max_bytes) {
    size_t n = src ? strlen(src) : 0;
    if (n > max_bytes) {
        n = max_bytes;
        while (n > 0 && ((uint8_t)src[n] & 0xC0) == 0x80) n--;   // back to a lead byte
    }
    if (n >= cap) n = cap - 1;
    for (size_t i = 0; i < n; i++) dst[i] = ((uint8_t)src[i] < 0x20) ? ' ' : src[i];
    dst[n] = 0;
}

static void set_bar(int w) {
    if (w == bar_w_last) return;
    bar_w_last = w;
    if (w < 1) {
        lv_obj_add_flag(bar_fill, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(bar_fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(bar_fill, w);
    }
}

bool perm_show(uint32_t id, const char* tool, const char* text, uint32_t timeout_ms) {
    if (!root || id == 0) return false;        // no id: nothing to answer
    if (shown && id == cur_id) return true;     // already up: keep its timer and arming
    // The whole command must be readable: a cut-off tail could be the part
    // that matters. Too long for the box: no card, the terminal decides.
    if (!text || strlen(text) > TEXT_MAX) return false;
    lv_point_t sz;
    lv_text_get_size(&sz, text, lv_obj_get_style_text_font(lbl_text, LV_PART_MAIN), 0, 0, text_box_w, LV_TEXT_FLAG_NONE);
    if (sz.y > text_box_h || sz.x > text_box_w) return false;
    char buf[TEXT_MAX + 4];
    copy_utf8(buf, sizeof buf, tool, 31);
    lv_label_set_text(lbl_tool, buf[0] ? buf : "use a tool");
    copy_utf8(buf, sizeof buf, text, TEXT_MAX);
    lv_label_set_text(lbl_text, buf);
    cur_id = id;
    t_start = lv_tick_get();
    t_total = timeout_ms;
    flashing = false;
    bar_w_last = -1;
    style_buttons(false, false);
    if (t_total) {
        lv_obj_clear_flag(bar_track, LV_OBJ_FLAG_HIDDEN);
        set_bar(bar_w_full);
    } else {
        lv_obj_add_flag(bar_track, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(bar_fill, LV_OBJ_FLAG_HIDDEN);
        bar_w_last = 0;
    }
    lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(root);
    shown = true;
    return true;
}

static void do_hide(void) {
    if (!root) return;
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    shown = false;
    flashing = false;
}

void perm_hide(uint32_t id) {
    if (shown && (id == 0 || id == cur_id)) do_hide();
}

bool perm_visible(void) { return shown; }

uint32_t perm_current_id(void) { return cur_id; }

// A card that just appeared ignores taps for a moment: a tap meant for the
// screen underneath (or an older card) must not land on Allow.
#define PERM_ARM_MS 700

int perm_hit(int x, int y) {
    if (!shown || flashing) return 0;
    if (lv_tick_get() - t_start < PERM_ARM_MS) return 0;
    if (y < by || y >= by + bh) return 0;
    if (x >= bx_allow && x < bx_allow + bw) return 1;
    if (x >= bx_deny && x < bx_deny + bw) return 2;
    return 0;
}

void perm_tick(void) {
    if (!shown) return;
    if (lv_obj_get_index(root) != (int32_t)lv_obj_get_child_count(lv_obj_get_parent(root)) - 1)
        lv_obj_move_foreground(root);            // over confetti / pet props made after it
    const uint32_t now = lv_tick_get();
    if (flashing) {
        if (now - t_flash >= FLASH_MS) do_hide();
        return;
    }
    if (!t_total) return;
    const uint32_t el = now - t_start;
    if (el >= t_total) { do_hide(); return; }   // timed out: no answer, the host gives up too
    set_bar((int)((uint64_t)bar_w_full * (t_total - el) / t_total));
}

bool perm_take_answer(uint32_t* id, bool* allow) {
    if (!ans_ready) return false;
    ans_ready = false;
    if (id) *id = ans_id;
    if (allow) *allow = ans_allow;
    return true;
}

void perm_answer(bool allow) {
    if (!shown || flashing) return;
    ans_id = cur_id;
    ans_allow = allow;
    ans_ready = true;
    flashing = true;
    t_flash = lv_tick_get();
    style_buttons(allow, !allow);
}
