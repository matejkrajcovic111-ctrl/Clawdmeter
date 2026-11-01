#include "pet.h"
#include "pet_mood.h"
#include "pet_props.h"
#include "face.h"
#include "ui.h"
#include "signals.h"
#include "idle.h"
#include "hal/board_caps.h"
#include "hal/imu_hal.h"
#include <Arduino.h>
#include <math.h>
#include <stdlib.h>

// Every frame: the mood (pet_mood) sets the eyes' resting shape, what has
// its attention sets where they look (your finger > the thinking dot > the
// tilt), and a running reaction or gag (a short timeline) overrides both.
// The face only redraws what changed, so this costs little when nothing does.

#define PET_IDLE_EXIT_MS  120000   // left alone this long: back to the meters
#define ACCEL_EVERY_MS    100      // the IMU polls at ~10 Hz anyway
#define BLINK_MIN_MS      2500
#define BLINK_MAX_MS      6000

enum Anim { A_NONE, A_SQUINT, A_HMPH, A_WIGGLE, A_DIZZY, A_SNEEZE, A_HICCUP, A_YAWN, A_STRETCH, A_YAY };
static const uint32_t ANIM_MS[] = { 0, 600, 900, 900, PET_DIZZY_MS, 1400, 450, 2200, 2000, 1500 };

static PetState st;
static bool     active = false;
static Anim     anim = A_NONE;
static uint32_t anim_start = 0;
static bool     sneeze_fired = false;     // the "ACHOO!" half of a sneeze
static uint32_t last_input = 0;
static uint32_t next_accel = 0;
static uint32_t next_blink = 0;
static bool     touching = false;
static int      touch_x = 0, touch_y = 0, drag_last_x = 0, drag_travel = 0;
static bool     squint_on = false, zzz_on = false, sweat_on = false, think_on = false;
static float    usage_pct = -1;

static int W = 480, H = 480, mind = 480;
static int base_ex[2], base_ey;           // the eyes at rest, captured on entering

static int S(int v) { return v * mind / 480; }
static int clampi(int v, int m) { return v < -m ? -m : v > m ? m : v; }
static bool reached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

// Rapid taps: at most one new bubble every 250 ms (a relayout storm crashed LVGL).
static uint32_t last_bubble = 0;
static void bubble(const char* text) {
    const uint32_t now = millis();
    if ((uint32_t)(now - last_bubble) < 250) return;
    last_bubble = now;
    props_bubble(text, W / 2, base_ey + S(150), 900);
}

static void start(Anim a, uint32_t now) {
    anim = a;
    anim_start = now;
    sneeze_fired = false;
    switch (a) {
    case A_SQUINT:  if (clawd_rng_chance(&st.rng, 35)) bubble("boop!"); break;
    case A_HMPH:    bubble("hmph!"); break;
    case A_WIGGLE:  bubble("prrr~"); break;
    case A_DIZZY:   bubble("whoa~"); break;
    case A_SNEEZE:  bubble("ah..."); break;
    case A_HICCUP:  bubble("hic!"); break;
    case A_YAWN:    bubble("*yawn*"); break;
    case A_STRETCH: bubble("mmmh~"); break;
    case A_YAY:     bubble("yay!"); break;
    case A_NONE:    break;
    }
}

void pet_init(void) {
    const BoardCaps& c = board_caps();
    W = c.width; H = c.height;
    mind = W < H ? W : H;
    props_init();
}

void pet_enter(void) {
    const uint32_t now = millis();
    pet_reset(&st, now, now ^ 0x9E7u);
    props_set_neon(ui_account_palette() == 2);   // bubbles in the shown account's colour
    active = true;
    anim = A_NONE;
    squint_on = zzz_on = sweat_on = think_on = touching = false;
    last_input = now;
    next_accel = now;
    next_blink = now + 2000;
    face_set_resting(false);              // the pet drives the eyes now
    face_pet_reset();
    face_eye_center(0, &base_ex[0], &base_ey);
    face_eye_center(1, &base_ex[1], &base_ey);
    props_set_neon(ui_account_palette() == 2);
    props_to_front();                     // the face went to the front as it rose
    Serial.printf("[%lu] pet: hello\n", (unsigned long)now);
}

