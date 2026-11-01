// estimate (clawdmeter2): session % creep between readings and run-out estimate.
//   pio test -d firmware -e native -f test_estimate
#include <unity.h>
#include <stdint.h>
#include <string.h>
#include "estimate.h"

static Estimate E;
#define MIN_MS 60000UL

void setUp(void) { estimate_reset(&E); }
void tearDown(void) {}

static void test_no_reading(void) {
    TEST_ASSERT_TRUE(estimate_session_pct(&E, 1000, 10.0f) < 0.0f);
    TEST_ASSERT_EQUAL(-1, estimate_runout_mins(&E, 1000, 10.0f));
}

static void test_creeps_at_rate(void) {
    estimate_reading(&E, 1000, 40.0f, 200);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 40.0f, estimate_session_pct(&E, 1000, 6.0f));
    // 6 %/h for 30 min = 3 points (the age limit is 10 min, so use 10 %/h-min scale instead)
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 41.0f, estimate_session_pct(&E, 1000 + 10 * MIN_MS, 6.0f));
}

static void test_capped_ahead_and_at_100(void) {
    estimate_reading(&E, 0, 40.0f, 200);
    // 60 %/h for 8 min = 8 points, capped to 5
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 45.0f, estimate_session_pct(&E, 8 * MIN_MS, 60.0f));
    estimate_reading(&E, 0, 98.0f, 200);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, estimate_session_pct(&E, 8 * MIN_MS, 60.0f));
}

static void test_frozen(void) {
    estimate_reading(&E, 0, 40.0f, 200);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 40.0f, estimate_session_pct(&E, 5 * MIN_MS, 0.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 40.0f, estimate_session_pct(&E, 5 * MIN_MS, -3.0f));
    // stale: older than EST_MAX_AGE_MS
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 40.0f, estimate_session_pct(&E, EST_MAX_AGE_MS + 1, 20.0f));
    TEST_ASSERT_TRUE(estimate_session_pct(&E, EST_MAX_AGE_MS, 40.0f) > 40.0f);
    // reset deadline passed
    estimate_reading(&E, 0, 40.0f, 3);
    TEST_ASSERT_TRUE(estimate_session_pct(&E, 2 * MIN_MS, 20.0f) > 40.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 40.0f, estimate_session_pct(&E, 3 * MIN_MS, 20.0f));
    TEST_ASSERT_EQUAL(-1, estimate_runout_mins(&E, 3 * MIN_MS, 20.0f));
}

static void test_unknown_reset_still_creeps(void) {
    estimate_reading(&E, 0, 40.0f, -1);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 42.0f, estimate_session_pct(&E, 6 * MIN_MS, 20.0f));
    TEST_ASSERT_EQUAL(174, estimate_runout_mins(&E, 6 * MIN_MS, 20.0f));
}

static void test_runout_math(void) {
    estimate_reading(&E, 0, 60.0f, 300);
    TEST_ASSERT_EQUAL(120, estimate_runout_mins(&E, 0, 20.0f));
    // 3 min later: est 61 -> 117 min left
    TEST_ASSERT_EQUAL(117, estimate_runout_mins(&E, 3 * MIN_MS, 20.0f));
}

static void test_runout_none_beyond_reset(void) {
    estimate_reading(&E, 0, 60.0f, 100);          // resets in 100 min, out in 120
    TEST_ASSERT_EQUAL(-1, estimate_runout_mins(&E, 0, 20.0f));
    estimate_reading(&E, 0, 60.0f, 130);
    TEST_ASSERT_EQUAL(120, estimate_runout_mins(&E, 0, 20.0f));
}

static void test_runout_none_when_rate_tiny(void) {
    estimate_reading(&E, 0, 90.0f, 300);
    TEST_ASSERT_EQUAL(-1, estimate_runout_mins(&E, 0, 1.9f));
    TEST_ASSERT_EQUAL(-1, estimate_runout_mins(&E, 0, 0.0f));
    TEST_ASSERT_TRUE(estimate_runout_mins(&E, 0, EST_MIN_RATE) >= 0);
}

static void test_runout_at_100(void) {
    estimate_reading(&E, 0, 100.0f, 300);
    TEST_ASSERT_EQUAL(0, estimate_runout_mins(&E, 0, 10.0f));
}

static void test_label(void) {
    char b[16];
    estimate_runout_label(-1, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("", b);
    estimate_runout_label(0, b, sizeof b);   TEST_ASSERT_EQUAL_STRING("out in 0m", b);
    estimate_runout_label(25, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("out in 25m", b);
    estimate_runout_label(59, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("out in 59m", b);
    estimate_runout_label(60, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("out in 1h", b);
    estimate_runout_label(100, b, sizeof b); TEST_ASSERT_EQUAL_STRING("out in 1h40m", b);
    estimate_runout_label(105, b, sizeof b); TEST_ASSERT_EQUAL_STRING("out in 1h45m", b);
    estimate_runout_label(120, b, sizeof b); TEST_ASSERT_EQUAL_STRING("out in 2h", b);
    estimate_runout_label(300, b, sizeof b); TEST_ASSERT_EQUAL_STRING("out in 5h", b);
}

static void test_wraparound(void) {
    uint32_t t0 = UINT32_MAX - 2 * MIN_MS;     // reading 2 min before the clock wraps
    estimate_reading(&E, t0, 40.0f, 200);
    uint32_t t1 = t0 + 6 * MIN_MS;             // wrapped past zero
    TEST_ASSERT_TRUE(t1 < t0);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 42.0f, estimate_session_pct(&E, t1, 20.0f));
    TEST_ASSERT_EQUAL(174, estimate_runout_mins(&E, t1, 20.0f));
    // stale across the wrap
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 40.0f, estimate_session_pct(&E, t0 + 11 * MIN_MS, 20.0f));
    // reset deadline across the wrap
    estimate_reading(&E, t0, 40.0f, 4);
    TEST_ASSERT_TRUE(estimate_session_pct(&E, t0 + 3 * MIN_MS, 20.0f) > 40.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 40.0f, estimate_session_pct(&E, t0 + 4 * MIN_MS, 20.0f));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_no_reading);
    RUN_TEST(test_creeps_at_rate);
    RUN_TEST(test_capped_ahead_and_at_100);
    RUN_TEST(test_frozen);
    RUN_TEST(test_unknown_reset_still_creeps);
    RUN_TEST(test_runout_math);
    RUN_TEST(test_runout_none_beyond_reset);
    RUN_TEST(test_runout_none_when_rate_tiny);
    RUN_TEST(test_runout_at_100);
    RUN_TEST(test_label);
    RUN_TEST(test_wraparound);
    return UNITY_END();
}
