#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include "data.h"
#include "ui.h"
#include "ble.h"
#include "performer.h"
#include "mascot.h"
#include "clawd_director.h"
#include "usage_rate.h"
#include "signals.h"
#include "payload.h"
#include "minimal_mode.h"
#include "build_info.h"
#include "idle.h"
#include "idle_cfg.h"
#include "brightness.h"
#include "clawd_prefs.h"
#include "perf.h"
#include "pair_hold.h"
#include "accounts.h"
#include "gesture.h"
#include "history.h"
#include "detail.h"
#include "sysmon.h"
#include "pet.h"
#include "estimate.h"
#include "portal.h"

#include "hal/board_caps.h"
#include "hal/display_hal.h"
#include "hal/touch_hal.h"
#include "hal/input_hal.h"
#include "hal/power_hal.h"
#include "hal/imu_hal.h"
#include "hal/sound_hal.h"
#include "hal/board_info_hal.h"
#include "perm.h"
#include "party.h"
#include "savers.h"
#include <ArduinoJson.h>

static UsageData usage = {};

// ---- LVGL draw buffers (partial render mode) ----
// PSRAM-equipped boards (S3) can comfortably hold larger strips. PSRAM-free
// boards (e.g. ESP32-C6) allocate from internal SRAM, so we shrink the strip
// — 480×20 RGB565 = 19 KB × 2 buffers = 38 KB, fits beside everything else.
#ifdef BOARD_HAS_PSRAM
#define BUF_LINES 40
#define LV_BUF_CAPS (MALLOC_CAP_SPIRAM)
#else
#define BUF_LINES 20
#define LV_BUF_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif
static uint16_t* buf1 = nullptr;
static uint16_t* buf2 = nullptr;

static uint32_t my_tick(void) { return millis(); }

static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    display_hal_draw_bitmap(area->x1, area->y1, w, h, (uint16_t*)px_map);
    lv_display_flush_ready(disp);
}

static void rounder_cb(lv_event_t* e) {
    lv_area_t* area = (lv_area_t*)lv_event_get_param(e);
    display_hal_round_area(&area->x1, &area->y1, &area->x2, &area->y2);
}

// Touch policy is driven by IDLE_WAKE_ON_TOUCH:
//   true  → a press edge while asleep wakes the device and the first touch is
//           swallowed (mirrors the button wake-consumption); a press while
//           awake counts as activity.
//   false → touch never counts as activity and is fully swallowed while the
//           panel is dark, so pets/sleeves can't wake it overnight and the
//           director can't act on taps nobody can see.
// A finger is on the panel (after the wake filter below); read by the
// touch-hold re-pair on boards without a PWR button.
static bool touch_down = false;
// Every touch is classified on release (gesture.h) and handled in loop().
static GestureState gesture_state = {};
static Gesture pending_gesture = { GESTURE_NONE, 0, 0 };
static int     touch_x = 0, touch_y = 0;   // the latest raw touch point
static Estimate est;                        // estimate.h: the session % between readings

static void my_touch_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    uint16_t x, y;
    bool pressed;
    touch_hal_read(&x, &y, &pressed);
    const bool raw_pressed = pressed;

    if (IDLE_WAKE_ON_TOUCH) {
        static bool touch_was = false;
        static bool touch_wake_swallowed = false;
        if (raw_pressed && !touch_was) {
            // Press edge — consume as wake if asleep.
            if (idle_consume_wake_press()) {
                touch_wake_swallowed = true;
                pressed = false;
            }
        } else if (!raw_pressed && touch_was) {
            // Release edge.
            if (touch_wake_swallowed) {
                touch_wake_swallowed = false;
                pressed = false;
            }
        } else if (raw_pressed && touch_wake_swallowed) {
            // Held finger through wake — keep hiding until release.
            pressed = false;
        }
        touch_was = raw_pressed;
    } else if (idle_is_asleep()) {
        pressed = false;
    }

    touch_down = pressed;
    touch_x = x; touch_y = y;   // the pet follows the finger (loop(), not here: no LVGL work in a read)
    {
        const Gesture g = gesture_step(&gesture_state, pressed, (int16_t)x, (int16_t)y, millis());
        if (g.kind != GESTURE_NONE) pending_gesture = g;
    }
    if (pressed) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// Parse a JSON line into UsageData (payload.cpp holds the pure parser).
static bool parse_json(const char* json, UsageData* out) {
    const char* err = nullptr;
    if (!payload_parse(json, out, &err)) {
        Serial.printf("JSON parse error: %s\n", err ? err : "?");
        return false;
    }
    return true;
}

