// accounts + gesture (clawdmeter2): two accounts swiped between on the device.
//   pio test -d firmware -e native -f test_accounts
#include <unity.h>
#include <string.h>
#include "accounts.h"
#include "gesture.h"
#include "payload.h"

void setUp(void) { accounts_reset(); }
void tearDown(void) {}

static UsageData parse(const char* json) {
    UsageData d;
    memset(&d, 0, sizeof(d));
    TEST_ASSERT_TRUE(payload_parse(json, &d, nullptr));
    return d;
}

static const char* TWO =
    "{\"s\":40,\"sr\":100,\"w\":20,\"wr\":5000,\"ok\":true,\"ai\":1,\"ac\":2,"
    "\"o\":[{\"i\":2,\"s\":75,\"sr\":30,\"w\":60,\"wr\":900}]}";

static void test_payload_parses_accounts(void) {
    UsageData d = parse(TWO);
    TEST_ASSERT_EQUAL(1, d.account_idx);
    TEST_ASSERT_EQUAL(2, d.account_count);
    TEST_ASSERT_EQUAL(1, d.other_count);
    TEST_ASSERT_EQUAL(2, d.others[0].index);
    TEST_ASSERT_EQUAL_FLOAT(75.0f, d.others[0].session_pct);
    TEST_ASSERT_EQUAL(900, d.others[0].weekly_reset_mins);
}

static void test_old_app_is_single_account(void) {
    UsageData d = parse("{\"s\":40,\"ok\":true}");
    TEST_ASSERT_EQUAL(-1, d.account_idx);
    TEST_ASSERT_EQUAL(0, d.other_count);
    accounts_ingest(&d);
    TEST_ASSERT_EQUAL(0, accounts_count());
    TEST_ASSERT_EQUAL(0, accounts_step(+1));   // nothing to swipe to
    UsageData v;
    TEST_ASSERT_TRUE(accounts_view(&d, &v));
    TEST_ASSERT_EQUAL_FLOAT(40.0f, v.session_pct);
}

static void test_follows_the_app_until_swiped(void) {
    UsageData d = parse(TWO);
    accounts_ingest(&d);
    TEST_ASSERT_EQUAL(1, accounts_shown());
    TEST_ASSERT_FALSE(accounts_user_chose());
    // The app's active account moves to #2: the screen follows.
    UsageData d2 = parse("{\"s\":76,\"sr\":29,\"w\":60,\"wr\":899,\"ok\":true,\"ai\":2,\"ac\":2,"
                         "\"o\":[{\"i\":1,\"s\":40,\"sr\":99,\"w\":20,\"wr\":4999}]}");
    accounts_ingest(&d2);
    TEST_ASSERT_EQUAL(2, accounts_shown());
    // A swipe picks #1 and it sticks while the app stays on #2.
    TEST_ASSERT_EQUAL(1, accounts_step(+1));   // wraps 2 -> 1
    accounts_ingest(&d2);
    TEST_ASSERT_EQUAL(1, accounts_shown());
    TEST_ASSERT_TRUE(accounts_user_chose());
}

static void test_view_shows_the_other_accounts_numbers(void) {
    UsageData d = parse(TWO);
    accounts_ingest(&d);
    TEST_ASSERT_EQUAL(2, accounts_step(+1));
    UsageData v;
    TEST_ASSERT_TRUE(accounts_view(&d, &v));
    TEST_ASSERT_EQUAL_FLOAT(75.0f, v.session_pct);
    TEST_ASSERT_EQUAL(30, v.session_reset_mins);
    TEST_ASSERT_EQUAL_FLOAT(60.0f, v.weekly_pct);
    TEST_ASSERT_EQUAL(2, v.account_idx);
    TEST_ASSERT_TRUE(v.ok);
    TEST_ASSERT_EQUAL(1, accounts_step(-1));     // back to #1
}

static void test_use_next_hint(void) {
    // #1 at 40 %: plenty left, no hint.
    UsageData d = parse(TWO);
    accounts_ingest(&d);
    TEST_ASSERT_EQUAL(0, accounts_use_next());
    // #1 tight (weekly 90 %), #2 at 75 %: not enough better.
    d = parse("{\"s\":50,\"sr\":100,\"w\":90,\"wr\":5000,\"ok\":true,\"ai\":1,\"ac\":2,"
              "\"o\":[{\"i\":2,\"s\":75,\"sr\":30,\"w\":60,\"wr\":900}]}");
    accounts_ingest(&d);
    TEST_ASSERT_EQUAL(0, accounts_use_next());
    // #2 has room: hint at it.
    d = parse("{\"s\":50,\"sr\":100,\"w\":90,\"wr\":5000,\"ok\":true,\"ai\":1,\"ac\":2,"
              "\"o\":[{\"i\":2,\"s\":10,\"sr\":30,\"w\":30,\"wr\":900}]}");
    accounts_ingest(&d);
    TEST_ASSERT_EQUAL(2, accounts_use_next());
}