void pet_leave(void) {
    if (!active) return;
    active = false;
    props_hide_all();
    if (squint_on) face_set_squint(false);
    squint_on = zzz_on = sweat_on = think_on = touching = false;
    face_pet_reset();
}

bool pet_active(void) { return active; }

bool pet_done(uint32_t now) {
    return active && ((uint32_t)(now - last_input) >= PET_IDLE_EXIT_MS || idle_is_asleep());
}

void pet_set_usage(float max_pct) { usage_pct = max_pct; }

void pet_tap(int x, int y) {
    if (!active) return;
    (void)x; (void)y;
    const uint32_t now = millis();
    last_input = now;
    touching = false;
    const PetReact r = pet_boop(&st, now);
    if (anim == A_DIZZY) return;          // still spinning: it doesn't notice
    start(r == REACT_GRUMPY ? A_HMPH : A_SQUINT, now);
}

// A rub: sideways travel adds up (back and forth counts); a third of the
// panel's width of it is a stroke.
void pet_touch(bool pressed, int x, int y) {
    if (!active) return;
    if (!pressed) { touching = false; return; }
    const uint32_t now = millis();
    if (!touching) { touching = true; drag_last_x = x; drag_travel = 0; }
    drag_travel += abs(x - drag_last_x);
    drag_last_x = x;
    touch_x = x; touch_y = y;
    last_input = now;
    if (drag_travel > W / 3) {
        drag_travel = 0;
        if (pet_stroke(&st, now) == REACT_WIGGLE && anim != A_DIZZY) start(A_WIGGLE, now);
    }
}

void pet_celebrate(void) {
    if (!active) return;
    props_confetti();
    start(A_YAY, millis());
}