// Forget every bonded host and advertise again, so another computer (or
// this one, after removing the device) can pair. The PWR hold gesture, the
// touch hold on boards without PWR, and the `unpair` serial command all end
// here.
static void clear_pairing(const char* why) {
    Serial.printf("Pair: clearing bonds (%s), advertising\n", why);
    ble_clear_bonds();
    ui_set_pair_hint(PAIR_HINT_CLEARED);
}

// ---- Serial command buffer ----
#define CMD_BUF_SIZE 64
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_pos = 0;

static void send_screenshot() {
#ifndef BOARD_HAS_PSRAM
    // A full RGB565 framebuffer doesn't fit in internal SRAM on PSRAM-free
    // boards (e.g. 480×480×2 = 460 KB). Capture is unsupported there.
    Serial.println("SCREENSHOT_UNSUPPORTED");
    return;
#else
    const uint32_t w = board_caps().width;
    const uint32_t h = board_caps().height;
    const uint32_t row_bytes = w * 2;
    const uint32_t buf_size = row_bytes * h;
    uint8_t* sbuf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!sbuf) {
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, w, h, LV_COLOR_FORMAT_RGB565, row_bytes, sbuf, buf_size);

    lv_result_t res = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &draw_buf);
    if (res != LV_RESULT_OK) {
        heap_caps_free(sbuf);
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    Serial.printf("SCREENSHOT_START %lu %lu %lu\n",
        (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    Serial.write(sbuf, buf_size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");
    heap_caps_free(sbuf);
#endif
}

// clawdmeter2 QA: the overlay layer (the face, pet props, history screens) as
// ARGB8888, which `screenshot` (the screen only) can't see. The host
// composites it over a `screenshot` (tools/screenshot.py).
static void send_screenshot_top() {
#ifndef BOARD_HAS_PSRAM
    Serial.println("SCREENSHOT_UNSUPPORTED");
#else
    const uint32_t w = board_caps().width, h = board_caps().height;
    const uint32_t row_bytes = w * 4, buf_size = row_bytes * h;
    uint8_t* sbuf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!sbuf) { Serial.println("SCREENSHOT_ERR"); return; }
    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, w, h, LV_COLOR_FORMAT_ARGB8888, row_bytes, sbuf, buf_size);
    if (lv_snapshot_take_to_draw_buf(lv_layer_top(), LV_COLOR_FORMAT_ARGB8888, &draw_buf) != LV_RESULT_OK) {
        heap_caps_free(sbuf);
        Serial.println("SCREENSHOT_ERR");
        return;
    }
    Serial.printf("SCREENSHOT_TOP_START %lu %lu %lu\n", (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    Serial.write(sbuf, buf_size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");
    heap_caps_free(sbuf);
#endif
}

// One JSON line identifying this build, for the desktop flasher's first
// detection probe. Keys are fixed: env, ver, rev, link.
static void send_info() {
    Serial.printf("{\"env\":\"%s\",\"ver\":\"%s\",\"rev\":\"%s\",\"link\":\"ble\"}\n",
                  CLAWD_ENV, CLAWD_VERSION, board_hal_rev());
}

static void check_serial_cmd() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_pos] = '\0';
            if (strcmp(cmd_buf, "screenshot") == 0) send_screenshot();
            else if (strcmp(cmd_buf, "screenshot top") == 0) send_screenshot_top();
            else if (strcmp(cmd_buf, "chime") == 0 || strcmp(cmd_buf, "buzz") == 0) sound_hal_chime();
            else if (strncmp(cmd_buf, "chime ", 6) == 0) {
                // Dev tool for tuning a board's speaker: `chime <gain> [volume]`,
                // e.g. `chime 0.6 70` (until reboot).
                float g = 0;
                int v = -1;
                if (sscanf(cmd_buf + 6, "%f %d", &g, &v) >= 1) {
                    sound_hal_tune(g, v);
                    Serial.printf("{\"chime\":{\"gain\":%.2f,\"volume\":%d}}\n", g, v);
                } else {
                    Serial.println("usage: chime [<gain 0..1> [<volume 0..100>]]");
                }
            }
#ifdef CLAWDMETER_DEV
            // clawdmeter2 dev tools: `saver <0..2>` / `saver off` previews a screensaver,
            // `batt <pct>` a battery level, `party` the confetti, `perm` a sample permission
            // card, `glow` the edge glow. Only in the dev build (env waveshare_amoled_216_dev).
            else if (strncmp(cmd_buf, "saver ", 6) == 0) {
                if (strcmp(cmd_buf + 6, "off") == 0) saver_stop();
                else saver_start(atoi(cmd_buf + 6));
            }
            else if (strncmp(cmd_buf, "batt ", 5) == 0) ui_update_battery(atoi(cmd_buf + 5), false, false);   // preview until the next real reading
            else if (strcmp(cmd_buf, "party") == 0) party_confetti();
            else if (strcmp(cmd_buf, "glow") == 0) party_glow(lv_color_hex(0xFFC107), 2);
            else if (strcmp(cmd_buf, "perm") == 0) perm_show(7, "Bash", "git push origin main", 45000);
#endif
            else if (strcmp(cmd_buf, "info") == 0)  send_info();
            else if (strcmp(cmd_buf, "perf") == 0)  perf_print();
            else if (strcmp(cmd_buf, "ble") == 0) ble_print_status();
            else if (strcmp(cmd_buf, "disconnect") == 0) ble_disconnect_all();
            else if (strcmp(cmd_buf, "unpair") == 0) {
                clear_pairing("serial");
                Serial.println("{\"unpair\":true}");
            }
            else if (strcmp(cmd_buf, "liveliness") == 0 || strncmp(cmd_buf, "liveliness ", 11) == 0) {
                // QA: `liveliness [1-5]` (until the app sends its own).
                if (cmd_buf[10] == ' ') liveliness_set(atoi(cmd_buf + 11));
                Serial.printf("{\"liveliness\":%d}\n", liveliness_get());
            }
            else if (strcmp(cmd_buf, "attention_anim") == 0 || strncmp(cmd_buf, "attention_anim ", 15) == 0) {
                // QA: `attention_anim [seconds]` (0 = off; until the app sends its own).
                if (cmd_buf[14] == ' ') attn_anim_set(atoi(cmd_buf + 15));
                Serial.printf("{\"attention_anim\":%d}\n", attn_anim_get());
            }
            else if (strcmp(cmd_buf, "pace_marker") == 0 || strncmp(cmd_buf, "pace_marker ", 12) == 0) {
                // QA: `pace_marker [0|1]` (until the app sends its own).
                if (cmd_buf[11] == ' ') pace_marker_set(atoi(cmd_buf + 12) != 0);
                Serial.printf("{\"pace_marker\":%s}\n", pace_marker_get() ? "true" : "false");
            }
            // clawdmeter2 QA: drive the touch gestures from serial.
            //   swipe left | swipe right | tap X Y
            else if (strcmp(cmd_buf, "swipe left") == 0)  pending_gesture = { GESTURE_SWIPE_LEFT, 0, 0 };
            else if (strcmp(cmd_buf, "swipe right") == 0) pending_gesture = { GESTURE_SWIPE_RIGHT, 0, 0 };
            else if (strncmp(cmd_buf, "tap ", 4) == 0) {
                int tx = 0, ty = 0;
                if (sscanf(cmd_buf + 4, "%d %d", &tx, &ty) == 2) pending_gesture = { GESTURE_TAP, tx, ty };
            }
            else if (strncmp(cmd_buf, "clawd ", 6) == 0) {
                const bool ok = director_debug(cmd_buf + 6);
                Serial.printf("{\"clawd\":\"%s\",\"ok\":%s}\n", cmd_buf + 6, ok ? "true" : "false");
            }
            cmd_pos = 0;
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

// Each board provides this. Must bring up the shared I2C bus (Wire.begin
// with the board's SDA/SCL pins) and any board-private hardware that has
// to settle before display/touch (e.g. an IO expander gating the LCD
// reset line). Called exactly once at the start of setup().
extern "C" void board_init(void);

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("{\"ready\":true}");

    board_init();

    display_hal_init();
    display_hal_begin();
    idle_init();        // takes over panel brightness and starts the idle timer
    brightness_init();  // load the user's saved brightness level and apply via idle

    power_hal_init();
    imu_hal_init();
    sound_hal_init();
    touch_hal_init();

    // ---- LVGL ----
    const int W = board_caps().width;
    const int H = board_caps().height;

    lv_init();
    lv_tick_set_cb(my_tick);

    buf1 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);
    buf2 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);

    lv_display_t* disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, W * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    perf_attach(disp, LV_DEF_REFR_PERIOD);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_cb);

    ble_init();
    input_hal_init();

    ui_init();
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());
    ui_update_battery(power_hal_battery_pct(), power_hal_is_charging(), power_hal_is_vbus_in());
    director_init();    // rooms + beats; boots on the meters (home)
    detail_init();      // the history screens (hidden), over the meters
    sysmon_init();      // the device page (hidden), left of account 1
    portal_init();      // the swipe transition, over everything
    saver_init(lv_screen_active());   // effect screensavers, over the meters
    party_init(lv_layer_top());   // confetti / edge glow
    perm_init(lv_layer_top());    // permission card, over everything
    clawd_prefs_init();  // the saved device settings, before the app says otherwise

    Serial.printf("Dashboard ready (%s, %dx%d) %lu ms after boot, waiting for data on BLE...\n",
        board_caps().name, W, H, (unsigned long)millis());
}

