#include "history.h"
#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>

static AccountHistory g_hist[9];

static void copy_str(char* dst, size_t cap, const char* src) {
    if (!cap) return;
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
}

bool history_is_message(const char* json) {
    return json && strstr(json, "\"h\":") != nullptr;
}

bool history_parse(const char* json, int* account, AccountHistory* out) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;
    if (doc["h"].isNull() || !doc["h"].as<bool>()) return false;
    const int i = doc["i"] | 0;
    if (i < 1 || i > 9) return false;
    JsonArrayConst d = doc["d"].as<JsonArrayConst>();
    if (d.isNull()) return false;

    AccountHistory h = {};
    const size_t n = d.size();
    // Keep the newest 30; a short array leaves zeros on the oldest side.
    const size_t take = n < HISTORY_DAYS ? n : HISTORY_DAYS;
    const size_t skip = n - take;
    size_t k = 0;
    for (JsonVariantConst v : d) {
        if (k++ < skip) continue;
        const double x = v.as<double>();
        const size_t idx = HISTORY_DAYS - take + (k - 1 - skip);
        h.days_k[idx] = x <= 0 ? 0u : x > 4294967295.0 ? 0xFFFFFFFFu : (uint32_t)x;
    }
    // double, not uint32: a week of tokens can pass 2^31.
    const double wt = doc["wt"].as<double>();
    h.week_tokens = wt <= 0 ? 0 : (uint64_t)wt;
    copy_str(h.joke, sizeof(h.joke), doc["j"] | "");
    JsonArrayConst hd = doc["hd"].as<JsonArrayConst>();
    if (!hd.isNull() && hd.size() > 0) {
        size_t k2 = 0;
        for (JsonVariantConst v : hd) {
            if (k2 >= HISTORY_HOURS) break;
            const double x = v.as<double>();
            h.hours_h[k2++] = x <= 0 ? 0u : x > 4294967295.0 ? 0xFFFFFFFFu : (uint32_t)x;
        }
        h.has_hours = true;
    }
    h.have = true;
    *account = i;
    *out = h;
    return true;
}

void history_store(int account, const AccountHistory* h) {
    if (account < 1 || account > 9 || !h) return;
    g_hist[account - 1] = *h;
    g_hist[account - 1].have = true;
}

const AccountHistory* history_get(int account) {
    if (account < 1 || account > 9) return nullptr;
    return g_hist[account - 1].have ? &g_hist[account - 1] : nullptr;
}

const uint32_t* history_hours_get(int account) {
    const AccountHistory* h = history_get(account);
    return (h && h->has_hours) ? h->hours_h : nullptr;
}

void history_format_tokens(uint64_t n, char* buf, size_t len) {
    if (!len) return;
    const char* suf = "";
    uint64_t unit = 1;
    if (n >= 1000000000ULL) { suf = "B"; unit = 1000000000ULL; }
    else if (n >= 1000000ULL) { suf = "M"; unit = 1000000ULL; }
    else if (n >= 1000ULL) { suf = "K"; unit = 1000ULL; }
    if (unit == 1) { snprintf(buf, len, "%u", (unsigned)n); return; }
    // Tenths, rounded; 999.95K would print as 1000K, so it rolls to the next unit.
    uint64_t tenths = (n * 10 + unit / 2) / unit;
    if (tenths >= 10000 && suf[0] != 'B') {
        tenths = (tenths + 500) / 1000;   // 1000.0 of a unit = 1.0 of the next, in tenths
        suf = suf[0] == 'K' ? "M" : "B";
    }
    if (tenths % 10) snprintf(buf, len, "%u.%u%s", (unsigned)(tenths / 10), (unsigned)(tenths % 10), suf);
    else snprintf(buf, len, "%u%s", (unsigned)(tenths / 10), suf);
}
