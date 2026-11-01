// pet_mood (clawdmeter2): the pet face's mood, reactions, tilt and gags.
//   pio test -d firmware -e native -f test_pet_mood
#include <unity.h>
#include <stdint.h>
#include <string.h>
#include "pet_mood.h"

static PetState S;

void setUp(void) { pet_reset(&S, 1000, 12345); }
void tearDown(void) {}

static PetInput in(uint32_t now, int hour = 12, float pct = 10.0f, bool working = false) {
    PetInput i;
    i.now = now; i.local_hour = hour; i.max_pct = pct; i.working = working;
    return i;
}
static PetMood mood(uint32_t now, int hour = 12, float pct = 10.0f, bool working = false) {
    PetInput i = in(now, hour, pct, working);
    return pet_mood(&S, &i);
}
// Keep the pet "active" so BORED does not interfere with unrelated checks.
static void shake(PetState* s, uint32_t t0, int n, PetReact* last = nullptr) {
    PetReact r = REACT_NONE;
    for (int k = 0; k < n; k++) {
        PetReact x = pet_accel(s, 1.8f, 0.0f, 0.0f, t0 + (uint32_t)k * 100);
        if (x != REACT_NONE) r = x;
    }
    if (last) *last = r;
}

// ─── boop / stroke ───────────────────────────────────────────────────────────
static void test_boop_squints(void) {
    TEST_ASSERT_EQUAL(REACT_SQUINT, pet_boop(&S, 2000));
    TEST_ASSERT_EQUAL(PET_CONTENT, mood(2100));
}

static void test_five_quick_boops_are_grumpy_and_expire(void) {
    for (int k = 0; k < 4; k++) TEST_ASSERT_EQUAL(REACT_SQUINT, pet_boop(&S, 2000 + k * 500));
    TEST_ASSERT_EQUAL(REACT_GRUMPY, pet_boop(&S, 4000));
    TEST_ASSERT_EQUAL(PET_GRUMPY, mood(4001));
    TEST_ASSERT_EQUAL(PET_GRUMPY, mood(4000 + PET_GRUMPY_MS - 1));
    TEST_ASSERT_EQUAL(PET_CONTENT, mood(4000 + PET_GRUMPY_MS));
}

static void test_slow_boops_do_not_annoy(void) {
    for (int k = 0; k < 10; k++) TEST_ASSERT_EQUAL(REACT_SQUINT, pet_boop(&S, 2000 + k * 2000));
}

static void test_boops_while_grumpy_extend(void) {
    for (int k = 0; k < 5; k++) pet_boop(&S, 2000 + k * 100);
    TEST_ASSERT_EQUAL(PET_GRUMPY, mood(2500));
    TEST_ASSERT_EQUAL(REACT_GRUMPY, pet_boop(&S, 8000));
    TEST_ASSERT_EQUAL(PET_GRUMPY, mood(8000 + PET_GRUMPY_MS - 1));
    TEST_ASSERT_EQUAL(PET_CONTENT, mood(8000 + PET_GRUMPY_MS));
}

static void test_stroke_is_happy_and_clears_grumpy(void) {
    for (int k = 0; k < 5; k++) pet_boop(&S, 2000 + k * 100);
    TEST_ASSERT_EQUAL(PET_GRUMPY, mood(2600));
    TEST_ASSERT_EQUAL(REACT_WIGGLE, pet_stroke(&S, 2700));
    TEST_ASSERT_EQUAL(PET_HAPPY, mood(2701));
    TEST_ASSERT_EQUAL(PET_HAPPY, mood(2700 + PET_HAPPY_MS - 1));
    TEST_ASSERT_EQUAL(PET_CONTENT, mood(2700 + PET_HAPPY_MS));
}

// ─── shake ───────────────────────────────────────────────────────────────────
static void test_shake_makes_dizzy_once(void) {
    PetReact r;
    shake(&S, 5000, 2, &r);
    TEST_ASSERT_EQUAL(REACT_NONE, r);
    TEST_ASSERT_EQUAL(REACT_DIZZY, pet_accel(&S, 0.0f, 2.0f, 0.0f, 5200));
    TEST_ASSERT_EQUAL(PET_DIZZY, mood(5201));
    // Keep shaking: no second trigger, mood stays dizzy then ends.
    shake(&S, 5300, 20, &r);
    TEST_ASSERT_EQUAL(REACT_NONE, r);
    TEST_ASSERT_EQUAL(PET_DIZZY, mood(5200 + PET_DIZZY_MS - 1));
    TEST_ASSERT_EQUAL(PET_CONTENT, mood(5200 + PET_DIZZY_MS));
}