// The meters show the chosen account (accounts.h): its numbers in the cards,
// its palette and its "#N". `fresh` = a payload just landed (counts toward
// the data's freshness); a swipe only redraws.
static void show_accounts(bool fresh) {
    UsageData view;
    int shown = accounts_shown();
    if (!accounts_view(&usage, &view)) {   // nothing read for it yet: the active one
        view = usage;
        shown = usage.account_idx > 0 ? usage.account_idx : 1;
    }
    ui_set_account(shown, accounts_count());
    // One account (no `ai`): its flag rides on the payload itself. An
    // {"ok":false} payload (every account logged out) has no top-level account.
    const bool legacy = usage.ok && usage.account_idx < 1;
    ui_set_working(legacy ? usage.working : accounts_working(shown));
    ui_set_thinking(legacy ? usage.thinking : accounts_thinking(shown));
    // `tl` is the reporting account's: only on its own page.
    ui_set_tool(legacy || shown == usage.account_idx ? usage.tool : "");
    int32_t turn_tok = usage.turn_tokens, turn_secs = usage.turn_secs;
    if (!legacy) accounts_turn(shown, &turn_tok, &turn_secs);
    ui_set_turn(turn_tok, turn_secs);
    // A logged-out account: its last reading, dimmed (before the render below).
    ui_set_offline(view.ok && accounts_offline(shown) == 1);
    if (view.valid && view.ok)
        pet_set_usage(view.session_pct > view.weekly_pct ? view.session_pct : view.weekly_pct);
    if (fresh) ui_update(&view);
    else       ui_show(&view);
    if (view.ok) detail_set_usage(&view);   // never zeros from a "no data" beat
}

