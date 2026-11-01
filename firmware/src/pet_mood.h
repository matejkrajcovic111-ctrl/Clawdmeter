#pragma once
#include <stdint.h>
#include "clawd_policy.h"   // ClawdRng (pure; also in the native build)

// The full-screen pet face (shown after a tap on the title): what mood it is
// in, which little gags it performs and how it reacts to boops, strokes,
// shakes and tilt. Pure logic, no LVGL/Arduino (test_pet_mood); another module
// draws it. Times are millis()-style uint32 and wrap-safe.

// ─── Tunables ────────────────────────────────────────────────────────────────
#define PET_BOOP_BURST_N        5      // this many boops...
#define PET_BOOP_BURST_MS       6000   // ...within this span annoy it (a few taps are friendly)
#define PET_GRUMPY_MS           8000   // grumpy long enough to read as a mood, not a blink
#define PET_HAPPY_MS            5000   // a stroke's afterglow
#define PET_SHAKE_DEV_G         0.6f   // |magnitude - 1 g| above this is a shake (ignores table bumps)
#define PET_SHAKE_SAMPLES       3      // this many such samples...
#define PET_SHAKE_WINDOW_MS     1500   // ...within this span are a shake (one jolt is not)
#define PET_DIZZY_MS            3000   // long enough to see the spinning eyes
#define PET_DIZZY_LOCKOUT_MS    2000   // after dizzy ends, so a long shake doesn't loop it
#define PET_TILT_ALPHA          0.3f   // EMA weight of a new sample: smooth at 10 Hz yet quick
#define PET_TILT_MAX_G          0.7f   // beyond ~45 deg the eyes are already at the edge
#define PET_WORRIED_PCT         90.0f  // usage at or above this worries it
#define PET_SLEEPY_FROM_HOUR    23     // night: 23:00...
#define PET_SLEEPY_TO_HOUR      5      //   ...to 05:59
#define PET_MORNING_FROM_HOUR   6      // morning stretch window: 06:00...
#define PET_MORNING_TO_HOUR     9      //   ...to 09:59
#define PET_BORED_MS            45000  // no interaction this long (and idle) = bored
#define PET_STRETCH_MIN_MS      3000   // morning stretch comes soon after entering,
#define PET_STRETCH_MAX_MS      6000   //   not after a minute of staring
#define PET_GAG_MIN_MS          20000  // random gags: often enough to feel alive,
#define PET_GAG_MAX_MS          45000  //   rare enough not to nag
#define PET_HICCUP_GAP_MIN_MS   1200   // hiccups follow each other quickly
#define PET_HICCUP_GAP_MAX_MS   2500
#define PET_HICCUP_RUN_MIN      2      // hiccups per run (including the first)
#define PET_HICCUP_RUN_MAX      4
#define PET_GAG_WEIGHT          30     // relative chance of sneeze / hiccup / yawn
#define PET_YAWN_WEIGHT_TIRED   70     // yawn weight when sleepy or bored

enum PetMood { PET_CONTENT, PET_HAPPY, PET_GRUMPY, PET_DIZZY, PET_SLEEPY, PET_BORED, PET_WORRIED };
enum PetGag  { GAG_NONE, GAG_SNEEZE, GAG_HICCUP, GAG_YAWN, GAG_STRETCH };
enum PetReact { REACT_NONE, REACT_SQUINT /*happy boop*/, REACT_GRUMPY /*booped too much*/,
                REACT_WIGGLE /*stroke*/, REACT_DIZZY /*shaken*/ };

struct PetInput {
    uint32_t now;      // ms
    int  local_hour;   // 0..23, -1 unknown (no clock yet)
    float max_pct;     // highest usage % of the shown account, <0 unknown
    bool working;      // a Claude session is working right now
};

struct PetState {
    ClawdRng rng;
    uint32_t last_activity;            // latest boop / stroke / shake
    uint32_t boops[PET_BOOP_BURST_N];  // times of the latest boops (oldest first)
    int      nboops;
    bool     grumpy;  uint32_t grumpy_until;
    bool     happy;   uint32_t happy_until;
    bool     dizzy;   uint32_t dizzy_until;
    uint32_t shakes[PET_SHAKE_SAMPLES];  // times of the latest shake samples
    int      nshakes;
    float    tilt_x, tilt_y;           // smoothed, g
    bool     tilt_init;
    bool     stretch_done;             // the first gag has been decided
    uint32_t stretch_at;               // earliest time for the morning stretch
    uint32_t next_gag_at;
    int      hiccups_left;             // hiccups still to come in the current run
};

void     pet_reset(PetState* s, uint32_t now, uint32_t seed);   // entering pet mode
PetReact pet_boop(PetState* s, uint32_t now);    // a tap on the face
PetReact pet_stroke(PetState* s, uint32_t now);  // a horizontal drag across the face (caller detects)
PetReact pet_accel(PetState* s, float x, float y, float z, uint32_t now);  // one sample in g, ~10 Hz
void     pet_tilt(const PetState* s, int max_px, int* dx, int* dy);  // eye offset from the smoothed tilt
PetMood  pet_mood(const PetState* s, const PetInput* in);
PetGag   pet_next_gag(PetState* s, const PetInput* in);  // every frame; a gag once when due, else GAG_NONE
