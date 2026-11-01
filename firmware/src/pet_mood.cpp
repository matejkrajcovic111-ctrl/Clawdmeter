#include "pet_mood.h"
#include <math.h>
#include <string.h>

// Deadline checks: a flag plus a wrap-safe comparison.
static inline bool before(uint32_t now, uint32_t t) { return (int32_t)(now - t) < 0; }
static inline bool reached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

static bool is_grumpy(const PetState* s, uint32_t now) { return s->grumpy && before(now, s->grumpy_until); }
static bool is_happy(const PetState* s, uint32_t now)  { return s->happy  && before(now, s->happy_until); }
static bool is_dizzy(const PetState* s, uint32_t now)  { return s->dizzy  && before(now, s->dizzy_until); }
// Dizzy plus the lockout after it.
static bool shake_locked(const PetState* s, uint32_t now) {
    return s->dizzy && before(now, s->dizzy_until + PET_DIZZY_LOCKOUT_MS);
}

static void schedule_gag(PetState* s, uint32_t now) {
    s->next_gag_at = now + clawd_rng_range(&s->rng, PET_GAG_MIN_MS, PET_GAG_MAX_MS);
}

void pet_reset(PetState* s, uint32_t now, uint32_t seed) {
    memset(s, 0, sizeof(*s));
    clawd_rng_seed(&s->rng, seed);
    s->last_activity = now;
    s->stretch_at = now + clawd_rng_range(&s->rng, PET_STRETCH_MIN_MS, PET_STRETCH_MAX_MS);
    schedule_gag(s, now);
}

PetReact pet_boop(PetState* s, uint32_t now) {
    s->last_activity = now;
    if (is_grumpy(s, now)) {
        s->grumpy_until = now + PET_GRUMPY_MS;
        return REACT_GRUMPY;
    }
    if (s->nboops == PET_BOOP_BURST_N) {   // drop the oldest
        memmove(s->boops, s->boops + 1, sizeof(s->boops[0]) * (PET_BOOP_BURST_N - 1));
        s->nboops--;
    }
    s->boops[s->nboops++] = now;
    if (s->nboops == PET_BOOP_BURST_N && (uint32_t)(now - s->boops[0]) <= PET_BOOP_BURST_MS) {
        s->grumpy = true;
        s->grumpy_until = now + PET_GRUMPY_MS;
        s->nboops = 0;
        return REACT_GRUMPY;
    }
    return REACT_SQUINT;
}

PetReact pet_stroke(PetState* s, uint32_t now) {
    s->last_activity = now;
    s->grumpy = false;
    s->nboops = 0;
    s->happy = true;
    s->happy_until = now + PET_HAPPY_MS;
    return REACT_WIGGLE;
}

PetReact pet_accel(PetState* s, float x, float y, float z, uint32_t now) {
    const float mag = sqrtf(x * x + y * y + z * z);
    const bool jolt = fabsf(mag - 1.0f) > PET_SHAKE_DEV_G;
    if (is_dizzy(s, now) || (jolt && shake_locked(s, now))) return REACT_NONE;   // keep shaking, ignored
    if (jolt) {
        s->last_activity = now;
        // Samples older than the window no longer count.
        int k = 0;
        for (int i = 0; i < s->nshakes; i++)
            if ((uint32_t)(now - s->shakes[i]) <= PET_SHAKE_WINDOW_MS) s->shakes[k++] = s->shakes[i];
        s->nshakes = k;
        if (s->nshakes == PET_SHAKE_SAMPLES) {
            memmove(s->shakes, s->shakes + 1, sizeof(s->shakes[0]) * (PET_SHAKE_SAMPLES - 1));
            s->nshakes--;
        }
        s->shakes[s->nshakes++] = now;
        if (s->nshakes == PET_SHAKE_SAMPLES) {
            s->nshakes = 0;
            s->dizzy = true;
            s->dizzy_until = now + PET_DIZZY_MS;
            return REACT_DIZZY;
        }
        return REACT_NONE;   // part of a shake: not a tilt either
    }
    if (!s->tilt_init) { s->tilt_x = x; s->tilt_y = y; s->tilt_init = true; }
    else {
        s->tilt_x += PET_TILT_ALPHA * (x - s->tilt_x);
        s->tilt_y += PET_TILT_ALPHA * (y - s->tilt_y);
    }
    return REACT_NONE;
}

