#include "portal.h"
#include "account_tint.h"
#include "field.h"
#include "theme.h"
#include "hal/board_caps.h"
#include <lvgl.h>
#include <Arduino.h>
#include <math.h>
#include <string.h>

// Timing, ms. Each cell flashes in the account colour for FLASH_MS as it
// arrives (cover) or leaves (reveal); the swap and the full redraw happen in
// the HOLD_MS while everything is dark.
#define COVER_MS   300
#define HOLD_MS     80
#define REVEAL_MS  340
#define FLASH_MS    90
#define CELL_PX     40     // on a 480 panel: a 12 x 12 grid
#define MAX_CELLS  256
#define MAX_INV     24     // more changed cells in one frame: one full redraw instead

enum { C_OFF = 0, C_GLOW = 1, C_DARK = 2 };

static lv_obj_t*  obj = NULL;
static lv_timer_t* timer = NULL;
static int        W = 480, H = 480, cols = 12, rows = 12, cw = 40, ch = 40;
static uint8_t    rank[MAX_CELLS];    // 0 = centre of the swirl, 255 = its far end
static uint8_t    state[MAX_CELLS];
static bool       running = false, swapped = false;
static uint32_t   started = 0;
static void     (*swap_cb)(void) = NULL;

static lv_color_t glow(void) { return g_account_tint ? NEON_ACCENT : THEME_ACCENT; }

static void cell_area(int i, lv_area_t* a) {
    const int c = i % cols, r = i / cols;
    a->x1 = c * W / cols;       a->x2 = (c + 1) * W / cols - 1;
    a->y1 = r * H / rows;       a->y2 = (r + 1) * H / rows - 1;
}

static void draw_cb(lv_event_t* e) {
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_COVER;
    d.radius = 0;
    const lv_color_t g = glow();
    const lv_color_t spark = lv_color_mix(lv_color_white(), g, 110);
    const int n = cols * rows;
    for (int i = 0; i < n; i++) {
        if (state[i] == C_OFF) continue;
        // A few glowing cells are brighter: sparks in the swirl.
        d.bg_color = state[i] == C_DARK ? THEME_BG : ((i * 37 + rank[i]) % 7 == 0 ? spark : g);
        lv_area_t a;
        cell_area(i, &a);
        lv_draw_rect(layer, &d, &a);
    }
}

static void do_show(void* o)  { lv_obj_clear_flag((lv_obj_t*)o, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground((lv_obj_t*)o); }
static void do_hide(void* o)  { lv_obj_add_flag((lv_obj_t*)o, LV_OBJ_FLAG_HIDDEN); }

void portal_init(void) {
    const BoardCaps& caps = board_caps();
    W = caps.width; H = caps.height;
    cols = (W + CELL_PX / 2) / CELL_PX; rows = (H + CELL_PX / 2) / CELL_PX;
    if (cols < 4) cols = 4;
    if (rows < 4) rows = 4;
    while (cols * rows > MAX_CELLS) { cols--; rows--; }
    cw = W / cols; ch = H / rows;
    obj = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, W, H);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(obj, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
}

// Spiral order: mostly distance from the centre, bent into two arms turning
// the way the finger went, with a little noise so the edge looks pixelated.
static void build_order(int dir, uint32_t seed) {
    const int n = cols * rows;
    float key[MAX_CELLS];
    float lo = 1e9f, hi = -1e9f;
    const float cx = (cols - 1) / 2.0f, cy = (rows - 1) / 2.0f;
    const float rmax = sqrtf(cx * cx + cy * cy);
    uint32_t s = seed | 1;
    for (int i = 0; i < n; i++) {
        const float dx = (i % cols) - cx, dy = (i / cols) - cy;
        const float r = sqrtf(dx * dx + dy * dy) / rmax;
        float a = atan2f(dy, dx) / 6.2831853f + 0.5f;           // 0..1 around
        float arm = a * 2.0f + (dir >= 0 ? 1.0f : -1.0f) * r * 1.3f;
        arm -= floorf(arm);
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        const float noise = (s % 1000) / 1000.0f;
        key[i] = 0.70f * r + 0.22f * arm + 0.08f * noise;
        if (key[i] < lo) lo = key[i];
        if (key[i] > hi) hi = key[i];
    }
    const float span = hi > lo ? hi - lo : 1.0f;
    for (int i = 0; i < n; i++) rank[i] = (uint8_t)((key[i] - lo) / span * 255.0f + 0.5f);
}

static uint8_t want_state(int i, uint32_t t) {
    if (t < COVER_MS) {                  // outside in: the far end of the swirl first
        const uint32_t at = (uint32_t)(255 - rank[i]) * (COVER_MS - FLASH_MS) / 255;
        return t < at ? C_OFF : t < at + FLASH_MS ? C_GLOW : C_DARK;
    }
    if (t < COVER_MS + HOLD_MS) return C_DARK;
    const uint32_t u = t - COVER_MS - HOLD_MS;   // centre out: the portal opens
    const uint32_t at = (uint32_t)rank[i] * (REVEAL_MS - FLASH_MS) / 255;
    return u < at ? C_DARK : u < at + FLASH_MS ? C_GLOW : C_OFF;
}

static void finish(void) {
    running = false;
    if (timer) { lv_timer_delete(timer); timer = NULL; }
    memset(state, 0, sizeof(state));
    field_quietly(obj, do_hide, obj);   // nothing drawn any more: hiding changes no pixel
}

static void step(lv_timer_t* tm) {
    (void)tm;
    if (!running) return;
    const uint32_t t = millis() - started;
    if (!swapped && t >= COVER_MS) {
        swapped = true;
        for (int i = 0; i < cols * rows; i++) state[i] = C_DARK;
        if (swap_cb) swap_cb();
        swap_cb = NULL;
        lv_obj_move_foreground(obj);   // a page the swap raised stays under the cells
        // A fresh page underneath: redraw all of it once, while it's covered.
        lv_obj_invalidate(lv_screen_active());
        lv_obj_invalidate(obj);
        return;
    }
    if (t >= COVER_MS + HOLD_MS + REVEAL_MS) {
        for (int i = 0; i < cols * rows; i++) {
            if (state[i] == C_OFF) continue;
            lv_area_t a; cell_area(i, &a);
            lv_obj_invalidate_area(obj, &a);
            state[i] = C_OFF;
        }
        finish();
        return;
    }
    int changed[MAX_INV];
    int nchanged = 0;
    bool full = false;
    for (int i = 0; i < cols * rows; i++) {
        const uint8_t w = want_state(i, t);
        if (w == state[i]) continue;
        state[i] = w;
        if (nchanged < MAX_INV) changed[nchanged++] = i;
        else full = true;
    }
    if (full) { lv_obj_invalidate(obj); return; }
    for (int k = 0; k < nchanged; k++) {
        lv_area_t a; cell_area(changed[k], &a);
        lv_obj_invalidate_area(obj, &a);
    }
}

bool portal_start(int dir, void (*swap)(void)) {
    if (!obj || running) { if (swap) swap(); return false; }
    build_order(dir, millis() * 2654435761u);
    memset(state, 0, sizeof(state));
    swap_cb = swap;
    swapped = false;
    running = true;
    started = millis();
    field_quietly(obj, do_show, obj);   // draws nothing yet: no redraw
    timer = lv_timer_create(step, 15, NULL);
    return true;
}

bool portal_active(void) { return running; }
