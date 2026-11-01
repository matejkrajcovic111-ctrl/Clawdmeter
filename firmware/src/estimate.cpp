#include "estimate.h"
#include <stdio.h>

void estimate_reset(Estimate* e) {
    e->has = false; e->at = 0; e->pct = 0.0f; e->has_reset = false; e->reset_at = 0;
}

void estimate_reading(Estimate* e, uint32_t now, float session_pct, int reset_mins) {
    e->has = true;
    e->at = now;
    e->pct = session_pct;
    e->has_reset = reset_mins >= 0;
    e->reset_at = e->has_reset ? now + (uint32_t)reset_mins * 60000UL : 0;
}

static bool reset_passed(const Estimate* e, uint32_t now) {
    return e->has_reset && (int32_t)(now - e->reset_at) >= 0;
}

// True when the estimate must stay at the reading.
static bool frozen(const Estimate* e, uint32_t now, float rate) {
    return rate <= 0.0f || (uint32_t)(now - e->at) > EST_MAX_AGE_MS || reset_passed(e, now);
}

float estimate_session_pct(const Estimate* e, uint32_t now, float rate) {
    if (!e->has) return -1.0f;
    if (frozen(e, now, rate)) return e->pct;
    float ahead = rate * ((float)(uint32_t)(now - e->at) / 3600000.0f);
    if (ahead > EST_MAX_AHEAD) ahead = EST_MAX_AHEAD;
    float p = e->pct + ahead;
    if (p > 100.0f) p = 100.0f;
    return p < e->pct ? e->pct : p;
}

int estimate_runout_mins(const Estimate* e, uint32_t now, float rate) {
    if (!e->has || rate < EST_MIN_RATE || frozen(e, now, rate)) return -1;
    float p = estimate_session_pct(e, now, rate);
    int mins = 0;
    if (p < 100.0f) mins = (int)((100.0f - p) / rate * 60.0f + 0.5f);
    if (e->has_reset) {
        uint32_t left_ms = e->reset_at - now;   // frozen() already excluded a passed deadline
        if ((uint32_t)mins > left_ms / 60000UL) return -1;   // the window resets first
    }
    return mins;
}

void estimate_runout_label(int mins, char* buf, int len) {
    if (len <= 0) return;
    if (mins < 0) { buf[0] = '\0'; return; }
    if (mins < 60) snprintf(buf, (size_t)len, "out in %dm", mins);
    else if (mins % 60 == 0) snprintf(buf, (size_t)len, "out in %dh", mins / 60);
    else snprintf(buf, (size_t)len, "out in %dh%dm", mins / 60, mins % 60);
}