// clawdmeter2: between readings the session bar creeps on at the current
// rate (estimate.h, a few points at most), and the run-out time is a fact on
// the status line. Only the account the readings are for: the active one.
#define ESTIMATE_EVERY_MS 5000
static uint32_t estimate_at = 0;

static void tick_estimate(uint32_t now) {
    if ((int32_t)(now - estimate_at) < 0) return;
    estimate_at = now + ESTIMATE_EVERY_MS;
    // clawdmeter2: no run-out line on the status line (by request); the
    // estimate still drives the session bar's creep below.
    ui_set_info(UI_INFO_RUNOUT, "");
    if (!usage.valid || !usage.ok || usage.enterprise) return;
    const float rate = usage_rate_pct_per_hour_at(now);
    const bool on_active = usage.account_idx < 1 || accounts_shown() == usage.account_idx;
    if (!on_active || !ui_meters_live() || detail_visible()) return;
    const float pct = estimate_session_pct(&est, now, rate);
    if (pct < usage.session_pct + 0.05f) return;   // nothing to add (or no estimate)
    UsageData v = usage;
    v.session_pct = pct;
    ui_show(&v);
}

// clawdmeter2: the screen follows the account Claude is being used with. A
// swipe looks at another one for this long, then it goes back to following.
#define ACCOUNT_SWIPE_HOLD_MS 60000
static uint32_t swiped_at = 0;

// A classified touch (gesture.h). Taps go to the director as before; a
// horizontal swipe on the meters steps to the next or previous account.
// Swipes: what changes while the portal covers the screen (portal.h).
static void swap_account(void) {
    director_end_show();      // a show on the old page doesn't carry over
    show_accounts(false);     // new numbers, colours and that account's own Clawd
}
static void swap_show_device(void) { sysmon_show_now(); }
static void swap_hide_device(void) { sysmon_hide(false); }

// clawdmeter2: `{"pq":1,"id":..,"tool":..,"t":..}` shows a permission
// request, `{"pq":0,"id":..}` withdraws it. True when `raw` was one.
static bool handle_perm_message(const char* raw) {
    if (!strstr(raw, "\"pq\"")) return false;
    static const char* const K_PQ = "pq", *K_ID = "id", *K_TOOL = "tool", *K_T = "t";   // not usage keys (payload key check)
    JsonDocument doc;
    if (deserializeJson(doc, raw)) return false;
    if (doc[K_PQ].isNull()) return false;
    const uint32_t id = doc[K_ID] | 0u;
    if ((doc[K_PQ] | 0) == 1) {
        if (perm_show(id, doc[K_TOOL] | "", doc[K_T] | "", 45000)) idle_note_activity();   // wake the panel for it
        else ble_send_perm_skip(id);   // can't show it whole: the terminal asks
    } else {
        perm_hide(id);
    }
    return true;
}

