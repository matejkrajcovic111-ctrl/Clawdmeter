#include "pet_props.h"
#include "clawd_policy.h"
#include "theme.h"
#include "hal/board_caps.h"
#include <lvgl.h>
#include <Arduino.h>

LV_FONT_DECLARE(font_styrene_28);

// Fixed pool, built once: 1 drop, 3 z labels, 18 confetti, 1 dot, 1 pill.
// Every anim's value is a plain 0..N progress, and its exec callback derives
// position, scale and opacity from it, so a prop is one anim and one callback.
#define N_Z      3
#define N_CONF   18
#define Z_CYCLE  2400
#define Z_STAGGER 650
#define SWEAT_MS 1000
#define SWEAT_GAP 600
#define ORBIT_MS 2000

#define COL_INK     lv_color_hex(0x141413)    // same dark as the eyes
#define COL_DROP    lv_color_hex(0x8fd3ff)

static int mind = 480, scr_h = 480;
static int S(int v) { return v * mind / 480; }

static lv_obj_t* drop = NULL;
static lv_obj_t* zl[N_Z];
static lv_obj_t* conf[N_CONF];
static lv_obj_t* dot = NULL;
static lv_obj_t* pill = NULL;
static lv_obj_t* pill_label = NULL;

static bool     neon = false;
static ClawdRng rng;

// Per-prop anchors, read by the exec callbacks.
static int sweat_x, sweat_y;
static int z_x[N_Z], z_y[N_Z];
static int z_idx[N_Z] = { 0, 1, 2 };
static int conf_idx[N_CONF];
static int conf_x[N_CONF], conf_drift[N_CONF];
static int orbit_cx, orbit_cy, orbit_r;
static bool orbiting = false;
static int dot_x, dot_y;                // current centre
static int pill_ms;                     // bubble lifetime
static int anim_tag[8];                 // unique anim handles for single props

static lv_color_t ivory(void) { return neon ? NEON_TEXT : THEME_TEXT; }
static lv_color_t dim_ivory(void) { return neon ? NEON_DIM : THEME_DIM; }

static lv_obj_t* base_obj(void) {
    lv_obj_t* o = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

static lv_obj_t* box(int w, int h, lv_color_t c, int radius) {
    lv_obj_t* o = base_obj();
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    return o;
}

static lv_obj_t* text_label(lv_obj_t* parent, lv_color_t c) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_remove_style_all(l);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_font(l, &font_styrene_28, 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}

static void show(lv_obj_t* o) { lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN); }
static void hide(lv_obj_t* o) { lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); }

// Opacity ramp: in over the first `in` permille, out over the last `out`.
static lv_opa_t env(int v, int in, int out) {
    int a = 255;
    if (v < in) a = 255 * v / in;
    else if (v > 1000 - out) a = 255 * (1000 - v) / out;
    return (lv_opa_t)(a < 0 ? 0 : a > 255 ? 255 : a);
}

static void anim_run(void* var, lv_anim_exec_xcb_t exec, int32_t from, int32_t to,
                     uint32_t ms, uint32_t delay, bool loop, uint32_t loop_gap,
                     lv_anim_completed_cb_t done) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, var);
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    if (loop) {
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_repeat_delay(&a, loop_gap);
    }
    if (done) lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}

// sweat

static void sweat_exec(void* var, int32_t v) {
    (void)var;
    lv_obj_set_pos(drop, sweat_x - lv_obj_get_width(drop) / 2,
                   sweat_y - lv_obj_get_height(drop) / 2 + S(70) * v / 1000);
    lv_obj_set_style_opa(drop, env(v, 120, 450), 0);
}

void props_sweat(bool on, int x, int y) {
    if (!drop) return;
    lv_anim_delete(&anim_tag[0], NULL);
    if (!on) { hide(drop); return; }
    sweat_x = x; sweat_y = y;
    sweat_exec(NULL, 0);
    show(drop);
    anim_run(&anim_tag[0], sweat_exec, 0, 1000, SWEAT_MS, 0, true, SWEAT_GAP, NULL);
}

// zzz

