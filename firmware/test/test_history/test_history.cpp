// history message parse/store + token formatting (clawdmeter2).
//   pio test -d firmware -e native -f test_history
#include <unity.h>
#include <string.h>
#include <stdio.h>
#include "history.h"

void setUp(void) {}
void tearDown(void) {}

static void test_parse_ok(void) {
    int acct = 0;
    AccountHistory h;
    const char* j = "{\"h\":1,\"i\":2,\"d\":[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,"
                    "21,22,23,24,25,26,27,28,29,30],\"wt\":1834000,\"j\":\"Hello.\"}";
    TEST_ASSERT_TRUE(history_is_message(j));
    TEST_ASSERT_TRUE(history_parse(j, &acct, &h));
    TEST_ASSERT_EQUAL(2, acct);
    TEST_ASSERT_TRUE(h.have);
    TEST_ASSERT_EQUAL_UINT32(1, h.days_k[0]);
    TEST_ASSERT_EQUAL_UINT32(30, h.days_k[29]);
    TEST_ASSERT_TRUE(h.week_tokens == 1834000ULL);
    TEST_ASSERT_EQUAL_STRING("Hello.", h.joke);
}

static void test_pad_short_d(void) {
    int acct = 0;
    AccountHistory h;
    TEST_ASSERT_TRUE(history_parse("{\"h\":1,\"i\":1,\"d\":[5,6,7],\"wt\":0,\"j\":\"\"}", &acct, &h));
    TEST_ASSERT_EQUAL_UINT32(0, h.days_k[0]);
    TEST_ASSERT_EQUAL_UINT32(0, h.days_k[26]);
    TEST_ASSERT_EQUAL_UINT32(5, h.days_k[27]);
    TEST_ASSERT_EQUAL_UINT32(7, h.days_k[29]);
}

static void test_truncate_long_d(void) {
    char j[400] = "{\"h\":1,\"i\":3,\"d\":[";
    for (int k = 1; k <= 32; k++) {
        char t[8];
        snprintf(t, sizeof(t), k > 1 ? ",%d" : "%d", k);
        strcat(j, t);
    }
    strcat(j, "],\"wt\":1}");
    int acct = 0;
    AccountHistory h;
    TEST_ASSERT_TRUE(history_parse(j, &acct, &h));
    TEST_ASSERT_EQUAL_UINT32(3, h.days_k[0]);    // last 30 of 1..32
    TEST_ASSERT_EQUAL_UINT32(32, h.days_k[29]);
}

static void test_big_wt(void) {
    int acct = 0;
    AccountHistory h;
    TEST_ASSERT_TRUE(history_parse("{\"h\":1,\"i\":1,\"d\":[],\"wt\":5400000000}", &acct, &h));
    TEST_ASSERT_TRUE(h.week_tokens == 5400000000ULL);
}

static void test_joke_truncated(void) {
    int acct = 0;
    AccountHistory h;
    const char* j = "{\"h\":1,\"i\":1,\"d\":[1],\"wt\":1,\"j\":\"0123456789012345678901234567890123456789"
                    "0123456789012345678901234567890123456789\"}";
    TEST_ASSERT_TRUE(history_parse(j, &acct, &h));
    TEST_ASSERT_EQUAL(sizeof(h.joke) - 1, strlen(h.joke));
}

static void test_rejects_non_history(void) {
    int acct = 0;
    AccountHistory h;
    const char* u = "{\"s\":40,\"sr\":100,\"w\":20,\"ok\":true,\"ai\":1}";
    TEST_ASSERT_FALSE(history_is_message(u));
    TEST_ASSERT_FALSE(history_parse(u, &acct, &h));
    TEST_ASSERT_FALSE(history_parse("{\"h\":1,\"i\":0,\"d\":[1]}", &acct, &h));
    TEST_ASSERT_FALSE(history_parse("{\"h\":1,\"i\":10,\"d\":[1]}", &acct, &h));
    TEST_ASSERT_FALSE(history_parse("not json", &acct, &h));
}

static void test_store_get(void) {
    TEST_ASSERT_NULL(history_get(4));
    AccountHistory h;
    memset(&h, 0, sizeof(h));
    h.week_tokens = 77;
    history_store(4, &h);
    TEST_ASSERT_NOT_NULL(history_get(4));
    TEST_ASSERT_TRUE(history_get(4)->week_tokens == 77);
    TEST_ASSERT_NULL(history_get(5));
    TEST_ASSERT_NULL(history_get(0));
    TEST_ASSERT_NULL(history_get(10));
}

static void test_hours(void) {
    int acct = 0;
    AccountHistory h;
    TEST_ASSERT_TRUE(history_parse("{\"h\":1,\"i\":6,\"d\":[1],\"wt\":1,\"hd\":[0,5,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,9]}", &acct, &h));
    TEST_ASSERT_TRUE(h.has_hours);
    TEST_ASSERT_EQUAL_UINT32(5, h.hours_h[1]);
    TEST_ASSERT_EQUAL_UINT32(9, h.hours_h[23]);
    history_store(acct, &h);
    TEST_ASSERT_NOT_NULL(history_hours_get(6));
    TEST_ASSERT_EQUAL_UINT32(5, history_hours_get(6)[1]);
    TEST_ASSERT_TRUE(history_parse("{\"h\":1,\"i\":7,\"d\":[1],\"wt\":1}", &acct, &h));
    TEST_ASSERT_FALSE(h.has_hours);
    history_store(acct, &h);
    TEST_ASSERT_NULL(history_hours_get(7));
    TEST_ASSERT_NULL(history_hours_get(8));
}

static void fmt(uint64_t n, const char* want) {
    char b[16];
    history_format_tokens(n, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING(want, b);
}

static void test_format(void) {
    fmt(0, "0");
    fmt(950, "950");
    fmt(1000, "1K");
    fmt(12400, "12.4K");
    fmt(1800000, "1.8M");
    fmt(2000000, "2M");
    fmt(2300000000ULL, "2.3B");
    fmt(999950, "1M");
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_ok);
    RUN_TEST(test_pad_short_d);
    RUN_TEST(test_truncate_long_d);
    RUN_TEST(test_big_wt);
    RUN_TEST(test_joke_truncated);
    RUN_TEST(test_rejects_non_history);
    RUN_TEST(test_store_get);
    RUN_TEST(test_hours);
    RUN_TEST(test_format);
    return UNITY_END();
}
