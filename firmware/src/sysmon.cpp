#include "sysmon.h"
#include "theme.h"
#include "layout.h"
#include "ble.h"
#include "brightness.h"
#include "build_info.h"
#include "hal/board_caps.h"
#include "hal/power_hal.h"
#include <lvgl.h>
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <stdio.h>

LV_FONT_DECLARE(font_mono_18);

#define REFRESH_MS 1000

// One line of the page: a label on the left, its value on the right, and
// for the gauges a square bar under them (the /usage look).
enum RowId { R_CPU, R_MEM, R_PSRAM, R_FLASH, R_TEMP, R_BATT, R_BLE, R_UP, R_BRIGHT, R_FW, R_COUNT };
static const char* const LABELS[R_COUNT] = {
    "Processor", "Memory", "PSRAM", "App flash", "Temperature",
    "Battery", "Bluetooth", "Uptime", "Brightness", "Firmware",
};
static const bool GAUGE[R_COUNT] = { true, true, true, true, true, true, false, false, false, false };

struct Row { lv_obj_t *label, *value, *track, *fill; bool fits; };

static lv_obj_t* root;
static lv_obj_t* lbl_head;
static lv_obj_t* lbl_chip;
static Row rows[R_COUNT];
static int cx, cy, cw, ch, line_h, bar_h;
static bool shown;
static uint32_t refreshed_at;
static uint32_t sketch_used, sketch_total;   // read once: slow-ish and fixed

static lv_obj_t* label(void) {
    lv_obj_t* l = lv_label_create(root);
    lv_obj_set_style_text_font(l, &font_mono_18, 0);
    return l;
}

static lv_obj_t* rect(void) {
    lv_obj_t* r = lv_obj_create(root);
    lv_obj_remove_style_all(r);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    return r;
}