static void test_unknown_account_view_falls_back(void) {
    UsageData d = parse("{\"s\":40,\"ok\":true,\"ai\":1,\"ac\":2}");
    accounts_ingest(&d);
    TEST_ASSERT_EQUAL(2, accounts_step(+1));
    UsageData v;
    TEST_ASSERT_FALSE(accounts_view(&d, &v));   // #2 never sent
}

static void test_restored_choice_survives_first_payload(void) {
    accounts_restore(2);
    UsageData d = parse(TWO);
    accounts_ingest(&d);
    TEST_ASSERT_EQUAL(2, accounts_shown());
    accounts_reset();
    accounts_restore(3);                         // saved with three accounts, now two
    accounts_ingest(&d);
    TEST_ASSERT_EQUAL(1, accounts_shown());
}

static void test_no_data_is_ignored(void) {
    UsageData d = parse(TWO);
    accounts_ingest(&d);
    UsageData off = parse("{\"ok\":false}");
    accounts_ingest(&off);
    TEST_ASSERT_EQUAL(2, accounts_count());
    TEST_ASSERT_TRUE(accounts_have(2));
}

static void test_offline_follows_o(void) {
    UsageData d = parse("{\"s\":40,\"sr\":100,\"w\":20,\"wr\":5000,\"ok\":true,\"ai\":1,\"ac\":2,"
                        "\"o\":[{\"i\":2,\"s\":75,\"sr\":-1,\"w\":60,\"wr\":900,\"off\":1}]}");
    TEST_ASSERT_EQUAL(-1, accounts_offline(2));      // nothing received yet
    accounts_ingest(&d);
    TEST_ASSERT_EQUAL(1, accounts_offline(2));
    TEST_ASSERT_EQUAL(0, accounts_offline(1));       // the live top-level account
    d = parse(TWO);                                  // 2 is back, no `off`
    accounts_ingest(&d);
    TEST_ASSERT_EQUAL(-1, accounts_offline(2));      // not said, like accounts_working
    TEST_ASSERT_EQUAL(-1, accounts_offline(0));
    TEST_ASSERT_EQUAL(-1, accounts_offline(10));
}

static void test_all_logged_out_keeps_pages(void) {
    UsageData d = parse(TWO);
    accounts_ingest(&d);
    d = parse("{\"ok\":false,\"ac\":2,\"o\":[{\"i\":1,\"s\":30,\"sr\":-1,\"w\":10,\"wr\":4000,\"off\":1},"
              "{\"i\":2,\"s\":70,\"sr\":-1,\"w\":50,\"wr\":800,\"off\":1}]}");
    accounts_ingest(&d);
    TEST_ASSERT_EQUAL(2, accounts_count());
    TEST_ASSERT_EQUAL(0, accounts_active());         // no top-level account is invented
    TEST_ASSERT_EQUAL(1, accounts_offline(1));
    TEST_ASSERT_EQUAL(1, accounts_offline(2));
    UsageData v;
    TEST_ASSERT_TRUE(accounts_view(&d, &v));         // shown = #1: its dimmed last reading
    TEST_ASSERT_TRUE(v.ok);
    TEST_ASSERT_EQUAL_FLOAT(30.0f, v.session_pct);
    TEST_ASSERT_EQUAL(-1, v.session_reset_mins);     // not started, not a countdown
    TEST_ASSERT_EQUAL(2, accounts_step(+1));
    TEST_ASSERT_TRUE(accounts_view(&d, &v));
    TEST_ASSERT_EQUAL_FLOAT(70.0f, v.session_pct);
    TEST_ASSERT_EQUAL(2, v.account_idx);
}

static void test_all_logged_out_without_pages_is_no_data(void) {
    UsageData d = parse("{\"ok\":false,\"ac\":2,\"o\":[{\"i\":1,\"off\":1}]}");
    accounts_ingest(&d);
    UsageData v;
    TEST_ASSERT_TRUE(accounts_view(&d, &v));         // shown #1 has a snapshot (offline)
    d = parse("{\"ok\":false}");
    accounts_ingest(&d);                             // a bare beat changes nothing
    TEST_ASSERT_EQUAL(2, accounts_count());
    accounts_reset();
    d = parse("{\"ok\":false}");
    accounts_ingest(&d);
    TEST_ASSERT_TRUE(accounts_view(&d, &v));
    TEST_ASSERT_FALSE(v.ok);                         // never zeros on a live-looking page
}