static void test_shake_no_retrigger_until_lockout_ends(void) {
    PetReact r;
    shake(&S, 5000, 3, &r);
    TEST_ASSERT_EQUAL(REACT_DIZZY, r);
    const uint32_t end = 5200 + PET_DIZZY_MS;
    shake(&S, end + 100, 5, &r);                       // inside the lockout
    TEST_ASSERT_EQUAL(REACT_NONE, r);
    shake(&S, end + PET_DIZZY_LOCKOUT_MS, 3, &r);      // after it
    TEST_ASSERT_EQUAL(REACT_DIZZY, r);
}

static void test_slow_or_gentle_motion_is_not_a_shake(void) {
    PetReact r = REACT_NONE;
    for (int k = 0; k < 6; k++) {   // jolts 1 s apart: never 3 within 1.5 s
        PetReact x = pet_accel(&S, 1.8f, 0, 0, 5000 + k * 1000);
        if (x != REACT_NONE) r = x;
    }
    TEST_ASSERT_EQUAL(REACT_NONE, r);
    for (int k = 0; k < 20; k++) TEST_ASSERT_EQUAL(REACT_NONE, pet_accel(&S, 0.4f, 0.3f, 0.9f, 8000 + k * 100));
}

// ─── tilt ────────────────────────────────────────────────────────────────────
static void test_tilt_flat_is_zero(void) {
    int dx = 99, dy = 99;
    pet_tilt(&S, 20, &dx, &dy);
    TEST_ASSERT_EQUAL(0, dx); TEST_ASSERT_EQUAL(0, dy);
    for (int k = 0; k < 30; k++) pet_accel(&S, 0, 0, 1.0f, 2000 + k * 100);
    pet_tilt(&S, 20, &dx, &dy);
    TEST_ASSERT_EQUAL(0, dx); TEST_ASSERT_EQUAL(0, dy);
}

static void test_tilt_maps_linearly_and_clamps(void) {
    int dx, dy;
    for (int k = 0; k < 60; k++) pet_accel(&S, 0.35f, -0.35f, 0.87f, 2000 + k * 100);
    pet_tilt(&S, 20, &dx, &dy);
    TEST_ASSERT_INT_WITHIN(1, 10, dx);
    TEST_ASSERT_INT_WITHIN(1, -10, dy);
    for (int k = 0; k < 60; k++) pet_accel(&S, 0.95f, 0.0f, 0.0f, 9000 + k * 100);   // beyond 0.7 g
    pet_tilt(&S, 20, &dx, &dy);
    TEST_ASSERT_EQUAL(20, dx);
    TEST_ASSERT_EQUAL(0, dy);
    for (int k = 0; k < 60; k++) pet_accel(&S, 0.0f, -0.95f, 0.0f, 20000 + k * 100);
    pet_tilt(&S, 20, &dx, &dy);
    TEST_ASSERT_EQUAL(0, dx);
    TEST_ASSERT_EQUAL(-20, dy);
}

static void test_tilt_is_smoothed(void) {
    int dx, dy;
    pet_accel(&S, 0, 0, 1.0f, 2000);
    pet_accel(&S, 0.7f, 0, 0.7f, 2100);   // one sample is only alpha of the way
    pet_tilt(&S, 100, &dx, &dy);
    TEST_ASSERT_INT_WITHIN(2, 30, dx);
}

static void test_tilt_ignores_shake_samples(void) {
    int dx, dy;
    pet_accel(&S, 0, 0, 1.0f, 2000);
    shake(&S, 3000, 6);
    pet_tilt(&S, 20, &dx, &dy);
    TEST_ASSERT_EQUAL(0, dx); TEST_ASSERT_EQUAL(0, dy);
}