static void handle_gesture(const Gesture& g) {
    if (portal_active()) return;               // a swipe transition is running: hands off
    if (perm_visible()) {                      // the permission card has the screen
        if (g.kind == GESTURE_TAP) {
            const int h = perm_hit(g.x, g.y);
            if (h == 1) perm_answer(true);
            else if (h == 2) perm_answer(false);
            idle_note_activity();
        }
        return;
    }
    switch (g.kind) {
    case GESTURE_TAP: {
        // A history screen is up: any tap closes it, nothing else.
        if (detail_visible()) { detail_hide(); idle_note_activity(); break; }
        // The device page: a tap does nothing but keep it up (swipe right leaves).
        if (sysmon_visible()) { idle_note_activity(); swiped_at = millis(); break; }
        // Pet mode: a tap on the title strip (where you came in) goes back to
        // the meters; anywhere else is a boop.
        if (director_in_pet()) {
            idle_note_activity();
            if (g.y < (int)board_caps().height * 15 / 100) director_pet_exit();
            else                                            pet_tap(g.x, g.y);
            break;
        }
        // On the live meters, the session card opens the 30-day chart and the
        // weekly card this week's tokens; everything else is the aquarium.
        // During an alert, a show or the screensaver a tap keeps meaning
        // "you're here" (director_tap), wherever it lands.
        const UiHit hit = director_cards_tappable() && ui_meters_live() && !ui_pair_hint_visible()
                              ? ui_hit_test(g.x, g.y) : UI_HIT_NONE;
        if (hit == UI_HIT_CLAWD && director_pet()) {   // Clawd in his seat: the face comes up to be petted
            idle_note_activity();
            break;
        }
        if (hit == UI_HIT_BATTERY) {                   // the battery: its percentage for a few seconds
            idle_note_activity();
            ui_show_battery_pct();
            break;
        }
        if (hit == UI_HIT_TITLE) {                     // the clock: the date for a few seconds
            idle_note_activity();
            ui_show_date();
            break;
        }
        if (hit == UI_HIT_SESSION || hit == UI_HIT_WEEKLY) {
            idle_note_activity();
            director_end_show();
            int shown = accounts_shown();
            if (shown <= 0) shown = usage.account_idx > 0 ? usage.account_idx : 1;
            detail_show(hit == UI_HIT_SESSION ? DETAIL_CHART : DETAIL_TOKENS, shown,
                        ui_account_palette() == 2);
            break;
        }
        director_tap(g.x);
        break;
    }
    case GESTURE_SWIPE_LEFT:
    case GESTURE_SWIPE_RIGHT: {
        // In pet mode a sideways drag is a stroke (pet_touch), not a swipe.
        if (director_room() != ROOM_METERS || ui_pair_hint_visible() || detail_visible()) break;
        // The pages, left to right: this device | #1 | #2 ... A swipe right
        // moves one page right, a swipe left one page left; no wrapping.
        const bool right = g.kind == GESTURE_SWIPE_RIGHT;
        if (portal_active()) break;            // mid-transition
        if (sysmon_visible()) {
            if (right) { idle_note_activity(); swiped_at = millis(); portal_start(+1, swap_hide_device); }
            break;
        }
        int at = accounts_shown();
        if (at < 1) at = 1;
        if (!right && at <= 1) {               // left of #1: the device page
            idle_note_activity();
            swiped_at = millis();
            portal_start(-1, swap_show_device);
            break;
        }
        if (accounts_count() < 2 || (right && at >= accounts_count())) break;
        const int n = accounts_step(right ? +1 : -1);
        if (!n) break;
        idle_note_activity();
        shown_account_save(n);
        swiped_at = millis();
        Serial.printf("accounts: swiped to #%d of %d\n", n, accounts_count());
        portal_start(right ? +1 : -1, swap_account);   // the page changes under the portal
        break;
    }
    case GESTURE_NONE:
        break;
    }
}

static ble_state_t last_ble_state = BLE_STATE_INIT;
static int shown_group = 0;   // mood group last logged