static void test_working_flag_per_account(void) {
    UsageData d = parse("{\"s\":40,\"ok\":true,\"ai\":1,\"ac\":2,\"wk\":1,"
                        "\"o\":[{\"i\":2,\"s\":75,\"sr\":30,\"w\":60,\"wr\":900,\"wk\":0}]}");
    TEST_ASSERT_EQUAL(1, d.working);
    TEST_ASSERT_EQUAL(0, d.others[0].working);
    accounts_ingest(&d);
    TEST_ASSERT_EQUAL(1, accounts_working(1));
    TEST_ASSERT_EQUAL(0, accounts_working(2));
    // Claude moves to #2: both flags follow.
    d = parse("{\"s\":40,\"ok\":true,\"ai\":1,\"ac\":2,\"wk\":0,"
              "\"o\":[{\"i\":2,\"s\":76,\"sr\":29,\"w\":60,\"wr\":899}]}");
    accounts_ingest(&d);
    TEST_ASSERT_EQUAL(0, accounts_working(1));
    TEST_ASSERT_EQUAL(-1, accounts_working(2));   // not said this time
    d.others[0].working = 1;
    accounts_ingest(&d);
    TEST_ASSERT_EQUAL(1, accounts_working(2));
    // An app without the flag, or no such account: not said.
    UsageData old = parse("{\"s\":40,\"ok\":true}");
    TEST_ASSERT_EQUAL(-1, old.working);
    TEST_ASSERT_EQUAL(-1, accounts_working(0));
    TEST_ASSERT_EQUAL(-1, accounts_working(5));
}

static void test_turn_tokens_and_time_per_account(void) {
    UsageData d = parse("{\"s\":40,\"ok\":true,\"ai\":1,\"ac\":2,\"wk\":1,\"wt\":1234,\"wd\":65,"
                        "\"o\":[{\"i\":2,\"s\":75,\"sr\":30,\"w\":60,\"wr\":900,\"wt\":7,\"wd\":3}]}");
    TEST_ASSERT_EQUAL(1234, d.turn_tokens);
    TEST_ASSERT_EQUAL(65, d.turn_secs);
    accounts_ingest(&d);
    int32_t tok, secs;
    accounts_turn(1, &tok, &secs);
    TEST_ASSERT_EQUAL(1234, tok);
    TEST_ASSERT_EQUAL(65, secs);
    accounts_turn(2, &tok, &secs);
    TEST_ASSERT_EQUAL(7, tok);
    TEST_ASSERT_EQUAL(3, secs);
    accounts_turn(3, &tok, &secs);
    TEST_ASSERT_EQUAL(-1, tok);
    UsageData old = parse("{\"s\":40,\"ok\":true}");
    TEST_ASSERT_EQUAL(-1, old.turn_tokens);
    TEST_ASSERT_EQUAL(-1, old.turn_secs);
}

static void test_follow_app_drops_the_swipe(void) {
    UsageData d = parse(TWO);
    accounts_ingest(&d);
    TEST_ASSERT_EQUAL(2, accounts_step(+1));
    TEST_ASSERT_TRUE(accounts_user_chose());
    accounts_follow_app();
    TEST_ASSERT_FALSE(accounts_user_chose());
    TEST_ASSERT_EQUAL(1, accounts_shown());
}

// ---- gesture ----

static Gesture drag(int x0, int y0, int x1, int y1, uint32_t ms) {
    GestureState s = {};
    Gesture g = gesture_step(&s, true, x0, y0, 1000);
    TEST_ASSERT_EQUAL(GESTURE_NONE, g.kind);
    const int steps = 5;
    for (int i = 1; i <= steps; i++)
        gesture_step(&s, true, x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps, 1000 + ms * i / steps);
    return gesture_step(&s, false, 0, 0, 1000 + ms + 10);
}

static void test_gestures(void) {
    TEST_ASSERT_EQUAL(GESTURE_TAP, drag(200, 200, 205, 198, 120).kind);
    TEST_ASSERT_EQUAL(GESTURE_SWIPE_LEFT, drag(400, 240, 150, 260, 250).kind);
    TEST_ASSERT_EQUAL(GESTURE_SWIPE_RIGHT, drag(80, 240, 330, 230, 250).kind);
    TEST_ASSERT_EQUAL(GESTURE_NONE, drag(200, 100, 220, 400, 250).kind);    // vertical
    TEST_ASSERT_EQUAL(GESTURE_NONE, drag(200, 200, 202, 201, 2000).kind);   // a hold
    TEST_ASSERT_EQUAL(GESTURE_NONE, drag(400, 240, 150, 240, 1500).kind);   // a slow drag
    Gesture g = drag(123, 45, 125, 46, 100);
    TEST_ASSERT_EQUAL(123, g.x);
    TEST_ASSERT_EQUAL(45, g.y);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_payload_parses_accounts);
    RUN_TEST(test_old_app_is_single_account);
    RUN_TEST(test_follows_the_app_until_swiped);
    RUN_TEST(test_view_shows_the_other_accounts_numbers);
    RUN_TEST(test_unknown_account_view_falls_back);
    RUN_TEST(test_use_next_hint);
    RUN_TEST(test_restored_choice_survives_first_payload);
    RUN_TEST(test_no_data_is_ignored);
    RUN_TEST(test_offline_follows_o);
    RUN_TEST(test_all_logged_out_keeps_pages);
    RUN_TEST(test_all_logged_out_without_pages_is_no_data);
    RUN_TEST(test_working_flag_per_account);
    RUN_TEST(test_turn_tokens_and_time_per_account);
    RUN_TEST(test_follow_app_drops_the_swipe);
    RUN_TEST(test_gestures);
    return UNITY_END();
}