// ─── mood priority ───────────────────────────────────────────────────────────
static void test_mood_priority(void) {
    // Worried beats sleepy, bored and content.
    TEST_ASSERT_EQUAL(PET_CONTENT, mood(2000, 12, 50.0f));
    TEST_ASSERT_EQUAL(PET_WORRIED, mood(2000, 12, 90.0f));
    TEST_ASSERT_EQUAL(PET_CONTENT, mood(2000, 12, -1.0f));
    TEST_ASSERT_EQUAL(PET_WORRIED, mood(2000, 2, 95.0f));
    // Sleepy at night, unless working or the clock is unknown.
    TEST_ASSERT_EQUAL(PET_SLEEPY, mood(2000, 23));
    TEST_ASSERT_EQUAL(PET_SLEEPY, mood(2000, 0));
    TEST_ASSERT_EQUAL(PET_SLEEPY, mood(2000, 5));
    TEST_ASSERT_EQUAL(PET_CONTENT, mood(2000, 6));
    TEST_ASSERT_EQUAL(PET_CONTENT, mood(2000, 22));
    TEST_ASSERT_EQUAL(PET_CONTENT, mood(2000, 2, 10.0f, true));
    TEST_ASSERT_EQUAL(PET_CONTENT, mood(2000, -1));
    // Bored after 45 s of nothing, unless working; sleepy outranks bored.
    TEST_ASSERT_EQUAL(PET_CONTENT, mood(1000 + PET_BORED_MS - 1));
    TEST_ASSERT_EQUAL(PET_BORED, mood(1000 + PET_BORED_MS));
    TEST_ASSERT_EQUAL(PET_CONTENT, mood(1000 + PET_BORED_MS, 12, 10.0f, true));
    TEST_ASSERT_EQUAL(PET_SLEEPY, mood(1000 + PET_BORED_MS, 1));
    TEST_ASSERT_EQUAL(PET_WORRIED, mood(1000 + PET_BORED_MS, 12, 99.0f));
    // A boop restarts the boredom clock.
    pet_boop(&S, 40000);
    TEST_ASSERT_EQUAL(PET_CONTENT, mood(40000 + PET_BORED_MS - 1));
    TEST_ASSERT_EQUAL(PET_BORED, mood(40000 + PET_BORED_MS));
    // Happy beats worried; grumpy beats happy; dizzy beats grumpy.
    pet_stroke(&S, 100000);
    TEST_ASSERT_EQUAL(PET_HAPPY, mood(100001, 2, 99.0f));
    for (int k = 0; k < 5; k++) pet_boop(&S, 100100 + k * 10);
    TEST_ASSERT_EQUAL(PET_GRUMPY, mood(100200, 2, 99.0f));
    shake(&S, 100300, 3);
    TEST_ASSERT_EQUAL(PET_DIZZY, mood(100600, 2, 99.0f));
}

// ─── gags ────────────────────────────────────────────────────────────────────
struct Ev { uint32_t t; PetGag g; };
static int collect(PetState* s, uint32_t t0, uint32_t span, int hour, bool working, Ev* out, int cap) {
    int n = 0;
    for (uint32_t d = 0; d <= span && n < cap; d += 50) {
        PetInput i = in(t0 + d, hour, 10.0f, working);
        PetGag g = pet_next_gag(s, &i);
        if (g != GAG_NONE) { out[n].t = t0 + d; out[n].g = g; n++; }
    }
    return n;
}

static void test_stretch_first_in_the_morning(void) {
    for (uint32_t seed = 1; seed <= 20; seed++) {
        pet_reset(&S, 1000, seed);
        Ev ev[8];
        int n = collect(&S, 1000, 100000, 7, true, ev, 8);
        TEST_ASSERT_TRUE(n >= 2);
        TEST_ASSERT_EQUAL(GAG_STRETCH, ev[0].g);
        TEST_ASSERT_TRUE(ev[0].t - 1000 <= PET_STRETCH_MAX_MS + 50);
        for (int k = 1; k < n; k++) TEST_ASSERT_NOT_EQUAL(GAG_STRETCH, ev[k].g);   // only once
    }
}

static void test_no_stretch_outside_the_morning(void) {
    for (int hour = -1; hour <= 23; hour++) {
        if (hour >= 6 && hour <= 9) continue;
        pet_reset(&S, 1000, 77);
        Ev ev[16];
        int n = collect(&S, 1000, 200000, hour, true, ev, 16);
        for (int k = 0; k < n; k++) TEST_ASSERT_NOT_EQUAL(GAG_STRETCH, ev[k].g);
    }
}