static void z_exec(void* var, int32_t v) {
    const int i = *(int*)var;
    lv_obj_t* l = zl[i];
    // Later letters travel a little further and end bigger: z, z, Z.
    const int reach = 1000 + i * 250;
    // No scale transform: a transformed layer can't get its buffer when
    // memory is tight and LVGL then crashes drawing it. Drift and fade only.
    lv_obj_set_pos(l, z_x[i] - lv_obj_get_width(l) / 2 + S(46) * reach / 1000 * v / 1000,
                      z_y[i] - lv_obj_get_height(l) / 2 - S(64) * reach / 1000 * v / 1000);
    lv_obj_set_style_text_opa(l, env(v, 180, 400), 0);
}

void props_zzz(bool on, int x, int y) {
    if (!zl[0]) return;
    for (int i = 0; i < N_Z; i++) {
        lv_anim_delete(&z_idx[i], NULL);
        hide(zl[i]);
    }
    if (!on) return;
    for (int i = 0; i < N_Z; i++) {
        z_x[i] = x; z_y[i] = y;
        z_exec(&z_idx[i], 0);
        show(zl[i]);
        anim_run(&z_idx[i], z_exec, 0, 1000, Z_CYCLE, (uint32_t)i * Z_STAGGER, true, 0, NULL);
    }
}

// confetti

static void conf_exec(void* var, int32_t v) {
    const int i = *(int*)var;
    const int h = S(14);
    lv_obj_set_pos(conf[i], conf_x[i] + conf_drift[i] * v / 1000,
                   -h + (scr_h + 2 * h) * v / 1000);
}

static void conf_done(lv_anim_t* a) { hide(conf[*(int*)a->var]); }

void props_confetti(void) {
    if (!conf[0]) return;
    const BoardCaps& c = board_caps();
    const lv_color_t pal[6] = {
        neon ? NEON_ACCENT : THEME_ACCENT, lv_color_hex(0xffd23f), lv_color_hex(0x4dd0e1),
        lv_color_hex(0xff6fae), lv_color_hex(0x9be564), ivory(),
    };
    for (int i = 0; i < N_CONF; i++) {
        lv_anim_delete(&conf_idx[i], NULL);
        conf_x[i] = (int)clawd_rng_range(&rng, 0, (uint32_t)(c.width - S(8)));
        conf_drift[i] = (int)clawd_rng_range(&rng, 0, (uint32_t)S(80)) - S(40);
        lv_obj_set_style_bg_color(conf[i], pal[clawd_rng_range(&rng, 0, 5)], 0);
        conf_exec(&conf_idx[i], 0);
        show(conf[i]);
        anim_run(&conf_idx[i], conf_exec, 0, 1000,
                 clawd_rng_range(&rng, 2200, 3000), clawd_rng_range(&rng, 0, 300),
                 false, 0, conf_done);
    }
}

// thinking

static void dot_place(int deg) {
    dot_x = orbit_cx + (int)((int64_t)lv_trigo_cos(deg) * orbit_r / 32768);
    dot_y = orbit_cy + (int)((int64_t)lv_trigo_sin(deg) * orbit_r / 32768);
    lv_obj_set_pos(dot, dot_x - lv_obj_get_width(dot) / 2, dot_y - lv_obj_get_height(dot) / 2);
}

static void dot_exec(void* var, int32_t v) { (void)var; dot_place(v); }

void props_thinking(bool on, int cx, int cy, int r) {
    if (!dot) return;
    lv_anim_delete(&anim_tag[1], NULL);
    orbiting = on;
    if (!on) { hide(dot); return; }
    orbit_cx = cx; orbit_cy = cy; orbit_r = r;
    dot_place(0);
    show(dot);
    anim_run(&anim_tag[1], dot_exec, 0, 360, ORBIT_MS, 0, true, 0, NULL);
}

bool props_thinking_pos(int* x, int* y) {
    if (!orbiting) return false;
    if (x) *x = dot_x;
    if (y) *y = dot_y;
    return true;
}

// bubble

static void pill_exec(void* var, int32_t t) {
    (void)var;
    // Pop: hops up past its spot and settles back in 220 ms, then fades.
    // (No scale transform or whole-object opacity: both need a layer buffer,
    // and LVGL crashed drawing one under rapid taps when memory ran short.)
    int dy = 0;
    if (t < 140)      dy = S(18) - S(24) * t / 140;
    else if (t < 220) dy = -S(6) + S(6) * (t - 140) / 80;
    lv_obj_set_style_translate_y(pill, dy, 0);
    int a = 255;
    if (t < 120) a = 255 * t / 120;
    if (t > pill_ms - 300) a = 255 * (pill_ms - t) / 300;
    const lv_opa_t o = (lv_opa_t)(a < 0 ? 0 : a > 255 ? 255 : a);
    lv_obj_set_style_bg_opa(pill, o, 0);
    lv_obj_set_style_border_opa(pill, o, 0);
    lv_obj_set_style_text_opa(pill_label, o, 0);
}