static int tilt_px(float g, int max_px) {
    if (g > PET_TILT_MAX_G) g = PET_TILT_MAX_G;
    if (g < -PET_TILT_MAX_G) g = -PET_TILT_MAX_G;
    const float v = g / PET_TILT_MAX_G * (float)max_px;
    return v >= 0 ? (int)(v + 0.5f) : -(int)(-v + 0.5f);
}

void pet_tilt(const PetState* s, int max_px, int* dx, int* dy) {
    *dx = tilt_px(s->tilt_x, max_px);
    *dy = tilt_px(s->tilt_y, max_px);
}

PetMood pet_mood(const PetState* s, const PetInput* in) {
    const uint32_t now = in->now;
    if (is_dizzy(s, now)) return PET_DIZZY;
    if (is_grumpy(s, now)) return PET_GRUMPY;
    if (is_happy(s, now)) return PET_HAPPY;
    if (in->max_pct >= PET_WORRIED_PCT) return PET_WORRIED;
    const int h = in->local_hour;
    if (!in->working && h >= 0 && (h >= PET_SLEEPY_FROM_HOUR || h <= PET_SLEEPY_TO_HOUR)) return PET_SLEEPY;
    if (!in->working && (uint32_t)(now - s->last_activity) >= PET_BORED_MS) return PET_BORED;
    return PET_CONTENT;
}

static bool morning(int h) { return h >= PET_MORNING_FROM_HOUR && h <= PET_MORNING_TO_HOUR; }

PetGag pet_next_gag(PetState* s, const PetInput* in) {
    const uint32_t now = in->now;
    if (is_dizzy(s, now) || is_grumpy(s, now)) return GAG_NONE;

    // The morning stretch comes first, shortly after entering.
    if (!s->stretch_done && morning(in->local_hour) && reached(now, s->stretch_at)) {
        s->stretch_done = true;
        return GAG_STRETCH;
    }
    if (!reached(now, s->next_gag_at)) return GAG_NONE;

    if (s->hiccups_left > 0) {
        s->hiccups_left--;
        if (s->hiccups_left > 0)
            s->next_gag_at = now + clawd_rng_range(&s->rng, PET_HICCUP_GAP_MIN_MS, PET_HICCUP_GAP_MAX_MS);
        else
            schedule_gag(s, now);
        return GAG_HICCUP;
    }

    if (!s->stretch_done) {   // first gag of the visit
        s->stretch_done = true;
        if (morning(in->local_hour)) { schedule_gag(s, now); return GAG_STRETCH; }
    }

    const PetMood m = pet_mood(s, in);
    const uint32_t yawn_w = (m == PET_SLEEPY || m == PET_BORED) ? PET_YAWN_WEIGHT_TIRED : PET_GAG_WEIGHT;
    const uint32_t roll = clawd_rng_range(&s->rng, 0, 2 * PET_GAG_WEIGHT + yawn_w - 1);
    if (roll < PET_GAG_WEIGHT) { schedule_gag(s, now); return GAG_SNEEZE; }
    if (roll < 2 * PET_GAG_WEIGHT) {
        s->hiccups_left = (int)clawd_rng_range(&s->rng, PET_HICCUP_RUN_MIN, PET_HICCUP_RUN_MAX) - 1;
        if (s->hiccups_left > 0)
            s->next_gag_at = now + clawd_rng_range(&s->rng, PET_HICCUP_GAP_MIN_MS, PET_HICCUP_GAP_MAX_MS);
        else
            schedule_gag(s, now);
        return GAG_HICCUP;
    }
    schedule_gag(s, now);
    return GAG_YAWN;
}