// Hold-to-pair gesture: hold the PWR button ~3s, then RELEASE → clear all BLE
// bonds and re-advertise. Clearing on *release* (not while held) is deliberate:
// holding to power the device OFF (AXP hardware shutdown at 8s) must not wipe
// the bond — a power-off hold never releases before shutdown. To stop a
// "chicken-out" release just before 8s from pairing, the gesture disarms at 6s.
//
//   ~1.5s long-press edge → PENDING
//   3.0s (+1500)          → ARMED   (release from here clears bonds)
//   6.0s (+4500)          → DISARMED (no clear; AXP powers off at 8s)
#define PAIR_WINDOW_MS            180000 // how long another computer may pair
#define PAIR_ARM_AFTER_LONG_MS    1500   // 3.0s total
#define PAIR_DISARM_AFTER_LONG_MS 4500   // 6.0s total
enum pair_state_t { PAIR_IDLE, PAIR_PENDING, PAIR_ARMED };
static pair_state_t pair_state        = PAIR_IDLE;
static uint32_t     pair_long_seen_ms = 0;

static void pair_tick(void) {
    if (pair_state == PAIR_IDLE && power_hal_pwr_long_pressed()) {
        pair_state = PAIR_PENDING;
        pair_long_seen_ms = millis();
        (void)power_hal_pwr_released();  // drain any stale release edge
        Serial.println("PWR long-press: hold to ~3s then release to pair");
        return;
    }
    if (pair_state == PAIR_IDLE) return;

    if (power_hal_pwr_released()) {
        if (pair_state == PAIR_ARMED) {
            // clawdmeter2: the first hold lets one more computer pair and keeps
            // the ones already paired; a second hold while that's open forgets
            // them all.
            if (ble_pairing_open()) {
                ble_close_pairing();
                clear_pairing("PWR hold twice");
            } else {
                ble_open_pairing(PAIR_WINDOW_MS);
            }
        } else {
            Serial.println("Pair: released too early — cancelled");
        }
        pair_state = PAIR_IDLE;
        return;
    }

    uint32_t held = millis() - pair_long_seen_ms;
    if (pair_state == PAIR_PENDING && held >= PAIR_ARM_AFTER_LONG_MS) {
        pair_state = PAIR_ARMED;
        Serial.println("Pair: armed — release to pair");
    } else if (pair_state == PAIR_ARMED && held >= PAIR_DISARM_AFTER_LONG_MS) {
        pair_state = PAIR_IDLE;  // power-off territory; don't pair
        Serial.println("Pair: disarmed (holding toward power-off)");
    }
}

// Boards without a PWR button: a 5 s touch hold on the pairing hint clears
// the bonds (pair_hold.h). Only counts while the hint is what you see.
static PairHold touch_hold = { PAIR_HOLD_UP, false, 0 };

static void touch_pair_tick(void) {
    if (board_caps().has_pwr_button) return;
    // Taps on the hint don't make Clawd peek (the face would cover it).
    director_set_pairing_touch(ui_pair_hint_visible());
    const bool enabled = ui_pair_hint_visible() && director_room() == ROOM_METERS && !idle_is_asleep();
    switch (pair_hold_step(&touch_hold, enabled, touch_down, millis())) {
    case PAIR_HOLD_HOLDING:
        Serial.println("Pair: touch hold, keep holding to clear the pairing");
        ui_set_pair_hint(PAIR_HINT_HOLDING);
        break;
    case PAIR_HOLD_CANCEL:
        Serial.println("Pair: touch hold released early, cancelled");
        ui_set_pair_hint(PAIR_HINT_NORMAL);
        break;
    case PAIR_HOLD_FIRE:
        clear_pairing("touch hold");
        break;
    case PAIR_HOLD_NONE:
        break;
    }
}

