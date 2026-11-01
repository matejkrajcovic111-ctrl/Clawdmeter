#include "savers.h"
#include "hal/board_caps.h"
#include "idle.h"
#include <Arduino.h>
#include <lvgl.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

LV_FONT_DECLARE(font_mono_18);


#define PEAK_OPA   178          // ~70% of full
#define PI_F       3.14159265f

static lv_obj_t* obj = NULL;
static int W = 480, H = 480;
static float scale = 1.0f, CX = 240, CY = 240, RAD = 240;
static bool round_panel = true;
static int kind_cur = -1;
static uint32_t last_ms = 0, t0_ms = 0, rng_s = 2463534242u;
static float T = 0;             // seconds since start

static uint32_t rnd(void) { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 17; rng_s ^= rng_s << 5; return rng_s; }
static float rndf(float a, float b) { return a + (rnd() & 0xFFFF) * (b - a) / 65535.0f; }

static const uint32_t C_ORANGE = 0xD77757, C_SHIMMER = 0xEB9F7F, C_AMBER = 0xFFC107, C_BLUE = 0x4AA3FF;

static const char* const NAMES[SAVER_COUNT] = { "starfield", "code rain", "constellation" };
static const uint16_t FRAME_MS_K[SAVER_COUNT] = { 33, 50, 50 };

// ---------------------------------------------------------------- starfield
#define NSTAR 90
struct Star { float x, y, z; uint8_t tint; };
static Star stars[NSTAR];

static void star_respawn(Star& s, bool anywhere) {
    const float a = rndf(0, 2 * PI_F), r = sqrtf(rndf(0.02f, 1.0f));
    s.x = cosf(a) * r; s.y = sinf(a) * r;
    s.z = anywhere ? rndf(0.08f, 1.0f) : rndf(0.85f, 1.0f);
    s.tint = rnd() % 3 ? 0 : 1;      // 0 near-white, 1 orange
}
static void stars_init(void) { for (int i = 0; i < NSTAR; i++) star_respawn(stars[i], true); }
static void stars_step(float dt) {
    for (int i = 0; i < NSTAR; i++) {
        Star& s = stars[i];
        s.z -= (0.22f + 0.5f * (1.0f - s.z)) * dt;      // accelerate toward the viewer
        const float inv = 1.0f / s.z;
        if (s.z < 0.04f || fabsf(s.x * inv) > 2.3f || fabsf(s.y * inv) > 2.3f) star_respawn(s, false);
    }
}
static void stars_draw(lv_layer_t* layer) {
    lv_draw_line_dsc_t l; lv_draw_line_dsc_init(&l);
    lv_draw_rect_dsc_t r; lv_draw_rect_dsc_init(&r);
    r.radius = LV_RADIUS_CIRCLE;
    const float f = RAD * 0.5f;
    for (int i = 0; i < NSTAR; i++) {
        const Star& s = stars[i];
        const float inv = 1.0f / s.z;
        const float x = CX + s.x * inv * f, y = CY + s.y * inv * f;
        const float zp = s.z + 0.03f + 0.10f * (1.0f - s.z);
        const float ip = 1.0f / zp;
        const float px = CX + s.x * ip * f, py = CY + s.y * ip * f;
        if (x < -4 || y < -4 || x > W + 4 || y > H + 4) continue;
        const float b = 1.0f - s.z;
        const int opa = 30 + (int)(b * (PEAK_OPA - 30));
        const lv_color_t c = s.tint ? lv_color_hex(C_SHIMMER) : lv_color_mix(lv_color_hex(C_ORANGE), lv_color_white(), 40);
        const float dx = x - px, dy = y - py;
        int sw = (b > 0.6f ? 3 : (b > 0.3f ? 2 : 1));
        if (scale < 0.7f && sw > 1) sw--;
        if (dx * dx + dy * dy < 4.0f) {
            r.bg_color = c; r.bg_opa = opa;
            const int rr = sw > 1 ? 1 : 0;
            lv_area_t a = { (int)x - rr, (int)y - rr, (int)x + rr, (int)y + rr };
            lv_draw_rect(layer, &r, &a);
        } else {
            l.color = c; l.opa = opa; l.width = sw;
            l.p1.x = px; l.p1.y = py; l.p2.x = x; l.p2.y = y;
            lv_draw_line(layer, &l);
        }
    }
}

// ---------------------------------------------------------------- code rain
#define NSTREAM 26
#define MAXLEN  20
struct RainStream { int col; float head, speed; int len; char ch[MAXLEN][2]; };
static RainStream st[NSTREAM];
static int rain_cw = 11, rain_ch = 20, rain_cols = 40;
static const lv_font_t* rain_font;