void sysmon_init(void) {
    if (root) return;
    const BoardCaps& c = board_caps();
    const int w = c.width, h = c.height;
    if (c.round_panel) {
        const int ix = w * 15 / 100, iy = h * 15 / 100;
        cx = ix; cy = iy; cw = w - 2 * ix; ch = h - 2 * iy;
    } else {
        const int pad = w / 24;
        cx = pad; cy = pad; cw = w - 2 * pad; ch = h - 2 * pad;
    }
    line_h = lv_font_get_line_height(&font_mono_18);
    bar_h = line_h / 4 > 3 ? line_h / 4 : 3;

    root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, w, h);
    lv_obj_set_style_bg_color(root, THEME_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);

    lbl_head = label();
    lv_label_set_text(lbl_head, "This device");
    lv_obj_set_style_text_color(lbl_head, THEME_TEXT, 0);
    lv_obj_set_pos(lbl_head, cx, cy);
    lbl_chip = label();
    lv_obj_set_style_text_color(lbl_chip, THEME_ACCENT, 0);

    // Rows top to bottom, spread over the panel's height; what doesn't fit a
    // small panel is left out.
    int y = cy + line_h + line_h / 2;
    const int bottom = cy + ch;
    int total = 0;
    for (int i = 0; i < R_COUNT; i++) total += line_h + (GAUGE[i] ? 3 + bar_h : 0);
    int gap = (bottom - y - total) / (R_COUNT - 1);
    if (gap < line_h / 3) gap = line_h / 3;
    if (gap > line_h) gap = line_h;
    for (int i = 0; i < R_COUNT; i++) {
        Row& r = rows[i];
        const int need = line_h + (GAUGE[i] ? 3 + bar_h : 0);
        r.fits = y + need <= bottom;
        r.label = label();
        r.value = label();
        lv_label_set_text(r.label, LABELS[i]);
        lv_obj_set_style_text_color(r.label, THEME_DIM, 0);
        lv_obj_set_style_text_color(r.value, THEME_TEXT, 0);
        lv_obj_set_pos(r.label, cx, y);
        r.track = r.fill = nullptr;
        if (GAUGE[i]) {
            r.track = rect();
            r.fill = rect();
            lv_obj_set_style_bg_color(r.track, THEME_BAR_BG, 0);
            lv_obj_set_size(r.track, cw, bar_h);
            lv_obj_set_pos(r.track, cx, y + line_h + 3);
            lv_obj_set_pos(r.fill, cx, y + line_h + 3);
            lv_obj_set_size(r.fill, 0, bar_h);
        }
        if (!r.fits) {
            lv_obj_add_flag(r.label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(r.value, LV_OBJ_FLAG_HIDDEN);
            if (r.track) { lv_obj_add_flag(r.track, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(r.fill, LV_OBJ_FLAG_HIDDEN); }
        }
        y += need + gap;
    }
    sketch_used = ESP.getSketchSize();
    sketch_total = sketch_used + ESP.getFreeSketchSpace();
}

static void set_value(RowId id, const char* text) {
    Row& r = rows[id];
    lv_label_set_text(r.value, text);
    lv_obj_align(r.value, LV_ALIGN_TOP_RIGHT, -(board_caps().width - cx - cw), lv_obj_get_y(r.label));
}

// pct < 0: no reading, an empty bar.
static void set_gauge(RowId id, int pct, bool high_is_bad = true) {
    Row& r = rows[id];
    if (!r.fill) return;
    if (pct > 100) pct = 100;
    const int w = pct > 0 ? cw * pct / 100 : 0;
    lv_obj_set_size(r.fill, w, bar_h);
    lv_color_t c = THEME_ACCENT;
    if (high_is_bad) c = pct >= 90 ? THEME_RED : pct >= 70 ? THEME_AMBER : THEME_ACCENT;
    else             c = pct <= 10 ? THEME_RED : pct <= 25 ? THEME_AMBER : THEME_GREEN;
    lv_obj_set_style_bg_color(r.fill, c, 0);
}

static void kb_pair(char* out, size_t n, uint32_t used, uint32_t total) {
    if (total >= 4u << 20) snprintf(out, n, "%.1f / %.1f MB", used / 1048576.0f, total / 1048576.0f);
    else                   snprintf(out, n, "%u / %u KB", (unsigned)(used / 1024), (unsigned)(total / 1024));
}

static void refresh(void) {
    char buf[48];

    snprintf(buf, sizeof(buf), "%s", ESP.getChipModel());
    lv_label_set_text(lbl_chip, buf);
    lv_obj_align(lbl_chip, LV_ALIGN_TOP_RIGHT, -(board_caps().width - cx - cw), cy);

    // The UI task's share of the time: LVGL's own idle measure.
    const int busy = 100 - (int)lv_timer_get_idle();
    snprintf(buf, sizeof(buf), "%u MHz x%d  %d%% busy", (unsigned)getCpuFrequencyMhz(), (int)ESP.getChipCores(),
             busy < 0 ? 0 : busy);
    set_value(R_CPU, buf);
    set_gauge(R_CPU, busy);

    const uint32_t heap_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    const uint32_t heap_used = heap_total - heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    kb_pair(buf, sizeof(buf), heap_used, heap_total);
    set_value(R_MEM, buf);
    set_gauge(R_MEM, heap_total ? (int)((uint64_t)heap_used * 100 / heap_total) : -1);

    const uint32_t ps_total = ESP.getPsramSize();
    if (ps_total) {
        const uint32_t ps_used = ps_total - ESP.getFreePsram();
        kb_pair(buf, sizeof(buf), ps_used, ps_total);
        set_gauge(R_PSRAM, (int)((uint64_t)ps_used * 100 / ps_total));
    } else {
        snprintf(buf, sizeof(buf), "none");
        set_gauge(R_PSRAM, -1);
    }
    set_value(R_PSRAM, buf);

    kb_pair(buf, sizeof(buf), sketch_used, sketch_total);
    set_value(R_FLASH, buf);
    set_gauge(R_FLASH, sketch_total ? (int)((uint64_t)sketch_used * 100 / sketch_total) : -1);

    const float t = temperatureRead();
    snprintf(buf, sizeof(buf), "%.1f C", t);
    set_value(R_TEMP, buf);
    set_gauge(R_TEMP, (int)((t - 20.0f) * 100.0f / 65.0f));   // 20..85 C, the chip's rated range

    const int pct = power_hal_battery_pct();
    if (board_caps().has_battery && pct >= 0) {
        snprintf(buf, sizeof(buf), "%d%%%s", pct, power_hal_is_charging() ? " charging" : power_hal_is_vbus_in() ? " on USB" : "");
        set_gauge(R_BATT, pct, false);
    } else {
        snprintf(buf, sizeof(buf), "%s", power_hal_is_vbus_in() ? "USB power" : "none");
        set_gauge(R_BATT, -1);
    }
    set_value(R_BATT, buf);

    const ble_state_t s = ble_get_state();
    set_value(R_BLE, s == BLE_STATE_CONNECTED ? "connected" : s == BLE_STATE_ADVERTISING ? "advertising" : "off");

    const uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    if (up >= 86400) snprintf(buf, sizeof(buf), "%ud %uh %um", (unsigned)(up / 86400), (unsigned)(up % 86400 / 3600), (unsigned)(up % 3600 / 60));
    else if (up >= 3600) snprintf(buf, sizeof(buf), "%uh %um %us", (unsigned)(up / 3600), (unsigned)(up % 3600 / 60), (unsigned)(up % 60));
    else snprintf(buf, sizeof(buf), "%um %us", (unsigned)(up / 60), (unsigned)(up % 60));
    set_value(R_UP, buf);

    snprintf(buf, sizeof(buf), "%d%%", (int)brightness_get() * 100 / 255);
    set_value(R_BRIGHT, buf);

    snprintf(buf, sizeof(buf), "%s %s", CLAWD_VERSION, CLAWD_ENV);
    set_value(R_FW, buf);
}

// The page slides across with the finger: in from the right after a swipe
// left, out to the right after a swipe right.
#define SLIDE_MS 300
static void slide_exec(void* obj, int32_t v) { lv_obj_set_style_translate_x((lv_obj_t*)obj, v, 0); }
static void slide_out_done(lv_anim_t* a) {
    lv_obj_t* o = (lv_obj_t*)a->var;
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_translate_x(o, 0, 0);
}
static void slide(int32_t from, int32_t to, bool out) {
    lv_anim_delete(root, slide_exec);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, root);
    lv_anim_set_exec_cb(&a, slide_exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, SLIDE_MS);
    lv_anim_set_path_cb(&a, out ? lv_anim_path_ease_in : lv_anim_path_ease_out);
    if (out) lv_anim_set_completed_cb(&a, slide_out_done);
    lv_anim_start(&a);
}

void sysmon_show(void) {
    if (!root) return;
    shown = true;
    refresh();
    refreshed_at = millis();
    lv_obj_set_style_translate_x(root, board_caps().width, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(root);
    slide(board_caps().width, 0, false);
}

void sysmon_show_now(void) {
    if (!root) return;
    shown = true;
    refresh();
    refreshed_at = millis();
    lv_anim_delete(root, slide_exec);
    lv_obj_set_style_translate_x(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(root);
}

void sysmon_hide(bool animated) {
    if (!root || !shown) return;
    shown = false;
    if (animated) {
        slide(lv_obj_get_style_translate_x(root, LV_PART_MAIN), board_caps().width, true);
    } else {
        lv_anim_delete(root, slide_exec);
        lv_obj_set_style_translate_x(root, 0, 0);
        lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    }
}

bool sysmon_visible(void) { return shown; }

void sysmon_tick(void) {
    if (!shown) return;
    const uint32_t now = millis();
    if (now - refreshed_at < REFRESH_MS) return;
    refreshed_at = now;
    refresh();
}