void loop() {
    idle_tick();
    lv_timer_handler();
    ui_tick_anim();
    ble_tick();
    power_hal_tick();
    imu_hal_tick();
    sound_hal_tick();
    performer_tick();
    mascot_tick();
    director_tick();
    if (director_in_pet()) pet_touch(touch_down, touch_x, touch_y);
    tick_estimate(millis());
    if (accounts_user_chose() && (uint32_t)(millis() - swiped_at) > ACCOUNT_SWIPE_HOLD_MS) {
        accounts_follow_app();
        shown_account_save(0);
        if (usage.valid && (usage.ok || accounts_count() >= 2)) show_accounts(false);
    }
    // A history screen never hides anything more important: an alert, the
    // screensaver, a dropped link, the panel going to sleep.
    if (detail_visible() && (!director_cards_tappable() || !ui_meters_live() || idle_is_asleep()))
        detail_hide();
    // The device page likewise, and after a minute without a touch.
    sysmon_tick();
    if (sysmon_visible() && (!director_cards_tappable() || idle_is_asleep()
                             || (uint32_t)(millis() - swiped_at) > ACCOUNT_SWIPE_HOLD_MS))
        sysmon_hide(false);
    if (pending_gesture.kind != GESTURE_NONE) {
        const Gesture g = pending_gesture;
        pending_gesture.kind = GESTURE_NONE;
        handle_gesture(g);
    }
    if (director_take_dismissed()) ble_send_dismiss();
    perm_tick();
    party_tick();
    saver_tick();
    {
        uint32_t pid;
        bool allow;
        if (perm_take_answer(&pid, &allow)) ble_send_perm(pid, allow);
    }
    // Rotation transition (blank + ramp) would fight the idle fade — skip
    // ticks while the panel is dark. A rotation that happens during sleep
    // is detected by the next tick after wake and ramped in then.
    if (!idle_is_asleep()) display_hal_tick();

    // ---- Physical buttons ----
    //   PRIMARY   → HID Space  (Claude Code voice-mode PTT)
    //   SECONDARY → HID Shift+Tab  (mode toggle; only if the board has one)
    //   PWR       → cycle brightness; hold ~3s + release: pairing mode
    // Any press also tells the director you're here (wakes the screensaver).
    // First press from sleep is consumed as a wake-only event by
    // idle_consume_wake_press(); the normal action fires from the second
    // press. Activity bookkeeping happens inside idle_consume_wake_press
    // so no separate idle_note_activity() call is needed here.
    {
        static bool primary_was = false;
        static bool primary_wake_swallowed = false;
        bool primary_now = input_hal_is_held(INPUT_BTN_PRIMARY);
        if (primary_now != primary_was) {
            if (primary_now) {
                if (idle_consume_wake_press()) primary_wake_swallowed = true;
                // No touchscreen: BOOT dismisses an active alert first and is
                // not a push-to-talk press then.
                else if (director_button(true)) primary_wake_swallowed = true;
                else { ble_keyboard_press(0x2C, 0); }                     // HID Space, no mods
            } else {
                if (primary_wake_swallowed) primary_wake_swallowed = false;
                else                        ble_keyboard_release();
            }
            primary_was = primary_now;
        }

        if (board_caps().button_count >= 2) {
            static bool secondary_was = false;
            static bool secondary_wake_swallowed = false;
            bool secondary_now = input_hal_is_held(INPUT_BTN_SECONDARY);
            if (secondary_now != secondary_was) {
                if (secondary_now) {
                    if (idle_consume_wake_press()) secondary_wake_swallowed = true;
                    else { ble_keyboard_press(0x2B, 0x02); director_button(false); }  // HID Tab + LEFT_SHIFT
                } else {
                    if (secondary_wake_swallowed) secondary_wake_swallowed = false;
                    else                          ble_keyboard_release();
                }
                secondary_was = secondary_now;
            }
        }

        if (power_hal_pwr_pressed()) {
            if (!idle_consume_wake_press()) {
                // Counts as you being here. On a touchless board whose only
                // button this is, it dismisses an active alert first; else it
                // cycles brightness.
                if (!director_button(board_caps().button_count == 0)) brightness_cycle();
            }
        }

        pair_tick();
        touch_pair_tick();

        // The pairing window on the status line, so you know it's open.
        static bool pairing_shown = false;
        const bool open = ble_pairing_open();
        if (open != pairing_shown) {
            pairing_shown = open;
            ui_set_notice(open ? "Pair another computer now" : nullptr);
            if (!open) { ble_close_pairing(); Serial.println("Pair: window closed"); }
        }
    }

    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
    }

    static int  last_pct      = -2;
    static bool last_charging = false;
    static bool last_vbus     = false;
    int  pct      = power_hal_battery_pct();
    bool charging = power_hal_is_charging();
    bool vbus     = power_hal_is_vbus_in();
    if (pct != last_pct || charging != last_charging || vbus != last_vbus) {
        if (pct != last_pct) ble_set_battery_level(pct);
        last_pct = pct;
        last_charging = charging;
        last_vbus = vbus;
        ui_update_battery(pct, charging, vbus);
    }

    check_serial_cmd();

    if (ble_has_data()) {
        const char* raw = ble_get_data();
        if (handle_perm_message(raw)) {
            // clawdmeter2: a permission request, not a usage payload.
        } else if (history_is_message(raw)) {
            // clawdmeter2: an account's 30-day history, not a usage payload.
            int a = 0;
            static AccountHistory h;
            if (history_parse(raw, &a, &h)) {
                history_store(a, &h);
                detail_refresh();
            }
        } else if (parse_json(raw, &usage)) {
            // The app switched which account it reports: the other account's
            // numbers aren't a drop (no reset chime) or a rate (fresh ring).
            static int last_ai = -1;
            if (usage.ok && usage.account_idx > 0) {
                if (last_ai > 0 && usage.account_idx != last_ai) {
                    usage_rate_clear();
                    estimate_reset(&est);
                    Serial.printf("accounts: app now on #%d\n", usage.account_idx);
                }
                last_ai = usage.account_idx;
            }
            // Fast + slow clock bookkeeping: rate sample (replays skipped),
            // hook activity lift, event dedupe. See signals.h.
            const SignalsUpdate sig = signals_ingest(&usage);
            const bool session_reset = sig.session_reset;
            const int g_before = sig.group_before;
            const int g_after  = sig.group_after;
            shown_group = g_after;
            if (sig.event_fired)
                Serial.printf("[%lu] hook event '%c' (#%lu)\n", (unsigned long)millis(),
                              sig.event ? sig.event : '?', (unsigned long)sig.event_seq);
            // 5-hour session limit refilled → ding so the user knows they can
            // use Claude again (no-op on boards without sound). Gated on the
            // app's opt-in `c` (reset_chime); the `chime` serial cmd ignores it.
            if (session_reset && usage.chime) {
                Serial.println("session reset detected — chime");
                sound_hal_chime();
            }
            // Claude Code hook fired on the host: the same ding. Not gated on
            // `c`: the app only sets `b` when its alert chime is on.
            if (usage.beep) {
                Serial.println("attention — chime");
                sound_hal_chime();
            }
            if (usage.pace_marker >= 0) pace_marker_set(usage.pace_marker != 0);
            if (usage.ok) {
                accounts_ingest(&usage);
                // A real reading of the active account anchors the estimate.
                if (!usage.replay && !usage.enterprise)
                    estimate_reading(&est, millis(), usage.session_pct, usage.session_reset_mins);
                // The account you're on is tight and another has room: say so.
                const int next = accounts_use_next();
                char hint[40] = "";
                if (next) snprintf(hint, sizeof(hint), "#%d has more room", next);
                ui_set_info(UI_INFO_USE_NEXT, hint);
                // Sessions per machine (`sm`); the status font may lack "·".
                char mach[40];
                int mo = 0;
                for (const char* p = usage.machines; *p && mo < (int)sizeof(mach) - 1; p++) {
                    if ((uint8_t)p[0] == 0xC2 && (uint8_t)p[1] == 0xB7) { mach[mo++] = '/'; p++; }
                    else mach[mo++] = *p;
                }
                mach[mo] = 0;
                ui_set_info(UI_INFO_MACHINES, mach);
                show_accounts(true);
            } else {
                // {"ok":false}: no data. When every account is logged out it
                // still carries `ac`/`o`: keep those pages (dimmed) swipeable.
                accounts_ingest(&usage);
                if (usage.other_count > 0) show_accounts(true);
                else                       ui_update(&usage);
            }
            // A prompt (or you're back after a gap): you're at the computer,
            // so a dimmed or dark panel wakes before Clawd notices you. A
            // Stop or tool activity doesn't: Claude working alone isn't you.
            if (sig.event_fired && (sig.event == 'p' || sig.event == 'b'))
                idle_note_activity();
            if (usage.liveliness > 0) liveliness_set(usage.liveliness);
            if (usage.attn_anim_s >= 0) attn_anim_set(usage.attn_anim_s);
            director_on_payload(&usage, &sig);
            if (usage.alert_msg[0]) {
                Serial.println("attention — mascot");
                idle_note_activity();
                director_alert(usage.alert_msg);
            }
            if (usage.alert_clear) director_alert_clear();
            if (g_after != g_before)
                Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
                    g_before, g_after, usage.session_pct);
            ble_send_ack();
        } else {
            ble_send_nack();
        }
    }

    // The mood group also moves without a payload: the 15-min window decays
    // to Idle on its own and a hook activity lift expires. Nothing needs to
    // react (the corner and the performer read the group when they pick), but log
    // it so the serial trace matches what Clawd does.
    {
        static uint32_t last_check = 0;
        if (millis() - last_check >= 1000) {
            last_check = millis();
            const int g = usage_rate_group();
            if (g != shown_group) {
                Serial.printf("usage rate: group %d -> %d (decay)\n", shown_group, g);
                shown_group = g;
            }
        }
    }

    delay(5);
}