void pet_tick(uint32_t now) {
    if (!active) return;

    if (reached(now, next_accel)) {
        next_accel = now + ACCEL_EVERY_MS;
        float ax, ay, az;
        if (imu_hal_accel(&ax, &ay, &az) && pet_accel(&st, ax, ay, az, now) == REACT_DIZZY) {
            last_input = now;
            start(A_DIZZY, now);
        }
    }

    PetInput in = { now, ui_local_hour(), usage_pct, signals_activity() > 0 };
    const PetMood mood = pet_mood(&st, &in);
    if (anim != A_NONE && (uint32_t)(now - anim_start) >= ANIM_MS[anim]) anim = A_NONE;
    if (anim == A_NONE) {
        switch (pet_next_gag(&st, &in)) {
        case GAG_SNEEZE:  start(A_SNEEZE, now); break;
        case GAG_HICCUP:  start(A_HICCUP, now); break;
        case GAG_YAWN:    start(A_YAWN, now); break;
        case GAG_STRETCH: start(A_STRETCH, now); break;
        case GAG_NONE:    break;
        }
    }

    // ---- the resting shape, from the mood ----
    int h = 1000, w = 1000;
    bool bottom = false, sq = false;
    int dx = 0, dy = 0;
    switch (mood) {
    case PET_SLEEPY:  h = 330; bottom = true; break;
    case PET_BORED:   h = 560; bottom = true; dx = (int)(sinf(now / 2600.0f) * S(22)); break;
    case PET_GRUMPY:  h = 520; w = 1100; bottom = true; break;
    case PET_HAPPY:   sq = true; break;
    case PET_WORRIED: h = 1080; w = 920; break;
    default:          break;
    }

    // ---- where it looks ----
    const int M = S(26);
    int px, py;
    if (touching) {
        dx = clampi((touch_x - W / 2) * M / (W / 2), M);
        dy = clampi((touch_y - base_ey) * M / (H / 2), M);
    } else if (think_on && props_thinking_pos(&px, &py)) {
        dx = clampi((px - W / 2) * M / S(60), M);
        dy = clampi((py - base_ey) * M / S(160), M);
    } else {
        int tx, ty;
        pet_tilt(&st, S(30), &tx, &ty);
        dx += tx; dy += ty;
    }
    int d0x = dx, d0y = dy, d1x = dx, d1y = dy;

    // ---- a reaction or gag on top ----
    const int32_t t = (int32_t)(now - anim_start);
    switch (anim) {
    case A_SQUINT: sq = true; break;
    case A_YAY:
        sq = true;
        d0y = d1y = -(int)(fabsf(sinf(t * 0.0105f)) * S(18));   // little hops
        break;
    case A_HMPH:   // lids down, and it looks away from you
        h = 520; w = 1150; bottom = true; sq = false;
        if (t > 300) d0x = d1x = -S(20);
        break;
    case A_WIGGLE: {
        const int o = (int)(sinf(t * 0.035f) * S(12));
        d0x += o; d1x += o; sq = true;
        break;
    }
    case A_DIZZY: {   // the eyes circle, out of step with each other
        const float a = t * 0.0105f;
        const int r = S(18);
        d0x = (int)(cosf(a) * r);         d0y = (int)(sinf(a) * r);
        d1x = (int)(cosf(-a + 1.6f) * r); d1y = (int)(sinf(-a + 1.6f) * r);
        h = 1000; w = 1000; bottom = false; sq = false;
        break;
    }
    case A_SNEEZE:   // ah... (eyes open up, rising) ...CHOO (squeeze, jolt)
        if (t < 700) { h = 1000 + t * 150 / 700; bottom = false; d0y = d1y = -S(6) * t / 700; }
        else if (t < 1000) {
            sq = true; d0y = d1y = S(16);
            if (!sneeze_fired) { sneeze_fired = true; bubble("ACHOO!"); }
        }
        break;
    case A_HICCUP:
        if (t < 160) { d0y = d1y = -S(18); h = 1100; bottom = false; }
        break;
    case A_YAWN: {   // lids down slowly, hold, back up
        const int k = t < 900 ? t * 1000 / 900 : t < 1500 ? 1000 : t < 2200 ? (2200 - t) * 1000 / 700 : 0;
        h = 1000 - 750 * k / 1000; w = 1000 + 150 * k / 1000; bottom = true; sq = false;
        break;
    }
    case A_STRETCH: {   // tall and thin, up, then back
        int k = t < 700 ? t * 1000 / 700 : t < 1300 ? 1000 : (2000 - t) * 1000 / 700;
        if (k < 0) k = 0;
        h = 1000 + 350 * k / 1000; w = 1000 - 250 * k / 1000; bottom = false; sq = false;
        d0y = d1y = -S(10) * k / 1000;
        break;
    }
    case A_NONE: break;
    }

    // ---- apply ----
    if (sq != squint_on) { face_set_squint(sq); squint_on = sq; }
    face_set_eye_shape(h, w, bottom);
    face_look_each(d0x, d0y, d1x, d1y);
    if (!sq && anim == A_NONE && mood != PET_SLEEPY && reached(now, next_blink)) {
        face_blink(clawd_rng_chance(&st.rng, 20));
        next_blink = now + clawd_rng_range(&st.rng, BLINK_MIN_MS, BLINK_MAX_MS);
    }

    // ---- props ----
    const bool z = mood == PET_SLEEPY;
    if (z != zzz_on) { zzz_on = z; props_zzz(z, base_ex[1] + S(60), base_ey - S(90)); }
    const bool sw = mood == PET_WORRIED;
    if (sw != sweat_on) { sweat_on = sw; props_sweat(sw, base_ex[1] + S(48), base_ey - S(70)); }
    const bool th = in.working && mood != PET_SLEEPY && anim != A_DIZZY;
    if (th != think_on) { think_on = th; props_thinking(th, W / 2, base_ey - S(150), S(44)); }
}