static void test_random_gag_interval(void) {
    pet_reset(&S, 1000, 5);
    Ev ev[64];
    int n = collect(&S, 1000, 600000, 12, true, ev, 64);
    TEST_ASSERT_TRUE(n >= 8);
    TEST_ASSERT_TRUE(ev[0].t - 1000 >= PET_GAG_MIN_MS);
    TEST_ASSERT_TRUE(ev[0].t - 1000 <= PET_GAG_MAX_MS + 50);
    bool sneeze = false, hiccup = false, yawn = false;
    for (int k = 0; k < n; k++) {
        sneeze |= ev[k].g == GAG_SNEEZE; hiccup |= ev[k].g == GAG_HICCUP; yawn |= ev[k].g == GAG_YAWN;
    }
    TEST_ASSERT_TRUE(sneeze && hiccup && yawn);
}

static void test_hiccup_runs(void) {
    int runs_seen = 0;
    for (uint32_t seed = 1; seed <= 60; seed++) {
        pet_reset(&S, 1000, seed);
        Ev ev[64];
        int n = collect(&S, 1000, 900000, 12, true, ev, 64);
        for (int k = 0; k < n; k++) {
            if (ev[k].g != GAG_HICCUP) continue;
            // k starts a run (the previous event, if any, was not a hiccup
            // inside its spacing window): count consecutive close hiccups.
            if (k > 0 && ev[k - 1].g == GAG_HICCUP && ev[k].t - ev[k - 1].t <= PET_HICCUP_GAP_MAX_MS + 50) continue;
            int len = 1;
            while (k + len < n && ev[k + len].g == GAG_HICCUP &&
                   ev[k + len].t - ev[k + len - 1].t <= PET_HICCUP_GAP_MAX_MS + 50) {
                uint32_t gap = ev[k + len].t - ev[k + len - 1].t;
                TEST_ASSERT_TRUE(gap >= PET_HICCUP_GAP_MIN_MS);
                len++;
            }
            TEST_ASSERT_TRUE(len >= PET_HICCUP_RUN_MIN);
            TEST_ASSERT_TRUE(len <= PET_HICCUP_RUN_MAX);
            runs_seen++;
        }
    }
    TEST_ASSERT_TRUE(runs_seen > 20);
}

static void test_no_gags_while_dizzy_or_grumpy(void) {
    Ev ev[4];
    // Dizzy starts just before a gag is due and covers it.
    pet_reset(&S, 1000, 9);
    uint32_t due = S.next_gag_at;
    shake(&S, due - 200, 3);                  // dizzy from due-0 for 3 s
    PetInput i = in(due + 100);
    TEST_ASSERT_EQUAL(PET_DIZZY, pet_mood(&S, &i));
    TEST_ASSERT_EQUAL(0, collect(&S, due - 100, 2900, 12, true, ev, 4));
    TEST_ASSERT_TRUE(collect(&S, due + 3000, 1000, 12, true, ev, 4) >= 1);

    // Same with grumpy.
    pet_reset(&S, 1000, 9);
    due = S.next_gag_at;
    for (int k = 0; k < 5; k++) pet_boop(&S, due - 400 + k * 100);   // grumpy until due+8000
    TEST_ASSERT_EQUAL(0, collect(&S, due, 7800, 12, true, ev, 4));
    TEST_ASSERT_TRUE(collect(&S, due + 8100, 1000, 12, true, ev, 4) >= 1);
}

static void test_yawn_likelier_when_tired(void) {
    int yawns_tired = 0, yawns_fresh = 0, total = 0;
    for (uint32_t seed = 1; seed <= 200; seed++) {
        PetState a, b;
        pet_reset(&a, 1000, seed);
        pet_reset(&b, 1000, seed);
        PetInput tired = in(0, 2, 10.0f, false);    // night: sleepy
        PetInput fresh = in(0, 12, 10.0f, true);
        for (uint32_t t = 1000; t < 60000; t += 50) {
            tired.now = fresh.now = t;
            PetGag ga = pet_next_gag(&a, &tired);
            PetGag gb = pet_next_gag(&b, &fresh);
            if (ga != GAG_NONE && ga != GAG_STRETCH) { total++; yawns_tired += ga == GAG_YAWN; }
            if (gb == GAG_YAWN) yawns_fresh++;
        }
    }
    TEST_ASSERT_TRUE(total > 100);
    TEST_ASSERT_TRUE(yawns_tired > yawns_fresh);
}