static char rand_char(void) { return (char)(33 + rnd() % 94); }

static void stream_respawn(RainStream& s, bool initial) {
    s.len = 0;                       // do not clash with itself
    for (int tries = 0; tries < 8; tries++) {
        s.col = (int)(rnd() % rain_cols);
        bool clash = false;
        for (int k = 0; k < NSTREAM; k++)
            if (st[k].len && st[k].col == s.col) clash = true;
        if (!clash) break;
    }
    s.len = 6 + (int)(rnd() % (MAXLEN - 6));
    s.speed = rndf(5, 12);
    const int rows = H / rain_ch;
    s.head = initial ? rndf(-(float)rows, (float)rows) : -rndf(1, 30);
    for (int i = 0; i < MAXLEN; i++) { s.ch[i][0] = rand_char(); s.ch[i][1] = 0; }
}
static void rain_init(void) {
    rain_font = &font_mono_18;
    rain_cw = lv_font_get_glyph_width(rain_font, 'M', 0);
    if (rain_cw < 4) rain_cw = 11;
    rain_ch = rain_font->line_height;
    rain_cols = W / rain_cw;
    for (int i = 0; i < NSTREAM; i++) st[i].len = 0;
    for (int i = 0; i < NSTREAM; i++) stream_respawn(st[i], true);
}
static void rain_step(float dt) {
    const int rows = H / rain_ch + 1;
    for (int i = 0; i < NSTREAM; i++) {
        RainStream& s = st[i];
        const int before = (int)floorf(s.head);
        s.head += s.speed * dt;
        const int steps = (int)floorf(s.head) - before;
        for (int k = 0; k < steps && k < 3; k++) {
            memmove(&s.ch[1][0], &s.ch[0][0], (MAXLEN - 1) * 2);
            s.ch[0][0] = rand_char(); s.ch[0][1] = 0;
        }
        if (rnd() % 6 == 0) s.ch[1 + rnd() % (MAXLEN - 1)][0] = rand_char();   // flicker tail
        if (s.head - s.len > rows) stream_respawn(s, false);
    }
}
static void rain_draw(lv_layer_t* layer) {
    lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d);
    d.font = rain_font; d.align = LV_TEXT_ALIGN_LEFT;
    const lv_color_t head = lv_color_hex(C_SHIMMER), mid = lv_color_hex(C_ORANGE), dark = lv_color_hex(0x4A2418);
    const int x0 = (W - rain_cols * rain_cw) / 2;
    const float rr = RAD + rain_ch * 0.5f;
    const lv_area_t clip = layer->_clip_area;
    for (int i = 0; i < NSTREAM; i++) {
        const RainStream& s = st[i];
        const int hr = (int)floorf(s.head);
        const int x = x0 + s.col * rain_cw;
        for (int k = 0; k < s.len; k++) {
            const int y = (hr - k) * rain_ch;
            if (y < -rain_ch || y >= H) continue;
            // Only the glyphs in the strip being drawn: LVGL renders in strips,
            // and queueing every glyph for every strip can run its pool dry.
            if (y + rain_ch < clip.y1 || y > clip.y2 || x + rain_cw + 2 < clip.x1 || x > clip.x2) continue;
            if (round_panel) {
                const float dx = x + rain_cw * 0.5f - CX, dy = y + rain_ch * 0.5f - CY;
                if (dx * dx + dy * dy > rr * rr) continue;
            }
            const float t = (float)k / s.len;
            if (k == 0)      { d.color = head; d.opa = PEAK_OPA; }
            else if (k == 1) { d.color = lv_color_mix(head, mid, 90); d.opa = 160; }
            else {
                d.color = lv_color_mix(dark, mid, (uint8_t)(t * 255));
                d.opa = (lv_opa_t)(140 * (1.0f - t) + 22);
            }
            d.text = s.ch[k];
            lv_area_t a = { x, y, x + rain_cw + 2, y + rain_ch };
            lv_draw_label(layer, &d, &a);
        }
    }
}