static void pill_done(lv_anim_t* a) { (void)a; hide(pill); }

void props_bubble(const char* text, int x, int y, uint32_t ms) {
    if (!pill) return;
    lv_anim_delete(&anim_tag[2], NULL);
    lv_label_set_text(pill_label, text ? text : "");
    lv_obj_update_layout(pill);
    const int w = lv_obj_get_width(pill), h = lv_obj_get_height(pill);
    lv_obj_set_style_transform_pivot_x(pill, w / 2, 0);
    lv_obj_set_style_transform_pivot_y(pill, h / 2, 0);
    lv_obj_set_pos(pill, x - w / 2, y - h / 2);
    pill_ms = ms < 600 ? 600 : (int)ms;
    pill_exec(NULL, 0);
    show(pill);
    anim_run(&anim_tag[2], pill_exec, 0, pill_ms, (uint32_t)pill_ms, 0, false, 0, pill_done);
}

// shared

void props_set_neon(bool n) {
    neon = n;
    if (!pill) return;
    for (int i = 0; i < N_Z; i++) lv_obj_set_style_text_color(zl[i], dim_ivory(), 0);
    lv_obj_set_style_bg_color(dot, ivory(), 0);
    lv_obj_set_style_bg_color(pill, ivory(), 0);
}

void props_to_front(void) {
    if (!pill) return;
    lv_obj_move_foreground(drop);
    for (int i = 0; i < N_Z; i++) lv_obj_move_foreground(zl[i]);
    for (int i = 0; i < N_CONF; i++) lv_obj_move_foreground(conf[i]);
    lv_obj_move_foreground(dot);
    lv_obj_move_foreground(pill);          // the speech pill stays on top
}

void props_hide_all(void) {
    if (!pill) return;
    lv_anim_delete(&anim_tag[0], NULL);
    lv_anim_delete(&anim_tag[1], NULL);
    lv_anim_delete(&anim_tag[2], NULL);
    orbiting = false;
    hide(drop); hide(dot); hide(pill);
    for (int i = 0; i < N_Z; i++) { lv_anim_delete(&z_idx[i], NULL); hide(zl[i]); }
    for (int i = 0; i < N_CONF; i++) { lv_anim_delete(&conf_idx[i], NULL); hide(conf[i]); }
}

void props_init(void) {
    if (pill) return;
    const BoardCaps& c = board_caps();
    mind = c.width < c.height ? c.width : c.height;
    scr_h = c.height;
    clawd_rng_seed(&rng, millis() ^ 0xC0FFEE);

    drop = box(S(18) > 2 ? S(18) : 2, S(26) > 2 ? S(26) : 2, COL_DROP, S(9));
    for (int i = 0; i < N_Z; i++) {
        zl[i] = text_label(lv_layer_top(), dim_ivory());
        lv_obj_add_flag(zl[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(zl[i], i == N_Z - 1 ? "Z" : "z");
        lv_obj_update_layout(zl[i]);
        lv_obj_set_style_transform_pivot_x(zl[i], lv_obj_get_width(zl[i]) / 2, 0);
        lv_obj_set_style_transform_pivot_y(zl[i], lv_obj_get_height(zl[i]) / 2, 0);
        lv_obj_set_style_opa(zl[i], LV_OPA_TRANSP, 0);
    }
    for (int i = 0; i < N_CONF; i++) {
        conf_idx[i] = i;
        conf[i] = box(S(8) > 1 ? S(8) : 1, S(14) > 1 ? S(14) : 1, THEME_TEXT, 0);
    }
    const int d = S(14) > 2 ? S(14) : 2;
    dot = box(d, d, ivory(), LV_RADIUS_CIRCLE);

    pill = box(10, 10, ivory(), LV_RADIUS_CIRCLE);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(pill, S(22), 0);
    lv_obj_set_style_pad_ver(pill, S(10), 0);
    pill_label = text_label(pill, COL_INK);
    lv_label_set_text(pill_label, "");
    props_to_front();
}