// ─── clock wraparound ────────────────────────────────────────────────────────
static void test_wraparound_reactions(void) {
    const uint32_t t0 = UINT32_MAX - 1000;
    pet_reset(&S, t0, 3);
    for (int k = 0; k < 4; k++) TEST_ASSERT_EQUAL(REACT_SQUINT, pet_boop(&S, t0 + k * 400));   // crosses the wrap
    TEST_ASSERT_EQUAL(REACT_GRUMPY, pet_boop(&S, t0 + 1600));
    TEST_ASSERT_EQUAL(PET_GRUMPY, mood(t0 + 1700));
    TEST_ASSERT_EQUAL(PET_GRUMPY, mood(t0 + 1600 + PET_GRUMPY_MS - 1));
    TEST_ASSERT_EQUAL(PET_CONTENT, mood(t0 + 1600 + PET_GRUMPY_MS));

    pet_stroke(&S, t0 + 20000);
    TEST_ASSERT_EQUAL(PET_HAPPY, mood(t0 + 20000 + PET_HAPPY_MS - 1));
    TEST_ASSERT_EQUAL(PET_CONTENT, mood(t0 + 20000 + PET_HAPPY_MS));

    PetReact r;
    shake(&S, UINT32_MAX - 150, 3, &r);   // samples at -150, -50, +50 around the wrap
    TEST_ASSERT_EQUAL(REACT_DIZZY, r);
    TEST_ASSERT_EQUAL(PET_DIZZY, mood(t0 + 1000 + 2000));
}

static void test_wraparound_bored_and_gags(void) {
    const uint32_t t0 = UINT32_MAX - 10000;
    pet_reset(&S, t0, 11);
    TEST_ASSERT_EQUAL(PET_CONTENT, mood(t0 + PET_BORED_MS - 1));
    TEST_ASSERT_EQUAL(PET_BORED, mood(t0 + PET_BORED_MS));
    Ev ev[16];
    int n = collect(&S, t0, 120000, 12, true, ev, 16);
    TEST_ASSERT_TRUE(n >= 2);
    TEST_ASSERT_TRUE(ev[0].t - t0 >= PET_GAG_MIN_MS);   // unsigned difference survives the wrap
    TEST_ASSERT_TRUE(ev[0].t - t0 <= PET_GAG_MAX_MS + 50);

    pet_reset(&S, t0, 11);
    n = collect(&S, t0, 20000, 8, true, ev, 16);        // morning stretch across the wrap
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL(GAG_STRETCH, ev[0].g);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_boop_squints);
    RUN_TEST(test_five_quick_boops_are_grumpy_and_expire);
    RUN_TEST(test_slow_boops_do_not_annoy);
    RUN_TEST(test_boops_while_grumpy_extend);
    RUN_TEST(test_stroke_is_happy_and_clears_grumpy);
    RUN_TEST(test_shake_makes_dizzy_once);
    RUN_TEST(test_shake_no_retrigger_until_lockout_ends);
    RUN_TEST(test_slow_or_gentle_motion_is_not_a_shake);
    RUN_TEST(test_tilt_flat_is_zero);
    RUN_TEST(test_tilt_maps_linearly_and_clamps);
    RUN_TEST(test_tilt_is_smoothed);
    RUN_TEST(test_tilt_ignores_shake_samples);
    RUN_TEST(test_mood_priority);
    RUN_TEST(test_stretch_first_in_the_morning);
    RUN_TEST(test_no_stretch_outside_the_morning);
    RUN_TEST(test_random_gag_interval);
    RUN_TEST(test_hiccup_runs);
    RUN_TEST(test_no_gags_while_dizzy_or_grumpy);
    RUN_TEST(test_yawn_likelier_when_tired);
    RUN_TEST(test_wraparound_reactions);
    RUN_TEST(test_wraparound_bored_and_gags);
    return UNITY_END();
}