// ---------------------------------------------------------------- constellation
#define NDOT 40
struct Dot { float x, y, vx, vy; uint8_t blue; };
static Dot dots[NDOT];
static void dots_init(void) {
    for (int i = 0; i < NDOT; i++) {
        Dot& d = dots[i];
        d.x = rndf(0, (float)W); d.y = rndf(0, (float)H);
        const float a = rndf(0, 2 * PI_F), sp = rndf(6, 18) * scale;
        d.vx = cosf(a) * sp; d.vy = sinf(a) * sp;
        d.blue = (rnd() % 3) != 0;
    }
}
static void dots_step(float dt) {
    const float m = 10 * scale;
    for (int i = 0; i < NDOT; i++) {
        Dot& d = dots[i];
        d.x += d.vx * dt; d.y += d.vy * dt;
        if (d.x < -m) d.x += W + 2 * m; else if (d.x > W + m) d.x -= W + 2 * m;
        if (d.y < -m) d.y += H + 2 * m; else if (d.y > H + m) d.y -= H + 2 * m;
    }
}
static void dots_draw(lv_layer_t* layer) {
    lv_draw_line_dsc_t l; lv_draw_line_dsc_init(&l);
    l.width = 1;
    const float D = 90.0f * scale, D2 = D * D;
    const lv_color_t cb = lv_color_hex(C_BLUE), co = lv_color_hex(C_ORANGE);
    for (int i = 0; i < NDOT; i++)
        for (int j = i + 1; j < NDOT; j++) {
            const float dx = dots[i].x - dots[j].x, dy = dots[i].y - dots[j].y;
            const float q = dx * dx + dy * dy;
            if (q >= D2) continue;
            const float f = 1.0f - sqrtf(q) / D;
            l.opa = (lv_opa_t)(10 + 100 * f);
            l.color = dots[i].blue == dots[j].blue ? (dots[i].blue ? cb : co) : lv_color_mix(cb, co, 128);
            l.p1.x = dots[i].x; l.p1.y = dots[i].y; l.p2.x = dots[j].x; l.p2.y = dots[j].y;
            lv_draw_line(layer, &l);
        }
    lv_draw_rect_dsc_t r; lv_draw_rect_dsc_init(&r);
    r.radius = LV_RADIUS_CIRCLE; r.bg_opa = 150;
    const int rr = scale < 0.7f ? 1 : 2;
    for (int i = 0; i < NDOT; i++) {
        r.bg_color = dots[i].blue ? cb : co;
        lv_area_t a = { (int)dots[i].x - rr, (int)dots[i].y - rr, (int)dots[i].x + rr, (int)dots[i].y + rr };
        lv_draw_rect(layer, &r, &a);
    }
}

// ---------------------------------------------------------------- glue
static void draw_cb(lv_event_t* e) {
    lv_layer_t* layer = lv_event_get_layer(e);
    switch (kind_cur) {
        case 0: stars_draw(layer); break;
        case 1: rain_draw(layer); break;
        case 2: dots_draw(layer); break;
        default: break;
    }
}

void saver_init(void* parent) {
    const BoardCaps& caps = board_caps();
    W = caps.width; H = caps.height;
    round_panel = caps.round_panel;
    scale = H / 480.0f;
    CX = W * 0.5f; CY = H * 0.5f;
    RAD = (W < H ? W : H) * 0.5f;
    obj = lv_obj_create(parent ? (lv_obj_t*)parent : lv_layer_top());
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, W, H);
    lv_obj_set_style_bg_color(obj, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(obj, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    rng_s ^= micros() | 1;
}

void saver_start(int kind) {
    if (!obj || kind < 0 || kind >= SAVER_COUNT) return;
    rng_s ^= micros() | 1;
    kind_cur = kind;
    switch (kind) {
        case 0: stars_init(); break;
        case 1: rain_init(); break;
        case 2: dots_init(); break;
    }
    t0_ms = last_ms = millis();
    T = 0;
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(obj);
    lv_obj_invalidate(obj);
}

void saver_stop(void) {
    if (!obj || kind_cur < 0) return;
    kind_cur = -1;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);     // hiding repaints what was under it
}

bool saver_active(void) { return kind_cur >= 0; }
int saver_kind(void) { return kind_cur; }
const char* saver_name(int kind) { return (kind >= 0 && kind < SAVER_COUNT) ? NAMES[kind] : ""; }

void saver_tick(void) {
    if (!obj || kind_cur < 0) return;
    if (idle_is_asleep()) return;   // the panel is dark: don't redraw it 30x a second
    const uint32_t now = millis();
    if (now - last_ms < FRAME_MS_K[kind_cur]) return;
    float dt = (now - last_ms) / 1000.0f;
    if (dt > 0.2f) dt = 0.2f;
    last_ms = now;
    T = (now - t0_ms) / 1000.0f;
    switch (kind_cur) {
        case 0: stars_step(dt); break;
        case 1: rain_step(dt); break;
        case 2: dots_step(dt); break;
    }
    lv_obj_invalidate(obj);
}
